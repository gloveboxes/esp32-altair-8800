/**
 * @file files_io.h
 * @brief Remote file transfer I/O port driver for Altair 8800 emulator on ESP32
 *
 * Implements the FT protocol over TCP to the Remote FT server.
 * Ports:
 *  - 60: Command/status
 *  - 61: Data (filename + chunk data)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// Chunk size for file transfers
#define FT_CHUNK_SIZE 256

// Command port (port 60) - OUT commands
typedef enum
{
    FT_CMD_NOP = 0,           // No operation
    FT_CMD_SET_FILENAME = 1,  // Begin filename transfer (compat)
    FT_CMD_FILENAME_CHAR = 2, // Legacy per-char marker (ignored)
    FT_CMD_REQUEST_CHUNK = 3, // Request next 256-byte chunk
    FT_CMD_CLOSE = 4          // Close file transfer
} ft_command_t;

// Status values (port 60 IN)
typedef enum
{
    FT_STATUS_IDLE = 0,      // Idle, ready for new transfer
    FT_STATUS_DATAREADY = 1, // Data ready to read
    FT_STATUS_EOF = 2,       // End of file reached
    FT_STATUS_BUSY = 3,      // Transfer in progress, not ready for new command
    FT_STATUS_ERROR = 0xFF   // Error occurred
} ft_status_t;

// Initialize file transfer port driver
void files_io_init(void);

// Handle output to file transfer ports (called from emulator core)
size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length);

// Handle input from file transfer ports (called from emulator core)
uint8_t files_input(uint8_t port);
