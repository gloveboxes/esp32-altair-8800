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
#define CONFIG_OPENAI_KEY_MAX_LEN 192
#define CONFIG_CHAT_PROVIDER_MAX_LEN 16
#define CONFIG_CHAT_ENDPOINT_MAX_LEN 160

/**
 * @brief Initialize the configuration module
 *
 * Must be called before any other config functions.
 * Initializes NVS if not already initialized.
 *
 * @return true if initialization successful, false otherwise
 */
bool altair_config_init(void);

/**
 * @brief Check if WiFi credentials exist in storage
 *
 * @return true if credentials are stored, false otherwise
 */
bool altair_config_exists(void);

/**
 * @brief Save WiFi and optional Remote FS configuration
 *
 * @param ssid WiFi SSID (required, max 32 chars)
 * @param password WiFi password (can be empty for open networks)
 * @param rfs_ip Remote FS server IP (optional, can be NULL)
 *
 * @return true if save successful, false otherwise
 */
bool altair_config_save(const char* ssid, const char* password, const char* rfs_ip);

/**
 * @brief Clear stored WiFi and Remote FS settings only
 *
 * Leaves chat provider/API-key settings intact.
 *
 * @return true if clear successful, false otherwise
 */
bool config_clear_wifi_settings(void);

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
 * @brief Load the stored OpenAI API key
 *
 * @param key Destination buffer
 * @param key_len Destination buffer length
 * @return true if a non-empty key was loaded, false otherwise
 */
bool config_load_openai_key(char* key, size_t key_len);

/**
 * @brief Save or clear the OpenAI API key
 *
 * Passing NULL or an empty string clears the stored key.
 *
 * @param key OpenAI API key to save
 * @return true if save successful, false otherwise
 */
bool config_save_openai_key(const char* key);

/**
 * @brief Load stored chat provider settings
 *
 * @return true if at least one chat setting was loaded, false otherwise
 */
bool config_load_chat_settings(char* provider, size_t provider_len,
							   char* endpoint, size_t endpoint_len,
							   char* key, size_t key_len);

/**
 * @brief Save chat provider settings
 *
 * Empty endpoint/model values clear those settings. Empty key clears the key.
 */
bool config_save_chat_settings(const char* provider, const char* endpoint,
							   const char* key);

/**
 * @brief Run the boot-time serial configuration shell
 *
 * Waits 5 seconds for the user to press C, then offers Bluetooth and chat
 * provider setup submenus. Safe to call once during app_main().
 */
void config_run_boot_shell(void);

/**
 * @brief Clear all stored configuration
 *
 * Erases WiFi credentials and Remote FS settings from storage.
 *
 * @return true if clear successful, false otherwise
 */
bool altair_config_clear(void);

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
