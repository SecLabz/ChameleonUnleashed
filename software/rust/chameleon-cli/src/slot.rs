//! Slot workflows shared by direct commands and the assisted console.
use crate::{selection, slot_file, ui};
use chameleon_client::{Client, Result, SLOT_TYPES, Sense, Slot, type_name, type_sense};
use clap::{Args, Subcommand, ValueEnum};
mod command;
mod dump;
mod setup;
mod status;
pub use command::Slots;
use command::{Command, Prng, Target, WriteMode};
use setup::{settings_text, setup, setup_menu};
pub use status::run as status;
use std::{
    io::IsTerminal,
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};

fn label(side: &chameleon_client::SlotSide) -> String {
    let mut value = type_name(side.tag_type);
    if !side.name.is_empty() {
        value.push_str(&format!(" · {}", side.name));
    }
    if side.tag_type != 0 && !side.enabled {
        value.push_str(" · disabled");
    }
    value
}

fn identity_fields(image: &chameleon_client::SlotImage) -> Result<Vec<(&'static str, String)>> {
    image.validate()?;
    let mut fields = vec![("Bytes", image.memory.len().to_string())];
    if !image.identity.is_empty() {
        let bytes = &image.identity;
        let uid_bytes = usize::from(bytes[0]);
        fields.extend([
            ("UID", ui::uid_text(&bytes[1..1 + uid_bytes])),
            (
                "ATQA (RF)",
                nfc_native::formats::flipper::hex(&bytes[1 + uid_bytes..3 + uid_bytes]),
            ),
            ("SAK", format!("{:02X}", bytes[3 + uid_bytes])),
        ]);
        if bytes[4 + uid_bytes] != 0 {
            fields.push((
                "ATS",
                nfc_native::formats::flipper::hex(&bytes[5 + uid_bytes..]),
            ));
        }
    } else if image.tag_type == 1300 {
        fields.push((
            "UID",
            ui::uid_text(&chameleon_client::DesfireSnapshot::from_slot(image)?.uid),
        ));
    } else if image.sense()? == Sense::Hf {
        fields.push(("UID", ui::uid_text(&slot_file::to_image(image)?.uid)));
    } else {
        let name = if image.tag_type == 200 {
            "hid".into()
        } else {
            type_name(image.tag_type)
        };
        let tag = chameleon_client::LfTag {
            kind: chameleon_client::LfType::parse(&name)?,
            data: image.memory.clone(),
            variant: None,
        };
        fields.extend(crate::lf::tag_fields(&tag));
    }
    Ok(fields)
}
fn list(slots: &[Slot], active: u8) {
    let width = crossterm::terminal::size().map_or(80, |(width, _)| usize::from(width));
    print!("{}", list_text(slots, active, width, ui::stdout_color()));
}

fn slot_labels(slots: &[Slot], width: usize, color: bool) -> (String, Vec<String>) {
    use unicode_width::UnicodeWidthStr;
    let hf_width = slots
        .iter()
        .map(|slot| label(&slot.hf).width())
        .max()
        .unwrap_or(2)
        .max(2);
    let lf_width = slots
        .iter()
        .map(|slot| label(&slot.lf).width())
        .max()
        .unwrap_or(2)
        .max(2);
    let table = hf_width + lf_width + 9 <= width;
    let header = if table {
        format!("{:hf_width$}  LF", "HF")
    } else {
        String::new()
    };
    let labels = slots
        .iter()
        .map(|slot| {
            let hf = label(&slot.hf);
            let lf = label(&slot.lf);
            let padding = " ".repeat(hf_width - hf.width());
            let hf = ui::paint(hf, "90", color && !slot.hf.enabled);
            let lf = ui::paint(lf, "90", color && !slot.lf.enabled);
            if table {
                format!("{hf}{padding}  {lf}")
            } else {
                format!("HF {hf}\nLF {lf}")
            }
        })
        .collect();
    (header, labels)
}

