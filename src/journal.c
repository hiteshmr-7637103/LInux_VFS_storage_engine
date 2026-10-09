#include "journal.h"
#include "disk.h"
#include "vfs_types.h"

#include <string.h>
#include <stdio.h>

#define JOURNAL_MAGIC 0x4A524E4Cu  /* "JRNL" */

typedef struct {
    uint32_t magic;
    uint32_t pending;        /* 1 = a write was in flight when we last looked */
    uint32_t target_block;
    uint32_t _pad;
} journal_header_t;

/* journal occupies JOURNAL_START_BLOCK (header) and JOURNAL_START_BLOCK+1 (before-image) */

void journal_protected_write(uint32_t target_block, const void *new_data) {
    uint8_t before_image[BLOCK_SIZE];
    uint8_t header_block[BLOCK_SIZE];
    journal_header_t hdr;

    if (raw_read_block(target_block, before_image) != 0) {
        fprintf(stderr, "journal: failed to read before-image of block %u\n", target_block);
        return;
    }

    /* 1. log before-image + mark pending */
    raw_write_block(JOURNAL_START_BLOCK + 1, before_image);
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = JOURNAL_MAGIC;
    hdr.pending = 1;
    hdr.target_block = target_block;
    memset(header_block, 0, sizeof(header_block));
    memcpy(header_block, &hdr, sizeof(hdr));
    raw_write_block(JOURNAL_START_BLOCK, header_block);

    /* 2. perform the real write (this is the step that could be interrupted by a crash) */
    raw_write_block(target_block, new_data);

    /* 3. commit: clear pending flag */
    hdr.pending = 0;
    memcpy(header_block, &hdr, sizeof(hdr));
    raw_write_block(JOURNAL_START_BLOCK, header_block);
}

void journal_recover(void) {
    uint8_t header_block[BLOCK_SIZE];
    journal_header_t hdr;

    if (raw_read_block(JOURNAL_START_BLOCK, header_block) != 0) return;
    memcpy(&hdr, header_block, sizeof(hdr));

    if (hdr.magic != JOURNAL_MAGIC) return;   /* never used yet (e.g. freshly mkfs'd) */

    if (hdr.pending) {
        uint8_t before_image[BLOCK_SIZE];
        printf("journal: detected incomplete write to block %u from a prior crash - rolling back\n",
               hdr.target_block);
        if (raw_read_block(JOURNAL_START_BLOCK + 1, before_image) == 0) {
            raw_write_block(hdr.target_block, before_image);
        }
        hdr.pending = 0;
        memcpy(header_block, &hdr, sizeof(hdr));
        raw_write_block(JOURNAL_START_BLOCK, header_block);
        printf("journal: recovery complete, block %u restored to its last consistent state\n",
               hdr.target_block);
    }
}
