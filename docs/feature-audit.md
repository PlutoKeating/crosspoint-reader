# Feature audit (firmware 2.7.4)

Report only: nothing below has been removed unless it says so. Each row says
what the feature is for in StockStick (a Xteink X3 that shows Studio cards
delivered by the mini program over BLE), what it costs, and a recommendation.
"Flash" is program size (the app slot is 6.25 MB, the 2.7.4 image is about
3.43 MB); "RAM" is internal SRAM, the scarce resource (see memory-budget.md).

Removed in 2.7.4 already: the Display and Controls settings tabs with every
setting on them (theme, sleep screen, refresh frequency, fading fix, short
power-key action, button remapping, front-button layout, power-key sleep
time), the RoundedRaff theme, BMP sleep images, 40 unused strings in 32
translations. See RELEASE_NOTES.md.

## Pages (activities)

| Page | What it does in StockStick | Cost | Recommendation |
|---|---|---|---|
| StockStick (`ProjectStickActivity`) | The product: the card, key hints, bubbles, notices, keyguard, setup QR, status screen | — | Keep |
| Home (`HomeActivity`) | Launcher with two entries: StockStick and 设置 | small | Keep for now. It only exists to reach Settings; a Back on the card page could open Settings directly and the launcher could go (one less page, one less framebuffer render on the way to Settings) |
| Boot splash (`BootActivity`) | Logo + "正在启动" + version on a cold boot; quick resume after the power-key sleep skips it | small | Keep (first boot, SD-error and crash boots need something on screen) |
| Sleep (`SleepActivity`) | Moon marker over the card, saves the quick-resume frame | small | Keep |
| Crash report (`CrashActivity`) | After a panic: shows the stored panic reason once | small | Keep: the only on-device trace of a field crash (the coredump partition and `crash_report.txt` are for us, this is for the user) |
| Full-screen message | Only "SD card error" at boot | tiny | Keep |
| Confirmation (`ConfirmationActivity`, `OptionPopup`) | Firmware-update confirmations | small | Keep |
| Wi-Fi selection + on-device keyboard (`WifiSelectionActivity`, `KeyboardEntryActivity`) | Scan, pick, type a password with four keys | ~1 page + keyboard layout | Keep the page (no-phone path), but it has two entry points: the card page's Confirm key ("Wi-Fi") and Settings > Wi-Fi 网络. Recommend dropping the card-page key: Wi-Fi is normally pushed by the phone over BLE, and the key invites accidental scans |
| Settings (`SettingsActivity`) | Wi-Fi 网络, 蓝牙, 固件更新, SD 卡固件更新, 语言 | small | Keep |
| Bluetooth (`BluetoothActivity`) | Turns the BLE radio off/on | small | Keep (privacy, airplane use) |
| Firmware update (`FirmwareUpdateActivity`) | Online check and install (Wi-Fi only, streams to SD, then the SD install) | — | Keep: the upgrade path for devices without the mini program |
| SD firmware update + picker (`SdFirmwareUpdateActivity`, `FirmwarePickerActivity`) | Installs a `.bin` from the SD card | small | Keep (recovery path; the BLE and Wi-Fi paths end in the same SD install) |
| Language (`LanguageSelectActivity`) | Built-in Chinese, optional SD language packs | small | Keep |

## Device features without a page

