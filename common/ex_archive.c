/*
 * ex_archive.c — the tar archiver (ustar format).
 *
 *   tar -c [-v] [-f ARCHIVE] [-C DIR] FILE...   create
 *   tar -t [-v] [-f ARCHIVE]                    list
 *   tar -x [-v] [-f ARCHIVE] [-C DIR]           extract
 *
 * Plain ustar: regular files and directories are stored; extract also
 * recreates symbolic links.  No compression.  With no -f the archive is read
 * from stdin (list/extract) or written to stdout (create), like GNU tar's "-".
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
#include <stdint.h>

#define TAR_BLOCK 512

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_hdr;

static const char tar_usage[] =
    "usage: tar -c|-t|-x [-v] [-f ARCHIVE] [-C DIR] [FILE...]\n"
    "  -c create, -t list, -x extract.  No -f means stdin/stdout.\n";

static void tar_octal(char *dst, int width, unsigned long long v) {
    snprintf(dst, (size_t)width, "%0*llo", width - 1, v);
}

static unsigned long long tar_parse_octal(const char *f, int width) {
    unsigned long long v = 0;
    int seen = 0;
    for (int i = 0; i < width; i++) {
        char c = f[i];
        if (c == ' ' || c == '\0') { if (seen) break; else continue; }
        if (c < '0' || c > '7') break;
        v = v * 8 + (unsigned long long)(c - '0');
        seen = 1;
    }
    return v;
}

static unsigned int tar_checksum(const tar_hdr *h) {
    unsigned int sum = 0;
    const unsigned char *p = (const unsigned char *)h;
    for (int i = 0; i < TAR_BLOCK; i++) {
        if (i >= 148 && i < 156) sum += ' ';
        else sum += p[i];
    }
    return sum;
}

/* Fill a header for `path` with stored `name`. */
static void tar_fill(tar_hdr *h, const char *name, const struct stat *st,
                     unsigned long long size, int isdir, int islink,
                     const char *linktarget) {
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "%s", name);
    tar_octal(h->mode, 8, st ? (st->st_mode & 07777) : 0644);
    tar_octal(h->uid, 8, 0);
    tar_octal(h->gid, 8, 0);
    tar_octal(h->size, 12, isdir ? 0 : size);
    tar_octal(h->mtime, 12, 0);   /* the VFS does not track mtimes yet */
    h->typeflag = isdir ? '5' : (islink ? '2' : '0');
    if (islink && linktarget) snprintf(h->linkname, sizeof(h->linkname), "%s", linktarget);
    memcpy(h->magic, "ustar", 5);
    h->magic[5] = '\0';
    memcpy(h->version, "00", 2);
    unsigned int sum = tar_checksum(h);
    snprintf(h->chksum, sizeof(h->chksum), "%06o", sum);
    h->chksum[6] = '\0';
    h->chksum[7] = ' ';
}

/* Split a long name into prefix/name so both fit the ustar fields. */
static int tar_set_name(tar_hdr *h, const char *orig) {
    if ((size_t)strlen(orig) <= sizeof(h->name)) {
        snprintf(h->name, sizeof(h->name), "%s", orig);
        return 0;
    }
    /* find a '/' such that the suffix fits in name[] and the head in prefix[] */
    size_t len = strlen(orig);
    for (size_t cut = len; cut > 0; cut--) {
        if (orig[cut - 1] != '/') continue;
        size_t suf = len - cut;
        if (suf <= sizeof(h->name) && cut <= sizeof(h->prefix)) {
            memcpy(h->prefix, orig, cut);
            if (cut >= sizeof(h->prefix)) return -1;
            h->prefix[cut] = '\0';
            snprintf(h->name, sizeof(h->name), "%s", orig + cut);
            return 0;
        }
    }
    return -1;
}

static int tar_write_header(int fd, const char *name, const struct stat *st,
                            unsigned long long size, int isdir, int islink,
                            const char *linktarget) {
    tar_hdr h;
    memset(&h, 0, sizeof(h));
    tar_fill(&h, "", st, size, isdir, islink, linktarget);
    if (tar_set_name(&h, name) != 0) {
        fprintf(stderr, "tar: %s: name too long for ustar\n", name);
        return -1;
    }
    /* checksum must be recomputed after the name is in place */
    unsigned int sum = tar_checksum(&h);
    snprintf(h.chksum, sizeof(h.chksum), "%06o", sum);
    h.chksum[6] = '\0';
    h.chksum[7] = ' ';
    return cact_ub_write_all(fd, &h, sizeof(h));
}

