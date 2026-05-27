/**
 * @file environment_io.c
 * @brief NVS-backed environment variable I/O port driver.
 */

#include "port_drivers/environment_io.h"

#include "config.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#define ENVIRONMENT_NAMESPACE "ENVIRONMENT"

#define ENV_KEY_SIZE 17
#define ENV_VALUE_SIZE 256
#define ENV_REQUEST_SIZE 512
#define ENV_ERROR_SIZE 128

#define ENV_STATUS_OK 0
#define ENV_STATUS_OPEN -1
#define ENV_STATUS_READ -2
#define ENV_STATUS_WRITE -3
#define ENV_STATUS_FULL -4
#define ENV_STATUS_NOT_FOUND -5

#define ENV_COMMAND_RESET 0
#define ENV_COMMAND_INIT 1
#define ENV_COMMAND_GET 2
#define ENV_COMMAND_SET 3
#define ENV_COMMAND_DELETE 4
#define ENV_COMMAND_LIST 5
#define ENV_COMMAND_COUNT 6
#define ENV_COMMAND_CLEAR 7
#define ENV_COMMAND_EXISTS 8
#define ENV_COMMAND_EXECUTE 9

typedef struct
{
    char request[ENV_REQUEST_SIZE];
    char key[ENV_KEY_SIZE];
    char value[ENV_VALUE_SIZE];
    char work[ENV_REQUEST_SIZE];
    char arg1[ENV_VALUE_SIZE];
    char arg2[ENV_VALUE_SIZE];
    char evaled[ENV_VALUE_SIZE];
    char error[ENV_ERROR_SIZE];
    char eval_left[ENV_VALUE_SIZE];
    char eval_right[ENV_VALUE_SIZE];
    char eval_value[ENV_VALUE_SIZE];
    char eval_key[ENV_KEY_SIZE];
} environment_state_t;

static environment_state_t *s_env = NULL;
static size_t s_request_len = 0;
static int8_t s_status = ENV_STATUS_OK;
static bool s_initialized = false;

static void environment_reset_request(void);
static void environment_append_byte(uint8_t data);
static size_t environment_execute_command(uint8_t command, char *buffer, size_t buffer_length);
static int environment_open(nvs_open_mode_t mode, nvs_handle_t *handle);
static void environment_normalize_key(const char *key, char *out, size_t out_length);
static bool environment_key_valid(const char *key);
static int environment_get_value(const char *key, char *value, size_t value_length);
static int environment_set_value(const char *key, const char *value);
static int environment_delete_value(const char *key);
static int environment_clear_values(void);
static int environment_count_values(size_t *count);
static int environment_list_values(char *buffer, size_t buffer_length, size_t *written);
static size_t environment_write_text(char *buffer, size_t buffer_length, const char *text);
static size_t environment_append_text(char *buffer, size_t buffer_length, size_t pos, const char *text);
static size_t environment_format_response(char *buffer, size_t buffer_length, const char *fmt, ...);
static char *environment_skip_spaces(char *text);
static void environment_trim_text(char *text);
static char *environment_next_token(char *text, char *token, size_t token_length);
static void environment_copy_text(char *dst, size_t dst_length, const char *src);
static bool environment_parse_assignment(char *name, char *rest, char *key, size_t key_length, char *value, size_t value_length);
static bool environment_split_delta(char *text, char *key, size_t key_length, char *delta, size_t delta_length);
static bool environment_parse_delta(char *name, char *rest, char *key, size_t key_length, char *delta, size_t delta_length);
static bool environment_is_number(const char *text);
static bool environment_is_now(const char *text);
static bool environment_is_help(const char *text);
static void environment_now(char *buffer, size_t buffer_length);
static int environment_eval_value(const char *expr, char *result, size_t result_length, char *error, size_t error_length);
static int environment_value_to_i64(const char *expr, int64_t *value, char *error, size_t error_length);
static size_t environment_help_text(char *buffer, size_t buffer_length);
static size_t environment_execute_cli(char *buffer, size_t buffer_length);
static bool environment_alloc_state(void);

