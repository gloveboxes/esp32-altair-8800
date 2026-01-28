#include "remote_fs.h"

#include "pico/stdlib.h"

// Remote FS is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "wifi.h"
#include "pico/util/queue.h"

// Server port (IP is configured via config module at runtime)
#ifndef RFS_SERVER_PORT
#define RFS_SERVER_PORT 8085
#endif

// Cache configuration - defined as number of tracks, actual bytes calculated from entry size
#if defined(PICO_RP2040)
// Pico W has 264KB RAM - cache 12 tracks
#define RFS_CACHE_NUM_TRACKS 12
#else
// RP2350 (Pico 2 W) has 520KB RAM - cache 33 tracks
#define RFS_CACHE_NUM_TRACKS 33
#endif

// Queue sizes
#define RFS_OUTBOUND_QUEUE_SIZE 4
#define RFS_INBOUND_QUEUE_SIZE 1

// Receive buffer size (status + track data for largest response)
#define RFS_RECV_BUF_SIZE (1 + RFS_TRACK_SIZE)

// Connection timeout (ms)
#define RFS_CONNECT_TIMEOUT_MS 5000 // Reduced for faster failure detection
#define RFS_OPERATION_TIMEOUT_MS 8000 // Reduced for faster retry
#define RFS_MAX_RETRIES 20
#define RFS_RECONNECT_DELAY_MS 500 // Minimal delay for immediate retry

// Queues for inter-core communication
static queue_t rfs_outbound_queue; // Core 0 -> Core 1
static queue_t rfs_inbound_queue;  // Core 1 -> Core 0

// Client state
static struct
{
    rfs_client_state_t state;
    struct tcp_pcb* pcb;

    // Current operation
    rfs_request_t current_request;
    bool request_in_progress;

    // Receive buffer
    uint8_t recv_buf[RFS_RECV_BUF_SIZE];
    size_t recv_len;
    size_t expected_len;

    // Timing
    uint32_t operation_start_time;

    // Auto-recovery
    bool has_pending_retry;
    rfs_request_t pending_retry_request;
    uint8_t retry_count;
    uint32_t reconnect_start_time;
} client;

// Static buffer for write requests to avoid stack allocation
static uint8_t rfs_write_buf[4 + RFS_SECTOR_SIZE];

// ============================================================================
// Track Cache - Write-through cache storing full tracks for spatial locality
// ============================================================================

// Cache configuration
// (Defined at top of file based on platform)

// Cache entry structure - stores entire track (4392 bytes each)
typedef struct
{
    uint8_t drive;
    uint8_t track;
    uint8_t valid; // 1 if entry contains valid data
    uint8_t _pad;  // Alignment padding
    uint32_t age;  // LRU counter - higher = more recently used
    uint8_t data[RFS_TRACK_SIZE]; // Full track data (32 sectors × 137 bytes)
} rfs_cache_entry_t;

#define RFS_CACHE_ENTRY_SIZE (sizeof(rfs_cache_entry_t))
#define RFS_CACHE_NUM_ENTRIES RFS_CACHE_NUM_TRACKS
#define RFS_CACHE_SIZE_BYTES (RFS_CACHE_NUM_ENTRIES * RFS_CACHE_ENTRY_SIZE)

// Static cache pool - shared across all drives
static rfs_cache_entry_t rfs_cache[RFS_CACHE_NUM_ENTRIES];
static uint32_t rfs_cache_age_counter = 0;
static uint32_t rfs_cache_hits = 0;
static uint32_t rfs_cache_misses = 0;
static uint32_t rfs_cache_write_skips = 0; // Writes skipped due to identical data

// Find track entry in cache (returns NULL if not found)
static rfs_cache_entry_t* rfs_cache_find_track(uint8_t drive, uint8_t track)
{
    for (size_t i = 0; i < RFS_CACHE_NUM_ENTRIES; i++)
    {
        if (rfs_cache[i].valid && rfs_cache[i].drive == drive && rfs_cache[i].track == track)
        {
            // Update LRU age
            rfs_cache[i].age = ++rfs_cache_age_counter;
            return &rfs_cache[i];
        }
    }
    return NULL;
}

