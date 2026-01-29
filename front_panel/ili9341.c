/**
 * @file ili9341.c
 * @brief ILI9341 LCD driver for Freenove ESP32-S3 FNK0104B board
 * 
 * Based on TFT_eSPI library initialization sequence (ILI9341_2_DRIVER)
 * Uses direct ESP-IDF SPI driver for proper data streaming
 */

#include "ili9341.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "ILI9341";

// SPI device handle
static spi_device_handle_t spi_handle = NULL;

// Simple 8x8 font (ASCII 32-127)
static const uint8_t font8x8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};

// Compact 5x7 font for LED labels (A-Z, 0-9, space)
// Each byte is a column, bit 0 = top
static const uint8_t font5x7[][5] = {
    // A-Z (index 0-25)
    {0x7E, 0x09, 0x09, 0x09, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x40, 0x3F, 0x00}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x02, 0x04, 0x08, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x26, 0x49, 0x49, 0x49, 0x32}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x30, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    // 0-9 (index 26-35)
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x22, 0x41, 0x49, 0x49, 0x36}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3E, 0x49, 0x49, 0x49, 0x32}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x26, 0x49, 0x49, 0x49, 0x3E}, // 9
    // Punctuation (index 36-40)
    {0x00, 0x60, 0x60, 0x00, 0x00}, // . (period)
    {0x00, 0x36, 0x36, 0x00, 0x00}, // : (colon)
    {0x08, 0x08, 0x08, 0x08, 0x08}, // - (hyphen/minus)
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // | (vertical bar)
    {0x20, 0x10, 0x08, 0x04, 0x02}, // / (forward slash)
};

// DMA-capable buffers for pixel data - double buffered for async LED row drawing
// Each buffer sized for full LED row block (320 x 15 = 4800 pixels)
static uint16_t *dma_buffers[2] = {NULL, NULL};
static int active_buffer = 0;  // Which buffer to fill next
#define DMA_BUFFER_SIZE (LCD_H_RES * 16)  // Full width x max LED height

// Legacy aliases for other functions
#define dma_buffer dma_buffers[0]
#define dma_buffer2 dma_buffers[1]

// Track if there's a pending async transaction
static bool async_pending = false;
static spi_transaction_t async_trans;

//-----------------------------------------------------------------------------
// Low-level SPI functions
//-----------------------------------------------------------------------------

// Wait for any pending async transfer to complete
static void lcd_wait_async(void)
{
    if (async_pending) {
        spi_transaction_t *rtrans;
        spi_device_get_trans_result(spi_handle, &rtrans, portMAX_DELAY);
        async_pending = false;
    }
}