void environment_io_init(void)
{
    if (s_initialized)
    {
        return;
    }

    if (!environment_alloc_state())
    {
        printf("[ENV] failed to allocate driver state\n");
        return;
    }

    altair_config_init();
    environment_reset_request();
    s_status = ENV_STATUS_OK;
    s_initialized = true;
    printf("[ENV] driver initialized on ports %d/%d using NVS namespace %s\n",
           ENVIRONMENT_PORT_COMMAND, ENVIRONMENT_PORT_DATA, ENVIRONMENT_NAMESPACE);
}

size_t environment_output(int port, uint8_t data, char *buffer, size_t buffer_length)
{
    if (!s_initialized)
    {
        environment_io_init();
    }

    if (!s_env)
    {
        s_status = ENV_STATUS_FULL;
        return 0;
    }

    if (port == ENVIRONMENT_PORT_DATA)
    {
        environment_append_byte(data);
        return 0;
    }

    if (port == ENVIRONMENT_PORT_COMMAND)
    {
        if (data == ENV_COMMAND_RESET)
        {
            environment_reset_request();
            s_status = ENV_STATUS_OK;
            return 0;
        }
        return environment_execute_command(data, buffer, buffer_length);
    }

    return 0;
}

uint8_t environment_input(uint8_t port)
{
    if (port == ENVIRONMENT_PORT_COMMAND)
    {
        return (uint8_t)s_status;
    }
    return 0;
}

static bool environment_alloc_state(void)
{
    if (s_env)
    {
        return true;
    }

    s_env = heap_caps_calloc(1, sizeof(environment_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_env)
    {
        s_env = calloc(1, sizeof(environment_state_t));
    }
    return s_env != NULL;
}

static void environment_reset_request(void)
{
    if (s_env)
    {
        memset(s_env->request, 0, sizeof(s_env->request));
    }
    s_request_len = 0;
}

static void environment_append_byte(uint8_t data)
{
    if (!s_env)
    {
        s_status = ENV_STATUS_FULL;
        return;
    }

    if (s_request_len < sizeof(s_env->request) - 1)
    {
        s_env->request[s_request_len++] = (char)data;
        s_env->request[s_request_len] = '\0';
    }
    else
    {
        s_status = ENV_STATUS_FULL;
    }
}

static size_t environment_execute_command(uint8_t command, char *buffer, size_t buffer_length)
{
    char *key = s_env->key;
    char *value = s_env->value;
    const char *second;
    size_t len = 0;
    size_t count = 0;
    int rc = ENV_STATUS_OK;

    if (buffer && buffer_length > 0)
    {
        buffer[0] = '\0';
    }

    switch (command)
    {
    case ENV_COMMAND_INIT:
        rc = environment_open(NVS_READONLY, NULL);
        if (rc == ENV_STATUS_OPEN)
        {
            nvs_handle_t handle;
            rc = environment_open(NVS_READWRITE, &handle);
            if (rc == ENV_STATUS_OK)
            {
                nvs_close(handle);
            }
        }
        break;

    case ENV_COMMAND_GET:
        environment_normalize_key(s_env->request, key, ENV_KEY_SIZE);
        rc = environment_get_value(key, value, ENV_VALUE_SIZE);
        if (rc == ENV_STATUS_OK)
        {
            len = environment_write_text(buffer, buffer_length, value);
        }
        break;

    case ENV_COMMAND_SET:
        environment_normalize_key(s_env->request, key, ENV_KEY_SIZE);
        second = s_env->request + strlen(s_env->request) + 1;
        rc = environment_set_value(key, second);
        break;

    case ENV_COMMAND_DELETE:
        environment_normalize_key(s_env->request, key, ENV_KEY_SIZE);
        rc = environment_delete_value(key);
        break;

    case ENV_COMMAND_LIST:
        rc = environment_list_values(buffer, buffer_length, &len);
        break;

    case ENV_COMMAND_COUNT:
        rc = environment_count_values(&count);
        if (rc == ENV_STATUS_OK)
        {
            len = environment_format_response(buffer, buffer_length, "%u", (unsigned)count);
        }
        break;

    case ENV_COMMAND_CLEAR:
        rc = environment_clear_values();
        break;

    case ENV_COMMAND_EXISTS:
        environment_normalize_key(s_env->request, key, ENV_KEY_SIZE);
        rc = environment_get_value(key, value, ENV_VALUE_SIZE);
        break;

    case ENV_COMMAND_EXECUTE:
        len = environment_execute_cli(buffer, buffer_length);
        rc = s_status;
        break;

    default:
        rc = ENV_STATUS_READ;
        break;
    }

    s_status = (int8_t)rc;
    if (command != ENV_COMMAND_EXECUTE)
    {
        environment_reset_request();
    }
    return len;
}

static int environment_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    nvs_handle_t local_handle;

    if (!handle)
    {
        handle = &local_handle;
    }

    esp_err_t err = nvs_open(ENVIRONMENT_NAMESPACE, mode, handle);
    if (err != ESP_OK)
    {
        return ENV_STATUS_OPEN;
    }

    if (handle == &local_handle)
    {
        nvs_close(local_handle);
    }
    return ENV_STATUS_OK;
}

