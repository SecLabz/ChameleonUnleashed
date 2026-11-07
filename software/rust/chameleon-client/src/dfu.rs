//! USB discovery and reboot lifecycle for the Nordic serial DFU bootloader.
mod protocol;

use crate::{Client, RequestCancelled, Result, command::ENTER_BOOTLOADER, frame};
use serialport::{SerialPortInfo, SerialPortType};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

const APPLICATION_USB_ID: (u16, u16) = (0x6868, 0x8686);
const BOOTLOADER_USB_ID: (u16, u16) = (0x1915, 0x521f);
const ENUMERATION_TIMEOUT: Duration = Duration::from_secs(30);
const POLL_INTERVAL: Duration = Duration::from_millis(100);
// Activation copies flash banks and restarts USB between images in a full package.
const ACTIVATION_DELAY: Duration = Duration::from_secs(3);

// Keep the original handle open while USB resets, including after an error or
// Ctrl-C. Watching the descriptor also handles re-enumeration at the same path.
fn with_usb_port<T>(
    path: &str,
    timeout: Duration,
    operation: impl FnOnce(&mut dyn serialport::SerialPort) -> Result<T>,
) -> Result<T> {
    let builder = serialport::new(path, 115200).timeout(timeout);
    #[cfg(not(target_os = "macos"))]
    {
        operation(builder.open()?.as_mut())
    }
    #[cfg(target_os = "macos")]
    {
        let mut port = builder.open_native()?;
        let result = operation(&mut port);
        let reset_expected = result.is_ok();
        let timeout = if reset_expected {
            ENUMERATION_TIMEOUT
        } else {
            ACTIVATION_DELAY
        };
        let cleanup = match wait_usb_disconnect(&port, timeout) {
            Ok(false) if reset_expected => {
                Err("USB reset was not observed within 30 seconds".into())
            }
            // An interrupted transfer can leave DFU active.
            Ok(_) => Ok(()),
            Err(error) => Err(error),
        };
        // Let driver teardown settle before Drop closes the descriptor. This
        // cleanup must run even when cancellation has already been requested.
        thread::sleep(Duration::from_millis(300));
        drop(port);
        result.and_then(|value| cleanup.map(|()| value))
    }
}

#[cfg(target_os = "macos")]
fn wait_usb_disconnect(port: &impl std::os::fd::AsRawFd, timeout: Duration) -> Result<bool> {
    let deadline = Instant::now() + timeout;
    let mut watched = libc::pollfd {
        fd: port.as_raw_fd(),
        // Darwin TTYs require these events explicitly; an empty mask misses hangups.
        events: libc::POLLHUP | libc::POLLERR,
        revents: 0,
    };
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        // SAFETY: watched contains a live descriptor borrowed from port. poll
        // only observes hangup/error events; it performs no serial I/O.
        let status = unsafe {
            libc::poll(
                &mut watched,
                1,
                remaining.min(POLL_INTERVAL).as_millis() as i32,
            )
        };
        if status < 0 {
            let error = std::io::Error::last_os_error();
            if error.kind() == std::io::ErrorKind::Interrupted && Instant::now() < deadline {
                continue;
            }
            return Err(error.into());
        }
        if watched.revents & (libc::POLLHUP | libc::POLLERR) != 0 {
            return Ok(true);
        }
        if watched.revents & libc::POLLNVAL != 0 {
            return Err("USB serial descriptor is invalid".into());
        }
        if Instant::now() >= deadline {
            return Ok(false);
        }
    }
}

/// Signed init packet and firmware bytes, in package installation order.
pub struct Image<'a> {
    pub init: &'a [u8],
    pub firmware: &'a [u8],
}

/// A selected USB identity, retained across application/bootloader port changes.
pub struct Device {
    port: SerialPortInfo,
    serial: String,
    cancelled: Arc<AtomicBool>,
}

fn usb_id(port: &SerialPortInfo) -> Option<(u16, u16)> {
    match &port.port_type {
        SerialPortType::UsbPort(info) => Some((info.vid, info.pid)),
        _ => None,
    }
}

