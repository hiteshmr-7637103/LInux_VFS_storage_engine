#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stddef.h>

/* Phase 2: low-level block I/O on top of a disk-image file using POSIX syscalls. */

int disk_create(const char *path, uint32_t total_blocks, uint32_t block_size);
int disk_open(const char *path);
void disk_close(void);

int raw_read_block(uint32_t block_num, void *buffer);
int raw_write_block(uint32_t block_num, const void *buffer);

#endif /* DISK_H */
