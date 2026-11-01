use chameleon_client::{SniffFrame as Frame, SniffSummary as Summary};
use nfc_native::sniff::{Analysis, NfcaAnalyzer, NfcvAnalyzer};

pub(super) struct Presentation {
    analyzer: Analyzer,
    losses: (u32, u32),
}

enum Analyzer {
    Nfca(NfcaAnalyzer),
    Nfcv(NfcvAnalyzer),
}

impl Presentation {
    pub fn new(nfcv: bool) -> Self {
        Self {
            analyzer: if nfcv {
                Analyzer::Nfcv(NfcvAnalyzer::default())
            } else {
                Analyzer::Nfca(NfcaAnalyzer::default())
            },
            losses: (0, 0),
        }
    }

    pub fn observe_losses(&mut self, summary: &Summary, empty: bool) {
        let losses = (summary.dropped_frames, summary.capture_overruns);
        if losses != self.losses {
            self.losses = losses;
            match &mut self.analyzer {
                Analyzer::Nfca(analyzer) => {
                    analyzer.capture_lost();
                    eprintln!(
                        "Capture loss ({} dropped, {} overruns); protocol context reset",
                        losses.0, losses.1
                    );
                }
                Analyzer::Nfcv(analyzer) => analyzer.capture_lost(),
            }
        }
        if empty {
            match &mut self.analyzer {
                Analyzer::Nfca(analyzer) => analyzer.queue_drained(),
                Analyzer::Nfcv(analyzer) => analyzer.queue_drained(),
            }
        }
    }

    pub fn analyze(&mut self, frame: &Frame) -> Analysis {
        match &mut self.analyzer {
            Analyzer::Nfca(analyzer) => {
                let mut analysis = analyzer.analyze(&frame.analysis_frame());
                if frame.errors != 0 {
                    analysis.annotation = format!("RECEIVER ERROR 0x{:02X}", frame.errors);
                } else if analysis.annotation == "UNCLASSIFIED REPLY" {
                    analysis.annotation = "UNCLASSIFIED MFRC522".into();
                }
                analysis
            }
            Analyzer::Nfcv(analyzer) => analyzer.analyze(&frame.analysis_frame()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chameleon_client::{
        SNIFF_CRC_CHECKED, SNIFF_CRC_VALID, SNIFF_DECODED, SNIFF_PARITY_CHECKED, SNIFF_PARITY_VALID,
    };
    use nfc_native::sniff::{Check, Direction};

    fn frame(direction: u8, size_bits: u16, data: &[u8]) -> Frame {
        Frame {
            end_us: 123456,
            size_bits,
            flags: SNIFF_DECODED
                | SNIFF_PARITY_CHECKED
                | SNIFF_PARITY_VALID
                | SNIFF_CRC_CHECKED
                | SNIFF_CRC_VALID,
            direction,
            errors: 0,
            data: data.to_vec(),
        }
    }

    #[test]
    fn receiver_details_stay_local_and_spurious_replies_preserve_context() {
        let mut presentation = Presentation::new(false);
        let wakeup = frame(0, 7, &[0x52]);
        let borrowed = wakeup.analysis_frame();
        assert_eq!(borrowed.data, &[0x52]);
        assert_eq!(borrowed.size_bits, 7);
        assert_eq!(borrowed.direction, Direction::Reader);
        assert_eq!(presentation.analyze(&wakeup).annotation, "WUPA");
        let mut error = frame(1, 16, &[4, 0]);
        error.errors = 2;
        assert!(error.analysis_frame().receiver_error);
        let analysis = presentation.analyze(&error);
        assert_eq!(analysis.annotation, "RECEIVER ERROR 0x02");
        assert_eq!(analysis.checks.crc, Check::Unavailable);
        assert_eq!(
            presentation.analyze(&frame(1, 4, &[0xa])).annotation,
            "UNCLASSIFIED MFRC522"
        );
        assert_eq!(
            presentation.analyze(&frame(1, 16, &[4, 0])).annotation,
            "ATQA 0004"
        );
    }

    #[test]
    fn batch_counters_signal_a_loss_once_and_empty_batches_release_the_barrier() {
        for type_v in [false, true] {
            let mut presentation = Presentation::new(type_v);
            let request = if type_v {
                frame(0, 40, &[0x26, 1, 0, 0, 0])
            } else {
                frame(0, 7, &[0x52])
            };
            let summary = Summary {
                dropped_frames: 1,
                ..Summary::default()
            };
            presentation.observe_losses(&summary, false);
            assert!(
                presentation
                    .analyze(&request)
                    .annotation
                    .contains("context unavailable")
            );
            presentation.observe_losses(&summary, true);
            presentation.observe_losses(&summary, false);
            assert!(
                !presentation
                    .analyze(&request)
                    .annotation
                    .contains("context unavailable")
            );
            presentation.observe_losses(&summary, true);
            let reply = if type_v {
                frame(1, 96, &[0, 0, 8, 7, 6, 5, 4, 3, 2, 0xe0, 0, 0])
            } else {
                frame(1, 16, &[4, 0])
            };
            let annotation = presentation.analyze(&reply).annotation;
            assert_eq!(
                annotation,
                if type_v {
                    "UID E002030405060708 · DSFID 00"
                } else {
                    "ATQA 0004"
                }
            );
        }
    }
}
