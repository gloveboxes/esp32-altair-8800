#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stdbool.h>

/*
 * Minimal hand-rolled JSON helpers for the MCP server. These operate directly
 * on the raw request text rather than building a parse tree, which is enough
 * for the small, well-formed JSON-RPC messages exchanged with the client.
 *
 * Callers own every returned heap pointer and must free() it.
 */

/* Return a freshly allocated, double-quoted JSON string literal for text,
 * escaping control characters. Returns NULL on allocation failure. */
char *json_escape_dup(const char *text);

/* Return the unescaped string value of the first "key":"..." pair, or NULL if
 * the key is absent or its value is not a string. */
char *json_get_string(const char *json, const char *key);

/* Return the raw token of the top-level "id" field (quoted or bare), or NULL
 * if absent. The quotes, if any, are preserved so the value can be echoed back
 * verbatim in responses. */
char *json_get_id(const char *json);

/* Return the integer value of "key", or fallback if absent. */
int json_get_int(const char *json, const char *key, int fallback);

/* Return the boolean value of "key", or fallback if absent/unparseable. */
bool json_get_bool(const char *json, const char *key, bool fallback);

#endif /* JSON_UTIL_H */
