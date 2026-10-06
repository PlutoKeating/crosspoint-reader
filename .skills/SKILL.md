# StockStick Firmware Development Guide

Project: StockStick device firmware for the Xteink X3 (ESP32-C3), forked from CrossPoint Reader. X3/X4 runtime detection is kept.
Mission: Reliable StockStick content display, BLE delivery and setup, and safe OTA on constrained hardware.
Scope: see `SCOPE.md`. The ebook reader, web file transfer, OPDS, KOReader and Calibre were removed in 2.0.0; do not reintroduce them.
Key docs: `docs/firmware-ota.md` (versioning, release, OTA, trial boot), `docs/project-stick.md`, `docs/studio-protocol.md`, `docs/memory-budget.md` (heap budget, OOM crash analysis, IRAM safety), `docs/feature-audit.md` (what every remaining feature is for), `docs/i18n.md`.

## Internal Testing Workflow (2026-09-29, overrides the push guidance below)

The firmware, mini program and website are in internal testing with no external users.
- After any code or doc change: commit, merge into `main`, and push `main` to `stick` immediately. No per-change authorization is needed. Never force-push without explicit confirmation, and never add agent co-author/attribution lines.
- Do not add compatibility or migration work for older firmware versions; keep the current firmware, backend and mini program consistent instead.

## AI Agent Identity and Cognitive Rules
* Role: Senior Embedded Systems Engineer (ESP-IDF/Arduino-ESP32 specialized).
* Primary Constraint: 380KB RAM is the hard ceiling. Stability is non-negotiable.
* Evidence-Based Reasoning: Before proposing a change, you MUST cite the specific file path and line numbers that justify the modification.
* Anti-Hallucination: Do not assume the existence of libraries or ESP-IDF functions. If you are unsure of an API's availability for the ESP32-C3 RISC-V target, check the freeink-sdk source or the FreeInk SDK docs (https://freeink.org/llms.txt for an LLM-readable index) first.
* No Unfounded Claims: Do not claim performance gains or memory savings without explaining the technical mechanism (e.g., DRAM vs IRAM usage).
* Resource Justification: You must justify any new heap allocation (new, malloc, std::vector) or explain why a stack/static alternative was rejected.
* Verification: After suggesting a fix, instruct the user on how to verify it (e.g., monitoring heap via Serial or checking a specific cache file).
---

## Development Environment Awareness

**CRITICAL**: Detect the host platform at session start to choose appropriate tools and commands.

### Platform Detection
```bash
# Detect platform (run once per session)
uname -s
# Returns: MINGW64_NT-* (Windows Git Bash), Linux, Darwin (macOS)
```

**Detection Required**: Run `uname -s` at session start to determine platform

### Platform-Specific Behaviors
- **Windows (Git Bash)**: Unix commands, `C:\` paths in Windows but `/` in bash, limited glob (use `find`+`xargs`)
- **Linux/WSL**: Full bash, Unix paths, native glob support

**Cross-Platform Code Formatting**:
```bash
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

---

## Platform and Hardware Constraints

### Hardware Specs
* MCU: ESP32-C3 (Single-core RISC-V @ 160MHz)
* RAM: ~380KB usable (VERY LIMITED - primary project constraint)
  * **NO PSRAM**: ESP32-C3 has no PSRAM capability (unlike ESP32-S3)
  * **Single Buffer Mode**: Only ONE framebuffer (not double-buffered), and since 2.7.4 none at all while a card shows (see "Framebuffer lifecycle" below)
* Flash: 16MB (Instruction storage and static data)
* Display: X3 792x528 E-Ink, UC8253 (cards are portrait 528x792; X4 800x480 is still detected at runtime)
  * Framebuffer: 52,272 bytes on the X3 (792 × 528 ÷ 8)
* Storage: SD Card (StockStick content, Studio frames, OTA staging, language packs)
* OTA slots: two app partitions of 6,553,600 bytes each; the partition table cannot change over OTA, so every change must check the final image size.

### The Resource Protocol
1. Stack Safety: Limit local function variables to < 256 bytes. The ESP32-C3 default stack is small; use std::unique_ptr or static pools for larger buffers.
2. Heap Fragmentation: Avoid repeated new/delete in loops. Allocate buffers once during onEnter() and reuse them.
3. Flash Persistence: Large constant data (UI strings, lookup tables) MUST be marked static const to stay in Flash (Instruction Bus), freeing DRAM.
4. String Policy: Prohibit std::string and Arduino String in hot paths. Use std::string_view for read-only access and snprintf with fixed char[] buffers for construction.
5. UI Strings: All user-facing text must use the `tr()` macro (e.g., `tr(STR_LOADING)`) for i18n support. Never hardcode UI strings directly. For the avoidance of doubt, logging messages (LOG_DBG/LOG_ERR) can be hardcoded, but user-facing text must use `tr()`.
6. `constexpr` First: Compile-time constants and lookup tables must be `constexpr`, not just `static const`. This moves computation to compile time, enables dead-branch elimination, and guarantees flash placement. Use `static constexpr` for class-level constants.
7. `std::vector` Pre-allocation: Always call `.reserve(N)` before any `push_back()` loop. Each growth event allocates a new block (2×), copies all elements, then frees the old one — three heap operations that fragment DRAM. When the final size is unknown, estimate conservatively.
8. SPIFFS Write Throttling: Never write a settings file on every user interaction. Guard all writes with a value-change check (`if (newVal == _current) return;`). Progress saves during reading must be debounced — write on activity exit or every N page turns, not on every turn. SPIFFS sectors have a finite erase cycle limit.
9. `new` is not nothrow on ESP32: With `-fno-exceptions`, bare `new` that fails calls `abort()` — it does NOT return `nullptr`. Always use `new (std::nothrow)` and null-check the result, or use `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`. Never write bare `new` for any fallible allocation.

