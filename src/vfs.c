#include "vfs.h"
#include "disk.h"
#include "cache.h"
#include "bitmap.h"
#include "inode.h"
#include "journal.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#define ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(dir_entry_t))

static char g_last_error[256] = "";
static void set_error(const char *msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}
const char *vfs_last_error(void) { return g_last_error; }

/* current-working-directory state (per mounted session, mirrors a shell's $PWD) */
#define MAX_CWD_DEPTH 32
#define MAX_CWD_LEN 1024
static uint32_t g_cwd_inode = ROOT_INODE_NUM;
static char g_cwd_components[MAX_CWD_DEPTH][NAME_MAX_LEN];
static int g_cwd_depth = 0;

static void rebuild_cwd_string(char *buf, size_t size) {
    if (g_cwd_depth == 0) { snprintf(buf, size, "/"); return; }
    size_t pos = 0;
    for (int i = 0; i < g_cwd_depth; i++) {
        int n = snprintf(buf + pos, size - pos, "/%s", g_cwd_components[i]);
        if (n < 0 || (size_t)n >= size - pos) break;
        pos += (size_t)n;
    }
}

void vfs_getcwd(char *buf, size_t size) {
    rebuild_cwd_string(buf, size);
}

int vfs_mount(const char *disk_path) {
    if (disk_open(disk_path) != 0) {
        set_error("cannot open disk image");
        return -1;
    }
    journal_recover();              /* Phase 6: crash recovery, before anything else touches the disk */
    cache_init();                   /* Phase 6: fresh block cache for this session */
    superblock_t sb;
    if (load_superblock(&sb) != 0) {
        set_error("invalid filesystem (bad magic) - run format first");
        return -1;
    }
    sb.clean_shutdown = 0;          /* mark dirty while mounted; set back to 1 on clean unmount */
    save_superblock(&sb);

    g_cwd_inode = ROOT_INODE_NUM;
    g_cwd_depth = 0;
    return 0;
}

void vfs_unmount(void) {
    cache_flush();
    superblock_t sb;
    if (load_superblock(&sb) == 0) {
        sb.clean_shutdown = 1;
        save_superblock(&sb);
    }
    disk_close();
}

/* ---------- permission helpers ---------- */

static int check_access(const inode_t *inode, uint16_t need) {
    uid_t me = getuid();
    if (me == 0 || me == inode->uid) {
        if ((inode->mode & need) == need) return 0;
    }
    set_error("permission denied");
    return -1;
}

/* ---------- directory payload helpers ---------- */

static int dir_find_entry(const inode_t *dir, const char *name, uint32_t *out_inode_num) {
    uint32_t n_blocks = (dir->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t phys;
        inode_t tmp = *dir;
        if (inode_get_block(&tmp, bi, 0, &phys) != 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(phys, buf) != 0) continue;
        dir_entry_t *entries = (dir_entry_t *)buf;
        for (uint32_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].inode_num != 0 && strcmp(entries[e].name, name) == 0) {
                *out_inode_num = entries[e].inode_num;
                return 0;
            }
        }
    }
    return -1;
}

static int dir_add_entry(inode_t *dir, uint32_t dir_inode_num, const char *name, uint32_t new_inode_num) {
    uint32_t n_blocks = (dir->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t phys;
        if (inode_get_block(dir, bi, 0, &phys) != 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(phys, buf) != 0) continue;
        dir_entry_t *entries = (dir_entry_t *)buf;
        for (uint32_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].inode_num == 0) {
                entries[e].inode_num = new_inode_num;
                entries[e].name_len = (uint8_t)strlen(name);
                strncpy(entries[e].name, name, NAME_MAX_LEN - 1);
                cache_write_block(phys, buf);
                write_inode(dir_inode_num, dir);
                return 0;
            }
        }
    }
    /* no free slot: grow the directory by one block */
    uint32_t phys;
    if (inode_get_block(dir, n_blocks, 1, &phys) != 0) return -1;
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    dir_entry_t *entries = (dir_entry_t *)buf;
    entries[0].inode_num = new_inode_num;
    entries[0].name_len = (uint8_t)strlen(name);
    strncpy(entries[0].name, name, NAME_MAX_LEN - 1);
    cache_write_block(phys, buf);
    dir->size = (n_blocks + 1) * BLOCK_SIZE;
    write_inode(dir_inode_num, dir);
    return 0;
}

