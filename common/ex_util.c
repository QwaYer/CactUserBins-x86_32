/*
 * ex_util.c — shared helpers for the CactUserBins utilities (see ex_util.h).
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

char *cact_ub_read_all(const char *path, size_t *len) {
    int fd = path ? open(path, O_RDONLY, 0) : STDIN_FILENO;
    if (fd < 0) return NULL;

    struct stat st;
    int have_st = (fstat(fd, &st) == 0);
    /* Only a non-zero st_size bounds the read.  procfs/sysfs virtual files are
     * regular files with st_size 0 but do have content, so a zero size means
     * "unknown" here, not "empty". */
    int bounded = have_st && S_ISREG(st.st_mode) && st.st_size > 0;

    size_t cap = bounded ? (size_t)st.st_size + 1 : 4096;
    char *buf = (char *)malloc(cap ? cap : 1);
    if (!buf) {
        if (path) close(fd);
        return NULL;
    }

    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            if (bounded) break;          /* st_size bytes read — done */
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                if (path) close(fd);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            free(buf);
            if (path) close(fd);
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
        if (bounded && n >= (size_t)st.st_size) break;
    }
    if (path) close(fd);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

int cact_ub_write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int cact_ub_shell_ctl(const char *line) {
    const char *fds = getenv("CACT_CTL_FD");
    if (!fds || !*fds) return -1;
    int fd = atoi(fds);
    if (fd <= STDERR_FILENO) return -1;

    /* One write: a request shorter than PIPE_BUF stays atomic, so the shell
     * never sees it interleaved with another writer's request. */
    char buf[600];
    size_t n = strlen(line);
    if (n + 1 > sizeof(buf)) return -1;
    memcpy(buf, line, n);
    buf[n] = '\n';
    return cact_ub_write_all(fd, buf, n + 1);
}

int cact_ub_dir_foreach(const char *dir,
                        int (*fn)(const char *full, const char *name, void *ud),
                        void *ud) {
    int fd = open(dir, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t dl = strlen(dir);
    int stop = 0;
    /* Small on purpose: recursive callers (find/du/tree) hold one of these per
     * directory level, and the user stack is only USER_STACK_BYTES (16 KiB). */
    struct dirent buf[4];
    for (;;) {
        int n = getdents(fd, buf, sizeof(buf));
        if (n <= 0) break;
        int cnt = n / (int)sizeof(struct dirent);
        for (int i = 0; i < cnt && !stop; i++) {
            const char *nm = buf[i].d_name;
            if (!nm[0] || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                continue;
            char full[512];
            if (dl > 0 && dir[dl - 1] == '/')
                snprintf(full, sizeof(full), "%s%s", dir, nm);
            else
                snprintf(full, sizeof(full), "%s/%s", dir, nm);
            stop = fn(full, nm, ud);
        }
        if (stop) break;
    }
    close(fd);
    return stop;
}

/* ── iterative tree walk ─────────────────────────────────────────────────────
 * One frame per directory level, all on the heap.  Recursion here used to cost
 * ~2.7 KiB of the 16 KiB user stack per level (this function's dirent buffer
 * plus the walker's own frame), so a tree a dozen levels deep killed the
 * process; with an explicit stack the depth costs heap instead. */
typedef struct {
    char        *name;
    struct stat  st;
} ub_walk_ent_t;

typedef struct {
    char          *path;        /* this directory (owned) */
    ub_walk_ent_t *ents;        /* entries that passed the filter (owned) */
    int            n;
    int            i;           /* cursor into ents */
    int            depth;
    cact_ub_walk_sel sel;       /* the walk's filter, if any */
    int            sort;        /* ...and whether entries are name-sorted */
    void          *ud;          /* filter state (the walk's `ud`) */
} ub_walk_frame_t;

static char *ub_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *ub_path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name) + 1;
    int slash = !(dl > 0 && dir[dl - 1] == '/');
    char *p = malloc(dl + (size_t)slash + nl);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    memcpy(p + dl + (size_t)slash, name, nl);
    return p;
}

static void ub_frame_clear(ub_walk_frame_t *f) {
    int i;
    for (i = 0; i < f->n; i++) free(f->ents[i].name);
    free(f->ents);
    free(f->path);
    f->ents = NULL;
    f->path = NULL;
    f->n = f->i = 0;
}

/* Stat every entry here (exactly one lstat per entry, as a walk needs anyway)
 * and keep the ones the filter accepts. */
static int ub_collect(const char *full, const char *name, void *ud) {
    ub_walk_frame_t *f = (ub_walk_frame_t *)ud;
    ub_walk_ent_t *grown;
    struct stat st;
    char *copy;

    if (lstat(full, &st) != 0) return 0;                 /* vanished: skip it */
    if (f->sel && !f->sel(name, &st, f->ud)) return 0;

    grown = realloc(f->ents, (size_t)(f->n + 1) * sizeof(*grown));
    if (!grown) return 1;
    f->ents = grown;
    copy = ub_strdup(name);
    if (!copy) return 1;
    f->ents[f->n].name = copy;
    f->ents[f->n].st   = st;
    f->n++;
    return 0;
}

