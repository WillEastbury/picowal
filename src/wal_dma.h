#ifndef WAL_DMA_H
#define WAL_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/dma.h"

// DMA-accelerated memory-to-memory copy for buffer slot writes.
// Uses a dedicated DMA channel to offload bulk payload copies
// from Core 0, freeing it to continue parsing TCP traffic.
//
// For small transfers (<64 bytes) falls back to CPU memcpy
// since DMA setup overhead exceeds the copy time.

#define DMA_THRESHOLD 64  // bytes; below this, CPU memcpy is faster

// Must be called once at startup (from Core 0).
void wal_dma_init(void);

// Non-blocking DMA copy. Returns immediately.
// Call wal_dma_wait() before accessing the destination.
void wal_dma_copy_start(volatile void *dst, const volatile void *src, uint32_t len);

// Block until the current DMA transfer completes.
void wal_dma_wait(void);

// Blocking DMA copy (start + wait). For transfers < DMA_THRESHOLD,
// falls back to CPU memcpy automatically.
void wal_dma_copy(volatile void *dst, const volatile void *src, uint32_t len);

// Returns true if a DMA transfer is currently in progress.
bool wal_dma_busy(void);

#endif
