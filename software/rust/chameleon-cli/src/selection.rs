//! Interactive choices and fields; explicit arguments never enter terminal mode.
use crate::ui;
use chameleon_client::Result;
use crossterm::{
    cursor::{Hide, MoveToColumn, MoveUp, Show},
    event::{
        self, DisableBracketedPaste, EnableBracketedPaste, Event, KeyCode, KeyEvent, KeyEventKind,
        KeyModifiers,
    },
    execute, queue,
    terminal::{self, Clear, ClearType},
};
use reedline::{
    EditCommand, Emacs, Prompt, PromptEditMode, PromptHistorySearch, Reedline, ReedlineEvent,
    Signal, default_emacs_keybindings,
};
use std::borrow::Cow;
use std::{
    fmt,
    io::{self, IsTerminal, Write},
};
use unicode_width::UnicodeWidthChar;

#[derive(Debug)]
pub struct Back;

impl fmt::Display for Back {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Selection cancelled")
    }
}

impl std::error::Error for Back {}

#[derive(Debug)]
pub struct Cancelled;

impl fmt::Display for Cancelled {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Interaction cancelled")
    }
}

impl std::error::Error for Cancelled {}

pub struct Choice {
    pub shortcut: String,
    pub label: String,
    pub enabled: bool,
}

impl Choice {
    pub fn numbered(number: usize, label: String) -> Self {
        Self {
            shortcut: number.to_string(),
            label,
            enabled: true,
        }
    }
}

// The cursor remains on the final rendered line. Every exit restores terminal mode,
// including drawing/input errors, and removes only the current interaction.
struct Terminal {
    rows: usize,
    was_raw: bool,
    color: bool,
}

impl Terminal {
    fn open(label: &str, allowed: bool) -> Result<Self> {
        if !allowed
            || !io::stdin().is_terminal()
            || !io::stdout().is_terminal()
            || std::env::var("TERM").is_ok_and(|term| term == "dumb")
        {
            return Err(format!(
                "{label} requires an interactive terminal; supply explicit arguments"
            )
            .into());
        }
        let was_raw = terminal::is_raw_mode_enabled()?;
        terminal::enable_raw_mode()?;
        let terminal = Self {
            rows: 0,
            was_raw,
            color: ui::stdout_color(),
        };
        execute!(io::stdout(), Hide, EnableBracketedPaste)?;
        Ok(terminal)
    }

    fn size(&self) -> (usize, usize) {
        let (width, height) = terminal::size().unwrap_or((80, 24));
        (
            usize::from(width).saturating_sub(1).max(1),
            usize::from(height).saturating_sub(1).max(1),
        )
    }

    fn draw(&mut self, lines: &[(String, &str)]) -> Result<()> {
        let mut out = io::stdout().lock();
        if self.rows > 1 {
            queue!(out, MoveUp((self.rows - 1) as u16))?;
        }
        queue!(out, MoveToColumn(0), Clear(ClearType::FromCursorDown), Hide)?;
        let (width, height) = self.size();
        let lines = &lines[lines.len().saturating_sub(height)..];
        for (index, (text, style)) in lines.iter().enumerate() {
            if index > 0 {
                write!(out, "\r\n")?;
            }
            write!(
                out,
                "{}",
                ui::paint(clip(text, width), style, self.color && !style.is_empty())
            )?;
        }
        self.rows = lines.len();
        out.flush()?;
        Ok(())
    }
}

impl Drop for Terminal {
    fn drop(&mut self) {
        let mut out = io::stdout().lock();
        if self.rows > 1 {
            let _ = queue!(out, MoveUp((self.rows - 1) as u16));
        }
        let _ = execute!(
            out,
            MoveToColumn(0),
            Clear(ClearType::FromCursorDown),
            Show,
            DisableBracketedPaste
        );
        if !self.was_raw {
            let _ = terminal::disable_raw_mode();
        }
    }
}

