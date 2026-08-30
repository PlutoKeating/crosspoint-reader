# Project.Stick integration

This private fork intentionally adds an external synchronization connector that
is outside upstream CrossPoint's temporarily closed scope. Project.Stick targets
the Xteink X3 (ESP32-C3, no PSRAM) only; it is not proposed as an upstream
multi-device feature.

The current production firmware release is `1.5.0-project-stick.10`. It adds
the X3-wide 20-second physical-button keyguard described below and reduces
cloud synchronization latency by reusing one TLS connection per synchronization
burst. Release binaries are built with the same version string from
`platformio.ini`.

## Runtime flow

1. Cold boots, ordinary restarts, and quick-resume wakeups open
   `ProjectStickActivity` by default. Recovery firmware mode, crash reporting,
   and explicit silent-restart targets retain their dedicated routes; holding
   Back during boot remains an escape hatch to the reader Home screen.
2. The activity opens in offline mode immediately. Wi-Fi is optional; when it
   is available the service creates a persistent UUID v4 identity, obtains a
   device bearer credential, and shows the short-lived binding code returned by
   `/api/v2/device/pairing`. The bearer credential is persisted on SD but never
   written to logs or rendered. After the mini program claims the code, the
   service registers as the bound device. If an older backend reports the
   pairing as already claimed before local state has observed `bound=true`, the
   firmware continues to authenticated registration instead of stopping at the
   pairing response.
3. The manifest is streamed through a 512-byte parser and written to a
   temporary SD file. Its entries become a compact release snapshot under
   `/.crosspoint/project_stick/snapshots/<version>.idx`.
4. Register, manifest, changed Release files, and the final event batch reuse
   one HTTP keep-alive/TLS connection for the synchronization burst. Release
   files remain serial to respect the ESP32-C3 memory ceiling. Each SHA-256
   object is stored once at
   `/.crosspoint/project_stick/objects/<sha256>.json`; an already verified SHA
   is reused without another download. Changed objects stream directly to a
   temporary SD file while their byte count and SHA-256 are checked.
5. Only after every object passes validation does
   `/.crosspoint/project_stick.json` atomically switch `active_version`.
   `previous_version` remains the rollback snapshot.
6. The active schedule and persisted content-rotation deadline are checked
   every 30 seconds, the manifest at the server-provided interval, and alerts
   during the documented trading windows. The default rotation interval is
   600 seconds; `0` disables it and other values are clamped to 60-86400 seconds.
   A release-version change invalidates the foreground schedule cache before
   selection, and the foreground clock adopts the Shanghai server time returned
   by each completed background synchronization.
7. Events are persisted in the same state file and retried after connectivity
   returns. Pending telemetry does not delay manifest or Release activation:
   the service combines old and newly produced events into one batch after the
   content-critical path completes. The TLS connection is then closed so its
   runtime memory is not retained between polling bursts.

Content rotation is local and independent from manifest polling. The active
Release's display snapshot, alert expiry, and rotation anchor are persisted as
one coherent runtime state, so restart restores the current page before checking
whether its schedule or deadline changed. At a deadline
the service first re-evaluates the schedule, then selects another weighted,
unused copy from that scenario. The Shanghai-time anchor is persisted across
reboots. Schedule transitions take priority, active alerts pause ordinary
rotation, and useful/meh/manual interactions restart the interval. A failed
selection retains the current display. Automatic selections enqueue the same
`trigger_fired` and `screen_view` events as other display changes; they do not
change the last successful synchronization label.

The `market_open` scenario is the normal non-alert display during trading
sessions. Its device tag is rendered as `盘中常态`; `volatility_alert` may
temporarily override it, while `post_close` remains the all-day fallback.

The service uses the device RTC as the offline scheduling clock and corrects its
working clock whenever the API returns `server_time`. RTC reads retain seconds,
while the desktop simulator reads the host UTC wall clock, so both production
deadlines and accelerated simulator checks use the same absolute wall-time
comparison.

## Configuration

The production service defaults to:

```text
https://stockstick.arr2018.dpdns.org
```

Override it at build time without editing source:

```ini
build_flags =
  ${base.build_flags}
  -DPROJECT_STICK_BASE_URL=\"http://192.168.1.10:3000\"
```

The authenticated HTTP API path prefix (`/api/v2/device`) is appended by the
service. Existing firmware releases continue to use the anonymous v1 protocol;
this firmware sends `Authorization: Bearer <device_token>` on register,
manifest, Release, alert, and event requests. A device can read only the
current Release belonging to its bound user.

