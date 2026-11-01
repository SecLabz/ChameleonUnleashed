//! Type 2 inspection, lossless captures and verified user-page writes.
use crate::{restore, ui};
use chameleon_client::{Client, DeviceError, Reader, Result, TagCredential};
use clap::{Args, Subcommand};
use nfc_native::formats::{image::Model, physical};
use std::{
    io::Write,
    path::{Path, PathBuf},
    sync::{Arc, atomic::AtomicBool},
    time::Instant,
};

#[derive(Args)]
pub struct Target {
    /// Required when GET_VERSION cannot identify original Ultralight or Ultralight C.
    #[arg(long, value_enum)]
    model: Option<restore::Type2Model>,
    /// Four-byte NTAG/Ultralight password in hexadecimal.
    #[arg(long, value_name = "HEX", conflicts_with = "key")]
    password: Option<String>,
    /// Ultralight C 3DES key, in cipher key order.
    #[arg(long, value_name = "HEX")]
    key: Option<String>,
}

#[derive(Subcommand)]
pub enum Command {
    /// Identify the Type 2 model and page geometry.
    Info {
        /// Tag model used to interpret its memory geometry.
        #[arg(long, value_enum)]
        model: Option<restore::Type2Model>,
    },
    /// Read pages; unknown credentials are never returned as zeroes.
    Read {
        #[command(flatten)]
        target: Target,
        /// Memory page index, starting at zero.
        #[arg(long, value_parser = clap::value_parser!(u8))]
        page: u8,
        /// Number of consecutive pages to read.
        #[arg(long, default_value_t = 1, value_parser = clap::value_parser!(u16).range(1..=231))]
        count: u16,
    },
    /// Save a lossless JSON capture, retaining completed pages on interruption.
    Dump {
        #[command(flatten)]
        target: Target,
        /// Output file; existing files are never overwritten.
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Read and optionally save the NFC Forum NDEF message.
    Ndef {
        #[command(flatten)]
        target: Target,
        /// Output file; existing files are never overwritten.
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Change one user page after checking its current bytes and locks.
    Write {
        #[command(flatten)]
        target: Target,
        /// Memory page index, starting at zero.
        #[arg(long)]
        page: u8,
        /// Four data bytes in hexadecimal.
        data: String,
        /// Preview changes without writing.
        #[arg(long)]
        dry_run: bool,
        /// Confirm the prepared operation without prompting.
        #[arg(long)]
        yes: bool,
    },
    /// Restore user data to a physical Type 2 tag.
    Restore(restore::Type2Options),
}

fn save(path: &Path, bytes: &[u8]) -> Result<()> {
    std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)?
        .write_all(bytes)?;
    ui::line("Saved", &[path.display().to_string()]);
    Ok(())
}

fn credential(target: &Target) -> Result<TagCredential> {
    if let Some(value) = &target.password {
        return Ok(TagCredential::Password(
            crate::uid::hex_bytes(value, &[4])?.try_into().unwrap(),
        ));
    }
    if let Some(value) = &target.key {
        return Ok(TagCredential::UltralightC(
            crate::uid::hex_bytes(value, &[16])?.try_into().unwrap(),
        ));
    }
    Ok(TagCredential::None)
}

fn capture(
    reader: &mut Reader<'_>,
    model: Model,
    credential: TagCredential,
) -> (Vec<Option<Vec<u8>>>, Option<String>) {
    let profile = model.type2_profile().unwrap();
    let _progress = crate::progress::Progress::new("Reading pages", None, false);
    let mut memory = vec![None; profile.pages];
    for page in 0..profile.credential_start {
        match reader.type2_page(model, page as u8, credential) {
            Ok(bytes) => memory[page] = Some(bytes.to_vec()),
            Err(error)
                if error
                    .downcast_ref::<DeviceError>()
                    .is_some_and(|error| matches!(error.0, 2 | 3 | 6 | 7)) =>
            {
                // A supplied credential is tried once; AUTHLIM can permanently lock authentication.
                if credential != TagCredential::None {
                    return (memory, Some(error.to_string()));
                }
            }
            Err(error) => return (memory, Some(error.to_string())),
        }
    }
    (memory, None)
}

pub(crate) fn document(
    card: &chameleon_client::Card,
    model: Model,
    memory: &[Option<Vec<u8>>],
    error: Option<&str>,
) -> serde_json::Value {
    let profile = model.type2_profile().unwrap();
    let counters = if profile.version.is_empty() {
        0
    } else {
        profile.counters.max(1)
    };
    let extra: Vec<Option<Vec<u8>>> =
        vec![None; counters + if profile.version.is_empty() { 0 } else { 10 }];
    let tag_type = match model {
        Model::Ntag213 => 1100,
        Model::Ntag215 => 1101,
        Model::Ntag216 => 1102,
        Model::Ultralight => 1103,
        Model::UltralightC => 1104,
        Model::UltralightEv1_48 => 1105,
        Model::UltralightEv1_128 => 1106,
        Model::Ntag210 => 1107,
        Model::Ntag212 => 1108,
        _ => unreachable!(),
    };
    let mut warnings = vec!["Hidden credentials and unread metadata remain unknown".to_owned()];
    if let Some(error) = error {
        warnings.push(error.to_owned());
    }
    serde_json::json!({"version": 1, "document": {
        "name": format!("{}-{}", model.name(), nfc_native::formats::flipper::hex(&card.uid)),
        "kind": "type2", "type": tag_type, "unitBytes": 4,
        "card": {"uid": card.uid, "atqa": [card.atqa[1], card.atqa[0]], "sak": 0, "ats": []},
        "memory": memory,
        "emulator": {"options": [0, 0, 0, profile.pages >> 8, profile.pages & 255], "extra": extra},
        "warnings": warnings,
    }})
}

fn page_role(model: Model, page: usize) -> &'static str {
    match page {
        0 | 1 => return "UID",
        2 => return "Lock",
        3 => {
            return if matches!(
                model,
                Model::Ultralight
                    | Model::UltralightC
                    | Model::UltralightEv1_48
                    | Model::UltralightEv1_128
            ) {
                "OTP"
            } else {
                "CC"
            };
        }
        _ => {}
    }
    if model == Model::UltralightC {
        return match page {
            40 => "Lock",
            41 => "Counter",
            42 | 43 => "Auth",
            44..=47 => "Key",
            _ => "",
        };
    }
    let profile = model.type2_profile().unwrap();
    if let Some(config) = profile.config_page() {
        if profile.pages > 20 && page == config - 1 {
            return "Lock";
        }
        if page == config || page == config + 1 {
            return "Config";
        }
        if page == profile.credential_start {
            return "PWD";
        }
        if page == profile.credential_start + 1 {
            return "PACK";
        }
    }
    ""
}

