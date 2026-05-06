# ESP32-S3 Altair 8800 Emulator

Altair 8800 emulator for ESP32-S3 boards, built with ESP-IDF v6.0. The project runs an Intel 8080/Altair environment with CP/M disks, physical display output, SD-card disk storage, WiFi setup, WebSocket terminal access, Bluetooth keyboard input, and an OpenAI-compatible chat I/O port for BDS C applications.

## Current Hardware Support

The project currently supports two ESP32-S3 board configurations:

| Board | Display | Config defaults |
|---|---|---|
| WAVESHARE-ESP32-S3-Touch-LCD-3.5B | AXS15231B QSPI LCD, 480x320 VT100/front-panel display | `sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults` |
| Freenove ESP32-S3 | ILI9341 TFT, 320x240 front panel | `sdkconfig.freenove.defaults` |

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

- ESP-IDF v6.0 for `esp32s3`.
- ESP-IDF VS Code extension, recommended for build/flash/monitor.
- ESP32-S3 board matching one of the supported configs.
- SD card formatted for the project disk layout when using CP/M disk images.

Install ESP-IDF tools for ESP32-S3 if needed:

```bash
$HOME/.espressif/v6.0/esp-idf/install.sh esp32s3
```

For CLI use, source ESP-IDF before running `idf.py`:

```bash
source $HOME/.espressif/v6.0/esp-idf/export.sh
```

## Swapping Target Board Configs

The board selection is stored in `sdkconfig`. The common ESP-IDF settings are in `sdkconfig.defaults`, and the board/display choice is layered on top with one of the board-specific defaults files.

When switching boards, remove the generated `sdkconfig`, rebuild with the common defaults plus the board defaults, then flash the resulting binary.

### VS Code Tasks

This workspace includes two VS Code tasks for switching the active generated config:

- `Altair: Switch config to Waveshare 3.5B AXS15231B`
- `Altair: Switch config to Freenove ILI9341`

Run one from **Terminal > Run Task...** or the command palette with **Tasks: Run Task**. Each task sources ESP-IDF, removes the generated `sdkconfig`, and runs `idf.py reconfigure` with the common defaults plus the selected board defaults. After the task completes, use the normal ESP-IDF build and flash commands for that board.

### Switch to Waveshare 3.5B AXS15231B

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

### Switch to Freenove ILI9341

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.freenove.defaults" build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

If you want to use the checked-in full Freenove snapshot instead of regenerating from defaults:

```bash
cp sdkconfig.freenove sdkconfig
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

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
- `sdkconfig.defaults`: active defaults file used when regenerating `sdkconfig`.
- `sdkconfig.WAVESHARE-ESP32-S3-Touch-LCD-3.5B.defaults`: Waveshare 3.5B defaults.
- `sdkconfig.freenove.defaults`: Freenove defaults.
- `sdkconfig.freenove`: full generated Freenove sdkconfig snapshot.
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
