/**
 * @file environment_io.c
 * @brief Text-file-backed environment variable I/O port driver (host build).
 *
 * Storage format: one KEY=VALUE entry per line in a UTF-8 text file. Blank
 * lines and lines starting with '#' are ignored on load. Keys are stored
 * uppercase (matching the ESP32 driver), values are stored verbatim.
 */

#include "environment_io.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENV_MAX_ENTRIES 128
#define ENV_KEY_SIZE 17
#define ENV_VALUE_SIZE 256
#define ENV_REQUEST_SIZE 512
#define ENV_ERROR_SIZE 128
#define ENV_PATH_SIZE 1024

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
    char key[ENV_KEY_SIZE];
    char value[ENV_VALUE_SIZE];
} env_entry_t;

static env_entry_t s_entries[ENV_MAX_ENTRIES];
static size_t s_count = 0;
static char s_request[ENV_REQUEST_SIZE];
static size_t s_request_len = 0;
static int8_t s_status = ENV_STATUS_OK;
static char s_file_path[ENV_PATH_SIZE] = "altair_env.txt";
static bool s_initialized = false;
static time_t s_start_time = 0;

/* Workspace scratch buffers (avoid large stack frames). */
static char s_key[ENV_KEY_SIZE];
static char s_value[ENV_VALUE_SIZE];
static char s_work[ENV_REQUEST_SIZE];
static char s_arg1[ENV_VALUE_SIZE];
static char s_arg2[ENV_VALUE_SIZE];
static char s_evaled[ENV_VALUE_SIZE];
static char s_error[ENV_ERROR_SIZE];
static char s_eval_left[ENV_VALUE_SIZE];
static char s_eval_right[ENV_VALUE_SIZE];
static char s_eval_key[ENV_KEY_SIZE];
static char s_eval_value[ENV_VALUE_SIZE];

static void env_reset_request(void);
static void env_append_byte(uint8_t data);
static size_t env_execute_command(uint8_t command, char *buffer, size_t buffer_length);
static void env_normalize_key(const char *key, char *out, size_t out_length);
static bool env_key_valid(const char *key);
static int env_load_file(void);
static int env_save_file(void);
static int env_find_index(const char *key);
static int env_get_value(const char *key, char *value, size_t value_length);
static int env_set_value(const char *key, const char *value);
static int env_delete_value(const char *key);
static int env_clear_values(void);
static size_t env_list_values(char *buffer, size_t buffer_length);
static size_t env_write_text(char *buffer, size_t buffer_length, const char *text);
static size_t env_append_text(char *buffer, size_t buffer_length, size_t pos, const char *text);
static size_t env_format_response(char *buffer, size_t buffer_length, const char *fmt, ...);
static char *env_skip_spaces(char *text);
static void env_trim_text(char *text);
static char *env_next_token(char *text, char *token, size_t token_length);
static void env_copy_text(char *dst, size_t dst_length, const char *src);
static bool env_parse_assignment(char *name, char *rest, char *key, size_t key_length, char *value, size_t value_length);
static bool env_split_delta(char *text, char *key, size_t key_length, char *delta, size_t delta_length);
static bool env_parse_delta(char *name, char *rest, char *key, size_t key_length, char *delta, size_t delta_length);
static bool env_is_number(const char *text);
static bool env_is_now(const char *text);
static bool env_is_help(const char *text);
static void env_now(char *buffer, size_t buffer_length);
static int env_eval_value(const char *expr, char *result, size_t result_length, char *error, size_t error_length);
static int env_value_to_i64(const char *expr, int64_t *value, char *error, size_t error_length);
static size_t env_help_text(char *buffer, size_t buffer_length);
static size_t env_execute_cli(char *buffer, size_t buffer_length);

