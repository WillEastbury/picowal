// Heatshrink config for PicoWAL — static allocation, no malloc
#ifndef PICOWAL_HEATSHRINK_CONFIG_H
#define PICOWAL_HEATSHRINK_CONFIG_H

#define HEATSHRINK_DYNAMIC_ALLOC 0
#define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 64
#define HEATSHRINK_STATIC_WINDOW_BITS 8      // 256-byte window
#define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4   // 16-byte lookahead
#define HEATSHRINK_DEBUGGING_LOGS 0
#define HEATSHRINK_USE_INDEX 0               // save RAM, slower compress OK

#endif
