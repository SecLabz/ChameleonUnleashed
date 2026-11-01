//! Line editing and persistent history, without a device connection at the prompt.
use crate::{Cli, parse_and_run, ui};
use chameleon_client::Result;
use clap::CommandFactory;
use nu_ansi_term::Color;
use reedline::{
    ColumnarMenu, Completer, DefaultHinter, DefaultPrompt, DefaultPromptSegment, Emacs,
    FileBackedHistory, KeyCode, KeyModifiers, MenuBuilder, Reedline, ReedlineEvent, ReedlineMenu,
    Signal, Span, Suggestion, default_emacs_keybindings,
};
use std::{
    io::IsTerminal,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

pub fn run(port: Option<&str>, cancelled: Arc<AtomicBool>) -> Result<()> {
    if !std::io::stdin().is_terminal()
        || !std::io::stdout().is_terminal()
        || !std::io::stderr().is_terminal()
    {
        return Err(
            "Console requires a terminal; use -c 'COMMAND' or a subcommand for scripts".into(),
        );
    }
    if std::env::var("TERM").is_ok_and(|term| term == "dumb") {
        return Err(
            "Console requires a cursor-capable terminal; use -c 'COMMAND' or a subcommand".into(),
        );
    }
    let history = history().unwrap_or_else(|error| {
        ui::warning(format!(
            "History unavailable: {error}; using in-memory history"
        ));
        FileBackedHistory::default()
    });
    let mut keys = default_emacs_keybindings();
    keys.add_binding(
        KeyModifiers::NONE,
        KeyCode::Tab,
        ReedlineEvent::UntilFound(vec![
            ReedlineEvent::Menu("completion".into()),
            ReedlineEvent::MenuNext,
        ]),
    );
    let mut syntax = Cli::command();
    syntax.build();
    let mut editor = Reedline::create()
        .with_history(Box::new(history))
        .with_history_exclusion_prefix(Some(" ".into()))
        .with_hinter(Box::new(
            DefaultHinter::default().with_style(Color::DarkGray.normal()),
        ))
        .with_completer(Box::new(Completion(syntax)))
        .with_quick_completions(true)
        .with_partial_completions(true)
        .with_menu(ReedlineMenu::EngineCompleter(Box::new(
            ColumnarMenu::default()
                .with_name("completion")
                .with_columns(1),
        )))
        .with_edit_mode(Box::new(Emacs::new(keys)))
        .with_ansi_colors(ui::color_enabled());
    let prompt = DefaultPrompt::new(
        DefaultPromptSegment::Basic("chameleon".into()),
        DefaultPromptSegment::Empty,
    );
    ui::report(
        "Chameleon Ultra",
        &[
            (
                "Console",
                "help | Tab completes | Up/Down history | Ctrl-R search".into(),
            ),
            ("Control", "Ctrl-C cancels | Ctrl-D or quit exits".into()),
        ],
    );
    println!();
    let mut last_lf = None;
    loop {
        match editor.read_line(&prompt)? {
            Signal::Success(line) => {
                if matches!(line.trim(), "quit" | "exit") {
                    break;
                }
                if line.trim().is_empty() {
                    continue;
                }
                cancelled.store(false, Ordering::Relaxed);
                println!();
                if let Err(error) =
                    parse_and_run(&line, port, cancelled.clone(), true, &mut last_lf)
                {
                    ui::error(error.as_ref());
                }
                editor = editor.with_ansi_colors(ui::color_enabled());
                if let Err(error) = editor.sync_history() {
                    ui::warning(format!("History could not be saved: {error}"));
                }
                println!();
            }
            Signal::CtrlC => (),
            Signal::CtrlD => break,
            _ => (),
        }
    }
    editor.sync_history()?;
    Ok(())
}

fn history() -> Result<FileBackedHistory> {
    let directory = std::env::var_os("CHAMELEON_DATA_DIR")
        .map(PathBuf::from)
        .or_else(|| dirs::data_local_dir().map(|path| path.join("chameleon")))
        .ok_or("Cannot determine history directory")?;
    std::fs::create_dir_all(&directory)?;
    Ok(FileBackedHistory::with_file(
        1000,
        directory.join("history.txt"),
    )?)
}

// Clap owns the command vocabulary so completion stays in sync with help.
struct Completion(clap::Command);
impl Completer for Completion {
    fn complete(&mut self, line: &str, pos: usize) -> Vec<Suggestion> {
        let Some((words, start, end, prefix)) = current_word(line, pos) else {
            return vec![];
        };
        let help = words.first().is_some_and(|word| word == "help");
        let mut command = &self.0;
        let mut pending = None;
        let mut used = std::collections::HashSet::new();
        let mut position = 1;
        let mut count = 0;
        let mut positional_only = false;
        for word in words.iter().skip(usize::from(help)) {
            if pending.take().is_some() {
                continue;
            }
            if !positional_only && word == "--" {
                positional_only = true;
                continue;
            }
            if !positional_only {
                if let Some(arg) = option(command, word) {
                    used.insert(arg.get_id().clone());
                    let attached = if word.starts_with("--") {
                        word.contains('=')
                    } else {
                        word.len() > 2
                    };
                    if arg.get_num_args().is_some_and(|range| range.takes_values()) && !attached {
                        pending = Some(arg);
                    }
                    continue;
                }
                if let Some(child) = command.find_subcommand(word) {
                    command = child;
                    position = 1;
                    count = 0;
                    used.retain(|id| {
                        self.0
                            .get_arguments()
                            .any(|arg| arg.is_global_set() && arg.get_id() == id)
                    });
                    continue;
                }
            }
            if let Some(arg) = command
                .get_positionals()
                .find(|arg| arg.get_index() == Some(position))
            {
                count += 1;
                if count >= arg.get_num_args().map_or(1, |range| range.max_values()) {
                    position += 1;
                    count = 0;
                }
            }
        }

        let mut attached = "";
        let mut value_prefix = prefix.as_str();
        if pending.is_none() && !positional_only && !help {
            let value_start = if prefix.starts_with("--") {
                prefix.find('=').map(|index| index + 1)
            } else if prefix.starts_with('-') && prefix.len() > 2 {
                Some(2)
            } else {
                None
            };
            if let Some(index) = value_start
                && let Some(arg) = option(command, &prefix)
                    .filter(|arg| arg.get_num_args().is_some_and(|range| range.takes_values()))
            {
                pending = Some(arg);
                attached = &prefix[..index];
                value_prefix = &prefix[index..];
            }
        }
        let mut candidates = if let Some(arg) = pending {
            values(arg, value_prefix)
        } else if prefix.starts_with('-') && !positional_only && !help {
            command
                .get_arguments()
                .filter(|arg| {
                    !arg.is_hide_set()
                        && (!used.contains(arg.get_id())
                            || matches!(arg.get_action(), clap::ArgAction::Append))
                })
                .filter_map(|arg| {
                    let value = format!("--{}", arg.get_long()?);
                    value.starts_with(&prefix).then(|| Suggestion {
                        value,
                        description: arg.get_help().map(ToString::to_string),
                        append_whitespace: true,
                        ..Suggestion::default()
                    })
                })
                .collect()
        } else {
            let mut candidates: Vec<_> = command
                .get_subcommands()
                .filter(|child| {
                    !positional_only
                        && !child.is_hide_set()
                        && child.get_name().starts_with(&prefix)
                })
                .map(|child| Suggestion {
                    value: child.get_name().into(),
                    description: child.get_about().map(ToString::to_string),
                    append_whitespace: true,
                    ..Suggestion::default()
                })
                .collect();
            if !help
                && let Some(arg) = command
                    .get_positionals()
                    .find(|arg| arg.get_index() == Some(position))
            {
                candidates.extend(values(arg, &prefix));
            }
            if words.is_empty() {
                for name in ["quit", "exit"] {
                    if name.starts_with(&prefix) {
                        candidates.push(Suggestion {
                            value: name.into(),
                            description: Some("Exit the console".into()),
                            append_whitespace: true,
                            ..Suggestion::default()
                        });
                    }
                }
            }
            candidates
        };
        candidates.retain_mut(|candidate| {
            candidate
                .display_override
                .get_or_insert_with(|| format!("{attached}{}", candidate.value));
            let Ok(quoted) = shlex::try_quote(&candidate.value) else {
                return false;
            };
            candidate.value = format!("{attached}{quoted}");
            candidate.span = Span::new(start, end);
            true
        });
        candidates
    }
}

fn option<'a>(command: &'a clap::Command, word: &str) -> Option<&'a clap::Arg> {
    if let Some(long) = word.strip_prefix("--") {
        let name = long.split('=').next()?;
        command
            .get_arguments()
            .find(|arg| arg.get_long() == Some(name))
    } else {
        let short = word.strip_prefix('-')?.chars().next()?;
        command
            .get_arguments()
            .find(|arg| arg.get_short() == Some(short))
    }
}

