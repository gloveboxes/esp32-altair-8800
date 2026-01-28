/**
 * @file websocket_console.h
 * @brief WebSocket console for Altair 8800 terminal I/O
 *
 * Provides cross-core communication between the WebSocket server (Core 0)
 * and the Altair emulator (Core 1) using FreeRTOS queues.
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
 * @brief Check if any WebSocket clients are connected
 *
 * @return true if at least one client is connected
 */
bool websocket_console_has_clients(void);

/**
 * @brief Get number of connected WebSocket clients
 *
 * @return Number of active WebSocket connections
 */
uint32_t websocket_console_get_client_count(void);

/**
 * @brief Enqueue a byte for transmission to WebSocket clients
 *
 * Called from the emulator (Core 1) to send terminal output.
 * Non-blocking - drops data if no clients connected or queue full.
 *
 * @param value Byte to transmit
 */
void websocket_console_enqueue_output(uint8_t value);

/**
 * @brief Try to dequeue a byte from WebSocket input
 *
 * Called from the emulator (Core 1) to receive terminal input.
 * Non-blocking.
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
