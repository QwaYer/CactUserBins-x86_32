/*
 * ex_system.c — system, environment and disk-usage utilities.
 *
 *   uname  hostname  nproc  sync  env  printenv  du  find
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <dirent.h>
#include <errno.h>
#include <nodeio.h>
#include <ioctl_abi.h>

/* ------------------------------------------------------------------- uname */

static const char uname_usage[] =
    "usage: uname [-a] [-s] [-n] [-r] [-m]\n"
    "  -s kernel name (default)   -n host name   -r kernel release\n"
    "  -m machine                 -a all of the above plus the version\n";

int cact_ub_uname(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, uname_usage, sizeof(uname_usage) - 1);
        return 0;
    }
    int all = 0, f_s = 0, f_n = 0, f_r = 0, f_m = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            fprintf(stderr, "uname: unexpected argument %s\n", a);
            return 1;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'a': all = 1; break;
                case 's': f_s = 1; break;
                case 'n': f_n = 1; break;
                case 'r': f_r = 1; break;
                case 'm': f_m = 1; break;
                default:
                    fprintf(stderr, "uname: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    if (!all && !f_s && !f_n && !f_r && !f_m) f_s = 1;

    struct utsname u;
    if (uname(&u) != 0) {
        fprintf(stderr, "uname: cannot read /proc/uname\n");
        return 1;
    }

    const char *parts[6];
    int n = 0;
    if (all) {
        parts[n++] = u.sysname;
        parts[n++] = u.nodename;
        parts[n++] = u.release;
        parts[n++] = u.version;
        parts[n++] = u.machine;
    } else {
        if (f_s) parts[n++] = u.sysname;
        if (f_n) parts[n++] = u.nodename;
        if (f_r) parts[n++] = u.release;
        if (f_m) parts[n++] = u.machine;
    }
    for (int i = 0; i < n; i++) {
        if (i) write(STDOUT_FILENO, " ", 1);
        fputs(parts[i], stdout);
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* ---------------------------------------------------------------- hostname */

static const char hostname_usage[] =
    "usage: hostname [-s] [NAME]\n"
    "Print the host name; with NAME set it (root, via /dev/sys).\n"
    "  -s print only the part before the first dot\n";

int cact_ub_hostname(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, hostname_usage, sizeof(hostname_usage) - 1);
        return 0;
    }
    int short_name = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-s") == 0) { short_name = 1; i++; }

    if (i < argc) {                       /* set the host name */
        if (i + 1 != argc) {
            write(STDERR_FILENO, hostname_usage, sizeof(hostname_usage) - 1);
            return 1;
        }
        if (nio_dev_cmd("sys", CACT_SYSCTL_SETHOSTNAME, (void *)argv[i]) < 0) {
            fprintf(stderr, "hostname: cannot set name: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }

    struct utsname u;
    if (uname(&u) != 0) {
        fprintf(stderr, "hostname: cannot read /proc/uname\n");
        return 1;
    }
    char *dot = short_name ? strchr(u.nodename, '.') : NULL;
    if (dot) *dot = '\0';
    printf("%s\n", u.nodename);
    return 0;
}

/* ------------------------------------------------------------------- nproc */

static const char nproc_usage[] =
    "usage: nproc\n"
    "Print the number of online processors (from /proc/cpuinfo).\n";

int cact_ub_nproc(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, nproc_usage, sizeof(nproc_usage) - 1);
        return 0;
    }
    size_t len = 0;
    char *buf = cact_ub_read_all("/proc/cpuinfo", &len);
    int count = 0;
    if (buf) {
        const char *p = buf;
        while (*p) {
            if (strncmp(p, "processor", 9) == 0) count++;
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
        free(buf);
    }
    if (count <= 0) count = 1;
    printf("%d\n", count);
    return 0;
}

/* -------------------------------------------------------------------- sync */

static const char sync_usage[] = "usage: sync\n";

int cact_ub_sync(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sync_usage, sizeof(sync_usage) - 1);
        return 0;
    }
    sync();
    return 0;
}

/* --------------------------------------------------------------- env/print */

static const char env_usage[] =
    "usage: env [NAME=VALUE]... [COMMAND [ARG]...]\n"
    "Without COMMAND print the environment; otherwise run COMMAND with the\n"
    "given variables added.\n";

