#pragma once

#include <stdint.h>

uint8_t io_port_in(uint8_t port);
void io_port_out(uint8_t port, uint8_t data);
