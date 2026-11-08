//! Firmware update presentation; package decoding and USB transport have separate owners.
mod package;

use crate::{progress::Progress, ui};
use chameleon_client::{
    Result,
    dfu::{Device, Image},
};
use clap::Args;
use std::{
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
};

#[derive(Args)]
pub struct Options {
    /// Existing signed application or full Nordic DFU ZIP.
    #[arg(value_hint = clap::ValueHint::FilePath)]
    package: PathBuf,
    /// Validate the ZIP and preview its images without opening USB.
    #[arg(long, conflicts_with = "yes")]
    dry_run: bool,
    /// Confirm the firmware update without prompting.
    #[arg(long)]
    yes: bool,
    /// Flash every image in the ZIP, even an already installed bootloader.
    #[arg(long)]
    full: bool,
}

fn bootloader_revision(version: u32) -> Option<u16> {
    // New Unleashed bootloaders reserve the upper bytes for their family.
    // Legacy counters, including the reused version 5, never enable skipping.
    let bytes = version.to_be_bytes();
    let revision = u16::from_be_bytes([bytes[2], bytes[3]]);
    (bytes[..2] == *b"CU" && revision != 0).then_some(revision)
}

fn matching_bootloader(
    images: &[package::Image],
    installed: Option<u32>,
    full: bool,
) -> Option<usize> {
    if full {
        return None;
    }
    images.iter().position(|image| {
        image.name == "bootloader"
            && image.version.is_some_and(|version| {
                bootloader_revision(version).is_some() && installed == Some(version)
            })
    })
}

pub fn enter(port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    let device = Device::discover(port, cancelled)?;
    ui::line(
        "Device",
        &[device.serial_number().into(), device.port_name().into()],
    );
    let progress = Progress::new("Entering USB DFU", None, false);
    let path = device.enter()?;
    drop(progress);
    ui::result(true, format!("USB DFU ready · {path}"));
    Ok(())
}

pub fn flash(
    options: Options,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
) -> Result<()> {
    let mut images = package::load(&options.package)?;
    ui::line("Firmware", &[options.package.display().to_string()]);
    for image in &images {
        ui::line(image.name, &[format!("{} bytes", image.firmware.len())]);
    }
    if images.iter().any(|image| image.name != "application") {
        ui::warning(
            "System firmware updates can overwrite slot storage with the stock bootloader; save slot exports before flashing",
        );
    }
    let target_revision = images
        .iter()
        .find(|image| image.name == "bootloader")
        .and_then(|image| image.version)
        .and_then(bootloader_revision);
    if images.iter().any(|image| image.name == "bootloader") {
        ui::line(
            "Bootloader",
            &[if options.full {
                "Will be reinstalled (--full)".into()
            } else if target_revision.is_some() {
                "Will be kept if the installed Unleashed version matches".into()
            } else {
                "Legacy or unversioned package · will be installed".into()
            }],
        );
    }
    if options.dry_run {
        ui::no_write("Preview");
        return Ok(());
    }
    let device = Device::discover(port, cancelled)?;
    ui::line(
        "Device",
        &[device.serial_number().into(), device.port_name().into()],
    );
    if !ui::confirm(
        "Install this firmware on this Ultra?",
        options.yes,
        prompt_allowed,
    )? {
        return Ok(());
    }
    let installed = if !options.full && target_revision.is_some() {
        let progress = Progress::new("Checking bootloader", None, false);
        let version = device.bootloader_version()?;
        drop(progress);
        version
    } else {
        None
    };
    if let Some(index) = matching_bootloader(&images, installed, options.full) {
        images.remove(index);
        ui::line(
            "Bootloader",
            &["Matching Unleashed version already installed · keeping it".into()],
        );
    }
    if images.is_empty() {
        device.leave()?;
        ui::result(true, "Bootloader already up to date · no images flashed");
        return Ok(());
    }
    let transfers = images
        .iter()
        .map(|image| Image {
            init: &image.init,
            firmware: &image.firmware,
        })
        .collect::<Vec<_>>();
    let mut progress = Progress::new("Flashing firmware", None, false);
    let mut last = (0, 0);
    let result = device
        .flash(&transfers, |index, done| {
            progress.phase(match images[index].name {
                "application" => "Flashing application",
                "bootloader" => "Flashing bootloader",
                "softdevice" => "Flashing SoftDevice",
                _ => "Flashing SoftDevice + bootloader",
            });
            last = (index, done);
        })
        .and_then(|()| {
            if images
                .last()
                .is_some_and(|image| image.name == "application")
            {
                progress.phase("Verifying application startup");
                device.wait_application().map(Some)
            } else {
                Ok(None)
            }
        });
    progress.finish();
    let (index, done) = last;
    let accepted = index + usize::from(done == images[index].firmware.len());
    let running = match result {
        Ok(running) => running,
        Err(error) => {
            let message = if accepted == images.len() {
                format!("{accepted} images accepted · application startup not verified")
            } else {
                format!(
                    "Flash stopped · {accepted}/{} images accepted · {done}/{} bytes accepted for {}",
                    images.len(),
                    images[index].firmware.len(),
                    images[index].name
                )
            };
            ui::result(false, message);
            return Err(error);
        }
    };
    ui::result(
        true,
        match running {
            Some(info) => format!("{accepted} images accepted · {info} running"),
            None => format!("{accepted} images accepted by bootloader · device restarting"),
        },
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(name: &'static str, version: Option<u32>) -> package::Image {
        package::Image {
            name,
            version,
            init: vec![1],
            firmware: vec![2],
        }
    }

    #[test]
    fn only_a_matching_versioned_bootloader_is_kept_and_full_always_reinstalls_it() {
        let version = u32::from_be_bytes(*b"CU\0\x01");
        let images = vec![
            image("bootloader", Some(version)),
            image("application", Some(1)),
        ];
        assert_eq!(matching_bootloader(&images, Some(version), false), Some(0));
        for installed in [None, Some(5), Some(version + 1), Some(version - 1)] {
            assert_eq!(matching_bootloader(&images, installed, false), None);
        }
        assert_eq!(matching_bootloader(&images, Some(version), true), None);
        for name in ["application", "softdevice", "softdevice_bootloader"] {
            assert_eq!(
                matching_bootloader(&[image(name, Some(version))], Some(version), false),
                None
            );
        }
        for legacy in [
            None,
            Some(5),
            Some(0),
            Some(u32::from_be_bytes(*b"XX\0\x01")),
        ] {
            assert_eq!(
                matching_bootloader(&[image("bootloader", legacy)], legacy, false),
                None
            );
        }
    }
}
