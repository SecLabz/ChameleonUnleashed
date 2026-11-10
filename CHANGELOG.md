# Changelog

## v2.1.1 — 2026-11-10

- Faster Classic AutoPwn dictionary checks and Hardnested capture.
- Hardnested capture checks the tag UID and requires firmware 2.1.1 or later.
- The web app skips redundant sector reads when verifying recovered keys.

## v2.1.0 — 2026-11-08

- Faster Classic AutoPwn in the native CLI and USB/Bluetooth web app.
- AutoPwn requires matching firmware 2.1.0 or newer.
- Battery readings complete before Bluetooth connections are accepted.

## v2.0.0 — 2026-11-08

### Added

- **USB web recovery:** hold **A** while connecting USB to open the update page,
  even when the application cannot start.
- **Bluetooth web app:** scan, read and manage slots without USB, with offline
  resources and a button action that shares the app URL over NFC.
- **Browser firmware updates over USB or Bluetooth:** choose a compatible Unleashed
  or Official release, the latest development build, or a local Nordic DFU ZIP.
- **Classic key export:** save known document keys as a deduplicated `.dic` dictionary.
- **Remembered Classic keys:** subsequent reads recheck saved keys before recovery.
  Clear them in **Device → Remembered tag keys → Clear**.

### Improved

- Faster MIFARE Classic recovery and reads through retained RF sessions, batched
  key reuse and nonce streaming.
- ST25TB emulation accepts wider reader timing and longer RF windows while USB is active.
- CLI updates preserve a matching bootloader; `--full` reinstalls all packaged components.
- Separate stable and development Bluetooth sites; `./dev serve ble` runs the app locally.

### Upgrade notes

- **From v1.0.0:** export saved tags, then flash the platform archive's `firmware.zip`
  through the CLI once to install web recovery and enable browser updates.
- NFC-B/NFC-V emulation pauses while Bluetooth is connected; disconnect to resume it.
- Installing Official removes Unleashed web recovery. Interrupted Bluetooth updates
  may require classic USB DFU; there is no automatic rollback.

See [installation](README.md#quick-installation) and [updates and recovery](docs/web-updates.md).
