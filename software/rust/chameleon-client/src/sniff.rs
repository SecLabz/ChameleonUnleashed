//! Passive NFC-A/V capture with bounded USB polling and session cleanup.
use crate::{
    Client, DeviceError, RequestCancelled, Result,
    command::{HF14A_SNIFF_LIVE, HF15_SNIFF_LIVE},
    frame,
};
use serde::Serialize;
use std::{
    sync::atomic::Ordering,
    thread,
    time::{Duration, Instant},
};

const REQUEST_TIMEOUT: Duration = Duration::from_secs(1);
const IDLE_POLL: Duration = Duration::from_millis(1);
pub use nfc_native::sniff::{
    CRC_CHECKED as SNIFF_CRC_CHECKED, CRC_VALID as SNIFF_CRC_VALID, DECODED as SNIFF_DECODED,
    PARITY_CHECKED as SNIFF_PARITY_CHECKED, PARITY_VALID as SNIFF_PARITY_VALID,
    TRUNCATED as SNIFF_TRUNCATED,
};
use nfc_native::sniff::{Direction, Frame as AnalysisFrame};

#[derive(Debug, Serialize)]
pub struct SniffFrame {
    /// Device RX completion time, in microseconds since START. Not an RF edge.
    pub end_us: u64,
    pub size_bits: u16,
    pub flags: u8,
    pub direction: u8,
    /// Raw receiver status; MFRC522 ErrorReg for tag frames.
    pub errors: u8,
    pub data: Vec<u8>,
}

impl SniffFrame {
    pub fn analysis_frame(&self) -> AnalysisFrame<'_> {
        AnalysisFrame {
            data: &self.data,
            size_bits: self.size_bits,
            direction: if self.direction == 0 {
                Direction::Reader
            } else {
                Direction::Tag
            },
            flags: self.flags,
            receiver_error: self.errors != 0,
        }
    }

    pub fn plaintext_valid(&self) -> bool {
        self.analysis_frame().plaintext_valid()
    }
}

#[derive(Debug, Default, Clone, Copy, Serialize)]
pub struct SniffSummary {
    pub reader_frames: u32,
    pub tag_frames: u32,
    pub capture_overruns: u32,
    pub tx_frames: u32,
    pub dropped_frames: u32,
    pub invalid_frames: u32,
}

#[derive(Debug)]
pub struct SniffBatch {
    pub active: bool,
    pub summary: SniffSummary,
    pub frames: Vec<SniffFrame>,
}

impl SniffBatch {
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < 28 || bytes[0] != 2 || bytes[1] > 1 {
            return Err("Invalid HF sniff batch header".into());
        }
        let count = u16::from_le_bytes(bytes[2..4].try_into()?) as usize;
        if count > 16 {
            return Err("Invalid HF sniff frame count".into());
        }
        let mut frames = Vec::with_capacity(count);
        let mut offset = 28;
        for _ in 0..count {
            let record = &bytes[offset..];
            if record.len() < 14 {
                return Err("Truncated HF sniff frame header".into());
            }
            let size_bits = u16::from_le_bytes(record[8..10].try_into()?);
            let flags = record[10];
            let length = record[13] as usize;
            if length > 143
                || size_bits == 0
                || usize::from(size_bits).div_ceil(8) != length
                || record.len() < 14 + length
                || flags & !63 != 0
                || record[11] > 1
                || (flags & SNIFF_PARITY_VALID != 0 && flags & SNIFF_PARITY_CHECKED == 0)
                || (flags & SNIFF_CRC_VALID != 0 && flags & SNIFF_CRC_CHECKED == 0)
            {
                return Err("Invalid HF sniff frame geometry or flags".into());
            }
            frames.push(SniffFrame {
                end_us: u64::from_le_bytes(record[..8].try_into()?),
                size_bits,
                flags,
                direction: record[11],
                errors: record[12],
                data: record[14..14 + length].to_vec(),
            });
            offset += 14 + length;
        }
        if offset != bytes.len() {
            return Err("Trailing HF sniff batch data".into());
        }
        Ok(Self {
            active: bytes[1] != 0,
            summary: SniffSummary {
                reader_frames: u32::from_le_bytes(bytes[4..8].try_into()?),
                tag_frames: u32::from_le_bytes(bytes[8..12].try_into()?),
                dropped_frames: u32::from_le_bytes(bytes[12..16].try_into()?),
                invalid_frames: u32::from_le_bytes(bytes[16..20].try_into()?),
                capture_overruns: u32::from_le_bytes(bytes[20..24].try_into()?),
                tx_frames: u32::from_le_bytes(bytes[24..28].try_into()?),
            },
            frames,
        })
    }
}

