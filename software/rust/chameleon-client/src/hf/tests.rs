use super::*;
use crate::test_support::{device, device_disconnect, device_with_hook};
use std::sync::{Arc, atomic::AtomicBool};

fn tag() -> HfTag {
    HfTag::NfcV {
        uid: [0xe0, 4, 1, 0x10, 3, 4, 5, 6],
        dsfid: 0x12,
        afi: None,
        ic_ref: None,
        blocks: Some(28),
        block_size: Some(4),
    }
}
fn partial() -> Vec<u8> {
    let mut data = vec![0; 24 + 28 * 6];
    data[..4].copy_from_slice(&[1, 3, 1, 3]);
    data[4..12].copy_from_slice(tag().uid());
    data[13] = 28;
    data[14] = 4;
    data[24..30].copy_from_slice(&[1, 2, 0, 1, 2, 3]);
    data
}
#[test]
fn snapshot_retains_unknowns_and_rejects_false_completion_or_wrong_tag() {
    let data = partial();
    let snapshot = HfSnapshot::decode(&data, &tag()).unwrap();
    assert!(!snapshot.complete);
    assert_eq!(snapshot.dsfid, Some(0x12));
    assert_eq!(snapshot.memory[0], Some([0, 1, 2, 3]));
    assert_eq!(snapshot.memory[1], None);
    assert_eq!(snapshot.locks[0], Some(true));
    assert_eq!(snapshot.locks[1], None);
    let mut corrupt = data.clone();
    corrupt[2] = 0;
    corrupt[3] = 0;
    assert!(HfSnapshot::decode(&corrupt, &tag()).is_err());
    corrupt = data.clone();
    corrupt[4] ^= 1;
    assert!(HfSnapshot::decode(&corrupt, &tag()).is_err());
    corrupt = data.clone();
    corrupt[24] = 2;
    assert!(HfSnapshot::decode(&corrupt, &tag()).is_err());
    assert!(HfSnapshot::decode(&data[..100], &tag()).is_err());
}
#[test]
fn one_dump_retains_error_reply_after_cancel_and_restores_mode() {
    let cancelled = Arc::new(AtomicBool::new(false));
    let trigger = cancelled.clone();
    let mut request = tag().uid().to_vec();
    request.extend([0, 28, 4]);
    let (mut client, worker) = device_with_hook(
        vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (NFCV_DUMP, request, 2, partial()),
            (SET_MODE, vec![0], 0x68, vec![]),
        ],
        move |command| {
            if command == NFCV_DUMP {
                trigger.store(true, Ordering::Relaxed);
            }
        },
    );
    client.cancelled = cancelled;
    client.capabilities.insert(NFCV_DUMP);
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        let snapshot = reader.dump(&tag()).unwrap();
        assert!(reader.cancelled());
        assert_eq!(snapshot.memory[0], Some([0, 1, 2, 3]));
        assert_eq!(snapshot.status, 2);
        reader.finish().unwrap();
    }
    drop(client);
    worker.join().unwrap();
}
#[test]
fn search_stops_at_nfc_a_and_does_not_probe_other_families() {
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![1]),
        (SCAN, vec![], 0, vec![4, 1, 2, 3, 4, 4, 0, 8, 0]),
    ]);
    client
        .capabilities
        .extend([NFCB_INFO, ST25TB_INFO, NFCV_INVENTORY]);
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        assert!(matches!(reader.search().unwrap(), HfTag::NfcA(_)));
    }
    drop(client);
    worker.join().unwrap();
}
#[test]
fn search_does_not_treat_crc_errors_as_absence() {
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![1]),
        (SCAN, vec![], 3, vec![]),
    ]);
    client.capabilities.insert(NFCB_INFO);
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        let error = reader.search().unwrap_err();
        assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 3);
    }
    drop(client);
    worker.join().unwrap();
}

#[test]
fn sri512_identities_read_all_blocks_and_system_without_changing_the_protocol() {
    for product in 0x18..=0x1b {
        let uid = [0xd0, 2, product, 1, 2, 3, 4, 5];
        let mut identity = uid.to_vec();
        identity.extend([0x5a, 16, 4]);
        let mut data = vec![0; 120];
        data[..16].copy_from_slice(&[1, 2, 0, 0, 0xd0, 2, product, 1, 2, 3, 4, 5, 0, 16, 4, 8]);
        data[19] = 0x5a;
        data[20..24].fill(255);
        for (block, entry) in data[24..].chunks_exact_mut(6).enumerate() {
            entry[0] = 1;
            entry[2..].fill(block as u8);
        }
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![1]),
            (ST25TB_INFO, vec![], 0, identity),
            (ST25TB_DUMP, uid.to_vec(), 0, data),
        ]);
        client
            .capabilities
            .extend([ST25TB_INFO, ST25TB_DUMP, ST25TB_TEAROFF]);
        {
            let mut reader = HfReader::begin(&mut client).unwrap();
            let tag = reader.st25tb().unwrap();
            assert!(tag.name().starts_with("SRI512"));
            let snapshot = reader.dump(&tag).unwrap();
            assert!(snapshot.complete);
            assert_eq!(snapshot.memory.len(), 16);
            assert_eq!(snapshot.memory[15], Some([15; 4]));
            assert_eq!(snapshot.system, Some([255; 4]));
            let request = reader
                .tearoff_request(&tag, 6, 0xffff_fffe, false, 3000, 0, false)
                .unwrap();
            assert_eq!(&request[1..9], &uid);
            assert_eq!(request[14], 0);
            reader.finish().unwrap();
        }
        drop(client);
        worker.join().unwrap();
    }
}