---

## Project Architecture

### Build System: PlatformIO

**PlatformIO is BOTH a VS Code extension AND a CLI tool**:

1. **VS Code Extension** (Recommended):
   * Extension ID: `platformio.platformio-ide` (see `.vscode/extensions.json`)
   * Provides: Toolbar buttons, IntelliSense, integrated build/upload/monitor
   * Configuration: `.vscode/c_cpp_properties.json`, `.vscode/tasks.json`
   * Usage: Click Build (✓), Upload (→), or Monitor (🔌) buttons

2. **CLI Tool** (`pio` command):
   * **Installation**: Python package (typically `pip install platformio`)
   * **Windows Location**: `C:\Users\<user>\AppData\Local\Programs\Python\Python3xx\Scripts\pio.exe`
   * **Verify**: `which pio` (Git Bash) or `where.exe pio` (cmd)
   * **Usage**: `pio run`, `pio run -t upload`, etc.

**Configuration Files**:
* `platformio.ini`: Main build configuration (committed to git)
* `platformio.local.ini`: Local overrides (gitignored, create if needed)
* `partitions.csv`: ESP32 flash partition layout

### Build Environment
* **Standard**: C++20 (`-std=c++2a`). No Exceptions, No RTTI.
* **Logging**: ALWAYS use `LOG_INF`, `LOG_DBG`, or `LOG_ERR` from `Logging.h`. Raw Serial output is deprecated.
* **Environments** (in `platformio.ini`):
  * `default`: Development (LOG_LEVEL=2, serial enabled)
  * `gh_release`: Production (LOG_LEVEL=0)
  * `gh_release_rc`: Release candidate (LOG_LEVEL=1)
  * `slim`: Minimal build (no serial logging)

### Critical Build Flags
These flags in `platformio.ini` fundamentally affect firmware behavior:

```cpp
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1  // Single framebuffer (saves 52KB RAM!)
-DARDUINO_USB_MODE=1                 // Enable USB CDC
-DARDUINO_USB_CDC_ON_BOOT=1          // Serial available immediately at boot
-DUSE_UTF8_LONG_NAMES=1              // SD card long filename support
-DDESTRUCTOR_CLOSES_FILE=1           // FsFile destructor auto-closes (SdFat)
```

**DESTRUCTOR_CLOSES_FILE implications**:
- SdFat's `FsBaseFile` destructor calls `close()` automatically when the object goes out of scope
- **Do NOT add explicit `file.close()` calls** for local `FsFile` variables — the destructor handles it
- Explicit `close()` is still required in these cases:
  1. **Close before delete**: Must close before `Storage.remove()` on the same path
  2. **Close before reopen**: Must close before reopening the same `FsFile` variable (e.g., write then reopen for read, or rewrite the same path)
  3. **Member variables**: `FsFile` members persist beyond any single function scope, so close at the intended release point (e.g., in `onExit()`)

**SINGLE_BUFFER_MODE implications**:
- Only ONE framebuffer exists (not double-buffered)
- **Framebuffer lifecycle (2.7.4)**: Studio cards are streamed from the SD card to the X3 panel in strips
  (`StudioFrame::stream`, `HalDisplay::displayStrips`, `GfxRenderer::beginColumnStripTarget` for the overlays)
  and the framebuffer is freed while a card shows (`renderer.releaseFrameBuffer()`). `ActivityManager` brings
  it back (`ensureFrameBuffer`) before `onEnter()` and `render()` of every page that does not override
  `ownsFrameBuffer()`; those that do (the card page, sleep) handle it themselves. Without a framebuffer every
  drawing and display call is a no-op, so code outside an activity's `render()`/`onEnter()` that draws must
  call `renderer.ensureFrameBuffer()` first and cope with `false`. Overlays drawn into a streamed card must be
  deterministic within one repaint (the panel asks for each strip at least twice)
- Grayscale rendering requires temporary buffer allocation (`renderer.storeBwBuffer()`)
- Must call `renderer.restoreBwBuffer()` to free temporary buffers
- See [lib/GfxRenderer/GfxRenderer.cpp:439-440](../lib/GfxRenderer/GfxRenderer.cpp) for malloc usage

