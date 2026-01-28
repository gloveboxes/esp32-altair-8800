/**
 * @file captive_portal.c
 * @brief WiFi AP mode captive portal implementation for ESP32
 *
 * Adapted from Raspberry Pi Pico W implementation to use ESP-IDF APIs.
 * Uses lwIP directly for DNS server and ESP-IDF HTTP server component.
 */

#include "captive_portal.h"
#include "config.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip4_addr.h"

// Include the gzipped config page
#include "config_page_hex.h"

static const char* TAG = "Captive";

// ============================================================================
// DNS Server (Captive Portal - redirect all queries to our IP)
// ============================================================================

static struct udp_pcb* dns_pcb = NULL;

// Our AP IP address
static uint8_t ap_ip[4] = {192, 168, 4, 1};

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static void dns_recv_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                        const ip_addr_t* addr, u16_t port)
{
    (void)arg;

    if (p->tot_len < sizeof(dns_header_t)) {
        pbuf_free(p);
        return;
    }

    // Read the query
    uint8_t query[512];
    size_t query_len = pbuf_copy_partial(p, query, sizeof(query), 0);
    pbuf_free(p);

    if (query_len < sizeof(dns_header_t)) {
        return;
    }

    dns_header_t* hdr = (dns_header_t*)query;

    // Only handle standard queries
    uint16_t flags = lwip_ntohs(hdr->flags);
    if ((flags & 0x8000) != 0) {
        return;  // This is a response, not a query
    }

    // Build response - we'll redirect ALL queries to our IP
    uint8_t response[512];
    memcpy(response, query, query_len);

    dns_header_t* resp_hdr = (dns_header_t*)response;
    resp_hdr->flags = lwip_htons(0x8180);   // Response, No error
    resp_hdr->ancount = lwip_htons(1);      // One answer
    resp_hdr->nscount = 0;
    resp_hdr->arcount = 0;

    // Find the end of the question section
    size_t qname_start = sizeof(dns_header_t);
    size_t pos = qname_start;
    while (pos < query_len && query[pos] != 0) {
        pos += query[pos] + 1;
    }
    pos++;      // Skip null terminator
    pos += 4;   // Skip QTYPE and QCLASS

    // Add answer section
    // Name pointer to question
    response[pos++] = 0xC0;
    response[pos++] = (uint8_t)qname_start;

    // Type A (1)
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // Class IN (1)
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // TTL (60 seconds)
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x3C;

    // RDLENGTH (4 bytes for IPv4)
    response[pos++] = 0x00;
    response[pos++] = 0x04;

    // RDATA (our IP address)
    response[pos++] = ap_ip[0];
    response[pos++] = ap_ip[1];
    response[pos++] = ap_ip[2];
    response[pos++] = ap_ip[3];

    // Send response
    struct pbuf* resp = pbuf_alloc(PBUF_TRANSPORT, pos, PBUF_RAM);
    if (resp) {
        memcpy(resp->payload, response, pos);
        udp_sendto(pcb, resp, addr, port);
        pbuf_free(resp);
    }
}

static bool dns_server_start(void)
{
    dns_pcb = udp_new();
    if (!dns_pcb) {
        ESP_LOGE(TAG, "Failed to create DNS PCB");
        return false;
    }

    err_t err = udp_bind(dns_pcb, IP_ADDR_ANY, CAPTIVE_PORTAL_DNS_PORT);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind DNS server: %d", err);
        udp_remove(dns_pcb);
        dns_pcb = NULL;
        return false;
    }

    udp_recv(dns_pcb, dns_recv_cb, NULL);
    ESP_LOGI(TAG, "DNS server started (captive portal redirect)");
    return true;
}

static void dns_server_stop(void)
{
    if (dns_pcb) {
        udp_remove(dns_pcb);
        dns_pcb = NULL;
    }
}

// ============================================================================
// HTTP Server for Config Page
// ============================================================================

static httpd_handle_t http_server = NULL;
static bool portal_running = false;
static bool reboot_pending = false;
static int64_t reboot_deadline = 0;

// URL decode helper
static void url_decode(char* dst, const char* src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

// Build device info JSON
static size_t build_device_info_json(char* out, size_t out_len)
{
    char id_hex[17];
    config_get_device_id(id_hex, sizeof(id_hex));

    const char* mdns = get_mdns_hostname();

    return (size_t)snprintf(out, out_len,
                            "{\"id\":\"%s\",\"mdns\":\"%s.local\"}",
                            id_hex, mdns ? mdns : "altair-8800");
}

// Handler: Serve config page (gzipped HTML)
static esp_err_t http_get_root_handler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "HTTP GET /");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    return httpd_resp_send(req, (const char*)config_page_gz, config_page_gz_len);
}

// Handler: Device info JSON
static esp_err_t http_get_device_handler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "HTTP GET /device.json");

    char json_body[128];
    size_t json_len = build_device_info_json(json_body, sizeof(json_body));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    return httpd_resp_send(req, json_body, json_len);
}

