//! Public DESFire observations; credentials and secure messaging are outside scope.
use crate::{Client, Result, SlotImage, command::*};
use nfc_native::formats::{
    desfire::{self, Application, Capture, File, ReadState},
    image::{Image, Model},
};
use serde::{Deserialize, Serialize};
use std::{sync::atomic::Ordering, time::Duration};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DesfireSnapshot {
    pub version: u8,
    pub protocol: String,
    pub uid: Vec<u8>,
    pub metadata: Vec<u8>,
    pub memory: Vec<u8>,
    pub complete: bool,
    pub error: Option<String>,
}
impl DesfireSnapshot {
    pub fn image(&self) -> Result<Image> {
        if self.version != 1
            || self.protocol != "desfire"
            || (self.complete && self.error.is_some())
        {
            return Err("Invalid DESFire capture".into());
        }
        let image = Image {
            model: if self.metadata.starts_with(b"DFA1") {
                Model::Desfire
            } else {
                Model::IsoDep
            },
            uid: self.uid.clone(),
            metadata: self.metadata.clone(),
            memory: self.memory.clone(),
            secrets: vec![],
        };
        image.validate()?;
        Ok(image)
    }
    pub fn from_image(image: &Image) -> Result<Self> {
        image.validate()?;
        if !matches!(image.model, Model::Desfire | Model::IsoDep) {
            return Err("Expected DESFire observations".into());
        }
        Ok(Self {
            version: 1,
            protocol: "desfire".into(),
            uid: image.uid.clone(),
            metadata: image.metadata.clone(),
            memory: image.memory.clone(),
            complete: true,
            error: None,
        })
    }
    pub fn slot_image(&self) -> Result<SlotImage> {
        let image = self.image()?;
        let mut memory = vec![image.uid.len() as u8];
        memory.extend(&image.uid);
        memory.extend((image.metadata.len() as u16).to_be_bytes());
        memory.extend((image.memory.len() as u16).to_be_bytes());
        memory.extend(&image.metadata);
        memory.extend(&image.memory);
        memory.resize((memory.len() + 3) & !3, 0);
        let slot = SlotImage {
            version: 1,
            tag_type: 1300,
            identity: vec![],
            options: vec![],
            memory,
        };
        slot.validate_load()?;
        Ok(slot)
    }
    pub fn from_slot(slot: &SlotImage) -> Result<Self> {
        if slot.version != 1
            || slot.tag_type != 1300
            || !slot.identity.is_empty()
            || !slot.options.is_empty()
        {
            return Err("Invalid DESFire slot image".into());
        }
        let b = &slot.memory;
        let uid = usize::from(*b.first().ok_or("Missing DESFire identity")?);
        if ![4, 7, 10].contains(&uid) || b.len() < uid + 5 {
            return Err("Invalid DESFire identity".into());
        }
        let meta = u16::from_be_bytes([b[uid + 1], b[uid + 2]]) as usize;
        let data = u16::from_be_bytes([b[uid + 3], b[uid + 4]]) as usize;
        let end = uid + 5 + meta + data;
        if b.len() != (end + 3) & !3 || b[end..].iter().any(|v| *v != 0) {
            return Err("Invalid DESFire slot length".into());
        }
        let snapshot = Self {
            version: 1,
            protocol: "desfire".into(),
            uid: b[1..1 + uid].to_vec(),
            metadata: b[uid + 5..uid + 5 + meta].to_vec(),
            memory: b[uid + 5 + meta..end].to_vec(),
            complete: true,
            error: None,
        };
        snapshot.image()?;
        Ok(snapshot)
    }
}

