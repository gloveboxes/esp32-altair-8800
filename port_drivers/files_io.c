/**
 * @file files_io.c
 * @brief Remote file transfer I/O port driver for Altair 8800 emulator on ESP32
 *
 * Uses a Core 0 task to handle TCP communication with remote_ft_server.py.
 * Emulator I/O runs on Core 1 and communicates via FreeRTOS queues.
 * Data payloads stay in a shared buffer to avoid large queue copies.
 */

#include "port_drivers/files_io.h"

#include "config.h"
#include "wifi.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netinet/tcp.h"

// Server port
#ifndef FT_SERVER_PORT
#define FT_SERVER_PORT 8090
#endif

// Protocol commands (to server)
#define FT_PROTO_GET_CHUNK 0x02
#define FT_PROTO_CLOSE 0x03

// Protocol responses (from server)
#define FT_PROTO_RESP_OK 0x00
#define FT_PROTO_RESP_EOF 0x01
#define FT_PROTO_RESP_ERROR 0xFF

// Queue sizes
#define FT_REQUEST_QUEUE_DEPTH 2
#define FT_RESPONSE_QUEUE_DEPTH 1

// Task config
#define FT_TASK_STACK_SIZE 4096
#define FT_TASK_PRIORITY 6
#define FT_TASK_CORE 0

// Socket timeout (ms)
#define FT_SOCKET_TIMEOUT_MS 5000

// Filename buffer size
#define FT_MAX_FILENAME 128

static const char* TAG = "FT_IO";

// Request types for inter-core queue
typedef enum
{
    FT_REQ_GET_CHUNK = 0,
    FT_REQ_CLOSE
} ft_request_type_t;

// Request structure (Core 1 -> Core 0)
typedef struct
{
    ft_request_type_t type;
    uint32_t offset; // file offset for GET_CHUNK (stateless protocol)
    char filename[FT_MAX_FILENAME];
} ft_request_t;

// Response metadata (Core 0 -> Core 1)
typedef struct
{
    ft_status_t status;
    uint8_t count;
    size_t len; // actual bytes in chunk (0-256)
    bool has_count;
} ft_response_meta_t;

// Queues for inter-core communication
static QueueHandle_t s_ft_request_queue = NULL;  // Core 1 -> Core 0
static QueueHandle_t s_ft_response_queue = NULL; // Core 0 -> Core 1

// Socket state (Core 0)
static int s_ft_sock = -1;

// Shared response data buffer (Core 0 writes, Core 1 reads)
static uint8_t s_ft_shared_data[FT_CHUNK_SIZE];

// Port state (Core 1)
static struct
{
    char filename[FT_MAX_FILENAME];
    size_t filename_idx;

    // Current chunk read state (count byte + data)
    size_t chunk_len;      // total bytes to read (count + data)
    size_t chunk_position; // current read position
    uint8_t count_byte;    // raw count byte (0 encodes 256)

    // File position tracking for stateless protocol
    uint32_t file_offset;

    ft_status_t status;
} port_state;

static bool s_initialized = false;

// Forward declarations
static size_t files_output_command(uint8_t data);
static void files_output_data(uint8_t data);
static uint8_t files_input_status(void);
static uint8_t files_input_data(void);
static void ft_client_task(void* arg);
static void ft_disconnect(void);
static bool ft_ensure_connected(void);
static bool ft_send_all(int sock, const uint8_t* data, size_t len);
static bool ft_recv_all(int sock, uint8_t* data, size_t len);
static bool ft_send_get_chunk(const ft_request_t* request);
static bool ft_receive_chunk(ft_response_meta_t* meta);
static bool ft_send_close(const ft_request_t* request);
static void ft_push_error_response(void);
static void ft_drain_response_queue(void);

// =====================================================================
// Initialization
// =====================================================================

