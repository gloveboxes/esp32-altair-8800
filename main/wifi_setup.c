/**
 * @file wifi_setup.c
 * @brief WiFi orchestration: NVS credential management, captive-portal
 *        fallback, and WebSocket terminal bring-up.
 *
 * Sits above the low-level wifi.c driver wrapper and handles the boot-time
 * decision tree: stored creds → connect → captive portal, with a serial
 * prompt for credentials when no stored config exists.
 */

#include "wifi_setup.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"

#include "config.h"
#include "port_drivers/chat_io.h"
#include "wifi.h"
#include "captive_portal.h"
#include "websocket_console.h"

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
#include "vt100_terminal.h"
#else
#include "altair_panel.h"
#endif

#define WIFI_TASK_STACK_SIZE 8192
#define WIFI_TASK_PRIORITY 4
#define WIFI_TASK_CORE 0

static bool g_wifi_connected = false;
static char g_ip_address[16] = {0};
static atomic_bool g_websocket_enabled = false;
static bool s_sntp_initialized = false;
static bool s_time_synced = false;

bool wifi_setup_websocket_enabled(void)
{
    return atomic_load(&g_websocket_enabled);
}

/**
 * @brief Check for config clear request during early boot
 *
 * Waits briefly for user to press 'C' to clear WiFi credentials
 * and enter captive portal mode. Press Enter to skip the wait.
 */
static bool check_config_clear_request(void)
{
    printf("\nWiFi credentials found in flash storage.\n");
    printf("Press 'C' within 5 seconds to clear config and enter AP mode...\n");
    printf("Press Enter to skip wait and connect now.\n");

    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < 5000000)
    { // 5 seconds
        uint8_t c;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len > 0)
        {
            if (c == 'c' || c == 'C')
            {
                printf("\nClearing WiFi configuration...\n");
                altair_config_clear();
                return true; // Config was cleared
            }
            else if (c == '\r' || c == '\n')
            {
                printf("\nSkipping wait...\n");
                break; // Skip remaining wait time
            }
        }
    }
    printf("\n");
    return false; // Config not cleared
}

