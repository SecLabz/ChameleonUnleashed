//! Assisted LF workflows; serial operations and field contracts live in the client.
use crate::{selection, ui};
use chameleon_client::{
    Client, HID_FORMATS, LfChip, LfNotFound, LfTag, LfType, Result, hid_payload,
};
use clap::{
    Args, Subcommand,
    builder::{PossibleValuesParser, TypedValueParser},
};
use std::sync::{Arc, atomic::AtomicBool};

#[derive(Args)]
pub struct Options {
    #[command(subcommand)]
    command: Option<Command>,
}
#[derive(Subcommand)]
enum Command {
    /// Identify an LF tag, its ID and its T55xx chip when detectable.
    Search(Inspect),
    /// Read a chosen family; omit TYPE to choose in the console.
    Read(Read),
    /// Copy a scanned ID to another tag, with target checks and verification.
    Copy(Copy),
    /// Program an ID onto T55xx, preserving its password and verifying the result.
    Write(Write),
}
#[derive(Args, Default)]
struct Inspect {
    /// Show T55xx configuration and readable blocks.
    #[arg(long)]
    verbose: bool,
}
#[derive(Args, Default, Clone)]
struct Programming {
    /// Check the target and preview changes without writing.
    #[arg(long)]
    dry_run: bool,
    /// Confirm the prepared write without prompting.
    #[arg(long)]
    yes: bool,
}
#[derive(Args, Default)]
struct Copy {
    /// Use the last successful LF read instead of scanning a source.
    #[arg(long)]
    last: bool,
    #[command(flatten)]
    programming: Programming,
}
#[derive(Args, Default)]
struct Read {
    #[command(flatten)]
    inspect: Inspect,
    /// LF family; choose interactively when omitted.
    #[arg(value_name = "TYPE", value_parser = type_parser(LfType::READABLE))]
    kind: Option<LfType>,
    /// Optional HID format hint (for example H10301).
    #[arg(long, value_parser = format_parser(), ignore_case = true)]
    format: Option<String>,
}
#[derive(Args, Default)]
struct Write {
    /// LF family; choose interactively when omitted.
    #[arg(value_name = "TYPE", value_parser = type_parser(LfType::WRITABLE))]
    kind: Option<LfType>,
    /// Hex ID, or eight ASCII characters for PAC.
    #[arg(long)]
    id: Option<String>,
    /// HID format name, for example H10301.
    #[arg(long, value_parser = format_parser(), ignore_case = true)]
    format: Option<String>,
    /// Facility code (HID or ioProx).
    #[arg(long)]
    fc: Option<u32>,
    /// Card number (HID or ioProx).
    #[arg(long)]
    cn: Option<u64>,
    /// HID issue level.
    #[arg(long)]
    il: Option<u8>,
    /// HID OEM code.
    #[arg(long)]
    oem: Option<u16>,
    /// ioProx version.
    #[arg(long)]
    ver: Option<u8>,
    /// ioProx raw eight-byte frame.
    #[arg(long)]
    raw8: Option<String>,
    /// Use the last successful LF read in this console.
    #[arg(long, conflicts_with_all = ["kind", "id", "format", "fc", "cn", "il", "oem", "ver", "raw8"])]
    last: bool,
    #[command(flatten)]
    programming: Programming,
}
fn type_parser(types: &'static [LfType]) -> impl TypedValueParser<Value = LfType> {
    PossibleValuesParser::new(types.iter().map(|kind| kind.name()))
        .map(|value| LfType::parse(&value).expect("declared LF family"))
}

