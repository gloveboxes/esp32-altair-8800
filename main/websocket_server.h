/**
 * @file websocket_server.h
 * @brief WebSocket server for Altair 8800 terminal
 *
 * Provides HTTP server with WebSocket support using ESP-IDF's esp_http_server.
 * Serves the terminal HTML page and handles WebSocket connections.
 * 
 * Single-client model: Only one WebSocket client supported at a time.
 * New connections automatically kick existing clients.
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket server port
#define WEBSOCKET_SERVER_PORT   80

/**
 * @brief Start the WebSocket server
 *
 * Initializes and starts the HTTP server with WebSocket support.
 * Listens on WEBSOCKET_SERVER_PORT (8088).
 *
 * @return true if server started successfully, false otherwise
 */
bool websocket_server_start(void);

/**
 * @brief Stop the WebSocket server
 */
void websocket_server_stop(void);

/**
 * @brief Check if server is running
 *
 * @return true if server is running
 */
bool websocket_server_is_running(void);

/**
 * @brief Get number of connected WebSocket clients
 *
 * @return 1 if connected, 0 if not
 */
uint32_t websocket_server_get_client_count(void);

/**
 * @brief Send data to connected WebSocket client
 *
 * @param data Pointer to data to send
 * @param len Length of data
 * @return true if sent successfully
 */
bool websocket_server_broadcast(const uint8_t* data, size_t len);

/**
 * @brief Send PING to all clients for keepalive
 *
 * Used for connection liveness detection.
 */
void websocket_server_send_ping(void);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_SERVER_H
