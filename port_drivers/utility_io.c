/**
 * @file utility_io.c
 * @brief Utility I/O port driver for Altair 8800 emulator on ESP32
 *
 * Utility ports:
 * - Port 45: Random number generator (returns 2-byte random value)
 * - Port 48: System info (sub-codes: 0=hostname, 1=WiFi IP, 2=device ID)
 * - Port 49: Reboot ESP32 (data byte must be magic 0xA5 to trigger)
 * - Port 70: Version information string
 */

#include "port_drivers/utility_io.h"

#include "esp_random.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UTILITY_REBOOT_MAGIC 0xA5

#include "config.h"
#include "wifi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;

    switch (port)
    {
        case 45: // Random number (2 bytes)
            (void)data;
            if (buffer != NULL && buffer_length >= 2)
            {
                uint16_t value = (uint16_t)esp_random();
                buffer[0] = (char)(value & 0x00FF);
                buffer[1] = (char)((value >> 8) & 0x00FF);
                len = 2;
            }
            break;

        case 48: // System info: 0=hostname, 1=WiFi IP, 2=device ID
            if (buffer != NULL && buffer_length > 0)
            {
                switch (data)
                {
                    case 0: {
                        const char *h = get_mdns_hostname();
                        len = (size_t)snprintf(buffer, buffer_length, "%s",
                                               (h && *h) ? h : "unknown");
                        break;
                    }
                    case 1: {
                        const char *ip = wifi_get_ip_address();
                        len = (size_t)snprintf(buffer, buffer_length, "%s",
                                               (ip && *ip) ? ip : "0.0.0.0");
                        break;
                    }
                    case 2: {
                        uint8_t mac[6] = {0};
                        esp_efuse_mac_get_default(mac);
                        len = (size_t)snprintf(buffer, buffer_length,
                                               "%02X:%02X:%02X:%02X:%02X:%02X",
                                               mac[0], mac[1], mac[2],
                                               mac[3], mac[4], mac[5]);
                        break;
                    }
                    default:
                        len = (size_t)snprintf(buffer, buffer_length, "unknown");
                        break;
                }
            }
            break;

        case 70: // Version information
            (void)data;
            if (buffer != NULL && buffer_length > 0)
            {
                len = (size_t)snprintf(buffer, buffer_length, "ESP32-S3 Altair8800 (IDF %s)\n",
                                       esp_get_idf_version());
            }
            break;

        case 49: // Reboot ESP32 (requires magic byte to avoid accidental triggers)
            if (data == UTILITY_REBOOT_MAGIC)
            {
                /* Give the CP/M caller and UART a moment to flush before reset. */
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            break;

        default:
            break;
    }

    return len;
}

uint8_t utility_input(uint8_t port)
{
    (void)port;
    return 0;
}
