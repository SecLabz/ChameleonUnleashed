//! Serial image transfers; each mutation is sent once and verified before persistence.
use crate::command::*;
use crate::{Client, Result, Sense, SlotImage, slots::type_sense};

pub(crate) fn geometry(id: u16) -> Result<(usize, usize, usize)> {
    Ok(match id {
        1000 => (16, 20, 0),
        1300 => (1, 0, 0),
        1001 => (16, 64, 0),
        1002 => (16, 128, 0),
        1003 => (16, 256, 0),
        1100 => (4, 45, 11),
        1101 => (4, 135, 11),
        1102 => (4, 231, 11),
        1103 => (4, 16, 0),
        1104 => (4, 48, 0),
        1105 => (4, 20, 13),
        1106 => (4, 41, 13),
        1107 => (4, 20, 11),
        1108 => (4, 41, 11),
        1500 => (1, 112, 0),
        1501 => (1, 192, 0),
        2000 => (1, 77, 0),
        100 => (1, 5, 0),
        104 | 200 => (1, 13, 0),
        150 | 310 => (1, 8, 0),
        170 => (1, 4, 0),
        180 => (1, 5, 0),
        201 => (1, 16, 0),
        _ => return Err("Unsupported slot image type".into()),
    })
}
fn lf_command(id: u16) -> Result<u16> {
    Ok(match id {
        100 | 104 => SET_EM410X_IMAGE,
        200 => SET_HID_IMAGE,
        170 => SET_VIKING_IMAGE,
        150 => SET_PAC_IMAGE,
        201 => SET_IOPROX_IMAGE,
        180 => SET_JABLOTRON_IMAGE,
        310 => SET_IDTECK_IMAGE,
        _ => return Err("Unsupported LF type".into()),
    })
}
impl SlotImage {
    pub fn sense(&self) -> Result<Sense> {
        type_sense(self.tag_type)
    }
    pub fn validate(&self) -> Result<()> {
        if self.tag_type == 1300 {
            crate::DesfireSnapshot::from_slot(self)?;
            // Same 4500-byte buffer as other HF slots. Keep oversized captures
            // on the host; never truncate public files to make a slot fit.
            if self.memory.len() > 4492 {
                return Err(
                    "DESFire image exceeds the 4500-byte slot capacity; keep the JSON capture"
                        .into(),
                );
            }
            return Ok(());
        }
        let (unit, count, extra) = geometry(self.tag_type)?;
        if self.version != 1 || self.memory.len() != unit * (count + extra) {
            return Err("Invalid slot image version or memory length".into());
        }
        let classic = (1000..=1003).contains(&self.tag_type);
        let type2 = (1100..=1108).contains(&self.tag_type);
        if classic || type2 {
            let uid = *self.identity.first().ok_or("Missing slot identity")? as usize;
            if ![4, 7, 10].contains(&uid)
                || self.identity.len() < uid + 5
                || self.identity.len() != uid + 5 + self.identity[uid + 4] as usize
            {
                return Err("Invalid slot activation data".into());
            }
            if classic {
                if self.options.len() != 7
                    || self.options[0] > 3
                    || self.options[1..6].iter().any(|v| *v > 1)
                    || self.options[6] > 2
                {
                    return Err("Invalid Classic emulator settings".into());
                }
            } else if self.options.len() != 3
                || self.options[0] > 1
                || self.options[1] > 3
                || self.options[2] > 1
            {
                return Err("Invalid Type 2 emulator settings".into());
            }
        } else if !self.identity.is_empty() || !self.options.is_empty() {
            return Err("Unexpected slot image fields".into());
        }
        if self.tag_type == 2000 && self.memory[5..8] != [0x33, 2, 0xd0] {
            return Err("Invalid ST25TB512-AT identity".into());
        }
        if matches!(self.tag_type, 1500 | 1501) {
            let raw = &self.memory;
            if raw[..3] != [0xe0, 4, if self.tag_type == 1500 { 3 } else { 1 }]
                || raw[11] & if self.tag_type == 1500 { 0xe0 } else { 0xe8 } != 0
                || raw[32 + (count - 32) / 5 * 4..].iter().any(|v| *v > 1)
            {
                return Err("Invalid SLIX slot image".into());
            }
            if self.tag_type == 1501 && raw[3] & 0x18 != 0x10 {
                return Err("Invalid SLIX UID".into());
            }
        }
        Ok(())
    }
    /// Serial setters cannot restore Type 2 authentication/tearing flags.
    pub fn validate_load(&self) -> Result<()> {
        self.validate()?;
        let (unit, count, extra) = geometry(self.tag_type)?;
        if extra > 0 {
            let counters = extra - 10;
            for i in 0..counters {
                if self.memory[unit * count + i * 4 + 3] != 0 {
                    return Err("Serial firmware cannot restore Type 2 authentication/tearing flags; retain the backup and reset those flags explicitly before loading".into());
                }
            }
        }
        Ok(())
    }
}
fn option_commands(id: u16) -> &'static [u16] {
    if (1000..=1003).contains(&id) {
        &[
            SET_CLASSIC_WRITE_MODE,
            SET_CLASSIC_GEN1A,
            SET_CLASSIC_GEN2,
            SET_CLASSIC_BLOCK_IDENTITY,
            SET_CLASSIC_DETECTION,
            SET_CLASSIC_FIELD_RESET,
            SET_CLASSIC_PRNG,
        ]
    } else {
        &[
            SET_TYPE2_UID_MAGIC,
            SET_TYPE2_WRITE_MODE,
            SET_TYPE2_DETECTION,
        ]
    }
}
impl Client {
    pub(crate) fn require_image_commands(&self, id: u16, write: bool) -> Result<()> {
        let (unit, _, extra) = geometry(id)?;
        let mut commands = vec![];
        if id == 1300 {
            commands.push(GET_DESFIRE_IMAGE);
            if write {
                commands.push(SET_DESFIRE_IMAGE);
            }
        } else if id < 1000 {
            commands.push(lf_command(id)? + 1);
            if write {
                commands.push(lf_command(id)?);
            }
        } else if matches!(id, 1500 | 1501 | 2000) {
            commands.push(if id == 2000 {
                GET_ST25TB_IMAGE
            } else {
                GET_NFCV_IMAGE
            });
            if write {
                commands.push(if id == 2000 {
                    SET_ST25TB_IMAGE
                } else {
                    SET_NFCV_IMAGE
                });
            }
        } else {
            commands.push(GET_ACTIVATION);
            if unit == 16 {
                commands.extend([
                    GET_CLASSIC_MEMORY,
                    GET_CLASSIC_CONFIG,
                    GET_CLASSIC_FIELD_RESET,
                    GET_CLASSIC_PRNG,
                ]);
            } else {
                commands.extend([GET_TYPE2_MEMORY, GET_TYPE2_CONFIG]);
            }
            if write {
                commands.extend([
                    SET_ACTIVATION,
                    if unit == 16 {
                        SET_CLASSIC_MEMORY
                    } else {
                        SET_TYPE2_MEMORY
                    },
                ]);
                commands.extend(option_commands(id));
                if extra > 0 {
                    commands.extend([SET_TYPE2_COUNTER, SET_TYPE2_VERSION, SET_TYPE2_SIGNATURE]);
                }
            }
        }
        self.require_slot_commands(&commands)
    }
    /// Temporarily select a slot in reader mode, restoring selection even on failure.
    pub(crate) fn with_slot<T>(
        &mut self,
        number: u8,
        persist_selection: bool,
        action: impl FnOnce(&mut Self) -> Result<T>,
    ) -> Result<T> {
        crate::slots::slot_index(number)?;
        self.require_slot_commands(&[GET_MODE, GET_ACTIVE_SLOT, SET_ACTIVE_SLOT])?;
        if persist_selection {
            self.require_slot_commands(&[SAVE_SLOTS])?;
        }
        self.require_slot_idle()?;
        let previous = self.active_slot()?;
        let result = (|| {
            if number != previous {
                self.select_slot(number)?;
            }
            action(self)
        })();
        // A broken serial session must never receive another command.
        if !self.usable {
            return Err(format!(
                "{}; selection could not be restored; inspect the device",
                result
                    .err()
                    .map_or("Connection failed".into(), |e| e.to_string())
            )
            .into());
        }
        let cancelled = self
            .cancelled
            .swap(false, std::sync::atomic::Ordering::Relaxed);
        let restore = (|| -> Result<()> {
            if self.active_slot()? != previous {
                self.select_slot(previous)?;
                if persist_selection {
                    self.store_slots()?;
                }
            }
            Ok(())
        })();
        self.cancelled
            .fetch_or(cancelled, std::sync::atomic::Ordering::Relaxed);
        if let Err(error) = restore {
            return Err(format!(
                "Could not restore slot {previous}: {error}; operation result: {}",
                result
                    .as_ref()
                    .err()
                    .map_or("completed".into(), ToString::to_string)
            )
            .into());
        }
        result
    }
    pub fn read_slot_image(&mut self, number: u8, sense: Sense) -> Result<SlotImage> {
        let index = crate::slots::slot_index(number)?;
        if self.supports_slot_snapshot() {
            return self.snapshot_slot(index, sense);
        }
        let id = self.slots()?[index as usize].side(sense).tag_type;
        self.require_image_commands(id, false)?;
        self.with_slot(number, false, |client| client.read_active_slot_image(id))
    }
    pub(crate) fn read_active_slot_image(&mut self, id: u16) -> Result<SlotImage> {
        let (unit, count, extra) = geometry(id)?;
        let mut image = SlotImage {
            version: 1,
            tag_type: id,
            identity: vec![],
            options: vec![],
            memory: vec![],
        };
        if id == 1300 {
            let first = self.slot_command(GET_DESFIRE_IMAGE, &[0, 0, 0, 12])?;
            if first.len() != 14 {
                return Err("Invalid DESFire image header".into());
            }
            let total = u16::from_be_bytes([first[0], first[1]]) as usize;
            if !(12..=4492).contains(&total) {
                return Err("Invalid DESFire slot size".into());
            }
            image.memory.extend(&first[2..]);
            while image.memory.len() < total {
                let offset = image.memory.len();
                let count = (total - offset).min(500);
                let mut data = (offset as u16).to_be_bytes().to_vec();
                data.extend((count as u16).to_be_bytes());
                let part = self.slot_command(GET_DESFIRE_IMAGE, &data)?;
                if part.len() != count + 2 || part[..2] != first[..2] {
                    return Err("Truncated DESFire slot image".into());
                }
                image.memory.extend(&part[2..]);
            }
        } else if id < 1000 {
            image.memory = self.slot_command(lf_command(id)? + 1, &[])?;
            if matches!(id, 100 | 104) && image.memory.len() == count + 2 {
                if image.memory[..2] != id.to_be_bytes() {
                    return Err("Unexpected EM410x image type".into());
                }
                image.memory.drain(..2);
            }
        } else if matches!(id, 1500 | 1501 | 2000) {
            image.memory = self.slot_command(
                if id == 2000 {
                    GET_ST25TB_IMAGE
                } else {
                    GET_NFCV_IMAGE
                },
                &[],
            )?;
        } else {
            image.identity = self.slot_command(GET_ACTIVATION, &[])?;
            if unit == 16 {
                let settings = self.slot_command(GET_CLASSIC_CONFIG, &[])?;
                if settings.len() != 5 {
                    return Err("Invalid Classic settings reply".into());
                }
                image.options = vec![
                    settings[4],
                    settings[1],
                    settings[2],
                    settings[3],
                    settings[0],
                ];
                image
                    .options
                    .extend(self.slot_command(GET_CLASSIC_FIELD_RESET, &[])?);
                image
                    .options
                    .extend(self.slot_command(GET_CLASSIC_PRNG, &[])?);
                if image.options[0] == 4 {
                    image.options[0] = 3;
                }
            } else {
                let settings = self.slot_command(GET_TYPE2_CONFIG, &[])?;
                if settings.len() != 3 {
                    return Err("Invalid Type 2 settings reply".into());
                }
                image.options = vec![
                    settings[1],
                    if settings[2] == 4 { 3 } else { settings[2] },
                    settings[0],
                ];
            }
            for start in (0..count + extra).step_by(32) {
                let length = (count + extra - start).min(32);
                let bytes = self.slot_command(
                    if unit == 16 {
                        GET_CLASSIC_MEMORY
                    } else {
                        GET_TYPE2_MEMORY
                    },
                    &[start as u8, length as u8],
                )?;
                if bytes.len() != length * unit {
                    return Err("Truncated slot memory reply".into());
                }
                image.memory.extend(bytes);
            }
        }
        image.validate()?;
        Ok(image)
    }
    pub fn load_slot_image(&mut self, number: u8, image: &SlotImage) -> Result<()> {
        image.validate_load()?;
        self.require_image_commands(image.tag_type, true)?;
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
            client.initialize_slot(number,image.tag_type)?;
            let id=image.tag_type;
            let (unit,count,extra)=geometry(id)?;
            if id==1300 {
                for (index,bytes) in image.memory.chunks(500).enumerate() {
                    let mut data=((index*500) as u16).to_be_bytes().to_vec();
                    data.extend((image.memory.len() as u16).to_be_bytes());data.extend(bytes);
                    client.mutate(SET_DESFIRE_IMAGE,&data)?;
                }
            }
            else if id<1000 { client.mutate(lf_command(id)?,&image.memory)?; }
            else if matches!(id,1500|1501|2000) { client.mutate(if id==2000 {SET_ST25TB_IMAGE} else {SET_NFCV_IMAGE},&image.memory)?; }
            else {
                client.mutate(SET_ACTIVATION,&image.identity)?;
                for (chunk,bytes) in image.memory[..count*unit].chunks(32*unit).enumerate() {
                    let mut data=vec![(chunk*32) as u8];
                    if unit==4 { data.push((bytes.len()/unit) as u8); }
                    data.extend(bytes);
                    client.mutate(if unit==16 {SET_CLASSIC_MEMORY} else {SET_TYPE2_MEMORY},&data)?;
                }
                if extra>0 {
                    let data=&image.memory[count*unit..];
                    for i in 0..extra-10 {
                        client.mutate(SET_TYPE2_COUNTER,&[(i as u8)|0x80,data[i*4],data[i*4+1],data[i*4+2]])?;
                    }
                    client.mutate(SET_TYPE2_VERSION,&data[(extra-10)*4..(extra-8)*4])?;
                    client.mutate(SET_TYPE2_SIGNATURE,&data[(extra-8)*4..])?;
                }
                client.write_slot_options(id,&image.options)?;
            }
            if client.read_active_slot_image(id)? != *image { return Err("Slot readback differs; replacement may already be persistent. Inspect the slot before continuing".into()); }
            client.store_slots()?;
            Ok(())
        })
    }
    pub(crate) fn write_slot_options(&mut self, id: u16, options: &[u8]) -> Result<()> {
        let commands = option_commands(id);
        if commands.len() != options.len() {
            return Err("Invalid emulator settings length".into());
        }
        for (&command, &value) in commands.iter().zip(options) {
            self.mutate(command, &[value])?;
        }
        Ok(())
    }
    pub fn configure_slot(&mut self, number: u8, sense: Sense, options: &[u8]) -> Result<()> {
        let index = crate::slots::slot_index(number)?;
        let id = self.slots()?[index as usize].side(sense).tag_type;
        self.require_image_commands(id, false)?;
        self.require_slot_commands(option_commands(id))?;
        self.with_slot(number, true, |client| {
            if !matches!(id,1000..=1003|1100..=1108) {
                return Err("This model has no configurable emulator options".into());
            }
            let mut image = client.read_active_slot_image(id)?;
            image.options = options.to_vec();
            image.validate()?;
            client.write_slot_options(id, options)?;
            if client.read_active_slot_image(id)?.options != options {
                return Err("Emulator settings verification failed".into());
            }
            client.store_slots()
        })
    }
}
