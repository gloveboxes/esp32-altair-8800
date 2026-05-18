/**
 * @file time_setup.c
 * @brief Local timezone offset setup and NVS-backed cache.
 */

#include "time_setup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TIME_SETUP_NVS_NAMESPACE "ENVIRONMENT"
#define TIME_SETUP_NVS_KEY_OFFSET "UTC_OFFSET"

static int s_offset_minutes = 0;
static bool s_loaded = false;

static void time_setup_drain_line(uint32_t idle_timeout_ms)
{
    int64_t idle_start = esp_timer_get_time();
    while ((esp_timer_get_time() - idle_start) < ((int64_t)idle_timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (len <= 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            return;
        }
        idle_start = esp_timer_get_time();
    }
}

static int time_setup_read_command_ms(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            return 0;
        }
        if (c == ' ' || c == '\t')
        {
            continue;
        }
        time_setup_drain_line(50);
        if (c >= 'a' && c <= 'z')
        {
            c = (uint8_t)(c - 'a' + 'A');
        }
        return c;
    }
    return -1;
}

static bool time_setup_read_line(const char *prompt, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0)
    {
        return false;
    }

    usb_serial_jtag_write_bytes((const uint8_t *)prompt, strlen(prompt), pdMS_TO_TICKS(100));

    size_t pos = 0;
    for (;;)
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            buffer[pos] = '\0';
            usb_serial_jtag_write_bytes((const uint8_t *)"\r\n", 2, pdMS_TO_TICKS(100));
            return true;
        }

        if ((c == '\b' || c == 0x7F) && pos > 0)
        {
            pos--;
            usb_serial_jtag_write_bytes((const uint8_t *)"\b \b", 3, pdMS_TO_TICKS(100));
            continue;
        }

        if (c >= 0x20 && c <= 0x7E && pos + 1 < buffer_len)
        {
            buffer[pos++] = (char)c;
            usb_serial_jtag_write_bytes(&c, 1, pdMS_TO_TICKS(100));
        }
    }
}

static void time_setup_trim_float(char *buffer)
{
    char *dot;
    char *end;

    dot = strchr(buffer, '.');
    if (!dot)
    {
        return;
    }

    end = buffer + strlen(buffer) - 1;
    while (end > dot + 1 && *end == '0')
    {
        *end = '\0';
        end--;
    }
}

static void time_setup_format_offset(int offset_minutes, char *buffer, size_t buffer_len)
{
    double offset_hours = (double)offset_minutes / 60.0;

    snprintf(buffer, buffer_len, "%.2f", offset_hours);
    time_setup_trim_float(buffer);
}

static bool time_setup_parse_offset(const char *text, int *offset_minutes)
{
    const char *p;
    char *end = NULL;
    double offset_hours;
    double offset_total;

    if (!text || !offset_minutes)
    {
        return false;
    }

    p = text;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    if (*p == '+' || *p == '-')
    {
        p++;
    }

    if ((*p < '0' || *p > '9') && *p != '.')
    {
        return false;
    }

    while (*p >= '0' && *p <= '9')
    {
        p++;
    }

    if (*p == '.')
    {
        p++;
        if (*p < '0' || *p > '9')
        {
            return false;
        }
        while (*p >= '0' && *p <= '9')
        {
            p++;
        }
    }

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p != '\0')
    {
        return false;
    }

    offset_hours = strtod(text, &end);
    if (end == text)
    {
        return false;
    }
    if (offset_hours < -14.0 || offset_hours > 14.0)
    {
        return false;
    }

    offset_total = offset_hours * 60.0;
    *offset_minutes = (int)(offset_total >= 0 ? offset_total + 0.5 : offset_total - 0.5);
    return true;
}

static void time_setup_print_status(void)
{
    char current[16];
    int offset_minutes = time_setup_get_offset_minutes();

    time_setup_format_offset(offset_minutes, current, sizeof(current));
    printf("\nTimezone settings\n");
    printf("  Current offset: %s\n", current);
}

static void time_setup_configure(void)
{
    char current[16];
    char input[16];
    int offset_minutes = time_setup_get_offset_minutes();

    time_setup_format_offset(offset_minutes, current, sizeof(current));
    printf("\nConfigure timezone\n");
    printf("Current offset: %s\n", current);
    printf("Enter UTC offset as decimal hours, e.g. 10.0, 8.5, or -8.5.\n");
    printf("Press Enter to keep the current value.\n");

    if (!time_setup_read_line("UTC offset: ", input, sizeof(input)))
    {
        return;
    }
    if (input[0] == '\0')
    {
        return;
    }

    if (!time_setup_parse_offset(input, &offset_minutes))
    {
        printf("Invalid timezone offset. Use a decimal value from -14.0 to 14.0.\n");
        return;
    }

    if (time_setup_save_offset_minutes(offset_minutes))
    {
        time_setup_format_offset(offset_minutes, current, sizeof(current));
        printf("Timezone offset saved: %s\n", current);
    }
}

static void time_setup_print_menu(void)
{
    printf("\nTimezone manager\n");
    printf("  C - configure timezone offset\n");
    printf("  S - show current settings\n");
    printf("  Q - return to main config menu\n");
}

void time_setup_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIME_SETUP_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        s_offset_minutes = 0;
        s_loaded = true;
        return;
    }

    char offset_text[16] = {0};
    size_t len = sizeof(offset_text);
    err = nvs_get_str(handle, TIME_SETUP_NVS_KEY_OFFSET, offset_text, &len);
    nvs_close(handle);

    int offset_minutes = 0;
    if (err == ESP_OK &&
        time_setup_parse_offset(offset_text, &offset_minutes))
    {
        s_offset_minutes = (int)offset_minutes;
    }
    else
    {
        s_offset_minutes = 0;
    }
    s_loaded = true;
}

void time_setup_reset_cache(void)
{
    s_offset_minutes = 0;
    s_loaded = true;
}

int time_setup_get_offset_minutes(void)
{
    if (!s_loaded)
    {
        time_setup_init();
    }

    return s_offset_minutes;
}

bool time_setup_save_offset_minutes(int offset_minutes)
{
    if (offset_minutes < TIME_SETUP_OFFSET_MINUTES_MIN ||
        offset_minutes > TIME_SETUP_OFFSET_MINUTES_MAX)
    {
        printf("[Time] Timezone offset out of range: %d minutes\n", offset_minutes);
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIME_SETUP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        printf("[Time] Failed to open NVS for timezone settings: %s\n", esp_err_to_name(err));
        return false;
    }

    char offset_text[16];
    time_setup_format_offset(offset_minutes, offset_text, sizeof(offset_text));

    err = nvs_set_str(handle, TIME_SETUP_NVS_KEY_OFFSET, offset_text);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        printf("[Time] Failed to save timezone settings: %s\n", esp_err_to_name(err));
        return false;
    }

    s_offset_minutes = offset_minutes;
    s_loaded = true;
    printf("[Time] Timezone offset saved\n");
    return true;
}

void time_setup_run_config_shell(void)
{
    time_setup_print_menu();

    for (;;)
    {
        printf("timezone> ");
        int cmd = time_setup_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("Timezone manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case 'C':
            time_setup_configure();
            time_setup_print_menu();
            break;

        case 'S':
            time_setup_print_status();
            time_setup_print_menu();
            break;

        case 'Q':
            printf("Leaving timezone manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use C, S, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}
