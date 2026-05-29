/**
 * @file websocket_console.h
 * @brief Host stub for the ESP32 firmware's websocket_console module.
 *
 * The shared monitor/disasm sources (main/i8080_disasm.c, main/cpu_state.c)
 * call `websocket_console_enqueue_output()` to deliver bytes to the browser
 * terminal. The host runner has no browser yet, so the stub forwards the
 * byte to stdout via host_terminal_write_byte(). When real WebSocket
 * support is added to altair_local, swap this implementation for one that
 * pushes bytes onto an outbound queue, exactly like the ESP version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void websocket_console_enqueue_output(uint8_t value);
bool websocket_console_has_clients(void);
void websocket_console_clear_queues(void);
