//! ST25TB command reports and family-specific requests.
use super::{HfReader, HfTag};
use crate::{DeviceError, RequestCancelled, Result, command::*};
use std::{
    ops::{Range, RangeInclusive},
    time::Duration,
};

const ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US: u8 = 22;
const USER_BLOCKS: u8 = 16;
const SYSTEM_BLOCK: u8 = 255;
const UID_FAMILY: u8 = 0xd0;
const ST_MANUFACTURER: u8 = 2;
const PRODUCT_512_AT: u8 = 0x33;
const UID_PRODUCT_OFFSET: usize = 2;
const SRI512_PRODUCTS: RangeInclusive<u8> = 0x18..=0x1b;
const COUNTER_BLOCKS: RangeInclusive<u8> = 5..=6;
const MIN_TARGET: u32 = 0x1000;
const DURATION_MS: RangeInclusive<u32> = 500..=7_200_000;
const MAX_ADJUSTMENT_US: u8 = 100;
const RESET_CHECK_DURATION_MS: u32 = 3000;
const RESPONSE_GRACE_MS: u64 = 4000;
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
const REQUEST_FORMAT: u8 = 0xa0;
const MODE_RUN: u8 = 0;
const MODE_RESET_CHECK: u8 = 4;
const STATUS_SUCCESS: u16 = 0;

const PROGRESS_BYTES: usize = 7;
const PROGRESS_KIND: usize = 0;
const PROGRESS_DELAY: Range<usize> = 1..3;
const PROGRESS_VALUE: Range<usize> = 3..7;
const REPORT_BYTES: usize = 64;
const REPORT_OUTCOME: usize = 0;
const REPORT_HAS_VALUE: usize = 1;
const REPORT_CLEANUP: usize = 2;
const REPORT_FLAGS: usize = 3;
const REPORT_VALUE: Range<usize> = 4..8;
const REPORT_INITIAL: Range<usize> = 8..12;
const REPORT_REQUEST_TO_OFF: Range<usize> = 12..16;
const REPORT_OFF_TO_ON: Range<usize> = 16..20;
const REPORT_READ_STAGE: usize = 20;
const REPORT_READ_ATTEMPTS: usize = 21;
const REPORT_READ_STATUS: Range<usize> = 22..24;
const REPORT_TIMING_OFFSET: usize = 24;
const TIMING_FIELDS: usize = 10;
const FLAG_INITIAL_VALUE: u8 = 1;
const FLAG_TIMING: u8 = 2;
const MAX_READ_STAGE: u8 = 4;
const OUTCOME_TRANSPORT_ERROR: u8 = 5;

