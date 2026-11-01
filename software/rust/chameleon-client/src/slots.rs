//! Existing serial slot controls. Public slot numbers are always 1..=8.
use crate::command::*;
use crate::{Client, Result};
use serde::{Deserialize, Serialize};
use std::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Sense {
    Hf,
    Lf,
}
impl Sense {
    pub fn code(self) -> u8 {
        if self == Self::Hf { 2 } else { 1 }
    }
}
#[derive(Clone, Debug)]
pub struct SlotSide {
    pub tag_type: u16,
    pub enabled: bool,
    pub name: String,
}
#[derive(Clone, Debug)]
pub struct Slot {
    pub number: u8,
    pub hf: SlotSide,
    pub lf: SlotSide,
}
impl Slot {
    pub fn side(&self, sense: Sense) -> &SlotSide {
        if sense == Sense::Hf {
            &self.hf
        } else {
            &self.lf
        }
    }
}
/// Exact serial-accessible image, including activation data and emulator options.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SlotImage {
    pub version: u8,
    pub tag_type: u16,
    pub identity: Vec<u8>,
    pub options: Vec<u8>,
    pub memory: Vec<u8>,
}
/// Model names and IDs already supported by Chameleon firmware.
pub const SLOT_TYPES: &[(u16, &str)] = &[
    (1000, "classic-mini"),
    (1001, "classic-1k"),
    (1002, "classic-2k"),
    (1003, "classic-4k"),
    (1100, "ntag213"),
    (1101, "ntag215"),
    (1102, "ntag216"),
    (1103, "ultralight"),
    (1104, "ultralight-c"),
    (1105, "ultralight-ev1-48"),
    (1106, "ultralight-ev1-128"),
    (1107, "ntag210"),
    (1108, "ntag212"),
    (1300, "desfire"),
    (1500, "slix-l"),
    (1501, "slix"),
    (2000, "st25tb512-at"),
    (100, "em410x"),
    (104, "electra"),
    (150, "pac"),
    (170, "viking"),
    (180, "jablotron"),
    (200, "hid-prox"),
    (201, "ioprox"),
    (310, "idteck"),
];
pub fn type_name(id: u16) -> String {
    if id == 0 {
        return "Empty".into();
    }
    SLOT_TYPES
        .iter()
        .find(|(value, _)| *value == id)
        .map_or_else(|| format!("Type {id}"), |(_, name)| (*name).into())
}
pub fn type_sense(id: u16) -> Result<Sense> {
    if !SLOT_TYPES.iter().any(|(value, _)| *value == id) {
        return Err("Unsupported slot model".into());
    }
    Ok(if id < 1000 { Sense::Lf } else { Sense::Hf })
}
pub(crate) fn slot_index(number: u8) -> Result<u8> {
    if !(1..=8).contains(&number) {
        return Err("Slot must be between 1 and 8".into());
    }
    Ok(number - 1)
}
pub fn validate_name(name: &str) -> Result<()> {
    if name.is_empty() || name.len() > 32 || name.chars().any(char::is_control) {
        return Err("Use a name of 1–32 UTF-8 bytes without control characters".into());
    }
    Ok(())
}
impl Client {
    pub(crate) fn require_slot_commands(&self, commands: &[u16]) -> Result<()> {
        if let Some(id) = commands.iter().find(|id| !self.capabilities.contains(id)) {
            return Err(format!(
                "Firmware lacks slot command {id}; install matching firmware before changing images"
            )
            .into());
        }
        Ok(())
    }