### Directory Structure
* lib/: Internal libraries (GfxRenderer, EpdFont, I18n, ProjectStick core logic incl. StickFirmware)
  * lib/hal/: Hardware Abstraction Layer (HalDisplay, HalGPIO, HalStorage)
  * lib/I18n/: Internationalization (translations in `translations/*.yaml`, generated string tables)
* src/activities/: UI logic using the Activity Lifecycle (onEnter, loop, onExit)
* freeink-sdk/: Low-level SDK (EInkDisplay, InputManager, BatteryMonitor, SDCardManager)
* .crosspoint/: SD state (settings, project_stick.json identity/binding state, studio/ frames and OTA staging, lang/ packs)

### Hardware Abstraction Layer (HAL)

**CRITICAL**: Always use HAL classes, NOT SDK classes directly.

| HAL Class | Wraps SDK Class | Purpose | Singleton Macro |
|-----------|----------------|---------|-----------------|
| `HalDisplay` | `EInkDisplay` | E-ink display control | *(none)* |
| `HalGPIO` | `InputManager` | Button input handling | *(none)* |
| `HalStorage` | `SDCardManager` | SD card file I/O | `Storage` |

**Location**: [lib/hal/](../lib/hal/)

**Why HAL?**
- Provides consistent error logging per module
- Abstracts SDK implementation details
- Centralizes resource management

**Example - HalStorage**:
```cpp
#include <HalStorage.h>

// Use Storage singleton (defined via macro)
HalFile file;
if (Storage.openFileForRead("MODULE", "/path/to/file.bin", file)) {
  // Read from file
  // No file.close() needed — DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}
```

**Usage**: Use `HalFile` (the mutex-wrapping handle), NOT raw SdFat `FsFile` or Arduino `File`. Do NOT add `file.close()` for local variables (see DESTRUCTOR_CLOSES_FILE above).

**SdFat is not thread-safe; all SD access MUST go through HalStorage**:
- SdFat's `SdSpiCard` tracks SPI bus state with an unsynchronized `m_spiActive` bool. Two tasks calling SdFat concurrently can confuse that state machine and end with one task calling `SPIClass::endTransaction()` against a paramLock the *other* task is holding. That trips FreeRTOS's `xTaskPriorityDisinherit` assert (`tasks.c:5156, pxTCB == pxCurrentTCBs[0]`) and panics the system. See SdFat issue #518.
- `HalStorage` serializes everything via `storageMutex`. Downstream code uses `HalFile` (declared in `<HalStorage.h>`); every method call (read, write, seek, close) takes the mutex. `HalFile`'s destructor also takes the mutex before letting the underlying SdFat `FsFile` close.
- **Never** call into `SdFat` / `SdSpiCard` / `FsBaseFile` / `SDCardManager` / raw `FsFile` directly — that bypasses the mutex.

---

## Coding Standards

### Naming Conventions
* Classes: PascalCase (e.g., ProjectStickActivity)
* Methods/Variables: camelCase (e.g., renderPage())
* Constants: UPPER_SNAKE_CASE (e.g., MAX_BUFFER_SIZE)
* Private Members: memberVariable (no prefix)
* File Names: Match Class names (e.g., ProjectStickActivity.cpp)

### Header Guards
* Use #pragma once for all header files.

### Memory Safety and RAII
* Smart Pointers: Prefer std::unique_ptr. Avoid std::shared_ptr (unnecessary atomic overhead for a single-core RISC-V).
* RAII: Use destructors for cleanup. Call `vTaskDelete()` explicitly for deterministic task release. Do NOT call `file.close()` on local `FsFile` variables — `DESTRUCTOR_CLOSES_FILE=1` handles it at scope exit (see Critical Build Flags).

### ESP32-C3 Platform Pitfalls

#### `std::string_view` and Null Termination
`string_view` is *not* null-terminated. Passing `.data()` to any C-style API (`drawText`, `snprintf`, `strcmp`, SdFat file paths) is undefined behaviour when the view is a substring or a view of a non-null-terminated buffer.

**Rule**: `string_view` is safe only when passing to C++ APIs that accept `string_view`. For any C API boundary, convert explicitly:
```cpp
// WRONG - undefined behaviour if view is a substring:
renderer.drawText(font, x, y, myView.data(), true);

// CORRECT - guaranteed null-terminated:
renderer.drawText(font, x, y, std::string(myView).c_str(), true);

// CORRECT - for short strings, use a stack buffer:
char buf[64];
snprintf(buf, sizeof(buf), "%.*s", (int)myView.size(), myView.data());
```

#### `IRAM_ATTR` and Flash Cache Safety
All code runs from flash via the instruction cache. During SPI flash operations (OTA write, SPIFFS commit, NVS update) the cache is briefly suspended. Any code that can execute during this window — ISRs in particular — must reside in IRAM or it will crash silently.

