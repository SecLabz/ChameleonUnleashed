# Web Bluetooth: build and qualification

The web app has an explicit Bluetooth backend. It uses the same device services
as the USB HTTP interface, including firmware-owned reservations and slot
transactions. There is no automatic HTTP fallback after a Bluetooth disconnect,
and no automatic retry of a mutation whose response was lost.

The implementation is available on `codex/web-bluetooth`. NFC-V and NFC-B
emulation are **unsupported while Bluetooth is connected**. Slot transfer and
autonomous emulation after disconnect remain available. This exception does
not exclude NFC-V/NFC-B reader operations from qualification. Remaining RF
parity is still being qualified; a successful slot exchange does not qualify
tag writes, tear-off timing, pairing or operation on battery.

## Run the standalone app

From the repository root, with the normal firmware build prerequisites installed:

```sh
./dev build
python3 resource/tools/prepare_hosted_web.py --output .cache/web/hosted
python3 -m http.server 8085 --bind 127.0.0.1 --directory .cache/web/hosted
```

Open `http://127.0.0.1:8085/app/`, wake the Ultra with its button, and choose
**Connect Bluetooth**. The output directory must be empty before preparation.
The tool also produces a static distribution suitable for an HTTPS host at any
repository prefix: publish the complete output tree, then open its `app/`
directory. This command prepares files locally; it does not deploy them or
modify the existing Pages resource catalogue.

Release CDN archives also carry the hosted shell under `web/<sha256>/` with its
file hashes, source commit timestamp and resource pins. The Pages generator
promotes a complete shell to `app/` and retains the immutable older bundles and
calculation resources. It selects the newest source commit across CI, release
archives and the existing baseline, so a scheduled catalogue refresh does not
roll back a newer development app. Equal timestamps prefer the current CI build.
Packaging refuses stale frontend resources or files changed after the build.
Release browser checks run the hosted suite against this archive/promotion path.

Chrome desktop and Android are the initial BLE targets. macOS Chromium has
been tested with the physical Ultra. Safari and Firefox can use local document
editing but do not provide the native BLE API. A secure origin and a user gesture
are required by [Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth).
The device's `http://192.168.16.1` origin remains the USB interface.

The hosted package starts without an HTTP device backend. After disconnecting,
reconnect explicitly and inspect the target before retrying an interrupted write.
Switching transport during an active workspace operation is refused. A Web Lock
prevents two tabs of the same origin from opening the same Ultra; the firmware
also isolates HTTP-owned and BLE-owned reservations.

The request deadline covers both the firmware response and completion of all
browser write promises (`writeValueWithResponse` or `writeValueWithoutResponse`). A notification can arrive before
the last write callback; it must not disarm timeout/disconnect cancellation
while that callback is still pending. Two regressions reproduce a stalled
callback after a valid reply, with respectively timeout and disconnection.
Both failed before the fix and pass afterward, including refusal to send the
queued mutation after a late callback. All 275 JS tests, type/format checks and
six rebuilt hosted Chromium/WebKit tests pass. This is software evidence; the
physical SLIX campaign below exercises normal exchanges with the corrected
transport. Bluefy remains unqualified.

Updates use **USB → http://192.168.16.1 → Device → Firmware update**. The hosted
app does not flash over BLE. Site permissions and BLE pairing/PIN dialogs belong
to the browser and OS; the existing pairing option is not bypassed.

### Manual iPhone / Bluefy check

The temporary, separate repository `SecLabz/chameleon-ble-preview-20261106`
hosts the optimized compiled interface (preview commit `080aef9`). It was built
from clean source `6d9c8a8` and matches application
`v1.0.0-82-g6d9c8a8`; the performance and qualification scope are recorded below:

`https://seclabz.github.io/chameleon-ble-preview-20261106/app/`

Open this HTTPS address directly in Bluefy, wake the Ultra, choose **Connect
Bluetooth**, and select `ChameleonUltra`. First check the device details and
slot list without changing settings or saving a slot. **Disconnect Bluetooth**
and reconnect explicitly to check the return path. Export an existing document
before checking its import or offline reopening. Keep the app in the foreground
during device operations. Avoid running automated hardware tests during a manual
phone session. To receive a new interface version, close all app tabs and reopen
the address: its service worker waits for old windows to close before activating.

All nine public file hashes, including version metadata and the unchanged
historical source archive, match the final package. Pages deployment
`42319058474` for preview commit `080aef9` succeeded. The new shell identity is
`ce83ca8b66ae4eab58b12b9d8398c32f111aa721d78a7b7d8b45faf73608cc51`.
Chromium on the public URL verified **Ready
offline**, export of all 128 Classic 2K blocks including edited block 126,
offline reopening and Device navigation at 320 px, without API requests or page
errors. For the previous `70cf55d` → `24472b8` update, a separate local check
serving those two actual successive packages first
requires a controlled old client, then verifies that the new worker waits, the
old window retains its version, and the new version activates after it closes.
That update-lifecycle check is local; it is distinct from the public-origin check. Actual Bluefy BLE, iOS file dialogs and background/reconnect behavior
remain separate manual qualifications. WebKit browser emulation cannot establish
native iOS BLE behavior. The temporary static repository retains its historical
`d5a58d3` source archive and license material, without development Git history
or device backups. The new source archive was excluded from publication.
Removing the repository later does not guarantee removal of copies or
GitHub retention records. The main development repository was not pushed.

This preview includes checked Plus SL1 2K maintenance and keeps explicit
known-model repair compatible. The checked requests require firmware `633159a`
or later; rejecting old firmware does not trigger an unguarded fallback.
Per-unit checked BLE maintenance, 2K data copying and sector-31 key/access
changes passed on synthetic PM3 RAM through the earlier interface as detailed
below; complete RF formatting and physical EEPROM behavior remain unqualified.
Before the performance changes, the interface snapshot now represented by
`5e50871` passed 315 JS tests, lint/typecheck, 12 hosted cases and 12 WebKit
mobile cases, without establishing native Bluefy BLE behavior. Its application
`v1.0.0-64-g265ddf2-dirty` was installed with all slots/settings preserved.
The current installed and published versions are qualified below.

USB and hosted deployment use separate artifacts of the common frontend. An
earlier synchronization of the copy fix used application-only firmware
`v1.0.0-62-g460ca13` was built and flashed after a fresh complete backup.
Its ZIP SHA-256 is
`233e38d9d1de7fbde375f1da2c078407de3d62187f78d54ca1b75ea594df0577`.
All eight metadata records, populated slot images, settings and active slot 0
match before/after. The actual USB `/`, `/app.mjs` and `/style.css` responses
match the build; the USB module is byte-identical to the verified hosted app.
On this firmware, a real browser connected over BLE, opened the active slot
image, disconnected and explicitly switched to the USB HTTP backend, with no
HTTP fallback during the BLE phase and device state preserved. The 2K RF copy
evidence above remains the separately recorded campaign on `633159a`; this
deployment check qualifies the synchronized assets and basic transport return.
Evidence: `460ca13-installed/` and `ble-usb-switch-460ca13/`.

### Physical SLIX reader: BLE coexistence fix

A physical ICODE SLIX (`E00401532560A988`, 28 × 4-byte blocks) exposed a
firmware coexistence failure on `1662597`: inventory, system information and
memory reads succeeded over USB with BLE disconnected, failed with status
`0x0001` over both BLE and USB while BLE was connected, then succeeded again
after disconnect. This separates the failure from browser framing or an API
command restriction. NFC-V **reading remains supported**; the accepted BLE
exclusion concerns NFC-V/NFC-B emulation.

Fix `4c011bf` replaces the continuous 25 ms NFC-V radio reservation with a
frame-sized 4–10 ms TX reservation. It starts ADC DMA inside the grant and
finishes the full 11,264-sample capture afterward, retaining CRC validation.
BLE uses normal-priority grants without extensions. A separate shared HFXO
owner holds the clock across capture and retained reader fields; releasing it
preserves the independent NFCT startup owner. Host regressions cover a scheduler
refusing grants above 10 ms, deferred peripheral capture, clock failure/loss,
retained fields and all clock-owner pairs. The scheduler regression fails
against the previous implementation. All 211 firmware/tool tests pass.

The application-only ZIP was flashed to the physical Ultra as
`v1.0.0-49-g4c011bf`. A fresh backup before/after confirmed all eight slot
metadata records, all three populated slot images, settings and active slot 0
were unchanged. On the same SLIX, all 16 subsequent command responses succeeded:
three inventories, system information and the full dump immediately after BLE
connection; the same five after connection negotiation; then inventory/system
information/dump over USB while BLE remained connected and after disconnect.
The two BLE dumps took approximately 480 and 544 ms. No RF writes or PM3
simulation were used.

The hosted production UI was also exercised in Chromium with the physical BLE
adapter: scan, identification, complete read, JSON export and browser-close
cleanup passed. The exported 28 blocks and locks match USB, with memory SHA-256
`b5fdab78d8947eacc864bfeecb4d2100780e5afe1cd8efafb124887913ac49fa`.
The GATT trace has no framing errors, pending exchanges or RF mutation commands,
and no HTTP device fallback occurred. Reproduce with the physical SLIX left on
the antenna, adapting its UID:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_SLIX_UID=E00401532560A988 \
  npm --prefix software/web run test:hardware:ble -- reader-nfcv.spec.mjs
