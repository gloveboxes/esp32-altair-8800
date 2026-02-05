/**
 * @file websocket_console.h
 * @brief WebSocket console for Altair 8800 terminal I/O
 *
 * Provides cross-core communication between the WebSocket server (Core 0)
 * and the Altair emulator (Core 1) using FreeRTOS queues.
 * Single-client model: only one WebSocket client at a time.
 */

#ifndef WEBSOCKET_CONSOLE_H
#define WEBSOCKET_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WebSocket console queues
 *
 * Must be called before starting the WebSocket server or emulator.
 * Sets up TX and RX queues for cross-core communication.
 */
void websocket_console_init(void);

/**
 * @brief Start the WebSocket server
 *
 * Initializes and starts the HTTP server with WebSocket support.
 * Also serves the terminal HTML page.
 *
 * @return true if server started successfully, false otherwise
 */
bool websocket_console_start_server(void);

/**
 * @brief Stop the WebSocket server
 */
void websocket_console_stop_server(void);

/**
 * @brief Check if a WebSocket client is connected
 *
 * @return true if a client is connected
 */
bool websocket_console_has_clients(void);

/**
 * @brief Enqueue a byte for transmission to WebSocket client
 *
 * Called from the emulator (Core 1) to send terminal output.
 * Non-blocking - drops data if no client connected or queue full.
 *
 * @param value Byte to transmit
 */
void websocket_console_enqueue_output(uint8_t value);

/**
 * @brief Try to dequeue a byte from WebSocket input
 *
 * Called from the emulator (Core 1) to receive terminal input.
 * Non-blocking. Emulator loop routes to emulator or monitor based on CPU state.
 *
 * @param value Pointer to store the received byte
 * @return true if a byte was available, false if queue empty
 */
bool websocket_console_try_dequeue_input(uint8_t* value);

/**
 * @brief Clear all console queues
 *
 * Removes all pending data from TX and RX queues.
 */
void websocket_console_clear_queues(void);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_CONSOLE_H