static int tar_add_file(int fd, const char *diskpath, const char *storedname,
                        int verbose) {
    struct stat st;
    if (lstat(diskpath, &st) != 0) {
        fprintf(stderr, "tar: %s: cannot stat\n", diskpath);
        return 1;
    }
    if (verbose) printf("%s\n", storedname);

    if (S_ISLNK(st.st_mode)) {
        char target[256];
        ssize_t l = readlink(diskpath, target, sizeof(target) - 1);
        if (l < 0) l = 0;
        target[l] = '\0';
        return tar_write_header(fd, storedname, &st, 0, 0, 1, target) != 0;
    }
    if (S_ISDIR(st.st_mode)) {
        char name[1024];
        snprintf(name, sizeof(name), "%s/", storedname);
        if (tar_write_header(fd, name, &st, 0, 1, 0, NULL) != 0) return 1;
        int dfd = open(diskpath, O_RDONLY, 0);
        if (dfd < 0) return 0;
        struct dirent buf[32];
        int r;
        while ((r = getdents(dfd, buf, sizeof(buf))) > 0) {
            int cnt = r / (int)sizeof(struct dirent);
            for (int k = 0; k < cnt; k++) {
                const char *nm = buf[k].d_name;
                if (!nm[0] || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
                char cdisk[1024], cstored[1024];
                snprintf(cdisk, sizeof(cdisk), "%s/%s", diskpath, nm);
                snprintf(cstored, sizeof(cstored), "%s/%s", storedname, nm);
                tar_add_file(fd, cdisk, cstored, verbose);
            }
        }
        close(dfd);
        return 0;
    }
    if (!S_ISREG(st.st_mode)) return 0;   /* skip devices, fifos, sockets */

    unsigned long long size = st.st_size;
    if (tar_write_header(fd, storedname, &st, size, 0, 0, NULL) != 0) return 1;

    /* strip a leading "/" so stored names are relative */
    const char *rp = storedname;
    while (rp[0] == '/') rp++;

    int in = open(diskpath, O_RDONLY, 0);
    if (in < 0) { fprintf(stderr, "tar: %s: cannot open\n", diskpath); return 1; }
    unsigned long long written = 0;
    char buf[8192];
    while (written < size) {
        size_t want = sizeof(buf);
        if ((unsigned long long)want > size - written) want = (size_t)(size - written);
        ssize_t n = read(in, buf, want);
        if (n < 0) { close(in); fprintf(stderr, "tar: %s: read error\n", diskpath); return 1; }
        if (n == 0) break;
        if (cact_ub_write_all(fd, buf, (size_t)n) != 0) { close(in); return 1; }
        written += (unsigned long long)n;
    }
    close(in);
    /* pad to a block */
    char zero[TAR_BLOCK];
    memset(zero, 0, sizeof(zero));
    unsigned long long rem = size % TAR_BLOCK;
    if (rem) {
        if (cact_ub_write_all(fd, zero, (size_t)(TAR_BLOCK - rem)) != 0) return 1;
    }
    return 0;
}

static int tar_create(int fd, char **argv, int from, int argc, int verbose) {
    if (from >= argc) { fprintf(stderr, "tar: nothing to archive\n"); return 1; }
    int rc = 0;
    for (int i = from; i < argc; i++) {
        const char *p = argv[i];
        const char *stored = p;
        while (stored[0] == '/' ) stored++;   /* absolute paths become relative */
        if (tar_add_file(fd, p, stored, verbose) != 0) rc = 1;
    }
    char zero[TAR_BLOCK];
    memset(zero, 0, sizeof(zero));
    cact_ub_write_all(fd, zero, sizeof(zero));
    cact_ub_write_all(fd, zero, sizeof(zero));
    return rc;
}

static int tar_read_block(int fd, void *buf) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < TAR_BLOCK) {
        ssize_t n = read(fd, p + got, TAR_BLOCK - got);
        if (n < 0) return -1;
        if (n == 0) return (int)got;      /* short/EOF */
        got += (size_t)n;
    }
    return TAR_BLOCK;
}

static int tar_is_zero(const unsigned char *b) {
    for (int i = 0; i < TAR_BLOCK; i++) if (b[i]) return 0;
    return 1;
}

static void tar_skip_data(int fd, unsigned long long size) {
    unsigned long long total = ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    if (total == 0) return;
    if (lseek(fd, 0, SEEK_CUR) >= 0 && lseek(fd, (off_t)total, SEEK_CUR) >= 0)
        return;   /* a seekable archive */
    char buf[TAR_BLOCK];
    unsigned long long left = total;
    while (left > 0) {
        size_t want = left > TAR_BLOCK ? TAR_BLOCK : (size_t)left;
        ssize_t n = read(fd, buf, want);
        if (n <= 0) break;
        left -= (unsigned long long)n;
    }
}

static void tar_mode_string(unsigned mode, char out[11]) {
    out[0] = '-';
    const char *rwx = "rwxrwxrwx";
    for (int k = 0; k < 9; k++) out[k + 1] = (mode & (1u << (8 - k))) ? rwx[k] : '-';
    out[10] = '\0';
}

static int tar_name_of(const tar_hdr *h, char *out, size_t n) {
    if (h->prefix[0]) snprintf(out, n, "%s/%s", h->prefix, h->name);
    else              snprintf(out, n, "%s", h->name);
    return 0;
}

