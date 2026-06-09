---
name: esp32-s3
description: Build, flash, configure, and debug the ESP32-S3 Altair 8800 firmware with ESP-IDF v6.0.1+. Use when working on the ESP-IDF side of this repo — C/C++ firmware under main/, altair8800/, front_panel/, drivers/, port_drivers/, or captive_portal/; CMakeLists.txt component files; sdkconfig / sdkconfig.*.defaults; partitions.csv; or any task that mentions idf.py, esp32s3, board switching (Waveshare AXS15231B, Freenove ILI9341, Lonely Binary, Seeed XIAO), PSRAM, partitions, USB Serial/JTAG console, BLE keyboard, WiFi/captive portal, WebSocket terminal, SD-card disk images, or flashing/monitoring the device. NOT for CP/M-side BDS C or MAC/8080 assembly (use the bds-c or mac-asm skills).
---

# ESP32-S3 Altair 8800 Firmware

## Core Facts

- Target is fixed: `esp32s3`. Toolchain is ESP-IDF **v6.0.1 or newer** (the
  build fails fast with a clear message in the root `CMakeLists.txt` if an older
  or missing IDF is sourced).
- ESP-IDF is **not** activated automatically (see user memory: never source
  `export.sh` from shell rc files). Activate it per command:

  ```bash
  source "$HOME/.espressif/v6.0.1/esp-idf/export.sh" >/dev/null
  ```

- All `idf.py` commands run from the workspace root (`${workspaceFolder}`),
  which holds the top-level `CMakeLists.txt`.
- This is a multi-board project. The active board is selected by which
  `sdkconfig.*.defaults` file was last applied; the generated `sdkconfig` is
  disposable and is regenerated on every board switch.

## Repository Layout (firmware side)

```text
main/                Entry point, config, WiFi setup, WebSocket server
altair8800/          Intel 8080 CPU, memory, 88-DCDD disk controller
front_panel/         Front panel + VT100 renderer, display abstraction
drivers/             Board/display/SD/Bluetooth helper drivers
port_drivers/        Emulator I/O ports: chat, files, time, utility
captive_portal/      WiFi setup captive portal
terminal/            Browser terminal assets
disks/               CP/M disk images and generated disk headers
```

Most firmware sources are compiled into a **single `main` component**:
`main/CMakeLists.txt` lists them in the `app_sources` variable using relative
paths into the sibling folders (e.g. `../port_drivers/chat_io.c`,
`../altair8800/intel8080.c`, `../front_panel/...`) and registers them with one
`idf_component_register(...)` call. Folders like `altair8800/`, `front_panel/`,
`port_drivers/`, and `captive_portal/` are **not** standalone IDF components —
they have no own `CMakeLists.txt`. Only a few helpers under `drivers/`
(`drivers/sdcard_esp32`, `drivers/fatfs`) are real separate components.

When you add a `.c` file under one of those plain folders, add its relative path
to `app_sources` in `main/CMakeLists.txt`, and add any new include folder to that
same call's `INCLUDE_DIRS`. New REQUIRES (IDF components you call into) also go in
this `idf_component_register`. Do not edit `build/` — it is generated output.

## Board Configurations

| Board | Defaults file | Notes |
|---|---|---|
| Waveshare ESP32-S3 Touch LCD 3.5B | `sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults` | AXS15231B QSPI LCD 480x320, VT100 + front panel, BLE on, 16 MB flash, 8 MB Octal PSRAM |
| Freenove ESP32-S3 LCD 2.8 | `sdkconfig.FREENOVE-ESP32-S3-LCD-2.8.defaults` | ILI9341 TFT 320x240 front panel |
| Lonely Binary Altair Kit | `sdkconfig.LONELY_BINARY_ALTAIR_KIT.defaults` | No display, BLE disabled, 16 MB flash, 8 MB Octal PSRAM |
| Seeed XIAO ESP32-S3 Altair Kit | `sdkconfig.SEEED_XIAO_ESP32_S3.defaults` | Front panel kit, BLE disabled, internal flash storage, 8 MB flash, 8 MB Octal PSRAM |

`sdkconfig.defaults` holds the shared base (target, custom `partitions.csv`,
BLE HID host, 240 MHz CPU, USB Serial/JTAG console, PSRAM). Each board file is
layered **on top** of it. Always set both files in `SDKCONFIG_DEFAULTS`,
base first, with `;` as the separator.

### Switch boards

Switching deletes the stale `sdkconfig` and reconfigures from the layered
defaults. Prefer the VS Code tasks (`Altair: Switch config to ...`) or run the
equivalent directly:

```bash
source "$HOME/.espressif/v6.0.1/esp-idf/export.sh" >/dev/null
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults" reconfigure
```

Always confirm the intended board with the user before switching, then build.

## Build / Flash / Monitor

After activating ESP-IDF (and selecting a board), from the workspace root:

```bash
idf.py build                 # compile firmware
idf.py -p <PORT> flash       # flash app + bootloader + partition table
idf.py -p <PORT> monitor     # USB Serial/JTAG console (Ctrl-] to exit)
idf.py -p <PORT> flash monitor
idf.py storage-flash         # flash the CP/M SD-card disk images only
```

- Console is **USB Serial/JTAG** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), not a
  UART bridge. On macOS the port is typically `/dev/cu.usbmodem*`; on Linux
  `/dev/ttyACM*`. Ask the user for the port rather than guessing if unset.
