//! Live presentation and JSONL export for passive NFC-A/V capture.
mod annotate;
mod display;
use chameleon_client::{Client, Result, SniffSummary};
use clap::Args;
use std::{
    fs::OpenOptions,
    io::{BufWriter, Write},
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
    time::Duration,
};

#[derive(Args)]
pub struct Options {
    /// MFRC522 receiver gain (0..7); coupling may require 6 or 7.
    #[arg(long, default_value_t = 7, value_parser = clap::value_parser!(u8).range(0..=7))]
    gain: u8,
    /// Stop after this many seconds; otherwise capture until Ctrl-C.
    #[arg(long, value_parser = clap::value_parser!(u32).range(1..=86400))]
    duration: Option<u32>,
    /// Save frames and the final summary to a new JSON Lines file.
    #[arg(short = 'o', long = "out", alias = "output", value_name = "FILE")]
    out: Option<PathBuf>,
    /// Include receiver details, bit counts and physical parity/CRC checks.
    #[arg(short, long)]
    verbose: bool,
    /// Show intervals between RX completions in microseconds (not RF frame delays).
    #[arg(long)]
    frame: bool,
}

pub fn run(
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    type_v: bool,
) -> Result<()> {
    // Reserve the output before starting RF. create_new also handles races.
    let mut file = options
        .out
        .as_ref()
        .map(|path| {
            OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path)
                .map(BufWriter::new)
        })
        .transpose()?;
    let mut client = Client::connect(port, cancelled)?;
    let mut out = std::io::stdout().lock();
    let mut display = display::Display::new(type_v, options.verbose, options.frame);
    display.start(&mut out, options.gain)?;
    out.flush()?;
    if let Some(file) = &mut file {
        serde_json::to_writer(
            &mut *file,
            &serde_json::json!({
                "type": "capture", "version": 2, "protocol": if type_v { "nfcv" } else { "nfca" },
                "mode": "passive_dual", "gain": options.gain, "timestamp": "device_rx_end_us",
                "clock_hz": 1000000
            }),
        )?;
        writeln!(file)?;
        file.flush()?;
    }
    let mut presentation = annotate::Presentation::new(type_v);
    let mut summary = SniffSummary::default();
    let mut emit = |batch: &chameleon_client::SniffBatch| {
        summary = batch.summary;
        presentation.observe_losses(&batch.summary, batch.frames.is_empty());
        for frame in &batch.frames {
            let analysis = presentation.analyze(frame);
            display.frame(&mut out, frame, &analysis)?;
            let annotation = analysis.annotation;
            if let Some(file) = &mut file {
                serde_json::to_writer(
                    &mut *file,
                    &serde_json::json!({
                        "type": "frame", "direction": if frame.direction == 0 { "reader_to_tag" } else { "tag_to_reader" }, "frame": frame,
                        "annotation": annotation
                    }),
                )?;
                writeln!(file)?;
            }
        }
        // Publish each batch immediately, including when stdout is a pipe.
        out.flush()?;
        if let Some(file) = &mut file {
            file.flush()?;
        }
        Ok(())
    };
    let duration = options.duration.map(|s| Duration::from_secs(u64::from(s)));
    let result = if type_v {
        client.sniff_nfcv(options.gain, duration, &mut emit)
    } else {
        client.sniff_nfca(options.gain, duration, &mut emit)
    };
    display.summary(&mut out, &summary)?;
    out.flush()?;
    if let Some(file) = &mut file {
        serde_json::to_writer(
            &mut *file,
            &serde_json::json!({
                "type": "summary", "summary": summary,
                "error": result.as_ref().err().map(ToString::to_string)
            }),
        )?;
        writeln!(file)?;
        file.flush()?;
    }
    result.map(|_| ())
}
