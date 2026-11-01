//! A read-only device overview, shared by direct commands and the console.
use super::*;
use chameleon_client::{DeviceSettings, SlotSide};
use std::fmt::Write;

const UNAVAILABLE: &str = "Unavailable on this firmware";

pub fn run(port: Option<&str>, cancelled: Arc<AtomicBool>, verbose: bool) -> Result<()> {
    let mut client = Client::connect(port, cancelled)?;
    let connection = client.port_name().unwrap_or_else(|| "unknown port".into());
    let firmware = client.info()?;
    let reader = client.reader_mode()?;
    let active = client.active_slot()?;
    let slots = client.slots()?;
    let slot = slots
        .iter()
        .find(|slot| slot.number == active)
        .ok_or("Missing active slot")?;
    let battery = client.battery()?;
    let settings = client.device_settings()?;
    let width = crossterm::terminal::size().map_or(80, |(width, _)| usize::from(width));
    let color = ui::stdout_color();
    let mut text = overview(
        &firmware,
        &connection,
        battery,
        reader,
        slot,
        &settings,
        width,
        color,
    );
    // Keep the useful overview even if an optional verbose read fails later.
    print!("{text}");
    text.clear();
    if verbose {
        let identity = client.device_identity()?;
        println!();
        let mut out = Display {
            text: &mut text,
            width,
            color,
        };
        out.section("Details");
        out.field(
            "Chip ID",
            &identity
                .chip_id
                .map_or(UNAVAILABLE.into(), |bytes| ui::uid_text(&bytes)),
        );
        out.field(
            "BLE address",
            &identity.ble_address.map_or(UNAVAILABLE.into(), |bytes| {
                bytes
                    .iter()
                    .map(|byte| format!("{byte:02X}"))
                    .collect::<Vec<_>>()
                    .join(":")
            }),
        );
        out.field(
            "Settings format",
            &settings
                .version
                .map_or(UNAVAILABLE.into(), |version| version.to_string()),
        );
        out.list("Features", &features(&client));
        // Legacy image reads can select a slot and require reader mode. A status
        // query must never take that path; frozen snapshots keep mode/selection.
        for (sense, name) in [(Sense::Hf, "HF image"), (Sense::Lf, "LF image")] {
            if slot.side(sense).tag_type == 0 {
                continue;
            }
            let mut out = Display {
                text: &mut text,
                width,
                color,
            };
            out.section(name);
            if let Some(reason) =
                image_unavailable(client.supports_slot_snapshot(), slot.side(sense).tag_type)
            {
                out.field("Details", reason);
                continue;
            }
            match client.read_slot_image(active, sense) {
                Ok(image) => {
                    out.image(&image)?;
                }
                Err(error)
                    if error
                        .downcast_ref::<chameleon_client::DeviceError>()
                        .is_some_and(|error| matches!(error.0, 0x67 | 0x69 | 0x76)) =>
                {
                    out.field("Details", &format!("Unavailable · {error}"));
                }
                Err(error) => return Err(error),
            }
        }
    }
    print!("{text}");
    Ok(())
}

struct Display<'a> {
    text: &'a mut String,
    width: usize,
    color: bool,
}

