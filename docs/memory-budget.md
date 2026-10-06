# X3 memory budget (ESP32-C3)

The C3 has one SRAM pool shared by IRAM and DRAM; there is no PSRAM. Anything
not placed statically comes out of one heap that Wi-Fi, NimBLE, the
framebuffer, TLS and the app share. This page records where that memory goes,
the 2.2.2 out-of-memory crash, and what 2.4.3 changed.

## 2.2.2 crash (`crash_report.txt`)

```
Panic reason: abort() was called at PC 0x4212920b on core 0
POST /api/v2/device/register attempt 1/3 (heap=26444 max=23540)  -> failed after ~37 s
POST /api/v2/device/register attempt 2/3 (heap=23664 max=17396)  -> failed after ~37 s
POST /api/v2/device/register attempt 3/3 (heap=23364 max=17396)  -> failed after ~46 s
```

Symbolised with `stockstick-2.2.2.elf`:

```
ProjectStickActivity::loop
  -> ProjectStickService::now()        snprintf of the RTC value "2000-01-01T00:00:00Z"
  -> parseIso8601ToShanghai()          const std::string input(value)
  -> operator new -> std::bad_alloc -> __cxxabiv1::__terminate -> abort()
```

| Bug | In 2.4.2? | Fix in 2.4.3 |
|---|---|---|
| The UI loop allocated a `std::string` on every call to `now()` (`lib/ProjectStick/ProjectStickCore.cpp` parser built `std::string input`). On a fragmented heap that throwing `new` aborted the firmware. | Yes, unchanged (`ProjectStickService::now()` still formatted and re-parsed a string). | Parser works on `const char*`; `now()` converts calendar fields with `shanghaiFromUtc()` directly. Host test counts allocations: zero. |
| Other per-loop heap churn: `StudioFrame::snapshot()` copied three heap strings every loop; `studio::step()` copied the scene id, built a 47-byte string for the shuffle seed and returned a fresh `std::vector<int>`, many times per second. | Yes. | `StudioFrame::hasContent()`; `tick()` re-evaluates only when the second, the alert or a key changes; `step()` uses a pointer to the scene id, mixes the seed without concatenating and reuses a scratch vector in `Playback`. Host test: 600 steps after warm-up, zero allocations. |
| An RTC that lost backup power reads 2000-01-01 and was treated as a real time. | Yes (RTC path had no year check). | Readings before 2025 are invalid; prefer server time, then the system clock, then a valid RTC; the UI task writes trusted time back to the RTC (`HalClock::setUtc`) and seeds the system clock from a valid RTC. With no trusted time, `StudioFrame::tick()` holds the current frame. |
| Every TLS handshake to the production API needed more heap than the device had: the chain is leaf → WE1 (P-256) → GTS Root R4 (P-384). P-384 was not in wolfSSL's SP code, so verifying WE1 used fast-math bignums sized by `FP_MAX_BITS=8192` (1,056-byte `fp_int`, 3.2 KB `ecc_point`) and `ecc_mul2add` heap-allocates 16 precomputed points (`SHAMIR_PRECOMP_SZ`, ~51 KB) under `WOLFSSL_SMALL_STACK`. | Yes (no `sp_384` symbols in the 2.4.2 ELF). | `-DWOLFSSL_SP_384`: P-384 runs on fixed-size SP arrays (a few KB). This is the most likely reason the X3 never reached the API. |
| A failed handshake was retried as TLS 1.2, doubling each failure (~40 s per attempt, matching the log) and the heap churn. | Yes. | `SecureClient::setTls12Fallback(false)` for the StockStick client; the fallback is also skipped after a transport failure, a stalled handshake or an out-of-memory error (`MEMORY_E`/`MP_MEM`). HTTP timeout 20 s → 15 s: one failed attempt costs at most 30 s. |
| Requests kept being attempted at heaps where a handshake cannot finish, fragmenting further (largest block 23.5 KB → 17.4 KB across the three attempts). | Partly (2.4.2 released NimBLE but still attempted). | Hard floor after releasing NimBLE: below 32 KB free or a 16 KB largest block the request is skipped and reported as a memory failure. Response bodies check the largest free block before a `std::string` grows. |
| A failed `new` left no heap information in the crash report. | Yes. | `std::new_handler` records free/min/largest heap and an out-of-memory marker in RTC memory before aborting; the main loop samples the heap every 5 s; the crash report prints the last sample. |

