#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <sys/types.h>
#include "vfs_types.h"

/* Phase 5: directory structure, path resolution, file/dir CRUD, permissions. */

int vfs_mount(const char *disk_path);
void vfs_unmount(void);

int vfs_mkdir(const char *path);
int vfs_create(const char *path);
ssize_t vfs_read(const char *path, void *buf, size_t size, off_t offset);
ssize_t vfs_write(const char *path, const void *buf, size_t size, off_t offset);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);

int vfs_stat(const char *path, inode_t *out_inode);
int vfs_chmod(const char *path, uint16_t mode);
int vfs_chown(const char *path, uint32_t uid);

/* calls cb(name, inode_num, type) for every entry in the directory at path (except . and ..) */
typedef void (*vfs_dirent_cb)(const char *name, uint32_t inode_num, uint8_t type, void *ctx);
int vfs_listdir(const char *path, vfs_dirent_cb cb, void *ctx);

const char *vfs_last_error(void);

#endif /* VFS_H */
