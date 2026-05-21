/**
 * @file altair_panel.c
 * @brief Altair 8800 Front Panel Display for ESP32-S3
 * 
 * Displays CPU state on the selected panel backend.
 * Display updates run on Core 0 at ~30Hz refresh rate.
 * Only redraws LEDs that have changed state for efficiency.
 */

#include "altair_panel.h"
#include "panel_display.h"
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
static char s_ip_addr[16] = {0};
static char s_hostname[32] = {0};

typedef struct {
    int led_size;
    int led_spacing_status;
    int led_spacing_address;
    int led_spacing_data;
    int led_label_offset_y;
    int x_text_left;
    int x_text_right_margin;
    int y_title;
    int y_status;
    int y_address;
    int y_data;
    int x_status_start;
    int x_address_start;
    int x_data_start;
    int y_ip_address;
} panel_layout_t;

typedef struct {
    panel_color_t background;
    panel_color_t led_on;
    panel_color_t led_off;
    panel_color_t text_primary;
    panel_color_t text_secondary;
    panel_color_t title_accent;
} panel_theme_t;

static panel_layout_t s_layout;
static panel_theme_t s_theme;

static void configure_layout_and_theme(void)
{
    const int led_rows_shift_x = 0;

    s_layout.led_size = 15;
    s_layout.led_spacing_status = 32;
    s_layout.led_spacing_address = 20;
    s_layout.led_spacing_data = 20;
    s_layout.led_label_offset_y = 17;
    s_layout.x_text_left = 2;
    s_layout.x_text_right_margin = 2;
    s_layout.y_title = 5;
    s_layout.y_status = 35;
    s_layout.y_address = 100;
    s_layout.y_data = 170;
    s_layout.x_address_start = 2;
    s_layout.y_ip_address = 220;

    s_theme.background = 0x18C8;
    s_theme.led_on = PANEL_COLOR_RED;
    s_theme.led_off = 0x3000;
    s_theme.text_primary = 0xEF5D;
    s_theme.text_secondary = 0xBDF7;
    s_theme.title_accent = 0xD69A;

    int address_right_led_x = s_layout.x_address_start + (15 * s_layout.led_spacing_address);
    s_layout.x_status_start = address_right_led_x - (9 * s_layout.led_spacing_status);
    s_layout.x_data_start = address_right_led_x - (7 * s_layout.led_spacing_data);

    s_layout.x_status_start += led_rows_shift_x;
    s_layout.x_address_start += led_rows_shift_x;
    s_layout.x_data_start += led_rows_shift_x;

    s_layout.x_status_start -= 4;
}

//-----------------------------------------------------------------------------
// Panel drawing functions
//-----------------------------------------------------------------------------

static int right_align_title_x(const char *title)
{
    int width = (int)strlen(title) * 8;
    int x = (panel_display_width() - s_layout.x_text_right_margin) - width;
    return (x < 0) ? 0 : x;
}

static void draw_centered_small_text(int y, const char *text, panel_color_t fg, panel_color_t bg)
{
    int width = (int)strlen(text) * 6;
    int x = (panel_display_width() - width) / 2;
    if (x < 0) {
        x = 0;
    }
    panel_display_draw_string_small(x, y, text, fg, bg);
}

