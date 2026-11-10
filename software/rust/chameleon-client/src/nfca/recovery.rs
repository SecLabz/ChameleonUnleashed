//! Batched Classic recovery with per-read memory and nonce reuse.
use super::{Reader, classic::ClassicBatchResult};
use crate::{DeviceError, Result};
use nfc_native::{
    RecoveryProgress, RecoveryReader, Solvers,
    autopwn::*,
    formats::image::classic::{blocks, first_block, key_b_readable, trailer},
    recovery::{DarksideObservation, Observation},
};
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, atomic::AtomicBool},
};

type Slot = (usize, bool);
type Credential = (usize, bool, [u8; 6]);
fn credential(source: &RecoverySource) -> Credential {
    (source.sector, source.is_key_b, source.key)
}

pub struct ClassicRecovery<'a, 'b> {
    reader: &'a mut Reader<'b>,
    memory: Vec<Option<Vec<u8>>>,
    sector_reads: HashSet<Credential>,
    replies: HashMap<Credential, bool>,
    reuse_capture: Option<Slot>,
    weak_prefix: HashMap<Slot, Vec<Observation>>,
    recovering: Option<Slot>,
}

impl<'a, 'b> ClassicRecovery<'a, 'b> {
    pub fn new(reader: &'a mut Reader<'b>) -> Result<Self> {
        let count = reader.card.model()?.classic().unwrap().1;
        if ![
            crate::command::CLASSIC_BATCH,
            crate::command::READ_SECTOR,
            crate::command::NFCA_SESSION,
        ]
        .iter()
        .all(|command| reader.client.capabilities.contains(command))
        {
            return Err("AutoPwn requires firmware 2.1.0 or later; update the Ultra".into());
        }
        Ok(Self {
            reader,
            memory: vec![None; count],
            sector_reads: HashSet::new(),
            replies: HashMap::new(),
            reuse_capture: None,
            weak_prefix: HashMap::new(),
            recovering: None,
        })
    }

    pub fn into_memory(self) -> Vec<Option<Vec<u8>>> {
        self.memory
    }

    fn remember_sector(&mut self, source: &RecoverySource, observed: Vec<Option<[u8; 16]>>) {
        self.sector_reads.insert(credential(source));
        let first = usize::from(first_block(source.sector));
        for (slot, block) in self.memory[first..].iter_mut().zip(observed) {
            if let Some(block) = block {
                *slot = Some(block.to_vec());
            }
        }
    }

    fn capture_sector(&mut self, source: &RecoverySource) -> Result<()> {
        if !self.sector_reads.contains(&credential(source)) {
            let observed = self.reader.sector(source)?;
            self.remember_sector(source, observed);
        }
        Ok(())
    }

    fn extract_trailer(&mut self, sector: usize, key: [u8; 6]) -> Result<Option<[u8; 6]>> {
        let source = RecoverySource {
            sector,
            is_key_b: false,
            key,
        };
        match self.capture_sector(&source) {
            Ok(()) => (),
            Err(error) if read_denied(&*error) => return Ok(None),
            Err(error) => return Err(error),
        }
        let index = usize::from(trailer(sector));
        if self.memory[index].is_none() {
            match self.reader.block(&source, index as u8) {
                Ok(block) => self.memory[index] = Some(block),
                Err(error) if read_denied(&*error) => return Ok(None),
                Err(error) => return Err(error),
            }
        }
        let block = self.memory[index].as_ref().unwrap();
        Ok(key_b_readable(block).then(|| block[10..16].try_into().unwrap()))
    }

    pub fn execute(
        &mut self,
        solvers: &mut Solvers,
        action: &AutoPwnAction,
        engine: &AutoPwnSession,
        dictionary_progress: impl FnMut(usize, bool, usize, usize),
    ) -> Result<AutoPwnActionResult> {
        if self.is_cancelled() {
            return Err(crate::RequestCancelled.into());
        }
        self.recovering = match action {
            AutoPwnAction::RecoverNested { target, .. } => Some((target.sector, target.is_key_b)),
            _ => None,
        };
        if let AutoPwnAction::SweepKey {
            sector,
            is_key_b,
            key,
        } = action
        {
            return self.sweep(solvers, engine, *sector, *is_key_b, *key);
        }
        nfc_native::execute(
            self,
            solvers,
            action,
            engine,
            dictionary_progress,
            |reader, sector, key| reader.extract_trailer(sector, key),
        )
    }

