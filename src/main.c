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
#include "key_store.h"

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

    lcd_init();
    touch_init();

    lcd_clear(COLOR_BLACK);
    lcd_draw_string(20, 30, "STORAGE APPLIANCE", COLOR_CYAN, COLOR_BLACK, 3);

    // ---- PSK: load or generate on first boot ----
    uint8_t psk[PSK_LEN];
    bool first_boot = !key_store_load(psk);

    if (first_boot) {
        printf("[key] First boot — generating PSK\n");
        key_store_generate(psk);

        char hex[PSK_LEN * 2 + 1];
        key_store_format_hex(psk, hex);

        lcd_draw_string(20, 80, "FIRST BOOT", COLOR_YELLOW, COLOR_BLACK, 3);
        lcd_draw_string(20, 120, "YOUR PSK:", COLOR_WHITE, COLOR_BLACK, 2);

        // Display PSK in 2 rows of 32 hex chars each
        char line1[33], line2[33];
        memcpy(line1, hex, 32); line1[32] = '\0';
        memcpy(line2, hex + 32, 32); line2[32] = '\0';

        lcd_draw_string(20, 150, line1, COLOR_GREEN, COLOR_BLACK, 2);
        lcd_draw_string(20, 175, line2, COLOR_GREEN, COLOR_BLACK, 2);

        lcd_draw_string(20, 220, "SAVE THIS KEY!", COLOR_RED, COLOR_BLACK, 2);
        lcd_draw_string(20, 250, "TOUCH OR WAIT 10S", COLOR_WHITE, COLOR_BLACK, 2);

        printf("[key] PSK: %s\n", hex);
        printf("[key] Touch screen or wait 10s to continue...\n");

        // Wait for touch to acknowledge, but continue automatically.
        sleep_ms(1000);  // debounce
        absolute_time_t deadline = make_timeout_time_ms(10000);
        while (true) {
            touch_point_t tp = touch_read();
            if (tp.pressed) break;
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) break;
            sleep_ms(50);
        }
        sleep_ms(300);  // debounce release
    } else {
        printf("[key] PSK loaded from flash\n");
    }

    net_core_set_psk(psk);

    // ---- Normal boot screen ----
    lcd_clear(COLOR_BLACK);
    lcd_draw_string(20, 30, "STORAGE APPLIANCE", COLOR_CYAN, COLOR_BLACK, 3);
    lcd_draw_string(40, 70, "STARTING...", COLOR_YELLOW, COLOR_BLACK, 2);

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