static void tar_mkparents(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    cact_ub_mkdir_p(tmp, 0755);
}

static int tar_extract(int fd, int verbose) {
    unsigned char block[TAR_BLOCK];
    int rc = 0;
    for (;;) {
        int r = tar_read_block(fd, block);
        if (r <= 0) break;
        if (tar_is_zero(block)) break;
        tar_hdr *h = (tar_hdr *)block;
        char name[512];
        tar_name_of(h, name, sizeof(name));
        unsigned long long size = tar_parse_octal(h->size, 12);
        char type = h->typeflag;

        if (verbose) printf("%s\n", name);

        if (type == '5') {
            cact_ub_mkdir_p(name, 0755);
            continue;
        }
        if (type == '2') {
            tar_mkparents(name);
            unlink(name);
            if (symlink(h->linkname, name) != 0 && errno != EEXIST)
                fprintf(stderr, "tar: %s: cannot create symlink\n", name);
            continue;
        }
        tar_mkparents(name);
        int out = open(name, O_WRONLY | O_CREAT | O_TRUNC, (int)(tar_parse_octal(h->mode, 8) & 07777));
        if (out < 0) {
            fprintf(stderr, "tar: %s: cannot create\n", name);
            rc = 1;
            tar_skip_data(fd, size);
            continue;
        }
        unsigned long long left = size;
        while (left > 0) {
            size_t want = left > TAR_BLOCK ? TAR_BLOCK : (size_t)left;
            ssize_t n = read(fd, block, want);
            if (n <= 0) break;
            if (cact_ub_write_all(out, block, (size_t)n) != 0) { rc = 1; break; }
            left -= (unsigned long long)n;
        }
        close(out);
        /* consume the block padding */
        unsigned long long rem = size % TAR_BLOCK;
        if (rem) {
            char skip[TAR_BLOCK];
            size_t need = TAR_BLOCK - rem, got = 0;
            while (got < need) {
                ssize_t n = read(fd, skip, need - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
        }
    }
    return rc;
}

static int tar_list(int fd, int verbose) {
    unsigned char block[TAR_BLOCK];
    for (;;) {
        int r = tar_read_block(fd, block);
        if (r <= 0) break;
        if (tar_is_zero(block)) break;
        tar_hdr *h = (tar_hdr *)block;
        unsigned int stored = (unsigned int)tar_parse_octal(h->chksum, 8);
        if (stored != tar_checksum(h))
            fprintf(stderr, "tar: warning: bad checksum\n");
        char name[512];
        tar_name_of(h, name, sizeof(name));
        if (verbose) {
            char mode[11];
            tar_mode_string((unsigned)tar_parse_octal(h->mode, 8), mode);
            printf("%s %8llu %s\n", mode, tar_parse_octal(h->size, 12), name);
        } else {
            printf("%s\n", name);
        }
        if (h->typeflag != '5' && h->typeflag != '2')
            tar_skip_data(fd, tar_parse_octal(h->size, 12));
    }
    return 0;
}

int cact_ub_tar(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tar_usage, sizeof(tar_usage) - 1);
        return 0;
    }
    char mode = 0;
    const char *archive = "-";
    const char *chdir_to = NULL;
    int verbose = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'c' || *c == 't' || *c == 'x') mode = *c;
            else if (*c == 'v') verbose = 1;
            else if (*c == 'f') {
                if (c[1]) { archive = c + 1; break; }
                if (i + 1 < argc) { archive = argv[++i]; break; }
                fprintf(stderr, "tar: -f needs an archive name\n");
                return 1;
            } else if (*c == 'C') {
                if (c[1]) { chdir_to = c + 1; break; }
                if (i + 1 < argc) { chdir_to = argv[++i]; break; }
                fprintf(stderr, "tar: -C needs a directory\n");
                return 1;
            } else {
                fprintf(stderr, "tar: unknown option -%c\n", *c);
                return 1;
            }
        }
    }
    if (!mode) { fprintf(stderr, "tar: choose -c, -t or -x\n"); return 1; }
    if (chdir_to && chdir(chdir_to) != 0) {
        fprintf(stderr, "tar: %s: cannot chdir\n", chdir_to);
        return 1;
    }

    if (mode == 'c') {
        int fd = STDOUT_FILENO;
        if (strcmp(archive, "-") != 0) {
            fd = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { fprintf(stderr, "tar: %s: cannot create\n", archive); return 1; }
        }
        int rc = tar_create(fd, argv, i, argc, verbose);
        if (fd != STDOUT_FILENO) close(fd);
        return rc;
    }

    int fd = STDIN_FILENO;
    if (strcmp(archive, "-") != 0) {
        fd = open(archive, O_RDONLY, 0);
        if (fd < 0) { fprintf(stderr, "tar: %s: cannot open\n", archive); return 1; }
    }
    int rc = (mode == 't') ? tar_list(fd, verbose) : tar_extract(fd, verbose);
    if (fd != STDIN_FILENO) close(fd);
    return rc;
}
