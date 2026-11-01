//! Frozen slot transfers use the same explicit image envelope as the web API.
use crate::{Client, Result, Sense, SlotImage, command::SLOT_SNAPSHOT, slot_image::geometry};
use std::sync::atomic::Ordering;

impl Client {
    pub fn supports_slot_snapshot(&self) -> bool {
        self.capabilities.contains(&SLOT_SNAPSHOT)
    }

    pub(crate) fn snapshot_slot(&mut self, index: u8, sense: Sense) -> Result<SlotImage> {
        let header = self.slot_command(SLOT_SNAPSHOT, &[index, sense.code()])?;
        if header.len() != 6 || header[..4] == [0; 4] {
            return Err("Invalid slot snapshot header".into());
        }
        let token = &header[..4];
        let total = u16::from_be_bytes([header[4], header[5]]) as usize;
        let result = (|| {
            if !(5..=4500).contains(&total) {
                return Err("Invalid slot snapshot size".into());
            }
            let mut bytes = Vec::with_capacity(total);
            while bytes.len() < total {
                let count = (total - bytes.len()).min(512);
                let mut request = token.to_vec();
                request.extend((bytes.len() as u16).to_be_bytes());
                request.extend((count as u16).to_be_bytes());
                let chunk = self.slot_command(SLOT_SNAPSHOT, &request)?;
                if chunk.len() != count {
                    return Err("Truncated slot snapshot".into());
                }
                bytes.extend(chunk);
            }
            let image = decode(&bytes)?;
            if image.sense()? != sense {
                return Err("Slot snapshot returned the wrong HF/LF image".into());
            }
            Ok(image)
        })();
        // Release the shared capture after cancellation too, but never send another
        // command over a broken serial session. RF already resumed before transfer.
        if self.usable {
            let cancelled = self.cancelled.swap(false, Ordering::Relaxed);
            let released = self.slot_command(SLOT_SNAPSHOT, token);
            self.cancelled.fetch_or(cancelled, Ordering::Relaxed);
            if result.is_ok() {
                released?;
            }
        }
        result
    }
}

