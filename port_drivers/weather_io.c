/**
 * @file weather_io.c
 * @brief OpenWeatherMap I/O port driver.
 *
 * A background FreeRTOS task fetches current conditions and a near-term
 * forecast from OpenWeatherMap once at startup (after the network is
 * up) and then every WEATHER_REFRESH_INTERVAL_MS. Parsed string
 * fields live in a single PSRAM-resident state struct guarded by a
 * mutex; the Altair side reads them through ports 46/47/200 with no
 * blocking and no allocation.
 *
 * Altair port contract:
 *   OUT 46, field_id   -> selects one field; reply available on port 200
 *   IN  47             -> WEATHER_STATUS_*
 *   IN  200            -> next byte of selected field reply (NUL-terminated)
 */

#include "port_drivers/weather_io.h"

#include "config.h"
#include "json_scan.h"
#include "wifi.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WEATHER_TAG "WEATHER_IO"

#define WEATHER_REFRESH_INTERVAL_MS (5 * 60 * 1000)
#define WEATHER_RETRY_INTERVAL_MS   (60 * 1000)
#define WEATHER_NETWORK_SETTLE_MS   5000
#define WEATHER_HTTP_TIMEOUT_MS     15000
#define WEATHER_HTTP_HOST           "api.openweathermap.org"
/* Keep response buffers small. /weather is ~500B, /forecast?cnt=1 is ~2KB. */
#define WEATHER_RESP_MAX            4096
#define WEATHER_URL_MAX             320
#define WEATHER_TASK_STACK          6144
#define WEATHER_TASK_PRIORITY       4
#define WEATHER_TASK_CORE           0

#define WEATHER_STR_MAX             40
#define WEATHER_DESC_MAX            48
#define WEATHER_LOC_MAX             64
#define WEATHER_KEY_MAX             40
#define WEATHER_ERR_MAX             96
#define WEATHER_NUM_MAX             12
#define WEATHER_WHEN_MAX            20

typedef struct
{
    /* Pre-formatted strings; access guarded by s_mutex. */
    char city[WEATHER_STR_MAX];
    char cur_main[WEATHER_STR_MAX];
    char cur_desc[WEATHER_DESC_MAX];
    char cur_temp[WEATHER_NUM_MAX];
    char cur_feels[WEATHER_NUM_MAX];
    char cur_humid[WEATHER_NUM_MAX];
    char cur_wind[WEATHER_NUM_MAX];
    char fc_main[WEATHER_STR_MAX];
    char fc_desc[WEATHER_DESC_MAX];
    char fc_temp[WEATHER_NUM_MAX];
    char fc_feels[WEATHER_NUM_MAX];
    char fc_when[WEATHER_WHEN_MAX];
    char units[4];      /* "C" / "F" / "K" */
    char err[WEATHER_ERR_MAX];
    int64_t last_fetch_us;
    uint8_t status;
} weather_state_t;

static weather_state_t *s_state = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_network_available = false;
static volatile bool s_initialized = false;

/* Settings (loaded from NVS). Location can be a city name like
 * "London,UK" or "lat=...&lon=..." raw fragment. */
static char s_api_key[WEATHER_KEY_MAX + 1];
static char s_location[WEATHER_LOC_MAX + 1];
static char s_units_str[12]; /* "metric" / "imperial" / "standard" */

/* Active field reply, reused across reads from port 200. */
static char s_reply[WEATHER_DESC_MAX + 4];
static size_t s_reply_len = 0;
static size_t s_reply_pos = 0;

/* HTTP body capture (PSRAM). */
typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
} weather_http_ctx_t;

typedef struct
{
    char url[WEATHER_URL_MAX];
    char body[WEATHER_RESP_MAX];
} weather_workspace_t;

static weather_workspace_t *s_workspace = NULL;

/* ---- forward decls ---- */
static void weather_task(void *arg);
static bool weather_fetch_once(void);
static void weather_load_settings(void);
static void weather_set_units_letter(void);
static void weather_set_error(const char *fmt, ...);
static void weather_clear_data_locked(void);
static esp_err_t weather_http_event(esp_http_client_event_t *evt);
static bool weather_http_get(const char *url, weather_http_ctx_t *ctx);
static bool weather_parse_current(const char *json);
static bool weather_parse_forecast(const char *json);
static void weather_capitalize(char *s);

