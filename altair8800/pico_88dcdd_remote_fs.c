#include "pico_88dcdd_remote_fs.h"
#include "remote_fs.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// MITS 88-DCDD Disk Controller Emulation for Pico with Remote FS
// Implements active-low status bit logic for Altair 8800 floppy disk controller
// Uses TCP to access disk images on remote server
// Note: Sector caching is handled transparently in remote_fs.c

// ============================================================================
// Disk Controller
// ============================================================================

// Global disk controller instance
rfs_disk_controller_t rfs_disk_controller;

static void rfs_writeSector(rfs_disk_t* pDisk);
static void rfs_readSectorFromServer(rfs_disk_t* pDisk);

static const uint8_t RFS_STATUS_DEFAULT =
    RFS_STATUS_ENWD | RFS_STATUS_MOVE_HEAD | RFS_STATUS_HEAD | RFS_STATUS_IE | RFS_STATUS_TRACK_0 | RFS_STATUS_NRDA;

// Set status condition to TRUE (clears bit for active-low hardware)
static inline void rfs_set_status(uint8_t bit)
{
    rfs_disk_controller.current->status &= ~bit;
}

// Set status condition to FALSE (sets bit for active-low hardware)
static inline void rfs_clear_status(uint8_t bit)
{
    rfs_disk_controller.current->status |= bit;
}

// Helper function to handle common track positioning logic
static void rfs_seek_to_track(void)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sectorDirty)
    {
        rfs_writeSector(disk);
    }

    disk->haveSectorData = false;
    disk->sectorPointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void rfs_disk_init(void)
{
    memset(&rfs_disk_controller, 0, sizeof(rfs_disk_controller_t));

    // Initialize all drives
    for (int i = 0; i < RFS_DISK_MAX_DRIVES; i++)
    {
        rfs_disk_controller.disk[i].status = RFS_STATUS_DEFAULT;
        rfs_disk_controller.disk[i].track = 0;
        rfs_disk_controller.disk[i].sector = 0;
        rfs_disk_controller.disk[i].disk_loaded = false;
        rfs_disk_controller.disk[i].op_state = RFS_DISK_OP_IDLE;
    }

    // Select drive 0 by default
    rfs_disk_controller.current = &rfs_disk_controller.disk[0];
    rfs_disk_controller.currentDisk = 0;
    rfs_disk_controller.connected = false;
    rfs_disk_controller.initialized = false;
    // Note: rfs_client_init() is called earlier from main.c before Core 1 starts
}

// Connect to remote server and initialize
bool rfs_disk_connect(void)
{
    printf("[RFS_DISK] Requesting connection to remote server...\n");

    if (!rfs_request_connect())
    {
        printf("[RFS_DISK] Failed to queue connect request\n");
        return false;
    }

    // Wait for INIT response from Core 1 (blocking)
    // This is acceptable during boot before emulation starts
    // IMPORTANT: Only use queue for inter-core communication, never read Core 1's state directly
    uint32_t timeout = 15000; // 15 second timeout
    uint32_t start = to_ms_since_boot(get_absolute_time());

    rfs_response_t response;
    while (true)
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            printf("[RFS_DISK] Connection timeout\n");
            return false;
        }

        // Check queue for INIT response
        if (rfs_get_response(&response))
        {
            printf("[RFS_DISK] Got response: op=%u status=%u\n", response.op, response.status);

            if (response.op == RFS_OP_INIT)
            {
                if (response.status == RFS_RESP_OK)
                {
                    printf("[RFS_DISK] Connected and initialized\n");
                    rfs_disk_controller.connected = true;
                    rfs_disk_controller.initialized = true;

                    // Mark all drives as loaded (server has disk images)
                    for (int i = 0; i < RFS_DISK_MAX_DRIVES; i++)
                    {
                        rfs_disk_controller.disk[i].disk_loaded = true;
                        rfs_disk_controller.disk[i].status = RFS_STATUS_DEFAULT;
                        rfs_disk_controller.disk[i].status &= (uint8_t)~RFS_STATUS_MOVE_HEAD;
                        rfs_disk_controller.disk[i].status &= (uint8_t)~RFS_STATUS_TRACK_0;
                        rfs_disk_controller.disk[i].status &= (uint8_t)~RFS_STATUS_SECTOR;
                    }

                    return true;
                }
                else
                {
                    printf("[RFS_DISK] INIT failed with error status\n");
                    return false;
                }
            }
            else
            {
                printf("[RFS_DISK] Unexpected response op=%u during INIT\n", response.op);
                return false;
            }
        }

        // Wait a bit before checking queue again
        sleep_ms(10);
    }
}