fn decode(bytes: &[u8]) -> Result<SlotImage> {
    if bytes.len() < 5 {
        return Err("Truncated slot image envelope".into());
    }
    let id = u16::from_be_bytes([bytes[1], bytes[2]]);
    let (unit, count, extra) = geometry(id)?;
    let mut image = SlotImage {
        version: 1,
        tag_type: id,
        identity: vec![],
        options: vec![],
        memory: vec![],
    };
    if bytes[0] == 2 && matches!(id, 1300 | 1500 | 1501 | 2000) {
        let length = u16::from_be_bytes([bytes[3], bytes[4]]) as usize;
        if bytes.len() != length + 5 {
            return Err("Truncated slot image payload".into());
        }
        image.memory = bytes[5..].to_vec();
    } else if bytes[0] == 1 && (id < 1000 || (1000..=1108).contains(&id)) {
        if bytes.len() < 9 {
            return Err("Truncated slot image fields".into());
        }
        let length = u16::from_be_bytes([bytes[4], bytes[5]]) as usize;
        let uid = bytes[6] as usize;
        let ats = bytes[7] as usize;
        let options = bytes[8] as usize;
        let hf = id >= 1000;
        let expected_options = if !hf {
            0
        } else if unit == 16 {
            7
        } else {
            5
        };
        let identity = if hf { uid + 3 + ats } else { 0 };
        if length != unit * (count + extra)
            || usize::from(bytes[3]) != if hf { unit } else { length }
            || options != expected_options
            || (hf && ![4, 7, 10].contains(&uid))
            || (!hf && (uid != 0 || ats != 0))
            || bytes.len() != 9 + identity + options + length
        {
            return Err("Invalid slot image geometry".into());
        }
        if hf {
            image.identity.push(uid as u8);
            image.identity.extend(&bytes[9..12 + uid]);
            image.identity.push(ats as u8);
            image.identity.extend(&bytes[12 + uid..9 + identity]);
        }
        let start = 9 + identity;
        image.options = bytes[start..start + options].to_vec();
        if hf && unit == 4 {
            if u16::from_be_bytes([image.options[3], image.options[4]]) as usize != count {
                return Err("Invalid Type 2 page count".into());
            }
            image.options.truncate(3);
        }
        if hf {
            let mode = if unit == 16 { 0 } else { 1 };
            if image.options[mode] == 4 {
                image.options[mode] = 3;
            }
        }
        image.memory = bytes[start + options..].to_vec();
    } else {
        return Err("Unsupported slot image envelope".into());
    }
    image.validate()?;
    Ok(image)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::{DeviceError, RequestCancelled, test_support};

    type Exchange = (u16, Vec<u8>, u16, Vec<u8>);

    fn reply(request: &[u8], response: &[u8]) -> Exchange {
        (SLOT_SNAPSHOT, request.to_vec(), 0x68, response.to_vec())
    }

    fn classic() -> Vec<u8> {
        let mut bytes = vec![1, 3, 0xeb, 16, 0x10, 0, 7, 2, 7];
        bytes.extend([4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0x18, 2, 0]);
        bytes.extend([4, 1, 1, 0, 1, 1, 2]);
        bytes.extend((0..4096).map(|value| value as u8));
        bytes
    }

    #[test]
    fn image_envelopes_preserve_activation_options_and_extra_type2_pages() {
        let bytes = classic();
        let image = decode(&bytes).unwrap();
        assert_eq!(image.tag_type, 1003);
        assert_eq!(
            image.identity,
            [7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0x18, 2, 2, 0]
        );
        assert_eq!(image.options, [3, 1, 1, 0, 1, 1, 2]);
        assert_eq!(image.memory, bytes[28..]);

        let mut bytes = vec![1, 4, 0x51, 4, 0, 132, 7, 0, 5];
        bytes.extend([4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0]);
        bytes.extend([1, 4, 1, 0, 20]);
        bytes.extend(0..132);
        let image = decode(&bytes).unwrap();
        assert_eq!(image.tag_type, 1105);
        assert_eq!(image.options, [1, 3, 1]);
        assert_eq!(image.memory, (0..132).collect::<Vec<_>>());
        assert_eq!(image.identity, [7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0]);
        bytes[23] = 21;
        assert!(decode(&bytes).is_err());

        let bytes = [1, 0, 100, 5, 0, 5, 0, 0, 0, 1, 2, 3, 4, 5];
        let image = decode(&bytes).unwrap();
        assert_eq!(image.memory, [1, 2, 3, 4, 5]);
        assert!(image.identity.is_empty() && image.options.is_empty());
    }

    #[test]
    fn v2_envelopes_keep_legacy_payloads_and_reject_truncated_or_unknown_images() {
        for id in [1300u16, 1500, 1501, 2000] {
            let mut payload = match id {
                1300 => vec![
                    7, 4, 1, 2, 3, 4, 5, 6, 0, 10, 0, 0, b'D', b'F', b'U', b'1', 0x44, 3, 0x20, 2,
                    2, 0, 0, 0,
                ],
                1500 => vec![0; 112],
                1501 => vec![0; 192],
                _ => vec![0; 77],
            };
            if id == 2000 {
                payload[5..8].copy_from_slice(&[0x33, 2, 0xd0]);
            } else if matches!(id, 1500 | 1501) {
                payload[..4].copy_from_slice(&[0xe0, 4, if id == 1500 { 3 } else { 1 }, 0x10]);
            }
            let mut bytes = vec![2];
            bytes.extend(id.to_be_bytes());
            bytes.extend((payload.len() as u16).to_be_bytes());
            bytes.extend(&payload);
            assert_eq!(decode(&bytes).unwrap().memory, payload);
            for end in 0..bytes.len() {
                assert!(decode(&bytes[..end]).is_err());
            }
            bytes[0] = 3;
            assert!(decode(&bytes).is_err());
        }
        let bytes = classic();
        for end in 0..bytes.len() {
            assert!(decode(&bytes[..end]).is_err());
        }
    }

    #[test]
    fn frozen_transfer_reads_all_chunks_then_releases_without_mode_or_slot_changes() {
        let bytes = classic();
        let mut requests = vec![reply(&[2, 2], &[0, 0, 0, 1, 0x10, 0x1c])];
        for (index, chunk) in bytes.chunks(512).enumerate() {
            let mut request = vec![0, 0, 0, 1];
            request.extend((index as u16 * 512).to_be_bytes());
            request.extend((chunk.len() as u16).to_be_bytes());
            requests.push(reply(&request, chunk));
        }
        requests.push(reply(&[0, 0, 0, 1], &[]));
        let (mut client, worker) = test_support::device(requests);
        client.capabilities.insert(SLOT_SNAPSHOT);
        assert_eq!(
            client.read_slot_image(3, Sense::Hf).unwrap(),
            decode(&bytes).unwrap()
        );
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn busy_open_sends_no_fallback_or_mode_change() {
        let (mut client, worker) =
            test_support::device(vec![(SLOT_SNAPSHOT, vec![0, 2], 0x76, vec![])]);
        client.capabilities.insert(SLOT_SNAPSHOT);
        assert_eq!(
            client
                .read_slot_image(1, Sense::Hf)
                .unwrap_err()
                .downcast_ref::<DeviceError>()
                .unwrap()
                .0,
            0x76
        );
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn transfer_errors_release_the_capture_and_keep_the_original_error() {
        for (status, chunk) in [(0x68, vec![1]), (0x76, vec![])] {
            let (mut client, worker) = test_support::device(vec![
                reply(&[0, 1], &[0, 0, 0, 3, 0, 14]),
                (SLOT_SNAPSHOT, vec![0, 0, 0, 3, 0, 0, 0, 14], status, chunk),
                reply(&[0, 0, 0, 3], &[]),
            ]);
            client.capabilities.insert(SLOT_SNAPSHOT);
            let error = client.read_slot_image(1, Sense::Lf).unwrap_err();
            if status == 0x76 {
                assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 0x76);
            } else {
                assert!(error.to_string().contains("Truncated slot snapshot"));
            }
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn cancellation_after_open_releases_without_consuming_any_chunk() {
        use std::sync::{Arc, atomic::AtomicBool};

        let cancelled = Arc::new(AtomicBool::new(false));
        let hook_flag = cancelled.clone();
        let (mut client, worker) = test_support::device_with_hook(
            vec![
                reply(&[0, 1], &[0, 0, 0, 3, 0, 14]),
                reply(&[0, 0, 0, 3], &[]),
            ],
            move |_| {
                hook_flag.store(true, Ordering::Relaxed);
            },
        );
        client.cancelled = cancelled.clone();
        client.capabilities.insert(SLOT_SNAPSHOT);
        assert!(
            client
                .read_slot_image(1, Sense::Lf)
                .unwrap_err()
                .is::<RequestCancelled>()
        );
        assert!(cancelled.load(Ordering::Relaxed));
        drop(client);
        worker.join().unwrap();
    }
}
