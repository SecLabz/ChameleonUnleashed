//! NFC-A identity, memory access and MIFARE Classic recovery.
use crate::{Client, DeviceError, Result, command::*};
mod classic;
mod recovery;
pub use recovery::ClassicRecovery;
mod type2;
use nfc_native::{
    RecoveryReader,
    autopwn::{PrngMode, RecoverySource, RecoveryTarget},
    formats::image::{Model, classic::first_block},
    recovery::{DarksideObservation, Observation},
};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

#[cfg(all(test, unix))]
mod tests;

#[derive(Clone, Debug)]
pub struct Card {
    pub uid: Vec<u8>,
    pub atqa: [u8; 2],
    pub sak: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TagCredential {
    None,
    KeyA([u8; 6]),
    KeyB([u8; 6]),
    Password([u8; 4]),
    UltralightC([u8; 16]),
}
impl TagCredential {
    fn wire(self) -> (u8, [u8; 16]) {
        let mut bytes = [0; 16];
        let (kind, key): (u8, &[u8]) = match &self {
            Self::None => (0, &[]),
            Self::KeyA(key) => (1, key),
            Self::KeyB(key) => (2, key),
            Self::Password(key) => (3, key),
            Self::UltralightC(key) => (4, key),
        };
        bytes[..key.len()].copy_from_slice(key);
        (kind, bytes)
    }
}
impl Card {
    pub fn model(&self) -> Result<Model> {
        match self.sak & !0x20 {
            0x09 => Ok(Model::ClassicMini),
            0x08 => Ok(Model::Classic1k),
            0x10 => Ok(Model::Classic2k),
            0x18 => Ok(Model::Classic4k),
            _ => Err("Card is not a supported MIFARE Classic model".into()),
        }
    }
}

pub struct Reader<'a> {
    client: &'a mut Client,
    pub card: Card,
    previous_mode: u8,
    report_progress: Option<fn(nfc_native::RecoveryProgress)>,
    capture_metrics: Option<crate::CaptureMetrics>,
    session: Option<Instant>,
    reading_memory: bool,
}

impl<'a> Reader<'a> {
    pub fn begin(client: &'a mut Client) -> Result<Self> {
        let mode = client.command(GET_MODE, &[], Duration::from_secs(3))?;
        let [previous_mode @ 0..=1] = mode.as_slice() else {
            return Err("Invalid device mode".into());
        };
        let previous_mode = *previous_mode;
        let result = (|| {
            if previous_mode == 0 {
                client.command(SET_MODE, &[1], Duration::from_secs(3))?;
            }
            scan(client)
        })();
        let card = match result {
            Ok(card) => card,
            Err(error) => {
                let cleanup = if previous_mode == 0 {
                    client.restore_reader_mode(false)
                } else {
                    Ok(())
                };
                return crate::with_cleanup(Err(error), cleanup);
            }
        };
        Ok(Self {
            client,
            card,
            previous_mode,
            report_progress: None,
            capture_metrics: None,
            session: None,
            reading_memory: false,
        })
    }
    /// Complete an operation and report RF or mode-restoration failures.
    pub fn run<T>(mut self, action: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        let result = action(&mut self);
        let cleanup = self.finish();
        crate::with_cleanup(result, cleanup)
    }

    pub fn finish(&mut self) -> Result<()> {
        let session = self.close_session();
        let previous = std::mem::replace(&mut self.previous_mode, 1);
        let restore = if previous == 0 {
            self.client.restore_reader_mode(false)
        } else {
            Ok(())
        };
        crate::with_cleanup(session, restore)
    }

