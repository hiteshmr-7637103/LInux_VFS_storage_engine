#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>

/*
 * Phase 6: crash recovery (distinct from fsck consistency validation).
 *
 * fsck (fsck.c) scans the disk AFTER the fact and repairs inconsistencies.
 * This journal instead PREVENTS corruption from an interrupted write: before
 * a critical metadata block (superblock / bitmaps) is overwritten, its
 * before-image is logged here. If the process crashes mid-write, the next
 * mount replays the journal and rolls the block back to a known-good state
 * instead of leaving it half-written.
 */

void journal_protected_write(uint32_t target_block, const void *new_data);
void journal_recover(void);   /* call once, right after disk_open, before any other I/O */

#endif /* JOURNAL_H */
