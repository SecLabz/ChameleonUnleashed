use super::*;
use crate::test_support::device;

pub(super) fn reader(client: &mut Client) -> Reader<'_> {
    Reader {
        client,
        card: Card {
            uid: vec![1, 2, 3, 4],
            atqa: [0, 4],
            sak: 8,
        },
        previous_mode: 1,
        report_progress: None,
        capture_metrics: None,
        session: None,
        reading_memory: false,
    }
}

#[test]
fn legacy_key_check_and_read_work_without_new_uid_commands() {
    let mut first_batch = vec![4, 0x61, 42];
    first_batch.extend([0xff; 42 * 6]);
    let mut second_batch = vec![4, 0x61, 1];
    second_batch.extend([0; 6]);
    let mut read = vec![0x60, 0];
    read.extend([0xff; 6]);
    let (mut client, worker) = device(vec![
        (2015, first_batch, 6, vec![0; 7]),
        (2015, second_batch, 0, vec![1, 0, 0, 0, 0, 0, 0]),
        (2008, read, 0, vec![0x5a; 16]),
    ]);
    client.capabilities.remove(&COLLECT_OBSERVATIONS);
    let mut reader = reader(&mut client);
    let mut candidates = vec![[0xff; 6]; 42];
    candidates.push([0; 6]);
    let mut progress = vec![];
    assert_eq!(
        reader
            .verify_progress(1, true, &candidates, |done, total| progress
                .push((done, total)))
            .unwrap(),
        Some([0; 6])
    );
    assert_eq!(progress, [(0, 43), (42, 43), (43, 43)]);
    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    assert_eq!(reader.block(&source, 0).unwrap(), [0x5a; 16]);
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn collection_sends_twelve_bytes_and_checks_full_parity() {
    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: true,
    };
    let mut payload = vec![0x60, 0];
    payload.extend([0xff; 6]);
    payload.extend([0x61, 4, 1, 1]);
    let response = vec![1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 0, 1];
    let mut bad_parity = response.clone();
    bad_parity[11] = 2;
    let (mut client, worker) = device(vec![
        (2023, payload.clone(), 0, response),
        (2023, payload.clone(), 0, bad_parity),
        (2023, payload, 0, vec![0; 11]),
    ]);
    let mut reader = reader(&mut client);
    reader.require_recovery_firmware().unwrap();
    assert!(reader.collect_static(&source, &target, true, 0).is_err());
    assert!(reader.collect_static(&source, &target, true, 17).is_err());
    let records = reader.collect_static(&source, &target, true, 1).unwrap();
    assert_eq!(records[0].nt1, 0x01020304);
    assert_eq!(records[0].ciphertext, 0x05060708);
    assert_eq!(records[0].parity, [0, 1, 0, 1]);
    assert_eq!(
        reader
            .collect_static(&source, &target, true, 1)
            .unwrap_err()
            .to_string(),
        "Invalid capture parity"
    );
    assert_eq!(
        reader
            .collect_static(&source, &target, true, 1)
            .unwrap_err()
            .to_string(),
        "Invalid normalized capture length"
    );
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn verification_rejects_a_key_outside_the_submitted_batch() {
    let mut payload = vec![4, 0x60, 1];
    payload.extend([0xff; 6]);
    let (mut client, worker) = device(vec![(2015, payload, 0, vec![1, 0, 0, 0, 0, 0, 0])]);
    assert!(reader(&mut client).verify(1, false, &[[0xff; 6]]).is_err());
    drop(client);
    worker.join().unwrap();
}

#[test]
fn profiled_batches_preserve_records_and_count_duplicates_and_failures() {
    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: true,
    };
    let mut payload = vec![0x60, 0];
    payload.extend([0xff; 6]);
    payload.extend([0x61, 4, 0, 1, 1]);
    let mut response: Vec<_> = [1u32, 28000, 25000, 1000, 300, 2, 25000, 1, 2, 1]
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect();
    response.extend([1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 0, 1]);
    let (mut client, worker) = device(vec![
        (2023, payload.clone(), 0, response.clone()),
        (2023, payload.clone(), 0, response),
        (
            2023,
            payload,
            6,
            [1u32, 28000, 50000, 1000, 300, 2, 50000, 2, 3, 0]
                .into_iter()
                .flat_map(u32::to_be_bytes)
                .collect(),
        ),
    ]);
    let mut reader = reader(&mut client);
    reader.enable_capture_metrics();
    for _ in 0..2 {
        assert_eq!(reader.collect(&source, &target, false, 1).unwrap().len(), 1);
    }
    assert!(reader.collect(&source, &target, false, 1).is_err());
    let metrics = reader.capture_metrics().unwrap();
    assert_eq!(metrics.observations, 2);
    assert_eq!(metrics.unique_observations, 1);
    assert_eq!(metrics.records.len(), 2);
    assert_eq!(metrics.completed_observations, 2);
    assert_eq!(metrics.batches[2].firmware.as_ref().unwrap().timeouts, 2);
    assert_eq!(metrics.batches.len(), 3);
    let first = metrics.batches[0].firmware.as_ref().unwrap();
    assert_eq!(
        (first.select_us, first.timeouts, first.wakeups),
        (25000, 1, 2)
    );
    assert_eq!(metrics.batches[2].observations, 0);
    assert!(metrics.batches[2].error.is_some());
    assert!(metrics.observations_per_second > 0.);
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn profile_version_and_record_count_are_validated_before_using_metrics() {
    for words in [
        [2u32, 0, 0, 0, 0, 0, 0, 0, 0, 1],
        [1, 0, 0, 0, 0, 0, 0, 0, 0, 2],
    ] {
        let mut response: Vec<_> = words.into_iter().flat_map(u32::to_be_bytes).collect();
        response.extend([0; 12]);
        assert!(crate::capture::decode(&response, 1, true).is_err());
    }
}

#[test]
fn hardnested_session_reuses_batches_and_closes_before_verification() {
    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: true,
    };
    let payload = vec![
        0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 4, 0, 1, 4, 1, 2, 3, 4,
    ];
    let mut verify = vec![4, 0x61, 1];
    verify.extend([0xff; 6]);
    let (mut client, worker) = device(vec![
        (2026, vec![1], 0x68, vec![]),
        (2023, payload.clone(), 0, vec![0; 12]),
        (2023, payload.clone(), 0, vec![0; 12]),
        (2026, vec![0], 0x68, vec![]),
        (2026, vec![1], 0x68, vec![]),
        (2023, payload, 0, vec![0; 12]),
        (2026, vec![0], 0x68, vec![]),
        (2015, verify, 6, vec![]),
    ]);
    client.capabilities.insert(NFCA_SESSION);
    let mut reader = reader(&mut client);
    assert!(reader.collect(&source, &target, false, 0).is_err());
    reader.collect(&source, &target, false, 1).unwrap();
    reader.collect(&source, &target, false, 1).unwrap();
    reader.session = Some(Instant::now() - Duration::from_secs(3));
    reader.collect(&source, &target, false, 1).unwrap();
    assert_eq!(reader.verify(1, true, &[[0xff; 6]]).unwrap(), None);
    reader.end().unwrap();
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn capture_failure_and_cancelled_drop_close_the_session() {
    for fail in [false, true] {
        let source = RecoverySource {
            sector: 0,
            is_key_b: false,
            key: [0xff; 6],
        };
        let target = RecoveryTarget {
            sector: 1,
            is_key_b: true,
        };
        let payload = vec![
            0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 4, 0, 1, 4, 1, 2, 3, 4,
        ];
        let (mut client, worker) = device(vec![
            (2026, vec![1], 0x68, vec![]),
            (
                2023,
                payload,
                if fail { 6 } else { 0 },
                if fail { vec![] } else { vec![0; 12] },
            ),
            (2026, vec![0], 0x68, vec![]),
        ]);
        client.capabilities.insert(NFCA_SESSION);
        let mut reader = reader(&mut client);
        let result = reader.collect(&source, &target, false, 1);
        assert_eq!(result.is_err(), fail);
        if fail {
            assert_eq!(
                result.unwrap_err().downcast_ref::<DeviceError>().unwrap().0,
                6
            );
        }
        reader.client.cancelled.store(true, Ordering::Relaxed);
        drop(reader);
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn scheduled_nested_does_not_open_a_session() {
    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: true,
    };
    let payload = vec![0x60, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x61, 4, 0, 1];
    let (mut client, worker) = device(vec![(2023, payload, 0, vec![0; 12])]);
    client.capabilities.insert(NFCA_SESSION);
    reader(&mut client)
        .collect(&source, &target, true, 1)
        .unwrap();
    drop(client);
    worker.join().unwrap();
}

#[test]
fn bounded_darkside_continues_and_preserves_full_uid_request() {
    let mut capture = vec![0, 1, 2, 3, 4, 0x7c, 0x39, 0xd2, 0xe6];
    capture.extend([0xa0; 8]);
    capture.extend([5; 8]);
    capture.extend([0; 8]);
    let (mut client, worker) = device(vec![
        (DARKSIDE_STEP, vec![0x60, 4, 0, 4, 1, 2, 3, 4], 0, vec![5]),
        (
            DARKSIDE_STEP,
            vec![0x60, 4, 1, 4, 1, 2, 3, 4],
            0,
            capture.clone(),
        ),
        (DARKSIDE_STEP, vec![0x60, 4, 2, 4, 1, 2, 3, 4], 0, capture),
    ]);
    client.capabilities.insert(DARKSIDE_STEP);
    let mut reader = reader(&mut client);
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: false,
    };
    let observation = reader.darkside(&target, 0).unwrap().unwrap();
    assert_eq!(observation.uid, 0x01020304);
    assert_eq!(observation.nt, 0x7c39d2e6);
    assert_eq!(reader.darkside(&target, 1).unwrap(), Some(observation));
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn legacy_darkside_remains_available_without_step_capability() {
    let (mut client, worker) = device(vec![(DARKSIDE, vec![0x61, 0, 1, 1], 0, vec![3])]);
    let mut reader = reader(&mut client);
    assert!(
        reader
            .darkside(
                &RecoveryTarget {
                    sector: 0,
                    is_key_b: true
                },
                0
            )
            .unwrap()
            .is_none()
    );
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn darkside_rejects_an_observation_for_another_card() {
    let mut capture = vec![0; 33];
    capture[4] = 9;
    let (mut client, worker) = device(vec![(
        DARKSIDE_STEP,
        vec![0x60, 0, 0, 4, 1, 2, 3, 4],
        0,
        capture,
    )]);
    client.capabilities.insert(DARKSIDE_STEP);
    let mut reader = reader(&mut client);
    assert!(
        reader
            .darkside(
                &RecoveryTarget {
                    sector: 0,
                    is_key_b: false
                },
                0
            )
            .unwrap_err()
            .to_string()
            .contains("UID mismatch")
    );
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn darkside_rejects_malformed_bounded_status() {
    for response in [vec![], vec![7], vec![5, 0], vec![3, 0]] {
        let (mut client, worker) = device(vec![(
            DARKSIDE_STEP,
            vec![0x60, 0, 0, 4, 1, 2, 3, 4],
            0,
            response,
        )]);
        client.capabilities.insert(DARKSIDE_STEP);
        let error = reader(&mut client)
            .darkside(
                &RecoveryTarget {
                    sector: 0,
                    is_key_b: false,
                },
                0,
            )
            .unwrap_err();
        assert!(
            error
                .to_string()
                .contains("Invalid Darkside capture status")
        );
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn bounded_darkside_cleanup_runs_after_error_and_cancellation() {
    let (mut client, worker) = device(vec![
        (DARKSIDE_STEP, vec![0x60, 0, 0, 4, 1, 2, 3, 4], 0, vec![7]),
        (NFCA_SESSION, vec![0], 0, vec![]),
    ]);
    client.capabilities.extend([DARKSIDE_STEP, NFCA_SESSION]);
    let mut reader = reader(&mut client);
    assert!(
        reader
            .darkside(
                &RecoveryTarget {
                    sector: 0,
                    is_key_b: false
                },
                0
            )
            .is_err()
    );
    reader.client.cancelled.store(true, Ordering::Relaxed);
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn magic_validates_identity_credentials_and_closes_recovery_before_probing() {
    let plain = vec![4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 4, 8, 0];
    let mut keyed = plain.clone();
    keyed[14] = 2;
    keyed.extend([0xff; 6]);
    let (mut client, worker) = device(vec![
        (NFCA_SESSION, vec![0], 0x68, vec![]),
        (MF1_MAGIC_PROBE, plain.clone(), 0, vec![0, 1, 0]),
        (MF1_MAGIC_PROBE, keyed, 0, vec![0, 4, 3]),
        (MF1_MAGIC_PROBE, plain.clone(), 0, vec![1]),
        (MF1_MAGIC_PROBE, plain, 0, vec![0, 4, 1]),
    ]);
    let mut reader = reader(&mut client);
    reader.session = Some(Instant::now());
    assert!(reader.magic(None).unwrap().gen1a());
    assert_eq!(
        reader.magic(Some(([0xff; 6], true))).unwrap().gen2,
        crate::Gen2Evidence::WriteAcknowledged
    );
    assert!(reader.magic(None).is_err());
    assert!(reader.magic(None).is_err());
    drop(reader);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn special_uid_preflight_and_write_keep_wire_contract_and_card_on_failure() {
    for response in [vec![4, 5, 6, 7, 8], vec![4, 9, 9, 9, 9], vec![]] {
        let mut request = vec![0; 21];
        request[0] = 3;
        request[1] = 4;
        request[2..6].copy_from_slice(&[1, 2, 3, 4]);
        request[9..13].copy_from_slice(&[5, 6, 7, 8]);
        let mut check = request.clone();
        check[20] = 1;
        let (mut client, worker) = device(vec![
            (MF1_UID_SPECIAL, check, 0, vec![4, 1, 2, 3, 4]),
            (MF1_UID_SPECIAL, request, 0, response.clone()),
        ]);
        client.capabilities.insert(MF1_UID_SPECIAL);
        {
            let mut reader = reader(&mut client);
            reader
                .special_uid(crate::MagicUidMethod::Gen3, &[5, 6, 7, 8], [0; 4], true)
                .unwrap();
            assert_eq!(reader.card.uid, [1, 2, 3, 4]);
            let result =
                reader.special_uid(crate::MagicUidMethod::Gen3, &[5, 6, 7, 8], [0; 4], false);
            let expected = if response == [4, 5, 6, 7, 8] {
                assert!(result.is_ok());
                [5, 6, 7, 8]
            } else {
                assert!(result.is_err());
                [1, 2, 3, 4]
            };
            assert_eq!(reader.card.uid, expected);
        }
        drop(client);
        worker.join().unwrap();
    }
}

#[test]
fn cancelled_reader_entry_restores_emulation_before_returning() {
    use crate::test_support::device_with_hook;
    let cancelled = Arc::new(AtomicBool::new(false));
    let trigger = cancelled.clone();
    let mut switched = false;
    let (mut client, worker) = device_with_hook(
        vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (SET_MODE, vec![0], 0x68, vec![]),
            (GET_MODE, vec![], 0x68, vec![0]),
        ],
        move |command| {
            if command == SET_MODE && !switched {
                switched = true;
                trigger.store(true, Ordering::Relaxed);
            }
        },
    );
    client.cancelled = cancelled.clone();
    assert!(
        Reader::begin(&mut client)
            .err()
            .unwrap()
            .is::<crate::RequestCancelled>()
    );
    assert!(cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}

#[test]
fn reader_operation_reports_cleanup_failures_and_preserves_primary_errors() {
    for failed in [false, true] {
        for status in [0x68, 0x76] {
            let mut requests = vec![
                (GET_MODE, vec![], 0x68, vec![0]),
                (SET_MODE, vec![1], 0x68, vec![]),
                (SCAN, vec![], 0, vec![4, 1, 2, 3, 4, 0, 4, 8, 0]),
                (SET_MODE, vec![0], status, vec![]),
            ];
            if status == 0x68 {
                requests.push((GET_MODE, vec![], 0x68, vec![0]));
            }
            let (mut client, worker) = device(requests);
            let result = Reader::begin(&mut client).unwrap().run(|reader| {
                reader.client.cancelled.store(true, Ordering::Relaxed);
                if failed {
                    return Err("Primary read failed".into());
                }
                Ok(7)
            });
            if failed || status != 0x68 {
                let message = result.unwrap_err().to_string();
                assert_eq!(message.contains("Primary read failed"), failed);
                assert_eq!(message.contains("cleanup failed"), status != 0x68);
            } else {
                assert_eq!(result.unwrap(), 7);
            }
            assert!(client.cancelled.load(Ordering::Relaxed));
            drop(client);
            worker.join().unwrap();
        }
    }
}

#[test]
fn reader_finish_reports_lost_restore_response_without_retrying() {
    let (mut client, worker) = crate::test_support::device_disconnect(
        vec![
            (GET_MODE, vec![], 0x68, vec![0]),
            (SET_MODE, vec![1], 0x68, vec![]),
            (SCAN, vec![], 0, vec![4, 1, 2, 3, 4, 0, 4, 8, 0]),
        ],
        SET_MODE,
        vec![0],
    );
    let error = Reader::begin(&mut client)
        .unwrap()
        .run(|_| Ok(()))
        .unwrap_err();
    assert!(error.to_string().contains("cleanup failed"));
    assert!(!client.usable);
    drop(client);
    worker.join().unwrap();
}

#[test]
fn recovery_end_keeps_reader_mode_until_the_operation_finishes() {
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![0]),
        (SET_MODE, vec![1], 0x68, vec![]),
        (SCAN, vec![], 0, vec![4, 1, 2, 3, 4, 0, 4, 8, 0]),
        (GET_MODE, vec![], 0x68, vec![1]),
        (SET_MODE, vec![0], 0x68, vec![]),
        (GET_MODE, vec![], 0x68, vec![0]),
    ]);
    Reader::begin(&mut client)
        .unwrap()
        .run(|reader| {
            reader.end()?;
            assert!(reader.client.reader_mode()?);
            Ok(())
        })
        .unwrap();
    drop(client);
    worker.join().unwrap();
}

#[test]
fn capture_and_session_close_failures_are_reported_before_restoring_emulation() {
    let mut request = vec![0x60, 0];
    request.extend([0xff; 6]);
    request.extend([0x60, 4, 0, 1, 4, 1, 2, 3, 4]);
    let (mut client, worker) = device(vec![
        (GET_MODE, vec![], 0x68, vec![0]),
        (SET_MODE, vec![1], 0x68, vec![]),
        (SCAN, vec![], 0, vec![4, 1, 2, 3, 4, 0, 4, 8, 0]),
        (NFCA_SESSION, vec![1], 0x68, vec![]),
        (COLLECT_OBSERVATIONS, request, 6, vec![]),
        (NFCA_SESSION, vec![0], 0x76, vec![]),
        (SET_MODE, vec![0], 0x68, vec![]),
        (GET_MODE, vec![], 0x68, vec![0]),
    ]);
    client.capabilities.insert(NFCA_SESSION);
    let error = Reader::begin(&mut client)
        .unwrap()
        .run(|reader| {
            reader.collect(
                &RecoverySource {
                    sector: 0,
                    is_key_b: false,
                    key: [0xff; 6],
                },
                &RecoveryTarget {
                    sector: 1,
                    is_key_b: false,
                },
                false,
                1,
            )
        })
        .unwrap_err()
        .to_string();
    assert!(error.contains("Authentication rejected"));
    assert!(error.contains("cleanup failed"));
    assert!(error.contains("Device busy or target changed"));
    drop(client);
    worker.join().unwrap();
}

#[test]
fn bounded_capture_reuses_the_rf_session_and_drains_cancellation_before_other_commands() {
    use crate::{frame::encode, st25tb_stream::CANCEL_SEQUENCE};
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;

    let source = RecoverySource {
        sector: 0,
        is_key_b: false,
        key: [0xff; 6],
    };
    let target = RecoveryTarget {
        sector: 1,
        is_key_b: true,
    };
    let (host, mut dev) = UnixStream::pair().unwrap();
    host.set_read_timeout(Some(Duration::from_millis(50)))
        .unwrap();
    dev.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let cancelled = Arc::new(AtomicBool::new(false));
    let device_cancelled = cancelled.clone();
    let worker = std::thread::spawn(move || {
        let expect = |dev: &mut UnixStream, command, payload: &[u8]| {
            let expected = encode(command, 0, payload).unwrap();
            let mut request = vec![0; expected.len()];
            dev.read_exact(&mut request).unwrap();
            assert_eq!(request, expected);
        };
        let reply = |dev: &mut UnixStream, command, status, payload: &[u8]| {
            dev.write_all(&encode(command, status, payload).unwrap())
                .unwrap();
        };
        expect(&mut dev, NFCA_SESSION, &[1]);
        reply(&mut dev, NFCA_SESSION, 0x68, &[]);

        let mut request = vec![0x60, 0];
        request.extend([0xff; 6]);
        request.extend([0x61, 4, 0, 0, 4, 1, 2, 3, 4]);
        for count in [16, 1] {
            request[11] = count;
            expect(&mut dev, COLLECT_CANCELLABLE, &request);
            let mut records = vec![0; usize::from(count) * 12];
            records[0] = count;
            reply(&mut dev, COLLECT_CANCELLABLE, 0, &records);
        }

        expect(&mut dev, COLLECT_CANCELLABLE, &request);
        device_cancelled.store(true, Ordering::Relaxed);
        let mut stop = [0; CANCEL_SEQUENCE.len()];
        dev.read_exact(&mut stop).unwrap();
        assert_eq!(&stop, CANCEL_SEQUENCE);
        reply(&mut dev, COLLECT_CANCELLABLE, 0x75, &[]);

        expect(&mut dev, NFCA_SESSION, &[0]);
        reply(&mut dev, NFCA_SESSION, 0x68, &[]);
        expect(&mut dev, PROBE_PRNG, &[]);
        reply(&mut dev, PROBE_PRNG, 0, &[2]);
        assert_eq!(dev.read(&mut [0]).unwrap(), 0);
    });

    let mut client = Client {
        port: Box::new(crate::test_support::Port(host)),
        usable: true,
        cancelled: cancelled.clone(),
        capabilities: [
            COLLECT_OBSERVATIONS,
            COLLECT_CANCELLABLE,
            CHECK_KEYS,
            READ_BLOCK,
            NFCA_SESSION,
        ]
        .into(),
    };
    let mut reader = reader(&mut client);
    for count in [16, 1] {
        let records = reader.collect(&source, &target, false, count).unwrap();
        assert_eq!(records.len(), count);
        assert_eq!(records[0].nt1, (count as u32) << 24);
    }
    let error = reader.collect(&source, &target, false, 1).unwrap_err();
    assert!(error.is::<RequestCancelled>(), "{error}");
    cancelled.store(false, Ordering::Relaxed);
    assert_eq!(reader.probe_prng().unwrap(), PrngMode::Hard);
    drop(reader);
    drop(client);
    worker.join().unwrap();
}
