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
    int bounded = have_st && S_ISREG(st.st_mode);

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

int cact_ub_dir_foreach(const char *dir,
                        int (*fn)(const char *full, const char *name, void *ud),
                        void *ud) {
    int fd = open(dir, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t dl = strlen(dir);
    int stop = 0;
    struct dirent buf[16];
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