// Locate the shell word at the cursor, including unfinished quotes and its
// remaining suffix. Accepted suggestions must replace that whole word.
fn current_word(line: &str, pos: usize) -> Option<(Vec<String>, usize, usize, String)> {
    line.get(..pos)?;
    let mut start = 0;
    let mut end = line.len();
    let mut quote = None;
    let mut escaped = false;
    let mut cursor_quote = None;
    let mut cursor_escaped = false;
    for (index, ch) in line.char_indices() {
        if index == pos {
            cursor_quote = quote;
            cursor_escaped = escaped;
        }
        if !escaped && quote.is_none() && ch.is_whitespace() {
            if index >= pos {
                end = index;
                break;
            }
            start = index + ch.len_utf8();
        } else if escaped {
            escaped = false;
        } else if ch == '\\' && quote != Some('\'') {
            escaped = true;
        } else if quote == Some(ch) {
            quote = None;
        } else if quote.is_none() && matches!(ch, '\'' | '"') {
            quote = Some(ch);
        }
    }
    if pos == line.len() {
        cursor_quote = quote;
        cursor_escaped = escaped;
    }
    if cursor_escaped {
        return None;
    }
    let words = shlex::split(&line[..start])?;
    let mut word = line[start..pos].to_owned();
    if let Some(ch) = cursor_quote {
        word.push(ch);
    }
    let prefix = shlex::split(&word)?.pop().unwrap_or_default();
    Some((words, start, end, prefix))
}