fn select(ports: Vec<SerialPortInfo>, path: Option<&str>) -> Result<SerialPortInfo> {
    let mut matches = ports.into_iter().filter(|port| {
        matches!(usb_id(port), Some(APPLICATION_USB_ID | BOOTLOADER_USB_ID))
            && path.map_or_else(
                || !cfg!(target_os = "macos") || port.port_name.starts_with("/dev/cu."),
                |path| port.port_name == path,
            )
    });
    let port = matches
        .next()
        .ok_or("Connect an Ultra in application or USB DFU mode, or select its USB --port")?;
    if matches.next().is_some() {
        return Err("Connect one Chameleon or select its USB --port".into());
    }
    if let SerialPortType::UsbPort(info) = &port.port_type
        && info
            .product
            .as_deref()
            .is_some_and(|name| name.contains("ChameleonLite"))
    {
        return Err("USB flashing supports Chameleon Ultra only".into());
    }
    Ok(port)
}

fn matching_port(
    ports: Vec<SerialPortInfo>,
    serial: &str,
    expected: Option<(u16, u16)>,
) -> Result<Option<SerialPortInfo>> {
    let ports = ports.into_iter().filter(|port| {
        matches!(usb_id(port), Some(APPLICATION_USB_ID | BOOTLOADER_USB_ID))
            && expected.is_none_or(|expected| usb_id(port) == Some(expected))
            && matches!(&port.port_type, SerialPortType::UsbPort(info)
                if info.serial_number.as_deref().is_some_and(|value| value.eq_ignore_ascii_case(serial)))
    }).collect::<Vec<_>>();
    if ports.is_empty() {
        return Ok(None);
    }
    select(ports, None).map(Some)
}

fn check_cancelled(cancelled: &AtomicBool) -> Result<()> {
    if cancelled.load(Ordering::Relaxed) {
        return Err(RequestCancelled.into());
    }
    Ok(())
}

fn pause(duration: Duration, cancelled: &AtomicBool) -> Result<()> {
    let deadline = Instant::now() + duration;
    while Instant::now() < deadline {
        check_cancelled(cancelled)?;
        thread::sleep(POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())));
    }
    Ok(())
}

fn wait_port(
    serial: &str,
    expected: (u16, u16),
    cancelled: &AtomicBool,
    timeout: Duration,
    mut ports: impl FnMut() -> serialport::Result<Vec<SerialPortInfo>>,
) -> Result<Option<SerialPortInfo>> {
    let deadline = Instant::now() + timeout;
    loop {
        check_cancelled(cancelled)?;
        if let Some(port) = matching_port(ports()?, serial, Some(expected))? {
            return Ok(Some(port));
        }
        if Instant::now() >= deadline {
            return Ok(None);
        }
        pause(POLL_INTERVAL, cancelled)?;
    }
}

