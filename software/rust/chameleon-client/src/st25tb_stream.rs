//! USB progress frames and the existing out-of-band cancellation sequence.
use crate::{
    Result,
    frame::{encode, read_frame},
};
use std::{
    io::{Read, Write},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

const STATUS_REQUEST: u16 = 0;
const STATUS_PROGRESS: u16 = 0x79;
pub(crate) const CANCEL_SEQUENCE: &[u8] = b"\x03ST25STOP";
const CANCEL_TIMEOUT: Duration = Duration::from_secs(5);

pub fn exchange(
    port: &mut (impl Read + Write),
    command: u16,
    data: &[u8],
    timeout: Duration,
    cancelled: &AtomicBool,
    progress: &mut dyn FnMut(&[u8]) -> Result<()>,
) -> Result<(u16, Vec<u8>)> {
    port.write_all(&encode(command, STATUS_REQUEST, data)?)?;
    port.flush()?;
    let mut deadline = Instant::now() + timeout;
    let mut stop_sent = false;
    loop {
        let (status, payload) = read_frame(command, |bytes| {
            read_stream(port, bytes, &mut deadline, cancelled, &mut stop_sent)
        })?;
        if status != STATUS_PROGRESS {
            return Ok((status, payload));
        }
        progress(&payload)?;
    }
}

fn read_stream(
    port: &mut (impl Read + Write),
    mut bytes: &mut [u8],
    deadline: &mut Instant,
    cancelled: &AtomicBool,
    stop_sent: &mut bool,
) -> Result<()> {
    while !bytes.is_empty() {
        if cancelled.load(Ordering::Relaxed) && !*stop_sent {
            port.write_all(CANCEL_SEQUENCE)?;
            port.flush()?;
            *stop_sent = true;
            *deadline = Instant::now() + CANCEL_TIMEOUT;
        }
        if Instant::now() >= *deadline {
            return Err("Device response timed out; reconnect before continuing".into());
        }
        match port.read(bytes) {
            Ok(0) => return Err("Device disconnected".into()),
            Ok(count) => bytes = &mut bytes[count..],
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::Interrupted
                ) => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_progress_is_delivered_before_terminal_reply_and_ctrl_c_sends_cancel() {
        struct LivePort {
            input: std::io::Cursor<Vec<u8>>,
            output: Vec<u8>,
            reads: usize,
            cancelled: std::sync::Arc<AtomicBool>,
        }
        impl Read for LivePort {
            fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
                self.reads += 1;
                if self.reads == 11 {
                    self.cancelled.store(true, Ordering::Relaxed);
                }
                let count = bytes.len().min(2);
                self.input.read(&mut bytes[..count])
            }
        }
        impl Write for LivePort {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.output.extend_from_slice(bytes);
                Ok(bytes.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let cancelled = std::sync::Arc::new(AtomicBool::new(false));
        let mut reply = encode(2310, 0x79, &[4, 4, 0, 0, 1, 0, 0, 0]).unwrap();
        reply.extend(encode(2310, 0x68, &[4, 1]).unwrap());
        let mut port = LivePort {
            input: std::io::Cursor::new(reply),
            output: vec![],
            reads: 0,
            cancelled: cancelled.clone(),
        };
        let mut events = vec![];
        let result = exchange(
            &mut port,
            2310,
            &[4],
            Duration::from_secs(1),
            &cancelled,
            &mut |event| {
                events.push(event.to_vec());
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(events, vec![vec![4, 4, 0, 0, 1, 0, 0, 0]]);
        assert_eq!(result, (0x68, vec![4, 1]));
        let mut sent = encode(2310, 0, &[4]).unwrap();
        sent.extend(b"\x03ST25STOP");
        assert_eq!(port.output, sent);
    }
}
