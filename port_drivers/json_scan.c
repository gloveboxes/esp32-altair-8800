#include "json_scan.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char *json_scan_skip_ws(const char *start, const char *end)
{
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n'))
    {
        start++;
    }
    return start;
}

static const char *json_scan_find_key(json_scan_range_t range, const char *key)
{
    char pattern[64];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern))
    {
        return NULL;
    }

    for (const char *cursor = range.start; cursor + pattern_len <= range.end && *cursor; cursor++)
    {
        if (memcmp(cursor, pattern, (size_t)pattern_len) != 0)
        {
            continue;
        }
        cursor += pattern_len;
        cursor = json_scan_skip_ws(cursor, range.end);
        if (cursor < range.end && *cursor == ':')
        {
            return json_scan_skip_ws(cursor + 1, range.end);
        }
    }
    return NULL;
}

static const char *json_scan_match_end(const char *open, const char *end)
{
    char open_ch = *open;
    char close_ch = (open_ch == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char *cursor = open; cursor < end && *cursor; cursor++)
    {
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (*cursor == '\\')
            {
                escaped = true;
            }
            else if (*cursor == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (*cursor == '"')
        {
            in_string = true;
        }
        else if (*cursor == open_ch)
        {
            depth++;
        }
        else if (*cursor == close_ch)
        {
            depth--;
            if (depth == 0)
            {
                return cursor + 1;
            }
        }
    }
    return NULL;
}

bool json_scan_get_string(json_scan_range_t range, const char *key, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0)
    {
        return false;
    }
    dst[0] = '\0';

    const char *cursor = json_scan_find_key(range, key);
    if (!cursor || cursor >= range.end || *cursor != '"')
    {
        return false;
    }
    cursor++;

    size_t out_len = 0;
    while (cursor < range.end && *cursor && *cursor != '"')
    {
        char ch = *cursor++;
        if (ch == '\\' && cursor < range.end && *cursor)
        {
            ch = *cursor++;
            if (ch == 'n' || ch == 'r' || ch == 't' || ch == 'b' || ch == 'f')
            {
                ch = ' ';
            }
            else if (ch == 'u')
            {
                ch = '?';
                for (int digit = 0; digit < 4 && cursor < range.end && *cursor; digit++)
                {
                    cursor++;
                }
            }
        }
        if (out_len + 1 < dst_len)
        {
            dst[out_len++] = ch;
        }
    }
    dst[out_len] = '\0';
    return cursor < range.end && *cursor == '"';
}

bool json_scan_get_number(json_scan_range_t range, const char *key, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0)
    {
        return false;
    }
    dst[0] = '\0';

    const char *cursor = json_scan_find_key(range, key);
    if (!cursor || cursor >= range.end)
    {
        return false;
    }

    char *num_end = NULL;
    double value = strtod(cursor, &num_end);
    if (num_end == cursor || num_end > range.end)
    {
        return false;
    }

    int rounded = (int)(value >= 0 ? value + 0.5 : value - 0.5);
    snprintf(dst, dst_len, "%d", rounded);
    return true;
}

bool json_scan_get_int(json_scan_range_t range, const char *key, int *value)
{
    const char *cursor = json_scan_find_key(range, key);
    if (!cursor || cursor >= range.end || !value)
    {
        return false;
    }

    if (*cursor == '"')
    {
        char tmp[12];
        if (!json_scan_get_string(range, key, tmp, sizeof(tmp)))
        {
            return false;
        }
        *value = atoi(tmp);
        return true;
    }

    char *num_end = NULL;
    long parsed = strtol(cursor, &num_end, 10);
    if (num_end == cursor || num_end > range.end)
    {
        return false;
    }
    *value = (int)parsed;
    return true;
}

bool json_scan_object(json_scan_range_t range, const char *key, json_scan_range_t *object)
{
    json_scan_range_t search = range;
    while (search.start < search.end)
    {
        const char *cursor = json_scan_find_key(search, key);
        if (!cursor)
        {
            return false;
        }
        cursor = json_scan_skip_ws(cursor, range.end);
        if (cursor < range.end && *cursor == '{')
        {
            const char *close = json_scan_match_end(cursor, range.end);
            if (close)
            {
                object->start = cursor;
                object->end = close;
                return true;
            }
            return false;
        }
        search.start = cursor + 1;
    }
    return false;
}

bool json_scan_first_array_object(json_scan_range_t range, const char *key, json_scan_range_t *object)
{
    json_scan_range_t search = range;
    while (search.start < search.end)
    {
        const char *cursor = json_scan_find_key(search, key);
        if (!cursor)
        {
            return false;
        }
        cursor = json_scan_skip_ws(cursor, range.end);
        if (cursor < range.end && *cursor == '[')
        {
            const char *array_end = json_scan_match_end(cursor, range.end);
            if (!array_end)
            {
                return false;
            }
            cursor = json_scan_skip_ws(cursor + 1, array_end);
            while (cursor < array_end && *cursor && *cursor != '{')
            {
                cursor++;
            }
            if (cursor >= array_end || *cursor != '{')
            {
                return false;
            }
            const char *close = json_scan_match_end(cursor, array_end);
            if (!close)
            {
                return false;
            }
            object->start = cursor;
            object->end = close;
            return true;
        }
        search.start = cursor + 1;
    }
    return false;
}