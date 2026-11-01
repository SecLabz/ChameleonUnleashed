//! Nordic secure serial DFU, independent of USB enumeration and ZIP files.
use super::check_cancelled;
use crate::Result;
use crc32fast::Hasher;
use std::{
    io::{Read, Write},
    sync::atomic::AtomicBool,
    time::{Duration, Instant},
};

const CREATE: u8 = 0x01;
const SET_PRN: u8 = 0x02;
const CRC: u8 = 0x03;
const EXECUTE: u8 = 0x04;
const SELECT: u8 = 0x06;
const MTU: u8 = 0x07;
const WRITE: u8 = 0x08;
const RESPONSE: u8 = 0x60;
const COMMAND: u8 = 1;
const DATA: u8 = 2;
const END: u8 = 0xc0;
const ESC: u8 = 0xdb;
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_RESPONSE_BYTES: usize = 64;

fn encode(operation: u8, payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity((payload.len() + 1) * 2 + 1);
    for byte in std::iter::once(&operation).chain(payload) {
        match *byte {
            END => frame.extend([ESC, 0xdc]),
            ESC => frame.extend([ESC, 0xdd]),
            byte => frame.push(byte),
        }
    }
    frame.push(END);
    frame
}

pub(super) struct Session<'a, P> {
    port: P,
    cancelled: &'a AtomicBool,
    packet_bytes: usize,
}

impl<'a, P: Read + Write> Session<'a, P> {
    pub(super) fn open(port: P, cancelled: &'a AtomicBool) -> Result<Self> {
        let mut session = Self {
            port,
            cancelled,
            packet_bytes: 0,
        };
        // One acknowledgement per write bounds in-flight USB buffers and detects
        // corruption before another chunk or object is sent.
        session.request::<0>(SET_PRN, &1u16.to_le_bytes())?;
        let mtu = u16::from_le_bytes(session.request(MTU, &[])?) as usize;
        if mtu < 5 {
            return Err("Invalid DFU serial MTU".into());
        }
        session.packet_bytes = ((mtu - 1) / 2 - 1).min(1024);
        Ok(session)
    }

    fn send(&mut self, operation: u8, payload: &[u8]) -> Result<()> {
        check_cancelled(self.cancelled)?;
        self.port.write_all(&encode(operation, payload))?;
        self.port.flush()?;
        Ok(())
    }

    fn response<const N: usize>(&mut self, operation: u8) -> Result<[u8; N]> {
        let deadline = Instant::now() + RESPONSE_TIMEOUT;
        let mut bytes = Vec::new();
        let mut escaped = false;
        loop {
            check_cancelled(self.cancelled)?;
            if Instant::now() >= deadline {
                return Err("DFU response timed out".into());
            }
            let mut byte = [0];
            match self.port.read(&mut byte) {
                Ok(0) => return Err("USB DFU disconnected".into()),
                Ok(_) => {}
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::TimedOut | std::io::ErrorKind::Interrupted
                    ) =>
                {
                    continue;
                }
                Err(error) => return Err(error.into()),
            }
            if escaped {
                bytes.push(match byte[0] {
                    0xdc => END,
                    0xdd => ESC,
                    _ => return Err("Invalid DFU SLIP escape".into()),
                });
                escaped = false;
            } else {
                match byte[0] {
                    END if bytes.is_empty() => continue,
                    END => break,
                    ESC => {
                        escaped = true;
                        continue;
                    }
                    byte => bytes.push(byte),
                }
            }
            if bytes.len() > MAX_RESPONSE_BYTES {
                return Err("Oversized DFU response".into());
            }
        }
        if bytes.len() < 3 || bytes[..2] != [RESPONSE, operation] {
            return Err(format!("Unexpected DFU response to opcode 0x{operation:02X}").into());
        }
        if bytes[2] != 1 {
            let reason = match (bytes[2], bytes.get(3)) {
                (0x0b, Some(0x05)) => "firmware version rejected",
                _ => "operation rejected",
            };
            return Err(format!(
                "DFU opcode 0x{operation:02X}: {reason} (status 0x{:02X}, details {:02X?})",
                bytes[2],
                &bytes[3..]
            )
            .into());
        }
        bytes[3..]
            .try_into()
            .map_err(|_| "Invalid DFU response length".into())
    }

    fn request<const N: usize>(&mut self, operation: u8, payload: &[u8]) -> Result<[u8; N]> {
        self.send(operation, payload)?;
        self.response(operation)
    }

    fn select(&mut self, kind: u8) -> Result<(usize, usize, u32)> {
        let bytes = self.request::<12>(SELECT, &[kind])?;
        let max = u32::from_le_bytes(bytes[..4].try_into()?) as usize;
        let offset = u32::from_le_bytes(bytes[4..8].try_into()?) as usize;
        let crc = u32::from_le_bytes(bytes[8..].try_into()?);
        if max == 0 {
            return Err("DFU reported a zero object size".into());
        }
        Ok((max, offset, crc))
    }

    fn create(&mut self, kind: u8, size: usize) -> Result<()> {
        let mut payload = vec![kind];
        payload.extend((size as u32).to_le_bytes());
        self.request::<0>(CREATE, &payload)?;
        Ok(())
    }

    fn write(&mut self, data: &[u8], offset: &mut usize, hash: &mut Hasher) -> Result<()> {
        for chunk in data.chunks(self.packet_bytes) {
            self.send(WRITE, chunk)?;
            hash.update(chunk);
            *offset += chunk.len();
            // PRN=1 returns a CRC response, rather than an OBJECT_WRITE response.
            let reply = self.response::<8>(CRC)?;
            let actual_offset = u32::from_le_bytes(reply[..4].try_into()?) as usize;
            let actual_crc = u32::from_le_bytes(reply[4..].try_into()?);
            if actual_offset != *offset || actual_crc != hash.clone().finalize() {
                return Err(
                    "DFU offset/CRC mismatch; transfer stopped before executing the object".into(),
                );
            }
        }
        Ok(())
    }

    pub(super) fn flash(
        &mut self,
        init: &[u8],
        firmware: &[u8],
        mut progress: impl FnMut(usize),
    ) -> Result<()> {
        let (max, _, _) = self.select(COMMAND)?;
        if init.is_empty() || init.len() > max || firmware.is_empty() {
            return Err("Invalid DFU init packet or empty firmware image".into());
        }
        // Creating a command resets bootloader progress. Explicit retries start
        // from zero, so stale or interrupted images are never combined.
        self.create(COMMAND, init.len())?;
        let mut offset = 0;
        self.write(init, &mut offset, &mut Hasher::new())?;
        self.request::<0>(EXECUTE, &[])?;

        let (max, offset, crc) = self.select(DATA)?;
        if offset != 0 || crc != 0 {
            return Err("DFU firmware progress was not reset by the init packet".into());
        }
        let mut offset = 0;
        let mut hash = Hasher::new();
        for chunk in firmware.chunks(max) {
            self.create(DATA, chunk.len())?;
            self.write(chunk, &mut offset, &mut hash)?;
            self.request::<0>(EXECUTE, &[])?;
            progress(offset);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
