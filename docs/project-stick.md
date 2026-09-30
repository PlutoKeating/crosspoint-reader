# Project.Stick integration

> See also: [Studio protocol](studio-protocol.md), [firmware versioning and OTA](firmware-ota.md),
> [i18n](i18n.md).

This private fork turns upstream CrossPoint into the dedicated StockStick
firmware. Since 2.0.0 the ebook reader, library, web file transfer, OPDS,
KOReader and Calibre integrations are removed; since 2.1.0 the legacy
"Release" companion pipeline (manifest polling, release snapshots, JSON
schedule/copy rotation and display themes) is removed as well. Everything the
device shows is a Studio frame or program rendered by the mini program. The
firmware keeps the device platform (display, input, power, SD, Wi-Fi, BLE,
settings, SD/recovery flashing). X3 is the product target; X3/X4 runtime
detection is kept so other panels can be added later.

The firmware identity lives in `platformio.ini` (`version = 2.1.0`,
`build = 20100`) and is embedded in every image; see
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
   user input; the offline->online edge registers and polls immediately.
3. The service keeps a persistent UUID v4 identity and a device bearer
   credential, persisted on SD but never written to logs or rendered. An
   unbound device (since 2.3.0) runs BLE setup mode and its status screen shows
   `用小程序扫码绑定` with the setup QR `stockstick://setup?d=<device_id>&k=<K>`
   (K: 16 random bytes generated in RAM on entering the unbound state; QR
   version 5-L). The phone binds the device and pushes Wi-Fi over Bluetooth —
   no working cloud link is needed, and sync errors never replace this screen
   while unbound. See "BLE setup" below. As a fallback, while online every sync
   also re-pairs over `/api/v2/device/pairing`, which refreshes the
   eight-character binding code (valid for 10 minutes); it is shown under the
   QR as `或输入绑定码 XXXXXXXX`. If the server already committed the claim
   (pairing answers 409), registration continues and returns the authoritative
   `bound` state. An unbound identity whose token the server rejects (401) is
   replaced by a fresh identity, unless a BLE bind replaced the token in the
   meantime (pairing/registration responses that raced a BLE bind are dropped).
4. `/api/v2/device/register` returns `bound`, `owner_id`, `server_time`,
   `is_trading_day` and the register/alert poll intervals. A successful
   registration corrects the working clock; an owner change clears Studio data,
   pending events and alert state and rotates the BLE credential.
5. While Wi-Fi is up and no BLE session is active, the Studio target is polled
   every 5 seconds (`/api/v2/device/studio`, see
   [studio-protocol.md](studio-protocol.md)). A register heartbeat runs at the
   server-provided poll interval (every 4 hours once bound).
6. Until a Studio frame is installed, a bound device shows `还没有内容` and
   `请在小程序发布官方计划或卡片` (offline: `可在小程序设备页通过蓝牙设置 Wi-Fi`). The mini program
   seeds every account with the official scenes and plan and publishes that plan
   right after binding, so this screen is normally transient.
7. Studio feedback events (`studio_next`, `studio_useful`) are persisted in the
   state file and uploaded after the Studio poll; the queue is capped at 32.

## BLE setup (protocol 3, since 2.3.0)

The wire protocol is defined in Project.StockStick
`docs/product/BLE-SETUP.md`; the firmware side lives in
`src/project_stick/StudioBluetooth.cpp` (GATT, sessions, ops),
`lib/ProjectStick/BleSetupProtocol.cpp` (MAC strings, AES-256-CTR seal, QR
payload, capped STATUS network list; host-tested against the shared vectors)
and `ProjectStickActivity::tickBleSetup` (applies queued work).

- Advertised name `StockStick-XXXX` (first four hex digits of the device id)
  in both modes. Unbound: setup mode keyed with K; bound: the cloud-issued
  secret (protocol 2 `begin`/`commit` unchanged, plus `scan`/`wifi`).
- Each connection pins its key. After `bind` the live setup session keeps K
  until the phone disconnects, so the phone can bind and then push Wi-Fi in one
  connection; the next connection is in bound mode.
- NimBLE callbacks only verify and queue. The activity loop applies a bind
  (`ProjectStickService::applyBleBinding`: token, owner, `bound=true`, pairing
  code cleared, no BLE revoke; then the BLE credential is written like
  `configure`), a Wi-Fi join (saved to `WifiCredentialStore` and marked last
  connected; auto-connect stands aside; 20 s, errors `no_ap` /
  `wrong_password` / `timeout`) and an async scan (top 5 by RSSI, STATUS kept
  within 512 bytes).
- Cloud requests wait while a phone is connected over BLE; the offline->online
  edge and the first registration after a BLE bind run once it disconnects.
- `studio_ble::revoke()` is a no-op in setup mode, so the unbound register
  heartbeat never tears down a setup session. The desktop/web simulator has no
  radio but still renders the setup QR.

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
service. Every request after pairing sends `Authorization: Bearer <device_token>`.

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
  5 seconds without one. Front labels: 返回 / Wi-Fi / – / 同步 (换一张 while a
  Studio card is shown). On a Studio card the side pills are thumb-down 没啥用
  (left) and 有用 thumb-up (right). Hints never show while locked.
- A device-wide Nokia-style keyguard (since 2.2.0 also on the Studio card page)
  locks after 20 seconds without button or touch activity: all non-power
  button events are suppressed and only a small lock icon is added at the top
  left (a lock icon in a small outline circle); content keeps updating
  underneath. The power button always works and
  cloud synchronization continues.
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
  front-right key selects the next card without feedback.
- Without a Studio frame, Back opens the system menu (StockStick / Settings),
  Confirm opens Wi-Fi selection, and the front-right key registers and polls
  Studio immediately.

## Desktop simulator

Build and launch the same Project.Stick activity used by the firmware:

```bash
pio run -e simulator -t run_simulator
```

The simulator uses the production API by default and persists its simulated SD
state under `fs_/.crosspoint/`. `CROSSPOINT_SIM_FORCE_ALERT_WINDOW=1` lets alert
polling run outside trading hours, `CROSSPOINT_SIM_POLL_ALERT_ONCE=1` polls
alerts once after the first successful sync, and
`CROSSPOINT_SIM_FAIL_FIRST_REGISTER=1` injects one register failure.

## X3 memory boundaries

- Register, alert and Studio JSON bodies are capped (alerts 32 KiB, Studio
  4 KiB) and parsed with ArduinoJson from a bounded string.
- Every HTTP GET completes before the next begins; one sync burst reuses the
  same keep-alive TLS connection.
- Studio polls (every 5 s) keep that connection between polls only while the
  heap has at least 80 KiB free with a 32 KiB contiguous block; the sync worker
  closes it after 45 s without work. A stale connection is retried once on a
  fresh one by SecureHttpClient.
- Studio frames and programs stream directly to SD; seen alerts and pending
  events have explicit caps (32 each).