// Find LRU entry to evict (or first empty slot)
static rfs_cache_entry_t* rfs_cache_find_lru(void)
{
    rfs_cache_entry_t* lru = &rfs_cache[0];

    for (size_t i = 0; i < RFS_CACHE_NUM_ENTRIES; i++)
    {
        // Return first empty slot
        if (!rfs_cache[i].valid)
        {
            return &rfs_cache[i];
        }
        // Track least recently used
        if (rfs_cache[i].age < lru->age)
        {
            lru = &rfs_cache[i];
        }
    }
    return lru;
}

// Add or update track entry in cache
static void rfs_cache_put_track(uint8_t drive, uint8_t track, const uint8_t* track_data)
{
    // Check if track already in cache
    rfs_cache_entry_t* entry = rfs_cache_find_track(drive, track);

    if (!entry)
    {
        // Find slot for new entry (LRU eviction)
        entry = rfs_cache_find_lru();
    }

    entry->drive = drive;
    entry->track = track;
    entry->valid = 1;
    entry->age = ++rfs_cache_age_counter;
    memcpy(entry->data, track_data, RFS_TRACK_SIZE);
}

// Update a single sector within a cached track
static void rfs_cache_put_sector(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* sector_data)
{
    rfs_cache_entry_t* entry = rfs_cache_find_track(drive, track);

    if (entry)
    {
        // Track is cached - update just the sector
        size_t offset = sector * RFS_SECTOR_SIZE;
        memcpy(&entry->data[offset], sector_data, RFS_SECTOR_SIZE);
        entry->age = ++rfs_cache_age_counter;
    }
    // If track not cached, don't cache single sector writes (wait for full track read)
}

// Get cached sector data (returns true if hit)
static bool rfs_cache_get_sector(uint8_t drive, uint8_t track, uint8_t sector, uint8_t* sector_data)
{
    rfs_cache_entry_t* entry = rfs_cache_find_track(drive, track);

    if (entry)
    {
        size_t offset = sector * RFS_SECTOR_SIZE;
        memcpy(sector_data, &entry->data[offset], RFS_SECTOR_SIZE);
        rfs_cache_hits++;
        return true;
    }

    rfs_cache_misses++;
    return false;
}

// Compare sector data with cached version (returns true if identical)
static bool rfs_cache_compare_sector(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* sector_data)
{
    rfs_cache_entry_t* entry = rfs_cache_find_track(drive, track);

    if (entry)
    {
        size_t offset = sector * RFS_SECTOR_SIZE;
        return memcmp(&entry->data[offset], sector_data, RFS_SECTOR_SIZE) == 0;
    }
    return false; // Not in cache = can't compare = must write
}

// Initialize cache
static void rfs_cache_init(void)
{
    memset(rfs_cache, 0, sizeof(rfs_cache));
    rfs_cache_age_counter = 0;
    rfs_cache_hits = 0;
    rfs_cache_misses = 0;
    printf("[RFS_CACHE] Initialized %u track entries (%u KB)\n", (unsigned)RFS_CACHE_NUM_ENTRIES,
           (unsigned)(RFS_CACHE_SIZE_BYTES / 1024));
}

// Clear/invalidate entire cache (called on system reset)
void rfs_cache_clear(void)
{
    for (size_t i = 0; i < RFS_CACHE_NUM_ENTRIES; i++)
    {
        rfs_cache[i].valid = 0;
    }
    rfs_cache_age_counter = 0;
    printf("[RFS_CACHE] Cache cleared\n");
}

// Print cache statistics
void rfs_cache_print_stats(void)
{
    uint32_t total = rfs_cache_hits + rfs_cache_misses;
    if (total > 0)
    {
        printf("[RFS_CACHE] Hits: %u, Misses: %u, Hit rate: %u%%\n", (unsigned)rfs_cache_hits,
               (unsigned)rfs_cache_misses, (unsigned)((rfs_cache_hits * 100) / total));
    }
}