    pub(crate) fn slot_command(&mut self, id: u16, data: &[u8]) -> Result<Vec<u8>> {
        self.command(id, data, Duration::from_secs(3))
    }
    pub fn active_slot(&mut self) -> Result<u8> {
        match self.slot_command(GET_ACTIVE_SLOT, &[])?.as_slice() {
            [number @ 0..=7] => Ok(number + 1),
            _ => Err("Invalid active slot reply".into()),
        }
    }
    pub fn select_slot(&mut self, number: u8) -> Result<()> {
        let index = slot_index(number)?;
        self.mutate(SET_ACTIVE_SLOT, &[index])?;
        if self.active_slot()? != number {
            return Err("Slot selection verification failed".into());
        }
        Ok(())
    }
    pub fn require_slot_idle(&mut self) -> Result<()> {
        if !self.reader_mode()? {
            return Err("Stop emulation first: slot stop".into());
        }
        Ok(())
    }
    pub fn slots(&mut self) -> Result<Vec<Slot>> {
        let types = self.slot_command(GET_SLOT_INFO, &[])?;
        let enabled = self.slot_command(GET_SLOT_ENABLED, &[])?;
        if types.len() != 32 || enabled.len() != 16 || enabled.iter().any(|v| *v > 1) {
            return Err("Invalid slot list reply".into());
        }
        let mut slots = Vec::new();
        for i in 0..8 {
            let mut side = |offset: usize, sense: Sense| -> Result<SlotSide> {
                let tag_type =
                    u16::from_be_bytes([types[i * 4 + offset * 2], types[i * 4 + offset * 2 + 1]]);
                let (status, bytes) = self.request(
                    GET_SLOT_NAME,
                    &[i as u8, sense.code()],
                    Duration::from_secs(3),
                )?;
                let name = match status {
                    0x68 | 0 => String::from_utf8(bytes)?,
                    0x71 => String::new(),
                    _ => return Err(crate::DeviceError(status).into()),
                };
                Ok(SlotSide {
                    tag_type,
                    enabled: enabled[i * 2 + offset] != 0,
                    name,
                })
            };
            slots.push(Slot {
                number: i as u8 + 1,
                hf: side(0, Sense::Hf)?,
                lf: side(1, Sense::Lf)?,
            });
        }
        Ok(slots)
    }
    pub fn store_slots(&mut self) -> Result<()> {
        self.mutate(SAVE_SLOTS, &[])
    }
    pub fn rename_slot(&mut self, number: u8, sense: Sense, name: &str) -> Result<()> {
        validate_name(name)?;
        let mut data = vec![slot_index(number)?, sense.code()];
        data.extend(name.as_bytes());
        self.require_slot_commands(&[SET_SLOT_NAME, GET_SLOT_NAME, GET_MODE])?;
        self.require_slot_idle()?;
        self.mutate(SET_SLOT_NAME, &data)?;
        if self.slot_command(GET_SLOT_NAME, &data[..2])? != name.as_bytes() {
            return Err("Slot name verification failed".into());
        }
        Ok(())
    }
    pub fn enable_slot(&mut self, number: u8, sense: Sense, enabled: bool) -> Result<()> {
        let index = slot_index(number)?;
        self.require_slot_commands(&[
            SET_SLOT_ENABLED,
            SAVE_SLOTS,
            GET_SLOT_INFO,
            GET_SLOT_ENABLED,
            GET_SLOT_NAME,
        ])?;
        self.with_slot(number, true, |client| {
            client.mutate(SET_SLOT_ENABLED, &[index, sense.code(), u8::from(enabled)])?;
            client.store_slots()?;
            if client.slots()?[index as usize].side(sense).enabled != enabled {
                return Err("Slot enable verification failed".into());
            }
            Ok(())
        })
    }

    pub fn clear_slot(&mut self, number: u8, sense: Sense) -> Result<()> {
        let index = slot_index(number)?;
        self.require_slot_commands(&[
            CLEAR_SLOT,
            SAVE_SLOTS,
            GET_MODE,
            GET_SLOT_INFO,
            GET_SLOT_ENABLED,
            GET_SLOT_NAME,
        ])?;
        self.require_slot_idle()?;
        self.mutate(CLEAR_SLOT, &[index, sense.code()])?;
        self.store_slots()?;
        if self.slots()?[index as usize].side(sense).tag_type != 0 {
            return Err("Slot clear verification failed".into());
        }
        Ok(())
    }
    pub fn initialize_slot(&mut self, number: u8, tag_type: u16) -> Result<()> {
        let index = slot_index(number)?;
        let sense = type_sense(tag_type)?;
        self.require_image_commands(tag_type, false)?;
        self.require_slot_commands(&[
            SET_SLOT_TYPE,
            SET_SLOT_DEFAULT,
            SET_SLOT_ENABLED,
            SAVE_SLOTS,
            GET_SLOT_INFO,
            GET_SLOT_ENABLED,
            GET_SLOT_NAME,
        ])?;
        self.with_slot(number, true, |client| {
            let [hi, lo] = tag_type.to_be_bytes();
            client.mutate(SET_SLOT_TYPE, &[index, hi, lo])?;
            client.mutate(SET_SLOT_DEFAULT, &[index, hi, lo])?;
            client.mutate(SET_SLOT_ENABLED, &[index, sense.code(), 1])?;
            client.store_slots()?;
            let slots = client.slots()?;
            let side = slots[index as usize].side(sense);
            if side.tag_type != tag_type || !side.enabled {
                return Err("Slot initialization verification failed".into());
            }
            let image = client.read_active_slot_image(tag_type)?;
            image.validate()
        })
    }
}
