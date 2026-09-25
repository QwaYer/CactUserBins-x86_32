/*
 * ex_copy.c — copy and file-creation utilities.
 *
 *   cp     — copy files, -r for directories
 *   touch  — create files (timestamp updates are not available in this ABI)
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <dirent.h>

/* ---------------------------------------------------------------------- cp */

static const char cp_usage[] =
    "usage: cp [-r] [-f] [-v] SRC... DST\n"
    "  -r copy directories recursively   -f remove an existing DST on failure\n"
    "  -v print each copied path\n";

/* Pointer to the last component of path (after the final '/'). */
static const char *path_base(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

typedef struct {
    const char *dst;
    int force;
    int verbose;
    int ret;
} cp_ctx_t;

static int cp_tree(const char *src, const char *dst, int recursive,
                   int force, int verbose);

static int cp_entry(const char *full, const char *name, void *ud) {
    cp_ctx_t *c = (cp_ctx_t *)ud;
    char target[512];
    snprintf(target, sizeof(target), "%s/%s", c->dst, name);
    if (cp_tree(full, target, 1, c->force, c->verbose) != 0)
        c->ret = -1;
    return 0;
}

static int cp_tree(const char *src, const char *dst, int recursive,
                   int force, int verbose) {
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "cp: %s: cannot stat\n", src);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "cp: %s: is a directory (use -r)\n", src);
            return -1;
        }
        if (mkdir(dst, 0755) != 0 && !cact_ub_is_dir(dst)) {
            fprintf(stderr, "cp: %s: cannot create directory\n", dst);
            return -1;
        }
        cp_ctx_t ctx = { dst, force, verbose, 0 };
        if (cact_ub_dir_foreach(src, cp_entry, &ctx) < 0) {
            fprintf(stderr, "cp: %s: cannot read directory\n", src);
            return -1;
        }
        if (verbose) printf("%s -> %s\n", src, dst);
        return ctx.ret;
    }

    if (cact_ub_copy_file(src, dst, 0644) != 0) {
        if (force) {
            unlink(dst);
            if (cact_ub_copy_file(src, dst, 0644) == 0) {
                if (verbose) printf("%s -> %s\n", src, dst);
                return 0;
            }
        }
        fprintf(stderr, "cp: %s -> %s: failed\n", src, dst);
        return -1;
    }
    if (verbose) printf("%s -> %s\n", src, dst);
    return 0;
}

int cact_ub_cp(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cp_usage, sizeof(cp_usage) - 1);
        return 0;
    }
    int recursive = 0, force = 0, verbose = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'r': case 'R': recursive = 1; break;
                case 'f': force = 1; break;
                case 'v': verbose = 1; break;
                default:
                    fprintf(stderr, "cp: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    int n = argc - i;
    if (n < 2) {
        write(STDERR_FILENO, cp_usage, sizeof(cp_usage) - 1);
        return 1;
    }

    const char *dst = argv[argc - 1];
    int dst_is_dir = cact_ub_is_dir(dst);
    if (n > 2 && !dst_is_dir) {
        fprintf(stderr, "cp: %s: not a directory\n", dst);
        return 1;
    }

    int ret = 0;
    for (int k = i; k < argc - 1; k++) {
        char target[512];
        if (dst_is_dir) {
            const char *base = path_base(argv[k]);
            if (!base[0]) base = ".";
            snprintf(target, sizeof(target), "%s/%s", dst, base);
        } else {
            snprintf(target, sizeof(target), "%s", dst);
        }
        if (cp_tree(argv[k], target, recursive, force, verbose) != 0)
            ret = 1;
    }
    return ret;
}

/* ------------------------------------------------------------------- touch */

static const char touch_usage[] =
    "usage: touch [-c] FILE...\n"
    "Create each FILE if it does not exist.  This ABI exposes no file\n"
    "timestamps, so an existing FILE is left untouched.\n"
    "  -c do not create a missing FILE\n";

int cact_ub_touch(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, touch_usage, sizeof(touch_usage) - 1);
        return 0;
    }
    int no_create = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'c': no_create = 1; break;
                case 'a': case 'm': break;   /* accepted; nothing to update */
                default:
                    fprintf(stderr, "touch: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    if (i >= argc) {
        write(STDERR_FILENO, touch_usage, sizeof(touch_usage) - 1);
        return 1;
    }

    int ret = 0;
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0) continue;     /* exists: nothing to do */
        if (no_create) continue;
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: cannot create\n", argv[i]);
            ret = 1;
        } else {
            close(fd);
        }
    }
    return ret;
}