impl Client {
    /// Capture until the duration expires or cancellation is requested. Complete
    /// each bounded USB exchange before STOP and drain so framing stays usable.
    /// The callback also receives the final stopped batch, including queued data.
    pub fn sniff_nfca(
        &mut self,
        gain: u8,
        duration: Option<Duration>,
        emit: &mut dyn FnMut(&SniffBatch) -> Result<()>,
    ) -> Result<SniffSummary> {
        self.sniff_capture(HF14A_SNIFF_LIVE, gain, duration, emit)
    }

    pub fn sniff_nfcv(
        &mut self,
        gain: u8,
        duration: Option<Duration>,
        emit: &mut dyn FnMut(&SniffBatch) -> Result<()>,
    ) -> Result<SniffSummary> {
        self.sniff_capture(HF15_SNIFF_LIVE, gain, duration, emit)
    }

    fn sniff_capture(
        &mut self,
        command: u16,
        gain: u8,
        duration: Option<Duration>,
        emit: &mut dyn FnMut(&SniffBatch) -> Result<()>,
    ) -> Result<SniffSummary> {
        if gain > 7 {
            return Err("HF receiver gain must be 0..7".into());
        }
        if !self.capabilities.contains(&command) {
            return Err("Update the Ultra firmware for live passive HF sniffing".into());
        }
        if self.cancelled.load(Ordering::Relaxed) {
            return Err(RequestCancelled.into());
        }
        let mut started_session = false;
        let mut emit_failed = false;
        let result = (|| {
            let ack = self.sniff_exchange(command, &[0, gain])?;
            started_session = true;
            if !ack.is_empty() {
                return Err("Invalid HF sniff start acknowledgement".into());
            }
            let started = Instant::now();
            while !self.cancelled.load(Ordering::Relaxed)
                && duration.is_none_or(|limit| started.elapsed() < limit)
            {
                let batch = SniffBatch::decode(&self.sniff_exchange(command, &[1])?)?;
                emit(&batch).inspect_err(|_| emit_failed = true)?;
                if batch.summary.tx_frames != 0 {
                    return Err("Passive capture observed an unexpected NFCT transmission".into());
                }
                if !batch.active {
                    return Err(
                        "HF capture expired or was stopped by another device operation".into(),
                    );
                }
                if batch.frames.is_empty() {
                    thread::sleep(IDLE_POLL);
                }
            }
            Ok(())
        })();
        if !self.usable {
            return Err(format!(
                "{}; capture stop unconfirmed (device lease expires after 2 seconds)",
                result.unwrap_err()
            )
            .into());
        }
        if !started_session {
            return result.map(|_| SniffSummary::default());
        }
        let finish = (|| {
            let mut batch = SniffBatch::decode(&self.sniff_exchange(command, &[2])?)?;
            let mut delivery_error = None;
            for _ in 0..4 {
                if batch.active {
                    return Err("HF capture did not stop".into());
                }
                let empty = batch.frames.is_empty();
                // After an output failure, still stop and drain the device.
                if !emit_failed && let Err(error) = emit(&batch) {
                    emit_failed = true;
                    delivery_error = Some(error);
                }
                if empty {
                    return delivery_error.map_or(Ok(batch.summary), Err);
                }
                batch = SniffBatch::decode(&self.sniff_exchange(command, &[1])?)?;
            }
            Err("HF capture queue did not drain after STOP".into())
        })()
        .and_then(|summary| {
            if summary.tx_frames != 0 {
                return Err("Passive capture observed an unexpected NFCT transmission".into());
            }
            Ok(summary)
        });
        match (result, finish) {
            (Ok(()), summary) => summary,
            (Err(error), Ok(_)) => Err(error),
            (Err(error), Err(cleanup)) => {
                Err(format!("{error}; capture cleanup failed: {cleanup}").into())
            }
        }
    }