    pub fn enable_capture_metrics(&mut self) {
        self.capture_metrics = Some(crate::CaptureMetrics::default());
    }
    pub fn capture_metrics(&self) -> Option<&crate::CaptureMetrics> {
        self.capture_metrics.as_ref()
    }
    pub fn set_progress(&mut self, report: fn(nfc_native::RecoveryProgress)) {
        self.report_progress = Some(report);
    }
    pub fn require_recovery_firmware(&self) -> Result<()> {
        if ![COLLECT_OBSERVATIONS, CHECK_KEYS, READ_BLOCK]
            .iter()
            .all(|command| self.client.capabilities.contains(command))
        {
            return Err("Update the Chameleon application firmware before key recovery".into());
        }
        Ok(())
    }
    pub fn validate_sector(&self, sector: usize) -> Result<()> {
        let blocks = self.card.model()?.classic().unwrap().1;
        if sector >= nfc_native::formats::image::classic::sector_count(blocks) {
            return Err("Sector outside card geometry".into());
        }
        Ok(())
    }
    fn validate_slots(&self, source: &RecoverySource, target: &RecoveryTarget) -> Result<()> {
        self.validate_sector(source.sector)?;
        self.validate_sector(target.sector)
    }
    pub fn probe_prng(&mut self) -> Result<PrngMode> {
        self.close_session()?;
        let data = self
            .client
            .command(PROBE_PRNG, &[], Duration::from_secs(5))?;
        match data.as_slice() {
            [0] => Ok(PrngMode::Static),
            [1] => Ok(PrngMode::Weak),
            [2] => Ok(PrngMode::Hard),
            _ => Err("Invalid PRNG reply".into()),
        }
    }
    pub fn block(&mut self, source: &RecoverySource, block: u8) -> Result<Vec<u8>> {
        if block as usize >= self.card.model()?.classic().unwrap().1 {
            return Err("Block outside card geometry".into());
        }
        self.close_session()?;
        let mut payload = vec![key_type(source.is_key_b), block];
        payload.extend(source.key);
        let data = self
            .client
            .command(READ_BLOCK, &payload, Duration::from_secs(3))?;
        if data.len() != 16 {
            return Err("Invalid block reply".into());
        }
        Ok(data)
    }
    pub fn magic(&mut self, key: Option<([u8; 6], bool)>) -> Result<crate::MagicDetection> {
        self.card.model()?;
        if !matches!(self.card.uid.len(), 4 | 7 | 10) {
            return Err("Invalid Classic UID length".into());
        }
        self.close_session()?;
        let mut request = vec![0; 15];
        request[0] = self.card.uid.len() as u8;
        request[1..1 + self.card.uid.len()].copy_from_slice(&self.card.uid);
        request[11..13].copy_from_slice(&self.card.atqa);
        request[13] = self.card.sak;
        if let Some((key, key_b)) = key {
            request[14] = if key_b { 2 } else { 1 };
            request.extend(key);
        }
        let response = self
            .client
            .command(MF1_MAGIC_PROBE, &request, Duration::from_secs(5))?;
        crate::MagicDetection::decode(&response)
    }
    pub fn special_uid(
        &mut self,
        method: crate::MagicUidMethod,
        new_uid: &[u8],
        password: [u8; 4],
        check: bool,
    ) -> Result<()> {
        self.card.model()?;
        if !self.client.capabilities.contains(&MF1_UID_SPECIAL)
            || new_uid.len() != self.card.uid.len()
            || !matches!(new_uid.len(), 4 | 7)
            || new_uid.len() != 4 && method != crate::MagicUidMethod::Gen3
            || new_uid == self.card.uid
            || method != crate::MagicUidMethod::Gen4 && password != [0; 4]
        {
            return Err(
                "Special UID write is unsupported or invalid; use matching firmware".into(),
            );
        }
        self.close_session()?;
        let mut request = [0; 21];
        request[0] = method as u8;
        request[1] = new_uid.len() as u8;
        request[2..2 + new_uid.len()].copy_from_slice(&self.card.uid);
        request[9..9 + new_uid.len()].copy_from_slice(new_uid);
        request[16..20].copy_from_slice(&password);
        request[20] = u8::from(check);
        let reply = self
            .client
            .command(MF1_UID_SPECIAL, &request, Duration::from_secs(5))
            .map_err(|error| {
                if check {
                    format!("UID preflight failed: {error}")
                } else {
                    format!(
                        "UID write outcome is unknown; inspect the tag before retrying: {error}"
                    )
                }
            })?;
        let expected = if check { &self.card.uid } else { new_uid };
        if reply.first().copied() != Some(expected.len() as u8) || reply[1..] != *expected {
            return Err("UID verification reply differs; inspect the tag before retrying".into());
        }
        if !check {
            self.card.uid = new_uid.to_vec();
        }
        Ok(())
    }

