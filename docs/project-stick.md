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
built in Simplified Chinese; other languages load from SD-card packs.

## Runtime flow

1. Cold boots, ordinary restarts, and quick-resume wakeups open
   `ProjectStickActivity` by default. Recovery firmware mode, crash reporting,
   and explicit silent-restart targets retain their dedicated routes; holding
   Back during boot opens the system menu (StockStick / Settings) instead.
2. The activity opens immediately and reconnects to saved Wi-Fi networks in the
   background (last network first, 15 s per attempt, exponential backoff up to
   5 min), so boot, deep-sleep wake and OTA restarts come back online without
   user input; a bound device registers on the offline->online edge.
3. The service keeps a persistent UUID v4 identity. An unbound device makes no
   cloud requests: it runs BLE setup mode and its status screen shows
   `用小程序扫码绑定` with the setup QR `stockstick://setup?d=<device_id>&k=<K>`
   (K: 16 random bytes generated in RAM on entering the unbound state; QR
   version 5-L). The phone binds the device (the device token and the BLE
   authority arrive over the encrypted session) and pushes Wi-Fi over
   Bluetooth. See "BLE setup" below. The token is persisted on SD but never
   written to logs or rendered.
4. A bound device sends the `/api/v2/device/register` heartbeat every 6 hours
   and on coming online; it returns `bound`, `owner_id`, `server_time`,
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
   rollbacks are persisted in the state file and uploaded after a heartbeat;
   the queue is capped at 32.

## BLE setup (protocol 3, since 2.3.0)

The wire protocol is defined in Project.StockStick
`docs/product/BLE-SETUP.md`; the firmware side lives in
`src/project_stick/StudioBluetooth.cpp` (GATT, sessions, ops),
`lib/ProjectStick/BleSetupProtocol.cpp` (MAC strings, AES-256-CTR seal, QR
payload, capped STATUS network list; host-tested against the shared vectors)
and `ProjectStickActivity::tickBleSetup` (applies queued work).

- Advertised name `StockStick-XXXX` (first four hex digits of the device id)
  in both modes. Unbound: setup mode keyed with K; bound: the secret delivered
  by `bind` (protocol 2 `begin`/`commit`, plus `scan`/`wifi`/`ota`).
- Each connection pins its key. After `bind` the live setup session keeps K
  until the phone disconnects, so the phone can bind and then push Wi-Fi in one
  connection; the next connection is in bound mode.
- NimBLE callbacks only verify and queue. The activity loop applies a bind
  (`ProjectStickService::applyBleBinding`: token, owner, `bound=true`; then the
  BLE credential is persisted by `studio_ble::finishBinding`), a Wi-Fi join (saved to `WifiCredentialStore` and marked last
  connected; auto-connect stands aside; 20 s, errors `no_ap` /
  `wrong_password` / `timeout`) and an async scan (top 5 by RSSI, STATUS kept
  within 512 bytes).
- `ota` (bound mode only): the phone names a catalogue image (`version`, `url`,
  `sha256`, `bytes`); the activity hands it to the background worker, which
  downloads it over Wi-Fi and installs it (see [firmware-ota.md](firmware-ota.md)).
  Only URLs under `<API base>/firmware/` are accepted. BLE pauses during the
  download; STATUS `ota` reports `queued`/`downloading`/…/`failed` with the
  error, and bound STATUS carries `fw` so the phone can confirm the new version
  after the restart.
- Cloud requests wait while a phone is connected over BLE; the offline->online
  edge and the first registration after a BLE bind run once it disconnects.
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
service. Every request sends `Authorization: Bearer <device_token>`; without a
token (unbound) the device makes no requests.

## X3 controls

All StockStick strings come from the built-in Simplified Chinese catalogue
(`lib/I18n/translations/chinese.yaml`); an SD-card language pack can override
them (see [i18n.md](i18n.md)).

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
  5 seconds without one. Front labels: 返回 / Wi-Fi / – / 换一张 (the last one
  only while a Studio card is shown). On a Studio card the side pills are thumb-down 没啥用
  (left) and 有用 thumb-up (right). Hints never show while locked.
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
  unlocking right release is consumed (it never sends feedback).
- With a Studio frame installed: the left side key is "没啥用" (show the next
  card, `studio_next`) and the right side key is "有用" (keep the card,
  `studio_useful`). Each shows a feedback card (white, 16 px radius, 2-pixel
  outline, Noto Sans SC 12 regular) with its thumb icon that slides
  from its key's edge to the centre in discrete frames over 360 ms
  (`没啥用！那试试下一条` / `有用！那就去执行`) and clears after 2.2 s. The
  front-right key selects the next card without feedback (local only, no
  cloud request).
- Without a Studio frame, Back opens the system menu (StockStick / Settings)
  and Confirm opens Wi-Fi selection; the front-right key is unassigned.

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
  at startup through the same `StudioFrame` path BLE uses, then deletes it.

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
  events have explicit caps (32 each).
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
