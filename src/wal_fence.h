#ifndef WAL_FENCE_H
#define WAL_FENCE_H

// Hardware data memory barrier — ensures all prior memory writes
// are visible to the other core before any subsequent access.
// Required when transferring ownership of shared memory between cores.
static inline void wal_dmb(void) {
    __asm volatile ("dmb" ::: "memory");
}

#endif