impl Device {
    /// Enumerate without opening a port or changing device state.
    pub fn discover(path: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<Self> {
        check_cancelled(&cancelled)?;
        let port = select(serialport::available_ports()?, path)?;
        let SerialPortType::UsbPort(info) = &port.port_type else {
            unreachable!();
        };
        let serial = info
            .serial_number
            .as_ref()
            .filter(|value| !value.is_empty())
            .ok_or("The USB device has no serial number; cannot follow it through DFU reboots")?
            .clone();
        Ok(Self {
            port,
            serial,
            cancelled,
        })
    }

    pub fn port_name(&self) -> &str {
        &self.port.port_name
    }

    pub fn serial_number(&self) -> &str {
        &self.serial
    }

    fn bootloader_port(&self) -> Result<SerialPortInfo> {
        wait_port(
            &self.serial,
            BOOTLOADER_USB_ID,
            &self.cancelled,
            ENUMERATION_TIMEOUT,
            serialport::available_ports,
        )?
        .ok_or_else(|| "USB DFU did not appear within 30 seconds. Power off, hold B, plug in USB, then release B; LEDs 4 and 5 should blink".into())
    }

    /// Wait for this USB identity in application mode and verify its protocol responds.
    pub fn wait_application(&self) -> Result<String> {
        let port = wait_port(
            &self.serial,
            APPLICATION_USB_ID,
            &self.cancelled,
            ENUMERATION_TIMEOUT,
            serialport::available_ports,
        )?
        .ok_or("Application USB did not appear within 30 seconds; return to DFU to diagnose the device")?;
        Client::connect(Some(&port.port_name), self.cancelled.clone())?.info()
    }

    /// Reboot without waiting for a reply on the disappearing application port.
    pub fn enter(&self) -> Result<String> {
        check_cancelled(&self.cancelled)?;
        // DFU may time out to the application while the user reviews a package.
        // Re-enumerate the selected identity before sending a reboot command.
        if let Some(current) = matching_port(serialport::available_ports()?, &self.serial, None)?
            && usb_id(&current) == Some(APPLICATION_USB_ID)
        {
            let result = with_usb_port(&current.port_name, Duration::from_secs(1), |port| {
                port.write_data_terminal_ready(true)?;
                port.write_all(&frame::encode(ENTER_BOOTLOADER, 0, &[])?)?;
                port.flush()?;
                Ok(())
            });
            // A reset can disconnect USB before write/flush returns. Rediscovery
            // of this exact serial number is the authoritative success check.
            return self.bootloader_port().map(|port| port.port_name).map_err(
                |error| match result {
                    Ok(()) => error,
                    Err(trigger) => format!("DFU entry failed: {trigger}; {error}").into(),
                },
            );
        }
        Ok(self.bootloader_port()?.port_name)
    }

    /// Read the standard Nordic bootloader version; older DFU implementations
    /// may explicitly report that this optional request is unsupported.
    pub fn bootloader_version(&self) -> Result<Option<u32>> {
        let path = self.enter()?;
        let mut port = serialport::new(path, 115200)
            .timeout(Duration::from_millis(200))
            .open()?;
        port.write_data_terminal_ready(true)?;
        port.clear(serialport::ClearBuffer::Input)?;
        protocol::Session::open(&mut port, &self.cancelled)?.bootloader_version()
    }

    /// Leave an unchanged bootloader without creating or transferring an image.
    pub fn leave(&self) -> Result<()> {
        let path = self.bootloader_port()?.port_name;
        with_usb_port(&path, Duration::from_millis(200), |port| {
            port.write_data_terminal_ready(true)?;
            protocol::Session::open(port, &self.cancelled)?.leave()
        })
    }

    /// Transfer each image once. A new invocation starts over, including after cancellation.
    /// Progress counts only firmware bytes accepted by OBJECT_EXECUTE.
    pub fn flash(
        &self,
        images: &[Image<'_>],
        mut progress: impl FnMut(usize, usize),
    ) -> Result<()> {
        if images.is_empty()
            || images.iter().any(|image| {
                image.init.is_empty()
                    || image.firmware.is_empty()
                    || image.init.len() > u32::MAX as usize
                    || image.firmware.len() > u32::MAX as usize
            })
        {
            return Err(
                "DFU requires nonempty init packets and firmware images within 32-bit sizes".into(),
            );
        }
        self.enter()?;
        for (index, image) in images.iter().enumerate() {
            if index > 0 {
                pause(ACTIVATION_DELAY, &self.cancelled)?;
                // A bootloader-only update can restart a still-valid previous
                // application. Re-enter DFU on that same device for the next image.
                self.enter()?;
            }
            progress(index, 0);
            let path = self.bootloader_port()?.port_name;
            with_usb_port(&path, Duration::from_millis(200), |port| {
                port.write_data_terminal_ready(true)?;
                port.clear(serialport::ClearBuffer::Input)?;
                let mut session = protocol::Session::open(port, &self.cancelled)?;
                session.flash(image.init, image.firmware, |done| progress(index, done))
            })?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
