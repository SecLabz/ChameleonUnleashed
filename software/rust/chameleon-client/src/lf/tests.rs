//! LF serial contracts, malformed responses and one-shot writes.
use super::*;
use crate::test_support::{device, device_disconnect, device_with_hook};
type Exchange = (u16, Vec<u8>, u16, Vec<u8>);
fn mock(exchanges: Vec<Exchange>) -> (Client, std::thread::JoinHandle<()>) {
    let (mut client, worker) = device(exchanges);
    client.capabilities = (0..=6000).collect();
    (client, worker)
}
fn em() -> LfTag {
    LfTag {
        kind: LfType::Em410x,
        data: vec![1, 2, 3, 4, 5],
        variant: None,
    }
}

#[test]
fn search_skips_unsupported_and_stops_at_first_match() {
    let (mut client, worker) = mock(vec![
        (3000, vec![], 0x41, vec![]),
        (3014, vec![], 0x40, b"12345678".to_vec()),
    ]);
    client.capabilities = [3000, 3014].into();
    let tag = client.lf_search().unwrap();
    assert_eq!(tag.kind, LfType::Pac);
    assert_eq!(tag.data, b"12345678");
    drop(client);
    worker.join().unwrap();
}
#[test]
fn search_propagates_errors_instead_of_trying_another_family() {
    let (mut client, worker) = mock(vec![(3000, vec![], 0x60, vec![])]);
    assert!(client.lf_search().unwrap_err().is::<DeviceError>());
    drop(client);
    worker.join().unwrap();
}

