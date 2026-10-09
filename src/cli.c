#include "vfs.h"
#include "bitmap.h"
#include "fsck.h"
#include "cache.h"
#include "inode.h"
#include "vfs_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_DISK_PATH "virtual_disk.img"
#define MAX_LINE 1024

static int g_mounted = 0;

static double elapsed_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1e6;
}

static const char *type_str(uint8_t t) {
    switch (t) {
        case INODE_TYPE_DIR: return "dir";
        case INODE_TYPE_FILE: return "file";
        default: return "free";
    }
}

static void ls_cb(const char *name, uint32_t inode_num, uint8_t type, void *ctx) {
    (void)ctx;
    printf("  %-20s inode=%-5u %s\n", name, inode_num, type_str(type));
}

static void ensure_mounted(void) {
    if (!g_mounted) {
        printf("not mounted - run 'format' first, or the disk image doesn't exist yet\n");
    }
}

static void cmd_format(const char *path) {
    if (g_mounted) { vfs_unmount(); g_mounted = 0; }
    if (mkfs(path) != 0) { printf("format failed\n"); return; }
    if (vfs_mount(path) == 0) g_mounted = 1;
}

static void cmd_df(void) {
    superblock_t sb;
    if (load_superblock(&sb) != 0) { printf("df: cannot read superblock\n"); return; }
    uint32_t used_blocks = sb.total_blocks - sb.free_blocks;
    double util_pct = 100.0 * used_blocks / sb.total_blocks;
    uint32_t used_inodes = sb.total_inodes - sb.free_inodes;
    double inode_pct = 100.0 * used_inodes / sb.total_inodes;
    printf("Blocks: %u/%u used (%.1f%% utilized), %u free\n",
           used_blocks, sb.total_blocks, util_pct, sb.free_blocks);
    printf("Inodes: %u/%u used (%.1f%% utilized), %u free\n",
           used_inodes, sb.total_inodes, inode_pct, sb.free_inodes);
    printf("Block size: %u bytes | Total capacity: %.2f MB\n",
           sb.block_size, (sb.total_blocks * (double)sb.block_size) / (1024.0 * 1024.0));
}

static void cmd_perf(void) {
    cache_stats_t s = cache_get_stats();
    uint64_t total = s.hits + s.misses;
    double hit_rate = total ? (100.0 * s.hits / total) : 0.0;
    printf("Cache: %llu hits, %llu misses, %llu evictions (hit rate %.1f%%)\n",
           (unsigned long long)s.hits, (unsigned long long)s.misses,
           (unsigned long long)s.evictions, hit_rate);
}

/* allocation efficiency: wasted space from indirect-pointer index blocks
 * vs. raw data blocks across every live file/dir on disk */
static void cmd_allocstats(void) {
    superblock_t sb;
    if (load_superblock(&sb) != 0) return;
    uint32_t index_blocks = 0, data_blocks = 0;
    for (uint32_t i = 0; i < sb.total_inodes; i++) {
        inode_t inode;
        if (read_inode(i, &inode) != 0) continue;
        if (inode.type == INODE_TYPE_FREE) continue;
        for (int d = 0; d < DIRECT_POINTERS; d++) if (inode.direct[d]) data_blocks++;
        if (inode.single_indirect) { index_blocks++; }
        if (inode.double_indirect) { index_blocks++; }
    }
    uint32_t total_used = index_blocks + data_blocks;
    double frag_pct = total_used ? (100.0 * index_blocks / total_used) : 0.0;
    printf("Allocation efficiency: %u data blocks, %u index(indirect) blocks\n", data_blocks, index_blocks);
    printf("Overhead ratio: %.2f%% of allocated blocks are index overhead, not file data\n", frag_pct);
}

static void cmd_write(char *path, char *rest) {
    char *offset_str = strtok(rest, " ");
    char *content = strtok(NULL, "");
    if (!offset_str || !content) { printf("usage: write <path> <offset> <text...>\n"); return; }
    off_t offset = atol(offset_str);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ssize_t n = vfs_write(path, content, strlen(content), offset);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (n < 0) { printf("write failed: %s\n", vfs_last_error()); return; }
    double ms = elapsed_ms(&t0, &t1);
    double throughput = ms > 0 ? (n / 1024.0) / (ms / 1000.0) : 0.0;
    printf("wrote %zd bytes in %.3f ms (%.1f KB/s)\n", n, ms, throughput);
}