// Forward declarations
static err_t rfs_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err);
static err_t rfs_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
static void rfs_tcp_err_cb(void* arg, err_t err);
static err_t rfs_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len);

static void rfs_start_connect(void);
static void rfs_send_init(void);
static err_t rfs_send_read_request(const rfs_request_t* req);
static err_t rfs_send_track_request(const rfs_request_t* req);
static err_t rfs_send_write_request(const rfs_request_t* req);
static void rfs_handle_response(void);
static void rfs_set_error(const char* msg);
static void rfs_attempt_reconnect(void);

// ============================================================================
// Initialization
// ============================================================================

void rfs_client_init(void)
{
    // Initialize queues
    queue_init(&rfs_outbound_queue, sizeof(rfs_request_t), RFS_OUTBOUND_QUEUE_SIZE);
    queue_init(&rfs_inbound_queue, sizeof(rfs_response_t), RFS_INBOUND_QUEUE_SIZE);

    // Initialize state
    memset(&client, 0, sizeof(client));
    client.state = RFS_STATE_DISCONNECTED;

    // Initialize cache
    rfs_cache_init();

}

// ============================================================================
// Core 1 Poll Function
// ============================================================================

void rfs_client_poll(void)
{
    // Check for timeout on operations
    if (client.request_in_progress)
    {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.operation_start_time;
        uint32_t timeout = (client.state == RFS_STATE_CONNECTING) ? RFS_CONNECT_TIMEOUT_MS : RFS_OPERATION_TIMEOUT_MS;

        if (elapsed > timeout)
        {
            printf("[RFS] Operation timeout\n");
            rfs_attempt_reconnect();
            return;
        }
    }

    // State machine
    switch (client.state)
    {
        case RFS_STATE_DISCONNECTED:
        {
            // Check for connect request
            rfs_request_t request;
            if (queue_try_peek(&rfs_outbound_queue, &request))
            {
                // Auto-connect on ANY request (Connect, Read, or Write)
                printf("[RFS] pending request op=%u, connecting...\n", request.op);

                // If it's explicitly a CONNECT request, we can consume it now.
                // For READ/WRITE, leave it in queue to be processed after connection.
                if (request.op == RFS_OP_CONNECT)
                {
                    queue_try_remove(&rfs_outbound_queue, &request);
                }

                rfs_start_connect();
            }
            break;
        }

        case RFS_STATE_CONNECTING:
            // Waiting for TCP connection callback
            break;

        case RFS_STATE_CONNECTED:
            // Send INIT command
            rfs_send_init();
            break;

        case RFS_STATE_INIT_SENT:
            // Waiting for INIT response
            break;

        case RFS_STATE_READY:
        {
            // Ready for operations - simple pop-and-send (strict request-response protocol)
            if (!client.request_in_progress)
            {
                rfs_request_t request;
                // Peek first to handle backpressure - only pop if send succeeds
                if (queue_try_peek(&rfs_outbound_queue, &request))
                {
                    err_t err = ERR_OK;
                    size_t expected = 0;

                    switch (request.op)
                    {
                        case RFS_OP_READ:
                            expected = 1 + RFS_SECTOR_SIZE; // status + data (legacy, not used)
                            err = rfs_send_read_request(&request);
                            break;

                        case RFS_OP_READ_TRACK:
                            expected = 1 + RFS_TRACK_SIZE; // status + track data (4385 bytes)
                            err = rfs_send_track_request(&request);
                            break;

                        case RFS_OP_WRITE:
                            expected = 1; // status only
                            err = rfs_send_write_request(&request);
                            break;

                        default:
                            // Unknown op, just remove it
                            queue_try_remove(&rfs_outbound_queue, &request);
                            break;
                    }

                    if (err == ERR_OK)
                    {
                        // Success - remove from request queue
                        queue_try_remove(&rfs_outbound_queue, &request);

                        if (request.op == RFS_OP_WRITE)
                        {
                            // Async Write: Assume success immediately (Fire-and-forget)
                            rfs_response_t response;
                            response.op = RFS_OP_WRITE;
                            response.status = RFS_RESP_OK;
                            queue_try_add(&rfs_inbound_queue, &response);

                            // Don't wait for network response - ready for next request immediately
                        }
                        else
                        {
                            // Read: Must wait for response
                            client.current_request = request;
                            client.expected_len = expected;
                            client.request_in_progress = true;
                            client.recv_len = 0;
                            client.operation_start_time = to_ms_since_boot(get_absolute_time());
                        }
                    }
                    else if (err == ERR_MEM)
                    {
                        // Backpressure: TCP buffer full, leave in queue, try again next poll
                        // (no action needed - request stays in queue)
                    }
                    else
                    {
                        // Actual error - trigger reconnect
                        queue_try_remove(&rfs_outbound_queue, &request);
                        client.current_request = request;
                        rfs_attempt_reconnect();
                    }
                }
            }
            break;
        }

        case RFS_STATE_RECONNECTING:
        {
            // Wait for reconnect delay, then attempt reconnect
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.reconnect_start_time;
            if (elapsed >= RFS_RECONNECT_DELAY_MS)
            {
                printf("[RFS] Reconnect attempt %d/%d\\n", client.retry_count, RFS_MAX_RETRIES);
                rfs_start_connect();
            }
            break;
        }

        case RFS_STATE_ERROR:
            // Fatal error state - no recovery (max retries exceeded)
            break;
    }
}