#[test]
fn lost_read_reply_identifies_command_and_unrestored_mode() {
    let (mut client, worker) = device_disconnect(
        vec![
            (1002, vec![], 0x68, vec![0]),
            (1001, vec![1], 0x68, vec![]),
            (1002, vec![], 0x68, vec![1]),
        ],
        3010,
        vec![],
    );
    let error = client
        .with_lf_reader(|client| client.lf_read(LfType::Ioprox, 0))
        .unwrap_err()
        .to_string();
    assert!(error.contains("LF command 3010 failed"));
    assert!(error.contains("previous device mode could not be restored"));
    assert!(!error.contains("mutation outcome"));
    assert!(!client.usable);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn missing_tag_restores_mode_and_keeps_connection_usable() {
    let (mut client, worker) = mock(vec![
        (1002, vec![], 0x68, vec![0]),
        (1001, vec![1], 0x68, vec![]),
        (1002, vec![], 0x68, vec![1]),
        (3004, vec![], 0x41, vec![]),
        (1001, vec![0], 0x68, vec![]),
        (1002, vec![], 0x68, vec![0]),
    ]);
    assert!(
        client
            .with_lf_reader(|client| client.lf_read(LfType::Viking, 0))
            .unwrap()
            .is_none()
    );
    assert!(client.usable);
    drop(client);
    worker.join().unwrap();
}
#[test]
fn reads_em_variants_hid_padding_and_actual_em4x05_reply() {
    let hid = crate::hid_payload("H10302", 0, 0x123456789, 0, 0).unwrap();
    let mut padded = hid.clone();
    padded.extend([0; 3]);
    let (mut client, worker) = mock(vec![
        (3000, vec![], 0x40, [vec![0, 104], vec![7; 13]].concat()),
        (3002, vec![26], 0x40, padded),
        (3030, vec![], 0x40, vec![0; 13]),
    ]);
    assert_eq!(
        client.lf_read(LfType::Electra, 0).unwrap().unwrap().data,
        vec![7; 13]
    );
    assert_eq!(client.lf_read(LfType::Hid, 26).unwrap().unwrap().data, hid);
    assert_eq!(
        client
            .lf_read(LfType::Em4x05, 0)
            .unwrap()
            .unwrap()
            .data
            .len(),
        13
    );
    drop(client);
    worker.join().unwrap();
}
#[test]
fn rejects_malformed_read_without_panicking() {
    for (id, kind, input, bytes) in [
        (3000, LfType::Em410x, vec![], vec![0]),
        (3000, LfType::Em410x, vec![], vec![0, 100, 1]),
        (3002, LfType::Hid, vec![0], vec![1; 13]),
        (3030, LfType::Em4x05, vec![], vec![0; 14]),
        (3014, LfType::Pac, vec![], vec![0xff; 8]),
    ] {
        let (mut client, worker) = mock(vec![(id, input, 0x40, bytes)]);
        assert!(client.lf_read(kind, 0).is_err());
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn cancellation_after_mode_change_still_restores_emulation() {
    let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let hook_flag = flag.clone();
    let mut changed = false;
    let (mut client, worker) = device_with_hook(
        vec![
            (1002, vec![], 0x68, vec![0]),
            (1001, vec![1], 0x68, vec![]),
            (1001, vec![0], 0x68, vec![]),
            (1002, vec![], 0x68, vec![0]),
        ],
        move |id| {
            if id == 1001 && !changed {
                changed = true;
                hook_flag.store(true, Ordering::Relaxed);
            }
        },
    );
    client.capabilities = (0..=6000).collect();
    client.cancelled = flag;
    let result: Result<()> =
        client.with_lf_reader(|_| panic!("Cancelled mode entry must not start LF"));
    assert!(result.unwrap_err().is::<crate::RequestCancelled>());
    assert!(client.cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}
#[test]
fn cancelled_nonmatching_read_reports_cancellation_and_restores_emulation() {
    let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let hook_flag = flag.clone();
    let (mut client, worker) = device_with_hook(
        vec![
            (1002, vec![], 0x68, vec![0]),
            (1001, vec![1], 0x68, vec![]),
            (1002, vec![], 0x68, vec![1]),
            (3000, vec![], 0x41, vec![]),
            (1001, vec![0], 0x68, vec![]),
            (1002, vec![], 0x68, vec![0]),
        ],
        move |id| {
            if id == 3000 {
                hook_flag.store(true, Ordering::Relaxed);
            }
        },
    );
    client.capabilities = (0..=6000).collect();
    client.cancelled = flag;
    let error = client
        .with_lf_reader(|client| client.lf_read(LfType::Em410x, 0))
        .unwrap_err();
    assert!(error.is::<crate::RequestCancelled>());
    assert!(client.cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}
#[test]
fn hid_limits_match_firmware_and_default_oem_is_canonical() {
    assert!(crate::hid_payload("ACTPHID", 1, 65536, 0, 0).is_err());
    assert!(crate::hid_payload("HCP32", 0, 0xffffff, 0, 0).is_ok());
    assert!(crate::hid_payload("OPTUS34", 0x7ff, 1, 0, 0).is_ok());
    assert!(crate::hid_payload("SMP34", 0x1fff, 1, 7, 0).is_ok());
    assert_eq!(
        &crate::hid_payload("C15001", 1, 2, 0, 0).unwrap()[11..],
        &900u16.to_be_bytes()
    );
}
#[test]
fn invalid_ioprox_framing_is_rejected_before_usb() {
    assert!(ioprox_fields(&[0, 0, 0, 0, 0, 0, 3, 0xfc]).is_err());
    assert!(ioprox_fields(&[0; 7]).is_err());
    assert!(ioprox_fields(&[0, 0, 0x40, 0x20, 0x10, 8, 7, 0xff]).is_err());
}

#[test]
fn ioprox_raw_fields_and_decoded_fields_must_agree() {
    let raw = [0x00, 0x78, 0x54, 0xe0, 0x3a, 0x5d, 0x65, 0xab];
    let fields = ioprox_fields(&raw).unwrap();
    assert_eq!(fields, [1, 83, 165, 172]);
    let mut tag = LfTag {
        kind: LfType::Ioprox,
        data: [fields.to_vec(), raw.to_vec(), vec![0; 4]].concat(),
        variant: None,
    };
    tag.validate().unwrap();
    tag.data[0] ^= 1;
    assert!(tag.validate().is_err());
    tag.data[0] ^= 1;
    tag.data[11] ^= 4;
    assert!(tag.validate().is_err());
}

#[test]
fn successful_and_failed_reads_restore_mode() {
    for status in [0x40, 0x60] {
        let (mut client, worker) = mock(vec![
            (1002, vec![], 0x68, vec![0]),
            (1001, vec![1], 0x68, vec![]),
            (1002, vec![], 0x68, vec![1]),
            (3000, vec![], status, vec![0, 103, 1, 2, 3, 4, 5]),
            (1001, vec![0], 0x68, vec![]),
            (1002, vec![], 0x68, vec![0]),
        ]);
        let result = client.with_lf_reader(|client| client.lf_read(LfType::Em410x, 0));
        assert_eq!(result.is_ok(), status == 0x40);
        drop(client);
        worker.join().unwrap();
    }
}

fn t55xx_request(operation: u8, tag: &LfTag) -> Vec<u8> {
    let mut input = vec![1, operation, 0, 0, 0, 0, 0, 0, 100, 0, tag.data.len() as u8];
    input.extend(&tag.data);
    if operation == 2 {
        input.extend([1, 2, 3, 4]);
    }
    input
}

#[test]
fn unsupported_configuration_does_not_report_a_missing_command() {
    assert!(DeviceError(0x69).to_string().contains("configuration"));
    assert!(DeviceError(0x67).to_string().contains("command"));
}

fn t55xx_plan(verified: bool) -> Vec<u8> {
    let desired = [0x00148041u32, 0x11223344, 0x55667788];
    let mut reply = vec![1, 1, 0x7f, if verified { 2 } else { 0 }];
    for (index, desired_word) in desired.iter().chain([0; 5].iter()).enumerate() {
        let value = if index == 0 || (verified && index < 3) {
            *desired_word
        } else {
            0
        };
        reply.extend(value.to_be_bytes());
    }
    reply.push(3);
    for index in 0..7 {
        reply.extend(desired.get(index).copied().unwrap_or(0).to_be_bytes());
    }
    reply.extend([1, 2, 3, 4]);
    reply
}

#[test]
fn guarded_write_prepares_once_and_checks_final_blocks() {
    let tag = em();
    let (mut client, worker) = mock(vec![
        (3033, t55xx_request(1, &tag), 0x40, t55xx_plan(false)),
        (3033, t55xx_request(2, &tag), 0x40, t55xx_plan(true)),
    ]);
    assert!(client.lf_write(&tag).unwrap());
    drop(client);
    worker.join().unwrap();
}

#[test]
fn guarded_write_rejects_malformed_preparation_before_programming() {
    for reply in [vec![], vec![1; 69], {
        let mut p = t55xx_plan(false);
        p[36] = 8;
        p
    }] {
        let (mut client, worker) = mock(vec![(3033, t55xx_request(1, &em()), 0x40, reply)]);
        assert!(client.lf_write(&em()).is_err());
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn changed_target_or_failed_verification_is_never_retried() {
    for (status, reply) in [
        (0x76, t55xx_plan(false)),
        (0x40, t55xx_plan(false)),
        (0x75, vec![]),
    ] {
        let (mut client, worker) = mock(vec![
            (3033, t55xx_request(1, &em()), 0x40, t55xx_plan(false)),
            (3033, t55xx_request(2, &em()), status, reply),
        ]);
        assert!(client.lf_write(&em()).is_err());
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn lost_guarded_write_reply_is_unknown_without_retry() {
    let (mut client, worker) = device_disconnect(
        vec![(3033, t55xx_request(1, &em()), 0x40, t55xx_plan(false))],
        3033,
        t55xx_request(2, &em()),
    );
    client.capabilities = [3033].into();
    let error = client.lf_write(&em()).unwrap_err().to_string();
    assert!(error.contains("uncertain"));
    assert!(!client.usable);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn invalid_input_and_older_firmware_never_fall_back_to_password_reset_writes() {
    let (mut client, worker) = mock(vec![]);
    let mut bad = em();
    bad.data.clear();
    assert!(client.lf_write(&bad).is_err());
    client.capabilities = [3000, 3001].into();
    assert!(
        client
            .lf_write(&em())
            .unwrap_err()
            .to_string()
            .contains("Update")
    );
    drop(client);
    worker.join().unwrap();
}

#[test]
fn confirmed_write_result_is_retained_when_stop_arrives_during_command() {
    let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let hook_flag = flag.clone();
    let mut commands = 0;
    let (mut client, worker) = device_with_hook(
        vec![
            (3033, t55xx_request(1, &em()), 0x40, t55xx_plan(false)),
            (3033, t55xx_request(2, &em()), 0x40, t55xx_plan(true)),
        ],
        move |_| {
            commands += 1;
            if commands == 2 {
                hook_flag.store(true, Ordering::Relaxed);
            }
        },
    );
    client.cancelled = flag;
    client.capabilities = [3033].into();
    assert!(client.lf_write(&em()).unwrap());
    assert!(client.cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}
