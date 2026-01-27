/**
 * @file utility_io.c
 * @brief Utility I/O port driver for Altair 8800 emulator on ESP32
 *
 * Utility ports:
 * - Port 45: Random number generator (returns 2-byte random value)
 * - Port 70: Version information string
 */

#include "PortDrivers/utility_io.h"

#include "esp_random.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)data;
    size_t len = 0;

    switch (port)
    {
        case 45: // Random number (2 bytes)
            if (buffer != NULL && buffer_length >= 2)
            {
                uint16_t value = (uint16_t)esp_random();
                buffer[0] = (char)(value & 0x00FF);
                buffer[1] = (char)((value >> 8) & 0x00FF);
                len = 2;
            }
            break;

        case 70: // Version information
            if (buffer != NULL && buffer_length > 0)
            {
                len = (size_t)snprintf(buffer, buffer_length, "ESP32-S3 Altair8800 (IDF %s)\n",
                                       esp_get_idf_version());
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
