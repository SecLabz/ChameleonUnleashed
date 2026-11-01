//! Discovery and bounded memory dumps, using the existing console presentation.
use crate::{progress::Progress, ui};
use chameleon_client::{Client, HfReader, HfSnapshot, HfTag, Result};
use clap::{Args, Subcommand};
use std::{
    io::Write,
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
    time::Instant,
};

#[derive(Args)]
pub struct Options {
    #[command(subcommand)]
    command: MemoryCommand,
}

#[derive(Args, Default)]
pub struct Authentication {
    /// Four-byte known SLIX-L privacy password in RF order, hexadecimal.
    #[arg(long, value_name = "HEX")]
    password: Option<String>,
}

#[derive(Subcommand)]
pub enum MemoryCommand {
    /// Identify the tag and readable memory geometry.
    Info(Authentication),
    /// Read one four-byte user block.
    Read {
        #[command(flatten)]
        authentication: Authentication,
        /// Memory block index, starting at zero.
        #[arg(long, value_parser = clap::value_parser!(u16).range(0..=255))]
        block: u16,
    },
    /// Read the complete supported tag in one firmware command.
    Dump {
        #[command(flatten)]
        authentication: Authentication,
        /// Save a lossless JSON capture, or a complete .nfc image.
        #[arg(long)]
        output: Option<PathBuf>,
        /// Explicitly replace unknown security metadata for emulator export.
        #[arg(long)]
        emulator_defaults: bool,
    },
    /// Capture NFC-V reader and tag frames passively with live output.
    Sniff(crate::sniff::Options),
    /// Exchange a bounded NFC-V sequence with automatic CRC.
    Raw(crate::raw::VicinityOptions),
    /// Write and verify one unlocked four-byte user block.
    Write {
        #[command(flatten)]
        authentication: Authentication,
        /// Memory block index, starting at zero.
        #[arg(long, value_parser = clap::value_parser!(u8))]
        block: u8,
        /// Four data bytes in hexadecimal.
        data: String,
        /// Preview changes without writing.
        #[arg(long)]
        dry_run: bool,
        /// Confirm the prepared operation without prompting.
        #[arg(long)]
        yes: bool,
    },
    /// Restore user blocks from a complete NFC-V JSON or Flipper capture.
    Restore {
        #[command(flatten)]
        authentication: Authentication,
        /// Input capture or image file.
        file: PathBuf,
        /// Preview changes without writing.
        #[arg(long)]
        dry_run: bool,
        /// Confirm the prepared operation without prompting.
        #[arg(long)]
        yes: bool,
    },
}

pub fn search(port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let tag = reader.search()?;
    reader.finish()?;
    heading(&tag);
    Ok(())
}

pub fn run(
    family: &str,
    command: MemoryCommand,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
) -> Result<()> {
    run_inner(family, command, None, port, cancelled, false)
}

pub fn run_options(
    family: &str,
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    let authentication = match &options.command {
        MemoryCommand::Info(authentication)
        | MemoryCommand::Read { authentication, .. }
        | MemoryCommand::Dump { authentication, .. }
        | MemoryCommand::Write { authentication, .. }
        | MemoryCommand::Restore { authentication, .. } => Some(authentication),
        MemoryCommand::Raw(_) | MemoryCommand::Sniff(_) => None,
    };
    let password = authentication
        .and_then(|options| options.password.as_ref())
        .map(|value| crate::uid::hex_bytes(value, &[4]).map(|bytes| bytes.try_into().unwrap()))
        .transpose()?;
    if password.is_some() && family == "slix" {
        return Err("Privacy passwords require SLIX-L; use hf slix-l".into());
    }
    run_inner(
        family,
        options.command,
        password,
        port,
        cancelled,
        prompt_allowed,
    )
}

