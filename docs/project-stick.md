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
   credential obtained from `/api/v2/device/pairing`. The credential is persisted
   on SD but never written to logs or rendered. While unbound, every sync
   re-pairs, which refreshes the eight-character binding code (valid for 10
   minutes on the server), and the screen shows the code and its
   `stockstick://bind?code=` QR. If the server already committed the claim
   (pairing answers 409), registration continues and returns the authoritative
   `bound` state. An unbound identity whose token the server rejects (401) is
   replaced by a fresh identity.
4. `/api/v2/device/register` returns `bound`, `owner_id`, `server_time`,
   `is_trading_day` and the register/alert poll intervals. A successful
   registration corrects the working clock; an owner change clears Studio data,
   pending events and alert state and rotates the BLE credential.
5. While Wi-Fi is up and no BLE session is active, the Studio target is polled
   every 5 seconds (`/api/v2/device/studio`, see
   [studio-protocol.md](studio-protocol.md)). A register heartbeat runs at the
   server-provided poll interval (every 4 hours once bound).
6. Until a Studio frame is installed, a bound device shows `还没有内容` and
   `请在小程序发布官方计划或卡片` (or a Wi-Fi prompt when offline). The mini program
   seeds every account with the official scenes and plan and publishes that plan
   right after binding, so this screen is normally transient.
7. Studio feedback events (`studio_next`, `studio_useful`) are persisted in the
   state file and uploaded after the Studio poll; the queue is capped at 32.

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

- All hints share one 1-bit visual language (since 2.2.1): black rounded chips
  (NOTO Sans SC 12 bold, white text, 38 px high, 20 px icons) with a 2-pixel
  white halo so they never merge with card pixels; feedback windows are white
  cards with a 2-pixel border in the same radius family.
- Any key action pops up the button hints and they hide after 5 seconds
  without one. Front chips sit above the four physical front keys: 返回 /
  Wi-Fi / – / 同步 (换一张 while a Studio card is shown), the unassigned slot
  stays empty. On a Studio card the side keys get edge tabs flush with the
  screen edge: thumb-down 没啥用 (left) and 有用 thumb-up (right). Hints never
  show while locked.
- A device-wide Nokia-style keyguard (since 2.2.0 also on the Studio card page)
  locks after 20 seconds without button or touch activity: all non-power
  button events are suppressed and only a small lock icon is added at the top
  left; content keeps updating underneath. The power button always works and
  cloud synchronization continues.
- Unlock by releasing the left side key, then the right side key. A key press
  while locked shows the chip `屏幕已锁定 · 依次按左、右侧边键` with step tabs 1
  (black, next) and 2 (outlined) at the side keys; after the left step the
  prompt becomes `左键已确认 · 请按右侧边键`, the left tab shows a check, the
  right tab turns black and chevrons walk toward it in three 150 ms frames.
  Right-first does nothing, a front key restarts at the left step, and the
  unlocking right release is consumed (it never sends feedback).
- With a Studio frame installed: the left side key is "没啥用" (show the next
  card, `studio_next`) and the right side key is "有用" (keep the card,
  `studio_useful`). Each shows a feedback card with its thumb icon that slides
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
