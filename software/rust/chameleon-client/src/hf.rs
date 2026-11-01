//! Active NFC discovery and one-command memory captures.
#[cfg(all(test, unix))]
use crate::RequestCancelled;
use crate::{Card, Client, DeviceError, Result, command::*};
use serde::{Deserialize, Serialize};
use std::{sync::atomic::Ordering, time::Duration};

mod st25tb;
pub use st25tb::{
    TearoffDiagnostics, TearoffLog, TearoffLogKind, TearoffOutcome, TearoffProgress, TearoffReport,
    TearoffResult, TearoffTiming, UnknownTearoffOutcome,
};

#[cfg(all(test, unix))]
mod tests;

#[derive(Clone, Debug)]
pub enum HfTag {
    NfcA(Card),
    NfcB {
        pupi: [u8; 4],
        application: [u8; 4],
        protocol: [u8; 3],
    },
    St25tb {
        uid: [u8; 8],
        chip_id: u8,
        blocks: u8,
    },
    NfcV {
        uid: [u8; 8],
        dsfid: u8,
        afi: Option<u8>,
        ic_ref: Option<u8>,
        blocks: Option<u16>,
        block_size: Option<u8>,
    },
}

impl HfTag {
    pub fn uid(&self) -> &[u8] {
        match self {
            Self::NfcA(card) => &card.uid,
            Self::NfcB { pupi, .. } => pupi,
            Self::St25tb { uid, .. } | Self::NfcV { uid, .. } => uid,
        }
    }
    pub fn name(&self) -> &'static str {
        match self {
            Self::NfcA(_) => "NFC-A",
            Self::NfcB { .. } => "ISO14443-B",
            Self::St25tb {
                uid: [0xd0, 2, 0x33, ..],
                ..
            } => "ST25TB512-AT",
            Self::St25tb {
                uid: [0xd0, 2, 0x1b, ..],
                ..
            } => "SRI512 / ST25TB512-AC",
            Self::St25tb {
                uid: [0xd0, 2, 0x18..=0x1a, ..],
                ..
            } => "SRI512",
            Self::St25tb { .. } => "ST25TB/SRx",
            Self::NfcV {
                uid,
                blocks: Some(28),
                block_size: Some(4),
                ..
            } if uid[..3] == [0xe0, 4, 1] => "SLIX",
            Self::NfcV {
                uid,
                blocks: Some(8),
                block_size: Some(4),
                ..
            } if uid[..3] == [0xe0, 4, 3] => "SLIX-L",
            Self::NfcV { .. } => "ISO15693",
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HfSnapshot {
    pub version: u8,
    pub protocol: String,
    pub uid: [u8; 8],
    pub memory: Vec<Option<[u8; 4]>>,
    pub locks: Vec<Option<bool>>,
    pub dsfid: Option<u8>,
    pub afi: Option<u8>,
    pub ic_ref: Option<u8>,
    pub chip_id: Option<u8>,
    pub system: Option<[u8; 4]>,
    pub complete: bool,
    pub deadline: bool,
    pub status: u8,
    pub tag_error: Option<u8>,
}

impl HfSnapshot {
    pub fn decode(data: &[u8], expected: &HfTag) -> Result<Self> {
        if data.len() < 24
            || data[0] != 1
            || !matches!(data[1], 2 | 3)
            || data[2] > 2
            || data[14] != 4
            || data[15] & !15 != 0
        {
            return Err("Invalid reader snapshot header".into());
        }
        let blocks = u16::from_be_bytes([data[12], data[13]]) as usize;
        if blocks == 0
            || blocks > 64
            || data.len() != 24 + blocks * 6
            || &data[4..12] != expected.uid()
        {
            return Err("Reader snapshot identity or geometry mismatch".into());
        }
        if (data[1] == 2 && data[15] & !8 != 0)
            || (data[1] == 3 && data[15] & 8 != 0)
            || (data[2] == 0 && data[3] != 0)
        {
            return Err("Inconsistent reader snapshot metadata".into());
        }
        match expected {
            HfTag::St25tb { blocks: 16, .. } if data[1] == 2 && blocks == 16 => (),
            HfTag::NfcV {
                blocks: Some(count),
                block_size: Some(4),
                ..
            } if data[1] == 3 && blocks == *count as usize => (),
            _ => return Err("Reader snapshot protocol mismatch".into()),
        }
        let mut memory = Vec::with_capacity(blocks);
        let mut locks = Vec::with_capacity(blocks);
        for entry in data[24..].chunks_exact(6) {
            if entry[0] > 1 || entry[1] > 2 {
                return Err("Invalid reader block state".into());
            }
            memory.push(if entry[0] == 1 {
                Some(entry[2..6].try_into()?)
            } else {
                None
            });
            locks.push(match entry[1] {
                1 => Some(false),
                2 => Some(true),
                _ => None,
            });
        }
        let system = if data[15] & 8 != 0 {
            Some(data[20..24].try_into()?)
        } else {
            None
        };
        let complete = data[2] == 0;
        if complete && (memory.iter().any(Option::is_none) || (data[1] == 2 && system.is_none())) {
            return Err("Incomplete data marked as a complete dump".into());
        }
        let (previous_dsfid, previous_afi, previous_ic_ref) = match expected {
            HfTag::NfcV {
                dsfid, afi, ic_ref, ..
            } => (Some(*dsfid), *afi, *ic_ref),
            _ => (None, None, None),
        };
        Ok(Self {
            version: 1,
            protocol: if data[1] == 2 { "st25tb" } else { "nfc_v" }.into(),
            uid: data[4..12].try_into()?,
            memory,
            locks,
            dsfid: (data[15] & 1 != 0).then_some(data[16]).or(previous_dsfid),
            afi: (data[15] & 2 != 0).then_some(data[17]).or(previous_afi),
            ic_ref: (data[15] & 4 != 0).then_some(data[18]).or(previous_ic_ref),
            chip_id: (data[1] == 2).then_some(data[19]),
            system,
            complete,
            deadline: data[2] == 2,
            status: data[3],
            tag_error: (data[1] == 3 && data[19] != 0).then_some(data[19]),
        })
    }
}

pub struct HfReader<'a> {
    client: &'a mut Client,
    previous_mode: u8,
    privacy_password: Option<[u8; 4]>,
    raw_field: bool,
}

impl<'a> HfReader<'a> {
    pub fn begin(client: &'a mut Client) -> Result<Self> {
        let data = client.command(GET_MODE, &[], Duration::from_secs(3))?;
        let [previous_mode @ 0..=1] = data.as_slice() else {
            return Err("Invalid device mode".into());
        };
        let previous_mode = *previous_mode;
        if previous_mode == 0 {
            client.command(SET_MODE, &[1], Duration::from_secs(3))?;
        }
        Ok(Self {
            client,
            previous_mode,
            privacy_password: None,
            raw_field: false,
        })
    }
    fn require(&self, command: u16) -> Result<()> {
        if !self.client.capabilities.contains(&command) {
            return Err("Update Chameleon application firmware for this reader operation".into());
        }
        Ok(())
    }
    pub fn search(&mut self) -> Result<HfTag> {
        match super::nfca::scan(self.client) {
            Ok(card) => return Ok(HfTag::NfcA(card)),
            Err(error) if absent(error.as_ref()) => (),
            Err(error) => return Err(error),
        }
        for (command, scan) in [
            (NFCB_INFO, Self::nfcb as fn(&mut Self) -> Result<HfTag>),
            (ST25TB_INFO, Self::st25tb),
            (NFCV_INVENTORY, Self::nfcv),
        ] {
            if !self.client.capabilities.contains(&command) {
                continue;
            }
            match scan(self) {
                Ok(tag) => return Ok(tag),
                Err(error) if absent(error.as_ref()) => (),
                Err(error) => return Err(error),
            }
        }
        Err("No supported NFC tag found".into())
    }
    pub fn nfcb(&mut self) -> Result<HfTag> {
        self.require(NFCB_INFO)?;
        let data = self
            .client
            .command(NFCB_INFO, &[], Duration::from_secs(5))?;
        if data.len() != 14 || data[0] != 0x50 || !crc_ok(&data) {
            return Err("Invalid ATQB response".into());
        }
        Ok(HfTag::NfcB {
            pupi: data[1..5].try_into()?,
            application: data[5..9].try_into()?,
            protocol: data[9..12].try_into()?,
        })
    }
    pub fn st25tb(&mut self) -> Result<HfTag> {
        self.require(ST25TB_INFO)?;
        let data = self
            .client
            .command(ST25TB_INFO, &[], Duration::from_secs(5))?;
        if data.len() != 11 || data[10] != 4 || !matches!(data[9], 0 | 16) {
            return Err("Invalid ST25TB identity".into());
        }
        Ok(HfTag::St25tb {
            uid: data[..8].try_into()?,
            chip_id: data[8],
            blocks: data[9],
        })
    }
    pub fn slix(&mut self, password: Option<[u8; 4]>, slix_l: bool) -> Result<HfTag> {
        if password.is_some() {
            self.require(SLIX_PRIVACY_SCAN)?;
        }
        self.privacy_password = password;
        let tag = self.nfcv()?;
        let expected = if slix_l { 3 } else { 1 };
        if !matches!(&tag, HfTag::NfcV { uid, .. } if uid[..3] == [0xe0, 4, expected]) {
            return Err("The physical tag is not the requested SLIX product".into());
        }
        Ok(tag)
    }

