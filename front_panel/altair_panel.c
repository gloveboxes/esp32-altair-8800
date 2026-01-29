/**
 * @file altair_panel.c
 * @brief Altair 8800 Front Panel Display for ESP32-S3
 * 
 * Displays CPU state on ILI9341 LCD using direct-write (no framebuffer).
 * Display updates run on Core 0 at ~60Hz refresh rate.
 * Only redraws LEDs that have changed state for efficiency.
 */

#include "altair_panel.h"
#include "ili9341.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "AltairPanel";

//-----------------------------------------------------------------------------
// Local state for change detection
//-----------------------------------------------------------------------------
static uint16_t last_status = 0;
static uint16_t last_address = 0;
static uint8_t last_data = 0;
static bool panel_initialized = false;

//-----------------------------------------------------------------------------
// Layout constants (matching Pico reference implementation)
//-----------------------------------------------------------------------------
#define LED_SIZE            15
#define LED_SPACING_STATUS  32
#define LED_SPACING_ADDRESS 20
#define LED_SPACING_DATA    20

// Y positions for each section
#define Y_STATUS    35
#define Y_ADDRESS   100
#define Y_DATA      170

// X positions
#define X_STATUS_START    8
#define X_ADDRESS_START   2
#define X_DATA_START      162

// Colors
#define LED_ON_COLOR      COLOR_RED
#define LED_OFF_COLOR     0x2000      // Dark red (RGB565: R=4, G=0, B=0)
#define TEXT_WHITE        COLOR_WHITE
#define TEXT_GRAY         0xC618      // Light gray

//-----------------------------------------------------------------------------
// Panel drawing functions
//-----------------------------------------------------------------------------

/**
 * @brief Draw static panel elements (labels, lines) - called once at init
 */
static void draw_static_elements(void)
{
    // Clear screen
    ili9341_fill_screen(COLOR_BLACK);
    
    // Title
    ili9341_draw_string(2, 5, "ALTAIR 8800", COLOR_CYAN, COLOR_BLACK, 2);
    ili9341_draw_string(180, 5, "ESP32-S3", COLOR_WHITE, COLOR_BLACK, 1);
    
    // STATUS section
    ili9341_draw_string(270, Y_STATUS - 15, "STATUS", TEXT_WHITE, COLOR_BLACK, 1);
    ili9341_fill_rect(0, Y_STATUS - 5, LCD_H_RES, 2, TEXT_WHITE);
    
    // Status labels - using smaller font to fit
    const char *status_labels[] = {"INT", "WO", "STCK", "HLTA", "OUT", "M1", "INP", "MEMR", "PROT", "INTE"};
    int x = X_STATUS_START;
    for (int i = 9; i >= 0; i--) {
        ili9341_draw_string_small(x, Y_STATUS + LED_SIZE + 2, status_labels[i], TEXT_GRAY, COLOR_BLACK);
        x += LED_SPACING_STATUS;
    }
    
    // ADDRESS section
    ili9341_draw_string(264, Y_ADDRESS - 15, "ADDRESS", TEXT_WHITE, COLOR_BLACK, 1);
    ili9341_fill_rect(0, Y_ADDRESS - 5, LCD_H_RES, 2, TEXT_WHITE);
    
    // Address labels (15-0) - using smaller font
    x = X_ADDRESS_START;
    for (int i = 15; i >= 0; i--) {
        char label[4];
        if (i >= 10) {
            snprintf(label, sizeof(label), "%d", i);
        } else {
            snprintf(label, sizeof(label), " %d", i);
        }
        ili9341_draw_string_small(x + 2, Y_ADDRESS + LED_SIZE + 2, label, TEXT_GRAY, COLOR_BLACK);
        x += LED_SPACING_ADDRESS;
    }
    
    // DATA section
    ili9341_draw_string(282, Y_DATA - 15, "DATA", TEXT_WHITE, COLOR_BLACK, 1);
    ili9341_fill_rect(0, Y_DATA - 5, LCD_H_RES, 2, TEXT_WHITE);
    
    // Data labels (7-0) - using smaller font
    x = X_DATA_START;
    for (int i = 7; i >= 0; i--) {
        char label[3];
        snprintf(label, sizeof(label), "%d", i);
        ili9341_draw_string_small(x + 8, Y_DATA + LED_SIZE + 2, label, TEXT_GRAY, COLOR_BLACK);
        x += LED_SPACING_DATA;
    }
    
    ESP_LOGI(TAG, "Static elements drawn");
}

