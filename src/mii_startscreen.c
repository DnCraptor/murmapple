/*
 * mii_startscreen.c
 *
 * Start screen display for MurmApple before emulator initialization
 * Shows system information and waits for user input to proceed
 */

#include "mii_startscreen.h"
#include "debug_log.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

/**
 * Display the start screen with system information
 * Shows on USB serial console
 */
int mii_startscreen_show(mii_startscreen_info_t *info) {
    if (!info) return -1;
    
    // Display title and information
    printf("\n");
    printf("┌──────────────────────────────────────────────────────┐\n");
    printf("│                                                      │\n");
    printf("│         %s                             │\n", info->title);
    printf("│         %s                             │\n", info->subtitle);
    printf("│         %s                             │\n", info->version);
    printf("│                                                      │\n");
    printf("└──────────────────────────────────────────────────────┘\n");
    printf("\n");
    
    printf("  System Information:\n");
    printf("  ├─ CPU Clock: %lu MHz\n", info->cpu_mhz);
    printf("  ├─ PSRAM: %lu MHz\n", info->psram_mhz);
    printf("  └─ Board: RP2350-M%d\n", info->board_variant);
    printf("\n");
    
    printf("  Initializing Apple IIe emulator...\n");
    printf("\n");
    
    MII_DEBUG_PRINTF("Start screen complete, proceeding to emulator\n");
    
    return 0;
}