    fn nfcv_request(&mut self, command: u16, data: &[u8]) -> Result<(u16, Vec<u8>)> {
        let mut payload = data.to_vec();
        if let Some(password) = self.privacy_password {
            payload.extend(password);
        }
        self.client
            .request(command, &payload, Duration::from_secs(5))
    }

    pub fn raw_nfca(
        &mut self,
        bytes: &[u8],
        bits: u16,
        append_crc: bool,
        check_crc: bool,
        select: bool,
        timeout_ms: u16,
    ) -> Result<(u16, Vec<u8>)> {
        if bytes.is_empty()
            || bytes.len() > 62
            || bits == 0
            || usize::from(bits).div_ceil(8) != bytes.len()
            || !bits.is_multiple_of(8) && append_crc
            || timeout_ms == 0
            || timeout_ms > 5000
        {
            return Err("Invalid NFC-A raw frame, CRC options or timeout".into());
        }
        self.require(NFCA_RAW)?;
        let mut flags = 0x48;
        if !self.raw_field {
            flags |= 0x80;
        }
        if append_crc {
            flags |= 0x20;
        }
        if check_crc {
            flags |= 4;
        }
        if select {
            flags |= 0x10;
        }
        let mut payload = vec![flags];
        payload.extend(timeout_ms.to_be_bytes());
        payload.extend(bits.to_be_bytes());
        payload.extend(bytes);
        // Record cleanup before transmitting, including an interrupted response.
        self.raw_field = true;
        self.client.request(
            NFCA_RAW,
            &payload,
            Duration::from_millis(u64::from(timeout_ms) + 1000),
        )
    }

