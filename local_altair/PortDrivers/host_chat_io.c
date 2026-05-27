/*
 * Host-side implementation of chat_io.h for local_altair.
 *
 * Uses libcurl for the streaming OpenAI chat completions request and a
 * background pthread so chat_input() can return characters as they arrive.
 *
 * API key is read from the OPENAI_API_KEY environment variable.
 */

#include "chat_io.h"

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

static char chat_api_key[256];

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

static void emit_json_string(uint32_t generation, const char* ptr)
{
    while (*ptr && *ptr != '"')
    {
        uint8_t out = (uint8_t)*ptr++;
        if (out == '\\' && *ptr)
        {
            char esc = *ptr++;
            switch (esc)
            {
                case 'n': out = '\n'; break;
                case 'r': out = '\r'; break;
                case 't': out = '\t'; break;
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                case 'u':
                    if (hex_nibble(ptr[0]) >= 0 && hex_nibble(ptr[1]) >= 0 &&
                        hex_nibble(ptr[2]) >= 0 && hex_nibble(ptr[3]) >= 0)
                    {
                        ptr += 4;
                    }
                    out = '?';
                    break;
                default:
                    out = (uint8_t)esc;
                    break;
            }
        }
        ring_put(generation, out & 0x7f);
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
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", chat_api_key);

    struct curl_slist* headers = NULL;
    if (!append_header(&headers, "Content-Type: application/json") ||
        !append_header(&headers, "Accept: text/event-stream") ||
        !append_header(&headers, auth))
    {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ring_put_str(generation, "OpenAI out of memory\n");
        ring_set_eof(generation);
        free(request_body);
        return NULL;
    }

    sse_parser_t parser;
    parser.len = 0;
    parser.generation = generation;
    parser.overflow = false;

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
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
        snprintf(msg, sizeof(msg), "OpenAI request failed: %s\n", curl_easy_strerror(res));
        ring_put_str(generation, msg);
    }
    else if (http_code != 200)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "OpenAI HTTP %ld\n", http_code);
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
    const char* env = getenv("OPENAI_API_KEY");
    if (env != NULL && env[0] != '\0')
    {
        strncpy(chat_api_key, env, sizeof(chat_api_key) - 1);
        chat_api_key[sizeof(chat_api_key) - 1] = '\0';
        printf("[Chat] OPENAI_API_KEY captured (host build).\n");
    }
    else
    {
        printf("[Chat] OPENAI_API_KEY not set; chat port will return an error.\n");
    }
#ifdef HAVE_LIBCURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#else
    printf("[Chat] Built without libcurl; chat port disabled.\n");
#endif
}

void chat_io_prompt_api_key(void)
{
    /* Host build reads the key from OPENAI_API_KEY in chat_io_init(); the
     * 5-second prompt is Pico-only. */
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

    if (chat_api_key[0] == '\0')
    {
        ring_put_str(generation, "OpenAI API key not configured\n");
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
