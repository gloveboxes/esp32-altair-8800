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

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "WS_Console";

// Queue depths - sized for burst terminal I/O (e.g., screen clears, listings)
#define WS_RX_QUEUE_DEPTH   128    // Input from WebSocket client
#define WS_TX_QUEUE_DEPTH   4096   // Output to WebSocket client - large for fast output

// Maximum bytes to batch in a single WebSocket send
#define WS_TX_BATCH_SIZE    512

// Timer interval for batched output (microseconds)
// 10ms for high throughput while still batching efficiently
#define WS_TX_TIMER_INTERVAL_US  (10 * 1000)  // 10ms

// Ping interval to keep WebSocket connections alive (microseconds)
#define WS_PING_INTERVAL_US  (30 * 1000 * 1000)  // 30 seconds

// TX task stack size and priority (lower than esp_timer to avoid starving system)
#define WS_TX_TASK_STACK    4096
#define WS_TX_TASK_PRIORITY 5      // Lower than default (esp_timer is 22)

// FreeRTOS queues for cross-core communication
static QueueHandle_t s_rx_queue = NULL;  // WebSocket -> Emulator
static QueueHandle_t s_tx_queue = NULL;  // Emulator -> WebSocket

// Semaphore to wake TX task
static SemaphoreHandle_t s_tx_sem = NULL;

// TX task handle
static TaskHandle_t s_tx_task = NULL;

// Timer for batched output (just signals the TX task)
static esp_timer_handle_t s_tx_timer = NULL;

// Timer for WebSocket keepalive pings
static esp_timer_handle_t s_ping_timer = NULL;

// Initialization flag
static bool s_initialized = false;

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
 * @brief Clear the RX queue
 */
static void clear_rx_queue(void)
{
    if (s_rx_queue) {
        uint8_t discard;
        while (xQueueReceive(s_rx_queue, &discard, 0) == pdTRUE) {
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
        xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(20));
        
        if (!s_initialized || !s_tx_queue) {
            continue;
        }

        // Don't bother if no client
        if (!websocket_console_has_clients()) {
            clear_tx_queue();
            continue;
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
 */
static void ping_timer_callback(void* arg)
{
    (void)arg;

    if (!s_initialized) {
        return;
    }

    if (websocket_console_has_clients()) {
        websocket_server_send_ping();
    }
}

void websocket_console_init(void)
{
    if (s_initialized) {
        return;
    }

    // Create queues
    s_rx_queue = xQueueCreate(WS_RX_QUEUE_DEPTH, sizeof(uint8_t));
    s_tx_queue = xQueueCreate(WS_TX_QUEUE_DEPTH, sizeof(uint8_t));

    if (!s_rx_queue || !s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        if (s_rx_queue) vQueueDelete(s_rx_queue);
        if (s_tx_queue) vQueueDelete(s_tx_queue);
        s_rx_queue = NULL;
        s_tx_queue = NULL;
        return;
    }

    // Create TX semaphore
    s_tx_sem = xSemaphoreCreateBinary();
    if (!s_tx_sem) {
        ESP_LOGE(TAG, "Failed to create TX semaphore");
        vQueueDelete(s_rx_queue);
        vQueueDelete(s_tx_queue);
        s_rx_queue = NULL;
        s_tx_queue = NULL;
        return;
    }

    // Create TX task at lower priority than esp_timer
    BaseType_t ret = xTaskCreate(tx_task, "ws_tx", WS_TX_TASK_STACK, NULL,
                                  WS_TX_TASK_PRIORITY, &s_tx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        vSemaphoreDelete(s_tx_sem);
        vQueueDelete(s_rx_queue);
        vQueueDelete(s_tx_queue);
        s_tx_sem = NULL;
        s_rx_queue = NULL;
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
        vQueueDelete(s_rx_queue);
        vQueueDelete(s_tx_queue);
        s_tx_task = NULL;
        s_tx_sem = NULL;
        s_rx_queue = NULL;
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
        vQueueDelete(s_rx_queue);
        vQueueDelete(s_tx_queue);
        s_tx_timer = NULL;
        s_tx_task = NULL;
        s_tx_sem = NULL;
        s_rx_queue = NULL;
        s_tx_queue = NULL;
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Console initialized (RX=%d, TX=%d, timer=%dms, task_prio=%d)",
             WS_RX_QUEUE_DEPTH, WS_TX_QUEUE_DEPTH, WS_TX_TIMER_INTERVAL_US / 1000,
             WS_TX_TASK_PRIORITY);
}

bool websocket_console_start_server(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Console not initialized");
        return false;
    }

    // Start the TX batching timer
    if (s_tx_timer) {
        esp_err_t err = esp_timer_start_periodic(s_tx_timer, WS_TX_TIMER_INTERVAL_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TX timer: %s", esp_err_to_name(err));
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

    return websocket_server_start();
}

void websocket_console_stop_server(void)
{
    // Stop the TX batching timer
    if (s_tx_timer) {
        esp_timer_stop(s_tx_timer);
    }

    // Stop the ping timer
    if (s_ping_timer) {
        esp_timer_stop(s_ping_timer);
    }

    websocket_server_stop();
}

bool websocket_console_has_clients(void)
{
    return websocket_server_get_client_count() > 0;
}

void websocket_console_enqueue_output(uint8_t value)
{
    if (!s_initialized || !s_tx_queue) {
        return;
    }

    // If no client connected, clear the queue to prevent accumulation
    if (!websocket_console_has_clients()) {
        clear_tx_queue();
        return;
    }

    // Non-blocking enqueue - drop if queue full (real-time data)
    if (xQueueSend(s_tx_queue, &value, 0) != pdTRUE) {
        // Queue full - drop oldest and try again
        uint8_t discard;
        xQueueReceive(s_tx_queue, &discard, 0);
        xQueueSend(s_tx_queue, &value, 0);
    }
}

bool websocket_console_try_dequeue_input(uint8_t* value)
{
    if (!s_initialized || !s_rx_queue || !value) {
        return false;
    }

    return xQueueReceive(s_rx_queue, value, 0) == pdTRUE;
}

void websocket_console_clear_queues(void)
{
    clear_tx_queue();
    clear_rx_queue();
}

/**
 * @brief Handle incoming WebSocket data (called from WebSocket server)
 *
 * This function is called by the WebSocket server when data is received
 * from a client. It queues the data for the emulator to process.
 *
 * @param data Pointer to received data
 * @param len Length of received data
 */
void websocket_console_handle_rx(const uint8_t* data, size_t len)
{
    if (!s_initialized || !s_rx_queue || !data || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];

        // Convert newline to carriage return (terminal convention)
        if (ch == '\n') {
            ch = '\r';
        }

        // Non-blocking enqueue
        if (xQueueSend(s_rx_queue, &ch, 0) != pdTRUE) {
            // Queue full - drop oldest and try again
            uint8_t discard;
            xQueueReceive(s_rx_queue, &discard, 0);
            xQueueSend(s_rx_queue, &ch, 0);
        }
    }
}

/**
 * @brief Handle client connect event
 */
void websocket_console_on_connect(void)
{
    // Clear any stale data when client connects
    clear_tx_queue();
}

/**
 * @brief Handle client disconnect event
 */
void websocket_console_on_disconnect(void)
{
    // Clear queues when client disconnects
    websocket_console_clear_queues();
}