    pub fn raw_nfcv(&mut self, frames: &[Vec<u8>]) -> Result<Vec<(u16, Vec<u8>)>> {
        self.raw_nfcv_with_field(frames, false)
    }

    pub fn raw_nfcv_with_field(
        &mut self,
        frames: &[Vec<u8>],
        keep: bool,
    ) -> Result<Vec<(u16, Vec<u8>)>> {
        if frames.is_empty()
            || frames.len() > 12
            || frames
                .iter()
                .any(|frame| frame.is_empty() || frame.len() > 22)
        {
            return Err("NFC-V raw accepts 1..12 frames of 1..22 bytes".into());
        }
        self.require(NFCV_RAW)?;
        let mut payload = vec![frames.len() as u8 | if keep { 0x80 } else { 0 }];
        for frame in frames {
            payload.push(frame.len() as u8);
            payload.extend(frame);
        }
        self.raw_field |= keep;
        let data = self
            .client
            .command(NFCV_RAW, &payload, Duration::from_secs(5))?;
        if data.is_empty() || data[0] == 0 || usize::from(data[0]) > frames.len() {
            return Err("Invalid NFC-V raw reply count".into());
        }
        let mut offset = 1;
        let mut replies = Vec::new();
        for _ in 0..data[0] {
            if offset + 3 > data.len() {
                return Err("Truncated NFC-V raw reply".into());
            }
            let status = u16::from_be_bytes([data[offset], data[offset + 1]]);
            let size = usize::from(data[offset + 2]);
            offset += 3;
            if size > 24
                || offset + size > data.len()
                || replies.last().is_some_and(|(status, _)| *status != 0)
            {
                return Err("Invalid NFC-V raw response".into());
            }
            let bytes = data[offset..offset + size].to_vec();
            if status == 0 && !crc_ok(&bytes) {
                return Err("Invalid NFC-V raw CRC".into());
            }
            replies.push((status, bytes));
            offset += size;
        }
        if offset != data.len()
            || replies.len() != frames.len()
                && replies.last().is_some_and(|(status, _)| *status == 0)
        {
            return Err("Incomplete NFC-V raw response".into());
        }
        Ok(replies)
    }

