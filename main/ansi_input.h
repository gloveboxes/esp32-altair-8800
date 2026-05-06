#pragma once

#include <stdint.h>

#define ANSI_INPUT_ESC_GRACE_MS 30u

uint8_t ansi_input_process(uint8_t ch, uint32_t now_ms);
