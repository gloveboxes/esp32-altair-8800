#include "axs15231b_lcd.h"

#include <ctype.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_config.h"

static const char *TAG = "AXS15231B";

#define LCD_SPI_HOST ALTAIR_AXS15231B_SPI_HOST
#define LCD_PIXEL_CLK_HZ (60 * 1000 * 1000)
#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

/*
 * The AXS15231B controller is configured with MADCTL MV so the panel scans in
 * landscape while the QSPI stream still uses its native 320-column order. The
 * UI draws through a logical 480 x 320 API, and the PSRAM backing store is kept
 * in the controller stream order so present can copy contiguous chunks into the
 * internal DMA buffers.
 *
 * NOTE: In QSPI mode the AXS15231B driver only emits CASET for color writes;
 * the panel auto-advances rows internally. Full-frame text refreshes use the
 * known-good stream order from y=0 with RAMWR followed by RAMWRC chunks.
 * Text/status partial updates are deliberately disabled until the local driver
 * owns the controller addressing rules completely.
 */
#define FRAMEBUFFER_PIXELS (AXS15231B_LCD_H_RES * AXS15231B_LCD_V_RES)
#define STREAM_LCD_W AXS15231B_LCD_V_RES
#define STREAM_LCD_H AXS15231B_LCD_H_RES
#define STATUS_REGION_H 20
/* Native rows per ping-pong DMA buffer. Two buffers × DMA_FLUSH_ROWS × 320px ×
 * 2 bytes are kept permanently in internal DRAM, so this directly trades
 * internal DRAM for QSPI transaction overhead. 32 rows = 20 KB per buffer
 * (40 KB total): a 480-row frame is sent in 15 ping-pong chunks. Sized to
 * leave ~30 KB DMA pool headroom for sustained SDMMC + WiFi traffic. */
#define DMA_FLUSH_ROWS 32
#define AXS15231B_MADCTL_LANDSCAPE LCD_CMD_MV_BIT
#define DMA_FLUSH_PIXELS (STREAM_LCD_W * DMA_FLUSH_ROWS)

#if AXS15231B_LCD_H_RES != 480 || AXS15231B_LCD_V_RES != 320
#error "AXS15231B logical/stream mapping expects 480x320 logical dimensions"
#endif

#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define BACKLIGHT_LEDC_FREQ_HZ 5000

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static bool s_backlight_pwm_ready = false;
static uint16_t *s_framebuffer = NULL;
/* Two DMA buffers for ping-pong: while one is being sent over QSPI DMA the
 * other is composed (memcpy) from PSRAM. The counting semaphore tracks how
 * many buffers are currently free; the on-color-trans-done ISR gives one slot
 * back per completed transfer. */
DMA_ATTR static uint16_t s_dma_flush_buf_a[DMA_FLUSH_PIXELS];
DMA_ATTR static uint16_t s_dma_flush_buf_b[DMA_FLUSH_PIXELS];
static SemaphoreHandle_t s_buf_free_sem = NULL;  /* counting, max 2 */

static inline uint16_t color_to_bus(panel_color_t color)
{
    /* RGB565 byte-swap for the panel bus */
    return (uint16_t)((color >> 8) | (color << 8));
}

static int qspi_cmd(int cmd, uint64_t opcode)
{
    cmd &= 0xff;
    cmd <<= 8;
    cmd |= opcode << 24;
    return cmd;
}

static esp_err_t qspi_tx_param(int cmd, const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_param(s_io_handle, qspi_cmd(cmd, LCD_OPCODE_WRITE_CMD), param, param_size);
}

