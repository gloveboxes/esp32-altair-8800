#pragma once

#include <stddef.h>

static inline const char *wifi_get_hostname(void)
{
    return "local-altair";
}

static inline const char *wifi_get_ip_address(void)
{
    return "127.0.0.1";
}

static inline int wifi_get_ip(char *buffer, size_t length)
{
    const char *ip = wifi_get_ip_address();
    size_t i;

    if (buffer == NULL || length == 0)
    {
        return 0;
    }

    for (i = 0; i + 1 < length && ip[i] != '\0'; i++)
    {
        buffer[i] = ip[i];
    }
    buffer[i] = '\0';
    return 1;
}