/**
 * @brief Draw all LEDs for initial state using efficient row drawing
 */
static void draw_all_leds(uint16_t status, uint16_t address, uint8_t data)
{
    // Draw entire rows at once using async DMA with double buffering
    // Each call starts DMA while preparing next row in alternate buffer
    ili9341_draw_led_row(status, 10, X_STATUS_START, Y_STATUS, 
                         LED_SIZE, LED_SPACING_STATUS, LED_ON_COLOR, LED_OFF_COLOR);
    ili9341_draw_led_row(address, 16, X_ADDRESS_START, Y_ADDRESS,
                         LED_SIZE, LED_SPACING_ADDRESS, LED_ON_COLOR, LED_OFF_COLOR);
    ili9341_draw_led_row(data, 8, X_DATA_START, Y_DATA,
                         LED_SIZE, LED_SPACING_DATA, LED_ON_COLOR, LED_OFF_COLOR);
    // Wait for final DMA transfer to complete
    ili9341_wait_async();
}

/**
 * @brief Update only changed LED rows using async DMA with double buffering
 * Redraws entire row if any LED in that row changed
 */
static void update_changed_leds(uint16_t new_status, uint16_t old_status,
                                 uint16_t new_address, uint16_t old_address,
                                 uint8_t new_data, uint8_t old_data)
{
    // Detect which individual LEDs changed
    uint16_t status_changed = new_status ^ old_status;
    uint16_t address_changed = new_address ^ old_address;
    uint8_t data_changed = new_data ^ old_data;
    
    // Count total changed LEDs
    int num_changed = __builtin_popcount(status_changed) + 
                      __builtin_popcount(address_changed) + 
                      __builtin_popcount(data_changed);
    
    if (num_changed == 0) return;
    
    // If many LEDs changed, use row-based drawing (more efficient for bulk updates)
    // Threshold: if more than ~6 LEDs changed, row drawing is faster
    if (num_changed > 6) {
        // Row-based update
        if (status_changed) {
            ili9341_draw_led_row(new_status, 10, X_STATUS_START, Y_STATUS,
                                 LED_SIZE, LED_SPACING_STATUS, LED_ON_COLOR, LED_OFF_COLOR);
        }
        if (address_changed) {
            ili9341_draw_led_row(new_address, 16, X_ADDRESS_START, Y_ADDRESS,
                                 LED_SIZE, LED_SPACING_ADDRESS, LED_ON_COLOR, LED_OFF_COLOR);
        }
        if (data_changed) {
            ili9341_draw_led_row(new_data, 8, X_DATA_START, Y_DATA,
                                 LED_SIZE, LED_SPACING_DATA, LED_ON_COLOR, LED_OFF_COLOR);
        }
        ili9341_wait_async();
        return;
    }
    
    // Per-LED update - only redraw LEDs that actually changed
    // Status LEDs (10 LEDs, MSB on left)
    if (status_changed) {
        for (int i = 9; i >= 0; i--) {
            if (status_changed & (1 << i)) {
                int x = X_STATUS_START + (9 - i) * LED_SPACING_STATUS;
                uint16_t color = (new_status & (1 << i)) ? LED_ON_COLOR : LED_OFF_COLOR;
                ili9341_fill_rect(x, Y_STATUS, LED_SIZE, LED_SIZE, color);
            }
        }
    }
    
    // Address LEDs (16 LEDs, MSB on left)
    if (address_changed) {
        for (int i = 15; i >= 0; i--) {
            if (address_changed & (1 << i)) {
                int x = X_ADDRESS_START + (15 - i) * LED_SPACING_ADDRESS;
                uint16_t color = (new_address & (1 << i)) ? LED_ON_COLOR : LED_OFF_COLOR;
                ili9341_fill_rect(x, Y_ADDRESS, LED_SIZE, LED_SIZE, color);
            }
        }
    }
    
    // Data LEDs (8 LEDs, MSB on left)
    if (data_changed) {
        for (int i = 7; i >= 0; i--) {
            if (data_changed & (1 << i)) {
                int x = X_DATA_START + (7 - i) * LED_SPACING_DATA;
                uint16_t color = (new_data & (1 << i)) ? LED_ON_COLOR : LED_OFF_COLOR;
                ili9341_fill_rect(x, Y_DATA, LED_SIZE, LED_SIZE, color);
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Panel API Functions
//-----------------------------------------------------------------------------

bool altair_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing panel on Core %d", xPortGetCoreID());
    
    // Initialize display
    if (ili9341_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display!");
        return false;
    }
    
    // Draw static elements
    draw_static_elements();
    
    // Initialize tracking state to 0
    last_status = 0;
    last_address = 0;
    last_data = 0;
    
    // Draw initial LED state (all off)
    draw_all_leds(0, 0, 0);
    
    panel_initialized = true;
    ESP_LOGI(TAG, "Panel initialized successfully");
    
    return true;
}

void altair_panel_update(const intel8080_t *cpu)
{
    if (!panel_initialized || cpu == NULL) {
        return;
    }
    
    // Read current CPU state directly from CPU struct
    uint16_t cur_status = cpu->cpuStatus;
    uint16_t cur_address = cpu->address_bus;
    uint8_t cur_data = cpu->data_bus;
    
    // Only update if something changed
    if (cur_status != last_status || cur_address != last_address || cur_data != last_data) {
        update_changed_leds(cur_status, last_status, cur_address, last_address, cur_data, last_data);
        
        last_status = cur_status;
        last_address = cur_address;
        last_data = cur_data;
    }
}

//-----------------------------------------------------------------------------
// Legacy Task-based API (for backward compatibility)
//-----------------------------------------------------------------------------

#define PANEL_TASK_STACK_SIZE   4096
#define PANEL_TASK_PRIORITY     6    // Above WS TX task (5) for responsive display
#define PANEL_UPDATE_INTERVAL_MS 17  // ~60Hz

// Y position for IP address (bottom of display)
#define Y_IP_ADDRESS    225

void altair_panel_show_ip(const char *ip_addr, const char *hostname)
{
    if (!panel_initialized || ip_addr == NULL) {
        return;
    }
    
    // Clear the entire bottom line
    ili9341_fill_rect(0, Y_IP_ADDRESS, LCD_H_RES, 15, COLOR_BLACK);
    
    // Build display string: "WIFI: IP | hostname.local"
    char display_str[72];
    if (hostname) {
        snprintf(display_str, sizeof(display_str), "WIFI: %s | %s.local", ip_addr, hostname);
    } else {
        snprintf(display_str, sizeof(display_str), "WIFI: %s", ip_addr);
    }
    
    // Draw the string using small font (same as address labels), offset 4 pixels right
    ili9341_draw_string_small(4, Y_IP_ADDRESS, display_str, TEXT_GRAY, COLOR_BLACK);
}

void altair_panel_show_captive_portal(const char *ap_ssid, const char *portal_ip)
{
    if (!panel_initialized) {
        return;
    }
    
    // Clear entire screen
    ili9341_fill_screen(COLOR_BLACK);
    
    // Draw border lines
    ili9341_fill_rect(10, 50, 300, 2, COLOR_CYAN);
    ili9341_fill_rect(10, 180, 300, 2, COLOR_CYAN);
    
    // Title - using small font, centered (6 pixels per char)
    const char *title = "WIFI SETUP MODE";
    int title_x = (LCD_H_RES - (strlen(title) * 6)) / 2;
    ili9341_draw_string_small(title_x, 80, title, COLOR_CYAN, COLOR_BLACK);
    
    // Instructions using small font
    char line1[48];
    char line2[48];
    snprintf(line1, sizeof(line1), "CONNECT TO: %s", ap_ssid ? ap_ssid : "Altair8800-Setup");
    snprintf(line2, sizeof(line2), "THEN OPEN: HTTP://%s/", portal_ip ? portal_ip : "192.168.4.1");
    
    int line1_x = (LCD_H_RES - (strlen(line1) * 6)) / 2;
    int line2_x = (LCD_H_RES - (strlen(line2) * 6)) / 2;
    
    ili9341_draw_string_small(line1_x, 110, line1, COLOR_WHITE, COLOR_BLACK);
    ili9341_draw_string_small(line2_x, 140, line2, COLOR_WHITE, COLOR_BLACK);
}
