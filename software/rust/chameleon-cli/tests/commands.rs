use std::{
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
};

fn run(arguments: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_chameleon"))
        .env("TERM", "xterm-256color")
        .env("NO_COLOR", "1")
        .args(arguments)
        .output()
        .unwrap()
}

#[test]
fn reader_commands_use_existing_families_without_technology_option() {
    for family in ["14b", "15", "st25tb"] {
        let output = run(&["hf", family, "--help"]);
        assert!(output.status.success());
        assert!(String::from_utf8_lossy(&output.stdout).contains("info"));
    }
    let output = run(&["hf", "search", "--help"]);
    assert!(output.status.success());
    assert!(!String::from_utf8_lossy(&output.stdout).contains("technology"));
    let output = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "15",
        "read",
        "--block",
        "256",
    ]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("invalid value"));
}

#[test]
fn tearoff_is_explicit_and_rejects_unbounded_or_invalid_inputs() {
    let output = run(&["hf", "st25tb", "tearoff", "--help"]);
    assert!(output.status.success());
    let help = String::from_utf8_lossy(&output.stdout);
    for option in [
        "--block",
        "--target",
        "--timing",
        "--safety-minutes",
        "--adjustment-us",
    ] {
        assert!(help.contains(option));
    }
    assert!(!help.contains("--duration-ms"));
    assert!(!help.contains("--profile"));
    assert!(!help.contains("--pulses"));
    for value in ["0", "121"] {
        let output = run(&[
            "hf",
            "st25tb",
            "tearoff",
            "--block",
            "6",
            "--target",
            "FFFFFFFE",
            "--safety-minutes",
            value,
        ]);
        assert!(!output.status.success());
        assert!(String::from_utf8_lossy(&output.stderr).contains("invalid value"));
    }
    for value in ["0", "101"] {
        let output = run(&[
            "hf",
            "st25tb",
            "tearoff",
            "--block",
            "6",
            "--target",
            "FFFFFFFE",
            "--adjustment-us",
            value,
        ]);
        assert!(!output.status.success());
        assert!(String::from_utf8_lossy(&output.stderr).contains("invalid value"));
    }
    for value in ["1", "100"] {
        let output = run(&[
            "--port",
            "/missing-chameleon",
            "hf",
            "st25tb",
            "tearoff",
            "--block",
            "6",
            "--target",
            "FFFFFFFE",
            "--adjustment-us",
            value,
        ]);
        assert!(!output.status.success());
        assert!(String::from_utf8_lossy(&output.stderr).contains("No such file"));
    }
    let output = run(&["hf", "15", "--help"]);
    assert!(!String::from_utf8_lossy(&output.stdout).contains("tearoff"));
    let output = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "st25tb",
        "tearoff",
        "--block",
        "5",
        "--target",
        "INVALID",
    ]);
    assert!(!output.status.success());
    assert!(!String::from_utf8_lossy(&output.stderr).contains("No such file"));
    let output = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "st25tb",
        "tearoff",
        "--block",
        "5",
        "--target",
        "FFFFFFFE",
    ]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("No such file"));
    let output = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "st25tb",
        "tearoff",
        "--block",
        "5",
        "--target",
        "FFFFFFFE",
        "--pulses",
        "0",
    ]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("unexpected argument"));
}

#[test]
fn help_and_script_errors_need_no_device_and_emit_plain_text() {
    let output = run(&["-c", "help hf mf autopwn"]);
    assert!(output.status.success());
    assert!(String::from_utf8_lossy(&output.stdout).contains("--dict"));
    for arguments in [
        vec![],
        vec!["-c", "'unfinished"],
        vec!["-c", "info", "hf", "search"],
    ] {
        let output = run(&arguments);
        assert!(!output.status.success());
        assert!(!output.stderr.contains(&0x1b));
        assert!(!String::from_utf8_lossy(&output.stderr).contains("panicked"));
    }
}