fn clip(text: &str, width: usize) -> String {
    let mut cells = 0;
    let mut output = String::new();
    for ch in text.chars().filter(|ch| !ch.is_control()) {
        let size = ch.width().unwrap_or(0);
        if cells + size > width {
            break;
        }
        output.push(ch);
        cells += size;
    }
    output
}

fn wrap(text: &str, width: usize) -> Vec<String> {
    let mut lines = vec![String::new()];
    let mut cells = 0;
    for ch in text.chars() {
        let size = ch.width().unwrap_or(0);
        if ch == '\n' || cells + size > width && cells > 0 {
            lines.push(String::new());
            cells = 0;
        }
        if !ch.is_control() {
            lines.last_mut().unwrap().push(ch);
            cells += size;
        }
    }
    lines
}

#[derive(Debug, PartialEq)]
enum Action {
    Continue,
    Accept,
    Back,
    Cancel,
}

fn navigation(key: KeyEvent) -> Option<Action> {
    match key.code {
        KeyCode::Esc => Some(Action::Back),
        KeyCode::Char('c' | 'd') if key.modifiers.contains(KeyModifiers::CONTROL) => {
            Some(Action::Cancel)
        }
        KeyCode::Enter => Some(Action::Accept),
        _ => None,
    }
}

struct Menu {
    selected: usize,
    shortcut: String,
}

impl Menu {
    fn new(choices: &[Choice], selected: usize) -> Result<Self> {
        let selected = if choices.get(selected).is_some_and(|choice| choice.enabled) {
            selected
        } else {
            choices
                .iter()
                .position(|choice| choice.enabled)
                .ok_or("No available choices")?
        };
        Ok(Self {
            selected,
            shortcut: String::new(),
        })
    }

    fn advance(&mut self, choices: &[Choice], forward: bool) {
        self.shortcut.clear();
        for _ in 0..choices.len() {
            self.selected = if forward {
                (self.selected + 1) % choices.len()
            } else {
                (self.selected + choices.len() - 1) % choices.len()
            };
            if choices[self.selected].enabled {
                break;
            }
        }
    }

