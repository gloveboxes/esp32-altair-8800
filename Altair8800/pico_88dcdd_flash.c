#include "pico_88dcdd_flash.h"
#include <stdio.h>
#include <string.h>

// Global disk controller instance
pico_disk_controller_t pico_disk_controller;

// Static patch pool - pre-allocated to avoid heap exhaustion
// 256 patches × ~141 bytes = ~36KB
static sector_patch_t g_patch_pool[PATCH_POOL_SIZE];
static uint16_t g_patch_pool_next_free = 0; // Next index to allocate
static uint16_t g_patch_pool_used = 0;      // Number of patches currently in use
static bool g_patch_pool_exhausted = false; // Set true when pool is full

// Invalid index marker
#define PATCH_INDEX_INVALID 0xFFFF

static inline void set_status(uint8_t bit)
{
    pico_disk_controller.current->status &= ~bit;
}

// Helper: Set status bit to FALSE (set bit for active-low)
static inline void clear_status(uint8_t bit)
{
    pico_disk_controller.current->status |= bit;
}

static const uint8_t STATUS_DEFAULT =
    STATUS_ENWD | STATUS_MOVE_HEAD | STATUS_HEAD | STATUS_IE | STATUS_TRACK_0 | STATUS_NRDA;

// Hash function for sector index (fast bitwise AND for modulo)
static inline uint8_t hash_sector(uint16_t index)
{
    return (uint8_t)(index & (PATCH_HASH_SIZE - 1));
}

// Find a patch in the hash table, returns pool index or PATCH_INDEX_INVALID
static uint16_t find_patch_index(pico_disk_t* disk, uint16_t sector_index)
{
    uint8_t bucket = hash_sector(sector_index);
    uint16_t pool_idx = disk->patch_hash[bucket];

    while (pool_idx != PATCH_INDEX_INVALID)
    {
        if (g_patch_pool[pool_idx].index == sector_index)
        {
            return pool_idx;
        }
        pool_idx = g_patch_pool[pool_idx].next_pool_index;
    }
    return PATCH_INDEX_INVALID;
}

// Allocate a new patch from the static pool
static uint16_t alloc_patch(void)
{
    // Linear scan for a free slot (could optimize with free list if needed)
    for (uint16_t i = 0; i < PATCH_POOL_SIZE; i++)
    {
        uint16_t idx = (g_patch_pool_next_free + i) % PATCH_POOL_SIZE;
        // Check if this slot is unused (index == 0xFFFF means free)
        if (g_patch_pool[idx].index == PATCH_INDEX_INVALID)
        {
            g_patch_pool_next_free = (idx + 1) % PATCH_POOL_SIZE;
            g_patch_pool_used++;
            return idx;
        }
    }

    // Pool exhausted
    if (!g_patch_pool_exhausted)
    {
        g_patch_pool_exhausted = true;
        printf("[DISK] ERROR: Patch pool exhausted (%u/%u). Disk writes will be lost!\n", g_patch_pool_used,
               PATCH_POOL_SIZE);
    }
    return PATCH_INDEX_INVALID;
}

// Get or create a patch for a sector, returns pool index or PATCH_INDEX_INVALID
static uint16_t get_patch(pico_disk_t* disk, uint16_t sector_index)
{
    // First, check if patch already exists
    uint16_t existing = find_patch_index(disk, sector_index);
    if (existing != PATCH_INDEX_INVALID)
    {
        return existing;
    }

    // Allocate new patch from pool
    uint16_t new_idx = alloc_patch();
    if (new_idx == PATCH_INDEX_INVALID)
    {
        return PATCH_INDEX_INVALID;
    }

    // Initialize the patch
    g_patch_pool[new_idx].index = sector_index;
    memset(g_patch_pool[new_idx].data, 0, SECTOR_SIZE);

    // Insert into hash table
    uint8_t bucket = hash_sector(sector_index);
    g_patch_pool[new_idx].next_pool_index = disk->patch_hash[bucket];
    disk->patch_hash[bucket] = new_idx;

    return new_idx;
}