// Handler: POST /configure
static esp_err_t http_post_configure_handler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "HTTP POST /configure");

    // Read POST body
    char body[256] = {0};
    int content_len = req->content_len;
    if (content_len >= sizeof(body)) {
        content_len = sizeof(body) - 1;
    }

    int received = httpd_req_recv(req, body, content_len);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST body");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    // Parse form data
    char ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
    char password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    char rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};

    const char* p = body;
    while (*p) {
        // Find key
        const char* key_start = p;
        while (*p && *p != '=') p++;
        if (!*p) break;

        size_t key_len = p - key_start;
        p++;  // Skip '='

        // Find value
        const char* val_start = p;
        while (*p && *p != '&') p++;

        size_t val_len = p - val_start;
        if (*p) p++;  // Skip '&'

        // Extract value
        char val_encoded[128];
        if (val_len >= sizeof(val_encoded)) {
            val_len = sizeof(val_encoded) - 1;
        }
        memcpy(val_encoded, val_start, val_len);
        val_encoded[val_len] = '\0';

        // Match keys and decode values
        if (key_len == 4 && strncmp(key_start, "ssid", 4) == 0) {
            url_decode(ssid, val_encoded, sizeof(ssid));
        } else if (key_len == 8 && strncmp(key_start, "password", 8) == 0) {
            url_decode(password, val_encoded, sizeof(password));
        } else if (key_len == 6 && strncmp(key_start, "rfs_ip", 6) == 0) {
            url_decode(rfs_ip, val_encoded, sizeof(rfs_ip));
        }
    }

    ESP_LOGI(TAG, "Received config: SSID='%s', RFS_IP='%s'", ssid, rfs_ip);

    // Validate
    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "Error: SSID is empty");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    // Save configuration
    bool saved = config_save(ssid, password, rfs_ip[0] ? rfs_ip : NULL);
    if (!saved) {
        ESP_LOGE(TAG, "Failed to save configuration");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    // Send success response with device info
    char json_body[128];
    size_t json_len = build_device_info_json(json_body, sizeof(json_body));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_body, json_len);

    // Schedule reboot after a short delay (let response flush)
    ESP_LOGI(TAG, "Configuration saved, rebooting in 2 seconds...");
    reboot_pending = true;
    reboot_deadline = esp_timer_get_time() + (2 * 1000000);  // 2 seconds

    return ESP_OK;
}

// Handler: Redirect all other requests to root (captive portal behavior)
static esp_err_t http_redirect_handler(httpd_req_t* req)
{
    ESP_LOGI(TAG, "HTTP redirect: %s", req->uri);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static bool http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;

    if (httpd_start(&http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    // Register handlers
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_get_root_handler,
    };
    httpd_register_uri_handler(http_server, &uri_root);

    httpd_uri_t uri_index = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = http_get_root_handler,
    };
    httpd_register_uri_handler(http_server, &uri_index);

    httpd_uri_t uri_device = {
        .uri = "/device.json",
        .method = HTTP_GET,
        .handler = http_get_device_handler,
    };
    httpd_register_uri_handler(http_server, &uri_device);

    httpd_uri_t uri_configure = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = http_post_configure_handler,
    };
    httpd_register_uri_handler(http_server, &uri_configure);

    // Wildcard handler for captive portal redirect (must be last)
    httpd_uri_t uri_redirect = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_redirect_handler,
    };
    httpd_register_uri_handler(http_server, &uri_redirect);

    ESP_LOGI(TAG, "HTTP server started on port %d", CAPTIVE_PORTAL_HTTP_PORT);
    return true;
}

static void http_server_stop(void)
{
    if (http_server) {
        httpd_stop(http_server);
        http_server = NULL;
    }
}

// ============================================================================
// Main Captive Portal API
// ============================================================================

bool captive_portal_start(void)
{
    if (portal_running) {
        return true;
    }

    ESP_LOGI(TAG, "Starting captive portal in AP mode...");

    // Start WiFi in AP mode (uses ESP-IDF's built-in DHCP server)
    if (!wifi_start_ap(CAPTIVE_PORTAL_AP_SSID, NULL)) {
        ESP_LOGE(TAG, "Failed to start WiFi AP mode");
        return false;
    }

    ESP_LOGI(TAG, "AP mode enabled: SSID='%s', IP=%s",
             CAPTIVE_PORTAL_AP_SSID, CAPTIVE_PORTAL_IP_ADDR);

    // Start DNS server (for captive portal redirect)
    if (!dns_server_start()) {
        ESP_LOGE(TAG, "Failed to start DNS server");
        wifi_stop_ap();
        return false;
    }

    // Start HTTP server
    if (!http_server_start()) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        dns_server_stop();
        wifi_stop_ap();
        return false;
    }

    portal_running = true;
    ESP_LOGI(TAG, "Captive portal running. Connect to '%s' and open http://%s/",
             CAPTIVE_PORTAL_AP_SSID, CAPTIVE_PORTAL_IP_ADDR);

    return true;
}

void captive_portal_stop(void)
{
    if (!portal_running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping captive portal...");

    http_server_stop();
    dns_server_stop();
    wifi_stop_ap();

    portal_running = false;
    ESP_LOGI(TAG, "Captive portal stopped");
}

bool captive_portal_is_running(void)
{
    return portal_running;
}

void captive_portal_poll(void)
{
    if (reboot_pending && esp_timer_get_time() >= reboot_deadline) {
        reboot_pending = false;
        ESP_LOGI(TAG, "Rebooting now...");
        vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay to flush logs
        esp_restart();
    }
}

const char* captive_portal_get_ip(void)
{
    return CAPTIVE_PORTAL_IP_ADDR;
}
