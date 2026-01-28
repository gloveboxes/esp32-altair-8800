/**
 * @file utility_io.h
 * @brief Utility I/O port driver for Altair 8800 emulator on ESP32
 *
 * Utility ports:
 * - Port 45: Random number generator (returns 2-byte random value)
 * - Port 70: Version information string
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Handle output to utility ports
 *
 * @param port Port number
 * @param data Data byte written (ignored for most utility ports)
 * @param buffer Output buffer for response data
 * @param buffer_length Size of output buffer
 * @return Number of bytes written to buffer
 */
size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length);

/**
 * @brief Handle input from utility ports
 *
 * @param port Port number
 * @return Data byte (currently unused)
 */
uint8_t utility_input(uint8_t port);
