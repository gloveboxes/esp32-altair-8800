/**
 * @file weather_io.c
 * @brief Host implementation of the OpenWeatherMap port driver.
 *
 * Mirrors the ESP32 driver semantics (ports 46 / 47, same field IDs and
 * status codes). HTTP is performed with libcurl on a dedicated pthread;
 * settings come from the env_io text store keys OWM_KEY / OWM_LOCATION /
 * OWM_UNITS so the CP/M ENV editor can configure the host driver.
 *
 * If libcurl is not available at build time, the driver still installs
 * but reports a permanent error and never fetches.
 */

#define _POSIX_C_SOURCE 200809L

#include "weather_io.h"

#include "environment_io.h"
#include "../../port_drivers/json_scan.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAVE_LIBCURL
#include <pthread.h>
#include <curl/curl.h>
#elif defined(_WIN32)
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;

#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0

static int pthread_mutex_lock(pthread_mutex_t* mutex)
{
    (void)mutex;
    return 0;
}

static int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    (void)mutex;
    return 0;
}
#endif

/* ---- constants ---- */

#define WEATHER_REFRESH_SECS        (20 * 60)
#define WEATHER_STARTUP_RETRY_SECS  (2 * 60)
#define WEATHER_RETRY_WINDOW_SECS   (10 * 60)
#define WEATHER_HTTP_TIMEOUT_SECS   15
#define WEATHER_HTTP_HOST           "api.openweathermap.org"

#define WEATHER_URL_MAX             320
#define WEATHER_BODY_MAX            8192
#define WEATHER_STR_MAX             40
#define WEATHER_DESC_MAX            48
#define WEATHER_LOC_MAX             64
#define WEATHER_KEY_MAX             40
#define WEATHER_UNITS_MAX           12
#define WEATHER_ERR_MAX             96
#define WEATHER_NUM_MAX             12
#define WEATHER_REPLY_SCRATCH_MAX   24
#define WEATHER_WHEN_MAX            20
#define WEATHER_REPLY_MAX           80

/* ---- state ---- */

typedef struct
{
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
    char units[4];
    char err[WEATHER_ERR_MAX];
    int64_t last_fetch_us;
    uint8_t status;
} weather_state_t;

static weather_state_t s_state;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t       s_thread;
static bool            s_initialized = false;
static bool            s_thread_started = false;

static char s_api_key[WEATHER_KEY_MAX + 1];
static char s_location[WEATHER_LOC_MAX + 1];
static char s_units_str[WEATHER_UNITS_MAX + 1];

static char s_reply[WEATHER_REPLY_MAX];

/* ---- forward decls ---- */

static int64_t weather_now_us(void);
static void    weather_set_error(const char *fmt, ...);
static void    weather_set_units_letter_locked(void);
static bool    weather_load_settings_locked(void);

#ifdef HAVE_LIBCURL
static bool    weather_fetch_once(void);
static bool    weather_parse_current(const char *json);
static bool    weather_parse_forecast(const char *json);
static void    weather_capitalize(char *s);
static void   *weather_thread_fn(void *arg);
static void    weather_wait_seconds_locked(int seconds);
#endif

/* ---- utility ---- */

static int64_t weather_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
#endif
}

static void weather_set_units_letter_locked(void)
{
    if (strcmp(s_units_str, "imperial") == 0)
    {
        strcpy(s_state.units, "F");
    }
    else if (strcmp(s_units_str, "standard") == 0)
    {
        strcpy(s_state.units, "K");
    }
    else
    {
        strcpy(s_state.units, "C");
    }
}

static void weather_copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

/**
 * Re-read OWM_KEY / OWM_LOCATION / OWM_UNITS from the env_io store.
 * Returns true when api key and location are both populated.
 * Caller must hold s_mutex.
 */
static bool weather_load_settings_locked(void)
{
    char tmp[WEATHER_LOC_MAX + 1];

    s_api_key[0] = '\0';
    s_location[0] = '\0';
    s_units_str[0] = '\0';

    if (environment_io_get("OWM_KEY", tmp, sizeof(tmp)))
    {
        weather_copy_str(s_api_key, sizeof(s_api_key), tmp);
    }
    if (environment_io_get("OWM_LOCATION", tmp, sizeof(tmp)))
    {
        weather_copy_str(s_location, sizeof(s_location), tmp);
    }
    if (environment_io_get("OWM_UNITS", tmp, sizeof(tmp)))
    {
        weather_copy_str(s_units_str, sizeof(s_units_str), tmp);
    }
    if (s_units_str[0] == '\0')
    {
        strcpy(s_units_str, "metric");
    }
    weather_set_units_letter_locked();
    return (s_api_key[0] != '\0' && s_location[0] != '\0');
}

