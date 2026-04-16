#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ili9488.h"
#include "wal_defs.h"
#include "wal_dma.h"
#include "kv_flash.h"
#include "net_core.h"
#include "wal_engine.h"
#include "sd_card.h"
#include "kv_sd.h"
#include "httpd/web_server.h"

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

    printf("\nPicoWAL v2\n");
    web_log("[boot] start\n");

    lcd_init();
    lcd_clear(COLOR_BLACK);
    lcd_draw_string(20, 30, "PICOWAL STARTING", COLOR_CYAN, COLOR_BLACK, 2);
    lcd_draw_string(20, 55, "PLEASE WAIT...", COLOR_YELLOW, COLOR_BLACK, 2);

    if (sd_init()) {
        web_log("[boot] SD:%s\n", sd_get_debug());
        kvsd_init();
    } else {
        web_log("[boot] SD fail:%s\n", sd_get_debug());
    }

    // Initialize WAL state
    memset(&wal, 0, sizeof(wal));
    for (int i = 0; i < SLOT_COUNT; i++) wal.slot_free[i] = 1;
    wal.next_seq = 1;

    // Initialize KV flash store (scan sectors, build sorted keymap)
    kv_init();

    // Initialize DMA for buffer transfers
    wal_dma_init();

    // Launch Core 1 (WAL engine + compactor)
    multicore_launch_core1(core1_entry);
    printf("[main] Core 1 engine launched\n");

    lcd_draw_string(20, 80, "CORE1 OK", COLOR_GREEN, COLOR_BLACK, 2);
    lcd_draw_string(20, 105, "LISTENING...", COLOR_YELLOW, COLOR_BLACK, 2);

    // Core 0: network receiver (never returns)
    net_core_run(&wal);

    return 0;
}
