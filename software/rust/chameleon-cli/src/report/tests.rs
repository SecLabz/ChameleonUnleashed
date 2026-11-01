use super::*;
use nfc_native::autopwn::{KeyOrigin, PrngMode, SectorKeys};

fn complete() -> Report {
    let mut memory = vec![Some(vec![b'A'; 16]); 20];
    for sector in 0..5 {
        memory[usize::from(trailer(sector))].as_mut().unwrap()[6..10]
            .copy_from_slice(&[0xff, 0x07, 0x80, 0x69]);
    }
    Report {
        card: Card {
            uid: vec![1, 2, 3, 4],
            atqa: [4, 0],
            sak: 9,
        },
        summary: AutoPwnSummary {
            uid: vec![1, 2, 3, 4],
            sector_count: 5,
            prng: PrngMode::Hard,
            mapping: vec![
                SectorKeys {
                    key_a: Some([0; 6]),
                    key_b: Some([0x11; 6]),
                    origin_a: Some(KeyOrigin::Dictionary),
                    origin_b: Some(KeyOrigin::NestedDictionary)
                };
                5
            ],
            resolved_keys: 10,
            total_keys: 10,
            stopped: false,
        },
        memory,
        error: None,
        warnings: vec![],
        elapsed: Duration::from_millis(1250),
    }
}

#[test]
fn normal_summary_keeps_key_details_in_verbose_output() {
    let report = complete();
    assert_eq!(
        report.summary_text(false, false),
        "✓ Complete · 10/10 keys · 20/20 blocks · 1.25 s\n"
    );
    let verbose = report.summary_text(true, false);
    assert!(verbose.contains("Sector  Key A         Source A       Key B         Source B"));
    assert!(verbose.contains("00      000000000000  dictionary     111111111111  nested dict\n"));
    assert!(!verbose.contains('\x1b'));
    assert!(
        report
            .summary_text(false, true)
            .contains("\x1b[1;32m✓ Complete\x1b[0m")
    );
}

#[test]
fn incomplete_memory_is_partial_and_cannot_be_exported_as_a_complete_dump() {
    let mut report = complete();
    report.memory[0] = None;
    assert_eq!(report.status(), "partial");
    assert!(
        report
            .summary_text(false, false)
            .starts_with("! Partial · 10/10 keys · 19/20 blocks · 1.25 s\n")
    );
    assert!(report.export().is_err());
    report.error = Some("Recovery cancelled".into());
    report.summary.stopped = true;
    assert!(
        report.summary_text(false, false).starts_with(
            "! Interrupted · 10/10 keys · 19/20 blocks · 1.25 s\nRecovery cancelled\n"
        )
    );
}

#[test]
fn dump_masks_unobserved_keys_and_renders_missing_bytes_and_ascii() {
    let mut report = complete();
    report.summary.mapping[0] = SectorKeys::default();
    report.memory[0] = None;
    report.memory[3] = Some(vec![b'A'; 16]);
    let observed = report.observed();
    assert!(observed[3][..6].iter().all(Option::is_none));
    assert!(observed[3][10..].iter().all(Option::is_none));
    let display = report.dump_text(false);
    assert!(display.starts_with("\nSector 00\n"));
    assert!(display.contains(
        "  000  ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ?? ??  |????????????????|\n"
    ));
    assert!(display.contains(
        "  001  41 41 41 41  41 41 41 41  41 41 41 41  41 41 41 41  |AAAAAAAAAAAAAAAA|\n"
    ));
    assert!(display.contains("?? = unread or unavailable bytes"));
    let image =
        nfc_native::formats::flipper::parse_with_warnings(&complete().export().unwrap()).unwrap();
    assert!(image.1.is_empty());
    assert_eq!(&image.0.memory[3 * 16..3 * 16 + 6], &[0; 6]);
    assert!(!complete().dump_text(false).contains("??"));
}

#[test]
fn empty_recovery_shows_a_selected_tag_without_fabricating_memory() {
    let mut report = complete();
    report.summary.mapping.fill(SectorKeys::default());
    report.summary.resolved_keys = 0;
    report.memory.fill(None);
    assert_eq!(
        report.dump_text(false),
        "\nNo readable memory recovered. Try a known key or another dictionary.\n"
    );
}

#[test]
fn dump_keeps_ascii_aligned_with_colors_unknown_bytes_and_large_sectors() {
    let mut report = complete();
    report.card.sak = 0x18;
    report.memory = vec![Some(vec![b'|'; 16]); 256];
    report.memory[1] = None;
    report.summary.mapping.resize(40, SectorKeys::default());
    let plain = report.dump_text(false);
    let colored = report.dump_text(true);
    assert!(!plain.contains('\x1b'));
    assert_eq!(ui::without_colors(&colored), plain);
    assert!(plain.contains("\nSector 32\n  128  "));
    assert!(plain.contains("\nSector 39\n  240  "));
    let rows: Vec<_> = plain
        .lines()
        .filter(|line| line.starts_with("  "))
        .collect();
    assert_eq!(rows.len(), 256);
    for line in rows {
        assert_eq!(&line[57..60], "  |", "{line}");
        assert_eq!(line.len(), 77, "{line}");
    }
    assert!(colored.contains("\x1b[95m7C\x1b[0m"));
    assert!(colored.contains("\x1b[32m00\x1b[0m"));
    assert!(colored.contains("\x1b[33m7C\x1b[0m"));
    assert!(colored.contains("\x1b[94m11\x1b[0m"));
    assert!(colored.contains("\x1b[90m??\x1b[0m"));
}

#[test]
fn json_retains_partial_bytes_verified_keys_and_full_uid_after_cancellation() {
    let mut report = complete();
    report.card.uid = vec![4, 1, 2, 3, 4, 5, 6];
    report.memory[0] = None;
    report.summary.mapping[0] = SectorKeys::default();
    report.memory[3] = Some(vec![0; 16]);
    report.summary.stopped = true;
    let value: serde_json::Value = serde_json::from_str(&report.json().unwrap()).unwrap();
    let doc = &value["document"];
    assert_eq!(doc["card"]["uid"], serde_json::json!([4, 1, 2, 3, 4, 5, 6]));
    assert_eq!(doc["memory"][0], serde_json::json!(vec![None::<u8>; 16]));
    assert!(doc["memory"][3][0].is_null());
    assert_eq!(doc["memory"][3][6], 0);
    assert_eq!(
        doc["recoveredKeys"][1]["key_a"],
        serde_json::json!(vec![0; 6])
    );
    assert!(!doc["warnings"].as_array().unwrap().is_empty());
}