    fn sweep(
        &mut self,
        solvers: &mut Solvers,
        engine: &AutoPwnSession,
        sector: usize,
        is_key_b: bool,
        key: [u8; 6],
    ) -> Result<AutoPwnActionResult> {
        let id = (sector, is_key_b, key);
        let reply = |hit| AutoPwnActionResult::Sweep {
            hit,
            physically_tested: true,
        };
        if let Some(hit) = self.replies.remove(&id) {
            return Ok(reply(hit));
        }
        let summary = engine.build_summary(false);
        if let Some(source) = engine
            .nonce_reuse_source(sector)
            .or_else(|| reuse_source(&summary, sector))
        {
            self.reuse_capture = (summary.prng == PrngMode::Weak).then_some((sector, is_key_b));
            let result =
                solvers.reuse_key(self, &source, &RecoveryTarget { sector, is_key_b }, &key);
            self.reuse_capture = None;
            let (hit, physically_tested) = result?;
            return Ok(AutoPwnActionResult::Sweep {
                hit,
                physically_tested,
            });
        }
        let mut sources = vec![RecoverySource {
            sector,
            is_key_b,
            key,
        }];
        for (index, keys) in summary.mapping.iter().enumerate() {
            let target = RecoveryTarget {
                sector: index,
                is_key_b,
            };
            if index != sector
                && (if is_key_b { keys.key_b } else { keys.key_a }).is_none()
                && !self.replies.contains_key(&(index, is_key_b, key))
                && !engine.rejected_keys(&target).contains(&key)
            {
                sources.push(RecoverySource {
                    sector: index,
                    is_key_b,
                    key,
                });
            }
        }
        sources.truncate(8);
        let replies = self.reader.classic_batch(false, &sources)?;
        let mut hits = Vec::new();
        for (source, result) in sources.iter().zip(replies) {
            let hit = match result {
                ClassicBatchResult::Authenticated => true,
                ClassicBatchResult::Rejected => false,
                ClassicBatchResult::Failed(status) => {
                    return Err(DeviceError(status).into());
                }
                ClassicBatchResult::Read(_) => unreachable!(),
            };
            self.replies.insert(credential(source), hit);
            if hit && !is_key_b {
                hits.push(source.clone());
            }
        }
        self.read_hits(&hits)?;
        Ok(reply(self.replies.remove(&id).unwrap()))
    }

    fn read_hits(&mut self, hits: &[RecoverySource]) -> Result<()> {
        let mut offset = 0;
        while offset < hits.len() {
            let mut size = 3;
            let count = hits[offset..]
                .iter()
                .take_while(|source| {
                    size += 6 + blocks(source.sector) * 16;
                    size <= 512
                })
                .count()
                .min(8);
            let sources = &hits[offset..offset + count];
            let replies = self.reader.classic_batch(true, sources)?;
            for (source, reply) in sources.iter().zip(replies) {
                match reply {
                    ClassicBatchResult::Read(observed) => self.remember_sector(source, observed),
                    ClassicBatchResult::Rejected => (), // The earlier authentication remains valid.
                    ClassicBatchResult::Failed(status) => return Err(DeviceError(status).into()),
                    ClassicBatchResult::Authenticated => unreachable!(),
                }
            }
            offset += count;
        }
        Ok(())
    }

