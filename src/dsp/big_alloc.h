/*
 * big_alloc.h -- allocations that go back to the OS when freed.
 *
 * Mini-JV's large buffers (the ~20 MB MCU with its wave ROMs inline, the 8 MB
 * unscramble scratch, the 2 MB wave-ROM temporaries, expansion ROMs) went
 * through malloc / new. glibc serves a large request with mmap at first, but
 * the first time such a block is freed it RAISES its mmap threshold to that
 * size -- so from then on the same requests come out of a heap arena, and an
 * arena does not give memory back. The load runs on a fresh thread per
 * instance, which can get a fresh arena. Measured on a Move: every
 * load/unload of Mini-JV left MoveOriginal ~8 MB larger (the scratch buffer's
 * size), with nothing actually leaked.
 *
 * Mapping these directly sidesteps the allocator: munmap always returns them,
 * and nothing about malloc's behaviour for the rest of the process changes
 * (a process-wide mallopt would retune Move's own allocator).
 */
#ifndef BIG_ALLOC_H
#define BIG_ALLOC_H

#include <stddef.h>
#include <sys/mman.h>

static inline void *big_alloc(size_t n)
{
    if (n == 0) return NULL;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;   /* zero-filled, like calloc */
}

static inline void big_free(void *p, size_t n)
{
    if (p && n) munmap(p, n);
}

#endif /* BIG_ALLOC_H */
