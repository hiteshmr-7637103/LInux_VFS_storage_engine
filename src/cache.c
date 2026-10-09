#include "cache.h"
#include "disk.h"
#include "vfs_types.h"

#include <string.h>

typedef struct {
    uint32_t block_num;
    int      valid;
    int      dirty;
    uint64_t last_used;
    uint8_t  data[BLOCK_SIZE];
} cache_frame_t;

static cache_frame_t g_frames[CACHE_FRAMES];
static uint64_t g_clock = 0;
static cache_stats_t g_stats;

void cache_init(void) {
    memset(g_frames, 0, sizeof(g_frames));
    for (int i = 0; i < CACHE_FRAMES; i++) g_frames[i].valid = 0;
    g_clock = 0;
    memset(&g_stats, 0, sizeof(g_stats));
}

static int find_frame(uint32_t block_num) {
    for (int i = 0; i < CACHE_FRAMES; i++) {
        if (g_frames[i].valid && g_frames[i].block_num == block_num) return i;
    }
    return -1;
}

static int find_victim(void) {
    for (int i = 0; i < CACHE_FRAMES; i++) {
        if (!g_frames[i].valid) return i;
    }
    int lru_idx = 0;
    uint64_t lru_time = g_frames[0].last_used;
    for (int i = 1; i < CACHE_FRAMES; i++) {
        if (g_frames[i].last_used < lru_time) {
            lru_time = g_frames[i].last_used;
            lru_idx = i;
        }
    }
    return lru_idx;
}

static int load_block(uint32_t block_num, int *out_idx) {
    int victim = find_victim();
    if (g_frames[victim].valid && g_frames[victim].dirty) {
        if (raw_write_block(g_frames[victim].block_num, g_frames[victim].data) != 0)
            return -1;
        g_stats.evictions++;
    }
    if (raw_read_block(block_num, g_frames[victim].data) != 0) return -1;
    g_frames[victim].block_num = block_num;
    g_frames[victim].valid = 1;
    g_frames[victim].dirty = 0;
    g_frames[victim].last_used = ++g_clock;
    *out_idx = victim;
    return 0;
}

int cache_read_block(uint32_t block_num, void *buffer) {
    int idx = find_frame(block_num);
    if (idx >= 0) {
        g_stats.hits++;
        g_frames[idx].last_used = ++g_clock;
        memcpy(buffer, g_frames[idx].data, BLOCK_SIZE);
        return 0;
    }
    g_stats.misses++;
    if (load_block(block_num, &idx) != 0) return -1;
    memcpy(buffer, g_frames[idx].data, BLOCK_SIZE);
    return 0;
}

int cache_write_block(uint32_t block_num, const void *buffer) {
    int idx = find_frame(block_num);
    if (idx < 0) {
        g_stats.misses++;
        if (load_block(block_num, &idx) != 0) return -1;
    } else {
        g_stats.hits++;
    }
    memcpy(g_frames[idx].data, buffer, BLOCK_SIZE);
    g_frames[idx].dirty = 1;
    g_frames[idx].last_used = ++g_clock;
    return 0;
}

void cache_flush(void) {
    for (int i = 0; i < CACHE_FRAMES; i++) {
        if (g_frames[i].valid && g_frames[i].dirty) {
            raw_write_block(g_frames[i].block_num, g_frames[i].data);
            g_frames[i].dirty = 0;
        }
    }
}

cache_stats_t cache_get_stats(void) {
    return g_stats;
}