The "~3 KB lost per failed attempt" in the log is not a leak in SecureClient:
`stop()` frees the `WOLFSSL`/`WOLFSSL_CTX` on every failure path. Free heap
recovered partially between attempts (26.4 → 23.7 → 23.4 KB) while the largest
block fell (23.5 → 17.4 KB), which is fragmentation from the large bignum
allocations plus lwIP holding closed sockets in TIME_WAIT (`LWIP_TCP_MSL=60 s`).
Removing the fast-math P-384 path and the TLS 1.2 retry removes most of that
churn.

## 2.4.3 crash: stack overflow in the NimBLE host task (fixed in 2.5.1)

`crash_report.txt` from a 2.4.3 device: empty panic reason, heap sample at
50.9 s `free=30432 min=27948 largest=27636`, log shows NimBLE up at 5.9 s with
83 KB free and Wi-Fi retrying an unreachable saved network. Symbolised with
`stockstick-2.4.3.elf`, the stack dump reads (innermost last):

```
studio_ble::Callbacks::onWrite            (NimBLE host task, `begin` op)
  -> std::string / operator new
  -> HalStorage::openFileForRead -> SDCardManager::openFileForRead
  -> FatFile::mkdir / FatFile::open (LFN) / readDirCache / FsCache::prepare
  -> SdSpiCard::cardCommand -> SPIClass::beginTransaction -> digitalWrite
  -> xQueueSemaphoreTake
```

The exception SP (0x3FCC7080) sits less than 64 bytes below the task's
`0xA5A5A5A5` stack fill: the `nimble_host` task (library default 4096 bytes)
overflowed while `StudioFrame::start()` did FAT long-name opens from inside the
GATT write callback. The canary check only runs at a context switch, so the
report had no reason.

