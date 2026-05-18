/**
 * @file chat_io.c
 * @brief OpenAI Chat Completions I/O port driver for ESP32.
 */

#include "port_drivers/chat_io.h"

#include "config.h"
#include "wifi.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "mbedtls/error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CHAT_PATH "/v1/chat/completions"
#define CHAT_DEFAULT_OPENAI_ENDPOINT "https://api.openai.com/v1/chat/completions"
#define CHAT_PROVIDER_OPENAI "openai"
#define CHAT_PROVIDER_COMPATIBLE "compatible"
#define CHAT_DEFAULT_MODEL "gemma3:1b"
#define CHAT_DEFAULT_MAX_TOKENS "1024"
#define CHAT_DEFAULT_TEMPERATURE "0.7"

#define CHAT_API_KEY_MAX 192
#define CHAT_ENDPOINT_MAX CONFIG_CHAT_ENDPOINT_MAX_LEN
#define CHAT_MODEL_MAX CONFIG_CHAT_MODEL_MAX_LEN
#define CHAT_MAX_TOKENS_MAX CONFIG_CHAT_MAX_TOKENS_MAX_LEN
#define CHAT_TEMPERATURE_MAX CONFIG_CHAT_TEMPERATURE_MAX_LEN
#define CHAT_HOST_MAX 96
#define CHAT_PATH_MAX 96
#define CHAT_REQUEST_MAX 8192
#define CHAT_RX_BUFFER_SIZE 1024
#define CHAT_SSE_LINE_MAX 1024
/* Response queue depth: the Altair app polls one byte at a time via
 * CHAT_PORT_DATA, so a deep queue is unnecessary. The producer task
 * blocks with a short timeout when full, providing natural
 * back-pressure rather than dropping bytes. Items live in PSRAM. */
#define CHAT_RESPONSE_QUEUE_DEPTH 128
#define CHAT_QUEUE_SEND_TIMEOUT_MS 100
#define CHAT_HEADER_MAX 768
#define CHAT_AUTH_HEADER_MAX 224
#define CHAT_CONNECT_TIMEOUT_MS 15000
#define CHAT_STREAM_TIMEOUT_MS 120000
/* Reduced from 8192: large request/parser/rx buffers now live on the
 * heap (PSRAM-preferred) rather than the task stack. */
#define CHAT_TASK_STACK_SIZE 4096
#define CHAT_TASK_PRIORITY 5
#define CHAT_TASK_CORE 0

typedef struct
{
    bool https;
    char host[CHAT_HOST_MAX];
    char host_header[CHAT_HOST_MAX + 8];
    char path[CHAT_PATH_MAX];
    int port;
} chat_endpoint_t;

typedef struct
{
    uint32_t generation;
    size_t len;
    char json[CHAT_REQUEST_MAX];
} chat_request_t;

typedef enum
{
    CHAT_RESP_CHAR = 0,
    CHAT_RESP_EOF
} chat_response_type_t;

typedef struct
{
    uint32_t generation;
    chat_response_type_t type;
    uint8_t data;
} chat_response_t;

typedef enum
{
    CHUNK_SIZE = 0,
    CHUNK_SIZE_LF,
    CHUNK_DATA,
    CHUNK_DATA_CR,
    CHUNK_DATA_LF
} chunk_state_t;

typedef struct
{
    uint32_t generation;
    bool headers_done;
    uint32_t header_match;
    bool status_checked;
    int status_code;
    char status_line[40];
    size_t status_line_len;
    chunk_state_t chunk_state;
    size_t chunk_size;
    size_t chunk_read;
    bool chunk_extension;
    char sse_line[CHAT_SSE_LINE_MAX];
    size_t sse_len;
    bool done;
    bool response_truncated;
} chat_parse_t;

/* Per-request workspace allocated from PSRAM to keep the chat task
 * stack small. Sizes here dominate the previous 8KB stack budget. */
typedef struct
{
    chat_parse_t parser;
    uint8_t rx_buffer[CHAT_RX_BUFFER_SIZE];
    char header[CHAT_HEADER_MAX];
    char auth_header[CHAT_AUTH_HEADER_MAX];
} chat_workspace_t;

static const char *TAG = "CHAT_IO";

static const unsigned char CHAT_GTS_ROOT_R4_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

static QueueHandle_t s_chat_request_queue = NULL;
static QueueHandle_t s_chat_response_queue = NULL;
static char s_chat_api_key[CHAT_API_KEY_MAX];
static char s_chat_provider[CONFIG_CHAT_PROVIDER_MAX_LEN + 1];
static char s_chat_endpoint[CHAT_ENDPOINT_MAX + 1];
static char s_chat_model[CHAT_MODEL_MAX + 1];
static char s_chat_max_tokens[CHAT_MAX_TOKENS_MAX + 1];
static char s_chat_temperature[CHAT_TEMPERATURE_MAX + 1];
static bool s_network_available = false;
static bool s_initialized = false;

static struct
{
    /* Heap-allocated (PSRAM-preferred) so the 8KB request buffer does
     * not sit in internal DRAM via BSS. Allocated in chat_io_init. */
    char *request;
    size_t request_capacity;
    size_t request_len;
    uint32_t generation;
    uint32_t next_generation;
    bool request_overflow;
    bool eof_seen;
    uint8_t pending_char;
    bool has_pending_char;
} port_state;