bool rfs_disk_is_ready(void)
{
    return rfs_disk_controller.connected && rfs_disk_controller.initialized;
}

// Poll function - check for async operation completion
void rfs_disk_poll(void)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (disk->op_state == RFS_DISK_OP_IDLE)
    {
        return;
    }

    rfs_response_t response;
    if (!rfs_get_response(&response))
    {
        return; // No response yet
    }

    if (disk->op_state == RFS_DISK_OP_READ_PENDING)
    {
        if (response.status == RFS_RESP_OK)
        {
            // Read data directly from shared cache (no copy through queue)
            if (rfs_try_read_cached(response.drive, response.track, response.sector, disk->sectorData))
            {
                disk->haveSectorData = true;
            }
            else
            {
                // Cache miss (shouldn't happen - Core 1 just wrote it)
                memset(disk->sectorData, 0x00, RFS_DISK_SECTOR_SIZE);
                disk->haveSectorData = false;
            }
        }
        else
        {
            memset(disk->sectorData, 0x00, RFS_DISK_SECTOR_SIZE);
            disk->haveSectorData = false;
        }
        disk->op_state = RFS_DISK_OP_IDLE;
    }
    else if (disk->op_state == RFS_DISK_OP_WRITE_PENDING)
    {
        if (response.status != RFS_RESP_OK)
        {
            printf("[RFS_DISK] Write failed for track %u, sector %u\n", disk->track, disk->sector);
        }
        disk->sectorDirty = false;
        disk->op_state = RFS_DISK_OP_IDLE;
    }
}

// Select disk drive
void rfs_disk_select(uint8_t drive)
{
    uint8_t select = drive & RFS_DISK_DRIVE_SELECT_MASK;

    if (select < RFS_DISK_MAX_DRIVES)
    {
        rfs_disk_controller.currentDisk = select;
        rfs_disk_controller.current = &rfs_disk_controller.disk[select];
    }
    else
    {
        rfs_disk_controller.currentDisk = 0;
        rfs_disk_controller.current = &rfs_disk_controller.disk[0];
    }
}

// Get disk status
uint8_t rfs_disk_status(void)
{
    return rfs_disk_controller.current->status;
}

// Disk control function
// Disk control function
void rfs_disk_function(uint8_t control)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    uint8_t status_before = disk->status;

    // Step in (increase track)
    if (control & RFS_CONTROL_STEP_IN)
    {
        if (disk->track < RFS_DISK_MAX_TRACKS - 1)
        {
            disk->track++;
        }
        if (disk->track != 0)
        {
            rfs_clear_status(RFS_STATUS_TRACK_0);
        }
        rfs_seek_to_track();
    }

    // Step out (decrease track)
    if (control & RFS_CONTROL_STEP_OUT)
    {
        if (disk->track > 0)
        {
            disk->track--;
        }
        if (disk->track == 0)
        {
            rfs_set_status(RFS_STATUS_TRACK_0);
        }
        rfs_seek_to_track();
    }

    // Head load
    if (control & RFS_CONTROL_HEAD_LOAD)
    {
        rfs_set_status(RFS_STATUS_HEAD);
        rfs_set_status(RFS_STATUS_NRDA);
    }

    // Head unload
    if (control & RFS_CONTROL_HEAD_UNLOAD)
    {
        rfs_clear_status(RFS_STATUS_HEAD);
    }

    // Write enable
    if (control & RFS_CONTROL_WE)
    {
        rfs_set_status(RFS_STATUS_ENWD);
        disk->write_status = 0;
    }
}

// Format sector position for reading
uint8_t rfs_disk_sector(void)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0xFF;
    }

    // Wrap sector to 0 after reaching end of track
    if (disk->sector >= RFS_DISK_SECTORS_PER_TRACK)
    {
        disk->sector = 0;
    }

    disk->sectorPointer = 0;
    disk->haveSectorData = false;

    // Format sector number (88-DCDD specification)
    // D7-D6: Always 1
    // D5-D1: Sector number
    // D0: Sector True bit (0 at sector start, 1 otherwise)
    uint8_t ret_val = 0xC0;
    ret_val |= (disk->sector << RFS_DISK_SECTOR_SHIFT_BITS);
    ret_val |= (disk->sectorPointer == 0) ? 0 : 1;

    disk->sector++;
    return ret_val;
}

