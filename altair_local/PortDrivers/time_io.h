#pragma once

#include <stddef.h>
#include <stdint.h>

size_t time_output(int port, uint8_t data, char* buffer, size_t buffer_length);
uint8_t time_input(uint8_t port);
void time_reset(void);
