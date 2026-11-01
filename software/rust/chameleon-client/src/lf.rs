//! Existing LF serial commands. RF timing remains in firmware.
use crate::{Client, DeviceError, Result};
use std::{sync::atomic::Ordering, time::Duration};

#[derive(Debug)]
pub struct LfNotFound;

impl std::fmt::Display for LfNotFound {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(out, "No supported LF tag found")
    }
}

impl std::error::Error for LfNotFound {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LfType {
    Em410x,
    Electra,
    Hid,
    Ioprox,
    Pac,
    Viking,
    Jablotron,
    Idteck,
    Em4x05,
}
impl LfType {
    pub const READABLE: &[Self] = &[
        Self::Em410x,
        Self::Electra,
        Self::Hid,
        Self::Ioprox,
        Self::Pac,
        Self::Viking,
        Self::Jablotron,
        Self::Em4x05,
    ];
    pub const WRITABLE: &[Self] = &[
        Self::Em410x,
        Self::Electra,
        Self::Hid,
        Self::Ioprox,
        Self::Pac,
        Self::Viking,
        Self::Jablotron,
        Self::Idteck,
    ];
    pub fn name(self) -> &'static str {
        match self {
            Self::Em410x => "em410x",
            Self::Electra => "electra",
            Self::Hid => "hid",
            Self::Ioprox => "ioprox",
            Self::Pac => "pac",
            Self::Viking => "viking",
            Self::Jablotron => "jablotron",
            Self::Idteck => "idteck",
            Self::Em4x05 => "em4x05",
        }
    }
    pub fn parse(value: &str) -> Result<Self> {
        Self::WRITABLE
            .iter()
            .chain(Self::READABLE)
            .copied()
            .find(|kind| kind.name() == value)
            .ok_or_else(|| format!("Unknown LF type: {value}").into())
    }
    fn scan(self) -> Result<u16> {
        match self {
            Self::Em410x | Self::Electra => Ok(3000),
            Self::Hid => Ok(3002),
            Self::Ioprox => Ok(3010),
            Self::Pac => Ok(3014),
            Self::Viking => Ok(3004),
            Self::Jablotron => Ok(3019),
            Self::Em4x05 => Ok(3030),
            Self::Idteck => Err("Firmware has no IDTECK reader command".into()),
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LfTag {
    pub kind: LfType,
    /// Canonical firmware fields, excluding the EM410x type prefix.
    pub data: Vec<u8>,
    /// Detected EM410x clock/type, when present.
    pub variant: Option<u16>,
}
impl LfTag {
    /// Canonicalize fields that the existing firmware changes when writing.
    pub fn for_t55xx(&self) -> Result<Self> {
        if !LfType::WRITABLE.contains(&self.kind) {
            return Err("This LF family cannot be programmed onto T55xx".into());
        }
        self.validate()?;
        let mut tag = self.clone();
        // Firmware substitutes OEM 900 for zero in these formats.
        if tag.kind == LfType::Hid && matches!(tag.data[0], 22 | 42) && tag.data[11..13] == [0, 0] {
            tag.data[11..13].copy_from_slice(&900u16.to_be_bytes());
        }
        Ok(tag)
    }
    pub fn validate(&self) -> Result<()> {
        let length = match self.kind {
            LfType::Em410x | LfType::Jablotron => 5,
            LfType::Electra | LfType::Hid => 13,
            LfType::Ioprox => 16,
            LfType::Pac | LfType::Idteck => 8,
            LfType::Viking => 4,
            LfType::Em4x05 => 13,
        };
        if self.data.len() != length {
            return Err(format!("Invalid {} reply or ID length", self.kind.name()).into());
        }
        if self.kind == LfType::Pac && !self.data.iter().all(|b| b.is_ascii_graphic()) {
            return Err("PAC ID must contain eight printable ASCII characters".into());
        }
        if self.kind == LfType::Idteck && self.data[..4] != *b"IDTK" {
            return Err("IDTECK frame must begin with 4944544B".into());
        }
        if self.kind == LfType::Ioprox && self.data[..4] != ioprox_fields(&self.data[4..12])? {
            return Err("ioProx fields do not match the raw frame".into());
        }
        if self.kind == LfType::Em4x05 && self.data[12] > 1 {
            return Err("Invalid EM4x69 flag".into());
        }
        if self.kind == LfType::Hid {
            let name = crate::HID_FORMATS
                .iter()
                .find(|(_, id, _)| *id == self.data[0])
                .ok_or("Unknown HID format in reply")?
                .0;
            crate::hid_payload(
                name,
                u32::from_be_bytes(self.data[1..5].try_into()?),
                u64::from_be_bytes([
                    0,
                    0,
                    0,
                    self.data[5],
                    self.data[6],
                    self.data[7],
                    self.data[8],
                    self.data[9],
                ]),
                self.data[10],
                u16::from_be_bytes(self.data[11..13].try_into()?),
            )?;
        }
        Ok(())
    }
}
impl Client {
    /// Restore the previous mode even if cancellation interrupts mode verification.
    pub fn with_lf_reader<T>(&mut self, action: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        self.with_reader_mode(action)
    }
    fn lf_request(&mut self, id: u16, data: &[u8]) -> Result<Option<Vec<u8>>> {
        if !self.capabilities.contains(&id) {
            return Err(format!("Firmware does not support LF command {id}").into());
        }
        let (status, bytes) = self
            .request(id, data, Duration::from_secs(15))
            .map_err(|error| {
                if !self.usable {
                    format!("LF command {id} failed: {error}").into()
                } else {
                    error
                }
            })?;
        match status {
            0x40 => Ok(Some(bytes)),
            0x41 if self.cancelled.load(Ordering::Relaxed) => Err(crate::RequestCancelled.into()),
            0x41 => Ok(None),
            _ => Err(DeviceError(status).into()),
        }
    }
    /// Reader mode must already be selected by the caller.
    pub fn lf_read(&mut self, kind: LfType, hid_format: u8) -> Result<Option<LfTag>> {
        let input = match kind {
            LfType::Hid => vec![hid_format],
            _ => vec![],
        };
        let Some(mut data) = self.lf_request(kind.scan()?, &input)? else {
            return Ok(None);
        };
        let mut actual = kind;
        let mut variant = None;
        if matches!(kind, LfType::Em410x | LfType::Electra) {
            if data.len() < 2 {
                return Err("Truncated EM410x reply".into());
            }
            let id = u16::from_be_bytes([data[0], data[1]]);
            if !(100..=104).contains(&id) {
                return Err("Unknown EM410x type in reply".into());
            }
            actual = if id == 104 {
                LfType::Electra
            } else {
                LfType::Em410x
            };
            variant = Some(id);
            data.drain(..2);
            if kind == LfType::Electra && actual != kind {
                return Ok(None);
            }
        }
        if kind == LfType::Hid {
            // Firmware replies with a 16-byte structure; only 13 bytes are fields.
            if data.len() != 16 {
                return Err("Invalid HID reply length".into());
            }
            data.truncate(13);
        }
        let tag = LfTag {
            kind: actual,
            data,
            variant,
        };
        tag.validate()?;
        Ok(Some(tag))
    }
    /// Try only broadcast-ID families. EM4x05 requires an explicit read.
    pub fn lf_search(&mut self) -> Result<LfTag> {
        let mut supported = false;
        for &kind in LfType::READABLE {
            if matches!(kind, LfType::Electra | LfType::Em4x05) {
                continue;
            }
            if !self.capabilities.contains(&kind.scan()?) {
                continue;
            }
            supported = true;
            if let Some(tag) = self.lf_read(kind, 0)? {
                return Ok(tag);
            }
        }
        if supported {
            return Err(LfNotFound.into());
        }
        Err("Firmware has no supported LF readers".into())
    }
    /// Prepare, program once and verify the T55xx block image.
    pub fn lf_write(&mut self, tag: &LfTag) -> Result<bool> {
        let plan = self.lf_t55xx_prepare(tag)?;
        self.lf_t55xx_program(&plan)?;
        Ok(true)
    }
    pub fn lf_ioprox(&mut self, raw: bool, input: &[u8]) -> Result<LfTag> {
        let id = if raw { 3012 } else { 3013 };
        if input.len() != if raw { 8 } else { 4 } {
            return Err("Invalid ioProx input length".into());
        }
        if raw {
            ioprox_fields(input)?;
        }
        if !self.capabilities.contains(&id) {
            return Err("Firmware lacks ioProx codec command".into());
        }
        let tag = LfTag {
            kind: LfType::Ioprox,
            data: self.command(id, input, Duration::from_secs(3))?,
            variant: None,
        };
        tag.validate()?;
        Ok(tag)
    }
}

/// Validate the existing ioProx raw frame before asking firmware to decode it.
pub fn ioprox_fields(raw: &[u8]) -> Result<[u8; 4]> {
    if raw.len() != 8 {
        return Err("ioProx needs eight raw bytes".into());
    }
    let bit = |position: usize| (raw[position / 8] >> (7 - position % 8)) & 1;
    if bit(9) != 1
        || (0..9).any(|p| bit(p) != 0)
        || [17, 26, 35, 44, 53, 62, 63].iter().any(|&p| bit(p) != 1)
    {
        return Err("Invalid ioProx frame preamble or separators".into());
    }
    let byte = |position: usize| (0..8).fold(0u8, |value, i| (value << 1) | bit(position + i));
    let bytes = [byte(9), byte(18), byte(27), byte(36), byte(45), byte(54)];
    let checksum = bytes[..5].iter().fold(0u8, |sum, b| sum.wrapping_add(*b));
    if 0xffu8.wrapping_sub(checksum) != bytes[5] {
        return Err("Invalid ioProx checksum".into());
    }
    Ok([bytes[2], bytes[1], bytes[3], bytes[4]])
}

#[cfg(all(test, unix))]
mod tests;
