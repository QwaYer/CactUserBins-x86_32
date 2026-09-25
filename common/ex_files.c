/*
 * builtins/files.c — file and directory operations.
 *
 *   ls / mkdir / rmdir / rm / cat / wrt / stat / mv / ln / readlink
 */


#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <syscall.h>

#include "common/ex_util.h"

/* "d" + rwxrwxrwx for a stat mode (both ls -l and stat use it). */
static void mode_string(uint32_t mode, char *out) {
    char t = '?';
    switch (mode & S_IFMT) {
        case S_IFDIR:  t = 'd'; break;
        case S_IFREG:  t = '-'; break;
        case S_IFCHR:  t = 'c'; break;
        case S_IFBLK:  t = 'b'; break;
        case S_IFIFO:  t = 'p'; break;
        case S_IFLNK:  t = 'l'; break;
        case S_IFSOCK: t = 's'; break;
    }
    out[0] = t;
    static const int bits[9] = { 0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001 };
    static const char chars[9] = { 'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x' };
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & bits[i]) ? chars[i] : '-';
    out[10] = '\0';
}

static const char ls_usage[] =
    "usage: ls [-l] [-a] [-h] [PATH...]\n"
    "  -l long format (mode, size, name)   -a include dot files\n"
    "  -h human-readable sizes (with -l)\n";

static void ls_print(const char *name, const struct stat *st,
                     int long_fmt, int human) {
    if (!long_fmt) {
        printf("%s\n", name);
        return;
    }
    char perm[11];
    mode_string(st->st_mode, perm);
    if (human) {
        char hs[16];
        cact_ub_human_size(st->st_size, hs, sizeof(hs));
        printf("%s %8s %s\n", perm, hs, name);
    } else {
        printf("%s %8u %s\n", perm, (unsigned)st->st_size, name);
    }
}

typedef struct {
    int long_fmt;
    int human;
    int show_all;
} ls_ctx_t;

static int ls_one(const char *full, const char *name, void *ud) {
    ls_ctx_t *c = (ls_ctx_t *)ud;
    if (!c->show_all && name[0] == '.') return 0;
    if (!c->long_fmt) {
        printf("%s\n", name);
        return 0;
    }
    struct stat st;
    if (stat(full, &st) != 0) {
        st.st_mode = 0;
        st.st_size = 0;
    }
    ls_print(name, &st, 1, c->human);
    return 0;
}

int cact_ub_ls(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, ls_usage, sizeof(ls_usage) - 1);
        return 0;
    }
    int long_fmt = 0, show_all = 0, human = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'l': long_fmt = 1; break;
                case 'a': show_all = 1; break;
                case 'h': human = 1; break;
                default:
                    fprintf(stderr, "ls: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }

    int ret = 0;
    if (i >= argc) {
        ls_ctx_t c = { long_fmt, human, show_all };
        if (cact_ub_dir_foreach(".", ls_one, &c) < 0) {
            fprintf(stderr, "ls: .: cannot open\n");
            return 1;
        }
        return 0;
    }

    int multi = (argc - i) > 1;
    for (int k = i; k < argc; k++) {
        const char *path = argv[k];
        struct stat st;
        if (stat(path, &st) != 0) {
            fprintf(stderr, "ls: %s: cannot open\n", path);
            ret = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (multi) {
                if (k > i) write(STDOUT_FILENO, "\n", 1);
                printf("%s:\n", path);
            }
            ls_ctx_t c = { long_fmt, human, show_all };
            if (cact_ub_dir_foreach(path, ls_one, &c) < 0) {
                fprintf(stderr, "ls: %s: cannot read directory\n", path);
                ret = 1;
            }
        } else {
            ls_print(path, &st, long_fmt, human);
        }
    }
    return ret;
}

static const char mkdir_usage[] =
    "usage: mkdir [-p] DIR...\n"
    "  -p create parents as needed and do not fail if DIR already exists\n";

