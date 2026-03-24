#include "wal_dma.h"
#include "hardware/dma.h"
#include <string.h>
#include <stdio.h>

static int dma_chan = -1;

void wal_dma_init(void) {
    dma_chan = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32); // 32-bit word transfers
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_FORCE);  // unpaced — run as fast as possible

    dma_channel_set_config(dma_chan, &cfg, false);

    printf("[dma] Channel %d allocated for buffer transfers\n", dma_chan);
}

void wal_dma_copy_start(volatile void *dst, const volatile void *src, uint32_t len) {
    // DMA transfers in 32-bit words; handle the bulk, CPU handles remainder
    uint32_t word_count = len / 4;

    if (word_count > 0) {
        dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, DREQ_FORCE);

        dma_channel_configure(
            dma_chan,
            &cfg,
            (void *)dst,         // write address
            (const void *)src,   // read address
            word_count,          // number of 32-bit transfers
            true                 // start immediately
        );
    }

    // Copy remaining bytes (0-3) with CPU
    uint32_t done = word_count * 4;
    if (done < len) {
        // Wait for DMA to finish the bulk first
        if (word_count > 0) {
            dma_channel_wait_for_finish_blocking(dma_chan);
        }
        memcpy((uint8_t *)dst + done, (const uint8_t *)src + done, len - done);
    }
}

void wal_dma_wait(void) {
    if (dma_chan >= 0) {
        dma_channel_wait_for_finish_blocking(dma_chan);
    }
}

bool wal_dma_busy(void) {
    if (dma_chan < 0) return false;
    return dma_channel_is_busy(dma_chan);
}

void wal_dma_copy(volatile void *dst, const volatile void *src, uint32_t len) {
    if (len < DMA_THRESHOLD) {
        memcpy((void *)dst, (const void *)src, len);
        return;
    }
    wal_dma_copy_start(dst, src, len);
    wal_dma_wait();
}