static int dir_remove_entry(inode_t *dir, const char *name) {
    uint32_t n_blocks = (dir->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t phys;
        if (inode_get_block(dir, bi, 0, &phys) != 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(phys, buf) != 0) continue;
        dir_entry_t *entries = (dir_entry_t *)buf;
        for (uint32_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].inode_num != 0 && strcmp(entries[e].name, name) == 0) {
                memset(&entries[e], 0, sizeof(dir_entry_t));
                cache_write_block(phys, buf);
                return 0;
            }
        }
    }
    return -1;
}

static int dir_is_empty(const inode_t *dir) {
    uint32_t n_blocks = (dir->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t phys;
        inode_t tmp = *dir;
        if (inode_get_block(&tmp, bi, 0, &phys) != 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(phys, buf) != 0) continue;
        dir_entry_t *entries = (dir_entry_t *)buf;
        for (uint32_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].inode_num == 0) continue;
            if (strcmp(entries[e].name, ".") == 0 || strcmp(entries[e].name, "..") == 0) continue;
            return 0;
        }
    }
    return 1;
}

/* ---------- path resolution ----------
 * Scope decision (documented in roadmap): only the leaf component is
 * created by mkdir/create; every parent directory in the path must already
 * exist (no recursive "mkdir -p" semantics). "." and ".." are resolved via
 * the real directory entries written at mkdir/mkfs time. */

static int split_path(const char *path, char components[][NAME_MAX_LEN], int max_components) {
    int count = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p && count < max_components) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len > 0) {
            if (len >= NAME_MAX_LEN) len = NAME_MAX_LEN - 1;
            memcpy(components[count], start, len);
            components[count][len] = '\0';
            count++;
        }
        while (*p == '/') p++;
    }
    return count;
}

#define MAX_PATH_COMPONENTS 32

