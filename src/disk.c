#include "disk.h"
#include "vfs_types.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static int g_disk_fd = -1;

int disk_create(const char *path, uint32_t total_blocks, uint32_t block_size) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("disk_create: open");
        return -1;
    }
    off_t total_size = (off_t)total_blocks * (off_t)block_size;
    if (ftruncate(fd, total_size) != 0) {
        perror("disk_create: ftruncate");
        close(fd);
        return -1;
    }
    g_disk_fd = fd;
    return 0;
}

int disk_open(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("disk_open: open");
        return -1;
    }
    g_disk_fd = fd;
    return 0;
}

void disk_close(void) {
    if (g_disk_fd >= 0) {
        close(g_disk_fd);
        g_disk_fd = -1;
    }
}

int raw_read_block(uint32_t block_num, void *buffer) {
    if (g_disk_fd < 0) return -1;
    off_t offset = (off_t)block_num * (off_t)BLOCK_SIZE;
    if (lseek(g_disk_fd, offset, SEEK_SET) < 0) {
        perror("raw_read_block: lseek");
        return -1;
    }
    ssize_t n = read(g_disk_fd, buffer, BLOCK_SIZE);
    if (n < 0) {
        perror("raw_read_block: read");
        return -1;
    }
    if ((size_t)n < BLOCK_SIZE) {
        memset((char *)buffer + n, 0, BLOCK_SIZE - n);
    }
    return 0;
}

int raw_write_block(uint32_t block_num, const void *buffer) {
    if (g_disk_fd < 0) return -1;
    off_t offset = (off_t)block_num * (off_t)BLOCK_SIZE;
    if (lseek(g_disk_fd, offset, SEEK_SET) < 0) {
        perror("raw_write_block: lseek");
        return -1;
    }
    ssize_t n = write(g_disk_fd, buffer, BLOCK_SIZE);
    if (n < 0 || (size_t)n != BLOCK_SIZE) {
        perror("raw_write_block: write");
        return -1;
    }
    return 0;
}
