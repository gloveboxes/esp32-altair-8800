/**
 * @file websocket_console.c
 * @brief WebSocket console implementation for Altair 8800
 *
 * Provides cross-core communication between WebSocket server (Core 0)
 * and Altair emulator (Core 1) using FreeRTOS queues.
 * Uses a dedicated low-priority task for TX to avoid blocking esp_timer.
 */

#include "websocket_console.h"
#include "websocket_server.h"
#include "terminal_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "WS_Console";

// Queue depth - sized for burst terminal output (e.g., screen clears, listings)
#define WS_TX_QUEUE_DEPTH   4096   // Output to WebSocket client - large for fast output

// Maximum bytes to batch in a single WebSocket send
// Under heavy output (WSPERF) the avg batch pinned at 1024 with 0 drops, so the
// batch size is the throughput lever here; raised to 2048 to fit more per send.
#define WS_TX_BATCH_SIZE    2048

// Timer interval for batched output (microseconds)
// 100ms lets more characters accumulate per frame so each WebSocket send
// carries more payload relative to its fixed header/round-trip overhead.
#define WS_TX_TIMER_INTERVAL_US  (100 * 1000)  // 100ms

// How long the emulator's terminal_write may block when the TX queue is full
// before falling back to dropping the oldest byte. This applies backpressure
// to the 8080 loop during heavy output bursts instead of silently dropping
// characters. Kept close to the batch timer (WS_TX_TIMER_INTERVAL_US) so a
// blocked writer wakes up roughly when the TX task next drains the queue.
#define WS_TX_ENQUEUE_TIMEOUT_MS  100

// Ping interval to keep WebSocket connections alive (microseconds)
#define WS_PING_INTERVAL_US  (30 * 1000 * 1000)  // 30 seconds

// How long to wait for the first WebSocket client before the emulator starts
// anyway (milliseconds). Prevents a headless boot from blocking forever.
#define WS_FIRST_CLIENT_TIMEOUT_MS  (20 * 1000)  // 20 seconds

// TX task stack size and priority (higher than other app tasks, still below esp_timer)
#define WS_TX_TASK_STACK    6144   // holds the WS_TX_BATCH_SIZE stack buffer plus headroom
#define WS_TX_TASK_PRIORITY 11     // Keep below esp_timer (22)
#define WS_TX_TASK_CORE     0      // Pin to Core 0 to avoid emulator core

// FreeRTOS queue for emulator -> WebSocket output (input is routed through the
// shared terminal_input queue so BLE keyboard and WebSocket clients share a
// single consumer in the emulator loop).
static QueueHandle_t s_tx_queue = NULL;  // Emulator -> WebSocket

// Semaphore to wake TX task
static SemaphoreHandle_t s_tx_sem = NULL;

// TX task handle
static TaskHandle_t s_tx_task = NULL;

// Signalled the first time a WebSocket client connects. Used by the emulator
// task (on non-VT100 builds) to defer starting the 8080 loop until a browser
// terminal is attached, so no boot output is lost. Binary semaphore: repeated
// gives are harmless and a give before the wait is latched.
static SemaphoreHandle_t s_first_client_sem = NULL;

// Timer for batched output (just signals the TX task)
static esp_timer_handle_t s_tx_timer = NULL;

// Timer for WebSocket keepalive pings
static esp_timer_handle_t s_ping_timer = NULL;

// Initialization flag
static bool s_initialized = false;

// Flag to signal TX task to send a ping (set by timer, cleared by TX task)
static volatile bool s_ping_pending = false;

/**
 * @brief Clear the TX queue
 */
static void clear_tx_queue(void)
{
    if (s_tx_queue) {
        uint8_t discard;
        while (xQueueReceive(s_tx_queue, &discard, 0) == pdTRUE) {
            // Drain queue
        }
    }
}

/**
 * @brief TX task - sends batched data to WebSocket client
 * 
 * Runs at lower priority than esp_timer to avoid starving system tasks.
 * Woken by timer or when queue has data.
 */
