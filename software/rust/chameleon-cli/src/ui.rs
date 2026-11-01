//! Terminal presentation; recovery policy lives in nfc-native.
use chameleon_client::Card;
use nfc_native::RecoveryProgress;
use std::{
    fmt::Display,
    io::{self, IsTerminal, Write},
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

static NO_COLOR: AtomicBool = AtomicBool::new(false);
pub const ACCENT: &str = "36";
pub const OK: &str = "32";
pub const WARN: &str = "33";

pub fn disable_color() {
    NO_COLOR.store(true, Ordering::Relaxed);
}

pub fn uid_text(uid: &[u8]) -> String {
    nfc_native::formats::flipper::hex(uid).replace(' ', "")
}

pub fn confirm(question: &str, yes: bool, prompt_allowed: bool) -> chameleon_client::Result<bool> {
    crate::selection::confirm(question, yes, prompt_allowed)
}

pub fn interactive() -> bool {
    std::io::stderr().is_terminal() && color_enabled()
}

pub fn color_enabled() -> bool {
    !NO_COLOR.load(Ordering::Relaxed)
        && std::env::var_os("NO_COLOR").is_none()
        && std::env::var("TERM").is_ok_and(|term| term != "dumb")
}

pub fn paint(text: impl Display, code: &str, enabled: bool) -> String {
    if enabled {
        format!("\x1b[{code}m{text}\x1b[0m")
    } else {
        text.to_string()
    }
}

pub fn stdout_color() -> bool {
    color_enabled() && io::stdout().is_terminal()
}

pub fn line(title: &str, details: &[String]) {
    let mut parts = Vec::new();
    if !title.is_empty() {
        parts.push(paint(title, "1;36", stdout_color()));
    }
    parts.extend_from_slice(details);
    println!("{}", parts.join(" · "));
}

pub fn result(complete: bool, detail: impl Display) {
    let (mark, code) = if complete { ("✓", OK) } else { ("!", WARN) };
    println!("{} {detail}", paint(mark, code, stdout_color()));
}

pub fn no_write(outcome: &str) {
    line(outcome, &["Nothing written".into()]);
}

pub fn change(label: impl Display, before: impl Display, after: impl Display) {
    let color = stdout_color();
    println!(
        "  {label}  {} → {}",
        paint(before, "90", color),
        paint(after, "94", color)
    );
}

pub fn role_color(role: &str) -> &'static str {
    match role {
        "UID" => "95",
        "Lock" | "OTP" | "Counter" | "sysOTP" => WARN,
        "CC" => OK,
        "Config" | "Auth" | "Key" | "PWD" | "PACK" => "94",
        _ => "",
    }
}

// Width is measured before adding ANSI escapes, including incomplete final lines.
pub fn hex_row(
    address: &str,
    bytes: &[Option<u8>],
    width: usize,
    role: &str,
    color: bool,
) -> String {
    let groups: Vec<_> = bytes
        .chunks(4)
        .map(|group| {
            group
                .iter()
                .map(|byte| {
                    let value = byte.map_or("??".into(), |byte| format!("{byte:02X}"));
                    let code = if byte.is_none() {
                        "90"
                    } else {
                        role_color(role)
                    };
                    paint(value, code, color && !code.is_empty())
                })
                .collect::<Vec<_>>()
                .join(" ")
        })
        .collect();
    let visible_width = |count: usize| (count * 3 + count.div_ceil(4)).saturating_sub(2);
    let padding = visible_width(width).saturating_sub(visible_width(bytes.len()));
    let ascii: String = bytes
        .iter()
        .map(|byte| match byte {
            Some(byte) if byte.is_ascii_graphic() || *byte == b' ' => *byte as char,
            Some(_) => '.',
            None => '?',
        })
        .collect();
    format!(
        "  {}  {}{}  {}",
        paint(address, "90", color),
        groups.join("  "),
        " ".repeat(padding),
        paint(format!("|{ascii:width$}|"), "90", color)
    )
}

pub fn classic_dump(blocks: &[Vec<Option<u8>>], color: bool) -> String {
    use nfc_native::formats::image::classic::{first_block, sector, trailer};
    use std::fmt::Write;

    // Sixteen spaced bytes plus three extra spaces between four-byte groups.
    const HEX_WIDTH: usize = 16 * 3 - 1 + 3;

    let mut text = String::new();
    for (index, bytes) in blocks.iter().enumerate() {
        let sector = sector(index);
        if index == usize::from(first_block(sector)) {
            writeln!(
                text,
                "\n{}",
                paint(format!("Sector {sector:02}"), "1;36", color)
            )
            .unwrap();
        }
        let is_trailer = index == usize::from(trailer(sector));
        let ends: &[usize] = if is_trailer {
            &[6, 10, 16]
        } else {
            &[4, 8, 12, 16]
        };
        // One space between bytes, two at group boundaries; ANSI escapes never affect padding.
        let width = bytes.len() * 3 - 1 + ends.len() - 1;
        let mut hex = String::new();
        let mut start = 0;
        for (group, &end) in ends.iter().enumerate() {
            if group > 0 {
                hex.push_str("  ");
            }
            let code = if index == 0 {
                "95"
            } else if is_trailer {
                ["32", "33", "94"][group]
            } else {
                ""
            };
            let cells = bytes[start..end]
                .iter()
                .map(|byte| {
                    let value = byte.map_or("??".into(), |byte| format!("{byte:02X}"));
                    let code = if byte.is_none() { "90" } else { code };
                    paint(value, code, color && !code.is_empty())
                })
                .collect::<Vec<_>>();
            hex.push_str(&cells.join(" "));
            start = end;
        }
        let ascii: String = bytes
            .iter()
            .map(|byte| match byte {
                Some(byte) if byte.is_ascii_graphic() || *byte == b' ' => *byte as char,
                Some(_) => '.',
                None => '?',
            })
            .collect();
        writeln!(
            text,
            "  {}  {hex}{}  {}",
            paint(format!("{index:03}"), "90", color),
            " ".repeat(HEX_WIDTH - width),
            paint(format!("|{ascii:16}|"), "90", color)
        )
        .unwrap();
    }
    text
}

