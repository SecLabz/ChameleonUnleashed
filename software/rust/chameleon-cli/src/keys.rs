//! Explicit, one-sector Classic trailer change.
use chameleon_client::{Reader, Result, TagCredential};
use clap::{Args, Subcommand};
use nfc_native::{
    autopwn::RecoverySource,
    formats::image::{Model, classic},
};

#[derive(Subcommand)]
pub enum Command {
    /// Change and verify one sector trailer's keys and access settings.
    Set(Options),
}

#[derive(Args)]
pub struct Options {
    /// Expected target UID in hexadecimal.
    #[arg(long)]
    pub uid: String,
    /// Target sector index, starting at zero.
    #[arg(long)]
    pub sector: u8,
    /// Current six-byte authentication key in hexadecimal.
    #[arg(long, default_value = "FFFFFFFFFFFF")]
    pub auth_key: String,
    /// Authenticate the current trailer with Key B.
    #[arg(long)]
    pub auth_key_b: bool,
    /// New six-byte Key A in hexadecimal.
    #[arg(long)]
    pub key_a: String,
    /// New six-byte Key B in hexadecimal.
    #[arg(long)]
    pub key_b: String,
    /// Three encoded access-condition bytes in hexadecimal.
    #[arg(long)]
    pub access: Option<String>,
    /// General-purpose trailer byte in hexadecimal.
    #[arg(long)]
    pub gpb: Option<String>,
    /// Tag model used to interpret its memory geometry.
    #[arg(long, value_enum)]
    pub model: Option<crate::restore::ClassicModel>,
    /// Preview changes without writing.
    #[arg(long)]
    pub dry_run: bool,
    /// Confirm the prepared operation without prompting.
    #[arg(long)]
    pub yes: bool,
}

pub fn set(reader: &mut Reader<'_>, options: Options, prompt_allowed: bool) -> Result<()> {
    let uid = crate::uid::hex_bytes(&options.uid, &[4, 7])?;
    if uid != reader.card.uid {
        return Err("Unexpected target UID; no keys were written".into());
    }
    let model = reader.card.model()?;
    if model.classic().is_none()
        || options
            .model
            .is_some_and(|value| Model::from(value) != model)
    {
        return Err("Target Classic model differs".into());
    }
    let blocks = model.classic().unwrap().1;
    if options.sector as usize >= classic::sector_count(blocks) {
        return Err("Sector outside target geometry".into());
    }
    let key: [u8; 6] = crate::uid::hex_bytes(&options.auth_key, &[6])?
        .try_into()
        .unwrap();
    let key_a = crate::uid::hex_bytes(&options.key_a, &[6])?;
    let key_b = crate::uid::hex_bytes(&options.key_b, &[6])?;
    let current = if options.auth_key_b {
        TagCredential::KeyB(key)
    } else {
        TagCredential::KeyA(key)
    };
    let sector = options.sector as usize;
    let trailer = reader.block(
        &RecoverySource {
            sector,
            is_key_b: options.auth_key_b,
            key,
        },
        classic::trailer(sector),
    )?;
    if !nfc_native::formats::image::classic_access_bits_valid(&trailer) {
        return Err("Target trailer access bits are invalid".into());
    }
    let before: [u8; 4] = trailer[6..10].try_into().unwrap();
    let mut after = [0u8; 16];
    after[..6].copy_from_slice(&key_a);
    after[6..10].copy_from_slice(&before);
    after[10..].copy_from_slice(&key_b);
    if let Some(access) = &options.access {
        after[6..9].copy_from_slice(&crate::uid::hex_bytes(access, &[3])?);
    }
    if let Some(gpb) = &options.gpb {
        after[9] = crate::uid::hex_bytes(gpb, &[1])?[0];
    }
    if !nfc_native::formats::image::classic_access_bits_valid(&after) {
        return Err("New Classic access-bit complements are invalid".into());
    }
    if !nfc_native::formats::physical::classic_trailer_writable(
        &trailer,
        options.auth_key_b,
        before != after[6..10],
    ) {
        return Err("Current trailer permissions do not allow this key and access change".into());
    }
    crate::ui::line(
        "Sector key change",
        &[
            format!("UID {}", crate::ui::uid_text(&uid)),
            format!("sector {sector:02}"),
        ],
    );
    crate::ui::line(
        "New Key A",
        &[crate::ui::paint(
            crate::ui::uid_text(&key_a),
            "32",
            crate::ui::stdout_color(),
        )],
    );
    crate::ui::line(
        "New Key B",
        &[crate::ui::paint(
            crate::ui::uid_text(&key_b),
            "94",
            crate::ui::stdout_color(),
        )],
    );
    crate::ui::change(
        "Access + GPB",
        nfc_native::formats::flipper::hex(&before),
        nfc_native::formats::flipper::hex(&after[6..10]),
    );
    if options.dry_run {
        crate::ui::no_write("Preview");
        return Ok(());
    }
    if !crate::ui::confirm(
        &format!(
            "Change keys for sector {sector} on tag {}?",
            crate::ui::uid_text(&uid)
        ),
        options.yes,
        prompt_allowed,
    )? {
        return Ok(());
    }
    reader.set_sector_keys(options.sector, current, before, after)?;
    crate::ui::result(true, "New keys and access bytes verified");
    Ok(())
}