fn format_parser() -> PossibleValuesParser {
    PossibleValuesParser::new(HID_FORMATS.iter().map(|entry| entry.0))
}
fn choose_type(
    kind: Option<LfType>,
    choices: &[LfType],
    title: &str,
    selected: &mut usize,
    allowed: bool,
) -> Result<LfType> {
    if let Some(kind) = kind {
        if !choices.contains(&kind) {
            return Err(format!("{} is not supported for this operation", kind.name()).into());
        }
        return Ok(kind);
    }
    *selected = selection::choose_current(
        title,
        &choices
            .iter()
            .map(|kind| kind.name().into())
            .collect::<Vec<_>>(),
        *selected,
        allowed,
    )?;
    Ok(choices[*selected])
}
fn hex(value: &str) -> Result<Vec<u8>> {
    if !value.len().is_multiple_of(2) || !value.is_ascii() {
        return Err("Expected an even number of hexadecimal digits".into());
    }
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair)?, 16).map_err(Into::into))
        .collect()
}
fn field<T>(
    value: Option<String>,
    title: &str,
    label: &str,
    hint: &str,
    allowed: bool,
    parse: impl Fn(&str) -> Result<T>,
) -> Result<T> {
    match value {
        Some(value) => parse(&value),
        None => selection::input(title, label, hint, "", allowed, parse),
    }
}

fn number<T: std::str::FromStr + ToString>(
    value: Option<T>,
    title: &str,
    label: &str,
    allowed: bool,
) -> Result<T> {
    field(
        value.map(|value| value.to_string()),
        title,
        label,
        "Decimal integer",
        allowed,
        |value| value.parse().map_err(|_| format!("Invalid {label}").into()),
    )
}

fn bytes_text(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02X}")).collect()
}
pub(crate) fn tag_fields(tag: &LfTag) -> Vec<(&'static str, String)> {
    let mut fields = Vec::new();
    match tag.kind {
        LfType::Hid => {
            fields.push((
                "Format",
                HID_FORMATS
                    .iter()
                    .find(|(_, id, _)| *id == tag.data[0])
                    .map_or("Unknown", |v| v.0)
                    .into(),
            ));
            fields.push((
                "Facility",
                u32::from_be_bytes(tag.data[1..5].try_into().unwrap()).to_string(),
            ));
            fields.push((
                "Card number",
                u64::from_be_bytes([
                    0,
                    0,
                    0,
                    tag.data[5],
                    tag.data[6],
                    tag.data[7],
                    tag.data[8],
                    tag.data[9],
                ])
                .to_string(),
            ));
            fields.push(("Issue level", tag.data[10].to_string()));
            fields.push((
                "OEM",
                u16::from_be_bytes(tag.data[11..13].try_into().unwrap()).to_string(),
            ));
        }
        LfType::Ioprox => {
            fields.extend([
                ("Version", tag.data[0].to_string()),
                ("Facility", tag.data[1].to_string()),
                (
                    "Card number",
                    u16::from_be_bytes([tag.data[2], tag.data[3]]).to_string(),
                ),
                ("Raw frame", bytes_text(&tag.data[4..12])),
            ]);
        }
        LfType::Pac => fields.push(("ID", String::from_utf8_lossy(&tag.data).into_owned())),
        LfType::Em4x05 => {
            fields.extend([
                ("Configuration", bytes_text(&tag.data[..4])),
                ("UID", bytes_text(&tag.data[4..8])),
                ("UID high", bytes_text(&tag.data[8..12])),
                ("EM4x69", (tag.data[12] != 0).to_string()),
            ]);
        }
        _ => fields.push(("ID", bytes_text(&tag.data))),
    }
    if let Some(clock) = match tag.variant {
        Some(101) => Some(16),
        Some(102) => Some(32),
        Some(103) => Some(64),
        _ => None,
    } {
        fields.push(("Clock", format!("RF/{clock}")));
    }
    fields
}

