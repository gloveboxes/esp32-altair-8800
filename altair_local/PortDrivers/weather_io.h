/**
 * @file weather_io.h
 * @brief OpenWeatherMap I/O port driver (host build, libcurl + pthread).
 *
 * Mirrors port_drivers/weather_io.h on the ESP32:
 *   OUT 46, field_id   -> selects one field; reply available on port 200
 *   IN  47             -> WEATHER_STATUS_*
 *
 * Field IDs and status codes match the ESP32 driver so DXWEATHER and any
 * CP/M weather app behave identically against either build.
 *
 * Settings come from the env_io text store (altair_env.txt), keys:
 *   OWM_KEY       OpenWeatherMap API key (required)
 *   OWM_LOCATION  city/zip query (required, e.g. "Sydney,AU")
 *   OWM_UNITS     "metric" (default), "imperial", or "standard"
 *
 * environment_io_init() MUST be called before weather_io_init() so the
 * file has been loaded. Settings are re-read on every fetch cycle, so
 * CP/M-side `ENV` edits take effect on the next refresh.
 *
 * When libcurl is not compiled in, the driver still installs but never
 * leaves WEATHER_STATUS_NONE and the error field reports the reason.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define WEATHER_PORT_FIELD   46
#define WEATHER_PORT_STATUS  47

#define WEATHER_STATUS_NONE     0
#define WEATHER_STATUS_FETCHING 1
#define WEATHER_STATUS_READY    2
#define WEATHER_STATUS_ERROR    3

#define WEATHER_FIELD_CITY        0
#define WEATHER_FIELD_CUR_MAIN    1
#define WEATHER_FIELD_CUR_DESC    2
#define WEATHER_FIELD_CUR_TEMP    3
#define WEATHER_FIELD_CUR_HUMID   4
#define WEATHER_FIELD_CUR_WIND    5
#define WEATHER_FIELD_FC_MAIN     6
#define WEATHER_FIELD_FC_DESC     7
#define WEATHER_FIELD_FC_TEMP     8
#define WEATHER_FIELD_FC_WHEN     9
#define WEATHER_FIELD_AGE_SEC    10
#define WEATHER_FIELD_UNITS      11
#define WEATHER_FIELD_ERROR      12
#define WEATHER_FIELD_CUR_FEELS  13
#define WEATHER_FIELD_FC_FEELS   14

/**
 * Initialize the weather driver and start the background fetch thread.
 * Reads OWM_KEY / OWM_LOCATION / OWM_UNITS from the env_io store; missing
 * key or location parks the driver in WEATHER_STATUS_ERROR.
 */
void weather_io_init(void);

size_t weather_output(int port, uint8_t data, char *buffer, size_t buffer_length);
uint8_t weather_input(uint8_t port);

#ifdef __cplusplus
}
#endif
