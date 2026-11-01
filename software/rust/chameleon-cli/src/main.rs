mod attack;
mod console;
mod desfire;
mod firmware;
mod hf;
mod keys;
mod lf;
mod maintenance;
mod progress;
mod raw;
mod recover;
mod report;
mod restore;
mod selection;
mod slot;
mod slot_file;
mod sniff;
mod st25tb;
mod type2;
mod ui;
mod uid;
use chameleon_client::{Client, Reader, Result};
use clap::{CommandFactory, Parser, Subcommand};
use nfc_native::{RecoveryReader, autopwn::RecoveryTarget, dictionary};
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

#[derive(Parser)]
#[command(name = "chameleon", about = "Chameleon Ultra console", version, color = clap::ColorChoice::Never)]
struct Cli {
    /// Serial port; discover a single connected Ultra when omitted.
    #[arg(long, global = true, value_hint = clap::ValueHint::FilePath)]
    port: Option<String>,
    /// Disable terminal colors.
    #[arg(long, global = true)]
    no_color: bool,
    /// Execute one console command and exit.
    #[arg(short = 'c')]
    execute: Option<String>,
    #[command(subcommand)]
    command: Option<Command>,
}
#[derive(Subcommand)]
enum Command {
    /// Show the connected device version.
    Info,
    /// Show device, emulation, settings and button assignments.
    Status {
        /// Include hardware identity, firmware features and selected image details.
        #[arg(short, long)]
        verbose: bool,
    },
    /// Stored images and autonomous emulation.
    Slot(slot::Slots),
    /// Low-frequency discovery, reading and T55xx writing.
    Lf(lf::Options),
    /// High-frequency discovery, reading and tag operations.
    Hf {
        #[command(subcommand)]
        command: Hf,
    },
    /// Reboot the selected Ultra into its USB DFU bootloader.
    Dfu,
    /// Flash an existing Nordic DFU ZIP directly over USB.
    Flash(firmware::Options),
}
#[derive(Subcommand)]
enum Hf {
    /// Identify the card on the antenna.
    Search,
    /// Control an explicitly retained reader field.
    Field {
        #[command(subcommand)]
        command: raw::Field,
    },
    /// NFC-A raw exchanges and passive capture.
    #[command(name = "14a")]
    TypeA {
        #[command(subcommand)]
        command: TypeA,
    },
    /// ISO14443-B tag identification.
    #[command(name = "14b")]
    TypeB {
        #[command(subcommand)]
        command: TypeB,
    },
    /// ISO15693 discovery and memory operations.
    #[command(name = "15")]
    Vicinity(hf::Options),
    /// MIFARE Classic inspection, recovery and maintenance.
    Mf {
        #[command(subcommand)]
        command: Mifare,
    },
    /// MIFARE Ultralight and NTAG memory operations.
    Mfu {
        #[command(subcommand)]
        command: type2::Command,
    },
    /// Public DESFire discovery, captures and slot saving.
    Desfire {
        #[command(subcommand)]
        command: desfire::Command,
    },
    /// NXP ICODE SLIX inspection and user-memory operations.
    Slix(hf::Options),
    /// NXP ICODE SLIX-L, including known-password privacy reads.
    #[command(name = "slix-l")]
    SlixL(hf::Options),
    /// ST25TB/SRx memory and counter operations.
    St25tb {
        #[command(subcommand)]
        command: st25tb::Command,
    },
}
#[derive(Subcommand)]
enum TypeA {
    /// Exchange raw frames without automatic tag discovery.
    Raw(raw::Options),
    /// Capture NFC-A reader and tag frames with live output.
    Sniff(sniff::Options),
}
#[derive(Subcommand)]
enum TypeB {
    /// Show the ISO14443-B identifier and protocol information.
    Info,
}
#[derive(Subcommand)]
enum Mifare {
    /// Identify the card and its nonce generator.
    Info(uid::InfoOptions),
    /// Verify a key against one sector.
    Check {
        /// Target sector index, starting at zero.
        #[arg(long)]
        sector: usize,
        /// Six-byte key in hexadecimal.
        #[arg(long)]
        key: String,
        /// Use Key B for authentication.
        #[arg(long)]
        key_b: bool,
    },
    /// Recover keys and optionally export a complete dump.
    Autopwn(recover::Options),
    /// Recover one key from a weak nonce generator.
    Nested(attack::Options),
    /// Recover one key from static nonces.
    #[command(name = "staticnested")]
    StaticNested(attack::Options),
    /// Recover one key using encrypted nonces.
    Hardnested {
        #[command(flatten)]
        options: attack::Options,
        /// Maximum recovery work budget.
        #[arg(long, default_value_t = 4096)]
        budget: u16,
    },
    /// Recover an initial key from a vulnerable card.
    Darkside(attack::Target),
    /// Inspect or change a magic card UID.
    Uid {
        #[command(subcommand)]
        command: uid::Command,
    },
    /// Change one Classic sector's keys and optional access settings.
    Keys {
        #[command(subcommand)]
        command: keys::Command,
    },
    /// Restore user data to a physical Classic tag.
    Restore(restore::ClassicOptions),
    /// Repair block 0 and optionally restore selected sector access.
    Repair(maintenance::Options),
    /// Factory format data, keys and access rights; preserve block 0.
    Format(maintenance::Options),
}