fn values(arg: &clap::Arg, prefix: &str) -> Vec<Suggestion> {
    let (head, prefix) = arg
        .get_value_delimiter()
        .and_then(|separator| {
            prefix
                .rfind(separator)
                .map(|index| prefix.split_at(index + separator.len_utf8()))
        })
        .unwrap_or(("", prefix));
    let mut candidates = if let Some(values) = arg.get_value_parser().possible_values() {
        values
            .filter(|value| !value.is_hide_set())
            .filter(|value| {
                if arg.is_ignore_case_set() {
                    value
                        .get_name()
                        .to_ascii_lowercase()
                        .starts_with(&prefix.to_ascii_lowercase())
                } else {
                    value.get_name().starts_with(prefix)
                }
            })
            .map(|value| Suggestion {
                value: value.get_name().into(),
                description: value.get_help().map(ToString::to_string),
                append_whitespace: true,
                ..Suggestion::default()
            })
            .collect()
    } else if matches!(
        arg.get_value_hint(),
        clap::ValueHint::AnyPath | clap::ValueHint::FilePath | clap::ValueHint::DirPath
    ) {
        paths(prefix, arg.get_value_hint() == clap::ValueHint::DirPath)
    } else {
        vec![]
    };
    for candidate in &mut candidates {
        candidate.value.insert_str(0, head);
    }
    candidates
}

