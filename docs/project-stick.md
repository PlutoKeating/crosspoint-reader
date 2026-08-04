# Project.Stick integration

This private fork intentionally adds an external synchronization connector that
is outside upstream CrossPoint's temporarily closed scope. Project.Stick targets
the Xteink X3 (ESP32-C3, no PSRAM) only; it is not proposed as an upstream
multi-device feature.

## Runtime flow

1. The Home screen opens `ProjectStickActivity`.
2. The activity opens in offline mode immediately. Wi-Fi is optional; when it
   is available the service registers the persistent UUID v4 device identity.
3. The manifest is streamed through a 512-byte parser and written to a
   temporary SD file. Its entries become a compact release snapshot under
   `/.crosspoint/project_stick/snapshots/<version>.idx`.
4. Release files are processed serially. Each SHA-256 object is stored once at
   `/.crosspoint/project_stick/objects/<sha256>.json`; an already verified SHA
   is reused without another download. Changed objects stream directly to a
   temporary SD file while their byte count and SHA-256 are checked.
5. Only after every object passes validation does
   `/.crosspoint/project_stick.json` atomically switch `active_version`.
   `previous_version` remains the rollback snapshot.
6. The active schedule is checked every 30 seconds, the manifest at the
   server-provided interval, and alerts during the documented trading windows.
7. Events are persisted in the same state file and retried after connectivity
   returns.

The `market_open` scenario is the normal non-alert display during trading
sessions. Its device tag is rendered as `盘中常态`; `volatility_alert` may
temporarily override it, while `post_close` remains the all-day fallback.

The service uses the device RTC as the offline scheduling clock and corrects its
working clock whenever the API returns `server_time`.

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

The HTTP API path prefix (`/api/v1/device`) is appended by the service.

## X3 controls

- Release the left side button (`Up`) to send `feedback_meh` for the displayed copy.
- Release the right side button (`Down`) to send `feedback_useful` for the displayed copy.
- Press the front-right button to select another copy and, when online, check for a new release.
- The front confirm button opens Wi-Fi selection; the front back button returns home.

## Desktop simulator

Build and launch the same Project.Stick activity used by the firmware:

```bash
pio run -e simulator -t run_simulator
```

The simulator uses the production API by default and persists its simulated SD state under `fs_/.crosspoint/`.

## X3 memory boundaries

- The manifest never occupies a 64 KiB RAM body: it is capped at 64 KiB on SD
  and parsed with a 512-byte heap buffer. Alerts remain capped at 32 KiB.
- Schedule and content JSON are rejected above 32 KiB / 96 KiB before parsing.
- The schedule parser keeps at most 32 windows in RAM.
- Content is read in two 512-byte streaming passes: the first measures weights
  without retaining text, and the second retains only the selected copy.
- Every HTTP GET completes before the next begins. Manifest, schedule, config,
  and content downloads therefore never hold two network links concurrently.
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
