#define _GNU_SOURCE

#include "host_files_io.h"
#include "universal_88dcdd.h"

#include "../Altair8800/intel8080.h"
#include "../Altair8800/memory.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup _strdup
#else
#include <strings.h>
#include <sys/time.h>
#endif
#include <time.h>

#define INPUT_CAP 16384
#define OUTPUT_CAP 1048576
#define BOOT_CYCLES 20000000
#define DEFAULT_CALL_CYCLES 12000000
#define BUILD_STEP_CYCLES 800000000
#define BUILD_MAX_STEPS 80
#define BUILD_TIMEOUT_SECONDS 5
#define SUBMIT_MAX_STEPS 300
#define SUBMIT_TIMEOUT_SECONDS 10

static const char *tools_list_result =
    "{"
    "\"tools\":["
    "{"
    "\"name\":\"run_cpm\","
    "\"description\":\"Run terminal input in the current stateful CP/M 2.2 session on the Altair emulator. IMPORTANT for builds: after reset CP/M is on A:, but B: contains BDS-C and the build tools. Do not start with submit <app>. First change to B: and fetch the submit file with FT, for example: b:\\\\nft -g breakout/breakout.sub. After FT reports Done, run submit breakout. SuperSUB runs one command per CP/M prompt, so call this tool again with empty input to advance each pending submit step. Submit files can end with mcpdone <name>; MCPDONE.COM prints MCP-TOOL-COMPLETED <NAME>, which is the standard completion marker to stop on. The session keeps disk and memory state between calls until reset is called. FT paths are served from the repo Apps directory; examples include ft -g snake/snake.sub, ft -g breakout/breakout.sub, and ft -g file://sdk/dxterm.c.\","
    "\"inputSchema\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"input\":{"
    "\"type\":\"string\","
    "\"description\":\"Text to type at the CP/M terminal. Build pattern: first use b:\\\\nft -g breakout/breakout.sub, then use submit breakout, then use empty input repeatedly to advance SuperSUB. Stop when the output contains MCP-TOOL-COMPLETED <NAME> if the submit file ends with mcpdone <name>. Newlines become carriage returns. Empty string runs the emulator until the next prompt without injecting a carriage return.\""
    "},"
    "\"cycles\":{"
    "\"type\":\"integer\","
    "\"description\":\"Maximum Intel 8080 cycles to run before returning. Increase this for long compiles, links, or submit-file steps. Default is enough for short commands.\""
    "}"
    "},"
    "\"required\":[\"input\"]"
    "}"
    "},"
    "{"
    "\"name\":\"build_app\","
    "\"description\":\"Build one app end-to-end in CP/M with one MCP call. This resets to fresh disks by default, changes to B:, fetches <app>/<app>.sub with FT, runs submit <app>, advances SuperSUB internally, and stops when MCPDONE prints MCP-TOOL-COMPLETED <APP>. Use this instead of many run_cpm empty-input calls for normal builds.\","
    "\"inputSchema\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"app\":{"
    "\"type\":\"string\","
    "\"description\":\"App folder/base name to build, for example breakout, snake, tetris, or pico. The submit file is fetched as <app>/<app>.sub.\""
    "},"
    "\"reset\":{"
    "\"type\":\"boolean\","
    "\"description\":\"When true, restore fresh disks before building. Defaults to true.\""
    "},"
    "\"max_steps\":{"
    "\"type\":\"integer\","
    "\"description\":\"Maximum SuperSUB prompt steps to advance before giving up. Defaults to 80.\""
    "},"
    "\"timeout_seconds\":{"
    "\"type\":\"integer\","
    "\"description\":\"Wall-clock timeout for the whole build. Defaults to 5 seconds, which is usually enough for host-side CP/M builds.\""
    "},"
    "\"verbose\":{"
    "\"type\":\"boolean\","
    "\"description\":\"When true, return the full CP/M transcript. Defaults to true. Set false for a compact build summary.\""
    "}"
    "},"
    "\"required\":[\"app\"]"
    "}"
    "},"
    "{"
    "\"name\":\"run_submit\","
    "\"description\":\"Run an arbitrary CP/M SuperSUB submit workflow in one MCP call. This resets to fresh disks by default, changes to B:, optionally fetches a submit file with FT, runs submit <submit>, advances prompts internally, and stops when the configured marker appears. Use this for BUILDALL.SUB or other submit jobs that do not match the build_app <app>/<app>.sub convention.\","
    "\"inputSchema\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"submit\":{"
    "\"type\":\"string\","
    "\"description\":\"Submit base name to run, for example buildall. The CP/M command will be submit <submit>.\""
    "},"
    "\"fetch\":{"
    "\"type\":\"string\","
    "\"description\":\"Optional FT path for the submit file. Defaults to <submit>.sub, then falls back to <submit>/<submit>.sub if the root fetch fails. Use an empty string to skip fetching.\""
    "},"
    "\"marker\":{"
    "\"type\":\"string\","
    "\"description\":\"Completion marker to stop on. Defaults to MCP-TOOL-COMPLETED <SUBMIT>.\""
    "},"
    "\"reset\":{"
    "\"type\":\"boolean\","
    "\"description\":\"When true, restore fresh disks before running. Defaults to true.\""
    "},"
    "\"max_steps\":{"
    "\"type\":\"integer\","
    "\"description\":\"Maximum SuperSUB prompt steps to advance before giving up. Defaults to 300.\""
    "},"
    "\"timeout_seconds\":{"
    "\"type\":\"integer\","
    "\"description\":\"Wall-clock timeout for the submit job. Defaults to 10 seconds.\""
    "},"
    "\"verbose\":{"
    "\"type\":\"boolean\","
    "\"description\":\"When true, return the full CP/M transcript. Defaults to true. Set false for a compact summary.\""
    "}"
    "},"
    "\"required\":[\"submit\"]"
    "}"
    "},"
    "{"
    "\"name\":\"reset\","
    "\"description\":\"Restore fresh working disk images for A: CP/M, B: BDS-C/tools, and C: blank scratch, then reboot CP/M to the A> prompt. Use this when you want a deterministic clean build environment. Do not use it between dependent run_cpm calls unless you intentionally want to discard files created on the CP/M disks.\","
    "\"inputSchema\":{"
    "\"type\":\"object\","
    "\"properties\":{}"
    "}"
    "}"
    "]"
    "}";

