use crate::Result;
use nfc_native::recovery::Observation;
use serde::Serialize;
use std::collections::HashSet;

#[derive(Debug, Serialize)]
pub struct FirmwareCaptureMetrics {
    pub setup_us: u32,
    pub select_us: u32,
    pub auth_us: u32,
    pub target_us: u32,
    pub housekeeping_us: u32,
    /// Included in selection/authentication/target times; do not add twice.
    pub wait_us: u32,
    pub timeouts: u32,
    pub wakeups: u32,
    pub observations: u32,
}

#[derive(Serialize)]
pub struct CaptureBatch {
    pub requested: usize,
    pub observations: usize,
    pub elapsed_us: u64,
    pub firmware: Option<FirmwareCaptureMetrics>,
    pub error: Option<String>,
}

#[derive(Default, Serialize)]
pub struct CaptureMetrics {
    pub observations: usize,
    pub unique_observations: usize,
    pub completed_observations: usize,
    pub acquisition_us: u64,
    pub observations_per_second: f64,
    pub batches: Vec<CaptureBatch>,
    pub records: Vec<Observation>,
    #[serde(skip)]
    seen: HashSet<(u32, [u8; 4])>,
}

impl CaptureMetrics {
    pub(crate) fn record(
        &mut self,
        requested: usize,
        elapsed_us: u64,
        firmware: Option<FirmwareCaptureMetrics>,
        result: &Result<Vec<Observation>>,
    ) {
        let (observations, error) = match result {
            Ok(records) => {
                self.records.extend_from_slice(records);
                self.seen.extend(
                    records
                        .iter()
                        .map(|record| (record.ciphertext, record.parity)),
                );
                (records.len(), None)
            }
            Err(error) => (0, Some(error.to_string())),
        };
        self.observations += observations;
        self.completed_observations += firmware
            .as_ref()
            .map_or(observations, |stats| stats.observations as usize);
        self.unique_observations = self.seen.len();
        self.acquisition_us += elapsed_us;
        self.observations_per_second =
            self.observations as f64 * 1_000_000. / self.acquisition_us.max(1) as f64;
        self.batches.push(CaptureBatch {
            requested,
            observations,
            elapsed_us,
            firmware,
            error,
        });
    }
}

pub(crate) fn decode(
    data: &[u8],
    count: usize,
    profiled: bool,
) -> Result<(Option<FirmwareCaptureMetrics>, Vec<Observation>)> {
    let offset = if profiled { 40 } else { 0 };
    if data.len() != offset + count * 12 {
        return Err("Invalid normalized capture length".into());
    }
    let firmware = if profiled {
        let stats = decode_metrics(&data[..40], count)?;
        if stats.observations as usize != count {
            return Err("Invalid capture metrics observation count".into());
        }
        Some(stats)
    } else {
        None
    };
    let records = data[offset..]
        .chunks_exact(12)
        .map(|record| {
            let parity: [u8; 4] = record[8..12].try_into()?;
            if parity.iter().any(|bit| *bit > 1) {
                return Err("Invalid capture parity".into());
            }
            Ok(Observation {
                nt1: u32::from_be_bytes(record[..4].try_into()?),
                ciphertext: u32::from_be_bytes(record[4..8].try_into()?),
                parity,
            })
        })
        .collect::<Result<_>>()?;
    Ok((firmware, records))
}

pub(crate) fn decode_metrics(data: &[u8], requested: usize) -> Result<FirmwareCaptureMetrics> {
    if data.len() != 40 {
        return Err("Invalid capture metrics length".into());
    }
    let words: [u32; 10] = std::array::from_fn(|index| {
        u32::from_be_bytes(data[index * 4..index * 4 + 4].try_into().unwrap())
    });
    if words[0] != 1 || words[9] as usize > requested {
        return Err("Invalid capture metrics version or observation count".into());
    }
    Ok(FirmwareCaptureMetrics {
        setup_us: words[1],
        select_us: words[2],
        auth_us: words[3],
        target_us: words[4],
        housekeeping_us: words[5],
        wait_us: words[6],
        timeouts: words[7],
        wakeups: words[8],
        observations: words[9],
    })
}
