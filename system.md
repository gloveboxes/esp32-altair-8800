# ESP32-S3 Altair 8800 — System Resource Map

Target board: **Waveshare ESP32-S3-Touch-LCD-3.5B** (AXS15231B QSPI LCD, 8 MB PSRAM, ESP-IDF v6.0).

## Cores

- **Core 1** — emulator only.
  - `altair_emu` runs the Intel 8080 inner loop and synchronous SD-card disk I/O.
  - Nothing else is pinned here so the CPU loop is never preempted by network or display work.
- **Core 0** — everything else: networking, Bluetooth, panel/VT100 rendering, file transfer, system services.

## Tasks

| Task | Core | Prio | Stack | HWM free | Util | Source |
|---|---|---|---|---|---|---|
| `altair_emu` | 1 | 10 | 3072 | 1204 | 61 % | [main/main.c](main/main.c) |
| httpd (websocket server) | 0¹ | 13 | IDF dflt | — | — | [main/websocket_server.c](main/websocket_server.c) |
| `ws_tx` (websocket console TX) | 0 | 11 | 4096 | — ² | — | [main/websocket_console.c](main/websocket_console.c) |
| `panel_update` | 0 | 7 | 3072 | 1740 | 43 % | [main/main.c](main/main.c) |
| `ft_io` (file transfer / RFS) | 0 | 6 | 4096 | — ² | — | [port_drivers/files_io.c](port_drivers/files_io.c) |
| `bt_keyboard` | 0 | 4 | 4096 | 1572 | 62 % | [main/bt_keyboard.c](main/bt_keyboard.c) |
| `wifi_setup` (one-shot) | 0 | 4 | 8192 | exits | — | [main/main.c](main/main.c) |

¹ `tskNO_AFFINITY`, observed running on Core 0.  
² Task wasn't active when the high-water-mark sample was taken.

`status_led` is compiled out for this board (no on-board WS2812).

### IDF-internal tasks (priorities for reference)

- `ipc0` / `ipc1` — 24
- BT / WiFi controllers — ~23
- `esp_timer` — 22
- LWIP `tiT` — 18
- idle — 0

### Priority ordering on Core 0 (high → low)

`esp_timer (22) > ipc/wifi/bt (~23–24) > tiT (18) > httpd (13) > ws_tx (11) > panel (7) > ft_io (6) > bt_keyboard / wifi_setup (4) > idle (0)`

The network stack always preempts panel rendering, so a wifi/console packet can never be blocked behind a 21 ms LCD flush.

## Memory (steady-state observed)

| Pool | Free | Min seen | Largest free block |
|---|---|---|---|
| Internal heap | ~50 KB | 37 895 | 25 600 |
| DMA-capable | ~43 KB | 30 107 | — |
| PSRAM | ~8 MB | ~8.0 MB | — |

### Major static allocations

| Buffer | Where | Size | Region |
|---|---|---|---|
| AXS framebuffer (`s_framebuffer`) | [front_panel/axs15231b_lcd.c](front_panel/axs15231b_lcd.c) | 300 KB (480×320×2) | PSRAM |
| AXS DMA flush A/B (ping-pong) | [front_panel/axs15231b_lcd.c](front_panel/axs15231b_lcd.c) | 2 × 20 KB = 40 KB | Internal DMA |
| Project task stacks (7 tasks) | various | ~33 KB | Internal |

## Display / render pipeline

- AXS15231B QSPI LCD, logical 480×320, native 320×480 (MADCTL MV).
- QSPI clock: **60 MHz** (70/80 MHz cause visible corruption).
- Framebuffer in PSRAM in stream order.
- DMA: ping-pong with two `DMA_ATTR static` 20 KB buffers (32 native rows × 320 px × 2 B each).
- 480-row frame sent in **15 chunks**; counting semaphore (max 2) released by `axs_on_color_transfer_done` ISR; `wait_all_dma_idle()` drains both slots before return.
- Panel cadence: 50 ms (~20 Hz).
- VT100 layout: 80 × 30 cells, 6 × 10 font, 5 × 7 glyphs with 1-row vertical offset.
- Bright-white `O` on black is rendered as a filled 5×7 oval (breakout-style ball glyph), matching the Pico reference.

## SD card

- 1-bit SDMMC, GPIO11 (CLK) / GPIO10 (CMD) / GPIO9 (D0).
- 88-DCDD disk emulator runs on Core 1, synchronous `fread`/`fwrite` against `/sdcard/Disks/*.dsk`.
- Sector size 137 B, 32 sectors/track, 77 tracks; standard Altair geometry.

## Build / flash

```bash
source $HOME/.espressif/v6.0.1/esp-idf/export.sh
ninja -C build -j6 all
idf.py -p /dev/cu.usbmodem* flash
```
