//! ST25TB commands and physical image handling.
mod tearoff;

use crate::hf::{self, MemoryCommand};
use crate::ui;
use chameleon_client::{Client, HfReader, HfSnapshot, Result, TearoffOutcome};
use clap::Subcommand;
use std::{
    ops::RangeInclusive,
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};

const USER_BLOCKS: usize = 16;
const BLOCK_BYTES: usize = 4;
const UID_BYTES: usize = 8;
const SYSTEM_BLOCK: u8 = 255;
const AT_UID_PREFIX: [u8; 3] = [0xd0, 2, 0x33];
const CAPTURE_VERSION: u8 = 1;
const COUNTER_BLOCKS: RangeInclusive<i64> = 5..=6;
const SAFETY_MINUTES_RANGE: RangeInclusive<i64> = 1..=120;
const ADJUSTMENT_US_RANGE: RangeInclusive<i64> = 1..=100;
const DEFAULT_SAFETY_MINUTES: u16 = 30;
const DEFAULT_ADJUSTMENT_US: u8 = 22;
const MILLIS_PER_MINUTE: u32 = 60_000;

#[derive(Subcommand)]
pub enum Command {
    /// Identify the tag, Chip ID and memory geometry.
    Info,
    /// Read one four-byte block; 255 selects the system block.
    Read {
        /// Memory block index, starting at zero.
        #[arg(long, value_parser = clap::value_parser!(u16).range(0..=i64::from(SYSTEM_BLOCK)))]
        block: u16,
    },
    /// Read all supported memory blocks and the system block.
    Dump {
        /// Output file; existing files are never overwritten.
        #[arg(long)]
        output: Option<PathBuf>,
        /// Use defaults for unavailable emulator metadata when exporting .nfc.
        #[arg(long)]
        emulator_defaults: bool,
    },
    /// Write one physical ST25TB512-AT block and verify its value.
    Write {
        /// Memory block index, starting at zero.
        #[arg(long, value_parser = clap::value_parser!(u8).range(0..=i64::from(SYSTEM_BLOCK)))]
        block: u8,
        /// New 32-bit word in hexadecimal.
        #[arg(value_parser = parse_target)]
        value: u32,
        /// Explicitly allow the irreversible system OTP block (255).
        #[arg(long)]
        otp: bool,
    },
    /// Restore a complete JSON or Flipper .nfc image to the same physical tag.
    Restore {
        /// Input capture or image file.
        file: PathBuf,
        /// Confirm physical writes after the change list is printed.
        #[arg(long)]
        yes: bool,
        /// Permit counter increases through the existing tearoff command.
        #[arg(long)]
        tearoff: bool,
        /// Permit irreversible changes to system OTP.
        #[arg(long)]
        otp: bool,
    },
    /// Run a bounded ST25TB counter tearoff on a tag.
    Tearoff {
        /// Counter block (5 or 6).
        #[arg(long, value_parser = clap::value_parser!(u8).range(COUNTER_BLOCKS))]
        block: u8,
        /// Eight-digit hexadecimal target value.
        #[arg(long, value_parser = parse_target)]
        target: u32,
        /// Device safety deadline in minutes (default 30, maximum 120).
        #[arg(long, default_value_t = DEFAULT_SAFETY_MINUTES, value_parser = clap::value_parser!(u16).range(SAFETY_MINUTES_RANGE))]
        safety_minutes: u16,
        /// Timing adjustment in microseconds (default 22, range 1..=100).
        #[arg(long, default_value_t = DEFAULT_ADJUSTMENT_US, value_parser = clap::value_parser!(u8).range(ADJUSTMENT_US_RANGE))]
        adjustment_us: u8,
        /// Report device time spent in radio, decoding, USB and pauses.
        #[arg(long)]
        timing: bool,
    },
}