/* resolves full path to its inode number; relative paths (no leading '/') start from cwd */
static int resolve_path(const char *path, uint32_t *out_inode_num) {
    char comps[MAX_PATH_COMPONENTS][NAME_MAX_LEN];
    int n = split_path(path, comps, MAX_PATH_COMPONENTS);
    uint32_t cur = (path[0] == '/') ? ROOT_INODE_NUM : g_cwd_inode;
    if (n == 0) { *out_inode_num = cur; return 0; }
    for (int i = 0; i < n; i++) {
        inode_t cur_inode;
        if (read_inode(cur, &cur_inode) != 0) { set_error("I/O error reading inode"); return -1; }
        if (cur_inode.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
        uint32_t next;
        if (dir_find_entry(&cur_inode, comps[i], &next) != 0) {
            set_error("no such file or directory");
            return -1;
        }
        cur = next;
    }
    *out_inode_num = cur;
    return 0;
}

/* resolves the parent directory of the last component; leaf_name receives the last component */
static int resolve_parent(const char *path, uint32_t *out_parent_inode_num, char *leaf_name) {
    char comps[MAX_PATH_COMPONENTS][NAME_MAX_LEN];
    int n = split_path(path, comps, MAX_PATH_COMPONENTS);
    if (n == 0) { set_error("invalid path"); return -1; }
    uint32_t cur = (path[0] == '/') ? ROOT_INODE_NUM : g_cwd_inode;
    for (int i = 0; i < n - 1; i++) {
        inode_t cur_inode;
        if (read_inode(cur, &cur_inode) != 0) { set_error("I/O error reading inode"); return -1; }
        if (cur_inode.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
        uint32_t next;
        if (dir_find_entry(&cur_inode, comps[i], &next) != 0) {
            set_error("parent directory does not exist");
            return -1;
        }
        cur = next;
    }
    *out_parent_inode_num = cur;
    strncpy(leaf_name, comps[n - 1], NAME_MAX_LEN - 1);
    return 0;
}

int vfs_chdir(const char *path) {
    char comps[MAX_PATH_COMPONENTS][NAME_MAX_LEN];
    int n = split_path(path, comps, MAX_PATH_COMPONENTS);

    uint32_t cur = (path[0] == '/') ? ROOT_INODE_NUM : g_cwd_inode;
    char new_stack[MAX_CWD_DEPTH][NAME_MAX_LEN];
    int new_depth;
    if (path[0] == '/') {
        new_depth = 0;
    } else {
        new_depth = g_cwd_depth;
        memcpy(new_stack, g_cwd_components, sizeof(new_stack));
    }

    for (int i = 0; i < n; i++) {
        inode_t cur_inode;
        if (read_inode(cur, &cur_inode) != 0) { set_error("I/O error reading inode"); return -1; }
        if (cur_inode.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
        if (check_access(&cur_inode, PERM_OWNER_EXEC) != 0) return -1;

        if (strcmp(comps[i], ".") == 0) {
            continue;
        } else if (strcmp(comps[i], "..") == 0) {
            uint32_t parent;
            if (dir_find_entry(&cur_inode, "..", &parent) != 0) { set_error("corrupt directory: missing .."); return -1; }
            cur = parent;
            if (new_depth > 0) new_depth--;
            continue;
        }

        uint32_t next;
        if (dir_find_entry(&cur_inode, comps[i], &next) != 0) {
            set_error("no such file or directory");
            return -1;
        }
        if (new_depth >= MAX_CWD_DEPTH) { set_error("path too deep"); return -1; }
        strncpy(new_stack[new_depth], comps[i], NAME_MAX_LEN - 1);
        new_stack[new_depth][NAME_MAX_LEN - 1] = '\0';
        new_depth++;
        cur = next;
    }

    inode_t final_inode;
    if (read_inode(cur, &final_inode) != 0) { set_error("I/O error reading inode"); return -1; }
    if (final_inode.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
    if (check_access(&final_inode, PERM_OWNER_EXEC) != 0) return -1;

    g_cwd_inode = cur;
    g_cwd_depth = new_depth;
    memcpy(g_cwd_components, new_stack, sizeof(g_cwd_components));
    return 0;
}

/* ---------- public API ---------- */

int vfs_mkdir(const char *path) {
    uint32_t parent_num;
    char leaf[NAME_MAX_LEN];
    if (resolve_parent(path, &parent_num, leaf) != 0) return -1;

    inode_t parent;
    if (read_inode(parent_num, &parent) != 0) return -1;
    if (parent.type != INODE_TYPE_DIR) { set_error("parent is not a directory"); return -1; }
    if (check_access(&parent, PERM_OWNER_WRITE) != 0) return -1;

    uint32_t existing;
    if (dir_find_entry(&parent, leaf, &existing) == 0) { set_error("already exists"); return -1; }

    uint32_t new_inode_num;
    if (allocate_inode(&new_inode_num) != 0) { set_error("no free inodes"); return -1; }

    uint32_t data_block;
    if (allocate_block(&data_block) != 0) { set_error("disk full"); return -1; }

    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    dir_entry_t *entries = (dir_entry_t *)buf;
    entries[0].inode_num = new_inode_num; entries[0].name_len = 1; strcpy(entries[0].name, ".");
    entries[1].inode_num = parent_num;    entries[1].name_len = 2; strcpy(entries[1].name, "..");
    cache_write_block(data_block, buf);

    inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode = PERM_DEFAULT_DIR;
    new_inode.type = INODE_TYPE_DIR;
    new_inode.links_count = 2;
    new_inode.uid = getuid();
    new_inode.size = BLOCK_SIZE;
    new_inode.block_count = 1;
    new_inode.direct[0] = data_block;
    new_inode.ctime = new_inode.mtime = (uint32_t)time(NULL);
    write_inode(new_inode_num, &new_inode);

    return dir_add_entry(&parent, parent_num, leaf, new_inode_num);
}

int vfs_rmdir(const char *path) {
    uint32_t parent_num;
    char leaf[NAME_MAX_LEN];
    if (resolve_parent(path, &parent_num, leaf) != 0) return -1;
    inode_t parent;
    if (read_inode(parent_num, &parent) != 0) return -1;
    if (check_access(&parent, PERM_OWNER_WRITE) != 0) return -1;

    uint32_t target_num;
    if (dir_find_entry(&parent, leaf, &target_num) != 0) { set_error("no such directory"); return -1; }
    inode_t target;
    if (read_inode(target_num, &target) != 0) return -1;
    if (target.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
    if (!dir_is_empty(&target)) { set_error("directory not empty"); return -1; }

    inode_free_all_blocks(&target);
    target.type = INODE_TYPE_FREE;
    write_inode(target_num, &target);
    free_inode(target_num);
    return dir_remove_entry(&parent, leaf);
}

int vfs_create(const char *path) {
    uint32_t parent_num;
    char leaf[NAME_MAX_LEN];
    if (resolve_parent(path, &parent_num, leaf) != 0) return -1;

    inode_t parent;
    if (read_inode(parent_num, &parent) != 0) return -1;
    if (parent.type != INODE_TYPE_DIR) { set_error("parent is not a directory"); return -1; }
    if (check_access(&parent, PERM_OWNER_WRITE) != 0) return -1;

    uint32_t existing;
    if (dir_find_entry(&parent, leaf, &existing) == 0) return 0; /* already exists: no-op, like touch */

    uint32_t new_inode_num;
    if (allocate_inode(&new_inode_num) != 0) { set_error("no free inodes"); return -1; }

    inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode = PERM_DEFAULT_FILE;
    new_inode.type = INODE_TYPE_FILE;
    new_inode.links_count = 1;
    new_inode.uid = getuid();
    new_inode.ctime = new_inode.mtime = (uint32_t)time(NULL);
    write_inode(new_inode_num, &new_inode);

    return dir_add_entry(&parent, parent_num, leaf, new_inode_num);
}

ssize_t vfs_read(const char *path, void *buf, size_t size, off_t offset) {
    if (offset < 0) { set_error("negative offset"); return -1; }

    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    inode_t inode;
    if (read_inode(inode_num, &inode) != 0) return -1;
    if (inode.type != INODE_TYPE_FILE) { set_error("not a regular file"); return -1; }
    if (check_access(&inode, PERM_OWNER_READ) != 0) return -1;

    if ((uint64_t)offset >= inode.size) return 0;
    if (offset + (off_t)size > (off_t)inode.size) size = inode.size - offset;

    size_t total_read = 0;
    uint8_t block_buf[BLOCK_SIZE];
    while (total_read < size) {
        uint32_t logical = (uint32_t)((offset + (off_t)total_read) / BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((offset + (off_t)total_read) % BLOCK_SIZE);
        uint32_t phys;
        if (inode_get_block(&inode, logical, 0, &phys) != 0) break;
        if (cache_read_block(phys, block_buf) != 0) break;
        size_t chunk = BLOCK_SIZE - block_off;
        if (chunk > size - total_read) chunk = size - total_read;
        memcpy((uint8_t *)buf + total_read, block_buf + block_off, chunk);
        total_read += chunk;
    }
    return (ssize_t)total_read;
}

ssize_t vfs_write(const char *path, const void *buf, size_t size, off_t offset) {
    if (offset < 0) { set_error("negative offset"); return -1; }

    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    inode_t inode;
    if (read_inode(inode_num, &inode) != 0) return -1;
    if (inode.type != INODE_TYPE_FILE) { set_error("not a regular file"); return -1; }
    if (check_access(&inode, PERM_OWNER_WRITE) != 0) return -1;

    size_t total_written = 0;
    uint8_t block_buf[BLOCK_SIZE];
    while (total_written < size) {
        uint32_t logical = (uint32_t)((offset + (off_t)total_written) / BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((offset + (off_t)total_written) % BLOCK_SIZE);
        uint32_t phys;
        if (inode_get_block(&inode, logical, 1, &phys) != 0) { set_error("disk full"); break; }
        if (block_off != 0 || (size - total_written) < BLOCK_SIZE) {
            cache_read_block(phys, block_buf); /* partial-block write: preserve existing bytes */
        }
        size_t chunk = BLOCK_SIZE - block_off;
        if (chunk > size - total_written) chunk = size - total_written;
        memcpy(block_buf + block_off, (const uint8_t *)buf + total_written, chunk);
        cache_write_block(phys, block_buf);
        total_written += chunk;
    }

    uint64_t end_offset = (uint64_t)offset + total_written;
    if (end_offset > inode.size) inode.size = (uint32_t)end_offset;
    inode.mtime = (uint32_t)time(NULL);
    write_inode(inode_num, &inode);
    return (ssize_t)total_written;
}

int vfs_unlink(const char *path) {
    uint32_t parent_num;
    char leaf[NAME_MAX_LEN];
    if (resolve_parent(path, &parent_num, leaf) != 0) return -1;
    inode_t parent;
    if (read_inode(parent_num, &parent) != 0) return -1;
    if (check_access(&parent, PERM_OWNER_WRITE) != 0) return -1;

    uint32_t target_num;
    if (dir_find_entry(&parent, leaf, &target_num) != 0) { set_error("no such file"); return -1; }
    inode_t target;
    if (read_inode(target_num, &target) != 0) return -1;
    if (target.type != INODE_TYPE_FILE) { set_error("not a regular file (use rmdir)"); return -1; }

    inode_free_all_blocks(&target);
    target.type = INODE_TYPE_FREE;
    write_inode(target_num, &target);
    free_inode(target_num);
    return dir_remove_entry(&parent, leaf);
}

int vfs_stat(const char *path, inode_t *out_inode) {
    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    return read_inode(inode_num, out_inode);
}

int vfs_chmod(const char *path, uint16_t mode) {
    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    inode_t inode;
    if (read_inode(inode_num, &inode) != 0) return -1;
    if (getuid() != 0 && getuid() != inode.uid) { set_error("permission denied: not the owner"); return -1; }
    inode.mode = mode & 0x7;
    return write_inode(inode_num, &inode);
}

int vfs_chown(const char *path, uint32_t uid) {
    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    inode_t inode;
    if (read_inode(inode_num, &inode) != 0) return -1;
    if (getuid() != 0 && getuid() != inode.uid) { set_error("permission denied: not the owner"); return -1; }
    inode.uid = uid;
    return write_inode(inode_num, &inode);
}

int vfs_listdir(const char *path, vfs_dirent_cb cb, void *ctx) {
    uint32_t inode_num;
    if (resolve_path(path, &inode_num) != 0) return -1;
    inode_t dir;
    if (read_inode(inode_num, &dir) != 0) return -1;
    if (dir.type != INODE_TYPE_DIR) { set_error("not a directory"); return -1; }
    if (check_access(&dir, PERM_OWNER_READ) != 0) return -1;

    uint32_t n_blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t phys;
        if (inode_get_block(&dir, bi, 0, &phys) != 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (cache_read_block(phys, buf) != 0) continue;
        dir_entry_t *entries = (dir_entry_t *)buf;
        for (uint32_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].inode_num == 0) continue;
            if (strcmp(entries[e].name, ".") == 0 || strcmp(entries[e].name, "..") == 0) continue;
            inode_t child;
            uint8_t type = INODE_TYPE_FILE;
            if (read_inode(entries[e].inode_num, &child) == 0) type = child.type;
            cb(entries[e].name, entries[e].inode_num, type, ctx);
        }
    }
    return 0;
}
