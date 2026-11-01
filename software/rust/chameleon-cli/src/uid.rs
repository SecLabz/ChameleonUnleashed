//! Explicit Classic magic detection and one-shot UID changes.
use chameleon_client::{Gen2Evidence, Reader, Result, TagCredential};
use clap::{Args, Subcommand, ValueEnum};
use nfc_native::{RecoveryReader, dictionary, formats::flipper};
use std::path::PathBuf;

#[derive(Args)]
pub struct InfoOptions {
    /// Known sector-0 key. Replaces automatic essentials trials.
    #[arg(long, value_name = "HEX")]
    pub key: Option<String>,
    /// Authenticate with Key B instead of Key A.
    #[arg(long, requires = "key")]
    pub key_b: bool,
}

#[derive(Subcommand)]
pub enum Command {
    /// Change the UID after checking the selected magic method.
    Set(Options),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Generation {
    #[value(name = "gen1a")]
    Gen1a,
    #[value(name = "gen1b")]
    Gen1b,
    #[value(name = "gen2")]
    Gen2,
    #[value(name = "gen3")]
    Gen3,
    #[value(name = "gen4")]
    Gen4,
    #[value(name = "gdm20")]
    Gdm20,
    #[value(name = "gdm40")]
    Gdm40,
    #[value(name = "gdm40-zuid")]
    Gdm40Zuid,
    #[value(name = "super-gen1")]
    SuperGen1,
}

#[derive(Args)]
pub struct Options {
    /// Replacement UID in hexadecimal.
    pub uid: String,
    /// Magic card generation or UID-write method.
    #[arg(long, value_enum)]
    pub generation: Option<Generation>,
    /// Local key dictionary; may be supplied more than once.
    #[arg(long = "dict", value_delimiter = ',')]
    pub dictionaries: Vec<PathBuf>,
    /// Known six-byte sector-0 Key A in hexadecimal.
    #[arg(long, conflicts_with = "key_b")]
    pub key_a: Option<String>,
    /// Known six-byte sector-0 Key B in hexadecimal.
    #[arg(long)]
    pub key_b: Option<String>,
    /// Six-byte key in hexadecimal.
    #[arg(long, conflicts_with_all=["key_a","key_b","password"])]
    pub key: Option<String>,
    /// Four-byte Gen4 password in hexadecimal.
    #[arg(long, conflicts_with_all=["key_a","key_b"])]
    pub password: Option<String>,
    /// Preview changes without writing.
    #[arg(long)]
    pub dry_run: bool,
    /// Confirm the prepared operation without prompting.
    #[arg(long)]
    pub yes: bool,
}

pub(crate) fn hex_bytes(text: &str, lengths: &[usize]) -> Result<Vec<u8>> {
    let clean: String = text
        .chars()
        .filter(|ch| !ch.is_ascii_whitespace() && *ch != ':')
        .collect();
    if !lengths.contains(&(clean.len() / 2))
        || !clean.len().is_multiple_of(2)
        || !clean.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(format!(
            "Expected {} hexadecimal bytes",
            lengths
                .iter()
                .map(usize::to_string)
                .collect::<Vec<_>>()
                .join(" or ")
        )
        .into());
    }
    (0..clean.len() / 2)
        .map(|index| u8::from_str_radix(&clean[index * 2..index * 2 + 2], 16).map_err(Into::into))
        .collect()
}