fn run_inner(
    family: &str,
    command: MemoryCommand,
    password: Option<[u8; 4]>,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    if let MemoryCommand::Raw(options) = command {
        return crate::raw::run(options.into(), true, port, cancelled);
    }
    if let MemoryCommand::Sniff(options) = command {
        return crate::sniff::run(options, port, cancelled, true);
    }
    if let MemoryCommand::Dump {
        output: Some(path), ..
    } = &command
    {
        if !matches!(
            path.extension().and_then(|value| value.to_str()),
            Some("json" | "nfc")
        ) {
            return Err("Dump output must end with .json or .nfc".into());
        }
        if path.exists() {
            return Err("Output already exists".into());
        }
    }
    let write_data: Option<[u8; 4]> = if let MemoryCommand::Write { data, .. } = &command {
        Some(crate::uid::hex_bytes(data, &[4])?.try_into().unwrap())
    } else {
        None
    };
    let restore_source = if let MemoryCommand::Restore { file, .. } = &command {
        Some(load_nfcv(file)?)
    } else {
        None
    };
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let tag = match family {
        "b" => reader.nfcb()?,
        "st25tb" => reader.st25tb()?,
        "slix" => reader.slix(password, false)?,
        "slix-l" => reader.slix(password, true)?,
        _ if password.is_some() => reader.slix(password, true)?,
        _ => reader.nfcv()?,
    };
    match command {
        MemoryCommand::Sniff(_) | MemoryCommand::Raw(_) => unreachable!(),
        MemoryCommand::Write {
            block,
            dry_run,
            yes,
            ..
        } => {
            let before = reader.dump(&tag)?;
            if !before.complete || before.locks.get(usize::from(block)) != Some(&Some(false)) {
                return Err("A complete read and an unlocked target block are required".into());
            }
            let original = before.memory[usize::from(block)].ok_or("Target block is unread")?;
            let after = write_data.unwrap();
            heading(&tag);
            ui::line("Write block", &[block.to_string()]);
            ui::change(format!("{block:03}"), hex(&original), hex(&after));
            if original == after {
                ui::no_write("Unchanged");
            } else if dry_run {
                ui::no_write("Preview");
            } else if ui::confirm(
                &format!("Write 1 block to tag {}?", ui::uid_text(tag.uid())),
                yes,
                prompt_allowed,
            )? {
                reader.write_nfcv(&tag, block, original, after)?;
                ui::result(true, "Block written and verified");
            }
            reader.finish()?;
        }
        MemoryCommand::Restore { dry_run, yes, .. } => {
            let (uid, source) = restore_source.unwrap();
            if tag.uid() != uid {
                return Err("Target UID differs from the source capture".into());
            }
            let before = reader.dump(&tag)?;
            if !before.complete || before.memory.len() != source.len() {
                return Err("A complete matching target read is required".into());
            }
            let writes: Vec<_> = source
                .iter()
                .enumerate()
                .filter(|(index, bytes)| before.memory[*index] != Some(**bytes))
                .collect();
            if writes
                .iter()
                .any(|(index, _)| before.locks[*index] != Some(false))
            {
                return Err("A differing block is locked or its lock state is unknown".into());
            }
            heading(&tag);
            ui::report(
                "Restore user blocks",
                &[
                    ("To write", writes.len().to_string()),
                    ("Preserved", "UID, passwords, locks, AFI and DSFID".into()),
                ],
            );
            for (index, after) in &writes {
                ui::change(
                    format!("{index:03}"),
                    hex(&before.memory[*index].unwrap()),
                    hex(&after[..]),
                );
            }
            if writes.is_empty() {
                ui::no_write("Unchanged");
            } else if dry_run {
                ui::no_write("Preview");
            } else if ui::confirm(
                &format!(
                    "Restore {} block{} to tag {}?",
                    writes.len(),
                    if writes.len() == 1 { "" } else { "s" },
                    ui::uid_text(tag.uid())
                ),
                yes,
                prompt_allowed,
            )? {
                for (done, (index, after)) in writes.iter().enumerate() {
                    reader
                        .write_nfcv(&tag, *index as u8, before.memory[*index].unwrap(), **after)
                        .map_err(|error| {
                            format!("{done}/{} blocks verified. {error}", writes.len())
                        })?;
                }
                ui::result(
                    true,
                    format!(
                        "{} block{} written and verified",
                        writes.len(),
                        if writes.len() == 1 { "" } else { "s" }
                    ),
                );
            }
            reader.finish()?;
        }
        MemoryCommand::Info(_) => {
            reader.finish()?;
            heading(&tag);
        }
        MemoryCommand::Read { block, .. } => {
            let bytes = reader.read(&tag, block)?;
            reader.finish()?;
            heading(&tag);
            if family == "st25tb" {
                ui::line("Memory", &["32-bit words, MSB first".into()]);
                let role = if block == 255 {
                    "sysOTP"
                } else if tag.uid()[..3] == [0xd0, 2, 0x33] && matches!(block, 5 | 6) {
                    "Counter"
                } else {
                    ""
                };
                println!(
                    "{}",
                    ui::word_row(
                        &format!("{block:03}"),
                        Some(u32::from_le_bytes(bytes)),
                        role,
                        ui::stdout_color()
                    )
                );
            } else {
                println!(
                    "{}",
                    ui::hex_row(
                        &format!("{block:03}"),
                        &bytes.into_iter().map(Some).collect::<Vec<_>>(),
                        4,
                        "",
                        ui::stdout_color()
                    )
                );
            }
        }
        MemoryCommand::Dump {
            output,
            emulator_defaults,
            ..
        } => {
            let started = Instant::now();
            let progress = Progress::new("Reading tag", None, false);
            let snapshot = match reader.dump(&tag) {
                Ok(snapshot) => snapshot,
                Err(error) => {
                    drop(progress);
                    heading(&tag);
                    return Err(error);
                }
            };
            let stopped = reader.cancelled();
            let cleanup = reader.finish();
            drop(progress);
            heading(&tag);
            ui::result(
                snapshot.complete && !stopped && cleanup.is_ok(),
                format!(
                    "{} · {}/{} blocks · {:.2} s",
                    if stopped {
                        "Interrupted"
                    } else if snapshot.deadline {
                        "Deadline"
                    } else if snapshot.complete {
                        "Complete"
                    } else {
                        "Partial"
                    },
                    snapshot.memory.iter().flatten().count(),
                    snapshot.memory.len(),
                    started.elapsed().as_secs_f64()
                ),
            );
            print!("{}", memory_text(&snapshot, ui::stdout_color()));
            if let Some(path) = output {
                let bytes = if path
                    .extension()
                    .is_some_and(|extension| extension == "json")
                {
                    let mut bytes = serde_json::to_vec_pretty(&snapshot)?;
                    bytes.push(b'\n');
                    bytes
                } else {
                    export_nfc(&snapshot, emulator_defaults)?.into_bytes()
                };
                std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&path)?
                    .write_all(&bytes)?;
                ui::line("Saved", &[path.display().to_string()]);
            }
            cleanup?;
            if stopped {
                return Err("Stopped; the in-flight dump response was retained".into());
            }
            if !snapshot.complete {
                return Err("Read incomplete; acquired blocks were retained".into());
            }
        }
    }
    Ok(())
}

