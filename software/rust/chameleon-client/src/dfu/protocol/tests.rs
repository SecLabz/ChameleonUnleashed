use super::*;
use std::{collections::VecDeque, io::Cursor, sync::atomic::Ordering};

struct Step {
    request: Vec<u8>,
    response: Vec<u8>,
}

fn step(operation: u8, payload: &[u8], response: &[u8]) -> Step {
    let mut reply = vec![operation, 1];
    reply.extend(response);
    Step {
        request: encode(operation, payload),
        response: encode(RESPONSE, &reply),
    }
}

struct Port {
    steps: VecDeque<Step>,
    incoming: Cursor<Vec<u8>>,
    sent: Vec<Vec<u8>>,
    timeout_once: bool,
}

impl Port {
    fn new(steps: Vec<Step>) -> Self {
        Self {
            steps: steps.into(),
            incoming: Cursor::new(vec![]),
            sent: vec![],
            timeout_once: false,
        }
    }
}

impl Read for Port {
    fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
        if self.timeout_once {
            self.timeout_once = false;
            return Err(std::io::ErrorKind::TimedOut.into());
        }
        self.incoming.read(&mut bytes[..1])
    }
}

impl Write for Port {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        let expected = self.steps.pop_front().expect("Unexpected write or retry");
        assert_eq!(bytes, expected.request);
        assert_eq!(
            self.incoming.position() as usize,
            self.incoming.get_ref().len()
        );
        self.incoming = Cursor::new(expected.response);
        self.sent.push(bytes.to_vec());
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn receipt(offset: u32, bytes: &[u8]) -> Vec<u8> {
    [offset.to_le_bytes(), crc32fast::hash(bytes).to_le_bytes()].concat()
}

fn write(data: &[u8], received: &[u8]) -> Step {
    Step {
        request: encode(WRITE, data),
        response: step(CRC, &[], &receipt(received.len() as u32, received)).response,
    }
}

fn transfer() -> Vec<Step> {
    vec![
        step(SET_PRN, &[1, 0], &[]),
        step(MTU, &[], &[11, 0]),
        // Stale command progress must be discarded, not resumed.
        step(SELECT, &[COMMAND], &[16, 0, 0, 0, 12, 0, 0, 0, 1, 2, 3, 4]),
        step(CREATE, &[COMMAND, 3, 0, 0, 0], &[]),
        write(&[END, ESC, 1], &[END, ESC, 1]),
        step(EXECUTE, &[], &[]),
        step(SELECT, &[DATA], &[8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        step(CREATE, &[DATA, 8, 0, 0, 0], &[]),
        write(b"1234", b"1234"),
        write(b"5678", b"12345678"),
        step(EXECUTE, &[], &[]),
        step(CREATE, &[DATA, 1, 0, 0, 0], &[]),
        write(b"9", b"123456789"),
        step(EXECUTE, &[], &[]),
    ]
}

#[test]
fn slip_matches_wire_vectors_and_decodes_fragmented_escaped_replies() {
    assert_eq!(
        encode(WRITE, &[END, ESC, 1]),
        [8, 0xdb, 0xdc, 0xdb, 0xdd, 1, 0xc0]
    );
    assert_eq!(encode(SET_PRN, &[1, 0]), [2, 1, 0, 0xc0]);
    let mut port = Port::new(vec![]);
    port.incoming = Cursor::new(vec![END, RESPONSE, CRC, 1, ESC, 0xdc, ESC, 0xdd, END]);
    port.timeout_once = true;
    let cancelled = AtomicBool::new(false);
    let mut session = Session {
        port,
        cancelled: &cancelled,
        packet_bytes: 4,
    };
    assert_eq!(session.response::<2>(CRC).unwrap(), [END, ESC]);
}

#[test]
fn malformed_rejected_and_truncated_replies_stop_without_writes() {
    let mut oversized = vec![0x60; MAX_RESPONSE_BYTES + 1];
    oversized.push(END);
    for bytes in [
        vec![RESPONSE, CRC, 1, ESC, 0x00, END],
        vec![RESPONSE, CRC, 1, ESC, END],
        vec![RESPONSE, EXECUTE, 1, END],
        vec![RESPONSE, CRC, 1, 0, END],
        vec![RESPONSE, CRC, 0x0b, 0x08, END],
        vec![RESPONSE, CRC],
        oversized,
    ] {
        let mut port = Port::new(vec![]);
        port.incoming = Cursor::new(bytes);
        let cancelled = AtomicBool::new(false);
        let mut session = Session {
            port,
            cancelled: &cancelled,
            packet_bytes: 4,
        };
        assert!(session.response::<0>(CRC).is_err());
        assert!(session.port.sent.is_empty());
    }
}

#[test]
fn transfer_resets_old_progress_obeys_mtu_and_commits_each_object_after_crc() {
    let mut port = Port::new(transfer());
    let cancelled = AtomicBool::new(false);
    let mut accepted = vec![];
    Session::open(&mut port, &cancelled)
        .unwrap()
        .flash(&[END, ESC, 1], b"123456789", |done| accepted.push(done))
        .unwrap();
    assert_eq!(accepted, [8, 9]);
    assert!(port.steps.is_empty());
    assert_eq!(crc32fast::hash(b"123456789"), 0xcbf43926);
}

#[test]
fn crc_or_offset_mismatch_and_missing_receipts_never_execute_or_retry_an_object() {
    for receipt in [receipt(5, b"1234"), receipt(4, b"bad!"), vec![]] {
        let mut steps = transfer();
        let failing = steps
            .iter_mut()
            .find(|step| step.request == encode(WRITE, b"1234"))
            .unwrap();
        failing.response = if receipt.is_empty() {
            vec![]
        } else {
            step(CRC, &[], &receipt).response
        };
        let mut port = Port::new(steps);
        let cancelled = AtomicBool::new(false);
        let mut accepted = vec![];
        assert!(
            Session::open(&mut port, &cancelled)
                .unwrap()
                .flash(&[END, ESC, 1], b"123456789", |done| accepted.push(done))
                .is_err()
        );
        assert!(accepted.is_empty());
        assert_eq!(port.sent.last().unwrap(), &encode(WRITE, b"1234"));
        assert_eq!(
            port.sent
                .iter()
                .filter(|frame| **frame == encode(EXECUTE, &[]))
                .count(),
            1
        );
    }
}

#[test]
fn rejected_init_packet_and_stale_firmware_progress_prevent_firmware_writes() {
    for rejection in [Some(0x08), Some(0x05), None] {
        let mut steps = transfer();
        if let Some(code) = rejection {
            steps
                .iter_mut()
                .find(|step| step.request == encode(EXECUTE, &[]))
                .unwrap()
                .response = vec![RESPONSE, EXECUTE, 0x0b, code, END];
        } else {
            steps
                .iter_mut()
                .find(|step| step.request == encode(SELECT, &[DATA]))
                .unwrap()
                .response = step(SELECT, &[], &[8, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]).response;
        }
        let mut port = Port::new(steps);
        let cancelled = AtomicBool::new(false);
        let error = Session::open(&mut port, &cancelled)
            .unwrap()
            .flash(&[END, ESC, 1], b"123456789", |_| {})
            .unwrap_err();
        if rejection == Some(0x05) {
            assert!(error.to_string().contains("firmware version rejected"));
        }
        assert!(!port.sent.contains(&encode(CREATE, &[DATA, 8, 0, 0, 0])));
    }
}

#[test]
fn cancellation_keeps_committed_progress_and_sends_no_next_object() {
    let cancelled = AtomicBool::new(false);
    let mut port = Port::new(transfer());
    let mut accepted = vec![];
    let error = Session::open(&mut port, &cancelled)
        .unwrap()
        .flash(&[END, ESC, 1], b"123456789", |done| {
            accepted.push(done);
            cancelled.store(true, Ordering::Relaxed);
        })
        .unwrap_err();
    assert!(error.is::<crate::RequestCancelled>());
    assert_eq!(accepted, [8]);
    assert_eq!(port.sent.last().unwrap(), &encode(EXECUTE, &[]));
    assert!(!port.sent.contains(&encode(CREATE, &[DATA, 1, 0, 0, 0])));

    let mut port = Port::new(vec![]);
    assert!(Session::open(&mut port, &cancelled).is_err());
    assert!(port.sent.is_empty());
}

#[test]
fn invalid_mtu_and_init_size_are_rejected_before_creating_objects() {
    let cancelled = AtomicBool::new(false);
    let mut port = Port::new(vec![step(SET_PRN, &[1, 0], &[]), step(MTU, &[], &[4, 0])]);
    assert!(Session::open(&mut port, &cancelled).is_err());
    for max in [0, 2] {
        let mut port = Port::new(vec![
            step(SET_PRN, &[1, 0], &[]),
            step(MTU, &[], &[11, 0]),
            step(SELECT, &[COMMAND], &[max, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        ]);
        assert!(
            Session::open(&mut port, &cancelled)
                .unwrap()
                .flash(&[END, ESC, 1], b"123456789", |_| {})
                .is_err()
        );
        assert_eq!(port.sent.len(), 3);
    }
}
