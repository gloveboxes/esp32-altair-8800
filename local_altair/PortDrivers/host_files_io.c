#define _GNU_SOURCE

#include "host_files_io.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FT_COMMAND_PORT 60
#define FT_DATA_PORT 61

#define FT_CHUNK_SIZE 256
#define FT_STATUS_IDLE 0
#define FT_STATUS_DATAREADY 1
#define FT_STATUS_EOF 2
#define FT_STATUS_ERROR 0xff

typedef enum {
    FT_CMD_NOP = 0,
    FT_CMD_SET_FILENAME = 1,
    FT_CMD_REQUEST_CHUNK = 3,
    FT_CMD_CLOSE = 4
} ft_command_t;

typedef struct {
    char apps_root[512];
    char filename[256];
    size_t filename_len;
    FILE *file;
    uint8_t chunk[FT_CHUNK_SIZE + 1];
    size_t chunk_len;
    size_t chunk_pos;
    bool eof_after_chunk;
    uint8_t status;
} host_files_t;

static host_files_t g_ft;

static void uppercase_component(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    while (*src && i + 1 < dst_size) {
        dst[i++] = (char)toupper((unsigned char)*src++);
    }
    dst[i] = '\0';
}

static bool resolve_path(char *out, size_t out_size)
{
    char tmp[256];
    char *name;
    char *slash;
    char dir[128];
    char file[128];

    name = g_ft.filename;
    if (strncmp(name, "file://", 7) == 0) {
        name += 7;
    } else if (strncmp(name, "FILE://", 7) == 0) {
        name += 7;
    }

    strncpy(tmp, name, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    slash = strchr(tmp, '/');
    if (!slash) {
        slash = strchr(tmp, '\\');
    }

    if (slash) {
        *slash = '\0';
        uppercase_component(dir, sizeof(dir), tmp);
        uppercase_component(file, sizeof(file), slash + 1);
        snprintf(out, out_size, "%s/%s/%s", g_ft.apps_root, dir, file);
    } else {
        uppercase_component(file, sizeof(file), tmp);
        snprintf(out, out_size, "%s/%s", g_ft.apps_root, file);
    }

    return true;
}

static void close_file(void)
{
    if (g_ft.file) {
        fclose(g_ft.file);
        g_ft.file = NULL;
    }
    g_ft.chunk_len = 0;
    g_ft.chunk_pos = 0;
    g_ft.eof_after_chunk = false;
    g_ft.status = FT_STATUS_IDLE;
}

static bool ensure_file_open(void)
{
    char path[1024];

    if (g_ft.file) {
        return true;
    }

    if (g_ft.filename[0] == '\0') {
        g_ft.status = FT_STATUS_ERROR;
        return false;
    }

    resolve_path(path, sizeof(path));
    g_ft.file = fopen(path, "rb");
    if (!g_ft.file) {
        g_ft.status = FT_STATUS_ERROR;
        return false;
    }

    return true;
}

static void request_chunk(void)
{
    size_t n;

    if (g_ft.chunk_pos < g_ft.chunk_len) {
        return;
    }

    if (!ensure_file_open()) {
        return;
    }

    n = fread(&g_ft.chunk[1], 1, FT_CHUNK_SIZE, g_ft.file);
    if (n == 0) {
        g_ft.chunk_len = 0;
        g_ft.chunk_pos = 0;
        g_ft.status = FT_STATUS_EOF;
        return;
    }

    g_ft.chunk[0] = (uint8_t)(n == FT_CHUNK_SIZE ? 0 : n);
    g_ft.chunk_len = n + 1;
    g_ft.chunk_pos = 0;
    g_ft.eof_after_chunk = n < FT_CHUNK_SIZE;
    g_ft.status = FT_STATUS_DATAREADY;
}

static void host_files_output_command(uint8_t data)
{
    switch ((ft_command_t)data) {
    case FT_CMD_NOP:
        break;

    case FT_CMD_SET_FILENAME:
        close_file();
        g_ft.filename_len = 0;
        g_ft.filename[0] = '\0';
        break;

    case FT_CMD_REQUEST_CHUNK:
        request_chunk();
        break;

    case FT_CMD_CLOSE:
        close_file();
        break;

    default:
        break;
    }
}

static void host_files_output_data(uint8_t data)
{
    if (data == 0) {
        g_ft.filename[g_ft.filename_len] = '\0';
        close_file();
        g_ft.status = FT_STATUS_IDLE;
    } else if (g_ft.filename_len + 1 < sizeof(g_ft.filename)) {
        g_ft.filename[g_ft.filename_len++] = (char)data;
    } else {
        g_ft.filename_len = 0;
        g_ft.filename[0] = '\0';
        close_file();
        g_ft.status = FT_STATUS_ERROR;
    }
}

static uint8_t host_files_input_status(void)
{
    if (g_ft.chunk_pos < g_ft.chunk_len) {
        return FT_STATUS_DATAREADY;
    }

    if (g_ft.eof_after_chunk) {
        close_file();
        g_ft.status = FT_STATUS_EOF;
        return FT_STATUS_EOF;
    }

    return g_ft.status;
}

static uint8_t host_files_input_data(void)
{
    if (g_ft.chunk_pos < g_ft.chunk_len) {
        return g_ft.chunk[g_ft.chunk_pos++];
    }

    return 0x00;
}

void host_files_init(const char *apps_root)
{
    memset(&g_ft, 0, sizeof(g_ft));
    strncpy(g_ft.apps_root, apps_root, sizeof(g_ft.apps_root) - 1);
    g_ft.status = FT_STATUS_IDLE;
}

void host_files_out(uint8_t port, uint8_t data)
{
    switch (port) {
    case FT_COMMAND_PORT:
        host_files_output_command(data);
        break;

    case FT_DATA_PORT:
        host_files_output_data(data);
        break;

    default:
        break;
    }
}

uint8_t host_files_in(uint8_t port)
{
    switch (port) {
    case FT_COMMAND_PORT:
        return host_files_input_status();

    case FT_DATA_PORT:
        return host_files_input_data();

    default:
        return 0x00;
    }
}