static intel8080_t g_cpu;
static const char *g_drive_a;
static const char *g_drive_b;
static const char *g_drive_c;
static const char *g_pristine_a;
static const char *g_pristine_b;
static const char *g_pristine_c;
static const char *g_apps_root;
static bool g_booted = false;
static uint8_t g_input[INPUT_CAP];
static size_t g_input_read = 0;
static size_t g_input_write = 0;
static char g_output[OUTPUT_CAP];
static size_t g_output_len = 0;

static uint8_t terminal_read(void)
{
    uint8_t ch;

    if (g_input_read == g_input_write) {
        return 0x00;
    }

    ch = g_input[g_input_read % INPUT_CAP];
    g_input_read++;
    return ch & 0x7f;
}

static void terminal_write(uint8_t c)
{
    c &= 0x7f;
    if (g_output_len + 1 < sizeof(g_output)) {
        g_output[g_output_len++] = (char)c;
        g_output[g_output_len] = '\0';
    }
}

static uint8_t sense_switches(void)
{
    return 0xff;
}

static uint8_t io_port_in(uint8_t port)
{
    if (port == 60 || port == 61) {
        return host_files_in(port);
    }
    return 0x00;
}

static void io_port_out(uint8_t port, uint8_t data)
{
    if (port == 60 || port == 61) {
        host_files_out(port, data);
    }
}

static void enqueue_text(const char *text)
{
    while (*text) {
        if (g_input_write - g_input_read >= INPUT_CAP) {
            break;
        }
        if (*text == '\n') {
            g_input[g_input_write % INPUT_CAP] = '\r';
        } else {
            g_input[g_input_write % INPUT_CAP] = (uint8_t)*text;
        }
        g_input_write++;
        text++;
    }
}

static bool output_has_prompt(char boot_only)
{
    size_t i = g_output_len;
    char ch;

    if (g_output_len < 2) {
        return false;
    }

    while (i > 0 && (g_output[i - 1] == '\r' || g_output[i - 1] == '\n' || g_output[i - 1] == ' ')) {
        i--;
    }
    if (i < 2 || g_output[i - 1] != '>') {
        return false;
    }

    ch = (char)toupper((unsigned char)g_output[i - 2]);
    if (boot_only && ch == 'A') {
        return true;
    }
    if (!boot_only && ch >= 'A' && ch <= 'P') {
        return true;
    }
    return false;
}

static bool input_empty(void)
{
    return g_input_read == g_input_write;
}

static void run_cycles(size_t cycles)
{
    size_t i;

    for (i = 0; i < cycles; i++) {
        i8080_cycle(&g_cpu);
    }
}

static bool run_until_prompt(size_t max_cycles, char boot_only)
{
    size_t i;

    for (i = 0; i < max_cycles; i++) {
        i8080_cycle(&g_cpu);
        if ((i & 0x3fff) == 0 && input_empty() && output_has_prompt(boot_only)) {
            return true;
        }
    }
    return input_empty() && output_has_prompt(boot_only);
}

