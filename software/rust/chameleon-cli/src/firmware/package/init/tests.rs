use super::*;

fn encoded(mut value: u64) -> Vec<u8> {
    let mut bytes = Vec::new();
    while value >= 128 {
        bytes.push(value as u8 | 0x80);
        value >>= 7;
    }
    bytes.push(value as u8);
    bytes
}

fn bytes(number: u8, value: &[u8]) -> Vec<u8> {
    let mut result = vec![number << 3 | 2];
    result.extend(encoded(value.len() as u64));
    result.extend(value);
    result
}

fn scalar(number: u8, value: u64) -> Vec<u8> {
    let mut result = vec![number << 3];
    result.extend(encoded(value));
    result
}

fn signed(init: &[u8]) -> Vec<u8> {
    let mut command = scalar(1, 1);
    command.extend(bytes(2, init));
    let mut signed = bytes(1, &command);
    signed.extend(scalar(2, 0));
    // Opaque signature bytes: these tests validate metadata, not cryptography.
    signed.extend(bytes(3, &[0; 64]));
    bytes(2, &signed)
}

pub(crate) fn packet(kind: u8, size: usize) -> Vec<u8> {
    versioned_packet(kind, size, None)
}

pub(crate) fn versioned_packet(kind: u8, size: usize, version: Option<u64>) -> Vec<u8> {
    let mut init = scalar(4, u64::from(kind));
    if let Some(version) = version {
        init.extend(scalar(1, version));
    }
    match kind {
        0 => init.extend(scalar(7, size as u64)),
        1 => init.extend(scalar(5, size as u64)),
        2 => init.extend(scalar(6, size as u64)),
        3 => {
            init.extend(scalar(5, (size / 2) as u64));
            init.extend(scalar(6, (size - size / 2) as u64));
        }
        _ => unreachable!(),
    }
    signed(&init)
}

#[test]
fn firmware_version_is_read_from_the_signed_metadata_and_must_fit_32_bits() {
    assert_eq!(validate(&packet(2, 8), "bootloader", 8).unwrap(), None);
    for version in [5, u32::from_be_bytes(*b"CU\0\x01"), u32::MAX] {
        assert_eq!(
            validate(
                &versioned_packet(2, 8, Some(u64::from(version))),
                "bootloader",
                8
            )
            .unwrap(),
            Some(version)
        );
    }
    assert!(
        validate(
            &versioned_packet(2, 8, Some(u64::from(u32::MAX) + 1)),
            "bootloader",
            8
        )
        .is_err()
    );
}

#[test]
fn generated_nordic_packets_match_their_manifest_types_and_sizes() {
    // Golden vectors from nrfutil nrf5sdk-tools, with real ECDSA signatures.
    for (hex, name, size) in [
        (
            "128a010a4408011240080110001a02800220002800300038a8a120422408031220f51151c1fa04dacdab1c5ab4e252a2995ec3a62a0c20f7e53f933f403c470ea1480052040801120010001a40fad0ede47dd6ceae4f1c0f7c85402617b6342962f1be7a1cd7adbe3e3e5bcafe8148601a3da47c8cf033fe1ccbb008c7a19f0de76b05dd0be44d63024e7d7c8c",
            "application",
            528552,
        ),
        (
            "1292010a4c08011248080310001a028002200328b4ac0930b4d40238004224080312207fc9cce0c00d20ec53626921bfdf7fd6a8f7000fd595573859a2f0461192cabb480052040801120052040801120010001a40ec0a2e87caf29b604040c7bac1be4b3fb6ea8860d0a62db4cdbd36bc48abe9f112dd9751e0499ea28b44156e8d695b0051e429dd6af796e2b4a16b92da62a12f",
            "softdevice_bootloader",
            196712,
        ),
    ] {
        let packet = (0..hex.len())
            .step_by(2)
            .map(|offset| u8::from_str_radix(&hex[offset..offset + 2], 16).unwrap())
            .collect::<Vec<_>>();
        validate(&packet, name, size).unwrap();
        assert!(validate(&packet, name, size - 1).is_err());
        assert!(validate(&packet, "bootloader", size).is_err());
    }
}

#[test]
fn manifest_cannot_disguise_another_signed_image_type() {
    let names = [
        "application",
        "softdevice",
        "bootloader",
        "softdevice_bootloader",
    ];
    for (kind, expected) in names.iter().enumerate() {
        let packet = packet(kind as u8, 8);
        for name in names {
            assert_eq!(validate(&packet, name, 8).is_ok(), &name == expected);
        }
        assert!(validate(&packet, expected, 7).is_err());
    }
}

#[test]
fn malformed_truncated_ambiguous_and_overflowing_packets_are_rejected() {
    let valid = packet(0, 8);
    for end in 0..valid.len() {
        assert!(validate(&valid[..end], "application", 8).is_err());
    }
    for invalid in [
        vec![0],
        vec![
            0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2,
        ],
        vec![0x13],
        [bytes(1, &[]), valid.clone()].concat(),
        [valid.clone(), valid].concat(),
        signed(&[0x20, 0, 0x20, 2, 0x38, 8]),
        signed(&[0x25, 0, 0, 0, 0, 0x38, 8]),
        signed(&[0x20, 0, 0x28, 1, 0x38, 8]),
        signed(&[0x20, 3, 0x28, 8]),
    ] {
        assert!(validate(&invalid, "application", 8).is_err());
    }
}

#[test]
fn protobuf_unknown_fields_are_skipped_and_integer_boundaries_are_checked() {
    for value in [0, 127, 128, u32::MAX as u64, u64::MAX] {
        let bytes = encoded(value);
        let mut input = bytes.as_slice();
        assert_eq!(varint(&mut input).unwrap(), value);
        assert!(input.is_empty());
    }
    let mut packet = packet(0, 8);
    packet.extend([0x79, 0, 0, 0, 0, 0, 0, 0, 0]);
    packet.extend([0x7d, 0, 0, 0, 0]);
    validate(&packet, "application", 8).unwrap();
}

#[test]
fn omitted_optional_opcode_and_type_use_the_nordic_schema_defaults() {
    let command = bytes(2, &scalar(7, 8));
    let mut signed = bytes(1, &command);
    signed.extend(scalar(2, 0));
    signed.extend(bytes(3, &[0; 64]));
    validate(&bytes(2, &signed), "application", 8).unwrap();
}