    pub fn read_memory(&mut self, mapping: &[SectorKeys]) -> Result<()> {
        for (sector, keys) in mapping.iter().enumerate() {
            let first = usize::from(first_block(sector));
            let count = blocks(sector);
            for (is_key_b, key) in [(false, keys.key_a), (true, keys.key_b)] {
                if self.memory[first..first + count]
                    .iter()
                    .all(Option::is_some)
                {
                    break;
                }
                if is_key_b && keys.origin_b == Some(KeyOrigin::Trailer) {
                    continue;
                }
                let Some(key) = key else {
                    continue;
                };
                let source = RecoverySource {
                    sector,
                    is_key_b,
                    key,
                };
                match self.capture_sector(&source) {
                    Ok(()) => (),
                    Err(error) if read_denied(&*error) => continue,
                    Err(error) => return Err(error),
                }
                for index in first..first + count {
                    if self.memory[index].is_none() {
                        match self.reader.block(&source, index as u8) {
                            Ok(block) => self.memory[index] = Some(block),
                            Err(error) if read_denied(&*error) => (),
                            Err(error) => return Err(error),
                        }
                    }
                }
            }
        }
        Ok(())
    }
}

fn read_denied(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<DeviceError>()
        .is_some_and(|e| matches!(e.0, 2 | 6))
}

fn reuse_source(summary: &AutoPwnSummary, target: usize) -> Option<RecoverySource> {
    if summary.prng != PrngMode::Weak {
        return None;
    }
    let unique: HashSet<_> = summary
        .mapping
        .iter()
        .flat_map(|keys| [keys.key_a, keys.key_b])
        .flatten()
        .collect();
    if unique.len() < 3 {
        return None;
    }
    let source = |sector: usize| {
        let keys = &summary.mapping[sector];
        keys.key_a
            .map(|key| RecoverySource {
                sector,
                is_key_b: false,
                key,
            })
            .or_else(|| {
                keys.key_b
                    .filter(|_| keys.origin_b != Some(KeyOrigin::Trailer))
                    .map(|key| RecoverySource {
                        sector,
                        is_key_b: true,
                        key,
                    })
            })
    };
    source(target).or_else(|| (0..summary.mapping.len()).find_map(source))
}

