//! Bounded raw exchanges; only successful --keep calls retain RF ownership.
use chameleon_client::{Client, DeviceError, HfReader, Result};
use clap::Args;
use std::{
    io::Read,
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};

#[derive(Args, Default)]
pub struct Options {
    /// Frame bytes. Quote spaces or supply separate hexadecimal words.
    #[arg(value_name = "HEX", num_args = 1.., required_unless_present = "file", conflicts_with = "file")]
    data: Vec<String>,
    /// One frame per line; optional /BITS suffix for short NFC-A frames. # starts a comment.
    #[arg(long)]
    file: Option<PathBuf>,
    /// Exact TX bit count for a single NFC-A frame.
    #[arg(long, value_parser = clap::value_parser!(u16).range(1..=496))]
    bits: Option<u16>,
    /// Append TX CRC and check/remove RX CRC.
    #[arg(long)]
    crc: bool,
    /// Append TX CRC.
    #[arg(long)]
    append_crc: bool,
    /// Check and remove RX CRC.
    #[arg(long)]
    check_crc: bool,
    /// Select the NFC-A tag before each frame.
    #[arg(long)]
    select: bool,
    /// NFC-A response timeout in milliseconds; default 100.
    #[arg(long, value_parser = clap::value_parser!(u16).range(1..=5000))]
    timeout_ms: Option<u16>,
    /// Keep the RF field and tag state after success; end with hf field off.
    #[arg(short = 'k', long)]
    keep: bool,
}

#[derive(Args)]
pub struct VicinityOptions {
    /// Frame bytes without CRC. Quote spaces or supply separate hexadecimal words.
    #[arg(value_name = "HEX", num_args = 1.., required_unless_present = "file", conflicts_with = "file")]
    data: Vec<String>,
    /// One hexadecimal frame per line, without CRC. # starts a comment.
    #[arg(long)]
    file: Option<PathBuf>,
    /// Keep the RF field and tag state after success; end with hf field off.
    #[arg(short = 'k', long)]
    keep: bool,
}

impl From<VicinityOptions> for Options {
    fn from(options: VicinityOptions) -> Self {
        Self {
            data: options.data,
            file: options.file,
            keep: options.keep,
            ..Self::default()
        }
    }
}

fn frame(line: &str, bits: Option<u16>, maximum: usize) -> Result<(Vec<u8>, u16)> {
    let (text, suffix) = line
        .split_once('/')
        .map_or((line, None), |(text, count)| (text, Some(count)));
    if suffix.is_some() && bits.is_some() {
        return Err("Specify the bit count once".into());
    }
    let bytes = crate::uid::hex_bytes(text, &(1..=maximum).collect::<Vec<_>>())?;
    let bits = suffix
        .map(str::parse::<u16>)
        .transpose()?
        .or(bits)
        .unwrap_or(bytes.len() as u16 * 8);
    if bits == 0 || usize::from(bits).div_ceil(8) != bytes.len() {
        return Err("TX bit count must describe exactly the supplied bytes".into());
    }
    Ok((bytes, bits))
}

