# Project.Stick integration

> See also: [Studio protocol](studio-protocol.md), [firmware versioning and OTA](firmware-ota.md),
> [i18n](i18n.md).

This private fork turns upstream CrossPoint into the dedicated StockStick
firmware. Since 2.0.0 the ebook reader, library, web file transfer, OPDS,
KOReader and Calibre integrations are removed; since 2.1.0 the legacy
"Release" companion pipeline (manifest polling, release snapshots, JSON
schedule/copy rotation and display themes) is removed as well; since 2.4.0
content arrives over BLE only (no cloud content delivery, no pairing codes, no
cloud command queue). Everything the device shows is a Studio frame or program
rendered by the mini program and written to the device over Bluetooth. The
firmware keeps the device platform (display, input, power, SD, Wi-Fi, BLE,
settings, SD/recovery flashing). X3 is the product target; X3/X4 runtime
detection is kept so other panels can be added later.

The firmware identity lives in `platformio.ini` (`version`, `build`) and is embedded in every image; see
[firmware-ota.md](firmware-ota.md) for release and upgrade rules. The UI is
built in Simplified Chinese (the only UI language since 2.7.5).

## Runtime flow

1. Cold boots, ordinary and silent restarts, and quick-resume wakeups open
   `ProjectStickActivity`, the device's home (there is no launcher since
   2.7.5). Recovery firmware mode and crash reporting keep their dedicated
   routes; holding Back during boot opens Settings instead.
2. The activity opens immediately. `ProjectStickHost` (ticked from the main
   loop, see "Page-independent host" below) reconnects to saved Wi-Fi networks
   in the background (last network first, 15 s per attempt, exponential backoff
   up to 5 min) on every page, so boot, deep-sleep wake and OTA restarts come
   back online without user input; a bound device registers on the
   offline->online edge.
3. The service keeps a persistent UUID v4 identity. An unbound device makes no
   device API requests (the public firmware catalogue is the one thing it may
   ask, from Settings > Firmware update): it runs BLE setup mode and its status screen shows
   `用小程序扫码绑定` with the setup QR `stockstick://setup?d=<device_id>&k=<K>`
   (K: 16 random bytes generated in RAM on entering the unbound state; QR
   version 5-L). The phone binds the device (the device token and the BLE
   authority arrive over the encrypted session) and pushes Wi-Fi over
   Bluetooth. See "BLE setup" below. The token is persisted on SD but never
   written to logs or rendered.
4. A bound device sends the `/api/v2/device/register` heartbeat every 6 hours
   and on coming online, unless a phone synced it over BLE within the last
   6 hours (2.6.0, "Phone-relayed sync" in studio-protocol.md: the phone
   relays firmware, metrics and events and hands back clock and trading day);
   it returns `bound`, `owner_id`, `server_time`,
   `is_trading_day`, the alert interval and the current BLE authority (a
   rotated key is adopted, see "Cloud requests" in studio-protocol.md). A successful registration corrects
   the working clock. `bound: false` (unbound in the mini program) or a 401
   drops the token, the Studio data and the BLE authority, and the device is
   back in setup mode. Apart from trading-hours alerts nothing is polled:
   content arrives over BLE only.
   429 / 5xx / unreachable back off 30 s → 10 min (429 honours `Retry-After`);
   see "Cloud requests" in [studio-protocol.md](studio-protocol.md).
5. Until a Studio frame is installed, a bound device shows `还没有内容` and
   `手机靠近设备，在小程序发布计划或卡片，经蓝牙传输`. The mini program publishes
   the official plan over BLE right after binding, so this screen is normally
   transient.
6. Studio feedback events (`studio_next`, `studio_useful`) and firmware
   rollbacks are persisted in the state file and drained by the phone over BLE
   (STATE `events`, `sync` `ack`) or, failing that, uploaded after a heartbeat;
   the queue is capped at 32.

## Page-independent host (since 2.7.1)

