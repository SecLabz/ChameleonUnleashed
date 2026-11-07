# Native CLI reference

[← Hands-on guide](README.md)

Detailed examples, supported formats and hardware validation limits.

## Quick start

Download the CLI ZIP for your operating system and architecture from GitHub
Releases, then extract it. On macOS/Linux, run `./chameleon --help`; in Windows
PowerShell, run `.\chameleon.exe --help`. See [release packages](../../docs/releases.md)
for the archive names.

To compile from the repository root instead:

```sh
./dev build cli
# Executable: software/target/release/chameleon
```

See [build prerequisites](../../docs/README.md#local-development). Connect one
Ultra with matching firmware, then run the executable directly or add it to your
`PATH`:

```sh
chameleon info
chameleon hf search
chameleon                    # Interactive console
chameleon hf mf --help       # Options for any command
```

Add `--port DEVICE_PORT` to select a device. Scripts use subcommands or
`chameleon -c 'COMMAND'`.

Jump to: [firmware](#usb-firmware-updates) · [slots](#slots-and-emulation) ·
[Classic](#classic-recovery) · [NTAG / Ultralight](#ntag-and-ultralight-inspection) ·
[NFC-B / ST25TB / NFC-V](#nfc-b-st25tb-and-nfc-v) · [LF](#lf-tags) ·
[sniffers](#passive-sniffers) · [raw exchanges](#raw-nfc-exchanges).

### Before writing

- Use `--dry-run` where available to preview, then `--yes` to apply in scripts.
  Interactive confirmations default to **No**.
- After an uncertain write reply, inspect the target before retrying.
- Unread bytes appear as `??`. Prefer JSON to preserve partial reads, hidden-byte
  state and credentials. Existing output files are never overwritten.
- Ctrl-C cancels between bounded device operations and waits for radio cleanup.

### Console shortcuts

| Action | Keys |
| --- | --- |
| Complete commands, options, values and paths | Tab (spaces are quoted automatically) |
| Recall / search history | Up/Down / Ctrl-R |
| Navigate guided menus | Up/Down or number, then Enter |
| Go back / cancel | Esc / Ctrl-C |
| Exit | Ctrl-D or `quit` |

In the slot menu, `›` marks the focused row and `*` the active device slot.
Selecting a row opens its actions.

History lives in `chameleon/history.txt` under local application data
(`CHAMELEON_DATA_DIR` to relocate). Prefix a command with a space to exclude it,
especially with credentials. Disable colors with `--no-color` or `NO_COLOR=1`.

## USB firmware updates

```sh
chameleon dfu
chameleon flash firmware.zip --dry-run
chameleon flash firmware.zip --yes
chameleon flash firmware/system.zip --yes
```

Use the release archive's `firmware.zip` for installation, reinstallation and updates.
You can also use **Device → Update firmware** in the USB web interface.
[Packages](../../docs/releases.md) · [Updates and recovery](../../docs/web-updates.md).

**Export slots before flashing a full/system package:** the stock bootloader can
overwrite slot storage. Application-only updates leave the bootloader unchanged.

- Existing Nordic DFU ZIPs need only the CLI; no Python or `nrfutil`.
- `--dry-run` validates the package without USB; the bootloader checks signatures
  and hashes during transfer. Scripts require `--yes`.
- `./dev flash` builds and flashes an application package; `./dev flash --full`
  includes system images. Building/signing still needs the firmware tools.

### Device selection and recovery

Connect one Ultra or use `--port DEVICE_PORT`, even in DFU. The CLI follows its
USB serial number across reboots.
For manual DFU entry: power off, hold **B**, plug in USB, release **B**; LEDs 4 and 5
should blink. Automatic DFU entry allows 30 seconds.

- An interrupted **application-only** transfer can restart from the same ZIP
  after reconnecting in DFU. Errors/Ctrl-C report completed images.
- After a partial **system** update, resume with the application ZIP if the
  bootloader step completed. Our v5 bootloader allows signed downgrades; stock
  bootloaders retain their own version policy.

## Slots and emulation

```sh
chameleon status
chameleon status --verbose
chameleon slot list
chameleon slot load card.nfc 2 --name "Test card" --yes
chameleon slot load tag.json 2 --emulator-defaults --yes
chameleon slot info 2
chameleon slot dump 2 --hf
chameleon slot export 2 --hf -o backup.json
chameleon slot setup 2 --hf --write-mode shadow
chameleon slot start 2
chameleon slot stop
chameleon slot init 2 --type ntag215 --yes
chameleon slot clear 2 --hf --yes
```

- `status` shows device/USB/battery, selected slot and HF/LF activation, device
  preferences and short/long button assignments. `Enabled` describes a slot
  setting; Reader mode stops emulation. BLE pairing describes whether pairing
  is required, not whether a BLE connection is active. Pairing keys are hidden.
  `--verbose` adds hardware identity, selected firmware features and the selected
  slot's image identity/settings via frozen snapshots. Snapshot capture briefly
  pauses RF; busy or unsupported snapshots are marked unavailable. Status never
  changes mode or slot. Older firmware may not expose every preference.
- `slot` opens the guided menu; scripts supply slot and HF/LF choices.
- `load` saves and verifies, preserving the opposite side. Interrupted uploads
  can leave partial data.
- `load` also accepts version 1 NTAG/Ultralight JSON captures from the CLI or web
  interface. Unknown credential bytes and emulator metadata require confirmation,
  or `--emulator-defaults` in scripts; `--yes` only confirms slot replacement.
  Defaults use password `FFFFFFFF`, zero for other credentials/counters/signature,
  and the model's version. Known bytes are preserved. Unread user/configuration
  pages block loading even with defaults. The source JSON stays unchanged;
  authentication and originality checks may fail with defaults.
- `start` emulates both enabled sides while powered, even after CLI exit.
  Stop before editing stored bytes.
- `info` decodes identities and settings; `dump` displays stored bytes, including
  credentials (`--hf` or `--lf`, also **Show dump** in the menu).
- `dump`/`info` take a snapshot without changing the active slot or mode.
  If busy, remove the reader and retry.

Use JSON for backups: it retains activation data, settings and reserved bytes;
LF requires it. ST25TB `.nfc` import needs `--chip-id N`. Nonzero Type 2
authentication/tearing flags cannot be restored over serial; keep their JSON backup.
Native persistence and RF still need qualification.

## Classic recovery

```sh
chameleon hf mf info
chameleon hf mf check --sector 0 --key FFFFFFFFFFFF
chameleon hf mf autopwn --output card.nfc
chameleon hf mf nested --key FFFFFFFFFFFF --sector 1
chameleon hf mf staticnested --key FFFFFFFFFFFF --sector 1
chameleon hf mf hardnested --key FFFFFFFFFFFF --sector 1
chameleon hf mf darkside --sector 0
```

- `info` probes PRNG and magic signatures; overlapping matches do not prove UID
  writability. Gen2 confirmation may need `--key HEX` and `--key-b` for sector 0.
- AutoPwn tries the built-in dictionary, recovers keys and reads memory.
  Add `--key KEY` / `--dict keys.dic`; `--verbose` shows progress.
- Use `--output card.json` for partial data, recovered keys and seven-byte UIDs.
  `.nfc` requires a complete read and four-byte UID. Without `--output`, the dump
  appears in the terminal. Failure/cancellation preserves partial data.
- Nested methods use source sector 0 Key A by default. Override with
  `--source-sector` / `--source-key-b`; `--key-b` selects target Key B.
  Keys are verified on-card.

Generation-specific magic probes still need physical qualification.

Dictionary files are plain text, one 12-hex-digit key per line. Their extension
is unrestricted; examples use `.dic`.

## Classic repair and factory format

```sh
chameleon hf mf repair --dry-run
chameleon hf mf repair --generation gen2 --model classic1k --yes
chameleon hf mf repair --reset-access 0,1 --access-only --dry-run
chameleon hf mf format --dict target.dic --dry-run
chameleon hf mf format --dict target.dic --yes
```

| Command | Changes | Preserves |
| --- | --- | --- |
| `repair` | Block-0 identity; optional `--uid HEX` / `--reset-access SECTORS` or `all` | Data blocks and manufacturer tail; UID unless `--uid` is supplied |
| `repair --access-only` | Selected sectors' access settings | Block 0 and data blocks |
| `format` | Zeros data; resets keys/access to MCT defaults | Block 0 |

Requires rewritable Gen1A/Gen1B (backdoor) or CUID (valid keys/permissions).
Supply `--key HEX` or `--dict`; essential keys are also tried, without recovery.
Undetected repair targets need `--generation` / `--model`; seven-byte cards
also need `--uid`.

Writes are preflighted and verified. Gen1A/B and seven-byte maintenance still
need on-card qualification.

## NTAG and Ultralight inspection

```sh
chameleon hf mfu info
chameleon hf mfu read --page 4 --count 8
chameleon hf mfu dump --output tag.json
chameleon hf mfu ndef --output message.ndef
chameleon hf mfu info --model ultralight-c
chameleon hf mfu dump --model ultralight-c --key 00112233445566778899AABBCCDDEEFF --output tag.json
chameleon hf mfu write --page 6 01020304 --dry-run
chameleon hf mfu write --page 6 01020304 --yes
```

- Models are detected automatically when GET_VERSION is available; otherwise
  specify `--model` for original Ultralight / Ultralight C.
- `--password HEX` supplies four password bytes; `--key HEX` supplies a 16-byte
  Ultralight C key. Rejected credentials stop the operation without retry.
- `ndef` extracts the tag's NDEF message and decodes simple text or URL records.
  `--output` saves that binary message (`.ndef`), rather than a full memory dump.
- `write` checks locks/current bytes, changes one user page and verifies it.
  Original Ultralight writing is unsupported. These commands do not change UID,
  locks, passwords or counters, or wipe tags.

## Physical NFC-A restoration

```sh
chameleon hf mf restore card.nfc --dry-run
chameleon hf mf restore card.nfc --yes --allow-different-uid
chameleon hf mfu restore tag.nfc --password FFFFFFFF --dry-run
chameleon hf mfu restore tag.nfc --password FFFFFFFF --yes
chameleon hf mfu restore source.nfc --model ultralight-c --key 00112233445566778899AABBCCDDEEFF --dry-run
chameleon hf mfu restore source.nfc --model ultralight-c --key 00112233445566778899AABBCCDDEEFF --yes
```

Restore accepts browser/native JSON and Flipper `.nfc`. It preflights the target,
previews changes and verifies each written user unit. **Target UID, keys, locks
and configuration stay unchanged.** Classic target keys use `--key HEX` / `--dict`.

Ultralight C restores pages 4–39 only. Its 32-digit
`--key` uses cipher order: each eight-byte half is reversed in on-card pages
44–47, so raw key-page concatenation is not the cipher key. Omit it for an
unprotected target. Authentication cannot bypass permanent locks.
Ultralight C and NTAG210/212 writes still need on-card qualification;
original Ultralight writing is unsupported.

### Change Classic keys or UID

```sh
chameleon hf mf keys set --uid 01020304 --sector 1 \
  --auth-key FFFFFFFFFFFF --key-a 001122334455 --key-b AABBCCDDEEFF --dry-run
chameleon hf mf uid set 01020304 --dry-run
chameleon hf mf uid set 01020304 --yes
```

Apply a reviewed key change with `--yes`; current trailer permissions must allow
it. Four-byte Gen1A UID writes need no key. Authenticated changes require
`--generation gen2 --key-a HEX` or `--key-b HEX` and may be one-time.

Other UID methods need explicit `--generation`:
`gen1b`, `gen3`, `gen4`, `gdm20`, `gdm40`, `gdm40-zuid`, `super-gen1`.
Gen4 accepts a four-byte `--password`; Gen3 also accepts seven-byte UIDs.
Additional generations and seven-byte changes remain unqualified.
FUID and other one-time UID methods are excluded.

## NFC-B, ST25TB and NFC-V

```sh
chameleon hf 14b info
chameleon hf st25tb info
chameleon hf st25tb dump --output tag.json
chameleon hf st25tb write --block 7 12345678
chameleon hf st25tb restore tag.json --yes
chameleon hf st25tb tearoff --block 6 --target 1FFFFFEA
chameleon hf 15 info
chameleon hf 15 dump --output slix.json
chameleon hf 15 dump --output slix.nfc --emulator-defaults
```

`hf search` tries NFC-A, Type B, ST25TB and NFC-V until it finds an identity.

| Family | Read / export support |
| --- | --- |
| Type B | PUPI/ATQB identity only |
| ST25TB512-AT | Memory reads; verified eight-hex-digit writes |
| SRI512 / ST25TB512-AC | Memory reads; JSON only, no `.nfc` export or emulation |
| NFC-V | Up to 64 four-byte blocks; `.nfc` needs `--emulator-defaults` for unread security metadata |

Incomplete reads still save partial JSON.
ST25TB/LF chip views display 32-bit words most significant byte first without
changing capture/export byte order.

ST25TB512-AT `restore` requires the same UID and verifies a final dump. Counter
increases need `--tearoff`; sysOTP changes need `--otp`. Save a fresh dump and
read the counter before/after tearoff.

Tearoff defaults to a 30-minute safety deadline. Tune with `--safety-minutes`,
`--adjustment-us` and `--timing`. For timing measurements, use
`--timing --safety-minutes 1`: 32-bit cycle totals wrap after about 67 seconds.
SRI512 counter pulses remain unqualified.

## SLIX, SLIX-L and NFC-V user memory

```sh
chameleon hf slix info
chameleon hf slix-l dump --password 11223344 --output tag.json
chameleon hf 15 write --block 2 01020304 --dry-run
chameleon hf 15 write --block 2 01020304 --yes
chameleon hf slix-l restore tag.json --password 11223344 --dry-run
chameleon hf slix-l restore tag.json --password 11223344 --yes
```

- Physical SLIX/SLIX-L expose 28/8 user blocks; these commands check the product
  UID. `hf 15` supports ISO15693 geometries up to 64 four-byte blocks.
- Privacy passwords use four bytes in RF order; they are neither cached nor printed.
- Restore accepts complete native/browser NFC-V JSON and `.nfc`, requires the
  same UID and checks every changed block. Unknown/locked permissions stop writes.
  UID, locks, passwords, AFI and DSFID are preserved.

## DESFire public captures

```sh
chameleon hf desfire info
chameleon hf desfire dump --output badge.json
chameleon hf desfire dump --slot 2 --name "Badge EV2" --yes --output badge.json
chameleon slot start 2
chameleon slot stop
chameleon slot export 2 --hf -o badge-backup.json
```

Captures retain identity, version, public applications/files and unread/denied
states. **No authentication or secure messaging**, including in emulation.

- Ctrl-C preserves acquired JSON observations and cancels slot saving.
- `--name` / `--yes` require `--slot`; replacement affects HF only.
- `slot load` accepts capture JSON and supported `.nfc`. Images over 4500 bytes
  including metadata are rejected without changing the slot.
- Other tools may ignore `.nfc` public-capture extensions; back up as JSON.

## LF tags

```sh
chameleon lf search --verbose
chameleon lf read hid --format H10301
chameleon lf read em4x05
chameleon lf write em410x --id 0102030405 --dry-run
chameleon lf write hid --format H10301 --fc 10 --cn 1234 --yes
```

Console: `lf copy` guides the copy; `lf copy --last` reuses the last successful
read. Scripts supply explicit fields.

- Search reads EM410x/Electra, HID Prox, ioProx, PAC, Viking and Jablotron, then
  checks T55x7 chip data. Detection does not establish writability.
- EM4x05/EM4x69 need explicit reads and are not T55xx write targets.
- Programming supports those families plus IDTECK (no broadcast reader). It checks
  the target, writes data before configuration and verifies without write retries.
  The password word is preserved.
- Password-mode, AOR, OTP, extended configurations and ambiguous PSK2/3 captures
  are unsupported. Constant NRZ/PSK1 words can prevent verification.
- Physical EM410x copies preserve clock/16 and clock/32; slots use clock/64.
  Reconnect after transport errors. Ctrl-C keeps completed verification results.

## Passive sniffers

```sh
chameleon hf 14a sniff --verbose --frame --out nfca.jsonl
chameleon hf 15 sniff --verbose --frame --out nfcv.jsonl
```

Place the Ultra between reader and card; use USB with no active BLE.
Stop with Ctrl-C or `--duration SECONDS`; `--out` creates a new JSONL file.
Gain defaults to 7; try `--gain 6` for coupling.

The live table shows RX end timestamps, source, data, CRC status and annotations.
Reader traffic is yellow, tag traffic light gray, valid CRC bytes green and errors
red. Short frames include their bit count, such as `52(7)`; `!` marks a frame
parity error. CRC status is `—` when unavailable or not applicable.
`--verbose` adds receiver gain, hardware details, bit counts and parity/CRC checks.
`--frame` adds `ΔRX`, the interval between RX completions, not an RF frame delay.
Redirected output and `--no-color`/`NO_COLOR` output contain no ANSI styling;
JSONL exports retain raw frames and annotations.

- NFC-A: 106 kbit/s; needs an observed REQA/WUPA for plaintext context.
  Crypto1 is not decrypted.
- NFC-V: 1-of-4/1-of-256 and single-subcarrier replies. Dual subcarrier and 10%
  ASK remain unqualified.
- Frames may be missed, malformed or truncated; losses are reported.
  Timestamps include software latency and do not measure RF delay.

## Raw NFC exchanges

```sh
chameleon hf 14a raw 26 --bits 7
chameleon hf 14a raw 3004 --select --crc
chameleon hf 14a raw 3004 --select --crc --keep
chameleon hf 14a raw 3008 --crc
chameleon hf field off
chameleon hf 14a raw --file frames.txt --timeout-ms 100
chameleon hf 15 raw 260100
chameleon hf 15 raw --file vicinity-frames.txt
```

Files contain one hex frame per line with `#` comments; the first error stops
execution. NFC-B raw is unavailable.

| | NFC-A | NFC-V |
| --- | --- | --- |
| Limit | 256 × 62-byte frames; 30-second budget checked between frames | 12 × 22-byte frames in one bounded operation |
| CRC | `--crc` appends TX, validates/removes RX; separate `--append-crc` / `--check-crc` available | TX appended; checked RX retained |
| Partial bytes | `/BITS` per frame or `--bits` for one; short replies byte-padded | — |

RF shuts down on exit unless a successful command uses `--keep` (`-k`), retaining
field/tag state across invocations. Finish with `hf field off` or a command without
`--keep`. Errors/cancellation request shutdown; field off leaves reader mode selected.

## Binary distribution

Include `LICENSE` and `docs/licenses/` from the pinned `nfc-libs-rs` revision
alongside the executable.