The active Release `config.json` supplies
`display.content_refresh_interval_seconds`. Firmware without that field uses
600 seconds. For fast desktop verification only, set
`CROSSPOINT_SIM_CONTENT_REFRESH_SECONDS=5`; this also shortens the simulator's
schedule/deadline check cadence to one second without changing production data.

The same file supplies a finite display schema. Supported theme IDs are
`calm`, `large`, `minimal`, and `information`; unknown values fall back to
`calm`. `text_scale` accepts `compact`, `standard`, or `large`, and `layout`
accepts `focused`, `balanced`, or `dense`. Scenario, tone, and synchronization
metadata each have explicit visibility flags. The firmware never interprets
arbitrary CSS or drawing instructions. These preferences are stored with the
active Release so offline boots render the last synchronized theme.

## X3 controls

Project.Stick is currently a Chinese-language product surface. Its schedule
labels, synchronization timestamp, and feedback confirmations intentionally use
the English fallback catalogue so the required Chinese wording remains stable
even when the reader shell is set to another locale.

- The four protruding front buttons are protected by a global X3 keyguard in
  every activity. After 20 seconds without button or touch activity, the screen
  switches to `按键已锁定` and all non-power button events are suppressed.
- Unlocking follows the Nokia-style physical sequence: release the left side
  button, then release the right side button. The first step is drawn as
  completed and the instruction changes to `左侧键已确认，再按右侧边键`.
  Right-first does nothing; a front-button press during an incomplete sequence
  resets it to the left step. The right-side release that unlocks is consumed,
  so it never leaks through as `feedback_useful`. The power button remains
  available while locked, and cloud synchronization continues in the
  background.

- Release the left side button (`Up`) to send `feedback_meh`, show a left-origin
  white confirmation box with a thin black rounded outline, and immediately
  select another unused copy from the currently active schedule window.
- Release the right side button (`Down`) to send `feedback_useful` and show a
  matching right-origin confirmation box without replacing the displayed copy.
  Both confirmations slide toward the centre and intentionally have no speech
  bubble tail. The X3 renderer uses discrete e-paper-safe frames over 360 ms and
  clears the confirmation after 2200 ms.
- The top of the content area records the Shanghai `HH:MM` of the latest
  completed background manifest synchronization.
- Press the front-right button to re-evaluate the current schedule, select
  another copy, and, when online, check for a new release.
- The front confirm button opens Wi-Fi selection; the front back button returns home.
- Before binding, the content area shows a clear mini-program instruction and
  the current eight-character code. The ordinary content UI appears as soon as
  the next authenticated registration reports `bound=true`.

## Desktop simulator

Build and launch the same Project.Stick activity used by the firmware:

```bash
pio run -e simulator -t run_simulator
```

The simulator uses the production API by default and persists its simulated SD state under `fs_/.crosspoint/`.

To verify local rotation quickly:

```bash
CROSSPOINT_SIM_CONTENT_REFRESH_SECONDS=5 pio run -e simulator -t run_simulator
```

## X3 memory boundaries

- The manifest never occupies a 64 KiB RAM body: it is capped at 64 KiB on SD
  and parsed with a 512-byte heap buffer. Alerts remain capped at 32 KiB.
- Config, schedule, and content JSON are rejected above 16 KiB / 32 KiB /
  96 KiB before parsing.
- The schedule parser keeps at most 32 windows in RAM.
- Content is read in two 512-byte streaming passes: the first measures weights
  without retaining text, and the second retains only the selected copy.
- Every HTTP GET completes before the next begins. Manifest, schedule, config,
  and content downloads therefore never hold two network links concurrently;
  completed responses reuse the same clean keep-alive connection during one
  sync burst to avoid repeated TLS handshakes.
- Release downloads stream directly to SD; copy/hash helpers use a 1 KiB heap
  buffer. Manifest entries, content copies, used IDs, seen alerts, and pending
  events all have explicit caps.

## SD retention and recovery

The active and previous snapshot are the only release indexes retained.
Content-addressed objects referenced by neither snapshot are removed. A failed
or interrupted download leaves `active_version` unchanged; temporary and
orphaned files are removed on the next service start. Older firmware's
`releases/<version>` layout is imported by SHA without redownloading and is
kept only while it is the sole rollback copy.
