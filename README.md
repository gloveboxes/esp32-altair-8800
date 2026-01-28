# ESP32-S3 Altair 8800 Emulator

An Altair 8800 emulator running on ESP32-S3 with WebSocket terminal access.

## Building

### Prerequisites

- ESP-IDF v6.0 or later installed (see [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/))
- ESP32-S3 target board

### Setup ESP-IDF Environment

Before running any `idf.py` commands, you must source the ESP-IDF environment script. This adds `idf.py` to your PATH and sets required environment variables.

```bash
# Run this in each new terminal session (adjust path to your ESP-IDF installation)
source $HOME/esp/esp-idf/export.sh
```

**Tip:** Add an alias to your shell profile (`~/.zshrc` or `~/.bashrc`):

```bash
alias get_idf='source $HOME/esp/esp-idf/export.sh'
```

Then simply run `get_idf` before building.

### Build Commands

```bash
# Source ESP-IDF environment
source $HOME/esp/esp-idf/export.sh

# Set target (first time only)
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuration

### Updating sdkconfig from sdkconfig.defaults

The `sdkconfig.defaults` file contains the project's default configuration settings. The `sdkconfig` file is generated from these defaults and may contain machine-specific settings.

**To regenerate sdkconfig from defaults:**

```bash
# Option 1: Delete sdkconfig and rebuild (recommended)
rm sdkconfig
idf.py build

# Option 2: Use reconfigure
idf.py reconfigure

# Option 3: Full clean rebuild
idf.py fullclean
idf.py build
```

**To open the configuration menu:**

```bash
idf.py menuconfig
```

After making changes in menuconfig, settings are saved to `sdkconfig`. To preserve important settings for the project, add them to `sdkconfig.defaults`.

**Note:** `sdkconfig` is typically added to `.gitignore` since it contains machine-specific paths. Only `sdkconfig.defaults` should be committed to version control.

## Project Structure

```
├── main/               # Application entry point and networking
├── altair8800/         # Altair 8800 emulator core (Intel 8080 CPU)
├── front_panel/        # ILI9341 display driver and panel rendering
├── port_drivers/       # I/O port emulation
├── disks/              # CP/M disk images
├── terminal/           # Web terminal HTML/JS
├── captive_portal/     # WiFi configuration portal
└── drivers/            # Hardware drivers (SD card)
```

## WebSocket Terminal

Connect to the device's IP address in a web browser to access the terminal. The server supports one client at a time - new connections automatically take over from existing ones.
