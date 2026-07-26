# Project.Stick integration

This private fork intentionally adds an external synchronization connector that
is outside upstream CrossPoint's temporarily closed scope. Project.Stick targets
the Xteink X3 (ESP32-C3, no PSRAM) only; it is not proposed as an upstream
multi-device feature.

## Runtime flow

1. The Home screen opens `ProjectStickActivity`.
2. The activity reuses the firmware Wi-Fi selection flow, then registers the
   persistent UUID v4 device identity.
3. `ProjectStickService` fetches the manifest and materializes a release under
   `/.crosspoint/project_stick/releases/<version>/`.
4. Unchanged files are copied from the active release. Changed files are
   streamed to SD and SHA-256 checked.
5. Only after every file passes validation does
   `/.crosspoint/project_stick.json` switch `active_version`.
6. The active schedule is checked every 30 seconds, the manifest at the
   server-provided interval, and alerts during the documented trading windows.
7. Events are persisted in the same state file and retried after connectivity
   returns.

The service uses the device RTC as the offline scheduling clock and corrects its
working clock whenever the API returns `server_time`.

## Configuration

The production service defaults to:

```text
https://stockstick.vercel.app
```

Override it at build time without editing source:

```ini
build_flags =
  ${base.build_flags}
  -DPROJECT_STICK_BASE_URL=\"http://192.168.1.10:3000\"
```

The HTTP API path prefix (`/api/v1/device`) is appended by the service.

## X3 memory boundaries

- Manifest and alert bodies are streamed into bounded buffers (64 KiB / 32 KiB).
- Schedule and content JSON are rejected above 32 KiB / 96 KiB before parsing.
- Release downloads stream directly to SD; the reusable copy/hash buffer is
  1 KiB on the heap.
- Manifest entries, content copies, used IDs, seen alerts, and pending events
  have explicit caps to prevent malformed releases or extended offline periods
  from exhausting ESP32-C3 RAM.