    pub fn set_uid_gen1a(&mut self, new_uid: &[u8]) -> Result<()> {
        self.set_uid(new_uid, TagCredential::None)
    }
    pub fn set_uid(&mut self, new_uid: &[u8], credential: TagCredential) -> Result<()> {
        self.card.model()?;
        if !self.client.capabilities.contains(&MF1_UID_SET)
            || self.card.uid.len() != 4
            || new_uid.len() != self.card.uid.len()
            || new_uid == self.card.uid
            || !matches!(
                credential,
                TagCredential::None | TagCredential::KeyA(_) | TagCredential::KeyB(_)
            )
        {
            return Err("UID write is unsupported or invalid".into());
        }
        let mut request = vec![
            0u8;
            if credential == TagCredential::None {
                15
            } else {
                22
            }
        ];
        request[0] = self.card.uid.len() as u8;
        request[1..1 + self.card.uid.len()].copy_from_slice(&self.card.uid);
        request[8..8 + new_uid.len()].copy_from_slice(new_uid);
        if request.len() == 22 {
            let (kind, key) = credential.wire();
            request[15] = kind;
            request[16..22].copy_from_slice(&key[..6]);
        }
        let response = self
            .client
            .command(MF1_UID_SET, &request, Duration::from_secs(10))
            .map_err(|error| {
                format!("UID write outcome is unknown; inspect the tag before retrying: {error}")
            })?;
        if response.len() != new_uid.len() + 1
            || response[0] != new_uid.len() as u8
            || response[1..] != *new_uid
        {
            return Err("UID verification mismatch; inspect the tag before retrying".into());
        }
        self.card.uid = new_uid.to_vec();
        Ok(())
    }
    pub fn set_sector_keys(
        &mut self,
        sector: u8,
        current: TagCredential,
        before: [u8; 4],
        after: [u8; 16],
    ) -> Result<()> {
        let model = self.card.model()?;
        let Some((_, blocks, _, _)) = model.classic() else {
            return Err("Expected a Classic tag".into());
        };
        if sector as usize >= nfc_native::formats::image::classic::sector_count(blocks)
            || !matches!(current, TagCredential::KeyA(_) | TagCredential::KeyB(_))
            || !nfc_native::formats::image::classic_access_bits_valid(&after)
            || !self.client.capabilities.contains(&MF1_TRAILER_SET)
        {
            return Err("Sector key change is unsupported or invalid".into());
        }
        let mut request = [0u8; 37];
        request[0] = self.card.uid.len() as u8;
        request[1..1 + self.card.uid.len()].copy_from_slice(&self.card.uid);
        request[8] = model as u8;
        request[9] = sector;
        let (kind, key) = current.wire();
        request[10] = kind;
        request[11..17].copy_from_slice(&key[..6]);
        request[17..21].copy_from_slice(&before);
        request[21..27].copy_from_slice(&after[..6]);
        request[27..33].copy_from_slice(&after[10..]);
        request[33..37].copy_from_slice(&after[6..10]);
        let reply = self
            .client
            .command(MF1_TRAILER_SET, &request, Duration::from_secs(10))
            .map_err(|error| {
                format!("Key write outcome is unknown; inspect the tag before retrying: {error}")
            })?;
        if !reply.is_empty() {
            return Err("Key write reply is invalid; inspect the tag before retrying".into());
        }
        Ok(())
    }
    pub fn write_user_unit(
        &mut self,
        model: Model,
        unit: &nfc_native::formats::physical::WriteUnit,
        credential: TagCredential,
    ) -> Result<()> {
        let size = if model.classic().is_some() {
            16
        } else if model.type2().is_some() {
            4
        } else {
            return Err("Physical write requires a Classic or Type 2 model".into());
        };
        if !nfc_native::formats::physical::user_unit(model, unit.index as usize)
            || unit.before.len() != size
            || unit.after.len() != size
            || !self.client.capabilities.contains(&NFCA_WRITE_USER_UNIT)
        {
            return Err("Physical user-data write is unsupported or invalid".into());
        }
        if !matches!(
            (model.classic().is_some(), credential),
            (true, TagCredential::KeyA(_) | TagCredential::KeyB(_))
                | (
                    false,
                    TagCredential::None
                        | TagCredential::Password(_)
                        | TagCredential::UltralightC(_)
                )
        ) {
            return Err("Credential does not match the tag family".into());
        }
        if (model == Model::UltralightC && matches!(credential, TagCredential::Password(_)))
            || (model != Model::UltralightC && matches!(credential, TagCredential::UltralightC(_)))
        {
            return Err("Credential does not match the Type 2 model".into());
        }
        let mut request = [0u8; 59];
        request[0] = model as u8;
        request[1] = self.card.uid.len() as u8;
        request[2..2 + self.card.uid.len()].copy_from_slice(&self.card.uid);
        request[9] = unit.index;
        let (kind, key) = credential.wire();
        request[10] = kind;
        request[11..27].copy_from_slice(&key);
        request[27..27 + size].copy_from_slice(&unit.before);
        request[43..43 + size].copy_from_slice(&unit.after);
        // A lost response can follow a completed physical write. Never retry it.
        let observed = self
            .client
            .command(NFCA_WRITE_USER_UNIT, &request, Duration::from_secs(5))
            .map_err(|error| {
                format!("Write outcome is unknown; inspect the tag before retrying: {error}")
            })?;
        if observed != unit.after {
            return Err("Write verification mismatch; inspect the tag before retrying".into());
        }
        Ok(())
    }
    fn close_session(&mut self) -> Result<()> {
        self.reading_memory = false;
        if self.session.take().is_none() || !self.client.usable {
            return Ok(());
        }
        // Cleanup is required after Ctrl-C; bypass cancellation, not framing.
        let response = crate::frame::exchange(
            &mut self.client.port,
            NFCA_SESSION,
            &[0],
            Duration::from_secs(3),
        );
        let (status, data) = response.inspect_err(|_| self.client.usable = false)?;
        if !matches!(status, 0 | 0x68) {
            return Err(DeviceError(status).into());
        }
        if !data.is_empty() {
            return Err("Invalid capture session reply".into());
        }
        Ok(())
    }
    fn verify_keys(
        &mut self,
        sector: usize,
        key_b: bool,
        candidates: &[[u8; 6]],
        mut progress: impl FnMut(usize, usize),
        retain: bool,
    ) -> Result<Option<[u8; 6]>> {
        self.validate_sector(sector)?;
        if !retain
            || self.reading_memory
            || self
                .session
                .is_some_and(|last| last.elapsed() >= Duration::from_secs(1))
        {
            self.close_session()?;
        }
        if retain && self.session.is_none() {
            self.session = Some(Instant::now());
            let bytes = self
                .client
                .command(NFCA_SESSION, &[1], Duration::from_secs(3))?;
            if !bytes.is_empty() {
                return Err("Invalid capture session reply".into());
            }
        }
        progress(0, candidates.len());
        for (index, batch) in candidates.chunks(42).enumerate() {
            let mut payload = vec![first_block(sector), key_type(key_b), batch.len() as u8];
            payload.extend(batch.iter().flatten());
            let (status, data) =
                self.client
                    .request(CHECK_KEYS, &payload, Duration::from_secs(15))?;
            if self.session.is_some() {
                self.session = Some(Instant::now());
            }
            if status == 6 {
                progress(index * 42 + batch.len(), candidates.len());
                continue;
            }
            if status != 0 {
                return Err(DeviceError(status).into());
            }
            if data.len() != 7 || data[0] != 1 {
                return Err("Invalid verification reply".into());
            }
            let key: [u8; 6] = data[1..].try_into()?;
            let matched = batch
                .iter()
                .position(|candidate| *candidate == key)
                .ok_or("Verification returned an unsubmitted key")?;
            progress(index * 42 + matched + 1, candidates.len());
            return Ok(Some(key));
        }
        Ok(None)
    }
    fn collect_full_parity(
        &mut self,
        source: &RecoverySource,
        target: &RecoveryTarget,
        chained: bool,
        count: usize,
        keep_field: bool,
    ) -> Result<Vec<Observation>> {
        if !(1..=16).contains(&count) {
            return Err("Capture count must be 1..16".into());
        }
        self.validate_slots(source, target)?;
        // Restart conservatively before the firmware's two-second idle expiry.
        if self.reading_memory
            || !keep_field
            || self
                .session
                .is_some_and(|last| last.elapsed() >= Duration::from_secs(1))
        {
            self.close_session()?;
        }
        if keep_field && self.session.is_none() && self.client.capabilities.contains(&NFCA_SESSION)
        {
            // Mark cleanup necessary before sending: malformed replies still need it.
            self.session = Some(Instant::now());
            let data = self
                .client
                .command(NFCA_SESSION, &[1], Duration::from_secs(3))?;
            if !data.is_empty() {
                return Err("Invalid capture session reply".into());
            }
        }
        let mut payload = source_payload(source, target);
        payload.extend([u8::from(chained), count as u8]);
        let profiled = self.capture_metrics.is_some();
        if profiled {
            payload.push(1);
        }
        let started = Instant::now();
        let mut firmware = None;
        let result = self
            .client
            .request(COLLECT_OBSERVATIONS, &payload, Duration::from_secs(15))
            .and_then(|(status, data)| {
                if status != 0 {
                    if profiled && !data.is_empty() {
                        firmware = Some(crate::capture::decode_metrics(&data, count)?);
                    }
                    return Err(DeviceError(status).into());
                }
                let (stats, records) = crate::capture::decode(&data, count, profiled)?;
                firmware = stats;
                Ok(records)
            });
        if let Some(metrics) = &mut self.capture_metrics {
            metrics.record(
                count,
                started.elapsed().as_micros() as u64,
                firmware,
                &result,
            );
        }
        if result.is_err() {
            let cleanup = self.close_session();
            return crate::with_cleanup(result, cleanup);
        }
        if self.session.is_some() {
            self.session = Some(Instant::now());
        }
        result
    }
}

