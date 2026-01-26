#include "pico_88dcdd_sd_card.h"

// MITS 88-DCDD Disk Controller Emulation for Pico with SD Card
// Implements active-low status bit logic for Altair 8800 floppy disk controller
// Uses FatFs for file I/O on SD card

// Global disk controller instance
sd_disk_controller_t sd_disk_controller;

static void writeSector(sd_disk_t* pDisk);

static const uint8_t STATUS_DEFAULT =
    STATUS_ENWD | STATUS_MOVE_HEAD | STATUS_HEAD | STATUS_IE | STATUS_TRACK_0 | STATUS_NRDA;

// Set status condition to TRUE (clears bit for active-low hardware)
static inline void set_status(uint8_t bit)
{
    sd_disk_controller.current->status &= ~bit;
}

// Set status condition to FALSE (sets bit for active-low hardware)
static inline void clear_status(uint8_t bit)
{
    sd_disk_controller.current->status |= bit;
}

// Helper function to handle common track positioning logic
static void seek_to_track(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sectorDirty)
    {
        writeSector(disk);
    }

    uint32_t seek_offset = disk->track * TRACK_SIZE;
    
    // Seek to track position in file
    FRESULT fr = f_lseek(&disk->fil, seek_offset);
    if (fr != FR_OK)
    {
        printf("[SD_DISK] Seek failed for track %u, error: %d\n", disk->track, fr);
    }

    disk->diskPointer = seek_offset;
    disk->haveSectorData = false;
    disk->sectorPointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void sd_disk_init(void)
{
    memset(&sd_disk_controller, 0, sizeof(sd_disk_controller_t));

    // Initialize all drives
    for (int i = 0; i < MAX_DRIVES; i++)
    {
        sd_disk_controller.disk[i].status = STATUS_DEFAULT;
        sd_disk_controller.disk[i].track = 0;
        sd_disk_controller.disk[i].sector = 0;
        sd_disk_controller.disk[i].disk_loaded = false;
    }

    // Select drive 0 by default
    sd_disk_controller.current = &sd_disk_controller.disk[0];
    sd_disk_controller.currentDisk = 0;
}

// Load disk image for specified drive from SD card
bool sd_disk_load(uint8_t drive, const char* disk_path)
{
    if (drive >= MAX_DRIVES)
    {
        printf("[SD_DISK] Invalid drive number: %u\n", drive);
        return false;
    }

    sd_disk_t* disk = &sd_disk_controller.disk[drive];

    // Close existing file if open
    if (disk->disk_loaded)
    {
        f_close(&disk->fil);
        disk->disk_loaded = false;
    }

    // Open disk file for read/write
    FRESULT fr = f_open(&disk->fil, disk_path, FA_READ | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("[SD_DISK] Failed to open %s, error: %d\n", disk_path, fr);
        return false;
    }

    // Verify file size
    FSIZE_t file_size = f_size(&disk->fil);
    if (file_size < DISK_SIZE)
    {
        printf("[SD_DISK] Warning: %s is smaller than expected (%lu bytes)\n", 
               disk_path, (unsigned long)file_size);
    }

    disk->disk_loaded = true;
    disk->diskPointer = 0;
    disk->sector = 0;
    disk->track = 0;
    disk->sectorPointer = 0;
    disk->sectorDirty = false;
    disk->haveSectorData = false;
    disk->write_status = 0;

    // Start from default hardware reset value, then reflect initial state
    disk->status = STATUS_DEFAULT;
    disk->status &= (uint8_t)~STATUS_MOVE_HEAD;
    disk->status &= (uint8_t)~STATUS_TRACK_0; // head at track 0 (active-low)
    disk->status &= (uint8_t)~STATUS_SECTOR;  // sector true

    return true;
}

// Select disk drive
void sd_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;

    if (select < MAX_DRIVES)
    {
        sd_disk_controller.currentDisk = select;
        sd_disk_controller.current = &sd_disk_controller.disk[select];
    }
    else
    {
        sd_disk_controller.currentDisk = 0;
        sd_disk_controller.current = &sd_disk_controller.disk[0];
    }
}

