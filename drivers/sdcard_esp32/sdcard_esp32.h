/**
 * @file sdcard_esp32.h
 * @brief ESP32-S3 SDMMC driver for Altair 8800 emulator
 * 
 * Provides SDMMC initialization and mounting for the ESP32-S3.
 * Pin assignments based on Freenove ESP32-S3 WROOM board.
 */

#ifndef _SDCARD_ESP32_H_
#define _SDCARD_ESP32_H_

#include <stdbool.h>
#include <stdint.h>

// SDMMC pin definitions for Freenove ESP32-S3 WROOM
// These match the Arduino reference: Sketch_06.1_SDMMC_Test.ino
#define SDMMC_PIN_CLK   38
#define SDMMC_PIN_CMD   40
#define SDMMC_PIN_D0    39
#define SDMMC_PIN_D1    41
#define SDMMC_PIN_D2    48
#define SDMMC_PIN_D3    47

// Mount point for the SD card filesystem
#define SDCARD_MOUNT_POINT "/sdcard"

/**
 * @brief Initialize SDMMC interface and mount FAT filesystem
 * 
 * @return true on success, false on failure
 */
bool sdcard_esp32_init(void);

/**
 * @brief Unmount SD card and free resources
 */
void sdcard_esp32_deinit(void);

/**
 * @brief Get total size of SD card in bytes
 * 
 * @return Total size in bytes, or 0 if not mounted
 */
uint64_t sdcard_esp32_get_total_bytes(void);

/**
 * @brief Get used space on SD card in bytes
 * 
 * @return Used space in bytes, or 0 if not mounted
 */
uint64_t sdcard_esp32_get_used_bytes(void);

/**
 * @brief Check if SD card is mounted
 * 
 * @return true if mounted, false otherwise
 */
bool sdcard_esp32_is_mounted(void);

#endif // _SDCARD_ESP32_H_