fn list_text(slots: &[Slot], active: u8, width: usize, color: bool) -> String {
    use std::fmt::Write;
    let (header, labels) = slot_labels(slots, width, color);
    let mut text = String::new();
    if !header.is_empty() {
        writeln!(text, "  #  {header}").unwrap();
    }
    for (slot, label) in slots.iter().zip(labels) {
        let marker = ui::paint(if slot.number == active { "*" } else { " " }, "1;36", color);
        for (index, line) in label.lines().enumerate() {
            if index == 0 {
                writeln!(text, "{marker} {}  {line}", slot.number).unwrap();
            } else {
                writeln!(text, "     {line}").unwrap();
            }
        }
    }
    text
}

fn slot_menu(
    slots: &[Slot],
    active: Option<u8>,
    selected: u8,
    allowed: bool,
    usable: bool,
) -> Result<u8> {
    let width = crossterm::terminal::size().map_or(80, |(width, _)| usize::from(width));
    let (header, labels) = slot_labels(slots, width.saturating_sub(3), false);
    let choices: Vec<_> = slots
        .iter()
        .zip(labels)
        .map(|(slot, label)| {
            let marker = if active == Some(slot.number) {
                '*'
            } else {
                ' '
            };
            let label = label
                .lines()
                .enumerate()
                .map(|(index, line)| {
                    if index == 0 {
                        format!("{marker} {line}")
                    } else {
                        format!("  {line}")
                    }
                })
                .collect::<Vec<_>>()
                .join("\n");
            let mut choice = selection::Choice::numbered(usize::from(slot.number), label);
            choice.enabled = !usable
                || slot.hf.enabled && slot.hf.tag_type != 0
                || slot.lf.enabled && slot.lf.tag_type != 0;
            choice
        })
        .collect();
    let initial = slots
        .iter()
        .position(|slot| slot.number == selected)
        .unwrap_or(0);
    let title = if active.is_some() {
        "Slots · * Active slot"
    } else {
        "Choose slot"
    };
    let header = if header.is_empty() {
        header
    } else {
        format!("  #    {header}")
    };
    Ok(slots[selection::menu(title, &header, &choices, initial, allowed)?].number)
}

fn choose_slot(slots: &[Slot], number: Option<u8>, allowed: bool, usable: bool) -> Result<u8> {
    match number {
        Some(number) => Ok(number),
        None => slot_menu(slots, None, 1, allowed, usable),
    }
}