void files_io_init(void)
{
    if (s_initialized) {
        return;
    }

    s_ft_request_queue = xQueueCreate(FT_REQUEST_QUEUE_DEPTH, sizeof(ft_request_t));
    s_ft_response_queue = xQueueCreate(FT_RESPONSE_QUEUE_DEPTH, sizeof(ft_response_meta_t));

    if (!s_ft_request_queue || !s_ft_response_queue) {
        ESP_LOGE(TAG, "Failed to create FT queues");
        if (s_ft_request_queue) {
            vQueueDelete(s_ft_request_queue);
            s_ft_request_queue = NULL;
        }
        if (s_ft_response_queue) {
            vQueueDelete(s_ft_response_queue);
            s_ft_response_queue = NULL;
        }
        return;
    }

    memset(&port_state, 0, sizeof(port_state));
    port_state.status = FT_STATUS_IDLE;

    BaseType_t ret = xTaskCreatePinnedToCore(
        ft_client_task,
        "ft_client",
        FT_TASK_STACK_SIZE,
        NULL,
        FT_TASK_PRIORITY,
        NULL,
        FT_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create FT client task");
        vQueueDelete(s_ft_request_queue);
        vQueueDelete(s_ft_response_queue);
        s_ft_request_queue = NULL;
        s_ft_response_queue = NULL;
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "File transfer driver initialized");
}

// =====================================================================
// Core 1: Port Handlers
// =====================================================================

size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;

    if (!s_initialized) {
        return 0;
    }

    switch (port)
    {
        case 60:
            return files_output_command(data);
        case 61:
            files_output_data(data);
            return 0;
        default:
            return 0;
    }
}

static size_t files_output_command(uint8_t data)
{
    ft_request_t request;

    switch ((ft_command_t)data)
    {
        case FT_CMD_NOP:
        case FT_CMD_FILENAME_CHAR:
            // No action required
            break;

        case FT_CMD_SET_FILENAME:
            // Start of filename - reset state to avoid stale data
            port_state.filename_idx = 0;
            memset(port_state.filename, 0, sizeof(port_state.filename));
            port_state.chunk_len = 0;
            port_state.chunk_position = 0;
            port_state.file_offset = 0;
            port_state.status = FT_STATUS_IDLE;
            ft_drain_response_queue();
            break;

        case FT_CMD_REQUEST_CHUNK:
            // If we still have data in buffer, don't request more
            if (port_state.chunk_len > 0 && port_state.chunk_position < port_state.chunk_len) {
                break;
            }

            if (port_state.filename[0] == '\0') {
                port_state.status = FT_STATUS_ERROR;
                break;
            }

            request.type = FT_REQ_GET_CHUNK;
            request.offset = port_state.file_offset;
            strncpy(request.filename, port_state.filename, sizeof(request.filename) - 1);
            request.filename[sizeof(request.filename) - 1] = '\0';

            if (s_ft_request_queue &&
                xQueueSend(s_ft_request_queue, &request, 0) == pdTRUE) {
                port_state.chunk_len = 0;
                port_state.chunk_position = 0;
                port_state.status = FT_STATUS_BUSY;
            } else {
                port_state.status = FT_STATUS_ERROR;
            }
            break;

        case FT_CMD_CLOSE:
            request.type = FT_REQ_CLOSE;
            request.offset = 0;
            strncpy(request.filename, port_state.filename, sizeof(request.filename) - 1);
            request.filename[sizeof(request.filename) - 1] = '\0';
            if (s_ft_request_queue) {
                (void)xQueueSend(s_ft_request_queue, &request, 0);
            }
            port_state.status = FT_STATUS_IDLE;
            break;

        default:
            break;
    }

    return 0;
}

static void files_output_data(uint8_t data)
{
    if (data == 0) {
        // Null terminator - filename complete
        port_state.filename[port_state.filename_idx] = '\0';

        // Reset state for new file
        port_state.chunk_len = 0;
        port_state.chunk_position = 0;
        port_state.file_offset = 0;
        port_state.filename_idx = 0;
        port_state.status = FT_STATUS_IDLE;
        ft_drain_response_queue();
        return;
    }

    if (port_state.filename_idx < sizeof(port_state.filename) - 1) {
        port_state.filename[port_state.filename_idx++] = (char)data;
        return;
    }

    // Filename buffer overflow - reject and signal error
    ESP_LOGE(TAG, "Filename too long (max %zu chars)", sizeof(port_state.filename) - 1);
    port_state.status = FT_STATUS_ERROR;
    port_state.filename_idx = 0;
    memset(port_state.filename, 0, sizeof(port_state.filename));
}