// Send command byte (DC=0) - uses polling (small transfer)
static void lcd_cmd(uint8_t cmd)
{
    lcd_wait_async();  // Wait for any pending pixel transfer
    gpio_set_level(LCD_PIN_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

// Send data bytes (DC=1) - uses polling (small transfer)
static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    lcd_wait_async();  // Wait for any pending pixel transfer
    gpio_set_level(LCD_PIN_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

// Send single data byte
static void lcd_data_byte(uint8_t data)
{
    lcd_data(&data, 1);
}

// Send pixel data (DC=1) - uses async DMA for large transfers
// Note: The buffer must remain valid until the transfer completes!
static void lcd_write_pixels_async(const uint16_t *data, size_t pixel_count)
{
    if (pixel_count == 0) return;
    lcd_wait_async();  // Wait for previous transfer
    
    gpio_set_level(LCD_PIN_DC, 1);
    memset(&async_trans, 0, sizeof(async_trans));
    async_trans.length = pixel_count * 16;
    async_trans.tx_buffer = data;
    
    spi_device_queue_trans(spi_handle, &async_trans, portMAX_DELAY);
    async_pending = true;
}

// Send pixel data (DC=1) - blocking version for small transfers
static void lcd_write_pixels(const uint16_t *data, size_t pixel_count)
{
    if (pixel_count == 0) return;
    lcd_wait_async();  // Wait for any pending async transfer
    gpio_set_level(LCD_PIN_DC, 1);
    spi_transaction_t t = {
        .length = pixel_count * 16,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

esp_err_t ili9341_init(void)
{
    ESP_LOGI(TAG, "Initializing ILI9341 display (FNK0104B board)");

    // Allocate DMA-capable buffers - both full size for double-buffered LED rows
    dma_buffers[0] = heap_caps_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    dma_buffers[1] = heap_caps_malloc(DMA_BUFFER_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!dma_buffers[0] || !dma_buffers[1]) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffers");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Allocated DMA buffers (%d + %d bytes)", DMA_BUFFER_SIZE * 2, LCD_H_RES * 2);

    // Configure GPIO pins (DC and backlight)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Keep backlight OFF during init to avoid showing uninitialized display
    gpio_set_level(LCD_PIN_BL, 0);

    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DMA_BUFFER_SIZE * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Attach LCD to SPI bus
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_PIXEL_CLK,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &devcfg, &spi_handle));

    ESP_LOGI(TAG, "SPI initialized: MOSI=%d, SCLK=%d, CS=%d, DC=%d",
             LCD_PIN_MOSI, LCD_PIN_SCLK, LCD_PIN_CS, LCD_PIN_DC);

    // Software reset (FNK0104B has no hardware RST pin - tied to ESP32 RST)
    lcd_cmd(0x01);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));

    //=========================================================================
    // ILI9341_2_DRIVER initialization sequence from TFT_eSPI
    //=========================================================================
    
    // Power control B
    lcd_cmd(0xCF);
    lcd_data((uint8_t[]){0x00, 0xC1, 0x30}, 3);

    // Power on sequence control
    lcd_cmd(0xED);
    lcd_data((uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);

    // Driver timing control A
    lcd_cmd(0xE8);
    lcd_data((uint8_t[]){0x85, 0x00, 0x78}, 3);

    // Power control A
    lcd_cmd(0xCB);
    lcd_data((uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);

    // Pump ratio control
    lcd_cmd(0xF7);
    lcd_data_byte(0x20);

    // Driver timing control B
    lcd_cmd(0xEA);
    lcd_data((uint8_t[]){0x00, 0x00}, 2);

    // Power Control 1 (ILI9341_2 uses 0x10)
    lcd_cmd(0xC0);
    lcd_data_byte(0x10);

    // Power Control 2 (ILI9341_2 uses 0x00)
    lcd_cmd(0xC1);
    lcd_data_byte(0x00);

    // VCOM Control 1 (ILI9341_2 uses 0x30, 0x30)
    lcd_cmd(0xC5);
    lcd_data((uint8_t[]){0x30, 0x30}, 2);

    // VCOM Control 2 (ILI9341_2 uses 0xB7)
    lcd_cmd(0xC7);
    lcd_data_byte(0xB7);

    // Memory Access Control - Landscape mode with BGR color order
    // MADCTL bits: MY(7), MX(6), MV(5), ML(4), BGR(3), MH(2)
    // 0xE8 = MY + MX + MV + BGR = Landscape with correct orientation (no mirror)
    lcd_cmd(0x36);
    lcd_data_byte(0xE8);  // MY=1, MX=1, MV=1, BGR=1 = Landscape, text reads correctly

    // Pixel Format Set - 16 bits per pixel
    lcd_cmd(0x3A);
    lcd_data_byte(0x55);

    // Frame Rate Control
    lcd_cmd(0xB1);
    lcd_data((uint8_t[]){0x00, 0x1A}, 2);

    // Display Function Control
    lcd_cmd(0xB6);
    lcd_data((uint8_t[]){0x08, 0x82, 0x27}, 3);

    // 3Gamma Function Disable
    lcd_cmd(0xF2);
    lcd_data_byte(0x00);

    // Gamma curve selected
    lcd_cmd(0x26);
    lcd_data_byte(0x01);

    // Positive Gamma Correction (ILI9341_2_DRIVER values)
    lcd_cmd(0xE0);
    lcd_data((uint8_t[]){0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9,
                         0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00}, 15);

    // Negative Gamma Correction (ILI9341_2_DRIVER values)
    lcd_cmd(0xE1);
    lcd_data((uint8_t[]){0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56,
                         0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F}, 15);

    // Set Column Address (0-319 for landscape)
    lcd_cmd(0x2A);
    lcd_data((uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);

    // Set Page Address (0-239 for landscape)
    lcd_cmd(0x2B);
    lcd_data((uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4);

    // Display Inversion ON (required for FNK0104B - TFT_INVERSION_ON in config)
    lcd_cmd(0x21);

    // Sleep Out
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Display ON
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear screen to black before turning on backlight (avoids flash of random data)
    ili9341_fill_screen(COLOR_BLACK);

    // Now turn on backlight - screen is ready
    gpio_set_level(LCD_PIN_BL, 1);
    ESP_LOGI(TAG, "Backlight ON (GPIO%d HIGH)", LCD_PIN_BL);

    ESP_LOGI(TAG, "ILI9341 initialization complete");
    return ESP_OK;
}

void ili9341_set_backlight(int brightness)
{
    gpio_set_level(LCD_PIN_BL, brightness > 50 ? 1 : 0);
}

//-----------------------------------------------------------------------------
// Drawing functions - Key fix: send RAMWR command ONCE, then stream all data
//-----------------------------------------------------------------------------

// Set drawing window and send RAMWR command
static void ili9341_set_window(int x0, int y0, int x1, int y1)
{
    // Column address set (CASET)
    lcd_cmd(0x2A);
    lcd_data((uint8_t[]){(x0 >> 8) & 0xFF, x0 & 0xFF,
                         (x1 >> 8) & 0xFF, x1 & 0xFF}, 4);
    
    // Page address set (PASET)
    lcd_cmd(0x2B);
    lcd_data((uint8_t[]){(y0 >> 8) & 0xFF, y0 & 0xFF,
                         (y1 >> 8) & 0xFF, y1 & 0xFF}, 4);
    
    // Memory write command - leave DC high after this for pixel data
    lcd_cmd(0x2C);
}

void ili9341_fill_screen(uint16_t color)
{
    // Byte swap for big-endian SPI transfer
    uint16_t swapped = (color >> 8) | (color << 8);
    
    // Fill both DMA buffers with color (one line worth each)
    for (int i = 0; i < LCD_H_RES; i++) {
        dma_buffer[i] = swapped;
        dma_buffer2[i] = swapped;
    }
    
    // Set window for full screen
    ili9341_set_window(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);
    
    // Stream all pixel data using async DMA with double buffering
    // While one buffer is being transmitted, we can prepare the next
    // (In this case both buffers have the same content, but this demonstrates the pattern)
    uint16_t *buffers[2] = {dma_buffer, dma_buffer2};
    for (int y = 0; y < LCD_V_RES; y++) {
        lcd_write_pixels_async(buffers[y & 1], LCD_H_RES);
    }
    lcd_wait_async();  // Wait for final transfer to complete
}

void ili9341_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    // Bounds check
    if (x >= LCD_H_RES || y >= LCD_V_RES) return;
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    
    // Byte swap
    uint16_t swapped = (color >> 8) | (color << 8);
    
    int total_pixels = w * h;
    
    // Set window
    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    
    // For small rectangles (like LEDs), send all pixels in one transfer
    if (total_pixels <= DMA_BUFFER_SIZE) {
        // Fill buffer with all pixels
        for (int i = 0; i < total_pixels; i++) {
            dma_buffer[i] = swapped;
        }
        lcd_write_pixels(dma_buffer, total_pixels);
    } else {
        // Large rectangles: stream row by row with double buffering
        int pixels_per_line = w;
        for (int i = 0; i < pixels_per_line; i++) {
            dma_buffer[i] = swapped;
            dma_buffer2[i] = swapped;
        }
        
        uint16_t *buffers[2] = {dma_buffer, dma_buffer2};
        for (int row = 0; row < h; row++) {
            lcd_write_pixels_async(buffers[row & 1], w);
        }
        lcd_wait_async();
    }
}

void ili9341_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) return;
    
    uint16_t swapped = (color >> 8) | (color << 8);
    
    ili9341_set_window(x, y, x, y);
    lcd_write_pixels(&swapped, 1);
}

void ili9341_draw_char(int x, int y, char c, uint16_t fg_color, uint16_t bg_color, int scale)
{
    if (c < 32 || c > 126) c = '?';
    int idx = c - 32;
    
    uint16_t fg_swap = (fg_color >> 8) | (fg_color << 8);
    uint16_t bg_swap = (bg_color >> 8) | (bg_color << 8);
    
    int char_width = 8 * scale;
    int char_height = 8 * scale;
    
    // Bounds check
    if (x + char_width > LCD_H_RES) char_width = LCD_H_RES - x;
    if (y + char_height > LCD_V_RES) char_height = LCD_V_RES - y;
    if (char_width <= 0 || char_height <= 0) return;
    
    // Set window for entire character
    ili9341_set_window(x, y, x + char_width - 1, y + char_height - 1);
    
    // Double-buffered async DMA character drawing
    uint16_t *buffers[2] = {dma_buffer, dma_buffer2};
    int buf_num = 0;
    
    // Draw character - stream all pixels after single RAMWR
    for (int row = 0; row < 8 && (row * scale) < char_height; row++) {
        uint8_t line = font8x8[idx][row];
        
        // Build one row of scaled character in current buffer
        uint16_t *cur_buf = buffers[buf_num & 1];
        int buf_idx = 0;
        for (int col = 0; col < 8 && (col * scale) < char_width; col++) {
            uint16_t pixel = (line & (1 << col)) ? fg_swap : bg_swap;
            for (int sx = 0; sx < scale && buf_idx < char_width; sx++) {
                cur_buf[buf_idx++] = pixel;
            }
        }
        
        // Write this row 'scale' times using async DMA
        for (int sy = 0; sy < scale; sy++) {
            lcd_write_pixels_async(cur_buf, char_width);
            buf_num++;  // Alternate buffer for next write
            // Copy to next buffer for subsequent writes of same row
            if (sy < scale - 1) {
                uint16_t *next_buf = buffers[buf_num & 1];
                memcpy(next_buf, cur_buf, char_width * sizeof(uint16_t));
                cur_buf = next_buf;
            }
        }
    }
    lcd_wait_async();  // Wait for final transfer
}

void ili9341_draw_string(int x, int y, const char *str, uint16_t fg_color, uint16_t bg_color, int scale)
{
    int char_width = 8 * scale;
    
    while (*str) {
        if (x + char_width > LCD_H_RES) break;
        ili9341_draw_char(x, y, *str, fg_color, bg_color, scale);
        x += char_width;
        str++;
    }
}

void ili9341_draw_string_centered(int y, const char *str, uint16_t fg_color, uint16_t bg_color, int scale)
{
    int len = strlen(str);
    int char_width = 8 * scale;
    int total_width = len * char_width;
    int x = (LCD_H_RES - total_width) / 2;
    
    if (x < 0) x = 0;
    
    ili9341_draw_string(x, y, str, fg_color, bg_color, scale);
}

// Draw a character using compact 5x7 font (6 pixels wide with spacing)
void ili9341_draw_char_small(int x, int y, char c, uint16_t fg_color, uint16_t bg_color)
{
    int glyph_idx = -1;
    
    // Map character to glyph index
    if (c >= 'A' && c <= 'Z')
        glyph_idx = c - 'A';
    else if (c >= 'a' && c <= 'z')
        glyph_idx = c - 'a';  // lowercase maps to uppercase
    else if (c >= '0' && c <= '9')
        glyph_idx = 26 + (c - '0');
    else if (c == '.')
        glyph_idx = 36;
    else if (c == ':')
        glyph_idx = 37;
    else if (c == '-')
        glyph_idx = 38;
    else if (c == '|')
        glyph_idx = 39;
    else if (c == '/')
        glyph_idx = 40;
    else if (c == ' ')
        return;  // Space - just skip (caller advances x)
    else
        return;  // Unsupported character
    
    uint16_t fg_swap = (fg_color >> 8) | (fg_color << 8);
    uint16_t bg_swap = (bg_color >> 8) | (bg_color << 8);
    
    // Set window for character (6x7 including spacing column)
    ili9341_set_window(x, y, x + 5, y + 6);
    
    // Build pixels row by row
    uint16_t char_buf[42];  // 6 cols x 7 rows
    int idx = 0;
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 6; col++) {
            if (col < 5) {
                uint8_t column_data = font5x7[glyph_idx][col];
                char_buf[idx++] = (column_data & (1 << row)) ? fg_swap : bg_swap;
            } else {
                char_buf[idx++] = bg_swap;  // Spacing column
            }
        }
    }
    lcd_write_pixels(char_buf, 42);
}

