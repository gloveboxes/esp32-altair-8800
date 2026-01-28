/**
 * @file ili9341.h
 * @brief ILI9341 LCD driver for Freenove ESP32-S3 board
 */

#ifndef ILI9341_H
#define ILI9341_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// Freenove ESP32-S3 LCD Pin Definitions (2.8" ILI9341 TFT)
// Reference: Freenove FNK0104B ESP32-S3 Display - TFT_eSPI_Setups/FNK0104B_2.8_240x320_ILI9341.h
#define LCD_PIN_MOSI    11      // SPI MOSI (Master Out Slave In) - FSPI_D
#define LCD_PIN_SCLK    12      // SPI Clock - FSPI_CLK
#define LCD_PIN_CS      10      // Chip Select - FSPI_CS
#define LCD_PIN_DC      46      // Data/Command (NOT 13!)
#define LCD_PIN_RST     -1      // Reset tied to ESP32 RST (no separate GPIO)
#define LCD_PIN_BL      45      // Backlight (active HIGH)

// LCD Parameters (Landscape mode: 320x240)
#define LCD_H_RES       320
#define LCD_V_RES       240
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   (80 * 1000 * 1000)  // 80 MHz (ILI9341 max for writes)

// Color definitions (RGB565)
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_MAGENTA   0xF81F
#define COLOR_ORANGE    0xFD20

/**
 * @brief Initialize the ILI9341 LCD display
 * @return ESP_OK on success
 */
esp_err_t ili9341_init(void);

/**
 * @brief Fill the entire screen with a color
 * @param color RGB565 color value
 */
void ili9341_fill_screen(uint16_t color);

/**
 * @brief Draw a single pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color RGB565 color value
 */
void ili9341_draw_pixel(int x, int y, uint16_t color);

/**
 * @brief Fill a rectangular area with a color
 * @param x Start X coordinate
 * @param y Start Y coordinate
 * @param w Width
 * @param h Height
 * @param color RGB565 color value
 */
void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Draw a character at specified position
 * @param x X coordinate
 * @param y Y coordinate
 * @param c Character to draw
 * @param fg_color Foreground color (RGB565)
 * @param bg_color Background color (RGB565)
 * @param scale Font scale (1 = 8x8, 2 = 16x16, etc.)
 */
void ili9341_draw_char(int x, int y, char c, uint16_t fg_color, uint16_t bg_color, int scale);

/**
 * @brief Draw a string at specified position
 * @param x X coordinate
 * @param y Y coordinate
 * @param str String to draw
 * @param fg_color Foreground color (RGB565)
 * @param bg_color Background color (RGB565)
 * @param scale Font scale (1 = 8x8, 2 = 16x16, etc.)
 */
void ili9341_draw_string(int x, int y, const char *str, uint16_t fg_color, uint16_t bg_color, int scale);

/**
 * @brief Draw a centered string
 * @param y Y coordinate
 * @param str String to draw
 * @param fg_color Foreground color (RGB565)
 * @param bg_color Background color (RGB565)
 * @param scale Font scale
 */
void ili9341_draw_string_centered(int y, const char *str, uint16_t fg_color, uint16_t bg_color, int scale);

/**
 * @brief Draw a string using compact 5x7 font (6 pixels per character)
 * @param x X coordinate
 * @param y Y coordinate
 * @param str String to draw (A-Z, 0-9, space supported)
 * @param fg_color Foreground color (RGB565)
 * @param bg_color Background color (RGB565)
 */
void ili9341_draw_string_small(int x, int y, const char *str, uint16_t fg_color, uint16_t bg_color);

/**
 * @brief Draw a row of LEDs efficiently using async DMA
 * Uses double buffering - prepares next row while current transfers
 * @param bits Bit pattern for LED states (bit 0 = rightmost LED)
 * @param num_leds Number of LEDs in the row
 * @param x_start Starting X position
 * @param y Y position
 * @param led_size Size of each LED (square)
 * @param spacing Distance between LED left edges
 * @param on_color Color for ON state
 * @param off_color Color for OFF state
 */
void ili9341_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                          int led_size, int spacing, uint16_t on_color, uint16_t off_color);

/**
 * @brief Wait for any pending async DMA transfer to complete
 * Call after drawing LED rows to ensure all transfers are done
 */
void ili9341_wait_async(void);

/**
 * @brief Set backlight brightness
 * @param brightness 0-100 percent
 */
void ili9341_set_backlight(int brightness);

#endif // ILI9341_H
