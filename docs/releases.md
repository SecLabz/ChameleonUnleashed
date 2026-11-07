# Release downloads

Download from [GitHub Releases](https://github.com/SecLabz/ChameleonUnleashed/releases).

Choose the archive for your computer. It contains the CLI, `firmware.zip` and
`INSTALL.txt` for installation, reinstallation and updates.

| Archive | Computer |
| --- | --- |
| `ChameleonUnleashed-v<version>-macos-arm64.zip` | Mac with Apple Silicon |
| `ChameleonUnleashed-v<version>-macos-x64.zip` | Mac with Intel processor |
| `ChameleonUnleashed-v<version>-windows-x64.zip` | Windows x64 |
| `ChameleonUnleashed-v<version>-linux-x64.zip` | Linux x64 |

Extract the archive, keep `firmware.zip` intact and follow `INSTALL.txt`.
[Step-by-step instructions](../README.md#quick-installation).

You can also update from **Device → Update firmware** in the USB web interface.
[Browser updates and recovery](web-updates.md).

For standalone firmware, HEX images or web resources, use
`ChameleonUnleashed-v<version>-firmware-resources.zip`:

| File or folder | Purpose |
| --- | --- |
| `firmware/install.zip` | Same firmware package as the platform archives. |
| `firmware/application.zip` | Update a device that already has web recovery. |
| `firmware/bootloader.zip` | Separate bootloader for DFU clients that need it. |
| `firmware/system.zip` | Firmware with bootloader and SoftDevice. |
| `images/` | HEX images for direct programming. |
| `cdn/` | Matching web resources for self-hosting. Keep older resources available. |

The `dev` pre-release contains development builds. Prefer a stable release for normal use.
