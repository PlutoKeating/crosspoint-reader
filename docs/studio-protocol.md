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
complete file 8,000,000 bytes, 64 windows. The phone checks per-frame hashes and
source mappings; firmware checks total SHA256, size and safe indices.

Since 2.4.0 content reaches the device over BLE only (Project.StockStick
`docs/product/BLE-ONLY-DELIVERY.md`): when a user publishes, the phone and the
device are both at hand, so the device never pulls content from the cloud.

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
it; on alert expiry the raw card resumes while still valid. BLE STATUS receipts
carry the actual program card. With no current scheduled frame, a newly installed
program is `scheduled`, not `displayed`, and retains the last valid visual.

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

## Cloud requests

Only a bound device holding a device token talks to the cloud; an unbound device
(and the website simulator, which is provisioned without a token) makes no
requests at all. All requests use `Authorization: Bearer <device_token>`:

- `POST /api/v2/device/register`: heartbeat every 6 h and on coming online
  (retried no faster than `poll_interval_seconds`) — **only when no phone has
  synced the device over BLE within the last 6 h** (2.6.0, "Phone-relayed
  sync" below; `last_phone_sync` in `project_stick.json`). Closing the device's
  Wi-Fi screen still forces one. Returns `bound`, `owner_id`,
  `server_time`, `is_trading_day`, `alert_poll_interval_seconds` and
  `bluetooth: {secret, epoch}`. `bound: false` or a 401 drops the credential,
  the Studio data and the BLE authority: the device is back in BLE setup mode.
  Since 2.4.1 register is how a bound device learns a rotated BLE authority
  (the server rotates it on ownership transfer, collaborator revoke and account
  deletion): when the secret (64 lowercase hex) or epoch (> 0) differs from the
  stored one, the device persists it like a bind, clears Studio content if the
  owner changed and drops a live BLE session so the phone reconnects with the
  new key.
- `GET /api/v2/device/alerts`: every `alert_poll_interval_seconds`, only inside
  trading windows (see project-stick.md "Market alerts").
- `POST /api/v2/device/events`: batched Studio feedback (`studio_next`,
  `studio_useful`) and `firmware_rolled_back` (`payload.detail` = "<version>
  <reason>"), sent after a heartbeat; queue capped at 32. Since 2.6.0 the
  phone drains the same queue over BLE first (STATE `events` + `sync` `ack`).
- `GET /api/v2/device/firmware/latest` on a manual 「检查更新」, then a direct
  download of the catalogue `url` (see OTA below).

Since 2.4.2 the register body also carries `metrics` (`heap_free`, `heap_min`,
`heap_max_alloc`, `uptime_ms`, `wifi_rssi`, `battery_percent`), so field heap and
radio problems are visible server-side (the server stores the names it knows).

One backoff gate covers every request. Transport failures, 429 and 5xx block all
cloud requests for 30 s, 60 s, 120 s … up to 10 min (reset by any other answer);
a 429 honours `Retry-After` in seconds (capped at 10 min; the simulator's HTTP
shim exposes no headers). A manual firmware check may skip an error backoff,
never a 429. Requests retry once, only on a transport failure. A failed
heartbeat changes nothing on screen: content keeps playing locally.

Worker, heap and time limits (2.4.2, tightened in 2.4.3; see docs/memory-budget.md):

- All cloud I/O runs on one worker task created once at boot in `setup()`
  (before Wi-Fi and NimBLE allocate) with a static 8 KB stack, so it always
  exists; activities only queue work and read results.
- Every request is bounded: no trusted clock after 10 s of NTP fails it (no
  request is sent), TCP connect and response reads use the 15 s HTTP timeout,
  and the wolfSSL handshake has its own 15 s deadline per attempt
  (SecureNet). Since 2.4.3 the StockStick API client does not retry a failed
  handshake as TLS 1.2 (Cloudflare speaks TLS 1.3), so one failed attempt
  costs at most 30 s instead of ~40 s. A firmware check that has not answered within 60 s ends on the
  screen as 「检查失败：网络请求超时」; other failures are shown specifically
  (no clock, unreachable server, low memory, server busy, HTTP error code).
  A BLE `ota` request the worker has not accepted within 60 s fails with
  `device_busy` in `STATUS.ota`.
- NimBLE (host and controller) holds heap a TLS session needs. Before each
  request the worker logs `heap`/`max`; below 56 KB free or a 24 KB largest
  block it deinitialises NimBLE for the rest of that cloud operation, and a
  transport failure is retried once with NimBLE released. NimBLE comes back
  with the same identity and key when the operation ends; a firmware install
  keeps it released until the restart. Requests never release NimBLE while a
  phone is connected (cloud requests wait for the phone to disconnect).
- Hard floor (2.4.3): if, after releasing NimBLE, free heap is still below
  32 KB or the largest block below 16 KB, the request is skipped and reported
  as a memory failure instead of attempting a handshake that cannot finish
  and only fragments the heap further.
- Time (2.4.3): an RTC reading before 2025 (a backup-power loss reads
  2000-01-01) is not a time. The device prefers server time, then the system
  clock (NTP, the phone's `time` in BLE `begin`, register `server_time`, which
  now also sets the system clock), then a valid RTC. The UI task copies trusted
  time into the RTC and, at boot, seeds the system clock from a valid RTC so
  TLS does not wait for NTP. Without any trusted time the Studio schedule
  holds the current frame.

Ownership changes clear Studio data, pending events and alert state; device
settings and saved Wi-Fi networks are kept. A new owner binds over BLE setup,
which delivers the new BLE authority.

The production API is `https://stockstick.plutokeating.beer`. TLS pins ISRG Root X2,
ISRG Root X1 and GTS Root R4 (`src/project_stick/StudioTrust.h`) with peer and hostname
verification in SecureNet, and requires a usable system clock. The Google root stays
pinned because Cloudflare may issue from Google Trust Services. Another deployment must
maintain its actual trust chain; never disable verification to transmit authorization
material.

## BLE protocol 2

Service `9fe10000-6bc2-4ce7-8e62-77262df32ef1`; CONTROL/DATA/STATUS use 0001/0002/0003.
CONTROL is newline-terminated ASCII JSON, max512 bytes; DATA is uint32LE offset plus
up to240 cipher bytes; STATUS is readable JSON max512 bytes. Control is sent in20-byte
chunks and data respects negotiated ATT MTU. One authenticated transfer at a time.

K is the independent 32-byte authority key delivered by the BLE setup `bind`, N a fresh16-byte random connection nonce.
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
The phone persists the frozen package/source/receipt and uploads the signed receipt
to the cloud history; receipt authentication checks current owner/grant and epoch.

Since 2.5.1 the GATT callbacks only verify, decrypt and queue (the NimBLE host
task must not touch the SD card, see memory-budget.md); `studio_ble::pump()` on
the main loop performs `start`/`append`/`commit`. The queue is 16 static slots
of 240 bytes (~4 KB, 2.6.2; 8 before) and every chunk goes straight to the
card's `incoming.bin`, so a program of any size costs no heap. A chunk waits
up to 6 s for a slot (the phone's write deadline is 8 s); longer main-loop
stalls fail the transfer as `device_busy`. Since 2.6.2 the first failure is
what STATUS keeps: chunks still in flight after it are dropped instead of
overwriting the cause with `unauthorized_or_invalid_chunk`. Since 2.6.5 the
card usage scan (`freeClusterCount`, seconds on a large card) runs only on the
background worker (20 s after boot, then every 10 min, never while a phone is
connected); `StudioFrame::start()` and the STATE metrics read its last value
and never scan, so `begin` no longer stalls the main loop while the first
chunks arrive (that scan was the "fails at 2 %" report). A stop the phone
resumes on its own — `device_busy`, `offset_mismatch`, `transfer_timeout`, a
dropped link — is shown on the device as 「传输暂停，正在等待手机续传」 and
only becomes 「传输中断」 after 45 s without a new `begin`; the mini program
reconnects and resumes the same task up to five times (1.5 s apart) before it
tells the user anything. STATUS `received` while
`receiving` is the number of bytes accepted from the phone (queued chunks
included), so the phone's progress check after each burst still matches; a
later storage failure shows up as `failed` on the next read, and a retry
resumes from what actually reached the card.

## Phone-relayed sync (since 2.6.0, BLE-first)

Everything the heartbeat used to carry goes through the phone, which is always
at hand when content is pushed (Project.StockStick `docs/product/BLE-ONLY-DELIVERY.md`):

- STATE characteristic `9fe10004…` (read, ≤ 512 bytes, no proof): built on the
  UI loop (`ProjectStickService::phoneStateJson`, refreshed every 2 s while a
  phone is connected, every 30 s otherwise; GATT reads only copy it). Fields:
  `v` (1), `firmware_version`, `firmware_build`, `capabilities{ble,ota,panel}`,
  `metrics{heap_free,heap_min,heap_max_alloc,uptime_ms,wifi_rssi,battery_percent,sd_total,sd_used}`
  (the register metrics, SD values only with a card), `clock` (trusted time
  held), `bound`, `trading`, `synced` (Unix seconds of the last phone sync, 0
  never), `pending` (queued event count), `events[{id,type,ts?,task?,card?,detail?}]`
  (oldest first, as many as fit in 512 bytes) and `ota_outcome{version,rolled_back,reason}`
  while a trial outcome is unreported.
- Op `sync` (bound mode): `{op:"sync", time, trading, ack:[ids], ota_ack, proof}`
  with proof `mac(secret, "sync3|n|time|trading|<ack count>|<ota_ack 0/1>")`.
  The phone sends it after `POST /api/v1/miniapp/studio/devices/state`
  accepted the STATE it read: `time` is the server clock (adopted like
  `begin`'s `time`, and it also becomes the working server time), `trading`
  the server's trading-day flag (-1 unknown), `ack` the event ids the cloud
  stored (removed from the queue), `ota_ack` clears the outcome. The device
  records `last_phone_sync`, restarts the heartbeat interval and, during a
  trial boot, confirms the image (`phone_ok`). The phone re-reads STATE and
  repeats while `pending` > 0.
- Op `unbind` (bound mode): `{op:"unbind", proof: mac(secret, "unbind3|n")}`,
  sent by the owner's phone right after `DELETE /api/v1/miniapp/devices`
  succeeded; the device drops the binding exactly like `bound:false`.

Trading-hours alert polling is unchanged: alerts are market events the phone
cannot relay in time. A rotated BLE authority (ownership transfer, collaborator
revoke) still reaches the device through register: the phone's transfer fails
with an epoch mismatch, which is not a successful sync, so the heartbeat is not
suppressed; the user can also close the Wi-Fi screen to force one.

The same service runs BLE setup protocol 3 (since 2.3.0; unbound devices, key from
the setup QR) and accepts `scan`/`wifi` and (since 2.4.0) `ota` ops in bound mode;
bound STATUS also carries `fw`, `wifi`, `scan`, `networks` and `ota`, and the
advertised name is `StockStick-XXXX` (scan response; the advertising packet
carries the service UUID and the id characters as manufacturer data). Protocol 3 is defined in Project.StockStick
`docs/product/BLE-SETUP.md` and `BLE-ONLY-DELIVERY.md`; see also
[project-stick.md](project-stick.md#ble-setup-protocol-3-since-230).

## OTA

Triggered over BLE (op `ota`, bound mode, MAC `ota3|N|version|sha256|bytes|url`)
or from Settings 「检查更新」; either way the device downloads the catalogue image
itself over Wi-Fi (resumable, `Range`), verifies SHA-256 and the image
descriptor, flashes, and boots it on trial with automatic rollback. The complete
lifecycle is in [firmware-ota.md](firmware-ota.md). Simulator OTA and BLE adapters
do not perform hardware work.

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