Until 2.7.0 everything a phone asks for over BLE was applied by
`ProjectStickActivity`: the radio (`studio_ble::tick`) ran on every page, but
bind, `sync`, `unbind`, Wi-Fi join/scan, `ota` (now `fw4`), the STATE refresh and the setup
key of an unbound device only existed while the StockStick page was open. On
Home or in Settings a phone could connect and nothing it sent was executed.

`ProjectStickHost` (`src/project_stick/ProjectStickHost.{h,cpp}`, started once
in `setup()` after the display, ticked from the main loop right after
`studio_ble::tick()`, same UI task) now owns all of it:

- BLE requests: `takeBinding`/`finishBinding`, `takeSyncRequest`,
  `takeUnbindRequest`, `takeWifiRequest` + join polling, `takeScanRequest` +
  scan polling. A BLE firmware transfer (`fw4`, 2.7.4) runs entirely in
  `studio_ble::pump()` on the sync worker.
- STATE: rebuilt every 2 s while a phone is connected, every 10 min otherwise,
  and right after a `sync`.
- STATUS `wifi.state` mirrors the real link: `connected` + SSID whenever Wi-Fi
  is up (a saved network joined by itself counts), `idle` when it drops; a
  failed BLE join stays readable until a link comes up.
- Setup mode: an unbound device gets its one-time key and starts advertising on
  any page; the QR payload is kept for the status screen.
- Wi-Fi auto-connect (`WifiAutoConnect`), suspended only while
  `WifiSelectionActivity` is open (it drives the radio itself).
- Cloud: the register heartbeat, trading-hours alert polls, their results
  (clock, activation, alert display), the RTC write-back and ownership changes.
- Power: publishes "external power is charging" for the install guard, and
  keeps the CPU at full speed (no idle power saving) while a phone is
  connected, a transfer, a BLE Wi-Fi job, a cloud job or a firmware update runs.
  There is no timed power-off at all since 2.7.3 (the card is a continuous
  low-power display): only a power-key long press puts the device to sleep.
- Page switches (`StickTakeover`, `project_stick::shouldShowStickPage`,
  host-tested): incoming content and a firmware update are shown by the
  StockStick page, so the host switches to it from ordinary pages. Without
  this a delivery made while the device sat in Settings was installed but
  never displayed, and the phone waited for a receipt that could not come.
  Boot, sleep, the crash report, SD flashing and a firmware check/install in
  progress are never replaced; the firmware screen shows update progress
  itself.

`ProjectStickActivity` keeps what belongs to its screen: the card, key hints,
feedback bubbles and notices. It shares the host's `ProjectStickService`.

## Credential loss and the `token` op (since 2.7.2)

A 2.6.x device could end up bound over BLE (`/.crosspoint/studio/ble.json`
holds identity, owner and secret) while its store
(`/.crosspoint/project_stick.json`) said unbound with no cloud token. Then the
on-device check said 「请先在小程序绑定设备」 and a phone-triggered `ota` failed
with `invalid_target` (2.6.5 `ProjectStickService.cpp:867`). Mechanism, all in
2.6.5:

1. `PersistableStoreBase::readDocFromFile` (`lib/Serialization/PersistableStore.cpp:42-56`)
   reads the whole file with `Storage.readFile` into an Arduino `String`. On a
   heap like the measured one (13 KB free, 10.7 KB largest block) that
   allocation fails, `readFile` returns an empty string, and the read counts
   as "no file" — for the backup too.
2. `ProjectStickService::begin` (`ProjectStickService.cpp:198-202`) then runs
   `ensureIdentity()` on the default store, creates a new device id and calls
   `saveToFile()`: the good file is rotated to `.bak` and replaced by
   defaults (`bound:false`, no token). The next save rotates the good copy
   away for good.
3. `ble.json` is a separate file and survives, so the phone still verifies the
   device and it still answers BLE, but the cloud credential is gone.
   Writes had the mirror-image weakness (`writeDocToFile` serialized into a
   `String`, which truncates when its growth fails; `writeCredentials` truncated
   `ble.json` in place before streaming into it).

2.7.2:

