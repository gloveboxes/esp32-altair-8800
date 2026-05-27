#pragma once

#include <stddef.h>
#include <stdint.h>

size_t utility_output(int port, uint8_t data, char* buffer, size_t buffer_length);
uint8_t utility_input(uint8_t port);
