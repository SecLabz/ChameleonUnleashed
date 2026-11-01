use crate::{progress::Progress, report::Report, ui};
use chameleon_client::{Client, Reader, Result};
use clap::Args;
use nfc_native::{
    RecoveryReader, Solvers,
    autopwn::*,
    dictionary,
    formats::image::classic::{blocks, first_block, key_b_readable, sector_count, trailer},
};
use std::{
    io::{Read, Write},
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
    time::Instant,
};

#[derive(Args)]
pub struct Options {
    /// Known key, verified before it is used as a recovery source.
    #[arg(short, long)]
    key: Option<String>,
    /// Sector containing the known source key.
    #[arg(long, default_value_t = 0)]
    source_sector: usize,
    /// Authenticate the source sector with Key B.
    #[arg(long)]
    source_key_b: bool,
    /// Local key dictionary; may be supplied more than once.
    #[arg(long = "dict")]
    dictionaries: Vec<PathBuf>,
    /// Save a complete .nfc dump or a lossless, possibly partial .json capture.
    #[arg(short, long)]
    output: Option<PathBuf>,
    /// Maximum recovery work budget.
    #[arg(long, default_value_t = 4096)]
    budget: u16,
    /// Show detailed progress and key sources.
    #[arg(long)]
    verbose: bool,
}

pub fn run(options: Options, port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    if !(2..=4096).contains(&options.budget) || !options.budget.is_multiple_of(2) {
        return Err("Budget must be even and within 2..4096".into());
    }
    if options.output.as_ref().is_some_and(|path| {
        !matches!(
            path.extension().and_then(|s| s.to_str()),
            Some("json" | "nfc")
        )
    }) {
        return Err("Choose a .json or .nfc output file".into());
    }
    if options.output.as_ref().is_some_and(|path| path.exists()) {
        return Err("Output already exists".into());
    }
    let anchor = options.key.as_deref().map(dictionary::key).transpose()?;
    let mut local = Vec::new();
    for path in &options.dictionaries {
        let mut text = String::new();
        std::fs::File::open(path)?
            .take(1024 * 1024 + 1)
            .read_to_string(&mut text)?;
        if text.len() > 1024 * 1024 {
            return Err("Dictionary exceeds 1 MiB".into());
        }
        local.extend(dictionary::parse(&text, &path.display().to_string())?);
    }
    let plan = dictionary::Plan::new(local, anchor, dictionary::Profile::Extended, true)?;
    let started = Instant::now();
    let mut client = Client::connect(port, cancelled)?;
    Reader::begin(&mut client)?.run(|reader| {
        reader.require_recovery_firmware()?;
        let model = reader.card.model()?;
        if options
            .output
            .as_ref()
            .is_some_and(|path| path.extension().is_some_and(|s| s == "nfc"))
            && reader.card.uid.len() != 4
        {
            return Err(
                "Flipper Classic export requires a 4-byte UID; use --output capture.json to retain the full capture"
                    .into(),
            );
        }
        let block_count = model.classic().unwrap().1;
        let sectors = sector_count(block_count);
        if options.source_sector >= sectors {
            return Err("Source sector outside card geometry".into());
        }
        let card = reader.card.clone();
        ui::message(
            format!(
                "AutoPwn · {} · dictionary: {} keys{}",
                model.name(),
                plan.nonce_keys.len(),
                if options.dictionaries.is_empty() {
                    ""
                } else {
                    " · local files first"
                }
            ),
            "1;36",
        );
        ui::message(
            format!(
                "UID {} · ATQA (RF) {} · SAK {:02X}",
                ui::uid_text(&card.uid),
                nfc_native::formats::flipper::hex(&card.atqa),
                card.sak
            ),
            "0",
        );
        if options.verbose {
            ui::message(
                format!("RF fallback: extended · {} candidates", plan.unique_keys),
                "0",
            );
        }
        let mut progress = Progress::new("Preparing recovery", Some(sectors * 2), options.verbose);
        let mut engine = AutoPwnSession::try_new(AutoPwnConfig {
            uid: card.uid.clone(),
            sector_count: sectors,
            first_sector: 0,
            last_sector: sectors - 1,
            prng: PrngMode::Unknown,
            budget: options.budget,
        })?;
        engine.configure_dictionaries(plan.passes, false)?;
        if options.verbose {
            reader.set_progress(ui::recovery_progress);
        }
        let mut solvers = Solvers::new(options.verbose);
        solvers.configure_dictionary(&plan.nonce_keys);
        let mut memory = vec![None; block_count];
        let mut warnings = Vec::new();
        let operation = (|| -> Result<()> {
            if let Some(key) = anchor {
                if reader
                    .verify(options.source_sector, options.source_key_b, &[key])?
                    .is_none()
                {
                    return Err("Supplied source key was rejected".into());
                }
                engine.record_verified_source(options.source_sector, options.source_key_b, key)?;
            }
            while let Some(action) = engine.next_action()? {
                let (done, total) = engine.count_keys();
                progress.counts(done, total);
                progress.action(&action);
                let result = nfc_native::execute(
                    reader,
                    &mut solvers,
                    &action,
                    &engine,
                    |sector, key_b, done, total| progress.dictionary(sector, key_b, done, total),
                    |reader, sector, key| {
                        let source = RecoverySource {
                            sector,
                            is_key_b: false,
                            key,
                        };
                        let index = usize::from(trailer(sector));
                        let Some(block) = read_block(reader, &source, index as u8)? else {
                            return Ok(None);
                        };
                        let key = key_b_readable(&block).then(|| block[10..16].try_into().unwrap());
                        memory[index] = Some(block);
                        Ok(key)
                    },
                )?;
                engine.submit_result(result)?;
                let (done, total) = engine.count_keys();
                progress.counts(done, total);
                events(&mut engine, &options, &mut warnings);
            }
            progress.phase("Reading card memory");
            read_memory(
                reader,
                &engine.build_summary(false).mapping,
                &mut memory,
            )
        })();
        events(&mut engine, &options, &mut warnings);
        progress.finish();
        let cleanup = reader.finish();
        let error = match (operation, cleanup) {
            (Err(error), Err(cleanup)) => Some(format!("{error}; RF cleanup failed: {cleanup}")),
            (Err(error), _) | (_, Err(error)) => Some(error.to_string()),
            _ => None,
        };
        let report = Report {
            card,
            summary: engine.build_summary(error.is_some()),
            memory,
            error,
            warnings,
            elapsed: started.elapsed(),
        };
        report.print_summary(options.verbose);
        let exported: Result<Option<()>> = options
            .output
            .as_ref()
            .map(|output| {
                let text = if output
                    .extension()
                    .is_some_and(|extension| extension == "json")
                {
                    report.json()?
                } else {
                    report.export()?
                };
                std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(output)?
                    .write_all(text.as_bytes())?;
                println!("Dump saved to {}", output.display());
                Ok(())
            })
            .transpose();
        if options.output.is_none() || exported.is_err() {
            let color = std::io::IsTerminal::is_terminal(&std::io::stdout()) && ui::color_enabled();
            print!("{}", report.dump_text(color));
        }
        if let Some(error) = report.error {
            return Err(error.into());
        }
        exported?;
        Ok(())
    })
}

