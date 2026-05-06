/**
 * @file vt100_terminal.h
 * @brief On-device VT100-style terminal display
 *
 * Renders emulator output as an 80x30 colour grid plus a 20 px status bar on
 * the 480x320 Waveshare AXS15231B display, matching the Pico VT100 layout.
 *
 * Thread-safe split: putchar() is called from the emulator task and updates
 * the terminal buffer; flush() is called from the panel display task and
 * renders dirty rows.
 */

#ifndef VT100_TERMINAL_H
#define VT100_TERMINAL_H

#include "sdkconfig.h"

#include <stdint.h>

#define VT100_COLS  80
#define VT100_ROWS  30

/**
 * @brief Initialise the VT100 terminal.
 *
 * Clears the character buffer, resets the cursor to (0,0), clears the
 * physical display to white, and marks every row dirty so the first
 * flush() call renders the blank terminal.
 *
 * Must be called after panel_display_init().
 */
void vt100_terminal_init(void);

/**
 * @brief Write one character to the terminal.
 *
 * Handles:
 *   - Printable ASCII 0x20–0x7E  – place glyph, advance cursor, wrap/scroll
 *   - CR  (0x0D) – move cursor to column 0
 *   - LF  (0x0A) – move cursor down one row, scroll if at bottom
 *   - BS  (0x08) – move cursor left one column and erase the vacated cell
 *   - All other bytes are silently discarded.
 *
 * Called from the emulator hot path. Thread-safe.
 *
 * @param c Character byte to write
 */
void vt100_terminal_putchar(uint8_t c);

/**
 * @brief Flush dirty rows to the display.
 *
 * Renders every row that has been modified since the last flush, then calls
 * panel_display_present() to push the updated framebuffer region to the
 * hardware. No-ops when there is nothing to redraw.
 *
 * Called from the panel display task.
 */
void vt100_terminal_flush(void);

/**
 * @brief Update the status bar CPU LEDs on colour VT100 displays.
 *
 * Monochrome displays do not have a status bar, so this is a no-op there.
 */
void vt100_terminal_update_status(uint16_t address, uint8_t data, uint16_t status);

/**
 * @brief Update the IP/host text shown in the colour VT100 status bar.
 *
 * Monochrome displays do not have a status bar, so this is a no-op there.
 */
void vt100_terminal_set_ip(const char *ip_addr, const char *hostname);

#endif /* VT100_TERMINAL_H */
