#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>

/* Phase 6: in-memory block buffer cache (LRU) sitting on top of disk.c. */

#define CACHE_FRAMES 64

void cache_init(void);
int cache_read_block(uint32_t block_num, void *buffer);
int cache_write_block(uint32_t block_num, const void *buffer);
void cache_flush(void);   /* write back all dirty frames */

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} cache_stats_t;

cache_stats_t cache_get_stats(void);

#endif /* CACHE_H */
