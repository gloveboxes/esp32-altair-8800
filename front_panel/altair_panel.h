/**
 * @file altair_panel.h
 * @brief Altair 8800 Front Panel Display for ESP32-S3
 * 
 * Displays CPU state (address bus, data bus, status LEDs) on ILI9341 LCD.
 * Display update runs on Core 0, called from main loop.
 * Emulator runs on Core 1.
 */

#ifndef ALTAIR_PANEL_H
#define ALTAIR_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include "intel8080.h"

// Status bit definitions (directly usable for LED display)
#define STATUS_INTE  (1 << 0)   // Interrupts enabled
#define STATUS_PROT  (1 << 1)   // Memory protect (unused)
#define STATUS_MEMR  (1 << 2)   // Memory read
#define STATUS_INP   (1 << 3)   // Input
#define STATUS_M1    (1 << 4)   // Machine cycle 1 (instruction fetch)
#define STATUS_OUT   (1 << 5)   // Output
#define STATUS_HLTA  (1 << 6)   // Halt acknowledge
#define STATUS_STCK  (1 << 7)   // Stack access
#define STATUS_WO    (1 << 8)   // Write out (active low on real Altair)
#define STATUS_INT   (1 << 9)   // Interrupt request

//-----------------------------------------------------------------------------
// Panel API
//-----------------------------------------------------------------------------

/**
 * @brief Initialize the front panel display
 * 
 * Initializes the LCD hardware and draws static elements.
 * Call once at startup before altair_panel_update().
 * 
 * @return true on success, false on failure
 */
bool altair_panel_init(void);

/**
 * @brief Update the front panel display
 * 
 * Reads CPU state and updates the display.
 * Call this periodically from Core 0 main loop (~60Hz recommended).
 * Only redraws LEDs that have changed state for efficiency.
 * 
 * @param cpu Pointer to the Intel 8080 CPU struct
 */
void altair_panel_update(const intel8080_t *cpu);

/**
 * @brief Display IP address and hostname on the panel
 * 
 * Shows the IP address and mDNS hostname on the bottom left of the display.
 * Call when IP address is obtained or changes.
 * 
 * @param ip_addr IP address string (e.g., "192.168.1.100")
 * @param hostname mDNS hostname (without .local suffix), or NULL to omit
 */
void altair_panel_show_ip(const char *ip_addr, const char *hostname);

#endif // ALTAIR_PANEL_H
