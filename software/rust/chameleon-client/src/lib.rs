//! Chameleon USB protocol, tag operations and persistent slot images.
mod lf;
mod t55xx;
pub use t55xx::{LfChip, LfWritePlan};
mod lf_hid;
pub use lf::{LfNotFound, LfTag, LfType, ioprox_fields};
pub use lf_hid::{HID_FORMATS, hid_payload};
mod capture;
pub use capture::{CaptureBatch, CaptureMetrics, FirmwareCaptureMetrics};
mod command;
mod device;
pub use device::{DeviceIdentity, DeviceSettings};
mod slot_image;
mod slot_snapshot;
mod slots;
pub use command::DARKSIDE_STEP;
pub use slots::{
    SLOT_TYPES, Sense, Slot, SlotImage, SlotSide, type_name, type_sense, validate_name,
};
pub mod dfu;
mod frame;
use command::*;
mod desfire;
mod hf;
pub use desfire::DesfireSnapshot;
mod maintenance;
pub use maintenance::ClassicMaintenance;
mod magic;
pub use magic::{Gen2Evidence, MagicDetection, MagicUidMethod};
mod nfca;
mod sniff;
mod st25tb_stream;
pub use hf::{
    HfReader, HfSnapshot, HfTag, TearoffDiagnostics, TearoffLog, TearoffLogKind, TearoffOutcome,
    TearoffProgress, TearoffReport, TearoffResult, TearoffTiming, UnknownTearoffOutcome,
};
pub use nfca::{Card, ClassicRecovery, Reader, TagCredential};
pub use sniff::{
    SNIFF_CRC_CHECKED, SNIFF_CRC_VALID, SNIFF_DECODED, SNIFF_PARITY_CHECKED, SNIFF_PARITY_VALID,
    SNIFF_TRUNCATED, SniffBatch, SniffFrame, SniffSummary,
};
pub type Result<T> = nfc_native::Result<T>;
use serialport::SerialPort;
use std::{
    collections::HashSet,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

pub(crate) fn with_cleanup<T>(result: Result<T>, cleanup: Result<()>) -> Result<T> {
    match (result, cleanup) {
        (Ok(value), Ok(())) => Ok(value),
        (Err(error), Ok(())) => Err(error),
        (Ok(_), Err(error)) => Err(format!("Reader cleanup failed: {error}").into()),
        (Err(error), Err(cleanup)) => {
            Err(format!("{error}; reader cleanup failed: {cleanup}").into())
        }
    }
}

#[derive(Debug)]
pub struct DeviceError(pub u16);
#[derive(Debug)]
pub struct RequestCancelled;
impl std::fmt::Display for RequestCancelled {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(out, "Operation cancelled")
    }
}
impl std::error::Error for RequestCancelled {}
impl std::fmt::Display for DeviceError {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let message = match self.0 {
            1 => "No tag found",
            2 => "Card communication error",
            5 => "Invalid UID checksum (BCC)",
            6 => "Authentication rejected",
            0x41 => "LF detection inconclusive; check placement. Protected tags are unsupported",
            0x42 => "Password-mode LF targets cannot be programmed by this workflow",
            0x60 => "Firmware rejected the command arguments",
            0x67 => "Firmware does not support this command",
            0x69 => "Operation unsupported for this configuration",
            0x76 => "Device busy or target changed; check the target again",
            _ => "Device command failed",
        };
        write!(out, "{message} (0x{:02X})", self.0)
    }
}
impl std::error::Error for DeviceError {}

pub struct Client {
    port: Box<dyn SerialPort>,
    usable: bool,
    pub cancelled: Arc<AtomicBool>,
    pub capabilities: HashSet<u16>,
}