fn main() {
    let cli = Cli::parse();
    if cli.execute.is_some() && cli.command.is_some() {
        Cli::command()
            .error(
                clap::error::ErrorKind::ArgumentConflict,
                "Use either -c 'COMMAND' or a subcommand",
            )
            .exit();
    }
    if cli.no_color {
        ui::disable_color();
    }
    let cancelled = Arc::new(AtomicBool::new(false));
    let flag = cancelled.clone();
    if let Err(error) = ctrlc::set_handler(move || flag.store(true, Ordering::Relaxed)) {
        ui::error(&error);
        std::process::exit(1);
    }
    let result = if let Some(text) = cli.execute {
        parse_and_run(&text, cli.port.as_deref(), cancelled, false, &mut None)
    } else if let Some(command) = cli.command {
        execute(command, cli.port.as_deref(), cancelled, true, &mut None)
    } else {
        console::run(cli.port.as_deref(), cancelled)
    };
    if let Err(error) = result {
        ui::error(error.as_ref());
        std::process::exit(1);
    }
}

fn parse_and_run(
    text: &str,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
    last_lf: &mut Option<chameleon_client::LfTag>,
) -> Result<()> {
    let words = shlex::split(text).ok_or("Unclosed command quote")?;
    match Cli::try_parse_from(std::iter::once("chameleon".to_owned()).chain(words)) {
        Ok(cli) => {
            if cli.no_color {
                ui::disable_color();
            }
            execute(
                cli.command.ok_or("Expected a command")?,
                cli.port.as_deref().or(port),
                cancelled,
                prompt_allowed,
                last_lf,
            )
        }
        Err(error)
            if matches!(
                error.kind(),
                clap::error::ErrorKind::DisplayHelp | clap::error::ErrorKind::DisplayVersion
            ) =>
        {
            print!("{error}");
            Ok(())
        }
        Err(error) => Err(error.into()),
    }
}
fn execute(
    command: Command,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
    last_lf: &mut Option<chameleon_client::LfTag>,
) -> Result<()> {
    let result = run(command, port, cancelled, prompt_allowed, last_lf);
    match result {
        Err(error) if error.is::<selection::Back>() => Ok(()),
        Err(error) if error.is::<selection::Cancelled>() => {
            ui::no_write("Cancelled");
            Ok(())
        }
        result => result,
    }
}

