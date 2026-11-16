use crate::Result;
use std::{
    io::{Read, Write},
    time::{Duration, Instant},
};

pub const MAX_PAYLOAD: usize = 4096;

pub fn encode(command: u16, status: u16, data: &[u8]) -> Result<Vec<u8>> {
    if data.len() > MAX_PAYLOAD {
        return Err("Command payload exceeds 4096 bytes".into());
    }
    let mut bytes = vec![0x11, 0xef];
    bytes.extend(command.to_be_bytes());
    bytes.extend(status.to_be_bytes());
    bytes.extend((data.len() as u16).to_be_bytes());
    bytes.push(checksum(&bytes));
    bytes.extend(data);
    bytes.push(checksum(&bytes));
    Ok(bytes)
}

fn checksum(bytes: &[u8]) -> u8 {
    0u8.wrapping_sub(bytes.iter().fold(0u8, |sum, byte| sum.wrapping_add(*byte)))
}

pub fn exchange(
    port: &mut (impl Read + Write),
    command: u16,
    data: &[u8],
    timeout: Duration,
) -> Result<(u16, Vec<u8>)> {
    let deadline = Instant::now() + timeout;
    port.write_all(&encode(command, 0, data)?)?;
    port.flush()?;
    read_frame(command, |bytes| read_before(port, bytes, deadline))
}

pub(crate) fn read_frame(
    command: u16,
    mut read_exact: impl FnMut(&mut [u8]) -> Result<()>,
) -> Result<(u16, Vec<u8>)> {
    let mut header = [0u8; 9];
    read_exact(&mut header)?;
    if header[..2] != [0x11, 0xef] || checksum(&header) != 0 {
        return Err("Invalid response header checksum".into());
    }
    if u16::from_be_bytes(header[2..4].try_into()?) != command {
        return Err("Response command does not match request".into());
    }
    let length = u16::from_be_bytes(header[6..8].try_into()?) as usize;
    if length > MAX_PAYLOAD {
        return Err("Response exceeds 4096 bytes".into());
    }
    let mut payload = vec![0; length + 1];
    read_exact(&mut payload)?;
    if checksum(&payload) != 0 {
        return Err("Invalid response payload checksum".into());
    }
    payload.pop();
    Ok((u16::from_be_bytes(header[4..6].try_into()?), payload))
}

fn read_before(port: &mut impl Read, mut bytes: &mut [u8], deadline: Instant) -> Result<()> {
    while !bytes.is_empty() {
        if Instant::now() >= deadline {
            return Err("Device response timed out; reconnect before continuing".into());
        }
        match port.read(bytes) {
            Ok(0) => return Err("Device disconnected".into()),
            Ok(count) => bytes = &mut bytes[count..],
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::Interrupted
                ) => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn version_request_matches_existing_protocol_vector() {
        assert_eq!(
            encode(1000, 0, &[]).unwrap(),
            [0x11, 0xef, 3, 0xe8, 0, 0, 0, 0, 0x15, 0]
        );
    }
    struct Port {
        response: std::io::Cursor<Vec<u8>>,
        writes: Vec<u8>,
    }
    impl Read for Port {
        fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
            let count = bytes.len().min(2);
            self.response.read(&mut bytes[..count])
        }
    }
    impl Write for Port {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.writes.extend(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    #[test]
    fn fragmented_responses_validate_command_length_and_checksums() {
        let mut port = Port {
            response: std::io::Cursor::new(encode(1000, 0x68, &[2, 2]).unwrap()),
            writes: vec![],
        };
        assert_eq!(
            exchange(&mut port, 1000, &[], Duration::from_secs(1)).unwrap(),
            (0x68, vec![2, 2])
        );
        assert_eq!(port.writes, encode(1000, 0, &[]).unwrap());
        for mut bytes in [
            encode(1001, 0, &[]).unwrap(),
            encode(1000, 0, &[2]).unwrap(),
        ] {
            *bytes.last_mut().unwrap() ^= 1;
            port.response = std::io::Cursor::new(bytes);
            assert!(exchange(&mut port, 1000, &[], Duration::from_secs(1)).is_err());
        }
        port.response = std::io::Cursor::new(vec![]);
        assert!(exchange(&mut port, 1000, &[], Duration::from_secs(1)).is_err());
    }

    #[test]
    fn oversized_payload_is_rejected_before_encoding() {
        assert!(encode(1000, 0, &vec![0; MAX_PAYLOAD + 1]).is_err());
    }
}