#[test]
fn invalid_autopwn_inputs_fail_before_connecting() {
    let output = run(&[
        "--port",
        "/chameleon-port-does-not-exist",
        "hf",
        "mf",
        "autopwn",
        "--budget",
        "3",
    ]);
    let text = String::from_utf8_lossy(&output.stderr);
    assert!(!output.status.success());
    assert!(text.contains("Budget must be even"));
    assert!(!text.contains("Recovery "));
    assert!(!text.contains("No such file"));
    assert!(!text.contains('\x1b'));
}

#[test]
fn an_existing_dump_is_preserved_before_any_device_work() {
    let path = std::env::temp_dir().join(format!(
        "chameleon-output-{}-{}.nfc",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::write(&path, "existing dump").unwrap();
    let output = run(&[
        "--port",
        "/chameleon-port-does-not-exist",
        "hf",
        "mf",
        "autopwn",
        "--output",
        path.to_str().unwrap(),
    ]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("Output already exists"));
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "existing dump");
    std::fs::remove_file(path).unwrap();
}

#[test]
fn slot_scripts_reject_invalid_or_missing_arguments_before_usb() {
    for arguments in [
        vec!["slot", "start", "0"],
        vec!["slot", "start", "9"],
        vec!["slot", "dump", "0", "--hf"],
        vec!["slot", "dump", "1", "--hf", "--lf"],
        vec!["slot", "dump", "--hf"],
        vec!["slot", "init", "1", "--type", "unsupported"],
        vec!["slot", "export", "1", "--hf", "--lf", "-o", "backup.json"],
        vec!["slot", "start"],
        vec!["slot", "init", "1"],
    ] {
        let mut args = vec!["--port", "/missing-chameleon"];
        args.extend(arguments);
        let output = run(&args);
        assert!(!output.status.success());
        let error = String::from_utf8_lossy(&output.stderr);
        assert!(!error.contains("No such file"), "{error}");
    }
    for command in ["slot", "status"] {
        let output = run(&[command, "--help"]);
        assert!(output.status.success());
    }
    let output = run(&["slot", "dump", "--help"]);
    assert!(output.status.success());
    let help = String::from_utf8_lossy(&output.stdout);
    assert!(help.contains("--hf"));
    assert!(help.contains("--lf"));
    assert!(help.contains("snapshot of the stored image"));
}

#[test]
fn slot_files_reject_unsupported_formats_before_usb() {
    for arguments in [
        vec!["slot", "load", "card.nfci", "2", "--yes"],
        vec!["slot", "export", "2", "--hf", "-o", "card.nfci"],
    ] {
        let mut args = vec!["--port", "/missing-chameleon"];
        args.extend(arguments);
        let output = run(&args);
        assert!(!output.status.success());
        let error = String::from_utf8_lossy(&output.stderr);
        assert!(error.contains("Choose a .json or .nfc"), "{error}");
        assert!(!error.contains("No such file"), "{error}");
    }
    let output = run(&["convert", "card.nfc", "card.nfci"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("unrecognized subcommand"));
}

#[test]
fn slot_type2_capture_defaults_are_explicit_and_validation_precedes_usb() {
    let path = std::env::temp_dir().join(format!(
        "chameleon-type2-import-{}-{}.json",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let mut capture = serde_json::json!({"version": 1, "document": {
        "kind": "type2", "type": 1100, "unitBytes": 4,
        "card": {"uid": [4, 1, 2, 3, 4, 5, 6], "atqa": [0, 0x44], "sak": 0, "ats": []},
        "memory": (0..45).map(|page| (page < 43).then_some([0u8; 4])).collect::<Vec<_>>(),
        "emulator": {"options": [0, 0, 0, 0, 45], "extra": vec![None::<[u8; 4]>; 11]}
    }});
    let text = capture.to_string();
    std::fs::write(&path, &text).unwrap();
    let base = [
        "--port",
        "/missing-chameleon",
        "slot",
        "load",
        path.to_str().unwrap(),
        "2",
        "--yes",
    ];
    let rejected = run(&base);
    let error = String::from_utf8_lossy(&rejected.stderr);
    assert!(!rejected.status.success());
    assert!(error.contains("--emulator-defaults"), "{error}");
    assert!(!error.contains("No such file"), "{error}");
    let mut args = base.to_vec();
    args.push("--emulator-defaults");
    let accepted = run(&args);
    let error = String::from_utf8_lossy(&accepted.stderr);
    assert!(error.contains("No such file"), "{error}"); // Reached discovery only after validation.
    assert_eq!(std::fs::read_to_string(&path).unwrap(), text);
    capture["document"]["memory"][4] = serde_json::Value::Null;
    std::fs::write(&path, capture.to_string()).unwrap();
    let partial = run(&args);
    let error = String::from_utf8_lossy(&partial.stderr);
    assert!(error.contains("page 4 contains unread bytes"), "{error}");
    assert!(!error.contains("No such file"), "{error}");
    std::fs::remove_file(path).unwrap();
}

#[test]
fn desfire_commands_validate_destinations_before_usb() {
    for args in [
        vec!["hf", "desfire", "dump", "--slot", "0"],
        vec!["hf", "desfire", "dump", "--slot", "9"],
        vec!["hf", "desfire", "dump", "--name", "Test"],
        vec!["hf", "desfire", "dump", "--yes"],
        vec!["hf", "desfire", "dump", "--output", "badge.bin"],
    ] {
        let mut command = vec!["--port", "/missing-desfire-device"];
        command.extend(args);
        let output = run(&command);
        assert!(!output.status.success());
        assert!(!String::from_utf8_lossy(&output.stderr).contains("No such file"));
    }
    let help = run(&["hf", "desfire", "dump", "--help"]);
    assert!(help.status.success());
    let text = String::from_utf8_lossy(&help.stdout);
    for option in ["--slot", "--name", "--yes", "--output"] {
        assert!(text.contains(option));
    }
}

#[test]
fn lf_commands_exclude_capture_and_reject_bad_inputs_before_usb() {
    let help = run(&["lf", "--help"]);
    assert!(help.status.success());
    let text = String::from_utf8_lossy(&help.stdout);
    for command in ["search", "read", "write"] {
        assert!(text.contains(command));
    }
    for command in ["capture", "inspect", "plot"] {
        assert!(!text.contains(command));
    }
    for args in [
        vec!["lf", "write", "em410x", "--id", "XYZ", "--yes"],
        vec![
            "lf",
            "write",
            "em410x",
            "--id",
            "0102030405",
            "--fc",
            "1",
            "--yes",
        ],
        vec![
            "lf", "write", "hid", "--format", "ACTPHID", "--fc", "1", "--cn", "65536", "--yes",
        ],
        vec![
            "lf",
            "write",
            "ioprox",
            "--raw8",
            "00000000000003FC",
            "--yes",
        ],
        vec!["lf", "read", "idteck"],
        vec!["lf", "read", "pac", "--format", "H10301"],
        vec!["lf", "write", "--last", "--yes"],
    ] {
        let mut arguments = vec!["--port", "/missing-lf-device", "-c"];
        let command = args.join(" ");
        arguments.push(&command);
        let output = run(&arguments);
        assert!(!output.status.success());
        let error = String::from_utf8_lossy(&output.stderr);
        assert!(!error.contains("No such file"), "{error}");
        assert!(!error.contains("/missing-lf-device"), "{error}");
    }
}

#[test]
fn nfca_sniff_exposes_live_capture_and_validates_inputs_before_usb() {
    let output = run(&["hf", "14a", "sniff", "--help"]);
    assert!(output.status.success());
    let help = String::from_utf8_lossy(&output.stdout);
    for option in ["--duration", "--out", "--verbose", "--frame", "--gain"] {
        assert!(help.contains(option));
    }
    for duration in ["0", "86401"] {
        let output = run(&[
            "--port",
            "/missing-chameleon",
            "hf",
            "14a",
            "sniff",
            "--duration",
            duration,
        ]);
        assert!(!output.status.success());
        assert!(String::from_utf8_lossy(&output.stderr).contains("invalid value"));
    }
    let output = run(&["hf", "14a", "sniff", "--gain", "8"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("invalid value"));
    let path = std::env::temp_dir().join(format!("chameleon-sniff-{}.jsonl", std::process::id()));
    std::fs::write(&path, "existing capture").unwrap();
    let output = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "14a",
        "sniff",
        "--out",
        path.to_str().unwrap(),
    ]);
    assert!(!output.status.success());
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "existing capture");
    assert!(String::from_utf8_lossy(&output.stderr).contains("exist"));
    std::fs::remove_file(path).unwrap();
}

#[test]
fn nfcv_sniff_exposes_the_shared_live_options() {
    let output = run(&["hf", "15", "sniff", "--help"]);
    assert!(output.status.success());
    let help = String::from_utf8_lossy(&output.stdout);
    for option in ["--duration", "--gain", "--out", "--verbose", "--frame"] {
        assert!(help.contains(option));
    }
    let invalid = run(&["hf", "15", "sniff", "--gain", "8"]);
    assert!(!invalid.status.success());
}

#[test]
fn ultralight_c_restore_validates_target_credentials_before_usb() {
    use nfc_native::formats::{
        flipper,
        image::{Image, Model},
    };
    let directory = std::env::temp_dir().join(format!("ulc-restore-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let image = Image {
        model: Model::UltralightC,
        uid: vec![4, 1, 2, 3, 4, 5, 6],
        memory: vec![0; 192],
        metadata: vec![],
        secrets: vec![],
    };
    let path = directory.join("source.nfc");
    std::fs::write(&path, flipper::export(&image).unwrap()).unwrap();
    let help = run(&["hf", "mfu", "restore", "--help"]);
    assert!(help.status.success());
    let text = String::from_utf8_lossy(&help.stdout);
    assert!(text.contains("ultralight-c") && text.contains("--key"));
    let mut args = [
        "--port",
        "/missing-chameleon",
        "hf",
        "mfu",
        "restore",
        path.to_str().unwrap(),
        "--model",
        "ultralight-c",
        "--key",
        "001122",
    ];
    let invalid = run(&args);
    assert!(!invalid.status.success());
    assert!(
        String::from_utf8_lossy(&invalid.stderr).contains("Expected 16 hexadecimal bytes"),
        "{}",
        String::from_utf8_lossy(&invalid.stderr)
    );
    let password = run(&[
        "--port",
        "/missing-chameleon",
        "hf",
        "mfu",
        "restore",
        path.to_str().unwrap(),
        "--password",
        "001122",
    ]);
    assert!(String::from_utf8_lossy(&password.stderr).contains("requires --key"));
    args[9] = "00112233445566778899AABBCCDDEEFF";
    let valid = run(&args);
    assert!(!valid.status.success());
    assert!(!String::from_utf8_lossy(&valid.stderr).contains("Expected 16 hexadecimal bytes"));
    assert!(!String::from_utf8_lossy(&valid.stderr).contains("invalid value"));
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn nfc_tools_expose_family_commands_and_reject_invalid_frames_before_usb() {
    for (family, commands) in [
        (
            "mfu",
            vec!["info", "read", "dump", "ndef", "write", "restore"],
        ),
        (
            "slix",
            vec!["info", "read", "dump", "write", "restore", "raw"],
        ),
        (
            "slix-l",
            vec!["info", "read", "dump", "write", "restore", "raw"],
        ),
        ("14a", vec!["raw", "sniff"]),
    ] {
        let help = run(&["hf", family, "--help"]);
        assert!(help.status.success());
        let text = String::from_utf8_lossy(&help.stdout);
        for command in commands {
            assert!(text.contains(command));
        }
    }
    for arguments in [
        vec!["hf", "14a", "raw", "26", "--bits", "9"],
        vec!["hf", "15", "raw", "26/7"],
        vec!["hf", "mfu", "write", "--page", "4", "invalid"],
        vec![
            "hf",
            "mfu",
            "dump",
            "--output",
            "/missing-parent/capture.bin",
        ],
    ] {
        let output = run(&[vec!["--port", "/missing-chameleon"], arguments].concat());
        assert!(!output.status.success());
        assert!(!String::from_utf8_lossy(&output.stderr).contains("No such file"));
    }
}

#[test]
fn raw_help_and_arguments_follow_the_selected_protocol() {
    let nfca = run(&["hf", "14a", "raw", "--help"]);
    assert!(nfca.status.success());
    let nfca = String::from_utf8_lossy(&nfca.stdout);
    for option in ["--bits", "--select", "--crc", "--timeout-ms", "--keep"] {
        assert!(nfca.contains(option));
    }

    for family in ["15", "slix", "slix-l"] {
        let output = run(&["hf", family, "raw", "--help"]);
        assert!(output.status.success());
        let help = String::from_utf8_lossy(&output.stdout);
        assert!(help.contains("--file"));
        assert!(help.contains("--keep"));
        assert!(help.contains("automatic CRC"));
        for option in [
            "--bits",
            "--select",
            "--crc",
            "--append-crc",
            "--check-crc",
            "--timeout-ms",
            "--password",
            "--password-file",
        ] {
            assert!(!help.contains(option), "{family}: {option}");
            let invalid = run(&[
                "--port",
                "/missing-chameleon",
                "hf",
                family,
                "raw",
                "260100",
                option,
            ]);
            assert!(!invalid.status.success());
            assert!(String::from_utf8_lossy(&invalid.stderr).contains("unexpected argument"));
        }
    }
}

#[test]
fn memory_help_and_credentials_match_the_operation() {
    for family in ["15", "slix", "slix-l"] {
        let output = run(&["hf", family, "read", "--help"]);
        assert!(output.status.success());
        let text = String::from_utf8_lossy(&output.stdout);
        assert!(!text.contains("ST25TB"));
        assert!(!text.contains("system block"));
        let output = run(&["hf", family, "sniff", "--help"]);
        assert!(output.status.success());
        assert!(!String::from_utf8_lossy(&output.stdout).contains("--password"));
    }
    let output = run(&["hf", "mfu", "read", "--help"]);
    let text = String::from_utf8_lossy(&output.stdout);
    assert!(text.contains("--password <HEX>") && text.contains("--key <HEX>"));
    assert!(!text.contains("--password-file"));
    let output = run(&["hf", "mfu", "info", "--help"]);
    assert!(!String::from_utf8_lossy(&output.stdout).contains("--password"));
    for arguments in [
        vec!["hf", "mfu", "read", "--page", "4", "--password", "001122"],
        vec!["hf", "slix-l", "info", "--password", "001122"],
    ] {
        let output = run(&[vec!["--port", "/missing-chameleon"], arguments].concat());
        assert!(!output.status.success());
        assert!(String::from_utf8_lossy(&output.stderr).contains("hexadecimal bytes"));
        assert!(!String::from_utf8_lossy(&output.stderr).contains("No such file"));
    }
    let output = run(&["hf", "field", "off", "--help"]);
    assert!(output.status.success());
}

#[test]
fn firmware_packages_are_validated_and_previewed_without_usb_or_external_tools() {
    use std::io::Write;
    use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

    let path = std::env::temp_dir().join(format!("chameleon firmware {}.zip", std::process::id()));
    let write_package = |kind: &str, init: &[u8]| {
        let file = std::fs::File::create(&path).unwrap();
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
        let manifest = serde_json::to_vec(&serde_json::json!({
            "manifest": {(kind): {"dat_file": "app.dat", "bin_file": "app.bin"}}
        }))
        .unwrap();
        for (name, data) in [
            ("manifest.json", manifest.as_slice()),
            ("app.dat", init),
            ("app.bin", b"firmware bytes".as_slice()),
        ] {
            writer.start_file(name, options).unwrap();
            writer.write_all(data).unwrap();
        }
        writer.finish().unwrap();
    };
    // Nordic signed command: APPLICATION, app_size=14, opaque 64-byte signature.
    let mut init = [
        &[
            0x12, 78, 0x0a, 8, 8, 1, 0x12, 4, 0x20, 0, 0x38, 14, 0x10, 0, 0x1a, 64,
        ][..],
        &[0; 64],
    ]
    .concat();
    write_package("application", &init);
    let bytes = std::fs::read(&path).unwrap();
    let preview = Command::new(env!("CARGO_BIN_EXE_chameleon"))
        .env("PATH", "")
        .env("NO_COLOR", "1")
        .args([
            "--port",
            "/missing-chameleon",
            "flash",
            path.to_str().unwrap(),
            "--dry-run",
        ])
        .output()
        .unwrap();
    assert!(
        preview.status.success(),
        "{}",
        String::from_utf8_lossy(&preview.stderr)
    );
    let output = String::from_utf8_lossy(&preview.stdout);
    assert!(output.contains("application") && output.contains("14 bytes"));
    assert!(output.contains("Preview") && output.contains("Nothing written"));
    assert!(!output.contains("Bootloader"));
    assert_eq!(std::fs::read(&path).unwrap(), bytes);
    for arguments in [vec!["dfu", "--help"], vec!["flash", "--help"]] {
        assert!(run(&arguments).status.success());
    }
    let conflict = run(&["flash", path.to_str().unwrap(), "--dry-run", "--yes"]);
    assert!(!conflict.status.success());

    init[9] = 2; // A BOOTLOADER init packet disguised as an application by the manifest.
    write_package("application", &init);
    let mismatched = run(&[
        "--port",
        "/missing-chameleon",
        "flash",
        path.to_str().unwrap(),
        "--yes",
    ]);
    assert!(!mismatched.status.success());
    assert!(
        String::from_utf8_lossy(&mismatched.stderr)
            .contains("disagrees with the signed init packet")
    );
    assert!(!String::from_utf8_lossy(&mismatched.stderr).contains("Connect an Ultra"));
    assert!(mismatched.stdout.is_empty());

    init[10] = 0x30; // BOOTLOADER size uses bl_size, rather than app_size.
    write_package("bootloader", &init);
    let system_preview = run(&[
        "--port",
        "/missing-chameleon",
        "flash",
        path.to_str().unwrap(),
        "--dry-run",
    ]);
    assert!(system_preview.status.success());
    assert!(String::from_utf8_lossy(&system_preview.stderr).contains("overwrite slot storage"));
    assert!(!String::from_utf8_lossy(&preview.stderr).contains("overwrite slot storage"));
    let full_preview = run(&[
        "--port",
        "/missing-chameleon",
        "flash",
        path.to_str().unwrap(),
        "--full",
        "--dry-run",
    ]);
    assert!(full_preview.status.success());
    let output = String::from_utf8_lossy(&full_preview.stdout);
    assert!(output.contains("--full") && output.contains("Nothing written"));

    std::fs::write(&path, b"not a ZIP file").unwrap();
    let invalid = run(&[
        "--port",
        "/missing-chameleon",
        "flash",
        path.to_str().unwrap(),
        "--yes",
    ]);
    assert!(!invalid.status.success());
    assert!(!String::from_utf8_lossy(&invalid.stderr).contains("Connect an Ultra"));
    assert!(invalid.stdout.is_empty());
    std::fs::remove_file(path).unwrap();
}