int cact_ub_mkdir(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mkdir_usage, sizeof(mkdir_usage) - 1);
        return 0;
    }
    int parents = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'p') parents = 1;
            else {
                fprintf(stderr, "mkdir: unknown option -%c\n", *p);
                return 1;
            }
        }
    }
    if (i >= argc) {
        write(STDERR_FILENO, mkdir_usage, sizeof(mkdir_usage) - 1);
        return 1;
    }
    int ret = 0;
    for (; i < argc; i++) {
        int rc = parents ? cact_ub_mkdir_p(argv[i], 0755) : mkdir(argv[i], 0755);
        if (rc != 0) {
            fprintf(stderr, "mkdir: %s: failed\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}

static const char rmdir_usage[] = "usage: rmdir DIR...\n";

int cact_ub_rmdir(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, rmdir_usage, sizeof(rmdir_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, rmdir_usage, sizeof(rmdir_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        if (rmdir(argv[i]) != 0) {
            write(STDERR_FILENO, "rmdir: ", 7);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

static const char rm_usage[] =
    "usage: rm [-r] [-f] FILE...\n"
    "  -r remove directories and their contents recursively\n"
    "  -f never prompt and ignore missing files\n";

static int rm_tree(const char *path, int force);

static int rm_walk(const char *full, const char *name, void *ud) {
    (void)name;
    return rm_tree(full, *(int *)ud);
}

static int rm_tree(const char *path, int force) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (!force) fprintf(stderr, "rm: %s: not found\n", path);
        return force ? 0 : -1;
    }
    if (S_ISDIR(st.st_mode)) {
        int f = force;
        cact_ub_dir_foreach(path, rm_walk, &f);
        if (rmdir(path) != 0) {
            if (!force) fprintf(stderr, "rm: %s: cannot remove directory\n", path);
            return -1;
        }
        return 0;
    }
    if (unlink(path) != 0) {
        if (!force) fprintf(stderr, "rm: %s: cannot remove\n", path);
        return -1;
    }
    return 0;
}

int cact_ub_rm(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, rm_usage, sizeof(rm_usage) - 1);
        return 0;
    }
    int recursive = 0, force = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'r': case 'R': recursive = 1; break;
                case 'f': force = 1; break;
                default:
                    fprintf(stderr, "rm: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    if (i >= argc) {
        if (force) return 0;
        write(STDERR_FILENO, rm_usage, sizeof(rm_usage) - 1);
        return 1;
    }
    int ret = 0;
    for (; i < argc; i++) {
        struct stat st;
        int is_dir = (lstat(argv[i], &st) == 0) && S_ISDIR(st.st_mode);
        if (is_dir && !recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", argv[i]);
            ret = 1;
            continue;
        }
        if (rm_tree(argv[i], force) != 0) ret = 1;
    }
    return ret;
}

static const char cat_usage[] =
    "usage: cat [-n] [FILE...]\n"
    "  -n number output lines   FILE '-' reads stdin\n";

static void cat_emit(const char *buf, int n, int number,
                     int *at_start, long *lineno) {
    int i = 0;
    while (i < n) {
        if (number && *at_start) {
            char pfx[24];
            int pl = snprintf(pfx, sizeof(pfx), "%6ld\t", (*lineno)++);
            cact_ub_write_all(STDOUT_FILENO, pfx, (size_t)pl);
            *at_start = 0;
        }
        int j = i;
        while (j < n && buf[j] != '\n') j++;
        if (j < n) j++;
        cact_ub_write_all(STDOUT_FILENO, buf + i, (size_t)(j - i));
        if (j > i && buf[j - 1] == '\n') *at_start = 1;
        i = j;
    }
}

static int cat_stream(int fd, int number, int *at_start, long *lineno) {
    struct stat st;
    long long budget = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        budget = (long long)st.st_size;    /* the FS may not report EOF */

    char buf[4096];
    int ret = 0;
    for (;;) {
        size_t want = sizeof(buf);
        if (budget >= 0) {
            if (budget == 0) break;
            if ((long long)want > budget) want = (size_t)budget;
        }
        ssize_t n = read(fd, buf, want);
        if (n < 0) { ret = -1; break; }
        if (n == 0) break;
        if (budget >= 0) budget -= n;
        cat_emit(buf, (int)n, number, at_start, lineno);
    }
    return ret;
}

int cact_ub_cat(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cat_usage, sizeof(cat_usage) - 1);
        return 0;
    }
    int number = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'n') number = 1;
            else {
                fprintf(stderr, "cat: unknown option -%c\n", *p);
                return 1;
            }
        }
    }

    int at_start = 1;
    long lineno = 1;
    if (i >= argc) {
        cat_stream(STDIN_FILENO, number, &at_start, &lineno);
        return 0;
    }
    int ret = 0;
    for (; i < argc; i++) {
        int fd;
        if (strcmp(argv[i], "-") == 0) fd = STDIN_FILENO;
        else fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: not found\n", argv[i]);
            ret = 1;
            continue;
        }
        if (cat_stream(fd, number, &at_start, &lineno) != 0) ret = 1;
        if (fd != STDIN_FILENO) close(fd);
    }
    return ret;
}

static const char wrt_usage[] = "usage: wrt FILE [text...]\n";

int cact_ub_wrt(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, wrt_usage, sizeof(wrt_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, wrt_usage, sizeof(wrt_usage) - 1); return 1; }
    int fd = open(argv[1], O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        write(STDERR_FILENO, "wrt: file not found: ", 21);
        write(STDERR_FILENO, argv[1], strlen(argv[1]));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    if (argc >= 3) {
        int i;
        for (i = 2; i < argc; i++) {
            write(fd, argv[i], strlen(argv[i]));
            if (i < argc - 1) write(fd, " ", 1);
        }
        write(fd, "\n", 1);
    }
    close(fd);
    return 0;
}

