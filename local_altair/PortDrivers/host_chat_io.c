/*
 * Host-side implementation of chat_io.h for local_altair.
 *
 * Uses libcurl for the streaming chat completions request and a
 * background pthread so chat_input() can return characters as they arrive.
 *
 * Configuration is read from the env store (altair_env.txt):
 *   CHAT_OPENAI_KEY  - Bearer token for OpenAI (and any compatible endpoint
 *                      that requires auth).
 *   CHAT_PROVIDER    - "openai" (default) or "compatible".
 *   CHAT_ENDPOINT    - Full URL used when CHAT_PROVIDER=compatible,
 *                      e.g. http://192.168.1.20:11434/v1/chat/completions
 */

#include "chat_io.h"
#include "environment_io.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#define CHAT_REQUEST_MAX 8192
#define CHAT_RING_SIZE 16384
#define CHAT_SSE_LINE_MAX 4096

#define CHAT_DEFAULT_OPENAI_URL "https://api.openai.com/v1/chat/completions"

static char chat_api_key[256];
static char chat_provider[32];
static char chat_endpoint[512];
static char chat_url[512];

typedef struct
{
    pthread_mutex_t lock;
    uint8_t buf[CHAT_RING_SIZE];
    size_t head;
    size_t tail;
    uint32_t generation;
    bool overflow;
    bool eof;
} ring_t;

typedef struct
{
    char* request_body;
    uint32_t generation;
} worker_arg_t;

static ring_t ring;

static struct
{
    char request[CHAT_REQUEST_MAX];
    size_t request_len;
    bool request_overflow;
} port_state;

static void ring_init(void)
{
    pthread_mutex_init(&ring.lock, NULL);
    ring.head = ring.tail = 0;
    ring.generation = 0;
    ring.overflow = false;
    ring.eof = false;
}

static void ring_put_locked(uint8_t b, bool force)
{
    size_t next = (ring.head + 1) % CHAT_RING_SIZE;
    if (next == ring.tail)
    {
        ring.overflow = true;
        if (!force)
        {
            return;
        }
        ring.tail = (ring.tail + 1) % CHAT_RING_SIZE;
    }

    ring.buf[ring.head] = b;
    ring.head = next;
}

static uint32_t ring_reset(void)
{
    pthread_mutex_lock(&ring.lock);
    ring.head = ring.tail = 0;
    ring.generation++;
    ring.overflow = false;
    ring.eof = false;
    uint32_t generation = ring.generation;
    pthread_mutex_unlock(&ring.lock);
    return generation;
}

static void ring_put(uint32_t generation, uint8_t b)
{
    pthread_mutex_lock(&ring.lock);
    if (generation == ring.generation)
    {
        ring_put_locked(b, false);
    }
    pthread_mutex_unlock(&ring.lock);
}

static void ring_put_str(uint32_t generation, const char* s)
{
    while (*s)
    {
        ring_put(generation, (uint8_t)(*s++ & 0x7f));
    }
}

static void ring_set_eof(uint32_t generation)
{
    pthread_mutex_lock(&ring.lock);
    if (generation == ring.generation)
    {
        if (ring.overflow)
        {
            static const char truncated[] = "\nOpenAI response truncated\n";
            for (size_t i = 0; truncated[i] != '\0'; i++)
            {
                ring_put_locked((uint8_t)(truncated[i] & 0x7f), true);
            }
            ring.overflow = false;
        }
        ring.eof = true;
    }
    pthread_mutex_unlock(&ring.lock);
}

static int ring_status(void)
{
    pthread_mutex_lock(&ring.lock);
    int s;
    if (ring.head != ring.tail)
    {
        s = CHAT_STATUS_DATA_READY;
    }
    else if (ring.eof)
    {
        s = CHAT_STATUS_EOF;
    }
    else
    {
        s = CHAT_STATUS_WAITING;
    }
    pthread_mutex_unlock(&ring.lock);
    return s;
}

static int ring_get(void)
{
    pthread_mutex_lock(&ring.lock);
    int v = -1;
    if (ring.head != ring.tail)
    {
        v = ring.buf[ring.tail];
        ring.tail = (ring.tail + 1) % CHAT_RING_SIZE;
    }
    pthread_mutex_unlock(&ring.lock);
    return v;
}

