# Altair Front Panel to Seeed XIAO ESP32-S3 Tiny Adapter

Tiny 2-layer KiCad layout intended to minimise board area.

Approximate board outline: **25.5 mm x 28.2 mm**. This is close to the requested 28 mm x 25 mm envelope, but the 1x11 2.54 mm header alone spans 25.4 mm, so one board dimension must be slightly over 27 mm once edge clearance is included.

## Mapping

| Altair header | Signal | XIAO pin | ESP32-S3 GPIO |
|---:|---|---|---|
| 1 | LOAD | D0 | GPIO1 |
| 2 | SWITCH_CS | D1 | GPIO2 |
| 4 | MISO | D9 | GPIO8 |
| 5 | MOSI | D10 | GPIO9 |
| 6 | RESET / MR | D3 | GPIO4 |
| 7 | CLK / SCK | D8 | GPIO7 |
| 8 | LED_STORE | D4 | GPIO5 |
| 9 | LED_OE | D5 | GPIO6 |
| 10 | 3V3 | 3V3 | power |
| 11 | GND | GND | ground |

Pin 3 is unconnected.

## Notes before fabrication

- Verify the physical pin-1 orientation of the Altair header before ordering.
- This design assumes J1 is fitted to the underside if the board is intended to snap onto an existing male header.
- This is a compact routing pass, not a professionally DRC-verified release. Open in KiCad, run DRC, and visually inspect against the actual kit before manufacture.
- The XIAO can be socketed using 2x7 2.54 mm female headers, or soldered using pin headers.