static bool serial_read_line(const char *prompt, char *buffer, size_t buffer_len, bool mask_input)
{
    size_t length = 0;

    if (!buffer || buffer_len == 0)
    {
        return false;
    }

    buffer[0] = '\0';
    usb_serial_jtag_write_bytes((const uint8_t *)prompt, strlen(prompt), pdMS_TO_TICKS(100));

    for (;;)
    {
        uint8_t c;
        int read_len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (read_len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            usb_serial_jtag_write_bytes((const uint8_t *)"\r\n", 2, pdMS_TO_TICKS(100));
            buffer[length] = '\0';
            return true;
        }

        if (c == 0x08 || c == 0x7F)
        {
            if (length > 0)
            {
                length--;
                buffer[length] = '\0';
                usb_serial_jtag_write_bytes((const uint8_t *)"\b \b", 3, pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (c < 32 || c > 126)
        {
            continue;
        }

        if (length + 1 >= buffer_len)
        {
            continue;
        }

        buffer[length++] = (char)c;
        buffer[length] = '\0';
        uint8_t echo = mask_input ? '*' : c;
        usb_serial_jtag_write_bytes(&echo, 1, pdMS_TO_TICKS(100));
    }
}

static void wifi_shell_serial_drain_line(uint32_t idle_timeout_ms)
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

static bool prompt_serial_wifi_credentials(char *ssid, size_t ssid_len,
                                           char *password, size_t password_len)
{
    printf("\n");
    printf("No stored WiFi credentials were found.\n");
    printf("Enter credentials in the serial monitor, or press Enter on SSID to use captive portal instead.\n\n");

    if (!serial_read_line("WiFi SSID: ", ssid, ssid_len, false))
    {
        return false;
    }
    if (ssid[0] == '\0')
    {
        return false;
    }

    if (!serial_read_line("WiFi Password: ", password, password_len, true))
    {
        return false;
    }

    return true;
}

static int wifi_shell_read_command_ms(uint32_t timeout_ms)
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
        wifi_shell_serial_drain_line(50);
        if (c >= 'a' && c <= 'z')
        {
            c = (uint8_t)(c - 'a' + 'A');
        }
        return c;
    }
    return -1;
}

static void wifi_setup_print_status(void)
{
    const char *ssid = config_get_ssid();
    const char *rfs_ip = config_get_rfs_ip();

    printf("\nWiFi settings\n");
    printf("  SSID: %s\n", ssid ? ssid : "(not set)");
    printf("  Password: %s\n", config_get_password() ? "set" : "not set");
    printf("  Remote FS IP: %s\n", rfs_ip ? rfs_ip : "(not set)");
}

static void wifi_setup_configure_serial(void)
{
    char ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
    char password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    char rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};
    const char *current_ssid = config_get_ssid();
    const char *current_password = config_get_password();
    const char *current_rfs_ip = config_get_rfs_ip();

    printf("\nConfigure WiFi credentials\n");
    if (current_ssid)
    {
        printf("Current SSID: %s\n", current_ssid);
        printf("Press Enter on SSID to keep current value.\n");
    }

    if (!serial_read_line("WiFi SSID: ", ssid, sizeof(ssid), false))
    {
        return;
    }
    if (ssid[0] == '\0' && current_ssid)
    {
        strncpy(ssid, current_ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
    }
    if (ssid[0] == '\0')
    {
        printf("SSID is required.\n");
        return;
    }

    if (current_password)
    {
        printf("Current password is set. Press Enter to keep it, '-' to clear it, or type a replacement.\n");
    }
    else
    {
        printf("Press Enter for an open network, or type the WiFi password.\n");
    }
    if (!serial_read_line("WiFi Password: ", password, sizeof(password), true))
    {
        return;
    }
    if (password[0] == '\0' && current_password)
    {
        strncpy(password, current_password, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    }
    else if (strcmp(password, "-") == 0)
    {
        password[0] = '\0';
    }

    if (current_rfs_ip)
    {
        printf("Current Remote FS IP: %s\n", current_rfs_ip);
        printf("Press Enter to keep it, '-' to clear it, or type a replacement.\n");
    }
    else
    {
        printf("Remote FS IP is optional. Press Enter to leave unset.\n");
    }
    if (!serial_read_line("Remote FS IP: ", rfs_ip, sizeof(rfs_ip), false))
    {
        return;
    }
    if (rfs_ip[0] == '\0' && current_rfs_ip)
    {
        strncpy(rfs_ip, current_rfs_ip, sizeof(rfs_ip) - 1);
        rfs_ip[sizeof(rfs_ip) - 1] = '\0';
    }
    else if (strcmp(rfs_ip, "-") == 0)
    {
        rfs_ip[0] = '\0';
    }

    if (altair_config_save(ssid, password, rfs_ip[0] ? rfs_ip : NULL))
    {
        printf("WiFi settings saved. They will be used when WiFi starts.\n");
    }
}

static void wifi_setup_print_menu(void)
{
    printf("\nWiFi manager\n");
    printf("  C - configure WiFi credentials\n");
    printf("  U - clear stored WiFi credentials\n");
    printf("  S - show current settings\n");
    printf("  Q - return to boot config\n");
}

void wifi_setup_run_config_shell(void)
{
    wifi_setup_print_menu();

    for (;;)
    {
        printf("wifi> ");
        int cmd = wifi_shell_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("WiFi manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case 'C':
            wifi_setup_configure_serial();
            wifi_setup_print_menu();
            break;

        case 'U':
            config_clear_wifi_settings();
            wifi_setup_print_menu();
            break;

        case 'S':
            wifi_setup_print_status();
            wifi_setup_print_menu();
            break;

        case 'Q':
            printf("Leaving WiFi manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use C, U, S, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}

/**
 * @brief Initialize WiFi - connect to stored network or start captive portal
 */
static void start_websocket_terminal(void)
{
    const char *mdns_name = get_mdns_hostname();
    if (mdns_name)
    {
        printf("Hostname: %s\n", mdns_name);
    }

    printf("Starting WebSocket terminal server...\n");
    websocket_console_init();
    if (websocket_console_start_server())
    {
        printf("WebSocket server started\n");
        printf("Terminal page: http://%s/\n", g_ip_address);
        atomic_store(&g_websocket_enabled, true);
    }
    else
    {
        printf("Failed to start WebSocket server\n");
    }
}

static bool system_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo;

    time(&now);
    if (now <= 0 || localtime_r(&now, &timeinfo) == NULL)
    {
        return false;
    }

    return (timeinfo.tm_year + 1900) >= 2024;
}

static void sntp_sync_callback(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    chat_io_set_network_available(g_wifi_connected);
}

static void sync_network_time(void)
{
    if (s_time_synced || system_time_is_valid())
    {
        s_time_synced = true;
        return;
    }

    if (!s_sntp_initialized)
    {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = sntp_sync_callback;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            printf("SNTP init failed: %s\n", esp_err_to_name(err));
            return;
        }
        s_sntp_initialized = true;
    }

    printf("Synchronizing time for TLS certificate validation...\n");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    if (err == ESP_OK || system_time_is_valid())
    {
        time_t now = 0;
        time(&now);
        s_time_synced = true;
        printf("Time synchronized: %s", ctime(&now));
        return;
    }

    printf("SNTP sync timed out: %s\n", esp_err_to_name(err));
}

static void setup_wifi(bool allow_serial_setup)
{
    char serial_ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
    char serial_password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};

    // Initialize WiFi subsystem
    if (!wifi_init())
    {
        printf("WiFi initialization failed!\n");
        return;
    }

    // Check for stored credentials
    if (altair_config_exists())
    {
        // Give user chance to clear config
        if (allow_serial_setup && check_config_clear_request())
        {
            // Config was cleared, fall through to captive portal
        }
        else
        {
            // Try to connect to stored network
            printf("Connecting to WiFi...\n");
            wifi_result_t result = wifi_connect();

            if (result == WIFI_RESULT_OK)
            {
                g_wifi_connected = true;
                wifi_get_ip(g_ip_address, sizeof(g_ip_address));
                printf("WiFi connected! IP: %s\n", g_ip_address);

                sync_network_time();
                chat_io_set_network_available(s_time_synced);
                if (!s_time_synced)
                {
                    printf("OpenAI chat disabled until time sync succeeds.\n");
                }

                start_websocket_terminal();

                return; // Successfully connected
            }

            if (!allow_serial_setup)
            {
                printf("WiFi connection failed (result=%d); captive portal disabled during normal boot.\n", result);
                chat_io_set_network_available(false);
                return;
            }

            printf("WiFi connection failed (result=%d), starting captive portal...\n", result);
        }
    }
    else
    {
        if (allow_serial_setup && prompt_serial_wifi_credentials(serial_ssid, sizeof(serial_ssid),
                                                                 serial_password, sizeof(serial_password)))
        {
            if (altair_config_save(serial_ssid, serial_password, NULL))
            {
                printf("Connecting to WiFi using serial-entered credentials...\n");
                wifi_result_t result = wifi_connect();
                if (result == WIFI_RESULT_OK)
                {
                    g_wifi_connected = true;
                    wifi_get_ip(g_ip_address, sizeof(g_ip_address));
                    printf("WiFi connected! IP: %s\n", g_ip_address);

                    sync_network_time();
                    chat_io_set_network_available(s_time_synced);
                    if (!s_time_synced)
                    {
                        printf("OpenAI chat disabled until time sync succeeds.\n");
                    }

                    start_websocket_terminal();
                    return;
                }

                printf("Serial-entered WiFi credentials did not connect (result=%d). Falling back to captive portal.\n", result);
            }
            else
            {
                printf("Failed to save serial-entered WiFi credentials. Falling back to captive portal.\n");
            }
        }
        else
        {
            if (!allow_serial_setup)
            {
                printf("No WiFi credentials configured; captive portal disabled during normal boot.\n");
                chat_io_set_network_available(false);
                return;
            }

            printf("No WiFi credentials configured - starting captive portal\n");
        }
    }

    // Start captive portal for configuration
    if (captive_portal_start())
    {
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
        vt100_terminal_set_ip(captive_portal_get_ip(), CAPTIVE_PORTAL_AP_SSID);
#else
        // Show setup screen on LCD (panel may be off until WiFi connects)
        altair_panel_show_captive_portal(CAPTIVE_PORTAL_AP_SSID, captive_portal_get_ip());
#endif

        printf("\n");
        printf("==============================================\n");
        printf("  WiFi Setup Mode\n");
        printf("  Connect to: '%s'\n", CAPTIVE_PORTAL_AP_SSID);
        printf("  Then open: http://%s/\n", captive_portal_get_ip());
        printf("==============================================\n");
        printf("\n");

        // Run captive portal until configuration is saved (device will reboot)
        while (captive_portal_is_running())
        {
            captive_portal_poll();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    else
    {
        printf("Failed to start captive portal!\n");
    }
}

static void wifi_task(void *pvParameters)
{
    (void)pvParameters;
    printf("WiFi setup task started on Core %d\n", xPortGetCoreID());
    setup_wifi(false);
    printf("WiFi setup task complete\n");
    vTaskDelete(NULL);
}

void wifi_setup_start(void)
{
    xTaskCreatePinnedToCore(
        wifi_task,
        "wifi_setup",
        WIFI_TASK_STACK_SIZE,
        NULL,
        WIFI_TASK_PRIORITY,
        NULL,
        WIFI_TASK_CORE);
}