#[derive(Debug)]
pub struct UnknownTearoffOutcome(pub Box<dyn std::error::Error>);
impl std::fmt::Display for UnknownTearoffOutcome {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(out, "Device outcome is unknown: {}", self.0)
    }
}
impl std::error::Error for UnknownTearoffOutcome {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TearoffOutcome {
    Success,
    Cancelled,
    DurationExpired,
    Unsupported,
    TagLost,
    TransportError,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TearoffResult {
    pub outcome: TearoffOutcome,
    pub last_value: Option<u32>,
    pub cleanup_ok: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TearoffDiagnostics {
    pub result: TearoffResult,
    pub request_to_off_cycles: u32,
    pub off_to_on_cycles: u32,
    pub read_stage: u8,
    pub read_status: u16,
    pub read_attempts: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TearoffLogKind {
    Advanced,
    Candidate,
    Lower,
    WriteVerified,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TearoffLog {
    pub kind: TearoffLogKind,
    pub value: u32,
    pub delay_us: u16,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TearoffReport {
    pub diagnostics: TearoffDiagnostics,
    pub initial_value: Option<u32>,
    pub timing: Option<TearoffTiming>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TearoffTiming {
    pub total_cycles: u32,
    pub timeslot_cycles: u32,
    pub grant_wait_cycles: u32,
    pub dma_cycles: u32,
    pub decode_cycles: u32,
    pub usb_cycles: u32,
    pub pause_cycles: u32,
    pub exchanges: u32,
    pub writes: u32,
    pub resets: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TearoffProgress {
    Initial(u32),
    Log(TearoffLog),
}

impl TearoffProgress {
    pub fn decode(data: &[u8]) -> Result<Self> {
        if data.len() != PROGRESS_BYTES {
            return Err("Invalid ST25TB live progress".into());
        }
        let value = u32::from_le_bytes(data[PROGRESS_VALUE].try_into()?);
        let delay_us = u16::from_le_bytes(data[PROGRESS_DELAY].try_into()?);
        let kind = match data[PROGRESS_KIND] {
            0 => TearoffLogKind::Advanced,
            1 => TearoffLogKind::Candidate,
            2 => TearoffLogKind::Lower,
            3 => TearoffLogKind::WriteVerified,
            4 if delay_us == 0 => return Ok(Self::Initial(value)),
            _ => return Err("Invalid ST25TB live progress kind".into()),
        };
        Ok(Self::Log(TearoffLog {
            kind,
            value,
            delay_us,
        }))
    }
}

impl TearoffDiagnostics {
    pub fn decode(data: &[u8]) -> Result<Self> {
        if data.len() != REPORT_BYTES || data[REPORT_READ_STAGE] > MAX_READ_STAGE {
            return Err("Invalid ST25TB diagnostic fields".into());
        }
        Ok(Self {
            result: TearoffResult::decode(data)?,
            request_to_off_cycles: u32::from_le_bytes(data[REPORT_REQUEST_TO_OFF].try_into()?),
            off_to_on_cycles: u32::from_le_bytes(data[REPORT_OFF_TO_ON].try_into()?),
            read_stage: data[REPORT_READ_STAGE],
            read_status: u16::from_le_bytes(data[REPORT_READ_STATUS].try_into()?),
            read_attempts: data[REPORT_READ_ATTEMPTS],
        })
    }
}

impl TearoffReport {
    pub fn decode(data: &[u8]) -> Result<Self> {
        let diagnostics = TearoffDiagnostics::decode(data)?;
        let initial = u32::from_le_bytes(data[REPORT_INITIAL].try_into()?);
        if data[REPORT_FLAGS] & FLAG_INITIAL_VALUE == 0 && initial != 0 {
            return Err("Invalid ST25TB report initial value".into());
        }
        if data[REPORT_FLAGS] & FLAG_TIMING == 0
            && data[REPORT_TIMING_OFFSET..].iter().any(|&byte| byte != 0)
        {
            return Err("Unexpected ST25TB timing fields".into());
        }
        let timing = (data[REPORT_FLAGS] & FLAG_TIMING != 0).then(|| {
            let [
                total_cycles,
                timeslot_cycles,
                grant_wait_cycles,
                dma_cycles,
                decode_cycles,
                usb_cycles,
                pause_cycles,
                exchanges,
                writes,
                resets,
            ]: [u32; TIMING_FIELDS] = std::array::from_fn(|index| {
                let offset = REPORT_TIMING_OFFSET + index * 4;
                u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
            });
            TearoffTiming {
                total_cycles,
                timeslot_cycles,
                grant_wait_cycles,
                dma_cycles,
                decode_cycles,
                usb_cycles,
                pause_cycles,
                exchanges,
                writes,
                resets,
            }
        });
        Ok(Self {
            diagnostics,
            initial_value: (data[REPORT_FLAGS] & FLAG_INITIAL_VALUE != 0).then_some(initial),
            timing,
        })
    }
}

impl TearoffResult {
    pub fn decode(data: &[u8]) -> Result<Self> {
        if data.len() != REPORT_BYTES
            || data[REPORT_OUTCOME] > OUTCOME_TRANSPORT_ERROR
            || data[REPORT_HAS_VALUE] > 1
            || data[REPORT_CLEANUP] > 1
            || data[REPORT_FLAGS] & !(FLAG_INITIAL_VALUE | FLAG_TIMING) != 0
        {
            return Err("Invalid ST25TB tearoff result".into());
        }
        let value = &data[REPORT_VALUE];
        if data[REPORT_HAS_VALUE] == 0 && value != [0; 4] {
            return Err("Invalid ST25TB tearoff value".into());
        }
        let outcome = match data[REPORT_OUTCOME] {
            0 => TearoffOutcome::Success,
            1 => TearoffOutcome::Cancelled,
            2 => TearoffOutcome::DurationExpired,
            3 => TearoffOutcome::Unsupported,
            4 => TearoffOutcome::TagLost,
            _ => TearoffOutcome::TransportError,
        };
        if outcome == TearoffOutcome::Success && data[REPORT_HAS_VALUE] == 0 {
            return Err("ST25TB success without a verified value".into());
        }
        Ok(Self {
            outcome,
            last_value: (data[REPORT_HAS_VALUE] == 1)
                .then(|| u32::from_le_bytes(value.try_into().unwrap())),
            cleanup_ok: data[REPORT_CLEANUP] == 1,
        })
    }
}

impl HfReader<'_> {
    pub fn write_st25tb(&mut self, tag: &HfTag, block: u8, value: [u8; 4]) -> Result<[u8; 4]> {
        let HfTag::St25tb {
            uid: [UID_FAMILY, ST_MANUFACTURER, PRODUCT_512_AT, ..],
            blocks: USER_BLOCKS,
            ..
        } = tag
        else {
            return Err("ST25TB512-AT tag required for physical writing".into());
        };
        if block >= USER_BLOCKS && block != SYSTEM_BLOCK {
            return Err("ST25TB block must be 0..15 or 255".into());
        }
        self.require(ST25TB_WRITE)?;
        let before = self.read(tag, u16::from(block))?;
        if before == value {
            return Ok(before);
        }
        let mut request = tag.uid().to_vec();
        request.push(block);
        request.extend(before);
        request.extend(value);
        let response = self.client.command(ST25TB_WRITE, &request, WRITE_TIMEOUT)?;
        if response.as_slice() != value {
            return Err("ST25TB write verification failed; reread the tag before retrying".into());
        }
        Ok(value)
    }

    pub fn tearoff(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
        duration_ms: u16,
        adjustment_us: u8,
    ) -> Result<TearoffResult> {
        Ok(self
            .tearoff_run(
                tag,
                block,
                target,
                false,
                duration_ms.into(),
                adjustment_us,
                false,
                &mut |_| {},
            )?
            .diagnostics
            .result)
    }

    pub fn tearoff_diagnostics(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
        duration_ms: u16,
        adjustment_us: u8,
    ) -> Result<TearoffDiagnostics> {
        Ok(self
            .tearoff_run(
                tag,
                block,
                target,
                false,
                duration_ms.into(),
                adjustment_us,
                false,
                &mut |_| {},
            )?
            .diagnostics)
    }

    pub fn tearoff_reset_check(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
    ) -> Result<TearoffDiagnostics> {
        Ok(self
            .tearoff_run(
                tag,
                block,
                target,
                true,
                RESET_CHECK_DURATION_MS,
                0,
                false,
                &mut |_| {},
            )?
            .diagnostics)
    }

    pub fn tearoff_live(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
        duration_ms: u32,
        timing: bool,
        progress: &mut dyn FnMut(TearoffProgress),
    ) -> Result<TearoffReport> {
        self.tearoff_live_with_adjustment(
            tag,
            block,
            target,
            duration_ms,
            ST25TB_TEAROFF_DEFAULT_ADJUSTMENT_US,
            timing,
            progress,
        )
    }

    #[allow(
        clippy::too_many_arguments,
        reason = "Preserve the explicit protocol API"
    )]
    pub fn tearoff_live_with_adjustment(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
        duration_ms: u32,
        adjustment_us: u8,
        timing: bool,
        progress: &mut dyn FnMut(TearoffProgress),
    ) -> Result<TearoffReport> {
        self.tearoff_run(
            tag,
            block,
            target,
            false,
            duration_ms,
            adjustment_us,
            timing,
            progress,
        )
    }

    #[allow(
        clippy::too_many_arguments,
        reason = "Keep protocol fields explicit through dispatch"
    )]
    fn tearoff_run(
        &mut self,
        tag: &HfTag,
        block: u8,
        target: u32,
        reset_check: bool,
        duration_ms: u32,
        adjustment_us: u8,
        timing: bool,
        progress: &mut dyn FnMut(TearoffProgress),
    ) -> Result<TearoffReport> {
        let request = self.tearoff_request(
            tag,
            block,
            target,
            reset_check,
            duration_ms,
            adjustment_us,
            timing,
        )?;
        let mut on_progress = |data: &[u8]| {
            let event = TearoffProgress::decode(data)?;
            progress(event);
            Ok(())
        };
        let (status, data) = self
            .client
            .request_stream(
                ST25TB_TEAROFF,
                &request,
                Duration::from_millis(u64::from(duration_ms) + RESPONSE_GRACE_MS),
                &mut on_progress,
            )
            .map_err(|error| {
                if error.downcast_ref::<RequestCancelled>().is_some() {
                    error
                } else {
                    Box::new(UnknownTearoffOutcome(error)) as Box<dyn std::error::Error>
                }
            })?;
        if status != STATUS_SUCCESS {
            return Err(DeviceError(status).into());
        }
        let report = TearoffReport::decode(&data).map_err(|error| {
            Box::new(UnknownTearoffOutcome(error)) as Box<dyn std::error::Error>
        })?;
        if report.diagnostics.result.outcome == TearoffOutcome::Success
            && report.diagnostics.result.last_value != Some(target)
        {
            return Err(
                UnknownTearoffOutcome("ST25TB success without the requested value".into()).into(),
            );
        }
        Ok(report)
    }

    #[allow(
        clippy::too_many_arguments,
        reason = "Keep request fields explicit at the wire boundary"
    )]
    pub(super) fn tearoff_request(
        &self,
        tag: &HfTag,
        block: u8,
        target: u32,
        reset_check: bool,
        duration_ms: u32,
        adjustment_us: u8,
        timing: bool,
    ) -> Result<Vec<u8>> {
        let HfTag::St25tb {
            uid,
            blocks: USER_BLOCKS,
            ..
        } = tag
        else {
            return Err("Supported 512-bit ST25TB/SRI512 tag is required".into());
        };
        if uid[..UID_PRODUCT_OFFSET] != [UID_FAMILY, ST_MANUFACTURER]
            || (uid[UID_PRODUCT_OFFSET] != PRODUCT_512_AT
                && !SRI512_PRODUCTS.contains(&uid[UID_PRODUCT_OFFSET]))
        {
            return Err("Supported 512-bit ST25TB/SRI512 tag is required".into());
        }
        if !COUNTER_BLOCKS.contains(&block)
            || target < MIN_TARGET
            || !DURATION_MS.contains(&duration_ms)
            || adjustment_us > MAX_ADJUSTMENT_US
            || (reset_check && adjustment_us != 0)
        {
            return Err("Invalid ST25TB tearoff options".into());
        }
        self.require(ST25TB_TEAROFF)?;
        let mut request = vec![REQUEST_FORMAT | u8::from(timing)];
        request.extend(uid);
        request.push(block);
        request.extend(target.to_le_bytes());
        request.push(if reset_check {
            MODE_RESET_CHECK
        } else {
            MODE_RUN
        });
        request.push(adjustment_us);
        request.extend(duration_ms.to_le_bytes());
        Ok(request)
    }
}