static void weather_set_error(const char *fmt, ...)
{
    char tmp[WEATHER_ERR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&s_mutex);
    weather_copy_str(s_state.err, sizeof(s_state.err), tmp);
    s_state.status = WEATHER_STATUS_ERROR;
    pthread_mutex_unlock(&s_mutex);

    fprintf(stderr, "[weather] %s\n", tmp);
}

/* ---- libcurl path ---- */

#ifdef HAVE_LIBCURL

typedef struct
{
    char  *buf;
    size_t cap;
    size_t len;
} weather_http_ctx_t;

static size_t weather_curl_write(void *contents, size_t size, size_t nmemb, void *userp)
{
    weather_http_ctx_t *ctx = (weather_http_ctx_t *)userp;
    size_t incoming = size * nmemb;
    if (ctx->len + incoming + 1 > ctx->cap)
    {
        incoming = (ctx->cap > ctx->len + 1) ? ctx->cap - ctx->len - 1 : 0;
    }
    if (incoming > 0)
    {
        memcpy(ctx->buf + ctx->len, contents, incoming);
        ctx->len += incoming;
        ctx->buf[ctx->len] = '\0';
    }
    return size * nmemb;
}

static bool weather_http_get(const char *url, weather_http_ctx_t *ctx)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        weather_set_error("curl_easy_init failed");
        return false;
    }
    ctx->len = 0;
    if (ctx->buf && ctx->cap > 0) ctx->buf[0] = '\0';

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, weather_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)WEATHER_HTTP_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "altair8800-local/1.0");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    if (rc == CURLE_OK)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
    {
        weather_set_error("HTTP failed: %s",
                          errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return false;
    }
    if (http_code < 200 || http_code >= 300)
    {
        weather_set_error("HTTP %ld", http_code);
        return false;
    }
    return true;
}

static void weather_build_url(char *url, size_t url_len,
                              const char *path, int cnt,
                              const char *api_key,
                              const char *location,
                              const char *units)
{
    const char *qprefix = "q=";
    const char *qval    = location;
    if (strncmp(location, "lat=", 4) == 0)
    {
        qprefix = "";
    }

    if (cnt > 0)
    {
        snprintf(url, url_len,
                 "https://%s/data/2.5/%s?%s%s&appid=%s&units=%s&cnt=%d",
                 WEATHER_HTTP_HOST, path, qprefix, qval,
                 api_key, units, cnt);
    }
    else
    {
        snprintf(url, url_len,
                 "https://%s/data/2.5/%s?%s%s&appid=%s&units=%s",
                 WEATHER_HTTP_HOST, path, qprefix, qval,
                 api_key, units);
    }
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
        msg[0] = '\0';
        json_scan_get_string(root, "message", msg, sizeof(msg));
        weather_set_error("OWM cod=%d: %s", cod, msg);
        return false;
    }

    char city[WEATHER_STR_MAX]    = "";
    char cur_main[WEATHER_STR_MAX] = "";
    char cur_desc[WEATHER_DESC_MAX] = "";
    char cur_temp[WEATHER_NUM_MAX]  = "";
    char cur_feels[WEATHER_NUM_MAX] = "";
    char cur_humid[WEATHER_NUM_MAX] = "";
    char cur_wind[WEATHER_NUM_MAX]  = "";

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

    pthread_mutex_lock(&s_mutex);
    weather_copy_str(s_state.city,      sizeof(s_state.city),      city);
    weather_copy_str(s_state.cur_main,  sizeof(s_state.cur_main),  cur_main);
    weather_copy_str(s_state.cur_desc,  sizeof(s_state.cur_desc),  cur_desc);
    weather_copy_str(s_state.cur_temp,  sizeof(s_state.cur_temp),  cur_temp);
    weather_copy_str(s_state.cur_feels, sizeof(s_state.cur_feels), cur_feels);
    weather_copy_str(s_state.cur_humid, sizeof(s_state.cur_humid), cur_humid);
    weather_copy_str(s_state.cur_wind,  sizeof(s_state.cur_wind),  cur_wind);
    pthread_mutex_unlock(&s_mutex);
    return true;
}

