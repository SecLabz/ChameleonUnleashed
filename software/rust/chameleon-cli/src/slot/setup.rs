//! Assisted configuration and explicit setup options.
use super::*;
pub(super) fn settings_text(image: &chameleon_client::SlotImage, color: bool) -> String {
    use std::fmt::Write;

    let title = ui::paint("Settings", "1;36", color);
    if image.tag_type == 1300 {
        return format!(
            "\n  {title}\n    Public data emulation; no authentication or secure messaging\n"
        );
    }
    let fields: Vec<_> = settings(image)
        .iter()
        .filter_map(|(name, index)| {
            let byte = *image.options.get(*index)?;
            let (value, code) = if matches!(*name, "Write mode" | "PRNG") {
                (
                    values(name)
                        .get(byte as usize)
                        .copied()
                        .unwrap_or("Unknown"),
                    "37",
                )
            } else if byte == 0 {
                ("Off", "90")
            } else {
                ("On", ui::ACCENT)
            };
            Some((*name, value, code))
        })
        .collect();
    if fields.is_empty() {
        return String::new();
    }
    let width = fields
        .iter()
        .map(|(name, _, _)| name.len())
        .max()
        .unwrap_or(0);
    let mut text = format!("\n  {title}\n");
    for (name, value, code) in fields {
        writeln!(
            text,
            "    {}  {}",
            ui::paint(format!("{name:width$}"), "2", color),
            ui::paint(value, code, color)
        )
        .unwrap();
    }
    text
}

pub(super) fn settings(image: &chameleon_client::SlotImage) -> &'static [(&'static str, usize)] {
    if (1000..=1003).contains(&image.tag_type) {
        &[
            ("Write mode", 0),
            ("PRNG", 6),
            ("Detection", 4),
            ("Gen1A", 1),
            ("Gen2", 2),
            ("Block identity", 3),
            ("Field reset", 5),
        ]
    } else if (1100..=1108).contains(&image.tag_type) {
        &[("Write mode", 1), ("Detection", 2), ("UID magic", 0)]
    } else {
        &[]
    }
}

pub(super) fn values(name: &str) -> &'static [&'static str] {
    match name {
        "Write mode" => &["Normal", "Denied", "Deceive", "Shadow"],
        "PRNG" => &["Static", "Weak", "Hard"],
        _ => &["Disabled", "Enabled"],
    }
}

pub(super) fn setup_menu(
    client: &mut Client,
    slots: &[Slot],
    number: u8,
    allowed: bool,
    target: Target,
) -> Result<Command> {
    let slot = &slots[(number - 1) as usize];
    let chooses_sense =
        !target.hf && !target.lf && (slot.hf.tag_type != 0) == (slot.lf.tag_type != 0);
    loop {
        let sense = choose_sense(&target, slot, allowed)?;
        match edit_side(client, slot, sense, allowed) {
            Err(error) if error.is::<selection::Back>() && chooses_sense => continue,
            result => return result,
        }
    }
}

