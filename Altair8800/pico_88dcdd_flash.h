#ifndef _PICO_88DCDD_FLASH_H_
#define _PICO_88DCDD_FLASH_H_

#include "types.h"
#include <stdbool.h>

// MITS 88-DCDD compatible disk controller for Pico
// Copy-on-write implemented via per-sector patch list

// Status bits (active-low)
#define STATUS_ENWD 1
#define STATUS_MOVE_HEAD 2
#define STATUS_HEAD 4
#define STATUS_SECTOR 8 // Bit 3: Sector position (0=positioned, 1=not ready)
#define STATUS_IE 32
#define STATUS_TRACK_0 64
#define STATUS_NRDA 128

// Control bits
#define CONTROL_STEP_IN 1
#define CONTROL_STEP_OUT 2
#define CONTROL_HEAD_LOAD 4
#define CONTROL_HEAD_UNLOAD 8
#define CONTROL_IE 16
#define CONTROL_ID 32
#define CONTROL_HCS 64
#define CONTROL_WE 128

// Disk geometry for 8" floppy
#define SECTOR_SIZE 137
#define SECTORS_PER_TRACK 32
#define MAX_TRACKS 77
#define TRACK_SIZE (SECTORS_PER_TRACK * SECTOR_SIZE)
#define DISK_SIZE (MAX_TRACKS * TRACK_SIZE)

// Drive selection
#define MAX_DRIVES 4
#define DRIVE_SELECT_MASK 0x0F
#define SECTOR_SHIFT_BITS 1

// Hash table size for sector patches (power of 2 for fast modulo)
#define PATCH_HASH_SIZE 32

// Static patch pool configuration
// Each patch is ~141 bytes (137 data + 2 index + 2 next_index)
// 1200 patches = ~165KB
#define PATCH_POOL_SIZE 1200

typedef struct sector_patch
{
    uint16_t index;           // Sector index this patch applies to
    uint16_t next_pool_index; // Index into static pool (0xFFFF = end of list)
    uint8_t data[SECTOR_SIZE];
} sector_patch_t;

typedef struct
{
    const uint8_t* disk_image_flash;      // Read-only pointer to flash image
    uint32_t disk_size;                   // Size of disk image
    uint8_t track;                        // Current track (0-76)
    uint8_t sector;                       // Current sector (0-31)
    uint8_t status;                       // Status register
    uint8_t write_status;                 // Write operation status
    uint32_t disk_pointer;                // Current position in disk
    uint8_t sector_pointer;               // Position within current sector
    uint8_t sector_data[SECTOR_SIZE + 2]; // Sector buffer
    bool sector_dirty;                    // Sector needs writing back
    bool have_sector_data;                // Sector buffer is valid
    bool disk_loaded;                     // Disk image is loaded
    uint16_t patch_hash[PATCH_HASH_SIZE]; // Hash table - indices into static pool (0xFFFF = empty)
} pico_disk_t;

typedef struct
{
    pico_disk_t disk[MAX_DRIVES];
    pico_disk_t* current;
    uint8_t current_disk;
} pico_disk_controller_t;

// Global disk controller
extern pico_disk_controller_t pico_disk_controller;

// Disk controller functions (88-DCDD compatible interface)
void pico_disk_select(uint8_t drive);
uint8_t pico_disk_status(void);
void pico_disk_function(uint8_t control);
uint8_t pico_disk_sector(void);
void pico_disk_write(uint8_t data);
uint8_t pico_disk_read(void);

// Initialization
void pico_disk_init(void);
bool pico_disk_load(uint8_t drive, const uint8_t* disk_image, uint32_t size);

// Statistics
void pico_disk_get_patch_stats(uint16_t* used, uint16_t* total);

#endif
