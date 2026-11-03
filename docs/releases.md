# Release packages

For step-by-step flashing instructions, see [Quick installation](../README.md#quick-installation).

Download archives from [GitHub Releases](https://github.com/SecLabz/ChameleonUnleashed/releases).

Choose the archive matching your OS and architecture. Each contains the native
CLI, application firmware (`firmware.zip`) and platform-specific `INSTALL.txt`.

| Archive (v1.0.0 examples) | Platform |
| --- | --- |
| `ChameleonUnleashed-v1.0.0-macos-arm64.zip` | macOS, Apple Silicon |
| `ChameleonUnleashed-v1.0.0-macos-x64.zip` | macOS, Intel |
| `ChameleonUnleashed-v1.0.0-windows-x64.zip` | Windows x64 |
| `ChameleonUnleashed-v1.0.0-linux-x64.zip` | Linux x64 |

Extract the downloaded archive, keep the included `firmware.zip` intact, and
follow `INSTALL.txt`. The executable is `chameleon` (`chameleon.exe` on Windows).
The Python CLI remains available from source.

`ChameleonUnleashed-v1.0.0-firmware-resources.zip` provides standalone packages:

| Path | Contents |
| --- | --- |
| `firmware/application.zip` | Application-only USB DFU update. |
| `firmware/system.zip` | Application, bootloader and SoftDevice USB DFU update. |
| `images/` | HEX images for direct programming, with notices. |
| `cdn/` | Matching web modules, tables, manifests and license notices. |

Web resources are published automatically to GitHub Pages. For manual hosting,
publish the contents of `cdn/` at the firmware's configured resource URL and keep
older versions available. Use firmware and resources from the same build.

Development archives include both the version and source commit in their names,
for example `ChameleonUnleashed-v1.0.0-dev.abcdef0-macos-arm64.zip`.

The `dev` pre-release tracks the default branch. The project version is defined
in `software/Cargo.toml` under `[workspace.package]`; the native CLI, firmware,
web interface and CDN release manifest use that same version. Release tags must
match it exactly, for example `v1.0.0` for version `1.0.0`.

CDN resource paths remain content-addressed. Manifest schemas, protocol versions
and shared NFC libraries have their own version numbers.

Every push runs builds, Rust/Python/JavaScript tests and code checks. The complete
Chromium/WebKit suite also runs for `v*` tags and manual **Push handler** runs;
it is skipped for ordinary branch pushes and pull requests.
