#ifndef _REMOTE_FS_H_
#define _REMOTE_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Remote FS Client - Non-blocking TCP client for remote disk operations
// Runs on Core 1, communicates with Core 0 via queues
// ============================================================================

// Disk geometry (8" floppy - same as SD card version)
#define RFS_SECTOR_SIZE 137
#define RFS_SECTORS_PER_TRACK 32
#define RFS_TRACK_SIZE (RFS_SECTORS_PER_TRACK * RFS_SECTOR_SIZE)  // 4384 bytes
#define RFS_MAX_TRACKS 77
#define RFS_MAX_DRIVES 4

// Protocol commands (from remote_fs_server.py)
// INIT payload: [len:1][client_ip:len]
#define RFS_CMD_READ_SECTOR 0x01
#define RFS_CMD_WRITE_SECTOR 0x02
#define RFS_CMD_INIT 0x03
#define RFS_CMD_READ_TRACK 0x04

// Response status
#define RFS_RESP_OK 0x00
#define RFS_RESP_ERROR 0xFF

// Client state
typedef enum
{
    RFS_STATE_DISCONNECTED,
    RFS_STATE_CONNECTING,
    RFS_STATE_CONNECTED,
    RFS_STATE_INIT_SENT,
    RFS_STATE_READY,
    RFS_STATE_RECONNECTING,
    RFS_STATE_ERROR
} rfs_client_state_t;

// Operation types
typedef enum
{
    RFS_OP_NONE,
    RFS_OP_CONNECT,
    RFS_OP_INIT,
    RFS_OP_READ,
    RFS_OP_READ_TRACK,
    RFS_OP_WRITE
} rfs_op_type_t;

// Request message (Core 0 -> Core 1)
typedef struct
{
    rfs_op_type_t op;
    uint8_t drive;
    uint8_t track;
    uint8_t sector;
    uint8_t data[RFS_SECTOR_SIZE];
} rfs_request_t;

// Response message (Core 1 -> Core 0) - Lightweight notification only
// Data is read directly from shared cache to avoid 4KB copy overhead
typedef struct
{
    rfs_op_type_t op;
    uint8_t status; // RFS_RESP_OK or RFS_RESP_ERROR
    uint8_t drive;  // For cache lookup
    uint8_t track;  // For cache lookup
    uint8_t sector; // For cache lookup
} rfs_response_t;

// ============================================================================
// Core 1 API (called from core1_io_mgr.c poll loop)
// ============================================================================

/**
 * Initialize remote FS client
 * Creates queues for inter-core communication
 * Must be called before starting Core 1 operations
 */
void rfs_client_init(void);

/**
 * Poll remote FS client
 * Called from Core 1's main loop
 * Handles TCP connection, sends/receives data
 */
void rfs_client_poll(void);

/**
 * Check if client is ready for operations
 * @return true if connected and initialized
 */
bool rfs_client_is_ready(void);

/**
 * Check if client encountered an error
 * @return true if in error state
 */
bool rfs_client_has_error(void);

// ============================================================================
// Core 0 API (called from pico_88dcdd_remote_fs.c)
// ============================================================================

/**
 * Request connection to server
 * Non-blocking - result available via rfs_get_response()
 * @return true if request queued successfully
 */
bool rfs_request_connect(void);

/**
 * Try to read sector from cache (synchronous)
 * Fast path for cache hits - avoids queue overhead
 * @param drive Drive number (0-3)
 * @param track Track number (0-76)
 * @param sector Sector number (0-31)
 * @param data_out Buffer to receive sector data (137 bytes) - must not be NULL
 * @return true if cache hit (data copied to data_out), false if cache miss
 */
bool rfs_try_read_cached(uint8_t drive, uint8_t track, uint8_t sector, uint8_t* data_out);

/**
 * Request sector read from server
 * Non-blocking - result available via rfs_get_response() only on cache miss
 * @param drive Drive number (0-3)
 * @param track Track number (0-76)
 * @param sector Sector number (0-31)
 * @return true if async request queued (cache miss), false if cache hit (use rfs_try_read_cached)
 */
bool rfs_request_read(uint8_t drive, uint8_t track, uint8_t sector);

/**
 * Request sector write to server
 * Non-blocking - result available via rfs_get_response() only on cache miss or data change
 * @param drive Drive number (0-3)
 * @param track Track number (0-76)
 * @param sector Sector number (0-31)
 * @param data Sector data (137 bytes)
 * @return true if async request queued, false if data unchanged (redundant write skipped)
 */
bool rfs_request_write(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data);

/**
 * Check for response from server
 * Non-blocking
 * @param response Pointer to receive response
 * @return true if response available
 */
bool rfs_get_response(rfs_response_t* response);

/**
 * Check if a request is pending
 * @return true if waiting for response
 */
bool rfs_request_pending(void);

/**
 * Get cache statistics
 * @param hits Pointer to receive hit count (can be NULL)
 * @param misses Pointer to receive miss count (can be NULL)
 * @param write_skips Pointer to receive write skip count (can be NULL)
 */
void rfs_get_cache_stats(uint32_t* hits, uint32_t* misses, uint32_t* write_skips);

/**
 * Clear the track cache
 * Call this when resetting the system to ensure fresh data from server
 */
void rfs_cache_clear(void);

#endif // _REMOTE_FS_H_