static void environment_normalize_key(const char *key, char *out, size_t out_length)
{
    size_t i;

    if (!out || out_length == 0)
    {
        return;
    }

    if (!key)
    {
        out[0] = '\0';
        return;
    }

    for (i = 0; i < out_length - 1 && key[i] != '\0'; i++)
    {
        out[i] = (char)toupper((unsigned char)key[i]);
    }
    out[i] = '\0';
}

static bool environment_key_valid(const char *key)
{
    return key && key[0] != '\0' && strlen(key) < ENV_KEY_SIZE;
}

static int environment_get_value(const char *key, char *value, size_t value_length)
{
    nvs_handle_t handle;
    size_t len;

    if (value && value_length > 0)
    {
        value[0] = '\0';
    }

    if (!environment_key_valid(key))
    {
        return ENV_STATUS_NOT_FOUND;
    }

    if (environment_open(NVS_READONLY, &handle) != ENV_STATUS_OK)
    {
        return ENV_STATUS_NOT_FOUND;
    }

    len = value_length;
    esp_err_t err = nvs_get_str(handle, key, value, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ENV_STATUS_NOT_FOUND;
    }
    if (err != ESP_OK)
    {
        return ENV_STATUS_READ;
    }

    if (value && value_length > 0)
    {
        value[value_length - 1] = '\0';
    }
    return ENV_STATUS_OK;
}

static int environment_set_value(const char *key, const char *value)
{
    nvs_handle_t handle;
    char *clipped = s_env->eval_value;

    if (!environment_key_valid(key))
    {
        return ENV_STATUS_WRITE;
    }

    strncpy(clipped, value ? value : "", ENV_VALUE_SIZE - 1);
    clipped[ENV_VALUE_SIZE - 1] = '\0';

    if (environment_open(NVS_READWRITE, &handle) != ENV_STATUS_OK)
    {
        return ENV_STATUS_OPEN;
    }

    esp_err_t err = nvs_set_str(handle, key, clipped);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK ? ENV_STATUS_OK : ENV_STATUS_WRITE;
}

static int environment_delete_value(const char *key)
{
    nvs_handle_t handle;

    if (!environment_key_valid(key))
    {
        return ENV_STATUS_NOT_FOUND;
    }

    if (environment_open(NVS_READWRITE, &handle) != ENV_STATUS_OK)
    {
        return ENV_STATUS_OPEN;
    }

    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ENV_STATUS_NOT_FOUND;
    }
    return err == ESP_OK ? ENV_STATUS_OK : ENV_STATUS_WRITE;
}

static int environment_clear_values(void)
{
    nvs_handle_t handle;

    if (environment_open(NVS_READWRITE, &handle) != ENV_STATUS_OK)
    {
        return ENV_STATUS_OPEN;
    }

    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK ? ENV_STATUS_OK : ENV_STATUS_WRITE;
}

static int environment_count_values(size_t *count)
{
    nvs_handle_t handle;
    nvs_iterator_t iterator = NULL;
    nvs_entry_info_t info;
    size_t total = 0;
    esp_err_t err;

    if (environment_open(NVS_READONLY, &handle) != ENV_STATUS_OK)
    {
        if (count)
        {
            *count = 0;
        }
        return ENV_STATUS_OK;
    }

    err = nvs_entry_find_in_handle(handle, NVS_TYPE_STR, &iterator);
    while (err == ESP_OK)
    {
        if (nvs_entry_info(iterator, &info) != ESP_OK)
        {
            nvs_release_iterator(iterator);
            nvs_close(handle);
            return ENV_STATUS_READ;
        }
        total++;
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    nvs_close(handle);

    if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        return ENV_STATUS_READ;
    }

    if (count)
    {
        *count = total;
    }
    return ENV_STATUS_OK;
}

