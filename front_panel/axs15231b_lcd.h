#ifndef AXS15231B_LCD_H
#define AXS15231B_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "panel_display.h"

/* Logical landscape orientation. axs15231b_lcd.c configures the AXS15231B with
 * MADCTL MV and stores pixels in the controller's 320-column QSPI stream order. */
#define AXS15231B_LCD_H_RES 480
#define AXS15231B_LCD_V_RES 320

esp_err_t axs15231b_lcd_init(void);
void axs15231b_lcd_fill_screen(panel_color_t color);
void axs15231b_lcd_fill_rect(int x, int y, int w, int h, panel_color_t color);
void axs15231b_lcd_draw_pixel(int x, int y, panel_color_t color);
void axs15231b_lcd_draw_string(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color, int scale);
void axs15231b_lcd_draw_string_small(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color);
void axs15231b_lcd_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                int led_size, int spacing, panel_color_t on_color,
                                panel_color_t off_color, panel_color_t bg_color);
void axs15231b_lcd_draw_led_span(uint32_t bits, int num_leds, int x_start, int y,
                                 int led_size, int spacing, panel_color_t on_color,
                                 panel_color_t off_color, panel_color_t bg_color,
                                 int left_index, int right_index);
bool axs15231b_lcd_status_region_supported(void);
void axs15231b_lcd_status_region_clear(panel_color_t color);
void axs15231b_lcd_status_region_fill_rect(int x, int y, int w, int h, panel_color_t color);
void axs15231b_lcd_status_region_draw_pixel(int x, int y, panel_color_t color);
void axs15231b_lcd_status_region_present(void);
bool axs15231b_lcd_present_band_supported(void);
void axs15231b_lcd_present_band(int y, int h);
int axs15231b_lcd_band_height(void);
void axs15231b_lcd_begin_band(int y, int h);
void axs15231b_lcd_present(void);
void axs15231b_lcd_set_backlight(int brightness);

/* Fast text-cell blitter: writes a `cell_w x cell_h` rectangle of `bg`, then
 * overlays a `glyph_w x glyph_h` 1-bpp glyph at offset (xoff, yoff) inside the
 * cell using `fg`. `glyph_cols` is glyph_w bytes; bit `fr` of byte `fc` is the
 * pixel at glyph coordinate (fc, fr). Pass NULL for `glyph_cols` to draw only
 * the background fill. Bypasses per-pixel function-call overhead by writing
 * directly to the framebuffer in stream order. */
void axs15231b_lcd_blit_glyph_cell(int x, int y,
                                   panel_color_t fg, panel_color_t bg,
                                   const uint8_t *glyph_cols,
                                   int cell_w, int cell_h,
                                   int glyph_w, int glyph_h,
                                   int xoff, int yoff);

#endif
