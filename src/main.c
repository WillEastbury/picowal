#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ili9488.h"
#include "xpt2046.h"
#include "wal_defs.h"
#include "wal_dma.h"
#include "net_core.h"
#include "wal_engine.h"

// ============================================================
// WAL state — 192 × 2KB = 384KB in BSS
// ============================================================
static wal_state_t wal;

// ============================================================
// Core 1 entry
// ============================================================
static void core1_entry(void) {
    wal_engine_run(&wal);
}

// ============================================================
// Main (Core 0)
// ============================================================
int main(void) {
    stdio_init_all();
    sleep_ms(500);

    printf("\n================================\n");
    printf("  Pico 2W WAL Appliance\n");
    printf("  192 x 2KB delta buffer pool\n");
    printf("  APPEND | READ | COMPACT\n");
    printf("================================\n");

    lcd_init();
    touch_init();

    lcd_clear(COLOR_BLACK);
    lcd_draw_string(60, 30, "WAL APPLIANCE", COLOR_CYAN, COLOR_BLACK, 4);
    lcd_draw_string(40, 90, "192 X 2KB SLOTS", COLOR_WHITE, COLOR_BLACK, 2);
    lcd_draw_string(40, 120, "APPEND READ COMPACT", COLOR_GREEN, COLOR_BLACK, 2);
    lcd_draw_string(40, 160, "STARTING...", COLOR_YELLOW, COLOR_BLACK, 2);

    // Initialize WAL state
    memset(&wal, 0, sizeof(wal));
    for (int i = 0; i < SLOT_COUNT; i++) wal.slot_free[i] = 1;
    wal.next_seq = 1;

    // Initialize DMA for buffer transfers
    wal_dma_init();

    // Launch Core 1 (WAL engine + compactor)
    multicore_launch_core1(core1_entry);
    printf("[main] Core 1 WAL engine launched\n");

    lcd_draw_string(40, 190, "CORE1 WAL ENGINE OK", COLOR_GREEN, COLOR_BLACK, 2);
    lcd_draw_string(40, 220, "CONNECTING...", COLOR_YELLOW, COLOR_BLACK, 2);

    // Core 0: network receiver (never returns)
    net_core_run(&wal);

    return 0;
}