    /// Leave an explicitly retained raw field in reader mode across connections.
    pub fn keep_field(&mut self) -> Result<()> {
        if self.cancelled() {
            return Err(crate::RequestCancelled.into());
        }
        self.raw_field = false;
        self.previous_mode = 1;
        Ok(())
    }

    pub fn write_nfcv(
        &mut self,
        tag: &HfTag,
        block: u8,
        before: [u8; 4],
        after: [u8; 4],
    ) -> Result<()> {
        if !matches!(tag, HfTag::NfcV { blocks: Some(count), block_size: Some(4), .. } if u16::from(block) < *count)
        {
            return Err("Block outside supported NFC-V memory".into());
        }
        self.require(NFCV_WRITE)?;
        let mut payload = tag.uid().to_vec();
        payload.push(block);
        payload.extend(before);
        payload.extend(after);
        let (status, reply) = self.nfcv_request(NFCV_WRITE, &payload).map_err(|error| {
            format!("Write outcome is unknown; inspect the tag before retrying: {error}")
        })?;
        if status != 0 || reply != after {
            return Err(format!(
                "NFC-V write unconfirmed (0x{status:02X}); inspect the tag before retrying"
            )
            .into());
        }
        Ok(())
    }

    pub fn nfcv(&mut self) -> Result<HfTag> {
        self.require(NFCV_INVENTORY)?;
        let (status, data) = if let Some(password) = self.privacy_password {
            self.client
                .request(SLIX_PRIVACY_SCAN, &password, Duration::from_secs(5))?
        } else {
            self.client
                .request(NFCV_INVENTORY, &[], Duration::from_secs(5))?
        };
        if status != 0 {
            return Err(DeviceError(status).into());
        }
        if data.len() != 12 || data[0] != 0 || !crc_ok(&data) {
            return Err("Invalid NFC-V Inventory".into());
        }
        let mut uid: [u8; 8] = data[2..10].try_into()?;
        uid.reverse();
        let mut tag = HfTag::NfcV {
            uid,
            dsfid: data[1],
            afi: None,
            ic_ref: None,
            blocks: None,
            block_size: None,
        };
        if !self.client.capabilities.contains(&NFCV_INFO) {
            return Ok(tag);
        }
        let (status, info) = self.nfcv_request(NFCV_INFO, &uid)?;
        if status == 2 && info.len() == 4 && info[0] & 1 != 0 && info[1] == 1 && crc_ok(&info) {
            return Ok(tag);
        }
        if status != 0 {
            return Err(DeviceError(status).into());
        }
        parse_nfcv_info(&info, &mut tag)?;
        Ok(tag)
    }
    pub fn read(&mut self, tag: &HfTag, block: u16) -> Result<[u8; 4]> {
        let command = match tag {
            HfTag::St25tb { blocks: 16, .. } if block < 16 || block == 255 => ST25TB_READ,
            HfTag::NfcV {
                blocks: Some(count),
                block_size: Some(4),
                ..
            } if block < *count && block < 256 => NFCV_READ,
            _ => return Err("Block outside supported tag geometry".into()),
        };
        self.require(command)?;
        let mut request = tag.uid().to_vec();
        request.push(block as u8);
        let (status, data) = if command == NFCV_READ {
            self.nfcv_request(command, &request)?
        } else {
            self.client
                .request(command, &request, Duration::from_secs(5))?
        };
        if status != 0 {
            return Err(DeviceError(status).into());
        }
        if command == ST25TB_READ {
            return Ok(data.as_slice().try_into()?);
        }
        if data.len() != 7 || data[0] != 0 || !crc_ok(&data) {
            return Err("Invalid NFC-V block response".into());
        }
        Ok(data[1..5].try_into()?)
    }