static void tx_task(void* arg)
{
    (void)arg;
    uint8_t buffer[WS_TX_BATCH_SIZE];

    while (1) {
        // Wait for timer signal or timeout (for periodic check)
        xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(WS_TX_TIMER_INTERVAL_US / 1000));
        
        if (!s_initialized || !s_tx_queue) {
            continue;
        }

        // Don't bother if no client
        if (!websocket_console_has_clients()) {
            clear_tx_queue();
            s_ping_pending = false;
            continue;
        }

        // Send ping if requested (from timer callback)
        // Do this from TX task to avoid racing with data sends
        if (s_ping_pending) {
            s_ping_pending = false;
            websocket_server_send_ping();
        }

        // Batch output for efficiency
        size_t count = 0;
        while (count < WS_TX_BATCH_SIZE) {
            if (xQueueReceive(s_tx_queue, &buffer[count], 0) == pdTRUE) {
                count++;
            } else {
                break;
            }
        }

        if (count > 0) {
            websocket_server_broadcast(buffer, count);

            // Yield to let other tasks run if we sent a full batch
            if (count == WS_TX_BATCH_SIZE) {
                taskYIELD();
            }
        }
    }
}

/**
 * @brief Timer callback - just signals TX task to wake up
 * 
 * Runs in esp_timer task but does minimal work (just gives semaphore).
 */
static void tx_timer_callback(void* arg)
{
    (void)arg;
    if (s_tx_sem) {
        xSemaphoreGive(s_tx_sem);
    }
}

/**
 * @brief Timer callback for WebSocket keepalive pings
 * 
 * Sets flag and wakes TX task to send ping (avoids racing with data sends).
 */
static void ping_timer_callback(void* arg)
{
    (void)arg;

    if (!s_initialized) {
        return;
    }

    if (websocket_console_has_clients()) {
        s_ping_pending = true;
        if (s_tx_sem) {
            xSemaphoreGive(s_tx_sem);  // Wake TX task to send ping
        }
    }
}

void websocket_console_init(void)
{
    if (s_initialized) {
        return;
    }

    // Create TX queue
    s_tx_queue = xQueueCreate(WS_TX_QUEUE_DEPTH, sizeof(uint8_t));

    if (!s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return;
    }

    // Create TX semaphore
    s_tx_sem = xSemaphoreCreateBinary();
    if (!s_tx_sem) {
        ESP_LOGE(TAG, "Failed to create TX semaphore");
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return;
    }

    // Create TX task at lower priority than esp_timer. Keep its stack in PSRAM
    // so BLE/WiFi/internal-DMA users do not starve internal RAM during boot.
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(tx_task, "ws_tx", WS_TX_TASK_STACK, NULL,
                                                     WS_TX_TASK_PRIORITY, &s_tx_task,
                                                     WS_TX_TASK_CORE,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create TX task in PSRAM; trying internal RAM");
        ret = xTaskCreatePinnedToCore(tx_task, "ws_tx", WS_TX_TASK_STACK, NULL,
                                      WS_TX_TASK_PRIORITY, &s_tx_task,
                                      WS_TX_TASK_CORE);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task (free internal=%lu largest internal=%lu free psram=%lu largest psram=%lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        vSemaphoreDelete(s_tx_sem);
        vQueueDelete(s_tx_queue);
        s_tx_sem = NULL;
        s_tx_queue = NULL;
        return;
    }

    // Create TX batching timer (just signals the task)
    const esp_timer_create_args_t timer_args = {
        .callback = tx_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_tx_timer"
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_tx_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX timer: %s", esp_err_to_name(err));
        vTaskDelete(s_tx_task);
        vSemaphoreDelete(s_tx_sem);
        vQueueDelete(s_tx_queue);
        s_tx_task = NULL;
        s_tx_sem = NULL;
        s_tx_queue = NULL;
        return;
    }

    // Create ping timer
    const esp_timer_create_args_t ping_timer_args = {
        .callback = ping_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_ping_timer"
    };

    err = esp_timer_create(&ping_timer_args, &s_ping_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_tx_timer);
        vTaskDelete(s_tx_task);
        vSemaphoreDelete(s_tx_sem);
        vQueueDelete(s_tx_queue);
        s_tx_timer = NULL;
        s_tx_task = NULL;
        s_tx_sem = NULL;
        s_tx_queue = NULL;
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Console initialized (TX=%d, timer=%dms, task_prio=%d)",
             WS_TX_QUEUE_DEPTH, WS_TX_TIMER_INTERVAL_US / 1000,
             WS_TX_TASK_PRIORITY);
}