static bool files_process_response(void)
{
    if (!s_ft_response_queue) {
        return false;
    }

    ft_response_meta_t meta;
    if (xQueueReceive(s_ft_response_queue, &meta, 0) != pdTRUE) {
        return false;
    }

    if (meta.has_count) {
        port_state.count_byte = meta.count;
        port_state.chunk_len = meta.len + 1; // count + data
        port_state.chunk_position = 0;
        port_state.file_offset += meta.len;
    } else {
        port_state.chunk_len = 0;
        port_state.chunk_position = 0;
    }

    port_state.status = meta.status;
    return true;
}

static uint8_t files_input_status(void)
{
    // Check for new response only if buffer is depleted
    if (port_state.chunk_len == 0 || port_state.chunk_position >= port_state.chunk_len) {
        (void)files_process_response();
    }

    // If data is available in the buffer, always report DATAREADY
    if (port_state.chunk_position < port_state.chunk_len && port_state.status != FT_STATUS_ERROR) {
        return FT_STATUS_DATAREADY;
    }

    return port_state.status;
}

static uint8_t files_input_data(void)
{
    if (port_state.chunk_position < port_state.chunk_len) {
        uint8_t byte = 0x00;
        if (port_state.chunk_position == 0) {
            byte = port_state.count_byte;
        } else {
            size_t data_index = port_state.chunk_position - 1;
            if (data_index < FT_CHUNK_SIZE) {
                byte = s_ft_shared_data[data_index];
            }
        }

        port_state.chunk_position++;
        if (port_state.chunk_position >= port_state.chunk_len) {
            port_state.chunk_len = 0;
            port_state.chunk_position = 0;
        }
        return byte;
    }
    return 0x00;
}

uint8_t files_input(uint8_t port)
{
    if (!s_initialized) {
        return FT_STATUS_ERROR;
    }

    switch (port)
    {
        case 60:
            return files_input_status();
        case 61:
            return files_input_data();
        default:
            return 0x00;
    }
}

// =====================================================================
// Core 0: TCP Client Task
// =====================================================================

static void ft_client_task(void* arg)
{
    (void)arg;

    for (;;) {
        ft_request_t request;
        if (xQueueReceive(s_ft_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected");
            ft_disconnect();
            ft_push_error_response();
            continue;
        }

        if (!ft_ensure_connected()) {
            ft_push_error_response();
            continue;
        }

        bool ok = true;
        if (request.type == FT_REQ_GET_CHUNK) {
            if (!ft_send_get_chunk(&request)) {
                ok = false;
            } else {
                ft_response_meta_t meta = {0};
                if (!ft_receive_chunk(&meta)) {
                    ok = false;
                } else if (s_ft_response_queue) {
                    xQueueOverwrite(s_ft_response_queue, &meta);
                }
            }
        } else if (request.type == FT_REQ_CLOSE) {
            if (!ft_send_close(&request)) {
                ok = false;
            } else {
                uint8_t status = 0;
                if (!ft_recv_all(s_ft_sock, &status, 1)) {
                    ok = false;
                }
            }
        }

        if (!ok) {
            ft_disconnect();
            if (request.type == FT_REQ_GET_CHUNK) {
                ft_push_error_response();
            }
        }
    }
}

static void ft_disconnect(void)
{
    if (s_ft_sock >= 0) {
        lwip_close(s_ft_sock);
        s_ft_sock = -1;
    }
}

static bool ft_ensure_connected(void)
{
    if (s_ft_sock >= 0) {
        return true;
    }

    const char* server_ip = config_get_rfs_ip();
    if (!server_ip || server_ip[0] == '\0') {
        ESP_LOGW(TAG, "FT server IP not configured");
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return false;
    }

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct timeval timeout = {
        .tv_sec = FT_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (FT_SOCKET_TIMEOUT_MS % 1000) * 1000
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(FT_SERVER_PORT)
    };

    if (inet_pton(AF_INET, server_ip, &dest_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid FT server IP: %s", server_ip);
        lwip_close(sock);
        return false;
    }

    ESP_LOGI(TAG, "Connecting to FT server %s:%d", server_ip, FT_SERVER_PORT);
    if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "connect() failed: errno=%d", errno);
        lwip_close(sock);
        return false;
    }

    s_ft_sock = sock;
    return true;
}

