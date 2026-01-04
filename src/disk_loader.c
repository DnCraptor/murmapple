/*
 * disk_loader.c
 * 
 * SD card disk image loader for murmapple
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disk_loader.h"
#include "ff.h"
#include "pico/stdlib.h"

// MII emulator headers
#include "mii.h"
#include "mii_dd.h"
#include "mii_floppy.h"
#include "mii_slot.h"

// PSRAM base address
#define PSRAM_BASE 0x11000000

// Global state
disk_entry_t g_disk_list[MAX_DISK_IMAGES];
int g_disk_count = 0;
loaded_disk_t g_loaded_disks[2] = {0};

// FatFS objects
static FATFS fs;
static bool sd_mounted = false;

// PSRAM allocation tracking (simple bump allocator)
static uint32_t psram_offset = 0;
#define PSRAM_SIZE (8 * 1024 * 1024)  // 8MB PSRAM

// Allocate memory from PSRAM
static uint8_t *psram_alloc(uint32_t size) {
    // Align to 4 bytes
    size = (size + 3) & ~3;
    
    if (psram_offset + size > PSRAM_SIZE) {
        printf("PSRAM allocation failed: need %lu, have %lu\n", 
               size, PSRAM_SIZE - psram_offset);
        return NULL;
    }
    
    uint8_t *ptr = (uint8_t *)(PSRAM_BASE + psram_offset);
    psram_offset += size;
    printf("PSRAM allocated %lu bytes at %p (total used: %lu)\n", 
           size, ptr, psram_offset);
    return ptr;
}

// Free PSRAM (simple: just reset if both drives empty)
static void psram_free(uint8_t *ptr) {
    // Simple allocator - we can only "free" by resetting if both drives are empty
    if (!g_loaded_disks[0].loaded && !g_loaded_disks[1].loaded) {
        psram_offset = 0;
    }
}

// Get disk type from filename extension
disk_type_t disk_get_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return DISK_TYPE_UNKNOWN;
    
    // Convert extension to lowercase for comparison
    char ext[8];
    int i;
    for (i = 0; i < 7 && dot[i+1]; i++) {
        ext[i] = tolower((unsigned char)dot[i+1]);
    }
    ext[i] = '\0';
    
    if (strcmp(ext, "dsk") == 0 || strcmp(ext, "do") == 0 || strcmp(ext, "po") == 0) {
        return DISK_TYPE_DSK;
    } else if (strcmp(ext, "nib") == 0) {
        return DISK_TYPE_NIB;
    } else if (strcmp(ext, "woz") == 0) {
        return DISK_TYPE_WOZ;
    }
    
    return DISK_TYPE_UNKNOWN;
}

// Initialize SD card
int disk_loader_init(void) {
    printf("Initializing SD card...\n");
    
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD card mount failed: %d\n", fr);
        return -1;
    }
    
    sd_mounted = true;
    printf("SD card mounted successfully\n");
    
    // Scan for disk images
    int count = disk_scan_directory();
    printf("Found %d disk images\n", count);
    
    return 0;
}

// Scan /apple directory for disk images
int disk_scan_directory(void) {
    if (!sd_mounted) {
        printf("SD card not mounted\n");
        return 0;
    }
    
    DIR dir;
    FILINFO fno;
    
    g_disk_count = 0;
    
    // First try /apple directory
    FRESULT fr = f_opendir(&dir, "/apple");
    if (fr != FR_OK) {
        // Try root directory
        printf("/apple not found, checking root directory\n");
        fr = f_opendir(&dir, "/");
        if (fr != FR_OK) {
            printf("Failed to open directory: %d\n", fr);
            return 0;
        }
    } else {
        printf("Scanning /apple directory...\n");
    }
    
    while (g_disk_count < MAX_DISK_IMAGES) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        
        // Skip directories
        if (fno.fattrib & AM_DIR) continue;
        
        // Check if it's a disk image
        disk_type_t type = disk_get_type(fno.fname);
        if (type == DISK_TYPE_UNKNOWN) continue;
        
        // Add to list
        strncpy(g_disk_list[g_disk_count].filename, fno.fname, MAX_FILENAME_LEN - 1);
        g_disk_list[g_disk_count].filename[MAX_FILENAME_LEN - 1] = '\0';
        g_disk_list[g_disk_count].size = fno.fsize;
        g_disk_list[g_disk_count].type = type;
        
        printf("  [%d] %s (%lu bytes, type %d)\n", 
               g_disk_count, fno.fname, fno.fsize, type);
        
        g_disk_count++;
    }
    
    f_closedir(&dir);
    return g_disk_count;
}

// Load a disk image from SD card to PSRAM
int disk_load_image(int drive, int index) {
    if (drive < 0 || drive > 1) {
        printf("Invalid drive: %d\n", drive);
        return -1;
    }
    if (index < 0 || index >= g_disk_count) {
        printf("Invalid disk index: %d\n", index);
        return -1;
    }
    
    // Unload existing image
    disk_unload_image(drive);
    
    disk_entry_t *entry = &g_disk_list[index];
    loaded_disk_t *disk = &g_loaded_disks[drive];
    
    // Construct path
    char path[128];
    snprintf(path, sizeof(path), "/apple/%s", entry->filename);
    
    printf("Loading %s to drive %d...\n", path, drive + 1);
    
    // Open file
    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_READ);
    if (fr != FR_OK) {
        // Try without /apple prefix
        snprintf(path, sizeof(path), "/%s", entry->filename);
        fr = f_open(&fp, path, FA_READ);
        if (fr != FR_OK) {
            printf("Failed to open %s: %d\n", path, fr);
            return -1;
        }
    }
    
    // Allocate PSRAM
    disk->data = psram_alloc(entry->size);
    if (!disk->data) {
        f_close(&fp);
        return -1;
    }
    
    // Read file
    UINT br;
    fr = f_read(&fp, disk->data, entry->size, &br);
    f_close(&fp);
    
    if (fr != FR_OK || br != entry->size) {
        printf("Failed to read %s: fr=%d, read=%u, expected=%lu\n", 
               path, fr, br, entry->size);
        return -1;
    }
    
    // Update loaded disk info
    disk->size = entry->size;
    disk->type = entry->type;
    strncpy(disk->filename, entry->filename, MAX_FILENAME_LEN - 1);
    disk->filename[MAX_FILENAME_LEN - 1] = '\0';
    disk->loaded = true;
    disk->write_back = false;
    
    printf("Loaded %s to drive %d (%lu bytes)\n", entry->filename, drive + 1, entry->size);
    
    return 0;
}

// Unload a disk image
void disk_unload_image(int drive) {
    if (drive < 0 || drive > 1) return;
    
    loaded_disk_t *disk = &g_loaded_disks[drive];
    
    if (!disk->loaded) return;
    
    // Write back if modified
    if (disk->write_back) {
        disk_writeback(drive);
    }
    
    // Free PSRAM (simplified - just mark as unloaded)
    psram_free(disk->data);
    disk->data = NULL;
    disk->loaded = false;
    
    printf("Unloaded drive %d\n", drive + 1);
}

// Write back modified disk image to SD card
int disk_writeback(int drive) {
    if (drive < 0 || drive > 1) return -1;
    
    loaded_disk_t *disk = &g_loaded_disks[drive];
    
    if (!disk->loaded || !disk->write_back) return 0;
    
    char path[128];
    snprintf(path, sizeof(path), "/apple/%s", disk->filename);
    
    printf("Writing back %s...\n", path);
    
    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_WRITE);
    if (fr != FR_OK) {
        printf("Failed to open %s for write: %d\n", path, fr);
        return -1;
    }
    
    UINT bw;
    fr = f_write(&fp, disk->data, disk->size, &bw);
    f_close(&fp);
    
    if (fr != FR_OK || bw != disk->size) {
        printf("Failed to write %s: fr=%d, wrote=%u, expected=%lu\n", 
               path, fr, bw, disk->size);
        return -1;
    }
    
    disk->write_back = false;
    printf("Written %s (%lu bytes)\n", disk->filename, disk->size);
    
    return 0;
}

// Convert our disk_type_t to mii_dd format enum
static uint8_t disk_type_to_mii_format(disk_type_t type, const char *filename) {
    switch (type) {
        case DISK_TYPE_DSK: {
            // Check if it's a .do or .po file
            const char *dot = strrchr(filename, '.');
            if (dot) {
                if (strcasecmp(dot, ".po") == 0) return MII_DD_FILE_PO;
                if (strcasecmp(dot, ".do") == 0) return MII_DD_FILE_DO;
            }
            return MII_DD_FILE_DSK;
        }
        case DISK_TYPE_NIB:
            return MII_DD_FILE_NIB;
        case DISK_TYPE_WOZ:
            return MII_DD_FILE_WOZ;
        default:
            return MII_DD_FILE_DSK;
    }
}

// Static mii_dd_file_t structures for the two drives
static mii_dd_file_t g_dd_files[2] = {0};

// Mount a loaded disk image to the emulator
int disk_mount_to_emulator(int drive, mii_t *mii, int slot) {
    if (drive < 0 || drive > 1) {
        printf("Invalid drive: %d\n", drive);
        return -1;
    }
    
    loaded_disk_t *disk = &g_loaded_disks[drive];
    if (!disk->loaded || !disk->data) {
        printf("No disk loaded in drive %d\n", drive + 1);
        return -1;
    }
    
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d (slot %d)\n", drive + 1, slot);
        return -1;
    }
    
    mii_floppy_t *floppy = floppies[drive];
    mii_dd_file_t *file = &g_dd_files[drive];
    
    // Set up the mii_dd_file_t structure pointing to PSRAM data
    memset(file, 0, sizeof(*file));
    file->pathname = disk->filename;  // Just point to our filename
    file->format = disk_type_to_mii_format(disk->type, disk->filename);
    file->read_only = 0;  // Allow writes (will be stored in PSRAM)
    file->start = disk->data;
    file->map = disk->data;
    file->fd = -1;  // No file descriptor (memory-mapped from PSRAM)
    file->size = disk->size;
    file->dd = NULL;  // Not associated with a specific mii_dd_t
    
    printf("Mounting %s to drive %d (format=%d, size=%lu)\n",
           disk->filename, drive + 1, file->format, file->size);
    
    printf("Disk data in PSRAM: first 16 bytes = ");
    for (int i = 0; i < 16; i++) {
        printf("%02X ", disk->data[i]);
    }
    printf("\n");
    
    // Initialize the floppy (clears all tracks)
    mii_floppy_init(floppy);
    
    // Load the disk image into the floppy structure
    res = mii_floppy_load(floppy, file);
    if (res < 0) {
        printf("Failed to load disk image to floppy: %d\n", res);
        return -1;
    }
    
    // Debug: check track 0 data after loading
    printf("Track 0 bit_count: %d bits (%d bytes)\n", 
           floppy->tracks[0].bit_count, floppy->tracks[0].bit_count / 8);
    printf("Track 0 first 16 bytes: ");
    for (int i = 0; i < 16; i++) {
        printf("%02X ", floppy->track_data[0][i]);
    }
    printf("\n");
    
    // Enable the boot signature so the slot is now bootable
    int enable = 1;
    mii_slot_command(mii, slot, MII_SLOT_D2_SET_BOOT, &enable);
    
    printf("Disk %s mounted successfully to drive %d\n", disk->filename, drive + 1);
    return 0;
}

// Eject a disk from the emulator
void disk_eject_from_emulator(int drive, mii_t *mii, int slot) {
    if (drive < 0 || drive > 1) return;
    
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d\n", drive + 1);
        return;
    }
    
    // Re-initialize the floppy (clears all data, makes it "empty")
    mii_floppy_init(floppies[drive]);
    
    // Clear the static file structure
    memset(&g_dd_files[drive], 0, sizeof(g_dd_files[drive]));
    
    printf("Drive %d ejected\n", drive + 1);
}
