#include "json_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *json_escape_dup(const char *text)
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

char *json_get_string(const char *json, const char *key)
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

char *json_get_id(const char *json)
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

int json_get_int(const char *json, const char *key, int fallback)
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

bool json_get_bool(const char *json, const char *key, bool fallback)
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
