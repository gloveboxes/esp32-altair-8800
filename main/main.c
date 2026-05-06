/**
 * @file main.c
 * @brief Altair 8800 Emulator for ESP32-S3
 *
 * Core Allocation:
 * ----------------
 * Core 0 (PRO_CPU / Default): System I/O
 *   - USB Serial JTAG terminal I/O
 *   - WiFi (when enabled)
 *   - FreeRTOS system tasks
 *
 * Core 1 (APP_CPU): Altair 8800 emulation and storage
 *   - Intel 8080 instruction execution
 *   - SD card disk I/O (synchronous with emulator)
 *   - VT100 display updates on VT100 display builds
 *   - Tight emulation loop with minimal interruption
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

// WiFi and configuration
#include "config.h"
#include "bt_keyboard.h"
#include "wifi_setup.h"
#include "websocket_console.h"

// Altair 8800 emulator includes - MUST be before FatFs includes due to naming conflicts
#include "intel8080.h"
#include "memory.h"
#include "esp_heap_caps.h"

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
#include "panel_display.h"

// VT100 terminal (Waveshare AXS15231B only)
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
#include "vt100_terminal.h"
#endif

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
#include "altair_splash.h"
#endif

// I/O port handlers
#include "port_drivers/io_ports.h"
#include "port_drivers/chat_io.h"
#include "port_drivers/files_io.h"

// CPU state and virtual monitor
#include "cpu_state.h"
#include "ansi_input.h"

// ASCII mask for 7-bit terminal
#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

// Static disk controller reference for reset
static disk_controller_t *g_disk_controller = NULL;

// Panel checkpoint trail (written by panel task, read by emulator heartbeat).
// Use volatile + plain int so a hung panel task can still be diagnosed.
volatile int g_panel_checkpoint = 0;
volatile uint32_t g_panel_checkpoint_count = 0;
#define PANEL_CHECKPOINT(n)         \
    do                              \
    {                               \
        g_panel_checkpoint = (n);   \
        g_panel_checkpoint_count++; \
    } while (0)

// Process character through ANSI escape sequence state machine
// Mirrors the Pico reference: shared ansi_input lib + monotonic_ms + terminal_postprocess.
static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t terminal_postprocess(uint8_t ch)
{
    ch &= ASCII_MASK_7BIT;
    if (ch == 28)
    {
        cpu_state_toggle_mode();
        return 0x00;
    }
    if (ch == '\n')
    {
        return '\r';
    }
    return ch;
}

// Terminal read function - non-blocking
static uint8_t terminal_read(void)
{
    // Input priority is WebSocket client, then BLE keyboard, then USB serial.
    uint8_t ws_ch = 0;
    if (wifi_setup_websocket_enabled() && websocket_console_try_dequeue_input(&ws_ch))
    {
        uint8_t ch = ansi_input_process((uint8_t)(ws_ch & ASCII_MASK_7BIT), monotonic_ms());
        return terminal_postprocess(ch);
    }

    uint8_t bt_ch = 0;
    if (bt_keyboard_try_dequeue_input(&bt_ch))
    {
        uint8_t ch = ansi_input_process((uint8_t)(bt_ch & ASCII_MASK_7BIT), monotonic_ms());
        return terminal_postprocess(ch);
    }

    // Fall back to USB serial if neither higher-priority source has input.
    uint8_t c;
    int len = usb_serial_jtag_read_bytes(&c, 1, 0);
    if (len > 0)
    {
        uint8_t ch = ansi_input_process((uint8_t)(c & ASCII_MASK_7BIT), monotonic_ms());
        return terminal_postprocess(ch);
    }

    // Idle pump: drives the lone-ESC grace timer in ansi_input.
    return terminal_postprocess(ansi_input_process(0x00, monotonic_ms()));
}

// Terminal write function
// Sends to both WebSocket client (if connected) and USB Serial JTAG
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT; // Take first 7 bits only

    // Mirror output to the on-device VT100 terminal.
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    vt100_terminal_putchar(c);
#endif

    if (wifi_setup_websocket_enabled())
    {
        websocket_console_enqueue_output(c);
    }

    // Always send to USB Serial JTAG as well
    // usb_serial_jtag_write_bytes(&c, 1, 0);
}

// Sense switches - return high byte of address bus (simple implementation)
static uint8_t sense(void)
{
    return (uint8_t)(bus_switches >> 8);
}

//-----------------------------------------------------------------------------
// Reset function for CPU virtual monitor
//-----------------------------------------------------------------------------
void altair_reset(void)
{
    if (g_disk_controller)
    {
        memset(memory, 0x00, 64 * 1024); // Clear Altair memory
        loadDiskLoader(0xFF00);          // Load disk boot loader at 0xFF00
        i8080_reset(&cpu, terminal_read, terminal_write, sense,
                    g_disk_controller, io_port_in, io_port_out);
        i8080_examine(&cpu, 0xFF00); // Reset to boot loader address
        bus_switches = cpu.address_bus;
    }
}

//-----------------------------------------------------------------------------
// Emulator Task (runs on Core 1)
//-----------------------------------------------------------------------------
#define EMULATOR_TASK_STACK_SIZE 3072 // observed HWM ~1.9 KB; 3 KB leaves >1 KB headroom
#define EMULATOR_TASK_PRIORITY 10     // High priority for consistent timing

// Panel update task. The VT100 renderer is heavy (PSRAM framebuffer + QSPI
// DMA) so it runs on Core 0 alongside the rest of the I/O (WiFi, BT, httpd,
// file transfer) to keep Core 1 reserved for the emulator. Priority is above
// the I/O workers but below the IDF httpd / esp_timer so network plumbing is
// not starved.
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
#define PANEL_UPDATE_TASK_STACK_SIZE 3072 // observed HWM ~1.4 KB
#define PANEL_UPDATE_TASK_PRIORITY 7
#define PANEL_UPDATE_TASK_CORE 0
#else
#define PANEL_UPDATE_TASK_STACK_SIZE 4096
#define PANEL_UPDATE_TASK_PRIORITY 4
#define PANEL_UPDATE_TASK_CORE 0
#endif

//-----------------------------------------------------------------------------
// Emulator Task (runs on Core 1)
//-----------------------------------------------------------------------------

// Panel update task
static void panel_update_task(void *pvParameters)
{
    (void)pvParameters;

    printf("Panel update task started on Core %d\n", xPortGetCoreID());

    TickType_t last_wake = xTaskGetTickCount();
    for (;;)
    {
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
        PANEL_CHECKPOINT(1); // before update_status
        vt100_terminal_update_status(cpu.address_bus, cpu.data_bus, cpu.cpuStatus);
        PANEL_CHECKPOINT(2); // before flush
        vt100_terminal_flush();
        PANEL_CHECKPOINT(3); // after flush
#else
        altair_panel_update(&cpu);
#endif
        // If we overran the period, reset to avoid "catch-up" bursts.
        if ((xTaskGetTickCount() - last_wake) > pdMS_TO_TICKS(PANEL_UPDATE_INTERVAL_MS))
        {
            last_wake = xTaskGetTickCount();
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PANEL_UPDATE_INTERVAL_MS));
    }
}

static void emulator_task(void *pvParameters)
{
    (void)pvParameters;

    printf("Emulator task started on Core %d\n", xPortGetCoreID());

    // Initialize file transfer driver (creates Core 0 socket task)
    files_io_init();
    chat_io_init();

    //-------------------------------------------------------------------------
    // Initialize SD card and disk system on Core 1 (same core as emulator)
    // This ensures synchronous disk I/O doesn't cross core boundaries
    //-------------------------------------------------------------------------
#ifdef SD_CARD_SUPPORT
    // Initialize SD card
    printf("Initializing SD card on Core 1...\n");
    if (!sdcard_esp32_init())
    {
        printf("SD card initialization failed!\n");
        printf("Possible causes:\n");
        printf("  - No SD card inserted\n");
        printf("  - SD card not formatted as FAT32\n");
        printf("  - Incorrect wiring\n");
        vTaskDelete(NULL);
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
    if (!esp32_sd_disk_load(0, DISK_A_PATH))
    {
        printf("  DISK_A load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_A loaded successfully\n");

    printf("Loading DISK_B: %s\n", DISK_B_PATH);
    if (!esp32_sd_disk_load(1, DISK_B_PATH))
    {
        printf("  DISK_B load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_B loaded successfully\n");

    printf("Loading DISK_C: %s\n", DISK_C_PATH);
    if (!esp32_sd_disk_load(2, DISK_C_PATH))
    {
        printf("  DISK_C load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_C loaded successfully\n");

    printf("Loading DISK_D: %s\n", DISK_D_PATH);
    if (!esp32_sd_disk_load(3, DISK_D_PATH))
    {
        printf("  DISK_D load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_D loaded successfully\n");

    // Set up disk controller for CPU
    static disk_controller_t disk_controller = {
        .disk_select = (port_out)esp32_sd_disk_select,
        .disk_status = (port_in)esp32_sd_disk_status,
        .disk_function = (port_out)esp32_sd_disk_function,
        .sector = (port_in)esp32_sd_disk_sector,
        .write = (port_out)esp32_sd_disk_write,
        .read = (port_in)esp32_sd_disk_read};
#else
    // Initialize disk controller (embedded flash disks)
    printf("Initializing disk controller...\n");
    pico_disk_init();

    // Load CPM disk image into drive 0 (DISK_A)
    printf("Loading DISK_A: cpm63k.dsk (embedded)\n");
    if (!pico_disk_load(0, cpm63k_dsk, cpm63k_dsk_len))
    {
        printf("  DISK_A load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_A loaded successfully (%u bytes)\n", cpm63k_dsk_len);

    // Load BDSC disk image into drive 1 (DISK_B)
    printf("Loading DISK_B: bdsc_v1_60.dsk (embedded)\n");
    if (!pico_disk_load(1, bdsc_v1_60_dsk, bdsc_v1_60_dsk_len))
    {
        printf("  DISK_B load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_B loaded successfully (%u bytes)\n", bdsc_v1_60_dsk_len);

    // Set up disk controller for CPU
    static disk_controller_t disk_controller = {
        .disk_select = (port_out)pico_disk_select,
        .disk_status = (port_in)pico_disk_status,
        .disk_function = (port_out)pico_disk_function,
        .sector = (port_in)pico_disk_sector,
        .write = (port_out)pico_disk_write,
        .read = (port_in)pico_disk_read};
#endif

    // Store disk controller reference for reset function
    g_disk_controller = &disk_controller;

    // Load disk boot loader ROM at 0xFF00
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Initialize CPU
    printf("Initializing Intel 8080 CPU...\n");
    i8080_reset(&cpu, terminal_read, terminal_write, sense,
                &disk_controller, io_port_in, io_port_out);

    // Set CPU to boot from disk loader at 0xFF00
    printf("Setting PC to 0xFF00 (disk boot loader)\n");
    i8080_examine(&cpu, 0xFF00);
    bus_switches = cpu.address_bus;

    // Set CPU to running mode
    cpu_state_set_mode(CPU_RUNNING);

    printf("\n");
    printf("Starting Altair 8800 emulation on Core 1...\n");
    printf("========================================\n\n");

    // Main emulation loop
    for (;;)
    {
        CPU_OPERATING_MODE mode = cpu_state_get_mode();
        switch (mode)
        {
        case CPU_RUNNING:
            // Hot path - execute 4000 cycles before checking state again
            for (int i = 0; i < 4000; ++i)
            {
                i8080_cycle(&cpu);
            }
            break;

        case CPU_STOPPED:
        {
            // Reuse terminal_read() so the monitor sees the same WS→BT→USB
            // priority and ANSI/Ctrl-M handling as the running CPU.
            // terminal_postprocess() converts Ctrl-M (28) into a mode
            // toggle and returns 0, so any non-zero byte is monitor input.
            uint8_t ch = terminal_read();
            if (ch != 0x00)
            {
                process_control_panel_commands_char(ch);
            }
            vTaskDelay(1); // Yield when stopped
            break;
        }

        default:
            vTaskDelay(1);
            break;
        }
    }
}

void app_main(void)
{
    // Initialize USB Serial JTAG driver for non-blocking terminal I/O
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 128, // Single-char input, just need small queue
        .tx_buffer_size = 128, // Single-char output, just need small queue
    };
    usb_serial_jtag_driver_install(&usb_config);

    // Brief delay to let USB enumerate
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
    printf("Core 0: terminal I/O, WiFi\n");
    printf("Core 1: Emulation, SD card I/O, VT100 display\n");

    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK)
    {
        printf("Flash size: %lu MB\n", flash_size / (1024 * 1024));
    }

    // Memory stats
    printf("\nMemory:\n");
    printf("  Free heap:     %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Min free heap: %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0)
    {
        printf("  PSRAM free:    %lu bytes\n", (unsigned long)psram_free);
    }
    printf("\n");

    // Initialize configuration (NVS storage)
    printf("Initializing configuration...\n");
    altair_config_init();

#ifdef SD_CARD_SUPPORT
    // Mount SD card BEFORE the LCD framebuffer is allocated. The SDMMC
    // controller's DMA descriptors must come from internal DRAM, and the
    // 480x160x2 framebuffer is large enough to starve that pool when
    // PSRAM is disabled. Subsequent calls to sdcard_esp32_init() are no-ops.
    printf("Mounting SD card early (pre-display)...\n");
    if (!sdcard_esp32_init())
    {
        printf("Early SD card mount failed; will retry from emulator task.\n");
    }
#endif

    // Initialize front panel display hardware
    printf("Initializing display hardware...\n");
#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    // AXS15231B display: initialise hardware then switch straight to VT100 mode.
    // The front-panel LED layout is not shown; the display acts as a text terminal.
    panel_display_init();
    vt100_terminal_init();
    altair_splash_show();
    int64_t splash_shown_us = esp_timer_get_time();
    printf("VT100 terminal ready: %d cols x %d rows\n", VT100_COLS, VT100_ROWS);
#else
    altair_panel_init();
#if CONFIG_ALTAIR_DISPLAY_ILI9341
    // Keep backlight off during WiFi connect to reduce cold-boot power draw
    altair_panel_set_backlight(0);
#endif
#endif

    // Start panel update task
    xTaskCreatePinnedToCore(
        panel_update_task,
        "panel_update",
        PANEL_UPDATE_TASK_STACK_SIZE,
        NULL,
        PANEL_UPDATE_TASK_PRIORITY,
        NULL,
        PANEL_UPDATE_TASK_CORE);

    bt_keyboard_init();
    config_run_boot_shell();

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    // Ensure the splash is visible for at least 5 seconds total, even if the
    // BT boot shell returned immediately (e.g. no serial monitor attached).
    const int64_t SPLASH_MIN_US = 5 * 1000 * 1000;
    int64_t elapsed_us = esp_timer_get_time() - splash_shown_us;
    if (elapsed_us < SPLASH_MIN_US)
    {
        vTaskDelay(pdMS_TO_TICKS((SPLASH_MIN_US - elapsed_us) / 1000));
    }

    // Splash done - clear display so the emulator boot output starts on a
    // blank terminal. Re-initialising the VT100 here would race with the
    // panel_update_task that is already running on Core 0, so just emit a
    // CSI 2J / CSI H clear+home through the normal putchar mutex path.
    static const char *clear_seq = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h";
    for (const char *p = clear_seq; *p; ++p)
    {
        vt100_terminal_putchar((uint8_t)*p);
    }
#endif

    // Start emulator task on Core 1 immediately; WiFi comes up independently.
    printf("Starting emulator task on Core 1...\n");
    xTaskCreatePinnedToCore(
        emulator_task,
        "altair_emu",
        EMULATOR_TASK_STACK_SIZE,
        NULL,
        EMULATOR_TASK_PRIORITY,
        NULL,
        1 // Pin to Core 1
    );

    // Setup WiFi asynchronously; the VT100 status bar is updated by WiFi events
    // when an IP address or captive portal address becomes available.
    wifi_setup_start();

    // app_main() can return - FreeRTOS scheduler continues running other tasks
    // The main task is automatically deleted by ESP-IDF when this function returns
}
