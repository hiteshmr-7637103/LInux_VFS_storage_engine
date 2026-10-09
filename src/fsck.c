#include "fsck.h"
#include "vfs_types.h"
#include "disk.h"
#include "bitmap.h"
#include "inode.h"
#include "journal.h"
#include "cache.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static inline int bit_test(const uint8_t *map, uint32_t bit) {
    return (map[bit / 8] >> (bit % 8)) & 1;
}
static inline void bit_set(uint8_t *map, uint32_t bit) { map[bit / 8] |= (uint8_t)(1u << (bit % 8)); }
static inline void bit_clear(uint8_t *map, uint32_t bit) { map[bit / 8] &= (uint8_t)~(1u << (bit % 8)); }

#define POINTERS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

static void mark_usage(uint8_t *usage, uint32_t total_blocks, uint32_t block, int *problems) {
    if (block == 0 || block >= total_blocks) return;
    if (bit_test(usage, block)) {
        printf("  [DOUBLE-ALLOC] block %u is referenced by more than one inode\n", block);
        (*problems)++;
    }
    bit_set(usage, block);
}

static void walk_inode_blocks(const inode_t *inode, uint8_t *usage, uint32_t total_blocks, int *problems) {
    for (int i = 0; i < DIRECT_POINTERS; i++)
        if (inode->direct[i]) mark_usage(usage, total_blocks, inode->direct[i], problems);

    if (inode->single_indirect) {
        mark_usage(usage, total_blocks, inode->single_indirect, problems);
        uint8_t buf[BLOCK_SIZE];
        if (raw_read_block(inode->single_indirect, buf) == 0) {
            uint32_t *ptrs = (uint32_t *)buf;
            for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++)
                if (ptrs[i]) mark_usage(usage, total_blocks, ptrs[i], problems);
        }
    }
    if (inode->double_indirect) {
        mark_usage(usage, total_blocks, inode->double_indirect, problems);
        uint8_t outer[BLOCK_SIZE];
        if (raw_read_block(inode->double_indirect, outer) == 0) {
            uint32_t *outer_ptrs = (uint32_t *)outer;
            for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++) {
                if (!outer_ptrs[i]) continue;
                mark_usage(usage, total_blocks, outer_ptrs[i], problems);
                uint8_t inner[BLOCK_SIZE];
                if (raw_read_block(outer_ptrs[i], inner) == 0) {
                    uint32_t *inner_ptrs = (uint32_t *)inner;
                    for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
                        if (inner_ptrs[j]) mark_usage(usage, total_blocks, inner_ptrs[j], problems);
                }
            }
        }
    }
}

int fsck_check(int fix) {
    cache_flush();   /* fsck reads raw blocks directly, so dirty cached pages must hit disk first */

    superblock_t sb;
    if (load_superblock(&sb) != 0) {
        printf("fsck: cannot read superblock\n");
        return 1;
    }

    int problems = 0;
    uint8_t *usage_bits = calloc((sb.total_blocks + 7) / 8, 1);

    for (uint32_t b = 0; b < sb.data_block_start; b++) bit_set(usage_bits, b);

    uint8_t inode_bitmap[BLOCK_SIZE];
    raw_read_block(INODE_BITMAP_BLOCK, inode_bitmap);
    int inode_bitmap_dirty = 0;

    printf("fsck: scanning %u inodes...\n", sb.total_inodes);
    for (uint32_t i = 0; i < sb.total_inodes; i++) {
        inode_t inode;
        if (read_inode(i, &inode) != 0) continue;
        int bit_allocated = bit_test(inode_bitmap, i);
        int actually_used = (inode.type != INODE_TYPE_FREE);

        if (bit_allocated && !actually_used && i != 0) {
            printf("  [ORPHAN] inode %u marked allocated but holds no file/dir\n", i);
            problems++;
            if (fix) { bit_clear(inode_bitmap, i); inode_bitmap_dirty = 1; }
        } else if (!bit_allocated && actually_used) {
            printf("  [UNMARKED] inode %u is in use but not marked allocated in the bitmap\n", i);
            problems++;
            if (fix) { bit_set(inode_bitmap, i); inode_bitmap_dirty = 1; }
        }

        if (actually_used) walk_inode_blocks(&inode, usage_bits, sb.total_blocks, &problems);
    }

    if (fix && inode_bitmap_dirty) {
        journal_protected_write(INODE_BITMAP_BLOCK, inode_bitmap);
        printf("fsck: repaired inode bitmap\n");
    }

    uint8_t block_bitmap[BLOCK_SIZE];
    raw_read_block(BLOCK_BITMAP_BLOCK, block_bitmap);
    int block_bitmap_dirty = 0;
    for (uint32_t b = sb.data_block_start; b < sb.total_blocks; b++) {
        int marked = bit_test(block_bitmap, b);
        int used = bit_test(usage_bits, b);
        if (marked && !used) {
            printf("  [LEAK] block %u marked allocated but not referenced by any inode\n", b);
            problems++;
            if (fix) { bit_clear(block_bitmap, b); block_bitmap_dirty = 1; }
        } else if (!marked && used) {
            printf("  [UNMARKED-BLOCK] block %u in use but not marked allocated\n", b);
            problems++;
            if (fix) { bit_set(block_bitmap, b); block_bitmap_dirty = 1; }
        }
    }
    if (fix && block_bitmap_dirty) {
        journal_protected_write(BLOCK_BITMAP_BLOCK, block_bitmap);
        printf("fsck: repaired block bitmap\n");
    }

    free(usage_bits);

    if (problems == 0) printf("fsck: filesystem is consistent, no problems found\n");
    else printf("fsck: %d problem(s) found%s\n", problems, fix ? " (repaired)" : " (run 'fsck fix' to repair)");

    return problems;
}
