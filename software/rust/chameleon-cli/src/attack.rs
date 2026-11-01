use crate::{progress::Progress, ui};
use chameleon_client::{Client, DARKSIDE_STEP, Reader, Result};
use clap::Args;
use nfc_native::{
    RecoveryReader, Solvers,
    autopwn::{RecoveryOutcome, RecoverySource, RecoveryTarget},
    dictionary,
};
use std::{
    collections::HashSet,
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};

#[derive(Args)]
pub struct Target {
    /// Target sector index, starting at zero.
    #[arg(long)]
    pub sector: usize,
    /// Use Key B for authentication.
    #[arg(long)]
    pub key_b: bool,
    /// Show acquisition details.
    #[arg(long)]
    pub verbose: bool,
}
#[derive(Args)]
pub struct Options {
    /// Known source key, verified before acquisition.
    #[arg(short, long)]
    key: String,
    /// Sector containing the known source key.
    #[arg(long, default_value_t = 0)]
    source_sector: usize,
    /// Authenticate the source sector with Key B.
    #[arg(long)]
    source_key_b: bool,
    #[command(flatten)]
    target: Target,
}
pub enum Method {
    Nested,
    StaticNested,
    Hardnested(u16),
}

pub fn run(
    method: Method,
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
) -> Result<()> {
    let key = dictionary::key(&options.key)?;
    if let Method::Hardnested(budget) = method
        && (!(2..=4096).contains(&budget) || !budget.is_multiple_of(2))
    {
        return Err("Budget must be even and within 2..4096".into());
    }
    let mut client = Client::connect(port, cancelled)?;
    let mut recovered = None;
    let result = Reader::begin(&mut client)?.run(|reader| {
        reader.require_recovery_firmware()?;
        reader.validate_sector(options.source_sector)?;
        reader.validate_sector(options.target.sector)?;
        if reader
            .verify(options.source_sector, options.source_key_b, &[key])?
            .is_none()
        {
            return Err("Supplied source key was rejected".into());
        }
        let source = RecoverySource {
            sector: options.source_sector,
            is_key_b: options.source_key_b,
            key,
        };
        let target = RecoveryTarget {
            sector: options.target.sector,
            is_key_b: options.target.key_b,
        };
        ui::card(&reader.card, None);
        if options.target.verbose {
            reader.set_progress(ui::recovery_progress);
        }
        let label = match method {
            Method::Nested => "Nested",
            Method::StaticNested => "Static Nested",
            Method::Hardnested(_) => "Hardnested",
        };
        ui::line(
            label,
            &[format!(
                "sector {:02} Key {}",
                target.sector,
                if target.is_key_b { 'B' } else { 'A' }
            )],
        );
        let started = Instant::now();
        let mut progress = Progress::new(label, None, options.target.verbose);
        let mut solvers = Solvers::new(options.target.verbose);
        let outcome = match method {
            Method::Nested => solvers.nested(reader, &source, &target, &HashSet::new())?,
            Method::StaticNested => {
                solvers.static_nested(reader, &source, &target, &HashSet::new())?
            }
            Method::Hardnested(budget) => solvers.hardnested(reader, &source, &target, budget)?,
        };
        recovered = Some((outcome, started.elapsed()));
        reader.end()?;
        progress.finish();
        Ok(())
    });
    report_key(result, recovered)
}

pub fn darkside(target: Target, port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    let mut client = Client::connect(port, cancelled)?;
    let client_supports_bounded = client.capabilities.contains(&DARKSIDE_STEP);
    let mut recovered = None;
    let result = Reader::begin(&mut client)?.run(|reader| {
        reader.require_recovery_firmware()?;
        ui::card(&reader.card, None);
        ui::line(
            "Darkside",
            &[format!(
                "sector {:02} Key {}",
                target.sector,
                if target.key_b { 'B' } else { 'A' }
            )],
        );
        let started = Instant::now();
        let mut progress = Progress::new(
            if client_supports_bounded {
                "Darkside"
            } else {
                "Darkside (cancel may take up to 150 s)"
            },
            None,
            target.verbose,
        );
        let mut solvers = Solvers::new(target.verbose);
        let outcome = solvers.darkside(
            reader,
            &RecoveryTarget {
                sector: target.sector,
                is_key_b: target.key_b,
            },
        )?;
        recovered = Some((outcome, started.elapsed()));
        reader.end()?;
        progress.finish();
        if target.verbose {
            println!(
                "Darkside phases: acquisition {:.3} s, solving {:.3} s, verification {:.3} s",
                solvers.metrics.acquisition_us as f64 / 1e6,
                solvers.metrics.solving_us as f64 / 1e6,
                solvers.metrics.verification_us as f64 / 1e6
            );
        }
        Ok(())
    });
    report_key(result, recovered)
}

fn report_key(result: Result<()>, recovered: Option<(RecoveryOutcome, Duration)>) -> Result<()> {
    let report = recovered.map_or(Ok(()), |(outcome, elapsed)| print_key(outcome, elapsed));
    match (result, report) {
        (Err(error), Err(report)) => Err(format!("{report}; {error}").into()),
        (Err(error), _) | (_, Err(error)) => Err(error),
        _ => Ok(()),
    }
}

fn print_key(outcome: RecoveryOutcome, elapsed: Duration) -> Result<()> {
    let RecoveryOutcome::Verified(key) = outcome else {
        return Err(format!("No verified key recovered: {outcome:?}").into());
    };
    ui::result(
        true,
        format!(
            "Verified key · {} · {:.2} s",
            ui::paint(ui::key_text(Some(key)), ui::OK, ui::stdout_color()),
            elapsed.as_secs_f64()
        ),
    );
    Ok(())
}