```cpp
// ISR handler: must be in IRAM
void IRAM_ATTR gpioISR() { ... }

// Data accessed from IRAM_ATTR code: must be in DRAM, never a flash const
static DRAM_ATTR uint32_t isrEventFlags = 0;
```

**Rules**:
- All ISR handlers: `IRAM_ATTR`
- Data read by `IRAM_ATTR` code: `DRAM_ATTR` (a flash-resident `static const` will fault)
- Normal task code does **not** need `IRAM_ATTR`

#### ISR vs Task Shared State
`xSemaphoreTake()` (mutex) **cannot** be called from ISR context — it will crash. Use the correct primitive for each communication direction:

| Direction | Correct primitive |
|---|---|
| ISR → task (data) | `xQueueSendFromISR()` + `portYIELD_FROM_ISR()` |
| ISR → task (signal) | `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` |
| Task → task | `xSemaphoreTake()` / mutex |
| Simple flag (single writer ISR) | `volatile bool` + `portENTER_CRITICAL_ISR()` |

**IRAM is scarce on purpose (2.7.4):** FreeRTOS non-ISR API, heap, ring buffer and the BLE controller run
from flash (`platformio.ini` custom_sdkconfig, analysis in `docs/memory-budget.md` "IRAM → flash safety").
Do not register an interrupt as IRAM-safe (`ESP_INTR_FLAG_IRAM`, `CONFIG_ARDUINO_ISR_IRAM`) and do not write
flash (OTA, NVS bursts) with a phone linked without checking that analysis first.

**NimBLE callbacks never touch the SD card.** `onRead`/`onWrite`/`onConnect` run on the `nimble_host` task; keep them to
verification, crypto and queueing (`studio_ble` chunk queue, drained by `pump()` on the `StudioWriter` task). A FAT long-name open
from a callback overflowed that task's stack in 2.4.3 (`docs/memory-budget.md`).

**Phone- and cloud-driven work never lives in an activity.** Requests a phone queues over BLE (bind, sync, unbind, Wi-Fi,
firmware over BLE `fw4`), the STATE refresh, Wi-Fi auto-connect and the cloud heartbeat belong to `ProjectStickHost`, ticked from the main loop
on every page (`docs/project-stick.md` "Page-independent host"). Until 2.7.1 they ran inside `ProjectStickActivity`, so a
phone could connect while the device sat in Settings and nothing it asked for was executed. Activities only render.

#### RISC-V Alignment
ESP32-C3 faults on unaligned multi-byte loads. Never cast a `uint8_t*` buffer to a wider pointer type and dereference it directly. Use `memcpy` for any unaligned read:

```cpp
// WRONG — faults if buf is not 4-byte aligned:
uint32_t val = *reinterpret_cast<const uint32_t*>(buf);

// CORRECT:
uint32_t val;
memcpy(&val, buf, sizeof(val));
```

This applies to all cache deserialization code and any raw buffer-to-struct casting. `__attribute__((packed))` structs have the same hazard when accessed via member reference.

#### Template and `std::function` Bloat
Each template instantiation generates a separate binary copy. `std::function<void()>` adds ~2–4 KB per unique signature and heap-allocates its closure. Avoid both in library code and any path called from the render loop:

```cpp
// Avoid — heap-allocating, large binary footprint:
std::function<void()> callback;

// Prefer — zero overhead:
void (*callback)() = nullptr;

// For member function + context (common activity callback pattern):
struct Callback { void* ctx; void (*fn)(void*); };
```

When a template is necessary, limit instantiations: use explicit template instantiation in a `.cpp` file to prevent the compiler from generating duplicates across translation units.

---

### Error Handling Philosophy

**Source**: [src/main.cpp:132-143](../src/main.cpp), [lib/GfxRenderer/GfxRenderer.cpp:10](../lib/GfxRenderer/GfxRenderer.cpp)

**Pattern Hierarchy**:
1. **LOG_ERR + return false** (90%): `LOG_ERR("MOD", "Failed: %s", reason); return false;`
2. **LOG_ERR + fallback**: `LOG_ERR("MOD", "Unavailable"); useDefault();`
3. **assert(false)**: Only for fatal "impossible" states (framebuffer missing)
4. **ESP.restart()**: Only for recovery (OTA complete)

**Rules**: NO exceptions, NO abort(), ALWAYS log before error return

### Heap Buffer Allocation

**Prefer `makeUniqueNoThrow` over `malloc`.** Both are nothrow (return `nullptr` on OOM rather than calling `abort()`), but `malloc` requires a manual `free` on every return path — a common source of leaks. `makeUniqueNoThrow<uint8_t[]>(size)` from `lib/Memory/Memory.h` frees automatically when it goes out of scope.

**Preferred pattern**:
```cpp
#include <Memory.h>

auto buffer = makeUniqueNoThrow<uint8_t[]>(bufferSize);
if (!buffer) {
  LOG_ERR("MODULE", "OOM: %d bytes", bufferSize);
  return false;
}

processData(buffer.get(), bufferSize);
// freed automatically — no manual free needed, no leak on early return
```

