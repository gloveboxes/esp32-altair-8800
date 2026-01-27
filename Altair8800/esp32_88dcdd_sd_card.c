/**
 * @file esp32_88dcdd_sd_card.c
 * @brief MITS 88-DCDD Disk Controller Emulation for ESP32 with SD Card
 * 
 * Implements active-low status bit logic for Altair 8800 floppy disk controller.
 * Uses standard C file I/O (via ESP-IDF VFS mapped to FAT filesystem).
 */

#include "esp32_88dcdd_sd_card.h"
#include "esp_log.h"

static const char* TAG = "88DCDD_SD";

// Global disk controller instance
esp32_sd_disk_controller_t esp32_sd_disk_controller;

static void writeSector(esp32_sd_disk_t* pDisk);

static const uint8_t STATUS_DEFAULT =
    STATUS_ENWD | STATUS_MOVE_HEAD | STATUS_HEAD | STATUS_IE | STATUS_TRACK_0 | STATUS_NRDA;

// Set status condition to TRUE (clears bit for active-low hardware)
static inline void set_status(uint8_t bit)
{
    esp32_sd_disk_controller.current->status &= ~bit;
}

// Set status condition to FALSE (sets bit for active-low hardware)
static inline void clear_status(uint8_t bit)
{
    esp32_sd_disk_controller.current->status |= bit;
}

// Helper function to handle common track positioning logic
static void seek_to_track(void)
{
    esp32_sd_disk_t* disk = esp32_sd_disk_controller.current;

    if (!disk->disk_loaded || !disk->file)
    {
        return;
    }

    if (disk->sectorDirty)
    {
        writeSector(disk);
    }

    uint32_t seek_offset = disk->track * TRACK_SIZE;
    
    // Seek to track position in file
    if (fseek(disk->file, seek_offset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Seek failed for track %u", disk->track);
    }

    disk->diskPointer = seek_offset;
    disk->haveSectorData = false;
    disk->sectorPointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void esp32_sd_disk_init(void)
{
    memset(&esp32_sd_disk_controller, 0, sizeof(esp32_sd_disk_controller_t));

    // Initialize all drives
    for (int i = 0; i < MAX_DRIVES; i++)
    {
        esp32_sd_disk_controller.disk[i].status = STATUS_DEFAULT;
        esp32_sd_disk_controller.disk[i].track = 0;
        esp32_sd_disk_controller.disk[i].sector = 0;
        esp32_sd_disk_controller.disk[i].disk_loaded = false;
        esp32_sd_disk_controller.disk[i].file = NULL;
    }

    // Select drive 0 by default
    esp32_sd_disk_controller.current = &esp32_sd_disk_controller.disk[0];
    esp32_sd_disk_controller.currentDisk = 0;
    
    ESP_LOGI(TAG, "Disk controller initialized with %d drives", MAX_DRIVES);
}

// Load disk image for specified drive from SD card
bool esp32_sd_disk_load(uint8_t drive, const char* disk_path)
{
    if (drive >= MAX_DRIVES)
    {
        ESP_LOGE(TAG, "Invalid drive number: %u", drive);
        return false;
    }

    esp32_sd_disk_t* disk = &esp32_sd_disk_controller.disk[drive];

    // Close existing file if open
    if (disk->disk_loaded && disk->file)
    {
        fclose(disk->file);
        disk->file = NULL;
        disk->disk_loaded = false;
    }

    // Open disk file for read/write (binary mode)
    disk->file = fopen(disk_path, "r+b");
    if (!disk->file)
    {
        ESP_LOGE(TAG, "Failed to open %s", disk_path);
        return false;
    }

    // Verify file size
    fseek(disk->file, 0, SEEK_END);
    long file_size = ftell(disk->file);
    fseek(disk->file, 0, SEEK_SET);
    
    if (file_size < DISK_SIZE)
    {
        ESP_LOGW(TAG, "%s is smaller than expected (%ld bytes, expected %d)", 
                 disk_path, file_size, DISK_SIZE);
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

    ESP_LOGI(TAG, "Drive %c: loaded %s (%ld bytes)", 
             'A' + drive, disk_path, file_size);

    return true;
}

// Select disk drive
void esp32_sd_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;

    if (select < MAX_DRIVES)
    {
        esp32_sd_disk_controller.currentDisk = select;
        esp32_sd_disk_controller.current = &esp32_sd_disk_controller.disk[select];
    }
    else
    {
        esp32_sd_disk_controller.currentDisk = 0;
        esp32_sd_disk_controller.current = &esp32_sd_disk_controller.disk[0];
    }
}

// Get disk status
uint8_t esp32_sd_disk_status(void)
{
    return esp32_sd_disk_controller.current->status;
}

// Disk control function
void esp32_sd_disk_function(uint8_t control)
{
    esp32_sd_disk_t* disk = esp32_sd_disk_controller.current;

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
uint8_t esp32_sd_disk_sector(void)
{
    esp32_sd_disk_t* disk = esp32_sd_disk_controller.current;

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
    if (disk->file && fseek(disk->file, seek_offset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Seek failed for sector");
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
void esp32_sd_disk_write(uint8_t data)
{
    esp32_sd_disk_t* disk = esp32_sd_disk_controller.current;

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
uint8_t esp32_sd_disk_read(void)
{
    esp32_sd_disk_t* disk = esp32_sd_disk_controller.current;

    if (!disk->disk_loaded || !disk->file)
    {
        return 0x00;
    }

    // Load sector data if not already loaded
    if (!disk->haveSectorData)
    {
        disk->sectorPointer = 0;
        memset(disk->sectorData, 0x00, SECTOR_SIZE);

        // Read sector from SD card
        size_t bytes_read = fread(disk->sectorData, 1, SECTOR_SIZE, disk->file);
        
        if (bytes_read != SECTOR_SIZE)
        {
            if (ferror(disk->file))
            {
                ESP_LOGE(TAG, "Sector read failed");
                disk->haveSectorData = false;
            }
            else
            {
                ESP_LOGW(TAG, "Sector read incomplete: read %u of %u bytes", 
                         bytes_read, SECTOR_SIZE);
                disk->haveSectorData = (bytes_read > 0);
            }
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
static void writeSector(esp32_sd_disk_t* pDisk)
{
    if (!pDisk->sectorDirty || !pDisk->file)
    {
        return;
    }

    // Write sector to SD card
    size_t bytes_written = fwrite(pDisk->sectorData, 1, SECTOR_SIZE, pDisk->file);
    
    if (bytes_written != SECTOR_SIZE)
    {
        ESP_LOGE(TAG, "Sector write failed: wrote %u of %u bytes", 
                 bytes_written, SECTOR_SIZE);
    }
    else
    {
        // Flush to ensure data is written to SD card
        fflush(pDisk->file);
    }

    pDisk->sectorPointer = 0;
    pDisk->sectorDirty = false;
}
