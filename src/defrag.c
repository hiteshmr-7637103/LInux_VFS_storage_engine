#include "defrag.h"
#include "vfs_types.h"
#include "disk.h"
#include "cache.h"
#include "bitmap.h"
#include "inode.h"
#include "journal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define POINTERS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

static inline void bit_set(uint8_t *map, uint32_t bit) { map[bit / 8] |= (uint8_t)(1u << (bit % 8)); }

/*
 * Safe two-phase compaction: EVERY old block's content is read into memory
 * before ANY block is written back out. That ordering is what makes this
 * safe - a naive in-place "copy forward as you go" compactor can clobber a
 * not-yet-relocated block whenever an old block number happens to fall
 * inside the already-compacted region.
 */

static uint32_t assign_slot(uint32_t old_block, uint32_t data_block_start, uint32_t *new_block_of,
                             uint8_t **buffers, uint32_t *slot_cursor, uint8_t *is_index,
                             uint32_t *index_slots, uint32_t *index_count, int as_index) {
    if (new_block_of[old_block] != 0) return new_block_of[old_block]; /* already relocated */

    uint32_t slot = (*slot_cursor)++;
    buffers[slot] = malloc(BLOCK_SIZE);
    raw_read_block(old_block, buffers[slot]);

    uint32_t new_block = data_block_start + slot;
    new_block_of[old_block] = new_block;

    if (as_index) {
        is_index[slot] = 1;
        index_slots[(*index_count)++] = slot;
    }
    return new_block;
}

int defrag_run(void) {
    cache_flush();

    superblock_t sb;
    if (load_superblock(&sb) != 0) { printf("defrag: cannot read superblock\n"); return -1; }

    uint32_t max_slots = sb.total_blocks - sb.data_block_start;
    uint32_t *new_block_of = calloc(sb.total_blocks, sizeof(uint32_t));
    uint8_t **buffers = calloc(max_slots, sizeof(uint8_t *));
    uint8_t *is_index = calloc(max_slots, 1);
    uint32_t *index_slots = calloc(max_slots, sizeof(uint32_t));
    uint32_t index_count = 0;
    uint32_t slot_cursor = 0;

    if (!new_block_of || !buffers || !is_index || !index_slots) {
        printf("defrag: out of memory\n");
        free(new_block_of); free(buffers); free(is_index); free(index_slots);
        return -1;
    }

    printf("defrag: scanning and relocating live blocks...\n");

    /* ---- Step A: walk every live inode's blocks, read old content, assign new slots ---- */
    for (uint32_t i = 0; i < sb.total_inodes; i++) {
        inode_t inode;
        if (read_inode(i, &inode) != 0) continue;
        if (inode.type == INODE_TYPE_FREE) continue;

        for (int d = 0; d < DIRECT_POINTERS; d++) {
            if (inode.direct[d])
                assign_slot(inode.direct[d], sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 0);
        }

        if (inode.single_indirect) {
            uint32_t s_new = assign_slot(inode.single_indirect, sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 1);
            uint32_t slot = s_new - sb.data_block_start;
            uint32_t *ptrs = (uint32_t *)buffers[slot];
            for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
                if (ptrs[j])
                    assign_slot(ptrs[j], sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 0);
        }

        if (inode.double_indirect) {
            uint32_t outer_new = assign_slot(inode.double_indirect, sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 1);
            uint32_t outer_slot = outer_new - sb.data_block_start;
            uint32_t *outer_ptrs = (uint32_t *)buffers[outer_slot];
            for (uint32_t oi = 0; oi < POINTERS_PER_BLOCK; oi++) {
                if (!outer_ptrs[oi]) continue;
                uint32_t inner_new = assign_slot(outer_ptrs[oi], sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 1);
                uint32_t inner_slot = inner_new - sb.data_block_start;
                uint32_t *inner_ptrs = (uint32_t *)buffers[inner_slot];
                for (uint32_t ii = 0; ii < POINTERS_PER_BLOCK; ii++)
                    if (inner_ptrs[ii])
                        assign_slot(inner_ptrs[ii], sb.data_block_start, new_block_of, buffers, &slot_cursor, is_index, index_slots, &index_count, 0);
            }
        }
    }

    /* ---- Step B: fix up embedded pointers inside every relocated index block ---- */
    for (uint32_t k = 0; k < index_count; k++) {
        uint32_t slot = index_slots[k];
        uint32_t *ptrs = (uint32_t *)buffers[slot];
        for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
            if (ptrs[j]) ptrs[j] = new_block_of[ptrs[j]];
    }

    /* ---- Step C: write every relocated block to its new (compacted) location ---- */
    for (uint32_t slot = 0; slot < slot_cursor; slot++) {
        raw_write_block(sb.data_block_start + slot, buffers[slot]);
        free(buffers[slot]);
    }

    /* ---- Step D: rewrite every inode's own pointer fields ---- */
    for (uint32_t i = 0; i < sb.total_inodes; i++) {
        inode_t inode;
        if (read_inode(i, &inode) != 0) continue;
        if (inode.type == INODE_TYPE_FREE) continue;
        int changed = 0;
        for (int d = 0; d < DIRECT_POINTERS; d++) {
            if (inode.direct[d] && new_block_of[inode.direct[d]]) { inode.direct[d] = new_block_of[inode.direct[d]]; changed = 1; }
        }
        if (inode.single_indirect && new_block_of[inode.single_indirect]) {
            inode.single_indirect = new_block_of[inode.single_indirect]; changed = 1;
        }
        if (inode.double_indirect && new_block_of[inode.double_indirect]) {
            inode.double_indirect = new_block_of[inode.double_indirect]; changed = 1;
        }
        if (changed) write_inode(i, &inode);
    }

    /* ---- Step E: rebuild the block bitmap as one packed run ---- */
    uint8_t block_bitmap[BLOCK_SIZE];
    memset(block_bitmap, 0, sizeof(block_bitmap));
    for (uint32_t b = 0; b < sb.data_block_start + slot_cursor; b++) bit_set(block_bitmap, b);
    journal_protected_write(BLOCK_BITMAP_BLOCK, block_bitmap);

    sb.free_blocks = sb.total_blocks - sb.data_block_start - slot_cursor;
    save_superblock(&sb);

    cache_flush();  /* Step D's write_inode calls are still sitting dirty in the cache - persist them first */
    cache_init();   /* ...then drop every cached page, since some may mirror stale pre-defrag block contents */

    free(new_block_of);
    free(buffers);
    free(is_index);
    free(index_slots);

    printf("defrag: compacted %u blocks into a contiguous run (%u blocks now free)\n",
           slot_cursor, sb.free_blocks);
    return 0;
}