**`malloc` or `new (std::nothrow)` are still acceptable** when the buffer must be passed to a C API that takes ownership and frees it itself (e.g., certain SDK callbacks). In that case follow the manual pattern:
```cpp
auto* buffer = static_cast<uint8_t*>(malloc(bufferSize));  // or new (std::nothrow) uint8_t[bufferSize]
if (!buffer) {
  LOG_ERR("MODULE", "OOM: %d bytes", bufferSize);
  return false;
}
sdkApiThatTakesOwnership(buffer, bufferSize);  // SDK calls free() / delete[]
```

**Rules**:
- **Prefer `makeUniqueNoThrow`** — automatic cleanup eliminates leak risk on error paths
- **ALWAYS check for nullptr** after any allocation and `LOG_ERR` before returning false
- **Raw allocation only** when a C API takes ownership; document why in a comment

**Examples in codebase**:
- Memory utilities: [Memory.h](../lib/Memory/Memory.h) (`makeUniqueNoThrow`)
- Transfer queue (allocated per transfer): [StudioBluetooth.cpp](../src/project_stick/StudioBluetooth.cpp)
- Bitmap rendering: [GfxRenderer.cpp:439-440](../lib/GfxRenderer/GfxRenderer.cpp)

### Heap Allocation with `new`: Always Use `makeUniqueNoThrow`

**CRITICAL**: With `-fno-exceptions`, bare `new` on OOM calls `abort()` — it does NOT return `nullptr`. Always use `makeUniqueNoThrow` from `lib/Memory/Memory.h`, which wraps `new (std::nothrow)` and returns a `std::unique_ptr` that is null on OOM and automatically frees on scope exit.

**Preferred pattern**:
```cpp
#include <Memory.h>

auto obj = makeUniqueNoThrow<MyClass>(args);
if (!obj) { LOG_ERR("MOD", "OOM: MyClass"); return false; }

auto buf = makeUniqueNoThrow<uint8_t[]>(size);
if (!buf) { LOG_ERR("MOD", "OOM: %d bytes", size); return false; }

// Pass to C APIs via .get(); unique_ptr frees automatically on return
someApi(buf.get(), size);
```

**`new (std::nothrow)` directly is acceptable** when the object must be passed to a C API that takes ownership and calls `delete` itself:
```cpp
auto* obj = new (std::nothrow) MyClass(args);
if (!obj) { LOG_ERR("MOD", "OOM: MyClass"); return false; }
sdkApiThatTakesOwnership(obj);  // SDK calls delete
```

**Rules**:
- **Prefer `makeUniqueNoThrow`** — automatic cleanup eliminates leak risk on error paths
- **NEVER use bare `new`** — always `makeUniqueNoThrow` or `new (std::nothrow)`
- **ALWAYS `LOG_ERR` before returning false** on OOM
- **Use `.get()`** to pass the raw pointer to C-style APIs; ownership stays with the `unique_ptr`
- **`new (std::nothrow)` directly only** when a C API takes ownership; document why in a comment

**Examples in codebase**:
- Memory utilities: [Memory.h](../lib/Memory/Memory.h) (`makeUniqueNoThrow`)

---

## UI and Orientation Guidelines

### Orientation-Aware Logic
* No Hardcoding: Never assume 800 or 480. Use renderer.getScreenWidth() and renderer.getScreenHeight().
* Viewable Area: Use renderer.getOrientedViewableTRBL() to stay within physical bezel margins.

### Logical Button Mapping

**Source**: [src/MappedInputManager.cpp:20-55](../src/MappedInputManager.cpp)

Constraint: Physical button positions are fixed on hardware; the logical layout is fixed too (front-button remapping was removed in 2.7.4).

**Button Categories**:
1. **Physical Fixed** (Up/Down side buttons):
   - `Button::Up` → Always `HalGPIO::BTN_UP`
   - `Button::Down` → Always `HalGPIO::BTN_DOWN`

2. **Front buttons** (fixed layout):
   - `Button::Back` → `HalGPIO::BTN_BACK`
   - `Button::Confirm` → `HalGPIO::BTN_CONFIRM`
   - `Button::Left` → `HalGPIO::BTN_LEFT`
   - `Button::Right` → `HalGPIO::BTN_RIGHT`

3. **Page navigation**:
   - `Button::PageBack` / `Button::PageForward` → side buttons

**Implementation**:
- Activities use **logical buttons** (e.g., `Button::Confirm`)
- `MappedInputManager` translates to **physical hardware buttons**
- Orientation changes handled separately by renderer coordinate transforms

**Rule**: Always use `MappedInputManager::Button::*` enums, never raw `HalGPIO::BTN_*` indices.

### UITheme (The GUI Macro)
* Rule: All UI rendering must go through the GUI macro (UITheme). 
* Do not hardcode fonts, colors, or positioning. This ensures orientation-aware layout consistency.