```

This qualification covers an unprotected SLIX reader with USB power. Bluefy on
the real iPhone, battery operation, protected tags, writes and other NFC-V tag
models still require separate qualification. The hosted preview can use the
corrected firmware after reconnecting.

After the later Classic campaigns and reported physical movement, the SLIX was
no longer detected. A final single USB inventory on `ac7e5ed`, followed by three
before and three after an application restart, all returned `0001` without a
BLE connection. This does not establish the cause or a positive NFC-V-to-NFC-A
handover. Current SLIX placement needs confirmation before a new positive
qualification on the final firmware; the earlier `4c011bf` evidence remains
scoped to its recorded fixture. No SLIX write occurred. Settings/active slot
and a fresh full slot-image comparison remain unchanged. Evidence:
`.cache/web-ble-study-20261106/ac7e5ed-installed/slix-reboot-control.json`.

A subsequent USB-only inventory on `633159a` again returned `0001`. An
independent single PM3 `hf 15 reader` inventory reported no UID either, and
its subsequent USB ping passed. This observation is limited to the current
antenna/tag arrangement; it does not establish absence of the tag, RF tuning
or a firmware cause. No simulation or RF write was used for this comparison.
The Ultra's full slot/settings backup still matches its pre-flash state.
Evidence: `.cache/web-ble-study-20261106/slix-repositioned-633159a/`.

## Classic read performance: real tags, no PM3 (2026-11-07)

Firmware source `fb33af9` combines the interface from `dd79e3a` with transport
and read optimizations (`1006389`, `487a40b`). Application-only firmware
`v1.0.0-77-gfb33af9` was installed after a fresh full backup. All eight slot
metadata records, populated images, settings and reader state match afterward.
Boot initially selected slot 1; the pre-flash selection, slot 2, was restored
and included in the final comparison. The bootloader was not replaced.

The transport uses native writes without response when the characteristic
advertises support. Application commands remain serialized and still require a
complete, validated, correlated firmware response and completed browser writes.
A timeout or disconnect closes the transport; mutations are never replayed.
HELLO retains its six-byte format and advertises an optional read-only
`TRANSPORT_INFO` operation (23). Its versioned three-byte response reports the
current connection's NUS payload limit. New connections start at 20 bytes;
this Ultra reported 244. Old firmware and clients retain their 20-byte path.
The frontend authenticates filtered single candidates through checked sector reads and
reuses their observed memory within the current operation, avoiding a separate
key check, trailer read and later repeat of the sector. UID/model checks,
unknown blocks, partial-read fallback and Stop remain enforced. Key-reuse sweeps
retain the fast firmware authentication session: trying full checked reads for
every rejected sweep candidate was measured and rejected because it slowed USB.

The first physical Classic 1K tag produced two baseline BLE complete reads of
9,990.8 and 10,087.7 ms, versus USB 2,522.7 and 2,512.2 ms. Changing only the
write mode yielded one valid BLE measurement of 5,516.6 ms, with the same 64
blocks. A second timing was invalid because the observer watched a replaced
status node; it is excluded. This was a preliminary transport-only result,
not a repeated measurement of the final implementation. The user subsequently
replaced that tag with a different Classic 1K requiring hardnested.

On the second tag, equal 16-observation acquisition batches distinguish
transport overhead from RF work:

| Frontend / firmware | USB before | USB with BLE idle | BLE | USB after |
| --- | ---: | ---: | ---: | ---: |
| Baseline, 20-byte acknowledged writes | 129.55 ms | 127.40 ms | 207.32 ms | 124.59 ms |
| Optimized, negotiated writes | 122.95 ms | 125.35 ms | 142.51 ms | 123.98 ms |

Each cell is the median of ten normal batches; each phase also includes two
separately recorded diagnostic batches. Both campaigns used the same physical
tag, four phases and 768 total nonce observations. BLE batch time fell 31.3%;
its remaining difference from the adjacent USB medians is approximately
14–16%. Radio diagnostic timings did not explain the original transport gap.
No nonce batch size or hardnested algorithm was changed.

A same-tag comparison then supplied the second tag's privately recovered keys
as an ordinary user dictionary. It did not import trusted credentials or skip
authentication. Three full reads per transport used the same installed firmware
and identical card/memory bytes; the legacy compiled frontend was routed for
both USB and BLE. The refined frontend is `c76d5e4` (firmware changes are the
same as `fb33af9`):

| Frontend | USB median (three samples) | BLE median (three samples) |
| --- | ---: | ---: |
| Legacy | 11,153.1 ms (11,103.3–11,228.1) | 44,743.2 ms (44,122.1–44,818.4) |
| Refined | 10,650.3 ms (10,581.6–10,682.5) | 25,292.2 ms (25,167.3–25,298.3) |

BLE elapsed time fell 43.5%, with USB slightly faster. The tag has many distinct
keys, hence many rejected reuse checks and more round trips than the first
tag. These figures must not be presented as repeated versions of its earlier
10 s / 5.5 s experiment. The intermediate optimization achieved 24,077.4 ms BLE
but regressed USB to 15,509.1 ms; it was refined to preserve the fast sweep
sessions rather than shipping that regression. Concurrent browser regressions
were running during both same-tag frontend campaigns; these are descriptive
measurements, not an unloaded-device timing guarantee.

A full cold recovery through the actual production UI then recovered all
64 blocks and 32 keys, including two keys labelled `Hardnested`, over both
USB and native Chromium BLE. Exported card/memory bytes match. Elapsed times
were 62,345.5 ms USB and 79,869.3 ms BLE. These totals include module loading,
dictionary work, variable acquisition and browser computation: they are not a
fixed-size throughput comparison. The first acquisition consumed 4,096
observations on both transports; the second consumed 1,616 USB versus 1,472
BLE, so comparing total acquisition time directly would mix different work.
The GATT trace has no framing errors and no HTTP device fallback occurred.
All slots, settings, active slot and reader state were preserved. No PM3 or
physical tag write was used. Raw captures, recovered keys and nonces remain
in the private ignored cache, outside the hosted distribution.

The refined implementation was then built cleanly from `92b069a` and flashed
as `v1.0.0-80-g92b069a`. Its application-only ZIP has SHA-256
`252aac9b0894a7e1d0fb07c51ea361bdb96a8ac56e7fe8bed8a64653baf829c3`.
Fresh backups before and after this flash match all eight metadata records,
all three populated images byte for byte, settings, reader state and active
slot 2. This restart retained the selection without an additional activation.
Actual USB HTML, JavaScript and CSS match the build; the USB application module
is byte-identical to the hosted module and to the refined benchmark override.
A second complete cold recovery on this installed version again recovered
all 64 blocks and 32 keys, including two hardnested keys, with identical USB/BLE
memory, no GATT framing errors and complete device-state preservation. Totals
were 58,988.0 ms USB and 84,053.5 ms BLE. The acquisitions used 4,096 + 1,728
observations over USB and 4,096 + 1,488 over BLE; software browser regressions
were running concurrently. These variable cold totals qualify recovery,
not a fixed-work speed ratio. Evidence: `refined-installed/`,
`refined-hardnested-full/`, `refined-hardnested-summary.json`.

Settings validation exposed a separate stale-display race: a device refresh
started before a settings write could be reused after it, repainting the old
value. `c76d5e4` invalidates that refresh and keeps the edited input disabled
through the fresh confirmation. A deterministic regression fails against the
old compiled bundle with value 0 instead of 1 and passes in both final browser
engines. This does not change settings wire values or replay failed saves. The physical
campaign also exposed overlapping numeric-field changes while the confirmation
was pending. A subsequent guard disables all settings fields through the fresh
read, survives rerenders and ignores duplicate change events. The native GATT
campaign then passed with exactly 15 expected settings requests (seven changes
and restores, plus one occupied-device rejection), no pending requests or
framing errors, and original values restored. Two targeted regressions pass in
both Chromium and WebKit; the simulated fixture now resets its settings as well
as its memory between tests.

Validation scope: 327 JavaScript tests, 212 firmware/tools tests, 340 complete
browser cases on `92b069a` (170 Chromium + 170 WebKit), 12 hosted cases and
12 mobile WebKit cases passed. The subsequent settings-only guard passes four
additional targeted browser cases and the actual BLE settings campaign.
On `92b069a`, the other five native campaigns passed: basic connection/slot
read, Stop during hardnested with partial observations retained, measured
512-byte reads and queued requests, all 25 slot formats including interrupted
large staging, and tab ownership/reconnection. The initial failed settings
campaign and rejected USB performance variant remain in the private evidence;
they are not counted as passing runs. No PM3 or physical tag write was used.

Final delivery uses clean source `6d9c8a8`, application
`v1.0.0-82-g6d9c8a8`, and shell
`ce83ca8b66ae4eab58b12b9d8398c32f111aa721d78a7b7d8b45faf73608cc51`.
The application-only ZIP SHA-256 is
`b55a6b52cda719988e0d5b37e3c7d1a153e87df520b75e119298db68fbf491c5`.
Its actual USB application bytes match the final hosted module and the
settings-guard candidate tested against the real device. Fresh complete
backups retain all slots/images, settings, reader state and active slot 2.
On this installed version, basic BLE slot/document access, the complete
settings campaign (exactly 15 writes, including the intentional busy rejection)
and Stop during actual hardnested acquisition all pass. Stop settled in
240.7 ms, retaining eight known blocks and releasing the device. All three
campaigns preserve device state; none writes the physical tag or uses a PM3.
The final compiled hosted package also passes all 12 Chromium/WebKit cases.
The local source server was restarted on `http://127.0.0.1:8080` and verified
against the real USB device, separately from the simulated browser fixtures.
The temporary preview publishes only compiled assets and version metadata;
its historical source ZIP is unchanged. No development-repository push occurred.
Evidence: `final-installed/`, `final-hardware/`, `final-hosted-tests.log`,
`public-live-hashes.json` and `public-browser.json`. The real public URL
passes Ready offline, export of 128 Classic 2K blocks with edited block 126,
offline reopening and Device navigation at 320 px, with zero API requests
or page errors. This public-origin check does not establish native Bluefy BLE.

Evidence: `.cache/ble-read-speed-20261107/` (`baseline-ui/`,
`without-response-ui/`, `capture-legacy-v2/`, `capture-optimized/`,
`hardnested-full-optimized/`, `known-key-legacy/`, `known-key-refined/`,
`installed/preservation.json`). Reproducible
opt-in benches are `classic-read-speed.spec.mjs` and
`classic-capture-speed.spec.mjs` under `software/web/tests/hardware/`.
A private exported capture can seed a normal dictionary via
`CHAMELEON_BLE_TEST_CLASSIC_SPEED_DICTIONARY_CAPTURE`; it never injects trusted
key mappings or skips physical authentication. The optional legacy bundle
comparison changes the frontend, retaining the same installed firmware.

## USB startup while NFC-V emulation is selected (2026-11-07)

A first load on the USB origin regressed with the active NFC-V slot. Three
fresh Chromium contexts on `v1.0.0-82-g6d9c8a8` reached an enabled Scan button
and confirmed USB connection in 6,143.0 / 5,932.0 / 6,248.7 ms. The JavaScript
transfer alone took 5,030–5,372 ms. A separate mode control transferred the
same 74,734 compressed bytes in 156–157 ms during a temporary reader lease,
versus 2,488–3,932 ms with emulation before/after. The lease was ended and the
original mode restored; no RF command or physical tag write was issued.

`5ca4ad4` makes queued HTTP dispatch, response bytes, unsent/unacknowledged TCP
segments and immediate ACKs visible to the read-only USB pending-work check.
NFC-V avoids requesting another field-free RF timeslot while this work remains.
A real detected field still runs the existing radio path, and an acquired RF
exchange retains its atomic handling. Idle preconnected HTTP sockets do not
count as work. The timed receiver, waveform, CRC rules, BLE protocol and web
JavaScript are unchanged. The quiet-window regression fails without the guard;
real-lwIP tests cover pending dispatch, send backpressure, unacknowledged data,
ACKs, idle sockets and disconnect cleanup. The complete firmware/tools suite
runs 213 tests: 212 pass and the optional public-release-download test is skipped.

Application-only firmware `v1.0.0-84-g5ca4ad4` is installed. Its ZIP SHA-256 is
`8a33a69fbacee9a6cc363fc16795c672ec8ebe870bf8cd1d395c8de107d69e63`.
Fresh complete backups match all eight slot metadata records, all three images
byte for byte, settings, reader mode and active slot 2; no selection restoration
was needed. The actual served JavaScript retains SHA-256
`a50b92fa7885b6af1b00746c5a808c408a5ba2ef1545383532f4ec741e8f4130`,
identical to the preceding firmware and current public preview. The public
frontend does not need a replacement for this firmware-only correction.

With the same active slot and three new empty browser contexts, readiness is
291.2 / 313.1 / 327.1 ms: a 313.1 ms median versus 6,143.0 ms before. The
JavaScript transfer takes 185–187 ms. These are physical USB cold-interface
measurements, with no saved draft and no device mutations, not a measurement
of recovery resource preparation or arbitrary Internet availability. NFC-V RF
behavior under simultaneous HTTP load remains scoped to software regression
evidence; no external reader or PM3 was used in this correction's campaign.
Private evidence: `.cache/usb-startup-20261107/` (`comparison.json`,
`browser-before.json`, `browser-after.json`, `baseline-mode-control.json`,
`host-tests.log`, `quiet-window-before.log`, `installed/preservation.json`).

## Offline package