static int environment_list_values(char *buffer, size_t buffer_length, size_t *written)
{
    nvs_handle_t handle;
    nvs_iterator_t iterator = NULL;
    nvs_entry_info_t info;
    char *value = s_env->eval_value;
    size_t pos = 0;
    esp_err_t err;

    if (buffer && buffer_length > 0)
    {
        buffer[0] = '\0';
    }

    if (environment_open(NVS_READONLY, &handle) != ENV_STATUS_OK)
    {
        if (written)
        {
            *written = 0;
        }
        return ENV_STATUS_OK;
    }

    err = nvs_entry_find_in_handle(handle, NVS_TYPE_STR, &iterator);
    while (err == ESP_OK)
    {
        if (nvs_entry_info(iterator, &info) != ESP_OK)
        {
            nvs_release_iterator(iterator);
            nvs_close(handle);
            return ENV_STATUS_READ;
        }

        size_t value_length = ENV_VALUE_SIZE;
        if (nvs_get_str(handle, info.key, value, &value_length) == ESP_OK)
        {
            size_t old_pos = pos;
            pos = environment_append_text(buffer, buffer_length, pos, info.key);
            pos = environment_append_text(buffer, buffer_length, pos, "=");
            pos = environment_append_text(buffer, buffer_length, pos, value);
            pos = environment_append_text(buffer, buffer_length, pos, "\r\n");
            if (pos == old_pos && buffer_length > 0)
            {
                nvs_release_iterator(iterator);
                nvs_close(handle);
                if (written)
                {
                    *written = pos;
                }
                return ENV_STATUS_FULL;
            }
        }
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    nvs_close(handle);

    if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        return ENV_STATUS_READ;
    }

    if (written)
    {
        *written = pos;
    }
    return ENV_STATUS_OK;
}

static size_t environment_write_text(char *buffer, size_t buffer_length, const char *text)
{
    if (!buffer || buffer_length == 0)
    {
        return 0;
    }
    return environment_append_text(buffer, buffer_length, 0, text ? text : "");
}

static size_t environment_append_text(char *buffer, size_t buffer_length, size_t pos, const char *text)
{
    if (!buffer || buffer_length == 0 || pos >= buffer_length - 1 || !text)
    {
        return pos;
    }

    while (*text && pos < buffer_length - 1)
    {
        buffer[pos++] = *text++;
    }
    buffer[pos] = '\0';
    return pos;
}

static size_t environment_format_response(char *buffer, size_t buffer_length, const char *fmt, ...)
{
    va_list args;

    if (!buffer || buffer_length == 0)
    {
        return 0;
    }

    va_start(args, fmt);
    int written = vsnprintf(buffer, buffer_length, fmt, args);
    va_end(args);

    if (written < 0)
    {
        buffer[0] = '\0';
        return 0;
    }
    if ((size_t)written >= buffer_length)
    {
        return buffer_length - 1;
    }
    return (size_t)written;
}

static char *environment_skip_spaces(char *text)
{
    while (text && (*text == ' ' || *text == '\t'))
    {
        text++;
    }
    return text;
}

