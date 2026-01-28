/**
 * @file captive_portal.h
 * @brief WiFi AP mode captive portal for configuration - ESP32 version
 *
 * When the Altair emulator cannot connect to a configured WiFi network,
 * it falls back to AP mode and serves a configuration page.
 */

#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AP mode configuration
#define CAPTIVE_PORTAL_AP_SSID   "Altair8800-Setup"
#define CAPTIVE_PORTAL_AP_CHANNEL 6

// AP mode IP configuration (192.168.4.x subnet)
#define CAPTIVE_PORTAL_IP_ADDR   "192.168.4.1"
#define CAPTIVE_PORTAL_NETMASK   "255.255.255.0"
#define CAPTIVE_PORTAL_GW_ADDR   "192.168.4.1"

// HTTP server port
#define CAPTIVE_PORTAL_HTTP_PORT 80

// DNS server port
#define CAPTIVE_PORTAL_DNS_PORT  53

/**
 * @brief Initialize and start the captive portal in AP mode
 *
 * This function:
 * 1. Enables WiFi AP mode with SSID "Altair8800-Setup"
 * 2. Configures static IP (192.168.4.1)
 * 3. Starts DNS server (redirects all queries to AP IP)
 * 4. Starts HTTP server to serve config page
 *
 * Note: ESP32's built-in DHCP server is used instead of custom implementation.
 *
 * @return true if portal started successfully, false otherwise
 */
bool captive_portal_start(void);

/**
 * @brief Stop the captive portal and disable AP mode
 */
void captive_portal_stop(void);

/**
 * @brief Check if captive portal is currently running
 * @return true if running in AP mode, false otherwise
 */
bool captive_portal_is_running(void);

/**
 * @brief Poll function - must be called regularly from main loop
 *
 * Handles pending reboot after configuration save.
 * Note: HTTP and DNS servers use ESP-IDF's async handling.
 */
void captive_portal_poll(void);

/**
 * @brief Get the AP IP address string
 * @return Static IP address string (e.g., "192.168.4.1")
 */
const char* captive_portal_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif // CAPTIVE_PORTAL_H
