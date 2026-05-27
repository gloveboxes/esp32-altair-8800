#include "PortDrivers/utility_io.h"

#include "pico/rand.h"
#include "pico/time.h"

#include "build_version.h"
#include "wifi.h"
#include "pico/unique_id.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;
    (void)data;

    switch (port)
    {
        case 45:
            if (buffer != NULL && buffer_length >= 2)
            {
                uint16_t value = (uint16_t)get_rand_32();
                buffer[0] = (char)(value & 0x00FF);
                buffer[1] = (char)((value >> 8) & 0x00FF);
                len = 2;
            }
            break;
        case 70: // Load Altair version number
            if (buffer != NULL && buffer_length > 0)
            {
                len = (size_t)snprintf(buffer, buffer_length, "%s %d (%s %s)\n", PICO_BOARD, BUILD_VERSION, BUILD_DATE, BUILD_TIME);
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