fn edit_side(client: &mut Client, slot: &Slot, sense: Sense, allowed: bool) -> Result<Command> {
    let number = slot.number;
    let side = slot.side(sense);
    let title = format!("Slot {number} · {sense:?} {} · Configure", label(side));
    // Reading an image must not interrupt active emulation. Metadata-only actions
    // remain available; the user explicitly stops emulation to inspect settings.
    let supported =
        (1000..=1003).contains(&side.tag_type) || (1100..=1108).contains(&side.tag_type);
    let image = if supported && client.reader_mode()? {
        Some(client.read_slot_image(number, sense)?)
    } else {
        None
    };
    let fields = image.as_ref().map_or(&[][..], settings);
    let mut selected = 0;
    loop {
        let mut choices: Vec<_> = fields
            .iter()
            .map(|(name, index)| {
                let current = values(name)
                    .get(image.as_ref().unwrap().options[*index] as usize)
                    .copied()
                    .unwrap_or("Unknown");
                format!("{name:<18} {current}")
            })
            .collect();
        choices.push("Rename…".into());
        choices.push(if side.enabled { "Disable" } else { "Enable" }.into());
        let mut entries: Vec<_> = choices
            .into_iter()
            .enumerate()
            .map(|(index, label)| selection::Choice::numbered(index + 1, label))
            .collect();
        if supported && image.is_none() {
            let mut entry = selection::Choice::numbered(
                entries.len() + 1,
                "Emulator settings · slot stop first".into(),
            );
            entry.enabled = false;
            entries.push(entry);
        }
        selected = selection::menu(&title, "", &entries, selected, allowed)?;
        let mut command = Command::Setup {
            target: Target {
                slot: Some(number),
                hf: sense == Sense::Hf,
                lf: sense == Sense::Lf,
            },
            name: None,
            enabled: None,
            write_mode: None,
            prng: None,
            detection: None,
            gen1a: None,
            gen2: None,
            block_identity: None,
            field_reset: None,
            uid_magic: None,
        };
        let Command::Setup {
            name,
            enabled,
            write_mode,
            prng,
            detection,
            gen1a,
            gen2,
            block_identity,
            field_reset,
            uid_magic,
            ..
        } = &mut command
        else {
            unreachable!()
        };
        let edit = if selected == fields.len() {
            match selection::input(
                &title,
                "Name",
                "1–32 UTF-8 bytes · no control characters",
                &side.name,
                allowed,
                |value| {
                    chameleon_client::validate_name(value)?;
                    Ok(value.to_owned())
                },
            ) {
                Ok(value) if value == side.name => continue,
                Ok(value) => {
                    *name = Some(value);
                    Ok(())
                }
                Err(error) => Err(error),
            }
        } else if selected == fields.len() + 1 {
            *enabled = Some(!side.enabled);
            Ok(())
        } else {
            let (field, index) = fields[selected];
            let current = image.as_ref().unwrap().options[index] as usize;
            let choices = values(field)
                .iter()
                .map(|value| (*value).into())
                .collect::<Vec<_>>();
            match selection::choose_current(
                &format!(
                    "{title}\n{field} · current {}",
                    choices.get(current).map_or("Unknown", String::as_str)
                ),
                &choices,
                current,
                allowed,
            ) {
                Ok(value) if value == current => continue,
                Ok(value) => {
                    match field {
                        "Write mode" => {
                            *write_mode = Some(
                                [
                                    WriteMode::Normal,
                                    WriteMode::Denied,
                                    WriteMode::Deceive,
                                    WriteMode::Shadow,
                                ][value],
                            )
                        }
                        "PRNG" => *prng = Some([Prng::Static, Prng::Weak, Prng::Hard][value]),
                        "Detection" => *detection = Some(value != 0),
                        "Gen1A" => *gen1a = Some(value != 0),
                        "Gen2" => *gen2 = Some(value != 0),
                        "Block identity" => *block_identity = Some(value != 0),
                        "Field reset" => *field_reset = Some(value != 0),
                        "UID magic" => *uid_magic = Some(value != 0),
                        _ => unreachable!(),
                    }
                    Ok(())
                }
                Err(error) => Err(error),
            }
        };
        match edit {
            Ok(()) => return Ok(command),
            Err(error) if error.is::<selection::Back>() => continue,
            Err(error) => return Err(error),
        }
    }
}

pub(super) fn setup(
    command: Command,
    client: &mut Client,
    slots: &[Slot],
    allowed: bool,
) -> Result<()> {
    let Command::Setup {
        target,
        name,
        enabled,
        write_mode,
        prng,
        detection,
        gen1a,
        gen2,
        block_identity,
        field_reset,
        uid_magic,
    } = command
    else {
        unreachable!()
    };
    let number = choose_slot(slots, target.slot, allowed, false)?;
    if name.is_none()
        && enabled.is_none()
        && write_mode.is_none()
        && prng.is_none()
        && detection.is_none()
        && gen1a.is_none()
        && gen2.is_none()
        && block_identity.is_none()
        && field_reset.is_none()
        && uid_magic.is_none()
    {
        if !allowed {
            return Err("Supply setup options; see slot setup --help".into());
        }
        let command = setup_menu(client, slots, number, allowed, target)?;
        return setup(command, client, slots, allowed);
    }
    let sense = choose_sense(&target, &slots[(number - 1) as usize], allowed)?;
    let has_settings = write_mode.is_some()
        || prng.is_some()
        || detection.is_some()
        || gen1a.is_some()
        || gen2.is_some()
        || block_identity.is_some()
        || field_reset.is_some()
        || uid_magic.is_some();
    if has_settings {
        let image = client.read_slot_image(number, sense)?;
        let classic = (1000..=1003).contains(&image.tag_type);
        let type2 = (1100..=1108).contains(&image.tag_type);
        if !classic && !type2 {
            return Err("This model has no emulator settings".into());
        }
        if classic && uid_magic.is_some()
            || type2
                && (prng.is_some()
                    || gen1a.is_some()
                    || gen2.is_some()
                    || block_identity.is_some()
                    || field_reset.is_some())
        {
            return Err("Settings do not apply to this model".into());
        }
        let mut options = image.options;
        if let Some(mode) = write_mode {
            options[if classic { 0 } else { 1 }] = mode as u8;
        }
        if let Some(value) = detection {
            options[if classic { 4 } else { 2 }] = u8::from(value);
        }
        if classic {
            for (index, value) in [(1, gen1a), (2, gen2), (3, block_identity), (5, field_reset)] {
                if let Some(value) = value {
                    options[index] = u8::from(value);
                }
            }
            if let Some(value) = prng {
                options[6] = value as u8;
            }
        } else if let Some(value) = uid_magic {
            options[0] = u8::from(value);
        }
        client.configure_slot(number, sense, &options)?;
    }
    if let Some(name) = name {
        client.rename_slot(number, sense, &name)?;
    }
    if let Some(enabled) = enabled {
        client.enable_slot(number, sense, enabled)?;
    }
    ui::result(
        true,
        format!("Slot {number} {sense:?} configuration verified"),
    );
    Ok(())
}
