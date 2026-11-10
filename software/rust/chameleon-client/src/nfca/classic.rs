//! Bounded, UID-checked Classic sector reads.
use crate::{Reader, Result, command::*};
use nfc_native::{autopwn::RecoverySource, formats::image::classic::blocks};
use std::time::{Duration, Instant};

#[derive(Debug, PartialEq, Eq)]
pub(super) enum ClassicBatchResult {
    Authenticated,
    Read(Vec<Option<[u8; 16]>>),
    Rejected,
    Failed(u16),
}

impl Reader<'_> {
    /// UID-checked operations, bounded by the same response limit as the web API.
    pub(super) fn classic_batch(
        &mut self,
        reading: bool,
        sources: &[RecoverySource],
    ) -> Result<Vec<ClassicBatchResult>> {
        if sources.is_empty() || sources.len() > 8 || !matches!(self.card.uid.len(), 4 | 7 | 10) {
            return Err("Invalid Classic batch request".into());
        }
        let mut response_size = 3;
        let mut payload = vec![1, u8::from(reading), self.card.uid.len() as u8];
        payload.extend(&self.card.uid);
        payload.push(sources.len() as u8);
        for source in sources {
            self.validate_sector(source.sector)?;
            response_size += if reading {
                6 + blocks(source.sector) * 16
            } else {
                11
            };
            payload.extend([
                if reading {
                    source.sector as u8
                } else {
                    nfc_native::formats::image::classic::first_block(source.sector)
                },
                if source.is_key_b { 0x61 } else { 0x60 },
            ]);
            payload.extend(source.key);
        }
        if response_size > 512 {
            return Err("Classic batch response exceeds 512 bytes".into());
        }
        if self.reading_memory != reading {
            self.close_session()?;
        }
        self.reading_memory = reading;
        self.session = Some(Instant::now());
        let result = self
            .client
            .command(CLASSIC_BATCH, &payload, Duration::from_secs(15))
            .and_then(|bytes| decode_batch(&bytes, reading, sources));
        if result.is_err() {
            let cleanup = self.close_session();
            return crate::with_cleanup(result, cleanup);
        }
        result
    }

    /// Retain the bounded authentication lease across key checks.
    pub(super) fn verify_retained(
        &mut self,
        sector: usize,
        key_b: bool,
        candidates: &[[u8; 6]],
        progress: impl FnMut(usize, usize),
    ) -> Result<Option<[u8; 6]>> {
        let result = self.verify_keys(sector, key_b, candidates, progress, true);
        if result.is_err() {
            let cleanup = self.close_session();
            return crate::with_cleanup(result, cleanup);
        }
        result
    }

    /// One entry per block; unread blocks stay unknown and can be retried separately.
    pub fn sector(&mut self, source: &RecoverySource) -> Result<Vec<Option<[u8; 16]>>> {
        self.validate_sector(source.sector)?;
        if !matches!(self.card.uid.len(), 4 | 7 | 10) {
            return Err("Invalid Classic UID length".into());
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

fn decode_batch(
    bytes: &[u8],
    reading: bool,
    sources: &[RecoverySource],
) -> Result<Vec<ClassicBatchResult>> {
    if bytes.len() < 3
        || bytes[0] != 1
        || bytes[1] != u8::from(reading)
        || bytes[2] == 0
        || usize::from(bytes[2]) > sources.len()
    {
        return Err("Invalid Classic batch reply".into());
    }
    let mut offset = 3;
    let mut results = Vec::new();
    for source in sources.iter().take(usize::from(bytes[2])) {
        let header = bytes
            .get(offset..offset + 4)
            .ok_or("Truncated Classic batch header")?;
        let status = u16::from_be_bytes([header[0], header[1]]);
        let size = usize::from(u16::from_be_bytes([header[2], header[3]]));
        offset += 4;
        let data = bytes
            .get(offset..offset + size)
            .ok_or("Truncated Classic batch result")?;
        offset += size;
        let result = match status {
            0 if reading => ClassicBatchResult::Read(decode_sector(data, blocks(source.sector))?),
            0 | 6 if !reading => {
                if data.len() != 7
                    || data[0] != u8::from(status == 0)
                    || (status == 0 && data[1..] != source.key)
                {
                    return Err("Invalid batched authentication result".into());
                }
                if status == 0 {
                    ClassicBatchResult::Authenticated
                } else {
                    ClassicBatchResult::Rejected
                }
            }
            6 => ClassicBatchResult::Rejected,
            status => ClassicBatchResult::Failed(status),
        };
        results.push(result);
    }
    if offset != bytes.len()
        || (results.len() != sources.len()
            && !matches!(results.last(), Some(ClassicBatchResult::Failed(_))))
    {
        return Err("Incomplete Classic batch reply".into());
    }
    Ok(results)
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
        capture.extend([0x60, 0, 0, 1, 4, 1, 2, 3, 4]);
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

    fn batch_request(reading: bool, sources: &[RecoverySource]) -> Vec<u8> {
        let mut bytes = vec![1, u8::from(reading), 4, 1, 2, 3, 4, sources.len() as u8];
        for source in sources {
            bytes.extend([
                if reading {
                    source.sector as u8
                } else {
                    source.sector as u8 * 4
                },
                if source.is_key_b { 0x61 } else { 0x60 },
            ]);
            bytes.extend(source.key);
        }
        bytes
    }

    fn batch_reply(reading: bool, records: &[(u16, Vec<u8>)]) -> Vec<u8> {
        let mut bytes = vec![1, u8::from(reading), records.len() as u8];
        for (status, data) in records {
            bytes.extend(status.to_be_bytes());
            bytes.extend((data.len() as u16).to_be_bytes());
            bytes.extend(data);
        }
        bytes
    }

    #[test]
    fn batch_checks_identity_credentials_lengths_masks_and_partial_card_loss() {
        let sources = [source(0), source(1)];
        let auth = batch_reply(
            false,
            &[(0, vec![1, 255, 255, 255, 255, 255, 255]), (6, vec![0; 7])],
        );
        assert_eq!(
            decode_batch(&auth, false, &sources).unwrap(),
            [
                ClassicBatchResult::Authenticated,
                ClassicBatchResult::Rejected
            ]
        );
        let lost = batch_reply(true, &[(0, reply(4, 5)), (1, vec![])]);
        let decoded = decode_batch(&lost, true, &sources).unwrap();
        assert!(
            matches!(&decoded[0], ClassicBatchResult::Read(blocks) if blocks[0].is_some() && blocks[1].is_none())
        );
        assert_eq!(decoded[1], ClassicBatchResult::Failed(1));
        let short = batch_reply(false, &[(6, vec![0; 7])]);
        assert!(decode_batch(&short, false, &sources).is_err());
        for length in [0, 3, auth.len() - 1] {
            assert!(decode_batch(&auth[..length], false, &sources).is_err());
        }
        let mut wrong_key = auth.clone();
        wrong_key[8] = 0;
        assert!(decode_batch(&wrong_key, false, &sources).is_err());
        assert!(
            decode_batch(
                &batch_reply(true, &[(0, reply(4, 16))]),
                true,
                &sources[..1]
            )
            .is_err()
        );
        assert!(decode_batch(&batch_reply(false, &[(1, vec![])]), false, &sources).is_ok());
    }

    #[test]
    fn serial_batches_are_uid_checked_bounded_and_release_rf() {
        let sources = [source(0), source(1)];
        let (mut client, worker) = device(vec![
            (
                CLASSIC_BATCH,
                batch_request(false, &sources),
                0,
                batch_reply(
                    false,
                    &[(0, vec![1, 255, 255, 255, 255, 255, 255]), (6, vec![0; 7])],
                ),
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (
                CLASSIC_BATCH,
                batch_request(true, &sources),
                0,
                batch_reply(true, &[(0, reply(4, 15)), (0, reply(4, 3))]),
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        client.capabilities.insert(CLASSIC_BATCH);
        {
            let mut reader = reader(&mut client);
            assert!(reader.classic_batch(true, &[]).is_err());
            assert!(reader.classic_batch(true, &vec![source(0); 8]).is_err());
            assert_eq!(
                reader.classic_batch(false, &sources).unwrap()[1],
                ClassicBatchResult::Rejected
            );
            assert!(
                matches!(&reader.classic_batch(true, &sources).unwrap()[1], ClassicBatchResult::Read(blocks) if blocks.iter().filter(|b| b.is_some()).count() == 2)
            );
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn retained_checks_reuse_the_lease_and_reopen_after_idle() {
        let payload = vec![0, 0x60, 1, 255, 255, 255, 255, 255, 255];
        let (mut client, worker) = device(vec![
            (NFCA_SESSION, vec![1], 0, vec![]),
            (CHECK_KEYS, payload.clone(), 6, vec![0; 7]),
            (
                CHECK_KEYS,
                payload.clone(),
                0,
                vec![1, 255, 255, 255, 255, 255, 255],
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (NFCA_SESSION, vec![1], 0, vec![]),
            (CHECK_KEYS, payload, 6, vec![0; 7]),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        client.capabilities.insert(NFCA_SESSION);
        {
            let mut reader = reader(&mut client);
            assert!(
                reader
                    .verify_retained(0, false, &[[255; 6]], |_, _| {})
                    .unwrap()
                    .is_none()
            );
            assert!(
                reader
                    .verify_retained(0, false, &[[255; 6]], |_, _| {})
                    .unwrap()
                    .is_some()
            );
            reader.session = Some(Instant::now() - Duration::from_secs(1));
            assert!(
                reader
                    .verify_retained(0, false, &[[255; 6]], |_, _| {})
                    .unwrap()
                    .is_none()
            );
        }
        drop(client);
        worker.join().unwrap();
    }
}