static void draw_startup_test_frame(uint32_t counter, uint32_t elapsed_ms, uint32_t frames)
{
    panel_color_t bg = s_theme.text_primary;
    panel_color_t fg = s_theme.background;
    char line[64];
    int w = panel_display_width();
    int h = panel_display_height();

    // Full-screen clear every frame so the entire framebuffer is pushed via DMA
    panel_display_fill_screen(bg);

    // Moving vertical bar (20 px wide, wraps across the screen)
    int bar_x = (int)((counter * 4) % (uint32_t)w);
    int bar_w = 20;
    if (bar_x + bar_w > w) {
        panel_display_fill_rect(bar_x, 0, w - bar_x, h, fg);
        panel_display_fill_rect(0, 0, bar_w - (w - bar_x), h, fg);
    } else {
        panel_display_fill_rect(bar_x, 0, bar_w, h, fg);
    }

    // Title row
    draw_centered_small_text(2, "DISPLAY DMA TEST", fg, bg);

    // 20 rows of counter text filling the screen
    int row_height = (h - 20) / 20;  // leave space for title and bottom stats
    for (int row = 0; row < 20; row++) {
        int y = 12 + row * row_height;
        snprintf(line, sizeof(line), "%02d: FRAME %lu", row, (unsigned long)counter);
        panel_display_draw_string_small(s_layout.x_text_left, y, line, fg, bg);
    }

    // FPS and elapsed at the bottom
    if (elapsed_ms > 0) {
        uint32_t fps_times_10 = (frames * 10000U) / elapsed_ms;
        snprintf(line, sizeof(line), "PUSH %lu.%lu FPS  ELAPSED %lu MS",
                 (unsigned long)(fps_times_10 / 10U),
                 (unsigned long)(fps_times_10 % 10U),
                 (unsigned long)elapsed_ms);
    } else {
        snprintf(line, sizeof(line), "PUSH 0.0 FPS  ELAPSED 0 MS");
    }
    draw_centered_small_text(h - 10, line, fg, bg);

    panel_display_present();
}

static bool update_led_row_span(uint32_t new_bits, uint32_t old_bits, int num_leds,
                                int x_start, int y, int spacing)
{
    uint32_t mask = (num_leds >= 32) ? 0xFFFFFFFFu : ((1u << num_leds) - 1u);
    new_bits &= mask;
    uint32_t changed = (new_bits ^ old_bits) & mask;
    if (!changed) return false;

    int left = 31 - __builtin_clz(changed);
    int right = __builtin_ctz(changed);

    if (left >= num_leds) left = num_leds - 1;
    if (right < 0) right = 0;

    panel_display_draw_led_span(new_bits, num_leds, x_start, y,
                                s_layout.led_size, spacing, s_theme.led_on,
                                s_theme.led_off, s_theme.background, left, right);
    return true;
}

static void draw_status_labels(void)
{
    const char *status_labels[] = {"INT ", "WO ", "STCK", "HLTA", "OUT ", "M1 ", "INP ", "MEMR", "PROT", "INTE"};
    int status_led_center_offset = s_layout.led_size / 2;
    int x = s_layout.x_status_start;

    for (int i = 9; i >= 0; i--) {
        int label_width = (int)strlen(status_labels[i]) * 6;
        int label_x = x + status_led_center_offset - (label_width / 2);
        label_x += 4;
        panel_display_draw_string_small(label_x, s_layout.y_status + s_layout.led_label_offset_y,
                                        status_labels[i], s_theme.text_secondary,
                                        s_theme.background);
        x += s_layout.led_spacing_status;
    }
}

static void draw_address_labels(void)
{
    int x = s_layout.x_address_start;

    for (int i = 15; i >= 0; i--) {
        char label[4];
        if (i >= 10) {
            snprintf(label, sizeof(label), "%d", i);
        } else {
            snprintf(label, sizeof(label), " %d", i);
        }
        panel_display_draw_string_small(x + 2, s_layout.y_address + s_layout.led_label_offset_y,
                                        label, s_theme.text_secondary, s_theme.background);
        x += s_layout.led_spacing_address;
    }
}

static void draw_data_labels(void)
{
    int data_label_offset_x = (s_layout.led_size - 6) / 2;
    int x = s_layout.x_data_start;

    for (int i = 7; i >= 0; i--) {
        char label[3];
        snprintf(label, sizeof(label), "%d", i);
        panel_display_draw_string_small(x + data_label_offset_x + 5,
                                        s_layout.y_data + s_layout.led_label_offset_y,
                                        label, s_theme.text_secondary, s_theme.background);
        x += s_layout.led_spacing_data;
    }
}

