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

Only a bound device holding a device token uses the device API; an unbound device
(and the website simulator, which is provisioned without a token) makes no
device API requests. These use `Authorization: Bearer <device_token>` and are
scheduled by `ProjectStickHost` on every page (2.7.1):

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

One request needs no binding and no credential, only Wi-Fi (2.7.1):

- `GET /api/v1/public/firmware/latest?channel=stable` on a manual 「检查更新」
  (Settings > System > Firmware update), then a direct download of the
  published image (see OTA below). The answer carries `version`, `url` (or
  `bin_url`), `sha256`, `bytes`, `notes`; the device compares `version` with
  its own (`stick_fw::compareVersions`: numeric fields, a plain release is
  newer than a suffixed build of the same numbers) and only offers a strictly
  newer one. 404 means nothing is published. No answer on this path touches
  the binding. Until 2.7.0 the check used the authenticated
  `/api/v2/device/firmware/latest` and refused unbound devices.

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

## BLE transfer protocol 4 (since 2.7.0)

Defined in Project.StockStick `docs/product/BLE-TRANSFER-V4.md`; protocol 2's
`begin`/DATA/`commit` transfer is gone (no compatibility). Service
`9fe10000-6bc2-4ce7-8e62-77262df32ef1`: CONTROL 0001 (write, JSON line ≤ 512
bytes, any chunking), DATA 0002 (write and write-without-response, uint32LE
stream offset + up to MTU − 3 − 4 cipher bytes; the device offers MTU 517 and
works with whatever is negotiated — WeChat asks for 511, payload ≤ 504 — since a
write of 1 … 510 cipher bytes fits one queue slot), STATUS 0003
(read, `protocol: 4`, plus `need` once known), STATE 0004 (unchanged),
PROGRESS 0005 (read + notify, 16 bytes: u32 `received`, u8 state, u8 error,
u16 need count, u32 0, u32 seq).

K is the 32-byte authority from `bind`, N a fresh 16-byte nonce per session:

```
hello|device_id|N|epoch|base_task|boundary
studio4|device_id|N|epoch|task|sha256|expires|size|header|phoneTime   (begin4)
enc|N                                                                 (AES-256-CTR key; IV = N)
<displayed or scheduled>2|task|sha256|N|base_task|expires|card_id     (receipt, unchanged)
```

The stream (encrypted as one CTR stream from offset 0) is the SSP1 header
(`header` bytes), then, for every frame the device lacks (`need`, ascending),
`u16 index | u32 comp_len | raw deflate` (window ≤ 1024 bytes). A single frame
is `header = 0`, `size = 52272`, `hash` = its digest, and only that record.

- **Device side** (`lib/ProjectStick/StudioTransfer`, `src/project_stick/StudioReceiver`):
  the Assembler writes the header to `incoming.bin`, resolves the frame
  digests (`frames[].sha256`), looks each up in the kept program files
  (active, saved program, last visual: those are the frame library, so a held
  frame is copied, never written twice) and announces `need` (PROGRESS state
  2, STATUS `need`, frame i = bit i % 8 of byte i / 8). Records are staged in
  `record.z` and inflated with uzlib (1 KB dictionary, static); each frame
  must be exactly 52,272 bytes with the header's digest (`frame_mismatch`).
  Held frames are copied in order between records. `commit` hands the rebuilt
  file to `StudioFrame::commit()` (full SHA-256 check, install, receipts as
  before).
- **Flow control:** the host callback never blocks. In-order writes are
  decrypted into up to 16 510-byte slots (heap, allocated at `begin4`, at least 4 or `insufficient_memory`, freed when the transfer ends; static until 2.7.1); a duplicate or out-of-order write
  is dropped (a gap triggers an immediate notification with error 2,
  `offset_mismatch`, once), as is a write that finds the queue full. PROGRESS
  is notified every 4 writes or 4 KB, but held back while the queue is over
  half full, which paces the phone's 8 KB window; the writer task notifies
  once it drained.
- **Pump** (since 2.7.2 on the background sync worker, which is idle while a
  phone is connected; 2.7.0–2.7.1 had a separate 6 KB `StudioWriter` task):
  `pump()` (start, chunks, commit, abort) runs at priority 2 while it has
  work, so an e-paper refresh on the UI loop or render task never stalls
  reception. The inflater and the frame digests live only while receiving. The UI loop's
  `tick()` only reports the outcome (`displayed`/`scheduled`) and pending
  progress.
- **Link:** on connect and on `begin4` the device asks for 2M PHY, 251-byte
  link-layer packets and a 7.5–15 ms interval; 30–50 ms after the transfer.
- **Resume:** the partial (`incoming.json` + `incoming.bin`) resumes at frame
  boundaries: once the header is complete the phone continues at offset
  `header` with the remaining `need` (frames already in `incoming.bin` are
  dropped from it); a single frame either is complete (commit without data)
  or restarts. `StudioFrame::start(..., resumeLimit)` rehashes the prefix and
  overwrites anything past it.
- **Link reuse:** after a transfer ends and the phone has read the outcome,
  the next STATUS read at least 2 s later starts a fresh session (new N) on
  the same connection.
- Errors (PROGRESS code / STATUS `error`): 1 unauthorized_or_invalid_chunk,
  2 offset_mismatch, 3 device_busy, 4 storage_or_cipher_error,
  5 frame_validation_failed, 6 frame_mismatch, 7 too_many_frames,
  8 transfer_timeout, 9 insufficient_storage, 10 authorization_failed,
  11 insufficient_memory (2.7.2: no heap for the transfer buffers; retryable).

Measured with the official 17-frame plan (892,146 bytes): the full stream is
41,919 bytes; with one changed card it is 6,054 bytes (the other 16 frames are
copied on the device). Host tests drive the receiver with the mini program's
vectors (`test/project_stick_core/BleV4Vectors.h`, from
`gen_ble_v4_vectors.py`) and, with `STUDIO_V4_DEMO_SSP`/`STUDIO_V4_DEMO_DIR`,
with that full-size plan.

The simulator has no radio; `/.crosspoint/studio/import.v4` +
`import.v4.json` (`{task, hash, size, header[, stop_at]}`) feed a decrypted
stream through the same StudioReceiver/StudioFrame path at boot (test hook;
`stop_at` interrupts and resumes).

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
- STATE `cred` (since 2.7.2): 1 when the device holds a cloud token, 0 when a
  bound device lost it; op `token` restores it (project-stick.md
  "Credential loss and the `token` op"). `metrics.wifi_on` (2.7.2): the Wi-Fi
  radio is powered (Wi-Fi is on demand).
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
bound STATUS also carries `fw`, `wifi`, `scan`, `networks` and `ota` (since
2.7.1 `wifi.state` follows the real link on every page: `connected` with the
SSID whenever Wi-Fi is up, not only after a BLE Wi-Fi push; since 2.7.2
`wifi.saved` says a network is saved and the radio comes up on demand), and the
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