impl RecoveryReader for Reader<'_> {
    fn progress(&mut self, event: nfc_native::RecoveryProgress) {
        if let Some(report) = self.report_progress {
            report(event);
        }
    }

    fn probe_prng(&mut self) -> Result<PrngMode> {
        self.probe_prng()
    }
    fn uid(&self) -> &[u8] {
        &self.card.uid
    }
    fn end(&mut self) -> Result<()> {
        self.close_session()
    }
    fn collect(
        &mut self,
        source: &RecoverySource,
        target: &RecoveryTarget,
        scheduled: bool,
        count: usize,
    ) -> Result<Vec<Observation>> {
        self.require_recovery_firmware()?;
        self.collect_full_parity(source, target, false, count, !scheduled)
    }
    fn collect_static(
        &mut self,
        source: &RecoverySource,
        target: &RecoveryTarget,
        chained: bool,
        count: usize,
    ) -> Result<Vec<Observation>> {
        self.require_recovery_firmware()?;
        self.collect_full_parity(source, target, chained, count, false)
    }
    fn darkside(
        &mut self,
        target: &RecoveryTarget,
        variant: u8,
    ) -> Result<Option<DarksideObservation>> {
        self.validate_sector(target.sector)?;
        self.close_session()?;
        let bytes = if self.client.capabilities.contains(&DARKSIDE_STEP) {
            // Bounded acquisition may retain RF; end/verification/Drop must
            // release it even when a reply is malformed or Ctrl-C was pressed.
            if self.client.capabilities.contains(&NFCA_SESSION) {
                self.session = Some(Instant::now());
            }
            let mut payload = vec![
                key_type(target.is_key_b),
                first_block(target.sector),
                if variant == 0 { 0 } else { 2 },
                self.card.uid.len() as u8,
            ];
            payload.extend_from_slice(&self.card.uid);
            let mut capture = None;
            for _ in 0..512 {
                let bytes = self
                    .client
                    .command(DARKSIDE_STEP, &payload, Duration::from_secs(5))?;
                if bytes == [5] {
                    payload[2] = 1;
                    continue;
                }
                if !matches!(bytes.as_slice(), [0, ..] | [1..=4] | [6]) {
                    return Err("Invalid Darkside capture status".into());
                }
                capture = Some(bytes);
                break;
            }
            capture.ok_or("Darkside acquisition exceeded its attempt budget")?
        } else {
            self.client.command(
                DARKSIDE,
                &[
                    key_type(target.is_key_b),
                    first_block(target.sector),
                    u8::from(variant == 0),
                    1,
                ],
                Duration::from_secs(150),
            )?
        };
        if bytes.first() != Some(&0) {
            if bytes == [4] {
                return Err("Card changed during Darkside".into());
            }
            return Ok(None);
        }
        if bytes.len() != 33 {
            return Err("Invalid Darkside capture length".into());
        }
        let uid = u32::from_be_bytes(bytes[1..5].try_into()?);
        if bytes[1..5] != self.card.uid[self.card.uid.len() - 4..] {
            return Err("Darkside observation UID mismatch".into());
        }
        if bytes[17..25].iter().any(|&nibble| nibble > 15) {
            return Err("Invalid Darkside keystream".into());
        }
        Ok(Some(DarksideObservation {
            uid,
            nt: u32::from_be_bytes(bytes[5..9].try_into()?),
            par_list: bytes[9..17].try_into()?,
            ks_list: bytes[17..25].try_into()?,
            nr: u32::from_be_bytes(bytes[25..29].try_into()?),
            ar: u32::from_be_bytes(bytes[29..33].try_into()?),
        }))
    }
    fn verify_progress(
        &mut self,
        sector: usize,
        key_b: bool,
        candidates: &[[u8; 6]],
        progress: impl FnMut(usize, usize),
    ) -> Result<Option<[u8; 6]>> {
        self.verify_keys(sector, key_b, candidates, progress, false)
    }
    fn cancellation_flag(&self) -> Arc<AtomicBool> {
        self.client.cancelled.clone()
    }
    fn is_cancelled(&self) -> bool {
        self.client.cancelled.load(Ordering::Relaxed)
    }
    fn capture_unavailable(&self, error: &(dyn std::error::Error + 'static)) -> bool {
        error
            .downcast_ref::<DeviceError>()
            .is_some_and(|error| matches!(error.0, 6 | 0x69))
    }
}

impl Drop for Reader<'_> {
    fn drop(&mut self) {
        let _ = self.finish();
    }
}

pub(crate) fn scan(client: &mut Client) -> Result<Card> {
    let data = client.command(SCAN, &[], Duration::from_secs(3))?;
    let size = *data.first().ok_or("Empty card identity")? as usize;
    if !matches!(size, 4 | 7 | 10) || data.len() < size + 5 {
        return Err("Invalid card identity".into());
    }
    let ats_size = data[size + 4] as usize;
    if data.len() != size + 5 + ats_size {
        return Err("Invalid card ATS length".into());
    }
    Ok(Card {
        uid: data[1..1 + size].to_vec(),
        atqa: data[size + 1..size + 3].try_into()?,
        sak: data[size + 3],
    })
}
fn key_type(key_b: bool) -> u8 {
    if key_b { 0x61 } else { 0x60 }
}
fn source_payload(source: &RecoverySource, target: &RecoveryTarget) -> Vec<u8> {
    let mut bytes = vec![key_type(source.is_key_b), first_block(source.sector)];
    bytes.extend(source.key);
    bytes.extend([key_type(target.is_key_b), first_block(target.sector)]);
    bytes
}