// Write byte to disk
void rfs_disk_write(uint8_t data)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sectorPointer >= RFS_DISK_SECTOR_SIZE + 2)
    {
        disk->sectorPointer = RFS_DISK_SECTOR_SIZE + 1;
    }

    disk->sectorData[disk->sectorPointer++] = data;
    disk->sectorDirty = true;

    if (disk->write_status == RFS_DISK_SECTOR_SIZE)
    {
        rfs_writeSector(disk);
        disk->write_status = 0;
        rfs_clear_status(RFS_STATUS_ENWD);
    }
    else
    {
        disk->write_status++;
    }
}

// Read byte from disk
uint8_t rfs_disk_read(void)
{
    rfs_disk_t* disk = rfs_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0x00;
    }

    // Load sector data if not already loaded
    if (!disk->haveSectorData)
    {
        disk->sectorPointer = 0;
        memset(disk->sectorData, 0x00, RFS_DISK_SECTOR_SIZE);

        // Fetch from server (cache is handled transparently in remote_fs.c)
        rfs_readSectorFromServer(disk);

        // For now, block until data arrives
        // TODO: Make this non-blocking for better performance
        uint32_t timeout = 25000; // Increased to accommodate Core 1 retries
        uint32_t start = to_ms_since_boot(get_absolute_time());

        while (disk->op_state == RFS_DISK_OP_READ_PENDING)
        {
            if (to_ms_since_boot(get_absolute_time()) - start > timeout)
            {
                printf("[RFS_DISK] Read timeout\n");
                disk->op_state = RFS_DISK_OP_IDLE;
                return 0x00;
            }

            rfs_disk_poll();
            sleep_ms(1);
        }
    }

    return disk->sectorData[disk->sectorPointer++];
}

// Write sector buffer to server
static void rfs_writeSector(rfs_disk_t* pDisk)
{
    if (!pDisk->sectorDirty)
    {
        return;
    }

    uint8_t drive = rfs_disk_controller.currentDisk;
    uint8_t track = pDisk->track;
    uint8_t sector = pDisk->sector > 0 ? pDisk->sector - 1 : 0;

    bool async_queued = rfs_request_write(drive, track, sector, pDisk->sectorData);
    
    if (!async_queued)
    {
        // Data unchanged or queue full - consider write complete
        pDisk->sectorPointer = 0;
        pDisk->sectorDirty = false;
        return;
    }

    pDisk->op_state = RFS_DISK_OP_WRITE_PENDING;

    // Block until write completes
    // TODO: Make this non-blocking
    uint32_t timeout = 25000; // Increased to accommodate Core 1 retries
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (pDisk->op_state == RFS_DISK_OP_WRITE_PENDING)
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            printf("[RFS_DISK] Write timeout\n");
            pDisk->op_state = RFS_DISK_OP_IDLE;
            break;
        }

        rfs_disk_poll();
        sleep_ms(1);
    }

    pDisk->sectorPointer = 0;
    pDisk->sectorDirty = false;
}

// Request sector read from server
static void rfs_readSectorFromServer(rfs_disk_t* pDisk)
{
    uint8_t drive = rfs_disk_controller.currentDisk;
    uint8_t track = pDisk->track;
    uint8_t sector = pDisk->sector > 0 ? pDisk->sector - 1 : 0;

    // Try synchronous cache read first (zero overhead)
    if (rfs_try_read_cached(drive, track, sector, pDisk->sectorData))
    {
        // Cache hit - data immediately available
        pDisk->haveSectorData = true;
        pDisk->op_state = RFS_DISK_OP_IDLE;
        return;
    }

    // Cache miss - queue async request
    if (!rfs_request_read(drive, track, sector))
    {
        // Request failed to queue (shouldn't happen on cache miss)
        pDisk->op_state = RFS_DISK_OP_IDLE;
        return;
    }

    pDisk->op_state = RFS_DISK_OP_READ_PENDING;
}