fn choose_sense(target: &Target, slot: &Slot, allowed: bool) -> Result<Sense> {
    if target.hf {
        return Ok(Sense::Hf);
    }
    if target.lf {
        return Ok(Sense::Lf);
    }
    match (slot.hf.tag_type != 0, slot.lf.tag_type != 0) {
        (true, false) => Ok(Sense::Hf),
        (false, true) => Ok(Sense::Lf),
        _ => Ok(
            if selection::choose(
                &format!("Slot {} · Choose HF or LF", slot.number),
                &[
                    format!("HF · {}", label(&slot.hf)),
                    format!("LF · {}", label(&slot.lf)),
                ],
                allowed,
            )? == 0
            {
                Sense::Hf
            } else {
                Sense::Lf
            },
        ),
    }
}
pub fn run(
    options: Slots,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    allowed: bool,
) -> Result<()> {
    let allowed = allowed
        && std::io::stdin().is_terminal()
        && std::io::stdout().is_terminal()
        && std::io::stderr().is_terminal()
        && std::env::var("TERM").is_ok_and(|term| term != "dumb");
    // File and script validation precede discovery or any mutation.
    let image = if let Some(Command::Load {
        file,
        name,
        chip_id,
        emulator_defaults,
        ..
    }) = &options.command
    {
        if let Some(name) = name {
            chameleon_client::validate_name(name)?;
        }
        Some(slot_file::load(
            file,
            *chip_id,
            *emulator_defaults,
            allowed,
        )?)
    } else {
        None
    };
    if let Some(Command::Export { output, .. }) = &options.command {
        if output.exists() {
            return Err("Output already exists".into());
        }
        if !matches!(
            output.extension().and_then(|s| s.to_str()),
            Some("json" | "nfc")
        ) {
            return Err("Choose a .json or .nfc output file".into());
        }
    }
    if let Some(Command::Setup {
        name: Some(name), ..
    }) = &options.command
    {
        chameleon_client::validate_name(name)?;
    }
    validate_script(options.command.as_ref(), allowed)?;
    let mut client = Client::connect(port, cancelled)?;
    if matches!(options.command, Some(Command::Stop)) {
        client.set_reader_mode(true)?;
        ui::result(true, "Emulation stopped · reader mode");
        return Ok(());
    }
    let slots = client.slots()?;
    let command = match options.command {
        Some(command) => command,
        None => {
            let active = client.active_slot()?;
            if !allowed {
                list(&slots, active);
                return Ok(());
            }
            return browse(&mut client, &slots, active, allowed);
        }
    };
    execute(command, image, &mut client, &slots, allowed)
}
fn image_path(
    title: &str,
    output: bool,
    allowed: bool,
) -> Result<(PathBuf, Option<chameleon_client::SlotImage>)> {
    selection::input(
        title,
        if output { "Output path" } else { "Image path" },
        ".json or .nfc · existing files are never overwritten",
        "",
        allowed,
        |value| {
            let path = PathBuf::from(value);
            if !matches!(
                path.extension().and_then(|extension| extension.to_str()),
                Some("json" | "nfc")
            ) {
                return Err("Choose a .json or .nfc file".into());
            }
            if output {
                if path.exists() {
                    return Err("Output already exists".into());
                }
                let parent = path
                    .parent()
                    .filter(|parent| !parent.as_os_str().is_empty())
                    .unwrap_or(std::path::Path::new("."));
                if !parent.is_dir() {
                    return Err("Output directory does not exist".into());
                }
            } else if !path.is_file() {
                return Err("Image file does not exist".into());
            }
            let image = if output {
                None
            } else {
                Some(slot_file::load(&path, None, false, allowed)?)
            };
            Ok((path, image))
        },
    )
}

fn browse(client: &mut Client, slots: &[Slot], active: u8, allowed: bool) -> Result<()> {
    let mut number = active;
    loop {
        number = slot_menu(slots, Some(active), number, allowed, false)?;
        let slot = &slots[(number - 1) as usize];
        let title = format!(
            "Slot {number} · Actions\nHF {}\nLF {}",
            label(&slot.hf),
            label(&slot.lf)
        );
        let has_image = slot.hf.tag_type != 0 || slot.lf.tag_type != 0;
        let can_start =
            slot.hf.tag_type != 0 && slot.hf.enabled || slot.lf.tag_type != 0 && slot.lf.enabled;
        let mut selected = 0;
        loop {
            let actions = [
                "Show information",
                "Show dump",
                "Start emulation",
                "Load image…",
                "Export image…",
                "Configure…",
                "Clear…",
                "Initialize…",
            ];
            let mut choices: Vec<_> = actions
                .iter()
                .enumerate()
                .map(|(index, label)| selection::Choice::numbered(index + 1, (*label).into()))
                .collect();
            choices[2].enabled = can_start;
            for index in [1, 4, 5, 6] {
                choices[index].enabled = has_image;
            }
            selected = match selection::menu(&title, "", &choices, selected, allowed) {
                Ok(index) => index,
                Err(error) if error.is::<selection::Back>() => break,
                Err(error) => return Err(error),
            };
            let command = (|| -> Result<(Command, Option<chameleon_client::SlotImage>)> {
                if selected == 3 {
                    let (file, image) = image_path(&title, false, allowed)?;
                    return Ok((
                        Command::Load {
                            slot: Some(number),
                            file,
                            name: None,
                            chip_id: None,
                            emulator_defaults: false,
                            yes: false,
                        },
                        image,
                    ));
                }
                let command = match selected {
                    0 => Command::Info { slot: Some(number) },
                    1 => Command::Dump {
                        target: Target {
                            slot: Some(number),
                            ..Default::default()
                        },
                    },
                    2 => Command::Start { slot: Some(number) },
                    4 => Command::Export {
                        target: Target {
                            slot: Some(number),
                            ..Default::default()
                        },
                        output: image_path(&title, true, allowed)?.0,
                    },
                    5 => setup_menu(client, slots, number, allowed, Target::default())?,
                    6 => Command::Clear {
                        target: Target {
                            slot: Some(number),
                            ..Default::default()
                        },
                        yes: false,
                    },
                    _ => Command::Init {
                        slot: Some(number),
                        model: None,
                        yes: false,
                    },
                };
                Ok((command, None))
            })();
            let result = command
                .and_then(|(command, image)| execute(command, image, client, slots, allowed));
            match result {
                Err(error) if error.is::<selection::Back>() => continue,
                result => return result,
            }
        }
    }
}

