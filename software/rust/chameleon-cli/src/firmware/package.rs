//! Bounded, in-memory Nordic DFU ZIP decoding; no device or filesystem extraction.
mod init;

use chameleon_client::Result;
use serde_json::Value;
use std::{
    fs::File,
    io::{Read, Seek},
    path::Path,
};
use zip::ZipArchive;

const MAX_ARCHIVE_BYTES: u64 = 16 * 1024 * 1024;
const MAX_MANIFEST_BYTES: usize = 64 * 1024;
const MAX_INIT_BYTES: usize = 4096;
const MAX_FIRMWARE_BYTES: usize = 1024 * 1024;
const ORDER: [&str; 4] = [
    "softdevice_bootloader",
    "softdevice",
    "bootloader",
    "application",
];

pub(super) struct Image {
    pub name: &'static str,
    pub init: Vec<u8>,
    pub firmware: Vec<u8>,
}

pub(super) fn load(path: &Path) -> Result<Vec<Image>> {
    let file = File::open(path)?;
    if file.metadata()?.len() > MAX_ARCHIVE_BYTES {
        return Err("DFU ZIP exceeds 16 MiB".into());
    }
    decode(file)
}

fn read<R: Read + Seek>(archive: &mut ZipArchive<R>, name: &str, max: usize) -> Result<Vec<u8>> {
    let file = archive.by_name(name)?;
    if file.is_dir() || file.size() == 0 || file.size() > max as u64 {
        return Err(format!("Invalid DFU entry {name:?}: expected 1..={max} bytes").into());
    }
    let mut bytes = Vec::new();
    file.take(max as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.is_empty() || bytes.len() > max {
        return Err(format!("Invalid DFU entry size: {name:?}").into());
    }
    Ok(bytes)
}

fn decode<R: Read + Seek>(reader: R) -> Result<Vec<Image>> {
    let mut archive = ZipArchive::new(reader)?;
    if archive.len() > 128 {
        return Err("DFU ZIP exceeds 128 entries".into());
    }
    let document: Value =
        serde_json::from_slice(&read(&mut archive, "manifest.json", MAX_MANIFEST_BYTES)?)?;
    let manifest = document
        .get("manifest")
        .and_then(Value::as_object)
        .ok_or("DFU ZIP must contain a manifest object")?;
    if manifest.keys().any(|name| !ORDER.contains(&name.as_str())) {
        return Err("DFU manifest contains an unsupported image type".into());
    }
    if !manifest
        .get("softdevice_bootloader")
        .is_none_or(Value::is_null)
        && ["softdevice", "bootloader"]
            .iter()
            .any(|name| !manifest.get(*name).is_none_or(Value::is_null))
    {
        return Err("DFU manifest mixes combined and separate SoftDevice/bootloader images".into());
    }
    let mut images = Vec::new();
    for name in ORDER {
        let Some(image) = manifest.get(name).filter(|value| !value.is_null()) else {
            continue;
        };
        let file_name = |key: &str| -> Result<&str> {
            image
                .get(key)
                .and_then(Value::as_str)
                .filter(|name| !name.is_empty())
                .ok_or_else(|| format!("DFU {name} is missing {key}").into())
        };
        let init = read(&mut archive, file_name("dat_file")?, MAX_INIT_BYTES)?;
        let firmware = read(&mut archive, file_name("bin_file")?, MAX_FIRMWARE_BYTES)?;
        init::validate(&init, name, firmware.len())?;
        images.push(Image {
            name,
            init,
            firmware,
        });
    }
    if images.is_empty() {
        return Err("DFU manifest contains no firmware images".into());
    }
    Ok(images)
}

#[cfg(test)]
mod tests;
