/**
 * @file terminal_input.c
 * @brief Shared FreeRTOS queue used by BLE keyboard and WebSocket producers.
 */

#include "terminal_input.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Sized for burst paste / fast typing from either source. 128 bytes matches
// the previous per-source depths and is plenty for sparse human input.
#define TERMINAL_INPUT_QUEUE_DEPTH 128

static QueueHandle_t s_queue = NULL;

void terminal_input_init(void)
{
    if (s_queue != NULL)
    {
        return;
    }
    s_queue = xQueueCreate(TERMINAL_INPUT_QUEUE_DEPTH, sizeof(uint8_t));
}

void terminal_input_enqueue(uint8_t value)
{
    if (s_queue == NULL)
    {
        return;
    }

    if (xQueueSend(s_queue, &value, 0) != pdTRUE)
    {
        // Queue full - drop oldest and try again so the most recent
        // keystrokes always make it through.
        uint8_t discard;
        if (xQueueReceive(s_queue, &discard, 0) == pdTRUE)
        {
            xQueueSend(s_queue, &value, 0);
        }
    }
}

bool terminal_input_try_dequeue(uint8_t *value)
{
    if (s_queue == NULL || value == NULL)
    {
        return false;
    }
    return xQueueReceive(s_queue, value, 0) == pdTRUE;
}