pub fn run(
    options: Options,
    vicinity: bool,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
) -> Result<()> {
    if vicinity && (options.bits.is_some() || options.select || options.timeout_ms.is_some()) {
        return Err("NFC-V raw uses whole bytes, automatic CRC and bounded firmware timing".into());
    }
    if options.file.is_some() && options.bits.is_some() {
        return Err("Use a /BITS suffix on individual file frames".into());
    }
    let maximum = if vicinity { 22 } else { 62 };
    let frames: Vec<_> = if let Some(path) = options.file {
        let mut text = String::new();
        std::fs::File::open(path)?
            .take(16385)
            .read_to_string(&mut text)?;
        if text.len() > 16384 {
            return Err("Raw sequence exceeds 16 KiB".into());
        }
        text.lines()
            .map(|line| line.split('#').next().unwrap().trim())
            .filter(|line| !line.is_empty())
            .map(|line| frame(line, None, maximum))
            .collect::<Result<_>>()?
    } else {
        vec![frame(&options.data.join(" "), options.bits, maximum)?]
    };
    let limit = if vicinity { 12 } else { 256 };
    if frames.is_empty() || frames.len() > limit {
        return Err(format!("A raw sequence requires 1..{limit} frames").into());
    }
    let append_crc = options.crc || options.append_crc;
    for (bytes, bits) in &frames {
        if vicinity && usize::from(*bits) != bytes.len() * 8
            || !vicinity && append_crc && !bits.is_multiple_of(8)
        {
            return Err("CRC and NFC-V require complete bytes".into());
        }
    }
    let mut client = Client::connect(port, cancelled)?;
    let mut reader = HfReader::begin(&mut client)?;
    let color = crate::ui::stdout_color();
    crate::ui::line(
        if vicinity { "NFC-V raw" } else { "NFC-A raw" },
        &[format!("{} frames", frames.len())],
    );
    let result = (|| -> Result<()> {
        if vicinity {
            let replies = reader.raw_nfcv_with_field(
                &frames
                    .iter()
                    .map(|(bytes, _)| bytes.clone())
                    .collect::<Vec<_>>(),
                options.keep,
            )?;
            for (index, (status, reply)) in replies.iter().enumerate() {
                println!(
                    "{:03} {} {}",
                    index + 1,
                    crate::ui::paint("TX", "94", color),
                    nfc_native::formats::flipper::hex(&frames[index].0)
                );
                println!(
                    "    {} {}",
                    crate::ui::paint("RX", crate::ui::OK, color),
                    nfc_native::formats::flipper::hex(reply)
                );
                if *status != 0 {
                    return Err(DeviceError(*status).into());
                }
            }
        } else {
            let started = Instant::now();
            for (index, (bytes, bits)) in frames.iter().enumerate() {
                if started.elapsed() >= Duration::from_secs(30) {
                    return Err("Raw sequence exceeded 30 seconds; RF cleanup requested".into());
                }
                println!(
                    "{:03} {} {} ({bits} bits)",
                    index + 1,
                    crate::ui::paint("TX", "94", color),
                    nfc_native::formats::flipper::hex(bytes)
                );
                let (status, reply) = reader.raw_nfca(
                    bytes,
                    *bits,
                    append_crc,
                    options.crc || options.check_crc,
                    options.select,
                    options.timeout_ms.unwrap_or(100),
                )?;
                println!(
                    "    {} {}",
                    crate::ui::paint("RX", crate::ui::OK, color),
                    nfc_native::formats::flipper::hex(&reply)
                );
                if status != 0 {
                    return Err(DeviceError(status).into());
                }
            }
        }
        Ok(())
    })();
    if result.is_ok() && options.keep && !reader.cancelled() {
        reader.keep_field()?;
        crate::ui::message("RF field kept on; use hf field off to stop it", "0");
        return Ok(());
    }
    let result = result.and_then(|()| {
        if reader.cancelled() {
            Err(chameleon_client::RequestCancelled.into())
        } else {
            Ok(())
        }
    });
    let cleanup = reader.finish();
    match (result, cleanup) {
        (Err(error), Err(cleanup)) => {
            Err(format!("{error}; RF cleanup unconfirmed: {cleanup}").into())
        }
        (Err(error), _) => Err(error),
        (Ok(()), cleanup) => cleanup,
    }
}

#[derive(clap::Subcommand)]
pub enum Field {
    /// Turn off a retained NFC-A or NFC-V reader field.
    Off,
}

pub fn field_off(port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    Client::connect(port, cancelled)?.field_off()?;
    crate::ui::message("RF field off", "0");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn frames_keep_explicit_bits_without_guessing_protocol_commands() {
        assert_eq!(frame("26/7", None, 62).unwrap(), (vec![0x26], 7));
        assert_eq!(frame("93 20", None, 62).unwrap(), (vec![0x93, 0x20], 16));
        assert!(frame("26/0", None, 62).is_err());
        assert!(frame("26/9", None, 62).is_err());
        assert!(frame("26/7", Some(7), 62).is_err());
        assert!(frame("not hex", None, 62).is_err());
    }
}
