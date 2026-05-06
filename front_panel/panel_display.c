#include "panel_display.h"

#include "sdkconfig.h"

#if CONFIG_ALTAIR_DISPLAY_ILI9341
#include "ili9341.h"
#else
#include "axs15231b_lcd.h"
#endif

bool panel_display_init(void)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    return ili9341_init() == ESP_OK;
#else
    return axs15231b_lcd_init() == ESP_OK;
#endif
}

int panel_display_width(void)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    return LCD_H_RES;
#else
    return AXS15231B_LCD_H_RES;
#endif
}

int panel_display_height(void)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    return LCD_V_RES;
#else
    return AXS15231B_LCD_V_RES;
#endif
}

bool panel_display_is_monochrome(void)
{
    return false;
}

bool panel_display_is_banded(void)
{
    return false;
}

bool panel_display_bands_are_vertical(void)
{
    return false;
}

int panel_display_band_height(void)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    return panel_display_height();
#else
    return axs15231b_lcd_band_height();
#endif
}

void panel_display_begin_band(int y, int h)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    (void)y;
    (void)h;
#else
    axs15231b_lcd_begin_band(y, h);
#endif
}

void panel_display_fill_screen(panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_fill_screen(color);
#else
    axs15231b_lcd_fill_screen(color);
#endif
}

void panel_display_fill_rect(int x, int y, int w, int h, panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_fill_rect(x, y, w, h, color);
#else
    axs15231b_lcd_fill_rect(x, y, w, h, color);
#endif
}

void panel_display_draw_pixel(int x, int y, panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_draw_pixel(x, y, color);
#else
    axs15231b_lcd_draw_pixel(x, y, color);
#endif
}

void panel_display_draw_string(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color, int scale)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_draw_string(x, y, str, fg_color, bg_color, scale);
#else
    axs15231b_lcd_draw_string(x, y, str, fg_color, bg_color, scale);
#endif
}

void panel_display_draw_string_small(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_draw_string_small(x, y, str, fg_color, bg_color);
#else
    axs15231b_lcd_draw_string_small(x, y, str, fg_color, bg_color);
#endif
}

void panel_display_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                int led_size, int spacing, panel_color_t on_color,
                                panel_color_t off_color, panel_color_t bg_color)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_draw_led_row(bits, num_leds, x_start, y, led_size, spacing,
                         on_color, off_color, bg_color);
#else
    axs15231b_lcd_draw_led_row(bits, num_leds, x_start, y, led_size, spacing,
                               on_color, off_color, bg_color);
#endif
}

void panel_display_draw_led_span(uint32_t bits, int num_leds, int x_start, int y,
                                 int led_size, int spacing, panel_color_t on_color,
                                 panel_color_t off_color, panel_color_t bg_color,
                                 int left_index, int right_index)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_draw_led_span(bits, num_leds, x_start, y, led_size, spacing,
                          on_color, off_color, bg_color, left_index, right_index);
#else
    axs15231b_lcd_draw_led_span(bits, num_leds, x_start, y, led_size, spacing,
                                on_color, off_color, bg_color, left_index, right_index);
#endif
}

bool panel_display_status_region_supported(void)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    return axs15231b_lcd_status_region_supported();
#else
    return false;
#endif
}

void panel_display_status_region_clear(panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    axs15231b_lcd_status_region_clear(color);
#else
    (void)color;
#endif
}

void panel_display_status_region_fill_rect(int x, int y, int w, int h, panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    axs15231b_lcd_status_region_fill_rect(x, y, w, h, color);
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
#endif
}

void panel_display_status_region_draw_pixel(int x, int y, panel_color_t color)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    axs15231b_lcd_status_region_draw_pixel(x, y, color);
#else
    (void)x;
    (void)y;
    (void)color;
#endif
}

void panel_display_status_region_present(void)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    axs15231b_lcd_status_region_present();
#endif
}

bool panel_display_present_band_supported(void)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    return axs15231b_lcd_present_band_supported();
#else
    return false;
#endif
}

void panel_display_present_band(int y, int h)
{
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    axs15231b_lcd_present_band(y, h);
#else
    (void)y;
    (void)h;
    panel_display_present();
#endif
}

void panel_display_present(void)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_wait_async();
#else
    axs15231b_lcd_present();
#endif
}

void panel_display_set_backlight(int brightness)
{
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    ili9341_set_backlight(brightness);
#else
    axs15231b_lcd_set_backlight(brightness);
#endif
}