/* Read a frame's entry list.  An unreadable directory stays empty: the caller
 * already saw the entry as a directory and gets no children. */
static int ub_ent_cmp(const void *a, const void *b) {
    const ub_walk_ent_t *x = (const ub_walk_ent_t *)a;
    const ub_walk_ent_t *y = (const ub_walk_ent_t *)b;
    return strcmp(x->name, y->name);
}

static void ub_frame_read(ub_walk_frame_t *f) {
    f->ents = NULL;
    f->n = 0;
    f->i = 0;
    (void)cact_ub_dir_foreach(f->path, ub_collect, f);
    if (f->sort && f->n > 1) qsort(f->ents, (size_t)f->n, sizeof(*f->ents), ub_ent_cmp);
}

int cact_ub_tree_walk(const char *root, int root_depth,
                      const cact_ub_walk_opts_t *opts,
                      cact_ub_walk_enter enter, cact_ub_walk_leave leave, void *ud) {
    int cap = 16, top = 0, rc = 0, stopped = 0;
    int max_depth = opts ? opts->max_depth : -1;
    cact_ub_walk_sel sel = opts ? opts->sel : NULL;
    int sort = opts ? opts->sort : 0;
    ub_walk_frame_t *st;

    if (!root) return -1;
    st = malloc(sizeof(ub_walk_frame_t) * (size_t)cap);
    if (!st) return -1;

    st[0].path   = ub_strdup(root);
    st[0].depth  = root_depth;
    st[0].ents   = NULL;
    st[0].n = st[0].i = 0;
    st[0].sel    = sel;
    st[0].ud     = ud;
    st[0].sort   = sort;
    if (!st[0].path) { free(st); return -1; }
    ub_frame_read(&st[0]);
    top = 1;

    while (top > 0) {
        ub_walk_frame_t *f = &st[top - 1];

        if (f->i >= f->n) {
            if (leave) leave(f->path, f->depth, ud);
            ub_frame_clear(f);
            top--;
            continue;
        }
        if (max_depth >= 0 && f->depth + 1 > max_depth) {
            f->i = f->n;          /* nothing below this depth is wanted */
            continue;
        }

        {
            const ub_walk_ent_t *e = &f->ents[f->i];
            int is_last = (f->i + 1 == f->n);
            int depth   = f->depth + 1;
            char *full  = ub_path_join(f->path, e->name);
            f->i++;
            if (!full) { rc = -1; break; }

            if (enter && enter(full, e->name, depth, is_last, &e->st, ud)) {
                free(full);
                stopped = 1;
                break;
            }

            if (S_ISDIR(e->st.st_mode)) {
                if (top == cap) {
                    ub_walk_frame_t *grown =
                        realloc(st, sizeof(ub_walk_frame_t) * (size_t)(cap * 2));
                    if (!grown) { free(full); rc = -1; break; }
                    st = grown;
                    cap *= 2;
                }
                st[top].path  = full;     /* the new frame takes ownership */
                st[top].depth = depth;
                st[top].ents  = NULL;
                st[top].n = st[top].i = 0;
                st[top].sel   = sel;
                st[top].ud    = ud;
                st[top].sort  = sort;
                ub_frame_read(&st[top]);
                top++;
                continue;
            }
            free(full);
        }
    }

    while (top > 0) ub_frame_clear(&st[--top]);
    free(st);
    return stopped ? 1 : rc;
}

void cact_ub_human_size(unsigned long long v, char *out, size_t n) {
    static const char units[] = "KMGT";
    unsigned long long unit = 1;
    int ui = -1;
    while (ui < 3 && v / unit >= 1024ULL) {
        unit *= 1024ULL;
        ui++;
    }
    if (ui < 0) {
        snprintf(out, n, "%llu", v);
        return;
    }
    unsigned long long whole = v / unit;
    unsigned long long frac = ((v % unit) * 10ULL) / unit;
    snprintf(out, n, "%llu.%llu%c", whole, frac, units[ui]);
}

long cact_ub_parse_size(const char *s) {
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end && *end) {
        switch (*end | 0x20) {
            case 'k': v *= 1024L; break;
            case 'm': v *= 1024L * 1024L; break;
            case 'g': v *= 1024L * 1024L * 1024L; break;
            case 'b': v *= 512L; break;
            default: break;
        }
    }
    return v;
}

int cact_ub_mkdir_p(const char *path, int mode) {
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, mode) != 0) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
        }
        tmp[i] = '/';
    }
    if (mkdir(tmp, mode) == 0) return 0;
    {
        struct stat st;
        if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}

int cact_ub_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int cact_ub_copy_file(const char *src, const char *dst, int mode) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }

    int in = open(src, O_RDONLY, 0);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { close(in); return -1; }

    char buf[8192];
    int rc = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        if (cact_ub_write_all(out, buf, (size_t)r) != 0) { rc = -1; break; }
    }
    if (close(out) != 0 && rc == 0) rc = -1;
    close(in);
    return rc;
}
