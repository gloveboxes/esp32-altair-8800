/**
 * @file io_ports.h
 * @brief I/O port handler interface for Altair 8800 emulator on ESP32
 *
 * This module routes I/O port operations to appropriate drivers:
 * - Ports 24-30, 41-43: Time/timer operations (time_io)
 * - Ports 50-51: Statistics (stats_io) - disabled on ESP32
 * - Ports 45, 70: Utility functions (utility_io)
 * - Ports 109, 110, 114, 33, 201: HTTP file transfer (http_io) - disabled on ESP32
 * - Ports 60, 61: File transfer (files_io)
 */

#pragma once

#include <stdint.h>

/**
 * @brief Handle input from an I/O port
 *
 * @param port Port number to read from
 * @return Data byte read from the port
 */
uint8_t io_port_in(uint8_t port);

/**
 * @brief Handle output to an I/O port
 *
 * @param port Port number to write to
 * @param data Data byte to write
 */
void io_port_out(uint8_t port, uint8_t data);
