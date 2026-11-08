use crate::{progress::Progress, report::Report, ui};
use chameleon_client::{ClassicRecovery, Client, Reader, Result};
use clap::Args;
use nfc_native::{
    RecoveryReader, Solvers, autopwn::*, dictionary, formats::image::classic::sector_count,
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
        let sectors = sector_count(model.classic().unwrap().1);
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
        let mut recovery = ClassicRecovery::new(reader)?;
        let mut warnings = Vec::new();
        let operation = (|| -> Result<()> {
            if let Some(key) = anchor {
                if recovery
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
                let result = recovery.execute(
                    &mut solvers,
                    &action,
                    &engine,
                    |sector, key_b, done, total| progress.dictionary(sector, key_b, done, total),
                )?;
                engine.submit_result(result)?;
                let (done, total) = engine.count_keys();
                progress.counts(done, total);
                events(&mut engine, &options, &mut warnings);
            }
            progress.phase("Reading card memory");
            recovery.read_memory(&engine.build_summary(false).mapping)
        })();
        let memory = recovery.into_memory();
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
