#ifndef WAL_ENGINE_H
#define WAL_ENGINE_H

#include "wal_defs.h"

// Core 1 entry: process FIFO ops + background compaction (never returns)
void wal_engine_run(wal_state_t *wal);

#endif
