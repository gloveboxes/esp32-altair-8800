#ifndef PANEL_DISPLAY_H
#define PANEL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t panel_color_t;

#define PANEL_COLOR_BLACK   0x0000
#define PANEL_COLOR_WHITE   0xFFFF
#define PANEL_COLOR_RED     0xF800
#define PANEL_COLOR_GREEN   0x07E0
#define PANEL_COLOR_BLUE    0x001F
#define PANEL_COLOR_YELLOW  0xFFE0
#define PANEL_COLOR_CYAN    0x07FF
#define PANEL_COLOR_MAGENTA 0xF81F
#define PANEL_COLOR_ORANGE  0xFD20

bool panel_display_init(void);
int panel_display_width(void);
int panel_display_height(void);
bool panel_display_is_monochrome(void);
bool panel_display_is_banded(void);
bool panel_display_bands_are_vertical(void);
int panel_display_band_height(void);
void panel_display_begin_band(int y, int h);
void panel_display_fill_screen(panel_color_t color);
void panel_display_fill_rect(int x, int y, int w, int h, panel_color_t color);
void panel_display_draw_pixel(int x, int y, panel_color_t color);
void panel_display_draw_string(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color, int scale);
void panel_display_draw_string_small(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color);
void panel_display_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                int led_size, int spacing, panel_color_t on_color,
                                panel_color_t off_color, panel_color_t bg_color);
void panel_display_draw_led_span(uint32_t bits, int num_leds, int x_start, int y,
                                 int led_size, int spacing, panel_color_t on_color,
                                 panel_color_t off_color, panel_color_t bg_color,
                                 int left_index, int right_index);
bool panel_display_status_region_supported(void);
void panel_display_status_region_clear(panel_color_t color);
void panel_display_status_region_fill_rect(int x, int y, int w, int h, panel_color_t color);
void panel_display_status_region_draw_pixel(int x, int y, panel_color_t color);
void panel_display_status_region_present(void);
bool panel_display_present_band_supported(void);
void panel_display_present_band(int y, int h);
void panel_display_present(void);
void panel_display_set_backlight(int brightness);

#endif