    pub fn dump(&mut self, tag: &HfTag) -> Result<HfSnapshot> {
        let mut request = tag.uid().to_vec();
        let command = match tag {
            HfTag::St25tb { blocks: 16, .. } => ST25TB_DUMP,
            HfTag::NfcV {
                blocks: Some(count @ 1..=64),
                block_size: Some(4),
                ..
            } => {
                request.extend(count.to_be_bytes());
                request.push(4);
                NFCV_DUMP
            }
            _ => return Err("Dump geometry is unsupported; identity was retained".into()),
        };
        self.require(command)?;
        let (status, data) = if command == NFCV_DUMP {
            self.nfcv_request(command, &request)?
        } else {
            self.client
                .request(command, &request, Duration::from_secs(5))?
        };
        if data.len() >= 24 {
            let mut snapshot = HfSnapshot::decode(&data, tag)?;
            if status != 0 {
                snapshot.complete = false;
                snapshot.status = status.try_into()?;
            }
            return Ok(snapshot);
        }
        Err(DeviceError(status).into())
    }
    pub fn finish(&mut self) -> Result<()> {
        if !self.client.usable {
            return Err("Connection is unusable; reader restoration is unknown".into());
        }
        if self.raw_field {
            self.raw_field = false;
            let (status, data) = crate::frame::exchange(
                &mut self.client.port,
                HF_FIELD_OFF,
                &[],
                Duration::from_secs(3),
            )
            .inspect_err(|_| self.client.usable = false)?;
            if !matches!(status, 0 | 0x68) || !data.is_empty() {
                return Err("RF field shutdown is unconfirmed".into());
            }
        }
        if self.previous_mode == 0 {
            let response = crate::frame::exchange(
                &mut self.client.port,
                SET_MODE,
                &[0],
                Duration::from_secs(3),
            );
            let (status, _) = match response {
                Ok(reply) => reply,
                Err(error) => {
                    self.client.usable = false;
                    return Err(error);
                }
            };
            if status != 0x68 {
                return Err(DeviceError(status).into());
            }
            self.previous_mode = 1;
        }
        Ok(())
    }
    pub fn cancelled(&self) -> bool {
        self.client.cancelled.load(Ordering::Relaxed)
    }
}

impl Client {
    pub fn field_off(&mut self) -> Result<()> {
        if !self.reader_mode()? {
            return Ok(());
        }
        if !self.capabilities.contains(&HF_FIELD_OFF) {
            return Err("RF field control requires matching firmware".into());
        }
        let reply = self.command(HF_FIELD_OFF, &[], Duration::from_secs(3))?;
        if !reply.is_empty() {
            return Err("RF field shutdown is unconfirmed".into());
        }
        Ok(())
    }
}

impl Drop for HfReader<'_> {
    fn drop(&mut self) {
        if self.client.usable {
            let _ = self.finish();
        }
    }
}

fn absent(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<DeviceError>()
        .is_some_and(|error| error.0 == 1)
}

fn parse_nfcv_info(data: &[u8], tag: &mut HfTag) -> Result<()> {
    let HfTag::NfcV {
        uid,
        dsfid,
        afi,
        ic_ref,
        blocks,
        block_size,
    } = tag
    else {
        unreachable!()
    };
    if data.len() < 12 || data[0] != 0 || data[1] & 0xf0 != 0 || !crc_ok(data) {
        return Err("Invalid NFC-V system information".into());
    }
    let wire_uid: Vec<_> = uid.iter().rev().copied().collect();
    if data[2..10] != wire_uid {
        return Err("NFC-V UID changed".into());
    }
    let mut offset = 10;
    for field in 0..4 {
        if data[1] & (1 << field) == 0 {
            continue;
        }
        let count = if field == 2 { 2 } else { 1 };
        if offset + count > data.len() - 2 {
            return Err("Truncated NFC-V system information".into());
        }
        match field {
            0 => *dsfid = data[offset],
            1 => *afi = Some(data[offset]),
            2 => {
                *blocks = Some(u16::from(data[offset]) + 1);
                *block_size = Some((data[offset + 1] & 31) + 1);
            }
            _ => *ic_ref = Some(data[offset]),
        }
        offset += count;
    }
    if offset != data.len() - 2 {
        return Err("Trailing NFC-V system information".into());
    }
    Ok(())
}

fn crc_ok(data: &[u8]) -> bool {
    if data.len() < 3 {
        return false;
    }
    let mut crc = 0xffffu16;
    for byte in &data[..data.len() - 2] {
        crc ^= u16::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0x8408 } else { 0 };
        }
    }
    (!crc).to_le_bytes() == data[data.len() - 2..]
}
