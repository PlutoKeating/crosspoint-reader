# StockStick single-screen transport (development)

This protocol adds investment companion card delivery on X3. Existing library,
reader, OPDS, synchronization and font/image reader functions are retained.
Full product scope and outstanding work live in the adjacent
`Project.StockStick/docs/product/` directory. This is not a hardware acceptance report.

## Raster and persistence

528 × 792 physical pixels, row-major, most significant bit first, 1 = black,
52,272 bytes. Phone preview uses exactly this payload. Landscape rotation occurs
on the phone before packing; firmware does not reflow or substitute fonts.
`StudioFrame` streams to SD, checks length and SHA-256 before activation, persists
state using a temporary file and backup, and verifies persisted pixels on reboot.
The active companion view refreshes before generating the displayed receipt.

Only the foreground companion activity draws Studio frames. Reader foreground,
sleep/keyguard overlays, alert interruption and failures during physical panel
refresh still require integration and real-device acceptance. A successful
`displayBuffer` call is currently the software completion boundary, not a panel
readback sensor.

## Cloud

`GET /api/v2/device/studio?device_id=...` uses the device bearer and returns the
desired task, immutable raster URL, digest, expiration epoch and BLE authority.
`POST` reports transferring/verifying/refreshing/displayed/failed. The server
rejects another device's task, validates the displayed digest, and does not let
old receipts complete new tasks. Polling is five seconds while Wi-Fi is connected
and BLE is idle; this is not yet a complete desktop/power-saving policy.

TLS uses the public GTS Root R4 trust anchor and SecureNet peer/domain checks;
the first cloud operation waits for a usable system clock (SNTP). For another
deployment domain, maintain the trusted CA; never transmit authority keys using
an insecure TLS fallback. Heap cost, certificate validity and first-boot time
synchronization must be tested on X3. Official root information: https://pki.goog/.
Domain verification follows https://www.wolfssl.com/documentation/manuals/wolfssl/group__Setup.html.

## BLE

- Service `9fe10000-6bc2-4ce7-8e62-77262df32ef1`.
- Control `...0001...`: writable, ASCII JSON terminated by newline, at most512 bytes.
- Data `...0002...`: writable, little-endian uint32 offset plus at most240 ciphertext bytes.
- Status `...0003...`: readable JSON, at most512 bytes.
- One connection/session. Phone uses20-byte control writes and negotiated MTU for data.

The server provisions a32-byte key K, independently of the device bearer, with
an epoch tied to ownership. A connection creates a16-byte random nonce N (hex).
Status includes device_id, epoch, nonce, base_task (active task at connection),
state, received, expires and proof. All HMAC strings below are ASCII, SHA-256,
hex output, with literal `|` separators.

```
hello proof = HMAC(K, "hello|device_id|N|epoch|base_task")
begin proof = HMAC(K, "studio1|device_id|N|epoch|task|frame_sha256|expires")
AES-256 key = HMAC(K, "enc|N") raw32 bytes
AES-CTR IV  = N decoded to16 bytes
receipt    = HMAC(K, "displayed|task|frame_sha256|N|base_task|expires")
```

Begin validates authority and reserves the single frame stream. Data writes must
advance the exact offset. Ciphertext is decrypted incrementally; commit requires
the authenticated frame digest and complete length. Interrupted or invalid
transfers preserve the previous active file. Expiration0 means until replaced.

After offline display the phone stores the complete frozen source/raster/receipt.
The cloud verifies key epoch and signature. Divergence from base_task requires an
explicit keep-device/keep-cloud decision. `accepted_ble_task` in the subsequent
device response releases the offline protection so later cloud tasks can apply.
Unbinding rotates authority server-side; a device clears local authority once it
observes unbinding. Instant remote revocation while completely offline is impossible.

No real-device BLE throughput, reconnect, long GATT read, coexistence or panel
timing claim has been made. Whole playback-plan BLE transfer/resume remains open.

## Reproducible build

PlatformIO release configuration uses the IDF BLE controller only and the external
NimBLE-Arduino host; central/observer roles are disabled. A narrow pre-build patch
moves Arduino3.3.7's BT-use flag outside the built-in-host conditional. LTO is
enabled using both the compiler flag and pioarduino's `-fno-lto` unflag mechanism.
Reader fonts and languages have not been removed. Existing OTA partitions remain.

If a generated `sdkconfig.gh_release` retains old host/mesh settings, move that
generated file aside and regenerate it. Do not delete user local configuration.
The final binary-size post-action is authoritative even if nested tool output
incorrectly ends in SUCCESS after an internal size failure. OTA capacity is tight;
check it on every build. Do not flash an oversized image.
