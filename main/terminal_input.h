/**
 * @file terminal_input.h
 * @brief Shared input queue for the emulated Altair terminal.
 *
 * The BLE keyboard task and the WebSocket RX handler enqueue bytes here;
 * the emulator loop on Core 1 dequeues them via terminal_input_try_dequeue().
 * USB Serial JTAG is read directly from its driver buffer and does not pass
 * through this queue.
 */

#ifndef TERMINAL_INPUT_H
#define TERMINAL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the shared terminal input queue.
 *
 * Must be called from app_main() before any producer (BLE keyboard, WebSocket
 * server) is started. Subsequent calls are no-ops.
 */
void terminal_input_init(void);

/**
 * @brief Enqueue a byte from a producer (BLE keyboard, WebSocket, ...).
 *
 * Non-blocking. If the queue is full, the oldest byte is discarded and the
 * new byte is enqueued in its place. Safe to call from any task context.
 */
void terminal_input_enqueue(uint8_t value);

/**
 * @brief Try to dequeue a byte for the emulator.
 *
 * @return true if a byte was available, false otherwise.
 */
bool terminal_input_try_dequeue(uint8_t *value);

#ifdef __cplusplus
}
#endif

#endif // TERMINAL_INPUT_H
