use crate::Result;

const GEN1A: u16 = 1;
const GEN2: u16 = 1 << 2;
const UNFUSED: u16 = 1 << 3;
const SIGNATURES: [(u16, &str); 13] = [
    (GEN1A, "Gen1A"),
    (1 << 1, "Gen1B-compatible"),
    (GEN2, "Gen2 / CUID"),
    (UNFUSED, "Unfused UID signature"),
    (1 << 4, "Super Gen1"),
    (1 << 5, "Super Gen2"),
    (1 << 7, "Gen3"),
    (1 << 8, "Gen4 GTU / UMC (default password)"),
    (1 << 9, "GDM / USCUID (magic auth)"),
    (1 << 10, "QL88"),
    (1 << 11, "GDM / USCUID (20 wakeup)"),
    (1 << 12, "GDM / USCUID (40 wakeup)"),
    (1 << 13, "GDM (ZUID-compatible)"),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Gen2Evidence {
    NotChecked,
    AuthenticationDenied,
    WriteDenied,
    WriteAcknowledged,
    KnownAts,
    Inconclusive,
}

impl Gen2Evidence {
    pub fn description(self) -> &'static str {
        match self {
            Self::NotChecked => "Not checked (no sector-0 key)",
            Self::AuthenticationDenied => "Authentication failed",
            Self::WriteDenied => "Block-0 WRITE refused",
            Self::WriteAcknowledged => "Block-0 WRITE accepted",
            Self::KnownAts => "Known ATS signature",
            Self::Inconclusive => "Block-0 WRITE inconclusive",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MagicDetection {
    flags: u16,
    pub gen2: Gen2Evidence,
}

impl MagicDetection {
    pub(crate) fn decode(data: &[u8]) -> Result<Self> {
        let [high, low, evidence] = data else {
            return Err("Invalid magic probe reply length".into());
        };
        let flags = u16::from_be_bytes([*high, *low]);
        let gen2 = match evidence {
            0 => Gen2Evidence::NotChecked,
            1 => Gen2Evidence::AuthenticationDenied,
            2 => Gen2Evidence::WriteDenied,
            3 => Gen2Evidence::WriteAcknowledged,
            4 => Gen2Evidence::KnownAts,
            5 => Gen2Evidence::Inconclusive,
            _ => return Err("Invalid Gen2 evidence".into()),
        };
        if flags & !0x3fbf != 0
            || (flags & GEN2 != 0)
                != matches!(
                    gen2,
                    Gen2Evidence::WriteAcknowledged | Gen2Evidence::KnownAts
                )
            || flags & 3 == 3
            || flags & 0x30 == 0x30
            || flags & 0x3000 == 0x3000
        {
            return Err("Invalid magic signatures or inconsistent Gen2 evidence".into());
        }
        Ok(Self { flags, gen2 })
    }

    pub fn maintenance_method(self) -> Option<u8> {
        if self.flags & !7 != 0 {
            return None;
        }
        if self.flags & 1 != 0 {
            Some(1)
        } else if self.flags & 2 != 0 {
            Some(2)
        } else if self.flags & 4 != 0 {
            Some(3)
        } else {
            None
        }
    }

    pub fn gen1a(self) -> bool {
        self.flags & GEN1A != 0
    }

    pub fn generation_detected(self) -> bool {
        self.flags & !UNFUSED != 0
    }

    pub fn gen2_authenticated(self) -> bool {
        matches!(
            self.gen2,
            Gen2Evidence::WriteDenied
                | Gen2Evidence::WriteAcknowledged
                | Gen2Evidence::Inconclusive
        )
    }

    pub fn summary(self) -> String {
        let names: Vec<_> = SIGNATURES
            .iter()
            .filter(|(flag, _)| self.flags & flag != 0)
            .map(|(_, name)| *name)
            .collect();
        if names.is_empty() {
            "Unconfirmed (no known signature)".into()
        } else if !self.generation_detected() {
            format!("Unconfirmed ({})", names.join(", "))
        } else {
            names.join(", ")
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MagicUidMethod {
    Gen1b = 1,
    Gen3 = 3,
    Gen4 = 4,
    Gdm20 = 6,
    Gdm40 = 7,
    Gdm40Zuid = 8,
    SuperGen1 = 9,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signatures_require_consistent_evidence_and_unfused_is_not_a_generation() {
        for data in [
            [0, 0, 0],
            [0, 1, 0],
            [0, 4, 3],
            [0, 4, 4],
            [0, 0, 5],
            [0, 8, 0],
        ] {
            let detection = MagicDetection::decode(&data).unwrap();
            assert_eq!(detection.generation_detected(), data[1] & !8 != 0);
        }
        assert!(
            MagicDetection::decode(&[0, 8, 0])
                .unwrap()
                .summary()
                .contains("Unconfirmed")
        );
        for data in [
            [0, 0x40, 0],
            [0, 3, 0],
            [0, 0x30, 0],
            [0x30, 0, 0],
            [0, 4, 0],
            [0, 0, 3],
            [0, 0, 6],
        ] {
            assert!(MagicDetection::decode(&data).is_err());
        }
        assert!(MagicDetection::decode(&[1]).is_err());
        assert!(MagicDetection::decode(&[0; 4]).is_err());
    }
}
