#pragma once

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static LARGE_INTEGER start;
    LARGE_INTEGER now;

    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    QueryPerformanceCounter(&now);
    return (absolute_time_t)(((now.QuadPart - start.QuadPart) * 1000000ULL) / frequency.QuadPart);
#else
    struct timespec ts;
    static uint64_t start_us = 0;
    uint64_t now_us;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_us = ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
    if (start_us == 0)
    {
        start_us = now_us;
    }
    return now_us - start_us;
#endif
}

static inline uint64_t to_ms_since_boot(absolute_time_t t)
{
    return t / 1000ULL;
}
