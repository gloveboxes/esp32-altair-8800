/**
 * @file main.c
 * @brief Altair 8800 Emulator for ESP32-S3
 *
 * Core Allocation:
 * ----------------
 * Core 0 (PRO_CPU / Default): Display and system I/O
 *   - USB Serial JTAG terminal I/O
 *   - Front panel LCD display updates
 *   - WiFi (when enabled)
 *   - FreeRTOS system tasks
 *
 * Core 1 (APP_CPU): Altair 8800 emulation and storage
 *   - Intel 8080 instruction execution
 *   - SD card disk I/O (synchronous with emulator)
 *   - Tight emulation loop with minimal interruption
 */

#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

// WiFi and configuration
#include "config.h"
#include "wifi.h"
#include "captive_portal.h"
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

// I/O port handlers
#include "port_drivers/io_ports.h"
#include "port_drivers/files_io.h"

// CPU state and virtual monitor
#include "cpu_state.h"

// ASCII mask for 7-bit terminal
#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

// Static disk controller reference for reset
static disk_controller_t* g_disk_controller = NULL;

// Global WiFi status
static bool g_wifi_connected = false;
static char g_ip_address[16] = {0};

// WebSocket console enable flag (set when WiFi connects)
// Using atomic for cross-core visibility (read by Core 1, written by Core 0)
static atomic_bool g_websocket_enabled = false;

// Cached copy of g_websocket_enabled for hot path (set once after task notification)
// Safe because: 1) Set before emulator starts, 2) Never changes after that,
// 3) Task notification provides memory barrier ensuring visibility
static bool s_ws_enabled_cached = false;

// Process character through ANSI escape sequence state machine
static uint8_t process_ansi_sequence(uint8_t ch)
{
    // Translate ANSI cursor sequences to the control keys CP/M expects (WordStar style).
    enum
    {
        KEY_STATE_NORMAL = 0,
        KEY_STATE_ESC,
        KEY_STATE_ESC_BRACKET,
        KEY_STATE_ESC_BRACKET_NUM
    };

    static uint8_t key_state = KEY_STATE_NORMAL;
    static uint8_t pending_key = 0;

    switch (key_state)
    {
        case KEY_STATE_NORMAL:
            if (ch == 0x1B)
            {
                key_state = KEY_STATE_ESC;
                return 0x00; // Start of escape sequence
            }
            if (ch == 0x7F || ch == 0x08)
            {
                return (uint8_t)CTRL_KEY('H'); // Map delete/backspace to Ctrl-H (0x08)
            }
            return ch;

        case KEY_STATE_ESC:
            if (ch == '[')
            {
                key_state = KEY_STATE_ESC_BRACKET;
                return 0x00; // Control sequence introducer
            }
            key_state = KEY_STATE_NORMAL;
            return ch; // Pass through unknown sequences

        case KEY_STATE_ESC_BRACKET:
            switch (ch)
            {
                case 'A':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('E'); // Up -> Ctrl-E
                case 'B':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('X'); // Down -> Ctrl-X
                case 'C':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('D'); // Right -> Ctrl-D
                case 'D':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('S'); // Left -> Ctrl-S
                case '2':
                    // Insert key sends ESC[2~ - need to consume the tilde
                    pending_key = (uint8_t)CTRL_KEY('O'); // Insert -> Ctrl-O
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                case '3':
                    // Delete key sends ESC[3~ - need to consume the tilde
                    pending_key = (uint8_t)CTRL_KEY('G'); // Delete -> Ctrl-G
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                default:
                    key_state = KEY_STATE_NORMAL;
                    return 0x00; // Ignore other sequences
            }

        case KEY_STATE_ESC_BRACKET_NUM:
            key_state = KEY_STATE_NORMAL;
            if (ch == '~')
            {
                // Return the pending key now that we've consumed the tilde
                uint8_t result = pending_key;
                pending_key = 0;
                return result;
            }
            pending_key = 0;
            return 0x00; // Unexpected character, ignore
    }

    key_state = KEY_STATE_NORMAL;
    return 0x00;
}

// Terminal read function - non-blocking
// Reads from WebSocket console if enabled, otherwise USB Serial JTAG
static uint8_t terminal_read(void)
{
    uint8_t ch = 0x00;
    
    if (s_ws_enabled_cached) {
        // WebSocket enabled - read from WebSocket console
        uint8_t ws_ch;
        if (websocket_console_try_dequeue_input(&ws_ch)) {
            ch = (uint8_t)(ws_ch & ASCII_MASK_7BIT);
        }
    } else {
        // WebSocket not enabled - fall back to USB Serial JTAG
        uint8_t c;
        int len = usb_serial_jtag_read_bytes(&c, 1, 0);
        if (len > 0) {
            ch = (uint8_t)(c & ASCII_MASK_7BIT);
            ch = process_ansi_sequence(ch);
        }
    }
    
    // Check for CTRL-M (ASCII 28) - toggle CPU mode
    if (ch == 28) {
        cpu_state_toggle_mode();
        return 0x00;  // Don't pass to emulator
    }
    
    return ch;
}

