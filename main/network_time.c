#include "network_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_netif_sntp.h"
#include "vt100_terminal.h"
#include "time_setup.h"

static bool s_sntp_initialized = false;
static bool s_time_synced = false;
static int s_timezone_offset_minutes = TIME_SETUP_OFFSET_MINUTES_MIN - 1;

void network_time_apply_timezone(void)
{
    int offset_minutes = time_setup_get_offset_minutes();
    if (offset_minutes == s_timezone_offset_minutes)
    {
        return;
    }

    int abs_minutes = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    int hours = abs_minutes / 60;
    int minutes = abs_minutes % 60;
    char tz[24];

    /* POSIX TZ offsets use the opposite sign: UTC-10 means UTC+10 local time. */
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", offset_minutes >= 0 ? '-' : '+', hours, minutes);
    setenv("TZ", tz, 1);
    tzset();
    s_timezone_offset_minutes = offset_minutes;
}

static bool system_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo;

    network_time_apply_timezone();
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
    vt100_terminal_refresh_status_time();
}

bool network_time_sync(void)
{
    network_time_apply_timezone();

    if (s_time_synced || system_time_is_valid())
    {
        s_time_synced = true;
        return true;
    }

    if (!s_sntp_initialized)
    {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = sntp_sync_callback;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            printf("SNTP init failed: %s\n", esp_err_to_name(err));
            return false;
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
        vt100_terminal_refresh_status_time();
        printf("Time synchronized: %s", ctime(&now));
        return true;
    }

    printf("SNTP sync timed out: %s\n", esp_err_to_name(err));
    return false;
}