static bool emulator_boot(const char *drive_a, const char *drive_b, const char *drive_c)
{
    disk_controller_t controller;

    host_disk_close();
    g_input_read = 0;
    g_input_write = 0;
    g_output_len = 0;
    g_output[0] = '\0';

    if (!host_disk_init(drive_a, drive_b, drive_c)) {
        fprintf(stderr, "failed to open MCP disk images\n");
        return false;
    }
    host_files_init(g_apps_root);

    controller = host_disk_controller();
    memset(memory, 0, 64 * 1024);
    loadDiskLoader(0xff00);
    i8080_reset(&g_cpu, terminal_read, terminal_write, sense_switches, &controller, io_port_in, io_port_out);
    i8080_examine(&g_cpu, 0xff00);

    if (!run_until_prompt(BOOT_CYCLES, 1)) {
        fprintf(stderr, "CP/M boot prompt was not seen\n");
        return false;
    }

    g_output_len = 0;
    g_output[0] = '\0';
    g_booted = true;
    return true;
}

static bool copy_file(const char *src_path, const char *dst_path)
{
    FILE *src;
    FILE *dst;
    unsigned char buffer[16384];
    size_t n;
    bool ok = true;

    src = fopen(src_path, "rb");
    if (!src) {
        return false;
    }

    dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }

    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, n, dst) != n) {
            ok = false;
            break;
        }
    }

    if (ferror(src)) {
        ok = false;
    }

    if (fclose(dst) != 0) {
        ok = false;
    }
    fclose(src);
    return ok;
}

static bool reset_emulator(void)
{
    host_disk_close();
    g_booted = false;

    if (!copy_file(g_pristine_a, g_drive_a)) {
        return false;
    }
    if (!copy_file(g_pristine_b, g_drive_b)) {
        return false;
    }
    if (!copy_file(g_pristine_c, g_drive_c)) {
        return false;
    }

    return emulator_boot(g_drive_a, g_drive_b, g_drive_c);
}

static bool ensure_booted(void)
{
    if (g_booted) {
        return true;
    }

    return emulator_boot(g_drive_a, g_drive_b, g_drive_c);
}

static char *json_escape_dup(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t cap = (strlen(text) * 6) + 3;
    char *out = (char *)malloc(cap);
    char *dst;

    if (!out) {
        return NULL;
    }

    dst = out;

    *dst++ = '"';
    while (*p) {
        switch (*p) {
        case '"':
            memcpy(dst, "\\\"", 2);
            dst += 2;
            break;
        case '\\':
            memcpy(dst, "\\\\", 2);
            dst += 2;
            break;
        case '\b':
            memcpy(dst, "\\b", 2);
            dst += 2;
            break;
        case '\f':
            memcpy(dst, "\\f", 2);
            dst += 2;
            break;
        case '\n':
            memcpy(dst, "\\n", 2);
            dst += 2;
            break;
        case '\r':
            memcpy(dst, "\\r", 2);
            dst += 2;
            break;
        case '\t':
            memcpy(dst, "\\t", 2);
            dst += 2;
            break;
        default:
            if (*p < 0x20) {
                snprintf(dst, 7, "\\u%04x", *p);
                dst += 6;
            } else {
                *dst++ = (char)*p;
            }
            break;
        }
        p++;
    }
    *dst++ = '"';
    *dst = '\0';
    return out;
}

static char *json_get_string(const char *json, const char *key)
{
    char pattern[80];
    const char *p;
    char *out;
    size_t len = 0;
    size_t cap = 256;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return NULL;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return NULL;
    }
    p++;

    out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }

    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\') {
            ch = *p++;
            switch (ch) {
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            default:
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
            if (!out) {
                return NULL;
            }
        }
        out[len++] = ch;
    }
    out[len] = '\0';
    return out;
}

static char *json_get_id(const char *json)
{
    const char *p = strstr(json, "\"id\"");
    const char *start;
    size_t len;
    char *id;

    if (!p) {
        return NULL;
    }
    p = strchr(p + 4, ':');
    if (!p) {
        return NULL;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    start = p;
    if (*p == '"') {
        p++;
        while (*p && (*p != '"' || *(p - 1) == '\\')) {
            p++;
        }
        if (*p == '"') {
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
            p++;
        }
    }
    len = (size_t)(p - start);
    id = (char *)malloc(len + 1);
    if (!id) {
        return NULL;
    }
    memcpy(id, start, len);
    id[len] = '\0';
    return id;
}

static int json_get_int(const char *json, const char *key, int fallback)
{
    char pattern[80];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return fallback;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return fallback;
    }
    return atoi(p + 1);
}

static bool json_get_bool(const char *json, const char *key, bool fallback)
{
    char pattern[80];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return fallback;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return fallback;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        return false;
    }
    return fallback;
}

