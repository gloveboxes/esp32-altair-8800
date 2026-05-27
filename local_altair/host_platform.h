#pragma once

#include <stdbool.h>
#include <stdint.h>

void host_prefer_efficiency_core(void);
uint32_t host_monotonic_ms(void);
bool host_terminal_configure(void);
void host_terminal_restore(void);
int host_terminal_read_byte(void);
bool host_terminal_write_byte(uint8_t ch);