- Stores parse from the file, report `Missing`/`Corrupt`/`Unavailable`, and a
  store whose file exists but could not be read never saves (boot retries the
  load 3×). Writes refuse overflowed documents, stream to `.tmp`, verify the
  byte count and the size on the card, then rotate (`.bak` kept). `ble.json`
  uses the same writer; StudioFrame's state checks overflow and length.
- `ProjectStickService::loadStore` heals the split at boot: if `ble.json`
  holds an authority, the store takes its identity and owner and becomes
  bound again. Only the cloud token is missing then.
- A BLE bind is all or nothing: if `ble.json` cannot be written, the store
  binding is undone. `revoke()` removes `ble.json`, its `.bak` and `.tmp`, so a
  revoked authority cannot be resurrected from the backup.
- STATE `cred` (1 = holds a cloud token). Op `token` (bound mode):
  `{op:"token", owner, ct, proof}`, `ct = seal(secret, "token3", n, token)`,
  `proof = mac(secret, "token3|n|owner|ct")`; owner must match the authority's
  owner. The owner's phone re-claims the device (`ble-claim`, same owner keeps
  the BLE secret) and writes the new token; failures show as `bind_failed` /
  `authorization_failed` / `invalid_control`. Vector:
  Project.StockStick `miniprogram/tests/fixtures/ble-token-vector.json`,
  checked by `BleSetupProtocol.TokenVector`.

## Wi-Fi on demand (since 2.7.2)

See memory-budget.md "2.7.2". STATUS `wifi.saved` is true when at least one
network is saved (the device joins on demand); `wifi.state` stays
`idle|connecting|connected|failed` and reflects the real link, so `idle` with
`saved:true` is a healthy device. The status header shows 「在线」 when
connected and 「Wi‑Fi 待机」 when a network is saved but the radio is down.
The Wi-Fi and firmware pages bring the radio up themselves. Since 2.7.4 the
radio stays off while a phone is linked (background jobs wait for it to
leave); a BLE Wi-Fi join or scan and an on-device firmware job still bring it
up (studio-protocol.md "Cloud jobs and a linked phone").

## BLE setup (protocol 3, since 2.3.0)

The wire protocol is defined in Project.StockStick
`docs/product/BLE-SETUP.md`; the firmware side lives in
`src/project_stick/StudioBluetooth.cpp` (GATT, sessions, ops),
`lib/ProjectStick/BleSetupProtocol.cpp` (MAC strings, AES-256-CTR seal, QR
payload, capped STATUS network list; host-tested against the shared vectors)
and `ProjectStickHost::tickBle` (applies queued work, on every page).

- Advertising (since 2.5.0): the 31-byte packet carries the flags, the 128-bit
  service UUID and manufacturer data `FF FF` + the four id characters; the
  complete name `StockStick-XXXX` (first four hex digits of the device id) is
  in the scan response, because it does not fit next to the UUID. Same in both
  modes.
- Radio lifecycle (`studio_ble::tick`, UI loop): a failed start (low heap,
  controller or host init, advertising) is recorded and retried with backoff
  (5 s … 60 s); an advertising watchdog restarts advertising, then the stack,
  when the controller stops on its own; a phone that holds the link idle for
  5 min is dropped. `HalPowerManager` never lowers the CPU clock while the BLE
  controller is up (the controller needs the 80 MHz APB clock).
- Settings > System > Bluetooth (`BluetoothActivity`): live state, advertised
  name, address, last start error, counters and heap, plus the on/off switch
  (`settings.json` `bluetoothEnabled`) and a stack restart.
- Status notices (`stick_overlay::drawNotice`, driven by
  `ProjectStickActivity::updateNotice`): phone connected, receiving with a
  progress bar, verifying, done/failed, Wi-Fi join and scan, cloud heartbeat.
  Busy notices animate once per `NOTICE_FRAME_MS`. The work itself runs in
  `ProjectStickHost`; the page reads its state and one-shot events.