static int64_t weather_now_us(void)
{
    return esp_timer_get_time();
}

void weather_io_init(void)
{
    if (s_initialized)
    {
        return;
    }

    s_state = heap_caps_calloc(1, sizeof(weather_state_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state)
    {
        s_state = calloc(1, sizeof(weather_state_t));
    }
    if (!s_state)
    {
        ESP_LOGE(WEATHER_TAG, "Failed to allocate weather state");
        return;
    }

    s_workspace = heap_caps_calloc(1, sizeof(weather_workspace_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_workspace)
    {
        s_workspace = calloc(1, sizeof(weather_workspace_t));
    }
    if (!s_workspace)
    {
        ESP_LOGE(WEATHER_TAG, "Failed to allocate weather workspace");
        free(s_state);
        s_state = NULL;
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        ESP_LOGE(WEATHER_TAG, "Failed to allocate weather mutex");
        free(s_workspace);
        s_workspace = NULL;
        free(s_state);
        s_state = NULL;
        return;
    }

    weather_load_settings();
    weather_set_units_letter();
    s_state->status = WEATHER_STATUS_NONE;

    BaseType_t ok = xTaskCreatePinnedToCore(weather_task, "weather_io",
                                            WEATHER_TASK_STACK, NULL,
                                            WEATHER_TASK_PRIORITY,
                                            &s_task, WEATHER_TASK_CORE);
    if (ok != pdPASS)
    {
        ESP_LOGE(WEATHER_TAG, "Failed to create weather task");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        free(s_workspace);
        s_workspace = NULL;
        free(s_state);
        s_state = NULL;
        return;
    }

    s_initialized = true;
    printf("[Weather] driver initialized on ports 46/47\n");
}

void weather_io_set_network_available(bool available)
{
    bool was = s_network_available;
    s_network_available = available;
    if (available && !was && s_task)
    {
        /* Wake task to start its network-up settle delay. */
        xTaskNotifyGive(s_task);
    }
}

/* ---- task ---- */

static void weather_task(void *arg)
{
    (void)arg;
    bool network_ready = false;

    for (;;)
    {
        /* Wait for network. Re-check every second so we don't burn CPU. */
        while (!s_network_available || !wifi_is_connected())
        {
            network_ready = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }

        if (!network_ready)
        {
            vTaskDelay(pdMS_TO_TICKS(WEATHER_NETWORK_SETTLE_MS));
            if (!s_network_available || !wifi_is_connected())
            {
                continue;
            }
            network_ready = true;
        }

        if (s_api_key[0] == '\0' || s_location[0] == '\0')
        {
            weather_set_error("API key or location not configured");
            /* Nothing useful to retry until settings change. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_state->status = WEATHER_STATUS_FETCHING;
            xSemaphoreGive(s_mutex);
        }

        bool ok = weather_fetch_once();
        TickType_t wait = pdMS_TO_TICKS(ok ? WEATHER_REFRESH_INTERVAL_MS
                                          : WEATHER_RETRY_INTERVAL_MS);
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

/* ---- HTTP ---- */

static esp_err_t weather_http_event(esp_http_client_event_t *evt)
{
    weather_http_ctx_t *ctx = (weather_http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data && evt->data_len > 0)
    {
        size_t copy = (size_t)evt->data_len;
        if (ctx->len + copy + 1 > ctx->cap)
        {
            copy = (ctx->cap > ctx->len + 1) ? (ctx->cap - ctx->len - 1) : 0;
            ctx->truncated = true;
        }
        if (copy > 0)
        {
            memcpy(ctx->buf + ctx->len, evt->data, copy);
            ctx->len += copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool weather_http_get(const char *url, weather_http_ctx_t *ctx)
{
    ctx->len = 0;
    ctx->truncated = false;
    if (ctx->buf && ctx->cap > 0)
    {
        ctx->buf[0] = '\0';
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = weather_http_event,
        .user_data = ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        weather_set_error("HTTP client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        weather_set_error("HTTP error: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200)
    {
        weather_set_error("HTTP %d from OpenWeatherMap", status);
        return false;
    }
    if (ctx->truncated)
    {
        ESP_LOGW(WEATHER_TAG, "response truncated to %u bytes", (unsigned)ctx->len);
    }
    return true;
}

/* ---- fetch ---- */

static void weather_build_url(char *url, size_t url_len, const char *path, int cnt)
{
    /* Location may be either a "q=..." style string or a "lat=...&lon=..."
     * raw fragment. We pass it through as a query parameter unchanged
     * unless it looks like coordinates. */
    bool is_coords = (strchr(s_location, '=') != NULL);
    if (cnt > 0)
    {
        snprintf(url, url_len,
                 "https://%s/data/2.5/%s?%s%s&appid=%s&units=%s&cnt=%d",
                 WEATHER_HTTP_HOST, path,
                 is_coords ? "" : "q=",
                 s_location, s_api_key, s_units_str, cnt);
    }
    else
    {
        snprintf(url, url_len,
                 "https://%s/data/2.5/%s?%s%s&appid=%s&units=%s",
                 WEATHER_HTTP_HOST, path,
                 is_coords ? "" : "q=",
                 s_location, s_api_key, s_units_str);
    }
}

static bool weather_fetch_once(void)
{
    if (!s_workspace)
    {
        weather_set_error("Weather workspace unavailable");
        return false;
    }

    char *url = s_workspace->url;
    weather_http_ctx_t ctx = {0};
    ctx.cap = sizeof(s_workspace->body);
    ctx.buf = s_workspace->body;

    bool ok = false;

    weather_build_url(url, WEATHER_URL_MAX, "weather", 0);
    if (weather_http_get(url, &ctx) && weather_parse_current(ctx.buf))
    {
        weather_build_url(url, WEATHER_URL_MAX, "forecast", 1);
        if (weather_http_get(url, &ctx))
        {
            weather_parse_forecast(ctx.buf); /* forecast is best-effort */
        }

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_state->status = WEATHER_STATUS_READY;
            s_state->last_fetch_us = weather_now_us();
            s_state->err[0] = '\0';
            xSemaphoreGive(s_mutex);
        }
        ok = true;
    }
    else
    {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_state->status = WEATHER_STATUS_ERROR;
            xSemaphoreGive(s_mutex);
        }
    }

    return ok;
}

static void weather_capitalize(char *s)
{
    if (s && s[0] >= 'a' && s[0] <= 'z')
    {
        s[0] = (char)(s[0] - 'a' + 'A');
    }
}

static bool weather_parse_current(const char *json)
{
    if (!json || !strchr(json, '{'))
    {
        weather_set_error("Invalid current JSON");
        return false;
    }
    json_scan_range_t root = { json, json + strlen(json) };

    int cod = 0;
    if (json_scan_get_int(root, "cod", &cod) && cod != 200)
    {
        char msg[WEATHER_ERR_MAX];
        json_scan_get_string(root, "message", msg, sizeof(msg));
        weather_set_error("OWM cod=%d: %s", cod, msg);
        return false;
    }

    char city[WEATHER_STR_MAX] = "";
    char cur_main[WEATHER_STR_MAX] = "";
    char cur_desc[WEATHER_DESC_MAX] = "";
    char cur_temp[WEATHER_NUM_MAX] = "";
    char cur_feels[WEATHER_NUM_MAX] = "";
    char cur_humid[WEATHER_NUM_MAX] = "";
    char cur_wind[WEATHER_NUM_MAX] = "";

    json_scan_get_string(root, "name", city, sizeof(city));

    json_scan_range_t weather_obj = {0};
    if (json_scan_first_array_object(root, "weather", &weather_obj))
    {
        json_scan_get_string(weather_obj, "main", cur_main, sizeof(cur_main));
        json_scan_get_string(weather_obj, "description", cur_desc, sizeof(cur_desc));
        weather_capitalize(cur_desc);
    }

    json_scan_range_t main_obj = {0};
    if (json_scan_object(root, "main", &main_obj))
    {
        json_scan_get_number(main_obj, "temp", cur_temp, sizeof(cur_temp));
        json_scan_get_number(main_obj, "feels_like", cur_feels, sizeof(cur_feels));
        json_scan_get_number(main_obj, "humidity", cur_humid, sizeof(cur_humid));
    }

    json_scan_range_t wind_obj = {0};
    if (json_scan_object(root, "wind", &wind_obj))
    {
        json_scan_get_number(wind_obj, "speed", cur_wind, sizeof(cur_wind));
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        strncpy(s_state->city, city, sizeof(s_state->city));
        strncpy(s_state->cur_main, cur_main, sizeof(s_state->cur_main));
        strncpy(s_state->cur_desc, cur_desc, sizeof(s_state->cur_desc));
        strncpy(s_state->cur_temp, cur_temp, sizeof(s_state->cur_temp));
        strncpy(s_state->cur_feels, cur_feels, sizeof(s_state->cur_feels));
        strncpy(s_state->cur_humid, cur_humid, sizeof(s_state->cur_humid));
        strncpy(s_state->cur_wind, cur_wind, sizeof(s_state->cur_wind));
        xSemaphoreGive(s_mutex);
    }
    return true;
}

static bool weather_parse_forecast(const char *json)
{
    if (!json || !strchr(json, '{'))
    {
        return false;
    }
    json_scan_range_t root = { json, json + strlen(json) };

    json_scan_range_t item = {0};
    if (!json_scan_first_array_object(root, "list", &item))
    {
        return false;
    }

    char fc_main[WEATHER_STR_MAX] = "";
    char fc_desc[WEATHER_DESC_MAX] = "";
    char fc_temp[WEATHER_NUM_MAX] = "";
    char fc_feels[WEATHER_NUM_MAX] = "";
    char fc_when[WEATHER_WHEN_MAX] = "";

    json_scan_range_t weather_obj = {0};
    if (json_scan_first_array_object(item, "weather", &weather_obj))
    {
        json_scan_get_string(weather_obj, "main", fc_main, sizeof(fc_main));
        json_scan_get_string(weather_obj, "description", fc_desc, sizeof(fc_desc));
        weather_capitalize(fc_desc);
    }

    json_scan_range_t main_obj = {0};
    if (json_scan_object(item, "main", &main_obj))
    {
        json_scan_get_number(main_obj, "temp", fc_temp, sizeof(fc_temp));
        json_scan_get_number(main_obj, "feels_like", fc_feels, sizeof(fc_feels));
    }
    json_scan_get_string(item, "dt_txt", fc_when, sizeof(fc_when));

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        strncpy(s_state->fc_main, fc_main, sizeof(s_state->fc_main));
        strncpy(s_state->fc_desc, fc_desc, sizeof(s_state->fc_desc));
        strncpy(s_state->fc_temp, fc_temp, sizeof(s_state->fc_temp));
        strncpy(s_state->fc_feels, fc_feels, sizeof(s_state->fc_feels));
        strncpy(s_state->fc_when, fc_when, sizeof(s_state->fc_when));
        xSemaphoreGive(s_mutex);
    }
    return true;
}

/* ---- helpers ---- */

static void weather_clear_data_locked(void)
{
    s_state->city[0] = '\0';
    s_state->cur_main[0] = '\0';
    s_state->cur_desc[0] = '\0';
    s_state->cur_temp[0] = '\0';
    s_state->cur_feels[0] = '\0';
    s_state->cur_humid[0] = '\0';
    s_state->cur_wind[0] = '\0';
    s_state->fc_main[0] = '\0';
    s_state->fc_desc[0] = '\0';
    s_state->fc_temp[0] = '\0';
    s_state->fc_feels[0] = '\0';
    s_state->fc_when[0] = '\0';
}

static void weather_set_error(const char *fmt, ...)
{
    if (!s_state || !s_mutex) return;
    char tmp[WEATHER_ERR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        strncpy(s_state->err, tmp, sizeof(s_state->err) - 1);
        s_state->err[sizeof(s_state->err) - 1] = '\0';
        s_state->status = WEATHER_STATUS_ERROR;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGW(WEATHER_TAG, "%s", tmp);
}

static void weather_set_units_letter(void)
{
    if (!s_state) return;
    if (strcmp(s_units_str, "imperial") == 0)
    {
        strcpy(s_state->units, "F");
    }
    else if (strcmp(s_units_str, "standard") == 0)
    {
        strcpy(s_state->units, "K");
    }
    else
    {
        strcpy(s_state->units, "C");
    }
}

/* ---- settings (NVS via config.c helpers) ---- */

static void weather_load_settings(void)
{
    s_api_key[0] = '\0';
    s_location[0] = '\0';
    s_units_str[0] = '\0';
    config_load_weather_settings(s_api_key, sizeof(s_api_key),
                                 s_location, sizeof(s_location),
                                 s_units_str, sizeof(s_units_str));
    if (s_units_str[0] == '\0')
    {
        strcpy(s_units_str, "metric");
    }
}

/* ---- boot-shell config ---- */

static void weather_serial_drain_line(uint32_t idle_timeout_ms)
{
    int64_t idle_start = esp_timer_get_time();
    while ((esp_timer_get_time() - idle_start) < ((int64_t)idle_timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (len <= 0) continue;
        if (c == '\r' || c == '\n') return;
        idle_start = esp_timer_get_time();
    }
}

static int weather_serial_read_command_ms(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        if (c == '\r' || c == '\n') return 0;
        if (c == ' ' || c == '\t') continue;
        weather_serial_drain_line(50);
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        return c;
    }
    return -1;
}

static void weather_serial_write(const char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0)
    {
        int w = usb_serial_jtag_write_bytes((const uint8_t *)s, len, pdMS_TO_TICKS(100));
        if (w <= 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        s += w; len -= (size_t)w;
    }
}

static bool weather_serial_read_line(const char *prompt, char *buffer, size_t buffer_len, bool mask)
{
    size_t length = 0;
    if (!buffer || buffer_len == 0) return false;
    buffer[0] = '\0';
    weather_serial_write(prompt);
    for (;;)
    {
        uint8_t c = 0;
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (c == '\r' || c == '\n')
        {
            weather_serial_write("\r\n");
            buffer[length] = '\0';
            return true;
        }
        if (c == 0x08 || c == 0x7F)
        {
            if (length > 0)
            {
                length--;
                buffer[length] = '\0';
                weather_serial_write("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126 || length + 1 >= buffer_len) continue;
        buffer[length++] = (char)c;
        buffer[length] = '\0';
        uint8_t out = mask ? (uint8_t)'*' : c;
        usb_serial_jtag_write_bytes(&out, 1, pdMS_TO_TICKS(100));
    }
}

static void weather_print_menu(void)
{
    printf("\nOpenWeatherMap manager\n");
    printf("  1 - edit API key, location, units\n");
    printf("  S - show current settings\n");
    printf("  Q - return to main config menu\n");
}

static void weather_print_settings(void)
{
    printf("\nOpenWeatherMap settings\n");
    printf("  API key: %s\n", s_api_key[0] ? "set" : "not set");
    printf("  Location: %s\n", s_location[0] ? s_location : "(not set)");
    printf("  Units: %s\n", s_units_str[0] ? s_units_str : "metric");
}

static void weather_configure_settings(void)
{
    char key[WEATHER_KEY_MAX + 1];
    char location[WEATHER_LOC_MAX + 1];
    char units[12];

    printf("\nEdit OpenWeatherMap settings\n");
    printf("Current API key: %s\n", s_api_key[0] ? "set" : "not set");
    printf("Current location: %s\n", s_location[0] ? s_location : "(not set)");
    printf("Current units: %s\n", s_units_str);
    printf("Press Enter at any prompt to keep the existing value, '-' to clear.\n");

    if (!weather_serial_read_line("api key> ", key, sizeof(key), true))
        return;
    if (key[0] == '\0')
    {
        strncpy(key, s_api_key, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    }
    else if (strcmp(key, "-") == 0)
    {
        key[0] = '\0';
    }

    printf("Location examples: London,UK   Seattle,US   lat=47.6&lon=-122.3\n");
    if (!weather_serial_read_line("location> ", location, sizeof(location), false))
        return;
    if (location[0] == '\0')
    {
        strncpy(location, s_location, sizeof(location) - 1);
        location[sizeof(location) - 1] = '\0';
    }
    else if (strcmp(location, "-") == 0)
    {
        location[0] = '\0';
    }

    printf("Units: metric (C), imperial (F), or standard (K)\n");
    if (!weather_serial_read_line("units> ", units, sizeof(units), false))
        return;
    if (units[0] == '\0')
    {
        strncpy(units, s_units_str, sizeof(units) - 1);
        units[sizeof(units) - 1] = '\0';
    }
    if (strcmp(units, "metric") != 0 &&
        strcmp(units, "imperial") != 0 &&
        strcmp(units, "standard") != 0)
    {
        printf("Unknown units '%s' - keeping '%s'\n", units, s_units_str);
        strncpy(units, s_units_str, sizeof(units) - 1);
        units[sizeof(units) - 1] = '\0';
    }

    if (config_save_weather_settings(key, location, units))
    {
        weather_load_settings();
        if (s_state)
        {
            weather_set_units_letter();
            if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
            {
                weather_clear_data_locked();
                s_state->status = WEATHER_STATUS_NONE;
                xSemaphoreGive(s_mutex);
            }
            if (s_task) xTaskNotifyGive(s_task);
        }
        printf("Weather settings saved.\n");
    }
}

void weather_io_run_config_shell(void)
{
    weather_load_settings();
    weather_print_menu();

    for (;;)
    {
        printf("weather> ");
        int cmd = weather_serial_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("OpenWeatherMap manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case '1':
            weather_configure_settings();
            weather_print_menu();
            break;

        case 'S':
            weather_print_settings();
            weather_print_menu();
            break;

        case 'Q':
            printf("Leaving OpenWeatherMap manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use 1, S, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}

/* ---- port surface ---- */

static const char *weather_field_locked(uint8_t field, char *scratch, size_t scratch_len)
{
    switch (field)
    {
        case WEATHER_FIELD_CITY:      return s_state->city;
        case WEATHER_FIELD_CUR_MAIN:  return s_state->cur_main;
        case WEATHER_FIELD_CUR_DESC:  return s_state->cur_desc;
        case WEATHER_FIELD_CUR_TEMP:  return s_state->cur_temp;
        case WEATHER_FIELD_CUR_FEELS: return s_state->cur_feels;
        case WEATHER_FIELD_CUR_HUMID: return s_state->cur_humid;
        case WEATHER_FIELD_CUR_WIND:  return s_state->cur_wind;
        case WEATHER_FIELD_FC_MAIN:   return s_state->fc_main;
        case WEATHER_FIELD_FC_DESC:   return s_state->fc_desc;
        case WEATHER_FIELD_FC_TEMP:   return s_state->fc_temp;
        case WEATHER_FIELD_FC_FEELS:  return s_state->fc_feels;
        case WEATHER_FIELD_FC_WHEN:   return s_state->fc_when;
        case WEATHER_FIELD_UNITS:     return s_state->units;
        case WEATHER_FIELD_ERROR:     return s_state->err;
        case WEATHER_FIELD_AGE_SEC:
        {
            int64_t age = 0;
            if (s_state->last_fetch_us > 0)
            {
                age = (weather_now_us() - s_state->last_fetch_us) / 1000000;
                if (age < 0) age = 0;
            }
            snprintf(scratch, scratch_len, "%lld", (long long)age);
            return scratch;
        }
        default:
            scratch[0] = '\0';
            return scratch;
    }
}

size_t weather_output(int port, uint8_t data, char *buffer, size_t buffer_length)
{
    if (!s_initialized || !s_state) return 0;

    if (port == WEATHER_PORT_FIELD)
    {
        char scratch[WEATHER_NUM_MAX];
        s_reply[0] = '\0';
        s_reply_len = 0;
        s_reply_pos = 0;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            const char *src = weather_field_locked(data, scratch, sizeof(scratch));
            strncpy(s_reply, src ? src : "", sizeof(s_reply) - 1);
            s_reply[sizeof(s_reply) - 1] = '\0';
            xSemaphoreGive(s_mutex);
        }
        s_reply_len = strlen(s_reply);

        /* If caller provided the standard request buffer, populate it
         * so port 200 reads work via the existing io_ports.c plumbing. */
        if (buffer && buffer_length > 0)
        {
            size_t n = s_reply_len;
            if (n >= buffer_length) n = buffer_length - 1;
            memcpy(buffer, s_reply, n);
            buffer[n] = '\0';
            return n;
        }
        return 0;
    }

    return 0;
}

uint8_t weather_input(uint8_t port)
{
    if (!s_initialized || !s_state) return WEATHER_STATUS_NONE;
    if (port == WEATHER_PORT_STATUS)
    {
        uint8_t s = WEATHER_STATUS_NONE;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s = s_state->status;
            xSemaphoreGive(s_mutex);
        }
        return s;
    }
    return 0;
}