| Feature | Where | Status | Recommendation |
|---|---|---|---|
| Screenshot (POWER + DOWN → BMP in `/screenshots/`) | `ScreenshotUtil`, main loop | Reader-era. Since 2.7.4 it cannot capture a card (cards are streamed, there is no framebuffer to save); it still works on UI pages | Remove (support asks users for photos; it also writes to the SD card behind the user's back), or make it re-render the card first |
| Serial `CMD:SCREENSHOT` | main loop | Same limitation; answers `SCREENSHOT_UNAVAILABLE` on a card | Keep (debug builds only would be better: `gh_release` has no serial log anyway) |
| Tilt sensor (`HalTiltSensor`) | main loop | Reader page-turn by tilting; StockStick only uses "tilted" as an activity signal (keeps full CPU speed) and powers it down for sleep | Remove the tilt events; keep only the power-down (saves a poll per loop) |
| Touch / X4 Pro home gesture | `HalGPIO`, `ActivityManager::loop` | X4 Pro only (the X3 has no touch panel) | Remove with X4 support (below) |
| X4 board support (`FREEINK_DEVICE_X4=1`, SSD1677 driver, X4 layouts) | every build env | StockStick ships on the X3 only; cards are 528 × 792 and never render on the X4. SKILL.md currently keeps X3/X4 runtime detection on purpose | Product decision: an X3-only build drops the SSD1677 driver, the X4 LUTs and every X4 branch (flash, less code to keep correct). Keep while X4 units are used for testing |
| SD-card fonts (`lib/EpdFont/SdCardFont*`, `GfxRenderer::sdCardFonts_`) | renderer | Nothing registers an SD font any more (reader feature); the lookup still runs per glyph draw | Remove (flash; one map lookup per text draw) |
| Language packs + 31 translations | `/.crosspoint/lang/*.lang`, `scripts/build_lang_pack.py` | Chinese is built in; the other 31 exist only as packs a user would have to copy to the SD card. The fonts cover Latin/Cyrillic/CJK; Arabic and Hebrew packs also pull in MiniBidi | Decide product scope: if only Chinese (+ English) ship, drop the other translations and MiniBidi (flash; translator upkeep) |
| Built-in fonts | `lib/EpdFont/builtinFonts` | Noto Sans SC 10/12/13 (≈ 7 MB of source, the bulk of `.flash.rodata`), Ubuntu 10/12 regular+bold, Noto Sans 8 | Audit sizes actually drawn: Noto Sans SC 12 is used by fallback text only; merging 12 into 13 would save roughly a third of the CJK font flash |
| Wolfssl trace hook (`WolfSslLog.cpp`) | network | Debug only | Keep (compiled out unless wolfSSL is the TLS backend) |
| Coredump partition (64 KB) | `partitions.csv` | Written on a panic, read back by us | Keep |
| SPIFFS partition (3.4 MB at `0xC90000`) | `partitions.csv` | Unused: nothing mounts SPIFFS/LittleFS | Leave it until a partition-table change is needed for another reason (changing the table needs a USB flash, an OTA cannot), then fold it into the app slots or drop it |

## SD-card content

| Item | Status | Recommendation |
|---|---|---|
| `自带外部字体` (bundled external fonts), `自带预览书籍` (bundled preview books) | Factory reader content on the card; no firmware code reads either folder | Tell users they can delete them; the firmware should never need them |
| `/screenshots/` | Created only by the screenshot combo | Goes away with the screenshot feature |
| `/.crosspoint/lang/` | Optional language packs | Keep with the language feature |
| `/.crosspoint/studio/`, `project_stick.json`, `ble.json`, `settings.json`, `firmware.*` | Product state | Keep |
| `/.crosspoint/sleep_frame.bin` | Quick-resume frame, deleted on boot | Keep |

## Libraries

| Library | Used by | Recommendation |
|---|---|---|
| `lib/ProjectStick` | protocol, policies, transfer, fw4 (host-tested) | Keep |
| `lib/GfxRenderer`, `lib/EpdFont`, `lib/Utf8`, `lib/I18n` | all drawing and text | Keep (minus SD fonts, above) |
| `lib/InflateReader`, `lib/uzlib` | compressed built-in fonts; BLE frame and firmware streams | Keep both (two inflaters: the font path could move to uzlib, saving one) |
| `lib/MiniBidi` | RTL language packs only | Remove with the RTL packs |
| `lib/JsonParser` (`StreamingJsonParser`) | nothing in `src/` | Remove (dead since the reader went) |
| `lib/Serialization` (`BufferedFile`) | nothing in `src/` | Remove (dead) |
| `lib/FsHelpers` | language packs, firmware picker, theme, screenshots | Keep |
| `lib/Memory` | no-throw allocation, build scratch | Keep (`BuildScratch` loans are reader-era; no caller lends the framebuffer any more) |
| `src/util/UrlUtils`, `src/util/StringUtils`, `src/util/TaskWatchdog.h` | nothing | Remove (dead) |

Dead code costs no RAM (the linker drops unreferenced functions) and little
flash, but it is code that still has to compile in both the firmware and the
simulator and that readers of the tree assume matters.