pub fn info(reader: &mut Reader<'_>, options: InfoOptions) -> Result<()> {
    let key = options
        .key
        .as_ref()
        .map(|value| -> Result<[u8; 6]> {
            hex_bytes(value, &[6])?
                .try_into()
                .map_err(|_| "Invalid sector-0 key".into())
        })
        .transpose()?;
    let prng = reader.probe_prng()?;
    let factory_key = [0xff; 6];
    let mut detection = reader.magic(Some((key.unwrap_or(factory_key), options.key_b)))?;
    let mut credentials = Vec::new();
    let mut evidence = Vec::new();
    if detection.gen2_authenticated() {
        credentials.push((options.key_b, key.unwrap_or(factory_key)));
    }
    evidence.push((options.key_b, detection.gen2));
    if key.is_none()
        && !detection.generation_detected()
        && detection.gen2 != Gen2Evidence::Inconclusive
    {
        let keys = dictionary::essentials()?;
        for key_b in [false, true] {
            if !key_b && detection.gen2_authenticated() {
                continue;
            }
            let candidates: Vec<_> = keys
                .iter()
                .copied()
                .filter(|candidate| key_b || *candidate != factory_key)
                .collect();
            if let Some(key) = reader.verify(0, key_b, &candidates)? {
                credentials.push((key_b, key));
                detection = reader.magic(Some((key, key_b)))?;
                evidence.push((key_b, detection.gen2));
                if detection.generation_detected() || detection.gen2 == Gen2Evidence::Inconclusive {
                    break;
                }
            }
        }
    }
    let show_evidence = !detection.generation_detected()
        || matches!(
            detection.gen2,
            Gen2Evidence::KnownAts | Gen2Evidence::WriteAcknowledged
        );
    crate::ui::print_report(&info_text(
        &reader.card,
        prng,
        &detection.summary(),
        if show_evidence { &evidence } else { &[] },
        &credentials,
        key.map(|key| (options.key_b, key)),
        key.is_none() && !detection.generation_detected(),
    ));
    Ok(())
}

fn info_text(
    card: &chameleon_client::Card,
    prng: nfc_native::autopwn::PrngMode,
    magic: &str,
    evidence: &[(bool, Gen2Evidence)],
    credentials: &[(bool, [u8; 6])],
    supplied: Option<(bool, [u8; 6])>,
    searched_keys: bool,
) -> String {
    let mut text = crate::ui::fields(
        "MIFARE Classic",
        &[
            (
                "Type",
                card.model().map_or("Unknown", |model| model.name()).into(),
            ),
            ("UID", flipper::hex(&card.uid)),
            ("ATQA (RF)", flipper::hex(&card.atqa)),
            ("SAK", format!("{:02X}", card.sak)),
            ("PRNG", format!("{prng:?}")),
        ],
    );
    let mut fields: Vec<_> = magic
        .split(", ")
        .enumerate()
        .map(|(index, name)| (if index == 0 { "Type" } else { "" }, name.into()))
        .collect();
    for &(key_b, result) in evidence {
        let label = match result {
            Gen2Evidence::KnownAts | Gen2Evidence::NotChecked => "Probe",
            _ if key_b => "Probe B",
            _ => "Probe A",
        };
        fields.push((label, result.description().into()));
    }
    if evidence.iter().any(|(_, result)| {
        matches!(
            result,
            Gen2Evidence::WriteDenied
                | Gen2Evidence::WriteAcknowledged
                | Gen2Evidence::Inconclusive
        )
    }) {
        fields.push(("Data", "Not sent (command-only probe)".into()));
    }
    text.push('\n');
    text.push_str(&crate::ui::fields("Magic detection", &fields));

    let mut keys: Vec<_> = credentials
        .iter()
        .map(|(key_b, key)| (if *key_b { "Key B" } else { "Key A" }, flipper::hex(key)))
        .collect();
    if let Some((key_b, key)) = supplied
        && !credentials.contains(&(key_b, key))
    {
        keys.push((
            if key_b { "Key B" } else { "Key A" },
            format!("{} (supplied, unverified)", flipper::hex(&key)),
        ));
    }
    if keys.is_empty() && searched_keys {
        keys.push(("Keys", "None found in essentials".into()));
    }
    if !keys.is_empty() {
        text.push('\n');
        text.push_str(&crate::ui::fields("Sector 0", &keys));
    }
    text
}

