use crate::{restore::ClassicModel, ui, uid};
use chameleon_client::{
    Card, ClassicMaintenance, Client, DeviceError, Reader, Result, TagCredential,
};
use clap::{Args, ValueEnum};
use nfc_native::{
    dictionary,
    formats::image::{Model, classic},
};
use std::{
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};

#[derive(Clone, Copy, ValueEnum)]
pub enum Method {
    Gen1a,
    Gen1b,
    Gen2,
}
impl Method {
    fn wire(self) -> u8 {
        match self {
            Self::Gen1a => 1,
            Self::Gen1b => 2,
            Self::Gen2 => 3,
        }
    }
}

#[derive(Args)]
pub struct Options {
    /// Required when the card cannot be identified normally.
    #[arg(long, value_enum)]
    generation: Option<Method>,
    /// Tag model used to interpret its memory geometry.
    #[arg(long, value_enum)]
    model: Option<ClassicModel>,
    /// Repair only: replacement UID; otherwise preserve the observed UID.
    #[arg(long)]
    uid: Option<String>,
    /// Repair: reset these sector trailers (comma-separated indices or all).
    #[arg(long)]
    reset_access: Option<String>,
    /// Repair only: reset requested access without changing block 0.
    #[arg(long, requires = "reset_access", conflicts_with = "uid")]
    access_only: bool,
    /// Six-byte target sector key in hexadecimal.
    #[arg(long, value_name = "HEX")]
    key: Option<String>,
    /// Local key dictionary; may be supplied more than once.
    #[arg(long = "dict")]
    dictionaries: Vec<PathBuf>,
    /// Preview changes without writing.
    #[arg(long)]
    dry_run: bool,
    /// Confirm the prepared operation without prompting.
    #[arg(long)]
    yes: bool,
}

fn retryable_read(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<DeviceError>()
        .is_some_and(|error| matches!(error.0, 2 | 6))
}

fn unselectable(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<DeviceError>()
        .is_some_and(|error| matches!(error.0, 1..=5 | 7 | 8))
}

fn read_identity(
    maintenance: &mut ClassicMaintenance<'_>,
    method: u8,
    keys: &[[u8; 6]],
) -> Result<(TagCredential, [u8; 16])> {
    let credentials = if method == 3 {
        keys.iter()
            .flat_map(|key| [TagCredential::KeyA(*key), TagCredential::KeyB(*key)])
            .collect::<Vec<_>>()
    } else {
        vec![TagCredential::None]
    };
    for key in credentials {
        match maintenance.read(0, key) {
            Ok(block) => return Ok((key, block)),
            Err(error) if retryable_read(error.as_ref()) => continue,
            Err(error) => return Err(error),
        }
    }
    Err("No usable sector-0 key; no writes sent".into())
}