fn validate_script(command: Option<&Command>, allowed: bool) -> Result<()> {
    if allowed {
        return Ok(());
    }
    let number = match command {
        None | Some(Command::List | Command::Stop) => return Ok(()),
        Some(
            Command::Info { slot }
            | Command::Start { slot }
            | Command::Init { slot, .. }
            | Command::Load { slot, .. },
        ) => *slot,
        Some(
            Command::Dump { target }
            | Command::Export { target, .. }
            | Command::Setup { target, .. }
            | Command::Clear { target, .. },
        ) => target.slot,
    };
    if matches!(command, Some(Command::Clear { yes: false, .. })) {
        return Err("Clearing requires --yes outside the console".into());
    }
    if number.is_none() {
        return Err("Slot number is required outside the console".into());
    }
    if matches!(command, Some(Command::Init { model: None, .. })) {
        return Err("--type is required outside the console".into());
    }
    Ok(())
}
fn execute(
    command: Command,
    image: Option<chameleon_client::SlotImage>,
    client: &mut Client,
    slots: &[Slot],
    allowed: bool,
) -> Result<()> {
    match command {
        Command::List => list(slots, client.active_slot()?),
        Command::Info { slot } => {
            let number = choose_slot(slots, slot, allowed, false)?;
            let slot = &slots[(number - 1) as usize];
            ui::line(&format!("Slot {number}"), &[]);
            let inspect = client.supports_slot_snapshot() || client.reader_mode()?;
            for (sense, title) in [(Sense::Hf, "HF"), (Sense::Lf, "LF")] {
                let side = slot.side(sense);
                println!();
                ui::line(title, &[label(side)]);
                if !inspect || !SLOT_TYPES.iter().any(|(id, _)| *id == side.tag_type) {
                    continue;
                }
                let image = client.read_slot_image(number, sense)?;
                let fields = identity_fields(&image)?;
                let width = fields
                    .iter()
                    .map(|(name, _)| name.len())
                    .max()
                    .unwrap_or(0)
                    .max(10);
                for (name, value) in fields {
                    if !value.is_empty() {
                        println!(
                            "  {}  {value}",
                            ui::paint(format!("{name:width$}"), "2", ui::stdout_color())
                        );
                    }
                }
                print!("{}", settings_text(&image, ui::stdout_color()));
            }
            if !inspect {
                ui::message(
                    format!(
                        "Emulation active · summary only. Run slot stop, then slot info {number} for identity and settings"
                    ),
                    ui::ACCENT,
                );
            }
        }
        Command::Dump { target } => {
            let number = choose_slot(slots, target.slot, allowed, false)?;
            let slot = &slots[(number - 1) as usize];
            let sense = choose_sense(&target, slot, allowed)?;
            let side = slot.side(sense);
            if side.tag_type == 0 {
                return Err(format!("Slot {number} {sense:?} is empty").into());
            }
            if !client.supports_slot_snapshot() && !client.reader_mode()? {
                let flag = if sense == Sense::Hf { "--hf" } else { "--lf" };
                return Err(format!(
                    "Emulation active · run \"slot stop\", then \"slot dump {number} {flag}\""
                )
                .into());
            }
            let image = client.read_slot_image(number, sense)?;
            let text = dump::text(&image, ui::stdout_color())?;
            ui::line(
                &format!("Slot {number}"),
                &[format!("{sense:?} · {}", label(side))],
            );
            print!("{text}");
        }
        Command::Start { slot } => {
            let number = choose_slot(slots, slot, allowed, true)?;
            let slot = &slots[(number - 1) as usize];
            if !(slot.hf.enabled && slot.hf.tag_type != 0
                || slot.lf.enabled && slot.lf.tag_type != 0)
            {
                return Err("Selected slot has no enabled images".into());
            }
            if !client.reader_mode()? {
                client.set_reader_mode(true)?;
            }
            client.select_slot(number)?;
            client.set_reader_mode(false)?;
            ui::result(true, format!("Emulating · slot {number}"));
            ui::line("HF", &[label(&slot.hf)]);
            ui::line("LF", &[label(&slot.lf)]);
            println!("Emulation continues after exit; keep the device powered.");
        }
        Command::Load {
            file,
            slot,
            name,
            chip_id,
            emulator_defaults,
            yes,
        } => {
            let image = match image {
                Some(image) => image,
                None => slot_file::load(&file, chip_id, emulator_defaults, allowed)?,
            };
            let number = choose_slot(slots, slot, allowed, false)?;
            save_image(client, slots, number, &image, name.as_deref(), yes, allowed)?;
        }
        Command::Export { target, output } => {
            let number = choose_slot(slots, target.slot, allowed, false)?;
            let sense = choose_sense(&target, &slots[(number - 1) as usize], allowed)?;
            let image = client.read_slot_image(number, sense)?;
            slot_file::export(&image, &output)?;
            ui::line("Exported", &[output.display().to_string()]);
        }
        Command::Clear { target, yes } => {
            let number = choose_slot(slots, target.slot, allowed, false)?;
            let sense = choose_sense(&target, &slots[(number - 1) as usize], allowed)?;
            if !selection::confirm(&format!("Clear slot {number} {sense:?}?"), yes, allowed)? {
                return Ok(());
            }
            client.clear_slot(number, sense)?;
            ui::result(true, format!("Slot {number} {sense:?} cleared"));
        }
        Command::Init { slot, model, yes } => {
            let number = choose_slot(slots, slot, allowed, false)?;
            let id = match model {
                Some(id) => id,
                None => {
                    SLOT_TYPES[selection::choose(
                        &format!("Slot {number} · Initialize · Choose model"),
                        &SLOT_TYPES
                            .iter()
                            .map(|(_, s)| (*s).into())
                            .collect::<Vec<_>>(),
                        allowed,
                    )?]
                    .0
                }
            };
            let sense = type_sense(id)?;
            if slots[(number - 1) as usize].side(sense).tag_type != 0
                && !selection::confirm(
                    &format!(
                        "Replace slot {number} {sense:?} with {} defaults?",
                        type_name(id)
                    ),
                    yes,
                    allowed,
                )?
            {
                return Ok(());
            }
            client.initialize_slot(number, id)?;
            ui::result(
                true,
                format!("Slot {number} initialized · {}", type_name(id)),
            );
        }
        command @ Command::Setup { .. } => setup(command, client, slots, allowed)?,
        Command::Stop => unreachable!(),
    }
    Ok(())
}

