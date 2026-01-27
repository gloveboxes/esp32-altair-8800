#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"

// Altair 8800 emulator includes - MUST be before FatFs includes due to naming conflicts
#include "intel8080.h"
#include "memory.h"

// SD Card support
#define SD_CARD_SUPPORT
#ifdef SD_CARD_SUPPORT
#include "sdcard_esp32.h"
#include "esp32_88dcdd_sd_card.h"
#else
#include "pico_88dcdd_flash.h"
// Disk images (embedded in flash)
#include "cpm63k_disk.h"
#include "bdsc_v1_60_disk.h"
#endif

// Front panel display
#include "altair_panel.h"

// ASCII mask for 7-bit terminal
#define ASCII_MASK_7BIT 0x7F

// Cycle counter for periodic FreeRTOS yield
#define YIELD_CYCLES 10000
static uint32_t cycle_count = 0;

// CPU instance
static intel8080_t cpu;

// Terminal read function - non-blocking using USB Serial JTAG driver
static uint8_t terminal_read(void)
{
    uint8_t c;
    // Try to read one byte with no wait (0 ticks timeout)
    int len = usb_serial_jtag_read_bytes(&c, 1, 0);
    if (len > 0) {
        return (uint8_t)(c & ASCII_MASK_7BIT);
    }
    return 0x00;  // No character available
}

// Terminal write function
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT;  // Take first 7 bits only
    usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY);
    // Flush immediately to ensure output in release mode's tight loop
    usb_serial_jtag_ll_txfifo_flush();
}

// Sense switches - return high byte of address bus (simple implementation)
static uint8_t sense(void)
{
    return 0x00;  // No sense switches configured
}

// I/O port handlers for ports not handled by the CPU core
// Note: Disk ports 0x08-0x0A are handled directly via disk_controller_t
static uint8_t io_port_in(uint8_t port)
{
    (void)port;
    return 0xFF;  // Unused ports return 0xFF
}

static void io_port_out(uint8_t port, uint8_t data)
{
    (void)port;
    (void)data;
    // Unused ports - ignore writes
}

void app_main(void)
{
    // Disable watchdog for main task (tight CPU emulation loop)
    esp_task_wdt_deinit();
    
    // Initialize USB Serial JTAG driver for non-blocking terminal I/O
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    usb_serial_jtag_driver_install(&usb_config);
    
    // Wait for serial terminal to connect
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Start front panel display task on Core 1
    printf("Starting front panel display on Core 1...\n");
    altair_panel_start();
    
    // Give the display task time to initialize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Print banner
    printf("\n\n");
    printf("========================================\n");
    printf("  Altair 8800 Emulator - ESP32-S3\n");
    printf("========================================\n\n");

    // Print chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("Chip: ESP32-S3 with %d CPU core(s)\n", chip_info.cores);
    
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash size: %lu MB\n", flash_size / (1024 * 1024));
    }
    printf("\n");