static bool weather_parse_forecast(const char *json)
{
    if (!json || !strchr(json, '{')) return false;
    json_scan_range_t root = { json, json + strlen(json) };

    json_scan_range_t item = {0};
    if (!json_scan_first_array_object(root, "list", &item)) return false;

    char fc_main[WEATHER_STR_MAX]   = "";
    char fc_desc[WEATHER_DESC_MAX]  = "";
    char fc_temp[WEATHER_NUM_MAX]   = "";
    char fc_feels[WEATHER_NUM_MAX]  = "";
    char fc_when[WEATHER_WHEN_MAX]  = "";

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

    pthread_mutex_lock(&s_mutex);
    weather_copy_str(s_state.fc_main,  sizeof(s_state.fc_main),  fc_main);
    weather_copy_str(s_state.fc_desc,  sizeof(s_state.fc_desc),  fc_desc);
    weather_copy_str(s_state.fc_temp,  sizeof(s_state.fc_temp),  fc_temp);
    weather_copy_str(s_state.fc_feels, sizeof(s_state.fc_feels), fc_feels);
    weather_copy_str(s_state.fc_when,  sizeof(s_state.fc_when),  fc_when);
    pthread_mutex_unlock(&s_mutex);
    return true;
}

static bool weather_fetch_once(void)
{
    char api_key[WEATHER_KEY_MAX + 1];
    char location[WEATHER_LOC_MAX + 1];
    char units[WEATHER_UNITS_MAX + 1];
    bool have_config;

    pthread_mutex_lock(&s_mutex);
    have_config = weather_load_settings_locked();
    weather_copy_str(api_key,  sizeof(api_key),  s_api_key);
    weather_copy_str(location, sizeof(location), s_location);
    weather_copy_str(units,    sizeof(units),    s_units_str);
    if (have_config)
    {
        s_state.status = WEATHER_STATUS_FETCHING;
        s_state.err[0] = '\0';
    }
    pthread_mutex_unlock(&s_mutex);

    if (!have_config)
    {
        weather_set_error("OWM_KEY or OWM_LOCATION not set");
        return false;
    }

    char url[WEATHER_URL_MAX];
    static char body[WEATHER_BODY_MAX];
    weather_http_ctx_t ctx = { body, sizeof(body), 0 };

    weather_build_url(url, sizeof(url), "weather", 0, api_key, location, units);
    if (!weather_http_get(url, &ctx) || !weather_parse_current(body))
    {
        pthread_mutex_lock(&s_mutex);
        s_state.status = WEATHER_STATUS_ERROR;
        pthread_mutex_unlock(&s_mutex);
        return false;
    }

    weather_build_url(url, sizeof(url), "forecast", 1, api_key, location, units);
    if (weather_http_get(url, &ctx))
    {
        weather_parse_forecast(body); /* best-effort */
    }

    pthread_mutex_lock(&s_mutex);
    s_state.status = WEATHER_STATUS_READY;
    s_state.last_fetch_us = weather_now_us();
    s_state.err[0] = '\0';
    pthread_mutex_unlock(&s_mutex);
    return true;
}

static void weather_wait_seconds_locked(int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    pthread_cond_timedwait(&s_cond, &s_mutex, &ts);
}

static void *weather_thread_fn(void *arg)
{
    (void)arg;
    /* Brief settle so the rest of the host has finished starting up. */
    struct timespec settle = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&settle, NULL);

    int64_t start_us = weather_now_us();

    for (;;)
    {
        bool ok = weather_fetch_once();

        int wait_secs = WEATHER_REFRESH_SECS;
        if (!ok)
        {
            int64_t elapsed = (weather_now_us() - start_us) / 1000000;
            if (elapsed < WEATHER_RETRY_WINDOW_SECS)
            {
                wait_secs = WEATHER_STARTUP_RETRY_SECS;
            }
        }

        pthread_mutex_lock(&s_mutex);
        weather_wait_seconds_locked(wait_secs);
        pthread_mutex_unlock(&s_mutex);
    }
    return NULL;
}