/**
 * @brief Draw static panel elements (labels, lines) - called once at init
 */
static void draw_static_elements(void)
{
    // Clear screen
    panel_display_fill_screen(s_theme.background);
    
    // Title
    panel_display_draw_string(s_layout.x_text_left, s_layout.y_title,
                              "ALTAIR 8800", s_theme.text_primary, s_theme.background, 2);
    panel_display_draw_string(180, s_layout.y_title, "ESP32-S3",
                              s_theme.title_accent, s_theme.background, 1);
    
    // STATUS section
    panel_display_draw_string(right_align_title_x("STATUS"), s_layout.y_status - 15,
                              "STATUS", s_theme.text_primary, s_theme.background, 1);
    panel_display_fill_rect(0, s_layout.y_status - 5, panel_display_width(), 2, s_theme.text_primary);
    draw_status_labels();
    
    // ADDRESS section
    panel_display_draw_string(right_align_title_x("ADDRESS"), s_layout.y_address - 15,
                              "ADDRESS", s_theme.text_primary, s_theme.background, 1);
    panel_display_fill_rect(0, s_layout.y_address - 5, panel_display_width(), 2, s_theme.text_primary);
    draw_address_labels();
    
    // DATA section
    panel_display_draw_string(right_align_title_x("DATA"), s_layout.y_data - 15,
                              "DATA", s_theme.text_primary, s_theme.background, 1);
    panel_display_fill_rect(0, s_layout.y_data - 5, panel_display_width(), 2, s_theme.text_primary);
    draw_data_labels();

}

/**
 * @brief Draw all LEDs for initial state using efficient row drawing
 */
static void draw_all_leds(uint16_t status, uint16_t address, uint8_t data)
{
    // Draw entire rows at once using async DMA with double buffering
    // Each call starts DMA while preparing next row in alternate buffer
    panel_display_draw_led_row(status, 10, s_layout.x_status_start, s_layout.y_status,
                               s_layout.led_size, s_layout.led_spacing_status,
                               s_theme.led_on, s_theme.led_off, s_theme.background);
    panel_display_draw_led_row(address, 16, s_layout.x_address_start, s_layout.y_address,
                               s_layout.led_size, s_layout.led_spacing_address,
                               s_theme.led_on, s_theme.led_off, s_theme.background);
    panel_display_draw_led_row(data, 8, s_layout.x_data_start, s_layout.y_data,
                               s_layout.led_size, s_layout.led_spacing_data,
                               s_theme.led_on, s_theme.led_off, s_theme.background);
}

static void draw_ip_line(void)
{
    if (s_ip_addr[0] == '\0') {
        return;
    }

    panel_display_fill_rect(0, s_layout.y_ip_address, panel_display_width(), 15, s_theme.background);

    char display_str[72];
    if (s_hostname[0] != '\0') {
        snprintf(display_str, sizeof(display_str), "WIFI: %s | %s", s_ip_addr, s_hostname);
    } else {
        snprintf(display_str, sizeof(display_str), "WIFI: %s", s_ip_addr);
    }

    panel_display_draw_string_small(s_layout.x_text_left, s_layout.y_ip_address, display_str,
                                    s_theme.text_primary, s_theme.background);
}

static void draw_full_panel(uint16_t status, uint16_t address, uint8_t data)
{
    draw_static_elements();
    draw_all_leds(status, address, data);
    draw_ip_line();
}

static void present_full_panel(uint16_t status, uint16_t address, uint8_t data)
{
    if (!panel_display_is_banded()) {
        draw_full_panel(status, address, data);
        panel_display_present();
        return;
    }

    int band_h = panel_display_band_height();
    int band_limit = panel_display_bands_are_vertical() ? panel_display_width() : panel_display_height();
    for (int y = 0; y < band_limit; y += band_h) {
        int h = band_limit - y;
        if (h > band_h) h = band_h;
        panel_display_begin_band(y, h);
        draw_full_panel(status, address, data);
        panel_display_present();
    }
}