fn run(
    command: Command,
    port: Option<&str>,
    cancelled: Arc<AtomicBool>,
    prompt_allowed: bool,
    last_lf: &mut Option<chameleon_client::LfTag>,
) -> Result<()> {
    match command {
        Command::Dfu => return firmware::enter(port, cancelled),
        Command::Flash(options) => {
            return firmware::flash(options, port, cancelled, prompt_allowed);
        }
        Command::Lf(options) => return lf::run(options, port, cancelled, prompt_allowed, last_lf),
        Command::Status { verbose } => return slot::status(port, cancelled, verbose),
        Command::Slot(options) => return slot::run(options, port, cancelled, prompt_allowed),
        Command::Info => ui::line("Device", &[Client::connect(port, cancelled)?.info()?]),
        Command::Hf { command } => {
            let command = match command {
                Hf::Field {
                    command: raw::Field::Off,
                } => return raw::field_off(port, cancelled),
                Hf::Mfu { command } => return type2::run(command, port, cancelled, prompt_allowed),
                Hf::Desfire { command } => {
                    return desfire::run(command, port, cancelled, prompt_allowed);
                }
                Hf::TypeA {
                    command: TypeA::Raw(options),
                } => return raw::run(options, false, port, cancelled),
                Hf::TypeA {
                    command: TypeA::Sniff(options),
                } => return sniff::run(options, port, cancelled, false),
                Hf::Search => return hf::search(port, cancelled),
                Hf::TypeB { .. } => {
                    return hf::run(
                        "b",
                        hf::MemoryCommand::Info(hf::Authentication::default()),
                        port,
                        cancelled,
                    );
                }
                Hf::St25tb { command } => return st25tb::run(command, port, cancelled),
                Hf::Vicinity(options) => {
                    return hf::run_options("v", options, port, cancelled, prompt_allowed);
                }
                Hf::Slix(options) => {
                    return hf::run_options("slix", options, port, cancelled, prompt_allowed);
                }
                Hf::SlixL(options) => {
                    return hf::run_options("slix-l", options, port, cancelled, prompt_allowed);
                }
                command => command,
            };
            let command = match command {
                Hf::Mf {
                    command: Mifare::Repair(options),
                } => return maintenance::run(false, options, port, cancelled, prompt_allowed),
                Hf::Mf {
                    command: Mifare::Format(options),
                } => return maintenance::run(true, options, port, cancelled, prompt_allowed),
                other => other,
            };
            if let Hf::Mf {
                command: Mifare::Restore(options),
            } = command
            {
                return restore::classic(options, port, cancelled, prompt_allowed);
            }
            if let Hf::Mf {
                command: Mifare::Autopwn(options),
            } = command
            {
                return recover::run(options, port, cancelled);
            }
            let command = match command {
                Hf::Mf {
                    command: Mifare::Nested(options),
                } => return attack::run(attack::Method::Nested, options, port, cancelled),
                Hf::Mf {
                    command: Mifare::StaticNested(options),
                } => return attack::run(attack::Method::StaticNested, options, port, cancelled),
                Hf::Mf {
                    command: Mifare::Hardnested { options, budget },
                } => {
                    return attack::run(
                        attack::Method::Hardnested(budget),
                        options,
                        port,
                        cancelled,
                    );
                }
                Hf::Mf {
                    command: Mifare::Darkside(target),
                } => return attack::darkside(target, port, cancelled),
                Hf::Mfu { .. }
                | Hf::Mf {
                    command: Mifare::Restore(_),
                } => unreachable!(),
                command => command,
            };
            // Validate credentials before opening USB.
            let key = match &command {
                Hf::Mf {
                    command: Mifare::Check { key, .. },
                } => Some(dictionary::key(key)?),
                _ => None,
            };
            let mut client = Client::connect(port, cancelled)?;
            Reader::begin(&mut client)?.run(|reader| {
                match command {
                    Hf::TypeB { .. } | Hf::St25tb { .. } | Hf::Vicinity { .. } => unreachable!(),
                    Hf::Search => ui::card(&reader.card, None),
                    Hf::Mf {
                        command: Mifare::Info(options),
                    } => uid::info(reader, options)?,
                    Hf::Mf {
                        command:
                            Mifare::Uid {
                                command: uid::Command::Set(options),
                            },
                    } => uid::set(reader, options, prompt_allowed)?,
                    Hf::Mf {
                        command:
                            Mifare::Keys {
                                command: keys::Command::Set(options),
                            },
                    } => keys::set(reader, options, prompt_allowed)?,
                    Hf::Mf {
                        command: Mifare::Check { sector, key_b, .. },
                    } => {
                        let target = RecoveryTarget {
                            sector,
                            is_key_b: key_b,
                        };
                        let found =
                            reader.verify(target.sector, target.is_key_b, &[key.unwrap()])?;
                        ui::card(&reader.card, None);
                        ui::result(
                            found.is_some(),
                            format!(
                                "{} · sector {sector:02} Key {}",
                                if found.is_some() {
                                    "Key verified"
                                } else {
                                    "Key rejected"
                                },
                                if key_b { 'B' } else { 'A' }
                            ),
                        );
                    }
                    _ => unreachable!(),
                }
                Ok(())
            })?;
        }
    }
    Ok(())
}