// Clear all patches for a disk (return them to the pool)
static void clear_patches(pico_disk_t* disk)
{
    for (uint8_t i = 0; i < PATCH_HASH_SIZE; i++)
    {
        uint16_t pool_idx = disk->patch_hash[i];
        while (pool_idx != PATCH_INDEX_INVALID)
        {
            uint16_t next = g_patch_pool[pool_idx].next_pool_index;
            // Mark as free by setting index to invalid
            g_patch_pool[pool_idx].index = PATCH_INDEX_INVALID;
            g_patch_pool[pool_idx].next_pool_index = PATCH_INDEX_INVALID;
            g_patch_pool_used--;
            pool_idx = next;
        }
        disk->patch_hash[i] = PATCH_INDEX_INVALID;
    }
    g_patch_pool_exhausted = false; // Pool might have space again
}

static void flush_sector(pico_disk_t* disk)
{
    if (!disk->sector_dirty)
    {
        return;
    }

    uint16_t sector_index = (uint16_t)(disk->disk_pointer / SECTOR_SIZE);
    uint16_t patch_idx = get_patch(disk, sector_index);
    if (patch_idx != PATCH_INDEX_INVALID)
    {
        memcpy(g_patch_pool[patch_idx].data, disk->sector_data, SECTOR_SIZE);
    }
    // Note: if patch allocation failed, data is lost (error already printed)

    disk->sector_dirty = false;
    disk->have_sector_data = false;
    disk->sector_pointer = 0;
}

// Helper: Seek to current track
static void seek_to_track(void)
{
    pico_disk_t* disk = pico_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    flush_sector(disk);

    uint32_t seek_offset = disk->track * TRACK_SIZE;
    disk->disk_pointer = seek_offset;
    disk->have_sector_data = false;
    disk->sector_pointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void pico_disk_init(void)
{
    memset(&pico_disk_controller, 0, sizeof(pico_disk_controller_t));

    // Initialize static patch pool - all entries marked as free
    for (uint16_t i = 0; i < PATCH_POOL_SIZE; i++)
    {
        g_patch_pool[i].index = PATCH_INDEX_INVALID;
        g_patch_pool[i].next_pool_index = PATCH_INDEX_INVALID;
    }
    g_patch_pool_next_free = 0;
    g_patch_pool_used = 0;
    g_patch_pool_exhausted = false;

    // Initialize all drives
    for (int i = 0; i < MAX_DRIVES; i++)
    {
        pico_disk_controller.disk[i].status = STATUS_DEFAULT;
        pico_disk_controller.disk[i].track = 0;
        pico_disk_controller.disk[i].sector = 0;
        pico_disk_controller.disk[i].disk_loaded = false;
        pico_disk_controller.disk[i].disk_image_flash = NULL;
        // Initialize hash table with invalid indices
        for (uint8_t j = 0; j < PATCH_HASH_SIZE; j++)
        {
            pico_disk_controller.disk[i].patch_hash[j] = PATCH_INDEX_INVALID;
        }
    }

    // Select drive 0 by default
    pico_disk_controller.current = &pico_disk_controller.disk[0];
    pico_disk_controller.current_disk = 0;

    printf("[DISK] Patch pool initialized: %u slots (%u KB)\n", PATCH_POOL_SIZE,
           (PATCH_POOL_SIZE * sizeof(sector_patch_t)) / 1024);
}

// Load disk image for specified drive (Copy-on-Write)
bool pico_disk_load(uint8_t drive, const uint8_t* disk_image, uint32_t size)
{
    if (drive >= MAX_DRIVES)
    {
        return false;
    }

    pico_disk_t* disk = &pico_disk_controller.disk[drive];
    clear_patches(disk);

    // Copy-on-Write: Keep flash pointer, allocate RAM on first write
    disk->disk_image_flash = disk_image;
    disk->disk_size = size;
    disk->disk_loaded = true;
    disk->disk_pointer = 0;
    disk->sector = 0;
    disk->track = 0;
    disk->sector_pointer = 0;
    disk->sector_dirty = false;
    disk->have_sector_data = false;
    disk->write_status = 0;
    // Initialize hash table with invalid indices
    for (uint8_t i = 0; i < PATCH_HASH_SIZE; i++)
    {
        disk->patch_hash[i] = PATCH_INDEX_INVALID;
    }

    // Start from default hardware reset value, then reflect initial state
    disk->status = STATUS_DEFAULT;
    disk->status &= (uint8_t)~STATUS_MOVE_HEAD;
    disk->status &= (uint8_t)~STATUS_TRACK_0; // head at track 0 (active-low)
    disk->status &= (uint8_t)~STATUS_SECTOR;  // sector true

    return true;
}

// Select disk drive
void pico_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;

    if (select < MAX_DRIVES)
    {
        pico_disk_controller.current_disk = select;
        pico_disk_controller.current = &pico_disk_controller.disk[select];
    }
    else
    {
        pico_disk_controller.current_disk = 0;
        pico_disk_controller.current = &pico_disk_controller.disk[0];
    }
}