/**
 * @brief Update only changed LED rows using async DMA with double buffering
 * Redraws the minimal contiguous LED span per row that covers all changes
 */
static void update_changed_leds(uint16_t new_status, uint16_t old_status,
                                 uint16_t new_address, uint16_t old_address,
                                 uint8_t new_data, uint8_t old_data)
{
    bool any_updated = false;

    any_updated |= update_led_row_span(new_status, old_status, 10,
                                       s_layout.x_status_start, s_layout.y_status,
                                       s_layout.led_spacing_status);
    any_updated |= update_led_row_span(new_address, old_address, 16,
                                       s_layout.x_address_start, s_layout.y_address,
                                       s_layout.led_spacing_address);
    any_updated |= update_led_row_span(new_data, old_data, 8,
                                       s_layout.x_data_start, s_layout.y_data,
                                       s_layout.led_spacing_data);

    if (any_updated) {
        panel_display_present();
    }
}

//-----------------------------------------------------------------------------
// Panel API Functions
//-----------------------------------------------------------------------------

bool altair_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing panel on Core %d", xPortGetCoreID());
    
    configure_layout_and_theme();

    if (!panel_display_init()) {
        ESP_LOGE(TAG, "Failed to initialize display!");
        return false;
    }
    
    // Initialize tracking state to 0
    last_status = 0;
    last_address = 0;
    last_data = 0;

    present_full_panel(0, 0, 0);
    
    panel_initialized = true;
    ESP_LOGI(TAG, "Panel initialized successfully");
    
    return true;
}

void altair_panel_run_startup_test(uint32_t duration_ms)
{
    if (!panel_initialized || duration_ms == 0) {
        return;
    }

    int64_t start_us = esp_timer_get_time();
    uint32_t frames = 0;
    TickType_t last_wake = xTaskGetTickCount();

    panel_display_set_backlight(100);

    while (true) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        if (elapsed_ms >= duration_ms) {
            break;
        }

        frames++;
        draw_startup_test_frame(frames, elapsed_ms, frames);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PANEL_UPDATE_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Startup panel test complete: %lu frames in %lu ms", (unsigned long)frames,
             (unsigned long)duration_ms);

    present_full_panel(0, 0, 0);
}

// Raw CPU status bits as set by the emulator core (intel8080.c).
// These don't match the panel's logical layout, so they need translating.
#define RAW_STATUS_MEMORY_READ   0x80
#define RAW_STATUS_PORT_INPUT    0x40
#define RAW_STATUS_OP_CODE_FETCH 0x20
#define RAW_STATUS_PORT_OUTPUT   0x10
#define RAW_STATUS_HALT          0x08
#define RAW_STATUS_STACK         0x04
#define RAW_STATUS_WRITE_OUTPUT  0x02
#define RAW_STATUS_INTERRUPT     0x01

static uint16_t translate_cpu_status(const intel8080_t *cpu)
{
    uint8_t raw = cpu->cpuStatus;
    uint16_t out = 0;

    if (cpu->iff)                          out |= STATUS_INTE;
    if (raw & RAW_STATUS_MEMORY_READ)      out |= STATUS_MEMR;
    if (raw & RAW_STATUS_PORT_INPUT)       out |= STATUS_INP;
    if (raw & RAW_STATUS_OP_CODE_FETCH)    out |= STATUS_M1;
    if (raw & RAW_STATUS_PORT_OUTPUT)      out |= STATUS_OUT;
    if (raw & RAW_STATUS_HALT)             out |= STATUS_HLTA;
    if (raw & RAW_STATUS_STACK)            out |= STATUS_STCK;
    if (raw & RAW_STATUS_WRITE_OUTPUT)     out |= STATUS_WO;
    if (raw & RAW_STATUS_INTERRUPT)        out |= STATUS_INT;
    return out;
}

