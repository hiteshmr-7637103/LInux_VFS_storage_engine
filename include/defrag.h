#ifndef DEFRAG_H
#define DEFRAG_H

/*
 * Compacts every in-use data/index block (direct, single- and
 * double-indirect) into a contiguous run starting right after the
 * filesystem metadata region, rewriting every inode's block pointers to
 * match. Reduces the "allocation efficiency" overhead reported by
 * `allocstats` and leaves maximal contiguous free space at the end of the
 * disk. Returns 0 on success, -1 on error.
 */
int defrag_run(void);

#endif /* DEFRAG_H */