static char *json_get_protocol_version(const char *json)
{
    char *version = json_get_string(json, "protocolVersion");

    if (version) {
        return version;
    }

    return strdup("2024-11-05");
}

static void send_body(const char *body)
{
    fputs(body, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_simple_result(const char *id, const char *result)
{
    char *body;
    size_t len;

    len = strlen(id) + strlen(result) + 48;
    body = (char *)malloc(len);
    if (!body) {
        return;
    }
    snprintf(body, len, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
    send_body(body);
    free(body);
}

static void send_error(const char *id, int code, const char *message)
{
    char body[1024];

    snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id ? id : "null", code, message);
    send_body(body);
}

static void send_tool_text_result(const char *id, const char *text)
{
    char *escaped;
    char *body;

    escaped = json_escape_dup(text);
    if (!escaped) {
        send_error(id, -32603, "failed to allocate response");
        return;
    }

    body = (char *)malloc(strlen(escaped) + 256);
    if (!body) {
        send_error(id, -32603, "failed to allocate response");
        free(escaped);
        return;
    }

    snprintf(body, strlen(escaped) + 256,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"isError\":false}}",
             id, escaped);
    send_body(body);
    free(body);
    free(escaped);
}

static void append_output_hint(const char *hint)
{
    size_t hint_len = strlen(hint);

    if (g_output_len + hint_len + 1 >= sizeof(g_output)) {
        return;
    }

    memcpy(&g_output[g_output_len], hint, hint_len + 1);
    g_output_len += hint_len;
}

static bool append_text(char **buf, size_t *len, size_t *cap, const char *text)
{
    size_t text_len = strlen(text);
    char *next;

    if (*len + text_len + 1 > *cap) {
        while (*len + text_len + 1 > *cap) {
            *cap *= 2;
        }
        next = (char *)realloc(*buf, *cap);
        if (!next) {
            return false;
        }
        *buf = next;
    }

    memcpy(*buf + *len, text, text_len + 1);
    *len += text_len;
    return true;
}

static bool run_cpm_step(const char *input, size_t cycles)
{
    if (!ensure_booted()) {
        return false;
    }

    g_output_len = 0;
    g_output[0] = '\0';
    if (input && strlen(input) > 0) {
        enqueue_text(input);
        if (input[strlen(input) - 1] != '\r') {
            enqueue_text("\r");
        }
    }
    run_until_prompt(cycles, 0);
    run_cycles(5000);
    return true;
}

static bool app_name_ok(const char *app)
{
    const char *p;

    if (!app || !*app) {
        return false;
    }
    for (p = app; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

static bool ft_path_ok(const char *path)
{
    const char *p;

    if (!path) {
        return false;
    }
    for (p = path; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.' && *p != '/' && *p != ':') {
            return false;
        }
    }
    return true;
}

static void upper_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < dst_size; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void lower_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < dst_size; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool output_has_error(const char *text)
{
    if (strstr(text, "Cannot open")) {
        return true;
    }
    if (strstr(text, "Can't find")) {
        return true;
    }
    if (strstr(text, "ABORTED") || strstr(text, "Aborted")) {
        return true;
    }
    if (strstr(text, "FAILED") || strstr(text, "Failed")) {
        return true;
    }
    if (strstr(text, "ERROR") || strstr(text, "Error")) {
        return true;
    }
    if (strstr(text, "Bad parameter") || strstr(text, "Bad expression") || strstr(text, "Bad syntax")) {
        return true;
    }
    if (strstr(text, "Undeclared identifier") || strstr(text, "Redeclaration")) {
        return true;
    }
    if (strstr(text, "Missing") || strstr(text, "Illegal")) {
        return true;
    }
    if (strstr(text, "SUBMIT?") || strstr(text, "CC?") || strstr(text, "CLINK?") || strstr(text, "FT?")) {
        return true;
    }
    return false;
}

static long long now_ms(void)
{
#if defined(_WIN32)
    return (long long)GetTickCount64();
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
#endif
}

static void handle_build_app(const char *id, const char *json)
{
    char *app = json_get_string(json, "app");
    bool do_reset = json_get_bool(json, "reset", true);
    int max_steps = json_get_int(json, "max_steps", BUILD_MAX_STEPS);
    int timeout_seconds = json_get_int(json, "timeout_seconds", BUILD_TIMEOUT_SECONDS);
    bool verbose = json_get_bool(json, "verbose", true);
    char app_lc[64];
    char app_uc[64];
    char cmd[192];
    char marker[128];
    char *log;
    size_t log_len = 0;
    size_t log_cap = 8192;
    int step;
    bool done = false;
    bool failed = false;
    const char *fail_reason = NULL;
    time_t deadline;
    long long build_start;
    long long step_start;
    long long total_ms;
    char summary[2048];
    const char *tail;

    if (!app_name_ok(app)) {
        send_error(id, -32602, "build_app requires an app name containing only letters, digits, underscore, or hyphen");
        free(app);
        return;
    }
    if (max_steps <= 0 || max_steps > 500) {
        max_steps = BUILD_MAX_STEPS;
    }
    if (timeout_seconds <= 0 || timeout_seconds > 3600) {
        timeout_seconds = BUILD_TIMEOUT_SECONDS;
    }
    deadline = time(NULL) + timeout_seconds;
    build_start = now_ms();

    lower_copy(app_lc, sizeof(app_lc), app);
    upper_copy(app_uc, sizeof(app_uc), app);
    snprintf(marker, sizeof(marker), "MCP-TOOL-COMPLETED %s", app_uc);

    log = (char *)malloc(log_cap);
    if (!log) {
        send_error(id, -32603, "failed to allocate build log");
        free(app);
        return;
    }
    log[0] = '\0';

    if (do_reset) {
        if (!reset_emulator()) {
            send_error(id, -32603, "failed to reset CP/M disk images");
            free(log);
            free(app);
            return;
        }
        append_text(&log, &log_len, &log_cap, "RESET complete.\n");
    } else if (!ensure_booted()) {
        send_error(id, -32603, "failed to boot CP/M");
        free(log);
        free(app);
        return;
    }

    snprintf(cmd, sizeof(cmd), "b:\nft -g %s/%s.sub", app_lc, app_lc);
    step_start = now_ms();
    if (!run_cpm_step(cmd, BUILD_STEP_CYCLES)) {
        send_error(id, -32603, "failed during FT submit download");
        free(log);
        free(app);
        return;
    }
    snprintf(cmd, sizeof(cmd), "--- FETCH SUBMIT (%lld ms) ---\n", now_ms() - step_start);
    append_text(&log, &log_len, &log_cap, cmd);
    append_text(&log, &log_len, &log_cap, g_output);
    if (output_has_error(g_output)) {
        failed = true;
        fail_reason = "submit file fetch failed";
    }

    snprintf(cmd, sizeof(cmd), "submit %s", app_lc);
    step_start = now_ms();
    if (!failed && !run_cpm_step(cmd, BUILD_STEP_CYCLES)) {
        send_error(id, -32603, "failed starting submit");
        free(log);
        free(app);
        return;
    }
    if (!failed) {
        snprintf(cmd, sizeof(cmd), "--- SUBMIT START (%lld ms) ---\n", now_ms() - step_start);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, g_output);
        if (strstr(g_output, marker)) {
            done = true;
        } else if (output_has_error(g_output)) {
            failed = true;
            fail_reason = "submit failed to start";
        }
    }

    for (step = 1; !done && !failed && step <= max_steps; step++) {
        if (time(NULL) >= deadline) {
            failed = true;
            fail_reason = "build timed out";
            break;
        }
        step_start = now_ms();
        if (!run_cpm_step("", BUILD_STEP_CYCLES)) {
            send_error(id, -32603, "failed advancing submit");
            free(log);
            free(app);
            return;
        }
        snprintf(cmd, sizeof(cmd), "--- SUBMIT STEP %d (%lld ms) ---\n", step, now_ms() - step_start);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, g_output);
        if (strstr(g_output, marker)) {
            done = true;
        } else if (output_has_error(g_output)) {
            failed = true;
            fail_reason = "CP/M command reported an error";
        }
    }

    total_ms = now_ms() - build_start;

    if (done) {
        snprintf(cmd, sizeof(cmd), "\nBUILD RESULT: PASS (%lld ms) - ", total_ms);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, marker);
        append_text(&log, &log_len, &log_cap, "\n");
    } else if (failed) {
        snprintf(cmd, sizeof(cmd), "\nBUILD RESULT: FAIL (%lld ms) - ", total_ms);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, fail_reason ? fail_reason : "build failed before completion marker");
        append_text(&log, &log_len, &log_cap, "\n");
    } else {
        snprintf(cmd, sizeof(cmd), "\nBUILD RESULT: INCOMPLETE (%lld ms) - completion marker not seen: ", total_ms);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, marker);
        append_text(&log, &log_len, &log_cap, "\n");
    }

    if (verbose) {
        send_tool_text_result(id, log);
    } else if (done) {
        snprintf(summary, sizeof(summary),
                 "BUILD RESULT: PASS (%lld ms) - %s\n"
                 "APP: %s\n"
                 "OUTPUT: A:%s.COM\n"
                 "STEPS: %d\n"
                 "NOTE: Full CP/M transcript suppressed. Re-run build_app with verbose=true if debugging is needed.\n",
                 total_ms, marker, app_uc, app_uc, step);
        send_tool_text_result(id, summary);
    } else {
        tail = log;
        if (strlen(log) > 1600) {
            tail = log + strlen(log) - 1600;
        }
        snprintf(summary, sizeof(summary),
                 "BUILD RESULT: %s (%lld ms) - %s\n"
                 "APP: %s\n"
                 "MARKER: %s\n"
                 "LOG TAIL:\n%s",
                 failed ? "FAIL" : "INCOMPLETE",
                 total_ms,
                 failed ? (fail_reason ? fail_reason : "build failed before completion marker")
                        : "completion marker not seen",
                 app_uc, marker, tail);
        send_tool_text_result(id, summary);
    }
    free(log);
    free(app);
}

