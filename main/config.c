/**
 * @file config.c
 * @brief Configuration storage using ESP32 NVS
 *
 * Stores WiFi credentials and Remote FS settings in non-volatile storage.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

// NVS namespace and keys
#define NVS_NAMESPACE   "altair_cfg"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_RFS_IP  "rfs_ip"

// Static storage for retrieved values
static char s_ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
static char s_password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
static char s_rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};
static char s_mdns_hostname[32] = {0};

static bool s_initialized = false;
static bool s_config_loaded = false;

/**
 * @brief Load configuration from NVS into static buffers
 */
static bool config_load(void)
{
    if (s_config_loaded) {
        return true;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // No config exists yet
        return false;
    }

    size_t len;

    // Load SSID
    len = sizeof(s_ssid);
    err = nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    // Load password (may be empty for open networks)
    len = sizeof(s_password);
    err = nvs_get_str(handle, NVS_KEY_PASS, s_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return false;
    }

    // Load RFS IP (optional)
    len = sizeof(s_rfs_ip);
    err = nvs_get_str(handle, NVS_KEY_RFS_IP, s_rfs_ip, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_rfs_ip[0] = '\0';  // Not configured, which is fine
    }

    nvs_close(handle);
    s_config_loaded = true;

    printf("[Config] Loaded: SSID='%s', RFS_IP='%s'\n", s_ssid, 
           s_rfs_ip[0] ? s_rfs_ip : "(not set)");

    return true;
}

bool config_init(void)
{
    if (s_initialized) {
        return true;
    }

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or new version, erase and retry
        printf("[Config] Erasing NVS flash...\n");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        printf("[Config] NVS init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    s_initialized = true;
    printf("[Config] NVS initialized\n");

    // Generate mDNS hostname from chip ID
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_mdns_hostname, sizeof(s_mdns_hostname), 
             "altair-8800-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);

    // Try to load existing config
    config_load();

    return true;
}

bool config_exists(void)
{
    if (!s_initialized) {
        config_init();
    }

    // Check if SSID exists in NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len = 0;
    err = nvs_get_str(handle, NVS_KEY_SSID, NULL, &len);
    nvs_close(handle);

    return (err == ESP_OK && len > 1);  // len includes null terminator
}

bool config_save(const char* ssid, const char* password, const char* rfs_ip)
{
    if (!s_initialized) {
        config_init();
    }

    if (!ssid || ssid[0] == '\0') {
        printf("[Config] Error: SSID cannot be empty\n");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to open NVS for writing: %s\n", esp_err_to_name(err));
        return false;
    }

    // Save SSID
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        printf("[Config] Failed to save SSID: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    // Save password (even if empty)
    err = nvs_set_str(handle, NVS_KEY_PASS, password ? password : "");
    if (err != ESP_OK) {
        printf("[Config] Failed to save password: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    // Save RFS IP if provided
    if (rfs_ip && rfs_ip[0] != '\0') {
        err = nvs_set_str(handle, NVS_KEY_RFS_IP, rfs_ip);
        if (err != ESP_OK) {
            printf("[Config] Failed to save RFS IP: %s\n", esp_err_to_name(err));
            nvs_close(handle);
            return false;
        }
    } else {
        // Clear RFS IP if not provided
        nvs_erase_key(handle, NVS_KEY_RFS_IP);
    }

    // Commit changes
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        printf("[Config] Failed to commit NVS: %s\n", esp_err_to_name(err));
        return false;
    }

    // Update local cache
    strncpy(s_ssid, ssid, CONFIG_SSID_MAX_LEN);
    s_ssid[CONFIG_SSID_MAX_LEN] = '\0';
    
    strncpy(s_password, password ? password : "", CONFIG_PASSWORD_MAX_LEN);
    s_password[CONFIG_PASSWORD_MAX_LEN] = '\0';
    
    if (rfs_ip && rfs_ip[0] != '\0') {
        strncpy(s_rfs_ip, rfs_ip, CONFIG_RFS_IP_MAX_LEN);
        s_rfs_ip[CONFIG_RFS_IP_MAX_LEN] = '\0';
    } else {
        s_rfs_ip[0] = '\0';
    }

    s_config_loaded = true;
    printf("[Config] Configuration saved successfully\n");

    return true;
}

const char* config_get_ssid(void)
{
    if (!s_config_loaded) {
        config_load();
    }
    return s_ssid[0] ? s_ssid : NULL;
}

const char* config_get_password(void)
{
    if (!s_config_loaded) {
        config_load();
    }
    return s_password[0] ? s_password : NULL;
}

const char* config_get_rfs_ip(void)
{
    if (!s_config_loaded) {
        config_load();
    }
    return s_rfs_ip[0] ? s_rfs_ip : NULL;
}

bool config_clear(void)
{
    if (!s_initialized) {
        config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to open NVS for clearing: %s\n", esp_err_to_name(err));
        return false;
    }

    // Erase all keys in our namespace
    err = nvs_erase_all(handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to erase NVS: %s\n", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        printf("[Config] Failed to commit NVS clear: %s\n", esp_err_to_name(err));
        return false;
    }

    // Clear local cache
    s_ssid[0] = '\0';
    s_password[0] = '\0';
    s_rfs_ip[0] = '\0';
    s_config_loaded = false;

    printf("[Config] Configuration cleared\n");
    return true;
}

bool config_get_device_id(char* buffer, size_t buffer_len)
{
    if (!buffer || buffer_len < 17) {
        return false;
    }

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    snprintf(buffer, buffer_len, "%02x%02x%02x%02x%02x%02x%02x%02x",
             0, 0, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return true;
}

const char* get_mdns_hostname(void)
{
    if (!s_initialized) {
        config_init();
    }
    return s_mdns_hostname;
}

/**
 * @brief Get the connected WiFi SSID (alias for config_get_ssid)
 *
 * Provided for compatibility with the Pico W codebase.
 */
const char* get_connected_ssid(void)
{
    return config_get_ssid();
}
