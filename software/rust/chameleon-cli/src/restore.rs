//! Physical user-data restore with full preflight and verified one-shot writes.
use chameleon_client::{Client, Reader, Result, TagCredential};
use clap::{Args, ValueEnum};
use nfc_native::{
    RecoveryReader,
    autopwn::RecoverySource,
    dictionary,
    formats::{
        image::{Model, classic},
        physical::{self, Family, ObservedUnit},
    },
};
use std::{
    collections::HashSet,
    io::Read,
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};

#[derive(Args)]
pub struct Input {
    /// Input capture or image file.
    pub file: PathBuf,
    /// Preview changes without writing.
    #[arg(long)]
    pub dry_run: bool,
    /// Confirm the prepared operation without prompting.
    #[arg(long)]
    pub yes: bool,
    /// Permit restoring user data to a tag with a different UID.
    #[arg(long)]
    pub allow_different_uid: bool,
}

#[derive(Clone, Copy, ValueEnum)]
pub enum ClassicModel {
    #[value(name = "classic-mini")]
    Mini,
    #[value(name = "classic1k")]
    OneK,
    #[value(name = "classic2k")]
    TwoK,
    #[value(name = "classic4k")]
    FourK,
}
impl From<ClassicModel> for Model {
    fn from(value: ClassicModel) -> Self {
        match value {
            ClassicModel::Mini => Self::ClassicMini,
            ClassicModel::OneK => Self::Classic1k,
            ClassicModel::TwoK => Self::Classic2k,
            ClassicModel::FourK => Self::Classic4k,
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
pub enum Type2Model {
    #[value(name = "ultralight-c")]
    UltralightC,
    Ultralight,
    Ntag210,
    Ntag212,
    Ntag213,
    Ntag215,
    Ntag216,
    #[value(name = "ultralight-ev1-48")]
    UltralightEv1_48,
    #[value(name = "ultralight-ev1-128")]
    UltralightEv1_128,
}
impl From<Type2Model> for Model {
    fn from(value: Type2Model) -> Self {
        match value {
            Type2Model::UltralightC => Self::UltralightC,
            Type2Model::Ultralight => Self::Ultralight,
            Type2Model::Ntag210 => Self::Ntag210,
            Type2Model::Ntag212 => Self::Ntag212,
            Type2Model::Ntag213 => Self::Ntag213,
            Type2Model::Ntag215 => Self::Ntag215,
            Type2Model::Ntag216 => Self::Ntag216,
            Type2Model::UltralightEv1_48 => Self::UltralightEv1_48,
            Type2Model::UltralightEv1_128 => Self::UltralightEv1_128,
        }
    }
}

#[derive(Args)]
pub struct ClassicOptions {
    #[command(flatten)]
    pub input: Input,
    /// Tag model used to interpret its memory geometry.
    #[arg(long, value_enum)]
    pub model: Option<ClassicModel>,
    /// Local key dictionary; may be supplied more than once.
    #[arg(long = "dict", value_delimiter = ',')]
    pub dictionaries: Vec<PathBuf>,
    /// Six-byte target sector key in hexadecimal.
    #[arg(long, value_name = "HEX")]
    pub key: Option<String>,
}

#[derive(Args)]
pub struct Type2Options {
    #[command(flatten)]
    pub input: Input,
    /// Tag model used to interpret its memory geometry.
    #[arg(long, value_enum)]
    pub model: Option<Type2Model>,
    /// Four-byte target password in hexadecimal.
    #[arg(long, value_name = "HEX")]
    pub password: Option<String>,
    /// Sixteen-byte target Ultralight C 3DES key in cipher key order, hexadecimal.
    #[arg(long, value_name = "HEX", conflicts_with = "password")]
    pub key: Option<String>,
}

struct TargetUnit {
    observed: ObservedUnit,
    credential: TagCredential,
}

fn keys(options: &ClassicOptions) -> Result<Vec<[u8; 6]>> {
    let mut keys = Vec::new();
    if let Some(value) = &options.key {
        keys.push(
            crate::uid::hex_bytes(value, &[6])?
                .try_into()
                .map_err(|_| "Invalid target key length")?,
        );
    }
    for path in &options.dictionaries {
        let mut text = String::new();
        std::fs::File::open(path)?
            .take(1024 * 1024 + 1)
            .read_to_string(&mut text)?;
        if text.len() > 1024 * 1024 {
            return Err("Dictionary exceeds 1 MiB".into());
        }
        keys.extend(dictionary::parse(&text, &path.display().to_string())?);
    }
    keys.extend(dictionary::essentials()?);
    let mut seen = HashSet::new();
    keys.retain(|key| seen.insert(*key));
    Ok(keys)
}

fn classic_target(
    reader: &mut Reader<'_>,
    source: &physical::Source,
    keys: &[[u8; 6]],
) -> Result<Vec<TargetUnit>> {
    let blocks = source.model.classic().unwrap().1;
    let mut result = Vec::new();
    for sector in 0..classic::sector_count(blocks) {
        let mut credentials = Vec::new();
        for key_b in [false, true] {
            if let Some(key) = reader.verify(sector, key_b, keys)? {
                credentials.push(if key_b {
                    TagCredential::KeyB(key)
                } else {
                    TagCredential::KeyA(key)
                });
            }
        }
        let trailer = classic::trailer(sector);
        let mut permissions = None;
        for credential in &credentials {
            let source = recovery_source(sector, *credential);
            if let Ok(bytes) = reader.block(&source, trailer) {
                permissions = Some(bytes);
                break;
            }
        }
        let trailer_bytes =
            permissions.ok_or_else(|| format!("Cannot read sector {sector} permissions"))?;
        for (index, after) in source
            .units
            .iter()
            .filter(|(index, _)| classic::sector(*index as usize) == sector)
        {
            let mut selected = None;
            for credential in &credentials {
                let source = recovery_source(sector, *credential);
                let Ok(before) = reader.block(&source, *index) else {
                    continue;
                };
                let writable = physical::classic_data_writable(
                    &trailer_bytes,
                    *index as usize,
                    matches!(credential, TagCredential::KeyB(_)),
                );
                if writable || before == *after {
                    selected = Some(TargetUnit {
                        observed: ObservedUnit {
                            index: *index,
                            before: Some(before),
                            after: Some(after.clone()),
                            writable,
                        },
                        credential: *credential,
                    });
                    break;
                }
            }
            result.push(selected.ok_or_else(|| {
                format!("Cannot restore block {index} with supplied target keys")
            })?);
        }
    }
    Ok(result)
}

fn recovery_source(sector: usize, credential: TagCredential) -> RecoverySource {
    match credential {
        TagCredential::KeyA(key) => RecoverySource {
            sector,
            is_key_b: false,
            key,
        },
        TagCredential::KeyB(key) => RecoverySource {
            sector,
            is_key_b: true,
            key,
        },
        _ => unreachable!(),
    }
}

fn type2_target(
    reader: &mut Reader<'_>,
    source: &physical::Source,
    credential: TagCredential,
) -> Result<Vec<TargetUnit>> {
    let (pages, version) = source.model.type2().unwrap();
    let ultralight_c = source.model == Model::UltralightC;
    if version.is_empty() && !ultralight_c {
        return Err("Original Ultralight physical writing is not qualified".into());
    }
    reader.end()?;
    if !ultralight_c
        && nfc_native::formats::type2::from_version(&reader.type2_version()?)
            != source.model.type2_profile()
    {
        return Err("Target Type 2 model/version cannot be confirmed".into());
    }
    let mut memory = vec![None; pages];
    let fast = credential == TagCredential::None
        && matches!(
            source.model,
            Model::Ntag213 | Model::Ntag215 | Model::Ntag216
        );
    let step = if ultralight_c {
        1
    } else if fast {
        8
    } else {
        4
    };
    let readable_pages = if ultralight_c { 44 } else { pages };
    for index in (0..readable_pages).step_by(step) {
        let count = (pages - index).min(step);
        let first = if fast { index } else { index.min(pages - 4) };
        let bytes = if ultralight_c || credential != TagCredential::None {
            reader
                .type2_pages_with_credential(source.model, first as u8, credential)?
                .to_vec()
        } else if fast {
            reader.type2_fast_pages(source.model, first as u8, count as u8)?
        } else {
            reader.type2_pages(first as u8)?.to_vec()
        };
        for page in index..index + count {
            memory[page] = Some(bytes[(page - first) * 4..(page - first + 1) * 4].to_vec());
        }
    }
    Ok(source
        .units
        .iter()
        .map(|(index, after)| TargetUnit {
            observed: ObservedUnit {
                index: *index,
                before: memory[*index as usize].clone(),
                after: Some(after.clone()),
                writable: physical::type2_page_writable(
                    source.model,
                    &memory,
                    *index as usize,
                    credential != TagCredential::None,
                ),
            },
            credential,
        })
        .collect())
}

fn confirm(
    input: &Input,
    different_uid: bool,
    question: &str,
    prompt_allowed: bool,
) -> Result<bool> {
    if input.dry_run {
        return Ok(false);
    }
    if input.yes {
        if different_uid && !input.allow_different_uid {
            return Err("Target UID differs; --yes also requires --allow-different-uid".into());
        }
        return Ok(true);
    }
    crate::ui::confirm(question, false, prompt_allowed)
}

fn execute<F: FnMut(&mut Reader<'_>) -> Result<Vec<TargetUnit>>>(
    input: Input,
    source: &physical::Source,
    mut targets: Vec<TargetUnit>,
    reader: &mut Reader<'_>,
    prompt_allowed: bool,
    mut refresh: F,
) -> Result<()> {
    let plan = physical::plan_user_data(
        source.model,
        targets.iter().map(|target| target.observed.clone()),
        false,
    )?;
    let different_uid = source.uid != reader.card.uid;
    crate::ui::line(
        "Restore user data",
        &[
            source.model.name().into(),
            format!("{} units to write", plan.writes.len()),
        ],
    );
    crate::ui::line(
        "UID",
        &[format!(
            "source {} → target {}",
            crate::ui::uid_text(&source.uid),
            crate::ui::uid_text(&reader.card.uid)
        )],
    );
    crate::ui::line(
        "Preserved",
        &["UID, keys, permissions, locks and configuration".into()],
    );
    for unit in &plan.writes {
        crate::ui::change(
            format!("{:03}", unit.index),
            crate::ui::uid_text(&unit.before),
            crate::ui::uid_text(&unit.after),
        );
    }
    if plan.writes.is_empty() {
        crate::ui::no_write("Unchanged");
        return Ok(());
    }
    let unit = if source.model.type2().is_some() {
        "page"
    } else {
        "block"
    };
    let question = format!(
        "Restore {} {unit}{} to tag {}?",
        plan.writes.len(),
        if plan.writes.len() == 1 { "" } else { "s" },
        crate::ui::uid_text(&reader.card.uid)
    );
    if input.dry_run {
        crate::ui::no_write("Preview");
    }
    if !confirm(&input, different_uid, &question, prompt_allowed)? {
        return Ok(());
    }
    let current = refresh(reader)?;
    if current.len() != targets.len()
        || current
            .iter()
            .zip(&targets)
            .any(|(after, before)| after.observed != before.observed)
    {
        return Err("Target data or permissions changed; nothing written".into());
    }
    targets = current;
    let mut verified = 0;
    for unit in &plan.writes {
        let credential = targets
            .iter()
            .find(|target| target.observed.index == unit.index)
            .unwrap()
            .credential;
        reader
            .write_user_unit(source.model, unit, credential)
            .map_err(|error| {
                format!(
                    "Stopped at unit {}: {error}; {verified} writes verified",
                    unit.index
                )
            })?;
        verified += 1;
    }
    crate::ui::result(true, format!("Written and verified · {verified} {unit}s"));
    Ok(())
}

fn parse_source(text: &str, family: Family) -> Result<physical::Source> {
    if !text.trim_start().starts_with('{') {
        return physical::Source::parse(text, family);
    }
    let value: serde_json::Value = serde_json::from_str(text)?;
    let document = &value["document"];
    if value["version"] != 1 {
        return Err("Unsupported browser document version".into());
    }
    let id: u16 = serde_json::from_value(document["type"].clone())?;
    let model = crate::slot_file::model(id)?;
    let (kind, count, size, expected_sak) = match family {
        Family::Classic => {
            let (_, count, _, sak) = model.classic().ok_or("Expected a Classic source")?;
            ("classic", count, 16, sak)
        }
        Family::Type2 => {
            let (count, _) = model.type2().ok_or("Expected a Type 2 source")?;
            ("type2", count, 4, 0)
        }
    };
    let uid: Vec<u8> = serde_json::from_value(document["card"]["uid"].clone())?;
    let sak: u8 = serde_json::from_value(document["card"]["sak"].clone())?;
    let _: [u8; 2] = serde_json::from_value(document["card"]["atqa"].clone())?;
    if document["kind"] != kind
        || document["unitBytes"] != size
        || sak & !0x20 != expected_sak
        || (family == Family::Type2 && (uid.len() != 7 || sak != 0))
        || (family == Family::Classic && !matches!(uid.len(), 4 | 7))
    {
        return Err("Source identity conflicts with its model".into());
    }
    let memory: Vec<Option<Vec<Option<u8>>>> = serde_json::from_value(document["memory"].clone())?;
    if memory.len() != count || memory.iter().flatten().any(|unit| unit.len() != size) {
        return Err("Source memory conflicts with its model".into());
    }
    let mut units = Vec::new();
    for (index, unit) in memory.into_iter().enumerate() {
        if physical::user_unit(model, index) {
            let bytes = unit.and_then(|bytes| bytes.into_iter().collect::<Option<Vec<_>>>());
            units.push((
                index as u8,
                bytes.ok_or_else(|| format!("Source user unit {index} contains unread bytes"))?,
            ));
        }
    }
    Ok(physical::Source { model, uid, units })
}

fn read_source(path: &std::path::Path, family: Family) -> Result<physical::Source> {
    let mut text = String::new();
    std::fs::File::open(path)?
        .take(128 * 1024 + 1)
        .read_to_string(&mut text)?;
    if text.len() > 128 * 1024 {
        return Err("Source exceeds 128 KiB".into());
    }
    parse_source(&text, family)
}

pub fn classic(
    options: ClassicOptions,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    let source = read_source(&options.input.file, Family::Classic)?;
    if options
        .model
        .is_some_and(|model| Model::from(model) != source.model)
    {
        return Err("Explicit target model must match the source model".into());
    }
    let keys = keys(&options)?;
    let mut client = Client::connect(port, cancelled)?;
    Reader::begin(&mut client)?.run(|reader| {
        if reader.card.model()? != source.model {
            return Err("Target model does not match the source".into());
        }
        let targets = classic_target(reader, &source, &keys)?;
        execute(
            options.input,
            &source,
            targets,
            reader,
            prompt_allowed,
            |reader| classic_target(reader, &source, &keys),
        )
    })
}

pub fn type2(
    options: Type2Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    let source = read_source(&options.input.file, Family::Type2)?;
    if source.model == Model::Ultralight {
        return Err("Original Ultralight writing is unsupported".into());
    }
    let credential = if let Some(value) = &options.key {
        if source.model != Model::UltralightC {
            return Err("--key requires an Ultralight C source".into());
        }
        TagCredential::UltralightC(crate::uid::hex_bytes(value, &[16])?.try_into().unwrap())
    } else if let Some(value) = &options.password {
        if source.model == Model::UltralightC {
            return Err("Ultralight C requires --key instead of --password".into());
        }
        TagCredential::Password(crate::uid::hex_bytes(value, &[4])?.try_into().unwrap())
    } else {
        TagCredential::None
    };
    if options
        .model
        .is_some_and(|model| Model::from(model) != source.model)
    {
        return Err("Explicit target model must match the source model".into());
    }
    let mut client = Client::connect(port, cancelled)?;
    Reader::begin(&mut client)?.run(|reader| {
        if reader.card.sak != 0 {
            return Err("Target is not a Type 2 tag".into());
        }
        let targets = type2_target(reader, &source, credential)?;
        execute(
            options.input,
            &source,
            targets,
            reader,
            prompt_allowed,
            |reader| type2_target(reader, &source, credential),
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn browser_type2_capture_restores_known_user_pages_without_inventing_credentials() {
        let mut memory = vec![Some(vec![0u8; 4]); 45];
        memory[0] = None;
        memory[43] = None;
        memory[44] = None;
        let mut document = serde_json::json!({ "version": 1, "document": {
            "kind": "type2", "type": 1100, "unitBytes": 4,
            "card": {"uid": [4, 1, 2, 3, 4, 5, 6], "sak": 0, "atqa": [0, 68]},
            "memory": memory
        }});
        let source = parse_source(&document.to_string(), Family::Type2).unwrap();
        assert_eq!(source.model, Model::Ntag213);
        assert_eq!(source.units.len(), 36);
        assert_eq!(source.units.first(), Some(&(4, vec![0; 4])));
        assert_eq!(source.units.last(), Some(&(39, vec![0; 4])));
        document["document"]["memory"][4][0] = serde_json::Value::Null;
        assert!(parse_source(&document.to_string(), Family::Type2).is_err());
        document["document"]["memory"][4][0] = serde_json::json!(0);
        document["document"]["card"]["sak"] = serde_json::json!(8);
        assert!(parse_source(&document.to_string(), Family::Type2).is_err());
    }
}