fn report(tag: &LfTag) {
    let fields = tag_fields(tag);
    for (index, group) in fields.chunks(3).enumerate() {
        ui::line(
            if index == 0 { tag.kind.name() } else { "" },
            &group
                .iter()
                .map(|(name, value)| format!("{name} {value}"))
                .collect::<Vec<_>>(),
        );
    }
    if matches!(tag.variant, Some(101 | 102)) {
        ui::warning(
            "Current slot emulation uses clock/64. T55xx programming preserves the detected clock",
        );
    }
}
// All prompts and local validation precede connection and mode changes.
enum Prepared {
    Tag(LfTag),
    Ioprox { raw: bool, input: Vec<u8> },
}
fn prepare(options: Write, last: &Option<LfTag>, allowed: bool) -> Result<Prepared> {
    if options.last {
        return Ok(Prepared::Tag(
            last.as_ref()
                .ok_or("No successful LF read in this console")?
                .for_t55xx()?,
        ));
    }
    let has_fields = options.kind.is_some()
        || options.id.is_some()
        || options.format.is_some()
        || options.fc.is_some()
        || options.cn.is_some()
        || options.il.is_some()
        || options.oem.is_some()
        || options.ver.is_some()
        || options.raw8.is_some();
    let reusable = !has_fields
        && allowed
        && last
            .as_ref()
            .is_some_and(|tag| LfType::WRITABLE.contains(&tag.kind));
    let mut selected = 0;
    loop {
        if reusable {
            let tag = last.as_ref().unwrap();
            if selection::choose(
                "LF · Write ID · Source",
                &[
                    format!(
                        "Last read · {} · {}",
                        tag.kind.name(),
                        bytes_text(&tag.data)
                    ),
                    "Enter an ID".into(),
                ],
                allowed,
            )? == 0
            {
                return Ok(Prepared::Tag(tag.for_t55xx()?));
            }
        }
        let result = (|| {
            loop {
                let kind = choose_type(
                    options.kind,
                    LfType::WRITABLE,
                    "LF · Write ID · Choose family",
                    &mut selected,
                    allowed,
                )?;
                match prepare_type(&options, kind, allowed) {
                    Err(error) if error.is::<selection::Back>() && options.kind.is_none() => {
                        continue;
                    }
                    result => return result,
                }
            }
        })();
        match result {
            Err(error) if error.is::<selection::Back>() && reusable => continue,
            result => return result,
        }
    }
}

fn prepare_type(options: &Write, kind: LfType, allowed: bool) -> Result<Prepared> {
    if kind != LfType::Hid
        && (options.format.is_some() || options.il.is_some() || options.oem.is_some())
    {
        return Err("--format, --il and --oem apply only to HID".into());
    }
    if kind != LfType::Ioprox && (options.ver.is_some() || options.raw8.is_some()) {
        return Err("--ver and --raw8 apply only to ioProx".into());
    }
    if !matches!(kind, LfType::Hid | LfType::Ioprox)
        && (options.fc.is_some() || options.cn.is_some())
    {
        return Err("--fc and --cn apply only to HID or ioProx".into());
    }
    if matches!(kind, LfType::Hid | LfType::Ioprox) && options.id.is_some() {
        return Err("Use format fields instead of --id for HID or ioProx".into());
    }
    let title = format!("LF · Write ID · {}", kind.name());
    if kind == LfType::Ioprox {
        let (raw, input) = if let Some(value) = &options.raw8 {
            if options.fc.is_some() || options.cn.is_some() || options.ver.is_some() {
                return Err("--raw8 conflicts with --ver, --fc and --cn".into());
            }
            let input = hex(value)?;
            if input.len() != 8 {
                return Err("--raw8 needs 16 hexadecimal digits".into());
            }
            chameleon_client::ioprox_fields(&input)?;
            (true, input)
        } else {
            let ver: u8 = number(
                options.ver.or(if allowed { None } else { Some(1) }),
                &title,
                "Version",
                allowed,
            )?;
            let fc = field(
                options.fc.map(|value| value.to_string()),
                &title,
                "Facility code",
                "Decimal · 0–255",
                allowed,
                |value| {
                    value
                        .parse::<u8>()
                        .map_err(|_| "ioProx facility code must be within 0..=255".into())
                },
            )?;
            let cn = field(
                options.cn.map(|value| value.to_string()),
                &title,
                "Card number",
                "Decimal · 0–65535",
                allowed,
                |value| {
                    value
                        .parse::<u16>()
                        .map_err(|_| "ioProx card number must be within 0..=65535".into())
                },
            )?;
            (false, vec![ver, fc, (cn >> 8) as u8, cn as u8])
        };
        return Ok(Prepared::Ioprox { raw, input });
    }
    let data = if kind == LfType::Hid {
        loop {
            let format = match &options.format {
                Some(value) => value.clone(),
                None => HID_FORMATS[selection::choose(
                    "LF · Write ID · HID format",
                    &HID_FORMATS
                        .iter()
                        .map(|value| value.0.into())
                        .collect::<Vec<_>>(),
                    allowed,
                )?]
                .0
                .into(),
            };
            match hid_fields(options, &format, allowed) {
                Err(error) if error.is::<selection::Back>() && options.format.is_none() => continue,
                result => break result?,
            }
        }
    } else {
        field(
            options.id.clone(),
            &title,
            "ID",
            if kind == LfType::Pac {
                "Eight ASCII characters"
            } else {
                "Hexadecimal ID"
            },
            allowed,
            |value| {
                let mut data = if kind == LfType::Pac {
                    value.as_bytes().to_vec()
                } else {
                    hex(value)?
                };
                if kind == LfType::Idteck && data.len() == 4 {
                    data.splice(..0, *b"IDTK");
                }
                LfTag {
                    kind,
                    data: data.clone(),
                    variant: None,
                }
                .validate()?;
                Ok(data)
            },
        )?
    };
    let tag = LfTag {
        kind,
        data,
        variant: None,
    };
    tag.validate()?;
    Ok(Prepared::Tag(tag))
}

