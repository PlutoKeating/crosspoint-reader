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

- `GET /api/v1/public/firmware/version?channel=stable` on a manual 「检查更新」 (2.7.9: the answer is only `{"v":"x.y.z"}`; `&full=1` adds `n`/`h`/`u` and is fetched only for a newer version; every outcome is written to `/.crosspoint/net_last.txt` and STATE metrics carry `net_stage`/`net_code`)
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
2.7.4 adds `heap_largest_min` (the smallest largest-free-block seen since boot,
sampled every 5 s: how fragmented the heap got; `heap_min` is already the
minimum ever) and `stack_min` (the least stack headroom, in bytes, of the loop
task, the sync worker and the NimBLE host).

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

### Cloud jobs and a linked phone (2.7.3, 2.7.4)

On the C3 a TLS session does not fit next to a connected NimBLE stack (the
2.6.5 device measured 13 KB free with Wi-Fi up and a phone linked). Before
2.7.3 `releaseRadio()` refused while a phone was connected, so a mini program
that kept reconnecting (state relay, presence, the 60 s parked link, the
firmware page's follow loop) blocked every request: a manual firmware check
was skipped as "out of memory", and the self-inflicted error backoff then made
the next check say 「云端繁忙」.

Now a cloud job that needs TLS makes the phone yield
(`project_stick::phoneLinkAction`, host-tested):

- User-initiated jobs (Settings > Firmware update check/install) disconnect
  the phone at once.
- Background jobs (register heartbeat, alert poll, event flush) wait for as
  long as a phone is linked (2.7.4; 2.7.3 forced an active phone off after
  2 min). The phone relays what the heartbeat would carry and delivers
  firmware itself (`fw4`), so nothing is lost by waiting.
- Wi-Fi follows the same rule (`project_stick::wifiWanted`, host-tested): while
  a phone is linked the radio is off and its ~50 KB released; only a BLE Wi-Fi
  join/scan, the Wi-Fi and firmware pages and an on-device firmware job bring
  it up. A content or firmware transfer always keeps it off.
- Yielding: bound STATUS shows `net_busy` (`firmware_check`,
  `firmware_install`, `sync`, `alerts`) for ~0.4 s, then the device
  disconnects the phone and deinitialises NimBLE. Nothing advertises until
  the job ends and the stack is restored, so the phone's reconnects fail for
  the length of the job (an install ends with the restart).

Errors are reported honestly:

- 「云端繁忙」 only for a real server 429 / `Retry-After`, with the seconds left.
  The device's own error backoff (after a transport failure, 5xx, a memory
  skip) holds background requests only; a user-initiated job and the single
  in-job retry go out anyway. The internal "backoff" reason (`NetFailure::Backoff`)
  is never shown as busy.
- 「设备内存不足（可用 X KB）」 only when the heap stays below the TLS floor
  with NimBLE released (or not running).
