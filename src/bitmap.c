#include "bitmap.h"
#include "disk.h"
#include "journal.h"
#include "vfs_types.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static superblock_t g_sb;
static int g_sb_loaded = 0;

static inline int bit_test(const uint8_t *map, uint32_t bit) {
    return (map[bit / 8] >> (bit % 8)) & 1;
}
static inline void bit_set(uint8_t *map, uint32_t bit) {
    map[bit / 8] |= (uint8_t)(1u << (bit % 8));
}
static inline void bit_clear(uint8_t *map, uint32_t bit) {
    map[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

int load_superblock(superblock_t *sb) {
    uint8_t buf[BLOCK_SIZE];
    if (raw_read_block(SUPERBLOCK_BLOCK, buf) != 0) return -1;
    memcpy(sb, buf, sizeof(superblock_t));
    if (sb->magic != VFS_MAGIC) {
        fprintf(stderr, "load_superblock: bad magic (disk not formatted?)\n");
        return -1;
    }
    g_sb = *sb;
    g_sb_loaded = 1;
    return 0;
}

int save_superblock(const superblock_t *sb) {
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, sb, sizeof(superblock_t));
    journal_protected_write(SUPERBLOCK_BLOCK, buf);
    g_sb = *sb;
    return 0;
}

int mkfs(const char *disk_path) {
    uint32_t inode_table_blocks = (MAX_INODES * (uint32_t)sizeof(inode_t) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t data_block_start = INODE_TABLE_START + inode_table_blocks;

    if (disk_create(disk_path, DISK_BLOCKS, BLOCK_SIZE) != 0) return -1;

    superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = VFS_MAGIC;
    sb.block_size = BLOCK_SIZE;
    sb.total_blocks = DISK_BLOCKS;
    sb.total_inodes = MAX_INODES;
    sb.inode_bitmap_block = INODE_BITMAP_BLOCK;
    sb.block_bitmap_block = BLOCK_BITMAP_BLOCK;
    sb.journal_start_block = JOURNAL_START_BLOCK;
    sb.journal_blocks = JOURNAL_BLOCKS;
    sb.inode_table_start = INODE_TABLE_START;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_block_start = data_block_start;
    sb.root_inode = ROOT_INODE_NUM;
    sb.clean_shutdown = 1;

    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));

    /* zero inode bitmap, block bitmap, journal blocks, inode table */
    raw_write_block(INODE_BITMAP_BLOCK, zero);
    raw_write_block(BLOCK_BITMAP_BLOCK, zero);
    for (uint32_t b = JOURNAL_START_BLOCK; b < JOURNAL_START_BLOCK + JOURNAL_BLOCKS; b++)
        raw_write_block(b, zero);
    for (uint32_t b = INODE_TABLE_START; b < data_block_start; b++)
        raw_write_block(b, zero);

    /* mark metadata blocks (everything before data_block_start) as permanently allocated */
    uint8_t block_bitmap[BLOCK_SIZE];
    memset(block_bitmap, 0, sizeof(block_bitmap));
    for (uint32_t b = 0; b < data_block_start; b++) bit_set(block_bitmap, b);
    raw_write_block(BLOCK_BITMAP_BLOCK, block_bitmap);

    /* mark inode 0 as permanently allocated (reserved / "no inode") */
    uint8_t inode_bitmap[BLOCK_SIZE];
    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    bit_set(inode_bitmap, 0);
    bit_set(inode_bitmap, ROOT_INODE_NUM);
    raw_write_block(INODE_BITMAP_BLOCK, inode_bitmap);

    sb.free_blocks = DISK_BLOCKS - data_block_start;
    sb.free_inodes = MAX_INODES - 2; /* inode 0 reserved + root */

    raw_write_block(SUPERBLOCK_BLOCK, zero); /* clear first */
    {
        uint8_t buf[BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, &sb, sizeof(sb));
        raw_write_block(SUPERBLOCK_BLOCK, buf);
    }

    g_sb = sb;
    g_sb_loaded = 1;

    /* allocate root directory's first data block and write "." / ".." entries */
    uint32_t root_block;
    if (allocate_block(&root_block) != 0) {
        fprintf(stderr, "mkfs: failed to allocate root directory block\n");
        return -1;
    }
    uint8_t dirblk[BLOCK_SIZE];
    memset(dirblk, 0, sizeof(dirblk));
    dir_entry_t *entries = (dir_entry_t *)dirblk;
    entries[0].inode_num = ROOT_INODE_NUM;
    entries[0].name_len = 1;
    strcpy(entries[0].name, ".");
    entries[1].inode_num = ROOT_INODE_NUM; /* root's parent is itself */
    entries[1].name_len = 2;
    strcpy(entries[1].name, "..");
    raw_write_block(root_block, dirblk);

    inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.mode = PERM_DEFAULT_DIR;
    root_inode.type = INODE_TYPE_DIR;
    root_inode.links_count = 2;
    root_inode.uid = getuid();
    root_inode.size = BLOCK_SIZE;
    root_inode.block_count = 1;
    root_inode.direct[0] = root_block;
    root_inode.ctime = root_inode.mtime = (uint32_t)time(NULL);

    uint32_t inode_block = INODE_TABLE_START + (ROOT_INODE_NUM * sizeof(inode_t)) / BLOCK_SIZE;
    uint32_t inode_offset = (ROOT_INODE_NUM * sizeof(inode_t)) % BLOCK_SIZE;
    uint8_t itab[BLOCK_SIZE];
    raw_read_block(inode_block, itab);
    memcpy(itab + inode_offset, &root_inode, sizeof(root_inode));
    raw_write_block(inode_block, itab);

    printf("mkfs: formatted %s - %u blocks (%u bytes/block), %u inodes, root dir ready\n",
           disk_path, sb.total_blocks, sb.block_size, sb.total_inodes);
    return 0;
}

