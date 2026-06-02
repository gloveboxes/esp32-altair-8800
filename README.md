# ESP32-S3 Altair 8800 Emulator

Altair 8800 emulator for ESP32-S3 boards, built and tested with ESP-IDF v6.0.1 (minimum required). The project runs an Intel 8080/Altair environment with CP/M disks, physical display output, SD-card disk storage, WiFi setup, WebSocket terminal access, Bluetooth keyboard input, and an OpenAI-compatible chat I/O port for BDS C applications.

## Current Hardware Support

The project currently supports four ESP32-S3 board configurations:

| Board | Display | Config defaults |
|---|---|---|
| WAVESHARE-ESP32-S3-Touch-LCD-3.5B | AXS15231B QSPI LCD, 480x320 VT100/front-panel display | `sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults` |
| Freenove ESP32-S3 LCD 2.8 | ILI9341 TFT, 320x240 front panel | `sdkconfig.FREENOVE-ESP32-S3-LCD-2.8.defaults` |
| Lonely Binary Altair Kit | No physical display; Bluetooth disabled; 16 MB flash; 8 MB Octal PSRAM | `sdkconfig.LONELY_BINARY_ALTAIR_KIT.defaults` |
| Seeed XIAO ESP32-S3 Altair Kit | Altair Front Panel Kit; Bluetooth disabled; internal flash storage; 8 MB flash; 8 MB Octal PSRAM | `sdkconfig.SEEED_XIAO_ESP32_S3.defaults` |

## Features

- Intel 8080 CPU emulator and Altair memory model.
- CP/M disk support using 88-DCDD disk images from SD card.
- Built-in disk images and generated disk headers under `disks/`.
- CP/M, BASIC, BDS C, and demo applications under `Apps/`.
- Physical front-panel rendering on supported displays.
- VT100 terminal renderer for the Waveshare AXS15231B board.
- USB Serial/JTAG console input.
- Optional Bluetooth keyboard input.
- WiFi provisioning through serial setup or captive portal fallback.
- WebSocket browser terminal after WiFi connects.
- Remote file system / file-transfer support through I/O ports.
- Time and utility I/O ports.
- OpenAI Chat Completions compatible I/O port for CP/M/BDS C programs.

## Chat / OpenAI-Compatible Endpoint Support

The ESP32 firmware exposes a chat I/O port used by the CP/M-side BDS C app in `Apps/CHAT/CHAT.C`. The CP/M app sends OpenAI-style JSON to the ESP32; the ESP32 forwards it to the configured endpoint and streams text back through I/O ports.

Supported providers:

- **OpenAI**: `https://api.openai.com/v1/chat/completions`, using a stored API key and pinned CA certificate.
- **OpenAI Compatible**: any OpenAI-compatible chat completions endpoint, including local Ollama endpoints such as `http://192.168.1.129:11434/v1/chat/completions`.

For compatible endpoints, `http://` and `https://` are both supported. The API key is optional, which keeps local Ollama-style deployments easy to use. The model and temperature are supplied by the CP/M-side request JSON, so OpenAI and compatible endpoints use the same model-selection path.

Chat I/O ports:

| Port | Direction | Purpose |
|---|---:|---|
| 120 | OUT | Trigger request processing |
| 121 | OUT | Request JSON byte stream |
| 122 | OUT | Reset response stream |
| 123 | IN | Response status |
| 124 | IN | Response data byte |

## Boot Configuration Manager

At boot, connect a serial monitor over USB Serial/JTAG. If serial is connected, the firmware offers one 5-second boot configuration prompt:

```text
Boot configuration
Press 'C' within 5 seconds to manage boot configuration.
Press Enter to continue boot now.
```

The boot configuration manager includes:

- `1` - Bluetooth keyboard setup.
- `2` - OpenAI / OpenAI-compatible chat endpoint setup.
- `3` - WiFi credentials setup.
- `Q` - Continue boot.

The submenus can show current settings, update stored values, clear values, and return to the main boot configuration menu. Settings are stored in ESP32 NVS.

## WiFi Setup

WiFi setup is managed by `main/wifi_setup.c` and `main/config.c`.

Startup behavior:

1. If stored WiFi credentials exist, the firmware connects automatically.
2. If connection succeeds, the device synchronizes time for TLS validation, enables chat networking, and starts the WebSocket terminal server.
3. If credentials are missing during interactive setup, you can enter them through the serial monitor.
4. If serial setup is skipped or WiFi connection fails during setup, the captive portal starts.

