//! Serial contracts for selection, cancellation and HF/LF isolation.
use crate::{Sense, SlotImage, test_support::device as mock_device};
use std::sync::atomic::Ordering;
fn device(exchanges: Vec<Exchange>) -> (crate::Client, std::thread::JoinHandle<()>) {
    let (mut client, worker) = mock_device(exchanges);
    client.capabilities = (0..=6000).collect();
    (client, worker)
}
type Exchange = (u16, Vec<u8>, u16, Vec<u8>);
fn reply(id: u16, payload: &[u8], bytes: &[u8]) -> Exchange {
    (id, payload.to_vec(), 0x68, bytes.to_vec())
}
fn slots(slot2_lf: u16) -> Vec<Exchange> {
    let mut types = vec![0; 32];
    types[6..8].copy_from_slice(&slot2_lf.to_be_bytes());
    // Keep a preexisting HF image in the destination slot.
    types[4..6].copy_from_slice(&1001u16.to_be_bytes());
    let mut enabled = vec![0; 16];
    enabled[2] = 1;
    enabled[3] = 1;
    let mut requests = vec![reply(1019, &[], &types), reply(1023, &[], &enabled)];
    for number in 0..8 {
        for sense in [2, 1] {
            requests.push((1008, vec![number, sense], 0x71, vec![]));
        }
    }
    requests
}
#[test]
fn initialize_selects_destination_and_restores_previous_without_touching_hf() {
    let mut requests = vec![
        reply(1002, &[], &[1]),
        reply(1018, &[], &[0]),
        reply(1003, &[1], &[]),
        reply(1018, &[], &[1]),
        reply(1004, &[1, 0, 100], &[]),
        reply(1005, &[1, 0, 100], &[]),
        reply(1006, &[1, 1, 1], &[]),
        reply(1009, &[], &[]),
    ];
    requests.extend(slots(100));
    requests.push(reply(5001, &[], &[0, 100, 1, 2, 3, 4, 5]));
    requests.extend([
        reply(1018, &[], &[1]),
        reply(1003, &[0], &[]),
        reply(1018, &[], &[0]),
        reply(1009, &[], &[]),
    ]);
    let (mut client, worker) = device(requests);
    client.initialize_slot(2, 100).unwrap();
    drop(client);
    worker.join().unwrap();
}
#[test]
fn cancellation_during_action_still_restores_selection_and_flag() {
    let (mut client, worker) = device(vec![
        reply(1002, &[], &[1]),
        reply(1018, &[], &[0]),
        reply(1003, &[1], &[]),
        reply(1018, &[], &[1]),
        reply(1018, &[], &[1]),
        reply(1003, &[0], &[]),
        reply(1018, &[], &[0]),
    ]);
    let result: crate::Result<()> = client.with_slot(2, false, |client| {
        client.cancelled.store(true, Ordering::Relaxed);
        Err(crate::RequestCancelled.into())
    });
    assert!(result.unwrap_err().is::<crate::RequestCancelled>());
    assert!(client.cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}
#[test]
fn invalid_slots_images_and_names_are_rejected_before_serial_io() {
    let (mut client, worker) = device(vec![]);
    assert!(client.select_slot(0).is_err());
    assert!(client.select_slot(9).is_err());
    assert!(client.rename_slot(1, Sense::Hf, "bad\nname").is_err());
    let image = SlotImage {
        version: 1,
        tag_type: 1001,
        identity: vec![],
        options: vec![],
        memory: vec![0; 1024],
    };
    assert!(client.load_slot_image(1, &image).is_err());
    drop(client);
    worker.join().unwrap();
}
#[test]
fn active_emulation_blocks_slot_mutation() {
    let (mut client, worker) = device(vec![reply(1002, &[], &[0])]);
    assert!(
        client
            .clear_slot(2, Sense::Lf)
            .unwrap_err()
            .to_string()
            .contains("slot stop")
    );
    drop(client);
    worker.join().unwrap();
}

#[test]
fn legacy_image_reads_keep_the_idle_requirement_and_restore_selection() {
    for idle in [false, true] {
        let mut requests = slots(100);
        requests.push(reply(1002, &[], &[u8::from(idle)]));
        if idle {
            requests.extend([
                reply(1018, &[], &[0]),
                reply(1003, &[1], &[]),
                reply(1018, &[], &[1]),
                reply(5001, &[], &[0, 100, 1, 2, 3, 4, 5]),
                reply(1018, &[], &[1]),
                reply(1003, &[0], &[]),
                reply(1018, &[], &[0]),
            ]);
        }
        let (mut client, worker) = device(requests);
        client.capabilities.remove(&crate::command::SLOT_SNAPSHOT);
        let result = client.read_slot_image(2, Sense::Lf);
        if idle {
            assert_eq!(result.unwrap().memory, [1, 2, 3, 4, 5]);
        } else {
            assert!(result.unwrap_err().to_string().contains("slot stop"));
        }
        drop(client);
        worker.join().unwrap();
    }
}
#[test]
fn slot_list_preserves_separate_sides_and_missing_names() {
    let (mut client, worker) = device(slots(100));
    let values = client.slots().unwrap();
    assert_eq!(values[1].hf.tag_type, 1001);
    assert_eq!(values[1].lf.tag_type, 100);
    assert!(values[1].hf.enabled);
    assert!(values[1].lf.name.is_empty());
    drop(client);
    worker.join().unwrap();
}
#[test]
fn all_image_shapes_reject_truncation_and_unknown_versions() {
    for &(id, _) in crate::SLOT_TYPES {
        let (unit, count, extra) = crate::slot_image::geometry(id).unwrap();
        let mut image = SlotImage {
            version: 1,
            tag_type: id,
            identity: vec![],
            options: vec![],
            memory: vec![0; unit * (count + extra)],
        };
        if (1000..=1003).contains(&id) {
            image.identity = vec![4, 1, 2, 3, 4, 4, 0, 8, 0];
            image.options = vec![0, 0, 0, 0, 0, 0, 1];
        }
        if (1100..=1108).contains(&id) {
            image.identity = vec![7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 0, 0];
            image.options = vec![0, 0, 0];
        }
        if id == 2000 {
            image.memory[5..8].copy_from_slice(&[0x33, 2, 0xd0]);
        }
        if id == 1300 {
            image.memory = vec![
                7, 4, 1, 2, 3, 4, 5, 6, 0, 10, 0, 0, b'D', b'F', b'U', b'1', 0x44, 3, 0x20, 2, 2,
                0, 0, 0,
            ];
        }
        if matches!(id, 1500 | 1501) {
            image.memory[..3].copy_from_slice(&[0xe0, 4, if id == 1500 { 3 } else { 1 }]);
            if id == 1501 {
                image.memory[3] = 0x10;
            }
        }
        image
            .validate_load()
            .unwrap_or_else(|error| panic!("{id}: {error}"));
        image.version = 2;
        assert!(image.validate().is_err());
        image.version = 1;
        image.memory.pop();
        assert!(image.validate().is_err());
    }
}

#[test]
fn cancellation_between_selection_and_verification_restores_previous_slot() {
    use std::sync::{Arc, atomic::AtomicBool};
    let flag = Arc::new(AtomicBool::new(false));
    let hook_flag = flag.clone();
    let mut selected = false;
    let (mut client, worker) = crate::test_support::device_with_hook(
        vec![
            reply(1002, &[], &[1]),
            reply(1018, &[], &[0]),
            reply(1003, &[1], &[]),
            reply(1018, &[], &[1]),
            reply(1003, &[0], &[]),
            reply(1018, &[], &[0]),
        ],
        move |id| {
            if id == 1003 && !selected {
                selected = true;
                hook_flag.store(true, Ordering::Relaxed);
            }
        },
    );
    client.cancelled = flag;
    client.capabilities = (0..=6000).collect();
    let result: crate::Result<()> = client.with_slot(2, false, |_| {
        panic!("Cancelled selection must not run the action")
    });
    assert!(result.unwrap_err().is::<crate::RequestCancelled>());
    assert!(client.cancelled.load(Ordering::Relaxed));
    drop(client);
    worker.join().unwrap();
}
#[test]
fn disabling_an_inactive_slot_restores_selection_after_firmware_auto_advance() {
    let mut requests = vec![
        reply(1002, &[], &[1]),
        reply(1018, &[], &[0]),
        reply(1003, &[1], &[]),
        reply(1018, &[], &[1]),
        reply(1006, &[1, 1, 0], &[]),
        reply(1009, &[], &[]),
    ];
    let mut snapshot = slots(100);
    snapshot[0].3[4..6].fill(0);
    snapshot[1].3[2..4].fill(0);
    requests.extend(snapshot);
    requests.extend([
        reply(1018, &[], &[2]),
        reply(1003, &[0], &[]),
        reply(1018, &[], &[0]),
        reply(1009, &[], &[]),
    ]);
    let (mut client, worker) = device(requests);
    client.enable_slot(2, Sense::Lf, false).unwrap();
    drop(client);
    worker.join().unwrap();
}

#[test]
fn missing_import_capability_rejects_image_before_any_serial_command() {
    let (mut client, worker) = device(vec![]);
    client.capabilities.remove(&4041);
    let image = SlotImage {
        version: 1,
        tag_type: 1001,
        identity: vec![4, 1, 2, 3, 4, 4, 0, 8, 0],
        options: vec![0, 0, 0, 0, 0, 0, 1],
        memory: vec![0; 1024],
    };
    let error = client.load_slot_image(2, &image).unwrap_err().to_string();
    assert!(error.contains("4041"), "{error}");
    drop(client);
    worker.join().unwrap();
}
#[test]
fn missing_initialization_getter_rejects_before_factory_replacement() {
    let (mut client, worker) = device(vec![]);
    client.capabilities.remove(&4040);
    let error = client.initialize_slot(2, 1001).unwrap_err().to_string();
    assert!(error.contains("4040"), "{error}");
    drop(client);
    worker.join().unwrap();
}
