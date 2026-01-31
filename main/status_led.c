/**
 * @file status_led.c
 * @brief RGB Status LED driver for ESP32-S3
 *
 * Uses the RMT peripheral to control the onboard WS2812 RGB LED.
 * The LED indicates WiFi connection status:
 * - Green flash every 30 seconds: WiFi connected
 * - Red flash every 10 seconds: WiFi disconnected or connection lost
 */

#include "status_led.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"

static const char* TAG = "StatusLED";

// ESP32-S3 DevKitC onboard RGB LED - GPIO 42 per reference sketch
#define STATUS_LED_GPIO         42

// LED brightness (0-255)
#define LED_BRIGHTNESS          20

// Flash durations
#define LED_FLASH_ON_MS         100
#define LED_CONNECTED_INTERVAL_MS    10000   // 10 seconds when connected
#define LED_DISCONNECTED_INTERVAL_MS 10000   // 10 seconds when disconnected

// Task configuration
#define STATUS_LED_TASK_STACK_SIZE  2048
#define STATUS_LED_TASK_PRIORITY    2

// WS2812 timing parameters (in RMT ticks at 10MHz = 100ns per tick)
#define WS2812_T0H_TICKS    4   // 400ns
#define WS2812_T0L_TICKS    8   // 800ns
#define WS2812_T1H_TICKS    8   // 800ns
#define WS2812_T1L_TICKS    4   // 400ns
#define WS2812_RESET_US     280 // Reset time

// State variables
static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static volatile bool s_wifi_connected = false;

// WS2812 encoder structure
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = ws2812_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = ws2812_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws2812_encoder->state) {
        case 0: // Send RGB data
            encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                ws2812_encoder->state = 1;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                *ret_state = (rmt_encode_state_t)(*ret_state | RMT_ENCODING_MEM_FULL);
                return encoded_symbols;
            }
            // Fall through
        case 1: // Send reset code
            encoded_symbols += copy_encoder->encode(copy_encoder, channel, &ws2812_encoder->reset_code,
                                                    sizeof(ws2812_encoder->reset_code), &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                ws2812_encoder->state = RMT_ENCODING_RESET;
                *ret_state = (rmt_encode_state_t)(*ret_state | RMT_ENCODING_COMPLETE);
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                *ret_state = (rmt_encode_state_t)(*ret_state | RMT_ENCODING_MEM_FULL);
            }
            break;
    }
    return encoded_symbols;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws2812_encoder->bytes_encoder);
    rmt_del_encoder(ws2812_encoder->copy_encoder);
    free(ws2812_encoder);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws2812_encoder->bytes_encoder);
    rmt_encoder_reset(ws2812_encoder->copy_encoder);
    ws2812_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_create(rmt_encoder_handle_t *ret_encoder)
{
    ws2812_encoder_t *ws2812_encoder = calloc(1, sizeof(ws2812_encoder_t));
    if (!ws2812_encoder) {
        return ESP_ERR_NO_MEM;
    }

    ws2812_encoder->base.encode = ws2812_encode;
    ws2812_encoder->base.del = ws2812_encoder_del;
    ws2812_encoder->base.reset = ws2812_encoder_reset;

    // Create bytes encoder for RGB data
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = WS2812_T0H_TICKS,
            .level1 = 0,
            .duration1 = WS2812_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = WS2812_T1H_TICKS,
            .level1 = 0,
            .duration1 = WS2812_T1L_TICKS,
        },
        .flags.msb_first = 1,
    };

    esp_err_t ret = rmt_new_bytes_encoder(&bytes_encoder_config, &ws2812_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        free(ws2812_encoder);
        return ret;
    }

    // Create copy encoder for reset code
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ret = rmt_new_copy_encoder(&copy_encoder_config, &ws2812_encoder->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(ws2812_encoder->bytes_encoder);
        free(ws2812_encoder);
        return ret;
    }

    // Reset code: low level for reset duration
    ws2812_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = WS2812_RESET_US * 10 / 2, // Convert to ticks, split between duration0 and duration1
        .level1 = 0,
        .duration1 = WS2812_RESET_US * 10 / 2,
    };

    *ret_encoder = &ws2812_encoder->base;
    return ESP_OK;
}

/**
 * @brief Set LED color (GRB order for WS2812)
 */
static void led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_rmt_channel || !s_encoder) {
        return;
    }

    // WS2812 uses GRB order
    uint8_t grb[3] = {green, red, blue};

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    rmt_transmit(s_rmt_channel, s_encoder, grb, sizeof(grb), &tx_config);
    rmt_tx_wait_all_done(s_rmt_channel, portMAX_DELAY);
}

/**
 * @brief Turn off the LED
 */
static void led_off(void)
{
    led_set_color(0, 0, 0);
}

/**
 * @brief Flash the LED with specified color
 */
static void led_flash(uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms)
{
    led_set_color(red, green, blue);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    led_off();
}

/**
 * @brief Status LED task - monitors WiFi status and flashes LED accordingly
 */
static void status_led_task(void* arg)
{
    ESP_LOGI(TAG, "Status LED task started");
    
    for (;;) {
        // Sleep for the interval
        vTaskDelay(pdMS_TO_TICKS(LED_CONNECTED_INTERVAL_MS));
        
        // Flash the appropriate color
        if (s_wifi_connected) {
            led_flash(0, LED_BRIGHTNESS, 0, LED_FLASH_ON_MS);
        } else {
            led_flash(LED_BRIGHTNESS, 0, 0, LED_FLASH_ON_MS);
        }
    }
}

bool status_led_init(void)
{
    if (s_rmt_channel != NULL) {
        ESP_LOGW(TAG, "Status LED already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing status LED on GPIO %d", STATUS_LED_GPIO);
    
    // Configure RMT TX channel
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz, 100ns per tick
        .trans_queue_depth = 4,
    };
    
    esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, &s_rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Create WS2812 encoder
    ret = ws2812_encoder_create(&s_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WS2812 encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(s_rmt_channel);
        s_rmt_channel = NULL;
        return false;
    }
    
    // Enable RMT channel
    ret = rmt_enable(s_rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_rmt_channel);
        s_encoder = NULL;
        s_rmt_channel = NULL;
        return false;
    }
    
    // Flash blue briefly on startup to confirm LED is working
    led_set_color(0, 0, LED_BRIGHTNESS);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_off();
    
    // Start the status LED task on Core 0 (Core 1 is reserved for emulator)
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        status_led_task,
        "status_led",
        STATUS_LED_TASK_STACK_SIZE,
        NULL,
        STATUS_LED_TASK_PRIORITY,
        NULL,
        0  // Core 0
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status LED task");
        rmt_disable(s_rmt_channel);
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_rmt_channel);
        s_encoder = NULL;
        s_rmt_channel = NULL;
        return false;
    }
    
    ESP_LOGI(TAG, "Status LED initialized");
    return true;
}

void status_led_set_wifi_status(bool connected)
{
    bool previous = s_wifi_connected;
    s_wifi_connected = connected;
    
    if (previous != connected) {
        ESP_LOGI(TAG, "WiFi status changed: %s", connected ? "connected" : "disconnected");
    }
}


