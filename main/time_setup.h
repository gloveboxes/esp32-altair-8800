/**
 * @file time_setup.h
 * @brief Local timezone offset setup and cache.
 */

#ifndef TIME_SETUP_H
#define TIME_SETUP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIME_SETUP_OFFSET_MINUTES_MIN (-14 * 60)
#define TIME_SETUP_OFFSET_MINUTES_MAX (14 * 60)

/**
 * @brief Load the stored timezone offset into RAM.
 *
 * Returns UTC/zero offset when no setting is present.
 */
void time_setup_init(void);

/**
 * @brief Reset the in-memory timezone offset cache to UTC.
 */
void time_setup_reset_cache(void);

/**
 * @brief Get the cached local timezone offset from UTC, in minutes.
 */
int time_setup_get_offset_minutes(void);

/**
 * @brief Save and cache the local timezone offset from UTC, in minutes.
 */
bool time_setup_save_offset_minutes(int offset_minutes);

/**
 * @brief Run the boot-time serial timezone setup shell.
 */
void time_setup_run_config_shell(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_SETUP_H
