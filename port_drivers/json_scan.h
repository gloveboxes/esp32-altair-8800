#ifndef JSON_SCAN_H
#define JSON_SCAN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    const char *start;
    const char *end;
} json_scan_range_t;

const char *json_scan_skip_ws(const char *start, const char *end);
bool json_scan_get_string(json_scan_range_t range, const char *key, char *dst, size_t dst_len);
bool json_scan_get_number(json_scan_range_t range, const char *key, char *dst, size_t dst_len);
bool json_scan_get_int(json_scan_range_t range, const char *key, int *value);
bool json_scan_object(json_scan_range_t range, const char *key, json_scan_range_t *object);
bool json_scan_first_array_object(json_scan_range_t range, const char *key, json_scan_range_t *object);

#endif