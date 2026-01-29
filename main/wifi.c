/**
 * @file wifi.c
 * @brief WiFi management for ESP32
 *
 * Handles WiFi station and AP mode connections using ESP-IDF WiFi driver.
 */

#include "wifi.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

static const char* TAG = "WiFi";

// Event bits for WiFi connection state machine
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Default connection timeout
#define DEFAULT_WIFI_TIMEOUT_MS  15000

// Maximum connection retry attempts
#define WIFI_MAX_RETRY  5

// State variables
static bool s_wifi_initialized = false;
static bool s_wifi_connected = false;
static bool s_wifi_ap_mode = false;
static char s_ip_address[16] = {0};
static uint32_t s_ip_raw = 0;
static int s_retry_count = 0;

// FreeRTOS event group for connection synchronization
static EventGroupHandle_t s_wifi_event_group = NULL;

// Network interfaces
static esp_netif_t* s_sta_netif = NULL;
static esp_netif_t* s_ap_netif = NULL;

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Station started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* event = 
                    (wifi_event_sta_disconnected_t*)event_data;
                ESP_LOGW(TAG, "Disconnected from AP, reason: %d", event->reason);
                
                s_wifi_connected = false;
                s_ip_address[0] = '\0';
                s_ip_raw = 0;

                if (s_retry_count < WIFI_MAX_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)...", 
                             s_retry_count, WIFI_MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "Connection failed after %d retries", WIFI_MAX_RETRY);
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                s_wifi_ap_mode = true;
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                s_wifi_ap_mode = false;
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = 
                    (wifi_event_ap_staconnected_t*)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " connected to AP, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = 
                    (wifi_event_ap_stadisconnected_t*)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " disconnected from AP, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
                s_ip_raw = event->ip_info.ip.addr;
                snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                         IP2STR(&event->ip_info.ip));
                ESP_LOGI(TAG, "Got IP address: %s", s_ip_address);
                
                // Disable WiFi power save for lowest latency
                esp_wifi_set_ps(WIFI_PS_NONE);
                
                // Initialize mDNS service
                const char* hostname = get_mdns_hostname();
                if (hostname) {
                    esp_err_t mdns_err = mdns_init();
                    if (mdns_err == ESP_OK) {
                        mdns_hostname_set(hostname);
                        mdns_instance_name_set("Altair 8800 Emulator");
                        // Advertise HTTP service
                        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                        ESP_LOGI(TAG, "mDNS initialized: %s.local", hostname);
                    } else {
                        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
                    }
                }
                
                s_wifi_connected = true;
                s_retry_count = 0;
                
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                }
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP address");
                s_wifi_connected = false;
                s_ip_address[0] = '\0';
                s_ip_raw = 0;
                break;

            default:
                break;
        }
    }
}

bool wifi_init(void)
{
    if (s_wifi_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");

    // Create event group for synchronization
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create network interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Set WiFi storage to RAM (we manage NVS ourselves via config module)
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");

    return true;
}

wifi_result_t wifi_connect(uint32_t timeout_ms)
{
    if (!s_wifi_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return WIFI_RESULT_NOT_INITIALIZED;
    }

    // Get credentials from config
    const char* ssid = config_get_ssid();
    const char* password = config_get_password();

    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi credentials stored");
        return WIFI_RESULT_NO_CREDENTIALS;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    // Reset state
    s_retry_count = 0;
    s_wifi_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Configure station mode
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0] != '\0') {
        strncpy((char*)wifi_config.sta.password, password, 
                sizeof(wifi_config.sta.password) - 1);
    } else {
        // Open network
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection or failure
    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_WIFI_TIMEOUT_MS;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        // Suppress WiFi driver diagnostic messages (like <ba-add>)
        esp_log_level_set("wifi", ESP_LOG_WARN);
        return WIFI_RESULT_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to %s", ssid);
        esp_wifi_stop();
        return WIFI_RESULT_CONNECT_FAILED;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        esp_wifi_stop();
        return WIFI_RESULT_TIMEOUT;
    }
}

void wifi_disconnect(void)
{
    if (!s_wifi_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Disconnecting...");
    esp_wifi_disconnect();
    esp_wifi_stop();
    
    s_wifi_connected = false;
    s_ip_address[0] = '\0';
    s_ip_raw = 0;
}

bool wifi_is_ready(void)
{
    return s_wifi_initialized;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

bool wifi_get_ip(char* buffer, size_t length)
{
    if (!buffer || length == 0 || !s_wifi_connected) {
        return false;
    }

    if (s_ip_address[0] == '\0') {
        return false;
    }

    strncpy(buffer, s_ip_address, length - 1);
    buffer[length - 1] = '\0';
    return true;
}

uint32_t wifi_get_ip_raw(void)
{
    return s_ip_raw;
}

void wifi_set_ready(bool ready)
{
    s_wifi_initialized = ready;
    ESP_LOGI(TAG, "Hardware ready set to: %d", ready);
}

void wifi_set_connected(bool connected)
{
    s_wifi_connected = connected;
    ESP_LOGI(TAG, "Connected set to: %d", connected);

    if (!connected) {
        s_ip_address[0] = '\0';
        s_ip_raw = 0;
    }
}

void wifi_set_ip_address(const char* ip)
{
    if (ip && ip[0] != '\0') {
        strncpy(s_ip_address, ip, sizeof(s_ip_address) - 1);
        s_ip_address[sizeof(s_ip_address) - 1] = '\0';
        ESP_LOGI(TAG, "IP address cached: %s", s_ip_address);
    }
}

const char* wifi_get_ip_address(void)
{
    return s_ip_address[0] != '\0' ? s_ip_address : NULL;
}

bool wifi_start_ap(const char* ssid, const char* password)
{
    if (!s_wifi_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return false;
    }

    // Stop any existing WiFi mode first (important for STA→AP transition)
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay for clean transition

    ESP_LOGI(TAG, "Starting AP mode: SSID=%s", ssid);

    // Configure AP
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,           // Channel 1 has best compatibility
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,       // Explicitly not hidden
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);

    if (password && password[0] != '\0') {
        strncpy((char*)wifi_config.ap.password, password,
                sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    // Configure AP with static IP (192.168.4.1)
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_ap_mode = true;
    ESP_LOGI(TAG, "AP started: SSID=%s, IP=192.168.4.1", ssid);

    return true;
}

void wifi_stop_ap(void)
{
    if (!s_wifi_initialized || !s_wifi_ap_mode) {
        return;
    }

    ESP_LOGI(TAG, "Stopping AP mode");
    esp_wifi_stop();
    s_wifi_ap_mode = false;
}

bool wifi_is_ap_mode(void)
{
    return s_wifi_ap_mode;
}