static bool axs_on_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    BaseType_t high_task_wakeup = pdFALSE;
    SemaphoreHandle_t free_sem = (SemaphoreHandle_t)user_ctx;

    /* Release the buffer slot consumed by the just-completed transfer. */
    xSemaphoreGiveFromISR(free_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t qspi_tx_color_async(int cmd, const void *param, size_t param_size)
{
    /* Non-blocking: the IDF lcd panel io driver queues the transaction; the
     * caller must have already reserved a free buffer slot via s_buf_free_sem
     * so the payload pointer remains valid until the done ISR releases it. */
    return esp_lcd_panel_io_tx_color(s_io_handle, qspi_cmd(cmd, LCD_OPCODE_WRITE_COLOR), param, param_size);
}

static void wait_all_dma_idle(void)
{
    if (!s_buf_free_sem) return;
    /* Drain both slots, then put them back. When this returns no DMA is in
     * flight and both buffers are safe to reuse. */
    xSemaphoreTake(s_buf_free_sem, portMAX_DELAY);
    xSemaphoreTake(s_buf_free_sem, portMAX_DELAY);
    xSemaphoreGive(s_buf_free_sem);
    xSemaphoreGive(s_buf_free_sem);
}

static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {LCD_CMD_MADCTL, (uint8_t[]){AXS15231B_MADCTL_LANDSCAPE}, 1, 0},
    {0x13, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static const uint8_t s_font5x7[][5] = {
    {0x7E, 0x09, 0x09, 0x09, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x40, 0x3F, 0x00},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x02, 0x04, 0x08, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x30, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x22, 0x41, 0x49, 0x49, 0x36},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3E, 0x49, 0x49, 0x49, 0x32}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x26, 0x49, 0x49, 0x49, 0x3E},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x02, 0x01, 0x51, 0x09, 0x06},
};

static int glyph_index(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == '.') return 36;
    if (c == ':') return 37;
    if (c == '-') return 38;
    if (c == '|') return 39;
    if (c == '/') return 40;
    if (c == '?') return 41;
    return -1;
}

static inline void fb_put_pixel(int x, int y, uint16_t bus_color)
{
    if ((unsigned)x >= AXS15231B_LCD_H_RES || (unsigned)y >= AXS15231B_LCD_V_RES) return;
    int stream_x = STREAM_LCD_W - 1 - y;
    int stream_y = x;
    s_framebuffer[stream_y * STREAM_LCD_W + stream_x] = bus_color;
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t bus_color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= AXS15231B_LCD_H_RES || y >= AXS15231B_LCD_V_RES) return;
    if (x + w > AXS15231B_LCD_H_RES) w = AXS15231B_LCD_H_RES - x;
    if (y + h > AXS15231B_LCD_V_RES) h = AXS15231B_LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    int stream_x_start = STREAM_LCD_W - y - h;
    for (int lx = x; lx < x + w; lx++) {
        uint16_t *row = &s_framebuffer[lx * STREAM_LCD_W + stream_x_start];
        for (int i = 0; i < h; i++) row[i] = bus_color;
    }
}

void axs15231b_lcd_blit_glyph_cell(int x, int y,
                                   panel_color_t fg, panel_color_t bg,
                                   const uint8_t *glyph_cols,
                                   int cell_w, int cell_h,
                                   int glyph_w, int glyph_h,
                                   int xoff, int yoff)
{
    if (s_framebuffer == NULL) return;
    /* Cells from vt100_terminal are always within the screen, but clamp
     * defensively rather than scattering bounds checks in the hot path. */
    if (x < 0 || y < 0) return;
    if (x + cell_w > AXS15231B_LCD_H_RES) return;
    if (y + cell_h > AXS15231B_LCD_V_RES) return;

    const uint16_t bg_bus = color_to_bus(bg);
    const uint16_t fg_bus = color_to_bus(fg);

    /* In stream order, each logical column lx is a contiguous run of
     * STREAM_LCD_W uint16's; pixels with logical_y in [y, y+cell_h) occupy
     * stream_x in [STREAM_LCD_W - y - cell_h, STREAM_LCD_W - y), where
     * row[0] corresponds to logical_y = y + cell_h - 1 (bottom of cell) and
     * row[cell_h - 1] corresponds to logical_y = y (top of cell). */
    int stream_x_start = STREAM_LCD_W - y - cell_h;

    for (int lx = 0; lx < cell_w; lx++) {
        uint16_t *col = &s_framebuffer[(x + lx) * STREAM_LCD_W + stream_x_start];

        /* Fill the whole column with bg first; PSRAM-friendly sequential write. */
        for (int i = 0; i < cell_h; i++) col[i] = bg_bus;

        /* Overlay the glyph column, if this lx falls inside the glyph rect. */
        if (!glyph_cols) continue;
        int fc = lx - xoff;
        if (fc < 0 || fc >= glyph_w) continue;

        uint8_t bits = glyph_cols[fc];
        if (!bits) continue;

        /* For glyph row fr, logical_y = y + yoff + fr; stream index from the
         * column base is (cell_h - 1) - (yoff + fr). */
        int base = (cell_h - 1) - yoff;
        for (int fr = 0; fr < glyph_h; fr++) {
            if (bits & (1U << fr)) {
                col[base - fr] = fg_bus;
            }
        }
    }
}

