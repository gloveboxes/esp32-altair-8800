/* Host-side stub for the ESP32 websocket_console module. See header for
   rationale. Currently every "enqueued" byte is written straight to stdout
   so the CPU monitor (and any other publish_message consumer) is visible
   in the local terminal. Replace with a real queue once altair_local grows
   a WebSocket server. */

#include "websocket_console.h"
#include "host_platform.h"

void websocket_console_enqueue_output(uint8_t value)
{
    (void)host_terminal_write_byte(value);
}

bool websocket_console_has_clients(void)
{
    return false;
}

void websocket_console_clear_queues(void)
{
}
