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
    let images = package::load(&options.package)?;
    ui::line("Firmware", &[options.package.display().to_string()]);
    for image in &images {
        ui::line(image.name, &[format!("{} bytes", image.firmware.len())]);
    }
    if images.iter().any(|image| image.name != "application") {
        ui::warning(
            "System firmware updates can overwrite slot storage with the stock bootloader; save slot exports before flashing",
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
        "Flash these images to this Ultra?",
        options.yes,
        prompt_allowed,
    )? {
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