fn tearoff_reply(outcome: u8) -> Vec<u8> {
    let mut data = vec![0; 64];
    data[0] = outcome;
    data[1] = 1;
    data[2] = 1;
    data[4..8].copy_from_slice(&0xffff_fffeu32.to_le_bytes());
    data
}

fn tearoff_wire_request(uid: [u8; 8]) -> Vec<u8> {
    let mut request = vec![0xa0];
    request.extend(uid);
    request.push(5);
    request.extend(0xffff_fffeu32.to_le_bytes());
    request.extend([0, 0]);
    request.extend(3000u32.to_le_bytes());
    request
}

#[test]
fn tearoff_reset_check_uses_the_fixed_read_only_request() {
    let uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];
    let mut request = tearoff_wire_request(uid);
    request[14] = 4;
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![1]),
        (ST25TB_TEAROFF, request, 0, tearoff_reply(0)),
    ]);
    client.capabilities.insert(ST25TB_TEAROFF);
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        let tag = HfTag::St25tb {
            uid,
            chip_id: 1,
            blocks: 16,
        };
        let result = reader.tearoff_reset_check(&tag, 5, 0xffff_fffe).unwrap();
        assert_eq!(result.result.outcome, TearoffOutcome::Success);
        reader.finish().unwrap();
    }
    drop(client);
    worker.join().unwrap();
}

#[test]
fn tearoff_result_rejects_missing_or_inconsistent_values() {
    let mut data = tearoff_reply(0);
    assert_eq!(
        TearoffResult::decode(&data).unwrap().last_value,
        Some(0xffff_fffe)
    );
    data[0] = 4;
    assert_eq!(
        TearoffResult::decode(&data).unwrap().outcome,
        TearoffOutcome::TagLost
    );
    data[0] = 6;
    assert!(TearoffResult::decode(&data).is_err());
    data[0] = 0;
    data[1] = 0;
    assert!(TearoffResult::decode(&data).is_err());
    data[0] = 4;
    data[4..8].fill(0);
    assert!(TearoffResult::decode(&data).is_ok());
    data[3] = 4;
    assert!(TearoffResult::decode(&data).is_err());
    assert!(TearoffResult::decode(&data[..63]).is_err());
}

#[test]
fn tearoff_diagnostics_decode_preserves_result_and_read_stage() {
    let mut data = tearoff_reply(0);
    data[16..20].copy_from_slice(&64_009u32.to_le_bytes());
    data[20] = 4;
    data[21] = 2;
    let report = TearoffDiagnostics::decode(&data).unwrap();
    assert_eq!(report.result.last_value, Some(0xffff_fffe));
    assert_eq!(report.off_to_on_cycles, 64_009);
    assert_eq!(report.read_stage, 4);
    assert_eq!(report.read_status, 0);
    assert_eq!(report.read_attempts, 2);
    data[20] = 5;
    assert!(TearoffDiagnostics::decode(&data).is_err());
}

#[test]
fn tearoff_report_decodes_fixed_result_and_live_progress() {
    let mut data = tearoff_reply(0);
    data[3] = 1;
    data[8..12].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
    let transcript = TearoffReport::decode(&data).unwrap();
    assert_eq!(transcript.initial_value, Some(0xffff_ffff));
    assert_eq!(transcript.timing, None);
    data[3] = 3;
    data[24..28].copy_from_slice(&640_000u32.to_le_bytes());
    data[32..36].copy_from_slice(&32_000u32.to_le_bytes());
    let timing = TearoffReport::decode(&data).unwrap().timing.unwrap();
    assert_eq!(timing.total_cycles, 640_000);
    assert_eq!(timing.grant_wait_cycles, 32_000);
    data[3] = 1;
    assert!(TearoffReport::decode(&data).is_err());
    data[3] = 0;
    data[24..].fill(0);
    assert!(TearoffReport::decode(&data).is_err());
    assert_eq!(
        TearoffProgress::decode(&[4, 0, 0, 1, 0, 0, 0]).unwrap(),
        TearoffProgress::Initial(1)
    );
    assert_eq!(
        TearoffProgress::decode(&[1, 0x84, 0x03, 7, 0, 0, 0]).unwrap(),
        TearoffProgress::Log(TearoffLog {
            kind: TearoffLogKind::Candidate,
            delay_us: 900,
            value: 7,
        })
    );
    assert!(TearoffProgress::decode(&[5, 0, 0, 1, 0, 0, 0]).is_err());
}

