#ifndef INODE_H
#define INODE_H

#include <stdint.h>
#include "vfs_types.h"

/* Phase 4: inode CRUD and block mapping/indexing. */

int read_inode(uint32_t inode_num, inode_t *inode);
int write_inode(uint32_t inode_num, const inode_t *inode);

/*
 * Maps logical block index -> physical block number for a file.
 * If allocate is non-zero and the mapping doesn't exist yet, new blocks
 * (including indirect index blocks, as needed) are allocated and the inode
 * is updated in place (caller must still write_inode afterwards if it wants
 * size/block_count persisted - this function only touches pointers).
 */
int inode_get_block(inode_t *inode, uint32_t logical_index, int allocate, uint32_t *out_block_num);

/* Frees every data block (direct + indirect) owned by the inode. */
int inode_free_all_blocks(inode_t *inode);

#endif /* INODE_H */