pub fn set(reader: &mut Reader<'_>, options: Options, prompt_allowed: bool) -> Result<()> {
    let new_uid = hex_bytes(&options.uid, &[4, 7])?;
    if new_uid.len() != reader.card.uid.len() {
        return Err("UID length cannot be changed".into());
    }
    if new_uid == reader.card.uid {
        return Err("Requested UID is already set".into());
    }
    let question = format!(
        "Change UID of tag {} to {}?",
        crate::ui::uid_text(&reader.card.uid),
        crate::ui::uid_text(&new_uid)
    );
    let method = match options.generation {
        Some(Generation::Gen1b) => Some(chameleon_client::MagicUidMethod::Gen1b),
        Some(Generation::Gen3) => Some(chameleon_client::MagicUidMethod::Gen3),
        Some(Generation::Gen4) => Some(chameleon_client::MagicUidMethod::Gen4),
        Some(Generation::Gdm20) => Some(chameleon_client::MagicUidMethod::Gdm20),
        Some(Generation::Gdm40) => Some(chameleon_client::MagicUidMethod::Gdm40),
        Some(Generation::Gdm40Zuid) => Some(chameleon_client::MagicUidMethod::Gdm40Zuid),
        Some(Generation::SuperGen1) => Some(chameleon_client::MagicUidMethod::SuperGen1),
        _ => None,
    };
    if let Some(method) = method {
        if options.key.is_some()
            || options.key_a.is_some()
            || options.key_b.is_some()
            || !options.dictionaries.is_empty()
        {
            return Err("This UID method does not use sector keys or dictionaries".into());
        }
        let password = options
            .password
            .as_ref()
            .map(|text| hex_bytes(text, &[4]).map(|bytes| bytes.try_into().unwrap()))
            .transpose()?
            .unwrap_or([0; 4]);
        if options.password.is_some() && method != chameleon_client::MagicUidMethod::Gen4 {
            return Err("--password is only used by Gen4".into());
        }
        reader.special_uid(method, &new_uid, password, true)?;
        crate::ui::report(
            "MIFARE UID change",
            &[
                ("Old UID", flipper::hex(&reader.card.uid)),
                ("New UID", flipper::hex(&new_uid)),
                ("Method", format!("{method:?}")),
                (
                    "Preserved",
                    "Tag capacity, activation settings and unrelated memory".into(),
                ),
            ],
        );
        if options.dry_run {
            crate::ui::no_write("Preview");
        } else if crate::ui::confirm(&question, options.yes, prompt_allowed)? {
            reader.special_uid(method, &new_uid, password, false)?;
            crate::ui::result(
                true,
                format!("UID verified · {}", crate::ui::uid_text(&reader.card.uid)),
            );
        }
        return Ok(());
    }
    if options.password.is_some() {
        return Err("Password UID writing is unsupported".into());
    }
    let gen1a = options.generation != Some(Generation::Gen2) && reader.magic(None)?.gen1a();
    if options.generation == Some(Generation::Gen1a) && !gen1a {
        return Err("Gen1A UID-write signature was not confirmed".into());
    }
    if !gen1a && options.generation != Some(Generation::Gen2) {
        return Err("UID method is unknown; select --generation gen2 explicitly for a key-based block-0 write, which may be one-time".into());
    }
    let credential = if gen1a {
        TagCredential::None
    } else {
        let explicit = options.key_a.as_ref().or(options.key.as_ref());
        let mut keys = if let Some(value) = explicit {
            vec![hex_bytes(value, &[6])?.try_into().unwrap()]
        } else if let Some(value) = &options.key_b {
            vec![hex_bytes(value, &[6])?.try_into().unwrap()]
        } else {
            dictionary::essentials()?
        };
        for path in &options.dictionaries {
            let text = std::fs::read_to_string(path)?;
            keys.extend(dictionary::parse(&text, &path.display().to_string())?);
        }
        let preferred_b = options.key_b.is_some();
        let mut found = None;
        for key_b in [preferred_b, !preferred_b] {
            if let Some(key) = reader.verify(0, key_b, &keys)? {
                found = Some(if key_b {
                    TagCredential::KeyB(key)
                } else {
                    TagCredential::KeyA(key)
                });
                break;
            }
        }
        found.ok_or("No supplied or common sector-0 key was accepted")?
    };
    crate::ui::report(
        "MIFARE UID change",
        &[
            ("Old UID", flipper::hex(&reader.card.uid)),
            ("New UID", flipper::hex(&new_uid)),
            (
                "Method",
                if gen1a {
                    "gen1a"
                } else {
                    "key-based block 0 (may be one-time)"
                }
                .into(),
            ),
        ],
    );
    if options.dry_run {
        crate::ui::no_write("Preview");
        return Ok(());
    }
    if !crate::ui::confirm(&question, options.yes, prompt_allowed)? {
        return Ok(());
    }
    reader.set_uid(&new_uid, credential)?;
    crate::ui::result(
        true,
        format!("UID verified · {}", crate::ui::uid_text(&reader.card.uid)),
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::Card;
    use nfc_native::autopwn::PrngMode;

    fn card() -> Card {
        Card {
            uid: vec![0x4b, 0x4c, 0xfc, 3],
            atqa: [4, 0],
            sak: 8,
        }
    }

    #[test]
    fn info_separates_card_evidence_and_keys_into_short_rows() {
        let text = info_text(
            &card(),
            PrngMode::Static,
            "Unconfirmed (no known signature)",
            &[
                (false, Gen2Evidence::WriteDenied),
                (true, Gen2Evidence::WriteDenied),
            ],
            &[(false, [0xff; 6]), (true, [0xff; 6])],
            None,
            true,
        );
        assert_eq!(
            text,
            concat!(
                "MIFARE Classic\n──────────────\n",
                "  Type       Classic 1K\n",
                "  UID        4B 4C FC 03\n",
                "  ATQA (RF)  04 00\n",
                "  SAK        08\n",
                "  PRNG       Static\n",
                "\nMagic detection\n───────────────\n",
                "  Type     Unconfirmed (no known signature)\n",
                "  Probe A  Block-0 WRITE refused\n",
                "  Probe B  Block-0 WRITE refused\n",
                "  Data     Not sent (command-only probe)\n",
                "\nSector 0\n────────\n",
                "  Key A  FF FF FF FF FF FF\n",
                "  Key B  FF FF FF FF FF FF\n",
            )
        );
        assert!(text.lines().all(|line| line.chars().count() <= 64));
    }

    #[test]
    fn info_lists_signatures_separately_and_does_not_duplicate_a_verified_key() {
        let text = info_text(
            &card(),
            PrngMode::Static,
            "Gen1A, GDM / USCUID (40 wakeup)",
            &[],
            &[(false, [0xff; 6])],
            Some((false, [0xff; 6])),
            false,
        );
        assert!(text.contains("  Type  Gen1A\n        GDM / USCUID (40 wakeup)\n"));
        assert_eq!(text.matches("FF FF FF FF FF FF").count(), 1);
        assert!(!text.contains("Probe"));
        assert!(!text.contains("unverified"));
    }

    #[test]
    fn info_distinguishes_supplied_keys_from_verified_keys_and_missing_keys() {
        let text = info_text(
            &card(),
            PrngMode::Static,
            "Unconfirmed (no known signature)",
            &[(true, Gen2Evidence::AuthenticationDenied)],
            &[],
            Some((true, [0xff; 6])),
            false,
        );
        assert!(text.contains("Key B  FF FF FF FF FF FF (supplied, unverified)"));
        assert!(!text.contains("Not sent"));
        let text = info_text(
            &card(),
            PrngMode::Static,
            "Unconfirmed (no known signature)",
            &[],
            &[],
            None,
            true,
        );
        assert!(text.contains("None found in essentials"));
    }

    #[test]
    fn ats_signature_does_not_claim_a_key_probe_or_authentication() {
        let text = info_text(
            &card(),
            PrngMode::Static,
            "Gen2 / CUID",
            &[(false, Gen2Evidence::KnownAts)],
            &[],
            Some((false, [0xff; 6])),
            false,
        );
        assert!(text.contains("  Probe  Known ATS signature\n"));
        assert!(!text.contains("Probe A"));
        assert!(!text.contains("Not sent"));
        assert!(text.contains("Key A  FF FF FF FF FF FF (supplied, unverified)"));
    }
}