fn parse_target(text: &str) -> std::result::Result<u32, String> {
    let digits = text.strip_prefix("0x").unwrap_or(text);
    if digits.len() != 8 || !digits.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err("target must have eight hexadecimal digits".into());
    }
    u32::from_str_radix(digits, 16).map_err(|error| error.to_string())
}

pub fn run(command: Command, port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    match command {
        Command::Info => hf::run(
            "st25tb",
            MemoryCommand::Info(hf::Authentication::default()),
            port,
            cancelled,
        ),
        Command::Read { block } => hf::run(
            "st25tb",
            MemoryCommand::Read {
                block,
                authentication: hf::Authentication::default(),
            },
            port,
            cancelled,
        ),
        Command::Dump {
            output,
            emulator_defaults,
        } => hf::run(
            "st25tb",
            MemoryCommand::Dump {
                authentication: hf::Authentication::default(),
                output,
                emulator_defaults,
            },
            port,
            cancelled,
        ),
        Command::Tearoff {
            block,
            target,
            safety_minutes,
            adjustment_us,
            timing,
        } => tearoff::run(
            port,
            cancelled,
            block,
            target,
            safety_minutes,
            adjustment_us,
            timing,
        ),
        Command::Write { block, value, otp } => write(port, cancelled, block, value, otp),
        Command::Restore {
            file,
            yes,
            tearoff,
            otp,
        } => restore(port, cancelled, &file, yes, tearoff, otp),
    }
}

