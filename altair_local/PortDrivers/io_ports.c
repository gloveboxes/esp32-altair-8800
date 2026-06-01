#include "io_ports.h"

#include "PortDrivers/chat_io.h"
#include "PortDrivers/environment_io.h"
#include "PortDrivers/host_files_io.h"
#include "PortDrivers/time_io.h"
#include "PortDrivers/utility_io.h"
#include "PortDrivers/weather_io.h"

#include <stdio.h>
#include <string.h>

/* Must be large enough to hold the full ENV list response (all keys=values
 * concatenated) and any chat/weather payload. Matches the ESP32 build in
 * port_drivers/io_ports.c. A 128-byte buffer truncated long values such as
 * CHAT_OPENAI_KEY in the ENV list output. */
#define REQUEST_BUFFER_SIZE 2048

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
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 31:
        case 37:
        case 38:
        case 39:
        case 41:
        case 42:
        case 43:
        case 44:
            request_unit.len = time_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case 45:
        case 48:
        case 70:
            request_unit.len = utility_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case WEATHER_PORT_FIELD:
            request_unit.len = weather_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case 60:
        case 61:
            host_files_out(port, data);
            break;
        case ENVIRONMENT_PORT_COMMAND:
        case ENVIRONMENT_PORT_DATA:
            request_unit.len = environment_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case CHAT_PORT_TRIGGER:
        case CHAT_PORT_REQUEST:
        case CHAT_PORT_RESET_RESPONSE:
            chat_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        default:
            break;
    }
}

uint8_t io_port_in(uint8_t port)
{
    switch (port)
    {
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
            return time_input(port);
        case 60:
        case 61:
            return host_files_in(port);
        case ENVIRONMENT_PORT_COMMAND:
            return environment_input(port);
        case WEATHER_PORT_STATUS:
            return weather_input(port);
        case CHAT_PORT_TRIGGER:
        case CHAT_PORT_STATUS:
        case CHAT_PORT_DATA:
            return chat_input(port);
        case 200:
            if (request_unit.count < request_unit.len && request_unit.count < sizeof(request_unit.buffer))
            {
                return (uint8_t)request_unit.buffer[request_unit.count++];
            }
            return 0x00;
        default:
            return 0x00;
    }
}