pub(crate) fn memory_text(
    model: Model,
    memory: &[Option<Vec<u8>>],
    start: usize,
    color: bool,
) -> String {
    let mut text = String::from("\n");
    for (index, page) in memory.iter().enumerate() {
        let address = start + index;
        let bytes = page.as_ref().map_or(vec![None; 4], |page| {
            page.iter().copied().map(Some).collect()
        });
        let role = page_role(model, address);
        text.push_str(&ui::hex_row(
            &format!("{address:03}"),
            &bytes,
            4,
            role,
            color,
        ));
        if !role.is_empty() {
            text.push_str(&format!(
                "  {}",
                ui::paint(role, ui::role_color(role), color)
            ));
        }
        text.push('\n');
    }
    if memory.iter().any(Option::is_none) {
        text.push_str("\n?? = unread or unavailable bytes.\n");
    }
    text
}

/// Extract a bounded NDEF TLV without interpreting proprietary records.
fn ndef(memory: &[Option<Vec<u8>>], model: Model) -> Result<Vec<u8>> {
    let cc = memory
        .get(3)
        .and_then(Option::as_ref)
        .ok_or("Capability page is unread")?;
    if cc.len() != 4 || cc[0] != 0xe1 || cc[1] >> 4 != 1 {
        return Err("Tag is not NFC Forum formatted".into());
    }
    let limit = 16 + usize::from(cc[2]) * 8;
    if !physical::user_unit(model, (limit - 1) / 4) {
        return Err("The declared NDEF capacity exceeds user memory".into());
    }
    let byte = |offset: usize| -> Result<u8> {
        if offset >= limit {
            return Err("NDEF exceeds the declared capacity".into());
        }
        memory
            .get(offset / 4)
            .and_then(Option::as_ref)
            .and_then(|page| page.get(offset % 4))
            .copied()
            .ok_or_else(|| "NDEF includes unread bytes".into())
    };
    let mut offset = 16;
    while offset < limit {
        let kind = byte(offset)?;
        offset += 1;
        if kind == 0 {
            continue;
        }
        if kind == 0xfe {
            break;
        }
        let short = byte(offset)?;
        offset += 1;
        let size = if short == 0xff {
            let size = usize::from(u16::from_be_bytes([byte(offset)?, byte(offset + 1)?]));
            offset += 2;
            size
        } else {
            usize::from(short)
        };
        if offset + size > limit {
            return Err("Truncated NDEF TLV".into());
        }
        if kind == 2 {
            return Err("Reserved-memory NDEF layouts require a raw page dump".into());
        }
        if kind == 1 {
            if size != 3 {
                return Err("Invalid NDEF lock-control layout".into());
            }
            let position = usize::from(byte(offset)?);
            let count = usize::from(byte(offset + 1)?);
            let lock_bytes = if count == 0 { 256 } else { count }.div_ceil(8);
            let address = (position >> 4) * (1usize << (byte(offset + 2)? & 15)) + (position & 15);
            if address < limit && address + lock_bytes > 16
                || address + lock_bytes > memory.len() * 4
            {
                return Err("Reserved lock-byte NDEF layouts require a raw page dump".into());
            }
        }
        if kind == 3 {
            return (offset..offset + size).map(byte).collect();
        }
        offset += size;
    }
    Err("No NDEF message found".into())
}