static void cmd_cat(const char *path) {
    inode_t st;
    if (vfs_stat(path, &st) != 0) { printf("cat: %s\n", vfs_last_error()); return; }
    char *buf = malloc(st.size + 1);
    if (!buf) return;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ssize_t n = vfs_read(path, buf, st.size, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (n < 0) { printf("cat: %s\n", vfs_last_error()); free(buf); return; }
    buf[n] = '\0';
    printf("%s\n", buf);
    double ms = elapsed_ms(&t0, &t1);
    double throughput = ms > 0 ? (n / 1024.0) / (ms / 1000.0) : 0.0;
    printf("[read %zd bytes in %.3f ms, %.1f KB/s]\n", n, ms, throughput);
    free(buf);
}

static void cmd_stat(const char *path) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    inode_t st;
    int rc = vfs_stat(path, &st);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc != 0) { printf("stat: %s\n", vfs_last_error()); return; }
    printf("type: %s\n", type_str(st.type));
    printf("size: %u bytes\n", st.size);
    printf("mode: %c%c%c (owner)\n",
           (st.mode & PERM_OWNER_READ) ? 'r' : '-',
           (st.mode & PERM_OWNER_WRITE) ? 'w' : '-',
           (st.mode & PERM_OWNER_EXEC) ? 'x' : '-');
    printf("uid: %u\n", st.uid);
    printf("blocks: %u\n", st.block_count);
    printf("links: %u\n", st.links_count);
    printf("[directory lookup took %.4f ms]\n", elapsed_ms(&t0, &t1));
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  format <diskfile>            create/format a new virtual disk\n"
        "  ls <path>                    list directory contents\n"
        "  mkdir <path>                 create a directory (parent must exist)\n"
        "  touch <path>                 create an empty file\n"
        "  write <path> <off> <text>    write text at byte offset\n"
        "  cat <path>                   print file contents\n"
        "  rm <path>                    delete a file\n"
        "  rmdir <path>                 delete an empty directory\n"
        "  stat <path>                  show inode metadata\n"
        "  chmod <path> <0-7>           set owner rwx bits\n"
        "  chown <path> <uid>           change owner uid\n"
        "  fsck [fix]                   run consistency check (optionally repair)\n"
        "  df                           show storage utilization\n"
        "  allocstats                   show allocation efficiency\n"
        "  perf                         show block cache hit/miss stats\n"
        "  help                         show this message\n"
        "  exit                         flush and quit\n");
}

int main(int argc, char **argv) {
    const char *disk_path = (argc > 1) ? argv[1] : DEFAULT_DISK_PATH;

    FILE *f = fopen(disk_path, "rb");
    if (f) {
        fclose(f);
        if (vfs_mount(disk_path) == 0) g_mounted = 1;
        else printf("warning: found %s but could not mount it - use 'format %s' to recreate\n", disk_path, disk_path);
    } else {
        printf("No disk image found at '%s'. Run 'format %s' to create one.\n", disk_path, disk_path);
    }

    print_help();

    char line[MAX_LINE];
    while (1) {
        printf("vfs> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;

        char linecopy[MAX_LINE];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';
        char *cmd = strtok(linecopy, " ");
        if (!cmd) continue;

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "format") == 0) {
            char *p = strtok(NULL, " ");
            cmd_format(p ? p : disk_path);
        } else if (strcmp(cmd, "ls") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (vfs_listdir(p ? p : "/", ls_cb, NULL) != 0) printf("ls: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "mkdir") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: mkdir <path>\n"); continue; }
            if (vfs_mkdir(p) != 0) printf("mkdir: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "rmdir") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: rmdir <path>\n"); continue; }
            if (vfs_rmdir(p) != 0) printf("rmdir: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "touch") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: touch <path>\n"); continue; }
            if (vfs_create(p) != 0) printf("touch: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "rm") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: rm <path>\n"); continue; }
            if (vfs_unlink(p) != 0) printf("rm: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "cat") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: cat <path>\n"); continue; }
            cmd_cat(p);
        } else if (strcmp(cmd, "write") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *rest = strtok(NULL, "");
            if (!p || !rest) { printf("usage: write <path> <offset> <text>\n"); continue; }
            cmd_write(p, rest);
        } else if (strcmp(cmd, "stat") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: stat <path>\n"); continue; }
            cmd_stat(p);
        } else if (strcmp(cmd, "chmod") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *m = strtok(NULL, " ");
            if (!p || !m) { printf("usage: chmod <path> <0-7>\n"); continue; }
            if (vfs_chmod(p, (uint16_t)atoi(m)) != 0) printf("chmod: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "chown") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *u = strtok(NULL, " ");
            if (!p || !u) { printf("usage: chown <path> <uid>\n"); continue; }
            if (vfs_chown(p, (uint32_t)atoi(u)) != 0) printf("chown: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "fsck") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *mode = strtok(NULL, " ");
            fsck_check(mode && strcmp(mode, "fix") == 0);
        } else if (strcmp(cmd, "df") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            cmd_df();
        } else if (strcmp(cmd, "allocstats") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            cmd_allocstats();
        } else if (strcmp(cmd, "perf") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            cmd_perf();
        } else {
            printf("unknown command '%s' (type 'help')\n", cmd);
        }
    }

    if (g_mounted) {
        vfs_unmount();
        printf("filesystem cleanly unmounted\n");
    }
    return 0;
}