int cact_ub_env(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, env_usage, sizeof(env_usage) - 1);
        return 0;
    }
    int i = 1;
    for (; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) break;
        char name[256];
        size_t nl = (size_t)(eq - argv[i]);
        if (nl >= sizeof(name)) break;
        memcpy(name, argv[i], nl);
        name[nl] = '\0';
        setenv(name, eq + 1, 1);
    }
    if (i >= argc) {
        if (environ)
            for (int k = 0; environ[k]; k++)
                printf("%s\n", environ[k]);
        return 0;
    }
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "env: %s: cannot execute\n", argv[i]);
    return 127;
}

static const char printenv_usage[] =
    "usage: printenv [NAME...]\n"
    "Print all (or the named) environment variables.\n";

int cact_ub_printenv(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, printenv_usage, sizeof(printenv_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        if (environ)
            for (int k = 0; environ[k]; k++)
                printf("%s\n", environ[k]);
        return 0;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char *v = getenv(argv[i]);
        if (v) printf("%s\n", v);
        else ret = 1;
    }
    return ret;
}

/* ---------------------------------------------------------------------- du */

static const char *sys_base(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

static void du_print(const char *path, unsigned long long size, int human) {
    if (human) {
        char hs[16];
        cact_ub_human_size(size, hs, sizeof(hs));
        printf("%8s %s\n", hs, path);
    } else {
        printf("%llu\t%s\n", size, path);
    }
}

typedef struct {
    int all;
    int summarize;
    int human;
    int ret;
    unsigned long long *acc;   /* one running total per depth, on the heap */
    int cap;
} du_ctx_t;

static int du_acc_reserve(du_ctx_t *c, int depth) {
    int ncap, k;
    unsigned long long *grown;

    if (depth < c->cap) return 0;
    ncap = c->cap ? c->cap * 2 : 16;
    while (ncap <= depth) ncap *= 2;
    grown = realloc(c->acc, (size_t)ncap * sizeof(*grown));
    if (!grown) return -1;
    for (k = c->cap; k < ncap; k++) grown[k] = 0;
    c->acc = grown;
    c->cap = ncap;
    return 0;
}

/* Pre-order: a directory opens a fresh total at its own depth, a file adds its
 * size to the total of the directory it lives in.  Depth costs heap here, not
 * the 16 KiB user stack (see cact_ub_tree_walk). */
static int du_enter(const char *full, const char *name, int depth, int is_last,
                    const struct stat *st, void *ud) {
    du_ctx_t *c = (du_ctx_t *)ud;
    (void)name; (void)is_last;
    if (S_ISDIR(st->st_mode)) {
        if (du_acc_reserve(c, depth) != 0) { c->ret = -1; return 1; }
        c->acc[depth] = 0;
    } else {
        c->acc[depth - 1] += st->st_size;
        if (c->all) du_print(full, st->st_size, c->human);
    }
    return 0;
}

/* Post-order: a directory's total is now complete — fold it into its parent and
 * report it (unless -s asked only for the per-PATH total). */
static int du_leave(const char *full, int depth, void *ud) {
    du_ctx_t *c = (du_ctx_t *)ud;
    unsigned long long total = c->acc[depth];
    if (depth > 0) c->acc[depth - 1] += total;
    if (!c->summarize) du_print(full, total, c->human);
    return 0;
}

/* One du argument: a non-directory reports its own size (lstat: du never
 * follows a symlink), a directory its tree total. */
static unsigned long long du_one(const char *path, du_ctx_t *c) {
    struct stat st;
    if (lstat(path, &st) != 0) { c->ret = -1; return 0; }
    if (!S_ISDIR(st.st_mode)) {
        du_print(path, st.st_size, c->human);
        return st.st_size;
    }
    if (du_acc_reserve(c, 0) != 0) { c->ret = -1; return 0; }
    c->acc[0] = 0;
    {
        cact_ub_walk_opts_t o = { -1, NULL, 1 };
        if (cact_ub_tree_walk(path, 0, &o, du_enter, du_leave, c) < 0) c->ret = -1;
    }
    if (c->summarize) du_print(path, c->acc[0], c->human);
    return c->acc[0];
}

static const char du_usage[] =
    "usage: du [-a] [-s] [-h] [PATH...]\n"
    "  -a include files   -s only a total per PATH   -h human-readable sizes\n";

int cact_ub_du(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, du_usage, sizeof(du_usage) - 1);
        return 0;
    }
    int all = 0, summarize = 0, human = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'a': all = 1; break;
                case 's': summarize = 1; break;
                case 'h': human = 1; break;
                default:
                    fprintf(stderr, "du: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    int ret = 0;
    if (i >= argc) {
        du_ctx_t c = { all, summarize, human, 0, NULL, 0 };
        (void)du_one(".", &c);
        free(c.acc);
        return c.ret ? 1 : 0;
    }
    for (; i < argc; i++) {
        du_ctx_t c = { all, summarize, human, 0, NULL, 0 };
        (void)du_one(argv[i], &c);
        free(c.acc);
        if (c.ret) ret = 1;
    }
    return ret;
}