static void handle_run_submit(const char *id, const char *json)
{
    char *sub = json_get_string(json, "submit");
    char *fetch = json_get_string(json, "fetch");
    char *mark_arg = json_get_string(json, "marker");
    bool do_reset = json_get_bool(json, "reset", true);
    int max_steps = json_get_int(json, "max_steps", SUBMIT_MAX_STEPS);
    int timeout_seconds = json_get_int(json, "timeout_seconds", SUBMIT_TIMEOUT_SECONDS);
    bool verbose = json_get_bool(json, "verbose", true);
    bool default_fetch;
    char sub_lc[64];
    char sub_uc[64];
    char fetch_buf[128];
    char cmd[192];
    char marker[160];
    char *log;
    size_t log_len = 0;
    size_t log_cap = 8192;
    int step;
    bool done = false;
    bool failed = false;
    const char *fail_reason = NULL;
    time_t deadline;
    long long start_ms;
    long long step_start;
    long long total_ms;
    char summary[2048];
    const char *tail;

    if (!app_name_ok(sub)) {
        send_error(id, -32602, "run_submit requires a submit name containing only letters, digits, underscore, or hyphen");
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }
    if (max_steps <= 0 || max_steps > 1000) {
        max_steps = SUBMIT_MAX_STEPS;
    }
    if (timeout_seconds <= 0 || timeout_seconds > 3600) {
        timeout_seconds = SUBMIT_TIMEOUT_SECONDS;
    }

    lower_copy(sub_lc, sizeof(sub_lc), sub);
    upper_copy(sub_uc, sizeof(sub_uc), sub);
    default_fetch = !fetch;
    if (!fetch) {
        snprintf(fetch_buf, sizeof(fetch_buf), "%s.sub", sub_lc);
        fetch = strdup(fetch_buf);
    }
    if (!fetch || !ft_path_ok(fetch)) {
        send_error(id, -32602, "run_submit fetch path contains unsupported characters");
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }
    if (mark_arg && strlen(mark_arg) > 0) {
        snprintf(marker, sizeof(marker), "%s", mark_arg);
    } else {
        snprintf(marker, sizeof(marker), "MCP-TOOL-COMPLETED %s", sub_uc);
    }

    log = (char *)malloc(log_cap);
    if (!log) {
        send_error(id, -32603, "failed to allocate submit log");
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }
    log[0] = '\0';
    deadline = time(NULL) + timeout_seconds;
    start_ms = now_ms();

    if (do_reset) {
        if (!reset_emulator()) {
            send_error(id, -32603, "failed to reset CP/M disk images");
            free(log);
            free(sub);
            free(fetch);
            free(mark_arg);
            return;
        }
        append_text(&log, &log_len, &log_cap, "RESET complete.\n");
    } else if (!ensure_booted()) {
        send_error(id, -32603, "failed to boot CP/M");
        free(log);
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }

    if (strlen(fetch) > 0) {
        snprintf(cmd, sizeof(cmd), "b:\nft -g %s", fetch);
    } else {
        snprintf(cmd, sizeof(cmd), "b:");
    }
    step_start = now_ms();
    if (!run_cpm_step(cmd, BUILD_STEP_CYCLES)) {
        send_error(id, -32603, "failed preparing submit");
        free(log);
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }
    snprintf(cmd, sizeof(cmd), "--- PREPARE SUBMIT (%lld ms) ---\n", now_ms() - step_start);
    append_text(&log, &log_len, &log_cap, cmd);
    append_text(&log, &log_len, &log_cap, g_output);
    if (output_has_error(g_output)) {
        if (default_fetch) {
            snprintf(fetch_buf, sizeof(fetch_buf), "%s/%s.sub", sub_lc, sub_lc);
            if (!ft_path_ok(fetch_buf)) {
                failed = true;
                fail_reason = "submit file fetch failed";
            } else {
                free(fetch);
                fetch = strdup(fetch_buf);
                if (!fetch) {
                    send_error(id, -32603, "failed to allocate fallback fetch path");
                    free(log);
                    free(sub);
                    free(mark_arg);
                    return;
                }
                snprintf(cmd, sizeof(cmd), "ft -g %s", fetch);
                step_start = now_ms();
                if (!run_cpm_step(cmd, BUILD_STEP_CYCLES)) {
                    send_error(id, -32603, "failed preparing submit fallback");
                    free(log);
                    free(sub);
                    free(fetch);
                    free(mark_arg);
                    return;
                }
                snprintf(cmd, sizeof(cmd), "--- PREPARE SUBMIT FALLBACK (%lld ms) ---\n", now_ms() - step_start);
                append_text(&log, &log_len, &log_cap, cmd);
                append_text(&log, &log_len, &log_cap, g_output);
                if (output_has_error(g_output)) {
                    failed = true;
                    fail_reason = "submit file fetch failed";
                }
            }
        } else {
            failed = true;
            fail_reason = "submit file fetch failed";
        }
    }

    snprintf(cmd, sizeof(cmd), "submit %s", sub_lc);
    step_start = now_ms();
    if (!failed && !run_cpm_step(cmd, BUILD_STEP_CYCLES)) {
        send_error(id, -32603, "failed starting submit");
        free(log);
        free(sub);
        free(fetch);
        free(mark_arg);
        return;
    }
    if (!failed) {
        snprintf(cmd, sizeof(cmd), "--- SUBMIT START (%lld ms) ---\n", now_ms() - step_start);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, g_output);
        if (strstr(g_output, marker)) {
            done = true;
        } else if (output_has_error(g_output)) {
            failed = true;
            fail_reason = "submit failed to start";
        }
    }

    for (step = 1; !done && !failed && step <= max_steps; step++) {
        if (time(NULL) >= deadline) {
            failed = true;
            fail_reason = "submit timed out";
            break;
        }
        step_start = now_ms();
        if (!run_cpm_step("", BUILD_STEP_CYCLES)) {
            send_error(id, -32603, "failed advancing submit");
            free(log);
            free(sub);
            free(fetch);
            free(mark_arg);
            return;
        }
        snprintf(cmd, sizeof(cmd), "--- SUBMIT STEP %d (%lld ms) ---\n", step, now_ms() - step_start);
        append_text(&log, &log_len, &log_cap, cmd);
        append_text(&log, &log_len, &log_cap, g_output);
        if (strstr(g_output, marker)) {
            done = true;
        } else if (output_has_error(g_output)) {
            failed = true;
            fail_reason = "CP/M command reported an error";
        }
    }

    total_ms = now_ms() - start_ms;
    if (done) {
        snprintf(cmd, sizeof(cmd), "\nSUBMIT RESULT: PASS (%lld ms) - %s\n", total_ms, marker);
        append_text(&log, &log_len, &log_cap, cmd);
    } else if (failed) {
        snprintf(cmd, sizeof(cmd), "\nSUBMIT RESULT: FAIL (%lld ms) - %s\n", total_ms,
                 fail_reason ? fail_reason : "submit failed before completion marker");
        append_text(&log, &log_len, &log_cap, cmd);
    } else {
        snprintf(cmd, sizeof(cmd), "\nSUBMIT RESULT: INCOMPLETE (%lld ms) - completion marker not seen: %s\n",
                 total_ms, marker);
        append_text(&log, &log_len, &log_cap, cmd);
    }

    if (verbose) {
        send_tool_text_result(id, log);
    } else if (done) {
        snprintf(summary, sizeof(summary),
                 "SUBMIT RESULT: PASS (%lld ms) - %s\n"
                 "SUBMIT: %s\n"
                 "FETCH: %s\n"
                 "STEPS: %d\n",
                 total_ms, marker, sub_uc, fetch, step);
        send_tool_text_result(id, summary);
    } else {
        tail = log;
        if (strlen(log) > 1600) {
            tail = log + strlen(log) - 1600;
        }
        snprintf(summary, sizeof(summary),
                 "SUBMIT RESULT: %s (%lld ms) - %s\n"
                 "SUBMIT: %s\n"
                 "MARKER: %s\n"
                 "LOG TAIL:\n%s",
                 failed ? "FAIL" : "INCOMPLETE", total_ms,
                 failed ? (fail_reason ? fail_reason : "submit failed before completion marker")
                        : "completion marker not seen",
                 sub_uc, marker, tail);
        send_tool_text_result(id, summary);
    }

    free(log);
    free(sub);
    free(fetch);
    free(mark_arg);
}