// Terminal write function
// Sends to both WebSocket client (if connected) and USB Serial JTAG
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT;  // Take first 7 bits only

    // Send to WebSocket client (if enabled)
    // Uses cached value to avoid atomic_load overhead in hot path
    if (s_ws_enabled_cached) {
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
        i8080_examine(&cpu, 0xFF00);     // Reset to boot loader address
        bus_switches = cpu.address_bus;
    }
}

//-----------------------------------------------------------------------------
// Emulator Task (runs on Core 1)
//-----------------------------------------------------------------------------
#define EMULATOR_TASK_STACK_SIZE  8192
#define EMULATOR_TASK_PRIORITY    10    // High priority for consistent timing

// Panel update task (runs on Core 0)
#define PANEL_UPDATE_TASK_STACK_SIZE  4096
#define PANEL_UPDATE_TASK_PRIORITY    4

// Emulator task handle for startup notification
static TaskHandle_t s_emulator_task = NULL;

//-----------------------------------------------------------------------------
// WiFi Setup
//-----------------------------------------------------------------------------

/**
 * @brief Check for config clear request during early boot
 *
 * Waits briefly for user to press 'C' to clear WiFi credentials
 * and enter captive portal mode. Press Enter to skip the wait.
 */
static bool check_config_clear_request(void)
{
    printf("\nWiFi credentials found in flash storage.\n");
    printf("Press 'C' within 5 seconds to clear config and enter AP mode...\n");
    printf("Press Enter to skip wait and connect now.\n");

    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < 5000000) {  // 5 seconds
        uint8_t c;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            if (c == 'c' || c == 'C') {
                printf("\nClearing WiFi configuration...\n");
                config_clear();
                return true;  // Config was cleared
            } else if (c == '\r' || c == '\n') {
                printf("\nSkipping wait...\n");
                break;  // Skip remaining wait time
            }
        }
    }
    printf("\n");
    return false;  // Config not cleared
}

/**
 * @brief Initialize WiFi - connect to stored network or start captive portal
 */
static void setup_wifi(void)
{
    // Initialize WiFi subsystem
    if (!wifi_init()) {
        printf("WiFi initialization failed!\n");
        return;
    }

    // Check for stored credentials
    if (config_exists()) {
        // Give user chance to clear config
        if (check_config_clear_request()) {
            // Config was cleared, fall through to captive portal
        } else {
            // Try to connect to stored network
            printf("Connecting to WiFi...\n");
            wifi_result_t result = wifi_connect();

            if (result == WIFI_RESULT_OK) {
                g_wifi_connected = true;
                wifi_get_ip(g_ip_address, sizeof(g_ip_address));
                printf("WiFi connected! IP: %s\n", g_ip_address);

                const char* mdns_name = get_mdns_hostname();
                if (mdns_name) {
                    printf("mDNS hostname: %s.local\n", mdns_name);
                }

                // Start WebSocket server for terminal access
                printf("Starting WebSocket terminal server...\n");
                websocket_console_init();
                if (websocket_console_start_server()) {
                    printf("WebSocket server started\n");
                    printf("Terminal page: http://%s/\n", g_ip_address);
                    atomic_store(&g_websocket_enabled, true);
                } else {
                    printf("Failed to start WebSocket server\n");
                }

                return;  // Successfully connected
            }

            printf("WiFi connection failed (result=%d), starting captive portal...\n", result);
        }
    } else {
        printf("No WiFi credentials configured - starting captive portal\n");
    }

    // Start captive portal for configuration
    if (captive_portal_start()) {
        // Show setup screen on LCD (panel may be off until WiFi connects)
        altair_panel_show_captive_portal(CAPTIVE_PORTAL_AP_SSID, captive_portal_get_ip());
        
        printf("\n");
        printf("==============================================\n");
        printf("  WiFi Setup Mode\n");
        printf("  Connect to: '%s'\n", CAPTIVE_PORTAL_AP_SSID);
        printf("  Then open: http://%s/\n", captive_portal_get_ip());
        printf("==============================================\n");
        printf("\n");

        // Run captive portal until configuration is saved (device will reboot)
        while (captive_portal_is_running()) {
            captive_portal_poll();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        printf("Failed to start captive portal!\n");
    }
}

//-----------------------------------------------------------------------------
// Emulator Task (runs on Core 1)
//-----------------------------------------------------------------------------

// Panel update task (runs on Core 0)
static void panel_update_task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        altair_panel_update(&cpu);
        // If we overran the period, reset to avoid "catch-up" bursts.
        if ((xTaskGetTickCount() - last_wake) > pdMS_TO_TICKS(PANEL_UPDATE_INTERVAL_MS)) {
            last_wake = xTaskGetTickCount();
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PANEL_UPDATE_INTERVAL_MS));
    }
}