static void environment_trim_text(char *text)
{
    char *start;
    char *end;

    if (!text)
    {
        return;
    }

    start = environment_skip_spaces(text);
    if (start != text)
    {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
    {
        end--;
    }
    *end = '\0';
}

static char *environment_next_token(char *text, char *token, size_t token_length)
{
    size_t i = 0;

    text = environment_skip_spaces(text);
    if (!text || *text == '\0')
    {
        if (token_length > 0)
        {
            token[0] = '\0';
        }
        return text;
    }

    while (*text && *text != ' ' && *text != '\t')
    {
        if (i < token_length - 1)
        {
            token[i++] = *text;
        }
        text++;
    }
    if (token_length > 0)
    {
        token[i] = '\0';
    }
    return environment_skip_spaces(text);
}

static void environment_copy_text(char *dst, size_t dst_length, const char *src)
{
    if (!dst || dst_length == 0)
    {
        return;
    }
    strncpy(dst, src ? src : "", dst_length - 1);
    dst[dst_length - 1] = '\0';
}

static bool environment_parse_assignment(char *name, char *rest, char *key, size_t key_length, char *value, size_t value_length)
{
    char *equals;
    char *assign_value;

    if (!name || !key || key_length == 0 || !value || value_length == 0)
    {
        return false;
    }

    equals = strchr(name, '=');
    if (equals)
    {
        *equals = '\0';
        environment_normalize_key(name, key, key_length);
        environment_copy_text(value, value_length, equals + 1);
        if (rest && rest[0] != '\0')
        {
            if (value[0] != '\0')
            {
                strncat(value, " ", value_length - strlen(value) - 1);
            }
            strncat(value, rest, value_length - strlen(value) - 1);
        }
        environment_trim_text(value);
        return key[0] != '\0';
    }

    rest = environment_skip_spaces(rest);
    if (!rest || rest[0] != '=')
    {
        return false;
    }

    environment_normalize_key(name, key, key_length);
    assign_value = environment_skip_spaces(rest + 1);
    environment_copy_text(value, value_length, assign_value);
    environment_trim_text(value);
    return key[0] != '\0';
}

static bool environment_split_delta(char *text, char *key, size_t key_length, char *delta, size_t delta_length)
{
    char *op;
    char *number;
    size_t key_len;

    if (key && key_length > 0)
    {
        key[0] = '\0';
    }
    if (delta && delta_length > 0)
    {
        delta[0] = '\0';
    }
    if (!text || !key || key_length == 0 || !delta || delta_length < 2)
    {
        return false;
    }

    op = text + 1;
    while (*op && *op != '+' && *op != '-')
    {
        op++;
    }
    if (*op != '+' && *op != '-')
    {
        return false;
    }

    key_len = (size_t)(op - text);
    if (key_len == 0)
    {
        return false;
    }
    if (key_len >= key_length)
    {
        key_len = key_length - 1;
    }
    memcpy(key, text, key_len);
    key[key_len] = '\0';
    environment_trim_text(key);

    number = environment_skip_spaces(op + 1);
    if (!number || *number == '\0')
    {
        return false;
    }

    delta[0] = *op;
    strncpy(delta + 1, number, delta_length - 2);
    delta[delta_length - 1] = '\0';
    environment_trim_text(delta);
    return environment_is_number(delta);
}

static bool environment_parse_delta(char *name, char *rest, char *key, size_t key_length, char *delta, size_t delta_length)
{
    size_t name_length;
    char *delta_text;

    if (!name || !key || key_length == 0 || !delta || delta_length < 2)
    {
        return false;
    }

    if (environment_split_delta(name, key, key_length, delta, delta_length))
    {
        environment_normalize_key(key, key, key_length);
        return true;
    }

    name_length = strlen(name);
    delta_text = environment_skip_spaces(rest);
    if (name_length > 1 && (name[name_length - 1] == '+' || name[name_length - 1] == '-') &&
        delta_text && delta_text[0] != '\0')
    {
        delta[0] = name[name_length - 1];
        environment_next_token(delta_text, delta + 1, delta_length - 1);
        if (environment_is_number(delta))
        {
            name[name_length - 1] = '\0';
            environment_normalize_key(name, key, key_length);
            return true;
        }
        return false;
    }

    if (delta_text && (delta_text[0] == '+' || delta_text[0] == '-'))
    {
        delta[0] = delta_text[0];
        delta_text = environment_skip_spaces(delta_text + 1);
        environment_next_token(delta_text, delta + 1, delta_length - 1);
        if (environment_is_number(delta))
        {
            environment_normalize_key(name, key, key_length);
            return true;
        }
        return false;
    }

    environment_next_token(rest, delta, delta_length);
    if (delta[0] != '\0' && environment_is_number(delta))
    {
        environment_normalize_key(name, key, key_length);
        return true;
    }

    delta[0] = '\0';
    return false;
}

static bool environment_is_number(const char *text)
{
    size_t i = 0;

    if (!text || text[0] == '\0')
    {
        return false;
    }
    if (text[i] == '+' || text[i] == '-')
    {
        i++;
    }
    if (text[i] == '\0')
    {
        return false;
    }
    while (text[i])
    {
        if (!isdigit((unsigned char)text[i]))
        {
            return false;
        }
        i++;
    }
    return true;
}

static bool environment_is_now(const char *text)
{
    return text && toupper((unsigned char)text[0]) == 'N' &&
           toupper((unsigned char)text[1]) == 'O' &&
           toupper((unsigned char)text[2]) == 'W' && text[3] == '\0';
}

static bool environment_is_help(const char *text)
{
    if (!text || text[0] == '\0')
    {
        return false;
    }

    if ((text[0] == '-' || text[0] == '/') && text[1] != '\0')
    {
        text++;
    }

    if (text[0] == '?' && text[1] == '\0')
    {
        return true;
    }

    if (toupper((unsigned char)text[0]) == 'H' && text[1] == '\0')
    {
        return true;
    }

    return toupper((unsigned char)text[0]) == 'H' &&
           toupper((unsigned char)text[1]) == 'E' &&
           toupper((unsigned char)text[2]) == 'L' &&
           toupper((unsigned char)text[3]) == 'P' && text[4] == '\0';
}

static void environment_now(char *buffer, size_t buffer_length)
{
    uint64_t seconds = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(buffer, buffer_length, "%llu", (unsigned long long)seconds);
}

static int environment_eval_value(const char *expr, char *result, size_t result_length, char *error, size_t error_length)
{
    char *left = s_env->eval_left;
    char *right = s_env->eval_right;
    char *normalized = s_env->eval_key;
    const char *op_pos = NULL;
    char op = 0;
    int64_t left_value;
    int64_t right_value;
    int64_t calc_value;
    size_t i;

    if (!expr || !result || result_length == 0)
    {
        return 0;
    }

    for (i = 1; expr[i] != '\0'; i++)
    {
        if (expr[i] == '+' || expr[i] == '-')
        {
            op_pos = &expr[i];
            op = expr[i];
            break;
        }
    }

    if (!op_pos)
    {
        if (environment_is_number(expr))
        {
            return 0;
        }
        if (environment_is_now(expr))
        {
            environment_now(result, result_length);
            return 1;
        }
        environment_normalize_key(expr, normalized, ENV_KEY_SIZE);
        if (environment_get_value(normalized, result, result_length) == ENV_STATUS_OK)
        {
            return 1;
        }
        return 0;
    }

    size_t left_len = (size_t)(op_pos - expr);
    if (left_len >= ENV_VALUE_SIZE)
    {
        left_len = ENV_VALUE_SIZE - 1;
    }
    memcpy(left, expr, left_len);
    left[left_len] = '\0';
    environment_trim_text(left);

    strncpy(right, op_pos + 1, ENV_VALUE_SIZE - 1);
    right[ENV_VALUE_SIZE - 1] = '\0';
    environment_trim_text(right);

    if (environment_value_to_i64(left, &left_value, error, error_length) != ENV_STATUS_OK)
    {
        return -1;
    }
    if (environment_value_to_i64(right, &right_value, error, error_length) != ENV_STATUS_OK)
    {
        return -1;
    }

    calc_value = (op == '+') ? (left_value + right_value) : (left_value - right_value);
    snprintf(result, result_length, "%lld", (long long)calc_value);
    return 1;
}

static int environment_value_to_i64(const char *expr, int64_t *value, char *error, size_t error_length)
{
    char *normalized = s_env->eval_key;
    char *value_text = s_env->eval_value;
    char *endptr;

    if (environment_is_number(expr))
    {
        *value = strtoll(expr, &endptr, 10);
        return (*endptr == '\0') ? ENV_STATUS_OK : ENV_STATUS_READ;
    }

    if (environment_is_now(expr))
    {
        environment_now(value_text, ENV_VALUE_SIZE);
    }
    else
    {
        environment_normalize_key(expr, normalized, ENV_KEY_SIZE);
        if (environment_get_value(normalized, value_text, ENV_VALUE_SIZE) != ENV_STATUS_OK)
        {
            environment_format_response(error, error_length, "Error: %s not found\r\n", expr);
            return ENV_STATUS_NOT_FOUND;
        }
    }

    if (!environment_is_number(value_text))
    {
        environment_format_response(error, error_length, "Error: %s not numeric\r\n", expr);
        return ENV_STATUS_READ;
    }

    *value = strtoll(value_text, &endptr, 10);
    return (*endptr == '\0') ? ENV_STATUS_OK : ENV_STATUS_READ;
}

static size_t environment_help_text(char *buffer, size_t buffer_length)
{
    return environment_write_text(buffer, buffer_length,
        "ENV - Environment Variable Manager\r\n"
        "==================================\r\n"
        "Stored on ESP32 NVS; available from CP/M programs through DXENV.\r\n"
        "\r\n"
        "Commands:\r\n"
        "  ENV                 List all variables\r\n"
        "  ENV NAME            Show NAME\r\n"
        "  ENV NAME=VALUE      Set NAME to VALUE\r\n"
        "  ENV NAME=VALUE TWO  Set NAME to text with spaces\r\n"
        "  ENV NAME=OTHER      Copy value from OTHER if OTHER exists\r\n"
        "  ENV NAME=A+B        Store numeric A plus B\r\n"
        "  ENV NAME=A-B        Store numeric A minus B\r\n"
        "  ENV NAME+N          Add N to numeric NAME, default 0\r\n"
        "  ENV NAME-N          Subtract N from numeric NAME\r\n"
        "  ENV NOW             Show emulator uptime seconds\r\n"
        "  ENV -I NAME=VALUE   Set only if NAME is undefined\r\n"
        "  ENV -D NAME         Delete NAME\r\n"
        "  ENV -N              Show variable count\r\n"
        "  ENV -H | HELP | ?   Show this help\r\n"
        "\r\n"
        "Use cases:\r\n"
        "  ENV LOCATION=SEATTLE\r\n"
        "  ENV USER=DAVE\r\n"
        "  ENV START=NOW\r\n"
        "  ENV COUNT +1\r\n"
        "  ENV BACKUP=LOCATION\r\n"
        "\r\n"
        "Spaces around =, +, and - are optional.\r\n"
        "Limits: names 16 chars, values 127 chars. Names are stored uppercase.\r\n"
        "Ports: command/status 71, data 72, response 200.\r\n");
}

static size_t environment_execute_cli(char *buffer, size_t buffer_length)
{
    char *work = s_env->work;
    char *arg1 = s_env->arg1;
    char *arg2 = s_env->arg2;
    char *key = s_env->key;
    char *value = s_env->value;
    char *evaled = s_env->evaled;
    char *error = s_env->error;
    char *rest;
    size_t pos = 0;
    size_t count = 0;
    int rc;

    s_status = ENV_STATUS_OK;
    error[0] = '\0';

    size_t request_len = strnlen(s_env->request, ENV_REQUEST_SIZE - 1);
    memcpy(work, s_env->request, request_len);
    work[request_len] = '\0';
    rest = environment_next_token(work, arg1, ENV_VALUE_SIZE);

    if (arg1[0] == '\0')
    {
        rc = environment_list_values(buffer, buffer_length, &pos);
        s_status = (int8_t)rc;
        if (rc == ENV_STATUS_OK && pos == 0)
        {
            pos = environment_write_text(buffer, buffer_length, "(no variables set)\r\n");
        }
        environment_reset_request();
        return pos;
    }

    if (environment_is_help(arg1))
    {
        pos = environment_help_text(buffer, buffer_length);
        environment_reset_request();
        return pos;
    }

    if (arg1[0] == '-' && toupper((unsigned char)arg1[1]) == 'N')
    {
        rc = environment_count_values(&count);
        s_status = (int8_t)rc;
        if (rc == ENV_STATUS_OK)
        {
            pos = environment_format_response(buffer, buffer_length, "%u variable(s) set\r\n", (unsigned)count);
        }
        else
        {
            pos = environment_write_text(buffer, buffer_length, "Error counting variables\r\n");
        }
        environment_reset_request();
        return pos;
    }

    if (arg1[0] == '-' && toupper((unsigned char)arg1[1]) == 'D')
    {
        environment_next_token(rest, arg2, ENV_VALUE_SIZE);
        if (arg2[0] == '\0')
        {
            s_status = ENV_STATUS_READ;
            pos = environment_write_text(buffer, buffer_length, "Usage: ENV -D NAME\r\n");
        }
        else
        {
            environment_normalize_key(arg2, key, ENV_KEY_SIZE);
            rc = environment_delete_value(key);
            s_status = (int8_t)rc;
            if (rc == ENV_STATUS_OK)
            {
                pos = environment_format_response(buffer, buffer_length, "%s deleted\r\n", key);
            }
            else if (rc == ENV_STATUS_NOT_FOUND)
            {
                pos = environment_format_response(buffer, buffer_length, "%s not found\r\n", key);
            }
            else
            {
                pos = environment_format_response(buffer, buffer_length, "Error deleting %s\r\n", key);
            }
        }
        environment_reset_request();
        return pos;
    }

    if (arg1[0] == '-' && toupper((unsigned char)arg1[1]) == 'I')
    {
        rest = environment_next_token(rest, arg2, ENV_VALUE_SIZE);
        if (!environment_parse_assignment(arg2, rest, key, ENV_KEY_SIZE, value, ENV_VALUE_SIZE))
        {
            s_status = ENV_STATUS_READ;
            pos = environment_write_text(buffer, buffer_length, "Usage: ENV -I NAME=VAL\r\n");
        }
        else
        {
            rc = environment_get_value(key, evaled, ENV_VALUE_SIZE);
            if (rc == ENV_STATUS_OK)
            {
                pos = environment_format_response(buffer, buffer_length, "%s already defined\r\n", key);
            }
            else
            {
                rc = environment_set_value(key, value);
                s_status = (int8_t)rc;
                pos = rc == ENV_STATUS_OK ?
                    environment_format_response(buffer, buffer_length, "%s=%s\r\n", key, value) :
                    environment_format_response(buffer, buffer_length, "Error setting %s\r\n", key);
            }
        }
        environment_reset_request();
        return pos;
    }

    if (environment_parse_assignment(arg1, rest, key, ENV_KEY_SIZE, value, ENV_VALUE_SIZE))
    {
        rc = environment_eval_value(value, evaled, ENV_VALUE_SIZE, error, ENV_ERROR_SIZE);
        if (rc < 0)
        {
            s_status = ENV_STATUS_READ;
            pos = environment_write_text(buffer, buffer_length, error);
            environment_reset_request();
            return pos;
        }
        if (rc > 0)
        {
            size_t evaled_len = strnlen(evaled, ENV_VALUE_SIZE - 1);
            memcpy(value, evaled, evaled_len);
            value[evaled_len] = '\0';
        }

        rc = environment_set_value(key, value);
        s_status = (int8_t)rc;
        pos = rc == ENV_STATUS_OK ?
            environment_format_response(buffer, buffer_length, "%s=%s\r\n", key, value) :
            environment_format_response(buffer, buffer_length, "Error setting %s\r\n", key);
        environment_reset_request();
        return pos;
    }

    if (environment_parse_delta(arg1, rest, key, ENV_KEY_SIZE, arg2, ENV_VALUE_SIZE))
    {
        int64_t current = 0;
        int64_t delta = strtoll(arg2, NULL, 10);

        if (environment_get_value(key, value, ENV_VALUE_SIZE) == ENV_STATUS_OK)
        {
            if (!environment_is_number(value))
            {
                s_status = ENV_STATUS_READ;
                pos = environment_format_response(buffer, buffer_length, "Error: %s is not numeric\r\n", key);
                environment_reset_request();
                return pos;
            }
            current = strtoll(value, NULL, 10);
        }

        snprintf(value, ENV_VALUE_SIZE, "%lld", (long long)(current + delta));
        rc = environment_set_value(key, value);
        s_status = (int8_t)rc;
        pos = rc == ENV_STATUS_OK ?
            environment_format_response(buffer, buffer_length, "%s=%s\r\n", key, value) :
            environment_format_response(buffer, buffer_length, "Error setting %s\r\n", key);
        environment_reset_request();
        return pos;
    }

    if (environment_is_now(arg1))
    {
        environment_now(value, ENV_VALUE_SIZE);
        pos = environment_format_response(buffer, buffer_length, "NOW=%s\r\n", value);
        environment_reset_request();
        return pos;
    }

    environment_normalize_key(arg1, key, ENV_KEY_SIZE);
    rc = environment_get_value(key, value, ENV_VALUE_SIZE);
    s_status = (int8_t)rc;
    if (rc == ENV_STATUS_OK)
    {
        pos = environment_format_response(buffer, buffer_length, "%s=%s\r\n", key, value);
    }
    else
    {
        pos = environment_format_response(buffer, buffer_length, "%s not found\r\n", key);
    }

    environment_reset_request();
    return pos;
}
