/**
 * @file time_io.h
 * @brief Time I/O port driver for Altair 8800 emulator on ESP32
 *
 * Timer ports:
 * - Ports 24/25: Millisecond timer 0 (high/low byte of delay)
 * - Ports 26/27: Millisecond timer 1 (high/low byte of delay)
 * - Ports 28/29: Millisecond timer 2 (high/low byte of delay)
 * - Port 30: Seconds timer (single byte delay)
 *
 * Time string ports (output):
 * - Port 41: Seconds since boot
 * - Port 42: UTC wall clock (ISO 8601 format)
 * - Port 43: Local wall clock (ISO 8601 format)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Handle output to time ports
 *
 * @param port Port number
 * @param data Data byte written
 * @param buffer Output buffer for time strings
 * @param buffer_length Size of output buffer
 * @return Number of bytes written to buffer (for string ports)
 */
size_t time_output(int port, uint8_t data, char* buffer, size_t buffer_length);

/**
 * @brief Handle input from timer ports
 *
 * @param port Port number
 * @return 1 if timer still running, 0 if expired or not set
 */
uint8_t time_input(uint8_t port);