// ============================================================================
// TCP Connection
// ============================================================================

static void rfs_start_connect(void)
{
    const char* server_ip = config_get_rfs_ip();

    // Check if RFS IP is configured
    if (server_ip[0] == '\0')
    {
        rfs_set_error("RFS server IP not configured - use serial console to configure");
        return;
    }

    printf("[RFS] Connecting to %s:%d\n", server_ip, RFS_SERVER_PORT);

    // Parse IP address
    ip_addr_t server_addr;
    if (!ip4addr_aton(server_ip, &server_addr))
    {
        rfs_set_error("Invalid server IP address");
        return;
    }

    // Create TCP PCB
    client.pcb = tcp_new();
    if (client.pcb == NULL)
    {
        rfs_set_error("Failed to create TCP PCB");
        return;
    }

    // Set callbacks
    tcp_arg(client.pcb, NULL);
    tcp_err(client.pcb, rfs_tcp_err_cb);
    tcp_recv(client.pcb, rfs_tcp_recv_cb);
    tcp_sent(client.pcb, rfs_tcp_sent_cb);

    // Start connection
    client.state = RFS_STATE_CONNECTING;
    client.request_in_progress = true;
    client.operation_start_time = to_ms_since_boot(get_absolute_time());

    // Disable Nagle's algorithm for lower latency on small packets
    tcp_nagle_disable(client.pcb);

    err_t err = tcp_connect(client.pcb, &server_addr, RFS_SERVER_PORT, rfs_tcp_connected_cb);
    if (err != ERR_OK)
    {
        rfs_set_error("TCP connect failed");
        return;
    }
}

static err_t rfs_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    (void)arg;
    (void)tpcb;

    if (err != ERR_OK)
    {
        rfs_set_error("Connection failed");
        return err;
    }

    printf("[RFS] Connected to server\n");
    client.state = RFS_STATE_CONNECTED;
    client.request_in_progress = false;

    return ERR_OK;
}

static void rfs_tcp_err_cb(void* arg, err_t err)
{
    (void)arg;
    printf("[RFS] TCP error: %d\n", err);

    client.pcb = NULL; // PCB already freed by lwIP

    // Attempt to reconnect instead of fatal error
    if (client.request_in_progress)
    {
        rfs_attempt_reconnect();
    }
    else
    {
        // No pending request, just go to disconnected state
        client.state = RFS_STATE_DISCONNECTED;
    }
}

static err_t rfs_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    (void)arg;
    (void)tpcb;
    (void)len;
    return ERR_OK;
}

// ============================================================================
// TCP Receive
// ============================================================================

