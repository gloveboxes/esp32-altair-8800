/*
 * Unit tests for the MCP server's hand-rolled JSON helpers
 * (../altair_mcp_server/json_util.c).
 *
 * These run on the host with no emulator dependencies. Each check prints a
 * PASS/FAIL line; the process exits non-zero if any check fails so ctest can
 * report the result.
 */

#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

static void check_str(const char *what, const char *got, const char *want)
{
    g_checks++;
    if (got == NULL) {
        if (want == NULL) {
            printf("PASS %s (NULL)\n", what);
        } else {
            printf("FAIL %s: got NULL, want \"%s\"\n", what, want);
            g_failures++;
        }
        return;
    }
    if (want == NULL) {
        printf("FAIL %s: got \"%s\", want NULL\n", what, got);
        g_failures++;
    } else if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        g_failures++;
    } else {
        printf("PASS %s\n", what);
    }
}

static void check_int(const char *what, long got, long want)
{
    g_checks++;
    if (got != want) {
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
        g_failures++;
    } else {
        printf("PASS %s\n", what);
    }
}

int main(void)
{
    /* json_escape_dup: plain text is wrapped in quotes. */
    {
        char *s = json_escape_dup("hello");
        check_str("escape plain", s, "\"hello\"");
        free(s);
    }

    /* json_escape_dup: named control escapes and embedded quote/backslash. */
    {
        char *s = json_escape_dup("a\"b\\c\n\t\r\b\f");
        check_str("escape specials", s, "\"a\\\"b\\\\c\\n\\t\\r\\b\\f\"");
        free(s);
    }

    /* json_escape_dup: other control chars become \u00xx. */
    {
        char *s = json_escape_dup("x\x01\x1f""y");
        check_str("escape \\u00xx", s, "\"x\\u0001\\u001fy\"");
        free(s);
    }

    /* json_get_string: simple value. */
    {
        char *s = json_get_string("{\"name\":\"altair\"}", "name");
        check_str("get_string simple", s, "altair");
        free(s);
    }

    /* json_get_string: unescapes \n \t \" \\ in the value. */
    {
        char *s = json_get_string("{\"v\":\"a\\nb\\t\\\"c\\\\d\"}", "v");
        check_str("get_string unescape", s, "a\nb\t\"c\\d");
        free(s);
    }

    /* json_get_string: tolerates whitespace after the colon. */
    {
        char *s = json_get_string("{\"k\" :   \"sp\"}", "k");
        check_str("get_string whitespace", s, "sp");
        free(s);
    }

    /* json_get_string: missing key -> NULL. */
    {
        char *s = json_get_string("{\"name\":\"altair\"}", "missing");
        check_str("get_string missing", s, NULL);
        free(s);
    }

    /* json_get_string: non-string value -> NULL. */
    {
        char *s = json_get_string("{\"n\":42}", "n");
        check_str("get_string non-string", s, NULL);
        free(s);
    }

    /* json_get_id: quoted id keeps its quotes for verbatim echo. */
    {
        char *s = json_get_id("{\"id\":\"abc\",\"x\":1}");
        check_str("get_id quoted", s, "\"abc\"");
        free(s);
    }

    /* json_get_id: bare numeric id. */
    {
        char *s = json_get_id("{\"id\": 17 }");
        check_str("get_id bare", s, "17");
        free(s);
    }

    /* json_get_id: absent -> NULL. */
    {
        char *s = json_get_id("{\"method\":\"ping\"}");
        check_str("get_id absent", s, NULL);
        free(s);
    }

    /* json_get_int: present and fallback. */
    check_int("get_int present", json_get_int("{\"n\":42}", "n", -1), 42);
    check_int("get_int negative", json_get_int("{\"n\": -5}", "n", 0), -5);
    check_int("get_int fallback", json_get_int("{\"n\":42}", "x", 99), 99);

    /* json_get_bool: true, false, and fallback. */
    check_int("get_bool true", json_get_bool("{\"b\":true}", "b", false), 1);
    check_int("get_bool false", json_get_bool("{\"b\":false}", "b", true), 0);
    check_int("get_bool fallback", json_get_bool("{\"b\":true}", "x", true), 1);
    check_int("get_bool unparseable", json_get_bool("{\"b\":maybe}", "b", true), 1);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