pub fn word_row(address: &str, word: Option<u32>, role: &str, color: bool) -> String {
    let value = word.map_or("????????".into(), |word| format!("{word:08X}"));
    let code = if word.is_none() {
        "90"
    } else {
        role_color(role)
    };
    let mut text = format!(
        "  {}  {}",
        paint(address, "90", color),
        paint(value, code, color && !code.is_empty())
    );
    if !role.is_empty() {
        text.push_str(&format!("  {}", paint(role, role_color(role), color)));
    }
    text
}

#[cfg(test)]
pub(crate) fn without_colors(text: &str) -> String {
    text.split('\x1b')
        .map(|part| {
            part.strip_prefix('[')
                .and_then(|part| part.split_once('m'))
                .map_or(part, |(_, text)| text)
        })
        .collect()
}

pub fn fields(title: &str, fields: &[(&str, String)]) -> String {
    let width = fields.iter().map(|(name, _)| name.len()).max().unwrap_or(0);
    let mut text = format!("{title}\n{}\n", "─".repeat(title.chars().count()));
    for (name, value) in fields {
        text.push_str(&format!("  {name:width$}  {value}\n"));
    }
    text
}

pub fn report(title: &str, fields: &[(&str, String)]) {
    print_report(&self::fields(title, fields));
}

pub fn print_report(text: &str) {
    let color = color_enabled() && std::io::stdout().is_terminal();
    let mut lines = text.lines().peekable();
    while let Some(line) = lines.next() {
        if color && line.starts_with('─') {
            continue;
        }
        let title = !line.is_empty()
            && !line.starts_with(' ')
            && lines.peek().is_some_and(|next| next.starts_with('─'));
        if color && title {
            println!("{}", paint(line, "1;36", true));
        } else if color
            && let Some((name, value)) = line
                .strip_prefix("  ")
                .and_then(|line| line.split_once("  "))
        {
            println!("  {}  {value}", paint(name, "2", true));
        } else {
            println!("{line}");
        }
    }
}

pub fn message(text: impl Display, code: &str) {
    let mut stderr = std::io::stderr().lock();
    if interactive() {
        let _ = write!(stderr, "\r\x1b[2K");
    }
    let _ = writeln!(
        stderr,
        "{}",
        paint(text, code, color_enabled() && interactive())
    );
}

pub fn warning(text: impl Display) {
    message(format!("Warning: {text}"), WARN);
}

pub fn error(error: &(dyn std::error::Error + 'static)) {
    if let Some(error) = error.downcast_ref::<clap::Error>() {
        eprint!("{error}");
        return;
    }
    message(format!("Error: {error}"), "31");
}

pub fn elapsed(label: &str, elapsed: Duration) {
    message(
        format!("{label} elapsed: {:.3} s", elapsed.as_secs_f64()),
        "2",
    );
}

pub fn card(card: &Card, prng: Option<nfc_native::autopwn::PrngMode>) {
    let title = card.model().map_or("NFC-A", |model| model.name());
    line(
        title,
        &[format!(
            "UID {}",
            paint(uid_text(&card.uid), "95", stdout_color())
        )],
    );
    let mut details = vec![
        format!(
            "ATQA (RF) {}",
            nfc_native::formats::flipper::hex(&card.atqa)
        ),
        format!("SAK {:02X}", card.sak),
    ];
    if let Some(prng) = prng {
        details.push(format!("PRNG {prng:?}"));
    }
    line("", &details);
}

pub fn key_text(key: Option<[u8; 6]>) -> String {
    key.map_or_else(
        || "------------".into(),
        |key| nfc_native::formats::flipper::hex(&key).replace(' ', ""),
    )
}

pub fn recovery_progress(event: RecoveryProgress) {
    use RecoveryProgress::*;
    let text = match event {
        EarlyCandidateRejected => "Early candidate rejected; collecting more observations".into(),
        EarlyKey {
            observations,
            budget,
        } => format!("Key recovered after {observations}/{budget} observations"),
        Observations {
            observations,
            budget,
        } => format!("Collected {observations}/{budget} observations"),
        IncompleteCoverage { covered } => format!("First-byte coverage: {covered}/256"),
        Solving { observations } => format!("Solving {observations} observations"),
    };
    message(text, ACCENT);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn memory_rows_align_ascii_for_partial_lines_and_unknown_bytes_with_or_without_color() {
        for size in 1..=16 {
            let mut bytes = vec![Some(b'|'); size];
            bytes[0] = None;
            let plain = hex_row("0010", &bytes, 16, "Config", false);
            let colored = hex_row("0010", &bytes, 16, "Config", true);
            assert_eq!(without_colors(&colored), plain);
            assert_eq!(plain.find('|'), Some(60), "{plain}");
            assert_eq!(plain.len(), 78);
            assert!(colored.contains("\x1b[90m??\x1b[0m"));
        }
        assert_eq!(
            hex_row(
                "004",
                &[Some(b'A'), Some(0), None, Some(b'B')],
                4,
                "",
                false
            ),
            "  004  41 00 ?? 42  |A.?B|"
        );
        assert_eq!(
            word_row("005", Some(0xffff_fffe), "Counter", false),
            "  005  FFFFFFFE  Counter"
        );
        assert_eq!(
            word_row("SYS", None, "sysOTP", false),
            "  SYS  ????????  sysOTP"
        );
    }
}
