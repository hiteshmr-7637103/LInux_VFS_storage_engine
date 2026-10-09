#include "vfs.h"
#include "bitmap.h"
#include "fsck.h"
#include "cache.h"
#include "inode.h"
#include "defrag.h"
#include "vfs_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_DISK_PATH "virtual_disk.img"
#define MAX_LINE 1024
#define MAX_CWD_PRINT 1024

static int g_mounted = 0;

static double elapsed_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1e6;
}

static const char *type_str(uint8_t t) {
    switch (t) {
        case INODE_TYPE_DIR: return "dir";
        case INODE_TYPE_FILE: return "file";
        case INODE_TYPE_SYMLINK: return "symlink";
        default: return "free";
    }
}

static void human_size(uint64_t bytes, char *buf, size_t bufsize) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(buf, bufsize, "%.0f%s", v, units[u]);
    else snprintf(buf, bufsize, "%.1f%s", v, units[u]);
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

static void cmd_df(int human) {
    superblock_t sb;
    if (load_superblock(&sb) != 0) { printf("df: cannot read superblock\n"); return; }
    uint32_t used_blocks = sb.total_blocks - sb.free_blocks;
    double util_pct = 100.0 * used_blocks / sb.total_blocks;
    uint32_t used_inodes = sb.total_inodes - sb.free_inodes;
    double inode_pct = 100.0 * used_inodes / sb.total_inodes;

    if (human) {
        char used_s[32], total_s[32], free_s[32];
        human_size((uint64_t)used_blocks * sb.block_size, used_s, sizeof(used_s));
        human_size((uint64_t)sb.total_blocks * sb.block_size, total_s, sizeof(total_s));
        human_size((uint64_t)sb.free_blocks * sb.block_size, free_s, sizeof(free_s));
        printf("Blocks: %s/%s used (%.1f%% utilized), %s free\n", used_s, total_s, util_pct, free_s);
    } else {
        printf("Blocks: %u/%u used (%.1f%% utilized), %u free\n",
               used_blocks, sb.total_blocks, util_pct, sb.free_blocks);
    }
    printf("Inodes: %u/%u used (%.1f%% utilized), %u free\n",
           used_inodes, sb.total_inodes, inode_pct, sb.free_inodes);
    printf("Block size: %u bytes | Total capacity: %.2f MB\n",
           sb.block_size, (sb.total_blocks * (double)sb.block_size) / (1024.0 * 1024.0));
}

static void cmd_du(const char *path, int human) {
    uint64_t bytes;
    if (vfs_disk_usage(path, &bytes) != 0) { printf("du: %s\n", vfs_last_error()); return; }
    if (human) {
        char s[32];
        human_size(bytes, s, sizeof(s));
        printf("%s\t%s\n", s, path);
    } else {
        printf("%llu\t%s\n", (unsigned long long)bytes, path);
    }
}

static void cmd_size(const char *path) {
    inode_t st;
    if (vfs_stat(path, &st) != 0) { printf("size: %s\n", vfs_last_error()); return; }
    printf("%u bytes\n", st.size);
}

static void cmd_head(const char *path, int n) {
    inode_t st;
    if (vfs_stat(path, &st) != 0) { printf("head: %s\n", vfs_last_error()); return; }
    char *buf = malloc(st.size + 1);
    if (!buf) return;
    ssize_t r = vfs_read(path, buf, st.size, 0);
    if (r < 0) { printf("head: %s\n", vfs_last_error()); free(buf); return; }
    buf[r] = '\0';

    int lines = 0;
    for (ssize_t i = 0; i < r && lines < n; i++) {
        putchar(buf[i]);
        if (buf[i] == '\n') lines++;
    }
    if (r > 0 && buf[r - 1] != '\n' && lines < n) putchar('\n');
    free(buf);
}

static void cmd_tail(const char *path, int n) {
    inode_t st;
    if (vfs_stat(path, &st) != 0) { printf("tail: %s\n", vfs_last_error()); return; }
    char *buf = malloc(st.size + 1);
    if (!buf) return;
    ssize_t r = vfs_read(path, buf, st.size, 0);
    if (r < 0) { printf("tail: %s\n", vfs_last_error()); free(buf); return; }
    buf[r] = '\0';

    int total_lines = 0;
    for (ssize_t i = 0; i < r; i++) if (buf[i] == '\n') total_lines++;
    if (r > 0 && buf[r - 1] != '\n') total_lines++; /* count a trailing partial line too */

    int skip = total_lines - n;
    if (skip < 0) skip = 0;
    ssize_t start = 0;
    if (skip > 0) {
        int seen = 0;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                seen++;
                if (seen == skip) { start = i + 1; break; }
            }
        }
    }
    for (ssize_t i = start; i < r; i++) putchar(buf[i]);
    if (r > 0 && buf[r - 1] != '\n') putchar('\n');
    free(buf);
}

