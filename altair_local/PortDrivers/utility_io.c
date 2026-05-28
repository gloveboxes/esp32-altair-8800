#include "PortDrivers/utility_io.h"

#include "build_version.h"
#include "wifi.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;

    switch (port)
    {
        case 45:
            (void)data;
            if (buffer != NULL && buffer_length >= 2)
            {
                /* 16 random bits split across two bytes. rand() is fine
                   here - this port feeds CP/M userland games, not crypto. */
                uint16_t value = (uint16_t)((rand() << 1) ^ rand());
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
                        const char *h = wifi_get_hostname();
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
                    case 2:
                        len = (size_t)snprintf(buffer, buffer_length, "LOCAL-ALTAIR");
                        break;
                    default:
                        len = (size_t)snprintf(buffer, buffer_length, "unknown");
                        break;
                }
            }
            break;
        case 70: // Load Altair version number
            (void)data;
            if (buffer != NULL && buffer_length > 0)
            {
                len = (size_t)snprintf(buffer, buffer_length, "%s %d (%s %s)\n", BOARD_NAME, BUILD_VERSION, BUILD_DATE, BUILD_TIME);
            }
            break;
        default:
            return 0;
    }

    return len;
}

uint8_t utility_input(uint8_t port)
{
    (void)port;
    return 0;
}
