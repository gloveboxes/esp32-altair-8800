/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Intel 8080 disassembler and utility functions for CPU monitor

// Convert uint8 to binary string representation
void uint8_to_binary(uint8_t value, char* buffer, size_t buffer_size);

// Get Intel 8080 instruction name for disassembly
const char* get_i8080_instruction_name(uint8_t opcode, uint8_t* instruction_length);

// Publish message to WebSocket clients
void publish_message(const char* message, size_t length);
