/**
 * @file wifi.h
 * @brief WiFi management for ESP32
 *
 * Handles WiFi station mode connection with stored credentials.
 */

#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection result codes
 */
typedef enum {
    WIFI_RESULT_OK = 0,           // Connected successfully
    WIFI_RESULT_NO_CREDENTIALS,   // No stored credentials
    WIFI_RESULT_CONNECT_FAILED,   // Failed to connect
    WIFI_RESULT_TIMEOUT,          // Connection timed out
    WIFI_RESULT_NOT_INITIALIZED,  // WiFi not initialized
} wifi_result_t;

/**
 * @brief Initialize the WiFi subsystem
 *
 * Initializes ESP32 WiFi driver, event loop, and netif.
 * Must be called before any other WiFi functions.
 *
 * @return true if initialization successful, false otherwise
 */
bool wifi_init(void);

/**
 * @brief Connect to WiFi using stored credentials
 *
 * Attempts to connect to the WiFi network configured via config module.
 * This is a blocking call that waits for connection or timeout.
 *
 * @param timeout_ms Maximum time to wait for connection (0 = default 15s)
 * @return WiFi result code
 */
wifi_result_t wifi_connect(uint32_t timeout_ms);

/**
 * @brief Disconnect from WiFi
 */
void wifi_disconnect(void);

/**
 * @brief Check if WiFi hardware is ready
 *
 * @return true if WiFi driver is initialized
 */
bool wifi_is_ready(void);

/**
 * @brief Check if connected to WiFi network
 *
 * @return true if connected and has IP address
 */
bool wifi_is_connected(void);

/**
 * @brief Get the current IP address
 *
 * @param buffer Buffer to store IP string (at least 16 bytes)
 * @param length Size of buffer
 * @return true if IP address was copied, false if not connected
 */
bool wifi_get_ip(char* buffer, size_t length);

/**
 * @brief Get the raw IP address as 32-bit integer
 *
 * @return IP address in network byte order, or 0 if not connected
 */
uint32_t wifi_get_ip_raw(void);

/**
 * @brief Set WiFi ready state (internal use)
 */
void wifi_set_ready(bool ready);

/**
 * @brief Set WiFi connected state (internal use)
 */
void wifi_set_connected(bool connected);

/**
 * @brief Set cached IP address (internal use)
 */
void wifi_set_ip_address(const char* ip);

/**
 * @brief Get cached IP address string
 *
 * @return IP address string or NULL if not available
 */
const char* wifi_get_ip_address(void);

/**
 * @brief Start WiFi in AP mode for captive portal
 *
 * @param ssid AP network name
 * @param password AP password (NULL for open network)
 * @return true if AP started successfully
 */
bool wifi_start_ap(const char* ssid, const char* password);

/**
 * @brief Stop WiFi AP mode
 */
void wifi_stop_ap(void);

/**
 * @brief Check if running in AP mode
 */
bool wifi_is_ap_mode(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H