static void chat_client_task(void *arg);
static void chat_reset_request(void);
static void chat_reset_response(void);
static void chat_request_add_char(uint8_t data);
static void chat_trigger_request(void);
static bool chat_load_next_char(void);
static void chat_queue_char(chat_parse_t *parser, uint32_t generation, uint8_t data, bool force);
static void chat_queue_text(uint32_t generation, const char *text);
static void chat_queue_eof(chat_parse_t *parser, uint32_t generation);
static void chat_process_request(chat_request_t *request);
static void chat_load_settings(void);
static bool chat_valid_uint_text(const char *text);
static bool chat_valid_temperature_text(const char *text);
static bool chat_parse_endpoint(const char *endpoint, chat_endpoint_t *parsed);
static bool chat_starts_with_ci(const char *text, const char *prefix);
static bool chat_str_eq_ci(const char *left, const char *right);
static bool chat_send_all(esp_tls_t *tls, const uint8_t *data, size_t len);
static bool chat_read_response(esp_tls_t *tls, chat_workspace_t *ws);
static void chat_log_tls_error(esp_tls_t *tls, int ret);
static void chat_process_rx_byte(chat_parse_t *parser, uint8_t ch);
static void chat_process_body_byte(chat_parse_t *parser, uint8_t ch);
static void chat_process_sse_byte(chat_parse_t *parser, uint8_t ch);
static void chat_extract_content(chat_parse_t *parser, const char *json);
static int chat_hex_nibble(char ch);

static uint32_t chat_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void chat_serial_drain_line(uint32_t idle_timeout_ms)
{
    int64_t idle_start = esp_timer_get_time();
    while ((esp_timer_get_time() - idle_start) < ((int64_t)idle_timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
        if (len <= 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            return;
        }
        idle_start = esp_timer_get_time();
    }
}

static void chat_serial_write_text(const char *text)
{
    if (!text)
    {
        return;
    }

    size_t len = strlen(text);
    while (len > 0)
    {
        int written = usb_serial_jtag_write_bytes((const uint8_t *)text, len, pdMS_TO_TICKS(100));
        if (written <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        text += written;
        len -= (size_t)written;
    }
}

static void chat_serial_write_char(uint8_t c)
{
    usb_serial_jtag_write_bytes(&c, 1, pdMS_TO_TICKS(100));
}

static bool chat_provider_is_compatible(void)
{
    return strcmp(s_chat_provider, CHAT_PROVIDER_COMPATIBLE) == 0;
}

static void chat_load_settings(void)
{
    s_chat_provider[0] = '\0';
    s_chat_endpoint[0] = '\0';
    s_chat_api_key[0] = '\0';
    s_chat_model[0] = '\0';
    s_chat_max_tokens[0] = '\0';
    s_chat_temperature[0] = '\0';

    config_load_chat_settings(s_chat_provider, sizeof(s_chat_provider),
                              s_chat_endpoint, sizeof(s_chat_endpoint),
                              s_chat_api_key, sizeof(s_chat_api_key));
    config_load_chat_options(s_chat_model, sizeof(s_chat_model),
                             s_chat_max_tokens, sizeof(s_chat_max_tokens),
                             s_chat_temperature, sizeof(s_chat_temperature));

    if (strcmp(s_chat_provider, CHAT_PROVIDER_COMPATIBLE) != 0)
    {
        strncpy(s_chat_provider, CHAT_PROVIDER_OPENAI, sizeof(s_chat_provider) - 1);
        s_chat_provider[sizeof(s_chat_provider) - 1] = '\0';
    }
    if (s_chat_model[0] == '\0')
    {
        strncpy(s_chat_model, CHAT_DEFAULT_MODEL, sizeof(s_chat_model) - 1);
        s_chat_model[sizeof(s_chat_model) - 1] = '\0';
    }
    if (s_chat_max_tokens[0] == '\0')
    {
        strncpy(s_chat_max_tokens, CHAT_DEFAULT_MAX_TOKENS, sizeof(s_chat_max_tokens) - 1);
        s_chat_max_tokens[sizeof(s_chat_max_tokens) - 1] = '\0';
    }
    if (s_chat_temperature[0] == '\0')
    {
        strncpy(s_chat_temperature, CHAT_DEFAULT_TEMPERATURE, sizeof(s_chat_temperature) - 1);
        s_chat_temperature[sizeof(s_chat_temperature) - 1] = '\0';
    }
}

static int chat_serial_read_command_ms(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t c = 0;
        int len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            return 0;
        }
        if (c == ' ' || c == '\t')
        {
            continue;
        }
        chat_serial_drain_line(50);
        if (c >= 'a' && c <= 'z')
        {
            c = (uint8_t)(c - 'a' + 'A');
        }
        return c;
    }
    return -1;
}

static void chat_print_menu(void)
{
    printf("\nChat provider manager\n");
    printf("  1 - configure OpenAI\n");
    printf("  2 - configure OpenAI Compatible endpoint\n");
    printf("  3 - configure model\n");
    printf("  4 - configure max tokens\n");
    printf("  5 - configure temperature\n");
    printf("  S - show current settings\n");
    printf("  Q - return to main config menu\n");
}

static bool chat_serial_read_line(const char *prompt, char *buffer, size_t buffer_len, bool mask_input)
{
    size_t length = 0;

    if (!buffer || buffer_len == 0)
    {
        return false;
    }

    buffer[0] = '\0';
    chat_serial_write_text(prompt);

    for (;;)
    {
        uint8_t c = 0;
        int read_len = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (read_len <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            chat_serial_write_text("\r\n");
            buffer[length] = '\0';
            return true;
        }

        if (c == 0x08 || c == 0x7F)
        {
            if (length > 0)
            {
                length--;
                buffer[length] = '\0';
                chat_serial_write_text("\b \b");
            }
            continue;
        }

        if (c < 32 || c > 126 || length + 1 >= buffer_len)
        {
            continue;
        }

        buffer[length++] = (char)c;
        buffer[length] = '\0';
        chat_serial_write_char(mask_input ? '*' : c);
    }
}