#ifdef HAVE_LIBCURL

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Map a Unicode codepoint to a printable 7-bit ASCII replacement so the
 * Altair VT100 (which only handles 7-bit ASCII) does not see UTF-8
 * continuation bytes leak through as stray characters (e.g. the curly
 * apostrophe U+2019 0xE2 0x80 0x99 surfacing as 'b'). Returns 0 if no
 * sensible replacement exists, in which case the caller should drop the
 * codepoint.
 *
 * Mirrors chat_cp_to_ascii() in port_drivers/chat_io.c. */
static char cp_to_ascii(uint32_t cp)
{
    if (cp < 0x80) return (char)cp;
    switch (cp)
    {
        case 0x00A0: return ' ';            /* NBSP */
        case 0x00A9: return 'C';            /* (C) */
        case 0x00AE: return 'R';            /* (R) */
        case 0x00B0: return ' ';            /* degree */
        case 0x00B7: return '.';            /* middle dot */
        case 0x2010: case 0x2011:
        case 0x2012: case 0x2013:
        case 0x2014: case 0x2015: return '-';   /* hyphen / en / em dash */
        case 0x2018: case 0x2019:
        case 0x201A: case 0x2032: return '\'';  /* single quotes / prime */
        case 0x201C: case 0x201D:
        case 0x201E: case 0x2033: return '"';   /* double quotes */
        case 0x2022: case 0x2023:
        case 0x25E6: case 0x2043: return '*';   /* bullets */
        case 0x2026: return '.';                /* ellipsis -> '.' */
        case 0x2192: return '>';                /* right arrow */
        case 0x2190: return '<';                /* left arrow */
        case 0x00D7: return 'x';                /* multiply */
        default: return 0;
    }
}

static void emit_codepoint(uint32_t generation, uint32_t cp)
{
    if (cp < 0x80)
    {
        ring_put(generation, (uint8_t)cp);
        return;
    }
    if (cp == 0x2026)
    {
        /* ellipsis: expand to "..." */
        ring_put(generation, '.');
        ring_put(generation, '.');
        ring_put(generation, '.');
        return;
    }
    char repl = cp_to_ascii(cp);
    if (repl)
    {
        ring_put(generation, (uint8_t)repl);
    }
    /* else: silently drop unknown codepoint */
}

static void emit_json_string(uint32_t generation, const char* ptr)
{
    while (*ptr && *ptr != '"')
    {
        uint8_t b = (uint8_t)*ptr++;
        if (b == '\\' && *ptr)
        {
            char esc = *ptr++;
            switch (esc)
            {
                case 'n': ring_put(generation, '\n'); break;
                case 'r': ring_put(generation, '\r'); break;
                case 't': ring_put(generation, '\t'); break;
                case '"': ring_put(generation, '"'); break;
                case '\\': ring_put(generation, '\\'); break;
                case '/': ring_put(generation, '/'); break;
                case 'b': ring_put(generation, '\b'); break;
                case 'f': ring_put(generation, '\f'); break;
                case 'u':
                {
                    int n0 = hex_nibble(ptr[0]);
                    int n1 = (n0 >= 0) ? hex_nibble(ptr[1]) : -1;
                    int n2 = (n1 >= 0) ? hex_nibble(ptr[2]) : -1;
                    int n3 = (n2 >= 0) ? hex_nibble(ptr[3]) : -1;
                    if (n3 >= 0)
                    {
                        uint32_t cp = (uint32_t)((n0 << 12) | (n1 << 8) | (n2 << 4) | n3);
                        ptr += 4;
                        /* surrogate pair */
                        if (cp >= 0xD800 && cp <= 0xDBFF && ptr[0] == '\\' && ptr[1] == 'u')
                        {
                            int m0 = hex_nibble(ptr[2]);
                            int m1 = (m0 >= 0) ? hex_nibble(ptr[3]) : -1;
                            int m2 = (m1 >= 0) ? hex_nibble(ptr[4]) : -1;
                            int m3 = (m2 >= 0) ? hex_nibble(ptr[5]) : -1;
                            if (m3 >= 0)
                            {
                                uint32_t lo = (uint32_t)((m0 << 12) | (m1 << 8) | (m2 << 4) | m3);
                                if (lo >= 0xDC00 && lo <= 0xDFFF)
                                {
                                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                                    ptr += 6;
                                }
                            }
                        }
                        emit_codepoint(generation, cp);
                    }
                    break;
                }
                default:
                    ring_put(generation, (uint8_t)esc & 0x7f);
                    break;
            }
            continue;
        }

        if (b < 0x80)
        {
            ring_put(generation, b);
            continue;
        }

        /* UTF-8 lead byte: decode codepoint, then map to ASCII. */
        uint32_t cp = 0;
        int extra = 0;
        if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
        else { continue; /* stray continuation byte */ }

        bool ok = true;
        for (int i = 0; i < extra; ++i)
        {
            uint8_t cb = (uint8_t)*ptr;
            if (!cb || (cb & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cb & 0x3F);
            ptr++;
        }
        if (ok)
        {
            emit_codepoint(generation, cp);
        }
    }
}

    static void process_sse_line(uint32_t generation, const char* line)
{
    if (strncmp(line, "data: ", 6) != 0)
    {
        return;
    }
    const char* payload = line + 6;
    if (strcmp(payload, "[DONE]") == 0)
    {
        return;
    }
    const char* marker = "\"content\":\"";
    size_t marker_len = strlen(marker);
    const char* p = payload;
    while ((p = strstr(p, marker)) != NULL)
    {
        p += marker_len;
        emit_json_string(generation, p);
    }
}

typedef struct
{
    char line[CHAT_SSE_LINE_MAX];
    size_t len;
    uint32_t generation;
    bool overflow;
} sse_parser_t;

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    sse_parser_t* parser = (sse_parser_t*)userdata;
    size_t total = size * nmemb;
    for (size_t i = 0; i < total; i++)
    {
        char ch = ptr[i];
        if (ch == '\r')
        {
            continue;
        }
        if (ch == '\n')
        {
            if (!parser->overflow)
            {
                parser->line[parser->len] = '\0';
                process_sse_line(parser->generation, parser->line);
            }
            parser->len = 0;
            parser->overflow = false;
        }
        else if (parser->len + 1 < sizeof(parser->line))
        {
            parser->line[parser->len++] = ch;
        }
        else
        {
            parser->overflow = true;
        }
    }
    return total;
}