// Get disk status
uint8_t sd_disk_status(void)
{
    return sd_disk_controller.current->status;
}

// Disk control function
void sd_disk_function(uint8_t control)
{
    sd_disk_t* disk = sd_disk_controller.current;

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
uint8_t sd_disk_sector(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0xC0; // Invalid sector
    }

    // Wrap sector to 0 after reaching end of track
    if (disk->sector == SECTORS_PER_TRACK)
    {
        disk->sector = 0;
    }

    if (disk->sectorDirty)
    {
        writeSector(disk);
    }

    uint32_t seek_offset = disk->track * TRACK_SIZE + disk->sector * SECTOR_SIZE;
    
    // Seek to sector position in file
    FRESULT fr = f_lseek(&disk->fil, seek_offset);
    if (fr != FR_OK)
    {
        printf("[SD_DISK] Seek failed for sector, error: %d\n", fr);
    }

    disk->diskPointer = seek_offset;
    disk->sectorPointer = 0;
    disk->haveSectorData = false;

    // Format sector number (88-DCDD specification)
    // D7-D6: Always 1
    // D5-D1: Sector number (0-31)
    // D0: Sector True bit (0 at sector start, 1 otherwise)
    uint8_t ret_val = 0xC0;                         // Set D7-D6
    ret_val |= (disk->sector << SECTOR_SHIFT_BITS); // D5-D1
    ret_val |= (disk->sectorPointer == 0) ? 0 : 1;  // D0

    disk->sector++;
    return ret_val;
}

// Write byte to disk
void sd_disk_write(uint8_t data)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sectorPointer >= SECTOR_SIZE + 2)
    {
        disk->sectorPointer = SECTOR_SIZE + 1;
    }

    disk->sectorData[disk->sectorPointer++] = data;
    disk->sectorDirty = true;

    if (disk->write_status == SECTOR_SIZE)
    {
        writeSector(disk);
        disk->write_status = 0;
        clear_status(STATUS_ENWD);
    }
    else
    {
        disk->write_status++;
    }
}

// Read byte from disk
uint8_t sd_disk_read(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0x00;
    }

    // Load sector data if not already loaded
    if (!disk->haveSectorData)
    {
        disk->sectorPointer = 0;
        memset(disk->sectorData, 0x00, SECTOR_SIZE);

        // Read sector from SD card
        UINT bytes_read;
        FRESULT fr = f_read(&disk->fil, disk->sectorData, SECTOR_SIZE, &bytes_read);
        
        if (fr != FR_OK)
        {
            printf("[SD_DISK] Sector read failed, error: %d\n", fr);
            disk->haveSectorData = false;
        }
        else if (bytes_read != SECTOR_SIZE)
        {
            printf("[SD_DISK] Sector read incomplete: read %u of %u bytes\n", 
                   bytes_read, SECTOR_SIZE);
            disk->haveSectorData = (bytes_read > 0);
        }
        else
        {
            disk->haveSectorData = true;
        }
    }

    // Return current byte and advance pointer within sector
    return disk->sectorData[disk->sectorPointer++];
}

// Write sector buffer back to disk
static void writeSector(sd_disk_t* pDisk)
{
    if (!pDisk->sectorDirty)
    {
        return;
    }

    // Write sector to SD card
    UINT bytes_written;
    FRESULT fr = f_write(&pDisk->fil, pDisk->sectorData, SECTOR_SIZE, &bytes_written);
    
    if (fr != FR_OK)
    {
        printf("[SD_DISK] Sector write failed, error: %d\n", fr);
    }
    else if (bytes_written != SECTOR_SIZE)
    {
        printf("[SD_DISK] Sector write incomplete: wrote %u of %u bytes\n", 
               bytes_written, SECTOR_SIZE);
    }
    else
    {
        // Flush to ensure data is written to SD card
        f_sync(&pDisk->fil);
    }

    pDisk->sectorPointer = 0;
    pDisk->sectorDirty = false;
}