---

## Common Patterns

### Singleton Access
**Available Singletons**:
```cpp
#define SETTINGS CrossPointSettings::getInstance()  // User settings
#define APP_STATE CrossPointState::getInstance()    // Runtime state
#define GUI UITheme::getInstance()                   // The UI theme (Lyra, the only one since 2.7.4)
#define Storage HalStorage::getInstance()            // SD card I/O
#define I18N I18n::getInstance()                     // Internationalization
```

### Activity Lifecycle and Memory Management

**Source**: [src/main.cpp:132-143](../src/main.cpp)

**CRITICAL**: Activities are **heap-allocated** and **deleted on exit**.

```cpp
// main.cpp navigation pattern
void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;  // Activity deleted here!
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;  // Heap-allocated activity
  currentActivity->onEnter();
}
```

**Memory Implications**:
- Activity navigation = `delete` old activity + `new` create next activity
- Any memory allocated in `onEnter()` MUST be freed in `onExit()`
- FreeRTOS tasks MUST be deleted in `onExit()` before activity destruction
- Member `FsFile` handles MUST be closed in `onExit()` (local `FsFile` variables auto-close via destructor)

**Activity Pattern**:
```cpp
void onEnter()  { Activity::onEnter(); /* alloc: buffer, tasks */ render(); }
void loop()     { mappedInput.update(); /* handle input */ }
void onExit()   { /* free: vTaskDelete, free buffer, close member FsFiles */ Activity::onExit(); }
```

**Critical**: Free resources in reverse order. Delete tasks BEFORE activity destruction.

### FreeRTOS Task Guidelines

**Source**: [src/activities/util/KeyboardEntryActivity.cpp:45-50](../src/activities/util/KeyboardEntryActivity.cpp)

**Pattern**: See Activity Lifecycle above. `xTaskCreate(&taskTrampoline, "Name", stackSize, this, 1, &handle)`

**Stack Sizing** (in BYTES, not words):
- **2048**: Simple rendering (most activities)
- **4096**: Network
- Monitor: `uxTaskGetStackHighWaterMark()` if crashes

**Rules**: Always `vTaskDelete()` in `onExit()` before destruction. Use mutex if shared state.

### Global Font Loading

**Source**: `src/main.cpp` (`setupDisplayAndFonts`)

Built-in fonts are global `EpdFont`/`EpdFontFamily` objects: Ubuntu UI 10/12 pt (regular/bold, with Hebrew,
Arabic presentation forms and Vietnamese), Noto Sans 8 pt (small), and Noto Sans SC 10/12/13 pt (key hint labels, Chinese UI
fallback and StockStick body text). Reader body fonts were removed in 2.0.0. Font IDs are in `src/fontIds.h`
(generated by `lib/EpdFont/scripts/build-font-ids.sh`).

---

## Testing and Debugging

### Build Commands

**Via CLI**:
```bash
# Build firmware (default environment)
pio run

# Build and upload to device
pio run -t upload

# Build specific environment
pio run -e gh_release

# Clean build artifacts
pio run -t clean

# Upload filesystem data (if using SPIFFS/LittleFS)
pio run -t uploadfs
```

**Via VS Code**:
* Use PlatformIO toolbar: Build (✓), Upload (→), Clean (🗑️)
* Or Command Palette: `PlatformIO: Build`, `PlatformIO: Upload`, etc.

### Monitoring and Debugging

```bash
# Enhanced monitor with color/logging (recommended)
python3 scripts/debugging_monitor.py

# Standard PlatformIO monitor
pio device monitor

# Combined upload + monitor
pio run -t upload && pio device monitor
```

**Via VS Code**: Click Monitor (🔌) button in PlatformIO toolbar

### Code Quality

```bash
# Static analysis (cppcheck)
pio check

# Format code (clang-format) - Windows Git Bash
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Format code (clang-format) - Linux
clang-format -i src/**/*.cpp src/**/*.h
```

### Debugging Crashes

**Common Crash Causes**:

1. **Out of Memory** (Most common):
   ```cpp
   LOG_DBG("MEM", "Free heap: %d bytes", ESP.getFreeHeap());
   ```
   - Monitor heap usage throughout activity lifecycle
   - Check if large allocations (>10KB) occur before crash
   - Verify buffers are freed in `onExit()`

2. **Stack Overflow**:
   ```cpp
   LOG_DBG("TASK", "Stack high water: %d", uxTaskGetStackHighWaterMark(taskHandle));
   ```
   - Occurs during deep recursion or large local variables
   - Increase task stack size in `xTaskCreate()` (2048 → 4096)
   - Move large buffers to heap with malloc

3. **Use-After-Free**:
   - Activity deleted but task still running
   - Always `vTaskDelete()` in `onExit()` BEFORE activity destruction
   - Set pointers to `nullptr` after `free()`

