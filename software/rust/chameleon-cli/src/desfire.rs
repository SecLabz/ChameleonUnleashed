//! DESFire uses the existing dump, file and persistent-slot workflows.
use crate::{progress::Progress, slot, ui};
use chameleon_client::{Client, DesfireSnapshot, Result};
use clap::Subcommand;
use nfc_native::formats::{
    desfire::{Capture, ReadState},
    flipper,
    image::Model,
};
use std::{
    io::{IsTerminal, Write},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

#[derive(Subcommand)]
pub enum Command {
    /// Show activation, version and public card settings.
    Info,
    /// Read public applications/files; retain denied and unread observations.
    Dump {
        /// Output file; existing files are never overwritten.
        #[arg(long)]
        output: Option<PathBuf>,
        /// Persist and verify the capture in one HF slot without starting it.
        #[arg(long,value_parser=clap::value_parser!(u8).range(1..=8))]
        slot: Option<u8>,
        /// Name of the stored image (1–32 UTF-8 bytes).
        #[arg(long, requires = "slot")]
        name: Option<String>,
        /// Confirm replacement of an existing HF image.
        #[arg(long, requires = "slot")]
        yes: bool,
    },
}
pub fn run(
    command: Command,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    allowed: bool,
) -> Result<()> {
    let (info, output, number, name, yes) = match command {
        Command::Info => (true, None, None, None, false),
        Command::Dump {
            output,
            slot,
            name,
            yes,
        } => (false, output, slot, name, yes),
    };
    if let Some(path) = &output {
        if !matches!(
            path.extension().and_then(|s| s.to_str()),
            Some("json" | "nfc")
        ) {
            return Err("Dump output must end with .json or .nfc".into());
        }
        if path.exists() {
            return Err("Output already exists".into());
        }
    }
    if let Some(name) = &name {
        chameleon_client::validate_name(name)?;
    }
    let mut client = Client::connect(port, cancelled.clone())?;
    let operation = |client: &mut Client| -> Result<()> {
        let started = Instant::now();
        let progress = Progress::new("Reading DESFire", None, false);
        let snapshot = client.desfire_capture(info, &mut |_| {})?;
        drop(progress);
        print!(
            "{}",
            capture_text(&snapshot, info, started.elapsed(), ui::stdout_color())?
        );
        if let Some(path) = output {
            let bytes = if path.extension().is_some_and(|s| s == "json") {
                let mut data = serde_json::to_vec_pretty(&snapshot)?;
                data.push(b'\n');
                data
            } else {
                if !snapshot.complete {
                    return Err("Incomplete DESFire read; use JSON to retain observations".into());
                }
                ui::warning(
                    "Other tools may ignore public-capture extensions; use JSON for faithful reload",
                );
                flipper::export(&snapshot.image()?)?.into_bytes()
            };
            std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)?
                .write_all(&bytes)?;
            ui::line("Saved", &[path.display().to_string()]);
        }
        if !snapshot.complete || cancelled.load(Ordering::Relaxed) {
            return Err(snapshot
                .error
                .unwrap_or_else(|| {
                    "Read stopped; acquired observations retained; slot unchanged".into()
                })
                .into());
        }
        if let Some(number) = number {
            let image = snapshot.slot_image()?; // Capacity is checked before any slot mutation.
            let slots = client.slots()?;
            let allowed = allowed
                && std::io::stdin().is_terminal()
                && std::io::stdout().is_terminal()
                && std::io::stderr().is_terminal()
                && std::env::var("TERM").is_ok_and(|t| t != "dumb");
            slot::save_image(
                client,
                &slots,
                number,
                &image,
                name.as_deref(),
                yes,
                allowed,
            )?;
        }
        Ok(())
    };
    if number.is_some() {
        client.with_reader_mode(operation)
    } else {
        operation(&mut client)
    }
}
pub(crate) fn state_text(state: ReadState) -> &'static str {
    match state {
        ReadState::Present => "read",
        ReadState::Denied => "denied",
        ReadState::Unsupported => "unsupported",
        ReadState::NotRead => "not read",
    }
}

