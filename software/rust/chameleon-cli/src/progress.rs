// SPDX-License-Identifier: GPL-3.0-or-later
//! Drop stops the worker before reports or errors.
use crate::ui;
use nfc_native::autopwn::AutoPwnAction;
use std::{
    io::Write,
    sync::{Arc, Condvar, Mutex},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

struct State {
    phase: &'static str,
    dictionary: Option<(usize, bool, usize, usize)>,
    counts: Option<(usize, usize)>,
    stopped: bool,
}

pub struct Progress {
    state: Arc<(Mutex<State>, Condvar)>,
    worker: Option<JoinHandle<()>>,
    verbose: bool,
}

// Activity labels contain only single-cell characters. Narrow terminals retain
// counts and time.
fn render(state: &State, tick: usize, elapsed: Duration, width: usize) -> String {
    let seconds = elapsed.as_secs();
    let time = format!("{:02}:{:02}", seconds / 60, seconds % 60);
    let spinner = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"][tick % 10];
    let mut text = if let Some((done, total)) = state.counts {
        let filled = if total == 0 {
            0
        } else {
            (done * 20 / total).min(20)
        };
        let bar = format!("{}{}", "━".repeat(filled), "─".repeat(20 - filled));
        format!(
            "  {spinner} {:22} [{bar}] {done:2}/{total:2} keys  {time}",
            state.phase
        )
    } else {
        format!("  {spinner} {}…  {time}", state.phase)
    };
    let available = width.saturating_sub(1);
    if text.chars().count() > available {
        let suffix = state.counts.map_or(time.clone(), |(done, total)| {
            format!("{done}/{total} keys  {time}")
        });
        let room = available.saturating_sub(suffix.len() + 2);
        text = if room == 0 {
            time
        } else {
            format!(
                "{}  {suffix}",
                state.phase.chars().take(room).collect::<String>()
            )
        };
    } else if let Some((sector, key_b, done, total)) = state.dictionary {
        text.push_str(&format!(
            " · sector {sector:02} Key {} · candidate {done}/{total}",
            if key_b { 'B' } else { 'A' }
        ));
    }
    text.chars().take(available).collect()
}

impl Progress {
    pub fn new(phase: &'static str, total: Option<usize>, verbose: bool) -> Self {
        let state = Arc::new((
            Mutex::new(State {
                phase,
                dictionary: None,
                counts: total.map(|total| (0, total)),
                stopped: false,
            }),
            Condvar::new(),
        ));
        let worker = (ui::interactive() && !verbose).then(|| {
            let state = state.clone();
            thread::spawn(move || {
                let started = Instant::now();
                let mut tick = 0;
                let (lock, wake) = &*state;
                let mut state = lock.lock().unwrap();
                while !state.stopped {
                    let width =
                        crossterm::terminal::size().map_or(80, |(width, _)| usize::from(width));
                    let text = render(&state, tick, started.elapsed(), width);
                    let mut stderr = std::io::stderr().lock();
                    let _ = write!(
                        stderr,
                        "\r\x1b[2K{}",
                        ui::paint(text, ui::ACCENT, ui::color_enabled())
                    );
                    let _ = stderr.flush();
                    drop(stderr);
                    tick += 1;
                    state = wake.wait_timeout(state, Duration::from_secs(1)).unwrap().0;
                }
                let mut stderr = std::io::stderr().lock();
                let _ = write!(stderr, "\r\x1b[2K");
                let _ = stderr.flush();
            })
        });
        if worker.is_none() && total.is_none() {
            ui::message(phase, ui::ACCENT);
        }
        Self {
            state,
            worker,
            verbose,
        }
    }

    pub fn phase(&self, phase: &'static str) {
        let mut state = self.state.0.lock().unwrap();
        if state.phase == phase && !self.verbose {
            return;
        }
        if self.worker.is_none() {
            ui::message(phase, "0");
        }
        state.phase = phase;
        state.dictionary = None;
    }

    pub fn counts(&self, done: usize, total: usize) {
        self.state.0.lock().unwrap().counts = Some((done, total));
    }

    pub fn action(&self, action: &AutoPwnAction) {
        use AutoPwnAction::*;
        let phase = match action {
            TestDictionary { kind, .. } => kind.name(),
            ProbePrng => "Probing PRNG",
            FilterDictionary { .. } => "Filtering dictionary nonces",
            RecoverNested { .. } => "Recovering · Nested",
            RecoverStaticNested { .. } => "Static Nested",
            RecoverHardnested { .. } => "Recovering · Hardnested",
            RecoverDarkside { .. } => "Recovering · Darkside",
            ExtractTrailer { .. } | SweepKey { .. } | VerifyCandidates { .. } if !self.verbose => {
                return;
            }
            ExtractTrailer { .. } => "Reading trailers",
            SweepKey { .. } => "Checking key reuse",
            VerifyCandidates { .. } => "Verifying candidates",
        };
        self.phase(phase);
    }

    pub fn dictionary(&self, sector: usize, key_b: bool, done: usize, total: usize) {
        let mut state = self.state.0.lock().unwrap();
        state.dictionary = Some((sector, key_b, done, total));
        if self.verbose && (done == 0 || done == total) {
            ui::message(
                format!(
                    "{} · sector {sector:02} Key {} · candidate {done}/{total}",
                    state.phase,
                    if key_b { 'B' } else { 'A' }
                ),
                "0",
            );
        }
    }

    pub fn finish(&mut self) {
        self.state.0.lock().unwrap().stopped = true;
        self.state.1.notify_one();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for Progress {
    fn drop(&mut self) {
        self.finish();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn activity_survives_resize_and_keeps_time_and_key_counts_readable() {
        let state = State {
            phase: "Recovering · Nested",
            dictionary: None,
            counts: Some((12, 32)),
            stopped: false,
        };
        assert_eq!(
            render(&state, 0, Duration::from_secs(70), 160),
            "  ⠋ Recovering · Nested    [━━━━━━━─────────────] 12/32 keys  01:10"
        );
        for width in [0, 1, 4, 20, 40, 80, 160] {
            let text = render(&state, 0, Duration::from_secs(70), width);
            assert!(text.chars().count() < width || width == 0);
            assert!(!text.contains('\x1b'));
            if width >= 20 {
                assert!(text.contains("01:10"));
            }
            if width >= 40 {
                assert!(text.contains("12/32 keys"));
            }
        }
    }
}