- STATE `metrics` carries `tls_heap` / `tls_max` (the heap right before the
  last TLS attempt) and `net` (the last failure code: 1 clock, 2 network,
  3 memory, 4 rate limited, 5 backoff, 6 server, 7 timeout).

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
  the Assembler writes the header, resolves the frame digests
  (`frames[].sha256`), looks each up in the frame slots and in the kept
  content (active, saved program, last visual, the backup state's) and
  announces `need` (PROGRESS state 2, STATUS `need`, frame i = bit i % 8 of
  byte i / 8). Records are staged in `record.z` and inflated with uzlib (1 KB
  dictionary, static); each frame must be exactly 52,272 bytes with the
  header's digest (`frame_mismatch`). Held frames are taken in order between
  records. `commit` hands the transfer to `StudioFrame::commit()` (full
  SHA-256 check, install, receipts as before).
- **Frame slots** (2.7.10, `lib/ProjectStick/FrameSlots`,
  `src/project_stick/FrameStore`): the card has two preallocated areas,
  created once at the first idle housekeeping with no phone connected (the
  status screen shows 「正在准备存储」; one attempt per boot) and never deleted:
  `frames.bin`, 128 slots × 52,736 bytes (a frame rounded up to 103 sectors,
  one contiguous cluster run, 6,750,208 bytes) with `frames.idx` (per slot:
  SHA-256 of the frame it holds, an allocation stamp, valid flag; 5,136
  bytes), and `firmware.area` (6,553,600 bytes, see fw4 below). A transfer
  writes the SSP1 header to `incoming.ssp` behind a slot table and each new
  frame to a slot: a free one, else the least recently stored one no kept
  content references (those are pinned). A slot loses its index entry before
  it is overwritten and gets the new digest once the frame is complete, then
  the record names it. A frame the device holds in a slot is referenced, not
  copied: only read to continue the transfer's SHA-256. `commit` renames the
  record to `<hash>.ssp`; rendering, the boot check and the plan parser read
  frames through its slot table. `<hash>.ssp`: `"SSPS"`, u16 version 1, u16
  frames, u32 header bytes, u32 frames done, u16 slot per frame, the SSP1
  header. Content of 2.7.9 and earlier (`<hash>.bin`, the whole SSP1 file)
  stays readable; a frame held only there is copied into a slot once.
  Garbage collection removes records and files nothing keeps; their slots
  stay valid as a cache until evicted. Without the areas, without enough
  unpinned slots for the transfer's frames, or without heap for the 5.5 KB
  index, a transfer is one whole file (`incoming.bin` → `<hash>.bin`) as
  before. No raw sector writes: the frame writes are 512-byte, sector-aligned
  writes into the preallocated file, which SdFat sends straight to the card
  (no cache, no FAT lookups or allocation, no directory updates).
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
- **Link:** since 2.7.10 the device asks for 251-byte link-layer packets (DLE)
  once per connection and, once the first PROGRESS of a transfer is out
  (state 1), ONE connection update: 15–30 ms, latency 0, 6 s supervision
  timeout (`lib/ProjectStick/LinkTuning.h`). A phone whose link drops within
  10 s of that request is not asked again until the device restarts (four
  phones remembered). The PHY is the phone's choice. 2.7.0–2.7.9 also
  requested a 2M-only PHY and a 7.5–15 ms interval with a 4 s supervision
  timeout on connect and again on `begin4`; on the field X3 (Android 16,
  WeChat) the link died about 4.2 s after every `begin4`, before the first
  PROGRESS. The log names every GAP connection-update, PHY and data-length
  event.
- **Prompt answers:** `begin4` is answered with PROGRESS state 1 (and the
  resume offset) at once for fresh, resumed and single-frame transfers;
  `commit` with state 3 at once, while the writer drains the queue and
  installs. Commit, install and refresh never run on the NimBLE host task or
  under the BLE state mutex, and the BLE callbacks read StudioFrame's state
  from copies published under a lock never held across card access (2.7.9's
  callbacks could wait for an SD write behind StudioFrame's lock).
- **Commit after a link loss:** a `commit` the phone sent stays valid when the
  link drops before the writer reached it (ATT delivered every chunk before
  it): the writer drains the queue and installs instead of aborting. The
  phone's next `begin4` for the same task gets state 3, then 4/5 once the
  frame is shown; a different task is `device_busy` until the install is
  done. (A field X3 kept a complete `incoming.bin` while `state.json` stayed
  unchanged: the disconnect's abort ran before the queued commit.)
- **Reconnect:** a link loss mid-transfer queues an abort that keeps the
  partial and wakes the writer at once; a `begin4` arriving while that abort
  is still pending is accepted (the writer aborts first, then starts the
  resume) instead of being refused as `device_busy` (2.7.9).
- **Diagnostics:** Settings › 蓝牙 「最近传输」 shows the stage
  (header/frames/commit/firmware), the outcome (`ok`, `ok_after_link_loss`,
  the failure code or `link_lost`), seconds since, and how many ms the first
  PROGRESS took after `begin4`/`fw4`; 「连接参数」 shows the result of the
  connection update (status · interval in ms) or 「已停用（请求后断开过）」 for a
  phone that dropped after it. The log has `begin4 … in N ms` and
  `First PROGRESS N ms after begin`.
- **Resume:** the partial (`incoming.json` + `incoming.ssp` with its slots,
  or `incoming.bin`; `incoming.json` `slots` says which) resumes at frame
  boundaries: once the header is complete the phone continues at offset
  `header` with the remaining `need` (frames already received are dropped
  from it); a single frame either is complete (commit without data) or
  restarts. `StudioFrame::start(..., resumeLimit)` rehashes the prefix and
  overwrites anything past it. A whole-file partial of 2.7.9 resumes as a
  whole file.
- **Link reuse:** after a transfer ends and the phone has read the outcome,
  the next STATUS read at least 2 s later starts a fresh session (new N) on
  the same connection.
- Errors (PROGRESS code / STATUS `error`): 1 unauthorized_or_invalid_chunk,
  2 offset_mismatch, 3 device_busy, 4 storage_or_cipher_error,
  5 frame_validation_failed, 6 frame_mismatch, 7 too_many_frames,
  8 transfer_timeout, 9 insufficient_storage, 10 authorization_failed,
  11 insufficient_memory (2.7.2: no heap for the transfer buffers; retryable),
  12 checksum_mismatch, 13 low_battery, 14 trial_active (2.7.4, `fw4`).

Measured with the official 17-frame plan (892,146 bytes): the full stream is
41,919 bytes; with one changed card it is 6,054 bytes (the other 16 frames are
held on the device; since 2.7.10 referenced in their slots, before copied).

Card work per import, simulator counters (`SimIoStats`; a 17-frame,
890,955-byte program with noisy frames, 2.7.9 path vs frame slots):

| import | 2.7.9 path: writes / sectors / reads | slots: writes / sectors / reads |
| --- | --- | --- |
| fresh, 17 frames | 2,262 / 4,462 / 493 KB | 2,317 / 2,782 / 498 KB |
| one changed frame | 1,797 / 3,565 / 874 KB | 188 / 220 / 880 KB |
| resumed mid-stream | 2,279 / 4,493 / 915 KB | 2,335 / 2,814 / 926 KB |
| single frame | 144 / 171 / 29 KB | 149 / 176 / 34 KB |

The reads of a changed-card import are the held frames hashed for the
transfer SHA-256 (they were read to be copied before). Sectors count every
512-byte sector a write touches; the slot path writes whole aligned sectors,
and on the card it also skips the FAT and directory updates of a growing file.

Host tests drive the receiver with the mini program's
vectors (`test/project_stick_core/BleV4Vectors.h`, from
`gen_ble_v4_vectors.py`) and, with `STUDIO_V4_DEMO_SSP`/`STUDIO_V4_DEMO_DIR`,
with that full-size plan.

The simulator has no radio; `/.crosspoint/studio/import.v4` +
`import.v4.json` (`{task, hash, size, header[, stop_at]}`) feed a decrypted
stream through the same StudioReceiver/StudioFrame path at boot (test hook;
`stop_at` interrupts and resumes). The simulator creates the transfer areas
before the imports (`STICK_SIM_NO_AREAS=1`: the whole-file path) and logs the
SD operations they took.

## Firmware over BLE (op `fw4`, since 2.7.4)

Defined in Project.StockStick `docs/product/BLE-TRANSFER-V4.md` 「固件经蓝牙传输」.
STATE `capabilities.ota` is 4; the op `ota` (device downloads the image itself)
is gone. The mini program downloads the catalogue `.bin`, checks its SHA-256
and size, and streams it on the protocol 4 session:

```
{"op":"fw4","version","size","sha256","time","proof"}
proof = mac(K, "fw4|device_id|N|epoch|version|sha256|size|time")
stream (CTR key mac(K,"enc|N"), counter N, seeked to the resume offset):
  repeat: uint32LE raw_offset | uint32LE comp_len | raw deflate of one block
  blocks of 32,768 raw bytes (last shorter), windowBits 10
```

- **Refusals** (STATUS `error`, PROGRESS state 6): `invalid_target` (version
  not newer than the running one by `stick_fw::compareVersions`, size outside
  100,000–6,553,600, malformed sha), `device_busy` (a transfer runs),
  `trial_active` (14), `low_battery` (13; below 30 % without external power),
  `insufficient_storage`, `insufficient_memory` (queue).
- **Accept** (on the sync worker, `pump()`): Wi-Fi is stopped (up to 10 s for
  the driver to go down), the chunk queue is allocated, and
  `FirmwareReceiver` opens `/.crosspoint/studio/firmware.tmp`. A partial of the
  same sha256 resumes at its last block boundary: `firmware.meta` holds
  `"<sha> <size> <rawDone> <streamOffset>"`, the prefix is rehashed and the
  file reopened at `rawDone`. PROGRESS state 1 reports `received` = the stream
  offset to continue from.
- **Receive** (`lib/ProjectStick/FirmwareTransfer`, host-tested with the mini
  program's vector `test/project_stick_core/fixtures/ble-fw4-vector.json`):
  each record must start at the raw bytes written so far; it is staged in
  `firmware.z`, inflated with uzlib (1 KB dictionary) block by block into
  `firmware.tmp` with a running SHA-256, and checkpointed in `firmware.meta`.
  Nothing larger than a record is held in RAM.
- **Install:** at `size` bytes the SHA-256 is compared (`checksum_mismatch`
  deletes the partial). PROGRESS state 3 (committing) is notified while the
  link is still up; then STATUS `ota` reports `verifying`, NimBLE is released
  and the shared SD install routine (`firmware_install::installFromSd`: trial
  guard, descriptor check, flash, trial arm) runs; `restarting` precedes the
  restart. A failure restores the radio and reports the reason.
- **Firmware area** (2.7.10): with `firmware.area` on the card (6,553,600
  bytes, created with the frame slots) the image is written to its first
  `size` bytes instead of growing `firmware.tmp`; the fw4 checkpoint reads
  `"<sha> <size> <raw done> <stream offset> area"`. Validation, flashing, the
  descriptor check and hashing take the image length (an SD-card `.bin` is
  still the whole file). The area is never deleted; a finished or rejected
  image only loses `firmware.meta`. The free-space refusal then only needs
  room for the block stage.
- The Wi-Fi path (Settings 「检查更新」) and the SD path end in the same routine.
  The Wi-Fi download writes `firmware.area` with `firmware.meta` =
  `"<sha> <size> area <bytes stored>"` (updated after every 256 KB flush), or
  `firmware.tmp` with `"<sha> <size>"` without the area, and resumes with
  `Range`; a partial of the other kind or place is restarted.
- Simulator: `/.crosspoint/studio/import.fw4` (the decrypted record stream) +
  `import.fw4.json` (`{sha256, size[, stop_at]}`) runs the receiver at boot.

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
the setup QR) and accepts `scan`/`wifi` ops in bound mode (`ota` 2.4.0–2.7.3, replaced by `fw4`);
bound STATUS also carries `fw`, `wifi`, `scan`, `networks` and `ota` (since
2.7.1 `wifi.state` follows the real link on every page: `connected` with the
SSID whenever Wi-Fi is up, not only after a BLE Wi-Fi push; since 2.7.2
`wifi.saved` says a network is saved and the radio comes up on demand), and the
advertised name is `StockStick-XXXX` (scan response; the advertising packet
carries the service UUID and the id characters as manufacturer data). Protocol 3 is defined in Project.StockStick
`docs/product/BLE-SETUP.md` and `BLE-ONLY-DELIVERY.md`; see also
[project-stick.md](project-stick.md#ble-setup-protocol-3-since-230).

## OTA

Three ways in, one install: the mini program streams the image over BLE (op
`fw4`, above), Settings 「检查更新」 downloads it over Wi-Fi (NimBLE released
first, resumable with `Range`), or a `.bin` is picked from the SD card. Each
lands on the SD card and goes through `firmware_install::installFromSd`
(SHA-256, image descriptor, flash, trial boot with automatic rollback). The complete
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