static void emulator_task(void *pvParameters)
{
    (void)pvParameters;

    // Wait for WiFi setup to complete before starting emulator
    printf("Emulator task waiting for WiFi setup...\n");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    // Cache websocket enabled state for hot path - safe because:
    // 1) g_websocket_enabled is set before this notification
    // 2) Task notification includes memory barrier ensuring visibility
    // 3) Value never changes after emulator starts
    s_ws_enabled_cached = atomic_load(&g_websocket_enabled);
    
    printf("Emulator task started on Core %d\n", xPortGetCoreID());

    // Initialize file transfer driver (creates Core 0 socket task)
    files_io_init();

    //-------------------------------------------------------------------------
    // Initialize SD card and disk system on Core 1 (same core as emulator)
    // This ensures synchronous disk I/O doesn't cross core boundaries
    //-------------------------------------------------------------------------
#ifdef SD_CARD_SUPPORT
    // Initialize SD card
    printf("Initializing SD card on Core 1...\n");
    if (!sdcard_esp32_init()) {
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
    if (!esp32_sd_disk_load(0, DISK_A_PATH)) {
        printf("  DISK_A load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_A loaded successfully\n");

    printf("Loading DISK_B: %s\n", DISK_B_PATH);
    if (!esp32_sd_disk_load(1, DISK_B_PATH)) {
        printf("  DISK_B load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_B loaded successfully\n");

    printf("Loading DISK_C: %s\n", DISK_C_PATH);
    if (!esp32_sd_disk_load(2, DISK_C_PATH)) {
        printf("  DISK_C load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_C loaded successfully\n");

    printf("Loading DISK_D: %s\n", DISK_D_PATH);
    if (!esp32_sd_disk_load(3, DISK_D_PATH)) {
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
        .read = (port_in)esp32_sd_disk_read
    };
#else
    // Initialize disk controller (embedded flash disks)
    printf("Initializing disk controller...\n");
    pico_disk_init();

    // Load CPM disk image into drive 0 (DISK_A)
    printf("Loading DISK_A: cpm63k.dsk (embedded)\n");
    if (!pico_disk_load(0, cpm63k_dsk, cpm63k_dsk_len)) {
        printf("  DISK_A load failed!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("  DISK_A loaded successfully (%u bytes)\n", cpm63k_dsk_len);

    // Load BDSC disk image into drive 1 (DISK_B)
    printf("Loading DISK_B: bdsc_v1_60.dsk (embedded)\n");
    if (!pico_disk_load(1, bdsc_v1_60_dsk, bdsc_v1_60_dsk_len)) {
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
        .read = (port_in)pico_disk_read
    };
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
    for (;;) {
        CPU_OPERATING_MODE mode = cpu_state_get_mode();
        switch (mode)
        {
            case CPU_RUNNING:
                // Hot path - execute 4000 cycles before checking state again
                for (int i = 0; i < 4000; ++i) {
                    i8080_cycle(&cpu);
                }
                break;
                
            case CPU_STOPPED:
            {
                // CPU stopped - poll for monitor commands from WebSocket
                uint8_t ch;
                if (websocket_console_try_dequeue_input(&ch) && ch != 0x00) {
                    // Check for CTRL-M (ASCII 28) to toggle back to running mode
                    if (ch == 28) {
                        cpu_state_toggle_mode();
                    } else {
                        process_control_panel_commands_char(ch);
                    }
                }
                vTaskDelay(1);  // Yield when stopped
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
        .rx_buffer_size = 128,   // Single-char input, just need small queue
        .tx_buffer_size = 128,   // Single-char output, just need small queue
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
    printf("Core 0: Display, terminal I/O, WiFi\n");
    printf("Core 1: Emulation, SD card I/O\n");
    
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash size: %lu MB\n", flash_size / (1024 * 1024));
    }
    
    // Memory stats
    printf("\nMemory:\n");
    printf("  Free heap:     %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Min free heap: %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0) {
        printf("  PSRAM free:    %lu bytes\n", (unsigned long)psram_free);
    }
    printf("\n");

    // Initialize configuration (NVS storage)
    printf("Initializing configuration...\n");
    config_init();

    // Initialize front panel display on Core 0
    printf("Initializing front panel display on Core 0...\n");
    altair_panel_init();
    // Keep backlight off during WiFi connect to reduce cold-boot power draw
    altair_panel_set_backlight(0);

    // Start panel update task on Core 0
    xTaskCreatePinnedToCore(
        panel_update_task,
        "panel_update",
        PANEL_UPDATE_TASK_STACK_SIZE,
        NULL,
        PANEL_UPDATE_TASK_PRIORITY,
        NULL,
        0  // Pin to Core 0
    );
    
    // Start emulator task on Core 1 (will wait for WiFi setup)
    printf("Starting emulator task on Core 1...\n");
    xTaskCreatePinnedToCore(
        emulator_task,
        "altair_emu",
        EMULATOR_TASK_STACK_SIZE,
        NULL,
        EMULATOR_TASK_PRIORITY,
        &s_emulator_task,
        1  // Pin to Core 1
    );

    // Setup WiFi (may start captive portal if no credentials)
    // This blocks until WiFi is connected or captive portal exits
    setup_wifi();

    // Signal emulator to start using task notification
    printf("WiFi setup complete, starting emulator...\n");
    if (s_emulator_task) {
        xTaskNotifyGive(s_emulator_task);
    }

    // app_main() can return - FreeRTOS scheduler continues running other tasks
    // The main task is automatically deleted by ESP-IDF when this function returns
}