4. **Corrupt SD State**:
   - Studio partial transfers under `.crosspoint/studio/` resume or are discarded; `project_stick.json` holds only
     identity, binding, alert and pending-event state
   - Deleting `.crosspoint/` resets settings, device identity, binding and Wi-Fi credentials (device must be re-bound over BLE setup)

5. **Watchdog Timeout**:
   - Loop/task blocked for >5 seconds
   - Add `vTaskDelay(1)` in tight loops
   - Check for blocking I/O operations

**Verification Steps**:
1. Check serial output for stack traces
2. Monitor heap with `ESP.getFreeHeap()` before/after operations
3. Verify task deletion with task list (`vTaskList()`)
4. Test with `LOG_LEVEL=2` (debug logging enabled)

---

## Git Workflow and Repository Awareness

### Repository Detection Protocol

**CRITICAL**: ALWAYS verify repository context before git operations. This could be:
- A **fork** with `origin` pointing to personal repo, `upstream` to main repo
- A **direct clone** with `origin` pointing to main repo
- Multiple collaborator remotes

**Verification Commands** (run at session start):
```bash
# Check current branch
git branch --show-current

# Check all remotes
git remote -v

# Identify main branch name (could be 'main' or 'master')
git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@^refs/remotes/origin/@@'

# Check working tree status
git status --short
```

**Example Output** (forked repository):
```text
origin      https://github.com/<your-username>/crosspoint-reader.git (fetch/push)
upstream    https://github.com/crosspoint-reader/crosspoint-reader.git (fetch/push)
```

### Git Operation Rules

1. **Never assume branch names**:
   ```bash
   # Bad: git push origin main
   # Good: git push origin $(git branch --show-current)
   ```

2. **Never assume remote names or write permissions**:
   - **Forked repos**: Push to `origin` (your fork), submit PR to `upstream`
   - **Direct contributors**: May push feature branches to `upstream`
   - **Always ask**: "Should I push to origin or create a PR?"

3. **Check for upstream changes before starting work**:
   ```bash
   # Sync fork with upstream (if applicable)
   git fetch upstream
   git merge upstream/main  # or upstream/master
   ```

4. **Use explicit remote and branch names**:
   ```bash
   # Check remotes first
   git remote -v

   # Use explicit syntax
   git push <remote> <branch>
   ```

### Branch Naming Convention

**For feature/fix branches**:
```text
feature/<short-description>       # New features
fix/<issue-number>-<description>  # Bug fixes
refactor/<component-name>         # Code refactoring
docs/<topic>                      # Documentation updates
```

**Examples**:
- `feature/sd-download-progress`
- `fix/123-orientation-crash`
- `refactor/hal-storage`

### Commit Message Format

**Pattern**:
```text
<type>: <short summary (50 chars max)>

<optional detailed description>

```

**Types**: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

**Example**:
```text
feat: add real-time SD download progress bar

Implements progress tracking for book downloads using
UITheme progress bar component with heap-safe updates.

Tested in all 4 orientations with 5MB+ files.
```

### When to Commit

**DO commit when**:
- User explicitly requests: "commit these changes"
- Feature is complete and tested on device
- Bug fix is verified working
- Refactoring preserves all functionality
- All tests pass (`pio run` succeeds)

**DO NOT commit when**:
- Changes are untested on actual hardware
- Build fails or has warnings
- Experimenting or debugging in progress
- User hasn't explicitly requested commit
- Files excluded by `.gitignore` would be included — always run `git status` and cross-check against `.gitignore` before staging (e.g., `*.generated.h`, `.pio/`, `compile_commands.json`, `platformio.local.ini`)

**Rule**: **If uncertain, ASK before committing.**

---

## Generated Files and Build Artifacts

### Files Generated by Build Scripts

**NEVER manually edit these files** - they are regenerated automatically:

1. **I18n Headers** (generated by `scripts/gen_i18n.py`):
   - `lib/I18n/I18nKeys.h`, `lib/I18n/I18nStrings.h`, `lib/I18n/I18nStrings.cpp`
   - **Source**: YAML translation files in `lib/I18n/translations/` (one per language)
   - **To modify**: Edit source YAML files, then run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
   - **Commit**: Source YAML files only. All three generated files (`I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`) are in `.gitignore` and regenerated at build time.

2. **Build Artifacts** (in `.gitignore`):
   - `.pio/` - PlatformIO build output
   - `build/` - Compiled binaries
   - `*.generated.h` - Any auto-generated headers
   - `compile_commands.json` - LSP/IDE metadata

### Modifying Generated Content Workflow

**To add/modify translations (i18n)**:
1. Add the key to `lib/I18n/translations/chinese.yaml` (the only compiled catalogue and the key reference) and
   English text to `english.yaml` (fallback for SD packs). See `docs/i18n.md`.
2. The build runs `scripts/gen_i18n.py`; it fails if source references a key missing from `chinese.yaml`.
3. Other languages ship as SD packs: `python3 scripts/build_lang_pack.py`.
4. **Commit** source YAML files only; `I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` are generated and gitignored.

