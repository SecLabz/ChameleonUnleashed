//! Existing firmware HID format identifiers and field limits.
use crate::Result;
pub const HID_FORMATS: &[(&str, u8, [u64; 4])] = &[
    ("H10301", 1, [255, 65535, 0, 0]),
    ("IND26", 2, [4095, 4095, 0, 0]),
    ("IND27", 3, [8191, 16383, 0, 0]),
    ("INDASC27", 4, [8191, 16383, 0, 0]),
    ("TECOM27", 5, [2047, 65535, 0, 0]),
    ("W2804", 6, [255, 32767, 0, 0]),
    ("IND29", 7, [8191, 65535, 0, 0]),
    ("ATSW30", 8, [4095, 65535, 0, 0]),
    ("ADT31", 9, [15, 8388607, 0, 0]),
    ("HCP32", 10, [0, 16777215, 0, 0]),
    ("HPP32", 11, [4095, 524287, 0, 0]),
    ("KASTLE", 12, [255, 65535, 31, 0]),
    ("KANTECH", 13, [255, 65535, 0, 0]),
    ("WIE32", 14, [4095, 65535, 0, 0]),
    ("D10202", 15, [127, 16777215, 0, 0]),
    ("H10306", 16, [65535, 65535, 0, 0]),
    ("N10002", 17, [65535, 65535, 0, 0]),
    ("OPTUS34", 18, [2047, 65535, 0, 0]),
    ("SMP34", 19, [8191, 65535, 7, 0]),
    ("BQT34", 20, [255, 16777215, 0, 0]),
    ("C1K35S", 21, [4095, 1048575, 0, 0]),
    ("C15001", 22, [255, 65535, 0, 1023]),
    ("S12906", 23, [255, 16777215, 3, 0]),
    ("ACTPHID", 42, [255, 65535, 0, 1023]),
    ("SIE36", 24, [262143, 65535, 0, 0]),
    ("H10320", 25, [0, 99999999, 0, 0]),
    ("H10302", 26, [0, 34359738367, 0, 0]),
    ("H10304", 27, [65535, 524287, 0, 0]),
    ("P10004", 28, [8191, 262143, 0, 0]),
    ("HGEN37", 29, [0, 4294967295, 0, 0]),
    ("MDI37", 30, [15, 536870911, 0, 0]),
];
pub fn hid_payload(format: &str, fc: u32, cn: u64, il: u8, oem: u16) -> Result<Vec<u8>> {
    let (_, id, limits) = HID_FORMATS
        .iter()
        .find(|(name, _, _)| name.eq_ignore_ascii_case(format))
        .ok_or("Unknown HID format")?;
    for ((name, value), limit) in [
        ("Facility code", u64::from(fc)),
        ("Card number", cn),
        ("Issue level", u64::from(il)),
        ("OEM", u64::from(oem)),
    ]
    .into_iter()
    .zip(limits)
    {
        if value > *limit {
            return Err(format!("{name} must be within 0..={limit} for {format}").into());
        }
    }
    let oem = if matches!(*id, 22 | 42) && oem == 0 {
        900
    } else {
        oem
    };
    let mut bytes = vec![*id];
    bytes.extend(fc.to_be_bytes());
    bytes.extend(&cn.to_be_bytes()[3..]);
    bytes.push(il);
    bytes.extend(oem.to_be_bytes());
    Ok(bytes)
}
