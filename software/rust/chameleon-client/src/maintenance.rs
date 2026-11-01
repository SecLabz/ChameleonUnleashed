//! Bounded Classic maintenance, including cards that normal selection cannot open.
use crate::{Card, Client, Result, TagCredential, command::*};
use nfc_native::formats::image::Model;
use std::time::Duration;

pub struct ClassicMaintenance<'a> {
    client: &'a mut Client,
    previous_reader: bool,
    method: u8,
    model: Model,
    unbound: bool,
    pub card: Card,
}

impl<'a> ClassicMaintenance<'a> {
    /// Method 1=Gen1A, 2=Gen1B, 3=CUID. An unbound session must identify block 0
    /// before any other operation; its UID length and model must be supplied.
    pub fn begin(
        client: &'a mut Client,
        method: u8,
        model: Model,
        card: Card,
        unbound: bool,
    ) -> Result<Self> {
        if !(1..=3).contains(&method)
            || model.classic().is_none()
            || !matches!(card.uid.len(), 4 | 7)
            || (method != 3 && card.uid.len() != 4)
            || !client.capabilities.contains(&MF1_MAINTENANCE)
        {
            return Err("Maintenance unsupported; check method, UID length and firmware".into());
        }
        let previous_reader = client.reader_mode()?;
        let mut session = Self {
            client,
            previous_reader,
            method,
            model,
            unbound,
            card,
        };
        if !previous_reader && let Err(error) = session.client.set_reader_mode(true) {
            let cleanup = session.finish();
            return crate::with_cleanup(Err(error), cleanup);
        }
        Ok(session)
    }

    pub fn run<T>(mut self, action: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        let result = action(&mut self);
        let cleanup = self.finish();
        crate::with_cleanup(result, cleanup)
    }

    pub fn finish(&mut self) -> Result<()> {
        if std::mem::replace(&mut self.previous_reader, true) {
            return Ok(());
        }
        self.client.restore_reader_mode(false)
    }

    fn request(
        &mut self,
        op: u8,
        block: u8,
        key: TagCredential,
        change: Option<([u8; 16], [u8; 16])>,
    ) -> Result<Vec<u8>> {
        if (op == 1 || op == 2) && self.unbound {
            return Err("Identify block 0 before maintenance writes".into());
        }
        if block as usize >= self.model.classic().unwrap().1 {
            return Err("Block outside Classic geometry".into());
        }
        let (kind, bytes) = match key {
            TagCredential::None if self.method != 3 => (0, [0; 6]),
            TagCredential::KeyA(key) if self.method == 3 => (1, key),
            TagCredential::KeyB(key) if self.method == 3 => (2, key),
            _ => return Err("Credential does not match maintenance method".into()),
        };
        let mut request = vec![op, self.method, self.model as u8, self.card.uid.len() as u8];
        request.extend(&self.card.uid);
        request.resize(11, 0);
        request.extend([block, kind]);
        request.extend(bytes);
        if let Some((before, after)) = change {
            request.extend(before);
            request.extend(after);
        }
        self.client
            .command(MF1_MAINTENANCE, &request, Duration::from_secs(5))
    }

    pub fn read(&mut self, block: u8, key: TagCredential) -> Result<[u8; 16]> {
        if self.unbound && block != 0 {
            return Err("Identify block 0 before reading other blocks".into());
        }
        let bytes = self.request(if self.unbound { 3 } else { 0 }, block, key, None)?;
        if bytes.len() != 27 || bytes[0] as usize != self.card.uid.len() {
            return Err("Invalid Classic maintenance reply".into());
        }
        let uid = bytes[1..1 + bytes[0] as usize].to_vec();
        if !self.unbound && uid != self.card.uid {
            return Err("Maintenance target changed; no further writes will be sent".into());
        }
        self.unbound = false;
        self.card.uid = uid;
        self.card.atqa = bytes[8..10].try_into()?;
        self.card.sak = bytes[10];
        Ok(bytes[11..].try_into()?)
    }

    pub fn check(
        &mut self,
        block: u8,
        key: TagCredential,
        before: [u8; 16],
        after: [u8; 16],
    ) -> Result<()> {
        if !self
            .request(1, block, key, Some((before, after)))?
            .is_empty()
        {
            return Err("Invalid maintenance preflight reply".into());
        }
        Ok(())
    }

    pub fn write(
        &mut self,
        block: u8,
        key: TagCredential,
        before: [u8; 16],
        after: [u8; 16],
    ) -> Result<()> {
        let bytes = self
            .request(2, block, key, Some((before, after)))
            .map_err(|error| {
                format!("Block {block}: write outcome uncertain; inspect before retrying. {error}")
            })?;
        if !bytes.is_empty() {
            return Err("Invalid maintenance write reply".into());
        }
        if block == 0 {
            self.card.uid = after[..self.card.uid.len()].to_vec();
        }
        Ok(())
    }
}