static int compose_flush_chunk(uint16_t *out, int physical_y, int remaining_rows)
{
    int chunk_rows = remaining_rows;
    if (chunk_rows > DMA_FLUSH_ROWS) chunk_rows = DMA_FLUSH_ROWS;

    memcpy(out, &s_framebuffer[physical_y * STREAM_LCD_W],
           chunk_rows * STREAM_LCD_W * sizeof(uint16_t));

    return chunk_rows;
}

static void backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num = ALTAIR_AXS15231B_PIN_BL,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    s_backlight_pwm_ready = true;
}

esp_err_t axs15231b_lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing %s AXS15231B QSPI LCD", ALTAIR_BOARD_NAME);

    backlight_init();

    s_framebuffer = (uint16_t *)heap_caps_malloc(FRAMEBUFFER_PIXELS * sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_framebuffer == NULL) {
        s_framebuffer = (uint16_t *)heap_caps_malloc(FRAMEBUFFER_PIXELS * sizeof(uint16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_framebuffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "panel framebuffer alloc failed (%u bytes)",
                        (unsigned)(FRAMEBUFFER_PIXELS * sizeof(uint16_t)));

    uint16_t bg = color_to_bus(PANEL_COLOR_BLACK);
    for (int i = 0; i < FRAMEBUFFER_PIXELS; i++) s_framebuffer[i] = bg;

    spi_bus_config_t buscfg = {
        .sclk_io_num = ALTAIR_AXS15231B_PIN_SCLK,
        .data0_io_num = ALTAIR_AXS15231B_PIN_DATA0,
        .data1_io_num = ALTAIR_AXS15231B_PIN_DATA1,
        .data2_io_num = ALTAIR_AXS15231B_PIN_DATA2,
        .data3_io_num = ALTAIR_AXS15231B_PIN_DATA3,
        .max_transfer_sz = DMA_FLUSH_PIXELS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    if (!s_buf_free_sem) {
        /* Two slots: one per ping-pong DMA buffer. */
        s_buf_free_sem = xSemaphoreCreateCounting(2, 2);
        ESP_RETURN_ON_FALSE(s_buf_free_sem != NULL, ESP_ERR_NO_MEM, TAG,
                            "panel free-buffer semaphore alloc failed");
    }

    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(
        ALTAIR_AXS15231B_PIN_CS, axs_on_color_transfer_done, s_buf_free_sem);
    io_config.pclk_hz = LCD_PIXEL_CLK_HZ;
    /* trans_queue_depth must be >= 2 so we can have one transfer in flight
     * while a second is queued, allowing CPU to compose the next chunk in the
     * other DMA buffer concurrently. */
    io_config.trans_queue_depth = 2;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_config, &s_io_handle),
                        TAG, "panel IO init failed");

    axs15231b_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ALTAIR_AXS15231B_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(s_io_handle, &panel_config, &s_panel_handle),
                        TAG, "panel driver init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    /* Component treats arg as `off`: false => display ON. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, false), TAG, "panel display-on failed");

    axs15231b_lcd_present();

    axs15231b_lcd_set_backlight(100);
    ESP_LOGI(TAG, "AXS15231B LCD ready: logical %dx%d native %dx%d framebuffer %u bytes, static DMA flush %u bytes, QSPI CS=%d SCLK=%d D0=%d D1=%d D2=%d D3=%d BL=%d",
             AXS15231B_LCD_H_RES, AXS15231B_LCD_V_RES,
             STREAM_LCD_W, STREAM_LCD_H,
             (unsigned)(FRAMEBUFFER_PIXELS * sizeof(uint16_t)),
             (unsigned)(DMA_FLUSH_PIXELS * sizeof(uint16_t)),
             ALTAIR_AXS15231B_PIN_CS, ALTAIR_AXS15231B_PIN_SCLK,
             ALTAIR_AXS15231B_PIN_DATA0, ALTAIR_AXS15231B_PIN_DATA1,
             ALTAIR_AXS15231B_PIN_DATA2, ALTAIR_AXS15231B_PIN_DATA3,
             ALTAIR_AXS15231B_PIN_BL);
    return ESP_OK;
}