- Unbound: setup mode keyed with K; bound: the secret delivered
  by `bind` (transfer protocol 4 `begin4`/DATA/`commit`, see studio-protocol.md,
  plus `scan`/`wifi`/`fw4`).
- Each connection pins its key. After `bind` the live setup session keeps K
  until the phone disconnects, so the phone can bind and then push Wi-Fi in one
  connection; the next connection is in bound mode.
- Content transfers (protocol 4, 2.7.0): the callbacks decrypt into a static
  chunk queue (heap, allocated per transfer since 2.7.2) and `studio_ble::pump()`
  on the background sync worker rebuilds the program through `StudioReceiver`:
  only frames the device lacks arrive, deflate-compressed; held frames are
  copied from the kept program files. See memory-budget.md for the 2.4.3
  host-task stack overflow that keeps SD I/O out of the callbacks.
- NimBLE callbacks only verify and queue. `ProjectStickHost` (main loop, any page) applies a bind
  (`ProjectStickService::applyBleBinding`: token, owner, `bound=true`; then the
  BLE credential is persisted by `studio_ble::finishBinding`), a Wi-Fi join (saved to `WifiCredentialStore` and marked last
  connected; auto-connect stands aside; 20 s, errors `no_ap` /
  `wrong_password` / `timeout`) and an async scan (top 5 by RSSI, STATUS kept
  within 512 bytes).
