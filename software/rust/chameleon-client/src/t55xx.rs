//! Guarded LF programming. Firmware owns codecs, RF timing and verification.
use crate::{Client, DeviceError, LfTag, LfType, Result};
use std::time::Duration;

const COMMAND: u16 = 3033;
const SNAPSHOT_BYTES: usize = 36;
const PLAN_BYTES: usize = 69;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LfChip {
    /// Readable page-0 words. Block 7 (password) is never returned.
    blocks: [Option<u32>; 7],
}

impl LfChip {
    pub fn blocks(&self) -> &[Option<u32>; 7] {
        &self.blocks
    }

    pub fn configuration(&self) -> u32 {
        self.blocks[0].unwrap()
    }

    pub fn password_enabled(&self) -> bool {
        self.configuration() & 0x10 != 0
    }

    pub fn programming_supported(&self) -> bool {
        self.configuration() & 0x00020310 == 0
    }

    pub fn clock(&self) -> u16 {
        [8, 16, 32, 40, 50, 64, 100, 128][((self.configuration() >> 18) & 7) as usize]
    }

    pub fn modulation(&self) -> &'static str {
        match (self.configuration() >> 12) & 31 {
            0 => "NRZ",
            1 => "PSK1",
            2 => "PSK2",
            3 => "PSK3",
            4 => "FSK1",
            5 => "FSK2",
            6 => "FSK1a",
            7 => "FSK2a",
            8 => "Manchester",
            16 => "Biphase",
            24 => "Diphase",
            _ => "Unknown",
        }
    }
}

/// A prepared write binds the source and observed target together.
pub struct LfWritePlan {
    pub chip: LfChip,
    blocks: Vec<u32>,
    request: Vec<u8>,
    fingerprint: [u8; 4],
}

impl LfWritePlan {
    pub fn changes(&self) -> usize {
        self.blocks
            .iter()
            .enumerate()
            .filter(|(index, word)| self.chip.blocks[*index] != Some(**word))
            .count()
    }
}

fn word(bytes: &[u8]) -> u32 {
    u32::from_be_bytes(bytes.try_into().unwrap())
}

fn snapshot(bytes: &[u8]) -> Result<Option<LfChip>> {
    if bytes.len() < SNAPSHOT_BYTES
        || bytes[0] != 1
        || bytes[1] > 1
        || bytes[2] & 0x80 != 0
        || bytes[3] > 2
        || bytes[32..36] != [0; 4]
    {
        return Err("Invalid T55xx inspection reply".into());
    }
    if bytes[1] == 0 {
        if bytes[1..SNAPSHOT_BYTES].iter().any(|&value| value != 0) {
            return Err("Invalid inconclusive T55xx reply".into());
        }
        return Ok(None);
    }
    if bytes[2] & 7 != 7 {
        return Err("Incomplete T55xx detection reply".into());
    }
    Ok(Some(LfChip {
        blocks: std::array::from_fn(|index| {
            (bytes[2] & (1 << index) != 0).then(|| word(&bytes[4 + index * 4..8 + index * 4]))
        }),
    }))
}

fn prefix(operation: u8) -> Vec<u8> {
    vec![1, operation, 0, 0, 0, 0, 0]
}

impl Client {
    pub fn lf_t55xx_supported(&self) -> bool {
        self.capabilities.contains(&COMMAND)
    }

    fn t55xx_request(&mut self, data: &[u8]) -> Result<Vec<u8>> {
        if !self.lf_t55xx_supported() {
            return Err(
                "Update Chameleon firmware to inspect and safely program T55xx tags".into(),
            );
        }
        let (status, reply) = self.request(COMMAND, data, Duration::from_secs(20))?;
        match status {
            0x40 => Ok(reply),
            _ => Err(DeviceError(status).into()),
        }
    }

    /// Direct-access detection only; an inconclusive reply does not prove a read-only chip.
    pub fn lf_t55xx_inspect(&mut self) -> Result<Option<LfChip>> {
        let reply = self.t55xx_request(&prefix(0))?;
        if reply.len() != SNAPSHOT_BYTES {
            return Err("Invalid T55xx inspection length".into());
        }
        snapshot(&reply)
    }

    pub fn lf_t55xx_prepare(&mut self, tag: &LfTag) -> Result<LfWritePlan> {
        let tag = tag.for_t55xx()?;
        let type_id: u16 = match tag.kind {
            LfType::Em410x => 100,
            LfType::Electra => 104,
            LfType::Hid => 200,
            LfType::Ioprox => 201,
            LfType::Pac => 150,
            LfType::Viking => 170,
            LfType::Jablotron => 180,
            LfType::Idteck => 310,
            LfType::Em4x05 => return Err("EM4x05 programming is unsupported".into()),
        };
        let clock = match tag.variant {
            Some(101) => 16,
            Some(102) => 32,
            _ => 0,
        };
        let mut request = prefix(1);
        request.extend(type_id.to_be_bytes());
        request.extend([clock, tag.data.len() as u8]);
        request.extend(tag.data);
        let reply = self.t55xx_request(&request)?;
        if reply.len() != PLAN_BYTES || !(2..=7).contains(&reply[36]) {
            return Err("Invalid T55xx preparation reply".into());
        }
        let chip = snapshot(&reply)?.ok_or("T55xx target not detected")?;
        let blocks: Vec<_> = reply[37..37 + reply[36] as usize * 4]
            .chunks_exact(4)
            .map(word)
            .collect();
        if blocks
            .iter()
            .enumerate()
            .any(|(index, _)| chip.blocks[index].is_none())
            || ((blocks[0] >> 5) & 7) as usize + 1 != blocks.len()
            || (blocks[0] ^ chip.configuration()) & 0xf0000011 != 0
        {
            return Err("Invalid T55xx write plan".into());
        }
        Ok(LfWritePlan {
            chip,
            blocks,
            request,
            fingerprint: reply[65..69].try_into()?,
        })
    }

    /// The device rechecks the target, writes each changed word once and verifies.
    /// Cancellation after dispatch still retains a successful verification reply.
    pub fn lf_t55xx_program(&mut self, plan: &LfWritePlan) -> Result<()> {
        let mut request = plan.request.clone();
        request[1] = 2;
        request.extend(plan.fingerprint);
        let result = (|| {
            let reply = self.t55xx_request(&request)?;
            if reply.len() != PLAN_BYTES || !matches!(reply[3], 1 | 2) {
                return Err("Invalid T55xx verification reply".into());
            }
            let actual = snapshot(&reply)?.ok_or("T55xx disappeared during verification")?;
            if plan
                .blocks
                .iter()
                .enumerate()
                .any(|(index, word)| actual.blocks[index] != Some(*word))
            {
                return Err("T55xx readback differs from the prepared data".into());
            }
            Ok(())
        })();
        result.map_err(|error: Box<dyn std::error::Error>| {
            if error.is::<crate::RequestCancelled>() || error.downcast_ref::<DeviceError>().is_some_and(|error| matches!(error.0, 0x41 | 0x42 | 0x60 | 0x67 | 0x69 | 0x76)) {
                return error;
            }
            format!("{error}. If programming started, its outcome is uncertain; read the tag before retrying").into()
        })
    }
}