void axs15231b_lcd_set_backlight(int brightness)
{
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    if (!s_backlight_pwm_ready) {
        gpio_set_level(ALTAIR_AXS15231B_PIN_BL, brightness > 50 ? 1 : 0);
        return;
    }

    uint32_t max_duty = (1U << BACKLIGHT_LEDC_DUTY_RES) - 1U;
    uint32_t duty = (max_duty * (uint32_t)brightness) / 100U;
    ESP_ERROR_CHECK(ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL));
}

void axs15231b_lcd_fill_screen(panel_color_t color)
{
    if (s_framebuffer == NULL) return;
    uint16_t bus = color_to_bus(color);
    for (int i = 0; i < FRAMEBUFFER_PIXELS; i++) s_framebuffer[i] = bus;
}

void axs15231b_lcd_fill_rect(int x, int y, int w, int h, panel_color_t color)
{
    if (s_framebuffer == NULL) return;
    fb_fill_rect(x, y, w, h, color_to_bus(color));
}

void axs15231b_lcd_draw_pixel(int x, int y, panel_color_t color)
{
    if (s_framebuffer == NULL) return;
    fb_put_pixel(x, y, color_to_bus(color));
}

static void draw_char_scaled(int x, int y, char c, panel_color_t fg_color, panel_color_t bg_color, int scale)
{
    if (scale < 1) scale = 1;
    int glyph = glyph_index(c);
    int char_width = 6 * scale;
    int char_height = 7 * scale;
    uint16_t fg = color_to_bus(fg_color);
    uint16_t bg = color_to_bus(bg_color);

    if (glyph < 0) {
        fb_fill_rect(x, y, char_width, char_height, bg);
        return;
    }

    for (int row = 0; row < 7; row++) {
        for (int sy = 0; sy < scale; sy++) {
            int py = y + row * scale + sy;
            if ((unsigned)py >= AXS15231B_LCD_V_RES) continue;
            for (int col = 0; col < 6; col++) {
                bool lit = (col < 5) && ((s_font5x7[glyph][col] & (1 << row)) != 0);
                uint16_t pix = lit ? fg : bg;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    fb_put_pixel(px, py, pix);
                }
            }
        }
    }
}

void axs15231b_lcd_draw_string(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color, int scale)
{
    if (str == NULL || s_framebuffer == NULL) return;
    int char_width = 6 * ((scale < 1) ? 1 : scale);
    while (*str) {
        if (x + char_width > AXS15231B_LCD_H_RES) break;
        draw_char_scaled(x, y, *str, fg_color, bg_color, scale);
        x += char_width;
        str++;
    }
}

void axs15231b_lcd_draw_string_small(int x, int y, const char *str, panel_color_t fg_color, panel_color_t bg_color)
{
    axs15231b_lcd_draw_string(x, y, str, fg_color, bg_color, 1);
}

void axs15231b_lcd_draw_led_span(uint32_t bits, int num_leds, int x_start, int y,
                                 int led_size, int spacing, panel_color_t on_color,
                                 panel_color_t off_color, panel_color_t bg_color,
                                 int left_index, int right_index)
{
    if (s_framebuffer == NULL) return;
    if (num_leds <= 0 || led_size <= 0 || spacing <= 0) return;
    if (left_index < right_index) return;
    if (left_index >= num_leds) left_index = num_leds - 1;
    if (right_index < 0) right_index = 0;

    uint16_t bg = color_to_bus(bg_color);
    uint16_t on = color_to_bus(on_color);
    uint16_t off = color_to_bus(off_color);

    /* Place LEDs left-to-right with led MSB at left_index. */
    int span_x = x_start + (num_leds - 1 - left_index) * spacing;

    for (int led = left_index; led >= right_index; led--) {
        int led_x = span_x + (left_index - led) * spacing;
        uint16_t col = ((bits >> led) & 1U) ? on : off;
        fb_fill_rect(led_x, y, led_size, led_size, col);
        int gap = spacing - led_size;
        if (gap > 0 && led > right_index) {
            fb_fill_rect(led_x + led_size, y, gap, led_size, bg);
        }
    }
}