struct Session<'a> {
    client: &'a mut Client,
    previous: bool,
    active: bool,
    closed: bool,
}
impl Session<'_> {
    fn exchange(&mut self, command: u8, params: &[u8]) -> Result<(ReadState, Vec<u8>)> {
        let mut data = vec![2, command];
        data.extend(params);
        let reply = self
            .client
            .command(DESFIRE_READER, &data, Duration::from_secs(10))?;
        let (&status, data) = reply.split_first().ok_or("Empty DESFire response")?;
        Ok((ReadState::status(status)?, data.to_vec()))
    }
    fn required(&mut self, command: u8, params: &[u8]) -> Result<Vec<u8>> {
        let (state, data) = self.exchange(command, params)?;
        if state != ReadState::Present {
            return Err(format!("DESFire {command:02X}: {state:?}").into());
        }
        Ok(data)
    }
    fn key_settings(&mut self) -> Result<(ReadState, Vec<u8>, Vec<u8>)> {
        let (state, settings) = self.exchange(0x45, &[])?;
        if state != ReadState::Present {
            return Ok((state, vec![], vec![]));
        }
        if settings.len() != 2 || settings[1] & 15 > 14 {
            return Err("Invalid DESFire key settings".into());
        }
        let mut versions = vec![];
        for id in 0..settings[1] & 15 {
            let (state, data) = self.exchange(0x64, &[id])?;
            if state != ReadState::Present {
                break;
            }
            if data.len() != 1 {
                return Err("Invalid DESFire key version".into());
            }
            versions.extend(data);
        }
        Ok((state, settings, versions))
    }
    fn read(&mut self, c: &mut Capture, info: bool, update: &mut dyn FnMut(&str)) -> Result<()> {
        let version = self.required(0x60, &[])?;
        c.version = version
            .try_into()
            .map_err(|_| "DESFire GetVersion must return 28 bytes")?;
        if c.version[0..3] != [4, 1, 1] {
            return Err("Tag is not an NXP DESFire card".into());
        }
        let (state, free) = self.exchange(0x6e, &[])?;
        if state == ReadState::Present {
            if free.len() != 3 {
                return Err("Invalid DESFire free memory".into());
            }
            c.free_memory = Some(free[0] as u32 | (free[1] as u32) << 8 | (free[2] as u32) << 16);
        }
        (c.settings_state, c.settings, c.key_versions) = self.key_settings()?;
        if info {
            return Ok(());
        }
        let (state, ids) = self.exchange(0x6a, &[])?;
        c.applications_state = state;
        if state != ReadState::Present {
            return Ok(());
        }
        if ids.len() % 3 != 0 || ids.len() / 3 > 28 {
            return Err("Invalid DESFire application directory".into());
        }
        for aid in ids.chunks_exact(3) {
            c.applications.push(Application {
                aid: aid.try_into()?,
                settings_state: ReadState::NotRead,
                settings: vec![],
                key_versions: vec![],
                files_state: ReadState::NotRead,
                files: vec![],
            });
        }
        let mut total = 0;
        for app in &mut c.applications {
            update(&format!(
                "Reading application {:02X}{:02X}{:02X}",
                app.aid[0], app.aid[1], app.aid[2]
            ));
            self.required(0x5a, &app.aid)?;
            (app.settings_state, app.settings, app.key_versions) = self.key_settings()?;
            let (state, ids) = self.exchange(0x6f, &[])?;
            app.files_state = state;
            if state != ReadState::Present {
                continue;
            }
            if ids.len() > 32 || total + ids.len() > 64 {
                return Err("DESFire capture exceeds 64 files".into());
            }
            total += ids.len();
            for id in ids {
                app.files.push(File {
                    id,
                    settings_state: ReadState::NotRead,
                    settings: vec![],
                    data_state: ReadState::NotRead,
                    data: vec![],
                });
            }
            for file in &mut app.files {
                let (state, settings) = self.exchange(0xf5, &[file.id])?;
                file.settings_state = state;
                if state != ReadState::Present {
                    continue;
                }
                file.settings = settings;
                let layout = desfire::file_layout(&file.settings)?;
                if !desfire::public_read(&file.settings) {
                    file.data_state = ReadState::Denied;
                    continue;
                }
                if layout.is_none() {
                    file.data_state = ReadState::Unsupported;
                }
            }
        }
        let mut captured_bytes = c.image()?.memory.len();
        for app in &mut c.applications {
            if !app.files.iter().any(|f| {
                f.settings_state == ReadState::Present && f.data_state == ReadState::NotRead
            }) {
                continue;
            }
            self.required(0x5a, &app.aid)?;
            for file in &mut app.files {
                if file.settings_state != ReadState::Present
                    || file.data_state != ReadState::NotRead
                {
                    continue;
                }
                let layout = desfire::file_layout(&file.settings)?;
                let Some((command, size, width)) = layout else {
                    file.data_state = ReadState::Unsupported;
                    continue;
                };
                if captured_bytes + size > 8192 {
                    return Err("Public DESFire file exceeds capture capacity".into());
                }
                let mut data = vec![];
                if command == 0x6c {
                    data = self.required(command, &[file.id])?;
                } else {
                    let records = (size / width).max(1);
                    let per = (128 / width).max(1);
                    if width > 511 {
                        return Err("DESFire record exceeds bounded exchange capacity".into());
                    }
                    for offset in (0..records).step_by(per) {
                        if size == 0 {
                            break;
                        }
                        let count = (records - offset).min(per);
                        let mut params = vec![file.id];
                        params.extend(&(offset as u32).to_le_bytes()[..3]);
                        params.extend(&(count as u32).to_le_bytes()[..3]);
                        let chunk = self.required(command, &params)?;
                        if chunk.len() != count * width {
                            return Err("Invalid DESFire file read length".into());
                        }
                        data.extend(chunk);
                    }
                }
                if data.len() != size {
                    return Err("Invalid DESFire file length".into());
                }
                captured_bytes += data.len();
                file.data = data;
                file.data_state = ReadState::Present;
                update("Public file read");
            }
        }
        Ok(())
    }
}
impl Session<'_> {
    fn close(&mut self) -> Result<()> {
        if self.closed {
            return Ok(());
        }
        self.closed = true;
        let cancelled = self.client.cancelled.swap(false, Ordering::Relaxed);
        let end = if self.active {
            self.client
                .command(DESFIRE_READER, &[0], Duration::from_secs(3))
                .map(|_| ())
        } else {
            Ok(())
        };
        let restore = self.client.set_reader_mode(self.previous);
        self.client.cancelled.fetch_or(cancelled, Ordering::Relaxed);
        crate::with_cleanup(end, restore)
    }
}
impl Drop for Session<'_> {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
impl Client {
    pub fn desfire_capture(
        &mut self,
        info: bool,
        update: &mut dyn FnMut(&str),
    ) -> Result<DesfireSnapshot> {
        if !self.capabilities.contains(&DESFIRE_READER) {
            return Err("Update firmware for DESFire reading".into());
        }
        let previous = self.reader_mode()?;
        let mut session = Session {
            client: self,
            previous,
            active: false,
            closed: false,
        };
        let activation = (|| -> Result<Capture> {
            session.client.set_reader_mode(true)?;
            // Even a malformed activation reply can leave an RF session open.
            session.active = true;
            let activation =
                session
                    .client
                    .command(DESFIRE_READER, &[1], Duration::from_secs(3))?;
            let n = usize::from(*activation.first().ok_or("Missing DESFire activation")?);
            if ![4, 7, 10].contains(&n) || activation.len() < n + 5 {
                return Err("Invalid DESFire activation".into());
            }
            let ats_len = activation[n + 4] as usize;
            if activation.len() != n + 5 + ats_len {
                return Err("Invalid DESFire ATS".into());
            }
            Ok(Capture {
                uid: activation[1..n + 1].to_vec(),
                atqa: activation[n + 1..n + 3].try_into()?,
                sak: activation[n + 3],
                ats: activation[n + 5..].to_vec(),
                version: [0; 28],
                free_memory: None,
                settings_state: ReadState::NotRead,
                settings: vec![],
                key_versions: vec![],
                applications_state: ReadState::NotRead,
                applications: vec![],
            })
        })();
        let mut c = match activation {
            Ok(capture) => capture,
            Err(error) => {
                let cleanup = session.close();
                return crate::with_cleanup(Err(error), cleanup);
            }
        };
        let result = session.read(&mut c, info, update);
        let cleanup = session.close();
        let result = crate::with_cleanup(result, cleanup);
        let image = if c.version[..3] != [4, 1, 1] {
            let mut metadata = b"DFU1".to_vec();
            metadata.extend(c.atqa);
            metadata.extend([c.sak, c.ats.len() as u8]);
            metadata.extend(&c.ats);
            Image {
                model: Model::IsoDep,
                uid: c.uid,
                metadata,
                memory: vec![],
                secrets: vec![],
            }
        } else {
            c.image()?
        };
        let mut snapshot = DesfireSnapshot::from_image(&image)?;
        snapshot.complete = result.is_ok();
        snapshot.error = result.err().map(|e| e.to_string());
        Ok(snapshot)
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::test_support::device;
    fn fixture() -> DesfireSnapshot {
        serde_json::from_str(include_str!(
            "../../../../resource/tools/tests/desfire-public.json"
        ))
        .unwrap()
    }
    #[test]
    fn public_capture_slot_roundtrip_and_capacity_preserve_observations() {
        let capture = fixture();
        let slot = capture.slot_image().unwrap();
        assert_eq!(
            DesfireSnapshot::from_slot(&slot).unwrap().image().unwrap(),
            capture.image().unwrap()
        );
        let mut c = Capture::from_image(&capture.image().unwrap()).unwrap();
        c.applications[0].files[0].settings[4..7].copy_from_slice(&[0, 18, 0]);
        c.applications[0].files[0].data = vec![0x42; 4608];
        let large = DesfireSnapshot::from_image(&c.image().unwrap()).unwrap();
        assert!(large.image().is_ok());
        assert!(
            large
                .slot_image()
                .unwrap_err()
                .to_string()
                .contains("4500-byte")
        );
    }
    #[test]
    fn denied_directory_is_an_observation_and_reader_cleanup_restores_mode() {
        let capture = fixture();
        let c = Capture::from_image(&capture.image().unwrap()).unwrap();
        let activation = [vec![7], c.uid, vec![0x44, 3, 0x20, 6], c.ats].concat();
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![1]),
            (DESFIRE_READER, vec![1], 0, activation),
            (
                DESFIRE_READER,
                vec![2, 0x60],
                0,
                [vec![0], c.version.to_vec()].concat(),
            ),
            (DESFIRE_READER, vec![2, 0x6e], 0, vec![0x1c]),
            (DESFIRE_READER, vec![2, 0x45], 0, vec![0x9d]),
            (DESFIRE_READER, vec![2, 0x6a], 0, vec![0x9d]),
            (DESFIRE_READER, vec![0], 0x68, vec![]),
            (SET_MODE, vec![0], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![0]),
        ]);
        client.capabilities.insert(DESFIRE_READER);
        let saved = client.desfire_capture(false, &mut |_| {}).unwrap();
        assert!(saved.complete);
        let observed = Capture::from_image(&saved.image().unwrap()).unwrap();
        assert_eq!(observed.applications_state, ReadState::Denied);
        assert!(observed.applications.is_empty());
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn cancelled_desfire_entry_restores_mode_without_opening_a_session() {
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
        client.capabilities.insert(DESFIRE_READER);
        client.cancelled = cancelled.clone();
        let error = client.desfire_capture(false, &mut |_| {}).unwrap_err();
        assert!(error.is::<crate::RequestCancelled>());
        assert!(cancelled.load(Ordering::Relaxed));
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn invalid_desfire_activation_preserves_cleanup_error_and_restores_mode() {
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![1]),
            (DESFIRE_READER, vec![1], 0x68, vec![]),
            (DESFIRE_READER, vec![0], 0x76, vec![]),
            (SET_MODE, vec![0], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![0]),
        ]);
        client.capabilities.insert(DESFIRE_READER);
        let error = client
            .desfire_capture(false, &mut |_| {})
            .unwrap_err()
            .to_string();
        assert!(error.contains("Missing DESFire activation"));
        assert!(error.contains("cleanup failed"));
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn desfire_read_and_restore_failures_remain_in_the_partial_capture() {
        let c = Capture::from_image(&fixture().image().unwrap()).unwrap();
        let uid = c.uid.clone();
        let activation = [vec![7], c.uid, vec![0x44, 3, 0x20, 6], c.ats].concat();
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![1]),
            (DESFIRE_READER, vec![1], 0x68, activation),
            (DESFIRE_READER, vec![2, 0x60], 2, vec![]),
            (DESFIRE_READER, vec![0], 0x68, vec![]),
            (SET_MODE, vec![0], 0x76, vec![]),
        ]);
        client.capabilities.insert(DESFIRE_READER);
        let snapshot = client.desfire_capture(false, &mut |_| {}).unwrap();
        assert!(!snapshot.complete);
        assert_eq!(snapshot.uid, uid);
        let error = snapshot.error.unwrap();
        assert!(error.contains("Card communication error"));
        assert!(error.contains("cleanup failed"));
        drop(client);
        worker.join().unwrap();
    }
}
