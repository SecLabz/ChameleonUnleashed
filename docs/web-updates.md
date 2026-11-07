# Firmware updates

## Update with the CLI

Download the archive for your computer from [Releases](https://github.com/SecLabz/ChameleonUnleashed/releases)
and follow its `INSTALL.txt`. The same procedure works for installation,
reinstallation and updates. [Step-by-step instructions](../README.md#quick-installation).
The CLI keeps an already matching Unleashed bootloader. Use
`chameleon flash firmware.zip --full` to reinstall every component in the ZIP.

## Update through a browser

1. Open [192.168.16.1](http://192.168.16.1) over USB, or connect through the HTTPS Bluetooth app.
2. Choose **Device → Firmware update**.
3. Select the firmware and version, then install. Over Bluetooth, select the Ultra in DFU when prompted.

You can also choose a Nordic DFU ZIP with **Update from a file**.
A local ZIP works without Internet; downloading a release requires Internet access.
The selector offers up to five compatible stable versions of Unleashed and Official.
Choose **Development** to install the latest development build; it may be unstable.

## First installation and versions

Older installations, including Unleashed v1.0.0, need the included `firmware.zip`
flashed through the CLI once to enable browser updates. It requires S140 7.2.0.
The package remains usable for later installations and updates.

**Export your saved tags before this migration:** the old bootloader may overwrite
stored data. Also export before switching between Unleashed and Official, since
stored data may be incompatible. The USB update page lets you erase tags and settings.

Installing Official removes Unleashed web recovery. To return, flash Unleashed's
`firmware.zip` through the CLI.

## If something goes wrong

- **Open web recovery:** power off, hold **A** and connect USB. Open
  [192.168.16.1](http://192.168.16.1); the update page opens directly.
  The two central LEDs stay cyan. An invalid application also opens recovery.
- **Leave recovery:** choose **Return to Chameleon**, or press and release **B**.
- **Use classic DFU:** power off, hold **B**, connect USB and release **B**.
  Flash again through the CLI. The two central LEDs alternate.

USB web application updates can be retried in recovery. Bluetooth DFU rewrites recovery;
an interrupted Bluetooth update, recovery replacement or Official restore may require classic DFU.
There is no automatic rollback.

Recovery stays available while USB is powered. Without USB power it sleeps after
five seconds; return to the normal application for tag emulation.
