/**
 * @file vt100_terminal.h
 * @brief On-device VT100-style terminal display
 *
 * Renders emulator output as an 80x30 colour grid plus a 20 px status bar on
 * the 480x320 Waveshare AXS15231B display, matching the Pico VT100 layout.
 *
 * Thread-safe split: putchar() is called from the emulator task and queues
 * terminal output; flush() is called from the panel display task and drains
 * queued output in batches before rendering dirty rows.
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
 * Queues one byte for batched processing by vt100_terminal_flush().
 *
 * The flush path handles:
 *   - Printable ASCII 0x20–0x7E  – place glyph, advance cursor, wrap/scroll
 *   - CR  (0x0D) – move cursor to column 0
 *   - LF  (0x0A) – move cursor down one row, scroll if at bottom
 *   - BS  (0x08) – move cursor left one column and erase the vacated cell
 *   - All other bytes are silently discarded.
 *
 * Called from the emulator hot path. Non-blocking and thread-safe. If the
 * queue is full, the oldest queued byte is dropped to keep the emulator moving.
 *
 * @param c Character byte to write
 */
void vt100_terminal_putchar(uint8_t c);

/**
 * @brief Flush dirty rows to the display.
 *
 * Drains queued terminal bytes into the terminal buffer, renders every row
 * that has been modified since the last flush, then calls panel_display_present()
 * to push the updated framebuffer region to the hardware. No-ops when there is
 * nothing to redraw.
 *
 * Called from the panel display task.
 */
void vt100_terminal_flush(void);

/**
 * @brief Update periodic status-bar state on VT100 displays.
 */
void vt100_terminal_update_status(uint16_t address, uint8_t data, uint16_t status);

/**
 * @brief Refresh the status-bar clock immediately.
 */
void vt100_terminal_refresh_status_time(void);

/**
 * @brief Update the IP/host text shown in the VT100 status bar.
 */
void vt100_terminal_set_ip(const char *ip_addr, const char *hostname);

#endif /* VT100_TERMINAL_H */
