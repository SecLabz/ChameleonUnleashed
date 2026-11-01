//! Stored-image presentation reuses the reader renderers and image codecs.
use crate::{desfire, slot_file, type2, ui};
use chameleon_client::{Result, SlotImage};
use nfc_native::formats::{
    desfire::Capture,
    image::{Model, layout},
};
use std::fmt::Write;

pub(super) fn text(slot: &SlotImage, color: bool) -> Result<String> {
    slot.validate()?;
    if slot.tag_type < 1000 {
        let mut text = String::from("\n");
        for (index, bytes) in slot.memory.chunks(16).enumerate() {
            writeln!(
                text,
                "{}",
                ui::hex_row(
                    &format!("{:04X}", index * 16),
                    &bytes.iter().copied().map(Some).collect::<Vec<_>>(),
                    16,
                    "",
                    color
                )
            )
            .unwrap();
        }
        return Ok(text);
    }

    let model = slot_file::model(slot.tag_type)?;
    // Dump stored memory even when it would fail portable-image validation.
    if model.classic().is_some() {
        let blocks = slot
            .memory
            .chunks_exact(16)
            .map(|block| block.iter().copied().map(Some).collect())
            .collect::<Vec<_>>();
        return Ok(ui::classic_dump(&blocks, color));
    }
    if let Some(profile) = model.type2_profile() {
        let pages = slot.memory[..profile.pages * 4]
            .chunks_exact(4)
            .map(|page| Some(page.to_vec()))
            .collect::<Vec<_>>();
        return Ok(type2::memory_text(model, &pages, 0, color));
    }
    let image = slot_file::to_image(slot)?;
    if slot.tag_type == 1300 {
        if image.model == Model::IsoDep {
            return Ok("Identity only · no file data stored\n".into());
        }
        let capture = Capture::from_image(&image)?;
        let mut text = format!(
            "Public data · {} applications · directory {}\n",
            capture.applications.len(),
            desfire::state_text(capture.applications_state)
        );
        text.push_str(&desfire::files_text(&capture, color));
        return Ok(text);
    }

    let words = image.model == Model::St25tb512At;
    let mut text = if words {
        String::from("32-bit words · MSB first\n\n")
    } else {
        String::from("\n")
    };
    for (index, bytes) in image.memory.chunks_exact(4).enumerate() {
        if words {
            let role = if matches!(index, 5 | 6) {
                "Counter"
            } else {
                ""
            };
            writeln!(
                text,
                "{}",
                ui::word_row(
                    &format!("{index:03}"),
                    Some(u32::from_le_bytes(bytes.try_into()?)),
                    role,
                    color
                )
            )
            .unwrap();
        } else {
            let locked = image.metadata[layout::VIC_METADATA_HEADER_BYTES + index] != 0;
            writeln!(
                text,
                "{}{}",
                ui::hex_row(
                    &format!("{index:03}"),
                    &bytes.iter().copied().map(Some).collect::<Vec<_>>(),
                    4,
                    "",
                    color
                ),
                if locked {
                    format!("  {}", ui::paint("locked", ui::WARN, color))
                } else {
                    String::new()
                }
            )
            .unwrap();
        }
    }
    if words {
        writeln!(
            text,
            "{}",
            ui::word_row(
                "SYS",
                Some(u32::from_le_bytes(image.metadata[..4].try_into()?)),
                "sysOTP",
                color
            )
        )
        .unwrap();
    }
    Ok(text)
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::DesfireSnapshot;

    fn activation() -> Vec<u8> {
        vec![4, 1, 2, 3, 4, 0x04, 0, 0x08, 0]
    }

    #[test]
    fn classic_slot_keeps_stored_keys_and_sector_geometry_with_aligned_ascii() {
        let mut memory = vec![b'A'; 4096];
        memory[48..64].copy_from_slice(&[
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xff, 7, 0x80, 0x69, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
            0xff,
        ]);
        let slot = SlotImage {
            version: 1,
            tag_type: 1003,
            identity: activation(),
            options: vec![0; 7],
            memory,
        };
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("11 22 33 44 55 66  FF 07 80 69  AA BB CC DD EE FF"));
        assert!(!plain.contains("??"));
        assert_eq!(plain.matches("Sector ").count(), 40);
        assert!(plain.contains("Sector 32\n  128"));
        assert!(plain.contains("Sector 33\n  144"));
        let rows: Vec<_> = plain
            .lines()
            .filter(|line| line.starts_with("  "))
            .collect();
        assert_eq!(rows.len(), 256);
        let ascii = rows[0].find('|').unwrap();
        assert!(rows.iter().all(|line| line.find('|') == Some(ascii)));
    }

    #[test]
    fn type2_slot_renders_physical_pages_without_treating_extra_metadata_as_pages() {
        let mut slot = SlotImage {
            version: 1,
            tag_type: 1100,
            identity: activation(),
            options: vec![0; 3],
            memory: vec![0; (45 + 11) * 4],
        };
        slot.memory[43 * 4..45 * 4].fill(0x42);
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("  043  42 42 42 42  |BBBB|  PWD"));
        assert!(plain.contains("  044  42 42 42 42  |BBBB|  PACK"));
        assert!(!plain.contains("  045 "));
        slot.memory.pop();
        assert!(text(&slot, false).is_err());
    }

    #[test]
    fn st25tb_slot_keeps_word_byte_order_counters_and_system_block() {
        let mut slot = SlotImage {
            version: 1,
            tag_type: 2000,
            identity: vec![],
            options: vec![],
            memory: vec![0; 77],
        };
        slot.memory[5..8].copy_from_slice(&[0x33, 2, 0xd0]);
        slot.memory[8..12].copy_from_slice(&[1, 2, 3, 4]);
        slot.memory[72..76].copy_from_slice(&[0x10, 0x20, 0x30, 0x40]);
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("  000  04030201\n"));
        assert!(plain.contains("  005  00000000  Counter\n"));
        assert!(plain.contains("  006  00000000  Counter\n"));
        assert!(plain.contains("  SYS  40302010  sysOTP\n"));
    }

    #[test]
    fn slix_slot_keeps_rf_byte_order_and_locks() {
        let mut slot = SlotImage {
            version: 1,
            tag_type: 1501,
            identity: vec![],
            options: vec![],
            memory: vec![0; 192],
        };
        slot.memory[..4].copy_from_slice(&[0xe0, 4, 1, 0x10]);
        slot.memory[32..36].copy_from_slice(&[1, 2, 3, 4]);
        slot.memory[32 + 32 * 4] = 1;
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("  000  01 02 03 04  |....|  locked\n"));
        assert!(plain.contains("  027  00 00 00 00  |....|\n"));
        assert!(!plain.contains("  028 "));
    }

    #[test]
    fn desfire_slot_retains_denied_files_and_identity_only_state() {
        let snapshot: DesfireSnapshot = serde_json::from_str(include_str!(
            "../../../../../resource/tools/tests/desfire-public.json"
        ))
        .unwrap();
        let slot = snapshot.slot_image().unwrap();
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("Application 335848"));
        assert!(plain.contains("data denied · 0 bytes captured"));
        assert!(plain.contains("data read ·"));

        let image = nfc_native::formats::image::Image {
            model: Model::IsoDep,
            uid: vec![1, 2, 3, 4],
            memory: vec![],
            metadata: vec![b'D', b'F', b'U', b'1', 4, 0, 0x20, 2, 2, 0],
            secrets: vec![],
        };
        let slot = DesfireSnapshot::from_image(&image)
            .unwrap()
            .slot_image()
            .unwrap();
        assert_eq!(
            text(&slot, false).unwrap(),
            "Identity only · no file data stored\n"
        );
    }

    #[test]
    fn lf_slot_retains_all_bytes_and_pads_the_partial_ascii_row() {
        let slot = SlotImage {
            version: 1,
            tag_type: 100,
            identity: vec![],
            options: vec![],
            memory: vec![1, 2, b'A', b'B', 0xff],
        };
        let plain = text(&slot, false).unwrap();
        assert_eq!(ui::without_colors(&text(&slot, true).unwrap()), plain);
        assert!(plain.contains("  0000  01 02 41 42  FF"));
        assert!(plain.contains("|..AB.           |\n"));
    }
}