static bool append_header(struct curl_slist** headers, const char* value)
{
    struct curl_slist* updated = curl_slist_append(*headers, value);
    if (updated == NULL)
    {
        return false;
    }
    *headers = updated;
    return true;
}

static void* chat_worker(void* arg)
{
    worker_arg_t* worker_arg = (worker_arg_t*)arg;
    char* request_body = worker_arg->request_body;
    uint32_t generation = worker_arg->generation;
    free(worker_arg);

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        ring_put_str(generation, "OpenAI curl init failed\n");
        ring_set_eof(generation);
        free(request_body);
        return NULL;
    }

    char auth[300];
    bool have_key = (chat_api_key[0] != '\0');
    if (have_key)
    {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", chat_api_key);
    }

    struct curl_slist* headers = NULL;
    bool headers_ok = append_header(&headers, "Content-Type: application/json") &&
                      append_header(&headers, "Accept: text/event-stream");
    if (headers_ok && have_key)
    {
        headers_ok = append_header(&headers, auth);
    }
    if (!headers_ok)
    {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ring_put_str(generation, "Chat out of memory\n");
        ring_set_eof(generation);
        free(request_body);
        return NULL;
    }

    sse_parser_t parser;
    parser.len = 0;
    parser.generation = generation;
    parser.overflow = false;

    curl_easy_setopt(curl, CURLOPT_URL, chat_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "Chat request failed: %s\n", curl_easy_strerror(res));
        ring_put_str(generation, msg);
    }
    else if (http_code != 200)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Chat HTTP %ld\n", http_code);
        ring_put_str(generation, msg);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    ring_set_eof(generation);
    free(request_body);
    return NULL;
}

#endif /* HAVE_LIBCURL */

