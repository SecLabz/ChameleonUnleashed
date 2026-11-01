//! Terminal styling only; raw capture and JSONL records stay unchanged.
use crate::ui;
use chameleon_client::{SniffFrame, SniffSummary};
use nfc_native::sniff::{Analysis, Check};
use std::io::{self, Write};

const DIM: &str = "2";
const TAG: &str = "37";
const FAIL: &str = "31";
const DATA_WIDTH: usize = 59;

pub(super) struct Display {
    colored: bool,
    type_v: bool,
    verbose: bool,
    show_intervals: bool,
    previous_us: Option<u64>,
}

impl Display {
    pub fn new(type_v: bool, verbose: bool, show_intervals: bool) -> Self {
        Self {
            colored: ui::stdout_color(),
            type_v,
            verbose,
            show_intervals,
            previous_us: None,
        }
    }

    pub fn start(&self, out: &mut impl Write, gain: u8) -> io::Result<()> {
        let protocol = if self.type_v { "NFC-V" } else { "NFC-A" };
        writeln!(
            out,
            "\n{}",
            self.paint(format!("{protocol} · Passive capture"), "1;36")
        )?;
        writeln!(
            out,
            "  {}",
            self.paint("Ctrl-C stops · RX completion timestamps in µs", DIM)
        )?;
        if self.verbose {
            writeln!(
                out,
                "  {}",
                self.paint(format!("Receiver gain {gain} · NFCT + MFRC522"), DIM)
            )?;
        }
        let data_label = if self.type_v {
            "Data"
        } else {
            "Data (! denotes frame parity error)"
        };
        writeln!(
            out,
            "{}",
            self.paint(
                format!("RX end (us) | Src | {data_label:DATA_WIDTH$} | CRC | Annotation"),
                "1"
            )
        )?;
        writeln!(
            out,
            "{}",
            self.paint(
                format!(
                    "------------+-----+-{}-+-----+----------------------",
                    "-".repeat(DATA_WIDTH)
                ),
                DIM
            )
        )
    }

    pub fn frame(
        &mut self,
        out: &mut impl Write,
        frame: &SniffFrame,
        analysis: &Analysis,
    ) -> io::Result<()> {
        let (source, color) = if frame.direction == 0 {
            ("Rdr", ui::WARN)
        } else {
            ("Tag", TAG)
        };
        let (crc_label, crc_color) = match analysis.checks.crc {
            Check::Valid => (" ok", ui::OK),
            Check::Invalid => ("bad", FAIL),
            Check::Unavailable => ("  —", DIM),
        };
        let has_crc = analysis.checks.crc != Check::Unavailable
            && frame.data.len() >= 2
            && frame.size_bits.is_multiple_of(8);
        let mut raw = String::new();
        let mut raw_width = 0;
        for (index, byte) in frame.data.iter().enumerate() {
            if index != 0 {
                raw.push(' ');
                raw_width += 1;
            }
            let mut value = format!("{byte:02X}");
            if index + 1 == frame.data.len() && !frame.size_bits.is_multiple_of(8) {
                value.push_str(&format!("({})", frame.size_bits % 8));
            }
            raw_width += value.len();
            let byte_color = if has_crc && index >= frame.data.len() - 2 {
                crc_color
            } else {
                color
            };
            raw.push_str(&self.paint(value, byte_color));
        }
        // Parity flags apply to the whole frame, not an identified byte.
        if analysis.checks.parity == Check::Invalid {
            raw.push_str(&self.paint("!", FAIL));
            raw_width += 1;
        }
        let invalid = !frame.analysis_frame().framing_valid()
            || analysis.checks.crc == Check::Invalid
            || analysis.checks.parity == Check::Invalid
            || analysis.annotation.starts_with("ERROR ")
            || analysis.annotation.starts_with("INVALID")
            || analysis.annotation.starts_with("TRUNCATED");
        let bar = self.paint(" | ", DIM);
        write!(
            out,
            "{}{bar}{}{bar}{raw}{}{bar}{}{bar}{}",
            self.paint(format!("{:>11}", frame.end_us), DIM),
            self.paint(source, color),
            " ".repeat(DATA_WIDTH.saturating_sub(raw_width)),
            self.paint(crc_label, crc_color),
            self.paint(&analysis.annotation, if invalid { FAIL } else { color })
        )?;
        if self.show_intervals {
            if let Some(previous) = self.previous_us {
                write!(
                    out,
                    "  {}",
                    self.paint(
                        format!("ΔRX={} us", frame.end_us.saturating_sub(previous)),
                        ui::ACCENT
                    )
                )?;
            }
            self.previous_us = Some(frame.end_us);
        }
        if self.verbose {
            let checks = if self.type_v {
                format!("CRC={}", analysis.checks.crc)
            } else {
                format!(
                    "parity={} CRC={}{}",
                    analysis.checks.parity,
                    analysis.checks.crc,
                    if analysis.checks.context_available {
                        ""
                    } else {
                        " (protocol context)"
                    }
                )
            };
            write!(
                out,
                "  {}",
                self.paint(format!("[{} bits; {checks}]", frame.size_bits), DIM)
            )?;
        }
        writeln!(out)
    }

