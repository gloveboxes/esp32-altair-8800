/**
 * @file config.c
 * @brief Configuration storage using ESP32 NVS
 *
 * Stores WiFi credentials and Remote FS settings in non-volatile storage.
 */

#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "bt_keyboard.h"
#include "port_drivers/chat_io.h"
#include "wifi_setup.h"

#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// NVS namespace and keys
#define NVS_NAMESPACE   "altair_cfg"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_RFS_IP  "rfs_ip"
#define NVS_KEY_OPENAI  "openai_key"
#define NVS_KEY_CHAT_PROVIDER "chat_provider"
#define NVS_KEY_CHAT_ENDPOINT "chat_endpoint"

// Static storage for retrieved values
static char s_ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
static char s_password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
static char s_rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};
static char s_openai_key[CONFIG_OPENAI_KEY_MAX_LEN + 1] = {0};
static char s_chat_provider[CONFIG_CHAT_PROVIDER_MAX_LEN + 1] = {0};
static char s_chat_endpoint[CONFIG_CHAT_ENDPOINT_MAX_LEN + 1] = {0};
static char s_mdns_hostname[32] = {0};

static bool s_initialized = false;
static bool s_config_loaded = false;

static void config_serial_drain_line(uint32_t idle_timeout_ms)
{
    int64_t idle_start = esp_timer_get_time();
    while ((esp_timer_get_time() - idle_start) < ((int64_t)idle_timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (len <= 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            return;
        }
        idle_start = esp_timer_get_time();
    }
}

static int config_read_command_ms(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            return 0;
        }
        if (c == ' ' || c == '\t')
        {
            continue;
        }
        config_serial_drain_line(50);
        if (c >= 'a' && c <= 'z')
        {
            c = (uint8_t)(c - 'a' + 'A');
        }
        return c;
    }
    return -1;
}

static void config_print_boot_menu(void)
{
    printf("\nBoot configuration manager\n");
    printf("  1 - Bluetooth keyboard\n");
    printf("  2 - OpenAI / compatible chat endpoint\n");
    printf("  3 - WiFi credentials\n");
    printf("  Q - continue boot\n");
}