    fn key(&mut self, choices: &[Choice], key: KeyEvent) -> Action {
        if let Some(action) = navigation(key) {
            if action == Action::Accept
                && !self.shortcut.is_empty()
                && choices[self.selected].shortcut != self.shortcut
            {
                return Action::Continue;
            }
            return action;
        }
        match key.code {
            KeyCode::Up | KeyCode::BackTab => self.advance(choices, false),
            KeyCode::Down | KeyCode::Tab => self.advance(choices, true),
            KeyCode::Home => {
                self.selected = choices.iter().position(|choice| choice.enabled).unwrap();
                self.shortcut.clear();
            }
            KeyCode::End => {
                self.selected = choices.iter().rposition(|choice| choice.enabled).unwrap();
                self.shortcut.clear();
            }
            KeyCode::Backspace => {
                self.shortcut.pop();
            }
            KeyCode::Char(ch) if !key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.shortcut.push(ch.to_ascii_lowercase());
                if !choices
                    .iter()
                    .any(|choice| choice.enabled && choice.shortcut.starts_with(&self.shortcut))
                {
                    self.shortcut = ch.to_ascii_lowercase().to_string();
                }
            }
            _ => {}
        }
        if let Some(index) = choices
            .iter()
            .position(|choice| choice.enabled && choice.shortcut == self.shortcut)
        {
            self.selected = index;
        }
        Action::Continue
    }

    fn lines(
        &self,
        title: &str,
        header: &str,
        choices: &[Choice],
        (width, height): (usize, usize),
    ) -> Vec<(String, &'static str)> {
        let mut lines: Vec<_> = wrap(title, width)
            .into_iter()
            .take(height.saturating_sub(3))
            .map(|line| (line, "1;36"))
            .collect();
        if !header.is_empty() && height.saturating_sub(lines.len()) > 3 {
            lines.push((header.into(), "90"));
        }
        let key_width = choices
            .iter()
            .map(|choice| choice.shortcut.len())
            .max()
            .unwrap_or(1);
        let labels: Vec<_> = choices
            .iter()
            .map(|choice| wrap(&choice.label, width.saturating_sub(key_width + 4).max(1)))
            .collect();
        let room = height.saturating_sub(lines.len() + 2).max(1);
        let row_height = labels.iter().map(Vec::len).max().unwrap_or(1).min(room);
        let count = (room / row_height).max(1);
        let first = self.selected / count * count;
        for (index, choice) in choices.iter().enumerate().skip(first).take(count) {
            let selected = index == self.selected;
            let style = if !choice.enabled {
                "90"
            } else if selected {
                "7"
            } else {
                ""
            };
            let marker = if selected { '›' } else { ' ' };
            for (line, label) in labels[index].iter().take(row_height).enumerate() {
                let prefix = if line == 0 {
                    format!("{marker} {:>key_width$}  ", choice.shortcut)
                } else {
                    " ".repeat(key_width + 4)
                };
                lines.push((format!("{prefix}{label}"), style));
            }
        }
        if height >= 3 {
            let invalid =
                !self.shortcut.is_empty() && choices[self.selected].shortcut != self.shortcut;
            lines.push((
                if invalid {
                    format!("No choice '{}'", self.shortcut)
                } else {
                    String::new()
                },
                "33",
            ));
        }
        if height >= 2 {
            let shortcuts = if choices
                .iter()
                .all(|choice| choice.shortcut.parse::<usize>().is_ok())
            {
                "number"
            } else {
                "y/n"
            };
            let hint = if width >= 56 {
                format!("↑↓ Choose · {shortcuts} Select · Enter Confirm · Esc Back")
            } else {
                "↑↓ · Enter OK · Esc Back".into()
            };
            lines.push((hint, "90"));
        }
        lines
    }
}

pub fn menu(
    title: &str,
    header: &str,
    choices: &[Choice],
    selected: usize,
    allowed: bool,
) -> Result<usize> {
    let mut menu = Menu::new(choices, selected)?;
    let mut terminal = Terminal::open(title, allowed)?;
    loop {
        terminal.draw(&menu.lines(title, header, choices, terminal.size()))?;
        if let Event::Key(key) = event::read()? {
            if key.kind == KeyEventKind::Release {
                continue;
            }
            match menu.key(choices, key) {
                Action::Accept => return Ok(menu.selected),
                Action::Back => return Err(Back.into()),
                Action::Cancel => return Err(Cancelled.into()),
                Action::Continue => {}
            }
        }
    }
}

pub fn choose_current(
    title: &str,
    choices: &[String],
    selected: usize,
    allowed: bool,
) -> Result<usize> {
    let choices: Vec<_> = choices
        .iter()
        .enumerate()
        .map(|(index, label)| Choice::numbered(index + 1, label.clone()))
        .collect();
    menu(title, "", &choices, selected, allowed)
}

pub fn choose(title: &str, choices: &[String], allowed: bool) -> Result<usize> {
    choose_current(title, choices, 0, allowed)
}

struct FieldPrompt<'a>(&'a str);

impl Prompt for FieldPrompt<'_> {
    fn render_prompt_left(&self) -> Cow<'_, str> {
        self.0.into()
    }
    fn render_prompt_right(&self) -> Cow<'_, str> {
        "".into()
    }
    fn render_prompt_indicator(&self, _: PromptEditMode) -> Cow<'_, str> {
        ": ".into()
    }
    fn render_prompt_multiline_indicator(&self) -> Cow<'_, str> {
        "  ".into()
    }
    fn render_prompt_history_search_indicator(&self, _: PromptHistorySearch) -> Cow<'_, str> {
        "Search: ".into()
    }
}

