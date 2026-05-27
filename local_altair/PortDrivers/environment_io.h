/**
 * @file environment_io.h
 * @brief Text-file-backed environment variable I/O port driver (host build).
 *
 * Mirrors the ESP32 NVS-backed driver in port_drivers/environment_io.c so
 * DXENV.C and the ENV CP/M program work identically on the host runner.
 *
 * Port map:
 *   OUT 71, 0      reset request buffer
 *   OUT 71, cmd    execute ENV command/API request
 *   OUT 72, byte   append request byte
 *   IN  71         last ENV status code
 *   IN  200        response bytes, NUL-terminated by io_ports
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ENVIRONMENT_PORT_COMMAND 71
#define ENVIRONMENT_PORT_DATA    72

/**
 * Initialize the environment driver. file_path is copied; if NULL or empty,
 * "altair_env.txt" in the current directory is used. Safe to call multiple
 * times; only the first call takes effect.
 */
void environment_io_init(const char *file_path);

/**
 * Look up an environment variable from the in-memory snapshot. key is
 * matched case-insensitively (stored uppercase). Returns true and fills
 * value/value_length if found; otherwise returns false and writes "" if
 * value/value_length is non-NULL.
 */
bool environment_io_get(const char *key, char *value, size_t value_length);

size_t environment_output(int port, uint8_t data, char *buffer, size_t buffer_length);
uint8_t environment_input(uint8_t port);

#ifdef __cplusplus
}
#endif