fn load_nfcv(path: &std::path::Path) -> Result<(Vec<u8>, Vec<[u8; 4]>)> {
    use std::io::Read;
    let mut bytes = Vec::new();
    std::fs::File::open(path)?
        .take(128 * 1024 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > 128 * 1024 {
        return Err("Capture exceeds 128 KiB".into());
    }
    if path
        .extension()
        .is_some_and(|extension| extension == "json")
    {
        let value: serde_json::Value = serde_json::from_slice(&bytes)?;
        if let Some(document) = value.get("document") {
            if value["version"] != 1
                || document["kind"] != "nfcv"
                || document["unitBytes"] != 4
                || document["card"]["protocol"] != "nfc_v"
                || document["card"]["block_size"] != 4
            {
                return Err("Expected a supported NFC-V browser document".into());
            }
            let uid: [u8; 8] = serde_json::from_value(document["card"]["uid"].clone())?;
            let memory: Vec<Option<[u8; 4]>> = serde_json::from_value(document["memory"].clone())?;
            if memory.is_empty()
                || memory.len() > 64
                || document["card"]["blocks"].as_u64() != Some(memory.len() as u64)
            {
                return Err("NFC-V source geometry is inconsistent".into());
            }
            return Ok((
                uid.to_vec(),
                memory
                    .into_iter()
                    .collect::<Option<Vec<_>>>()
                    .ok_or("Source contains unread bytes")?,
            ));
        }
        let snapshot: HfSnapshot = serde_json::from_value(value)?;
        if snapshot.version != 1
            || snapshot.deadline
            || snapshot.status != 0
            || snapshot.tag_error.is_some()
            || snapshot.system.is_some()
            || snapshot.chip_id.is_some()
            || snapshot.locks.len() != snapshot.memory.len()
            || snapshot.protocol != "nfc_v"
            || !snapshot.complete
            || snapshot.memory.is_empty()
            || snapshot.memory.len() > 64
        {
            return Err("Expected a complete NFC-V capture with up to 64 blocks".into());
        }
        return Ok((
            snapshot.uid.to_vec(),
            snapshot
                .memory
                .into_iter()
                .collect::<Option<Vec<_>>>()
                .ok_or("Source contains unread blocks")?,
        ));
    }
    if path.extension().is_none_or(|extension| extension != "nfc") {
        return Err("Choose a .json or .nfc capture".into());
    }
    let (image, warnings) =
        nfc_native::formats::flipper::parse_with_warnings(std::str::from_utf8(&bytes)?)?;
    if !matches!(
        image.model,
        nfc_native::formats::image::Model::Slix
            | nfc_native::formats::image::Model::SlixL
            | nfc_native::formats::image::Model::Iso15693
    ) || image.uid.len() != 8
        || image.memory.is_empty()
        || image.memory.len() % 4 != 0
        || image.memory.len() > 256
    {
        return Err("Expected a supported NFC-V memory image".into());
    }
    for warning in warnings {
        ui::warning(warning);
    }
    Ok((
        image.uid,
        image
            .memory
            .chunks_exact(4)
            .map(|bytes| bytes.try_into().unwrap())
            .collect(),
    ))
}

fn hex(bytes: &[u8]) -> String {
    nfc_native::formats::flipper::hex(bytes)
}

pub fn info(tag: &HfTag) {
    if let HfTag::NfcA(card) = tag {
        ui::card(card, None);
        return;
    }
    ui::report(tag.name(), &tag_fields(tag));
}

pub fn heading(tag: &HfTag) {
    if let HfTag::NfcA(card) = tag {
        ui::card(card, None);
        return;
    }
    let fields = tag_fields(tag);
    let mut identity = vec![format!(
        "{} {}",
        fields[0].0,
        ui::paint(ui::uid_text(tag.uid()), "95", ui::stdout_color())
    )];
    let mut details = Vec::new();
    for (name, value) in fields.iter().skip(1) {
        if *name == "Memory" {
            identity.push(value.clone());
        } else {
            details.push(format!("{name} {value}"));
        }
    }
    ui::line(tag.name(), &identity);
    if !details.is_empty() {
        ui::line("", &details);
    }
}

fn tag_fields(tag: &HfTag) -> Vec<(&'static str, String)> {
    let mut fields = vec![(
        if matches!(tag, HfTag::NfcB { .. }) {
            "PUPI"
        } else {
            "UID"
        },
        hex(tag.uid()),
    )];
    match tag {
        HfTag::NfcB {
            application,
            protocol,
            ..
        } => {
            fields.push(("Application", hex(application)));
            fields.push(("Protocol", hex(protocol)));
            fields.push((
                "Memory",
                "Application-specific; reading is unsupported".into(),
            ));
        }
        HfTag::St25tb {
            chip_id, blocks, ..
        } => {
            fields.push(("Chip ID", format!("{chip_id:02X}")));
            fields.push((
                "Memory",
                if *blocks == 0 {
                    "Unsupported geometry".into()
                } else {
                    format!("{blocks} × 4 bytes + system block")
                },
            ));
        }
        HfTag::NfcV {
            dsfid,
            afi,
            blocks,
            block_size,
            ic_ref,
            ..
        } => {
            fields.push(("DSFID", format!("{dsfid:02X}")));
            if let Some(afi) = afi {
                fields.push(("AFI", format!("{afi:02X}")));
            }
            if let Some(ic_ref) = ic_ref {
                fields.push(("IC reference", format!("{ic_ref:02X}")));
            }
            fields.push((
                "Memory",
                match (blocks, block_size) {
                    (Some(count), Some(width)) => format!("{count} × {width} bytes"),
                    _ => "Unknown geometry".into(),
                },
            ));
        }
        _ => (),
    }
    fields
}

fn memory_text(snapshot: &HfSnapshot, color: bool) -> String {
    let words = snapshot.protocol == "st25tb";
    let mut text = String::new();
    if words {
        text.push_str("32-bit words · MSB first\n");
    } else {
        let locked = snapshot
            .locks
            .iter()
            .filter(|state| **state == Some(true))
            .count();
        let unlocked = snapshot
            .locks
            .iter()
            .filter(|state| **state == Some(false))
            .count();
        text.push_str(&format!(
            "Locks · {locked} locked · {unlocked} unlocked · {} unknown\n",
            snapshot.locks.len() - locked - unlocked
        ));
    }
    if snapshot.status != 0 {
        text.push_str(&format!("Device status {:02X}\n", snapshot.status));
    }
    if let Some(error) = snapshot.tag_error {
        text.push_str(&format!("Tag error {error:02X}\n"));
    }
    text.push('\n');
    let uniform_locks = snapshot.locks.windows(2).all(|pair| pair[0] == pair[1]);
    for (index, block) in snapshot.memory.iter().enumerate() {
        if words {
            let role = if snapshot.uid[..3] == [0xd0, 2, 0x33] && matches!(index, 5 | 6) {
                "Counter"
            } else {
                ""
            };
            text.push_str(&ui::word_row(
                &format!("{index:03}"),
                block.map(u32::from_le_bytes),
                role,
                color,
            ));
        } else {
            let bytes = block.map_or(vec![None; 4], |bytes| bytes.into_iter().map(Some).collect());
            text.push_str(&ui::hex_row(&format!("{index:03}"), &bytes, 4, "", color));
            if !uniform_locks {
                match snapshot.locks[index] {
                    Some(true) => {
                        text.push_str(&format!("  {}", ui::paint("locked", ui::WARN, color)))
                    }
                    None => text.push_str("  lock unknown"),
                    _ => {}
                }
            }
        }
        text.push('\n');
    }
    if words {
        text.push_str(&ui::word_row(
            "SYS",
            snapshot.system.map(u32::from_le_bytes),
            "sysOTP",
            color,
        ));
        text.push('\n');
    }
    if snapshot.memory.iter().any(Option::is_none) || words && snapshot.system.is_none() {
        text.push_str("\n?? = unread or unavailable bytes.\n");
    }
    text
}

fn export_nfc(snapshot: &HfSnapshot, defaults: bool) -> Result<String> {
    use nfc_native::formats::image::{Image, Model, layout};
    if !snapshot.complete || snapshot.memory.iter().any(Option::is_none) {
        return Err("Complete .nfc export requires every block; use .json for partial data".into());
    }
    if snapshot.protocol == "st25tb" {
        if snapshot.uid[..3] != [0xd0, 2, 0x33] {
            return Err(
                "Only ST25TB512-AT .nfc export is supported; use .json for this tag".into(),
            );
        }
        let system = snapshot.system.ok_or("ST25TB system block is unknown")?;
        let image = Image {
            model: Model::St25tb512At,
            uid: snapshot.uid.to_vec(),
            memory: snapshot
                .memory
                .iter()
                .flatten()
                .flatten()
                .copied()
                .collect(),
            metadata: system.to_vec(),
            secrets: Vec::new(),
        };
        return nfc_native::formats::flipper::export(&image);
    }
    if !defaults {
        return Err(
            "Security metadata remains unknown; use --emulator-defaults explicitly, or save .json"
                .into(),
        );
    }
    let model = match (
        snapshot.uid[..3].try_into().unwrap_or([0; 3]),
        snapshot.memory.len(),
    ) {
        ([0xe0, 4, 1], 28) => Model::Slix,
        ([0xe0, 4, 3], 8) => Model::SlixL,
        (_, 64) => Model::Iso15693,
        _ => return Err("No lossless .nfc geometry for this tag; use .json".into()),
    };
    let mut metadata = b"VIC1".to_vec();
    metadata.extend([
        0,
        snapshot.dsfid.unwrap_or(0),
        snapshot.afi.unwrap_or(0),
        snapshot.ic_ref.unwrap_or(0),
        4,
        snapshot.memory.len() as u8,
        0,
        0,
    ]);
    metadata.extend(
        snapshot
            .locks
            .iter()
            .map(|lock| u8::from(lock.unwrap_or(false))),
    );
    let secrets = vec![
        0;
        match model {
            Model::Slix => layout::VIC_PASSWORD_BYTES,
            Model::SlixL => 3 * layout::VIC_PASSWORD_BYTES,
            _ => 0,
        }
    ];
    let image = Image {
        model,
        uid: snapshot.uid.to_vec(),
        memory: snapshot
            .memory
            .iter()
            .flatten()
            .flatten()
            .copied()
            .collect(),
        metadata,
        secrets,
    };
    ui::warning(
        "Unknown security metadata and credentials use emulator defaults; original capture is unchanged",
    );
    nfc_native::formats::flipper::export(&image)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn memory_keeps_rf_bytes_and_lock_states_separate_from_st25_words() {
        let mut snapshot = HfSnapshot {
            version: 1,
            protocol: "nfc_v".into(),
            uid: [0xe0, 4, 3, 1, 2, 3, 4, 5],
            memory: vec![Some([0x41, 0, 0x42, 0x43]), None, Some([0; 4])],
            locks: vec![Some(true), Some(false), None],
            dsfid: None,
            afi: None,
            ic_ref: None,
            chip_id: None,
            system: None,
            complete: false,
            deadline: false,
            status: 0,
            tag_error: None,
        };
        let plain = memory_text(&snapshot, false);
        assert!(plain.contains("Locks · 1 locked · 1 unlocked · 1 unknown\n"));
        assert!(plain.contains("  000  41 00 42 43  |A.BC|  locked\n"));
        assert!(plain.contains("  001  ?? ?? ?? ??  |????|\n"));
        assert!(plain.contains("  002  00 00 00 00  |....|  lock unknown\n"));
        assert_eq!(ui::without_colors(&memory_text(&snapshot, true)), plain);
        snapshot.locks.fill(None);
        assert!(!memory_text(&snapshot, false).contains("lock unknown"));
        snapshot.protocol = "st25tb".into();
        snapshot.uid[..3].copy_from_slice(&[0xd0, 2, 0x33]);
        snapshot.memory.resize(16, Some([0; 4]));
        snapshot.memory[5] = Some([0xfe, 0xff, 0xff, 0xff]);
        let plain = memory_text(&snapshot, false);
        assert!(plain.contains("32-bit words · MSB first\n"));
        assert!(plain.contains("  005  FFFFFFFE  Counter\n"));
        assert!(plain.contains("  SYS  ????????  sysOTP\n"));
        assert_eq!(snapshot.memory[5], Some([0xfe, 0xff, 0xff, 0xff]));
        assert_eq!(ui::without_colors(&memory_text(&snapshot, true)), plain);
        snapshot.uid[2] = 0x1b;
        assert!(!memory_text(&snapshot, false).contains("Counter"));
    }

    #[test]
    fn sri512_export_requires_json_and_preserves_the_capture() {
        let snapshot = HfSnapshot {
            version: 1,
            protocol: "st25tb".into(),
            uid: [0xd0, 2, 0x1b, 1, 2, 3, 4, 5],
            memory: vec![Some([0; 4]); 16],
            locks: vec![None; 16],
            dsfid: None,
            afi: None,
            ic_ref: None,
            chip_id: Some(0x5a),
            system: Some([255; 4]),
            complete: true,
            deadline: false,
            status: 0,
            tag_error: None,
        };
        assert!(
            export_nfc(&snapshot, false)
                .unwrap_err()
                .to_string()
                .contains("use .json")
        );
        let restored: HfSnapshot =
            serde_json::from_str(&serde_json::to_string(&snapshot).unwrap()).unwrap();
        assert_eq!(restored.uid, snapshot.uid);
        assert_eq!(restored.memory, snapshot.memory);
        assert_eq!(restored.system, snapshot.system);
        let mut at = snapshot;
        at.uid[2] = 0x33;
        assert!(
            export_nfc(&at, false)
                .unwrap()
                .contains("ST25TB Type: 512AT")
        );
    }
}