void environment_io_init(const char *file_path)
{
    if (s_initialized)
    {
        return;
    }

    if (file_path && file_path[0] != '\0')
    {
        strncpy(s_file_path, file_path, ENV_PATH_SIZE - 1);
        s_file_path[ENV_PATH_SIZE - 1] = '\0';
    }

    s_start_time = time(NULL);
    env_reset_request();
    s_status = ENV_STATUS_OK;
    s_initialized = true;

    if (env_load_file() == ENV_STATUS_OK)
    {
        printf("[ENV] driver initialized: %s (%zu entries)\n", s_file_path, s_count);
    }
    else
    {
        printf("[ENV] driver initialized: %s (new/empty)\n", s_file_path);
    }
}

size_t environment_output(int port, uint8_t data, char *buffer, size_t buffer_length)
{
    if (!s_initialized)
    {
        environment_io_init(NULL);
    }

    if (port == ENVIRONMENT_PORT_DATA)
    {
        env_append_byte(data);
        return 0;
    }

    if (port == ENVIRONMENT_PORT_COMMAND)
    {
        if (data == ENV_COMMAND_RESET)
        {
            env_reset_request();
            s_status = ENV_STATUS_OK;
            return 0;
        }
        return env_execute_command(data, buffer, buffer_length);
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

bool environment_io_get(const char *key, char *value, size_t value_length)
{
    char normalized[ENV_KEY_SIZE];
    int rc;

    if (!s_initialized)
    {
        environment_io_init(NULL);
    }
    env_normalize_key(key, normalized, ENV_KEY_SIZE);
    rc = env_get_value(normalized, value, value_length);
    return rc == ENV_STATUS_OK;
}

static void env_reset_request(void)
{
    memset(s_request, 0, sizeof(s_request));
    s_request_len = 0;
}

static void env_append_byte(uint8_t data)
{
    if (s_request_len < sizeof(s_request) - 1)
    {
        s_request[s_request_len++] = (char)data;
        s_request[s_request_len] = '\0';
    }
    else
    {
        s_status = ENV_STATUS_FULL;
    }
}

static size_t env_execute_command(uint8_t command, char *buffer, size_t buffer_length)
{
    const char *second;
    size_t len = 0;
    int rc = ENV_STATUS_OK;

    if (buffer && buffer_length > 0)
    {
        buffer[0] = '\0';
    }

    switch (command)
    {
    case ENV_COMMAND_INIT:
        rc = ENV_STATUS_OK;
        break;

    case ENV_COMMAND_GET:
        env_normalize_key(s_request, s_key, ENV_KEY_SIZE);
        rc = env_get_value(s_key, s_value, ENV_VALUE_SIZE);
        if (rc == ENV_STATUS_OK)
        {
            len = env_write_text(buffer, buffer_length, s_value);
        }
        break;

    case ENV_COMMAND_SET:
        env_normalize_key(s_request, s_key, ENV_KEY_SIZE);
        second = s_request + strlen(s_request) + 1;
        rc = env_set_value(s_key, second);
        break;

    case ENV_COMMAND_DELETE:
        env_normalize_key(s_request, s_key, ENV_KEY_SIZE);
        rc = env_delete_value(s_key);
        break;

    case ENV_COMMAND_LIST:
        len = env_list_values(buffer, buffer_length);
        rc = ENV_STATUS_OK;
        break;

    case ENV_COMMAND_COUNT:
        len = env_format_response(buffer, buffer_length, "%u", (unsigned)s_count);
        rc = ENV_STATUS_OK;
        break;

    case ENV_COMMAND_CLEAR:
        rc = env_clear_values();
        break;

    case ENV_COMMAND_EXISTS:
        env_normalize_key(s_request, s_key, ENV_KEY_SIZE);
        rc = env_get_value(s_key, s_value, ENV_VALUE_SIZE);
        break;

    case ENV_COMMAND_EXECUTE:
        len = env_execute_cli(buffer, buffer_length);
        rc = s_status;
        break;

    default:
        rc = ENV_STATUS_READ;
        break;
    }

    s_status = (int8_t)rc;
    if (command != ENV_COMMAND_EXECUTE)
    {
        env_reset_request();
    }
    return len;
}

static void env_normalize_key(const char *key, char *out, size_t out_length)
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

static bool env_key_valid(const char *key)
{
    return key && key[0] != '\0' && strlen(key) < ENV_KEY_SIZE;
}

static int env_find_index(const char *key)
{
    size_t i;
    for (i = 0; i < s_count; i++)
    {
        if (strcmp(s_entries[i].key, key) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static int env_load_file(void)
{
    FILE *fp;
    char line[ENV_REQUEST_SIZE];
    char *eq;
    char *nl;

    s_count = 0;

    fp = fopen(s_file_path, "r");
    if (!fp)
    {
        return ENV_STATUS_OPEN;
    }

    while (fgets(line, sizeof(line), fp))
    {
        nl = strchr(line, '\n');
        if (nl)
        {
            *nl = '\0';
        }
        nl = strchr(line, '\r');
        if (nl)
        {
            *nl = '\0';
        }

        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        eq = strchr(line, '=');
        if (!eq)
        {
            continue;
        }
        *eq = '\0';

        if (s_count >= ENV_MAX_ENTRIES)
        {
            break;
        }

        env_normalize_key(line, s_entries[s_count].key, ENV_KEY_SIZE);
        if (!env_key_valid(s_entries[s_count].key))
        {
            continue;
        }
        strncpy(s_entries[s_count].value, eq + 1, ENV_VALUE_SIZE - 1);
        s_entries[s_count].value[ENV_VALUE_SIZE - 1] = '\0';
        s_count++;
    }

    fclose(fp);
    return ENV_STATUS_OK;
}

static int env_save_file(void)
{
    FILE *fp;
    size_t i;

    fp = fopen(s_file_path, "w");
    if (!fp)
    {
        return ENV_STATUS_WRITE;
    }

    for (i = 0; i < s_count; i++)
    {
        if (fprintf(fp, "%s=%s\n", s_entries[i].key, s_entries[i].value) < 0)
        {
            fclose(fp);
            return ENV_STATUS_WRITE;
        }
    }

    if (fflush(fp) != 0)
    {
        fclose(fp);
        return ENV_STATUS_WRITE;
    }
    fclose(fp);
    return ENV_STATUS_OK;
}

static int env_get_value(const char *key, char *value, size_t value_length)
{
    int idx;

    if (value && value_length > 0)
    {
        value[0] = '\0';
    }

    if (!env_key_valid(key))
    {
        return ENV_STATUS_NOT_FOUND;
    }

    idx = env_find_index(key);
    if (idx < 0)
    {
        return ENV_STATUS_NOT_FOUND;
    }

    if (value && value_length > 0)
    {
        strncpy(value, s_entries[idx].value, value_length - 1);
        value[value_length - 1] = '\0';
    }
    return ENV_STATUS_OK;
}

static int env_set_value(const char *key, const char *value)
{
    int idx;
    const char *src;

    if (!env_key_valid(key))
    {
        return ENV_STATUS_WRITE;
    }

    src = value ? value : "";
    idx = env_find_index(key);
    if (idx < 0)
    {
        if (s_count >= ENV_MAX_ENTRIES)
        {
            return ENV_STATUS_FULL;
        }
        strncpy(s_entries[s_count].key, key, ENV_KEY_SIZE - 1);
        s_entries[s_count].key[ENV_KEY_SIZE - 1] = '\0';
        idx = (int)s_count;
        s_count++;
    }
    strncpy(s_entries[idx].value, src, ENV_VALUE_SIZE - 1);
    s_entries[idx].value[ENV_VALUE_SIZE - 1] = '\0';
    return env_save_file();
}

static int env_delete_value(const char *key)
{
    int idx;
    size_t i;

    if (!env_key_valid(key))
    {
        return ENV_STATUS_NOT_FOUND;
    }

    idx = env_find_index(key);
    if (idx < 0)
    {
        return ENV_STATUS_NOT_FOUND;
    }

    for (i = (size_t)idx; i + 1 < s_count; i++)
    {
        s_entries[i] = s_entries[i + 1];
    }
    s_count--;
    return env_save_file();
}

static int env_clear_values(void)
{
    s_count = 0;
    return env_save_file();
}

static size_t env_list_values(char *buffer, size_t buffer_length)
{
    size_t pos = 0;
    size_t i;

    if (buffer && buffer_length > 0)
    {
        buffer[0] = '\0';
    }

    for (i = 0; i < s_count; i++)
    {
        pos = env_append_text(buffer, buffer_length, pos, s_entries[i].key);
        pos = env_append_text(buffer, buffer_length, pos, "=");
        pos = env_append_text(buffer, buffer_length, pos, s_entries[i].value);
        pos = env_append_text(buffer, buffer_length, pos, "\r\n");
    }
    return pos;
}

static size_t env_write_text(char *buffer, size_t buffer_length, const char *text)
{
    if (!buffer || buffer_length == 0)
    {
        return 0;
    }
    return env_append_text(buffer, buffer_length, 0, text ? text : "");
}

static size_t env_append_text(char *buffer, size_t buffer_length, size_t pos, const char *text)
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

static size_t env_format_response(char *buffer, size_t buffer_length, const char *fmt, ...)
{
    va_list args;
    int written;

    if (!buffer || buffer_length == 0)
    {
        return 0;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer, buffer_length, fmt, args);
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

static char *env_skip_spaces(char *text)
{
    while (text && (*text == ' ' || *text == '\t'))
    {
        text++;
    }
    return text;
}

static void env_trim_text(char *text)
{
    char *start;
    char *end;

    if (!text)
    {
        return;
    }

    start = env_skip_spaces(text);
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

static char *env_next_token(char *text, char *token, size_t token_length)
{
    size_t i = 0;

    text = env_skip_spaces(text);
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
    return env_skip_spaces(text);
}

static void env_copy_text(char *dst, size_t dst_length, const char *src)
{
    if (!dst || dst_length == 0)
    {
        return;
    }
    strncpy(dst, src ? src : "", dst_length - 1);
    dst[dst_length - 1] = '\0';
}

static bool env_parse_assignment(char *name, char *rest, char *key, size_t key_length, char *value, size_t value_length)
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
        env_normalize_key(name, key, key_length);
        env_copy_text(value, value_length, equals + 1);
        if (rest && rest[0] != '\0')
        {
            if (value[0] != '\0')
            {
                strncat(value, " ", value_length - strlen(value) - 1);
            }
            strncat(value, rest, value_length - strlen(value) - 1);
        }
        env_trim_text(value);
        return key[0] != '\0';
    }

    rest = env_skip_spaces(rest);
    if (!rest || rest[0] != '=')
    {
        return false;
    }

    env_normalize_key(name, key, key_length);
    assign_value = env_skip_spaces(rest + 1);
    env_copy_text(value, value_length, assign_value);
    env_trim_text(value);
    return key[0] != '\0';
}

static bool env_split_delta(char *text, char *key, size_t key_length, char *delta, size_t delta_length)
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
    env_trim_text(key);

    number = env_skip_spaces(op + 1);
    if (!number || *number == '\0')
    {
        return false;
    }

    delta[0] = *op;
    strncpy(delta + 1, number, delta_length - 2);
    delta[delta_length - 1] = '\0';
    env_trim_text(delta);
    return env_is_number(delta);
}

static bool env_parse_delta(char *name, char *rest, char *key, size_t key_length, char *delta, size_t delta_length)
{
    size_t name_length;
    char *delta_text;

    if (!name || !key || key_length == 0 || !delta || delta_length < 2)
    {
        return false;
    }

    if (env_split_delta(name, key, key_length, delta, delta_length))
    {
        env_normalize_key(key, key, key_length);
        return true;
    }

    name_length = strlen(name);
    delta_text = env_skip_spaces(rest);
    if (name_length > 1 && (name[name_length - 1] == '+' || name[name_length - 1] == '-') &&
        delta_text && delta_text[0] != '\0')
    {
        delta[0] = name[name_length - 1];
        env_next_token(delta_text, delta + 1, delta_length - 1);
        if (env_is_number(delta))
        {
            name[name_length - 1] = '\0';
            env_normalize_key(name, key, key_length);
            return true;
        }
        return false;
    }

    if (delta_text && (delta_text[0] == '+' || delta_text[0] == '-'))
    {
        delta[0] = delta_text[0];
        delta_text = env_skip_spaces(delta_text + 1);
        env_next_token(delta_text, delta + 1, delta_length - 1);
        if (env_is_number(delta))
        {
            env_normalize_key(name, key, key_length);
            return true;
        }
        return false;
    }

    env_next_token(rest, delta, delta_length);
    if (delta[0] != '\0' && env_is_number(delta))
    {
        env_normalize_key(name, key, key_length);
        return true;
    }

    delta[0] = '\0';
    return false;
}

static bool env_is_number(const char *text)
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

static bool env_is_now(const char *text)
{
    return text && toupper((unsigned char)text[0]) == 'N' &&
           toupper((unsigned char)text[1]) == 'O' &&
           toupper((unsigned char)text[2]) == 'W' && text[3] == '\0';
}

static bool env_is_help(const char *text)
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

static void env_now(char *buffer, size_t buffer_length)
{
    time_t now = time(NULL);
    long long seconds = (long long)(now - s_start_time);
    if (seconds < 0)
    {
        seconds = 0;
    }
    snprintf(buffer, buffer_length, "%lld", seconds);
}

static int env_eval_value(const char *expr, char *result, size_t result_length, char *error, size_t error_length)
{
    const char *op_pos = NULL;
    char op = 0;
    int64_t left_value;
    int64_t right_value;
    int64_t calc_value;
    size_t i;
    size_t left_len;

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
        if (env_is_number(expr))
        {
            return 0;
        }
        if (env_is_now(expr))
        {
            env_now(result, result_length);
            return 1;
        }
        env_normalize_key(expr, s_eval_key, ENV_KEY_SIZE);
        if (env_get_value(s_eval_key, result, result_length) == ENV_STATUS_OK)
        {
            return 1;
        }
        return 0;
    }

    left_len = (size_t)(op_pos - expr);
    if (left_len >= ENV_VALUE_SIZE)
    {
        left_len = ENV_VALUE_SIZE - 1;
    }
    memcpy(s_eval_left, expr, left_len);
    s_eval_left[left_len] = '\0';
    env_trim_text(s_eval_left);

    strncpy(s_eval_right, op_pos + 1, ENV_VALUE_SIZE - 1);
    s_eval_right[ENV_VALUE_SIZE - 1] = '\0';
    env_trim_text(s_eval_right);

    if (env_value_to_i64(s_eval_left, &left_value, error, error_length) != ENV_STATUS_OK)
    {
        return -1;
    }
    if (env_value_to_i64(s_eval_right, &right_value, error, error_length) != ENV_STATUS_OK)
    {
        return -1;
    }

    calc_value = (op == '+') ? (left_value + right_value) : (left_value - right_value);
    snprintf(result, result_length, "%lld", (long long)calc_value);
    return 1;
}

static int env_value_to_i64(const char *expr, int64_t *value, char *error, size_t error_length)
{
    char *endptr;

    if (env_is_number(expr))
    {
        *value = strtoll(expr, &endptr, 10);
        return (*endptr == '\0') ? ENV_STATUS_OK : ENV_STATUS_READ;
    }

    if (env_is_now(expr))
    {
        env_now(s_eval_value, ENV_VALUE_SIZE);
    }
    else
    {
        env_normalize_key(expr, s_eval_key, ENV_KEY_SIZE);
        if (env_get_value(s_eval_key, s_eval_value, ENV_VALUE_SIZE) != ENV_STATUS_OK)
        {
            env_format_response(error, error_length, "Error: %s not found\r\n", expr);
            return ENV_STATUS_NOT_FOUND;
        }
    }

    if (!env_is_number(s_eval_value))
    {
        env_format_response(error, error_length, "Error: %s not numeric\r\n", expr);
        return ENV_STATUS_READ;
    }

    *value = strtoll(s_eval_value, &endptr, 10);
    return (*endptr == '\0') ? ENV_STATUS_OK : ENV_STATUS_READ;
}

static size_t env_help_text(char *buffer, size_t buffer_length)
{
    return env_write_text(buffer, buffer_length,
        "ENV - Environment Variable Manager\r\n"
        "==================================\r\n"
        "Stored on host in a text file; available from CP/M through DXENV.\r\n"
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
        "Spaces around =, +, and - are optional.\r\n"
        "Limits: names 16 chars, values 127 chars. Names stored uppercase.\r\n"
        "Ports: command/status 71, data 72, response 200.\r\n");
}

static size_t env_execute_cli(char *buffer, size_t buffer_length)
{
    char *rest;
    size_t pos = 0;
    size_t request_len;
    size_t evaled_len;
    int rc;
    int64_t current;
    int64_t delta;

    s_status = ENV_STATUS_OK;
    s_error[0] = '\0';

    request_len = strnlen(s_request, ENV_REQUEST_SIZE - 1);
    memcpy(s_work, s_request, request_len);
    s_work[request_len] = '\0';
    rest = env_next_token(s_work, s_arg1, ENV_VALUE_SIZE);

    if (s_arg1[0] == '\0')
    {
        pos = env_list_values(buffer, buffer_length);
        if (pos == 0)
        {
            pos = env_write_text(buffer, buffer_length, "(no variables set)\r\n");
        }
        env_reset_request();
        return pos;
    }

    if (env_is_help(s_arg1))
    {
        pos = env_help_text(buffer, buffer_length);
        env_reset_request();
        return pos;
    }

    if (s_arg1[0] == '-' && toupper((unsigned char)s_arg1[1]) == 'N')
    {
        pos = env_format_response(buffer, buffer_length, "%u variable(s) set\r\n", (unsigned)s_count);
        env_reset_request();
        return pos;
    }

    if (s_arg1[0] == '-' && toupper((unsigned char)s_arg1[1]) == 'D')
    {
        env_next_token(rest, s_arg2, ENV_VALUE_SIZE);
        if (s_arg2[0] == '\0')
        {
            s_status = ENV_STATUS_READ;
            pos = env_write_text(buffer, buffer_length, "Usage: ENV -D NAME\r\n");
        }
        else
        {
            env_normalize_key(s_arg2, s_key, ENV_KEY_SIZE);
            rc = env_delete_value(s_key);
            s_status = (int8_t)rc;
            if (rc == ENV_STATUS_OK)
            {
                pos = env_format_response(buffer, buffer_length, "%s deleted\r\n", s_key);
            }
            else if (rc == ENV_STATUS_NOT_FOUND)
            {
                pos = env_format_response(buffer, buffer_length, "%s not found\r\n", s_key);
            }
            else
            {
                pos = env_format_response(buffer, buffer_length, "Error deleting %s\r\n", s_key);
            }
        }
        env_reset_request();
        return pos;
    }

    if (s_arg1[0] == '-' && toupper((unsigned char)s_arg1[1]) == 'I')
    {
        rest = env_next_token(rest, s_arg2, ENV_VALUE_SIZE);
        if (!env_parse_assignment(s_arg2, rest, s_key, ENV_KEY_SIZE, s_value, ENV_VALUE_SIZE))
        {
            s_status = ENV_STATUS_READ;
            pos = env_write_text(buffer, buffer_length, "Usage: ENV -I NAME=VAL\r\n");
        }
        else
        {
            rc = env_get_value(s_key, s_evaled, ENV_VALUE_SIZE);
            if (rc == ENV_STATUS_OK)
            {
                pos = env_format_response(buffer, buffer_length, "%s already defined\r\n", s_key);
            }
            else
            {
                rc = env_set_value(s_key, s_value);
                s_status = (int8_t)rc;
                pos = rc == ENV_STATUS_OK ?
                    env_format_response(buffer, buffer_length, "%s=%s\r\n", s_key, s_value) :
                    env_format_response(buffer, buffer_length, "Error setting %s\r\n", s_key);
            }
        }
        env_reset_request();
        return pos;
    }

    if (env_parse_assignment(s_arg1, rest, s_key, ENV_KEY_SIZE, s_value, ENV_VALUE_SIZE))
    {
        rc = env_eval_value(s_value, s_evaled, ENV_VALUE_SIZE, s_error, ENV_ERROR_SIZE);
        if (rc < 0)
        {
            s_status = ENV_STATUS_READ;
            pos = env_write_text(buffer, buffer_length, s_error);
            env_reset_request();
            return pos;
        }
        if (rc > 0)
        {
            evaled_len = strnlen(s_evaled, ENV_VALUE_SIZE - 1);
            memcpy(s_value, s_evaled, evaled_len);
            s_value[evaled_len] = '\0';
        }

        rc = env_set_value(s_key, s_value);
        s_status = (int8_t)rc;
        pos = rc == ENV_STATUS_OK ?
            env_format_response(buffer, buffer_length, "%s=%s\r\n", s_key, s_value) :
            env_format_response(buffer, buffer_length, "Error setting %s\r\n", s_key);
        env_reset_request();
        return pos;
    }

    if (env_parse_delta(s_arg1, rest, s_key, ENV_KEY_SIZE, s_arg2, ENV_VALUE_SIZE))
    {
        current = 0;
        delta = strtoll(s_arg2, NULL, 10);

        if (env_get_value(s_key, s_value, ENV_VALUE_SIZE) == ENV_STATUS_OK)
        {
            if (!env_is_number(s_value))
            {
                s_status = ENV_STATUS_READ;
                pos = env_format_response(buffer, buffer_length, "Error: %s is not numeric\r\n", s_key);
                env_reset_request();
                return pos;
            }
            current = strtoll(s_value, NULL, 10);
        }

        snprintf(s_value, ENV_VALUE_SIZE, "%lld", (long long)(current + delta));
        rc = env_set_value(s_key, s_value);
        s_status = (int8_t)rc;
        pos = rc == ENV_STATUS_OK ?
            env_format_response(buffer, buffer_length, "%s=%s\r\n", s_key, s_value) :
            env_format_response(buffer, buffer_length, "Error setting %s\r\n", s_key);
        env_reset_request();
        return pos;
    }

    if (env_is_now(s_arg1))
    {
        env_now(s_value, ENV_VALUE_SIZE);
        pos = env_format_response(buffer, buffer_length, "NOW=%s\r\n", s_value);
        env_reset_request();
        return pos;
    }

    env_normalize_key(s_arg1, s_key, ENV_KEY_SIZE);
    rc = env_get_value(s_key, s_value, ENV_VALUE_SIZE);
    s_status = (int8_t)rc;
    if (rc == ENV_STATUS_OK)
    {
        pos = env_format_response(buffer, buffer_length, "%s=%s\r\n", s_key, s_value);
    }
    else
    {
        pos = env_format_response(buffer, buffer_length, "%s not found\r\n", s_key);
    }

    env_reset_request();
    return pos;
}