fn hid_fields(options: &Write, format: &str, allowed: bool) -> Result<Vec<u8>> {
    let limits = HID_FORMATS
        .iter()
        .find(|value| value.0.eq_ignore_ascii_case(format))
        .ok_or("Unknown HID format")?
        .2;
    let title = format!("LF · Write ID · HID {format}");
    let fc = if limits[0] == 0 {
        options.fc.unwrap_or(0)
    } else {
        field(
            options.fc.map(|value| value.to_string()),
            &title,
            "Facility code",
            &format!("Decimal · {} bits", limits[0]),
            allowed,
            |value| {
                let fc = value.parse().map_err(|_| "Invalid facility code")?;
                hid_payload(format, fc, 0, 0, 0)?;
                Ok(fc)
            },
        )?
    };
    let cn = field(
        options.cn.map(|value| value.to_string()),
        &title,
        "Card number",
        &format!("Decimal · {} bits", limits[1]),
        allowed,
        |value| {
            let cn = value.parse().map_err(|_| "Invalid card number")?;
            hid_payload(format, fc, cn, 0, 0)?;
            Ok(cn)
        },
    )?;
    let il = if allowed && limits[2] != 0 {
        field(
            options.il.map(|value| value.to_string()),
            &title,
            "Issue level",
            &format!("Decimal · {} bits", limits[2]),
            allowed,
            |value| {
                let il = value.parse().map_err(|_| "Invalid issue level")?;
                hid_payload(format, fc, cn, il, 0)?;
                Ok(il)
            },
        )?
    } else {
        options.il.unwrap_or(0)
    };
    let oem = if allowed && limits[3] != 0 {
        field(
            options.oem.map(|value| value.to_string()),
            &title,
            "OEM",
            &format!("Decimal · {} bits", limits[3]),
            allowed,
            |value| {
                let oem = value.parse().map_err(|_| "Invalid OEM")?;
                hid_payload(format, fc, cn, il, oem)?;
                Ok(oem)
            },
        )?
    } else {
        options.oem.unwrap_or(0)
    };
    hid_payload(format, fc, cn, il, oem)
}

