#include "inode.h"
#include "cache.h"
#include "bitmap.h"

#include <string.h>
#include <stdio.h>

#define POINTERS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))
#define SINGLE_INDIRECT_CAP (DIRECT_POINTERS + POINTERS_PER_BLOCK)
#define DOUBLE_INDIRECT_CAP (SINGLE_INDIRECT_CAP + POINTERS_PER_BLOCK * POINTERS_PER_BLOCK)

static inline uint32_t inode_table_block_of(uint32_t inode_num, superblock_t *sb) {
    return sb->inode_table_start + (inode_num * sizeof(inode_t)) / BLOCK_SIZE;
}

int read_inode(uint32_t inode_num, inode_t *inode) {
    superblock_t sb;
    if (load_superblock(&sb) != 0) return -1;
    if (inode_num >= sb.total_inodes) {
        fprintf(stderr, "read_inode: inode %u out of range\n", inode_num);
        return -1;
    }
    uint32_t block = inode_table_block_of(inode_num, &sb);
    uint32_t offset = (inode_num * sizeof(inode_t)) % BLOCK_SIZE;
    uint8_t buf[BLOCK_SIZE];
    if (cache_read_block(block, buf) != 0) return -1;
    memcpy(inode, buf + offset, sizeof(inode_t));
    return 0;
}

int write_inode(uint32_t inode_num, const inode_t *inode) {
    superblock_t sb;
    if (load_superblock(&sb) != 0) return -1;
    if (inode_num >= sb.total_inodes) {
        fprintf(stderr, "write_inode: inode %u out of range\n", inode_num);
        return -1;
    }
    uint32_t block = inode_table_block_of(inode_num, &sb);
    uint32_t offset = (inode_num * sizeof(inode_t)) % BLOCK_SIZE;
    uint8_t buf[BLOCK_SIZE];
    if (cache_read_block(block, buf) != 0) return -1;
    memcpy(buf + offset, inode, sizeof(inode_t));
    return cache_write_block(block, buf);
}

static int ensure_indirect_block(uint32_t *ptr_field, int allocate, uint32_t *block_count) {
    if (*ptr_field != 0) return 0;
    if (!allocate) return -1;
    uint32_t blk;
    if (allocate_block(&blk) != 0) return -1;
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    cache_write_block(blk, zero);
    *ptr_field = blk;
    (*block_count)++;
    return 0;
}

int inode_get_block(inode_t *inode, uint32_t logical_index, int allocate, uint32_t *out_block_num) {
    if (logical_index < DIRECT_POINTERS) {
        if (inode->direct[logical_index] == 0) {
            if (!allocate) return -1;
            uint32_t blk;
            if (allocate_block(&blk) != 0) return -1;
            inode->direct[logical_index] = blk;
            inode->block_count++;
        }
        *out_block_num = inode->direct[logical_index];
        return 0;
    }

    if (logical_index < SINGLE_INDIRECT_CAP) {
        uint32_t idx = logical_index - DIRECT_POINTERS;
        if (ensure_indirect_block(&inode->single_indirect, allocate, &inode->block_count) != 0) return -1;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(inode->single_indirect, buf) != 0) return -1;
        uint32_t *ptrs = (uint32_t *)buf;
        if (ptrs[idx] == 0) {
            if (!allocate) return -1;
            uint32_t blk;
            if (allocate_block(&blk) != 0) return -1;
            ptrs[idx] = blk;
            cache_write_block(inode->single_indirect, buf);
            inode->block_count++;
        }
        *out_block_num = ptrs[idx];
        return 0;
    }

    if (logical_index < DOUBLE_INDIRECT_CAP) {
        uint32_t idx = logical_index - SINGLE_INDIRECT_CAP;
        uint32_t outer_idx = idx / POINTERS_PER_BLOCK;
        uint32_t inner_idx = idx % POINTERS_PER_BLOCK;

        if (ensure_indirect_block(&inode->double_indirect, allocate, &inode->block_count) != 0) return -1;
        uint8_t outer_buf[BLOCK_SIZE];
        if (cache_read_block(inode->double_indirect, outer_buf) != 0) return -1;
        uint32_t *outer_ptrs = (uint32_t *)outer_buf;

        if (outer_ptrs[outer_idx] == 0) {
            if (!allocate) return -1;
            uint32_t blk;
            if (allocate_block(&blk) != 0) return -1;
            uint8_t zero[BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            cache_write_block(blk, zero);
            outer_ptrs[outer_idx] = blk;
            cache_write_block(inode->double_indirect, outer_buf);
            inode->block_count++;
        }

        uint8_t inner_buf[BLOCK_SIZE];
        if (cache_read_block(outer_ptrs[outer_idx], inner_buf) != 0) return -1;
        uint32_t *inner_ptrs = (uint32_t *)inner_buf;
        if (inner_ptrs[inner_idx] == 0) {
            if (!allocate) return -1;
            uint32_t blk;
            if (allocate_block(&blk) != 0) return -1;
            inner_ptrs[inner_idx] = blk;
            cache_write_block(outer_ptrs[outer_idx], inner_buf);
            inode->block_count++;
        }
        *out_block_num = inner_ptrs[inner_idx];
        return 0;
    }

    fprintf(stderr, "inode_get_block: logical index %u exceeds max file size\n", logical_index);
    return -1;
}

int inode_free_all_blocks(inode_t *inode) {
    for (int i = 0; i < DIRECT_POINTERS; i++) {
        if (inode->direct[i] != 0) {
            free_block(inode->direct[i]);
            inode->direct[i] = 0;
        }
    }
    if (inode->single_indirect != 0) {
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(inode->single_indirect, buf) == 0) {
            uint32_t *ptrs = (uint32_t *)buf;
            for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++)
                if (ptrs[i] != 0) free_block(ptrs[i]);
        }
        free_block(inode->single_indirect);
        inode->single_indirect = 0;
    }
    if (inode->double_indirect != 0) {
        uint8_t outer_buf[BLOCK_SIZE];
        if (cache_read_block(inode->double_indirect, outer_buf) == 0) {
            uint32_t *outer_ptrs = (uint32_t *)outer_buf;
            for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++) {
                if (outer_ptrs[i] == 0) continue;
                uint8_t inner_buf[BLOCK_SIZE];
                if (cache_read_block(outer_ptrs[i], inner_buf) == 0) {
                    uint32_t *inner_ptrs = (uint32_t *)inner_buf;
                    for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++)
                        if (inner_ptrs[j] != 0) free_block(inner_ptrs[j]);
                }
                free_block(outer_ptrs[i]);
            }
        }
        free_block(inode->double_indirect);
        inode->double_indirect = 0;
    }
    inode->block_count = 0;
    inode->size = 0;
    return 0;
}
