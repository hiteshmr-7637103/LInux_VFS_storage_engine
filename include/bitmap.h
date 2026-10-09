#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>
#include "vfs_types.h"

/* Phase 3: superblock & free-space (bitmap) management. */

int mkfs(const char *disk_path);                 /* format a fresh disk image */
int load_superblock(superblock_t *sb);
int save_superblock(const superblock_t *sb);

int allocate_inode(uint32_t *out_inode_num);
int free_inode(uint32_t inode_num);

int allocate_block(uint32_t *out_block_num);
int free_block(uint32_t block_num);

#endif /* BITMAP_H */