// Get disk status
uint8_t pico_disk_status(void)
{
    uint8_t status = pico_disk_controller.current->status;
    return status;
}

// Disk control function
void pico_disk_function(uint8_t control)
{
    pico_disk_t* disk = pico_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    // Step in (increase track)
    if (control & CONTROL_STEP_IN)
    {
        if (disk->track < MAX_TRACKS - 1)
        {
            disk->track++;
        }
        if (disk->track != 0)
        {
            clear_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }

    // Step out (decrease track)
    if (control & CONTROL_STEP_OUT)
    {
        if (disk->track > 0)
        {
            disk->track--;
        }
        if (disk->track == 0)
        {
            set_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }

    // Head load
    if (control & CONTROL_HEAD_LOAD)
    {
        set_status(STATUS_HEAD);
        set_status(STATUS_NRDA);
    }

    // Head unload
    if (control & CONTROL_HEAD_UNLOAD)
    {
        clear_status(STATUS_HEAD);
    }

    // Write enable
    if (control & CONTROL_WE)
    {
        set_status(STATUS_ENWD);
        disk->write_status = 0;
    }
}

// Get current sector
uint8_t pico_disk_sector(void)
{
    pico_disk_t* disk = pico_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0xC0; // Invalid sector
    }

    // Wrap sector to 0 after reaching end of track
    if (disk->sector == SECTORS_PER_TRACK)
    {
        disk->sector = 0;
    }

    flush_sector(disk);

    uint32_t seek_offset = disk->track * TRACK_SIZE + disk->sector * SECTOR_SIZE;
    disk->disk_pointer = seek_offset;
    disk->sector_pointer = 0;
    disk->have_sector_data = false;

    // Format sector number (88-DCDD specification)
    // D7-D6: Always 1
    // D5-D1: Sector number (0-31)
    // D0: Sector True bit (0 at sector start, 1 otherwise)
    uint8_t ret_val = 0xC0;                         // Set D7-D6
    ret_val |= (disk->sector << SECTOR_SHIFT_BITS); // D5-D1
    ret_val |= (disk->sector_pointer == 0) ? 0 : 1; // D0

    disk->sector++;
    return ret_val;
}

// Write byte to disk (Copy-on-Write)
void pico_disk_write(uint8_t data)
{
    pico_disk_t* disk = pico_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sector_pointer >= SECTOR_SIZE + 2)
    {
        disk->sector_pointer = SECTOR_SIZE + 1;
    }

    disk->sector_data[disk->sector_pointer++] = data;
    disk->sector_dirty = true;
    disk->have_sector_data = true;

    if (disk->write_status == SECTOR_SIZE)
    {
        flush_sector(disk);
        disk->write_status = 0;
        clear_status(STATUS_ENWD);
    }
    else
    {
        disk->write_status++;
    }
}

// Read byte from disk
uint8_t pico_disk_read(void)
{
    pico_disk_t* disk = pico_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0x00;
    }

    // Load sector data if not already loaded
    if (!disk->have_sector_data)
    {
        disk->sector_pointer = 0;
        memset(disk->sector_data, 0x00, SECTOR_SIZE);

        uint32_t offset = disk->disk_pointer;
        if (offset + SECTOR_SIZE <= disk->disk_size)
        {
            memcpy(disk->sector_data, &disk->disk_image_flash[offset], SECTOR_SIZE);
            disk->have_sector_data = true;

            // Apply patch if exists
            uint16_t sector_index = (uint16_t)(offset / SECTOR_SIZE);
            uint16_t patch_idx = find_patch_index(disk, sector_index);
            if (patch_idx != PATCH_INDEX_INVALID)
            {
                memcpy(disk->sector_data, g_patch_pool[patch_idx].data, SECTOR_SIZE);
            }
        }
    }

    // Return current byte and advance pointer within sector
    // Note: Sector positioning is controlled by pico_disk_sector() (port 0x09), not here
    return disk->sector_data[disk->sector_pointer++];
}

// Get patch pool statistics
void pico_disk_get_patch_stats(uint16_t* used, uint16_t* total)
{
    if (used)
    {
        *used = g_patch_pool_used;
    }
    if (total)
    {
        *total = PATCH_POOL_SIZE;
    }
}
