<h1 align="center">Chameleon Unleashed</h1>

<p align="center">
  <strong>Experimental firmware.</strong>
</p>

<p align="center">
  <img src="docs/images/ultra-overview.png" alt="Chameleon Ultra hardware, front and back views" width="830">
</p>

An opinionated Chameleon Ultra fork with faster MIFARE Classic recovery and
emulation, a web interface over USB, and experimental NFC-B/NFC-V support.

None of this would exist without [ChameleonUltra](https://github.com/RfidResearchGroup/ChameleonUltra)
and the people who built its firmware, hardware, tools and research. They deserve
the credit for the foundation this fork stands on.

[Features](#whats-different) · [Screenshots](#see-it-in-action) ·
[Performance](docs/performance.md) · [Before you flash](#before-you-flash) ·
[Install](#quick-installation) · [Documentation](docs/README.md)

## What's different?

- **Much faster RF wake-up.** From around **1.8 seconds with animations** to
  roughly **16 ms for Classic** and **23 ms for SLIX-L** in development benchmarks.
- **Faster MIFARE Classic emulation.** Optimized frame delay times (FDT), close
  to those of real tags.
  [See timing measurements →](docs/performance.md#mifare-classic-emulation-timing)
- **Faster MIFARE Classic AutoPwn.** Optimized key recovery and card reading
  through the CLI and web interface.
- **A web interface over USB.** Scan cards, inspect captures and manage emulation
  slots from a browser, including on compatible smartphones. Save resources for
  offline use.
- **A native Rust CLI.** An interactive console for reading cards, managing slots
  and updating firmware over USB.
- **Experimental NFC-B and NFC-V reading and emulation.** Support for selected
  tags, including ST25TB and SLIX/SLIX-L. Results may vary across tags, readers
  and hardware.
- **ST25TB tearoff.** Built-in tearoff tooling for experimenting with ST25TB counters.
- **Real-time NFC-A and NFC-V sniffing.** Stream traffic over USB to the Rust CLI,
  with protocol annotations and capture export. Frames may be missed or truncated.

## See it in action

| Scan | Studio | Slots |
| :---: | :---: | :---: |
| <a href="docs/images/readme/web-scan.png"><img src="docs/images/readme/web-scan.png" alt="Mobile web interface ready to scan a tag" width="240"></a> | <a href="docs/images/readme/web-studio.png"><img src="docs/images/readme/web-studio.png" alt="Mobile Studio showing a MIFARE Classic capture" width="240"></a> | <a href="docs/images/readme/web-slots.png"><img src="docs/images/readme/web-slots.png" alt="Mobile interface showing persistent emulation slots" width="240"></a> |

## USB web interface

Connect an Ultra running matching firmware over USB and open
[192.168.16.1](http://192.168.16.1) in your browser. Use Scan to read a tag, Studio
to inspect it, and Slots to save it for emulation.

The first use needs Internet access to download matching CDN resources, which
are saved in your browser. Before going offline, check
**Device → Offline use → Ready offline**. [Web guide →](software/web/README.md)

## Native CLI

Connect one device and run:

```sh
chameleon info
chameleon hf search
```

Run `chameleon` for the interactive console, or add `--port DEVICE_PORT`
to select a device. [Hands-on CLI guide →](software/rust/README.md)

## Before you flash

This fork is where I experiment with what I want my Chameleon Ultra to do. USB
comes first, BLE takes a back seat, and the priorities are my own. Expect rough
edges and things that change.

I use AI heavily to develop this firmware. I try to keep control of what goes
in, but some AI slop will probably slip through. Development moves fast, and not
everything gets tested as thoroughly as it should. That tradeoff also lets me
explore ideas and ship features I find cool. This is an RFID playground, not
nuclear power plant software—but bugs are still bugs, and I'd rather be upfront
about them.

I test on a clone. Features like the very experimental NFC-B and NFC-V support
may behave differently or fail entirely on other hardware. Treat them as
experiments, not promises.

If you want a polished, stable experience, go with the
[original ChameleonUltra project](https://github.com/RfidResearchGroup/ChameleonUltra).

## Quick installation

Flash a prebuilt application package over USB using the native CLI. This needs
no Rust, Python or `nrfutil`.

1. From [GitHub Releases](https://github.com/SecLabz/ChameleonUnleashed/releases),
   download **`chameleon-unleashed-app.zip`** and the **CLI archive for your OS
   and architecture** from the same release. See [archive names](docs/releases.md).
2. Extract the CLI archive and put `chameleon-unleashed-app.zip` beside the
   executable. **Keep the firmware ZIP intact.**
3. Connect one Chameleon Ultra with a USB data cable. Close any other client
   using the device.
4. Open a terminal in that folder, run the command for your OS below, and
   confirm the update when prompted.

**macOS / Linux**

```sh
chmod +x chameleon
./chameleon flash chameleon-unleashed-app.zip
```

**Windows (PowerShell)**

```powershell
.\chameleon.exe flash chameleon-unleashed-app.zip
```

The CLI switches the Ultra into USB DFU mode automatically. Keep it connected
until the update succeeds and the application restarts. Then open
[http://192.168.16.1](http://192.168.16.1) in your browser.

**If automatic DFU entry fails:** power the Ultra off, hold **B**, plug in USB,
then release **B**. LEDs **4 and 5** should blink. Run the same flash command again.

## Local development

From the repository root:

```sh
./dev build cli  # Build the native Rust CLI
./dev flash      # Build and flash the application over USB
./dev serve      # Run the local web interface connected to the Ultra
```

See the [development guide](docs/README.md#local-development) for prerequisites
and build options, including shared libraries and GitHub Pages.

## Attribution and license

Chameleon Unleashed builds on [ChameleonUltra](https://github.com/RfidResearchGroup/ChameleonUltra)
and retains its hardware, firmware, Python client and third-party notices.
See [AUTHORS.md](AUTHORS.md), [LICENSE](LICENSE),
[web dependency notices](software/web/THIRD_PARTY.txt), notices in source files
and the [upstream wiki](https://github.com/RfidResearchGroup/ChameleonUltra/wiki).