static err_t rfs_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    (void)arg;

    if (err != ERR_OK)
    {
        if (p != NULL)
            pbuf_free(p);
        rfs_set_error("Receive error");
        return err;
    }

    if (p == NULL)
    {
        // Connection closed by server
        rfs_set_error("Server closed connection");
        return ERR_OK;
    }

    // Copy data to receive buffer
    size_t copy_len = p->tot_len;
    if (client.recv_len + copy_len > RFS_RECV_BUF_SIZE)
    {
        copy_len = RFS_RECV_BUF_SIZE - client.recv_len;
    }

    pbuf_copy_partial(p, &client.recv_buf[client.recv_len], copy_len, 0);
    client.recv_len += copy_len;

    // ACK received data
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Check if we have complete response
    if (client.recv_len >= client.expected_len)
    {
        rfs_handle_response();
    }

    return ERR_OK;
}

// ============================================================================
// Protocol Commands
// ============================================================================

static void rfs_send_init(void)
{
    if (client.pcb == NULL)
        return;

    const char* ip = wifi_get_ip_address();
    if (ip == NULL || ip[0] == '\0')
    {
        rfs_set_error("Cached IP not available for INIT");
        return;
    }

    size_t ip_len = strlen(ip);
    if (ip_len == 0 || ip_len > 15)
    {
        rfs_set_error("Invalid IP length for INIT");
        return;
    }

    uint8_t buf[1 + 1 + 16];
    buf[0] = RFS_CMD_INIT;
    buf[1] = (uint8_t)ip_len;
    memcpy(&buf[2], ip, ip_len);

    err_t err = tcp_write(client.pcb, buf, 2 + ip_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        rfs_set_error("Failed to send INIT");
        return;
    }

    tcp_output(client.pcb);

    client.state = RFS_STATE_INIT_SENT;
    client.expected_len = 1; // status only
    client.recv_len = 0;
    client.request_in_progress = true;
    client.operation_start_time = to_ms_since_boot(get_absolute_time());
}

static err_t rfs_send_read_request(const rfs_request_t* req)
{
    if (client.pcb == NULL)
        return ERR_CONN;

    // Build request: cmd + drive + track + sector
    uint8_t buf[4];
    buf[0] = RFS_CMD_READ_SECTOR;
    buf[1] = req->drive;
    buf[2] = req->track;
    buf[3] = req->sector;

    err_t err = tcp_write(client.pcb, buf, 4, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        tcp_output(client.pcb);
    }
    return err;
}

static err_t rfs_send_track_request(const rfs_request_t* req)
{
    if (client.pcb == NULL)
        return ERR_CONN;

    // Build request: cmd + drive + track (no sector - reading entire track)
    uint8_t buf[3];
    buf[0] = RFS_CMD_READ_TRACK;
    buf[1] = req->drive;
    buf[2] = req->track;

    err_t err = tcp_write(client.pcb, buf, 3, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        tcp_output(client.pcb);
    }
    return err;
}

static err_t rfs_send_write_request(const rfs_request_t* req)
{
    if (client.pcb == NULL)
        return ERR_CONN;

    // Build request in static buffer: cmd + drive + track + sector + data
    rfs_write_buf[0] = RFS_CMD_WRITE_SECTOR;
    rfs_write_buf[1] = req->drive;
    rfs_write_buf[2] = req->track;
    rfs_write_buf[3] = req->sector;
    memcpy(&rfs_write_buf[4], req->data, RFS_SECTOR_SIZE);

    err_t err = tcp_write(client.pcb, rfs_write_buf, 4 + RFS_SECTOR_SIZE, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        tcp_output(client.pcb);
    }
    return err;
}