/// Decode the common single-record view; raw bytes remain the lossless output.
fn ndef_content(message: &[u8]) -> Option<(&'static str, String)> {
    let header = *message.first()?;
    if header & !0x10 != 0xc1 || *message.get(1)? != 1 {
        return None;
    }
    let (length, offset) = if header & 0x10 != 0 {
        (usize::from(*message.get(2)?), 3)
    } else {
        (
            u32::from_be_bytes(message.get(2..6)?.try_into().ok()?) as usize,
            6,
        )
    };
    let kind = *message.get(offset)?;
    let payload = message.get(offset + 1..)?;
    if payload.len() != length {
        return None;
    }
    let status = *payload.first()?;
    if kind == b'T' && status & 0xc0 == 0 {
        let text = std::str::from_utf8(payload.get(1 + usize::from(status)..)?).ok()?;
        return Some(("Text", text.to_owned()));
    }
    if kind == b'U' {
        const PREFIXES: &[&str] = &[
            "",
            "http://www.",
            "https://www.",
            "http://",
            "https://",
            "tel:",
            "mailto:",
            "ftp://anonymous:anonymous@",
            "ftp://ftp.",
            "ftps://",
            "sftp://",
            "smb://",
            "nfs://",
            "ftp://",
            "dav://",
            "news:",
            "telnet://",
            "imap:",
            "rtsp://",
            "urn:",
            "pop:",
            "sip:",
            "sips:",
            "tftp:",
            "btspp://",
            "btl2cap://",
            "btgoep://",
            "tcpobex://",
            "irdaobex://",
            "file://",
            "urn:epc:id:",
            "urn:epc:tag:",
            "urn:epc:pat:",
            "urn:epc:raw:",
            "urn:epc:",
            "urn:nfc:",
        ];
        let prefix = PREFIXES.get(usize::from(status))?;
        return Some((
            "URL",
            format!("{prefix}{}", std::str::from_utf8(&payload[1..]).ok()?),
        ));
    }
    None
}