#ifdef SD_CARD_SUPPORT
    // Initialize SD card
    printf("Initializing SD card...\n");
    if (!sdcard_esp32_init()) {
        printf("SD card initialization failed!\n");
        printf("Possible causes:\n");
        printf("  - No SD card inserted\n");
        printf("  - SD card not formatted as FAT32\n");
        printf("  - Incorrect wiring\n");
        return;
    }
    
    // Print SD card info
    uint64_t total_bytes = sdcard_esp32_get_total_bytes();
    uint64_t used_bytes = sdcard_esp32_get_used_bytes();
    printf("SD card total: %llu MB\n", total_bytes / (1024 * 1024));
    printf("SD card used:  %llu MB\n", used_bytes / (1024 * 1024));
    printf("\n");

    // Initialize disk controller
    printf("Initializing disk controller...\n");
    esp32_sd_disk_init();

    // Load disk images from SD card (4 drives: A, B, C, D)
    printf("Loading DISK_A: %s\n", DISK_A_PATH);
    if (esp32_sd_disk_load(0, DISK_A_PATH)) {
        printf("  DISK_A loaded successfully\n");
    } else {
        printf("  DISK_A load failed!\n");
        return;
    }

    printf("Loading DISK_B: %s\n", DISK_B_PATH);
    if (esp32_sd_disk_load(1, DISK_B_PATH)) {
        printf("  DISK_B loaded successfully\n");
    } else {
        printf("  DISK_B load failed!\n");
        return;
    }

    printf("Loading DISK_C: %s\n", DISK_C_PATH);
    if (esp32_sd_disk_load(2, DISK_C_PATH)) {
        printf("  DISK_C loaded successfully\n");
    } else {
        printf("  DISK_C load failed!\n");
        return;
    }

    printf("Loading DISK_D: %s\n", DISK_D_PATH);
    if (esp32_sd_disk_load(3, DISK_D_PATH)) {
        printf("  DISK_D loaded successfully\n");
    } else {
        printf("  DISK_D load failed!\n");
        return;
    }
#else
    // Initialize disk controller
    printf("Initializing disk controller...\n");
    pico_disk_init();

    // Load CPM disk image into drive 0 (DISK_A)
    printf("Loading DISK_A: cpm63k.dsk (embedded)\n");
    if (pico_disk_load(0, cpm63k_dsk, cpm63k_dsk_len)) {
        printf("  DISK_A loaded successfully (%u bytes)\n", cpm63k_dsk_len);
    } else {
        printf("  DISK_A load failed!\n");
        return;
    }

    // Load BDSC disk image into drive 1 (DISK_B)
    printf("Loading DISK_B: bdsc_v1_60.dsk (embedded)\n");
    if (pico_disk_load(1, bdsc_v1_60_dsk, bdsc_v1_60_dsk_len)) {
        printf("  DISK_B loaded successfully (%u bytes)\n", bdsc_v1_60_dsk_len);
    } else {
        printf("  DISK_B load failed!\n");
        return;
    }
#endif

    // Load disk boot loader ROM at 0xFF00
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Set up disk controller for CPU
#ifdef SD_CARD_SUPPORT
    static disk_controller_t disk_controller = {
        .disk_select = (port_out)esp32_sd_disk_select,
        .disk_status = (port_in)esp32_sd_disk_status,
        .disk_function = (port_out)esp32_sd_disk_function,
        .sector = (port_in)esp32_sd_disk_sector,
        .write = (port_out)esp32_sd_disk_write,
        .read = (port_in)esp32_sd_disk_read
    };
#else
    static disk_controller_t disk_controller = {
        .disk_select = (port_out)pico_disk_select,
        .disk_status = (port_in)pico_disk_status,
        .disk_function = (port_out)pico_disk_function,
        .sector = (port_in)pico_disk_sector,
        .write = (port_out)pico_disk_write,
        .read = (port_in)pico_disk_read
    };
#endif

    // Initialize CPU
    printf("Initializing Intel 8080 CPU...\n");
    i8080_reset(&cpu, terminal_read, terminal_write, sense, 
                &disk_controller, io_port_in, io_port_out);

    // Set CPU to boot from disk loader at 0xFF00
    printf("Setting PC to 0xFF00 (disk boot loader)\n");
    i8080_examine(&cpu, 0xFF00);

    printf("\n");
    printf("Starting Altair 8800 emulation...\n");
    printf("========================================\n\n");

    // Main emulation loop - run CPU cycles with periodic yield
    for (;;) {
        i8080_cycle(&cpu);
        
        // Update front panel display state (read by panel task on Core 1)
        g_panel_address = cpu.address_bus;
        g_panel_data = cpu.data_bus;
        g_panel_status = cpu.cpuStatus;
        
        // Yield to FreeRTOS periodically to avoid watchdog timeout
        if (++cycle_count >= YIELD_CYCLES) {
            cycle_count = 0;
            taskYIELD();
        }
    }
}