impl RecoveryReader for ClassicRecovery<'_, '_> {
    fn probe_prng(&mut self) -> Result<PrngMode> {
        self.reader.probe_prng()
    }
    fn progress(&mut self, event: RecoveryProgress) {
        self.reader.progress(event);
    }
    fn uid(&self) -> &[u8] {
        &self.reader.card.uid
    }
    fn end(&mut self) -> Result<()> {
        self.reader.end()
    }
    fn collect(
        &mut self,
        source: &RecoverySource,
        target: &RecoveryTarget,
        scheduled: bool,
        count: usize,
    ) -> Result<Vec<Observation>> {
        if self.is_cancelled() {
            return Err(crate::RequestCancelled.into());
        }
        let slot = (target.sector, target.is_key_b);
        let mut prefix = Vec::new();
        if self.recovering == Some(slot)
            && let Some(records) = self.weak_prefix.get_mut(&slot)
        {
            prefix.extend(records.drain(..count.min(records.len())));
        }
        if prefix.len() < count {
            prefix.extend(
                self.reader
                    .collect(source, target, scheduled, count - prefix.len())?,
            );
        }
        if self.reuse_capture == Some(slot) {
            self.weak_prefix.insert(slot, prefix.clone());
        }
        Ok(prefix)
    }
    fn collect_static(
        &mut self,
        source: &RecoverySource,
        target: &RecoveryTarget,
        chained: bool,
        count: usize,
    ) -> Result<Vec<Observation>> {
        self.reader.collect_static(source, target, chained, count)
    }
    fn darkside(
        &mut self,
        target: &RecoveryTarget,
        variant: u8,
    ) -> Result<Option<DarksideObservation>> {
        self.reader.darkside(target, variant)
    }
    fn verify_progress(
        &mut self,
        sector: usize,
        key_b: bool,
        candidates: &[[u8; 6]],
        mut progress: impl FnMut(usize, usize),
    ) -> Result<Option<[u8; 6]>> {
        self.reader.validate_sector(sector)?;
        if self.is_cancelled() {
            return Err(crate::RequestCancelled.into());
        }
        if candidates.len() == 1 {
            let source = RecoverySource {
                sector,
                is_key_b: key_b,
                key: candidates[0],
            };
            let first = usize::from(first_block(sector));
            if self.memory[first..first + blocks(sector)]
                .iter()
                .any(Option::is_none)
            {
                match self.capture_sector(&source) {
                    Ok(()) => {
                        progress(1, 1);
                        return Ok(Some(candidates[0]));
                    }
                    Err(error)
                        if error
                            .downcast_ref::<DeviceError>()
                            .is_some_and(|e| e.0 == 6) =>
                    {
                        progress(1, 1);
                        return Ok(None);
                    }
                    Err(error) => return Err(error),
                }
            }
        }
        self.reader
            .verify_retained(sector, key_b, candidates, progress)
    }
    fn cancellation_flag(&self) -> Arc<AtomicBool> {
        self.reader.cancellation_flag()
    }
    fn is_cancelled(&self) -> bool {
        self.reader.is_cancelled()
    }
    fn capture_unavailable(&self, error: &(dyn std::error::Error + 'static)) -> bool {
        self.reader.capture_unavailable(error)
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::{
        command::*,
        nfca::tests::reader,
        test_support::{device as serial_device, device_with_hook as serial_device_with_hook},
    };
    use std::sync::atomic::Ordering;

    fn device(
        exchanges: Vec<(u16, Vec<u8>, u16, Vec<u8>)>,
    ) -> (crate::Client, std::thread::JoinHandle<()>) {
        let (mut client, worker) = serial_device(exchanges);
        client
            .capabilities
            .extend([CLASSIC_BATCH, READ_SECTOR, NFCA_SESSION]);
        (client, worker)
    }
    fn device_with_hook(
        exchanges: Vec<(u16, Vec<u8>, u16, Vec<u8>)>,
        hook: impl FnMut(u16) + Send + 'static,
    ) -> (crate::Client, std::thread::JoinHandle<()>) {
        let (mut client, worker) = serial_device_with_hook(exchanges, hook);
        client
            .capabilities
            .extend([CLASSIC_BATCH, READ_SECTOR, NFCA_SESSION]);
        (client, worker)
    }
    fn engine(prng: PrngMode) -> AutoPwnSession {
        AutoPwnSession::try_new(AutoPwnConfig {
            uid: vec![1, 2, 3, 4],
            sector_count: 5,
            first_sector: 0,
            last_sector: 4,
            prng,
            budget: 4096,
        })
        .unwrap()
    }
    fn sector_reply(mask: u8) -> Vec<u8> {
        let mut bytes = vec![0, mask];
        bytes.extend([0; 64]);
        bytes[50..66].copy_from_slice(&[
            0, 0, 0, 0, 0, 0, 255, 7, 128, 105, 255, 255, 255, 255, 255, 255,
        ]);
        bytes
    }
    fn batch(reading: bool, sectors: &[u8]) -> Vec<u8> {
        let mut bytes = vec![1, u8::from(reading), 4, 1, 2, 3, 4, sectors.len() as u8];
        for sector in sectors {
            bytes.extend([if reading { *sector } else { *sector * 4 }, 0x60]);
            bytes.extend([255; 6]);
        }
        bytes
    }
    fn batch_reply(reading: bool, count: usize) -> Vec<u8> {
        let mut bytes = vec![1, u8::from(reading), count as u8];
        for _ in 0..count {
            let data = if reading {
                sector_reply(15)
            } else {
                vec![1, 255, 255, 255, 255, 255, 255]
            };
            bytes.extend([0, 0, 0, data.len() as u8]);
            bytes.extend(data);
        }
        bytes
    }

    #[test]
    fn sweep_batches_capture_memory_once_and_acknowledge_prefetched_authentications() {
        let (mut client, worker) = device(vec![
            (
                CLASSIC_BATCH,
                batch(false, &[1, 2, 3, 4]),
                0,
                batch_reply(false, 4),
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (
                CLASSIC_BATCH,
                batch(true, &[1, 2, 3, 4]),
                0,
                batch_reply(true, 4),
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        client.capabilities.insert(CLASSIC_BATCH);
        {
            let mut reader = reader(&mut client);
            reader.card.sak = 9;
            let mut recovery = ClassicRecovery::new(&mut reader).unwrap();
            let mut engine = engine(PrngMode::Unknown);
            engine.record_verified_source(0, false, [255; 6]).unwrap();
            let mut solvers = Solvers::new(false);
            for sector in [1, 2] {
                let result = recovery
                    .execute(
                        &mut solvers,
                        &AutoPwnAction::SweepKey {
                            sector,
                            is_key_b: false,
                            key: [255; 6],
                        },
                        &engine,
                        |_, _, _, _| {},
                    )
                    .unwrap();
                assert!(matches!(
                    result,
                    AutoPwnActionResult::Sweep {
                        hit: true,
                        physically_tested: true
                    }
                ));
            }
            let result = recovery
                .execute(
                    &mut solvers,
                    &AutoPwnAction::ExtractTrailer {
                        sector: 2,
                        key_a: [255; 6],
                    },
                    &engine,
                    |_, _, _, _| {},
                )
                .unwrap();
            assert!(matches!(
                result,
                AutoPwnActionResult::TrailerKey(Some(key)) if key == [255; 6]
            ));
            let memory = recovery.into_memory();
            assert!(memory[..4].iter().all(Option::is_none));
            assert!(memory[4..].iter().all(Option::is_some));
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn partial_sector_keeps_completed_blocks_and_uses_individual_read_fallback() {
        let mut sector = vec![0x60, 0];
        sector.extend([255; 6]);
        sector.extend([4, 1, 2, 3, 4]);
        let block = |index| {
            let mut bytes = vec![0x60, index];
            bytes.extend([255; 6]);
            bytes
        };
        let (mut client, worker) = device(vec![
            (READ_SECTOR, sector, 0, sector_reply(3)),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (READ_BLOCK, block(2), 2, vec![]),
            (READ_BLOCK, block(3), 0, vec![7; 16]),
        ]);
        client.capabilities.insert(READ_SECTOR);
        {
            let mut reader = reader(&mut client);
            let mut recovery = ClassicRecovery::new(&mut reader).unwrap();
            let source = RecoverySource {
                sector: 0,
                is_key_b: false,
                key: [255; 6],
            };
            recovery.capture_sector(&source).unwrap();
            let mut mapping = vec![SectorKeys::default(); 16];
            mapping[0].key_a = Some([255; 6]);
            recovery.read_memory(&mapping).unwrap();
            let memory = recovery.into_memory();
            assert!(memory[0].is_some() && memory[1].is_some());
            assert!(memory[2].is_none());
            assert_eq!(memory[3], Some(vec![7; 16]));
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn cancellation_retains_sector_and_does_not_start_another_operation() {
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();
        let mut sector = vec![0x60, 0];
        sector.extend([255; 6]);
        sector.extend([4, 1, 2, 3, 4]);
        let (mut client, worker) = device_with_hook(
            vec![
                (READ_SECTOR, sector, 0, sector_reply(15)),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ],
            move |_| {
                flag.store(true, Ordering::Relaxed);
            },
        );
        client.capabilities.insert(READ_SECTOR);
        client.cancelled = cancelled;
        {
            let mut reader = reader(&mut client);
            let mut recovery = ClassicRecovery::new(&mut reader).unwrap();
            recovery
                .capture_sector(&RecoverySource {
                    sector: 0,
                    is_key_b: false,
                    key: [255; 6],
                })
                .unwrap();
            assert!(
                recovery
                    .execute(
                        &mut Solvers::new(false),
                        &AutoPwnAction::ProbePrng,
                        &engine(PrngMode::Unknown),
                        |_, _, _, _| {}
                    )
                    .is_err()
            );
            assert_eq!(
                recovery
                    .into_memory()
                    .iter()
                    .filter(|b| b.is_some())
                    .count(),
                4
            );
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn weak_nonce_prefix_is_consumed_without_recapturing_or_crossing_targets() {
        let source = RecoverySource {
            sector: 0,
            is_key_b: false,
            key: [255; 6],
        };
        let target = RecoveryTarget {
            sector: 1,
            is_key_b: true,
        };
        let mut payload = vec![0x60, 0];
        payload.extend([255; 6]);
        payload.extend([0x61, 4, 0, 1]);
        let (mut client, worker) = device(vec![(COLLECT_OBSERVATIONS, payload, 0, vec![0; 12])]);
        {
            let mut reader = reader(&mut client);
            let mut recovery = ClassicRecovery::new(&mut reader).unwrap();
            let observation = Observation {
                nt1: 1,
                ciphertext: 2,
                parity: [0; 4],
            };
            recovery
                .weak_prefix
                .insert((1, true), vec![observation.clone(), observation]);
            recovery.recovering = Some((1, true));
            assert_eq!(
                recovery.collect(&source, &target, true, 1).unwrap()[0].nt1,
                1
            );
            let records = recovery.collect(&source, &target, true, 2).unwrap();
            assert_eq!(records.len(), 2);
            assert_eq!(records[0].nt1, 1);
            assert_eq!(records[1].nt1, 0);
            assert!(recovery.weak_prefix[&(1, true)].is_empty());
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn weak_reuse_filters_misses_locally_and_authenticates_a_matching_key() {
        let key = [0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f];
        let mut collect = vec![0x60, 4];
        collect.extend([1; 6]);
        collect.extend([0x61, 4, 0, 2, 4, 0x53, 0xbf, 0x9c, 0x43]);
        let mut records = Vec::new();
        for _ in 0..2 {
            records.extend(0x85430819u32.to_be_bytes());
            records.extend(0xaf9a42f6u32.to_be_bytes());
            records.extend([1, 1, 0, 0]);
        }
        let (mut client, worker) = device(vec![
            (NFCA_SESSION, vec![1], 0, vec![]),
            (COLLECT_OBSERVATIONS, collect, 0, records),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (
                READ_SECTOR,
                {
                    let mut bytes = vec![0x61, 1];
                    bytes.extend(key);
                    bytes.extend([4, 0x53, 0xbf, 0x9c, 0x43]);
                    bytes
                },
                0,
                sector_reply(15),
            ),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        {
            let mut reader = reader(&mut client);
            reader.card.uid = vec![0x53, 0xbf, 0x9c, 0x43];
            reader.card.sak = 9;
            let mut recovery = ClassicRecovery::new(&mut reader).unwrap();
            let mut engine = engine(PrngMode::Weak);
            engine.record_verified_source(0, false, [255; 6]).unwrap();
            engine.record_verified_source(0, true, [0; 6]).unwrap();
            engine.record_verified_source(1, false, [1; 6]).unwrap();
            let mut solvers = Solvers::new(false);
            let miss = recovery
                .execute(
                    &mut solvers,
                    &AutoPwnAction::SweepKey {
                        sector: 1,
                        is_key_b: true,
                        key: [255; 6],
                    },
                    &engine,
                    |_, _, _, _| {},
                )
                .unwrap();
            assert!(matches!(
                miss,
                AutoPwnActionResult::Sweep {
                    hit: false,
                    physically_tested: false
                }
            ));
            let hit = recovery
                .execute(
                    &mut solvers,
                    &AutoPwnAction::SweepKey {
                        sector: 1,
                        is_key_b: true,
                        key,
                    },
                    &engine,
                    |_, _, _, _| {},
                )
                .unwrap();
            assert!(matches!(
                hit,
                AutoPwnActionResult::Sweep {
                    hit: true,
                    physically_tested: true
                }
            ));
            assert_eq!(recovery.weak_prefix[&(1, true)].len(), 2);
            solvers.disable_nonce_reuse(&RecoveryTarget {
                sector: 1,
                is_key_b: true,
            });
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn missing_required_commands_reject_recovery_before_any_radio_operation() {
        for missing in [CLASSIC_BATCH, READ_SECTOR, NFCA_SESSION] {
            let (mut client, worker) = device(vec![]);
            client
                .capabilities
                .extend([CLASSIC_BATCH, READ_SECTOR, NFCA_SESSION]);
            client.capabilities.remove(&missing);
            assert!(
                ClassicRecovery::new(&mut reader(&mut client))
                    .err()
                    .unwrap()
                    .to_string()
                    .contains("firmware 2.1.0")
            );
            drop(client);
            worker.join().unwrap();
        }
    }
}