int allocate_inode(uint32_t *out_inode_num) {
    uint8_t bitmap[BLOCK_SIZE];
    if (raw_read_block(INODE_BITMAP_BLOCK, bitmap) != 0) return -1;

    for (uint32_t i = 0; i < g_sb.total_inodes; i++) {
        if (!bit_test(bitmap, i)) {
            bit_set(bitmap, i);
            journal_protected_write(INODE_BITMAP_BLOCK, bitmap);
            g_sb.free_inodes--;
            save_superblock(&g_sb);
            *out_inode_num = i;
            return 0;
        }
    }
    fprintf(stderr, "allocate_inode: no free inodes\n");
    return -1;
}

int free_inode(uint32_t inode_num) {
    uint8_t bitmap[BLOCK_SIZE];
    if (raw_read_block(INODE_BITMAP_BLOCK, bitmap) != 0) return -1;
    bit_clear(bitmap, inode_num);
    journal_protected_write(INODE_BITMAP_BLOCK, bitmap);
    g_sb.free_inodes++;
    save_superblock(&g_sb);
    return 0;
}

int allocate_block(uint32_t *out_block_num) {
    uint8_t bitmap[BLOCK_SIZE];
    if (raw_read_block(BLOCK_BITMAP_BLOCK, bitmap) != 0) return -1;

    for (uint32_t b = g_sb.data_block_start; b < g_sb.total_blocks; b++) {
        if (!bit_test(bitmap, b)) {
            bit_set(bitmap, b);
            journal_protected_write(BLOCK_BITMAP_BLOCK, bitmap);
            g_sb.free_blocks--;
            save_superblock(&g_sb);
            *out_block_num = b;
            return 0;
        }
    }
    fprintf(stderr, "allocate_block: disk full\n");
    return -1;
}

int free_block(uint32_t block_num) {
    uint8_t bitmap[BLOCK_SIZE];
    if (raw_read_block(BLOCK_BITMAP_BLOCK, bitmap) != 0) return -1;
    bit_clear(bitmap, block_num);
    journal_protected_write(BLOCK_BITMAP_BLOCK, bitmap);
    g_sb.free_blocks++;
    save_superblock(&g_sb);
    return 0;
}
