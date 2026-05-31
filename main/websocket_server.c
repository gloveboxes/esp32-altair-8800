/**
 * @file websocket_server.c
 * @brief WebSocket server implementation for Altair 8800 terminal
 *
 * Uses ESP-IDF's esp_http_server with WebSocket support.
 * Serves terminal HTML on root path and handles WebSocket connections.
 * 
 * Single-client model: Only one WebSocket client at a time. New connections
 * kick the existing client, which handles browser refresh gracefully.
 */

#include "websocket_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"

// Terminal HTML (gzip compressed)
#include "static_html_hex.h"

static const char* TAG = "WS_Server";

// Maximum payload of a single WebSocket data frame. Must be >= the TX task's
// drain batch (WS_TX_BATCH_SIZE in websocket_console.c, currently 1024).
#define WS_SEND_BUF_SIZE 1024

typedef struct {
    int fd;
    size_t len;
    bool is_ping;
    uint8_t data[WS_SEND_BUF_SIZE];
} ws_send_work_t;

// Single statically-allocated send-work buffer. Output frames are produced by a
// single TX task and sent one-at-a-time, so one buffer suffices and avoids a
// per-chunk malloc (which could fail under heap pressure and silently drop
// output). A plain static lands in internal SRAM (.bss), NOT PSRAM, so the
// memcpy below stays fast. s_send_done is a "buffer free" token: it is taken
// before filling the buffer and given back when the httpd task finishes.
static ws_send_work_t s_send_work;
static SemaphoreHandle_t s_send_done = NULL;

// ===== Ping/Pong keepalive =====
// Liveness state lives here (near the top) because C requires it be declared
// before its first use in ws_handler() and websocket_send_work() below. The
// rest of the keepalive logic is grouped under the matching "Ping/Pong
// keepalive" banner lower in this file (websocket_server_send_ping), plus the
// PONG-reset points inside ws_handler.
//
// Count pings sent but not yet answered by a PONG. Reset to 0 whenever a PONG
// arrives. If it reaches MAX_PING_FAILURES the round trip has gone unanswered
// for that many ping cycles and the connection is treated as dead.
static int s_pings_unanswered = 0;
#define MAX_PING_FAILURES 3
// ===== End Ping/Pong keepalive (state) =====

// External callbacks from websocket_console.c
extern void websocket_console_handle_rx(const uint8_t* data, size_t len);
extern void websocket_console_on_connect(void);
extern void websocket_console_on_disconnect(void);

// Server handle
static httpd_handle_t s_server = NULL;

// Single WebSocket client fd (-1 = no client)
static volatile int s_client_fd = -1;

/**
 * @brief Mark the tracked WebSocket client disconnected.
 *
 * Once the terminal is gone, emulator characters should be dropped by
 * websocket_console_enqueue_output() instead of queued for a dead socket.
 */
static void mark_client_disconnected(int fd, const char* reason)
{
    if (fd >= 0 && fd == s_client_fd) {
        s_client_fd = -1;
        s_pings_unanswered = 0;
        ESP_LOGI(TAG, "WebSocket client disconnected (fd=%d, %s)", fd, reason);
        websocket_console_on_disconnect();
    }
}

/**
 * @brief HTTP handler for root path - serves terminal HTML
 */
static esp_err_t root_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char*)static_html_gz, static_html_gz_len);
}

/**
 * @brief Post-handshake callback for the WebSocket URI.
 *
 * In esp_http_server the WebSocket handshake (the HTTP GET upgrade) is consumed
 * internally and the URI handler is NEVER invoked for it - the handler only
 * runs for subsequent data frames. Connection detection must therefore happen
 * here, immediately after the handshake completes, while we still have the
 * request (and thus the socket fd) for the freshly-connected client. This
 * requires CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y.
 */
static esp_err_t ws_post_handshake_cb(httpd_req_t* req)
{
    int new_fd = httpd_req_to_sockfd(req);
    int old_fd = s_client_fd;

    // Kick existing client if any (new connection takes over).
    if (old_fd >= 0 && old_fd != new_fd) {
        s_client_fd = -1;  // Clear before triggering close
        httpd_sess_trigger_close(s_server, old_fd);
    }

    // Accept new client.
    s_client_fd = new_fd;
    ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", new_fd);
    websocket_console_on_connect();

    // Set TCP_NODELAY for low latency.
    int nodelay = 1;
    setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    return ESP_OK;
}

/**
 * @brief WebSocket handler for terminal I/O (data frames only).
 */