#endif /* HAVE_LIBCURL */

/* ---- public init ---- */

void weather_io_init(void)
{
    if (s_initialized) return;
    memset(&s_state, 0, sizeof(s_state));
    s_state.status = WEATHER_STATUS_NONE;

    pthread_mutex_lock(&s_mutex);
    (void)weather_load_settings_locked();
    pthread_mutex_unlock(&s_mutex);

    s_initialized = true;

#ifdef HAVE_LIBCURL
    static bool s_curl_global_inited = false;
    if (!s_curl_global_inited)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        s_curl_global_inited = true;
    }
    if (pthread_create(&s_thread, NULL, weather_thread_fn, NULL) == 0)
    {
        pthread_detach(s_thread);
        s_thread_started = true;
        fprintf(stderr, "[weather] driver started (units=%s)\n", s_units_str);
    }
    else
    {
        weather_set_error("pthread_create failed: %s", strerror(errno));
    }
#else
    weather_set_error("libcurl not available - weather disabled");
#endif
}

/* ---- port surface ---- */

static const char *weather_field_locked(uint8_t field, char *scratch, size_t scratch_len)
{
    switch (field)
    {
        case WEATHER_FIELD_CITY:      return s_state.city;
        case WEATHER_FIELD_CUR_MAIN:  return s_state.cur_main;
        case WEATHER_FIELD_CUR_DESC:  return s_state.cur_desc;
        case WEATHER_FIELD_CUR_TEMP:  return s_state.cur_temp;
        case WEATHER_FIELD_CUR_FEELS: return s_state.cur_feels;
        case WEATHER_FIELD_CUR_HUMID: return s_state.cur_humid;
        case WEATHER_FIELD_CUR_WIND:  return s_state.cur_wind;
        case WEATHER_FIELD_FC_MAIN:   return s_state.fc_main;
        case WEATHER_FIELD_FC_DESC:   return s_state.fc_desc;
        case WEATHER_FIELD_FC_TEMP:   return s_state.fc_temp;
        case WEATHER_FIELD_FC_FEELS:  return s_state.fc_feels;
        case WEATHER_FIELD_FC_WHEN:   return s_state.fc_when;
        case WEATHER_FIELD_UNITS:     return s_state.units;
        case WEATHER_FIELD_ERROR:     return s_state.err;
        case WEATHER_FIELD_AGE_SEC:
        {
            int64_t age = 0;
            if (s_state.last_fetch_us > 0)
            {
                age = (weather_now_us() - s_state.last_fetch_us) / 1000000;
                if (age < 0) age = 0;
            }
            snprintf(scratch, scratch_len, "%lld", (long long)age);
            return scratch;
        }
        default:
            if (scratch_len > 0) scratch[0] = '\0';
            return scratch;
    }
}

size_t weather_output(int port, uint8_t data, char *buffer, size_t buffer_length)
{
    if (!s_initialized) return 0;
    if (port != WEATHER_PORT_FIELD) return 0;

    char scratch[WEATHER_REPLY_SCRATCH_MAX];
    pthread_mutex_lock(&s_mutex);
    const char *src = weather_field_locked(data, scratch, sizeof(scratch));
    weather_copy_str(s_reply, sizeof(s_reply), src ? src : "");
    pthread_mutex_unlock(&s_mutex);

    size_t reply_len = strlen(s_reply);
    if (buffer && buffer_length > 0)
    {
        size_t n = reply_len;
        if (n >= buffer_length) n = buffer_length - 1;
        memcpy(buffer, s_reply, n);
        buffer[n] = '\0';
        return n;
    }
    return 0;
}

uint8_t weather_input(uint8_t port)
{
    if (!s_initialized) return WEATHER_STATUS_NONE;
    if (port != WEATHER_PORT_STATUS) return 0;

    uint8_t s;
    pthread_mutex_lock(&s_mutex);
    s = s_state.status;
    pthread_mutex_unlock(&s_mutex);
    return s;
}

#ifndef HAVE_LIBCURL
/* Silence -Wunused for the no-libcurl build (only static functions
 * declared above; nothing referenced). */
static void weather_unused_no_curl(void)
{
    (void)weather_now_us;
    (void)weather_set_error;
    (void)weather_load_settings_locked;
}
#endif