Captive portal mode prints the AP name and URL to the serial monitor and displays them on the active display backend.

## WebSocket Terminal

After WiFi connects, the firmware starts the WebSocket terminal server for both supported board configs. Open the device IP address shown in the serial log:

```text
Terminal page: http://<device-ip>/
```

The WebSocket console bridges browser input/output with the emulator terminal. On the AXS15231B build, output is mirrored to the on-device VT100 display and the browser terminal.

## Project Structure

```text
main/                 Application entry point, config, WiFi, WebSocket server
altair8800/           Intel 8080 CPU, memory, and disk controller emulation
front_panel/          Front panel, VT100 renderer, display abstraction
drivers/              Board/display/SD/Bluetooth helper drivers
port_drivers/         Emulator I/O ports: chat, files, time, utility
captive_portal/       WiFi setup captive portal
terminal/             Browser terminal assets
disks/                CP/M disk images and generated disk headers
Apps/                 CP/M, BASIC, BDS C, and demo applications
docs/                 Development notes
```

## Build Prerequisites

- ESP-IDF v6.0.1 or newer for `esp32s3` (the build will fail with a clear error if an older ESP-IDF is sourced).
- ESP-IDF VS Code extension, recommended for build/flash/monitor.
- ESP32-S3 board matching one of the supported configs.
- SD card formatted for the project disk layout when using CP/M disk images.

> **Install location.** This project expects ESP-IDF and its tools to live under `$HOME/.espressif/` — specifically `$HOME/.espressif/<version>/esp-idf/` for the SDK and `$HOME/.espressif/tools/` for the toolchain. The ESP-IDF extension does not expand VS Code variables such as `${userHome}` in all of its own settings, so its `idf.*` paths in `.vscode/settings.json` are literal local paths. After cloning, run `python3 scripts/update_vscode_idf_paths.py` to rewrite those paths for your user account before building in VS Code.

> **SDK version pinned in settings.** `.vscode/settings.json` hard-codes the v6.0.1 SDK path (for example `/Users/<you>/.espressif/v6.0.1/esp-idf`) along with version-specific subdirectories of the toolchain (e.g. `tools/xtensa-esp-elf/esp-15.2.0_20251204`, `tools/esp-clang/esp-20.1.1_20250829`, `python_env/idf6.0_py3.14_env`). When upgrading to a newer ESP-IDF, update these paths in `.vscode/settings.json` (and the `source` paths in `.vscode/tasks.json`) to match the new install. The CMake guard in `CMakeLists.txt` enforces only the *minimum* SDK version (v6.0.1) and is independent of these IDE paths.

Install ESP-IDF tools for ESP32-S3 if needed:

```bash
$HOME/.espressif/v6.0.1/esp-idf/install.sh esp32s3
```

Update the VS Code ESP-IDF extension paths for your local account:

```bash
python3 scripts/update_vscode_idf_paths.py
```

If ESP-IDF is installed somewhere other than `$HOME/.espressif/v6.0.1/esp-idf`, pass the paths explicitly:

```bash
python3 scripts/update_vscode_idf_paths.py \
  --idf-path /path/to/esp-idf \
  --tools-path /path/to/.espressif \
  --python-env-path /path/to/python_env/idf6.0_py3.14_env
```

For CLI use, source ESP-IDF before running `idf.py`:

```bash
source $HOME/.espressif/v6.0.1/esp-idf/export.sh
```

### ESP-IDF environment for VS Code on macOS

The ESP-IDF VS Code extension shells out to `cmake`/`ninja` using the **process environment of the VS Code app**, not your interactive shell. On macOS, GUI apps launched from the Dock/Finder/Spotlight do **not** read `~/.zshrc` or `~/.zprofile`, so shell-only ESP-IDF setup can be invisible to them. Symptom:

```
CMake Error at CMakeLists.txt:26 (message):
  IDF_PATH=/Users/<you>/esp/esp-idf does not contain tools/cmake/project.cmake.
```

...or, mid-build, the bootloader sub-build fails with `IDF_PATH environment variable is different from inferred IDF_PATH` and a stale path.

This workspace keeps the extension environment explicit in `.vscode/settings.json` via `idf.customExtraVars`: it sets `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_TARGET`, `IDF_PYTHON_ENV_PATH`, `PYTHON`, `ESP_IDF_VERSION`, and a deterministic ESP-IDF `PATH` that includes the v6.0.1 Python environment, `idf.py`, toolchains, OpenOCD, Ninja, and Homebrew CMake. Keep those `idf.*` values as literal absolute paths; using `${userHome}` there makes the ESP-IDF extension treat the setup as missing.