static esp_err_t ws_handler(httpd_req_t* req)
{
    int frame_fd = httpd_req_to_sockfd(req);
    if (frame_fd >= 0 && frame_fd != s_client_fd) {
        ESP_LOGW(TAG, "Refreshing WebSocket client fd: %d -> %d", s_client_fd, frame_fd);
        s_client_fd = frame_fd;
    }

    // Handle WebSocket frames
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // First call with max_len=0 to get frame info
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        // ESP_ERR_INVALID_STATE / ESP_FAIL / ESP_ERR_TIMEOUT just mean the peer
        // went away (browser tab closed, AP roam, TCP reset, idle timeout).
        // Log quietly and let httpd tear down the session.
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL || ret == ESP_ERR_TIMEOUT) {
            ESP_LOGI(TAG, "WS client gone (%s), closing session", esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %d (%s)", ret, esp_err_to_name(ret));
        }
        mark_client_disconnected(httpd_req_to_sockfd(req), "recv_frame failed");
        return ret;
    }

    // Handle control frames with no payload
    if (ws_pkt.len == 0) {
        if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
            // ===== Ping/Pong keepalive ===== Keepalive reply - round trip done.
            s_pings_unanswered = 0;
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            // Send close response, server will close session
            httpd_ws_frame_t close_pkt = {
                .final = true,
                .type = HTTPD_WS_TYPE_CLOSE,
                .payload = NULL,
                .len = 0
            };
            httpd_ws_send_frame(req, &close_pkt);
        }
        return ESP_OK;
    }

    // Read the payload into a stack buffer for the common case (keystrokes and
    // small pastes); fall back to heap only for a rare oversized frame. This
    // keeps the steady-state RX path allocation-free.
    uint8_t stackbuf[256];
    uint8_t* buf;
    bool heap_buf = false;
    if (ws_pkt.len < sizeof(stackbuf)) {
        buf = stackbuf;
    } else {
        buf = malloc(ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            return ESP_ERR_NO_MEM;
        }
        heap_buf = true;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL || ret == ESP_ERR_TIMEOUT) {
            ESP_LOGI(TAG, "WS client gone during payload read (%s)", esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "httpd_ws_recv_frame payload failed: %d (%s)", ret, esp_err_to_name(ret));
        }
        mark_client_disconnected(httpd_req_to_sockfd(req), "payload recv failed");
        if (heap_buf) {
            free(buf);
        }
        return ret;
    }

    // Process frame
    switch (ws_pkt.type) {
        case HTTPD_WS_TYPE_TEXT:
        case HTTPD_WS_TYPE_BINARY:
            websocket_console_handle_rx(ws_pkt.payload, ws_pkt.len);
            break;

        case HTTPD_WS_TYPE_PONG:
            // ===== Ping/Pong keepalive ===== reply (echoed payload) - round trip done.
            s_pings_unanswered = 0;
            break;

        case HTTPD_WS_TYPE_CLOSE: {
            httpd_ws_frame_t close_pkt = {
                .final = true,
                .type = HTTPD_WS_TYPE_CLOSE,
                .payload = NULL,
                .len = 0
            };
            httpd_ws_send_frame(req, &close_pkt);
            break;
        }

        default:
            break;
    }

    if (heap_buf) {
        free(buf);
    }
    return ESP_OK;
}

/**
 * @brief Socket open callback - configure socket options
 */
static esp_err_t socket_open_callback(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    // SO_LINGER with zero timeout: RST on close, bypasses TIME_WAIT
    struct linger so_linger = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
    
    // Increase socket send buffer for high-throughput WebSocket
    int sndbuf = 16384;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    return ESP_OK;
}

/**
 * @brief Socket close callback - clear tracked client and queues.
 */
static void socket_close_callback(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    mark_client_disconnected(sockfd, "socket closed");
    close(sockfd);
}

bool websocket_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }

    s_client_fd = -1;

    // Create the "buffer free" token for the single static send buffer and
    // start it available (no send in flight yet).
    if (!s_send_done) {
        s_send_done = xSemaphoreCreateBinary();
        if (!s_send_done) {
            ESP_LOGE(TAG, "Failed to create send-done semaphore");
            return false;
        }
        xSemaphoreGive(s_send_done);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEBSOCKET_SERVER_PORT;
    config.ctrl_port = WEBSOCKET_SERVER_PORT + 1;
    config.task_priority = tskIDLE_PRIORITY + 12;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.max_open_sockets = 4;     // Minimal: 1 WS + 1 HTTP + headroom
    config.backlog_conn = 2;
    config.lru_purge_enable = true;
    config.open_fn = socket_open_callback;
    config.close_fn = socket_close_callback;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;   // Allow more time for congested links
    config.keep_alive_enable = false;

    ESP_LOGI(TAG, "Starting server on port %d", WEBSOCKET_SERVER_PORT);

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return false;
    }

    // Register handlers
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .is_websocket = false
    };
    httpd_register_uri_handler(s_server, &root_uri);

    const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .ws_post_handshake_cb = ws_post_handshake_cb
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    ESP_LOGI(TAG, "WebSocket server started");
    return true;
}

