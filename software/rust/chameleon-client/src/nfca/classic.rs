//! Bounded, UID-checked Classic sector reads.
use crate::{DeviceError, Reader, Result, command::*};
use nfc_native::{autopwn::RecoverySource, formats::image::classic::blocks};
use std::time::{Duration, Instant};

impl Reader<'_> {
    /// One entry per block; unread blocks stay unknown and can be retried separately.
    pub fn sector(&mut self, source: &RecoverySource) -> Result<Vec<Option<[u8; 16]>>> {
        self.validate_sector(source.sector)?;
        if !matches!(self.card.uid.len(), 4 | 7 | 10) {
            return Err("Invalid Classic UID length".into());
        }
        if !self.client.capabilities.contains(&READ_SECTOR) {
            return Err(DeviceError(0x69).into());
        }
        if !self.reading_memory {
            self.close_session()?;
        }
        let mut payload = vec![
            if source.is_key_b { 0x61 } else { 0x60 },
            source.sector as u8,
        ];
        payload.extend(source.key);
        payload.push(self.card.uid.len() as u8);
        payload.extend(&self.card.uid);
        // Command 2026 closes either RF lease. Mark cleanup before sending.
        self.reading_memory = true;
        self.session = Some(Instant::now());
        let result = self
            .client
            .command(READ_SECTOR, &payload, Duration::from_secs(3))
            .and_then(|bytes| decode_sector(&bytes, blocks(source.sector)));
        if result.is_err() {
            let cleanup = self.close_session();
            return crate::with_cleanup(result, cleanup);
        }
        result
    }
}

fn decode_sector(bytes: &[u8], count: usize) -> Result<Vec<Option<[u8; 16]>>> {
    if bytes.len() != 2 + count * 16 {
        return Err("Invalid sector reply length".into());
    }
    let mask = u16::from_be_bytes([bytes[0], bytes[1]]);
    if u32::from(mask) >> count != 0 {
        return Err("Invalid sector block mask".into());
    }
    Ok(bytes[2..]
        .chunks_exact(16)
        .enumerate()
        .map(|(index, block)| (mask & (1 << index) != 0).then(|| block.try_into().unwrap()))
        .collect())
}

#[cfg(all(test, unix))]
mod tests {
    use super::super::tests::reader;
    use super::*;
    use crate::test_support::{device, device_with_hook};
    use nfc_native::RecoveryReader;
    use std::sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    };

    fn source(sector: usize) -> RecoverySource {
        RecoverySource {
            sector,
            is_key_b: false,
            key: [0xff; 6],
        }
    }
    fn request(sector: u8, keep: bool) -> Vec<u8> {
        let mut bytes = vec![0x60, sector];
        bytes.extend([0xff; 6]);
        if keep {
            bytes.extend([4, 1, 2, 3, 4]);
        }
        bytes
    }
    fn reply(count: usize, mask: u16) -> Vec<u8> {
        let mut bytes = mask.to_be_bytes().to_vec();
        for index in 0..count {
            bytes.extend([index as u8; 16]);
        }
        bytes
    }

    #[test]
    fn sector_masks_preserve_unknown_blocks_and_validate_large_sectors() {
        let result = decode_sector(&reply(4, 5), 4).unwrap();
        assert_eq!(result, vec![Some([0; 16]), None, Some([2; 16]), None]);
        assert!(decode_sector(&reply(4, 16), 4).is_err());
        assert!(decode_sector(&reply(4, 15)[..65], 4).is_err());
        assert_eq!(decode_sector(&reply(16, 65535), 16).unwrap().len(), 16);
    }

    #[test]
    fn missing_sector_command_leaves_legacy_block_reads_available() {
        let (mut client, worker) = device(vec![(READ_BLOCK, request(0, false), 0, vec![7; 16])]);
        {
            let mut reader = reader(&mut client);
            let error = reader.sector(&source(0)).unwrap_err();
            assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 0x69);
            assert_eq!(reader.block(&source(0), 0).unwrap(), vec![7; 16]);
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn sector_capability_reads_small_and_large_sectors_and_checks_geometry() {
        let (mut client, worker) = device(vec![
            (READ_SECTOR, request(0, true), 0, reply(4, 15)),
            (READ_SECTOR, request(39, true), 0, reply(16, 65535)),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        client.capabilities.insert(READ_SECTOR);
        {
            let mut reader = reader(&mut client);
            assert!(reader.sector(&source(16)).is_err());
            assert_eq!(reader.sector(&source(0)).unwrap().len(), 4);
            reader.card.sak = 0x18;
            assert_eq!(reader.sector(&source(39)).unwrap().len(), 16);
            assert!(reader.sector(&source(40)).is_err());
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn sector_reads_share_the_lease_and_close_before_individual_fallback() {
        let (mut client, worker) = device(vec![
            (READ_SECTOR, request(0, true), 0, reply(4, 15)),
            (READ_SECTOR, request(1, true), 0, reply(4, 1)),
            (NFCA_SESSION, vec![0], 0x68, vec![]),
            (READ_BLOCK, request(5, false), 0, vec![5; 16]),
        ]);
        client.capabilities.insert(READ_SECTOR);
        {
            let mut reader = reader(&mut client);
            assert!(
                reader
                    .sector(&source(0))
                    .unwrap()
                    .iter()
                    .all(Option::is_some)
            );
            assert!(reader.sector(&source(1)).unwrap()[1].is_none());
            assert_eq!(reader.block(&source(1), 5).unwrap(), vec![5; 16]);
            reader.end().unwrap();
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn cancelled_read_retains_completed_sector_and_releases_rf() {
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();
        let (mut client, worker) = device_with_hook(
            vec![
                (READ_SECTOR, request(0, true), 0, reply(4, 15)),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ],
            move |command| {
                if command == READ_SECTOR {
                    flag.store(true, Ordering::Relaxed);
                }
            },
        );
        client.cancelled = cancelled;
        client.capabilities.insert(READ_SECTOR);
        {
            let mut reader = reader(&mut client);
            assert!(
                reader
                    .sector(&source(0))
                    .unwrap()
                    .iter()
                    .all(Option::is_some)
            );
            assert!(reader.sector(&source(1)).is_err());
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn malformed_or_rejected_sector_reads_release_rf() {
        for (status, bytes) in [(0, reply(4, 16)), (6, vec![])] {
            let (mut client, worker) = device(vec![
                (READ_SECTOR, request(0, true), status, bytes),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ]);
            client.capabilities.insert(READ_SECTOR);
            assert!(reader(&mut client).sector(&source(0)).is_err());
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn reading_then_capture_reopens_the_capture_lease_once() {
        let mut capture = request(0, false);
        capture.extend([0x60, 0, 0, 1]);
        let (mut client, worker) = device(vec![
            (READ_SECTOR, request(0, true), 0, reply(4, 15)),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (NFCA_SESSION, vec![1], 0, vec![]),
            (COLLECT_OBSERVATIONS, capture.clone(), 0, vec![0; 12]),
            (COLLECT_OBSERVATIONS, capture, 0, vec![0; 12]),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        client.capabilities.insert(NFCA_SESSION);
        client.capabilities.insert(READ_SECTOR);
        {
            let mut reader = reader(&mut client);
            reader.sector(&source(0)).unwrap();
            let target = nfc_native::autopwn::RecoveryTarget {
                sector: 0,
                is_key_b: false,
            };
            for _ in 0..2 {
                assert_eq!(
                    reader.collect(&source(0), &target, false, 1).unwrap().len(),
                    1
                );
            }
        }
        drop(client);
        worker.join().unwrap();
    }
}