static bool ft_send_all(int sock, const uint8_t* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t res = send(sock, data + sent, len - sent, 0);
        if (res <= 0) {
            return false;
        }
        sent += (size_t)res;
    }
    return true;
}

static bool ft_recv_all(int sock, uint8_t* data, size_t len)
{
    size_t received = 0;
    while (received < len) {
        ssize_t res = recv(sock, data + received, len - received, 0);
        if (res <= 0) {
            return false;
        }
        received += (size_t)res;
    }
    return true;
}

static bool ft_send_get_chunk(const ft_request_t* request)
{
    uint8_t buf[1 + 4 + FT_MAX_FILENAME];
    size_t name_len = strnlen(request->filename, FT_MAX_FILENAME - 1);

    buf[0] = FT_PROTO_GET_CHUNK;
    buf[1] = (uint8_t)(request->offset & 0xFF);
    buf[2] = (uint8_t)((request->offset >> 8) & 0xFF);
    buf[3] = (uint8_t)((request->offset >> 16) & 0xFF);
    buf[4] = (uint8_t)((request->offset >> 24) & 0xFF);
    memcpy(&buf[5], request->filename, name_len);
    buf[5 + name_len] = '\0';

    size_t send_len = 5 + name_len + 1;
    return ft_send_all(s_ft_sock, buf, send_len);
}

static bool ft_receive_chunk(ft_response_meta_t* meta)
{
    uint8_t header[2];
    if (!ft_recv_all(s_ft_sock, header, sizeof(header))) {
        return false;
    }

    uint8_t server_status = header[0];
    uint8_t count = header[1];
    size_t payload_len = 0;
    bool has_payload = (server_status == FT_PROTO_RESP_OK || server_status == FT_PROTO_RESP_EOF);

    if (has_payload) {
        payload_len = (count == 0) ? FT_CHUNK_SIZE : count;
        if (payload_len > FT_CHUNK_SIZE) {
            ESP_LOGE(TAG, "Invalid chunk size: %zu", payload_len);
            return false;
        }
        if (payload_len > 0 && !ft_recv_all(s_ft_sock, s_ft_shared_data, payload_len)) {
            return false;
        }
    }

    meta->status = (server_status == FT_PROTO_RESP_OK) ? FT_STATUS_DATAREADY :
                   (server_status == FT_PROTO_RESP_EOF) ? FT_STATUS_EOF : FT_STATUS_ERROR;
    meta->has_count = has_payload;
    meta->count = count;
    meta->len = payload_len;
    return true;
}

static bool ft_send_close(const ft_request_t* request)
{
    uint8_t buf[1 + FT_MAX_FILENAME];
    size_t name_len = strnlen(request->filename, FT_MAX_FILENAME - 1);

    buf[0] = FT_PROTO_CLOSE;
    memcpy(&buf[1], request->filename, name_len);
    buf[1 + name_len] = '\0';

    size_t send_len = 1 + name_len + 1;
    return ft_send_all(s_ft_sock, buf, send_len);
}

static void ft_push_error_response(void)
{
    if (!s_ft_response_queue) {
        return;
    }

    ft_response_meta_t meta = {
        .status = FT_STATUS_ERROR,
        .count = 0,
        .len = 0,
        .has_count = false
    };
    xQueueOverwrite(s_ft_response_queue, &meta);
}

static void ft_drain_response_queue(void)
{
    if (!s_ft_response_queue) {
        return;
    }

    ft_response_meta_t meta;
    while (xQueueReceive(s_ft_response_queue, &meta, 0) == pdTRUE) {
        // Drain queue
    }
}
