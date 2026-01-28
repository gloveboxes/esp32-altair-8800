#ifndef _PICO_88DCDD_SD_CARD_H_
#define _PICO_88DCDD_SD_CARD_H_

#include "types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"

// MITS 88-DCDD compatible disk controller for Pico with SD Card support
// Uses FatFs for file I/O on SD card

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

// Drive numbers
#define DRIVE_A 0
#define DRIVE_B 1
#define DRIVE_C 2
#define DRIVE_D 3

// Disk file paths on SD card
#define DISK_A_PATH "Disks/cpm63k.dsk"
#define DISK_B_PATH "Disks/bdsc-v1.60.dsk"
#define DISK_C_PATH "Disks/escape-posix.dsk"
#define DISK_D_PATH "Disks/blank.dsk"

typedef struct
{
    FIL fil;                                 // FatFs file handle
    uint8_t track;                           // Current track (0-76)
    uint8_t sector;                          // Current sector (0-31)
    uint8_t status;                          // Status register
    uint8_t write_status;                    // Write operation status
    uint32_t diskPointer;                    // Current position in disk
    uint8_t sectorPointer;                   // Position within current sector
    uint8_t sectorData[SECTOR_SIZE + 2];     // Sector buffer
    bool sectorDirty;                        // Sector needs writing back
    bool haveSectorData;                     // Sector buffer is valid
    bool disk_loaded;                        // Disk file is open
} sd_disk_t;

typedef struct
{
    sd_disk_t disk[MAX_DRIVES];
    sd_disk_t* current;
    uint8_t currentDisk;
} sd_disk_controller_t;

// Global disk controller
extern sd_disk_controller_t sd_disk_controller;

// Disk controller functions (88-DCDD compatible interface)
void sd_disk_select(uint8_t drive);
uint8_t sd_disk_status(void);
void sd_disk_function(uint8_t control);
uint8_t sd_disk_sector(void);
void sd_disk_write(uint8_t data);
uint8_t sd_disk_read(void);

// Initialization
void sd_disk_init(void);
bool sd_disk_load(uint8_t drive, const char* disk_path);

#endif // _PICO_88DCDD_SD_CARD_H_
