//! File IO and conversion use the shared image codecs; serial layouts stay local.
use chameleon_client::{DesfireSnapshot, Result, SlotImage};
use nfc_native::formats::{
    flipper,
    image::{Image, Model},
};
use std::{
    io::{Read, Write},
    path::Path,
};

pub fn model(id: u16) -> Result<Model> {
    Ok(match id {
        1000 => Model::ClassicMini,
        1001 => Model::Classic1k,
        1002 => Model::Classic2k,
        1003 => Model::Classic4k,
        1100 => Model::Ntag213,
        1101 => Model::Ntag215,
        1102 => Model::Ntag216,
        1103 => Model::Ultralight,
        1104 => Model::UltralightC,
        1105 => Model::UltralightEv1_48,
        1106 => Model::UltralightEv1_128,
        1107 => Model::Ntag210,
        1108 => Model::Ntag212,
        1300 => Model::Desfire,
        1500 => Model::SlixL,
        1501 => Model::Slix,
        2000 => Model::St25tb512At,
        _ => return Err("Use JSON for this slot model".into()),
    })
}
pub fn load(
    path: &Path,
    chip_id: Option<u8>,
    emulator_defaults: bool,
    allowed: bool,
) -> Result<SlotImage> {
    let format = path.extension().and_then(|s| s.to_str());
    if !matches!(format, Some("json" | "nfc")) {
        return Err("Choose a .json or .nfc input file".into());
    }
    let mut bytes = Vec::new();
    std::fs::File::open(path)?
        .take(128 * 1024 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > 128 * 1024 {
        return Err("Slot file exceeds 128 KiB".into());
    }
    let image = if format == Some("json") {
        let value: serde_json::Value = serde_json::from_slice(&bytes)?;
        if value.get("protocol").and_then(|v| v.as_str()) == Some("desfire") {
            serde_json::from_value::<DesfireSnapshot>(value)?.slot_image()?
        } else if value.get("document").is_some() {
            from_type2_capture(&value, emulator_defaults, allowed)?
        } else {
            serde_json::from_value(value)?
        }
    } else {
        let (image, warnings) = flipper::parse_with_warnings(std::str::from_utf8(&bytes)?)?;
        for warning in warnings {
            crate::ui::warning(warning);
        }
        crate::ui::warning(
            "Imported image uses default Chameleon activation and emulator settings",
        );
        from_image(&image, chip_id)?
    };
    image.validate_load()?;
    Ok(image)
}

fn from_type2_capture(
    value: &serde_json::Value,
    emulator_defaults: bool,
    allowed: bool,
) -> Result<SlotImage> {
    let document = &value["document"];
    if value["version"] != 1 || document["kind"] != "type2" {
        return Err("Expected a version 1 NTAG/Ultralight capture or a JSON slot backup".into());
    }
    let id: u16 = serde_json::from_value(document["type"].clone())?;
    let model = model(id)?;
    let profile = model
        .type2_profile()
        .ok_or("Expected an NTAG/Ultralight model")?;
    let uid: Vec<u8> = serde_json::from_value(document["card"]["uid"].clone())?;
    let atqa: [u8; 2] = serde_json::from_value(document["card"]["atqa"].clone())?;
    let sak: u8 = serde_json::from_value(document["card"]["sak"].clone())?;
    let ats: Vec<u8> = document["card"]
        .get("ats")
        .map(|value| serde_json::from_value(value.clone()))
        .transpose()?
        .unwrap_or_default();
    if document["unitBytes"] != 4 || uid.len() != 7 || sak != 0 || !ats.is_empty() {
        return Err("Capture identity conflicts with its Type 2 model".into());
    }
    let options: [u8; 5] = serde_json::from_value(document["emulator"]["options"].clone())?;
    if u16::from_be_bytes([options[3], options[4]]) as usize != profile.pages {
        return Err("Capture emulator settings conflict with its model".into());
    }
    let pages: Vec<Option<[Option<u8>; 4]>> = serde_json::from_value(document["memory"].clone())?;
    let extra: Vec<Option<[Option<u8>; 4]>> =
        serde_json::from_value(document["emulator"]["extra"].clone())?;
    let counters = if profile.version.is_empty() {
        0
    } else {
        profile.counters.max(1)
    };
    if pages.len() != profile.pages || extra.len() != counters + if counters == 0 { 0 } else { 10 }
    {
        return Err("Capture memory length conflicts with its model".into());
    }
    let mut missing = false;
    let mut memory = Vec::new();
    for (index, page) in pages.iter().chain(&extra).enumerate() {
        for byte in 0..4 {
            let value = page.as_ref().and_then(|page| page[byte]);
            if value.is_none() && index < profile.credential_start {
                return Err(format!(
                    "Capture page {index} contains unread bytes; read the tag again before loading"
                )
                .into());
            }
            missing |= value.is_none();
            let default = if index == profile.credential_start && counters != 0 {
                0xff // Password; PACK and Ultralight C key defaults remain zero.
            } else if index >= profile.pages + counters && index < profile.pages + counters + 2 {
                profile.version[(index - profile.pages - counters) * 4 + byte]
            } else {
                0
            };
            memory.push(value.unwrap_or(default));
        }
    }
    let slot = SlotImage {
        version: 1,
        tag_type: id,
        identity: identity(&uid, [atqa[1], atqa[0]], sak),
        options: options[..3].to_vec(),
        memory,
    };
    // Keep the existing serial restrictions and portable model/version validation.
    slot.validate_load()?;
    to_image(&slot)?;
    if missing {
        if !emulator_defaults {
            if !allowed {
                return Err("Capture credentials or metadata are unknown; use --emulator-defaults to fill only those fields, or keep the JSON capture".into());
            }
            if !crate::ui::confirm(
                "Use emulator defaults for unknown credentials and metadata?\nPassword FFFFFFFF · other credentials/counters/signature zero · model version\nAuthentication and originality checks may fail; the capture file stays unchanged.",
                false,
                allowed,
            )? {
                return Err(crate::selection::Back.into());
            }
        }
        crate::ui::warning(
            "Unknown credentials and metadata use emulator defaults; authentication and originality checks may fail. The capture file is unchanged.",
        );
    }
    Ok(slot)
}
pub fn export(image: &SlotImage, path: &Path) -> Result<()> {
    let bytes = match path.extension().and_then(|s| s.to_str()) {
        Some("json") => serde_json::to_vec_pretty(image)?,
        Some("nfc") => {
            crate::ui::warning(
                "Flipper export omits Chameleon activation overrides and emulator settings; use JSON to preserve them",
            );
            if matches!(image.tag_type, 1500 | 1501) {
                crate::ui::warning(
                    "Export omits reserved SLIX slot bytes; use JSON for exact backups",
                );
            }
            flipper::export(&to_image(image)?)?.into_bytes()
        }
        _ => return Err("Choose a .json or .nfc output file".into()),
    };
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)?;
    file.write_all(&bytes)?;
    Ok(())
}
fn identity(uid: &[u8], atqa: [u8; 2], sak: u8) -> Vec<u8> {
    let mut bytes = vec![uid.len() as u8];
    bytes.extend(uid);
    bytes.extend(atqa);
    bytes.extend([sak, 0]);
    bytes
}
fn from_image(image: &Image, chip_id: Option<u8>) -> Result<SlotImage> {
    image.validate()?;
    if matches!(image.model, Model::Desfire | Model::IsoDep) {
        return DesfireSnapshot::from_image(image)?.slot_image();
    }
    let id = chameleon_client::SLOT_TYPES
        .iter()
        .find_map(|(id, _)| (model(*id).ok() == Some(image.model)).then_some(*id))
        .ok_or("Unsupported Chameleon model")?;
    let mut slot = SlotImage {
        version: 1,
        tag_type: id,
        identity: vec![],
        options: vec![],
        memory: image.memory.clone(),
    };
    if let Some((_, _, atqa, sak)) = image.model.classic() {
        slot.identity = identity(&image.uid, [atqa[1], atqa[0]], sak);
        slot.options = vec![0, 0, 0, 0, 0, 0, 1];
    } else if let Some(profile) = image.model.type2_profile() {
        slot.identity = identity(&image.uid, [0x44, 0], 0);
        slot.options = vec![0, 0, 0];
        if !profile.version.is_empty() {
            let counters = if matches!(id, 1105 | 1106) { 3 } else { 1 };
            let mut extra = vec![0; 4 * counters];
            if image.metadata[56..60] != [0, 0, 0, 0] {
                return Err(
                    "Serial firmware cannot import nonzero Type 2 authentication attempts".into(),
                );
            }
            for (i, chunk) in extra.chunks_exact_mut(4).enumerate() {
                let source = if profile.counters == 1 { 2 } else { i };
                if profile.counters != 0 {
                    if image.metadata[53 + source] != 0xbd {
                        return Err(
                            "Serial firmware cannot import this Type 2 tearing state".into()
                        );
                    }
                    chunk[..3].copy_from_slice(&image.metadata[44 + source * 3..47 + source * 3]);
                }
            }
            extra.extend(&image.metadata[4..12]);
            extra.extend(&image.metadata[12..44]);
            slot.memory.extend(extra);
        }
    } else if id == 2000 {
        let chip_id = chip_id.ok_or(
            "ST25TB .nfc files omit the Chip ID; supply --chip-id or use a JSON slot backup",
        )?;
        slot.memory = image.uid.iter().rev().copied().collect();
        slot.memory.extend(&image.memory);
        slot.memory.extend(&image.metadata);
        slot.memory.push(chip_id);
    } else {
        let stored = if id == 1500 { 16 } else { 32 };
        let count = image.memory.len() / 4;
        let flags = image.metadata[4];
        if flags & 0x60 != 0 {
            return Err("This emulator cannot preserve protected EAS/AFI settings".into());
        }
        let mut raw = vec![0; 32 + stored * 5];
        raw[..8].copy_from_slice(&image.uid);
        raw[8..11].copy_from_slice(&image.metadata[5..8]);
        raw[11] = (flags & 3) | ((flags & 4) << 2) | ((flags & 8) >> 1) | ((flags & 16) >> 1);
        let offsets: &[usize] = if id == 1500 { &[20, 24, 28] } else { &[28] };
        for (i, &offset) in offsets.iter().enumerate() {
            for j in 0..4 {
                raw[offset + j] = image.secrets[i * 4 + 3 - j];
            }
        }
        raw[32..32 + count * 4].copy_from_slice(&image.memory);
        raw[32 + stored * 4..32 + stored * 4 + count].copy_from_slice(&image.metadata[12..]);
        slot.memory = raw;
        crate::ui::warning(
            "Imported SLIX reserved slot bytes use defaults; use JSON for exact backups",
        );
    }
    slot.validate_load()?;
    Ok(slot)
}
pub(crate) fn to_image(slot: &SlotImage) -> Result<Image> {
    slot.validate()?;
    if slot.tag_type == 1300 {
        return DesfireSnapshot::from_slot(slot)?.image();
    }
    let model = model(slot.tag_type)?;
    let mut image = Image {
        model,
        uid: vec![],
        memory: slot.memory.clone(),
        metadata: vec![],
        secrets: vec![],
    };
    if model.classic().is_some() || model.type2_profile().is_some() {
        let length = slot.identity[0] as usize;
        image.uid = slot.identity[1..1 + length].to_vec();
        if let Some(profile) = model.type2_profile() {
            let end = profile.pages * 4;
            image.memory.truncate(end);
            if !profile.version.is_empty() {
                let counters = if matches!(slot.tag_type, 1105 | 1106) {
                    3
                } else {
                    1
                };
                let extra = &slot.memory[end..];
                let mut meta = b"NTA1".to_vec();
                meta.extend(&extra[counters * 4..]);
                meta.resize(60, 0);
                meta[53..56].fill(0xbd);
                for i in 0..counters {
                    let dest = if profile.counters == 1 { 2 } else { i };
                    if profile.counters != 0 {
                        meta[44 + dest * 3..47 + dest * 3]
                            .copy_from_slice(&extra[i * 4..i * 4 + 3]);
                        meta[53 + dest] = if extra[i * 4 + 3] & 0x80 != 0 {
                            0
                        } else {
                            0xbd
                        };
                    }
                }
                meta[56] = extra[3] & 15;
                image.metadata = meta;
            }
        }
    } else if slot.tag_type == 2000 {
        image.uid = slot.memory[..8].iter().rev().copied().collect();
        image.memory = slot.memory[8..72].to_vec();
        image.metadata = slot.memory[72..76].to_vec();
    } else {
        let count = if slot.tag_type == 1500 { 8 } else { 28 };
        let stored = if count == 8 { 16 } else { 32 };
        let raw = &slot.memory;
        image.uid = raw[..8].to_vec();
        image.memory = raw[32..32 + count * 4].to_vec();
        image.metadata = b"VIC1".to_vec();
        let flags = raw[11];
        image
            .metadata
            .push((flags & 3) | ((flags & 16) >> 2) | ((flags & 4) << 1) | ((flags & 8) << 1));
        image.metadata.extend(&raw[8..11]);
        image.metadata.extend([4, count as u8, 0, 0]);
        image
            .metadata
            .extend(&raw[32 + stored * 4..32 + stored * 4 + count]);
        let offsets: &[usize] = if count == 8 { &[20, 24, 28] } else { &[28] };
        for &offset in offsets {
            image.secrets.extend(raw[offset..offset + 4].iter().rev());
        }
    }
    image.validate()?;
    Ok(image)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn type2_capture(id: u16, complete: bool) -> serde_json::Value {
        let model = model(id).unwrap();
        let profile = model.type2_profile().unwrap();
        let memory: Vec<_> = (0..profile.pages)
            .map(|page| (complete || page < profile.credential_start).then(|| vec![page as u8; 4]))
            .collect();
        let mut value = crate::type2::document(
            &chameleon_client::Card {
                uid: vec![4, 1, 2, 3, 4, 5, 6],
                atqa: [0x44, 0],
                sak: 0,
            },
            model,
            &memory,
            None,
        );
        if complete && !profile.version.is_empty() {
            let counters = profile.counters.max(1);
            let mut extra = vec![vec![0; 4]; counters + 10];
            for (page, bytes) in profile.version.chunks_exact(4).enumerate() {
                extra[counters + page] = bytes.to_vec();
            }
            extra[0][0] = 7;
            extra[counters + 2].fill(0x42);
            value["document"]["emulator"]["extra"] = serde_json::json!(extra);
        }
        value
    }

    #[test]
    fn native_type2_captures_import_with_explicit_defaults_only_for_hidden_fields() {
        for id in 1100..=1108 {
            let value = type2_capture(id, false);
            let original = value.clone();
            let profile = model(id).unwrap().type2_profile().unwrap();
            if id != 1103 {
                assert!(
                    from_type2_capture(&value, false, false)
                        .unwrap_err()
                        .to_string()
                        .contains("--emulator-defaults")
                );
            }
            let slot = from_type2_capture(&value, true, false).unwrap();
            assert_eq!(slot.tag_type, id);
            assert_eq!(slot.identity, [7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0]);
            assert_eq!(slot.options, [0, 0, 0]);
            for page in 0..profile.credential_start {
                assert_eq!(&slot.memory[page * 4..page * 4 + 4], &[page as u8; 4]);
            }
            if !profile.version.is_empty() {
                assert_eq!(
                    &slot.memory[profile.credential_start * 4..][..4],
                    &[0xff; 4]
                );
                let version = (profile.pages + profile.counters.max(1)) * 4;
                assert_eq!(&slot.memory[version..version + 8], profile.version);
            }
            assert_eq!(value, original, "capture {id} must remain lossless");
        }
    }

    #[test]
    fn complete_browser_type2_captures_preserve_settings_credentials_and_metadata() {
        for id in 1100..=1108 {
            let mut value = type2_capture(id, true);
            value["document"]["emulator"]["options"][0] = 1.into();
            value["document"]["emulator"]["options"][1] = 2.into();
            value["document"]["emulator"]["options"][2] = 1.into();
            let slot = from_type2_capture(&value, false, false).unwrap();
            let pages: Vec<Vec<u8>> =
                serde_json::from_value(value["document"]["memory"].clone()).unwrap();
            let extra: Vec<Vec<u8>> =
                serde_json::from_value(value["document"]["emulator"]["extra"].clone()).unwrap();
            assert_eq!(
                slot.memory,
                pages.into_iter().chain(extra).flatten().collect::<Vec<_>>()
            );
            assert_eq!(slot.options, [1, 2, 1]);
            assert_eq!(from_type2_capture(&value, true, false).unwrap(), slot);
        }
    }

    #[test]
    fn type2_defaults_preserve_known_bytes_in_partial_credentials_and_metadata() {
        let mut value = type2_capture(1100, true);
        value["document"]["memory"][43] = serde_json::json!([0x12, null, 0x34, null]);
        value["document"]["emulator"]["extra"][0] = serde_json::json!([9, null, 1, null]);
        value["document"]["emulator"]["extra"][3] = serde_json::json!([0x42, null, 0x43, null]);
        let slot = from_type2_capture(&value, true, false).unwrap();
        assert_eq!(&slot.memory[43 * 4..44 * 4], &[0x12, 0xff, 0x34, 0xff]);
        assert_eq!(&slot.memory[45 * 4..46 * 4], &[9, 0, 1, 0]);
        assert_eq!(&slot.memory[48 * 4..49 * 4], &[0x42, 0, 0x43, 0]);
    }

    #[test]
    fn type2_defaults_never_mask_invalid_captures_or_unread_noncredential_pages() {
        let invalid = [
            ("/version", serde_json::json!(2)),
            ("/document/type", serde_json::json!(1001)),
            ("/document/kind", serde_json::json!("classic")),
            ("/document/unitBytes", serde_json::json!(16)),
            ("/document/card/uid", serde_json::json!([1, 2, 3, 4])),
            ("/document/card/sak", serde_json::json!(8)),
            ("/document/card/atqa", serde_json::json!([256, 0])),
            ("/document/card/ats", serde_json::json!([1])),
            ("/document/memory", serde_json::json!([])),
            ("/document/memory/4", serde_json::Value::Null),
            ("/document/memory/41/0", serde_json::Value::Null),
            ("/document/memory/4/0", serde_json::json!(256)),
            ("/document/memory/43", serde_json::json!([1, 2, 3])),
            ("/document/emulator/options", serde_json::json!([0, 0, 0])),
            ("/document/emulator/options/0", serde_json::json!(2)),
            ("/document/emulator/options/4", serde_json::json!(135)),
            ("/document/emulator/extra", serde_json::json!([])),
            ("/document/emulator/extra/0/3", serde_json::json!(0x80)),
            ("/document/emulator/extra/2/2", serde_json::json!(0x11)),
        ];
        for (pointer, replacement) in invalid {
            let mut value = type2_capture(1100, true);
            *value.pointer_mut(pointer).unwrap() = replacement;
            assert!(
                from_type2_capture(&value, true, false).is_err(),
                "{pointer}"
            );
        }
    }

    #[test]
    fn slot_files_roundtrip_json_and_nfc_and_reject_binary_images() {
        let directory = std::env::temp_dir().join(format!(
            "chameleon-slot-formats-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        let image = Image {
            model: Model::Ultralight,
            uid: vec![4, 1, 2, 3, 4, 5, 6],
            memory: vec![0; 64],
            metadata: vec![],
            secrets: vec![],
        };
        let slot = from_image(&image, None).unwrap();
        for extension in ["json", "nfc"] {
            let path = directory.join(format!("card.{extension}"));
            export(&slot, &path).unwrap();
            assert_eq!(load(&path, None, false, false).unwrap(), slot);
            let saved = std::fs::read(&path).unwrap();
            assert!(export(&slot, &path).is_err());
            assert_eq!(std::fs::read(&path).unwrap(), saved);
        }
        let path = directory.join("card.nfci");
        assert!(export(&slot, &path).is_err());
        assert!(!path.exists());
        std::fs::write(&path, image.encode().unwrap()).unwrap();
        assert!(load(&path, None, false, false).is_err());
        let renamed = directory.join("binary.nfc");
        std::fs::rename(&path, &renamed).unwrap();
        assert!(load(&renamed, None, false, false).is_err());
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn type2_portable_roundtrip_keeps_versions_signatures_and_counters() {
        for id in 1100..=1108 {
            let model = model(id).unwrap();
            let profile = model.type2_profile().unwrap();
            let mut image = Image {
                model,
                uid: vec![4, 1, 2, 3, 4, 5, 6],
                memory: vec![0; profile.pages * 4],
                metadata: vec![],
                secrets: vec![],
            };
            if !profile.version.is_empty() {
                image.metadata = b"NTA1".to_vec();
                image.metadata.extend(profile.version);
                image.metadata.resize(60, 0);
                image.metadata[12..44].fill(0x42);
                image.metadata[53..56].fill(0xbd);
                for index in profile.counter_indices() {
                    image.metadata[44 + index * 3] = 7;
                }
            }
            let slot = from_image(&image, None).unwrap();
            assert_eq!(to_image(&slot).unwrap(), image, "model {id}");
        }
    }
    #[test]
    fn st25tb_import_requires_explicit_chip_id() {
        let image = Image {
            model: Model::St25tb512At,
            uid: vec![0xd0, 2, 0x33, 4, 5, 6, 7, 8],
            memory: vec![0; 64],
            metadata: vec![1, 2, 3, 4],
            secrets: vec![],
        };
        assert!(from_image(&image, None).is_err());
        let slot = from_image(&image, Some(123)).unwrap();
        assert_eq!(slot.memory[76], 123);
        assert_eq!(to_image(&slot).unwrap(), image);
    }
    #[test]
    fn unsupported_type2_authentication_state_is_rejected_before_import() {
        let model = Model::Ntag213;
        let profile = model.type2_profile().unwrap();
        let mut image = Image {
            model,
            uid: vec![4; 7],
            memory: vec![0; profile.pages * 4],
            metadata: b"NTA1".to_vec(),
            secrets: vec![],
        };
        image.metadata.extend(profile.version);
        image.metadata.resize(60, 0);
        image.metadata[53..56].fill(0xbd);
        image.metadata[56] = 1;
        assert!(
            from_image(&image, None)
                .unwrap_err()
                .to_string()
                .contains("authentication")
        );
    }
    #[test]
    fn json_backup_retains_custom_activation_options_and_reserved_bytes() {
        let mut image = SlotImage {
            version: 1,
            tag_type: 1500,
            identity: vec![],
            options: vec![],
            memory: vec![0; 112],
        };
        image.memory[..3].copy_from_slice(&[0xe0, 4, 3]);
        image.memory[70] = 0xa5;
        let bytes = serde_json::to_vec(&image).unwrap();
        let restored: SlotImage = serde_json::from_slice(&bytes).unwrap();
        restored.validate().unwrap();
        assert_eq!(image, restored);
    }
}
