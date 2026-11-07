# Chameleon Unleashed documentation

- [Quick installation and USB flashing](../README.md#quick-installation)
- [USB browser updates and permanent recovery](web-updates.md)
- [Native CLI](../software/rust/README.md)
- [USB web interface](../software/web/README.md)
- [Performance & measurements](performance.md)
- [Release packages](releases.md)
- [Upstream wiki](https://github.com/RfidResearchGroup/ChameleonUltra/wiki) for hardware and the original Python client

## Local development

Run `./dev COMMAND --help` for options. On Windows, use `py dev COMMAND`;
firmware builds need a Unix environment such as MSYS2 and remain unqualified.

Requirements:

- Python 3.10+, Git, Rust 1.93.1, `wasm32-unknown-unknown` and Zopfli.
- Firmware: Bash, GNU Make, Arm GNU toolchain (`arm-none-eabi-gcc`), Nordic
  `nrfutil` with `nrf5sdk-tools`, `mergehex` and `zip`.

```sh
./dev build          # Firmware and matching CDN ZIPs in firmware/objects/
./dev build --clean  # Rebuild all firmware objects
./dev build cli      # Native CLI in software/target/release/chameleon
./dev flash          # Build and flash the application over USB
./dev flash --full   # Include bootloader and SoftDevice
./dev flash --install-recovery # Bootloader v5 + Nordic ZIP web recovery installation
./dev serve          # Local web interface at http://127.0.0.1:8080
./dev cdn            # CDN ZIP in .cache/web/chameleon-unleashed-cdn.zip
```

`build cli` only needs Python, Git, Rust and a native linker. Cargo locks its
shared NFC libraries; Cargo target-directory settings can relocate the executable.

Web builds download and verify locked NFC sources in `.cache/web/shared`.
Use `--shared-source PATH` for an editable checkout. Rust is pinned in
`software/web/build-lock.json`; `RUSTUP_TOOLCHAIN` overrides it locally.
The Arm toolchain is detected automatically; `GNU_INSTALL_ROOT` takes precedence.

Firmware builds reuse compiled objects and regenerate the release packages.
Use `./dev build --clean` (or `./dev flash --clean`) after changing the Arm
toolchain, compiler options or build configuration. Direct builds accept
`bash firmware/build.sh --clean` too.

`serve` serves HTML/CSS/SVG directly from `software/web/public`, bundles current
JavaScript sources on request and relays `/api` to `http://192.168.16.1`.
Use `--port PORT` or `--device http://ADDRESS` to override defaults. Refresh the
browser after web edits; keep the server running. Compatible firmware does not
need reflashing for web changes.

WASM and tables are prepared automatically if missing, then reused from
`.cache/web`. After changing Rust/WASM sources, run `./dev cdn` and refresh.
Use the same `--shared-source PATH` for both commands when editing shared sources.
Changes to the shared JavaScript package, dictionaries and HID format definitions
are picked up on refresh. `serve` does not generate a CDN ZIP or embedded assets.

To flash an existing application or full DFU package:

```sh
chameleon flash firmware/objects/chameleon-unleashed-app.zip --dry-run
chameleon flash firmware/objects/chameleon-unleashed-app.zip --yes
```

The native CLI flashes existing packages without Python or `nrfutil`. `./dev flash`
builds the firmware and runs that same native command; generating and signing ZIPs
still needs the firmware build tools and Python `cryptography`. Default application
packages require an already migrated device. See [USB firmware updates](../software/rust/REFERENCE.md#usb-firmware-updates)
for device selection and recovery.

## Local checks

```sh
python3 -m unittest discover -s resource/tools
python3 -m unittest discover -s software/script/tests
```

The tools suite needs Node.js, build prerequisites and prepared web assets (`./dev cdn`).
Neither suite requires connected hardware.

## GitHub Pages and forks

Fork and clone this repository, enable workflows in **Actions**, then select
**Settings → Pages → Build and deployment → Source → GitHub Actions**.
On GitHub Free, use a public repository.

Push to the default branch or a `v*` tag to publish resources to your fork's
Pages URL before creating the release. You can also run **Push handler** manually
on the default branch. Other branches and pull requests only build artifacts.

Use `./dev serve` for local edits. To build and flash firmware using your fork's
published resources:

```sh
./dev flash --asset-base https://alice.github.io/ChameleonUnleashed/
```

Replace the example with your Pages URL, keeping the trailing `/`, and publish
matching resources first. `--asset-base` also works with `build` and `cdn`;
without it, local builds use the upstream Pages URL in `software/web/config.json`.