static void chat_print_settings(void)
{
    printf("\nChat provider settings\n");
    printf("  Provider: %s\n", chat_provider_is_compatible() ? "OpenAI Compatible" : "OpenAI");
    printf("  Endpoint: %s\n", chat_provider_is_compatible() ?
           (s_chat_endpoint[0] ? s_chat_endpoint : "(not set)") : CHAT_DEFAULT_OPENAI_ENDPOINT);
    printf("  Model: %s\n", s_chat_model);
    printf("  Max tokens: %s\n", s_chat_max_tokens);
    printf("  Temperature: %s\n", s_chat_temperature);
    printf("  API key: %s\n", s_chat_api_key[0] ? "set" : "not set");
}

static bool chat_valid_uint_text(const char *text)
{
    if (!text || text[0] == '\0')
    {
        return false;
    }

    for (const char *p = text; *p; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return false;
        }
    }

    return atoi(text) > 0;
}

static bool chat_valid_temperature_text(const char *text)
{
    char *end = NULL;

    if (!text || text[0] == '\0')
    {
        return false;
    }

    double value = strtod(text, &end);
    if (end == text || *end != '\0')
    {
        return false;
    }

    return value >= 0.0 && value <= 2.0;
}

static void chat_configure_openai(void)
{
    char key[CHAT_API_KEY_MAX];

    printf("\nConfigure OpenAI\n");
    printf("Endpoint: %s\n", CHAT_DEFAULT_OPENAI_ENDPOINT);
    if (s_chat_api_key[0])
    {
        printf("Current API key is set. Press Enter to keep it, '-' to clear it, or paste a replacement.\n");
    }
    else
    {
        printf("Paste an OpenAI API key, or press Enter to leave it unset.\n");
    }

    if (!chat_serial_read_line("openai key> ", key, sizeof(key), true))
    {
        return;
    }

    if (key[0] == '\0' && s_chat_api_key[0])
    {
        strncpy(key, s_chat_api_key, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    }
    else if (strcmp(key, "-") == 0)
    {
        key[0] = '\0';
    }

    if (config_save_chat_settings(CHAT_PROVIDER_OPENAI, "", key))
    {
        chat_load_settings();
    }
}

static void chat_configure_compatible(void)
{
    char endpoint[CHAT_ENDPOINT_MAX + 1];
    char key[CHAT_API_KEY_MAX];
    chat_endpoint_t parsed;
    bool editing_compatible = chat_provider_is_compatible();

    printf("\nConfigure OpenAI Compatible Endpoint\n");
    printf("Examples: http://192.168.1.20:11434 or http://192.168.1.20:11434/v1/chat/completions\n");
    if (s_chat_endpoint[0])
    {
        printf("Current endpoint: %s\n", s_chat_endpoint);
        printf("Press Enter to keep it, or type a replacement.\n");
    }
    else
    {
        printf("Type the endpoint URL. Missing path defaults to /v1/chat/completions.\n");
    }
    if (!chat_serial_read_line("endpoint> ", endpoint, sizeof(endpoint), false))
    {
        return;
    }
    if (endpoint[0] == '\0' && s_chat_endpoint[0])
    {
        strncpy(endpoint, s_chat_endpoint, sizeof(endpoint) - 1);
        endpoint[sizeof(endpoint) - 1] = '\0';
    }
    if (!chat_parse_endpoint(endpoint, &parsed))
    {
        printf("Endpoint must start with http:// or https:// and include a host.\n");
        return;
    }
    printf("Endpoint: %s\n", endpoint);

    if (editing_compatible && s_chat_api_key[0])
    {
        printf("API key is optional. Press Enter to keep it, '-' to clear, or paste a replacement.\n");
    }
    else
    {
        printf("API key is optional. Press Enter for no key, or paste a value.\n");
    }
    if (!chat_serial_read_line("api key> ", key, sizeof(key), true))
    {
        return;
    }
    if (key[0] == '\0' && editing_compatible && s_chat_api_key[0])
    {
        strncpy(key, s_chat_api_key, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    }
    else if (strcmp(key, "-") == 0)
    {
        key[0] = '\0';
    }

    if (config_save_chat_settings(CHAT_PROVIDER_COMPATIBLE, endpoint, key))
    {
        chat_load_settings();
    }
}

static void chat_configure_model(void)
{
    char model[CHAT_MODEL_MAX + 1];

    printf("\nConfigure chat model\n");
    printf("Current model: %s\n", s_chat_model);
    printf("Press Enter to keep it, '-' to restore default, or type a replacement.\n");
    if (!chat_serial_read_line("model> ", model, sizeof(model), false))
    {
        return;
    }
    if (model[0] == '\0')
    {
        return;
    }
    if (strcmp(model, "-") == 0)
    {
        strncpy(model, CHAT_DEFAULT_MODEL, sizeof(model) - 1);
        model[sizeof(model) - 1] = '\0';
    }
    if (config_save_chat_options(model, s_chat_max_tokens, s_chat_temperature))
    {
        chat_load_settings();
    }
}

static void chat_configure_max_tokens(void)
{
    char max_tokens[CHAT_MAX_TOKENS_MAX + 1];

    printf("\nConfigure chat max tokens\n");
    printf("Current max tokens: %s\n", s_chat_max_tokens);
    printf("Press Enter to keep it, '-' to restore default, or type a replacement.\n");
    if (!chat_serial_read_line("max tokens> ", max_tokens, sizeof(max_tokens), false))
    {
        return;
    }
    if (max_tokens[0] == '\0')
    {
        return;
    }
    if (strcmp(max_tokens, "-") == 0)
    {
        strncpy(max_tokens, CHAT_DEFAULT_MAX_TOKENS, sizeof(max_tokens) - 1);
        max_tokens[sizeof(max_tokens) - 1] = '\0';
    }
    if (!chat_valid_uint_text(max_tokens))
    {
        printf("Max tokens must be a positive integer.\n");
        return;
    }
    if (config_save_chat_options(s_chat_model, max_tokens, s_chat_temperature))
    {
        chat_load_settings();
    }
}

static void chat_configure_temperature(void)
{
    char temperature[CHAT_TEMPERATURE_MAX + 1];

    printf("\nConfigure chat temperature\n");
    printf("Current temperature: %s\n", s_chat_temperature);
    printf("Press Enter to keep it, '-' to restore default, or type a replacement.\n");
    if (!chat_serial_read_line("temperature> ", temperature, sizeof(temperature), false))
    {
        return;
    }
    if (temperature[0] == '\0')
    {
        return;
    }
    if (strcmp(temperature, "-") == 0)
    {
        strncpy(temperature, CHAT_DEFAULT_TEMPERATURE, sizeof(temperature) - 1);
        temperature[sizeof(temperature) - 1] = '\0';
    }
    if (!chat_valid_temperature_text(temperature))
    {
        printf("Temperature must be a number from 0.0 to 2.0.\n");
        return;
    }
    if (config_save_chat_options(s_chat_model, s_chat_max_tokens, temperature))
    {
        chat_load_settings();
    }
}

void chat_io_init(void)
{
    if (s_initialized)
    {
        return;
    }

    chat_load_settings();

    s_chat_request_queue = xQueueCreate(1, sizeof(chat_request_t *));
    s_chat_response_queue = xQueueCreateWithCaps(CHAT_RESPONSE_QUEUE_DEPTH,
                                                 sizeof(chat_response_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chat_response_queue)
    {
        s_chat_response_queue = xQueueCreate(CHAT_RESPONSE_QUEUE_DEPTH, sizeof(chat_response_t));
    }

    if (!s_chat_request_queue || !s_chat_response_queue)
    {
        ESP_LOGE(TAG, "Failed to create chat queues");
        return;
    }

    memset(&port_state, 0, sizeof(port_state));
    port_state.next_generation = 1;
    s_network_available = false;

    /* Move the 8KB request buffer out of internal DRAM into PSRAM. */
    port_state.request_capacity = CHAT_REQUEST_MAX;
    port_state.request = heap_caps_malloc(port_state.request_capacity,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!port_state.request)
    {
        port_state.request = malloc(port_state.request_capacity);
    }
    if (!port_state.request)
    {
        ESP_LOGE(TAG, "Failed to allocate chat request buffer");
        vQueueDelete(s_chat_request_queue);
        vQueueDelete(s_chat_response_queue);
        s_chat_request_queue = NULL;
        s_chat_response_queue = NULL;
        return;
    }
    port_state.request[0] = '\0';

    BaseType_t ret = xTaskCreatePinnedToCore(chat_client_task,
                                             "chat_client",
                                             CHAT_TASK_STACK_SIZE,
                                             NULL,
                                             CHAT_TASK_PRIORITY,
                                             NULL,
                                             CHAT_TASK_CORE);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create chat client task");
        vQueueDelete(s_chat_request_queue);
        vQueueDelete(s_chat_response_queue);
        s_chat_request_queue = NULL;
        s_chat_response_queue = NULL;
        return;
    }

    s_initialized = true;
    printf("[Chat] Chat port driver initialized on ports 120-124\n");
}

void chat_io_set_network_available(bool available)
{
    s_network_available = available;
}

void chat_io_run_config_shell(void)
{
    chat_load_settings();

    chat_print_menu();

    for (;;)
    {
        printf("chat> ");
        int cmd = chat_serial_read_command_ms(60000);
        printf("\n");
        if (cmd == -1)
        {
            printf("Chat provider manager timed out.\n\n");
            return;
        }

        switch (cmd)
        {
        case 0:
            break;

        case '1':
            chat_configure_openai();
            chat_print_menu();
            break;

        case '2':
            chat_configure_compatible();
            chat_print_menu();
            break;

        case '3':
            chat_configure_model();
            chat_print_menu();
            break;

        case '4':
            chat_configure_max_tokens();
            chat_print_menu();
            break;

        case '5':
            chat_configure_temperature();
            chat_print_menu();
            break;

        case 'S':
            chat_print_settings();
            chat_print_menu();
            break;

        case 'Q':
            printf("Leaving chat provider manager.\n\n");
            return;

        default:
            if (cmd > ' ')
            {
                printf("Unknown command '%c'. Use 1, 2, 3, 4, 5, S, or Q.\n", (char)cmd);
            }
            break;
        }
    }
}

void chat_io_run_boot_shell(void)
{
    chat_load_settings();

    if (!usb_serial_jtag_is_connected())
    {
        return;
    }

    chat_print_settings();
    printf("Press 'A' within 5 seconds to manage chat provider settings.\n");
    printf("Press Enter to continue boot now.\n");

    int c = chat_serial_read_command_ms(5000);
    if (c == -1 || c == 0 || c != 'A')
    {
        return;
    }

    chat_io_run_config_shell();
}

static void chat_reset_request(void)
{
    port_state.request_len = 0;
    if (port_state.request)
    {
        port_state.request[0] = '\0';
    }
    port_state.request_overflow = false;
}

static void chat_reset_response(void)
{
    chat_response_t discarded;
    while (s_chat_response_queue && xQueueReceive(s_chat_response_queue, &discarded, 0) == pdTRUE)
    {
    }
    port_state.eof_seen = false;
    port_state.has_pending_char = false;
    port_state.generation = port_state.next_generation++;
}

static void chat_request_add_char(uint8_t data)
{
    if (!port_state.request)
    {
        port_state.request_overflow = true;
        return;
    }

    if (data == 0)
    {
        if (port_state.request_len < port_state.request_capacity)
        {
            port_state.request[port_state.request_len] = '\0';
        }
        return;
    }

    if (port_state.request_len + 1 < port_state.request_capacity)
    {
        port_state.request[port_state.request_len++] = (char)data;
        port_state.request[port_state.request_len] = '\0';
    }
    else
    {
        port_state.request_overflow = true;
    }
}

static void chat_trigger_request(void)
{
    chat_reset_response();

    if (port_state.request_overflow || port_state.request_len == 0)
    {
        chat_queue_text(port_state.generation, "Chat request buffer error\n");
        chat_queue_eof(NULL, port_state.generation);
        return;
    }

    chat_request_t *request = heap_caps_malloc(sizeof(chat_request_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request)
    {
        request = malloc(sizeof(chat_request_t));
    }

    if (!request)
    {
        chat_queue_text(port_state.generation, "Chat out of memory\n");
        chat_queue_eof(NULL, port_state.generation);
        return;
    }

    memset(request, 0, sizeof(*request));
    request->generation = port_state.generation;
    request->len = port_state.request_len;
    memcpy(request->json, port_state.request, port_state.request_len + 1);

    if (!s_chat_request_queue || xQueueSend(s_chat_request_queue, &request, 0) != pdTRUE)
    {
        free(request);
        chat_queue_text(port_state.generation, "Chat request queue busy\n");
        chat_queue_eof(NULL, port_state.generation);
    }
}

size_t chat_output(int port, uint8_t data, char *buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;

    if (!s_initialized)
    {
        return 0;
    }

    if (port == CHAT_PORT_TRIGGER)
    {
        chat_reset_request();
    }
    else if (port == CHAT_PORT_REQUEST)
    {
        chat_request_add_char(data);
    }
    else if (port == CHAT_PORT_RESET_RESPONSE)
    {
        chat_reset_response();
    }

    return 0;
}

static bool chat_load_next_char(void)
{
    chat_response_t response;

    while (s_chat_response_queue && xQueueReceive(s_chat_response_queue, &response, 0) == pdTRUE)
    {
        if (response.generation != port_state.generation)
        {
            continue;
        }
        if (response.type == CHAT_RESP_EOF)
        {
            port_state.eof_seen = true;
            return false;
        }
        port_state.pending_char = response.data;
        port_state.has_pending_char = true;
        return true;
    }

    return false;
}

uint8_t chat_input(uint8_t port)
{
    if (!s_initialized)
    {
        return CHAT_STATUS_EOF;
    }

    if (port == CHAT_PORT_TRIGGER)
    {
        chat_trigger_request();
        return 0;
    }

    if (port == CHAT_PORT_STATUS)
    {
        if (port_state.has_pending_char || chat_load_next_char())
        {
            return CHAT_STATUS_DATA_READY;
        }
        return port_state.eof_seen ? CHAT_STATUS_EOF : CHAT_STATUS_WAITING;
    }

    if (port == CHAT_PORT_DATA)
    {
        if (!port_state.has_pending_char)
        {
            chat_load_next_char();
        }
        if (port_state.has_pending_char)
        {
            port_state.has_pending_char = false;
            return port_state.pending_char;
        }
    }

    return 0;
}

static void chat_client_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        chat_request_t *request = NULL;
        if (xQueueReceive(s_chat_request_queue, &request, portMAX_DELAY) != pdTRUE || !request)
        {
            continue;
        }

        chat_process_request(request);
        free(request);
    }
}

static bool chat_starts_with_ci(const char *text, const char *prefix)
{
    while (*prefix)
    {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

static bool chat_str_eq_ci(const char *left, const char *right)
{
    while (*left && *right)
    {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

static bool chat_parse_endpoint(const char *endpoint, chat_endpoint_t *parsed)
{
    const char *cursor;
    const char *path_start;
    const char *host_end;
    const char *port_start = NULL;
    size_t host_len;
    size_t host_header_len;
    size_t path_len;
    int port = 0;

    if (!endpoint || !parsed)
    {
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));

    if (chat_starts_with_ci(endpoint, "https://"))
    {
        parsed->https = true;
        parsed->port = 443;
        cursor = endpoint + 8;
    }
    else if (chat_starts_with_ci(endpoint, "http://"))
    {
        parsed->https = false;
        parsed->port = 80;
        cursor = endpoint + 7;
    }
    else
    {
        return false;
    }

    path_start = strchr(cursor, '/');
    host_end = path_start ? path_start : cursor + strlen(cursor);
    if (host_end == cursor)
    {
        return false;
    }

    for (const char *p = cursor; p < host_end; p++)
    {
        if (*p == ':')
        {
            port_start = p + 1;
            host_end = p;
            break;
        }
    }

    host_len = (size_t)(host_end - cursor);
    if (host_len == 0 || host_len >= sizeof(parsed->host))
    {
        return false;
    }
    memcpy(parsed->host, cursor, host_len);
    parsed->host[host_len] = '\0';

    if (port_start)
    {
        port = atoi(port_start);
        if (port <= 0 || port > 65535)
        {
            return false;
        }
        parsed->port = port;
    }

    host_header_len = path_start ? (size_t)(path_start - cursor) : strlen(cursor);
    if (host_header_len == 0 || host_header_len >= sizeof(parsed->host_header))
    {
        return false;
    }
    memcpy(parsed->host_header, cursor, host_header_len);
    parsed->host_header[host_header_len] = '\0';

    if (!path_start || path_start[0] == '\0' || strcmp(path_start, "/") == 0)
    {
        strncpy(parsed->path, CHAT_PATH, sizeof(parsed->path) - 1);
        parsed->path[sizeof(parsed->path) - 1] = '\0';
    }
    else if (chat_str_eq_ci(path_start, CHAT_PATH))
    {
        strncpy(parsed->path, CHAT_PATH, sizeof(parsed->path) - 1);
        parsed->path[sizeof(parsed->path) - 1] = '\0';
    }
    else
    {
        path_len = strlen(path_start);
        if (path_len >= sizeof(parsed->path))
        {
            return false;
        }
        memcpy(parsed->path, path_start, path_len + 1);
    }

    return true;
}

static void chat_process_request(chat_request_t *request)
{
    if (!chat_provider_is_compatible() && s_chat_api_key[0] == '\0')
    {
        chat_queue_text(request->generation, "Chat API key not configured\n");
        chat_queue_eof(NULL, request->generation);
        return;
    }

    if (!s_network_available || !wifi_is_connected())
    {
        chat_queue_text(request->generation, "Chat network unavailable\n");
        chat_queue_eof(NULL, request->generation);
        return;
    }

    const char *endpoint = chat_provider_is_compatible() ? s_chat_endpoint : CHAT_DEFAULT_OPENAI_ENDPOINT;
    chat_endpoint_t parsed_endpoint;
    if (!chat_parse_endpoint(endpoint, &parsed_endpoint))
    {
        ESP_LOGE(TAG, "Failed to parse chat endpoint: %s", (endpoint && endpoint[0]) ? endpoint : "(empty)");
        chat_queue_text(request->generation, "Chat endpoint invalid\n");
        chat_queue_eof(NULL, request->generation);
        return;
    }

    /* Allocate the parser, RX buffer and HTTP header buffers from PSRAM
     * in a single block so the chat task stack stays small. */
    chat_workspace_t *ws = heap_caps_malloc(sizeof(chat_workspace_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ws)
    {
        ws = malloc(sizeof(chat_workspace_t));
    }
    if (!ws)
    {
        chat_queue_text(request->generation, "Chat out of memory\n");
        chat_queue_eof(NULL, request->generation);
        return;
    }
    memset(&ws->parser, 0, sizeof(ws->parser));
    ws->parser.generation = request->generation;
    ws->parser.chunk_state = CHUNK_SIZE;
    ws->auth_header[0] = '\0';

    size_t body_len = request->len;
    const char *body = request->json;

    esp_tls_t *tls = esp_tls_init();
    if (!tls)
    {
        chat_queue_text(request->generation, "Chat TLS init failed\n");
        chat_queue_eof(NULL, request->generation);
        free(ws);
        return;
    }

    esp_tls_cfg_t cfg = {
        .timeout_ms = CHAT_CONNECT_TIMEOUT_MS,
        .is_plain_tcp = !parsed_endpoint.https,
    };
    if (parsed_endpoint.https)
    {
        cfg.common_name = parsed_endpoint.host;
        if (chat_provider_is_compatible())
        {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        else
        {
            cfg.cacert_buf = CHAT_GTS_ROOT_R4_PEM;
            cfg.cacert_bytes = sizeof(CHAT_GTS_ROOT_R4_PEM);
        }
    }

    int ret = esp_tls_conn_new_sync(parsed_endpoint.host, strlen(parsed_endpoint.host), parsed_endpoint.port, &cfg, tls);
    if (ret != 1)
    {
        chat_log_tls_error(tls, ret);
        chat_queue_text(request->generation, "Chat connect failed\n");
        chat_queue_eof(NULL, request->generation);
        esp_tls_conn_destroy(tls);
        free(ws);
        return;
    }

    if (s_chat_api_key[0] != '\0')
    {
        snprintf(ws->auth_header, CHAT_AUTH_HEADER_MAX,
                 "Authorization: Bearer %s\r\n", s_chat_api_key);
    }
    int header_len = snprintf(ws->header, CHAT_HEADER_MAX,
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "%s"
                              "Content-Type: application/json\r\n"
                              "Accept: text/event-stream\r\n"
                              "Connection: close\r\n"
                              "Content-Length: %u\r\n\r\n",
                              parsed_endpoint.path, parsed_endpoint.host_header, ws->auth_header, (unsigned int)body_len);
    if (header_len <= 0 || (size_t)header_len >= CHAT_HEADER_MAX)
    {
        chat_queue_text(request->generation, "Chat request header error\n");
        chat_queue_eof(NULL, request->generation);
        esp_tls_conn_destroy(tls);
        free(ws);
        return;
    }

    if (!chat_send_all(tls, (const uint8_t *)ws->header, (size_t)header_len) ||
        !chat_send_all(tls, (const uint8_t *)body, body_len))
    {
        chat_queue_text(request->generation, "Chat send failed\n");
        chat_queue_eof(NULL, request->generation);
        esp_tls_conn_destroy(tls);
        free(ws);
        return;
    }

    if (!chat_read_response(tls, ws) && !ws->parser.done)
    {
        chat_queue_text(request->generation, "Chat stream error\n");
    }
    chat_queue_eof(&ws->parser, request->generation);

    esp_tls_conn_destroy(tls);
    free(ws);
}

static void chat_log_tls_error(esp_tls_t *tls, int ret)
{
    esp_tls_error_handle_t error_handle = NULL;
    int esp_tls_code = 0;
    int esp_tls_flags = 0;

    if (tls && esp_tls_get_error_handle(tls, &error_handle) == ESP_OK && error_handle &&
        esp_tls_get_and_clear_last_error(error_handle, &esp_tls_code, &esp_tls_flags) == ESP_OK)
    {
        char error_text[96];
        mbedtls_strerror(esp_tls_code, error_text, sizeof(error_text));
        ESP_LOGE(TAG, "TLS connect failed: ret=%d tls=0x%x flags=0x%x %s",
                 ret, esp_tls_code, esp_tls_flags, error_text);
        return;
    }

    ESP_LOGE(TAG, "TLS connect failed: ret=%d", ret);
}

static bool chat_send_all(esp_tls_t *tls, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    uint32_t start_ms = chat_ms();

    while (sent < len)
    {
        ssize_t written = esp_tls_conn_write(tls, data + sent, len - sent);
        if (written > 0)
        {
            sent += (size_t)written;
            start_ms = chat_ms();
            continue;
        }

        if (written == 0 || (errno != EAGAIN && errno != EINTR))
        {
            return false;
        }

        if (chat_ms() - start_ms > CHAT_CONNECT_TIMEOUT_MS)
        {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

static bool chat_read_response(esp_tls_t *tls, chat_workspace_t *ws)
{
    chat_parse_t *parser = &ws->parser;
    uint32_t last_rx_ms = chat_ms();

    while (!parser->done)
    {
        ssize_t len = esp_tls_conn_read(tls, ws->rx_buffer, CHAT_RX_BUFFER_SIZE);
        if (len > 0)
        {
            last_rx_ms = chat_ms();
            for (ssize_t i = 0; i < len && !parser->done; i++)
            {
                chat_process_rx_byte(parser, ws->rx_buffer[i]);
            }
            continue;
        }

        if (len == 0)
        {
            return true;
        }

        if (errno != EAGAIN && errno != EINTR)
        {
            return false;
        }

        if (chat_ms() - last_rx_ms > CHAT_STREAM_TIMEOUT_MS)
        {
            chat_queue_text(parser->generation, "Chat stream timeout\n");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

static void chat_process_rx_byte(chat_parse_t *parser, uint8_t ch)
{
    if (!parser->headers_done)
    {
        if (!parser->status_checked)
        {
            if (ch == '\r')
            {
                parser->status_line[parser->status_line_len] = '\0';
                parser->status_checked = true;
                const char *sp = strchr(parser->status_line, ' ');
                if (sp)
                {
                    parser->status_code = atoi(sp + 1);
                }
                if (parser->status_code != 200)
                {
                    char msg[160];
                    if (parser->status_code == 404)
                    {
                        snprintf(msg, sizeof(msg),
                                 "Chat HTTP 404 - endpoint or model not found.\n"
                                 "Check 'endpoint' URL and 'model' in chat.cfg.\n");
                    }
                    else if (parser->status_code == 401 || parser->status_code == 403)
                    {
                        snprintf(msg, sizeof(msg),
                                 "Chat HTTP %d - auth failed. Check API key.\n",
                                 parser->status_code);
                    }
                    else if (parser->status_code == 400)
                    {
                        snprintf(msg, sizeof(msg),
                                 "Chat HTTP 400 - bad request. Check 'model' in chat.cfg\n"
                                 "and chat.sys content.\n");
                    }
                    else if (parser->status_code == 429)
                    {
                        snprintf(msg, sizeof(msg),
                                 "Chat HTTP 429 - rate limit or quota exceeded.\n");
                    }
                    else if (parser->status_code >= 500)
                    {
                        snprintf(msg, sizeof(msg),
                                 "Chat HTTP %d - server error. Try again later.\n",
                                 parser->status_code);
                    }
                    else
                    {
                        snprintf(msg, sizeof(msg), "Chat HTTP %d\n", parser->status_code);
                    }
                    chat_queue_text(parser->generation, msg);
                    parser->done = true;
                    return;
                }
            }
            else if (parser->status_line_len + 1 < sizeof(parser->status_line))
            {
                parser->status_line[parser->status_line_len++] = (char)ch;
            }
        }

        parser->header_match = (parser->header_match << 8) | ch;
        if (parser->header_match == 0x0d0a0d0au)
        {
            parser->headers_done = true;
        }
        return;
    }

    chat_process_body_byte(parser, ch);
}

static void chat_process_body_byte(chat_parse_t *parser, uint8_t ch)
{
    switch (parser->chunk_state)
    {
        case CHUNK_SIZE:
        {
            int value = chat_hex_nibble((char)ch);
            if (value >= 0 && !parser->chunk_extension)
            {
                parser->chunk_size = (parser->chunk_size << 4) | (size_t)value;
            }
            else if (ch == ';')
            {
                parser->chunk_extension = true;
            }
            else if (ch == '\r')
            {
                parser->chunk_state = CHUNK_SIZE_LF;
            }
            break;
        }

        case CHUNK_SIZE_LF:
            if (ch == '\n')
            {
                if (parser->chunk_size == 0)
                {
                    parser->done = true;
                }
                else
                {
                    parser->chunk_read = 0;
                    parser->chunk_state = CHUNK_DATA;
                }
            }
            break;

        case CHUNK_DATA:
            chat_process_sse_byte(parser, ch);
            parser->chunk_read++;
            if (parser->chunk_read >= parser->chunk_size)
            {
                parser->chunk_state = CHUNK_DATA_CR;
            }
            break;

        case CHUNK_DATA_CR:
            parser->chunk_state = CHUNK_DATA_LF;
            break;

        case CHUNK_DATA_LF:
            parser->chunk_size = 0;
            parser->chunk_read = 0;
            parser->chunk_extension = false;
            parser->chunk_state = CHUNK_SIZE;
            break;
    }
}

static void chat_process_sse_byte(chat_parse_t *parser, uint8_t ch)
{
    if (ch == '\r')
    {
        return;
    }

    if (ch != '\n')
    {
        if (parser->sse_len + 1 < sizeof(parser->sse_line))
        {
            parser->sse_line[parser->sse_len++] = (char)ch;
        }
        return;
    }

    parser->sse_line[parser->sse_len] = '\0';
    if (strncmp(parser->sse_line, "data: ", 6) == 0)
    {
        const char *payload = parser->sse_line + 6;
        if (strcmp(payload, "[DONE]") == 0)
        {
            parser->done = true;
        }
        else
        {
            chat_extract_content(parser, payload);
        }
    }
    parser->sse_len = 0;
}

static int chat_hex_nibble(char ch)
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
 */
static char chat_cp_to_ascii(uint32_t cp)
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
        case 0x201A: case 0x2032: return '\''; /* single quotes / prime */
        case 0x201C: case 0x201D:
        case 0x201E: case 0x2033: return '"';  /* double quotes */
        case 0x2022: case 0x2023:
        case 0x25E6: case 0x2043: return '*';  /* bullets */
        case 0x2026: return '.';                /* ellipsis -> '.' */
        case 0x2192: return '>';                /* right arrow */
        case 0x2190: return '<';                /* left arrow */
        case 0x00D7: return 'x';                /* multiply */
        default: return 0;
    }
}

static void chat_emit_codepoint(chat_parse_t *parser, uint32_t cp)
{
    if (cp < 0x80)
    {
        chat_queue_char(parser, parser->generation, (uint8_t)cp, false);
        return;
    }
    char repl = chat_cp_to_ascii(cp);
    if (repl == '.' && cp == 0x2026)
    {
        /* ellipsis: emit "..." */
        chat_queue_char(parser, parser->generation, '.', false);
        chat_queue_char(parser, parser->generation, '.', false);
        chat_queue_char(parser, parser->generation, '.', false);
        return;
    }
    if (repl)
    {
        chat_queue_char(parser, parser->generation, (uint8_t)repl, false);
    }
    /* else: silently drop unknown codepoint */
}

static void chat_emit_json_string(chat_parse_t *parser, const char *ptr)
{
    while (*ptr && *ptr != '"')
    {
        uint8_t b = (uint8_t)*ptr++;
        if (b == '\\' && *ptr)
        {
            char esc = *ptr++;
            switch (esc)
            {
                case 'n': chat_queue_char(parser, parser->generation, '\n', false); break;
                case 'r': chat_queue_char(parser, parser->generation, '\r', false); break;
                case 't': chat_queue_char(parser, parser->generation, '\t', false); break;
                case '"': chat_queue_char(parser, parser->generation, '"', false); break;
                case '\\': chat_queue_char(parser, parser->generation, '\\', false); break;
                case '/': chat_queue_char(parser, parser->generation, '/', false); break;
                case 'b': chat_queue_char(parser, parser->generation, '\b', false); break;
                case 'f': chat_queue_char(parser, parser->generation, '\f', false); break;
                case 'u':
                {
                    int n0 = chat_hex_nibble(ptr[0]);
                    int n1 = (n0 >= 0) ? chat_hex_nibble(ptr[1]) : -1;
                    int n2 = (n1 >= 0) ? chat_hex_nibble(ptr[2]) : -1;
                    int n3 = (n2 >= 0) ? chat_hex_nibble(ptr[3]) : -1;
                    if (n3 >= 0)
                    {
                        uint32_t cp = (uint32_t)((n0 << 12) | (n1 << 8) | (n2 << 4) | n3);
                        ptr += 4;
                        /* surrogate pair */
                        if (cp >= 0xD800 && cp <= 0xDBFF && ptr[0] == '\\' && ptr[1] == 'u')
                        {
                            int m0 = chat_hex_nibble(ptr[2]);
                            int m1 = (m0 >= 0) ? chat_hex_nibble(ptr[3]) : -1;
                            int m2 = (m1 >= 0) ? chat_hex_nibble(ptr[4]) : -1;
                            int m3 = (m2 >= 0) ? chat_hex_nibble(ptr[5]) : -1;
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
                        chat_emit_codepoint(parser, cp);
                    }
                    break;
                }
                default:
                    chat_queue_char(parser, parser->generation, (uint8_t)esc & 0x7f, false);
                    break;
            }
            continue;
        }

        if (b < 0x80)
        {
            chat_queue_char(parser, parser->generation, b, false);
            continue;
        }

        /* UTF-8 lead byte: decode the codepoint, then map to ASCII. */
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
            chat_emit_codepoint(parser, cp);
        }
    }
}


static void chat_extract_content(chat_parse_t *parser, const char *json)
{
    const char *ptr = json;
    const char *marker = "\"content\":\"";
    size_t marker_len = strlen(marker);

    while ((ptr = strstr(ptr, marker)) != NULL)
    {
        ptr += marker_len;
        chat_emit_json_string(parser, ptr);
    }
}

static void chat_queue_response(chat_parse_t *parser, const chat_response_t *response, bool force)
{
    if (!s_chat_response_queue)
    {
        return;
    }

    /* Force path: used for EOF / truncation messages. Displaces the
     * oldest queued char if the consumer is wedged so the EOF always
     * gets through. */
    if (force)
    {
        if (xQueueSend(s_chat_response_queue, response, 0) == pdTRUE)
        {
            return;
        }
        chat_response_t discarded;
        while (xQueueSend(s_chat_response_queue, response, 0) != pdTRUE)
        {
            if (xQueueReceive(s_chat_response_queue, &discarded, 0) != pdTRUE)
            {
                return;
            }
        }
        return;
    }

    /* Streaming chars: block with a short timeout so we apply
     * back-pressure rather than dropping data. If the app has moved on
     * to a new generation, abort the stream cleanly. */
    while (xQueueSend(s_chat_response_queue, response, pdMS_TO_TICKS(CHAT_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        if (parser && port_state.generation != parser->generation)
        {
            parser->done = true;
            return;
        }
    }
}

static void chat_queue_char(chat_parse_t *parser, uint32_t generation, uint8_t data, bool force)
{
    chat_response_t response = {
        .generation = generation,
        .type = CHAT_RESP_CHAR,
        .data = data,
    };
    chat_queue_response(parser, &response, force);
}

static void chat_queue_text(uint32_t generation, const char *text)
{
    while (*text)
    {
        chat_queue_char(NULL, generation, (uint8_t)(*text++ & 0x7f), false);
    }
}

static void chat_queue_text_force(uint32_t generation, const char *text)
{
    while (*text)
    {
        chat_queue_char(NULL, generation, (uint8_t)(*text++ & 0x7f), true);
    }
}

static void chat_queue_eof(chat_parse_t *parser, uint32_t generation)
{
    if (parser && parser->response_truncated)
    {
        parser->response_truncated = false;
        chat_queue_text_force(generation, "\nChat response truncated\n");
    }

    chat_response_t response = {
        .generation = generation,
        .type = CHAT_RESP_EOF,
        .data = 0,
    };
    chat_queue_response(parser, &response, true);
}
