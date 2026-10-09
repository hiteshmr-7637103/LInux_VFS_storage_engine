#ifndef VFS_TYPES_H
#define VFS_TYPES_H

#include <stdint.h>

#define VFS_MAGIC         0x56465331u   /* "VFS1" */
#define BLOCK_SIZE        4096u
#define DISK_BLOCKS       4096u         /* 16 MiB virtual disk */
#define MAX_INODES        1024u

#define SUPERBLOCK_BLOCK      0u
#define INODE_BITMAP_BLOCK    1u
#define BLOCK_BITMAP_BLOCK    2u
#define JOURNAL_START_BLOCK   3u
#define JOURNAL_BLOCKS        2u        /* header block + 1 data block */
#define INODE_TABLE_START     (JOURNAL_START_BLOCK + JOURNAL_BLOCKS)

#define DIRECT_POINTERS   12
#define INVALID_BLOCK     0xFFFFFFFFu
#define INVALID_INODE     0xFFFFFFFFu
#define ROOT_INODE_NUM    1u            /* inode 0 is reserved/never used */

#define INODE_TYPE_FREE   0
#define INODE_TYPE_FILE   1
#define INODE_TYPE_DIR    2

/* permission bits, unix-style rwx for owner only (single simulated-owner model) */
#define PERM_OWNER_READ   0x4
#define PERM_OWNER_WRITE  0x2
#define PERM_OWNER_EXEC   0x1
#define PERM_DEFAULT_FILE 0x6   /* rw- */
#define PERM_DEFAULT_DIR  0x7   /* rwx */

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;

    uint32_t inode_bitmap_block;
    uint32_t block_bitmap_block;
    uint32_t journal_start_block;
    uint32_t journal_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t data_block_start;

    uint32_t root_inode;
    uint8_t  clean_shutdown;   /* 1 = cleanly unmounted, 0 = crashed */
    uint8_t  _pad[3];
} superblock_t;

typedef struct {
    uint16_t mode;          /* permission bits, see PERM_* */
    uint8_t  type;          /* INODE_TYPE_* */
    uint8_t  links_count;
    uint32_t uid;           /* owner uid, captured at creation via getuid() */
    uint32_t size;          /* file size in bytes */
    uint32_t block_count;   /* number of data blocks allocated */
    uint32_t ctime;
    uint32_t mtime;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t single_indirect;
    uint32_t double_indirect;
} inode_t;

#define NAME_MAX_LEN 59

typedef struct {
    uint32_t inode_num;
    uint8_t  name_len;
    char     name[NAME_MAX_LEN];
} dir_entry_t;   /* exactly 64 bytes */

#endif /* VFS_TYPES_H */