pub fn input<T>(
    title: &str,
    label: &str,
    hint: &str,
    initial: &str,
    allowed: bool,
    parse: impl Fn(&str) -> Result<T>,
) -> Result<T> {
    if !allowed || !io::stdin().is_terminal() || !io::stdout().is_terminal() {
        return Err(format!(
            "{label} is required outside the interactive console; supply explicit arguments"
        )
        .into());
    }
    ui::line(title, &[]);
    if !hint.is_empty() {
        println!("{hint}");
    }
    println!("Enter Confirm · Esc Back · Ctrl-U Clear");
    let mut keys = default_emacs_keybindings();
    keys.add_binding(
        KeyModifiers::NONE,
        KeyCode::Esc,
        ReedlineEvent::ExecuteHostCommand("back".into()),
    );
    keys.add_binding(KeyModifiers::NONE, KeyCode::Enter, ReedlineEvent::Submit);
    keys.add_binding(
        KeyModifiers::CONTROL,
        KeyCode::Char('u'),
        ReedlineEvent::Edit(vec![EditCommand::Clear]),
    );
    let mut editor = Reedline::create()
        .with_ansi_colors(ui::color_enabled())
        .with_edit_mode(Box::new(Emacs::new(keys)));
    editor.run_edit_commands(&[EditCommand::InsertString(initial.into())]);
    loop {
        match editor.read_line(&FieldPrompt(label))? {
            Signal::Success(value) => match parse(value.trim()) {
                Ok(value) => return Ok(value),
                Err(error) if error.is::<Back>() || error.is::<Cancelled>() => return Err(error),
                Err(error) => {
                    ui::warning(error);
                    editor
                        .run_edit_commands(&[EditCommand::Clear, EditCommand::InsertString(value)]);
                }
            },
            Signal::HostCommand(_) => return Err(Back.into()),
            _ => return Err(Cancelled.into()),
        }
    }
}

pub fn pause(title: &str, instruction: &str, allowed: bool) -> Result<()> {
    let mut terminal = Terminal::open(title, allowed)?;
    loop {
        let mut lines: Vec<_> = title.lines().map(|line| (line.into(), "1;36")).collect();
        lines.extend([
            (instruction.into(), ""),
            (String::new(), ""),
            ("Enter Continue · Esc Back".into(), "90"),
        ]);
        terminal.draw(&lines)?;
        if let Event::Key(key) = event::read()? {
            if key.kind == KeyEventKind::Release {
                continue;
            }
            match navigation(key) {
                Some(Action::Accept) => return Ok(()),
                Some(Action::Back) => return Err(Back.into()),
                Some(Action::Cancel) => return Err(Cancelled.into()),
                _ => {}
            }
        }
    }
}