| Fix in 2.5.1 | Mechanism |
|---|---|
| GATT callbacks no longer touch the SD card | `onData` decrypts into a static 16-slot queue (~4 KB, 8 slots before 2.6.2); `studio_ble::pump()` on the main loop runs start/append/commit/abort with `mutex` released. The host task waits on a counting semaphore for a free slot (back-pressure through the ATT write response), 6 s max (the phone's write deadline is 8 s). |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192` | Margin for JSON, HMAC/AES and STATUS building that stay in the callback (+4 KB heap). |
| `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y` | An overflow now traps immediately with the task name instead of corrupting the neighbour. |
| Crash report records CPU exceptions | `mcause`/`mepc`/`ra`/`mtval`, the faulting task and its stack high-water mark (`HalSystem`). |
| Wi-Fi driver released between failed auto-connect attempts | `WiFi.disconnect(true)` after a timed-out join; ~50 KB back while the saved network is out of reach. |
| Stack telemetry | `[MEM]` line adds the loop task's free stack; the sync worker logs its high-water mark after each job; Settings > Bluetooth shows the host task's. |

Task stacks after 2.5.1: Arduino loop 8 KB (UI, StudioFrame I/O, pump), render
task 8 KB (frame reads, fonts), sync worker 8 KB static (TLS, firmware
download), `nimble_host` 8 KB (callbacks only); there is no input task (buttons are polled by `gpio.update()` in `loop()`),
esp_timer 4 KB, FreeRTOS timers 2.5 KB. None of the app tasks does recursion;
the largest stack objects are SdFat long-name paths and the 512-byte resume
buffer in `StudioFrame::start()`.

## 2.7.0: BLE transfer protocol 4

Static RAM, release ELF (`riscv32-esp-elf-size -A`), 2.6.5 → 2.7.0:

| Section | 2.6.5 | 2.7.0 |
|---|---|---|
| `.iram0.text` | 86,936 | 86,936 |
| `.dram0.data` | 17,601 | 17,625 |
| `.dram0.bss` | 51,040 | 66,424 (+15,384) |

| New static object | Bytes | Why static |
|---|---|---|
| `StudioWriter` task stack + TCB | 6,144 + 348 | created in `setup()` like the sync worker, so it cannot fail on a fragmented heap; deepest path is a commit (verify block, program JSON, state write) |
| Chunk queue, 16 × 510-byte slots | 8,256 (was ~3,900) | one MTU-517 write per slot, matching the phone's 8 KB window; the host callback never allocates |
| `FrameInflater` (uzlib state, 1 KB dictionary, 512 B out, 256 B in) | 3,084 | one per receiver; the phone's deflate window is ≤ 1 KB, so the 32 KB streaming dictionary `InflateReader` would allocate is not needed |
| Receiver copy block, `StudioFrame::start` rehash block | 512 + 512 | moved off the task stacks |

Transient heap per transfer: the incoming header JSON while resolving frames
(the SSP1 header, ~3.5 KB for 17 frames, `makeUniqueNoThrow`, checked against
the largest free block) and 32 bytes per frame for the digests; each kept
program header is parsed the same way, one at a time. While NimBLE is up its
host now has 20 mbufs and 20 controller ACL buffers (library default 12/12,
about +4 KB) so ~6 MTU-sized writes can be in flight; NimBLE is still released
for TLS, so the cloud-request heap only loses the static +15 KB.

Verify on the device: the log line `Transfer installed (… writes dropped,
writer stack free N)` after a push (N should stay above ~1 KB), and
`[MEM] Free/MaxAlloc` with NimBLE up and with it released before a request.

## 2.7.1: page-independent host

`ProjectStickHost` is a static singleton (it must exist on every page), so the
UI-side `ProjectStickService` (HTTP client object, a few strings) and the
Wi-Fi auto-connect state that used to live on the heap inside
`ProjectStickActivity` are now static: `.dram0.bss` 66,424 → 67,216 (+792 B),
`.dram0.data` and IRAM unchanged, and the activity allocation shrinks by the
same amount. The host's per-loop work on other pages is a handful of mutex
checks; the unbound setup-key check (which builds a string) runs every 500 ms
instead of every loop.

## 2.7.2: measured headroom, transfer buffers on demand, Wi-Fi on demand

First number from a real X3, read by the phone over STATE (firmware 2.6.5,
uptime 26.5 s, Wi-Fi connected at -33 dBm, phone connected over BLE):
`heap_free` 13,120, `heap_min` 11,044, `heap_max_alloc` 10,740. That is the
steady state with both radios up: there is no room for 2.7.0's +16 KB of
static RAM plus ~4 KB of larger NimBLE pools. Every 2.7.2 change below follows
from that measurement.

Static RAM, release ELF (`riscv32-esp-elf-size -A`):

| Section | 2.6.5 | 2.7.1 | 2.7.2 |
|---|---|---|---|
| `.iram0.text` | 86,936 | 86,936 | 87,266 |
| `.dram0.data` | 17,601 | 17,625 | 17,625 |
| `.dram0.bss` | 51,040 | 67,208 | 49,528 |
| Total static in SRAM | 155,577 | 171,769 (+16,192) | 154,419 (−1,158) |

What moved off the static image:

| Object | 2.7.1 | 2.7.2 |
|---|---|---|
| Writer task (6 KB stack + TCB) | static | gone: transfer work runs on the sync worker (idle while a phone is connected), raised to priority 2 while it pumps |
| Chunk queue, 16 × 510 B | static 8,256 B | heap, allocated at `begin4` (4–16 slots, whatever fits), freed at commit/abort/drop |
| `FrameInflater` (uzlib state + 1 KB dictionary) | static 3,084 B | heap, frames phase only, freed with the receiver |
| NimBLE mbufs / ACL buffers | 20 / 20 (+~4 KB heap) | library default 12 / 12 |

Free heap per state (2.6.5 measured, the rest **estimated** from the static
difference and the Wi-Fi driver's footprint of ~40–55 KB):

| State | 2.6.5 | 2.7.1 | 2.7.2 |
|---|---|---|---|
| Wi-Fi + BLE (phone linked) | 13.1 KB measured | ≈ −7 KB (does not fit) | does not occur: Wi-Fi goes off while a phone is linked |
| BLE only, phone linked | — | — | ≈ 55–70 KB |
| Content transfer (BLE only) | — | — | ≈ 40–55 KB (queue ≤ 8.2 KB, inflater 3 KB, header JSON ~4 KB) |
| Idle, Wi-Fi off | — | — | ≈ 55–70 KB |
| Cloud job (Wi-Fi up, NimBLE released) | — | — | ≥ 2.6.5's TLS headroom + 1.2 KB |

Wi-Fi on demand (`lib/ProjectStick/ProjectStickWifiPolicy.h`, host-tested):
the radio is up for a queued/running cloud job, a BLE `ota` request, a BLE
Wi-Fi join or scan (and 60 s after a successful join), the Wi-Fi and firmware
pages, a trial boot or unreported outcome, a due heartbeat, and trading-hours
alert polls (device holds a token). A running transfer always wins; a linked
phone otherwise keeps it down (it relays what the cloud would say). Unwanted
for 20 s (immediately while a phone is linked) the driver is stopped
(`WiFi.disconnect(true)` + `WIFI_OFF`). Jobs wait up to 20 s for the link.

Persistence under a starved heap: 2.6.x read stores with `Storage.readFile`
into a `String` (fails first on a fragmented heap) and wrote them through a
`String`; a failed read was treated as "no file", so the device created a new
identity and saved defaults over the store (see project-stick.md
"Credential loss"). 2.7.2 parses straight from the file, never saves a store
whose file exists but could not be read, refuses documents that overflowed
their allocator, and verifies the byte count before rotating `.tmp` → file
→ `.bak`.

Verify on the device: STATE `metrics.heap_free`/`heap_max_alloc` with the phone
linked (Wi-Fi off) and `metrics.wifi_on`; the serial line
`Wi-Fi wanted/not wanted (…)` explains every radio change, and
`Transfer installed (…, heap=N, worker stack free M)` after a push.

## 2.7.3: a linked phone no longer starves cloud jobs

Field report on 2.7.2: with the mini program connected, Settings > Firmware
update failed once with 「设备内存不足…」 and once with 「云端繁忙」, and no
request reached Cloudflare. Cause: `studio_ble::releaseRadio()` refused while a
phone was connected, so the TLS heap stayed at the 2.6.5 level (13 KB free,
10.7 KB largest block with both radios up and a phone linked): below the hard
floor, the request was skipped as Memory. The in-job retry then hit the error
backoff its own failure had set, which was reported as RateLimited.

2.7.3: a cloud job disconnects the phone and releases NimBLE before it brings
Wi-Fi up (user jobs at once, background jobs after an active phone has had up
to 2 min; see studio-protocol.md "Cloud jobs and a linked phone"); the stack
stays down, not advertising, for the whole job. Static RAM is unchanged
(`.dram0.data` + `.dram0.bss` 67,212 B vs 67,153 B in 2.7.2: +59 B).
STATE metrics now carry `tls_heap`/`tls_max` (heap right before the last TLS
attempt) and `net` (last failure code) so the next field failure is readable
server-side.

## 2.7.4: about 72 KB more free heap while a card shows

Static RAM, release ELF (`riscv32-esp-elf-size -A`, `nm` for `_heap_start`;
both builds from this tree, 2.7.3 at `51a95b80`):

| | 2.7.3 | 2.7.4 | Change |
|---|---|---|---|
| `.iram0.text` | 87,264 | 70,390 | −16,874 |
| `.dram0.data` | 17,649 | 17,361 | −288 |
| `.dram0.bss` | 49,600 | 47,064 | −2,536 |
| `_heap_start` | `0x3FCA5CC0` | `0x3FCA0FC0` | heap starts 19,712 B lower: +19.7 KB heap at boot |
| `firmware.bin` | 3,465,392 | 3,434,560 | −30,832 |

Per step, in commit order (measured on the release ELF while the step was made;
they add up to within ~300 B of the totals above, which are authoritative):

| Step | What | RAM effect |
|---|---|---|
| Settings tabs removed (`145308e9`) | the Display/Controls settings, button remapping, the RoundedRaff theme, sleep-screen modes, 40 strings | `.dram0.bss` −128 B (settings fields, remap table); flash ≈ −40 KB |
| IRAM → flash (`572527ad`) | FreeRTOS non-ISR API, ring buffer, heap functions, the BLE controller, unused driver ISRs | `.iram0.text` 87,266 → 70,390 (−16,876); heap +17.4 KB |
| BiDi buffers (`1efeb059`) | MiniBidi's static shaping buffers become call-time allocations (RTL packs only) | `.dram0.bss` 49,432 → 47,024 (−2,408) |
| Telemetry (`456ae8c2`) | `heap_largest_min`, `stack_min` | `.dram0.bss` +40 B |
| Streamed cards (`74060167`) | no framebuffer while a card shows | heap +52,272 B while a card is on screen; +5,280 B transient during a card refresh |

Free heap per state (static numbers measured, runtime **estimated** from them;
the device reports the real ones in STATE `metrics`):

| State | 2.7.3 | 2.7.4 |
|---|---|---|
| Card on screen, phone linked (Wi-Fi off) | ≈ 55–70 KB | ≈ +72 KB: +19.7 KB static, +52.3 KB framebuffer |
| UI page (Settings, Wi-Fi, firmware) | as before | +19.7 KB (the framebuffer is back) |
| Card refresh | — | strip block 4,752 + 528 B for the refresh only (72 columns; 24 or 8 when the heap is tight) |

### Streamed cards

Studio cards are pre-rendered portrait frames on the SD card (528 × 792, 1 =
black). The X3 panel is landscape 792 × 528 and the renderer's Portrait
orientation maps logical (x, y) to physical (y, 527 − x), so a strip of
physical columns [x0, x0 + n) is exactly card rows [x0, x0 + n):
`StudioFrame::stream()` reads the file front to back, eight rows at a time,
transposes them into the strip (`lib/ProjectStick/CardStrip.h`, host-tested
against the framebuffer render), then draws the overlays into the strip with
the renderer's column strip target (`beginColumnStripTarget`: drawPixel and
fillRect clip to the strip and use its stride; text, lines, rounded rects and
polygons go through them). The X3 driver writes each strip through a PTL
window in gate space, bottom row first (`Uc8253X3Driver::sendStrips`, the
addressing of the hardware-proven grayscale strip writer, narrowed to the
strip's byte columns), and runs `displayStart`/`displayFinish` unchanged, so
the waveform choices (FAST for cards, HALF on a short power press, the boot
full syncs and their settle passes) are the same as with a framebuffer. Each
plane write (DTM2 before the waveform, DTM1 after, more on a full sync) asks
for every strip again; the overlay inputs are captured once per repaint
(`ProjectStickActivity::captureLayers`) so every pass gets the same pixels
(the simulator checks this on every streamed refresh).

The framebuffer is freed after a streamed card (`GfxRenderer::releaseFrameBuffer`)
and comes back, white, before any page that needs it
(`ActivityManager`: before `onEnter` and `render` unless the page
`ownsFrameBuffer()`). Without it every drawing and display call is a no-op.
If the heap cannot give 52,272 contiguous bytes back, the render is retried
each loop; after 5 s, with no transfer, cloud job or firmware update running,
the device reboots to the StockStick page (the framebuffer is the first allocation at boot).
Sleep rebuilds the card into the framebuffer (the moon and the quick-resume
frame need it). The serial `CMD:SCREENSHOT` answers `SCREENSHOT_UNAVAILABLE`
while a streamed card is shown (the POWER+DOWN combo was removed in 2.7.5).

Verify on the device: STATE `metrics.heap_free` / `heap_max_alloc` on the card
page should be ≈ 50 KB higher than on a UI page; `heap_largest_min` shows
whether the heap ever fragmented below 52 KB (the framebuffer realloc needs
one block that large; `Framebuffer unavailable … rebooting` in the log means it
did). A card refresh should look and time like 2.7.3 (FAST, no flash).

### IRAM → flash safety

Code in flash cannot run while the flash cache is disabled (flash writes and
erases: OTA, NVS, the coredump). What can run then is IRAM code reached from an
interrupt registered as IRAM-safe (`ESP_INTR_FLAG_IRAM`). In this firmware:

- Our IRAM code is the panic wrappers (`lib/hal/HalSystem.cpp`, which run
  with the cache in whatever state the panic left) and the e-paper BUSY ISR
  (`freeink-sdk/.../EpdBus.cpp`, `xSemaphoreGiveFromISR`). The ISR is attached
  with `attachInterrupt`, whose GPIO ISR service is not IRAM-safe
  (`CONFIG_ARDUINO_ISR_IRAM` is off): during a flash write it is held off and
  runs afterwards. FreeRTOS keeps its `FromISR` API in IRAM; only the non-ISR
  API moved (`CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`).
- Ring buffer and heap functions (`*_PLACE_FUNCTION(S)_INTO_FLASH`): no IRAM
  ISR of ours calls them.
- Driver ISR-in-IRAM options turned off: I2S, RMT, GPTimer (unused on the X3),
  I2C master (RTC, IMU and fuel gauge are polled from tasks), esp_event posting
  from IRAM ISRs (unused).
- The BLE controller runs from flash (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`):
  Bluetooth stalls while flash is written. The device releases NimBLE before
  every flash write of its own (all three firmware paths, SD → flash). What
  can still write flash with a phone linked is NVS: the trial-boot
  confirmation (`ota_trial`, Preferences) when the phone's `sync` confirms an
  image, and the Wi-Fi driver's PHY calibration on a BLE Wi-Fi join. Both are
  single NVS page writes (milliseconds, an erase at worst ~100 ms), well under
  the link supervision timeout.
- `CONFIG_SPI_FLASH_ROM_IMPL` stays off (the ROM flash driver lacks IDF fixes
  the OTA path relies on).

### Stacks

Not shrunk: there are no hardware high-water marks yet. STATE `stack_min` now
reports the least headroom of `loopTask` (8 KB), the sync worker (8 KB,
static; task `StickSync` since 2.7.6, read by handle) and `nimble_host` (8 KB). Shrink a stack only after field reports show
its headroom stays above ~2 KB across transfers, firmware installs and cloud
jobs.

## 2.7.5: removed features (flash, a little RAM)

Release ELF (`riscv32-esp-elf-size -A`, both built in this tree; 2.7.4 from
`dist/firmware/2.7.4`):

| Section | 2.7.4 | 2.7.5 |
|---|---|---|
| `.iram0.text` | 70,390 | 70,390 |
| `.dram0.data` | 17,361 | 17,353 |
| `.dram0.bss` | 47,064 | 46,552 (−512) |
| `_heap_start` | 0x3FCA0FC0 | 0x3FCA0DB0 (+528 B heap) |
| `.flash.text` | 1,417,474 | 1,402,266 (−15,208) |
| `.flash.rodata` | 1,916,376 | 1,825,816 (−90,560) |
| `firmware.bin` | 3,434,560 | 3,328,784 (−105,776) |

| Removed | Where the saving shows |
|---|---|
| SD language packs, pack loader, language picker | `.bss`: the 182-entry override table and pack identity in `I18n`; a loaded pack (up to 32 KB of heap) can no longer exist |
| MiniBidi / BiDi in the renderer | flash; the call-time shaping allocations (2.7.4) are gone |
| Hebrew and Arabic glyphs in the Ubuntu 10/12 and Noto Sans 8 faces | `.flash.rodata` ≈ −87 KB |
| SD-card fonts, screenshot combo, tilt events, Home launcher, dead utilities | flash; one map lookup per measured string; no Home page render on the way to Settings |

## 2.7.6: back to hardware-validated placements (2.7.4/2.7.5 boot-looped on an X3)

A real X3 flashed with 2.7.5 looped (boot screen → black → reboot) and the
trial rolled back to 2.7.2. **Root cause (from the user's
`crash_report.txt`):** `assert failed: xTaskGetHandle tasks.c:2864
(strlen( pcNameToQuery ) < 16)` on the loop task about 5.6 s after every
boot. The 2.7.4 stack telemetry looked tasks up by name, and
`"ProjectStickSync"` is 16 characters (configMAX_TASK_NAME_LEN is 16,
including the terminator); the first phone-state refresh panicked. Heap was
fine (free 145 KB, largest block 114 KB). 2.7.6 reads stack headroom from
task handles, shortens the task names (`StickSync`, `UiRender`) and checks
them against configMAX_TASK_NAME_LEN at compile time.

The same log shows `[BLE] Start failed at init (code=0)` on every boot of
2.7.5: NimBLE never started (code 0 was the controller status, IDLE). The
likely cause is `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` (2.7.4), reverted below;
2.7.6 also records which call failed (`nvs_flash_init`,
`controller_init`, `controller_enable` or `host_init`) with its esp_err_t,
so a remaining failure names itself.

Besides the fix, 2.7.6 removes the unvalidated hardware changes of 2.7.4 and
adds diagnostics:

- **Heap functions back in IRAM** (`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`
  off). With them in flash, the Wi-Fi/BLE/coex OSI allocation wrappers IDF
  keeps in IRAM (`calloc/zalloc/realloc_internal_wrapper`,
  `esp_coex_common_malloc_internal_wrapper`, `malloc`/`free`) called into
  flash — forbidden by the option's own help (fault on any NVS/otadata write
  with a radio active). A definite defect.
- **BLE controller back in ROM/IRAM** (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`
  off): unvalidated, stalls BLE during flash writes. Pending a hardware test.
- FreeRTOS non-ISR and ring-buffer functions stay in flash (IDF keeps the ISR
  variants in IRAM). The IRAM→flash call edges of the 2.7.6 ELF are identical
  to 2.7.3's (`objdump` of `.iram0.text`, every `jalr`/`jr` into 0x42xxxxxx).
- **X3 strip rendering off** (`Uc8253X3Driver::supportsStripSource()` false,
  freeink-sdk 023881a): per-strip partial windows narrower than the panel
  were never exercised on hardware (the simulator does not run this driver).
  Cards render through the framebuffer again, and
  `GfxRenderer::releaseFrameBuffer()` is a no-op without strip support, so
  the 52 KB framebuffer stays resident and the reacquire/restart path never
  runs on X3. The strip code stays for a hardware test.

| | 2.7.3 | 2.7.5 | 2.7.6 |
|---|---|---|---|
| `.iram0.text` | 87,264 | 70,390 | 86,678 |
| `.dram0.data` | 17,649 | 17,353 | 17,697 |
| `.dram0.bss` | 49,600 | 46,552 | 46,656 |
| `_heap_start` | 0x3FCA5CA0 | 0x3FCA0DB0 | 0x3FCA4F70 |
| heap vs 2.7.3 | — | +19.9 KB | +3.4 KB |
| framebuffer while a card shows | resident | freed (52 KB) | resident |

Net: about 3.4 KB more heap than 2.7.3 (removed features, smaller buffers),
instead of 2.7.4's estimated +72 KB with a card on screen. Both disabled
gains can come back one at a time after a hardware test behind the trial
rollback (strip path first, ideally with a USB serial capture of a
`gh_release_rc` build); the heap never goes to flash while Wi-Fi/BLE are in
use.

## Where the RAM goes

Static, from `riscv32-esp-elf-size -A` on the release ELF (2.4.2 → 2.4.3):

| Section | 2.4.2 | 2.4.3 | Notes |
|---|---|---|---|
| `.iram0.text` (occupies SRAM) | 86,354 | 86,354 | Wi-Fi IRAM opts already moved to flash |
| `.dram0.data` | 17,425 | 17,449 | |
| `.dram0.bss` | 46,304 | 46,456 | includes the 8 KB static sync-worker stack; +152 B heap sample / tick state |
| Heap start | `0x3FCA2000` | `0x3FCA2000` | heap region runs to the ROM-reserved top of DRAM (~240 KB at boot) |

Large dynamic consumers (sizes from the code and sdkconfig; not measured on
hardware yet, see "Verify on the device"):

| Consumer | Size | When | 2.4.3 |
|---|---|---|---|
| E-paper framebuffer (`FreeInkDisplay`, single-buffer mode) | 52,272 B (528 × 792 / 8) | UI pages; freed while a streamed card shows (2.7.4) | unchanged (no async shadow on X3) |
| Wi-Fi static RX buffers | 8 × ~1.6 KB ≈ 13 KB | while Wi-Fi is up | 4 buffers ≈ 6.5 KB (−6.5 KB) |
| Wi-Fi dynamic RX / TX buffers | up to 32 + 32 × ~1.6 KB | bursts | capped at 16 + 16 (peak −~50 KB worst case) |
| Wi-Fi TX A-MPDU buffers | per BA session | while associated | disabled |
| Wi-Fi driver, supplicant, lwIP | ~40–55 KB | while Wi-Fi is up | unchanged |
| NimBLE host + controller | tens of KB | bound/setup modes | released for low-heap requests (2.4.2) |
| TLS session (wolfSSL CTX + SSL + record buffers) | ~20–30 KB | during a request | unchanged |
| TLS handshake P-384 verify | ~51 KB fast-math → a few KB SP | during a handshake | `WOLFSSL_SP_384` |
| Task stacks (Arduino loop 8 KB, lwIP 4 KB, timers, sync worker 8 KB static) | ~25 KB | always | unchanged |

## Verify on the device

Use a `default` or `gh_release_rc` build (the release build has `LOG_LEVEL=0`)
and watch the serial log:

- `[MEM] Free: … MaxAlloc: …` every 10 s: free heap with Wi-Fi up should now
  be well above the 2.2.2 crash's 26 KB.
- `[STICK] POST /api/v2/device/register: heap=… max=… min=…` before each
  request, and `[SecureClient] handshake ok (auto): TLSv1.3 / … in N ms`.
  A failure now prints `wolfSSL_connect failed (auto): <err>, free heap … max …`.
- `[STICK] … skipped: low memory (heap=… max=…)` means the hard floor was hit:
  report the numbers.
- `[CLK] RTC set to …` after the first trusted time (NTP, phone or server).
- After any crash, `crash_report.txt` now contains `Heap at … ms: free=…
  min=… largest=…` (and `Out of memory (operator new failed).` when that was
  the cause).