pub fn run(
    format: bool,
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    if format && (options.uid.is_some() || options.reset_access.is_some() || options.access_only) {
        return Err(
            "Format preserves block 0 and resets all trailers; use repair for targeted changes"
                .into(),
        );
    }
    let uid = options
        .uid
        .as_deref()
        .map(|text| uid::hex_bytes(text, &[4, 7]))
        .transpose()?;
    let mut keys = Vec::new();
    if let Some(value) = &options.key {
        keys.push(uid::hex_bytes(value, &[6])?.try_into().unwrap());
    }
    for path in &options.dictionaries {
        let text = std::fs::read_to_string(path)?;
        if text.len() > 1024 * 1024 {
            return Err("Dictionary exceeds 1 MiB".into());
        }
        keys.extend(dictionary::parse(&text, &path.display().to_string())?);
    }
    keys.extend(dictionary::essentials()?);
    let mut seen = std::collections::HashSet::new();
    keys.retain(|key| seen.insert(*key));
    let mut client = Client::connect(port, cancelled)?;
    let mut detected = None;
    let mut method = options.generation.map(Method::wire);
    match Reader::begin(&mut client) {
        Ok(reader) => {
            detected = Some(reader.run(|reader| {
                if method.is_none() {
                    let mut magic = reader.magic(None)?;
                    if magic.maintenance_method().is_none() {
                        use nfc_native::RecoveryReader;
                        for key_b in [false, true] {
                            if let Some(key) = reader.verify(0, key_b, &keys)? {
                                magic = reader.magic(Some((key, key_b)))?;
                                if magic.maintenance_method().is_some() {
                                    break;
                                }
                            }
                        }
                    }
                    method = magic.maintenance_method();
                }
                Ok(reader.card.clone())
            })?);
        }
        Err(error) => {
            if !unselectable(error.as_ref()) {
                return Err(error);
            }
            if method.is_none() || options.model.is_none() || format {
                return Err(format!("{error}. Repair an unselectable card with --generation and --model; add --uid for a 7-byte or replacement UID").into());
            }
        }
    }
    let method =
        method.ok_or("Maintenance method unconfirmed; specify a known rewritable --generation")?;
    let model = match options.model {
        Some(model) => Model::from(model),
        None => detected.as_ref().ok_or("Specify --model")?.model()?,
    };
    if let Some(card) = &detected {
        let matches =
            card.model().is_ok_and(|actual| actual == model) && matches!(card.uid.len(), 4 | 7);
        if !matches {
            if !format && options.generation.is_some() && options.model.is_some() {
                detected = None;
            } else {
                return Err("Selected model differs from the card".into());
            }
        }
    }
    let unbound = detected.is_none();
    let card = detected.unwrap_or(Card {
        uid: vec![0; uid.as_ref().map_or(4, Vec::len)],
        atqa: [0; 2],
        sak: 0,
    });
    ClassicMaintenance::begin(&mut client, method, model, card, unbound)?.run(|maintenance| {
        let (identity_key, initial_block0) = read_identity(maintenance, method, &keys)?;
        let blocks = model.classic().unwrap().1;
        let sectors = classic::sector_count(blocks);
        let mut reset: Vec<usize> = match options.reset_access.as_deref() {
            Some("all") => (0..sectors).collect(),
            Some(text) => text
                .split(',')
                .map(|value| value.parse::<usize>().map_err(Into::into))
                .collect::<Result<_>>()?,
            None => Vec::new(),
        };
        reset.sort_unstable();
        reset.dedup();
        if reset.iter().any(|sector| *sector >= sectors) {
            return Err("Sector outside card geometry".into());
        }
        let units: Vec<usize> = if format {
            (1..blocks).collect()
        } else {
            std::iter::once(0)
                .filter(|_| !options.access_only)
                .chain(
                    reset
                        .iter()
                        .map(|sector| classic::trailer(*sector) as usize),
                )
                .collect()
        };
        let mut plan = Vec::new();
        let mut preferred = vec![None; sectors];
        preferred[0] = Some(identity_key);
        for unit in units {
            let sector = classic::sector(unit);
            let trailer = classic::trailer(sector) as usize == unit;
            let mut credentials = Vec::new();
            if method != 3 {
                credentials.push(TagCredential::None);
            } else {
                if let Some(key) = preferred[sector] {
                    credentials.push(key);
                }
                for key in &keys {
                    credentials.push(TagCredential::KeyA(*key));
                    credentials.push(TagCredential::KeyB(*key));
                }
            }
            let mut selected = None;
            for key in credentials {
                let before = match maintenance.read(unit as u8, key) {
                    Ok(before) => before,
                    Err(error) if retryable_read(error.as_ref()) => continue,
                    Err(error) => return Err(error),
                };
                let mut after = [0; 16];
                if unit == 0 {
                    after = before;
                    if let Some(uid) = &uid {
                        if uid.len() != maintenance.card.uid.len() {
                            return Err("UID length cannot change".into());
                        }
                        after[..uid.len()].copy_from_slice(uid);
                    }
                    let offset = if maintenance.card.uid.len() == 4 {
                        5
                    } else {
                        7
                    };
                    if offset == 5 {
                        after[4] = after[..4].iter().fold(0, |bcc, byte| bcc ^ byte);
                    }
                    let (_, _, atqa, sak) = model.classic().unwrap();
                    after[offset] = sak;
                    after[offset + 1] = atqa[1] | if offset == 7 { 0x40 } else { 0 };
                    after[offset + 2] = atqa[0];
                } else if trailer {
                    after = [0xff; 16];
                    after[6..10].copy_from_slice(&[
                        0xff,
                        7,
                        0x80,
                        if format && sector == sectors - 1 {
                            0xbc
                        } else {
                            0x69
                        },
                    ]);
                }
                if before == after && !trailer {
                    selected = Some(None);
                    break;
                }
                match maintenance.check(unit as u8, key, before, after) {
                    Ok(()) => {
                        preferred[sector] = Some(key);
                        selected = Some(Some((unit as u8, key, before, after)));
                        break;
                    }
                    Err(error)
                        if error
                            .downcast_ref::<DeviceError>()
                            .is_some_and(|error| matches!(error.0, 6 | 0x75)) =>
                    {
                        continue;
                    }
                    Err(error) => return Err(error),
                }
            }
            let entry = selected.ok_or_else(|| {
                format!("Block {unit}: no usable key or write permission. No writes sent")
            })?;
            if let Some(entry) = entry {
                plan.push(entry);
            }
        }
        // Preserve the original access rights until all data writes have completed.
        plan.sort_by_key(|(unit, ..)| classic::trailer(classic::sector(*unit as usize)) == *unit);
        ui::report(
            if format {
                "Factory format"
            } else {
                "Repair tag"
            },
            &[
                (
                    "UID",
                    nfc_native::formats::flipper::hex(&maintenance.card.uid),
                ),
                (
                    "Method",
                    ["", "Gen1A", "Gen1B", "Gen2 / CUID"][method as usize].into(),
                ),
                (
                    "Blocks to write",
                    plan.iter()
                        .map(|(unit, ..)| unit.to_string())
                        .collect::<Vec<_>>()
                        .join(", "),
                ),
                (
                    "Effect",
                    if format {
                        "Zero data; reset keys/access; preserve complete block 0"
                    } else {
                        "Repair block 0; reset only requested trailers; do not write data blocks"
                    }
                    .into(),
                ),
                (
                    "Previous keys",
                    "May be unknown; reset trailers use FFFFFFFFFFFF".into(),
                ),
            ],
        );
        for (unit, _, before, after) in &plan {
            let before = if *unit == classic::trailer(classic::sector(*unit as usize)) {
                format!(
                    "keys hidden; access/GPB {}",
                    nfc_native::formats::flipper::hex(&before[6..10])
                )
            } else {
                ui::uid_text(before)
            };
            ui::change(format!("{unit:03}"), before, ui::uid_text(after));
        }
        if plan.is_empty() {
            ui::no_write("Unchanged");
            return Ok(());
        }
        if options.dry_run {
            ui::no_write("Preview");
            return Ok(());
        }
        let question = format!(
            "{} {} block{} on tag {}?",
            if format { "Format" } else { "Repair" },
            plan.len(),
            if plan.len() == 1 { "" } else { "s" },
            ui::uid_text(&maintenance.card.uid)
        );
        if !ui::confirm(&question, options.yes, prompt_allowed)? {
            return Ok(());
        }
        let mut done = 0;
        for (unit, key, before, after) in &plan {
            if let Err(error) = maintenance.write(*unit, *key, *before, *after) {
                return Err(
                    format!("{done}/{} blocks written and verified. {error}", plan.len()).into(),
                );
            }
            done += 1;
        }
        let final_key = if method == 3 && (format || reset.contains(&0)) {
            TagCredential::KeyA([0xff; 6])
        } else {
            identity_key
        };
        let expected = plan
            .iter()
            .find(|(unit, ..)| *unit == 0)
            .map_or(initial_block0, |(_, _, _, after)| *after);
        if maintenance.read(0, final_key)? != expected {
            return Err(format!(
                "{done} blocks verified, but final block-0 verification failed; inspect before retrying"
            )
            .into());
        }
        ui::result(
            true,
            format!(
                "Maintenance verified · {done} blocks · UID {}",
                ui::uid_text(&maintenance.card.uid)
            ),
        );
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::RequestCancelled;
    use std::io;

    #[test]
    fn read_retries_access_and_authentication_errors_only() {
        assert!(retryable_read(&DeviceError(2)));
        assert!(retryable_read(&DeviceError(6)));
        for status in [1, 5, 0x60, 0x67, 0x69, 0x75, 0x76] {
            assert!(!retryable_read(&DeviceError(status)), "status {status}");
        }
        assert!(!retryable_read(&RequestCancelled));
        assert!(!retryable_read(&io::Error::new(
            io::ErrorKind::TimedOut,
            "serial read timed out",
        )));
    }

    #[test]
    fn unselectable_card_fallback_does_not_hide_mode_or_cleanup_errors() {
        for status in [1, 2, 3, 4, 5, 7, 8] {
            assert!(unselectable(&DeviceError(status)));
        }
        for status in [6, 0x60, 0x66, 0x67, 0x69, 0x76] {
            assert!(!unselectable(&DeviceError(status)));
        }
        assert!(!unselectable(&RequestCancelled));
        assert!(!unselectable(&io::Error::new(
            io::ErrorKind::TimedOut,
            "Reader cleanup failed"
        )));
    }
}