- `fw4` (bound mode only, 2.7.4; replaces `ota`): the phone streams the
  firmware image itself over the protocol 4 session, block-compressed, straight
  to `firmware.tmp` on the SD card with block-boundary resume; at the end the
  device verifies it and runs the SD install (studio-protocol.md "Firmware
  over BLE"). STATUS `ota` reports `verifying`/`installing`/`restarting` or
  `failed`, and bound STATUS carries `fw` so the phone can confirm the new
  version after the restart. STATE `capabilities.ota` is 4.
- Loop hygiene (2.6.3): the main loop sleeps 2 ms per iteration instead of a
  busy `yield()` whenever a page asks for no delay; the feedback-bubble check
  no longer takes the render lock (it used to wait out every e-paper refresh,
  stalling the BLE chunk queue); the receiving notice repaints every 3 s
  instead of 1.2 s; the STATE characteristic is rebuilt when a phone connects,
  every 2 s while connected and every 10 min otherwise; alert polls are only
  requested inside the A-share trading window and the store is saved only when
  an alert or the trading-day flag changed; boot hashes each program file once.
- Streamed cards (2.7.4, **disabled since 2.7.6**): `StudioFrame::stream`
  can send a card from the SD card to the panel in strips with the overlays
  drawn into each strip, freeing the 52 KB framebuffer while it shows. The X3
  driver reports no strip support since 2.7.6 (2.7.4/2.7.5 boot-looped on a
  real X3; memory-budget.md "2.7.6"), so cards render through the resident
  framebuffer until the path is validated on hardware.
- `StudioFrame::render` (2.6.4) holds the frame lock only while choosing the
  file and offset; the SD read and pixel loop run unlocked on the render task.
  Before, every repaint stalled the UI loop for hundreds of milliseconds and a
  side key released during the repaint that a first key press triggers (the
  hints appearing) was never seen, so the click did nothing.
- `sync` / `unbind` (bound mode only, 2.6.0): the phone relays the STATE
  characteristic to the cloud and acknowledges it, or drops the binding after
  an unbind in the mini program; see "Phone-relayed sync" in studio-protocol.md.
- Cloud requests wait while a phone is connected over BLE; the offline->online
  edge and the first registration after a BLE bind run once it disconnects,
  and not at all while a phone sync is fresh.
- `studio_ble::revoke()` is a no-op in setup mode. The desktop/web simulator has
  no radio but still renders the setup QR.

## Market alerts

On trading days between 09:30–11:30 and 13:00–15:00 (Shanghai) the device polls
`/api/v2/device/alerts` at the server-provided interval. Each unseen alert
extends the persisted `alertUntil` to its `active_until`; the device keeps no
alert copy of its own. `StudioFrame::tick` shows the installed program's alert
scene (or overlays a permitted temporary card) while `alertUntil` lies in the
future. The last 32 alert IDs are remembered so a restart does not replay them.

The service uses the device RTC as the offline clock and corrects its working
clock whenever the API returns `server_time`. The desktop simulator reads the
host UTC wall clock.

## Configuration

The production service defaults to:

```text
https://stockstick.plutokeating.beer
```

TLS pins ISRG Root X2, ISRG Root X1 and GTS Root R4 (`src/project_stick/StudioTrust.h`);
the production certificate currently chains to ISRG Root X2.

Override it at build time without editing source:

```ini
build_flags =
  ${base.build_flags}
  -DPROJECT_STICK_BASE_URL=\"http://192.168.1.10:3000\"
```

The authenticated HTTP API path prefix (`/api/v2/device`) is appended by the
service. Every device API request sends `Authorization: Bearer <device_token>`;
without a token (unbound) the device makes none. The firmware catalogue
(`/api/v1/public/firmware/latest`) and the image download are public and need
only Wi-Fi.

## X3 controls

All StockStick strings come from the built-in Simplified Chinese catalogue
(`lib/I18n/translations/chinese.yaml`); the firmware is Chinese-only since
2.7.5 (see [i18n.md](i18n.md)).

- Every key hint on the device, on every screen, comes from one
  implementation (`src/components/StickOverlays.cpp`, since 2.2.2); the
  themes' `drawButtonHints` / `drawSideButtonHints` delegate to it and are not
  themeable. One light outline language: white fills, 2-pixel black outlines,
  Noto Sans SC 10 regular labels (about 20 px), 20 px 2-stroke icons.
  - Front keys: a white bar across the bottom 64 px (it covers whatever is
    underneath) with a 2-pixel rule on top; each assigned label sits over its
    physical key with a small solid triangle pointing down at it. Unassigned
    keys leave their slot empty. Themes reserve the bar as `buttonHintsHeight`.
  - Side keys: white pills (40 px high, fully rounded) inset 10 px from the
    edge, centred on the key, with a small solid triangle pointing at it.
    Themes reserve 56 px per side as `sideButtonHintsWidth`.
- On the StockStick page any key action pops up the hints and they hide after
  5 seconds without one. Front labels (2.7.5): 设置 / – / – / 换一张 (the last one
  only while a Studio card is shown). On a Studio card the side pills are thumb-down 没啥用
  (left) and 有用 thumb-up (right). Hints never show while locked; since
  2.6.1 they show right after an unlock and then run the same 5 s countdown.
- A device-wide Nokia-style keyguard (since 2.2.0 also on the Studio card page)
  locks after 20 seconds without button or touch activity: all non-power
  button events are suppressed and only a small lock icon is added at the top
  left (a lock icon in a small outline circle); content keeps updating
  underneath, on every screen: since 2.4.2 the activity manager renders the
  current activity under the lock and draws the lock on top (before, screens
  other than the StockStick page froze on their last frame, e.g. a firmware
  check stayed on 「正在检查更新…」 after its result arrived). The power button always works and
  BLE delivery and the cloud heartbeat continue.
- Unlock by releasing the left side key, then the right side key. A key press
  while locked shows `屏幕已锁定 · 依次按左、右侧边键` in the front bar and step
  pills `1 先按` (filled black: the key to press next, the only filled hint)
  and `2 再按` (outlined) at the side keys; after the left step the prompt
  becomes `左键已确认 · 请按右侧边键`, the left pill turns into an outlined
  `已按` with a check, the right pill is filled and outline chevrons walk
  toward it in three 150 ms frames.
  Right-first does nothing, a front key restarts at the left step, and the
  unlocking right release is consumed (it never sends feedback). Since 2.6.1
  the guide hides again after 5 s without a key action (`PROMPT_TIMEOUT_MS`),
  and a left step taken before that is forgotten; the next key action shows
  the guide from step 1.
- With a Studio frame installed: the left side key is "没啥用" (show the next
  card, `studio_next`) and the right side key is "有用" (keep the card,
  `studio_useful`). Each shows a feedback card (white, 16 px radius, 2-pixel
  outline, Noto Sans SC 12 regular) with its thumb icon that slides
  from its key's edge to the centre in discrete frames over 360 ms
  (`没啥用！那试试下一条` / `有用！那就去执行`) and clears after 2.2 s. The
  front-right key selects the next card without feedback (local only, no
  cloud request).
- The StockStick page is the device's home (2.7.5: no Home launcher). Its
  front-left key (Back) opens Settings, and Settings' Back returns here;
  holding Back at boot opens Settings directly. Confirm is unassigned (the
  card page's Wi-Fi key was removed in 2.7.5: Wi-Fi comes from the phone over
  BLE, or Settings > Wi-Fi 网络). Without a Studio frame the front-right key is
  unassigned too.

## Desktop and web simulator

Build and launch the same Project.Stick activity used by the firmware:

```bash
pio run -e simulator -t run_simulator
```

The simulator persists its simulated SD state under `fs_/.crosspoint/`. It has
no BLE radio, so content is provisioned by the host before `setup()`
(crosspoint-simulator `SimulatorLifecycle::applyProvisioning`):

- `device_id` (web `Module.simConfig.device_id`, native
  `CROSSPOINT_SIM_CONFIG_DEVICE_ID`): a bound identity **without** a device
  token, so the simulator shows content and makes no cloud requests at all.
- `program` (base64 SSP1; natively also `program_path`, a raw file): written
  to `/.crosspoint/studio/import.ssp`. A SIMULATOR firmware build installs it
  at startup through `StudioFrame` (the same install and verification BLE
  ends in), then deletes it. `import.v4` + `import.v4.json` instead feed a
  decrypted protocol 4 stream through `StudioReceiver` (test hook, see
  studio-protocol.md).

`CROSSPOINT_SIM_FORCE_ALERT_WINDOW=1` lets alert polling run outside trading
hours, `CROSSPOINT_SIM_POLL_ALERT_ONCE=1` polls alerts once after the first
successful heartbeat, and `CROSSPOINT_SIM_FAIL_FIRST_REGISTER=1` injects one
register failure (these need a provisioned device token).

## X3 memory boundaries

- Register and alert JSON bodies are capped (alerts 32 KiB) and parsed with
  ArduinoJson from a bounded string.
- Every HTTP request completes before the next begins; one heartbeat burst
  (register, events) reuses the same TLS connection and closes it afterwards.
- Studio frames and programs stream directly to SD; seen alerts and pending
  events have explicit caps (32 each). Firmware from the phone or the
  download streams to SD the same way, one 32 KB block at a time.
- The framebuffer stays resident on X3 (2.7.6): `releaseFrameBuffer()` is a
  no-op on panels without strip support.
- NimBLE is deinitialised (host and controller heap freed) for a cloud request
  when the heap is below what a TLS handshake needs, for a retry after a
  transport failure, and for a whole firmware install; it is restored with the
  same identity afterwards (see "Cloud requests" in studio-protocol.md).
- The cloud worker's 8 KB stack is static (created at boot), so it cannot fail
  to start on a fragmented heap.
- NimBLE APIs are never called while holding the BLE state mutex, which the
  NimBLE host callbacks take (`radioMutex` serialises init/deinit/advertising).
- The UI loop does not allocate (2.4.3): `now()` and the ISO-8601 parser work
  on fixed buffers, `StudioFrame::tick()` re-evaluates the schedule only when
  the second, the alert or a key changes, `studio::step()` reuses scratch
  buffers, and the loop checks `StudioFrame::hasContent()` instead of copying a
  snapshot. A throwing `new` that fails records the heap in RTC memory before
  aborting, and crash reports include the last heap sample
  (`HalSystem::sampleHeap()`, every 5 s). The full budget and the 2.2.2 crash
  analysis are in `docs/memory-budget.md`.