/* -------------------------------------------------------------------- find */

static const char find_usage[] =
    "usage: find [PATH...] [-name PATTERN] [-type f|d] [-maxdepth N]\n"
    "Print matching paths; PATTERN supports '*' and '?'.\n";

/* Shell-style glob match (only '*' and '?'). */
static int find_glob(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *s = str; ; s++) {
                if (find_glob(pat, s)) return 1;
                if (!*s) break;
            }
            return 0;
        }
        if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
            continue;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
    return *str == '\0';
}

typedef struct {
    const char *pat;
    char type;          /* 0, 'f' or 'd' */
    int maxdepth;       /* -1 = unlimited */
} find_ctx_t;

/* Does this entry match a `-type` letter? */
static int find_type_match(int type, const struct stat *st) {
    switch (type) {
        case 'f': return S_ISREG(st->st_mode);
        case 'd': return S_ISDIR(st->st_mode);
        case 'l': return S_ISLNK(st->st_mode);
        case 'b': return S_ISBLK(st->st_mode);
        case 'c': return S_ISCHR(st->st_mode);
        case 'p': return S_ISFIFO(st->st_mode);
        case 's': return S_ISSOCK(st->st_mode);
    }
    return 0;
}

static int find_match(const find_ctx_t *c, const char *base, const struct stat *st) {
    if (c->pat && !find_glob(c->pat, base)) return 0;
    if (c->type && !find_type_match(c->type, st)) return 0;
    return 1;
}

/* One entry, reported as the iterative walker reaches it (pre-order). */
static int find_enter(const char *full, const char *name, int depth, int is_last,
                      const struct stat *st, void *ud) {
    const find_ctx_t *c = (const find_ctx_t *)ud;
    (void)depth; (void)is_last;
    if (find_match(c, name, st)) printf("%s\n", full);
    return 0;
}

int cact_ub_find(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, find_usage, sizeof(find_usage) - 1);
        return 0;
    }
    const char *paths[64];
    int np = 0;
    find_ctx_t c = { NULL, 0, -1 };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "-name") == 0 && i + 1 < argc) c.pat = argv[++i];
            else if (strcmp(a, "-type") == 0 && i + 1 < argc) {
                c.type = argv[++i][0];
                if (!strchr("fdlbcps", c.type)) {
                    fprintf(stderr, "find: unknown -type %c\n", c.type);
                    return 1;
                }
            }
            else if (strcmp(a, "-maxdepth") == 0 && i + 1 < argc) c.maxdepth = atoi(argv[++i]);
            else {
                fprintf(stderr, "find: unknown predicate %s\n", a);
                return 1;
            }
        } else if (np < (int)(sizeof(paths) / sizeof(paths[0]))) {
            paths[np++] = a;
        }
    }
    if (np == 0) paths[np++] = ".";

    int ret = 0;
    for (int i = 0; i < np; i++) {
        struct stat st;
        /* lstat, so a dangling symlink is still "found" as a link (GNU find). */
        if (lstat(paths[i], &st) != 0) {
            fprintf(stderr, "find: %s: not found\n", paths[i]);
            ret = 1;
            continue;
        }
        /* The root itself (depth 0) …
         * …then everything below, walked iteratively: depth costs heap, not the
         * 16 KiB user stack (see cact_ub_tree_walk). */
        if (find_match(&c, sys_base(paths[i]), &st)) printf("%s\n", paths[i]);
        {
            cact_ub_walk_opts_t o = { c.maxdepth, NULL, 1 };
            if (cact_ub_tree_walk(paths[i], 0, &o, find_enter, NULL, &c) < 0) ret = 1;
        }
    }
    return ret;
}
