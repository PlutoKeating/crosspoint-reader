# StockStick single-screen and playback-program protocol

Full product definition, use cases, diagrams and API contract are in the adjacent
[StockStick product documentation](../../Project.StockStick/docs/product/README.md).
Since 2.0.0 the firmware no longer contains the upstream ebook reader. This document is not a
physical-device acceptance report.

## Pixels and SSP1

Physical raster: 528 × 792, row-major, MSB first, 1 = black, exactly 52,272 bytes.
The phone renders the final monochrome pixels and rotates landscape content into
physical panel coordinates. Firmware does not reflow text or replace fonts.

A complete program begins with `SSP1`, a uint32LE JSON-header length, an ASCII
escaped JSON header, then contiguous rasters. The header includes version 1,
plan, scene-to-frame indices and frame IDs/digests. Maximum header 32,768 bytes,
complete file 8,000,000 bytes, 64 windows. Cloud/phone check per-frame hashes and
source mappings; firmware checks total SHA256, size and safe indices.

`StudioFrame` streams to SD. `incoming.json` and `incoming.bin` retain a valid
partial transfer; restart rehashes its plaintext prefix and continues at its
length. A corrupted protocol discards the partial; a connection interruption
preserves it. Capacity checks reserve the remaining bytes plus 64 KiB. Only a
validated file may be activated. State writes use temporary/backup files.
Garbage collection retains active, saved program, last visual and backup files.

## Schedule, overlays and recovery

`lib/ProjectStick/StudioProgram` mirrors the TypeScript playback engine: Shanghai
UTC+8, daily/weekdays/selected weekdays/dates/trading calendar, overnight ownership
by start date, priority, fallback, sequence or deterministic daily shuffle.
Equal-priority ambiguity is rejected by publication validation; device fallback
holds the screen. Playback index, last time/change, manual hold and boundary are
persisted. A backward clock cannot replay elapsed rotation.

A temporary raw card preserves the installed program. Expiration or manual resume
restores that program. A permitted alert may overlay the raw card without destroying
it; on alert expiry the raw card resumes while still valid. Reports include the
actual program card and current override context. With no current scheduled frame,
a newly installed program is `scheduled`, not `displayed`, and retains the last
valid visual.

Since 2.2.0 every key that reaches Studio acts: the device-wide Nokia keyguard
(20 s idle, left-then-right unlock) owns locking on the Studio page as on every
other page, so the plan's `keyguardSeconds` no longer locks input and `mode`
only selects the idle power policy. The playback engine's portable first-press
unlock is bypassed by refreshing `lastKey` before each key event. Key hints, the
left/right feedback windows and the lock prompt are transient overlays drawn
over the frame (see [project-stick.md](project-stick.md#x3-controls)); they do
not alter the stored card, and the software receipt still reports the card
underneath. Only the companion activity draws Studio content. `displayBuffer`
completion is the software receipt boundary, not a physical panel-readback
sensor.

Portable idle CPU policy keeps scheduling and BLE running. Battery lifetime and
BLE/TLS memory coexistence require X3 measurements, not simulator estimates.

## Cloud and ownership

Device-bearer `GET /api/v2/device/studio` returns desired task, payload size/hash,
expiry, owner identity, BLE authority and accepted offline task. `frame?offset=`
streams the remaining payload. POST reports receiving/verification/refresh states,
`scheduled`, `displayed` with actual card ID, or failure. Late receipts cannot
complete a newer target. A cloud poll occurs every five seconds while Wi-Fi is
connected and BLE is idle.

Ownership changes clear Studio data, pending events and alert state and rotate BLE
credentials; device settings and saved Wi-Fi networks are kept. A revoked fully offline device cannot learn
about revocation until it reconnects; the mini-program makes that limitation visible.

The production API is `https://stockstick.plutokeating.beer`. TLS pins ISRG Root X2,
ISRG Root X1 and GTS Root R4 (`src/project_stick/StudioTrust.h`) with peer and hostname
verification in SecureNet, and requires a usable system clock. The domain currently
chains Let's Encrypt YE1 -> Root YE -> ISRG Root X2; the Google root stays pinned because
Cloudflare may reissue from Google Trust Services. Another deployment must maintain its
actual trust chain; never disable verification to transmit authorization material.

## BLE protocol 2

Service `9fe10000-6bc2-4ce7-8e62-77262df32ef1`; CONTROL/DATA/STATUS use 0001/0002/0003.
CONTROL is newline-terminated ASCII JSON, max512 bytes; DATA is uint32LE offset plus
up to240 cipher bytes; STATUS is readable JSON max512 bytes. Control is sent in20-byte
chunks and data respects negotiated ATT MTU. One authenticated transfer at a time.

K is an independent 32-byte authority key, N a fresh16-byte random connection nonce.
HMAC-SHA256 input strings are ASCII with literal separators; output is lowercase hex:

```
hello|device_id|N|epoch|base_task|boundary
studio2|device_id|N|epoch|task|sha256|expires|size|phoneTime
enc|N
<displayed or scheduled>2|task|sha256|N|base_task|expires|card_id
```

The raw HMAC of `enc|N` is the AES256-CTR key; decoded N is its initial counter.
Resumption uses a new N and seeks that connection's cipher stream to the stored
plaintext offset. BEGIN retries for an already installed task return its current
state instead of reinstalling. Invalid-time devices may adopt an authenticated
phoneTime; valid clocks are not blindly reset by a phone.

Commit ticks the installed plan. STATUS signs `scheduled2` with empty card ID
if no current frame, or `displayed2` with the actual frame after display completion.
The phone persists the frozen package/source/receipt, then reconciles online.
Conflict requires explicit keep-device/keep-cloud choice; receipt authentication
checks current owner/grant and epoch. `accepted_ble_task` releases offline protection.

## OTA

`studio_ota:1` enables the owner-controlled command API and `ota:2` the
current protocol: resumable downloads, image identity checks, trial boot with
automatic rollback and outcome reports. The studio poll's `command_pending`
flag gates `/commands` polling. The complete lifecycle, release tooling and
server contract are in [firmware-ota.md](firmware-ota.md). Simulator OTA and
BLE adapters do not perform hardware work.

## Build and verification

```
pio run -e gh_release
cmake --build build/test --target ProjectStickCoreTest
ctest --test-dir build/test -R 'ProjectStick|StudioProgram' --output-on-failure
```

Release uses the IDF BLE controller and NimBLE-Arduino peripheral host, with LTO.
Check the actual binary-size verifier as well as the outer PlatformIO status:
nested size failures must not be hidden by an outer SUCCESS. Native SDL builds also compile this state/render path; the adjacent
StockStick `scripts/studio-native-fixture.ts` and `check-studio-screens.ts` verify
exact pixels for base cards. See its QUICK_START for the
isolated offline fixture command. Neither these tests nor the build certify real
BLE throughput, mobile permissions, panel refresh, power usage or OTA boot safety.