static void cmd_append(const char *path, const char *text) {
    inode_t st;
    if (vfs_stat(path, &st) != 0) { printf("append: %s\n", vfs_last_error()); return; }
    ssize_t n = vfs_write(path, text, strlen(text), st.size);
    if (n < 0) { printf("append: %s\n", vfs_last_error()); return; }
    printf("appended %zd bytes at offset %u\n", n, st.size);
}

static void cmd_cp(const char *src, const char *dst) {
    if (vfs_copy(src, dst) != 0) printf("cp: %s\n", vfs_last_error());
}

static void cmd_mv(const char *src, const char *dst) {
    if (vfs_move(src, dst) != 0) printf("mv: %s\n", vfs_last_error());
}

static void cmd_import(const char *host_path, const char *vfs_path) {
    FILE *f = fopen(host_path, "rb");
    if (!f) { printf("import: cannot open host file '%s'\n", host_path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); printf("import: cannot determine size of '%s'\n", host_path); return; }

    char *buf = malloc((size_t)size);
    if (size > 0 && (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size)) {
        fclose(f); free(buf);
        printf("import: read error on '%s'\n", host_path);
        return;
    }
    fclose(f);

    if (vfs_create(vfs_path) != 0) { printf("import: %s\n", vfs_last_error()); free(buf); return; }
    ssize_t n = (size > 0) ? vfs_write(vfs_path, buf, (size_t)size, 0) : 0;
    free(buf);
    if (n < 0) { printf("import: %s\n", vfs_last_error()); return; }
    printf("imported %ld bytes: %s -> %s\n", size, host_path, vfs_path);
}

