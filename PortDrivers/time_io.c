/**
 * @file time_io.c
 * @brief Time I/O port driver for Altair 8800 emulator on ESP32
 *
 * Timer ports:
 * - Ports 24/25: Millisecond timer 0 (high/low byte of delay)
 * - Ports 26/27: Millisecond timer 1 (high/low byte of delay)
 * - Ports 28/29: Millisecond timer 2 (high/low byte of delay)
 * - Port 30: Seconds timer (single byte delay)
 *
 * Time string ports (output):
 * - Port 41: Seconds since boot
 * - Port 42: UTC wall clock (ISO 8601 format)
 * - Port 43: Local wall clock (ISO 8601 format)
 */

#include "PortDrivers/time_io.h"

#include "esp_timer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TIMER_0 0
#define TIMER_1 1
#define TIMER_2 2
#define NUM_MS_TIMERS 3

static uint64_t ms_timer_targets[NUM_MS_TIMERS] = {0, 0, 0};
static uint16_t ms_timer_delays[NUM_MS_TIMERS] = {0, 0, 0};
static uint64_t seconds_timer_target = 0;

/**
 * @brief Get elapsed milliseconds since boot using ESP-IDF timer
 */
static inline uint64_t get_elapsed_ms(void)
{
    return esp_timer_get_time() / 1000ULL;
}

/**
 * @brief Map port number to timer index
 */
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

/**
 * @brief Format boot-relative time as string
 */
static size_t format_boot_relative_time(char* buffer, size_t buffer_length)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return 0;
    }

    uint64_t seconds_since_boot = get_elapsed_ms() / 1000ULL;
    return (size_t)snprintf(buffer, buffer_length, "+%llus", (unsigned long long)seconds_since_boot);
}

/**
 * @brief Format wall clock time as ISO 8601 string
 */
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

size_t time_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;
    int timer_idx = get_timer_index(port);

    switch (port)
    {
        // High byte of timer delay
        case 24:
        case 26:
        case 28:
            if (timer_idx >= 0 && timer_idx < NUM_MS_TIMERS)
            {
                ms_timer_delays[timer_idx] = (ms_timer_delays[timer_idx] & 0x00FFu) | ((uint16_t)data << 8);
            }
            break;

        // Low byte of timer delay - starts the timer
        case 25:
        case 27:
        case 29:
            if (timer_idx >= 0 && timer_idx < NUM_MS_TIMERS)
            {
                ms_timer_delays[timer_idx] = (ms_timer_delays[timer_idx] & 0xFF00u) | data;
                ms_timer_targets[timer_idx] = get_elapsed_ms() + ms_timer_delays[timer_idx];
            }
            break;

        // Seconds timer
        case 30:
            seconds_timer_target = get_elapsed_ms() / 1000ULL + data;
            break;

        // Seconds since boot
        case 41:
            len = (size_t)snprintf(buffer, buffer_length, "%llu", (unsigned long long)(get_elapsed_ms() / 1000ULL));
            break;

        // UTC wall clock
        case 42:
            len = format_wall_clock(buffer, buffer_length, true);
            break;

        // Local wall clock
        case 43:
            len = format_wall_clock(buffer, buffer_length, false);
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
        // Millisecond timers
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
                    // Timer expired
                    ms_timer_targets[timer_idx] = 0;
                    ms_timer_delays[timer_idx] = 0;
                    retVal = 0;
                }
                else if (target_time > 0)
                {
                    // Timer still running
                    retVal = 1;
                }
                else
                {
                    // Timer not set
                    retVal = 0;
                }
            }
            break;

        // Seconds timer
        case 30:
        {
            uint64_t target_time = seconds_timer_target;
            uint64_t now_seconds = get_elapsed_ms() / 1000ULL;

            if (target_time > 0 && now_seconds >= target_time)
            {
                // Timer expired
                seconds_timer_target = 0;
                retVal = 0;
            }
            else if (target_time > 0)
            {
                // Timer still running
                retVal = 1;
            }
            else
            {
                // Timer not set
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