impl Client {
    pub fn connect(path: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<Self> {
        let path = match path {
            Some(path) => path.to_owned(),
            None => {
                let ports: Vec<_> = serialport::available_ports()?
                    .into_iter()
                    .filter(|port| {
                        matches!(&port.port_type, serialport::SerialPortType::UsbPort(info)
                        if info.vid == 0x6868 && info.pid == 0x8686)
                            && (!cfg!(target_os = "macos")
                                || port.port_name.starts_with("/dev/cu."))
                    })
                    .collect();
                if ports.len() != 1 {
                    return Err("Connect one Chameleon or select --port".into());
                }
                ports[0].port_name.clone()
            }
        };
        let mut port = serialport::new(path, 115200)
            .timeout(Duration::from_millis(50))
            .open()?;
        port.write_data_terminal_ready(true)?;
        port.clear(serialport::ClearBuffer::Input)?;
        let mut client = Self {
            port,
            usable: true,
            cancelled,
            capabilities: HashSet::new(),
        };
        let capabilities = client.command(CAPABILITIES, &[], Duration::from_secs(3))?;
        if !capabilities.len().is_multiple_of(2) {
            return Err("Invalid capabilities reply".into());
        }
        client.capabilities = capabilities
            .chunks_exact(2)
            .map(|id| u16::from_be_bytes([id[0], id[1]]))
            .collect();
        if client.command(DEVICE_MODEL, &[], Duration::from_secs(3))? != [0] {
            return Err("MIFARE reader operations require Chameleon Ultra".into());
        }
        Ok(client)
    }
    pub fn request(
        &mut self,
        command: u16,
        data: &[u8],
        timeout: Duration,
    ) -> Result<(u16, Vec<u8>)> {
        if !self.usable {
            return Err("Connection is unusable; reconnect before continuing".into());
        }
        if self.cancelled.load(Ordering::Relaxed) && command != ENTER_BOOTLOADER {
            return Err(RequestCancelled.into());
        }
        match frame::exchange(&mut self.port, command, data, timeout) {
            Ok(response) => Ok(response),
            Err(error) => {
                self.usable = false;
                Err(error)
            }
        }
    }
    /// Exchange ST25TB progress frames using the existing cancellation sequence.
    pub fn request_stream(
        &mut self,
        command: u16,
        data: &[u8],
        timeout: Duration,
        progress: &mut dyn FnMut(&[u8]) -> Result<()>,
    ) -> Result<(u16, Vec<u8>)> {
        if !self.usable {
            return Err("Connection is unusable; reconnect before continuing".into());
        }
        if self.cancelled.load(Ordering::Relaxed) {
            return Err(RequestCancelled.into());
        }
        match st25tb_stream::exchange(
            &mut self.port,
            command,
            data,
            timeout,
            &self.cancelled,
            progress,
        ) {
            Ok(response) => Ok(response),
            Err(error) => {
                self.usable = false;
                Err(error)
            }
        }
    }
    pub fn command(&mut self, command: u16, data: &[u8], timeout: Duration) -> Result<Vec<u8>> {
        let (status, data) = self.request(command, data, timeout)?;
        if !matches!(status, 0 | 0x68) {
            return Err(DeviceError(status).into());
        }
        Ok(data)
    }
    pub(crate) fn mutate(&mut self, id: u16, data: &[u8]) -> Result<()> {
        if let Err(error) = self.command(id, data, Duration::from_secs(3)) {
            if !self.usable {
                return Err(format!(
                    "{error}; mutation outcome is unknown. Inspect the device before retrying"
                )
                .into());
            }
            return Err(error);
        }
        Ok(())
    }

    pub fn port_name(&self) -> Option<String> {
        self.port.name()
    }

    pub fn reader_mode(&mut self) -> Result<bool> {
        match self
            .command(GET_MODE, &[], Duration::from_secs(3))?
            .as_slice()
        {
            [0] => Ok(false),
            [1] => Ok(true),
            _ => Err("Invalid device mode reply".into()),
        }
    }

    pub fn set_reader_mode(&mut self, reader: bool) -> Result<()> {
        self.mutate(SET_MODE, &[u8::from(reader)])?;
        if self.reader_mode()? != reader {
            return Err("Device mode verification failed".into());
        }
        Ok(())
    }

    pub(crate) fn restore_reader_mode(&mut self, reader: bool) -> Result<()> {
        if !self.usable {
            return Err(
                "Connection is unusable; previous device mode could not be restored".into(),
            );
        }
        let cancelled = self.cancelled.swap(false, Ordering::Relaxed);
        let result = self.set_reader_mode(reader);
        self.cancelled.fetch_or(cancelled, Ordering::Relaxed);
        result
    }

    /// Run in reader mode and restore the previous mode, including after cancellation.
    pub fn with_reader_mode<T>(
        &mut self,
        action: impl FnOnce(&mut Self) -> Result<T>,
    ) -> Result<T> {
        let previous = self.reader_mode()?;
        let result = (|| {
            if !previous {
                self.set_reader_mode(true)?;
            }
            action(self)
        })();
        let cleanup = if previous {
            Ok(())
        } else {
            self.restore_reader_mode(false)
        };
        with_cleanup(result, cleanup)
    }

    pub fn battery(&mut self) -> Result<(u16, u8)> {
        match self
            .command(GET_BATTERY, &[], Duration::from_secs(3))?
            .as_slice()
        {
            [hi, lo, percent @ 0..=100] => Ok((u16::from_be_bytes([*hi, *lo]), *percent)),
            _ => Err("Invalid battery reply".into()),
        }
    }

    pub fn info(&mut self) -> Result<String> {
        let version = self.command(APP_VERSION, &[], Duration::from_secs(3))?;
        let git = String::from_utf8(self.command(GIT_VERSION, &[], Duration::from_secs(3))?)?;
        let [major, minor] = version.as_slice() else {
            return Err("Invalid version reply".into());
        };
        Ok(format!("Chameleon Ultra {major}.{minor} ({git})"))
    }
}

#[cfg(all(test, unix))]
mod slot_tests;

#[cfg(all(test, unix))]
mod test_support;
