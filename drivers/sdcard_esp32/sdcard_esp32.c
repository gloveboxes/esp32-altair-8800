/**
 * @file sdcard_esp32.c
 * @brief ESP32-S3 SDMMC driver implementation for Altair 8800 emulator
 * 
 * Uses ESP-IDF's SDMMC peripheral in 4-bit mode for high-speed SD card access.
 */

#include "sdcard_esp32.h"
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

static const char* TAG = "SDCARD_ESP32";

// SDMMC card handle
static sdmmc_card_t* s_card = NULL;
static bool s_mounted = false;

bool sdcard_esp32_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing SDMMC interface...");
    ESP_LOGI(TAG, "  CLK: GPIO%d", SDMMC_PIN_CLK);
    ESP_LOGI(TAG, "  CMD: GPIO%d", SDMMC_PIN_CMD);
    ESP_LOGI(TAG, "  D0:  GPIO%d", SDMMC_PIN_D0);
    ESP_LOGI(TAG, "  D1:  GPIO%d", SDMMC_PIN_D1);
    ESP_LOGI(TAG, "  D2:  GPIO%d", SDMMC_PIN_D2);
    ESP_LOGI(TAG, "  D3:  GPIO%d", SDMMC_PIN_D3);

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Configure SDMMC host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz

    // Configure SDMMC slot with custom pins
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    
    // Set the GPIO pins for SDMMC
    slot_config.clk = SDMMC_PIN_CLK;
    slot_config.cmd = SDMMC_PIN_CMD;
    slot_config.d0 = SDMMC_PIN_D0;
    slot_config.d1 = SDMMC_PIN_D1;
    slot_config.d2 = SDMMC_PIN_D2;
    slot_config.d3 = SDMMC_PIN_D3;
    
    // Use 4-bit bus width for faster transfers
    slot_config.width = 4;

    // Enable internal pullups on the bus lines
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card filesystem at %s...", SDCARD_MOUNT_POINT);

    ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, 
                                   &mount_config, &s_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Check if SD card is formatted as FAT32.");
        } else if (ret == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Failed to allocate memory for SD card.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s). "
                     "Make sure SD card is inserted and pins are correct.",
                     esp_err_to_name(ret));
        }
        return false;
    }

    s_mounted = true;

    // Print card info
    ESP_LOGI(TAG, "SD card mounted successfully!");
    sdmmc_card_print_info(stdout, s_card);

    return true;
}

void sdcard_esp32_deinit(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}

uint64_t sdcard_esp32_get_total_bytes(void)
{
    if (!s_mounted || !s_card) {
        return 0;
    }
    
    // Calculate total size from card info
    // sector_size is typically 512 bytes
    uint64_t total = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    return total;
}

uint64_t sdcard_esp32_get_used_bytes(void)
{
    if (!s_mounted) {
        return 0;
    }

    FATFS* fs;
    DWORD free_clusters;
    
    if (f_getfree(SDCARD_MOUNT_POINT, &free_clusters, &fs) != FR_OK) {
        return 0;
    }

    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors = free_clusters * fs->csize;
    uint64_t used_sectors = total_sectors - free_sectors;
    
    // Multiply by sector size (typically 512)
    return used_sectors * fs->ssize;
}

bool sdcard_esp32_is_mounted(void)
{
    return s_mounted;
}
