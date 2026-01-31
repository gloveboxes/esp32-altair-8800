/**
 * @file io_ports.c
 * @brief I/O port handler implementation for Altair 8800 emulator on ESP32
 *
 * Routes I/O port operations to appropriate drivers based on port number.
 */

#include "port_drivers/io_ports.h"
#include "port_drivers/files_io.h"
#include "port_drivers/time_io.h"
#include "port_drivers/utility_io.h"

#include <stdio.h>
#include <string.h>

#define REQUEST_BUFFER_SIZE 128

typedef struct
{
    size_t len;
    size_t count;
    char buffer[REQUEST_BUFFER_SIZE];
} request_unit_t;

static request_unit_t request_unit;

void io_port_out(uint8_t port, uint8_t data)
{
    memset(&request_unit, 0, sizeof(request_unit));

    switch (port)
    {
        // Time/timer ports
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 41:
        case 42:
        case 43:
            request_unit.len = time_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;

        // Utility ports
        case 45:
        case 70:
            request_unit.len = utility_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;

        // Stats ports (50, 51) - not implemented on ESP32 yet
        // HTTP ports (109, 110, 114) - not implemented on ESP32 yet
        // Files ports (60, 61)
        case 60:
        case 61:
            files_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;

        default:
            break;
    }
}

uint8_t io_port_in(uint8_t port)
{
    switch (port)
    {
        // Time/timer ports
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
            return time_input(port);

        // Request buffer read port
        case 200:
            if (request_unit.count < request_unit.len && request_unit.count < sizeof(request_unit.buffer))
            {
                return (uint8_t)request_unit.buffer[request_unit.count++];
            }
            return 0x00;

        // HTTP status/data ports (33, 201) - not implemented on ESP32 yet
        // Files ports (60, 61)
        case 60:
        case 61:
            return files_input(port);

        default:
            return 0x00;
    }
}
