//! Console presentation for the existing ST25TB tearoff command.
use super::MILLIS_PER_MINUTE;
use crate::{hf::info, ui};
use chameleon_client::{
    Client, HfReader, Result, TearoffLogKind, TearoffOutcome, TearoffProgress, TearoffResult,
    TearoffTiming, UnknownTearoffOutcome,
};
use std::{
    io::{IsTerminal, Write},
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};

const DEVICE_CYCLES_PER_US: u32 = 64;
const TIMING_REPORT_LIMIT: Duration = Duration::from_secs(60);
const HEADING_COLOR: &str = "1;36";

pub(super) fn run(
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    block: u8,
    target: u32,
    safety_minutes: u16,
    adjustment_us: u8,
    timing: bool,
) -> Result<()> {
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let tag = reader.st25tb()?;
    info(&tag);
    println!(
        "{}\n",
        ui::paint(
            "START TEAR OFF",
            HEADING_COLOR,
            ui::color_enabled() && std::io::stdout().is_terminal()
        )
    );
    println!(
        " Target block: {block}\n Target value: 0x{target:08X}\nAdjustment us: {adjustment_us}\nSafety deadline: {safety_minutes} min\n"
    );
    let mut progress = |event| print_progress(event);
    let duration_ms = u32::from(safety_minutes) * MILLIS_PER_MINUTE;
    let started = Instant::now();
    let response = reader.tearoff_live_with_adjustment(
        &tag,
        block,
        target,
        duration_ms,
        adjustment_us,
        timing,
        &mut progress,
    );
    let elapsed = started.elapsed();
    let cleanup = reader.finish();
    let stopped = reader.cancelled();
    let result = match response {
        Ok(transcript) => transcript,
        Err(error) => {
            ui::elapsed("Tearoff", elapsed);
            if error.downcast_ref::<UnknownTearoffOutcome>().is_some() {
                ui::warning(
                    "Device outcome is unknown after a missing or invalid response; inspect the tag before retrying",
                );
            }
            if stopped {
                ui::warning("Host stop requested");
            }
            if let Err(cleanup_error) = cleanup {
                return Err(format!("{error}; host cleanup failed: {cleanup_error}").into());
            }
            return Err(error);
        }
    };
    ui::print_report(&summary(
        &result.diagnostics.result,
        stopped,
        cleanup.is_ok(),
        elapsed,
    ));
    if let Some(timing) = result.timing {
        if elapsed >= TIMING_REPORT_LIMIT {
            ui::warning(
                "Device phase timings wrap during long runs; use a one-minute safety deadline for timing measurements",
            );
        } else {
            ui::print_report(&timing_report(&timing));
        }
    }
    cleanup?;
    if result.diagnostics.result.outcome != TearoffOutcome::Success
        || !result.diagnostics.result.cleanup_ok
        || stopped
    {
        return Err(if stopped {
            "Host stop requested after the device response"
        } else if !result.diagnostics.result.cleanup_ok {
            "Device radio cleanup failed"
        } else {
            "ST25TB tearoff did not reach the target"
        }
        .into());
    }
    Ok(())
}

fn timing_report(timing: &TearoffTiming) -> String {
    let us = |cycles: u32| format!("{} us", cycles / DEVICE_CYCLES_PER_US);
    let radio_work = timing
        .timeslot_cycles
        .saturating_sub(timing.grant_wait_cycles.saturating_add(timing.dma_cycles));
    let other = timing.total_cycles.saturating_sub(
        timing
            .timeslot_cycles
            .saturating_add(timing.decode_cycles)
            .saturating_add(timing.usb_cycles)
            .saturating_add(timing.pause_cycles),
    );
    ui::fields(
        "ST25TB timing (64 MHz cycles)",
        &[
            ("Total", us(timing.total_cycles)),
            ("Timeslots", us(timing.timeslot_cycles)),
            ("  Grant wait", us(timing.grant_wait_cycles)),
            ("  DMA capture", us(timing.dma_cycles)),
            ("  RF work and close", us(radio_work)),
            ("Decode", us(timing.decode_cycles)),
            ("USB transfers", us(timing.usb_cycles)),
            ("Explicit pauses", us(timing.pause_cycles)),
            ("Other and radio setup", us(other)),
            (
                "RF calls",
                format!(
                    "{} exchanges, {} writes, {} resets",
                    timing.exchanges, timing.writes, timing.resets
                ),
            ),
        ],
    )
}

