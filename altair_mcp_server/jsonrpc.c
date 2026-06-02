#define _GNU_SOURCE

#include "jsonrpc.h"
#include "json_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup _strdup
#else
#include <strings.h>
#endif

void send_body(const char *body)
{
    fputs(body, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void send_simple_result(const char *id, const char *result)
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

void send_error(const char *id, int code, const char *message)
{
    char body[1024];

    snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id ? id : "null", code, message);
    send_body(body);
}

void send_tool_text_result(const char *id, const char *text)
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

static char *json_get_protocol_version(const char *json)
{
    char *version = json_get_string(json, "protocolVersion");

    if (version) {
        return version;
    }

    return strdup("2024-11-05");
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
        /* Newline-delimited JSON: the MCP stdio transport's primary framing.
         * A single message may exceed our stack line buffer, so accumulate
         * chunks into a growable buffer until the terminating newline or EOF
         * rather than truncating at sizeof(line). */
        size_t cap = 1024;
        size_t used;
        char *grown;

        body = (char *)malloc(cap);
        if (!body) {
            return false;
        }
        used = strlen(p);
        memcpy(body, p, used + 1);

        while (used == 0 || body[used - 1] != '\n') {
            if (!fgets(line, sizeof(line), stdin)) {
                break;
            }
            len = strlen(line);
            if (used + len + 1 > cap) {
                while (used + len + 1 > cap) {
                    cap *= 2;
                }
                grown = (char *)realloc(body, cap);
                if (!grown) {
                    free(body);
                    return false;
                }
                body = grown;
            }
            memcpy(body + used, line, len + 1);
            used += len;
        }

        while (used > 0 && (body[used - 1] == '\n' || body[used - 1] == '\r')) {
            body[--used] = '\0';
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

static void handle_message(const jsonrpc_server_t *server, const char *json)
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
                 "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
                 version, server->server_name, server->server_version);
        send_simple_result(id, init_result);
        free(version);
    } else if (strcmp(method, "tools/list") == 0) {
        fprintf(stderr, "[MCP] tools/list\n");
        send_simple_result(id, server->tools_list_result);
    } else if (strcmp(method, "tools/call") == 0) {
        fprintf(stderr, "[MCP] tools/call\n");
        server->on_tools_call(id, json);
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

void jsonrpc_run(const jsonrpc_server_t *server)
{
    char *message;

    while (read_message(&message)) {
        handle_message(server, message);
        free(message);
    }
}