static void rfs_handle_response(void)
{
    rfs_response_t response;

    if (client.state == RFS_STATE_INIT_SENT)
    {
        // INIT response
        response.op = RFS_OP_INIT;
        response.status = client.recv_buf[0];

        if (response.status == RFS_RESP_OK)
        {
            printf("[RFS] INIT OK, ready for operations\n");
            client.state = RFS_STATE_READY;
            client.retry_count = 0; // Reset retry counter on success

            // If we have a pending retry request, resend it now
            if (client.has_pending_retry)
            {
                const char* op_name = "UNKNOWN";
                if (client.pending_retry_request.op == RFS_OP_READ || 
                    client.pending_retry_request.op == RFS_OP_READ_TRACK)
                    op_name = "READ";
                else if (client.pending_retry_request.op == RFS_OP_WRITE)
                    op_name = "WRITE";
                    
                printf("[RFS] ✓ Reconnected! Resending failed %s request...\n", op_name);
                client.has_pending_retry = false;
                client.current_request = client.pending_retry_request;
                client.request_in_progress = true;
                client.recv_len = 0;
                client.operation_start_time = to_ms_since_boot(get_absolute_time());

                err_t err = ERR_OK;
                switch (client.current_request.op)
                {
                    case RFS_OP_READ:
                        client.expected_len = 1 + RFS_SECTOR_SIZE;
                        err = rfs_send_read_request(&client.current_request);
                        break;
                    case RFS_OP_READ_TRACK:
                        client.expected_len = 1 + RFS_TRACK_SIZE;
                        err = rfs_send_track_request(&client.current_request);
                        break;
                    case RFS_OP_WRITE:
                        client.expected_len = 1;
                        err = rfs_send_write_request(&client.current_request);
                        break;
                    default:
                        client.request_in_progress = false;
                        break;
                }

                if (err != ERR_OK)
                {
                    rfs_attempt_reconnect();
                }
                return; // Don't send INIT response to Core 0 during retry
            }
        }
        else
        {
            rfs_set_error("INIT failed");
            return;
        }

        // Queue response for Core 0 (only if not retry)
        queue_try_add(&rfs_inbound_queue, &response);
        client.request_in_progress = false;
    }
    else if (client.request_in_progress)
    {
        // READ, READ_TRACK, or WRITE response
        response.op = client.current_request.op;
        response.status = client.recv_buf[0];
        response.drive = client.current_request.drive;
        response.track = client.current_request.track;
        response.sector = client.current_request.sector;

        if (response.op == RFS_OP_READ && response.status == RFS_RESP_OK)
        {
            // Update cache with fetched sector data (legacy, not used with track caching)
            rfs_cache_put_sector(client.current_request.drive, client.current_request.track, 
                                client.current_request.sector, &client.recv_buf[1]);
        }
        else if (response.op == RFS_OP_READ_TRACK && response.status == RFS_RESP_OK)
        {
            // Received entire track - write directly to cache (Core 0 will read from cache)
            rfs_cache_put_track(client.current_request.drive, client.current_request.track, &client.recv_buf[1]);
            
            // Response op should be READ (not READ_TRACK) for Core 0
            response.op = RFS_OP_READ;
        }

        // Queue lightweight notification for Core 0 (no bulk data copy)
        // Core 0 reads data directly from shared cache
        queue_try_add(&rfs_inbound_queue, &response);
        client.request_in_progress = false;
        client.recv_len = 0;

        // Log recovery success if we just completed a retry
        if (client.retry_count > 0 && response.status == RFS_RESP_OK)
        {
            printf("[RFS] ✓ Recovery successful! Operation completed after %d retry(s)\n", client.retry_count);
            client.retry_count = 0;
        }
    }
}

static void rfs_set_error(const char* msg)
{
    printf("[RFS] FATAL ERROR: %s (retries exhausted)\n", msg);

    client.state = RFS_STATE_ERROR;
    client.request_in_progress = false;
    client.has_pending_retry = false;

    // Close connection if open
    if (client.pcb != NULL)
    {
        tcp_abort(client.pcb);
        client.pcb = NULL;
    }

    // Send error response to Core 0
    rfs_response_t response;
    memset(&response, 0, sizeof(response));
    response.op = client.current_request.op;
    response.status = RFS_RESP_ERROR;
    queue_try_add(&rfs_inbound_queue, &response);
}

