/**
 * @file altair_panel.h
 * @brief Altair 8800 Front Panel Display for ESP32-S3
 * 
 * Displays CPU state (address bus, data bus, status LEDs) on ILI9341 LCD.
 * Runs as a FreeRTOS task on Core 1 to avoid blocking the emulator.
 */

#ifndef ALTAIR_PANEL_H
#define ALTAIR_PANEL_H

#include <stdint.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// Global CPU state (updated by emulator on Core 0, read by panel on Core 1)
//-----------------------------------------------------------------------------
extern volatile uint16_t g_panel_address;   // Address bus (A15-A0)
extern volatile uint8_t  g_panel_data;      // Data bus (D7-D0)
extern volatile uint16_t g_panel_status;    // Status LEDs

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
// Panel Task API
//-----------------------------------------------------------------------------

/**
 * @brief Start the front panel display task on Core 1
 * 
 * Initializes the LCD and starts a FreeRTOS task that updates the display
 * at approximately 60Hz, showing the current CPU state.
 */
void altair_panel_start(void);

/**
 * @brief Update panel state from emulator
 * 
 * Call this from the emulator loop to update the displayed CPU state.
 * Thread-safe (uses volatile globals).
 * 
 * @param address Current address bus value
 * @param data Current data bus value  
 * @param status Current status flags
 */
static inline void altair_panel_update(uint16_t address, uint8_t data, uint16_t status)
{
    g_panel_address = address;
    g_panel_data = data;
    g_panel_status = status;
}

#endif // ALTAIR_PANEL_H
