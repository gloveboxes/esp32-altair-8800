/**
 * @file weather_io.h
 * @brief OpenWeatherMap I/O port driver for Altair 8800 emulator on ESP32.
 *
 * The weather is fetched asynchronously by a background FreeRTOS task that
 * runs on startup (once the network is available) and again every 20
 * minutes. The Altair side never blocks: it polls a status port, then
 * reads pre-parsed string fields from the request buffer (port 200).
 *
 * Port map:
 *   Port 46 (OUT) - select field id; the snprintf'd value is then read
 *                   byte-by-byte from port 200 until a NUL.
 *   Port 47 (IN)  - status byte:
 *                       WEATHER_STATUS_NONE     0  (no data yet)
 *                       WEATHER_STATUS_FETCHING 1  (refresh in progress)
 *                       WEATHER_STATUS_READY    2  (data available)
 *                       WEATHER_STATUS_ERROR    3  (last fetch failed)
 *
 * Field ids (port 46 OUT data byte):
 *   0  city name
 *   1  current condition main (e.g. "Clouds")
 *   2  current condition description (e.g. "broken clouds")
 *   3  current temperature, integer (units depend on config; default C)
 *   4  current humidity %
 *   5  current wind speed, integer m/s (or mph if imperial)
 *   6  forecast condition main (next ~3h block)
 *   7  forecast condition description
 *   8  forecast temperature, integer
 *   9  forecast time text "YYYY-MM-DD HH:MM"
 *  10  age of data in seconds since last successful fetch
 *  11  units string ("C", "F", or "K")
 *  12  last error message (only meaningful when status == ERROR)
 *  13  current feels-like temperature, integer
 *  14  forecast feels-like temperature, integer
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

/* Field ids written to port 46. */
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
 * @brief Initialize the weather port driver.
 *
 * Allocates PSRAM-resident state and starts the background fetch task.
 * The task waits for chat_io_set_network_available()-style signalling
 * via weather_io_set_network_available() before issuing any HTTP calls.
 */
void weather_io_init(void);

/**
 * @brief Inform the driver whether the network (WiFi + time) is up.
 *
 * Mirrors chat_io_set_network_available(). When transitioning to true
 * the background task is woken to perform an immediate fetch.
 */
void weather_io_set_network_available(bool available);

/**
 * @brief Boot-shell helper for configuring API key / location / units.
 *
 * Wired into main/config.c boot menu (option 4). Reads from USB serial.
 */
void weather_io_run_config_shell(void);

/**
 * @brief Handle an Altair OUT to a weather port.
 *
 * @return number of bytes written into @p buffer (read back through
 *         port 200), or 0.
 */
size_t weather_output(int port, uint8_t data, char *buffer, size_t buffer_length);

/**
 * @brief Handle an Altair IN from a weather port.
 */
uint8_t weather_input(uint8_t port);

#ifdef __cplusplus
}
#endif