fn report_chip(chip: Option<&LfChip>, verbose: bool) {
    let Some(chip) = chip else {
        ui::line("LF chip", &["Unconfirmed".into()]);
        ui::line(
            "Writing",
            &["Unknown; detection may fail on protected tags".into()],
        );
        return;
    };
    let fields = [
        (
            "Protection",
            if chip.password_enabled() {
                "Password enabled; programming unsupported"
            } else {
                "Password disabled"
            }
            .into(),
        ),
        (
            "Writing",
            if chip.programming_supported() {
                "Candidate; checked again before programming"
            } else {
                "Unsupported configuration"
            }
            .into(),
        ),
    ];
    ui::line(
        "T55x7",
        &[format!("{} · RF/{}", chip.modulation(), chip.clock())],
    );
    for (name, value) in fields {
        ui::line(name, &[value]);
    }
    if verbose {
        println!("\n32-bit words · MSB first");
        for (index, word) in chip.blocks().iter().enumerate() {
            println!(
                "{}",
                ui::word_row(
                    &format!("{index:03}"),
                    *word,
                    if index == 0 { "Config" } else { "" },
                    ui::stdout_color()
                )
            );
        }
    }
}

fn program(client: &mut Client, tag: &LfTag, options: &Programming, allowed: bool) -> Result<()> {
    client.with_lf_reader(|client| {
        let plan = client.lf_t55xx_prepare(tag)?;
        report_chip(Some(&plan.chip), false);
        ui::line(
            "T55xx programming",
            &[
                format!("{} blocks to write", plan.changes()),
                "password preserved".into(),
            ],
        );
        if plan.changes() == 0 {
            ui::no_write("Unchanged");
            return Ok(());
        }
        if options.dry_run {
            ui::no_write("Preview");
            return Ok(());
        }
        if !selection::confirm(
            "Keep the target on the antenna. Program and verify this ID?",
            options.yes,
            allowed,
        )? {
            return Ok(());
        }
        client.lf_t55xx_program(&plan)?;
        ui::result(
            true,
            format!("Written and verified · {} blocks", plan.changes()),
        );
        Ok(())
    })
}

pub fn run(
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    allowed: bool,
    last: &mut Option<LfTag>,
) -> Result<()> {
    if let Some(command) = options.command {
        return execute(command, port, cancelled, allowed, last);
    }
    let mut selected = 0;
    loop {
        selected = selection::choose_current(
            "LF · Choose action",
            &[
                "Search".into(),
                "Copy to another tag…".into(),
                "Read a family…".into(),
                "Write an ID…".into(),
            ],
            selected,
            allowed,
        )?;
        let command = match selected {
            0 => Command::Search(Inspect::default()),
            1 => Command::Copy(Copy::default()),
            2 => Command::Read(Read::default()),
            _ => Command::Write(Write::default()),
        };
        match execute(command, port, cancelled.clone(), allowed, last) {
            Err(error) if error.is::<selection::Back>() => continue,
            result => return result,
        }
    }
}