fn capture_text(
    snapshot: &DesfireSnapshot,
    details: bool,
    elapsed: Duration,
    color: bool,
) -> Result<String> {
    use std::fmt::Write;
    let image = snapshot.image()?;
    let uid = ui::paint(ui::uid_text(&image.uid), "95", color);
    if image.model != Model::Desfire {
        return Ok(format!(
            "{} · UID {uid}\n! Identity only · {:.2} s\n",
            ui::paint("MIFARE DESFire", "1;36", color),
            elapsed.as_secs_f64()
        ));
    }
    let c = Capture::from_image(&image)?;
    let generation = match c.version[3] {
        0x12 => " EV2",
        0x01 => " EV1",
        0x33 => " EV3",
        _ => "",
    };
    let mut text = format!(
        "{} · UID {uid}\nATQA (RF) {} · SAK {:02X}",
        ui::paint(format!("MIFARE DESFire{generation}"), "1;36", color),
        flipper::hex(&c.atqa),
        c.sak
    );
    if !c.ats.is_empty() {
        write!(text, " · ATS {}", flipper::hex(&c.ats)).unwrap();
    }
    text.push('\n');
    if details {
        text.push_str("Version\n");
        for (index, bytes) in c.version.chunks(16).enumerate() {
            writeln!(
                text,
                "{}",
                ui::hex_row(
                    &format!("{:04X}", index * 16),
                    &bytes.iter().copied().map(Some).collect::<Vec<_>>(),
                    16,
                    "Config",
                    color
                )
            )
            .unwrap();
        }
    }
    let free = c.free_memory.map_or("free memory unavailable".into(), |n| {
        format!("{n} bytes free")
    });
    writeln!(
        text,
        "{} {} · {} applications ({}) · {free} · {:.2} s",
        ui::paint(
            if snapshot.complete { "✓" } else { "!" },
            if snapshot.complete { ui::OK } else { ui::WARN },
            color
        ),
        if snapshot.complete {
            "Public read"
        } else {
            "Partial public read"
        },
        c.applications.len(),
        state_text(c.applications_state),
        elapsed.as_secs_f64()
    )
    .unwrap();
    text.push_str("Public data only; no authentication or secure messaging.\n");
    text.push_str(&files_text(&c, color));
    Ok(text)
}

pub(crate) fn files_text(capture: &Capture, color: bool) -> String {
    use std::fmt::Write;

    let mut text = String::new();
    for app in &capture.applications {
        writeln!(
            text,
            "\n{} · settings {} · files {}",
            ui::paint(
                format!("Application {}", ui::uid_text(&app.aid)),
                "1;36",
                color
            ),
            state_text(app.settings_state),
            state_text(app.files_state)
        )
        .unwrap();
        for file in &app.files {
            let status = format!("data {}", state_text(file.data_state));
            writeln!(
                text,
                "  File {:02X} · settings {} · {} · {} bytes captured",
                file.id,
                state_text(file.settings_state),
                ui::paint(
                    status,
                    if file.data_state == ReadState::Present {
                        ui::OK
                    } else {
                        ui::WARN
                    },
                    color
                ),
                file.data.len()
            )
            .unwrap();
            for (offset, bytes) in file.data.chunks(16).enumerate() {
                writeln!(
                    text,
                    "  {}",
                    ui::hex_row(
                        &format!("{:04X}", offset * 16),
                        &bytes.iter().copied().map(Some).collect::<Vec<_>>(),
                        16,
                        "",
                        color
                    )
                )
                .unwrap();
            }
        }
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn public_dump_retains_denials_and_aligns_short_file_lines_without_inventing_bytes() {
        let snapshot: DesfireSnapshot = serde_json::from_str(include_str!(
            "../../../../resource/tools/tests/desfire-public.json"
        ))
        .unwrap();
        let mut capture = Capture::from_image(&snapshot.image().unwrap()).unwrap();
        capture.applications[0].files[0].data = vec![b'|'; 19];
        capture.applications[0].files[0].settings[4..7].copy_from_slice(&[19, 0, 0]);
        let snapshot = DesfireSnapshot::from_image(&capture.image().unwrap()).unwrap();
        let elapsed = Duration::from_millis(250);
        let plain = capture_text(&snapshot, false, elapsed, false).unwrap();
        let colored = capture_text(&snapshot, false, elapsed, true).unwrap();
        assert_eq!(ui::without_colors(&colored), plain);
        assert!(plain.contains("Application 335848 · settings read · files read"));
        assert!(plain.contains("data denied · 0 bytes captured"));
        assert!(plain.contains("data read · 19 bytes captured"));
        let first_file = plain.split("  File ").nth(1).unwrap();
        let rows: Vec<_> = first_file
            .lines()
            .filter(|line| line.starts_with("    00"))
            .collect();
        assert_eq!(rows.len(), 2);
        for line in rows {
            assert_eq!(line.find('|'), Some(62), "{line}");
            assert_eq!(line.len(), 80);
        }
        assert!(!plain.contains("Version\n"));
        assert!(
            capture_text(&snapshot, true, elapsed, false)
                .unwrap()
                .contains("Version\n")
        );
    }
}
