#ifndef FSCK_H
#define FSCK_H

/* Phase 6: consistency validation (fsck-like). Scans the mounted disk and
 * reports/repairs: bitmap-vs-inode mismatches, orphaned inodes, and
 * double-allocated blocks. This is a point-in-time scan-and-repair tool -
 * it is deliberately separate from journal.c's crash recovery, which
 * instead prevents corruption during an interrupted write. Returns the
 * number of problems found (0 = clean). */
int fsck_check(int fix);

#endif /* FSCK_H */