void config_run_boot_shell(void)
{
    if (!usb_serial_jtag_is_connected())
    {
        return;
    }

    printf("\nBoot configuration\n");
    bt_keyboard_print_status();
    printf("Press 'C' within 5 seconds to manage boot configuration.\n");
    printf("Press Enter to continue boot now.\n");

    int c = config_read_command_ms(5000);
    if (c == -1 || c == 0 || c != 'C')
    {
        return;
    }

    config_print_boot_menu();

    for (;;)
    {
        printf("config> ");
        int cmd = config_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("Boot configuration manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case '1':
            bt_keyboard_run_config_shell();
            config_print_boot_menu();
            break;

        case '2':
            chat_io_run_config_shell();
            config_print_boot_menu();
            break;

        case '3':
            wifi_setup_run_config_shell();
            config_print_boot_menu();
            break;

        case 'Q':
            printf("Leaving boot configuration manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use 1, 2, 3, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}

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

    // Load OpenAI key (optional)
    len = sizeof(s_openai_key);
    err = nvs_get_str(handle, NVS_KEY_OPENAI, s_openai_key, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_openai_key[0] = '\0';
    }

    len = sizeof(s_chat_provider);
    err = nvs_get_str(handle, NVS_KEY_CHAT_PROVIDER, s_chat_provider, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_chat_provider[0] = '\0';
    }

    len = sizeof(s_chat_endpoint);
    err = nvs_get_str(handle, NVS_KEY_CHAT_ENDPOINT, s_chat_endpoint, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_chat_endpoint[0] = '\0';
    }

    nvs_close(handle);
    s_config_loaded = true;

    printf("[Config] Loaded: SSID='%s', RFS_IP='%s'\n", s_ssid, 
           s_rfs_ip[0] ? s_rfs_ip : "(not set)");

    return true;
}

bool altair_config_init(void)
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
             "altair-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);

    // Try to load existing config
    config_load();

    return true;
}

bool altair_config_exists(void)
{
    if (!s_initialized) {
        altair_config_init();
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

bool altair_config_save(const char* ssid, const char* password, const char* rfs_ip)
{
    if (!s_initialized) {
        altair_config_init();
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

bool config_clear_wifi_settings(void)
{
    if (!s_initialized) {
        altair_config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to open NVS for WiFi clear: %s\n", esp_err_to_name(err));
        return false;
    }

    esp_err_t first_err = ESP_OK;
    const char *keys[] = { NVS_KEY_SSID, NVS_KEY_PASS, NVS_KEY_RFS_IP };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        err = nvs_erase_key(handle, keys[i]);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && first_err == ESP_OK) {
            first_err = err;
        }
    }

    if (first_err == ESP_OK) {
        first_err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (first_err != ESP_OK) {
        printf("[Config] Failed to clear WiFi settings: %s\n", esp_err_to_name(first_err));
        return false;
    }

    s_ssid[0] = '\0';
    s_password[0] = '\0';
    s_rfs_ip[0] = '\0';
    s_config_loaded = false;
    printf("[Config] WiFi settings cleared\n");
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

bool config_load_openai_key(char* key, size_t key_len)
{
    if (!key || key_len == 0) {
        return false;
    }

    key[0] = '\0';

    if (!s_initialized) {
        altair_config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len = key_len;
    err = nvs_get_str(handle, NVS_KEY_OPENAI, key, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        key[0] = '\0';
        return false;
    }

    key[key_len - 1] = '\0';
    strncpy(s_openai_key, key, CONFIG_OPENAI_KEY_MAX_LEN);
    s_openai_key[CONFIG_OPENAI_KEY_MAX_LEN] = '\0';
    return key[0] != '\0';
}

bool config_save_openai_key(const char* key)
{
    if (!s_initialized) {
        altair_config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to open NVS for OpenAI key: %s\n", esp_err_to_name(err));
        return false;
    }

    if (key && key[0] != '\0') {
        err = nvs_set_str(handle, NVS_KEY_OPENAI, key);
    } else {
        err = nvs_erase_key(handle, NVS_KEY_OPENAI);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        printf("[Config] Failed to save OpenAI key: %s\n", esp_err_to_name(err));
        return false;
    }

    if (key && key[0] != '\0') {
        strncpy(s_openai_key, key, CONFIG_OPENAI_KEY_MAX_LEN);
        s_openai_key[CONFIG_OPENAI_KEY_MAX_LEN] = '\0';
        printf("[Config] OpenAI API key saved\n");
    } else {
        s_openai_key[0] = '\0';
        printf("[Config] OpenAI API key cleared\n");
    }

    return true;
}

static bool config_nvs_get_string(const char *nvs_key, char *value, size_t value_len)
{
    if (!value || value_len == 0) {
        return false;
    }

    value[0] = '\0';

    if (!s_initialized) {
        altair_config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len = value_len;
    err = nvs_get_str(handle, nvs_key, value, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        value[0] = '\0';
        return false;
    }

    value[value_len - 1] = '\0';
    return value[0] != '\0';
}

bool config_load_chat_settings(char* provider, size_t provider_len,
                               char* endpoint, size_t endpoint_len,
                               char* key, size_t key_len)
{
    bool loaded = false;

    if (provider && provider_len > 0) {
        loaded |= config_nvs_get_string(NVS_KEY_CHAT_PROVIDER, provider, provider_len);
    }
    if (endpoint && endpoint_len > 0) {
        loaded |= config_nvs_get_string(NVS_KEY_CHAT_ENDPOINT, endpoint, endpoint_len);
    }
    if (key && key_len > 0) {
        loaded |= config_nvs_get_string(NVS_KEY_OPENAI, key, key_len);
    }

    return loaded;
}

static esp_err_t config_nvs_set_or_erase(nvs_handle_t handle, const char *nvs_key, const char *value)
{
    if (value && value[0] != '\0') {
        return nvs_set_str(handle, nvs_key, value);
    }

    esp_err_t err = nvs_erase_key(handle, nvs_key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

bool config_save_chat_settings(const char* provider, const char* endpoint,
                               const char* key)
{
    if (!s_initialized) {
        altair_config_init();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("[Config] Failed to open NVS for chat settings: %s\n", esp_err_to_name(err));
        return false;
    }

    err = config_nvs_set_or_erase(handle, NVS_KEY_CHAT_PROVIDER, provider);
    if (err == ESP_OK) {
        err = config_nvs_set_or_erase(handle, NVS_KEY_CHAT_ENDPOINT, endpoint);
    }
    if (err == ESP_OK) {
        err = config_nvs_set_or_erase(handle, NVS_KEY_OPENAI, key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        printf("[Config] Failed to save chat settings: %s\n", esp_err_to_name(err));
        return false;
    }

    strncpy(s_chat_provider, provider ? provider : "", CONFIG_CHAT_PROVIDER_MAX_LEN);
    s_chat_provider[CONFIG_CHAT_PROVIDER_MAX_LEN] = '\0';
    strncpy(s_chat_endpoint, endpoint ? endpoint : "", CONFIG_CHAT_ENDPOINT_MAX_LEN);
    s_chat_endpoint[CONFIG_CHAT_ENDPOINT_MAX_LEN] = '\0';
    strncpy(s_openai_key, key ? key : "", CONFIG_OPENAI_KEY_MAX_LEN);
    s_openai_key[CONFIG_OPENAI_KEY_MAX_LEN] = '\0';

    printf("[Config] Chat settings saved\n");
    return true;
}

bool altair_config_clear(void)
{
    if (!s_initialized) {
        altair_config_init();
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
    s_openai_key[0] = '\0';
    s_chat_provider[0] = '\0';
    s_chat_endpoint[0] = '\0';
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
        altair_config_init();
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