pub(super) fn print_progress(event: TearoffProgress) {
    let color = ui::color_enabled() && std::io::stdout().is_terminal();
    let (line, code) = match event {
        TearoffProgress::Initial(value) => (format!("Current value: 0x{value:08X}"), HEADING_COLOR),
        TearoffProgress::Log(log) => match log.kind {
            TearoffLogKind::WriteVerified => {
                (format!("Writing:  0x{:08X} -> OK", log.value), ui::OK)
            }
            TearoffLogKind::Advanced => (
                format!(
                    "{:08X} : {:032b} : {:02} us",
                    log.value, log.value, log.delay_us
                ),
                ui::OK,
            ),
            TearoffLogKind::Candidate => (
                format!(
                    "{:08X} : {:032b} : {:02} us",
                    log.value, log.value, log.delay_us
                ),
                ui::ACCENT,
            ),
            TearoffLogKind::Lower => (
                format!(
                    "{:08X} : {:032b} : {:02} us",
                    log.value, log.value, log.delay_us
                ),
                ui::WARN,
            ),
        },
    };
    println!("{}", ui::paint(line, code, color));
    let _ = std::io::stdout().flush();
}

fn summary(result: &TearoffResult, stopped: bool, host_cleanup: bool, elapsed: Duration) -> String {
    let outcome = match result.outcome {
        TearoffOutcome::Success => "Success",
        TearoffOutcome::Cancelled => "Device cancelled",
        TearoffOutcome::DurationExpired => "Duration expired",
        TearoffOutcome::Unsupported => "Unsupported",
        TearoffOutcome::TagLost => "Tag lost",
        TearoffOutcome::TransportError => "Transport error",
    };
    ui::fields(
        "ST25TB tearoff",
        &[
            ("Device", outcome.into()),
            (
                "Last verified",
                result
                    .last_value
                    .map_or("Unknown".into(), |value| format!("{value:08X}")),
            ),
            (
                "Device cleanup",
                if result.cleanup_ok {
                    "Complete"
                } else {
                    "Failed"
                }
                .into(),
            ),
            (
                "Host cleanup",
                if host_cleanup { "Complete" } else { "Failed" }.into(),
            ),
            ("Host stop", if stopped { "Requested" } else { "No" }.into()),
            (
                "Elapsed",
                format!("{}m {:02}s", elapsed.as_secs() / 60, elapsed.as_secs() % 60),
            ),
        ],
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn terminal_fixtures_keep_device_and_host_outcomes_distinct() {
        for (outcome, label) in [
            (TearoffOutcome::Success, "Success"),
            (TearoffOutcome::Cancelled, "Device cancelled"),
            (TearoffOutcome::DurationExpired, "Duration expired"),
            (TearoffOutcome::Unsupported, "Unsupported"),
            (TearoffOutcome::TagLost, "Tag lost"),
            (TearoffOutcome::TransportError, "Transport error"),
        ] {
            let summary = summary(
                &TearoffResult {
                    outcome,
                    last_value: None,
                    cleanup_ok: false,
                },
                true,
                false,
                Duration::from_secs(125),
            );
            assert!(summary.contains(&format!("Device          {label}")));
            assert!(summary.contains("Last verified   Unknown"));
            assert!(summary.contains("Device cleanup  Failed"));
            assert!(summary.contains("Host cleanup    Failed"));
            assert!(summary.contains("Host stop       Requested"));
            assert!(!summary.contains("Memory"));
            assert!(summary.contains("Elapsed         2m 05s"));
            assert!(!summary.contains('\x1b'));
        }
        ui::disable_color();
        assert!(!ui::color_enabled());
        let summary = summary(
            &TearoffResult {
                outcome: TearoffOutcome::Success,
                last_value: Some(0xffff_fffe),
                cleanup_ok: true,
            },
            false,
            true,
            Duration::from_secs(0),
        );
        assert!(summary.contains("Last verified   FFFFFFFE"));
        assert!(summary.contains("Host stop       No"));
        assert!(!summary.contains('\x1b'));
    }
}