bool websocket_console_start_server(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Console not initialized");
        return false;
    }

    if (!websocket_server_start()) {
        return false;
    }

    // Start the TX batching timer
    if (s_tx_timer) {
        esp_err_t err = esp_timer_start_periodic(s_tx_timer, WS_TX_TIMER_INTERVAL_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TX timer: %s", esp_err_to_name(err));
            websocket_server_stop();
            return false;
        }
        ESP_LOGI(TAG, "TX batching timer started (%dms interval)", WS_TX_TIMER_INTERVAL_US / 1000);
    }

    // Start the ping timer
    if (s_ping_timer) {
        esp_err_t err = esp_timer_start_periodic(s_ping_timer, WS_PING_INTERVAL_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start ping timer: %s", esp_err_to_name(err));
            // Non-fatal - continue without pings
        } else {
            ESP_LOGI(TAG, "Ping timer started (%ds interval)", WS_PING_INTERVAL_US / 1000000);
        }
    }

    return true;
}

bool websocket_console_has_clients(void)
{
    return websocket_server_get_client_count() > 0;
}

void websocket_console_enqueue_output(uint8_t value)
{
    bool queued;

    if (!s_initialized || !s_tx_queue) {
        return;
    }

    // No browser terminal is connected. This is a normal idle state; stale
    // queued output is cleared by connection callbacks and the TX task.
    if (!websocket_console_has_clients()) {
        return;
    }

    // Block for up to WS_TX_ENQUEUE_TIMEOUT_MS so a burst of emulator output
    // (e.g. WSPERF flooding the terminal) applies backpressure to the 8080
    // loop instead of silently dropping characters. Only after the timeout
    // do we give up and drop the oldest byte to make room for the newest.
    queued = xQueueSend(s_tx_queue, &value,
                        pdMS_TO_TICKS(WS_TX_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    if (!queued) {
        // Still full after waiting - drop oldest and try again immediately.
        uint8_t discard;
        xQueueReceive(s_tx_queue, &discard, 0);
        queued = xQueueSend(s_tx_queue, &value, 0) == pdTRUE;
    }

    (void)queued;
}

void websocket_console_clear_queues(void)
{
    // Only the TX queue is owned by this module; input is queued into the
    // shared terminal_input queue and must not be flushed here (doing so
    // would drop bytes typed on the BLE keyboard).
    clear_tx_queue();
}

/**
 * @brief Handle incoming WebSocket data (called from WebSocket server)
 *
 * This function is called by the WebSocket server when data is received
 * from a client. Bytes are pushed into the shared terminal_input queue so
 * the emulator on Core 1 sees them alongside BLE keyboard input.
 *
 * @param data Pointer to received data
 * @param len Length of received data
 */
void websocket_console_handle_rx(const uint8_t* data, size_t len)
{
    if (!s_initialized || !data || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];

        // Convert newline to carriage return (terminal convention)
        if (ch == '\n') {
            ch = '\r';
        }

        terminal_input_enqueue(ch);
    }
}

void websocket_console_first_client_signal_init(void)
{
    if (s_first_client_sem == NULL) {
        s_first_client_sem = xSemaphoreCreateBinary();
        if (s_first_client_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create first-client semaphore");
        }
    }
}

void websocket_console_wait_for_first_client(void)
{
    if (s_first_client_sem == NULL) {
        // No signal created (e.g. headless build); don't block.
        return;
    }
    // Wait up to WS_FIRST_CLIENT_TIMEOUT_MS for the first client. If no browser
    // terminal attaches in that window, fall through so the emulator still starts.
    xSemaphoreTake(s_first_client_sem, pdMS_TO_TICKS(WS_FIRST_CLIENT_TIMEOUT_MS));
}

/**
 * @brief Handle client connect event
 */
void websocket_console_on_connect(void)
{
    // Clear any stale data when client connects
    clear_tx_queue();

    // Unblock the emulator task if it is waiting for the first client.
    if (s_first_client_sem != NULL) {
        xSemaphoreGive(s_first_client_sem);
    }
}

/**
 * @brief Handle client disconnect event
 */
void websocket_console_on_disconnect(void)
{
    // Drain any stale outbound data; inbound bytes are owned by the shared
    // terminal_input queue and are intentionally left alone.
    websocket_console_clear_queues();
}