    fn sniff_exchange(&mut self, command: u16, data: &[u8]) -> Result<Vec<u8>> {
        if !self.usable {
            return Err("Connection is unusable; reconnect before continuing".into());
        }
        // Complete each exchange despite cancellation so STOP can reuse its framing.
        let response = frame::exchange(
            &mut self.port,
            command,
            data,
            if command == HF15_SNIFF_LIVE {
                Duration::from_secs(2)
            } else {
                REQUEST_TIMEOUT
            },
        );
        let (status, bytes) = response.inspect_err(|_| self.usable = false)?;
        if !matches!(status, 0 | 0x68) {
            return Err(DeviceError(status).into());
        }
        Ok(bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn batch() -> Vec<u8> {
        let mut bytes = vec![2, 1, 1, 0];
        bytes.extend([0; 24]);
        bytes.extend(123456u64.to_le_bytes());
        bytes.extend([7, 0, 1, 0, 0, 1, 0x26]);
        bytes
    }

    #[test]
    fn decode_requires_complete_valid_batches() {
        let bytes = batch();
        let decoded = SniffBatch::decode(&bytes).unwrap();
        assert_eq!(decoded.frames[0].end_us, 123456);
        assert_eq!(decoded.frames[0].size_bits, 7);
        assert_eq!(decoded.frames[0].data, [0x26]);
        for length in 0..bytes.len() {
            assert!(SniffBatch::decode(&bytes[..length]).is_err());
        }
        for (offset, value) in [
            (0, 3),
            (1, 2),
            (2, 17),
            (36, 0),
            (38, 128),
            (38, 4),
            (38, 16),
            (39, 2),
            (41, 65),
        ] {
            let mut invalid = bytes.clone();
            invalid[offset] = value;
            assert!(SniffBatch::decode(&invalid).is_err());
        }
        let mut extra = bytes;
        extra.push(0);
        assert!(SniffBatch::decode(&extra).is_err());
    }

    #[test]
    fn nfcv_batches_preserve_full_slix_option_replies() {
        let data: Vec<u8> = (0..143).collect();
        let mut bytes = vec![2, 0, 1, 0];
        bytes.extend([0; 24]);
        bytes.extend(42u64.to_le_bytes());
        bytes.extend((143u16 * 8).to_le_bytes());
        bytes.extend([
            SNIFF_DECODED | SNIFF_CRC_CHECKED | SNIFF_CRC_VALID,
            1,
            0,
            143,
        ]);
        bytes.extend(&data);
        let decoded = SniffBatch::decode(&bytes).unwrap();
        assert_eq!(decoded.frames[0].data, data);
        assert_eq!(decoded.frames[0].size_bits, 1144);
        assert!(decoded.frames[0].plaintext_valid());
    }

    #[cfg(unix)]
    #[test]
    fn nfcv_stop_uses_its_own_command_and_preserves_the_serial_session() {
        let mut stopped = vec![0; 28];
        stopped[0] = 2;
        let (mut client, worker) = crate::test_support::device(vec![
            (HF15_SNIFF_LIVE, vec![0, 7], 0x68, vec![]),
            (HF15_SNIFF_LIVE, vec![2], 0x68, stopped),
            (1000, vec![], 0x68, vec![2, 2]),
        ]);
        client.capabilities.insert(HF15_SNIFF_LIVE);
        let mut delivered = 0;
        client
            .sniff_nfcv(7, Some(Duration::ZERO), &mut |_| {
                delivered += 1;
                Ok(())
            })
            .unwrap();
        assert_eq!(delivered, 1);
        assert_eq!(client.command(1000, &[], REQUEST_TIMEOUT).unwrap(), [2, 2]);
        drop(client);
        worker.join().unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn cancellation_stops_and_drains_without_losing_the_serial_session() {
        let mut stopped = batch();
        stopped[1] = 0;
        let mut empty = vec![0; 28];
        empty[0] = 2;
        let (mut client, worker) = crate::test_support::device(vec![
            (HF14A_SNIFF_LIVE, vec![0, 6], 0x68, vec![]),
            (HF14A_SNIFF_LIVE, vec![1], 0x68, batch()),
            (HF14A_SNIFF_LIVE, vec![2], 0x68, stopped),
            (HF14A_SNIFF_LIVE, vec![1], 0x68, empty),
            (1000, vec![], 0x68, vec![2, 2]),
        ]);
        client.capabilities.insert(HF14A_SNIFF_LIVE);
        let cancelled = client.cancelled.clone();
        let mut frames = 0;
        client
            .sniff_nfca(6, None, &mut |batch| {
                frames += batch.frames.len();
                cancelled.store(true, Ordering::Relaxed);
                Ok(())
            })
            .unwrap();
        assert_eq!(frames, 2);
        assert!(cancelled.load(Ordering::Relaxed));
        cancelled.store(false, Ordering::Relaxed);
        assert_eq!(client.command(1000, &[], REQUEST_TIMEOUT).unwrap(), [2, 2]);
        drop(client);
        worker.join().unwrap();
    }
}

#[cfg(all(test, unix))]
mod failure_tests {
    use super::*;

    fn empty() -> Vec<u8> {
        let mut bytes = vec![0; 28];
        bytes[0] = 2;
        bytes
    }

    #[test]
    fn rejected_start_does_not_stop_another_capture() {
        let (mut client, worker) =
            crate::test_support::device(vec![(HF14A_SNIFF_LIVE, vec![0, 6], 0x76, vec![])]);
        client.capabilities.insert(HF14A_SNIFF_LIVE);
        assert!(
            client
                .sniff_nfca(6, None, &mut |_| panic!("No frames expected"))
                .is_err()
        );
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn malformed_start_acknowledgement_still_stops_and_drains() {
        let (mut client, worker) = crate::test_support::device(vec![
            (HF14A_SNIFF_LIVE, vec![0, 6], 0x68, vec![1]),
            (HF14A_SNIFF_LIVE, vec![2], 0x68, empty()),
        ]);
        client.capabilities.insert(HF14A_SNIFF_LIVE);
        assert!(
            client
                .sniff_nfca(6, None, &mut |_| Ok(()))
                .unwrap_err()
                .to_string()
                .contains("acknowledgement")
        );
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn output_failure_still_stops_and_does_not_retry_the_callback() {
        let mut active = empty();
        active[1] = 1;
        let (mut client, worker) = crate::test_support::device(vec![
            (HF14A_SNIFF_LIVE, vec![0, 6], 0x68, vec![]),
            (HF14A_SNIFF_LIVE, vec![1], 0x68, active),
            (HF14A_SNIFF_LIVE, vec![2], 0x68, empty()),
        ]);
        client.capabilities.insert(HF14A_SNIFF_LIVE);
        let mut calls = 0;
        assert!(
            client
                .sniff_nfca(6, None, &mut |_| {
                    calls += 1;
                    Err("Output unavailable".into())
                })
                .unwrap_err()
                .to_string()
                .contains("Output unavailable")
        );
        assert_eq!(calls, 1);
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn transmission_in_the_final_batch_cannot_succeed() {
        let mut stopped = empty();
        stopped[24] = 1;
        let (mut client, worker) = crate::test_support::device(vec![
            (HF14A_SNIFF_LIVE, vec![0, 6], 0x68, vec![]),
            (HF14A_SNIFF_LIVE, vec![2], 0x68, stopped),
        ]);
        client.capabilities.insert(HF14A_SNIFF_LIVE);
        let mut observed_tx = 0;
        let error = client
            .sniff_nfca(6, Some(Duration::ZERO), &mut |batch| {
                observed_tx = batch.summary.tx_frames;
                Ok(())
            })
            .unwrap_err();
        assert!(error.to_string().contains("unexpected NFCT transmission"));
        assert_eq!(observed_tx, 1);
        drop(client);
        worker.join().unwrap();
    }
}