// Draw string using compact 5x7 font (6 pixels per character)
void ili9341_draw_string_small(int x, int y, const char *str, uint16_t fg_color, uint16_t bg_color)
{
    while (*str) {
        if (*str == ' ') {
            x += 6;
        } else {
            ili9341_draw_char_small(x, y, *str, fg_color, bg_color);
            x += 6;
        }
        str++;
    }
}

// Draw a row of LEDs efficiently using async DMA with double buffering
// Builds block in inactive buffer while previous transfer completes
void ili9341_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                          int led_size, int spacing, uint16_t on_color, uint16_t off_color)
{
    // Calculate total width of the row
    int total_width = (num_leds - 1) * spacing + led_size;
    
    // Bounds check
    if (x_start < 0 || y < 0 || x_start + total_width > LCD_H_RES || y + led_size > LCD_V_RES)
        return;
    
    // Byte swap colors for SPI
    uint16_t on_swap = (on_color >> 8) | (on_color << 8);
    uint16_t off_swap = (off_color >> 8) | (off_color << 8);
    uint16_t bg_swap = 0;  // Black background between LEDs
    
    // Get the buffer we'll fill (not the one currently being transmitted)
    uint16_t *buf = dma_buffers[active_buffer];
    
    // Build the entire LED row block directly in DMA buffer
    // LEDs are drawn MSB on left (highest bit index first)
    int scanline_width = 0;
    
    // Build first scanline
    for (int led = num_leds - 1; led >= 0; led--) {
        uint16_t led_color = (bits >> led) & 1 ? on_swap : off_swap;
        
        // LED pixels
        for (int px = 0; px < led_size; px++) {
            buf[scanline_width++] = led_color;
        }
        
        // Gap pixels (except after last LED)
        if (led > 0) {
            int gap = spacing - led_size;
            for (int px = 0; px < gap; px++) {
                buf[scanline_width++] = bg_swap;
            }
        }
    }
    
    // Replicate first scanline for remaining rows (use memcpy for speed)
    for (int row = 1; row < led_size; row++) {
        memcpy(buf + row * scanline_width, buf, scanline_width * sizeof(uint16_t));
    }
    
    int total_pixels = scanline_width * led_size;
    
    // Wait for any previous async transfer to complete before setting new window
    lcd_wait_async();
    
    // Set window for entire LED row block
    ili9341_set_window(x_start, y, x_start + total_width - 1, y + led_size - 1);
    
    // Start async DMA transfer
    lcd_write_pixels_async(buf, total_pixels);
    
    // Switch to other buffer for next call
    active_buffer = 1 - active_buffer;
}

// Wait for any pending LED row DMA transfer to complete
void ili9341_wait_async(void)
{
    lcd_wait_async();
}
