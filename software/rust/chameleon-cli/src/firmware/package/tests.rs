use super::*;
use serde_json::json;
use std::io::{Cursor, Write};
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

fn archive(manifest: Value, files: &[(&str, &[u8])], method: CompressionMethod) -> Vec<u8> {
    let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
    let options = SimpleFileOptions::default().compression_method(method);
    writer.start_file("manifest.json", options).unwrap();
    writer
        .write_all(&serde_json::to_vec(&manifest).unwrap())
        .unwrap();
    for (name, bytes) in files {
        writer.start_file(*name, options).unwrap();
        writer.write_all(bytes).unwrap();
    }
    writer.finish().unwrap().into_inner()
}

fn application() -> Value {
    json!({"manifest":{"application":{"dat_file":"application.dat","bin_file":"application.bin"}}})
}

#[test]
fn stored_and_deflated_packages_preserve_init_and_firmware_bytes() {
    let init = init::tests::packet(0, 4);
    for method in [CompressionMethod::Stored, CompressionMethod::Deflated] {
        let bytes = archive(
            application(),
            &[
                ("application.dat", &init),
                ("application.bin", &[0xde, 0xad, 0xbe, 0xef]),
                ("THIRD_PARTY.txt", b"Optional license information"),
            ],
            method,
        );
        let images = decode(Cursor::new(bytes)).unwrap();
        assert_eq!(images.len(), 1);
        assert_eq!(images[0].name, "application");
        assert_eq!(images[0].init, init);
        assert_eq!(images[0].firmware, [0xde, 0xad, 0xbe, 0xef]);
    }
}

#[test]
fn installation_order_is_independent_of_manifest_and_zip_entry_order() {
    for combined in [false, true] {
        let image = |name: &str| json!({"dat_file":format!("{name}.dat"), "bin_file":"image.bin"});
        let manifest = if combined {
            json!({"manifest":{"application":image("application"),"softdevice_bootloader":image("combined")}})
        } else {
            json!({"manifest":{"application":image("application"),"bootloader":image("bootloader"),"softdevice":image("softdevice")}})
        };
        let bytes = archive(
            manifest,
            &[
                ("image.bin", &[4, 5, 6]),
                ("application.dat", &init::tests::packet(0, 3)),
                ("bootloader.dat", &init::tests::packet(2, 3)),
                ("softdevice.dat", &init::tests::packet(1, 3)),
                ("combined.dat", &init::tests::packet(3, 3)),
            ],
            CompressionMethod::Stored,
        );
        let images = decode(Cursor::new(bytes)).unwrap();
        let names = images.iter().map(|image| image.name).collect::<Vec<_>>();
        assert_eq!(
            names,
            if combined {
                vec!["softdevice_bootloader", "application"]
            } else {
                vec!["softdevice", "bootloader", "application"]
            }
        );
    }
}

#[test]
fn invalid_manifests_missing_files_and_empty_images_are_rejected() {
    let image = json!({"dat_file":"image.dat", "bin_file":"image.bin"});
    for manifest in [
        json!({}),
        json!({"manifest":[]}),
        json!({"manifest":{}}),
        json!({"manifest":{"application":null}}),
        json!({"manifest":{"unknown":image}}),
        json!({"manifest":{"application":"invalid"}}),
        json!({"manifest":{"application":{"bin_file":"image.bin"}}}),
        json!({"manifest":{"application":{"dat_file":"image.dat","bin_file":"missing.bin"}}}),
        json!({"manifest":{"softdevice_bootloader":image,"bootloader":image}}),
        json!({"manifest":{"softdevice_bootloader":image,"softdevice":image}}),
    ] {
        let bytes = archive(
            manifest,
            &[("image.dat", &[1]), ("image.bin", &[2])],
            CompressionMethod::Stored,
        );
        assert!(decode(Cursor::new(bytes)).is_err());
    }
    for files in [
        vec![("application.dat", &[][..]), ("application.bin", &[2][..])],
        vec![("application.dat", &[1][..]), ("application.bin", &[][..])],
    ] {
        let bytes = archive(application(), &files, CompressionMethod::Stored);
        assert!(decode(Cursor::new(bytes)).is_err());
    }
    assert!(decode(Cursor::new(b"not a ZIP file")).is_err());
}

#[test]
fn corrupt_and_oversized_entries_fail_during_decoding() {
    let mut bytes = archive(
        application(),
        &[
            ("application.dat", &init::tests::packet(0, 4)),
            ("application.bin", &[0xde, 0xad, 0xbe, 0xef]),
        ],
        CompressionMethod::Stored,
    );
    let offset = bytes
        .windows(4)
        .position(|window| window == [0xde, 0xad, 0xbe, 0xef])
        .unwrap();
    bytes[offset] ^= 1;
    assert!(decode(Cursor::new(bytes)).is_err());

    let oversized_init = vec![1; MAX_INIT_BYTES + 1];
    let oversized_firmware = vec![2; MAX_FIRMWARE_BYTES + 1];
    for files in [
        vec![
            ("application.dat", oversized_init.as_slice()),
            ("application.bin", &[2][..]),
        ],
        vec![
            ("application.dat", &[1][..]),
            ("application.bin", oversized_firmware.as_slice()),
        ],
    ] {
        let bytes = archive(application(), &files, CompressionMethod::Deflated);
        assert!(decode(Cursor::new(bytes)).is_err());
    }
}

#[test]
fn signed_type_and_size_mismatches_fail_before_device_discovery() {
    for init in [init::tests::packet(2, 4), init::tests::packet(0, 5)] {
        let bytes = archive(
            application(),
            &[
                ("application.dat", &init),
                ("application.bin", &[1, 2, 3, 4]),
            ],
            CompressionMethod::Stored,
        );
        let error = decode(Cursor::new(bytes)).err().unwrap();
        assert!(
            error
                .to_string()
                .contains("disagrees with the signed init packet")
        );
    }
}