void websocket_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_client_fd = -1;
        ESP_LOGI(TAG, "Server stopped");
    }
    if (s_send_done) {
        vSemaphoreDelete(s_send_done);
        s_send_done = NULL;
    }
}

uint32_t websocket_server_get_client_count(void)
{
    return (s_client_fd >= 0) ? 1 : 0;
}

static void websocket_send_work(void *arg)
{
    ws_send_work_t *work = (ws_send_work_t *)arg;

    if (!work) {
        return;
    }

    if (s_server && work->fd >= 0 && work->fd == s_client_fd) {
        httpd_ws_frame_t ws_pkt = {
            .final = true,
            .fragmented = false,
            .type = work->is_ping ? HTTPD_WS_TYPE_PING : HTTPD_WS_TYPE_BINARY,
            .payload = work->is_ping ? NULL : work->data,
            .len = work->is_ping ? 0 : work->len
        };

        if (work->is_ping || work->len > 0) {
            esp_err_t ret = httpd_ws_send_frame_async(s_server, work->fd, &ws_pkt);
            if (ret != ESP_OK) {
                if (work->is_ping) {
                    // Could not even enqueue the ping - count it as unanswered.
                    s_pings_unanswered++;
                    if (s_pings_unanswered >= MAX_PING_FAILURES) {
                        ESP_LOGW(TAG, "Connection dead after %d unanswered pings", s_pings_unanswered);
                        mark_client_disconnected(work->fd, "ping failed");
                        httpd_sess_trigger_close(s_server, work->fd);
                    }
                } else {
                    ESP_LOGW(TAG, "WebSocket send failed: %s", esp_err_to_name(ret));
                    mark_client_disconnected(work->fd, "send failed");
                    httpd_sess_trigger_close(s_server, work->fd);
                }
            } else if (work->is_ping) {
                // Ping is on the wire; await a PONG. If too many go unanswered
                // the link is dead even though the local enqueue succeeded.
                s_pings_unanswered++;
                if (s_pings_unanswered >= MAX_PING_FAILURES) {
                    ESP_LOGW(TAG, "Connection dead after %d unanswered pings", s_pings_unanswered);
                    mark_client_disconnected(work->fd, "pong timeout");
                    httpd_sess_trigger_close(s_server, work->fd);
                }
            }
        }
    }

    // Release the shared buffer for the next broadcast or ping.
    if (s_send_done) {
        xSemaphoreGive(s_send_done);
    }
}

bool websocket_server_broadcast(const uint8_t* data, size_t len)
{
    int fd = s_client_fd;
    if (!s_server || fd < 0 || !data || len == 0 || !s_send_done) {
        return false;
    }

    if (len > WS_SEND_BUF_SIZE) {
        len = WS_SEND_BUF_SIZE;
    }

    // Wait for the previous send to complete so the static buffer can be reused.
    // The bounded wait provides backpressure to the (single) TX task without
    // letting a stalled httpd task or dead client block it forever.
    if (xSemaphoreTake(s_send_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;  // previous send still in flight - drop this chunk
    }

    s_send_work.fd = fd;
    s_send_work.len = len;
    s_send_work.is_ping = false;
    memcpy(s_send_work.data, data, len);

    esp_err_t ret = httpd_queue_work(s_server, websocket_send_work, &s_send_work);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue WebSocket send: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_send_done);  // nothing queued - release buffer
        return false;
    }

    return true;
}

// ===== Ping/Pong keepalive =====
// Periodic liveness check. websocket_console.c's TX task calls this on a timer.
// The unanswered-ping accounting that decides when the link is dead lives in
// websocket_send_work() (shared with the data path); the PONG that resets the
// counter is handled in ws_handler(); the counter itself is declared under the
// matching banner at the top of this file.
void websocket_server_send_ping(void)
{
    int fd = s_client_fd;
    if (!s_server || fd < 0 || !s_send_done) {
        return;
    }

    // Route the ping through the SAME single-in-flight token and httpd work
    // queue as data sends. Otherwise the ping (sent on the TX task) could run
    // httpd_ws_send_frame_async concurrently with an in-flight broadcast on the
    // httpd task, interleaving bytes on the same socket and corrupting frames.
    if (xSemaphoreTake(s_send_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;  // a send is still in flight - skip this ping cycle
    }

    s_send_work.fd = fd;
    s_send_work.len = 0;
    s_send_work.is_ping = true;

    esp_err_t ret = httpd_queue_work(s_server, websocket_send_work, &s_send_work);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue WebSocket ping: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_send_done);  // nothing queued - release buffer
    }
}
// ===== End Ping/Pong keepalive =====
