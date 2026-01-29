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
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"

// Terminal HTML (gzip compressed)
#include "static_html_hex.h"

static const char* TAG = "WS_Server";

// External callbacks from websocket_console.c
extern void websocket_console_handle_rx(const uint8_t* data, size_t len);
extern void websocket_console_on_connect(void);
extern void websocket_console_on_disconnect(void);

// Server handle
static httpd_handle_t s_server = NULL;

// Single WebSocket client fd (-1 = no client)
static volatile int s_client_fd = -1;

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
 * @brief WebSocket handler for terminal I/O
 */
static esp_err_t ws_handler(httpd_req_t* req)
{
    // Handle WebSocket handshake
    if (req->method == HTTP_GET) {
        int new_fd = httpd_req_to_sockfd(req);
        int old_fd = s_client_fd;

        // Kick existing client if any (new connection takes over)
        if (old_fd >= 0 && old_fd != new_fd) {
            s_client_fd = -1;  // Clear before triggering close
            httpd_sess_trigger_close(s_server, old_fd);
        }

        // Accept new client
        s_client_fd = new_fd;
        websocket_console_on_connect();

        // Set TCP_NODELAY for low latency
        int nodelay = 1;
        setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        return ESP_OK;
    }

    // Handle WebSocket frames
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // First call with max_len=0 to get frame info
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %d", ret);
        return ret;
    }

    // Handle control frames with no payload
    if (ws_pkt.len == 0) {
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
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

    // Allocate buffer for payload
    uint8_t* buf = malloc(ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame payload failed: %d", ret);
        free(buf);
        return ret;
    }

    // Process frame
    switch (ws_pkt.type) {
        case HTTPD_WS_TYPE_TEXT:
        case HTTPD_WS_TYPE_BINARY:
            websocket_console_handle_rx(ws_pkt.payload, ws_pkt.len);
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

    free(buf);
    return ESP_OK;
}

/**
 * @brief Socket open callback - configure socket options
 */
static esp_err_t socket_open_callback(httpd_handle_t hd, int sockfd)
{
    // SO_LINGER with zero timeout: RST on close, bypasses TIME_WAIT
    struct linger so_linger = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
    return ESP_OK;
}

/**
 * @brief Session close callback - cleanup on disconnect
 * 
 * IMPORTANT: With custom close_fn, we must call lwip_close() ourselves.
 * See esp_http_server.h documentation.
 */
static void session_close_callback(httpd_handle_t hd, int sockfd)
{
    // Only notify if this was our active WebSocket client
    if (sockfd == s_client_fd) {
        s_client_fd = -1;
        websocket_console_on_disconnect();
    }
    
    lwip_close(sockfd);
}

bool websocket_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }

    s_client_fd = -1;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEBSOCKET_SERVER_PORT;
    config.ctrl_port = WEBSOCKET_SERVER_PORT + 1;
    config.max_open_sockets = 4;     // Minimal: 1 WS + 1 HTTP + headroom
    config.backlog_conn = 2;
    config.lru_purge_enable = true;
    config.open_fn = socket_open_callback;
    config.close_fn = session_close_callback;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
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
        .handle_ws_control_frames = true
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
}

bool websocket_server_is_running(void)
{
    return s_server != NULL;
}

uint32_t websocket_server_get_client_count(void)
{
    return (s_client_fd >= 0) ? 1 : 0;
}

bool websocket_server_broadcast(const uint8_t* data, size_t len)
{
    int fd = s_client_fd;
    if (!s_server || fd < 0 || !data || len == 0) {
        return false;
    }

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t*)data,
        .len = len
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, fd, &ws_pkt);
    if (ret != ESP_OK) {
        // Client gone, trigger cleanup
        httpd_sess_trigger_close(s_server, fd);
        return false;
    }
    return true;
}

void websocket_server_send_ping(void)
{
    int fd = s_client_fd;
    if (!s_server || fd < 0) {
        return;
    }

    httpd_ws_frame_t ping_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_PING,
        .payload = NULL,
        .len = 0
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, fd, &ping_pkt);
    if (ret != ESP_OK) {
        httpd_sess_trigger_close(s_server, fd);
    }
}