fn restore(
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    file: &PathBuf,
    yes: bool,
    allow_tearoff: bool,
    allow_otp: bool,
) -> Result<()> {
    use nfc_native::formats::image::Model;
    let bytes = std::fs::read(file)?;
    let (uid, memory, system) = if file
        .extension()
        .is_some_and(|extension| extension == "json")
    {
        let snapshot: HfSnapshot = serde_json::from_slice(&bytes)?;
        if snapshot.version != CAPTURE_VERSION
            || snapshot.protocol != "st25tb"
            || snapshot.uid[..AT_UID_PREFIX.len()] != AT_UID_PREFIX
            || !snapshot.complete
            || snapshot.memory.len() != USER_BLOCKS
            || snapshot.memory.iter().any(Option::is_none)
        {
            return Err("Complete ST25TB512-AT JSON capture required".into());
        }
        (
            snapshot.uid,
            snapshot
                .memory
                .into_iter()
                .map(Option::unwrap)
                .collect::<Vec<_>>(),
            snapshot.system.ok_or("JSON capture has no system block")?,
        )
    } else {
        let (image, _) =
            nfc_native::formats::flipper::parse_with_warnings(std::str::from_utf8(&bytes)?)?;
        if image.model != Model::St25tb512At
            || image.uid.len() != UID_BYTES
            || image.memory.len() != USER_BLOCKS * BLOCK_BYTES
            || image.metadata.len() != BLOCK_BYTES
        {
            return Err("Complete ST25TB512-AT .nfc image required".into());
        }
        (
            image.uid.as_slice().try_into()?,
            image
                .memory
                .chunks_exact(BLOCK_BYTES)
                .map(|bytes| bytes.try_into().unwrap())
                .collect::<Vec<_>>(),
            image.metadata.as_slice().try_into()?,
        )
    };
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let result: Result<()> = (|| {
        let tag = reader.st25tb()?;
        if tag.uid() != uid {
            return Err("Physical ST25TB UID differs from the image".into());
        }
        hf::heading(&tag);
        ui::line("Restore", &["32-bit words, MSB first".into()]);
        let current = reader.dump(&tag)?;
        if !current.complete
            || current.memory.iter().any(Option::is_none)
            || current.system.is_none()
        {
            return Err("Complete physical read required before restore".into());
        }
        let mut normal = Vec::new();
        let mut counters = Vec::new();
        for (block, desired) in memory.iter().enumerate() {
            let before = current.memory[block].unwrap();
            if before == *desired {
                continue;
            }
            if COUNTER_BLOCKS.contains(&(block as i64))
                && u32::from_le_bytes(*desired) > u32::from_le_bytes(before)
            {
                counters.push((block as u8, *desired));
            } else {
                normal.push((block as u8, *desired));
            }
            ui::change(
                format!("{block:03}"),
                format!("{:08X}", u32::from_le_bytes(before)),
                format!("{:08X}", u32::from_le_bytes(*desired)),
            );
        }
        let old_system = current.system.unwrap();
        let otp_changed = old_system != system;
        if otp_changed {
            if system
                .iter()
                .zip(old_system)
                .any(|(after, before)| after | before != before)
            {
                return Err("sysOTP bits can only change from 1 to 0".into());
            }
            ui::change(
                "sysOTP",
                format!("{:08X}", u32::from_le_bytes(old_system)),
                format!("{:08X}", u32::from_le_bytes(system)),
            );
        }
        if !counters.is_empty() && !allow_tearoff {
            return Err("Counter increase requires --tearoff".into());
        }
        if otp_changed && !allow_otp {
            return Err("sysOTP write requires --otp".into());
        }
        if normal.is_empty() && counters.is_empty() && !otp_changed {
            println!("Tag already matches the image");
            return Ok(());
        }
        if !yes {
            return Err("Review the changes above, then rerun with --yes".into());
        }
        for (block, value) in normal {
            reader.write_st25tb(&tag, block, value)?;
            if reader.read(&tag, u16::from(block))? != value {
                return Err(format!("Block {block} readback differs").into());
            }
            println!("Verified block {block}");
        }
        for (block, value) in counters {
            let target = u32::from_le_bytes(value);
            let report = reader.tearoff_live(
                &tag,
                block,
                target,
                u32::from(DEFAULT_SAFETY_MINUTES) * MILLIS_PER_MINUTE,
                false,
                &mut tearoff::print_progress,
            )?;
            if report.diagnostics.result.outcome != TearoffOutcome::Success
                || report.diagnostics.result.last_value != Some(target)
                || reader.read(&tag, u16::from(block))? != value
            {
                return Err(format!("Tearoff block {block} was not verified").into());
            }
            println!("Verified tearoff block {block}");
        }
        if otp_changed {
            reader.write_st25tb(&tag, SYSTEM_BLOCK, system)?;
        }
        let final_capture = reader.dump(&tag)?;
        if !final_capture.complete
            || final_capture.system != Some(system)
            || final_capture
                .memory
                .iter()
                .zip(&memory)
                .any(|(actual, expected)| actual != &Some(*expected))
        {
            return Err("Final physical dump differs from the image".into());
        }
        ui::result(true, "ST25TB restore verified by a full dump");
        Ok(())
    })();
    let cleanup = reader.finish();
    result?;
    cleanup
}

fn write(
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    block: u8,
    value: u32,
    otp: bool,
) -> Result<()> {
    if usize::from(block) >= USER_BLOCKS && block != SYSTEM_BLOCK {
        return Err("ST25TB block must be 0..15 or 255".into());
    }
    if block == SYSTEM_BLOCK && !otp {
        return Err("Use --otp to write the irreversible system block".into());
    }
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let result: Result<()> = (|| {
        let tag = reader.st25tb()?;
        hf::heading(&tag);
        let before = reader.read(&tag, u16::from(block))?;
        ui::line("Write", &["32-bit word, MSB first".into()]);
        ui::change(
            format!("{block:03}"),
            format!("{:08X}", u32::from_le_bytes(before)),
            format!("{value:08X}"),
        );
        let actual = reader.write_st25tb(&tag, block, value.to_le_bytes())?;
        ui::result(
            true,
            format!(
                "Block {block} verified · {:08X}",
                u32::from_le_bytes(actual)
            ),
        );
        Ok(())
    })();
    let cleanup = reader.finish();
    result?;
    cleanup
}