impl Display<'_> {
    fn list(&mut self, label: &str, values: &[&str]) {
        use unicode_width::UnicodeWidthStr;
        let indent = if self.width < 48 { 4 } else { 18 };
        let available = self.width.saturating_sub(indent);
        let mut lines = vec![String::new()];
        for value in values {
            let line = lines.last_mut().unwrap();
            if !line.is_empty() && line.width() + 3 + value.width() > available {
                lines.push((*value).into());
            } else {
                if !line.is_empty() {
                    line.push_str(" · ");
                }
                line.push_str(value);
            }
        }
        self.field(label, &lines.join(&format!("\n{}", " ".repeat(indent))));
    }

    fn image(&mut self, image: &chameleon_client::SlotImage) -> Result<()> {
        for (label, value) in identity_fields(image)? {
            self.field(label, &value);
        }
        if image.tag_type == 1300 {
            self.field(
                "Emulation",
                "Public data only; no authentication or secure messaging",
            );
        }
        for &(name, index) in setup::settings(image) {
            if let Some(&byte) = image.options.get(index) {
                self.field(name, &setting_name(Some(byte), setup::values(name)));
            }
        }
        Ok(())
    }

    fn section(&mut self, title: &str) {
        if !self.text.is_empty() {
            self.text.push('\n');
        }
        writeln!(self.text, "{}", ui::paint(title, "1;36", self.color)).unwrap();
    }

    fn field(&mut self, label: &str, value: &str) {
        if self.width < 48 {
            writeln!(
                self.text,
                "  {}\n    {value}",
                ui::paint(label, "2", self.color)
            )
            .unwrap();
        } else {
            let label = ui::paint(format!("{label:14}"), "2", self.color);
            writeln!(self.text, "  {label}  {value}").unwrap();
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn overview(
    firmware: &str,
    connection: &str,
    (mv, percent): (u16, u8),
    reader: bool,
    slot: &Slot,
    settings: &DeviceSettings,
    width: usize,
    color: bool,
) -> String {
    let mut text = String::new();
    let mut out = Display {
        text: &mut text,
        width,
        color,
    };
    out.section("Device");
    out.field("Model", "Chameleon Ultra");
    out.field(
        "Firmware",
        firmware
            .strip_prefix("Chameleon Ultra ")
            .unwrap_or(firmware),
    );
    out.field("USB", connection);
    let level = ui::paint(
        format!("{percent}%"),
        if percent <= 20 { ui::WARN } else { ui::OK },
        color,
    );
    let bar = if width >= 60 {
        let filled = usize::from(percent.min(100)).div_ceil(10);
        format!(
            "{}{} ",
            ui::paint(
                "█".repeat(filled),
                if percent <= 20 { ui::WARN } else { ui::OK },
                color
            ),
            ui::paint("░".repeat(10 - filled), "90", color)
        )
    } else {
        String::new()
    };
    out.field(
        "Battery",
        &format!("{bar}{level} · {}.{:03} V", mv / 1000, mv % 1000),
    );

    out.section("Emulation");
    out.field(
        "Mode",
        if reader {
            "Reader · emulation stopped"
        } else {
            "Emulator"
        },
    );
    out.field("Selected slot", &format!("{} / 8", slot.number));
    for (band, side) in [("HF", &slot.hf), ("LF", &slot.lf)] {
        let state = if side.tag_type == 0 {
            ui::paint("Empty", "90", color)
        } else {
            let (state, code) = if side.enabled {
                ("Enabled", ui::OK)
            } else {
                ("Disabled", ui::WARN)
            };
            format!("{} · {}", image_name(side), ui::paint(state, code, color))
        };
        out.field(band, &state);
        if !side.name.is_empty() {
            out.field(&format!("{band} name"), &side.name);
        }
    }
    if !reader
        && ![&slot.hf, &slot.lf]
            .iter()
            .any(|side| side.tag_type != 0 && side.enabled)
    {
        out.field(
            "State",
            &ui::paint("No enabled image in selected slot", ui::WARN, color),
        );
    }

    out.section("Device settings");
    out.field(
        "BLE pairing",
        settings.ble_pairing.map_or(UNAVAILABLE, |required| {
            if required { "Required" } else { "Not required" }
        }),
    );
    out.field(
        "Animation",
        &setting_name(
            settings.animation,
            &["Full", "Minimal", "None", "Symmetric"],
        ),
    );
    out.field(
        "Wake timeout",
        &settings
            .wake_timeout_s
            .map_or(UNAVAILABLE.into(), |seconds| {
                format!("{seconds} s after button wakeup")
            }),
    );
    out.field(
        "Long press",
        &settings
            .long_press_ms
            .map_or(UNAVAILABLE.into(), |ms| format!("{ms} ms")),
    );

    out.section("Buttons");
    let actions = settings.buttons.map(button_name);
    use unicode_width::UnicodeWidthStr;
    let short_width = actions[..2]
        .iter()
        .map(|value| value.width())
        .max()
        .unwrap()
        .max(11);
    let long_width = actions[2..]
        .iter()
        .map(|value| value.width())
        .max()
        .unwrap()
        .max(10);
    if 16 + short_width + long_width <= width {
        writeln!(
            out.text,
            "  {}",
            ui::paint(
                format!("Button    {:short_width$}  Long press", "Short press"),
                "2",
                color
            )
        )
        .unwrap();
        for (index, button) in ["A", "B"].iter().enumerate() {
            writeln!(
                out.text,
                "  {button}         {:short_width$}  {}",
                actions[index],
                actions[index + 2]
            )
            .unwrap();
        }
    } else {
        for (index, button) in ["A", "B"].iter().enumerate() {
            out.field(&format!("{button} short"), &actions[index]);
            out.field(&format!("{button} long"), &actions[index + 2]);
        }
    }
    text
}

fn setting_name(value: Option<u8>, names: &[&str]) -> String {
    value.map_or(UNAVAILABLE.into(), |value| {
        names
            .get(usize::from(value))
            .map_or_else(|| format!("Unknown ({value})"), |name| (*name).into())
    })
}

fn button_name(value: Option<u8>) -> String {
    setting_name(
        value,
        &[
            "No action",
            "Next slot",
            "Previous slot",
            "Clone HF/LF ID",
            "Show battery",
            "Toggle NFC field",
        ],
    )
}

fn image_name(side: &SlotSide) -> String {
    match side.tag_type {
        1000 => "MIFARE Classic Mini".into(),
        1001..=1003 => format!(
            "MIFARE Classic {}K",
            [1, 2, 4][usize::from(side.tag_type - 1001)]
        ),
        1103 => "MIFARE Ultralight".into(),
        1104 => "MIFARE Ultralight C".into(),
        1105 => "MIFARE Ultralight EV1 48".into(),
        1106 => "MIFARE Ultralight EV1 128".into(),
        1300 => "MIFARE DESFire".into(),
        _ => type_name(side.tag_type).to_uppercase(),
    }
}

fn image_unavailable(snapshot: bool, tag_type: u16) -> Option<&'static str> {
    if !SLOT_TYPES.iter().any(|(id, _)| *id == tag_type) {
        Some("Unavailable: unsupported image type")
    } else if !snapshot {
        Some("Unavailable: firmware lacks slot snapshots")
    } else {
        None
    }
}

fn features(client: &Client) -> Vec<&'static str> {
    // User-facing workflows rather than an opaque list of wire command numbers.
    let names: Vec<_> = [
        (2000, "NFC-A"),
        (2304, "NFC-B"),
        (2300, "NFC-V"),
        (2035, "DESFire reader"),
        (2036, "NFC-A sniff"),
        (2037, "NFC-V sniff"),
    ]
    .into_iter()
    .filter_map(|(id, name)| client.capabilities.contains(&id).then_some(name))
    .chain(client.supports_slot_snapshot().then_some("Slot snapshots"))
    .collect();
    if names.is_empty() {
        vec!["None reported"]
    } else {
        names
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(reader: bool, width: usize, color: bool) -> String {
        overview(
            "Chameleon Ultra 2.2 (78cfa0f-dirty)",
            "/dev/cu.usbmodemF3EB5E5655C33",
            (4324, 100),
            reader,
            &Slot {
                number: 2,
                hf: SlotSide {
                    tag_type: 1103,
                    enabled: true,
                    name: "Badge entrée".into(),
                },
                lf: SlotSide {
                    tag_type: 0,
                    enabled: false,
                    name: String::new(),
                },
            },
            &DeviceSettings {
                version: Some(7),
                animation: Some(0),
                buttons: [Some(1), Some(2), Some(3), Some(4)],
                ble_pairing: Some(true),
                wake_timeout_s: Some(8),
                long_press_ms: Some(1000),
            },
            width,
            color,
        )
    }

    #[test]
    fn overview_preserves_content_across_color_and_terminal_widths() {
        let plain = sample(false, 80, false);
        assert_eq!(ui::without_colors(&sample(false, 80, true)), plain);
        assert!(!plain.contains('\x1b'));
        for width in [32, 50, 80] {
            let text = sample(true, width, false);
            assert_eq!(ui::without_colors(&sample(true, width, true)), text);
            for value in [
                "Reader · emulation stopped",
                "2 / 8",
                "MIFARE Ultralight · Enabled",
                "Badge entrée",
                "Empty",
                "Required",
                "Next slot",
                "Previous slot",
                "Clone HF/LF ID",
                "Show battery",
                "8 s after button wakeup",
                "1000 ms",
                "4.324 V",
            ] {
                assert!(text.contains(value), "Missing {value} at width {width}");
            }
        }
        assert!(sample(false, 32, false).contains("A short"));
        assert!(plain.contains("Short press"));
        assert!(!plain.contains("654321"));
    }

    #[test]
    fn disabled_images_and_unavailable_or_future_settings_remain_explicit() {
        let slot = Slot {
            number: 1,
            hf: SlotSide {
                tag_type: 1001,
                enabled: false,
                name: "Stored badge".into(),
            },
            lf: SlotSide {
                tag_type: 100,
                enabled: false,
                name: String::new(),
            },
        };
        let settings = DeviceSettings {
            animation: Some(99),
            buttons: [Some(99), None, None, None],
            ..DeviceSettings::default()
        };
        let text = overview("2.2", "USB", (3300, 10), false, &slot, &settings, 60, true);
        let text = ui::without_colors(&text);
        for value in [
            "MIFARE Classic 1K · Disabled",
            "EM410X · Disabled",
            "Stored badge",
            "No enabled image",
            "Unavailable on this firmware",
            "Unknown (99)",
            "10%",
            "3.300 V",
        ] {
            assert!(text.contains(value), "Missing {value}");
        }
        assert!(!text.contains("Short press")); // Long unavailable labels use stacked rows.
    }

    #[test]
    fn verbose_images_share_configuration_values_and_keep_credentials_hidden() {
        let mut text = String::new();
        let mut out = Display {
            text: &mut text,
            width: 80,
            color: false,
        };
        out.image(&chameleon_client::SlotImage {
            version: 1,
            tag_type: 1001,
            identity: vec![4, 0xde, 0xad, 0xbe, 0xef, 4, 0, 8, 0],
            options: vec![3, 1, 0, 0, 1, 1, 2],
            memory: vec![0xab; 1024],
        })
        .unwrap();
        for value in ["DEADBEEF", "Shadow", "Hard", "Enabled", "Disabled"] {
            assert!(text.contains(value));
        }
        assert!(!text.contains("AB AB"));
        assert!(image_unavailable(true, 9999).is_some());
        assert!(image_unavailable(false, 1001).is_some());
        assert_eq!(image_unavailable(true, 1001), None);
    }

    #[test]
    fn firmware_features_wrap_without_splitting_feature_names() {
        use unicode_width::UnicodeWidthStr;
        let features = [
            "NFC-A",
            "NFC-B",
            "NFC-V",
            "DESFire reader",
            "NFC-A sniff",
            "NFC-V sniff",
            "Slot snapshots",
        ];
        for width in [32, 50, 80] {
            let mut text = String::new();
            Display {
                text: &mut text,
                width,
                color: false,
            }
            .list("Features", &features);
            assert!(text.lines().all(|line| line.width() <= width));
            for feature in features {
                assert!(text.contains(feature));
            }
        }
    }
}