pub fn confirm(message: &str, yes: bool, allowed: bool) -> Result<bool> {
    if yes {
        return Ok(true);
    }
    if !allowed {
        return Err(
            "Use --yes to apply noninteractive changes, or --dry-run to inspect them".into(),
        );
    }
    let choices = [
        Choice {
            shortcut: "n".into(),
            label: "No · cancel".into(),
            enabled: true,
        },
        Choice {
            shortcut: "y".into(),
            label: "Yes · apply".into(),
            enabled: true,
        },
    ];
    let confirmed = match menu(message, "", &choices, 0, allowed) {
        Ok(index) => index == 1,
        Err(error) if error.is::<Back>() => false,
        Err(error) => return Err(error),
    };
    if !confirmed {
        ui::no_write("Cancelled");
    }
    Ok(confirmed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use unicode_width::UnicodeWidthStr;

    fn key(code: KeyCode) -> KeyEvent {
        KeyEvent::new(code, KeyModifiers::NONE)
    }

    #[test]
    fn navigation_keeps_real_shortcuts_skips_disabled_choices_and_never_accepts_invalid_input() {
        let mut choices: Vec<_> = (1..=8)
            .map(|number| Choice::numbered(number, format!("Slot {number}")))
            .collect();
        choices[1].enabled = false;
        let mut menu = Menu::new(&choices, 0).unwrap();
        assert_eq!(menu.key(&choices, key(KeyCode::Down)), Action::Continue);
        assert_eq!(menu.selected, 2);
        menu.key(&choices, key(KeyCode::Char('8')));
        assert_eq!(menu.selected, 7);
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Accept);
        menu.key(&choices, key(KeyCode::Char('9')));
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Continue);
        menu.key(&choices, key(KeyCode::Char('2')));
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Continue);
        menu.key(&choices, key(KeyCode::Home));
        assert_eq!(menu.selected, 0);
        menu.key(&choices, key(KeyCode::Up));
        assert_eq!(menu.selected, 7);
        assert_eq!(menu.key(&choices, key(KeyCode::Esc)), Action::Back);
        assert_eq!(
            menu.key(
                &choices,
                KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL)
            ),
            Action::Cancel
        );
        assert_eq!(Menu::new(&choices, 1).unwrap().selected, 0);
        choices.iter_mut().for_each(|choice| choice.enabled = false);
        assert!(Menu::new(&choices, 0).is_err());
    }

    #[test]
    fn menu_preserves_selection_across_resize_and_keeps_the_selected_row_visible() {
        let choices: Vec<_> = (1..=18)
            .map(|number| {
                Choice::numbered(
                    number,
                    format!("Badge entrée {number} 🔑 · HF classic-1k\nLF em410x"),
                )
            })
            .collect();
        let mut menu = Menu::new(&choices, 0).unwrap();
        menu.key(&choices, key(KeyCode::Char('1')));
        menu.key(&choices, key(KeyCode::Char('8')));
        assert_eq!(menu.selected, 17);
        for size in [(120, 24), (40, 9), (20, 6), (8, 3), (1, 1)] {
            let lines = menu.lines("Slots", "", &choices, size);
            assert!(lines.len() <= size.1, "{size:?}: {lines:?}");
            assert!(
                lines.iter().any(|(line, _)| line.starts_with("› 18")),
                "{lines:?}"
            );
            assert_eq!(menu.selected, 17);
        }
        assert!(clip("entrée🔑", 8).width() <= 8);
        assert_eq!(wrap("é🔑x", 3), ["é🔑", "x"]);
    }

    #[test]
    fn deleting_shortcut_digits_selects_the_remaining_number() {
        let choices: Vec<_> = (1..=18)
            .map(|number| Choice::numbered(number, format!("Choice {number}")))
            .collect();
        let mut menu = Menu::new(&choices, 0).unwrap();
        menu.key(&choices, key(KeyCode::Char('1')));
        menu.key(&choices, key(KeyCode::Char('8')));
        assert_eq!(menu.selected, 17);
        menu.key(&choices, key(KeyCode::Backspace));
        assert_eq!(menu.selected, 0);
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Accept);
    }

    #[test]
    fn confirmations_require_explicit_yes_and_scripts_require_the_yes_flag() {
        let choices = [
            Choice {
                shortcut: "n".into(),
                label: "No".into(),
                enabled: true,
            },
            Choice {
                shortcut: "y".into(),
                label: "Yes".into(),
                enabled: true,
            },
        ];
        let mut menu = Menu::new(&choices, 0).unwrap();
        assert_eq!(menu.selected, 0);
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Accept);
        assert_eq!(
            menu.key(&choices, key(KeyCode::Char('Y'))),
            Action::Continue
        );
        assert_eq!(menu.selected, 1);
        assert_eq!(menu.key(&choices, key(KeyCode::Enter)), Action::Accept);
        assert!(confirm("Write?", true, false).unwrap());
        assert!(
            confirm("Write?", false, false)
                .unwrap_err()
                .to_string()
                .contains("--yes")
        );
        assert!(choose("Action", &["Read".into()], false).is_err());
        assert!(input("ID", "ID", "", "", false, |value| Ok(value.to_owned())).is_err());
    }
}
