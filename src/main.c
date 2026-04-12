#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ili9488.h"
#include "xpt2046.h"
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

    printf("\n================================\n");
    printf("  Pico 2W Storage Appliance\n");
    printf("  192 x 2KB delta buffer pool\n");
    printf("  APPEND | READ | COMPACT\n");
    printf("================================\n");

    web_log("[boot] PicoWAL starting\n");

    lcd_init();
    touch_init();

    lcd_clear(COLOR_BLACK);
    lcd_draw_string(20, 30, "STORAGE APPLIANCE", COLOR_CYAN, COLOR_BLACK, 3);
    lcd_draw_string(40, 70, "STARTING...", COLOR_YELLOW, COLOR_BLACK, 2);

    // SD card init — deferred to after network is up.
    // sd_init() can hang if the card is absent or SPI1 is in a bad state,
    // so we skip it at boot and let the admin endpoint trigger it safely.
    web_log("[boot] SD init deferred (use /admin/sd to trigger)\n");

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

    lcd_draw_string(40, 95, "CORE1 ENGINE OK", COLOR_GREEN, COLOR_BLACK, 2);
    lcd_draw_string(40, 120, "LISTENING...", COLOR_YELLOW, COLOR_BLACK, 2);

    // Core 0: network receiver (never returns)
    net_core_run(&wal);

    return 0;
}
