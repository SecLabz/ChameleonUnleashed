# Release packages

For step-by-step flashing instructions, see [Quick installation](../README.md#quick-installation).

Download archives from [GitHub Releases](https://github.com/SecLabz/ChameleonUnleashed/releases).

| Archive | Contents |
| --- | --- |
| `chameleon-unleashed-app.zip` | Application-only USB DFU update. |
| `chameleon-unleashed-full.zip` | Application, bootloader and SoftDevice USB DFU update. |
| `chameleon-unleashed-binaries.zip` | HEX images for direct programming. |
| `chameleon-unleashed-cdn.zip` | Matching web modules, tables and resource manifests. |
| `chameleon-linux-x64.zip` | Native CLI for Linux x64. |
| `chameleon-windows-x64.zip` | Native CLI for Windows x64. |
| `chameleon-macos-arm64.zip` | Native CLI for Apple Silicon Macs. |
| `chameleon-macos-x64.zip` | Native CLI for Intel Macs. |

Extract the CLI archive for your platform and run `chameleon` (`chameleon.exe`
on Windows). The Python CLI is available from source.

Use firmware and CDN archives from the same build. Web resources are published
automatically to GitHub Pages. For manual hosting, extract the CDN archive at
the firmware's configured resource URL and keep older versions available.

The `dev` pre-release tracks the default branch. The project version is defined
in `software/Cargo.toml` under `[workspace.package]`; the native CLI, firmware,
web interface and CDN release manifest use that same version. Release tags must
match it exactly, for example `v1.0.0` for version `1.0.0`.

CDN resource paths remain content-addressed. Manifest schemas, protocol versions
and shared NFC libraries have their own version numbers.

Every push runs builds, Rust/Python/JavaScript tests and code checks. The complete
Chromium/WebKit suite also runs for `v*` tags and manual **Push handler** runs;
it is skipped for ordinary branch pushes and pull requests.