**To use translated strings in code**:
```cpp
#include <I18n.h>
// Use tr() macro with StrId enum (defined in generated I18nKeys.h)
renderer.drawText(FONT_UI, x, y, tr(STR_LOADING), true);
```

**To add custom fonts**:
1. Place source fonts in `lib/EpdFont/fontsrc/` (gitignored)
2. Run conversion script (see `lib/EpdFont/README`)
3. Update global font objects in `src/main.cpp:40-115`
4. Add font ID constant to `src/fontIds.h`

---

## Local Development Configuration

### platformio.local.ini (Personal Overrides)

**Purpose**: Personal development settings that should NEVER be committed.

**Use Cases**:
- Serial port configuration (varies by machine)
- Debug flags for specific testing
- Local build optimizations
- Developer-specific paths

**Example** `platformio.local.ini`:
```ini
# platformio.local.ini (gitignored)
[env:default]
upload_port = COM7              # Windows: COMx, Linux: /dev/ttyUSBx
monitor_port = COM7

build_flags =
  ${base.build_flags}
  -DMY_DEBUG_FLAG=1             # Personal debug flags
  -DTEST_FEATURE_ENABLED=1
```

**Configuration Hierarchy**:
1. `platformio.ini` - **Committed**, shared project settings
2. `platformio.local.ini` - **Gitignored**, personal overrides
3. Local file extends/overrides base config

**Rules**:
- **NEVER commit** `platformio.local.ini`
- **NEVER put** personal info (serial ports, credentials) in main `platformio.ini`
- Use `${base.build_flags}` to extend (not replace) base flags

---

## Testing and Verification Workflow

### Testing Checklist

**AI agent scope** (what you CAN verify):
1. ✅ **Build**: `pio run -t clean && pio run` (0 errors/warnings)
2. ✅ **Quality**: `pio check` + `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
3. ✅ **Format**: Commit messages (`feat:`/`fix:`), no `.gitignore`-excluded files staged (e.g., `*.generated.h`, `.pio/`, `platformio.local.ini`)
4. ✅ **CI**: Fix GitHub Actions failures before review
5. ✅ **Code review**: Ensure orientation-aware logic is correct in all 4 modes by inspecting switch/case coverage

**Human tester scope** (flag these for the user):
6. 🔲 **Device**: Test on hardware
7. 🔲 **OTA**: Run the on-device checklist in `docs/firmware-ota.md` for any OTA/boot/flash change
8. 🔲 **Heap**: `ESP.getFreeHeap()` > 50KB with Wi-Fi, TLS and BLE active, no leaks

### CI/CD Pipeline Awareness

**GitHub Actions** run automatically on pull requests:

| Workflow | File | Purpose |
|----------|------|---------|
| Build Check | `.github/workflows/ci.yml` | Verifies code compiles |
| Format Check | `.github/workflows/pr-formatting-check.yml` | Validates clang-format |
| Release Build | `.github/workflows/release.yml` | Production releases |
| RC Build | `.github/workflows/release_candidate.yml` | Release candidates |

**Rules**:
- **Fix CI failures BEFORE** requesting review
- CI runs on: Push to PR, PR updates
- Format check fails → Run clang-format locally
- Build check fails → Fix compile errors

---

## Serial Monitoring and Live Debugging

### Serial Monitor Options

1. **Enhanced**: `python3 scripts/debugging_monitor.py` (color-coded, recommended)
2. **Standard**: `pio device monitor` (basic, no colors)
3. **VS Code**: Monitor (🔌) button (IDE-integrated)

### Live Debugging Patterns

**Heap**: `LOG_DBG("MEM", "Free: %d", ESP.getFreeHeap());` (every 5s in loop)
**Stack**: `uxTaskGetStackHighWaterMark(nullptr)` (< 512 bytes → increase stack)
**Flush**: `logSerial.flush();` (force output before crash)

**Port Detection**: Windows: `mode` | Linux: `ls /dev/ttyUSB* /dev/ttyACM*` or `dmesg | grep tty`

---

## Firmware Versioning and OTA

- Bump `[crosspoint] version` and `build` in `platformio.ini` together; `build` must strictly increase
  (`major*10000 + minor*100 + patch` for final releases). Never lower `min_install_build` below 20000.
- Every image embeds `stick_firmware_descriptor` (`src/platform/StickFirmwareDescriptor.cpp`); installers
  reject images without it (except recovery mode).
- Package releases only with `scripts/firmware_release.py`; follow the on-device checklist in
  `docs/firmware-ota.md` before registering a build in the catalogue.
- Code that runs before `ota_trial::onBoot()` in `setup()` cannot be rolled back by the app; keep it minimal.

---

Philosophy: We are building a dedicated StockStick device, not a Swiss Army knife. If a feature adds RAM pressure or image size without serving StockStick content, sync or device operations, it is Out of Scope.
