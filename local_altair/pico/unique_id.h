#pragma once

#include <stdio.h>
#include <string.h>

typedef unsigned int uint;

static inline void pico_get_unique_board_id_string(char *buffer, uint buffer_length)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return;
    }

    snprintf(buffer, buffer_length, "LOCAL-ALTAIR");
    buffer[buffer_length - 1] = '\0';
}