static const char stat_usage[] = "usage: stat FILE...\n";

int cact_ub_stat(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, stat_usage, sizeof(stat_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, stat_usage, sizeof(stat_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            write(STDERR_FILENO, "stat: ", 6);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": not found\n", 12);
            ret = 1;
            continue;
        }
        char buf[32];
        write(STDOUT_FILENO, "File:  ", 7);
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Size:  ", 7);
        itoa((int)st.st_size, buf); write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Inode: ", 7);
        itoa((int)st.st_ino, buf);  write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
        {
            char perm[11];
            mode_string(st.st_mode, perm);
            snprintf(buf, sizeof(buf), "Mode:  %04o (%s)\n",
                     (unsigned)(st.st_mode & 07777), perm);
            write(STDOUT_FILENO, buf, strlen(buf));
        }
        write(STDOUT_FILENO, "Type:  ", 7);
        if      (S_ISDIR(st.st_mode))  write(STDOUT_FILENO, "directory\n",    10);
        else if (S_ISREG(st.st_mode))  write(STDOUT_FILENO, "regular file\n",  13);
        else if (S_ISCHR(st.st_mode))  write(STDOUT_FILENO, "char device\n",   12);
        else if (S_ISBLK(st.st_mode))  write(STDOUT_FILENO, "block device\n",  13);
        else if (S_ISFIFO(st.st_mode)) write(STDOUT_FILENO, "fifo/pipe\n",     10);
        else                           write(STDOUT_FILENO, "other\n",          6);
    }
    return ret;
}

static const char mv_usage[] =
    "usage: mv SRC... DST\n"
    "Rename SRC to DST; with several SRCs DST must be a directory.\n"
    "Falls back to copy+unlink when rename(2) cannot cross filesystems.\n";

static const char *mv_base(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

static int mv_one(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;

    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "mv: %s: not found\n", src);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mv: %s: cannot move a directory across filesystems\n", src);
        return -1;
    }
    if (cact_ub_copy_file(src, dst, 0644) != 0) {
        fprintf(stderr, "mv: %s -> %s: failed\n", src, dst);
        return -1;
    }
    unlink(src);
    return 0;
}

int cact_ub_mv(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mv_usage, sizeof(mv_usage) - 1);
        return 0;
    }
    if (argc < 3) { write(STDERR_FILENO, mv_usage, sizeof(mv_usage) - 1); return 1; }

    const char *dst = argv[argc - 1];
    int dst_is_dir = cact_ub_is_dir(dst);
    int nsrc = argc - 2;
    if (nsrc > 1 && !dst_is_dir) {
        fprintf(stderr, "mv: %s: not a directory\n", dst);
        return 1;
    }

    int ret = 0;
    for (int k = 1; k < argc - 1; k++) {
        char target[512];
        if (dst_is_dir) {
            const char *b = mv_base(argv[k]);
            if (!b[0]) b = ".";
            snprintf(target, sizeof(target), "%s/%s", dst, b);
        } else {
            snprintf(target, sizeof(target), "%s", dst);
        }
        if (mv_one(argv[k], target) != 0) ret = 1;
    }
    return ret;
}

static const char ln_usage[] =
    "usage: ln [-s] [-f] SRC DST\n"
    "  -s symbolic link   -f remove an existing DST first\n";

int cact_ub_ln(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, ln_usage, sizeof(ln_usage) - 1);
        return 0;
    }
    int soft = 0, force = 0, base = 1;
    for (; base < argc; base++) {
        const char *a = argv[base];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 's': soft = 1; break;
                case 'f': force = 1; break;
                default:
                    fprintf(stderr, "ln: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    if (argc < base + 2) {
        write(STDERR_FILENO, ln_usage, sizeof(ln_usage) - 1);
        return 1;
    }
    if (force) unlink(argv[base + 1]);

    int r = soft
        ? symlink(argv[base], argv[base + 1])
        : link(argv[base], argv[base + 1]);
    if (r != 0) {
        fprintf(stderr, "ln: %s -> %s: failed\n", argv[base], argv[base + 1]);
        return 1;
    }
    return 0;
}

static const char readlink_usage[] = "usage: readlink PATH\n";

int cact_ub_readlink(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, readlink_usage, sizeof(readlink_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, readlink_usage, sizeof(readlink_usage) - 1); return 1; }
    char buf[512];
    int n = (int)readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        write(STDERR_FILENO, "readlink: failed\n", 17);
        return 1;
    }
    buf[n] = '\0';
    write(STDOUT_FILENO, buf, (size_t)n);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