fn paths(prefix: &str, directories_only: bool) -> Vec<Suggestion> {
    let expanded = prefix
        .strip_prefix("~/")
        .and_then(|tail| dirs::home_dir().map(|home| home.join(tail)))
        .unwrap_or_else(|| PathBuf::from(prefix));
    let expanded = expanded.to_string_lossy();
    let (parent, basename) = expanded
        .rfind(std::path::is_separator)
        .map_or(("", expanded.as_ref()), |index| {
            expanded.split_at(index + 1)
        });
    let Ok(entries) = std::fs::read_dir(if parent.is_empty() { "." } else { parent }) else {
        return vec![];
    };
    let mut candidates: Vec<_> = entries
        .filter_map(|entry| {
            let entry = entry.ok()?;
            let name = entry.file_name().into_string().ok()?;
            if name.chars().any(char::is_control)
                || !name.starts_with(basename)
                || (name.starts_with('.')
                    && !prefix
                        .rsplit(std::path::is_separator)
                        .next()
                        .unwrap_or_default()
                        .starts_with('.'))
            {
                return None;
            }
            let directory = entry.path().is_dir();
            if directories_only && !directory {
                return None;
            }
            Some(Suggestion {
                value: format!(
                    "{parent}{name}{}",
                    if directory {
                        std::path::MAIN_SEPARATOR_STR
                    } else {
                        ""
                    }
                ),
                append_whitespace: !directory,
                ..Suggestion::default()
            })
        })
        .collect();
    candidates.sort_by(|a, b| {
        a.append_whitespace
            .cmp(&b.append_whitespace)
            .then(a.value.cmp(&b.value))
    });
    candidates
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser;

    fn completion() -> Completion {
        let mut syntax = Cli::command();
        syntax.build();
        Completion(syntax)
    }

    #[test]
    fn visible_commands_and_arguments_have_help() {
        fn check(command: &clap::Command, missing: &mut Vec<String>) {
            if matches!(command.get_name(), "sniff" | "tearoff") {
                return;
            }
            if command.get_about().is_none() {
                missing.push(command.get_name().into());
            }
            for arg in command.get_arguments().filter(|arg| !arg.is_hide_set()) {
                if arg.get_help().is_none() {
                    missing.push(format!("{} {}", command.get_name(), arg.get_id()));
                }
            }
            for child in command
                .get_subcommands()
                .filter(|child| !child.is_hide_set())
            {
                check(child, missing);
            }
        }
        let mut missing = vec![];
        check(&completion().0, &mut missing);
        assert!(missing.is_empty(), "{missing:?}");
    }

    #[test]
    fn values_follow_the_declared_family_model_and_format_parsers() {
        let mut completion = completion();
        for (line, expected) in [
            ("lf read em", "em410x"),
            ("lf read em", "em4x05"),
            ("lf write id", "idteck"),
            ("lf write hid --format h103", "H10301"),
            ("slot init 1 --type classic-", "classic-1k"),
            ("hf mfu dump --model=ntag2", "--model=ntag215"),
            ("slot setup 1 --enabled f", "false"),
        ] {
            let values = completion.complete(line, line.len());
            assert!(
                values.iter().any(|item| item.value == expected),
                "{line}: {values:?}"
            );
        }
        assert!(completion.complete("lf read id", 10).is_empty());
        assert!(
            !completion
                .complete("lf write em", 11)
                .iter()
                .any(|item| item.value == "em4x05")
        );
        let line = "hf mfu dump --model=ntag215 --no-color";
        let value = completion
            .complete(line, "hf mfu dump --model=ntag2".len())
            .into_iter()
            .find(|item| item.value == "--model=ntag215")
            .unwrap();
        let accepted = format!(
            "{}{}{}",
            &line[..value.span.start],
            value.value,
            &line[value.span.end..]
        );
        assert_eq!(accepted, line);
        let line = "--no-color hf mf autopwn --";
        assert!(
            !completion
                .complete(line, line.len())
                .iter()
                .any(|item| item.value == "--no-color")
        );
    }

    #[test]
    fn file_completion_roundtrips_spaces_quotes_unicode_and_argument_boundaries() {
        let directory = std::env::temp_dir().join(format!(
            "chameleon-completion-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let nested = directory.join("cards entrée");
        std::fs::create_dir_all(&nested).unwrap();
        let path = nested.join("badge 'A'.nfc");
        std::fs::write(&path, "").unwrap();
        std::fs::write(nested.join(".hidden.nfc"), "").unwrap();
        let mut completion = completion();
        for line in [
            format!("slot load '{}/ba", nested.display()),
            format!("slot load \"{}/ba", nested.display()),
            format!(
                "slot load {}",
                shlex::try_quote(&format!("{}/ba", nested.display())).unwrap()
            ),
            format!("hf mf autopwn --output=\"{}/ba", nested.display()),
            format!("hf mf autopwn -o \"{}/ba", nested.display()),
        ] {
            let values = completion.complete(&line, line.len());
            assert_eq!(values.len(), 1, "{line}: {values:?}");
            let value = &values[0];
            assert!(value.append_whitespace);
            let accepted = format!("{}{}", &line[..value.span.start], value.value);
            let words = shlex::split(&accepted).unwrap();
            let filename = words
                .last()
                .unwrap()
                .strip_prefix("--output=")
                .unwrap_or(words.last().unwrap());
            assert_eq!(filename, path.to_str().unwrap());
            assert!(
                Cli::try_parse_from(std::iter::once("chameleon".to_owned()).chain(words)).is_ok(),
                "{accepted}"
            );
        }
        let line = format!("slot load '{}/", directory.display());
        let values = completion.complete(&line, line.len());
        assert_eq!(values.len(), 1);
        assert!(!values[0].append_whitespace);
        assert!(shlex::split(&values[0].value).unwrap()[0].ends_with(std::path::MAIN_SEPARATOR));
        let line = format!("slot load '{}/", nested.display());
        assert_eq!(completion.complete(&line, line.len()).len(), 1);
        let line = format!(
            "slot load {} 3 --na",
            shlex::try_quote(path.to_str().unwrap()).unwrap()
        );
        assert!(
            completion
                .complete(&line, line.len())
                .iter()
                .any(|item| item.value == "--name")
        );
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn completion_follows_commands_and_skips_option_values_and_used_flags() {
        let mut syntax = Cli::command();
        syntax.build();
        let mut completion = Completion(syntax);
        for (line, expected) in [
            ("hf mf au", "autopwn"),
            ("hf mf autopwn --ou", "--output"),
            ("--port /dev/example hf mf au", "autopwn"),
            ("help hf mf au", "autopwn"),
        ] {
            assert!(
                completion
                    .complete(line, line.len())
                    .iter()
                    .any(|item| item.value == expected),
                "{line}"
            );
        }
        for line in [
            "hf mf autopwn --key ",
            "hf mf autopwn --dict '/my incomplete path",
        ] {
            assert!(completion.complete(line, line.len()).is_empty());
        }
        let line = "hf mf autopwn --verbose --";
        assert!(
            !completion
                .complete(line, line.len())
                .iter()
                .any(|item| item.value == "--verbose")
        );
        assert!(completion.complete("é", 1).is_empty());
    }
}