    pub fn summary(&self, out: &mut impl Write, summary: &SniffSummary) -> io::Result<()> {
        writeln!(out, "\n{}", self.paint("Capture stopped", "1"))?;
        writeln!(
            out,
            "  {}   {}",
            self.paint(format!("Reader {}", summary.reader_frames), ui::WARN),
            self.paint(format!("Tag {}", summary.tag_frames), TAG)
        )?;
        let errors = summary.invalid_frames != 0
            || summary.dropped_frames != 0
            || summary.capture_overruns != 0
            || summary.tx_frames != 0;
        writeln!(
            out,
            "  {}",
            self.paint(
                format!(
                    "Invalid {} · dropped {} · overruns {} · TX {}",
                    summary.invalid_frames,
                    summary.dropped_frames,
                    summary.capture_overruns,
                    summary.tx_frames
                ),
                if errors { FAIL } else { DIM }
            )
        )
    }

    fn paint(&self, text: impl std::fmt::Display, code: &str) -> String {
        ui::paint(text, code, self.colored)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::{SNIFF_CRC_CHECKED, SNIFF_CRC_VALID, SNIFF_DECODED};
    use nfc_native::sniff::NfcvAnalyzer;

    #[test]
    fn protocol_errors_are_red_even_when_the_frame_crc_is_valid() {
        for (direction, data, expected) in [
            (1, vec![0x01, 0x0f, 0, 0], "ERROR 0x0F"),
            (0, vec![0x02, 0, 0], "INVALID REQUEST"),
            (0, vec![0x22, 0x20, 0, 0], "TRUNCATED UID"),
        ] {
            let frame = SniffFrame {
                end_us: 120,
                size_bits: (data.len() * 8) as u16,
                flags: SNIFF_DECODED | SNIFF_CRC_CHECKED | SNIFF_CRC_VALID,
                direction,
                errors: 0,
                data,
            };
            let analysis = NfcvAnalyzer::default().analyze(&frame.analysis_frame());
            assert_eq!(analysis.annotation, expected);
            assert_eq!(analysis.checks.crc, Check::Valid);

            let mut display = Display::new(true, false, false);
            display.colored = true;
            let mut colored = Vec::new();
            display.frame(&mut colored, &frame, &analysis).unwrap();
            let colored = String::from_utf8(colored).unwrap();
            assert!(colored.contains(&format!("\x1b[31m{expected}\x1b[0m")));
            assert!(colored.contains("\x1b[32m ok\x1b[0m"));

            display.colored = false;
            let mut plain = Vec::new();
            display.frame(&mut plain, &frame, &analysis).unwrap();
            assert_eq!(
                ui::without_colors(&colored),
                String::from_utf8(plain).unwrap()
            );
        }
    }
}
