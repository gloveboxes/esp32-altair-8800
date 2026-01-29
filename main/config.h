/**
 * @file config.h
 * @brief Configuration storage for WiFi and Remote FS settings
 *
 * Stores configuration data in ESP32 NVS (Non-Volatile Storage).
 * Compatible with captive_portal.c for WiFi credential management.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum lengths for configuration values
#define CONFIG_SSID_MAX_LEN     32
#define CONFIG_PASSWORD_MAX_LEN 64
#define CONFIG_RFS_IP_MAX_LEN   15  // "xxx.xxx.xxx.xxx"

/**
 * @brief Initialize the configuration module
 *
 * Must be called before any other config functions.
 * Initializes NVS if not already initialized.
 *
 * @return true if initialization successful, false otherwise
 */
bool config_init(void);

/**
 * @brief Check if WiFi credentials exist in storage
 *
 * @return true if credentials are stored, false otherwise
 */
bool config_exists(void);

/**
 * @brief Save WiFi and optional Remote FS configuration
 *
 * @param ssid WiFi SSID (required, max 32 chars)
 * @param password WiFi password (can be empty for open networks)
 * @param rfs_ip Remote FS server IP (optional, can be NULL)
 *
 * @return true if save successful, false otherwise
 */
bool config_save(const char* ssid, const char* password, const char* rfs_ip);

/**
 * @brief Get the stored WiFi SSID
 *
 * @return Pointer to SSID string, or NULL if not configured
 */
const char* config_get_ssid(void);

/**
 * @brief Get the stored WiFi password
 *
 * @return Pointer to password string, or NULL if not configured
 */
const char* config_get_password(void);

/**
 * @brief Get the stored Remote FS server IP
 *
 * @return Pointer to IP string, or NULL if not configured
 */
const char* config_get_rfs_ip(void);

/**
 * @brief Clear all stored configuration
 *
 * Erases WiFi credentials and Remote FS settings from storage.
 *
 * @return true if clear successful, false otherwise
 */
bool config_clear(void);

/**
 * @brief Get the device's unique ID as hex string
 *
 * @param buffer Buffer to store the ID (at least 17 bytes for 8-byte ID + null)
 * @param buffer_len Length of buffer
 *
 * @return true if successful, false otherwise
 */
bool config_get_device_id(char* buffer, size_t buffer_len);

/**
 * @brief Get the mDNS hostname for this device
 *
 * Format: "altair-XXXXXXXX" where XXXXXXXX is last 4 bytes of chip ID
 *
 * @return Pointer to hostname string (static buffer)
 */
const char* get_mdns_hostname(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_H