static void cmd_export(const char *vfs_path, const char *host_path) {
    inode_t st;
    if (vfs_stat(vfs_path, &st) != 0) { printf("export: %s\n", vfs_last_error()); return; }
    char *buf = malloc(st.size ? st.size : 1);
    ssize_t n = (st.size > 0) ? vfs_read(vfs_path, buf, st.size, 0) : 0;
    if (n < 0) { printf("export: %s\n", vfs_last_error()); free(buf); return; }

    FILE *f = fopen(host_path, "wb");
    if (!f) { printf("export: cannot open host file '%s' for writing\n", host_path); free(buf); return; }
    if (n > 0) fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    free(buf);
    printf("exported %zd bytes: %s -> %s\n", n, vfs_path, host_path);
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
    int rc = vfs_lstat(path, &st);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc != 0) { printf("stat: %s\n", vfs_last_error()); return; }
    printf("type: %s\n", type_str(st.type));
    if (st.type == INODE_TYPE_SYMLINK) {
        char target[BLOCK_SIZE];
        ssize_t n = vfs_readlink(path, target, sizeof(target));
        if (n >= 0) printf("target: %s\n", target);
    }
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
        "  cd <path>                    change current directory (default: /)\n"
        "  pwd                          print current directory\n"
        "  ls [path]                    list directory contents (default: cwd)\n"
        "  mkdir [-p] <path>             create a directory (-p: create missing parents too)\n"
        "  touch <path>                 create an empty file\n"
        "  write <path> <off> <text>    write text at byte offset\n"
        "  append <path> <text>         append text at end of file\n"
        "  cat <path>                   print file contents\n"
        "  head <path> [n]              print first n lines (default 10)\n"
        "  tail <path> [n]              print last n lines (default 10)\n"
        "  cp <src> <dst>                copy a regular file\n"
        "  mv <src> <dst>                move/rename a file or directory\n"
        "  rm [-r] <path>                delete a file (-r: delete a directory tree)\n"
        "  rmdir <path>                  delete an empty directory\n"
        "  ln <target> <linkpath>        create a hard link\n"
        "  ln -s <target> <linkpath>     create a symbolic link\n"
        "  stat <path>                   show inode metadata (does not follow a final symlink)\n"
        "  size <path>                   print file size in bytes\n"
        "  chmod <path> <0-7>            set owner rwx bits\n"
        "  chown <path> <uid>            change owner uid\n"
        "  umask [0-7]                   show or set the default-permission mask\n"
        "  import <hostfile> <vfspath>   copy a file from the real filesystem in\n"
        "  export <vfspath> <hostfile>   copy a file out to the real filesystem\n"
        "  fsck [fix]                    run consistency check (optionally repair)\n"
        "  defrag                        compact fragmented blocks into one contiguous run\n"
        "  df [-h]                       show storage utilization\n"
        "  du [-h] <path>                show disk usage of a file/directory tree\n"
        "  allocstats                    show allocation efficiency\n"
        "  perf                          show block cache hit/miss stats\n"
        "  help                          show this message\n"
        "  exit                          flush and quit\n");
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
        char cwd[MAX_CWD_PRINT] = "/";
        if (g_mounted) vfs_getcwd(cwd, sizeof(cwd));
        printf("vfs:%s> ", cwd);
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
        } else if (strcmp(cmd, "cd") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (vfs_chdir(p ? p : "/") != 0) printf("cd: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "pwd") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char buf[MAX_CWD_PRINT];
            vfs_getcwd(buf, sizeof(buf));
            printf("%s\n", buf);
        } else if (strcmp(cmd, "ls") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (vfs_listdir(p ? p : ".", ls_cb, NULL) != 0) printf("ls: %s\n", vfs_last_error());
        } else if (strcmp(cmd, "mkdir") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *a1 = strtok(NULL, " ");
            if (!a1) { printf("usage: mkdir [-p] <path>\n"); continue; }
            if (strcmp(a1, "-p") == 0) {
                char *p = strtok(NULL, " ");
                if (!p) { printf("usage: mkdir -p <path>\n"); continue; }
                if (vfs_mkdir_p(p) != 0) printf("mkdir: %s\n", vfs_last_error());
            } else {
                if (vfs_mkdir(a1) != 0) printf("mkdir: %s\n", vfs_last_error());
            }
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
            char *a1 = strtok(NULL, " ");
            if (!a1) { printf("usage: rm [-r] <path>\n"); continue; }
            if (strcmp(a1, "-r") == 0) {
                char *p = strtok(NULL, " ");
                if (!p) { printf("usage: rm -r <path>\n"); continue; }
                if (vfs_remove_recursive(p) != 0) printf("rm: %s\n", vfs_last_error());
            } else {
                if (vfs_unlink(a1) != 0) printf("rm: %s\n", vfs_last_error());
            }
        } else if (strcmp(cmd, "cat") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: cat <path>\n"); continue; }
            cmd_cat(p);
        } else if (strcmp(cmd, "head") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *n = strtok(NULL, " ");
            if (!p) { printf("usage: head <path> [n]\n"); continue; }
            cmd_head(p, n ? atoi(n) : 10);
        } else if (strcmp(cmd, "tail") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *n = strtok(NULL, " ");
            if (!p) { printf("usage: tail <path> [n]\n"); continue; }
            cmd_tail(p, n ? atoi(n) : 10);
        } else if (strcmp(cmd, "append") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *text = strtok(NULL, "");
            if (!p || !text) { printf("usage: append <path> <text>\n"); continue; }
            cmd_append(p, text);
        } else if (strcmp(cmd, "cp") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *src = strtok(NULL, " ");
            char *dst = strtok(NULL, " ");
            if (!src || !dst) { printf("usage: cp <src> <dst>\n"); continue; }
            cmd_cp(src, dst);
        } else if (strcmp(cmd, "mv") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *src = strtok(NULL, " ");
            char *dst = strtok(NULL, " ");
            if (!src || !dst) { printf("usage: mv <src> <dst>\n"); continue; }
            cmd_mv(src, dst);
        } else if (strcmp(cmd, "ln") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *a1 = strtok(NULL, " ");
            if (!a1) { printf("usage: ln [-s] <target> <linkpath>\n"); continue; }
            if (strcmp(a1, "-s") == 0) {
                char *target = strtok(NULL, " ");
                char *linkpath = strtok(NULL, " ");
                if (!target || !linkpath) { printf("usage: ln -s <target> <linkpath>\n"); continue; }
                if (vfs_symlink(target, linkpath) != 0) printf("ln: %s\n", vfs_last_error());
            } else {
                char *linkpath = strtok(NULL, " ");
                if (!linkpath) { printf("usage: ln <target> <linkpath>\n"); continue; }
                if (vfs_link(a1, linkpath) != 0) printf("ln: %s\n", vfs_last_error());
            }
        } else if (strcmp(cmd, "umask") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *m = strtok(NULL, " ");
            if (!m) {
                printf("%u\n", vfs_get_umask());
            } else {
                vfs_umask((uint16_t)atoi(m));
            }
        } else if (strcmp(cmd, "import") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *host = strtok(NULL, " ");
            char *dst = strtok(NULL, " ");
            if (!host || !dst) { printf("usage: import <hostfile> <vfspath>\n"); continue; }
            cmd_import(host, dst);
        } else if (strcmp(cmd, "export") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *src = strtok(NULL, " ");
            char *host = strtok(NULL, " ");
            if (!src || !host) { printf("usage: export <vfspath> <hostfile>\n"); continue; }
            cmd_export(src, host);
        } else if (strcmp(cmd, "defrag") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            defrag_run();
        } else if (strcmp(cmd, "write") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            char *rest = strtok(NULL, "");
            if (!p || !rest) { printf("usage: write <path> <offset> <text>\n"); continue; }
            cmd_write(p, rest);
        } else if (strcmp(cmd, "size") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *p = strtok(NULL, " ");
            if (!p) { printf("usage: size <path>\n"); continue; }
            cmd_size(p);
        } else if (strcmp(cmd, "du") == 0) {
            if (!g_mounted) { ensure_mounted(); continue; }
            char *a1 = strtok(NULL, " ");
            char *a2 = strtok(NULL, " ");
            int human = 0;
            const char *path = ".";
            if (a1 && strcmp(a1, "-h") == 0) { human = 1; if (a2) path = a2; }
            else if (a1) path = a1;
            cmd_du(path, human);
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
            char *flag = strtok(NULL, " ");
            cmd_df(flag && strcmp(flag, "-h") == 0);
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