static void handle_tools_call(const char *id, const char *json)
{
    char *name = json_get_string(json, "name");
    char *input = json_get_string(json, "input");
    int cycles = json_get_int(json, "cycles", DEFAULT_CALL_CYCLES);

    if (!name) {
        send_error(id, -32602, "unknown tool");
        free(input);
        return;
    }

    if (strcmp(name, "reset") == 0) {
        if (!reset_emulator()) {
            send_error(id, -32603, "failed to reset CP/M disk images");
        } else {
            send_tool_text_result(
                id,
                "CP/M reset complete. Fresh disks restored and A> prompt is ready.\n\n"
                "Build workflow: first change to B: and fetch the submit file, for example:\n"
                "  run_cpm input: b:\\nft -g breakout/breakout.sub\n"
                "Then run:\n"
                "  run_cpm input: submit breakout\n"
                "Then call run_cpm with empty input until MCP-TOOL-COMPLETED <NAME> appears.");
        }
        free(name);
        free(input);
        return;
    }

    if (strcmp(name, "build_app") == 0) {
        handle_build_app(id, json);
        free(name);
        free(input);
        return;
    }

    if (strcmp(name, "run_submit") == 0) {
        handle_run_submit(id, json);
        free(name);
        free(input);
        return;
    }

    if (strcmp(name, "run_cpm") != 0 && strcmp(name, "cpm") != 0) {
        send_error(id, -32602, "unknown tool");
        free(name);
        free(input);
        return;
    }

    if (!ensure_booted()) {
        send_error(id, -32603, "failed to boot CP/M");
        free(name);
        free(input);
        return;
    }

    if (!input) {
        input = strdup("");
    }

    run_cpm_step(input, (size_t)cycles);

    if (strstr(g_output, "SUBMIT?")) {
        append_output_hint(
            "\nMCP-HINT: CP/M reported SUBMIT?, usually because you are on A: or the submit file is not present. "
            "For builds, first run: b:\\nft -g <app>/<app>.sub, then run: submit <app>.\n");
    }

    send_tool_text_result(id, g_output);
    free(name);
    free(input);
}