void altair_panel_update(const intel8080_t *cpu)
{
    if (!panel_initialized || cpu == NULL) {
        return;
    }

    // Translate raw emulator status bits into the panel's logical layout.
    uint16_t cur_status = translate_cpu_status(cpu);
    uint16_t cur_address = cpu->address_bus;
    uint8_t cur_data = cpu->data_bus;
    
    // Only update if something changed
    if (cur_status != last_status || cur_address != last_address || cur_data != last_data) {
        if (panel_display_is_banded()) {
            present_full_panel(cur_status, cur_address, cur_data);
        } else {
            update_changed_leds(cur_status, last_status, cur_address, last_address, cur_data, last_data);
        }

        last_status = cur_status;
        last_address = cur_address;
        last_data = cur_data;
    }
}

//-----------------------------------------------------------------------------
// Legacy Task-based API (for backward compatibility)
//-----------------------------------------------------------------------------

#define PANEL_TASK_STACK_SIZE   4096
#define PANEL_TASK_PRIORITY     4    // Legacy value (panel update task is created in main)
void altair_panel_show_ip(const char *ip_addr, const char *hostname)
{
    if (!panel_initialized || ip_addr == NULL) {
        return;
    }

    // Bring panel to normal brightness once WiFi is connected and IP is known
    panel_display_set_backlight(80);

    snprintf(s_ip_addr, sizeof(s_ip_addr), "%s", ip_addr);
    if (hostname) {
        snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
    } else {
        s_hostname[0] = '\0';
    }

    if (panel_display_is_banded()) {
        present_full_panel(last_status, last_address, last_data);
        return;
    }

    draw_ip_line();
    panel_display_present();
}

void altair_panel_show_captive_portal(const char *ap_ssid, const char *portal_ip)
{
    if (!panel_initialized) {
        return;
    }

    int passes = panel_display_is_banded()
                     ? (panel_display_bands_are_vertical() ? panel_display_width() : panel_display_height())
                     : 1;
    int band_h = panel_display_is_banded() ? panel_display_band_height() : panel_display_height();

    for (int band_y = 0; band_y < passes; band_y += band_h) {
        if (panel_display_is_banded()) {
            int h = panel_display_height() - band_y;
            if (h > band_h) h = band_h;
            panel_display_begin_band(band_y, h);
        }

    // Clear entire screen
    panel_display_fill_screen(s_theme.background);

    // Draw border lines
    panel_display_fill_rect(10, 50, panel_display_width() - 20, 2, s_theme.text_primary);
    panel_display_fill_rect(10, 180, panel_display_width() - 20, 2, s_theme.text_primary);

    // Title - using small font, centered (6 pixels per char)
    const char *title = "WIFI SETUP MODE";
    int title_x = (panel_display_width() - (strlen(title) * 6)) / 2;
    panel_display_draw_string_small(title_x, 80, title, s_theme.text_primary, s_theme.background);

    // Instructions using small font
    char line1[48];
    char line2[48];
    snprintf(line1, sizeof(line1), "CONNECT TO: %s", ap_ssid ? ap_ssid : "Altair8800-Setup");
    snprintf(line2, sizeof(line2), "THEN OPEN: HTTP://%s/", portal_ip ? portal_ip : "192.168.4.1");

    int line1_x = (panel_display_width() - (strlen(line1) * 6)) / 2;
    int line2_x = (panel_display_width() - (strlen(line2) * 6)) / 2;

    panel_display_draw_string_small(line1_x, 110, line1, s_theme.title_accent, s_theme.background);
    panel_display_draw_string_small(line2_x, 140, line2, s_theme.title_accent, s_theme.background);
    panel_display_present();
    }
}

void altair_panel_set_backlight(int brightness)
{
    if (!panel_initialized) {
        return;
    }

    panel_display_set_backlight(brightness);
}