pub fn run(
    command: Command,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    if let Command::Restore(options) = command {
        return restore::type2(options, port, cancelled, prompt_allowed);
    }
    let (model_hint, credential) = match &command {
        Command::Info { model } => (*model, TagCredential::None),
        Command::Read { target, .. }
        | Command::Dump { target, .. }
        | Command::Ndef { target, .. }
        | Command::Write { target, .. } => (target.model, credential(target)?),
        Command::Restore(_) => unreachable!(),
    };
    let write = if let Command::Write { data, .. } = &command {
        Some(crate::uid::hex_bytes(data, &[4])?)
    } else {
        None
    };
    if let Command::Dump {
        output: Some(path), ..
    }
    | Command::Ndef {
        output: Some(path), ..
    } = &command
    {
        if path.exists() {
            return Err("Output already exists".into());
        }
        if matches!(&command, Command::Dump { .. })
            && path.extension().is_none_or(|extension| extension != "json")
        {
            return Err("Use .json to preserve partial pages and unread credentials".into());
        }
    }
    let mut client = Client::connect(port, cancelled)?;
    Reader::begin(&mut client)?.run(|reader| {
        let model = reader.type2_model(model_hint.map(Into::into))?;
        if matches!(credential, TagCredential::UltralightC(_)) && model != Model::UltralightC
            || matches!(credential, TagCredential::Password(_))
                && matches!(model, Model::Ultralight | Model::UltralightC)
        {
            return Err("Credential does not match the selected model".into());
        }
        if matches!(command, Command::Write { .. }) && model == Model::Ultralight {
            return Err(
                "Original Ultralight writing is unsupported; inspection and dumps remain available"
                    .into(),
            );
        }
        ui::line(
            model.name(),
            &[
                format!(
                    "UID {}",
                    ui::paint(ui::uid_text(&reader.card.uid), "95", ui::stdout_color())
                ),
                format!("{} pages", model.type2().unwrap().0),
            ],
        );
        ui::line(
            "",
            &[
                format!(
                    "ATQA (RF) {}",
                    nfc_native::formats::flipper::hex(&reader.card.atqa)
                ),
                format!("SAK {:02X}", reader.card.sak),
            ],
        );
        let ndef_only = matches!(command, Command::Ndef { .. });
        match command {
            Command::Info { .. } => {}
            Command::Read { page, count, .. } => {
                let started = Instant::now();
                if usize::from(page) + usize::from(count) > model.type2().unwrap().0 {
                    return Err("Page range exceeds tag memory".into());
                }
                let mut memory = Vec::new();
                let mut error = None;
                let mut progress = crate::progress::Progress::new("Reading pages", None, false);
                for index in u16::from(page)..u16::from(page) + count {
                    match reader.type2_page(model, index as u8, credential) {
                        Ok(bytes) => memory.push(Some(bytes.to_vec())),
                        Err(failure) => {
                            error = Some(failure);
                            break;
                        }
                    }
                }
                progress.finish();
                ui::result(
                    error.is_none(),
                    format!(
                        "Read · {}/{} pages · {:.2} s",
                        memory.len(),
                        count,
                        started.elapsed().as_secs_f64()
                    ),
                );
                print!(
                    "{}",
                    memory_text(model, &memory, usize::from(page), ui::stdout_color())
                );
                if let Some(error) = error {
                    return Err(error);
                }
            }
            Command::Dump { output, .. } | Command::Ndef { output, .. } => {
                let started = Instant::now();
                let (memory, error) = capture(reader, model, credential);
                let profile = model.type2_profile().unwrap();
                let complete = error.is_none()
                    && memory[..profile.credential_start]
                        .iter()
                        .all(Option::is_some);
                ui::result(
                    complete,
                    format!(
                        "{} · {}/{} pages · {:.2} s",
                        if complete { "Read" } else { "Partial" },
                        memory.iter().flatten().count(),
                        memory.len(),
                        started.elapsed().as_secs_f64()
                    ),
                );
                if ndef_only {
                    let bytes = ndef(&memory, model)?;
                    ui::line("NDEF", &[format!("{} bytes", bytes.len())]);
                    for (index, chunk) in bytes.chunks(16).enumerate() {
                        println!(
                            "{}",
                            ui::hex_row(
                                &format!("{:04X}", index * 16),
                                &chunk.iter().copied().map(Some).collect::<Vec<_>>(),
                                16,
                                "",
                                ui::stdout_color()
                            )
                        );
                    }
                    if let Some((kind, content)) = ndef_content(&bytes) {
                        ui::line(kind, &[content]);
                    }
                    if let Some(path) = output {
                        save(&path, &bytes)?;
                    }
                } else {
                    print!("{}", memory_text(model, &memory, 0, ui::stdout_color()));
                    if let Some(path) = output {
                        save(
                            &path,
                            (serde_json::to_string_pretty(&document(
                                &reader.card,
                                model,
                                &memory,
                                error.as_deref(),
                            ))? + "\n")
                                .as_bytes(),
                        )?;
                    }
                }
                if let Some(error) = error {
                    return Err(error.into());
                }
            }
            Command::Write {
                page, dry_run, yes, ..
            } => {
                if !physical::user_unit(model, usize::from(page)) {
                    return Err("Only user pages can be written".into());
                }
                let (memory, error) = capture(reader, model, credential);
                if let Some(error) = error {
                    return Err(error.into());
                }
                if !physical::type2_page_writable(
                    model,
                    &memory,
                    usize::from(page),
                    credential != TagCredential::None,
                ) {
                    return Err("Page is locked or its permissions are unknown".into());
                }
                let before = memory[usize::from(page)].clone().ok_or("Page is unread")?;
                let after = write.unwrap();
                ui::line("Write page", &[page.to_string()]);
                ui::change(
                    format!("{page:03}"),
                    nfc_native::formats::flipper::hex(&before),
                    nfc_native::formats::flipper::hex(&after),
                );
                if before == after {
                    ui::no_write("Unchanged");
                } else if dry_run {
                    ui::no_write("Preview");
                } else if ui::confirm(
                    &format!("Write 1 page to tag {}?", ui::uid_text(&reader.card.uid)),
                    yes,
                    prompt_allowed,
                )? {
                    reader.write_user_unit(
                        model,
                        &physical::WriteUnit {
                            index: page,
                            before,
                            after,
                        },
                        credential,
                    )?;
                    ui::result(true, "Page written and verified");
                }
            }
            Command::Restore(_) => unreachable!(),
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dump_labels_use_model_geometry_and_keep_hidden_credentials_unknown() {
        let memory = vec![Some(vec![0; 4]), Some(vec![1; 4]), None, None];
        let plain = memory_text(Model::Ntag215, &memory, 131, false);
        assert!(plain.contains("  131  00 00 00 00  |....|  Config\n"));
        assert!(plain.contains("  133  ?? ?? ?? ??  |????|  PWD\n"));
        assert!(plain.contains("  134  ?? ?? ?? ??  |????|  PACK\n"));
        assert_eq!(
            ui::without_colors(&memory_text(Model::Ntag215, &memory, 131, true)),
            plain
        );
        for (page, role) in [
            (40, "Lock"),
            (41, "Counter"),
            (42, "Auth"),
            (43, "Auth"),
            (44, "Key"),
            (47, "Key"),
        ] {
            assert_eq!(page_role(Model::UltralightC, page), role);
        }
        for model in [
            Model::Ultralight,
            Model::UltralightC,
            Model::UltralightEv1_48,
            Model::UltralightEv1_128,
        ] {
            assert!(
                memory_text(model, &[Some(vec![0; 4])], 3, false)
                    .contains("  003  00 00 00 00  |....|  OTP\n")
            );
        }
        assert_eq!(page_role(Model::Ntag210, 3), "CC");
        assert_eq!(page_role(Model::Ntag210, 15), "");
        assert_eq!(page_role(Model::Ntag213, 40), "Lock");
    }
    #[test]
    fn ndef_content_decodes_text_and_uri_without_guessing_complex_records() {
        assert_eq!(
            ndef_content(&[0xd1, 1, 4, b'T', 2, b'e', b'n', b'a']),
            Some(("Text", "a".into()))
        );
        assert_eq!(
            ndef_content(&[0xd1, 1, 2, b'U', 4, b'x']),
            Some(("URL", "https://x".into()))
        );
        assert!(ndef_content(&[0xd1, 1, 4, b'T', 3, b'e']).is_none());
        assert!(ndef_content(&[0x91, 1, 4, b'T', 2, b'e', b'n', b'a']).is_none());
    }

    #[test]
    fn ndef_respects_declared_capacity_and_unread_pages() {
        let mut memory = vec![Some(vec![0; 4]); 45];
        memory[3] = Some(vec![0xe1, 0x10, 0x12, 0]);
        memory[4] = Some(vec![3, 3, 0xd0, 0]);
        memory[5] = Some(vec![0, 0xfe, 0, 0]);
        assert_eq!(ndef(&memory, Model::Ntag213).unwrap(), [0xd0, 0, 0]);
        let mut small = memory[..20].to_vec();
        small[3] = Some(vec![0xe1, 0x10, 6, 0]);
        assert!(ndef(&small, Model::Ntag210).is_ok());
        memory[5] = None;
        assert!(ndef(&memory, Model::Ntag213).is_err());
        memory[4] = Some(vec![3, 0xff, 0xff, 0xff]);
        assert!(ndef(&memory, Model::Ntag213).is_err());
    }
}