static void rfs_attempt_reconnect(void)
{
    // Close existing connection
    if (client.pcb != NULL)
    {
        tcp_abort(client.pcb);
        client.pcb = NULL;
    }

    client.retry_count++;

    if (client.retry_count > RFS_MAX_RETRIES)
    {
        rfs_set_error("Max retries exceeded");
        return;
    }

    printf("[RFS] Connection lost, will retry (%d/%d)...\n", client.retry_count, RFS_MAX_RETRIES);

    // Save current request for retry
    client.has_pending_retry = true;
    client.pending_retry_request = client.current_request;
    client.request_in_progress = false;

    // Enter reconnecting state with delay
    client.state = RFS_STATE_RECONNECTING;
    client.reconnect_start_time = to_ms_since_boot(get_absolute_time());
}

// ============================================================================
// Core 0 API
// ============================================================================

bool rfs_client_is_ready(void)
{
    return client.state == RFS_STATE_READY;
}

bool rfs_client_has_error(void)
{
    return client.state == RFS_STATE_ERROR;
}

bool rfs_request_connect(void)
{
    rfs_request_t request = {0};
    request.op = RFS_OP_CONNECT;

    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_try_read_cached(uint8_t drive, uint8_t track, uint8_t sector, uint8_t* data_out)
{
    // Synchronous cache lookup - no queue overhead on cache hit
    return rfs_cache_get_sector(drive, track, sector, data_out);
}

bool rfs_request_read(uint8_t drive, uint8_t track, uint8_t sector)
{
    // Cache hit - no async operation (caller can use rfs_try_read_cached for synchronous access)
    uint8_t dummy_buf[RFS_SECTOR_SIZE];
    if (rfs_cache_get_sector(drive, track, sector, dummy_buf))
    {
        return false; // Cache hit - no async request queued
    }

    // Cache miss - queue track read request (fetches all 32 sectors in one go)
    rfs_request_t request = {.op = RFS_OP_READ_TRACK, .drive = drive, .track = track, .sector = sector};
    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_request_write(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    // Check if data is identical to cached version (skip redundant write)
    if (rfs_cache_compare_sector(drive, track, sector, data))
    {
        rfs_cache_write_skips++;
        return false; // Data unchanged - no async request queued
    }

    // Update cache (write-through: cache updated immediately, request still goes to server)
    rfs_cache_put_sector(drive, track, sector, data);

    rfs_request_t request = {.op = RFS_OP_WRITE, .drive = drive, .track = track, .sector = sector};
    memcpy(request.data, data, RFS_SECTOR_SIZE);

    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_get_response(rfs_response_t* response)
{
    return queue_try_remove(&rfs_inbound_queue, response);
}

bool rfs_request_pending(void)
{
    return client.request_in_progress || !queue_is_empty(&rfs_outbound_queue);
}

void rfs_get_cache_stats(uint32_t* hits, uint32_t* misses, uint32_t* write_skips)
{
    if (hits)
        *hits = rfs_cache_hits;
    if (misses)
        *misses = rfs_cache_misses;
    if (write_skips)
        *write_skips = rfs_cache_write_skips;
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

void rfs_client_init(void) {}
void rfs_client_poll(void) {}
bool rfs_client_is_ready(void)
{
    return false;
}
bool rfs_client_has_error(void)
{
    return true;
}
bool rfs_request_connect(void)
{
    return false;
}
bool rfs_try_read_cached(uint8_t drive, uint8_t track, uint8_t sector, uint8_t* data_out)
{
    (void)drive;
    (void)track;
    (void)sector;
    (void)data_out;
    return false;
}
bool rfs_request_read(uint8_t drive, uint8_t track, uint8_t sector)
{
    (void)drive;
    (void)track;
    (void)sector;
    return false;
}
bool rfs_request_write(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    (void)drive;
    (void)track;
    (void)sector;
    (void)data;
    return false;
}
bool rfs_get_response(rfs_response_t* response)
{
    (void)response;
    return false;
}
bool rfs_request_pending(void)
{
    return false;
}

void rfs_get_cache_stats(uint32_t* hits, uint32_t* misses, uint32_t* write_skips)
{
    if (hits)
        *hits = 0;
    if (misses)
        *misses = 0;
    if (write_skips)
        *write_skips = 0;
}

void rfs_cache_clear(void) {}

#endif // CYW43_WL_GPIO_LED_PIN
