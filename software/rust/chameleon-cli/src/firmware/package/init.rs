//! Read Nordic dfu-cc.proto metadata; cryptographic validation belongs to the bootloader.
use chameleon_client::Result;

enum Value<'a> {
    Integer(u64),
    Bytes(&'a [u8]),
    Fixed,
}

fn take<'a>(input: &mut &'a [u8], size: usize) -> Result<&'a [u8]> {
    let (value, rest) = input
        .split_at_checked(size)
        .ok_or("Truncated DFU init packet")?;
    *input = rest;
    Ok(value)
}

fn varint(input: &mut &[u8]) -> Result<u64> {
    let mut value = 0;
    for shift in (0..64).step_by(7) {
        let byte = take(input, 1)?[0];
        if shift == 63 && byte > 1 {
            return Err("Overflowing DFU init integer".into());
        }
        value |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            return Ok(value);
        }
    }
    Err("Invalid DFU init integer".into())
}

fn field(mut input: &[u8], wanted: u64) -> Result<Option<Value<'_>>> {
    let mut found = None;
    while !input.is_empty() {
        let key = varint(&mut input)?;
        if key >> 3 == 0 || key >> 3 > 0x1fff_ffff {
            return Err("Invalid DFU init field number".into());
        }
        let value = match key & 7 {
            0 => Value::Integer(varint(&mut input)?),
            2 => {
                let size = usize::try_from(varint(&mut input)?)?;
                Value::Bytes(take(&mut input, size)?)
            }
            wire @ (1 | 5) => {
                take(&mut input, if wire == 1 { 8 } else { 4 })?;
                Value::Fixed
            }
            _ => return Err("Unsupported DFU init wire type".into()),
        };
        if key >> 3 == wanted {
            if found.is_some() {
                return Err("Duplicate DFU init metadata field".into());
            }
            found = Some(value);
        }
    }
    Ok(found)
}

fn message(input: &[u8], number: u64) -> Result<&[u8]> {
    match field(input, number)? {
        Some(Value::Bytes(bytes)) => Ok(bytes),
        _ => Err("Missing or invalid DFU init message".into()),
    }
}

fn integer(input: &[u8], number: u64) -> Result<u64> {
    match field(input, number)? {
        Some(Value::Integer(value)) => Ok(value),
        None => Ok(0),
        _ => Err("Invalid DFU init metadata integer".into()),
    }
}

pub(super) fn validate(packet: &[u8], name: &str, firmware_bytes: usize) -> Result<Option<u32>> {
    if field(packet, 1)?.is_some() {
        return Err("DFU init packet must contain only a signed command".into());
    }
    let signed = message(packet, 2)?;
    if !matches!(field(signed, 2)?, Some(Value::Integer(0))) || message(signed, 3)?.len() != 64 {
        return Err("DFU init packet requires a 64-byte ECDSA signature".into());
    }
    let command = message(signed, 1)?;
    if !matches!(field(command, 1)?, None | Some(Value::Integer(1))) {
        return Err("Unsupported DFU init command".into());
    }
    let init = message(command, 2)?;
    let expected = match name {
        "application" => 0,
        "softdevice" => 1,
        "bootloader" => 2,
        "softdevice_bootloader" => 3,
        _ => unreachable!(),
    };
    if integer(init, 4)? != expected {
        return Err(
            format!("DFU manifest type {name} disagrees with the signed init packet").into(),
        );
    }
    let sd = integer(init, 5)?;
    let bl = integer(init, 6)?;
    let app = integer(init, 7)?;
    let bytes = firmware_bytes as u64;
    let valid = match expected {
        0 => sd == 0 && bl == 0 && app == bytes,
        1 => sd == bytes && bl == 0 && app == 0,
        2 => sd == 0 && bl == bytes && app == 0,
        _ => sd > 0 && bl > 0 && sd.checked_add(bl) == Some(bytes) && app == 0,
    };
    if !valid {
        return Err(format!("DFU {name} size disagrees with the signed init packet").into());
    }
    match field(init, 1)? {
        Some(Value::Integer(version)) => Ok(Some(u32::try_from(version)?)),
        None => Ok(None),
        _ => Err("Invalid DFU firmware version".into()),
    }
}

#[cfg(test)]
pub(super) mod tests;
