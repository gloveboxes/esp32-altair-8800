/**
 * @file status_led.h
 * @brief RGB Status LED driver for ESP32-S3
 *
 * Controls the onboard WS2812 RGB LED to indicate WiFi connection status:
 * - Green flash every 30 seconds: WiFi connected
 * - Red flash every 10 seconds: WiFi disconnected or connection lost
 */

#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

/**
 * @brief Initialize and start the status LED task
 * 
 * Creates a FreeRTOS task that monitors WiFi status and flashes
 * the onboard RGB LED accordingly.
 * 
 * @return true if task started successfully, false otherwise
 */
bool status_led_init(void);

/**
 * @brief Update the WiFi connection status for LED indication
 * 
 * @param connected true if WiFi is connected, false otherwise
 */
void status_led_set_wifi_status(bool connected);

#endif // STATUS_LED_H