#[test]
fn tearoff_lost_reply_keeps_device_outcome_unknown() {
    let uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];
    let request = tearoff_wire_request(uid);
    let (mut client, worker) = device_disconnect(
        vec![(GET_MODE, vec![], 0x68, vec![1])],
        ST25TB_TEAROFF,
        request,
    );
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        let tag = HfTag::St25tb {
            uid,
            chip_id: 1,
            blocks: 16,
        };
        let error = reader.tearoff(&tag, 5, 0xffff_fffe, 3000, 0).unwrap_err();
        assert!(error.downcast_ref::<UnknownTearoffOutcome>().is_some());
        assert!(reader.finish().is_err());
    }
    drop(client);
    worker.join().unwrap();
}

#[test]
fn tearoff_request_has_one_format_and_bounded_duration() {
    let (mut client, worker) = device(vec![(GET_MODE, vec![], 0x68, vec![1])]);
    client.capabilities.insert(ST25TB_TEAROFF);
    let mut reader = HfReader::begin(&mut client).unwrap();
    let tag = HfTag::St25tb {
        uid: [0xd0, 2, 0x33, 1, 2, 3, 4, 5],
        chip_id: 1,
        blocks: 16,
    };
    let request = reader
        .tearoff_request(&tag, 6, 0xffff_fffe, false, 1_800_000, 25, false)
        .unwrap();
    assert_eq!(request.len(), 20);
    assert_eq!(request[0], 0xa0);
    assert_eq!(&request[15..20], &[25, 0x40, 0x77, 0x1b, 0]);
    assert!(
        reader
            .tearoff_request(&tag, 6, 0xffff_fffe, false, 7_200_001, 25, false)
            .is_err()
    );
    for product in [0x17, 0x1f, 0x30, 0x34] {
        let tag = HfTag::St25tb {
            uid: [0xd0, 2, product, 1, 2, 3, 4, 5],
            chip_id: 1,
            blocks: 16,
        };
        assert!(
            reader
                .tearoff_request(&tag, 5, 0xffff_fffe, false, 3000, 0, false)
                .is_err()
        );
    }
    reader.finish().unwrap();
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn tearoff_preflight_stop_does_not_claim_submission() {
    let uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];
    let (mut client, worker) = device(vec![(GET_MODE, vec![], 0x68, vec![1])]);
    client.capabilities.insert(ST25TB_TEAROFF);
    let cancelled = client.cancelled.clone();
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        let tag = HfTag::St25tb {
            uid,
            chip_id: 1,
            blocks: 16,
        };
        cancelled.store(true, Ordering::Relaxed);
        let error = reader.tearoff(&tag, 5, 0xffff_fffe, 3000, 0).unwrap_err();
        assert!(error.downcast_ref::<RequestCancelled>().is_some());
        // This request fails local validation before any radio command.
        let error = reader.tearoff(&tag, 4, 0xffff_fffe, 3000, 0).unwrap_err();
        assert!(error.downcast_ref::<UnknownTearoffOutcome>().is_none());
    }
    drop(client);
    worker.join().unwrap();
}

#[test]
fn raw_a_retains_failure_reply_and_closes_field_before_restoring_mode() {
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![0]),
        (SET_MODE, vec![1], 0x68, vec![]),
        (NFCA_RAW, vec![0xc8, 0, 100, 0, 7, 0x26], 3, vec![0xaa]),
        (2101, vec![], 0x68, vec![]),
        (SET_MODE, vec![0], 0x68, vec![]),
    ]);
    client.capabilities.insert(NFCA_RAW);
    {
        let mut reader = HfReader::begin(&mut client).unwrap();
        assert!(
            reader
                .raw_nfca(&[0x26], 7, true, false, false, 100)
                .is_err()
        );
        assert_eq!(
            reader
                .raw_nfca(&[0x26], 7, false, false, false, 100)
                .unwrap(),
            (3, vec![0xaa])
        );
        reader.finish().unwrap();
    }
    drop(client);
    worker.join().unwrap();
}