impl Drop for ClassicMaintenance<'_> {
    fn drop(&mut self) {
        let _ = self.finish();
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::test_support::device;
    use std::sync::atomic::Ordering;

    #[test]
    fn a_different_reply_uid_does_not_retarget_the_session() {
        let mut request = vec![0, 3, 1, 4, 1, 2, 3, 4, 0, 0, 0, 0, 1];
        request.extend([255; 6]);
        let mut response = vec![4, 9, 2, 3, 4, 0, 0, 0, 4, 0, 8];
        response.extend([0; 16]);
        let (mut client, peer) = device(vec![
            (GET_MODE, vec![], 0, vec![1]),
            (MF1_MAINTENANCE, request, 0, response),
        ]);
        client.capabilities.insert(MF1_MAINTENANCE);
        let mut session = ClassicMaintenance::begin(
            &mut client,
            3,
            Model::Classic1k,
            Card {
                uid: vec![1, 2, 3, 4],
                atqa: [4, 0],
                sak: 8,
            },
            false,
        )
        .unwrap();
        assert!(
            session
                .read(0, TagCredential::KeyA([255; 6]))
                .unwrap_err()
                .to_string()
                .contains("target changed")
        );
        assert_eq!(session.card.uid, [1, 2, 3, 4]);
        drop(session);
        drop(client);
        peer.join().unwrap();
    }

    #[test]
    fn unselected_card_is_bound_by_read_before_writes_and_mode_is_restored_on_cancel() {
        let mut request = vec![3, 3, 1, 4];
        request.extend([0; 7]);
        request.extend([0, 1]);
        request.extend([255; 6]);
        let mut response = vec![4, 1, 2, 3, 4, 0, 0, 0, 4, 0, 8];
        response.extend([0; 16]);
        let (mut client, peer) = device(vec![
            (GET_MODE, vec![], 0, vec![0]),
            (SET_MODE, vec![1], 0, vec![]),
            (GET_MODE, vec![], 0, vec![1]),
            (MF1_MAINTENANCE, request, 0, response),
            (SET_MODE, vec![0], 0, vec![]),
            (GET_MODE, vec![], 0, vec![0]),
        ]);
        client.capabilities.insert(MF1_MAINTENANCE);
        let cancelled = client.cancelled.clone();
        let mut session = ClassicMaintenance::begin(
            &mut client,
            3,
            Model::Classic1k,
            Card {
                uid: vec![0; 4],
                atqa: [0; 2],
                sak: 0,
            },
            true,
        )
        .unwrap();
        let key = TagCredential::KeyA([255; 6]);
        assert!(session.check(0, key, [0; 16], [0; 16]).is_err());
        assert!(session.read(1, key).is_err());
        assert_eq!(session.read(0, key).unwrap(), [0; 16]);
        assert_eq!(session.card.uid, [1, 2, 3, 4]);
        cancelled.store(true, Ordering::Relaxed);
        drop(session);
        drop(client);
        peer.join().unwrap();
    }

    #[test]
    fn cancelled_maintenance_entry_restores_emulation() {
        use crate::test_support::device_with_hook;
        use std::sync::{Arc, atomic::AtomicBool};
        let cancelled = Arc::new(AtomicBool::new(false));
        let trigger = cancelled.clone();
        let mut switched = false;
        let (mut client, worker) = device_with_hook(
            vec![
                (GET_MODE, vec![], 0x68, vec![0]),
                (SET_MODE, vec![1], 0x68, vec![]),
                (SET_MODE, vec![0], 0x68, vec![]),
                (GET_MODE, vec![], 0x68, vec![0]),
            ],
            move |command| {
                if command == SET_MODE && !switched {
                    switched = true;
                    trigger.store(true, Ordering::Relaxed);
                }
            },
        );
        client.capabilities.insert(MF1_MAINTENANCE);
        client.cancelled = cancelled.clone();
        let card = Card {
            uid: vec![1, 2, 3, 4],
            atqa: [4, 0],
            sak: 8,
        };
        let error = ClassicMaintenance::begin(&mut client, 1, Model::Classic1k, card, false)
            .err()
            .unwrap();
        assert!(error.is::<crate::RequestCancelled>());
        assert!(cancelled.load(Ordering::Relaxed));
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn maintenance_operation_reports_failed_restoration() {
        for failed in [false, true] {
            let (mut client, worker) = device(vec![
                (GET_MODE, vec![], 0x68, vec![0]),
                (SET_MODE, vec![1], 0x68, vec![]),
                (GET_MODE, vec![], 0x68, vec![1]),
                (SET_MODE, vec![0], 0x76, vec![]),
            ]);
            client.capabilities.insert(MF1_MAINTENANCE);
            let card = Card {
                uid: vec![1, 2, 3, 4],
                atqa: [4, 0],
                sak: 8,
            };
            let error = ClassicMaintenance::begin(&mut client, 1, Model::Classic1k, card, false)
                .unwrap()
                .run(|_| -> Result<()> {
                    if failed {
                        return Err("Primary maintenance failed".into());
                    }
                    Ok(())
                })
                .unwrap_err();
            let message = error.to_string();
            assert_eq!(message.contains("Primary maintenance failed"), failed);
            assert!(message.contains("cleanup failed"));
            drop(client);
            worker.join().unwrap();
        }
    }
}
