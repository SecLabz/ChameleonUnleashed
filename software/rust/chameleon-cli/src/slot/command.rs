//! Typed command definitions used by the console and scripts.
use super::*;
#[derive(Args)]
pub struct Slots {
    #[command(subcommand)]
    pub(super) command: Option<Command>,
}
#[derive(Args, Default)]
pub(super) struct Target {
    /// Slot number (1–8); choose interactively when omitted.
    #[arg(value_parser=clap::value_parser!(u8).range(1..=8))]
    pub(super) slot: Option<u8>,
    /// Select the HF image.
    #[arg(long, conflicts_with = "lf")]
    pub(super) hf: bool,
    /// Select the LF image.
    #[arg(long)]
    pub(super) lf: bool,
}
#[derive(Subcommand)]
pub(super) enum Command {
    /// Show all eight slots.
    List,
    /// Show slot information and emulator settings.
    Info {
        /// Slot number (1–8); choose interactively when omitted.
        #[arg(value_parser=clap::value_parser!(u8).range(1..=8))]
        slot: Option<u8>,
    },
    /// Show a snapshot of the stored image, including during idle emulation.
    Dump {
        #[command(flatten)]
        target: Target,
    },
    /// Export one image; JSON preserves Chameleon settings.
    Export {
        #[command(flatten)]
        target: Target,
        /// Output file; existing files are never overwritten.
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Start the selected slot's enabled HF/LF images; continues after exit.
    Start {
        /// Slot number (1–8); choose interactively when omitted.
        #[arg(value_parser=clap::value_parser!(u8).range(1..=8))]
        slot: Option<u8>,
    },
    /// Leave emulator mode; reader mode keeps the reader field idle.
    Stop,
    /// Import an image, persist it and verify readback.
    Load {
        /// Input capture or image file.
        file: PathBuf,
        /// Slot number (1–8); choose interactively when omitted.
        #[arg(value_parser=clap::value_parser!(u8).range(1..=8))]
        slot: Option<u8>,
        /// Name of the stored image (1–32 UTF-8 bytes).
        #[arg(long)]
        name: Option<String>,
        /// Inventory Chip ID omitted by ST25TB .nfc images (decimal).
        #[arg(long)]
        chip_id: Option<u8>,
        /// Fill unknown Type 2 credentials/metadata with emulator defaults; never unread user pages.
        #[arg(long)]
        emulator_defaults: bool,
        /// Confirm replacement of an existing image.
        #[arg(long)]
        yes: bool,
    },
    /// Create a factory image for a supported model.
    Init {
        /// Slot number (1–8); choose interactively when omitted.
        #[arg(value_parser=clap::value_parser!(u8).range(1..=8))]
        slot: Option<u8>,
        /// Stored-image model; see the available type names.
        #[arg(long="type",value_parser=model_parser())]
        model: Option<u16>,
        /// Confirm the prepared operation without prompting.
        #[arg(long)]
        yes: bool,
    },
    /// Rename, enable/disable or configure an emulator image.
    Setup {
        #[command(flatten)]
        target: Target,
        /// Name of the stored image (1–32 UTF-8 bytes).
        #[arg(long)]
        name: Option<String>,
        /// Enable or disable this image.
        #[arg(long)]
        enabled: Option<bool>,
        /// Emulator write behavior.
        #[arg(long)]
        write_mode: Option<WriteMode>,
        /// Classic nonce-generator mode.
        #[arg(long)]
        prng: Option<Prng>,
        /// Enable or disable credential detection.
        #[arg(long)]
        detection: Option<bool>,
        /// Enable or disable Gen1A compatibility.
        #[arg(long)]
        gen1a: Option<bool>,
        /// Enable or disable Gen2 compatibility.
        #[arg(long)]
        gen2: Option<bool>,
        /// Use block 0 as the emulated identity.
        #[arg(long)]
        block_identity: Option<bool>,
        /// Reset emulation state when the reader field disappears.
        #[arg(long)]
        field_reset: Option<bool>,
        /// Enable or disable Type 2 UID magic mode.
        #[arg(long)]
        uid_magic: Option<bool>,
    },
    /// Clear one side of a slot after confirmation.
    Clear {
        #[command(flatten)]
        target: Target,
        /// Confirm the prepared operation without prompting.
        #[arg(long)]
        yes: bool,
    },
}
#[derive(Clone, Copy, ValueEnum)]
pub(super) enum WriteMode {
    Normal,
    Denied,
    Deceive,
    Shadow,
}
#[derive(Clone, Copy, ValueEnum)]
pub(super) enum Prng {
    Static,
    Weak,
    Hard,
}
fn model_parser() -> impl clap::builder::TypedValueParser<Value = u16> {
    use clap::builder::{PossibleValuesParser, TypedValueParser};

    PossibleValuesParser::new(SLOT_TYPES.iter().map(|(_, name)| *name)).map(|value| {
        SLOT_TYPES
            .iter()
            .find(|(_, name)| *name == value)
            .unwrap()
            .0
    })
}