#[test]
fn raw_v_keeps_partial_failure_and_rejects_inconsistent_completion() {
    for response in [
        vec![1, 0, 3, 1, 0xaa],
        vec![1, 0, 0, 0],
        vec![2, 0, 3, 0, 0, 0, 0],
    ] {
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![1]),
            (
                NFCV_RAW,
                vec![2, 3, 0x26, 1, 0, 3, 0x26, 1, 0],
                0,
                response.clone(),
            ),
        ]);
        client.capabilities.insert(NFCV_RAW);
        {
            let mut reader = HfReader::begin(&mut client).unwrap();
            let result = reader.raw_nfcv(&[vec![0x26, 1, 0], vec![0x26, 1, 0]]);
            if response == [1, 0, 3, 1, 0xaa] {
                assert_eq!(result.unwrap(), vec![(3, vec![0xaa])]);
            } else {
                assert!(result.is_err());
            }
            reader.finish().unwrap();
        }
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn nfcv_write_binds_before_bytes_and_never_retries_an_uncertain_response() {
    let mut request = tag().uid().to_vec();
    request.extend([2, 1, 2, 3, 4, 5, 6, 7, 8]);
    for response in [vec![5, 6, 7, 8], vec![1, 2, 3, 4], vec![]] {
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![1]),
            (NFCV_WRITE, request.clone(), 0, response.clone()),
        ]);
        client.capabilities.insert(NFCV_WRITE);
        {
            let mut reader = HfReader::begin(&mut client).unwrap();
            assert_eq!(
                reader
                    .write_nfcv(&tag(), 2, [1, 2, 3, 4], [5, 6, 7, 8])
                    .is_ok(),
                response == [5, 6, 7, 8]
            );
            reader.finish().unwrap();
        }
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn successful_raw_keep_leaves_reader_mode_and_field_across_connections() {
    for vicinity in [false, true] {
        let (command, payload, reply) = if vicinity {
            // ISO15693 error reply with valid CRC, captured independently of the host parser.
            (
                NFCV_RAW,
                vec![0x81, 3, 0x26, 1, 0],
                vec![1, 0, 0, 4, 1, 1, 0x16, 0x07],
            )
        } else {
            (NFCA_RAW, vec![0xc8, 0, 100, 0, 7, 0x26], vec![0x44, 0])
        };
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (command, payload, 0, reply),
            (GET_MODE, vec![], 0x68, vec![1]),
            (HF_FIELD_OFF, vec![], 0x68, vec![]),
        ]);
        client.capabilities.extend([command, HF_FIELD_OFF]);
        {
            let mut reader = HfReader::begin(&mut client).unwrap();
            if vicinity {
                reader
                    .raw_nfcv_with_field(&[vec![0x26, 1, 0]], true)
                    .unwrap();
            } else {
                reader
                    .raw_nfca(&[0x26], 7, false, false, false, 100)
                    .unwrap();
            }
            reader.keep_field().unwrap();
        }
        client.field_off().unwrap();
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn failed_or_cancelled_keep_requests_release_rf_and_restore_mode() {
    for cancelled_after_reply in [false, true] {
        let cancelled = Arc::new(AtomicBool::new(false));
        let trigger = cancelled.clone();
        let (mut client, worker) = device_with_hook(
            vec![
                (GET_MODE, vec![], 0x68, vec![0]),
                (SET_MODE, vec![1], 0x68, vec![]),
                (NFCV_RAW, vec![0x81, 3, 0x26, 1, 0], 3, vec![]),
                (HF_FIELD_OFF, vec![], 0x68, vec![]),
                (SET_MODE, vec![0], 0x68, vec![]),
            ],
            move |command| {
                if command == NFCV_RAW && cancelled_after_reply {
                    trigger.store(true, Ordering::Relaxed);
                }
            },
        );
        client.cancelled = cancelled;
        client.capabilities.insert(NFCV_RAW);
        {
            let mut reader = HfReader::begin(&mut client).unwrap();
            assert!(
                reader
                    .raw_nfcv_with_field(&[vec![0x26, 1, 0]], true)
                    .is_err()
            );
            if cancelled_after_reply {
                assert!(reader.keep_field().is_err());
            }
            reader.finish().unwrap();
        }
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn field_off_checks_acknowledgement_and_is_harmless_in_emulation_mode() {
    for reply in [vec![], vec![1]] {
        let (mut client, worker) = device(vec![
            (GET_MODE, vec![], 0x68, vec![1]),
            (HF_FIELD_OFF, vec![], 0x68, reply.clone()),
        ]);
        client.capabilities.insert(HF_FIELD_OFF);
        assert_eq!(client.field_off().is_ok(), reply.is_empty());
        drop(client);
        worker.join().unwrap();
    }
    let (mut client, worker) = device(vec![(GET_MODE, vec![], 0x68, vec![0])]);
    client.field_off().unwrap();
    drop(client);
    worker.join().unwrap();
}