Do **not** source `export.sh` from `~/.zshenv`. That file runs for every zsh invocation, including non-interactive probes from VS Code and tooling; sourcing ESP-IDF there can make `/bin/zsh` exit with code 1 and break terminals system-wide. If you want global static shell variables, keep `~/.zshenv` limited to simple exports only:

```bash
export IDF_PATH="$HOME/.espressif/v6.0.1/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif"
```

For interactive CLI use, source ESP-IDF in the terminal where you need it:

```bash
source "$HOME/.espressif/v6.0.1/esp-idf/export.sh"
```

After cloning, installing ESP-IDF, or changing ESP-IDF-related VS Code settings, **fully quit VS Code with Cmd+Q** (a window reload or "Reopen Folder" is not enough because VS Code's process environment is captured at launch). Then relaunch VS Code from the Dock or via `code <folder>`. Run **ESP-IDF: Doctor Command** and verify that `ESP-IDF Path`, `Virtual environment Python path`, `CMake`, `Ninja`, and `ESP-IDF Tools Path` are accessible. For CLI builds, source ESP-IDF first and then check the terminal environment:

```bash
source "$HOME/.espressif/v6.0.1/esp-idf/export.sh"
echo $IDF_PATH
# → /Users/<you>/.espressif/v6.0.1/esp-idf
```

> If you upgrade ESP-IDF, update `.vscode/settings.json`, `.vscode/tasks.json`, and any static `~/.zshenv` exports to the new path.

### CMake Tools extension note

If you have the Microsoft **CMake Tools** extension installed, it will try to configure this project directly with `cmake` (without sourcing `export.sh`) and fail. `.vscode/settings.json` already disables its auto-configure for this workspace and points its source dir at a non-existent folder so it stays out of the way:

```jsonc
"cmake.configureOnOpen": false,
"cmake.configureOnEdit": false,
"cmake.automaticReconfigure": false,
"cmake.sourceDirectory": "${workspaceFolder}/.no-cmake-tools"
```

Always build via the **ESP-IDF** extension or the workspace tasks (`Altair: Switch config to …`), not the CMake Tools status-bar buttons.

## Swapping Target Board Configs

The board selection is stored in `sdkconfig`. The common ESP-IDF settings are in `sdkconfig.defaults`, and the board/display choice is layered on top with one of the board-specific defaults files.

When switching boards, remove the generated `sdkconfig`, rebuild with the common defaults plus the board defaults, then flash the resulting binary.

### VS Code Tasks

This workspace includes VS Code tasks for switching the active generated config:

- `Altair: Switch config to Waveshare 3.5B AXS15231B`
- `Altair: Switch config to Freenove ESP32-S3 LCD 2.8`
- `Altair: Switch config to Lonely Binary Altair Kit`
- `Altair: Switch config to Seeed XIAO ESP32-S3 Altair Kit`

Run one from **Terminal > Run Task...** or the command palette with **Tasks: Run Task**. Each task sources ESP-IDF, removes the generated `sdkconfig`, and runs `idf.py reconfigure` with the common defaults plus the selected board defaults. After the task completes, use the normal ESP-IDF build and flash commands for that board.

### Switch to Waveshare 3.5B AXS15231B

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

### Switch to Freenove ESP32-S3 LCD 2.8

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.FREENOVE-ESP32-S3-LCD-2.8.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

### Switch to Lonely Binary Altair Kit

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.LONELY_BINARY_ALTAIR_KIT.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

The Lonely Binary Altair Kit profile has no physical display and disables Bluetooth keyboard support. The kit has 16 MB flash and 8 MB Octal PSRAM; PSRAM is configured at 40 MHz for reliable boot.

Do not wire an external SD card to GPIO35, GPIO36, GPIO37, or GPIO38 on this profile. Those pins are FSPI flash/PSRAM signals on ESP32-S3 modules with Octal PSRAM, and loading them can make the boot-time PSRAM memory test fail before the application starts. The Lonely Binary SDSPI mapping uses MOSI/DI on GPIO4, MISO/DO on GPIO5, SCLK on GPIO6, and CS on GPIO7.

### Switch to Seeed XIAO ESP32-S3 Altair Kit

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.SEEED_XIAO_ESP32_S3.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash
idf.py -p /dev/cu.usbmodem2101 storage-flash
idf.py -p /dev/cu.usbmodem2101 monitor
```

The Seeed XIAO ESP32-S3 profile targets the 8 MB flash / 8 MB Octal PSRAM XIAO board and uses the internal flash FAT partition for the emulator disk images. It drives the Altair Front Panel Kit directly from the XIAO header pins and leaves the board headless for terminal output through USB serial and the WebSocket terminal. Because the XIAO has 8 MB flash, this profile uses `partitions_8mb.csv` with a 4 MB app slot and a 3 MB FAT storage partition.

Wire the Altair Front Panel Kit to the XIAO ESP32-S3 as follows:

| Altair header pin | Front panel kit signal | XIAO pin | ESP32-S3 GPIO |
|---:|---|---|---:|
| 1 | `LOAD` / switch load | `D0` | `GPIO1` |
| 2 | `SWITCH_CS` | `D1` | `GPIO2` |
| 4 | `MISO` | `D9` / MISO | `GPIO8` |
| 5 | `MOSI` | `D10` / MOSI | `GPIO9` |
| 6 | `RESET` / MR | `D3` | `GPIO4` |
| 7 | `CLK` / SCK | `D8` / SCK | `GPIO7` |
| 8 | `LED_STORE` | `D4` / SDA | `GPIO5` |
| 9 | `LED_OE` | `D5` / SCL | `GPIO6` |
| 10 | `3V3` | `3V3` | power |
| 11 | `GND` | `GND` | ground |

Leave `D2` / `GPIO3` unused for this wiring; Seeed documents it as a JTAG strapping-related pin at reset. The profile does not use an external SD card, so do not wire SD storage to the SPI pins above.

Run `idf.py fullclean` when switching boards if you see stale build output, unexpected Kconfig values, or display-driver link/build issues. After a full clean, rerun the same `idf.py -D SDKCONFIG_DEFAULTS=... build` command for the target board.

In VS Code, use the ESP-IDF extension commands after swapping the config file:

- **ESP-IDF: Set Espressif Device Target**: choose `esp32s3`.
- **ESP-IDF: SDK Configuration Editor**: under **Altair 8800**, choose the target board/display.
- **ESP-IDF: Build your project**.
- **ESP-IDF: Flash your project**.
- **ESP-IDF: Monitor your device**.

## Building

### VS Code

Use the ESP-IDF extension commands from the command palette:

- **ESP-IDF: Set Espressif Device Target**: choose `esp32s3`.
- **ESP-IDF: SDK Configuration Editor**: select the target board/display if needed.
- **ESP-IDF: Build your project**.
- **ESP-IDF: Flash your project**.
- **ESP-IDF: Monitor your device**.

### CLI

For a normal build using the current `sdkconfig`:

```bash
idf.py set-target esp32s3
idf.py build
```

Flash and monitor, adjusting the port for your board:

```bash
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

If the build fails after switching configs or ESP-IDF versions:

```bash
idf.py fullclean
idf.py build
```

## Configuration Files

- `sdkconfig`: active generated configuration used by ESP-IDF.
- `sdkconfig.defaults`: common defaults file used when regenerating `sdkconfig`.
- `sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults`: Waveshare 3.5B defaults.
- `sdkconfig.FREENOVE-ESP32-S3-LCD-2.8.defaults`: Freenove ESP32-S3 LCD 2.8 defaults.
- `sdkconfig.LONELY_BINARY_ALTAIR_KIT.defaults`: Lonely Binary Altair Kit defaults.
- `sdkconfig.SEEED_XIAO_ESP32_S3.defaults`: Seeed XIAO ESP32-S3 Altair Front Panel Kit defaults.
- `partitions.csv`: partition layout.

When changing project-level ESP-IDF settings, update the appropriate defaults file so the configuration can be reproduced.

## Runtime Notes

- Emulator work is pinned to Core 1; networking, Bluetooth, display updates, and WebSocket work run on Core 0.
- The AXS15231B framebuffer is stored in PSRAM and flushed through DMA buffers.
- TLS certificate validation requires valid time. The firmware uses SNTP after WiFi connects before enabling OpenAI chat networking.
- WebSocket support is built and linked in both supported configs.

## Troubleshooting

- **No WebSocket terminal URL appears**: check WiFi connection and stored credentials. The server starts only after WiFi connects.
- **OpenAI chat says network unavailable**: confirm WiFi is connected and time synchronization completed.
- **OpenAI-compatible endpoint returns HTTP 404**: verify the endpoint path and that the requested model exists on the server. For Ollama, pull the model first.
- **TLS failures with OpenAI**: check that system time synchronized and that the endpoint is using HTTPS.
- **Build fails after switching boards**: run `idf.py fullclean`, then rebuild.