The standalone app saves its complete interface in a versioned service-worker
cache. Each file's size and SHA-256 must match before installation succeeds.
WASM and tables retain their existing content-checked IndexedDB storage.
**Ready offline** requires both caches. An evicted shell entry removes that
status; **Retry** reinstalls the complete shell. A new shell version waits until
old app windows close, preserving a coherent version during device operations.
See the [service-worker lifecycle](https://w3c.github.io/ServiceWorker/).

The service worker caches only the six listed interface resources, never `/api`
responses or device state. It supplies no permanent BLE connection in the
background. Keep the app foregrounded during device operations.

Documents and dictionaries stay in the browser origin that created them.
Moving from the USB address or localhost to an HTTPS site requires explicit
export/import. A service worker does not migrate or back up those documents.

## Automated bench

```sh
npm test --prefix software/web
npm run check --prefix software/web
python3 -m unittest discover -s resource/tools -p test_web_contracts.py
python3 -m unittest discover -s resource/tools -p test_hosted_web.py
npm run test:hosted --prefix software/web
npm run test:mobile --prefix software/web
python3 -m unittest discover -s software/web/tests/hardware -p 'test_pm3_*.py'
npm run test:hardware:ble --prefix software/web
```

The last command is an explicit physical-hardware test. It runs Chromium against
the local dev server on port 8083 and selects a unique real `ChameleonUltra`
advertising Nordic UART. It does **not** enable BluetoothEmulation. With multiple
matching devices, it leaves the chooser unresolved rather than selecting one at
random. Enable macOS Bluetooth access for Chromium during initial bench setup.

The default hardware scenario reads the device, lists all slots and opens slot 1
in Studio while HTTP device requests are blocked. It checks chip identity,
settings, mode and active slot before/after, then disconnects cleanly. Hardware
artifacts are isolated in `software/web/test-results-hardware/` so parallel
software tests cannot delete their traces.
Each hardware run also writes `test-results-hardware/report.json`, including the
attached device identities, measured outcomes and restoration evidence. Archive
this directory before the next hardware invocation, which replaces the previous run.

The `test:mobile` suite serves the same prefixed package with WebKit, touch
input and iPhone/320px viewports, retaining artifacts in `test-results-mobile/`.
All 12 mobile cases and 12 desktop hosted cases pass on `ac7e5ed`, including
the navigation, fresh 2K ATS and known-model corrections.
It exercises offline reopening/cache eviction and actual file download/import
round trips for Classic 2K/4K and NTAG216, including their final user data. Only
the optional empty ATS/warnings fields materialized during JSON import are
normalized in the comparison; memory, keys and emulator data must match.
Native iOS BLE and the OS file picker are not emulated by this evidence. The
browser-enabled CI job is configured to run this suite; its software PM3
stop/recovery/report tests also run in the host checks without physical hardware.
Failed browser artifacts are retained for seven days by the configured CI job.
The initial mobile run exposed a real 320px navigation failure: after offline
reopening, the sticky document action bar intercepted clicks on **Device**.
The same scenario fails before and passes after placing navigation above that
bar. This CSS correction is included in temporary preview `24472b8`.

The second read-only scenario, `tabs.spec.mjs`, opens two real Chromium tabs.
It rejects the second BLE owner without interrupting the first, transfers
ownership after explicit disconnect, then reconnects the first after the
second tab closes. The same lock name and distinct browser client IDs are
recorded, with HTTP fallback blocked. Chooser actions bring the selected tab
to the foreground. This passed against the prefixed standalone package.
The lock coordinates tabs of the same origin and browser storage partition;
keep one active BLE client across other origins, profiles and applications.
See the [Web Locks scope specification](https://www.w3.org/TR/web-locks/#scope).

A read-only transport benchmark is available independently of the PM3:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_PERFORMANCE=1 \
npm run test:hardware:ble --prefix software/web -- performance.spec.mjs
```

Both transports run the app's real request/slot modules in the same Chromium.
USB loads the document from the actual Ultra address, preserving Chromium's
local network classification; BLE uses the secure loopback origin, its physical
chooser and a hosted marker. No Bluetooth emulation is enabled. HTTP device
requests from the BLE page are blocked. Independent USB snapshots supply the
expected bytes and revisions for every nonempty HF/LF image and all eight slot
metadata records.

Each transport performs 12 sequential device reads, three groups of eight
concurrent device requests, 24 metadata reads and six complete transfers of
each image. Stable device fields and every slot byte/revision are checked.
The groups exercise the real BLE queue;
their timings include time waiting behind earlier requests. Final device state,
all images and eight metadata records must match. The attached JSON retains
every sample, fixture hashes, connection timing and preservation evidence.

One physical run on macOS Chromium, USB-powered firmware
`v1.0.0-28-g76f8159`, measured these medians:

| Operation | Samples per transport | USB | BLE |
| --- | ---: | ---: | ---: |
| Device information | 12 | 181.7 ms | 120.3 ms |
| Slot metadata | 24 | 173.5 ms | 120.2 ms |
| 512-byte slot read | 12 | 180.1 ms | 151.7 ms |
| Complete Classic 1K image, 1047 bytes | 6 | 680.3 ms | 555.0 ms |
| Complete SLIX-L image, 117 bytes | 6 | 350.3 ms | 240.4 ms |
| Complete ST25TB image, 82 bytes | 6 | 321.0 ms | 240.1 ms |

The 512-byte read samples yielded 2771 payload bytes/s over USB and 3293 over
BLE, excluding slot-open overhead. All 24 grouped requests matched; the slowest
BLE result including queue wait was 1107 ms. BLE connection including the
automated chooser took 1570 ms. These are descriptive measurements of this
small read workload, not a general speed guarantee or qualification of RF
timing, writes, battery operation, other OS/browser combinations or larger images.

An additional destructive-to-the-test-slot-only campaign is explicitly opt-in:

```sh
cd software/web
CHAMELEON_BLE_TEST_EMPTY_SLOT_8=1 npm run test:hardware:ble
```

Reserve **empty slot 8** for this campaign. It refuses a nonempty or active slot
8 and requires Classic 1K, SLIX-L and ST25TB512 images in slots 1–3 as read-only
fixtures. It writes copies to slot 8, verifies readback, UTF-8 names, enable,
stale revisions, cross-frequency preservation and USB owner rejection. It then
clears slot 8, abandons a partial upload, reconnects and checks lease invalidation.
Cleanup can use the independent USB path after a broken radio link. No physical
RF tag is written. Keep a full device backup and the USB recovery path available.

A separate format/boundary campaign uses the same reserved empty slot 8:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_SLOT_MATRIX=1 \
npm run test:hardware:ble --prefix software/web -- slot-matrix.spec.mjs
```

It refuses an active/nonempty test slot and generates synthetic records with
the app codecs and shared WASM geometry. It saves and reads all **25 storable
formats** over physical BLE: four Classic, nine Type 2, SLIX/SLIX-L, ST25TB and
DESFire, plus eight LF formats. Every save has an independent USB wire readback.
An LF sentinel remains throughout the HF formats; the 4497-byte DESFire HF
record remains throughout the LF formats. The other frequency's complete bytes,
type, name, enabled state and revision must remain unchanged. All test records
remain disabled and slot 8 is never selected for emulation.

The fixtures include 4/7/10-byte UID fields, a synthetic 255-byte ATS field,
Type 2 options/counters/version/signature cells, NFC-V locks/secrets and the
application's LF field encoders. The Classic 4K fixture is 4374 bytes on wire;
the maximum aligned DESFire record is 4497 bytes. The ATS case exercises the
storage field limit, not a valid RF activation or Classic coexistence claim.

After the 26 saves (25 unique formats, including the LF sentinel), the test
stages 2000 of 4374 bytes for a replacement HF image and disconnects without
commit or END. Independent USB snapshots must still match both persisted
records. Reconnection must reject the expired lease with HTTP-equivalent 409.
The test then clears both frequencies through BLE and compares every original
slot image, all eight metadata records, mode, settings and active slot. Failure
cleanup can clear recognized test records through USB; it refuses to erase an
unexpected name.

This passed on the physical Ultra with `v1.0.0-28-g76f8159` in about 2.4 minutes,
with no HTTP fallback or fallback erase. The 37 committed upload chunks carried
at most 1002 body bytes (1000 data + two-byte offset); two further chunks were
abandoned before commit. The large Classic and DESFire save/readback operations
took 16.47 s and 17.37 s respectively on this run. This qualifies storage,
transfer and cleanup for these records, not RF reading/writing/emulation of
each format, battery operation or other platforms. No PM3 is used.

To qualify reversible device settings through the real UI, enable a separate
opt-in campaign:

```sh
CHAMELEON_BLE_TEST_SETTINGS=1 npm run test:hardware:ble --prefix software/web -- settings.spec.mjs
```

It changes and restores animation, the four button actions, sleep timeout and
long-press threshold over BLE. Independent USB reads verify each value and
that all other settings remain unchanged. A USB reader reservation must reject
the BLE settings save. Cleanup restores remaining changes through USB if the
browser fails, then compares all eight slot metadata records and device state.
Pairing and PIN are untouched. This campaign passed on the physical Ultra;
pairing dialogs require separate qualification.

Persistence across reboot has its own opt-in campaign:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_SETTINGS_REBOOT=1 npm run test:hardware:ble --prefix software/web -- settings-reboot.spec.mjs
```

It requires USB networking and an installed layout-2 recovery. It changes all
seven non-security settings through the real BLE UI, disconnects explicitly,
enters USB recovery and reboots the existing application. It verifies the
modified settings through USB and the reconnected BLE UI, restores the original
values through BLE, then repeats the reboot and checks the restored values.
All eight slot metadata records and SHA-256 hashes of every nonempty HF/LF
image must match before/after. Device identity, firmware, mode and active slot
must also match. Failure cleanup can restore settings through independent USB.

This passed on the physical Ultra from the prefixed standalone package. Both
recovery visits reported a valid application and no update session, received
bytes or image size; no firmware update is begun. Pairing and PIN remain
unchanged. The attached JSON records the two actual recovery/application
transitions and complete preservation evidence. This verifies persistent
settings after software reset with USB power, not a battery power cycle.

An empty-field reader campaign compares physical USB/BLE error paths:

```sh
CHAMELEON_BLE_TEST_NO_TAG=1 npm run test:hardware:ble --prefix software/web -- reader.spec.mjs
```

Remove tags and stop PM3 simulations first. It requires the expected no-tag
status for NFC-A, NFC-B, NFC-V, EM410x, Viking, HID Prox, ioProx, PAC and
Jablotron over USB, then compares the real BLE outcomes. It also rejects USB
access to the BLE reader lease and deliberately closes the tab without END:
the firmware must release the reservation and restore the initial mode,
settings and all slot metadata. This passed on the physical Ultra. It qualifies
no-tag outcomes and disconnect cleanup, not successful RF decoding or writes.

The same campaign starts a tear-off job against a synthetic ST25TB UID while
the field is empty, observes RUNNING, rejects another RF command, then cancels
the job. USB and BLE must report the same final outcome, zero writes and
successful radio cleanup. Cancellation was observed in 240 ms over BLE on the
physical bench (including state polling). This does not measure RF pulse
timings or cancellation during writes to a real counter.
It then restarts the empty-field job and closes the tab while RUNNING, without
CANCEL or END. A fresh USB reservation must recover its DONE/CANCELLED state,
zero writes and successful cleanup; device and slot state must be restored.

Browser regressions use port 8080 by default. Set `CHAMELEON_WEB_TEST_PORT=8084`
when another developer already owns that port. Hosted-package tests use 8085
and exercise a nested repository prefix in Chromium and WebKit.
To repeat the read-only physical scenario against this standalone package, use
`CHAMELEON_BLE_TEST_HOSTED=1 npm run test:hardware:ble -- ble.spec.mjs` from
`software/web`; it also uses port 8085.

A positive NFC-A reader campaign is also available with the aligned PM3 HF
antenna. First verify the software stop paths on this physical fixture:

```sh
python3 software/web/tests/hardware/pm3-simulation-safety.py \
  /absolute/path/to/proxmark3 /dev/cu.usbmodemiceman1 .cache/pm3-stop-checks
```

This preflight starts a synthetic NFC-A simulation and independently tests
normal owner EOF, owner SIGKILL, a forced five-second deadline and restoration
of changed emulator RAM after owner SIGKILL. Every case
requires a successful Ultra USB scan before stopping, then a PM3 ping with
matching payload and no NFC-A tag after stopping. All four passed physically;
normal and killed-owner cleanup took about 3.4 seconds on this bench, or about
7.4 seconds including the RAM restore and independent readback.

The simulation helper arms a detached stop supervisor **before** starting the
serial-owning PM3 client. A launch gate prevents that client from opening USB
until its PID is registered with the supervisor; losing the gate pipe before
registration exits without launching the client. Losing the owner's pipe or
reaching the hard deadline terminates that client to release the serial port,
always sends **hw break**, then verifies **hw ping** content. It does not infer firmware shutdown from CLI
exit or an Enter key: some PM3 modes can leave simulation running after their
client exits. The maximum lifetime is 180 seconds; the preflight shortens it to
five seconds for its deadline case. Cleanup JSON and raw break/ping logs record
the result. This requires an active host and an accessible USB device.
The helper accepts the stop-qualified `hf 14a sim` family and the exact
guarded Classic 1K mode described below. Any additional simulation mode needs
its own stop preflight before being added.

The full Classic simulation (`hf mf sim`) failed that preflight on this PM3.
After the simulation client was killed, a new `hw break` client stalled while
opening/configuring USB. A targeted USB device reset recovered one occurrence,
but the repeat lost the serial interface; hub port reset/power commands did not
restore it, and the Ultra still scanned synthetic UID `11223344`. The PM3 was
subsequently physically reconnected and recovered as verified below. The
previous unguarded Classic launch remains refused. The held-descriptor
preflight below subsequently qualified one exact Classic mode; the incident
remains recorded and does not add a product exclusion or qualify a memory read.
The failed runs and original 4096-byte RAM backups are retained locally.
They required physical PM3 reconnection and verified RAM restoration before
resuming RF tests; both recovery steps subsequently passed as recorded below.

After physical reconnection, `pm3-recovery.py` provides a bounded recovery
workflow. Prepare an explicit manifest with absolute client/backup/port paths,
the original 4096-byte RAM SHA-256 and the expected Ultra chip identity:

```json
{
  "version": 1,
  "pm3Client": "/absolute/path/to/proxmark3",
  "pm3Port": "/dev/cu.usbmodemiceman1",
  "ramBackup": "/absolute/path/to/ram-before.bin",
  "ramSha256": "<exact SHA-256 of the original 4096 bytes>",
  "ultraURL": "http://192.168.16.1",
  "ultraChip": "<expected Ultra chip identity>"
}
```

```sh
python3 software/web/tests/hardware/pm3-recovery.py recovery.json .cache/pm3-recoveries
python3 software/web/tests/hardware/test_pm3_recovery.py -v
```

It refuses a missing port or mismatched backup before any device request.
After confirming the expected idle Ultra, it sends `hw break`, requires actual
ping content, saves the current PM3 RAM to a separate backup, restores the
frozen original if needed and independently reads all 4096 bytes again. It then
requires no NFC-A tag from the Ultra, releases the reader lease and compares
device state and all eight slot metadata records. Every CLI call has a deadline;
an unknown RAM-load outcome is not retried. Phase logs and terminal JSON retain
failures; only the complete sequence can report `ready: true`. The workflow
never launches a simulation or flashes firmware, and readiness does not enable
Classic simulation.

Nine software tests cover successful restoration, already-matching RAM, bad
backup hash, missing port, wrong Ultra, unconfirmed ping, remaining RF identity,
unknown load outcome and changed Ultra state. Two physical runs subsequently
passed: one verified RAM already matching the original, and another backed up
deliberately changed RAM, restored the original and verified all 4096 bytes.
Both required an actual ping, no NFC-A tag and preserved Ultra state/metadata.

A persistent stop channel now opens and verifies its serial descriptor
before simulation and keeps it in the detached supervisor. Its stop path sends
`CMD_BREAK_LOOP` and requires a fresh matching ping through that existing
descriptor, without reopening USB or changing termios. It remains disabled by default;
`CHAMELEON_PM3_TEST_PERSISTENT_STOP=1` is a diagnostic opt-in for the
`hf 14a sim` family. This variable alone does not enable Classic simulation. Add it
to the physical preflight command to require `persistentStopPingConfirmed`;
successful legacy fallback alone cannot qualify this channel.

```sh
python3 software/web/tests/hardware/test_pm3_stop_channel.py -v
```

Fourteen software tests pass with PTY protocol fixtures, including actual owner
SIGKILL while the detached supervisor retains its descriptor, fragmented/noisy
responses, wrong ping data, failed status, invalid trailers, closed channels,
rejection of unqualified Classic commands before launch and preservation of
timeout diagnostics. The physical PM3 returned successful pings with reason
`FF`; a regression built from that captured frame failed before the parser fix
and passes afterward. BREAK and PING are separated by 50 ms to keep them in
distinct CDC transfers: the initial grouped exchange stopped emulation but
did not deliver the expected ping.

The four physical `hf 14a sim` fault cases then passed with the held descriptor:
normal EOF, owner SIGKILL, forced deadline and RAM restoration after SIGKILL.
Every case confirmed the direct ping, independent CLI ping and no NFC-A tag.
Cleanup took about 3.2 seconds without RAM restore and 7.0 seconds with it.
This qualifies the held channel for that family. The separate Classic stop
campaign below also passed; other modes remain unqualified.
Both real BLE NTAG213 scenarios (memory capture and write/cancel/stale-capture
checks) also passed again with this channel enabled, requiring the direct stop
ping, full PM3 RAM restoration and no remaining NFC-A tag. A final full Ultra
backup matched the pre-campaign images, metadata and settings.

The exact Classic 1K command `hf mf sim --1k -u 11223344 -i` then passed
its own four physical fault cases: normal EOF, owner SIGKILL, forced five-second
deadline and modified-RAM restoration after SIGKILL. Every case required a
fresh direct ping on the held descriptor, an independent CLI ping, exact
4096-byte RAM restoration and no remaining tag in a fresh Ultra USB scan.
Stop plus restoration and verification took about 15.2 seconds per case.
The first attempt with a five-second direct-ping timeout required legacy
fallback; it safely stopped and restored RAM but did **not** qualify. The
Classic direct-ping budget is now 15 seconds, with the success assertions intact.

```sh
python3 software/web/tests/hardware/pm3-simulation-safety.py \
  /absolute/path/to/proxmark3 /dev/cu.usbmodemiceman1 .cache/pm3-stop-checks classic
```

The `classic` preflight selects both explicit guard flags and backs up all RAM
for every case. The normal Python launcher still requires
`CHAMELEON_PM3_TEST_CLASSIC_STOP=1`,
`CHAMELEON_PM3_TEST_PERSISTENT_STOP=1` and
`CHAMELEON_PM3_SIM_RESTORE_BIN` for this exact command form with an eight-digit
hex UID. Missing interactive mode and attack flags are refused before launch.
Mini, 2K and 4K each require their own stop report or bounded preflight.
The JS helper's `{ restoreBin, classicStop: true }`
selects these guards and also rejects cleanup without the direct ping or RAM
restoration. This qualification applies to this PM3 firmware/client/USB setup;
it does not authorize arbitrary simulations or guarantee recovery if the host
loses power or USB disappears.

Mini and 4K have now passed their four physical stop fault cases and the
read-only USB/BLE UI comparison on Ultra `4c011bf`. A new setup must first run
the explicit stop preflight, with the same
held descriptor/full RAM backup and a maximum 30-second simulation lifetime
(five seconds in the deadline case):

```sh
python3 software/web/tests/hardware/pm3-simulation-safety.py \
  /absolute/path/to/proxmark3 /dev/cu.usbmodemiceman1 .cache/pm3-stop-checks classic-mini
python3 software/web/tests/hardware/pm3-simulation-safety.py \
  /absolute/path/to/proxmark3 /dev/cu.usbmodemiceman1 .cache/pm3-stop-checks classic-2k
python3 software/web/tests/hardware/pm3-simulation-safety.py \
  /absolute/path/to/proxmark3 /dev/cu.usbmodemiceman1 .cache/pm3-stop-checks classic-4k
```

Routine launches require the completed `results.json` for their exact command,
client binary SHA-256, serial port and reported PM3 firmware identity. The gate requires all four distinct
fault cases, direct stop ping, original RAM hash/restoration, positive identity,
post-stop absence and preserved Ultra state. Missing, fallback-only or
other-size reports fail before opening serial. A structurally valid report is
then compared against a bounded read-only `hw version` query before any Ultra
USB request or emulator RAM load. The Python launcher repeats that identity
check immediately before arming its supervisor. The report requires identical
Model/ARM/FPGA/Hardware sections before and after qualification, plus their
fingerprint after each stopped fault case. Raw preflight version logs and the
UI's `pm3-identity-check.json` preserve this evidence. Missing version replies,
timeouts or changes fail without a retry or simulation launch.

The same software regression accepts a stale report with the old launcher and
rejects it with the new live check. All 41 PM3 software tests pass, including a
CLI round trip with a synthetic executable that accepts only a fake serial
port and `hw version`. The parser also accepts the historical physical version
log; that is not a fresh hardware check. These are reported version strings,
not a readback/hash of the flashed binary, a unique board identifier or proof
of antenna alignment. Repeat the physical preflight after changing PM3 firmware,
client or USB/RF setup even if the reported strings are unchanged. Existing 1K
physical evidence retains its original scope; the new automatic identity gate
applies to the Mini/4K campaigns below.

The JS launcher clears the experimental preflight flag and
passes the normal report explicitly. These preflights were held during the manual
iPhone trial. After hardware control returned, the first Mini preflight on
`4c011bf` failed its initial positive USB identity check (`0001`); its direct stop
ping and complete RAM restoration succeeded. A separate three-scan control of
the previously qualified 1K mode also found no NFC-A tag, while the physical SLIX
remained readable. Neither failure qualifies Mini or establishes a Mini-specific
cause. The final backup confirmed all Ultra slot images/metadata and settings
unchanged. A reverse-direction PM3 reader subsequently detected the Ultra's
emulated NFC-A identity. After an application reboot, six 1K control scans
succeeded, but the real SLIX was no longer detected. Movement of the physical
setup was also reported, so this recovery does not establish a firmware cause
or prove positive SLIX-to-NFC-A handover.

Separate Mini and 4K preflights then passed all four fault cases each: positive
USB identity, direct held-descriptor ping, independent post-stop absence,
reported PM3 identity and exact full RAM restoration. The read-only Mini UI
campaign passed with all 20 blocks and five key pairs equal to USB. The first
4K UI attempt lost the card during sector-5 authentication; its GATT trace had
no framing error or pending response, and autonomous cleanup restored RAM.
Keep this failure in the evidence. After the user reported the Ultra back in
place, the separate 4K rerun passed: all 256 blocks, 40 key pairs, observed
identity and export matched the independent USB reference. Its trace contains
268 operations, no framing errors and no pending response. Both campaigns
preserved Ultra slots, settings and active slot, stopped the simulation and
restored all 4096 PM3 RAM bytes. These successes qualify these fixtures and
read paths, not general RF reliability or physical EEPROM writes.

Local evidence: `.cache/web-ble-study-20261106/pm3-stop-mini-after-reset-4c011bf/`,
`pm3-stop-4k-4c011bf/`, `reader-mini-4k-first-4c011bf/` (including the failed
4K attempt) and `reader-4k-repositioned-4c011bf/` under the same cache root.

The opt-in read-only UI cases check all 20 Mini or 256 4K blocks and all sector
keys against independent USB reads. Each report is checked before any USB
request or emulator RAM load:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_CLASSIC_SIZES=1 \
CHAMELEON_PM3_CLASSIC_MINI_STOP_REPORT=/absolute/path/to/mini/results.json \
CHAMELEON_PM3_CLASSIC_2K_STOP_REPORT=/absolute/path/to/2k/results.json \
CHAMELEON_PM3_CLASSIC_4K_STOP_REPORT=/absolute/path/to/4k/results.json \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic.spec.mjs --grep 'after stop qualification'
```

Software checks compare fixture geometry with production WASM, including the
16-block sectors from block 128 through trailer 255. They preserve the exact
SHA-256 of the previously RF-tested 1K fixture. The production reader's final
4K-sector decoding is also checked with full `FFFF` and partial `8001` masks,
so unread middle blocks remain unknown. These checks are not RF evidence.
The matching PM3's `--2k` mode advertises SAK `08` with RATS support. Loading
128 blocks into RAM alone cannot establish a 128-block UI read. Fresh-scan
recognition now has an optional ATS evidence path, described below, qualified
in a physical read-only 2K campaign. It is not added to the accepted
NFC-V/NFC-B emulation exclusions.

Rereading an **existing** Classic document now carries its known model into the
recovery engine, after verifying the same UID, SAK and ATQA. A known 2K image
therefore retains 128 blocks and 32 sectors even with observed SAK `08`; an
internal geometry selector never changes the observed card identity. An invalid
model or changed identity is refused before key/memory operations or replacement
of the open document. A missing upper block remains unknown, prevents a complete
dump and keeps the model at 2K. Production-WASM regressions reproduce the old
64-block truncation, verify complete and partial 2K reads and check identity
mismatches. Chromium/WebKit tests exercise the actual Read/Recover/export/reload
path through block 127 and preserve all edited data on a mismatched 4K reread.
All 292 JS tests pass. The full 320 Chromium/WebKit browser cases passed before
the follow-up ATS retention correction, including the new ATS evidence cases
below. The earlier 12 hosted
and 12 mobile cases include 2K/4K/NTAG216 offline export/import with the final
user data preserved. These are software checks, not physical 2K RF qualification.

The WASM geometry selector `16` is not proof of a physical SAK `10`.
[NXP AN10833, Figure 1](https://www.nxp.com/docs/en/application-note/AN10833.pdf)
distinguishes Plus 2K in SL1 under SAK `08` from SL2 under `10`, and uses
RATS/ATS or GetVersion for further identification. The browser now requests
optional evidence in Classic inspection (`2040`): a trailing request byte `1`
extends the unchanged 11-byte reply with version `1`, raw ATS length (0–64),
and ATS including CRC-A. The firmware reuses its existing RATS exchange,
checks byte alignment, TL and CRC, and suppresses the complete reply if a later
UID/ATQA/SAK check fails. Legacy requests keep their original response and RF
probe sequence. A firmware rejecting this optional request with parameter
status `0060` receives one legacy read-only request; malformed replies,
transport errors and cancellation do not trigger that negotiation.

The browser independently validates TL and CRC-A. Only the known Plus S/X SL1
historical-byte patterns, SAK `08` and matching ATQA/UID layout select 128
blocks and 32 sectors. The observed SAK stays `08`; the actual ATS is retained
without its CRC. Unknown ATS, Magic Gen2 signatures, Plus SE/EV1 patterns and
different identities do not establish 2K. Modern Plus GetVersion recognition
is not implemented. The scan leaves memory unknown until **Read** is chosen.
Host tests compare the extended and legacy RF probe counts and reject invalid
requests before RF. A Chromium/WebKit scenario starts from a fresh 2K scan,
reads all 128 blocks and exports the upper data without mutations. A follow-up
regression exposed ATS loss when **Read** performed basic selection, which
does not read ATS. After matching UID/SAK/ATQA, recovery now retains the open
model's ATS if selection supplies none; any newly observed ATS takes precedence.
Both production-WASM cases failed before this correction and pass after it,
along with four targeted Chromium/WebKit scenarios. The retained ATS is prior
model metadata, not a fresh RATS exchange during memory recovery.

The extended firmware `71549de` was installed for the first qualification. A fresh backup and
post-update comparison verified all eight slot metadata records, all populated
images, settings and active slot unchanged. Real Mini and 4K reader evidence against
`4c011bf` also exercises rejection of the optional format and successful legacy
negotiation over GATT.

The new bounded `classic-2k` preflight passed all four physical fault cases on
`71549de`, with exact SAK `08` identity, direct stop ping, post-stop absence,
unchanged reported PM3 firmware and full 4096-byte RAM restoration. Normal
launch still requires that exact command's completed report. A 1K report cannot
substitute despite sharing the same SAK/ATQA; SAK `10` is not accepted as this
fixture's identity. All 41 software PM3 tests pass, including refusal before
process launch and the 30-second preflight ceiling. The existing read-only 1K
USB/BLE UI campaign passed on `71549de`.

The separate physical 2K UI campaign starts from a fresh scan, recognizes
**Classic 2K** with all memory unknown, then reads and exports all 128 blocks
and 32 distinct key pairs identically to independent USB reads. It preserves
ATQA `0004`, SAK `08` and the 12-byte ATS. The first capture passed these data
comparisons, but its final trace snapshot cut a background slot-list request
in flight (operation 2, not an RF command). That harness failure remains in
`reader-1k-2k-first-71549de/`; it is not a lost RF response. The harness now
waits up to three seconds for existing replies before snapshot/close, without
issuing another device command, and always closes the page even on observation
failure. The separate rerun passed with 220 GATT operations, no framing errors
and no pending reply. Both runs stopped the simulation, restored all 4096 PM3
RAM bytes and preserved Ultra slots/settings/active slot. No RF mutation or
HTTP fallback occurred. This proves one PM3 Plus X SL1 fixture with dictionary
keys under USB power; it does not qualify other Plus generations, writes,
physical EEPROM, battery use or general RF reliability.

Local evidence under `.cache/web-ble-study-20261106/`:
`classic-ats-71549de/` (application ZIP hash, pre/post-update preservation),
`pm3-stop-2k-71549de/`, `reader-1k-2k-first-71549de/` and
`reader-2k-settled-71549de/`. The application package SHA-256 is
`47ce2edae05b3afdfd134ed3bbc4118387f3b0bed43187ba9fd02e4040f56c59`.

The final `ac7e5ed` application also embeds the ATS retention correction for
USB users. It was rebuilt from the clean commit, installed application-only,
and compared with a fresh backup: every slot metadata/image, setting and active
slot remained unchanged. The real BLE 2K UI campaign then passed again on
`v1.0.0-54-gac7e5ed`: all 128 blocks and 32 key pairs match USB, SAK and ATS
are retained, and all 217 GATT operations have matched replies with no framing
errors. Autonomous PM3 stop/full RAM restoration and Ultra preservation pass.
Evidence: `ac7e5ed-installed/` and `reader-2k-ac7e5ed/` under the same cache root.
The final application package SHA-256 is
`0e6880a6ae21c8e81fd208816da41b01a5a377d5ac59dbd8973a04c577e3499b`.

On `ac7e5ed`, the model-6 mutation guards still required SAK `10`. A real USB
attempt to write PM3 block 126 with model 6 returned `0001` before mutation;
all 32 sectors had just read correctly. The bounded simulation stopped and
restored all RAM. Evidence: `classic-2k-writes-before-ac7e5ed/` under the same
cache root.

The follow-up implementation accepts SL1 SAK `08` only after a fresh, read-only
RATS identification: correct UID/ATQA layout, TL, CRC-A, ATS interface framing
and known Plus S/X history. It checks the entire UID/ATQA/SAK before RATS and
after a field reset/reselection, restores reader timeout/configuration, and
leaves RF off on failure. It sends no authentication, memory read or WRITE
opcode. This is model evidence, not cryptographic authenticity. Model-6 data,
trailer, guarded block-0 and authenticated maintenance paths use this check;
the existing SAK `10` contract remains intact. Other model paths keep their
existing RF sequence. Production WASM maps selector `16` to 128 blocks but
returns canonical SAK `08`; the selector is not a manufacturer-byte value.
The old C validation demanded `10` even for a canonical SL1 repair. Repair now
keeps an observed valid SAK (`08` or `10`), or uses the production geometry's
canonical `08` when repairing an unreadable known-model identity. Legacy
explicit `10` repair requests remain accepted.

The frontend's format/repair path now carries a matching known 2K model or a
new validated ATS into its 128-block/32-sector geometry. A stale document
cannot authorize a model-6 mutation on a 1K target: the firmware's own model
check runs before writing. Software tests reproduce the former 64-block format
and repair incompatibility, verify cancellation leaves upper data intact, then format and
reread through block 127. The host peer covers bad ATS, changed identity,
other sizes, bounds, stale before-images, upper data/trailer dispatch and
reader configuration restoration. All 212 host tests and 294 JS tests pass;
48 targeted browser cases pass. The full 324-case rerun passed 322 cases; its
two new wrong-capacity cases inspected a hidden success panel instead of the
visible error dialog. The actual UI correctly reported zero writes and offered
**Check again**. After correcting that assertion, both negative cases and both
positive 2K format cases pass. This was a test-selector correction, not a product
retry or a relaxed capacity check.

Compatibility audit caught a separate repair regression in the `bdc0067`
prototype: applying fresh ATS validation to every model-6 maintenance request
also refused a manual known-model card whose SAK itself needed repair. The
frozen prototype reproduces that failure. Normal authenticated SL1 2K
maintenance now appends a required byte `1`: requests are 20 bytes for reads
and 52 for preflight/write. Only method 3/model 6 accepts this extension; bad
flags or other models are rejected before selection. Every checked call
validates fresh ATS, including after write verification. A rejecting older
firmware receives no fallback request or automatic write replay.

The original 19/51-byte maintenance contract keeps explicit known-model repair
semantics. The frontend uses it for manually supplied generation/model when
normal identification cannot establish capacity. These requests do not claim
ATS-backed identification. Host tests repair a malformed SAK to both `08` and
the legacy `10`, preserving configuration; browser tests exercise manual 2K
repair with the actual WASM canonical `08`. An earlier test used a fabricated
geometry SAK `10`; it was replaced with production-WASM evidence, which also
shows that the older repair routine overwrote an observed SL2 `10` with `08`.
The checked wire format was then qualified per unit on application
`v1.0.0-58-g633159a` over a real native BLE connection. On a PM3 Plus X SL1 2K
fixture, a 20-byte read of block 126, 52-byte preflight/write, checked readback,
preflight/restoration and final checked read all passed. A separate 1K fixture
accepted legacy model-1 reads before and after refusing the checked model-6
read with `0001`. All 11 and 7 API exchanges respectively had matching
correlations/checksums and no pending response. Both guarded simulations stopped
autonomously with direct persistent ping confirmation and restored all 4096 RAM
bytes. Evidence: `checked-maintenance-633159a/`.

This is native BLE per-unit qualification against volatile simulator memory;
it does not qualify full formatting in the Web Bluetooth UI, EEPROM writes or
manual recovery of a physically malformed tag. The application-only ZIP has
SHA-256 `55d4df986d73899b3a04f2c7c511d025809654a796712441117b66d4441a4b4a`.
Fresh backups before flashing, afterward and after the RF campaign match all
eight metadata records, all populated images, settings and active slot 0.
All 212 host tests, 294 JS tests, 48 targeted browser cases, 12 hosted checks
and 12 WebKit touch/mobile checks pass for this version. Mobile browser
emulation remains distinct from physical Bluefy qualification.

Application `bdc0067` was rebuilt from a clean commit and installed without
changing slots/settings/active slot. Its real 2K data-write campaign passed:
USB write/readback/restore of block 126, BLE cancellation preserving all 128
blocks, confirmed writes to blocks 1 and 126 with independent full USB checks,
restoration, and refusal of a stale before-image before any further write.
The trace has 231 GATT operations, no framing errors and no pending reply;
the expected stale write returns `0075`. The simulator stops via its held
descriptor and restores all 4096 RAM bytes. This qualifies synthetic PM3 RAM,
not physical EEPROM, block 0 or keys/access writes. Local evidence:
`classic-2k-mutations-first-bdc0067/` and `bdc0067-installed/`; application ZIP
SHA-256 `4d081722487af4a699a6c7435544d4a12045a1ad7239c4bd77903a4d6f166e6f`.

The first sector-31 key/access campaigns did not qualify. The first attempt
stopped during initial USB sector-16 reading (mask `0007`), and a separate
second attempt stopped at sector 26 (mask `0003`), both before a mutation or
BLE connection. The physical 1K capacity-negative control observed `0001` for
all four model-6 paths (`2030`, `2034`, `2038`, `2039`) despite the same
UID/ATQA/SAK. Its initial 64-block reference passed; the subsequent sector-15
read returned mask `0001`, so the complete 1K regression did not qualify.
Both trailer attempts and the negative control stopped and restored the full
RAM. Preserve their failures in `classic-2k-trailers-second-bdc0067/` and
`classic-2k-capacity-negative-first-bdc0067/`, alongside the first run.

A separate read-only diagnostic kept one PM3 2K fixture in place: 64 sector
reads before an application restart were complete; 64 afterward included one
all-unknown sector (mask `0000`); ten sector-31 trailer preflights then returned
`0000`, followed by 64 complete sector reads. Every known byte matches the
fixture. No RF write was sent, all RAM and Ultra state were restored, and a
fresh complete slot backup matched the pre-update state. This does not isolate
the intermittent RF cause or show that reboot resolves it. Evidence:
`reader-state-control-bdc0067/`. Its first HTTP harness attempt omitted the
binary Content-Type and was rejected with 415 before RF; that attempt and its
successful cleanup remain archived separately. Further key/format qualification
requires a reliable RF reference; no failed sample is discarded or retried
inside a mutation. The later 2K trailer campaign uses three independently
bounded simulations: initial capture/cancel/key change, new-key read and
B-only data write/restore, then original-key restoration and final read. Each
transition must stop, ping, restore/read back all 4096 original PM3 bytes,
verify tag disappearance, reload/read back the exact expected synthetic image,
and arm the same qualified command with a fresh 180-second limit. Its USB
sector controls use the optional diagnostic without retries.

That campaign now passes using bench `42300a8`, the published `d5a58d3`
frontend and application `v1.0.0-64-g265ddf2`. It verifies cancellation with
all 128 blocks unchanged, replacement of both sector-31 keys/access bytes,
independent USB authentication with new keys and refusal of old keys, full BLE
read with the new credentials, key-B data write/restore of block 124, original
key/access restoration through the UI, refusal of replacement keys and a final
complete BLE read. The trace has 661 GATT operations, exactly two trailer
mutations and two data mutations, no framing error, pending reply or HTTP
fallback. All 193 USB sector controls are complete; all three simulations
stop normally with ping and full 4096-byte RAM restoration/readback. A fresh
Ultra backup preserves all slots, images, settings and active slot 0. Evidence:
`classic-2k-trailers-split-265ddf2/summary.json` (bench source is `42300a8`,
firmware source is `265ddf2`). This qualifies the real native Web BLE UI on
synthetic PM3 RAM. It does not qualify physical EEPROM, a real tag’s negative
key-A write permission, general unknown-key recovery or full formatting. The
previous failed samples remain archived; a passing campaign does not establish
the cause or resolution of their intermittent RF errors.

The separate 2K full-format UI bench is prepared behind
`CHAMELEON_BLE_TEST_CLASSIC_FORMAT=1` plus the existing size opt-in and exact
2K stop report. It uses independent guarded simulations for cancellation and
confirmed formatting. It requires all 128 blocks unchanged on cancellation,
95 data blocks erased, 32 factory trailers (final GPB `BC`), block 0 preserved,
checked model-6 requests throughout, data writes before trailers, and exactly
127 mutations without replay. Full USB readbacks and a final BLE read compare
every block; an already-formatted second invocation must add zero mutations.
The software/fixture checks pass, but this new hardware campaign has not run:
the Ultra is available for the user's requested local manual test. No physical
full-format qualification is claimed.

A broader software browser control now starts with every 2K data block
nonzero and all 32 trailers different from factory defaults. Chromium and
WebKit both verify cancellation with all 128 blocks intact, exactly 127
mutations in data-then-trailer order, full-memory export after formatting,
block-0 preservation, and zero additional writes when formatted again. This
uses an isolated simulated HTTP backend, not BLE or physical RF; its two
passing cases complement the prepared real-BLE bench without replacing it.
Evidence: `format-2k-all-blocks-software/`.

For the user's local test, port 8080 now serves live main-checkout sources and
the real USB API instead of the old simulated backend. A separately frozen
snapshot of the 13 edited main-checkout files (patch SHA-256
`4786e6322caf561848c2a0da30f3607f8a3f27880ee090a9ad16320afeaf8b4a`)
built application `v1.0.0-64-g265ddf2-dirty`, installed application-only after a
fresh backup. ZIP SHA-256:
`500655e49aa873234f8c1ac3c7743940316fb950b23db5935c5f6c47680821b2`.
All eight slot metadata records, all populated images, settings, reader state
and active slot 0 match before/after. The actual USB HTML/module/CSS match the
build, and the USB app module is byte-identical to hosted shell
`e4fe0afaf595285b3e73e604548f0ebc41b86fe6f9da30e1dc73ecd5ec20ed5b`.
The snapshot passes 315 JS tests, lint/typecheck, 12 hosted cases and 12 WebKit
mobile cases. These software checks do not establish native Bluefy BLE or
repeat the earlier PM3 qualifications on the redesigned interface. Main UI
and documentation edits remain untouched; diagnostic/key/format bench work
stays on the isolated branch until reconciliation. Local evidence:
`latest-ui-installed/`, `latest-ui-flash-preview-source.json` and both full
backups `usb-backup-20261106-235441/`, `usb-backup-20261106-235710/`.

The temporary public preview was updated to that snapshot in separate-repo
commit `5436c48`; Pages job `42308779773` completed successfully. Nine public
file hashes match the staged package. A direct public-origin Chromium check
at 320 px exports all 128 2K blocks including edited block 126, reopens the
document offline and navigates to Device without API requests or page errors.
The new source ZIP was excluded after approval review; the existing archive
remains explicitly identified as historical `d5a58d3` source. The development
repository was not pushed. Native Bluefy remains a manual qualification.

To identify the first underlying read error without changing normal callers,
checked command `2027` now accepts one optional trailing byte `1`, after the
UID. Its unchanged mask/data prefix gains a four-byte tail: version `1`, first
failed relative block offset (`FF` for a complete sector), and the original
read status in big-endian 16-bit form. Selection/authentication failures keep
their original outer status and empty body. The original 8-byte and checked
UID requests retain their exact response lengths and RF sequence; diagnostics
add no authentication, RF read or retry. Unknown block bytes remain zero.
Maximum output is 262 bytes, within the existing 272-byte command buffer.

Sanitizer-backed production-code tests cover all 40 sectors, both geometries,
4/7/10-byte checked UIDs, every failed block position, 16-bit error preservation,
invalid flags and the retained-field lifecycle. All 314 JS tests pass; the
diagnostic decoder rejects inconsistent masks, offsets, lengths and unknown
bytes. The explicit 2K bench mode measures all 32 sectors twice in each of six
USB/BLE idle/load phases, preserving each error without missing-block retries:

```sh
CHAMELEON_BLE_TEST_CLASSIC_DIAGNOSTICS=1 \
CHAMELEON_BLE_TEST_CLASSIC_DIAGNOSTICS_SIZE=2k \
  npm --prefix software/web run test:hardware:ble -- reader-classic-diagnostics.spec.mjs
```

Set the PM3 client/port and exact 2K four-fault report as for the size campaigns.
The bench validates the live PM3 identity before RAM changes, then requires
autonomous stop/ping, full RAM restoration, tag disappearance and preservation
of all Ultra images/settings. Its first physical campaign on application
`v1.0.0-64-g265ddf2` completed all 408 RF commands and 384 sector measurements
without an RF error or partial sector, including USB reads under BLE metadata
load. The real BLE trace contains 166 operations, no framing errors or pending
reply, and no HTTP fallback. Supervised stop, persistent ping, all 4096 RAM
bytes independently read back, and tag disappearance passed. A fresh complete
Ultra backup matched all slot images/settings and active slot 0. Evidence:
`sector-rf-diag-265ddf2/`; install archive SHA-256
`bdfa0befe915c98b4bffefcf58ace40f56139246def6e76fb7a08fdc47832e14`.
This validates the optional diagnostic on hardware; it does not reproduce or
resolve the earlier intermittent failures or qualify general RF reliability.
A read status distinguishes timeout,
CRC and parity classifications; an invalid-frame status alone cannot establish
whether the physical cause is a NAK, interference or another RF condition.

The separate **Copy to a tag** path now obtains fresh bound ATS evidence for
SAK `08` targets before reading target memory or offering confirmation. Known
Plus S/X SL1 ATS selects 128 blocks; an extended inspection without ATS retains
the 1K interpretation. An older/unavailable inspection or an unrecognized
nonempty ATS stops the copy instead of trusting the source document or its
cached model. After confirmation, capacity and ATS are checked again before
the existing full-memory comparison. The final clone identity check obtains
fresh ATS when the Classic scan omitted RATS, avoiding a false failure after
verified writes. This does not authenticate the silicon or make RF silence
proof of physical capacity.

All 311 JS tests pass, including upper-block copying, cancellation, both
directions of 1K/2K mismatch, capacity/ATS changes during review, missing or
invalid evidence and no write replay after a lost upper-block reply. Production
WASM is used for the geometry and planning. All 54 targeted Chromium/WebKit
cases pass, including existing copy/clone and neighboring 2K read/format/repair
paths. The physical copy campaign has a
separate opt-in, in addition to the size-specific stop report and HF alignment:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_CLASSIC_COPY=1 \
CHAMELEON_BLE_TEST_CLASSIC_SIZES=1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic.spec.mjs --grep 'cancels and copies upper'
```

It cancels a copy, stops/restores the simulator, then arms a second bounded
simulation to copy data blocks 1/126 through the real BLE UI. It compares all
128 blocks independently over USB after each stage and restores both blocks
through USB. Both simulations retain the existing 180-second hard deadline;
the supervisor must stop the PM3, confirm its ping and restore all 4096 original
RAM bytes. The fixture is reloaded and verified between phases. The initial
single-simulation attempt exceeded that deadline during the long copy path:
the supervisor stopped it and restored RAM without physical intervention;
independent readback matches all 4096 original bytes. That attempt is not a
copy qualification and its logs remain under `classic-copy-2k-first-7f6860f/`.
The split campaign then passed on frontend `d5a58d3` (copy code `7f6860f`),
using installed application `v1.0.0-58-g633159a`: cancellation preserved all
128 blocks; confirmed data copy wrote blocks 1/126 with key A and independent
complete USB readback, followed by USB restoration and another full comparison.
Four bound ATS inspections passed. The trace records 1,131 GATT exchanges,
no framing errors or pending reply, and exactly the two intended data writes.
Both simulations ended normally with persistent ping confirmation and all
4096 original RAM bytes restored and independently read back. A fresh complete
Ultra backup still matches all eight slots, populated images, settings and
active slot 0. Evidence: `classic-copy-2k-split-d5a58d3/`.

This qualifies the real Web Bluetooth UI data-copy path against synthetic PM3
RAM under USB power; UID, key changes and EEPROM programming are separate
scopes. The browser clone case changes only data while identity/keys already
match; it is not a physical UID/key-cloning proof. Other Plus generations and
ambiguous backdoor-only 2K clones still
need identification/qualification. These are pending work, not additional
accepted exclusions.

When a test changes PM3 emulator RAM, `startSimulation` receives `restoreBin`:
an existing, exact 4096-byte backup. The supervisor freezes it in its own memory
before arming. After break/ping, it reloads that backup, saves all 4096 bytes
again and requires exact equality, recording `ramRestored` and its SHA-256.
This restoration survived an actual owner SIGKILL, with independent readback.
Only volatile emulator RAM changes; PM3 firmware and flash remain untouched.
The test keeps the original backup and recovery logs even if qualification fails.

Then run the real BLE UI comparison:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_READER_POSITIVE=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
npm run test:hardware:ble --prefix software/web -- reader-positive.spec.mjs
```

Three USB scans must return UID `11223344`, ATQA `0004`, SAK `08` and no ATS.
The actual standalone app then scans over physical BLE and exports the same
identity, with HTTP fallback blocked. After the independent stop completes, a
fresh USB scan must report no NFC-A tag. Device settings, mode, active slot and
all eight metadata records must be preserved. This passed on the physical Ultra
with firmware `v1.0.0-28-g76f8159`. It qualifies this positive identity capture,
not Classic memory recovery, other NFC-A identities or physical tag writes.
Neither the PM3 firmware nor its stored flash is modified.

A positive Type 2 memory campaign uses the same stop-qualified simulation family:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_TYPE2_READER=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
npm run test:hardware:ble --prefix software/web -- reader-type2.spec.mjs
```

It backs up the full PM3 emulator RAM, loads and verifies a synthetic NTAG213
fixture, then reads its UID, version, 43 public pages, counter and 32-byte
signature through USB. The actual BLE UI must detect the type automatically,
finish its memory capture and export identical public data and metadata while
HTTP fallback is blocked. Password/PACK pages 43–44 must remain unknown in the
document. The signature is synthetic; no authenticity claim is made.

The detached supervisor stops RF and restores the original PM3 RAM. Independent
readback compares all 4096 bytes, and a fresh Ultra USB scan must find no tag.
Device identity, firmware, settings, mode, active slot and eight slot metadata
records must match. This passed on the physical Ultra with
`v1.0.0-28-g76f8159`. The final full Ultra backup also matched the pre-campaign
images and metadata. This qualifies this synthetic NTAG213 read path, not
protected-tag access, other Type 2 profiles, physical tag writes or tear-off.

The same bench has a separate opt-in write scenario:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 CHAMELEON_BLE_TEST_TYPE2_WRITES=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
npm run test:hardware:ble --prefix software/web -- reader-type2.spec.mjs
```

The synthetic NTAG213 configuration disables password protection with CFG0
byte 3 (`AUTH0`) set to `FF`. USB first writes one user page, independently
reads it back, requires status `0075` for a stale expected value, then restores
the original page. The actual BLE UI captures the fixture, edits pages 6 and 8,
cancels at confirmation and must leave all 43 public pages unchanged. It then
confirms both writes and restores both pages through the same UI. Independent
USB reads compare every public page after each step, including unchanged pages.

Finally USB changes page 6 after the BLE capture. The BLE UI must refuse the
pending edits before confirmation, report zero verified writes and invalidate
its previous read binding. Independent USB reads must find only the deliberate
external change, which is then restored. HTTP fallback remains blocked, and the
same stop, full PM3 RAM restoration and Ultra preservation checks are required.
This passed on the physical bench. It qualifies RF writes to PM3 volatile RAM;
password/lock enforcement, EEPROM programming, interrupted physical writes and
other tag models still require separate qualification.

Two positive Classic 1K **reader** scenarios now use the separately
stop-qualified full Classic simulation:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 \
CHAMELEON_BLE_TEST_CLASSIC_READER=1 \
CHAMELEON_BLE_TEST_CLASSIC_WRITES=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic.spec.mjs
```

Run the Classic stop preflight above first on the same client/firmware/USB
setup. The bench backs up all 4096 PM3 RAM bytes, loads and independently
verifies a synthetic 1K image, then starts only the exact guarded interactive
command. Key A is `FFFFFFFFFFFF`, Key B is `A0A1A2A3A4A5`; trailer access
bytes `7F078869` keep Key B hidden so successful B authentication is meaningful.
The USB reference authenticates both keys on all 16 sectors and compares the
48 data blocks and 16 access-byte groups, including full sector masks. The
real hosted BLE UI scans, runs its built-in dictionary recovery and exports
all 64 blocks, with both verified keys reconstructed into each trailer. UID,
ATQA, SAK, ATS, every byte and the 16 key pairs must match. HTTP device requests
from the page are blocked throughout. The read-only case passed in 46.2 seconds.
This qualifies known-dictionary keys, not acquisition/recovery of unknown keys.

The second scenario establishes a USB write/readback/stale-before control,
then edits data blocks 1 and 4 in the real BLE UI. It cancels and independently
compares all memory, writes and verifies both blocks, restores them through
the UI, then modifies block 1 externally through USB. The stale BLE write is
refused with zero completed blocks, the remaining edited block stays unchanged
and the UI invalidates its read binding. USB restores the fixture. All memory
is independently compared after each step; trailer keys remain hidden in raw
reads. This case passed in 57.5 seconds. These are RF writes to volatile PM3
RAM, not EEPROM or tear-off validation. Block 0, UID changes, other protected
permission modes and other Classic sizes remain unqualified. The separate trailer campaign below covers one key/access change.

The trailer campaign also exposed and reproduced a frontend preflight bug.
For trailer condition `011`, Key A can authenticate/read but only B can change
the keys/access. The firmware correctly returns `CMD_ERR` (`0x75`) for the
read-only check with A and succeeds with B. The frontend previously stopped
at A's rejection. It now tries the other known key for this preflight status;
all trailers still need an accepted preflight before any data or security
write. Mutating commands are never retried. The actual BLE failure and two
software failures were captured before the fix; all 273 JS tests now pass,
including both-key refusal without writes and lost write-reply behavior.
Ten targeted Classic UI regressions also passed in Chromium/WebKit.

The subsequent key/trailer round trip passed on real BLE in 86.5 seconds:
UI cancellation preserved all memory, the UI wrote both new keys and access
bytes for sector 1 through Key B, old keys were rejected, and USB authenticated
both replacements. A fresh UI read recovered the supplied new keys and all
64 blocks. The UI then restored the original keys/access; USB rejected the
replacements and accepted both originals, followed by another full UI read.
Both security writes invalidated the prior read binding. The full 4096-byte
PM3 RAM backup, all Ultra images/metadata/settings and no-tag state were
verified after cleanup. This uses provided keys and simulated volatile RAM;
it does not qualify unknown-key attacks or physical EEPROM programming.

```sh
CHAMELEON_BLE_TEST_HOSTED=1 \
CHAMELEON_BLE_TEST_CLASSIC_TRAILERS=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic.spec.mjs --grep 'keys and access through key B'
```

One subsequent trailer run stopped during its independent USB memory check
after UI cancellation, before any RF write: sector 6 returned mask `0007`
instead of `000F`, leaving trailer block 27 unread. BLE was connected at rest.
The supervisor confirmed its direct ping and restored all PM3 RAM. This RF
incident remains unresolved and is retained separately from the preflight fix;
it cannot be attributed to the BLE transport from this observation alone.

Another attempt reached the BLE UI after the USB controls passed but stopped
with RF status `5` (UID BCC error) during scan/inspection, before writing. This
is retained as a second unresolved RF incident; the fixture's UID/BCC were
valid and no repair or retry of a mutation was attempted. The exact BLE
command was not captured in that run.

`gatt-trace.mjs` now records the actual native NUS writes/notifications in this
bench, leaving their bytes and method results intact. It decodes them with the
production frame parser, checks checksums/correlations and writes the raw chunks
and decoded operations into `gatt-trace.json`, including firmware RF statuses.
It does not enable Bluetooth emulation. The first trace captured 241 operations
without framing errors, including A preflight `0x75`, B preflight success and
one actual trailer write with B. The extended scenario requires
these exact pairs for both security writes and B credentials for both data
writes, rather than inferring key selection from the UI result alone.

The extended scenario then passed in 94.9 seconds, with 365 real GATT
operations and no framing errors. It wrote and restored block 4 using Key B
under the configured data access condition `100`, independently comparing all
memory after each operation. The trace requires exactly two data writes with
credential kind B and two trailer writes with B; each trailer write follows
a rejected A preflight and an accepted B preflight. There is no replay of a
mutating command. One final device-info read was pending at page close; all RF
requests had confirmed replies. The supervisor's direct ping, full RAM restore,
no-tag scan and all Ultra preservation checks passed.

A proposed negative data-write control also exposed a PM3 fixture limitation:
a write with A under condition `100` returned `0x75`, not a NAK-related status
`2`. The compatible simulator source ACKs the WRITE command and its data phase
even for denied data access (`MFEMUL_WRITEBL2`), so it cannot prove a physical
tag's refusal semantics or absence of mutation. The attempt was archived and
its original RAM restored. It is not counted as negative protection coverage;
physical tag access-denial/EEPROM behavior remains to qualify. Positive writes
using B can still be checked through the real GATT credential, full USB
readback and restoration.

All three scenarios require the held-descriptor ping, independent full RAM readback,
no tag after stop, and preservation of all Ultra images, eight metadata records,
settings and mode. The detached supervisor restores the initial PM3 RAM even
if the owner crashes. A fallback restore makes the test fail.

A read-only characterization separates intermittent RF errors from GATT
framing failures:

```sh
CHAMELEON_BLE_TEST_HOSTED=1 \
CHAMELEON_BLE_TEST_CLASSIC_DIAGNOSTICS=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic-diagnostics.spec.mjs
```

It uses the same frozen Classic fixture and independently supervised PM3 mode.
Eight cycles per phase each run selection (`2000`), the exact 59-key UI
inspection (`2040`) and sector-6 read (`2027`). Partial sectors retain their mask
and trigger recorded, independent missing-block reads; they never become an
error-free sector sample. All RF error replies remain in `diagnostic.json`.
A passing diagnostic means the measurements and cleanup completed; it is
**not** an RF parity or reliability qualification.

The first campaign on `76f8159` completed 48 cycles and 144 RF commands:

| Phase | Inspection errors / 8 | Selection errors | Partial/error sector reads |
| --- | --- | --- | --- |
| USB before BLE | 0 | 0 | 0 |
| USB, BLE connected idle | 1 (`5`) | 0 | 0 |
| BLE first pass | 1 (`5`) | 0 | 0 |
| USB with continuous BLE device refresh | 1 (`5`) | 0 | 0 |
| BLE second pass | 1 (`5`) | 0 | 0 |
| USB after BLE disconnect | 0 | 0 | 0 |

All four errors came from inspection `2040`; two arrived through USB, so the
failure is not specific to carrying that command over GATT. They occurred in
the connected phases of this limited sample; this does not establish a failure
rate or its cause. The refresh phase completed 24 actual BLE device reads.
The GATT trace had no frame/checksum/correlation error. Direct PM3 stop, exact
4096-byte RAM restoration, no remaining NFC-A tag, and preservation of every
Ultra image/metadata/settings/mode passed.

Inspection of the shared NFC-A selector then found an independent parser
defect: it copied five anticollision bytes and checked BCC without validating
the received bit count. A four-bit NAK could therefore report an invalid UID
checksum, and a plausible short frame could be accepted under a BCC override.
Selection now requires exactly 40 anticollision bits and 24 SAK bits, and caps
selection at three cascade levels before writing beyond the ten-byte UID.
Genuine five-byte replies with a bad BCC still report status `5`; the explicit
BCC-repair setting retains its behavior for a correctly sized frame. No RF
retry or checksum override was added.

The sanitizer-backed regression compiles the actual production selector. It
failed before this change and passes afterward, covering 4/7/10-byte UIDs,
NAK/truncated/oversized anticollision replies, all BCC override values, a real
bad BCC, a short SAK and an excessive cascade. The eight capture tests and
four magic/guarded-write tests pass. This fixes reply classification and
bounds. The application-only archive (642624 bytes) was validated and flashed
to the pinned Ultra. Firmware `v1.0.0-41-gdbd62e9` booted successfully, with all
images, eight metadata records, settings and active mode identical to the
pre-flash backup. Its static RAM use is unchanged. All 210 host tests passed.
The PM3 firmware was not changed.

The identical 48-cycle/144-command campaign on this firmware again measured
four inspection failures: three status `2` and one status `1`, with no status
`5`. Selection and sector reads remained complete, and the GATT trace had no
frame errors. USB-before and connected-idle phases had no inspection errors;
the first BLE phase had one status `2`, the second had status `1` and `2`, and
the first USB cycle after requesting BLE disconnect had status `2`. The test
requests host-side disconnect immediately before that last phase; it does not
prove the firmware's disconnect-event processing finished before the first
USB command. These small samples establish neither an improved failure rate
nor the physical cause. RF reliability remains unqualified.

The full known-key Classic UI capture also passed on `dbd62e9` in 46.4
seconds: all 64 blocks and key pairs matched USB, with complete supervised
cleanup and preservation. This positive run does not resolve the diagnostic
inspection failures.

The retained PM3 radio traces contain 2378 frames before and 2380 after the
parser change. In respectively eight and six cases, a logged valid
`11 22 33 44 44` UID response was followed by another WUPA rather than SELECT.
The PM3 trace records its own intended transmission, not an independent analog
capture, so this does not prove where the signal was lost or misinterpreted.
The diagnostic now also downloads `pm3-rf-trace.log` after supervised stop and
RAM restoration. Both campaigns confirmed the direct stop ping, full RAM
restoration, no remaining tag and preservation of every Ultra image and setting.

The IRQ wait then revealed a separate timing defect. Normal waits used a
shared BSP counter updated in 10 ms steps, including the inspection's 5 ms
budget. A global tick just after resetting that counter could expire a new
transaction after less than one millisecond. The old loop also rejected a
latched completion when its subsequent elapsed-time check expired after an
interrupt delayed CPU execution.

Every wait now measures elapsed RTC ticks from its own start; the existing
1 ms wake-up and configured normal budgets are preserved. A completion already
read from the chip is processed before checking elapsed time. The MFRC522
[ComIrqReg descriptions, table 27](https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf)
identify received-stream/command completion flags and specify their software
clearing semantics. This change sends no additional RF request and adds no
static RAM. A transfer with no completion still times out.

The production-code regression failed before and passed after: a valid reply
after about 1 ms survives a global 10 ms tick, absent replies expire against
actual 1/5/25 ms elapsed time, RTC wrap is handled, and a completion survives
delayed polling. Zero budgets, unrelated IRQ bits and completed error flags
are covered. All nine capture tests and four magic/guarded-write tests pass.
All 211 host tests pass. The application-only image (642496 bytes) was flashed
to the pinned Ultra as `v1.0.0-43-g1662597`, with unchanged static RAM use and
all images, eight metadata records, settings and mode preserved. The same
48-cycle/144-command physical campaign then completed without RF errors or
partial sectors in any of its six phases. Its 107 GATT operations had no frame
errors; 25 device refreshes ran during the USB-with-BLE-load phase. Direct PM3
stop, exact RAM restoration and disappearance of the simulated tag were
confirmed. This first clean sample supports the timing correction, but does
not establish general RF reliability or resolve the separate Classic emulation
failure.

Three independent repetitions of this unchanged diagnostic also completed
without RF errors. Across all four runs: 192 cycles, 576 RF commands, no
partial sectors, 425 valid GATT operations and 97 concurrent BLE device
refreshes. Every run confirmed the direct stop ping, exact PM3 RAM restoration,
absence of the simulated tag and preservation of all Ultra data and settings.
The four retained PM3 traces contain 9440 frames, including 1034 anticollision
requests, without the previously observed UID-without-SELECT sequence. These
finite traces still describe PM3 transmission intent, not an independent
analog capture. This is evidence for the reader timing fix on this fixture;
it does not qualify physical EEPROM writes, other tag families or all RF
conditions. Earlier failure reports remain retained.

The complete Classic UI regression also passed on this firmware: known-key
reading (64 blocks and 16 key pairs), data-write cancellation/write/readback/
restoration/stale-capture refusal, and key/access changes with Key-B-only data
writes followed by full restoration. The three traces contain 609 actual GATT
operations without frame errors. The independent USB checks and supervised
cleanup passed throughout.

The first NTAG213 public-read regression instead failed on its initial USB
scan with status `1`, before connecting BLE. The following NTAG213 write/
restore scenario passed, as did three independent repetitions of the unchanged
public-read scenario. No delay or retry was added to those tests. The original
failure remains unresolved; the PM3 client's simulation banner alone is not a
device-side RF-ready acknowledgement. That startup race is a hypothesis, not
an established cause. The Type 2 bench now saves each USB reply and downloads
the PM3 radio trace after supervised stop/restoration so another occurrence
can be inspected. Successful simulated writes still do not qualify physical
tag EEPROM programming.

A separately opted-in recovery case replaces only sector 1's key A in the
volatile Classic 1K fixture. The actual packaged WASM first confirms that
`D7C2A9E46135` occurs in neither the 2511-key nonce dictionary nor any RF
dictionary pass. USB controls verify that the original key fails and the new
key authenticates; no credential is supplied to the browser. The normal BLE
UI then recovered the key with origin `Nested` and read all 64 blocks with
all 16 key pairs matching the fixture. Four actual nonce-acquisition commands
returned eight observations, and the recovered key passed a real BLE physical
authentication. All 121 GATT operations decoded without frame errors; no RF
mutation command was sent. The run passed in 47.6 seconds, including direct
PM3 stop, complete RAM restoration, disappearance of the tag and Ultra data
preservation. This qualifies this one weak-PRNG recovery case, not StaticNested,
Hardnested, Darkside or all Classic sizes.

```sh
CHAMELEON_BLE_TEST_HOSTED=1 \
CHAMELEON_BLE_TEST_CLASSIC_RECOVERY=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
  npm --prefix software/web run test:hardware:ble -- reader-classic.spec.mjs --grep 'non-dictionary'
```

A separate Classic 1K RF campaign uses the aligned Proxmark3 HF antenna:

```sh
cd software/web
CHAMELEON_BLE_TEST_HOSTED=1 \
CHAMELEON_BLE_TEST_CLASSIC_EMULATION=1 \
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
npm run test:hardware:ble -- classic-emulation.spec.mjs
```

It requires an enabled Classic 1K image in slot 1 with known key A and readable
data/access bytes in all 16 sectors. It snapshots every nonempty HF/LF image
and all eight metadata records before temporarily selecting that slot. The PM3
compares UID, ATQA, SAK, all 48 data blocks and the 16 sector access-byte groups.
It authenticates with the saved key A. Printed trailer keys are excluded from
the comparison because the client injects the supplied key into its output.
Key B authentication and writes are not tested.

Two full sector passes run without BLE, two with BLE connected at rest and two
with repeated device refreshes throughout each PM3 process. Failed reads are
retained across rounds; they are never retried into a passing result. A USB
reader reservation is a negative control: the emulation must disappear, then
return after release. This PM3 client reports that absence silently with
`PM3_ESOFT` (-10, process exit 246), so the test checks the exit code and missing
identity fields. The real BLE UI selects the Classic slot and restores the
original selection. Cleanup also verifies every saved image, metadata, settings
and device mode, with independent USB restoration available after a failure.

The initial physical comparison passed all 16 reference sectors, then lost one
RF response at block 17 while BLE was connected. A subsequent 96-sector-read
comparison passed (32 reference, 32 BLE idle, 32 with one refresh per sector).
That later pass does not resolve the initial intermittent failure. The current
campaign keeps refresh traffic active throughout the PM3 process because a
single refresh can finish during client startup, before its RF exchange.
That strengthened campaign also passed all 96 sector reads, with 64 completed
BLE UI refreshes during its 32 traffic-phase reads, no HTTP fallback and full
device/slot preservation. It ran on firmware `v1.0.0-20-g8aa99b3`, with USB
power and Chromium on macOS. Its attached JSON qualifies that individual run;
the earlier lost response remains an unresolved reliability observation.
Classic coexistence reliability remains under investigation; this is not an
additional accepted scope exclusion and does not qualify the Ultra as a reader.

An optional Proxmark3 diagnostic retains the investigation of unsupported
NFC-V emulation during an active BLE link. It is skipped in normal hardware
campaigns and is expected to fail when explicitly enabled:

```sh
cd software/web
CHAMELEON_PM3_CLIENT=/absolute/path/to/proxmark3 \
CHAMELEON_PM3_PORT=/dev/cu.usbmodemiceman1 \
CHAMELEON_BLE_TEST_UNSUPPORTED_EMULATION=1 \
npm run test:hardware:ble -- emulation.spec.mjs
```

Position its antenna at the Ultra and select a saved SLIX-L slot. The test reads
the expected image over USB, compares RF identity and all eight memory/lock
blocks, then reserves reader mode briefly as a negative control: the tag must
disappear. It restores emulation, connects Chromium over BLE and repeats the RF
reads while refreshing device information. Raw PM3 logs and a JSON report are
written to the hardware artifact directory. The report records `qualified`
separately from `preserved`: a failed BLE RF comparison still checks disconnect
cleanup, the original slot image, metadata and device settings. This is a read-only
campaign with USB power; it does not qualify battery operation or physical tag
writes. Use a protocol-compatible PM3 client; no PM3 flashing is performed.

This strict RF test currently fails on the physical Ultra: baseline SLIX reads
work, but they fail with Web Bluetooth connected. A measured 30 ms connection
interval and experiments with 100 ms and 20 ms RF windows did not establish
parity; those firmware experiments have been removed. This accepted scope
exception no longer blocks qualification of supported features. Closing the BLE connection restores the existing RF
path; a successful slot transfer alone is insufficient to qualify emulation.

The interface offers **Device → Disconnect Bluetooth** in the hosted app and on
localhost. It keeps the current document open and does not switch to HTTP. An
active NFC-V or NFC-B slot is marked paused in Device, Slots and the navigation;
the Device notice sits next to **Disconnect Bluetooth**. Simultaneous BLE control
and NFC-V/NFC-B emulation are outside the supported scope. Experiments with local connection latency and 200/400 ms
RF targets also failed the strict comparison and have been removed.

With only a PM3 available, reader tests need a reproducible USB control against
its RAM simulation before comparing BLE. The positive NFC-A, NTAG213 and known-key Classic campaigns
above now establish that control; NFC-V simulation has not established it.
Neither these identity captures nor host tests
qualify writes or physical ST25TB tear-off. Those campaigns require suitable
test tags and a repeatable antenna position.

## Contract 1

Nordic UART service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`; browser writes RX
`…0002…` with response and subscribes to TX `…0003…` notifications before HELLO.
Writes use conservative 20-byte fragments. Only one request is in flight.
Connect/discovery/subscription/HELLO have a combined 30-second deadline after
device selection; each subsequent exchange has a 20-second deadline.

Command **1090** carries a versioned envelope inside the existing Chameleon
frame. Integers below are big-endian:

| Envelope | Fields | Limits |
| --- | --- | --- |
| Request | version u8, correlation u32, operation u8, lease u32, argument u16, body | 12-byte header, body ≤1024 |
| Reply | version u8, correlation u32, service status u16, encoding u8, body | 8-byte header, body ≤512 |

Outer command status `0x68` means the envelope was processed. The service status
preserves HTTP outcomes; command/RF statuses remain inside the service body.
Encoding is 0 for bytes, 1 for JSON. HELLO returns contract 1, maximum body/reply
sizes and capabilities. A mismatch is rejected before any mutation. Malformed
frames, timeouts or mismatched correlations close the connection and reject
queued operations.

The closed operation list is defined in
[ble_api.h](../firmware/application/src/net/ble_api.h) and matched against the JS
codec by unit tests. It covers device/slot/status metadata, reader and slot
leases, keepalive/end, the shared RF command allowlist, settings, all slot
transactions, tear-off start/state/cancel and read-only update information.
It accepts no arbitrary HTTP URI or firmware-update entry operation.

Slot revision tokens include the live native buffer as well as saved records.
Loading an image clears the unused remainder of the shared HF/LF buffer, so
switching through a larger card does not change a restored smaller image's
revision merely because unrelated bytes remained in RAM. Host regressions
exercise Classic 1K/Mini/4K and EM410x/Viking/ioProx round trips. This fix does
not call save callbacks from revision queries or change shadow-write settings.
Revision values remain opaque; refresh them after a firmware update.

The corrected firmware `v1.0.0-28-g76f8159` passed the six standalone-package
hardware scenarios (device/slots, Classic RF, reader without tags, settings,
settings/reboot and tab ownership), plus the separate slot-8 transaction
campaign. The Classic RF run completed another 96 sector reads. Its return
to the original SLIX slot preserved the complete original metadata, including
the revision token. Original settings persisted after two further resets.
The slot-8 campaign also passed and cleared its test images. The earlier
intermittent Classic RF loss remains separate from the fixed revision defect.

During a long tear-off job, a bounded service callback processes only command
1090 controls and lease expiry. General legacy RF dispatch is not reentered.
Host tests cover cancellation and cleanup, but physical tear-off timing and
success rate over BLE remain unqualified.