void axs15231b_lcd_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                int led_size, int spacing, panel_color_t on_color,
                                panel_color_t off_color, panel_color_t bg_color)
{
    axs15231b_lcd_draw_led_span(bits, num_leds, x_start, y, led_size, spacing,
                                on_color, off_color, bg_color, num_leds - 1, 0);
}

bool axs15231b_lcd_status_region_supported(void)
{
    return false;
}

void axs15231b_lcd_status_region_clear(panel_color_t color)
{
    (void)color;
}

void axs15231b_lcd_status_region_fill_rect(int x, int y, int w, int h, panel_color_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void axs15231b_lcd_status_region_draw_pixel(int x, int y, panel_color_t color)
{
    (void)x;
    (void)y;
    (void)color;
}

void axs15231b_lcd_status_region_present(void)
{
}

bool axs15231b_lcd_present_band_supported(void)
{
    return false;
}

void axs15231b_lcd_present_band(int y, int h)
{
    (void)y;
    (void)h;
    axs15231b_lcd_present();
}

void axs15231b_lcd_present(void)
{
    if (s_panel_handle == NULL || s_io_handle == NULL || s_framebuffer == NULL) return;

    extern volatile int g_panel_checkpoint;
    extern volatile uint32_t g_panel_checkpoint_count;
    g_panel_checkpoint = 11; g_panel_checkpoint_count++;  // entered axs present

    esp_err_t err = qspi_tx_param(LCD_CMD_CASET, (uint8_t[]) {
        0x00,
        0x00,
        ((STREAM_LCD_W - 1) >> 8) & 0xFF,
        (STREAM_LCD_W - 1) & 0xFF,
    }, 4);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "frame CASET failed: %s", esp_err_to_name(err));
        return;
    }
    g_panel_checkpoint = 12; g_panel_checkpoint_count++;

    int physical_y = 0;
    int remaining_rows = STREAM_LCD_H;
    int ram_cmd = LCD_CMD_RAMWR;

    /* Ping-pong: alternate between buffer A and B. While one is in flight
     * over QSPI DMA, the CPU composes the next chunk into the other from
     * PSRAM. The counting semaphore (released by the done ISR) gates each
     * compose so we never overwrite a buffer that is still being DMA'd. */
    uint16_t *bufs[2] = { s_dma_flush_buf_a, s_dma_flush_buf_b };
    int idx = 0;

    while (remaining_rows > 0) {
        int chunk_y = physical_y;

        /* Wait until this buffer slot is free (no DMA in flight on it). */
        xSemaphoreTake(s_buf_free_sem, portMAX_DELAY);

        int chunk_rows = compose_flush_chunk(bufs[idx], physical_y, remaining_rows);
        physical_y += chunk_rows;
        remaining_rows -= chunk_rows;

        err = qspi_tx_color_async(ram_cmd, bufs[idx],
                                  chunk_rows * STREAM_LCD_W * sizeof(uint16_t));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "frame flush chunk failed @ y=%d: %s", chunk_y, esp_err_to_name(err));
            /* Release the slot we reserved; ISR won't fire for a failed submit. */
            xSemaphoreGive(s_buf_free_sem);
            wait_all_dma_idle();
            return;
        }
        ram_cmd = LCD_CMD_RAMWRC;
        idx ^= 1;
    }

    /* Ensure all DMA has completed before returning so the next call (which
     * may issue a CASET param) doesn't race with in-flight color transfers. */
    wait_all_dma_idle();

    g_panel_checkpoint = 13; g_panel_checkpoint_count++;
    g_panel_checkpoint = 17; g_panel_checkpoint_count++;
}

int axs15231b_lcd_band_height(void)
{
    return AXS15231B_LCD_V_RES;
}

void axs15231b_lcd_begin_band(int y, int h)
{
    (void)y;
    (void)h;
}
