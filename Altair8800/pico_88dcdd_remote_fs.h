#ifndef _PICO_88DCDD_REMOTE_FS_H_
#define _PICO_88DCDD_REMOTE_FS_H_

#include "types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MITS 88-DCDD compatible disk controller for Pico with Remote FS support
// Uses TCP to access disk images on remote server

// Status bits (active-low) - same as SD card version
#define RFS_STATUS_ENWD 1
#define RFS_STATUS_MOVE_HEAD 2
#define RFS_STATUS_HEAD 4
#define RFS_STATUS_SECTOR 8
#define RFS_STATUS_IE 32
#define RFS_STATUS_TRACK_0 64
#define RFS_STATUS_NRDA 128

// Control bits - same as SD card version
#define RFS_CONTROL_STEP_IN 1
#define RFS_CONTROL_STEP_OUT 2
#define RFS_CONTROL_HEAD_LOAD 4
#define RFS_CONTROL_HEAD_UNLOAD 8
#define RFS_CONTROL_IE 16
#define RFS_CONTROL_ID 32
#define RFS_CONTROL_HCS 64
#define RFS_CONTROL_WE 128

// Disk geometry for 8" floppy
#define RFS_DISK_SECTOR_SIZE 137
#define RFS_DISK_SECTORS_PER_TRACK 32
#define RFS_DISK_MAX_TRACKS 77
#define RFS_DISK_TRACK_SIZE (RFS_DISK_SECTORS_PER_TRACK * RFS_DISK_SECTOR_SIZE)
#define RFS_DISK_SIZE (RFS_DISK_MAX_TRACKS * RFS_DISK_TRACK_SIZE)

// Drive selection
#define RFS_DISK_MAX_DRIVES 4
#define RFS_DISK_DRIVE_SELECT_MASK 0x0F
#define RFS_DISK_SECTOR_SHIFT_BITS 1

// Drive numbers
#define RFS_DISK_DRIVE_A 0
#define RFS_DISK_DRIVE_B 1
#define RFS_DISK_DRIVE_C 2
#define RFS_DISK_DRIVE_D 3

// Async operation states
typedef enum
{
    RFS_DISK_OP_IDLE,
    RFS_DISK_OP_READ_PENDING,
    RFS_DISK_OP_WRITE_PENDING
} rfs_disk_op_state_t;

typedef struct
{
    uint8_t track;                                // Current track (0-76)
    uint8_t sector;                               // Current sector (0-31)
    uint8_t status;                               // Status register
    uint8_t write_status;                         // Write operation status
    uint8_t sectorPointer;                        // Position within current sector
    uint8_t sectorData[RFS_DISK_SECTOR_SIZE + 2]; // Sector buffer
    bool sectorDirty;                             // Sector needs writing back
    bool haveSectorData;                          // Sector buffer is valid
    bool disk_loaded;                             // Disk is available

    // Async operation state
    rfs_disk_op_state_t op_state;
} rfs_disk_t;

typedef struct
{
    rfs_disk_t disk[RFS_DISK_MAX_DRIVES];
    rfs_disk_t* current;
    uint8_t currentDisk;
    bool connected;   // Connected to server
    bool initialized; // INIT command completed
} rfs_disk_controller_t;

// Global disk controller
extern rfs_disk_controller_t rfs_disk_controller;

// Disk controller functions (88-DCDD compatible interface)
void rfs_disk_select(uint8_t drive);
uint8_t rfs_disk_status(void);
void rfs_disk_function(uint8_t control);
uint8_t rfs_disk_sector(void);
void rfs_disk_write(uint8_t data);
uint8_t rfs_disk_read(void);

// Initialization
void rfs_disk_init(void);
bool rfs_disk_connect(void);  // Connect to remote server
bool rfs_disk_is_ready(void); // Check if ready for operations

// Poll function - must be called from main loop on Core 0
void rfs_disk_poll(void);

#endif // _PICO_88DCDD_REMOTE_FS_H_