- Prefer incremental `idf.py build`. Only run `idf.py fullclean` when changing
  partitions, PSRAM, or other settings that invalidate the build, or when the
  user reports stale-config symptoms. `fullclean` does not touch `sdkconfig`.
- `idf.py menuconfig` edits the live `sdkconfig`. Persist real defaults into the
  appropriate `sdkconfig.*.defaults` file so board switches keep them.

## Core Assignment (Altair emulator)

The Intel 8080 / Altair emulator runs **exclusively on Core 1**. It is a tight,
continuous instruction-execution loop, so Core 1 is effectively dedicated to it
and must not be shared with other long-running work. **All other Altair emulator
services run on Core 0** — WiFi, the WebSocket terminal, captive portal, BLE
keyboard, the chat/files/time/utility I/O port drivers, display rendering, and
SD-card disk I/O. When adding a new task, pin it to Core 0
(`xTaskCreatePinnedToCore(..., 0)`) unless it is part of the core emulation
itself, and never block Core 1 with networking, I/O, or display work.

Inter-core data transfer is done primarily through **thread-safe FreeRTOS
queues** (`xQueueCreate` / `xQueueSend` / `xQueueReceive`), with a separate
queue per direction — Core 1 -> Core 0 for requests and Core 0 -> Core 1 for
responses (see the chat and file-transfer port drivers in
`port_drivers/chat_io.c` and `port_drivers/files_io.c`). Follow this pattern for
new cross-core paths: pass small fixed-size messages or pointers through a
queue rather than sharing mutable state, and keep the Core 1 side non-blocking
(zero/short timeouts) so the emulator loop never stalls.

## Memory, Partitions, and PSRAM

- Custom partition table `partitions.csv` is used because the BLE-enabled app
  exceeds the stock large-app slot. `partitions_8mb.csv` exists for 8 MB boards.
  If the app outgrows its slot, adjust the partition CSV, not the linker.
- PSRAM is **Octal** (`CONFIG_SPIRAM_MODE_OCT`) at 80 MHz on the PSRAM-equipped
  boards. Large display buffers go in PSRAM so SDMMC, WiFi, and WebSocket keep
  internal RAM free. Use `MALLOC_CAP_SPIRAM` / `heap_caps_malloc` for big
  buffers; keep DMA-capable and ISR-touched buffers in internal RAM.
- The emulator runs CPU-intensive work on Core 1, so the Core 1 IDLE task
  watchdog is disabled (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`). Do not
  add long blocking work to a Core 0 task without yielding, or you risk tripping
  the watchdog there.

## ESP-IDF C Conventions (firmware side)

This is modern ESP-IDF C — the opposite of the CP/M-side BDS C rules. Do not
carry 7-character names, K&R definitions, or no-cast rules into firmware code.

- Use FreeRTOS + ESP-IDF APIs (`xTaskCreatePinnedToCore`, `esp_log`,
  `nvs_*`, `esp_wifi_*`, `heap_caps_*`).
- Log with `ESP_LOGI/W/E(TAG, ...)`; define a `static const char *TAG` per file.
- Check and propagate `esp_err_t`; use `ESP_ERROR_CHECK` only where a failure is
  genuinely fatal at boot. Avoid it on recoverable runtime paths.
- Persist settings (WiFi creds, chat endpoint, BLE pairing) in NVS, matching the
  existing patterns in `main/config.c` and `main/wifi_setup.c`.
- Match the surrounding component's existing style before introducing new
  patterns.

## Memory Best Practices (general embedded)

Prefer deterministic memory usage. Where possible:

- Use static, global, or stack allocation with fixed-size buffers.
- Allocate resources up front during initialization.
- Avoid heap allocation in normal runtime paths.
- Do not allocate memory inside ISRs, tight loops, drivers, or timing-critical
  code.
- Prefer internal RAM for hot paths, latency-sensitive code, DMA buffers, ISRs,
  drivers, and frequently accessed data.
- Use PSRAM only for larger, less time-critical buffers such as frame buffers,
  logs, caches, assets, or bulk data.
- Avoid PSRAM on hot paths where access latency, bandwidth, cache behaviour, or
  DMA compatibility may matter.
- If heap allocation is unavoidable, keep it bounded, check failures, define
  ownership clearly, and free resources predictably.
- Prefer caller-provided buffers over functions that allocate internally.

## Workflow

1. Confirm which board the user is targeting; if the wrong defaults are active,
   switch and reconfigure first.
2. Read the relevant component's `CMakeLists.txt` and neighboring source before
   editing. Add new sources/includes to the component registration.
3. Build incrementally with `idf.py build` and fix real errors. Run
   `get_errors` on edited files.
4. Flash and, when the user wants runtime verification, monitor over USB
   Serial/JTAG. For display work, follow the user's edit -> build/flash ->
   camera-verify loop (see user memory).
5. Reflash disk images with `idf.py storage-flash` only when the CP/M disk
   content changed.
6. Keep durable config changes in the right `sdkconfig.*.defaults`, never only
   in the generated `sdkconfig`.

## Don't

- Don't source `export.sh` from `~/.zshenv`/`~/.zshrc` — it breaks every
  non-interactive shell. Activate it inline per command instead.
- Don't edit anything under `build/`.
- Don't hardcode a serial port; ask if it isn't provided.
- Don't apply CP/M BDS C / MAC assembly conventions to ESP-IDF C code.