void chat_io_init(void)
{
    ring_init();
    memset(&port_state, 0, sizeof(port_state));

    /* Read CHAT_OPENAI_KEY (matches ESP32 firmware). */
    if (!(environment_io_get("CHAT_OPENAI_KEY", chat_api_key, sizeof(chat_api_key)) &&
          chat_api_key[0] != '\0'))
    {
        chat_api_key[0] = '\0';
    }

    /* Read CHAT_PROVIDER (default: openai). */
    if (!(environment_io_get("CHAT_PROVIDER", chat_provider, sizeof(chat_provider)) &&
          chat_provider[0] != '\0'))
    {
        strncpy(chat_provider, "openai", sizeof(chat_provider) - 1);
        chat_provider[sizeof(chat_provider) - 1] = '\0';
    }

    /* Read CHAT_ENDPOINT (only used when provider=compatible). */
    if (!(environment_io_get("CHAT_ENDPOINT", chat_endpoint, sizeof(chat_endpoint)) &&
          chat_endpoint[0] != '\0'))
    {
        chat_endpoint[0] = '\0';
    }

    /* Resolve the effective URL. */
    if (strcmp(chat_provider, "compatible") == 0 && chat_endpoint[0] != '\0')
    {
        strncpy(chat_url, chat_endpoint, sizeof(chat_url) - 1);
        chat_url[sizeof(chat_url) - 1] = '\0';
    }
    else
    {
        strncpy(chat_url, CHAT_DEFAULT_OPENAI_URL, sizeof(chat_url) - 1);
        chat_url[sizeof(chat_url) - 1] = '\0';
    }

    printf("[Chat] provider=%s url=%s key=%s\n",
           chat_provider, chat_url,
           chat_api_key[0] ? "set" : "(none)");

#ifdef HAVE_LIBCURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#else
    printf("[Chat] Built without libcurl; chat port disabled.\n");
#endif
}

void chat_io_prompt_api_key(void)
{
    /* Host build reads config from env at chat_io_init(); the 5-second
     * prompt is Pico-only. */
}

void chat_client_poll(void)
{
    /* Worker thread does the work on the host. */
}

static void reset_request(void)
{
    port_state.request_len = 0;
    port_state.request[0] = '\0';
    port_state.request_overflow = false;
}

static void request_add_char(uint8_t data)
{
    if (data == 0)
    {
        if (port_state.request_len < sizeof(port_state.request))
        {
            port_state.request[port_state.request_len] = '\0';
        }
        return;
    }
    if (port_state.request_len + 1 < sizeof(port_state.request))
    {
        port_state.request[port_state.request_len++] = (char)data;
        port_state.request[port_state.request_len] = '\0';
    }
    else
    {
        port_state.request_overflow = true;
    }
}

static void trigger_request(void)
{
    uint32_t generation = ring_reset();

    if (port_state.request_overflow || port_state.request_len == 0)
    {
        ring_put_str(generation, "OpenAI request buffer error\n");
        ring_set_eof(generation);
        return;
    }

    if (chat_api_key[0] == '\0' && strcmp(chat_provider, "compatible") != 0)
    {
        ring_put_str(generation, "Chat: CHAT_OPENAI_KEY not configured\n");
        ring_set_eof(generation);
        return;
    }

#ifdef HAVE_LIBCURL
    char* body = (char*)malloc(port_state.request_len + 1);
    if (!body)
    {
        ring_put_str(generation, "OpenAI out of memory\n");
        ring_set_eof(generation);
        return;
    }
    memcpy(body, port_state.request, port_state.request_len + 1);

    worker_arg_t* worker_arg = (worker_arg_t*)malloc(sizeof(worker_arg_t));
    if (!worker_arg)
    {
        free(body);
        ring_put_str(generation, "OpenAI out of memory\n");
        ring_set_eof(generation);
        return;
    }
    worker_arg->request_body = body;
    worker_arg->generation = generation;

    pthread_t worker;
    if (pthread_create(&worker, NULL, chat_worker, worker_arg) != 0)
    {
        free(worker_arg);
        free(body);
        ring_put_str(generation, "OpenAI thread create failed\n");
        ring_set_eof(generation);
        return;
    }
    pthread_detach(worker);
#else
    ring_put_str(generation, "OpenAI chat unavailable (no libcurl)\n");
    ring_set_eof(generation);
#endif
}

size_t chat_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;
    if (port == CHAT_PORT_TRIGGER)
    {
        reset_request();
    }
    else if (port == CHAT_PORT_REQUEST)
    {
        request_add_char(data);
    }
    else if (port == CHAT_PORT_RESET_RESPONSE)
    {
        ring_reset();
    }
    return 0;
}

uint8_t chat_input(uint8_t port)
{
    if (port == CHAT_PORT_TRIGGER)
    {
        trigger_request();
        return 0;
    }
    if (port == CHAT_PORT_STATUS)
    {
        return (uint8_t)ring_status();
    }
    if (port == CHAT_PORT_DATA)
    {
        int v = ring_get();
        return v < 0 ? 0 : (uint8_t)v;
    }
    return 0;
}