fn execute(
    command: Command,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    allowed: bool,
    last: &mut Option<LfTag>,
) -> Result<()> {
    match command {
        Command::Write(options) => {
            let programming = options.programming.clone();
            let prepared = prepare(options, last, allowed)?;
            if !programming.yes && !programming.dry_run && !allowed {
                return Err(
                    "Writing requires --yes outside the console; use --dry-run to preview".into(),
                );
            }
            let mut client = Client::connect(port, cancelled)?;
            let tag = match prepared {
                Prepared::Tag(tag) => tag,
                Prepared::Ioprox { raw, input } => client.lf_ioprox(raw, &input)?,
            };
            report(&tag);
            program(&mut client, &tag, &programming, allowed)
        }
        Command::Copy(options) => {
            if !options.last && !allowed {
                return Err("Guided copying needs the interactive console. Use lf write TYPE --id ID --dry-run for an explicit source".into());
            }
            if !options.programming.yes && !options.programming.dry_run && !allowed {
                return Err("Writing requires --yes outside the console".into());
            }
            let cached = if options.last {
                Some(
                    last.as_ref()
                        .ok_or("No successful LF read in this console")?
                        .for_t55xx()?,
                )
            } else {
                None
            };
            let mut client = Client::connect(port, cancelled)?;
            loop {
                let tag = if let Some(tag) = &cached {
                    tag.clone()
                } else {
                    selection::pause(
                        "LF copy · 1/2 · Read source",
                        "Place the source tag on the antenna.",
                        allowed,
                    )?;
                    let tag = client.with_lf_reader(Client::lf_search)?;
                    *last = Some(tag.clone());
                    tag.for_t55xx()?
                };
                report(&tag);
                if allowed {
                    let next = selection::pause(
                        &format!(
                            "LF copy · 2/2 · Check target\nSource · {} · {}",
                            tag.kind.name(),
                            bytes_text(&tag.data)
                        ),
                        "Replace the source with the target tag.",
                        allowed,
                    );
                    match next {
                        Err(error) if error.is::<selection::Back>() && cached.is_none() => continue,
                        result => result?,
                    }
                }
                return program(&mut client, &tag, &options.programming, allowed);
            }
        }

        Command::Search(_) | Command::Read(_) => {
            let (read, inspect) = match command {
                Command::Read(options) => {
                    let kind = choose_type(
                        options.kind,
                        LfType::READABLE,
                        "LF · Read · Choose family",
                        &mut 0,
                        allowed,
                    )?;
                    if kind != LfType::Hid && options.format.is_some() {
                        return Err("--format applies only to HID".into());
                    }
                    let format = options
                        .format
                        .as_deref()
                        .map(|name| {
                            HID_FORMATS
                                .iter()
                                .find(|value| value.0.eq_ignore_ascii_case(name))
                                .map(|value| value.1)
                                .ok_or("Unknown HID format")
                        })
                        .transpose()?
                        .unwrap_or(0);
                    (Some((kind, format)), options.inspect)
                }
                Command::Search(inspect) => (None, inspect),
                _ => unreachable!(),
            };
            let mut client = Client::connect(port, cancelled)?;
            client.with_lf_reader(|client| {
                let result = match read {
                    Some((kind, format)) => client
                        .lf_read(kind, format)?
                        .ok_or_else(|| "No matching LF tag found".into()),
                    None => client.lf_search(),
                };
                let tag = match result {
                    Ok(tag) => Some(tag),
                    Err(error) if read.is_none() && error.is::<LfNotFound>() => None,
                    Err(error) => return Err(error),
                };
                let found_id = tag.is_some();
                if let Some(tag) = tag {
                    report(&tag);
                    *last = Some(tag);
                }
                if client.lf_t55xx_supported() {
                    let chip = client.lf_t55xx_inspect()?;
                    report_chip(chip.as_ref(), inspect.verbose);
                    if !found_id {
                        if chip.is_none() {
                            return Err(LfNotFound.into());
                        }
                        ui::warning("Chip detected; no supported broadcast ID decoded");
                    }
                } else {
                    if !found_id {
                        return Err(LfNotFound.into());
                    }
                    ui::warning(
                        "Chip unconfirmed: update firmware for T55xx detection and programming",
                    );
                }
                Ok(())
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cached_hid_source_is_normalized_before_confirmation_without_changing_the_read() {
        let mut data = hid_payload("C15001", 1, 2, 0, 0).unwrap();
        data[11..13].fill(0);
        let source = LfTag {
            kind: LfType::Hid,
            data,
            variant: None,
        };
        let last = Some(source.clone());
        let options = Write {
            last: true,
            programming: Programming {
                yes: true,
                ..Programming::default()
            },
            ..Write::default()
        };
        let Prepared::Tag(tag) = prepare(options, &last, false).unwrap() else {
            panic!("Cached reads are already decoded");
        };
        assert_eq!(&tag.data[11..13], &900u16.to_be_bytes());
        assert_eq!(last, Some(source));
    }
}