pub(crate) fn save_image(
    client: &mut Client,
    slots: &[Slot],
    number: u8,
    image: &chameleon_client::SlotImage,
    name: Option<&str>,
    yes: bool,
    allowed: bool,
) -> Result<()> {
    image.validate_load()?;
    if let Some(name) = name {
        chameleon_client::validate_name(name)?;
    }
    let sense = image.sense()?;
    let side = slots[(number - 1) as usize].side(sense);
    if side.tag_type != 0
        && !selection::confirm(
            &format!("Replace slot {number} {sense:?} · {}?", label(side)),
            yes,
            allowed,
        )?
    {
        return Ok(());
    }
    ui::warning(
        "Serial replacement is not atomic; an interrupted load can leave default or partial data",
    );
    client.load_slot_image(number, image)?;
    if let Some(name) = name {
        client.rename_slot(number, sense, name)?;
    }
    ui::result(
        true,
        format!(
            "Slot {number} saved and verified · {}",
            type_name(image.tag_type)
        ),
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::SlotSide;

    #[test]
    fn image_information_decodes_rf_activation_and_lf_fields_without_transport_headers() {
        let image = chameleon_client::SlotImage {
            version: 1,
            tag_type: 1001,
            identity: vec![7, 4, 1, 2, 3, 4, 5, 6, 0x44, 0, 8, 2, 2, 0],
            options: vec![0; 7],
            memory: vec![0; 1024],
        };
        let fields = identity_fields(&image).unwrap();
        assert!(fields.contains(&("UID", "04010203040506".into())));
        assert!(fields.contains(&("ATQA (RF)", "44 00".into())));
        assert!(fields.contains(&("SAK", "08".into())));
        assert!(fields.contains(&("ATS", "02 00".into())));
        assert!(!fields.iter().any(|(name, _)| *name == "Activation"));
        let mut invalid = image;
        invalid.identity.pop();
        assert!(identity_fields(&invalid).is_err());
        let mut image = chameleon_client::SlotImage {
            version: 1,
            tag_type: 100,
            identity: vec![],
            options: vec![],
            memory: vec![1, 2, 3, 4, 5],
        };
        assert!(
            identity_fields(&image)
                .unwrap()
                .contains(&("ID", "0102030405".into()))
        );
        image.tag_type = 200;
        image.memory = chameleon_client::hid_payload("H10301", 12, 345, 0, 0).unwrap();
        let fields = identity_fields(&image).unwrap();
        assert!(fields.contains(&("Format", "H10301".into())));
        assert!(fields.contains(&("Facility", "12".into())));
        assert!(fields.contains(&("Card number", "345".into())));
    }

    #[test]
    fn slot_columns_use_visible_width_and_narrow_layout_retains_full_names() {
        let slots = vec![
            Slot {
                number: 1,
                hf: SlotSide {
                    tag_type: 1001,
                    enabled: true,
                    name: "Badge entrée principale".into(),
                },
                lf: SlotSide {
                    tag_type: 100,
                    enabled: false,
                    name: "Porte".into(),
                },
            },
            Slot {
                number: 2,
                hf: SlotSide {
                    tag_type: 1101,
                    enabled: false,
                    name: "Test".into(),
                },
                lf: SlotSide {
                    tag_type: 0,
                    enabled: false,
                    name: String::new(),
                },
            },
        ];
        let plain = list_text(&slots, 1, 160, false);
        let colored = list_text(&slots, 1, 160, true);
        assert_eq!(ui::without_colors(&colored), plain);
        let lines: Vec<_> = plain.lines().collect();
        let lf_column = lines[0].find("LF").unwrap();
        assert_eq!(
            lines[1].chars().nth(lf_column),
            label(&slots[0].lf).chars().next()
        );
        assert!(lines[1].starts_with("* 1"));
        assert!(colored.contains("\x1b[90m"));
        let narrow = list_text(&slots, 1, 40, false);
        assert!(narrow.contains(&format!("HF {}\n", label(&slots[0].hf))));
        assert!(narrow.contains(&format!("LF {}\n", label(&slots[0].lf))));
    }
}
