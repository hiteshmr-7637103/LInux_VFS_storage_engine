#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <sys/types.h>
#include "vfs_types.h"

/* Phase 5: directory structure, path resolution, file/dir CRUD, permissions. */

int vfs_mount(const char *disk_path);
void vfs_unmount(void);

/* current-working-directory (relative paths elsewhere resolve against this) */
int vfs_chdir(const char *path);
void vfs_getcwd(char *buf, size_t size);

int vfs_mkdir(const char *path);
int vfs_mkdir_p(const char *path);      /* create any missing intermediate directories too */
int vfs_create(const char *path);
ssize_t vfs_read(const char *path, void *buf, size_t size, off_t offset);
ssize_t vfs_write(const char *path, const void *buf, size_t size, off_t offset);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_remove_recursive(const char *path);   /* rm -r: file or directory tree */

int vfs_copy(const char *src, const char *dst);      /* cp (regular files only) */
int vfs_move(const char *src, const char *dst);      /* mv (files and directories) */

int vfs_link(const char *target, const char *linkpath);     /* ln: hard link */
int vfs_symlink(const char *target, const char *linkpath);  /* ln -s: symbolic link */
ssize_t vfs_readlink(const char *path, char *buf, size_t size);

/* process-wide umask (3-bit rwx mask), applied to new file/dir creation. Returns previous value. */
uint16_t vfs_umask(uint16_t new_mask);
uint16_t vfs_get_umask(void);

int vfs_stat(const char *path, inode_t *out_inode);     /* follows symlinks */
int vfs_lstat(const char *path, inode_t *out_inode);    /* does not follow the final symlink */
int vfs_chmod(const char *path, uint16_t mode);
int vfs_chown(const char *path, uint32_t uid);

/* calls cb(name, inode_num, type) for every entry in the directory at path (except . and ..) */
typedef void (*vfs_dirent_cb)(const char *name, uint32_t inode_num, uint8_t type, void *ctx);
int vfs_listdir(const char *path, vfs_dirent_cb cb, void *ctx);

/* recursively sums block_count*BLOCK_SIZE for path (file, or directory tree) - like `du` */
int vfs_disk_usage(const char *path, uint64_t *out_bytes);

const char *vfs_last_error(void);

#endif /* VFS_H */
