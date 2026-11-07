# Chameleon Unleashed web interface

Connect an Ultra with matching firmware over USB and open
[192.168.16.1](http://192.168.16.1). Use one active browser client.
Internet access is needed to download the matching app resources.

For Bluetooth, use the standalone app on HTTPS or localhost, wake the Ultra,
and choose **Connect Bluetooth** in Chrome or another compatible browser.
The connection pill (top right on mobile, bottom left on desktop) shows the link
and battery. After a disconnection it becomes **Reconnect**.
The firmware must include Web API contract 1; an older firmware needs a USB
update first. The hosted app works directly over BLE and does not require
the USB network connection. Safari and Firefox do not provide native Web Bluetooth.
[Build, compatibility and test bench →](../../docs/web-bluetooth.md)

NFC-V and NFC-B emulation pause while Bluetooth is connected; Slots and Device
mark them as paused. Transfer the slot image over BLE, then choose
**Device → Disconnect Bluetooth** to emulate. This restriction applies to
emulation, not the reader functions.

Firmware updates require USB. Choose **Device → Firmware update** and select a release or local ZIP.
You can also use the CLI with a release archive.
[First installation and recovery →](../../docs/web-updates.md)

Select **NFC** or **RFID**, place a tag on the antenna and choose **Tap to Scan**.
Studio opens when the tag is identified. You can also **Import…** a file or
start with **New…**.

## Studio and slots

| Studio | Slots |
| :---: | :---: |
| <a href="../../docs/images/readme/web-studio.png"><img src="../../docs/images/readme/web-studio.png" alt="Studio with a Classic capture, memory and document actions" width="320"></a> | <a href="../../docs/images/readme/web-slots.png"><img src="../../docs/images/readme/web-slots.png" alt="Slots with slot 1 selected and its NFC image enabled" width="320"></a> |

Mobile layout with demo data. Click a screenshot to enlarge it.

- **Read** acquires memory; for Classic, it also recovers keys.
- **Export → JSON file** saves a backup, including partial reads and device settings.
- **Send to** saves to a **Slot…**, starts **Slot & emulate**, or guides copying
  to a physical tag (**Copy to a tag…** / **Program a tag…**).

In **Slots**, select the slot to emulate and enable its NFC/RFID sides.
Emulation continues while the Ultra is powered, even after closing the browser.

Editing a document changes it locally. **Write changes** applies those edits
to a compatible physical tag. `??` means unread or unknown bytes.
After an interrupted write or upload, inspect the target before retrying.
Export documents you want to keep; browser storage is not a permanent backup.

## Offline use

Resources are saved automatically in the browser when Internet is available.
Before going offline, check **Device → Offline use → Ready offline** in the
browser you will use. Use **Retry** if preparation is incomplete.
**Session only** does not survive a reload.

In the hosted Bluetooth app, **Ready offline** also checks the HTML, stylesheet,
icons, app bundle and worker. Close and reopen that same URL once before leaving
Internet access. Browser storage can be evicted; export important documents.
Firmware updates continue to require USB.
