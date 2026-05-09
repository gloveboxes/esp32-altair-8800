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
 * Uses internal timeout suitable for WPA3-SAE networks.
 *
 * @return WiFi result code
 */
wifi_result_t wifi_connect(void);

/**
 * @brief Disconnect from WiFi
 */
void wifi_disconnect(void);

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