fn events(engine: &mut AutoPwnSession, options: &Options, warnings: &mut Vec<String>) {
    for event in engine.take_events() {
        match event {
            AutoPwnEvent::Warning(warning) => {
                let message = warning.message(options.budget);
                if !warnings.contains(&message) {
                    warnings.push(message);
                }
            }
            AutoPwnEvent::KeyFound {
                sector,
                is_key_b,
                key,
                origin,
            } if options.verbose => {
                ui::message(
                    format!(
                        "Sector {sector:02} {}: {} ({})",
                        if is_key_b { 'B' } else { 'A' },
                        nfc_native::formats::flipper::hex(&key),
                        origin.name()
                    ),
                    "0",
                );
            }
            _ => (),
        }
    }
}

fn read_block(
    reader: &mut Reader<'_>,
    source: &RecoverySource,
    block: u8,
) -> Result<Option<Vec<u8>>> {
    match reader.block(source, block) {
        Ok(bytes) => Ok(Some(bytes)),
        Err(error) if read_unavailable(&*error) => Ok(None),
        Err(error) => Err(format!(
            "Reading block {block} (sector {}, Key {}): {error}",
            source.sector,
            if source.is_key_b { 'B' } else { 'A' },
        )
        .into()),
    }
}

fn read_unavailable(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<chameleon_client::DeviceError>()
        .is_some_and(|error| matches!(error.0, 2 | 6))
}

fn sector_unsupported(error: &(dyn std::error::Error + 'static)) -> bool {
    error
        .downcast_ref::<chameleon_client::DeviceError>()
        .is_some_and(|error| matches!(error.0, 0x60 | 0x69))
}

fn read_memory(
    reader: &mut Reader<'_>,
    mapping: &[SectorKeys],
    memory: &mut [Option<Vec<u8>>],
) -> Result<()> {
    for (sector, keys) in mapping.iter().enumerate() {
        for (key_b, key) in [(false, keys.key_a), (true, keys.key_b)] {
            if key_b && keys.origin_b == Some(KeyOrigin::Trailer) {
                continue;
            }
            let Some(key) = key else {
                continue;
            };
            let source = RecoverySource {
                sector,
                is_key_b: key_b,
                key,
            };
            let first = usize::from(first_block(sector));
            let count = blocks(sector);
            if memory[first..first + count].iter().all(Option::is_some) {
                break;
            }
            match reader.sector(&source) {
                Ok(result) => {
                    for (slot, block) in memory[first..first + count].iter_mut().zip(result) {
                        if let Some(block) = block {
                            *slot = Some(block.to_vec());
                        }
                    }
                }
                Err(error) if sector_unsupported(&*error) => {}
                Err(error) if read_unavailable(&*error) => continue,
                Err(error) => return Err(format!("Reading sector {sector}: {error}").into()),
            }
            for (index, slot) in memory
                .iter_mut()
                .enumerate()
                .skip(usize::from(first_block(sector)))
                .take(blocks(sector))
            {
                if slot.is_none() {
                    *slot = read_block(reader, &source, index as u8)?;
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn denied_read_leaves_the_block_for_the_other_sector_key() {
        assert!(read_unavailable(&chameleon_client::DeviceError(2)));
        assert!(read_unavailable(&chameleon_client::DeviceError(6)));
        assert!(!read_unavailable(&chameleon_client::DeviceError(3)));
    }

    #[test]
    fn legacy_sector_rejections_fall_back_without_hiding_transport_errors() {
        assert!(sector_unsupported(&chameleon_client::DeviceError(0x60)));
        assert!(sector_unsupported(&chameleon_client::DeviceError(0x69)));
        assert!(!sector_unsupported(&chameleon_client::DeviceError(2)));
        assert!(!sector_unsupported(&chameleon_client::RequestCancelled));
        assert!(!sector_unsupported(&std::io::Error::from(
            std::io::ErrorKind::TimedOut
        )));
    }
}
