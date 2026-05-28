#include "PortDrivers/time_io.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define TIMER_0 0
#define TIMER_1 1
#define TIMER_2 2
#define NUM_MS_TIMERS 3

static uint64_t ms_timer_targets[NUM_MS_TIMERS] = {0, 0, 0};
static uint16_t ms_timer_delays[NUM_MS_TIMERS] = {0, 0, 0};
static uint64_t seconds_timer_target = 0;
static uint64_t emulator_start_ms = 0;

/* Monotonic milliseconds since process start. */
static uint64_t host_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;

    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000ULL) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}

static inline uint64_t get_elapsed_ms(void)
{
    uint64_t now_ms = host_now_ms();

    if (now_ms < emulator_start_ms)
    {
        return 0;
    }

    return now_ms - emulator_start_ms;
}

void time_reset(void)
{
    for (int i = 0; i < NUM_MS_TIMERS; i++)
    {
        ms_timer_targets[i] = 0;
        ms_timer_delays[i] = 0;
    }

    seconds_timer_target = 0;
    emulator_start_ms = host_now_ms();
}

static int get_timer_index(int port)
{
    switch (port)
    {
        case 24:
        case 25:
            return TIMER_0;
        case 26:
        case 27:
            return TIMER_1;
        case 28:
        case 29:
            return TIMER_2;
        default:
            return -1;
    }
}

static size_t format_boot_relative_time(char* buffer, size_t buffer_length)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return 0;
    }

    uint64_t seconds_since_boot = get_elapsed_ms() / 1000ULL;
    return (size_t)snprintf(buffer, buffer_length, "+%llus", (unsigned long long)seconds_since_boot);
}

static size_t format_wall_clock(char* buffer, size_t buffer_length, bool utc)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return 0;
    }

    time_t now = time(NULL);
    if (now == 0)
    {
        return format_boot_relative_time(buffer, buffer_length);
    }

    struct tm* result = utc ? gmtime(&now) : localtime(&now);

    if (result == NULL)
    {
        return format_boot_relative_time(buffer, buffer_length);
    }

    size_t len = strftime(buffer, buffer_length, utc ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%S", result);
    return len;
}

static size_t format_local_date(char* buffer, size_t buffer_length)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return 0;
    }

    time_t now = time(NULL);
    if (now == 0)
    {
        return format_boot_relative_time(buffer, buffer_length);
    }

    struct tm* result = localtime(&now);
    if (result == NULL)
    {
        return format_boot_relative_time(buffer, buffer_length);
    }

    return strftime(buffer, buffer_length, "%A, %d %B %Y", result);
}

size_t time_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;
    int timer_idx = get_timer_index(port);

    switch (port)
    {
        case 24:
        case 26:
        case 28:
            if (timer_idx >= 0 && timer_idx < NUM_MS_TIMERS)
            {
                ms_timer_delays[timer_idx] = (ms_timer_delays[timer_idx] & 0x00FFu) | ((uint16_t)data << 8);
            }
            break;
        case 25:
        case 27:
        case 29:
            if (timer_idx >= 0 && timer_idx < NUM_MS_TIMERS)
            {
                ms_timer_delays[timer_idx] = (ms_timer_delays[timer_idx] & 0xFF00u) | data;
                ms_timer_targets[timer_idx] = get_elapsed_ms() + ms_timer_delays[timer_idx];
            }
            break;
        case 30:
            seconds_timer_target = get_elapsed_ms() / 1000ULL + data;
            break;
        case 41:
            len = (size_t)snprintf(buffer, buffer_length, "%llu", (unsigned long long)(get_elapsed_ms() / 1000ULL));
            break;
        case 42:
            len = format_wall_clock(buffer, buffer_length, true);
            break;
        case 43:
            len = format_wall_clock(buffer, buffer_length, false);
            break;
        case 44:
            len = format_local_date(buffer, buffer_length);
            break;
        default:
            break;
    }

    return len;
}

uint8_t time_input(uint8_t port)
{
    uint8_t retVal = 0;
    int timer_idx = get_timer_index(port);

    switch (port)
    {
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            if (timer_idx >= 0 && timer_idx < NUM_MS_TIMERS)
            {
                uint64_t target_time = ms_timer_targets[timer_idx];
                if (target_time > 0 && get_elapsed_ms() >= target_time)
                {
                    ms_timer_targets[timer_idx] = 0;
                    ms_timer_delays[timer_idx] = 0;
                    retVal = 0;
                }
                else if (target_time > 0)
                {
                    retVal = 1;
                }
                else
                {
                    retVal = 0;
                }
            }
            break;
        case 30:
        {
            uint64_t target_time = seconds_timer_target;
            uint64_t now_seconds = get_elapsed_ms() / 1000ULL;

            if (target_time > 0 && now_seconds >= target_time)
            {
                seconds_timer_target = 0;
                retVal = 0;
            }
            else if (target_time > 0)
            {
                retVal = 1;
            }
            else
            {
                retVal = 0;
            }
        }
        break;
        default:
            retVal = 0;
            break;
    }

    return retVal;
}