static bool read_message(char **out)
{
    char line[512];
    size_t content_length = 0;
    char *body;
    size_t len;
    char *p;

    if (!fgets(line, sizeof(line), stdin)) {
        return false;
    }

    p = line;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '{') {
        len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) {
            p[--len] = '\0';
        }
        body = strdup(p);
        if (!body) {
            return false;
        }
        *out = body;
        return true;
    }

    do {
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)strtoull(line + 15, NULL, 10);
        }
    } while (fgets(line, sizeof(line), stdin));

    if (content_length == 0 || feof(stdin)) {
        return false;
    }

    body = (char *)malloc(content_length + 1);
    if (!body) {
        return false;
    }
    if (fread(body, 1, content_length, stdin) != content_length) {
        free(body);
        return false;
    }
    body[content_length] = '\0';
    *out = body;
    return true;
}

static void handle_message(const char *json)
{
    char *id = json_get_id(json);
    char *method = json_get_string(json, "method");
    char *version;
    char init_result[512];

    if (!method) {
        free(id);
        return;
    }

    if (!id) {
        fprintf(stderr, "[MCP] notification: %s\n", method);
        free(method);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        fprintf(stderr, "[MCP] initialize\n");
        version = json_get_protocol_version(json);
        snprintf(init_result, sizeof(init_result),
                 "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"altair-cpm-build\",\"version\":\"0.1.0\"}}",
                 version);
        send_simple_result(id, init_result);
        free(version);
    } else if (strcmp(method, "tools/list") == 0) {
        fprintf(stderr, "[MCP] tools/list\n");
        send_simple_result(id, tools_list_result);
    } else if (strcmp(method, "tools/call") == 0) {
        fprintf(stderr, "[MCP] tools/call\n");
        handle_tools_call(id, json);
    } else if (strcmp(method, "ping") == 0) {
        fprintf(stderr, "[MCP] ping\n");
        send_simple_result(id, "{}");
    } else {
        fprintf(stderr, "[MCP] unknown method: %s\n", method);
        send_error(id, -32601, "method not found");
    }

    free(method);
    free(id);
}

int main(int argc, char **argv)
{
    char *message;

    g_drive_a = (argc > 1) ? argv[1] : "disks/cpm63k.dsk";
    g_drive_b = (argc > 2) ? argv[2] : "disks/bdsc-v1.60.dsk";
    g_drive_c = (argc > 3) ? argv[3] : "disks/blank.dsk";
    g_pristine_a = (argc > 4) ? argv[4] : "../Disks/cpm63k.dsk";
    g_pristine_b = (argc > 5) ? argv[5] : "../Disks/bdsc-v1.60.dsk";
    g_pristine_c = (argc > 6) ? argv[6] : "../Disks/blank.dsk";
    g_apps_root = (argc > 7) ? argv[7] : "../Apps";

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "[MCP] altair-cpm-build started\n");

    while (read_message(&message)) {
        handle_message(message);
        free(message);
    }

    host_disk_close();
    return 0;
}
