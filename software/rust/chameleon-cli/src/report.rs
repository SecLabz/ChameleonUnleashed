// SPDX-License-Identifier: GPL-3.0-or-later
use crate::ui;
use chameleon_client::{Card, Result};
use nfc_native::{
    autopwn::AutoPwnSummary,
    formats::image::{
        Image,
        classic::{key_b_readable, sector, trailer},
    },
};
use std::{fmt::Write, io::IsTerminal, time::Duration};

pub struct Report {
    pub card: Card,
    pub summary: AutoPwnSummary,
    pub memory: Vec<Option<Vec<u8>>>,
    pub error: Option<String>,
    pub warnings: Vec<String>,
    pub elapsed: Duration,
}

impl Report {
    pub fn status(&self) -> &'static str {
        if self.error.is_some() || self.summary.stopped {
            "interrupted"
        } else if self.summary.resolved_keys == self.summary.total_keys
            && self.memory.iter().all(Option::is_some)
        {
            "complete"
        } else {
            "partial"
        }
    }

    fn reason(&self) -> String {
        if let Some(error) = &self.error {
            return error.clone();
        }
        if !self.warnings.is_empty() {
            return self.warnings.join("; ");
        }
        "Some keys or memory blocks remain unavailable".into()
    }

    pub fn summary_text(&self, verbose: bool, color: bool) -> String {
        let status = self.status();
        let (title, code) = match status {
            "complete" => ("✓ Complete", "1;32"),
            "interrupted" => ("! Interrupted", "1;33"),
            _ => ("! Partial", "1;33"),
        };
        let mut text = String::new();
        writeln!(
            text,
            "{} · {}/{} keys · {}/{} blocks · {:.2} s",
            ui::paint(title, code, color),
            self.summary.resolved_keys,
            self.summary.total_keys,
            self.memory.iter().flatten().count(),
            self.memory.len(),
            self.elapsed.as_secs_f64()
        )
        .unwrap();
        if status != "complete" {
            writeln!(text, "{}", self.reason()).unwrap();
        }
        if !verbose {
            return text;
        }
        writeln!(
            text,
            "\n{}",
            ui::paint(
                "Sector  Key A         Source A       Key B         Source B",
                "1;36",
                color
            )
        )
        .unwrap();
        text.push_str("------  ------------  -------------  ------------  -------------\n");
        for (sector, keys) in self.summary.mapping.iter().enumerate() {
            let key = |key: Option<[u8; 6]>, code| {
                ui::paint(
                    ui::key_text(key),
                    if key.is_some() { code } else { "90" },
                    color,
                )
            };
            let origin = |origin: Option<nfc_native::autopwn::KeyOrigin>, padded| {
                let name = origin.map_or("unknown", |origin| origin.name());
                ui::paint(
                    if padded {
                        format!("{name:13}")
                    } else {
                        name.into()
                    },
                    if origin.is_some() { "36" } else { "33" },
                    color,
                )
            };
            writeln!(
                text,
                "{}      {}  {}  {}  {}",
                ui::paint(format!("{sector:02}"), "36", color),
                key(keys.key_a, "32"),
                origin(keys.origin_a, true),
                key(keys.key_b, "94"),
                origin(keys.origin_b, false)
            )
            .unwrap();
        }
        text
    }

    pub fn print_summary(&self, verbose: bool) {
        let terminal = std::io::stdout().is_terminal();
        print!(
            "{}",
            self.summary_text(verbose, terminal && ui::color_enabled())
        );
    }

    fn observed(&self) -> Vec<Vec<Option<u8>>> {
        self.memory
            .iter()
            .enumerate()
            .map(|(index, block)| {
                let mut bytes = block.as_ref().map_or(vec![None; 16], |block| {
                    block.iter().copied().map(Some).collect()
                });
                let sector = sector(index);
                if index != usize::from(trailer(sector)) {
                    return bytes;
                }
                bytes[..6].fill(None);
                if !block.as_ref().is_some_and(|block| key_b_readable(block)) {
                    bytes[10..].fill(None);
                }
                let keys = &self.summary.mapping[sector];
                for (offset, key) in [(0, keys.key_a), (10, keys.key_b)] {
                    if let Some(key) = key {
                        for (slot, byte) in bytes[offset..offset + 6].iter_mut().zip(key) {
                            *slot = Some(byte);
                        }
                    }
                }
                bytes
            })
            .collect()
    }

    pub fn dump_text(&self, color: bool) -> String {
        let observed = self.observed();
        if observed.iter().flatten().all(Option::is_none) {
            return "\nNo readable memory recovered. Try a known key or another dictionary.\n"
                .into();
        }
        let mut text = ui::classic_dump(&observed, color);
        if observed.iter().flatten().any(Option::is_none) {
            text.push_str("\n?? = unread or unavailable bytes; credentials shown only after authentication.\n");
        }
        text
    }

    pub fn json(&self) -> Result<String> {
        let model = self.card.model()?;
        let tag_type = match model {
            nfc_native::formats::image::Model::ClassicMini => 1000,
            nfc_native::formats::image::Model::Classic1k => 1001,
            nfc_native::formats::image::Model::Classic2k => 1002,
            nfc_native::formats::image::Model::Classic4k => 1003,
            _ => return Err("Expected a Classic capture".into()),
        };
        let keys: Vec<_> = self
            .summary
            .mapping
            .iter()
            .map(|entry| {
                serde_json::json!({
                    "key_a": entry.key_a,
                    "key_b": entry.key_b,
                })
            })
            .collect();
        let mut warnings = self.warnings.clone();
        if self.status() != "complete" {
            warnings.push(self.reason());
        }
        Ok(serde_json::to_string_pretty(&serde_json::json!({
            "version": 1,
            "document": {
                "name": format!("{}-{}", model.name(), nfc_native::formats::flipper::hex(&self.card.uid)),
                "kind": "classic", "type": tag_type, "unitBytes": 16,
                "card": {"uid": self.card.uid, "sak": self.card.sak, "atqa": [self.card.atqa[1], self.card.atqa[0]]},
                "memory": self.observed(), "recoveredKeys": keys, "warnings": warnings,
            }
        }))? + "\n")
    }

    pub fn export(&self) -> Result<String> {
        if self.status() != "complete" {
            return Err("Recovery is incomplete; no complete dump exported".into());
        }
        let memory = self
            .observed()
            .into_iter()
            .flatten()
            .collect::<Option<Vec<_>>>()
            .ok_or("Dump contains unavailable bytes")?;
        nfc_native::formats::flipper::export(&Image {
            model: self.card.model()?,
            uid: self.card.uid.clone(),
            memory,
            metadata: vec![],
            secrets: vec![],
        })
    }
}

#[cfg(test)]
mod tests;
