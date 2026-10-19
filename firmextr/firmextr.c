/*
 * firmextr.c — extract individual files from the compressed "localrepo"
 * archive.
 *
 *   firmextr [-C DIR] [-f ARCHIVE] [-l] MEMBER...
 *
 * The archive is one zstd frame wrapping a ustar image of everything that does
 * not fit in initfs (firmware, /usr/lib/cact-install, the offline cactpkg
 * repository).  It is read as a stream: only the requested members are written,
 * and the pass stops as soon as the last one is found, so pulling one firmware
 * blob out of a 300 MB archive costs a single sequential read of the compressed
 * data instead of a full unpack.
 *
 * MEMBER is the path inside the archive without a leading '/', e.g.
 *   firmextr usr/lib/firmware/rt2870.bin
 * writes it to /usr/lib/firmware/rt2870.bin (or under -C DIR).  The default
 * archive is /tmp/localrepo.tar.zst, the copy cgoct drops in tmpfs at boot.
 *
 * The zstd streaming API needs both I/O buffers at least ZSTD_DStreamOutSize()
 * (128 KB) large, and the DStream workspace is sized from the frame's window —
 * both live on the heap, since the user stack is only 16 KiB.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <errno.h>
#include <stdint.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "compat/zstd.h"

#define TAR_BLOCK       512
#define COPY_BUF        2048
#define DEFAULT_ARCHIVE "/tmp/localrepo.tar.zst"

typedef struct {
    int            fd;
    ZSTD_DStream  *ds;
    void          *ws;
    unsigned char *in;    size_t in_cap,  in_len,  in_pos;
    unsigned char *out;   size_t out_cap, out_len, out_pos;
    int            ended;      /* compressed input exhausted / frame complete */
    int            failed;
} zstream;

/* ustar header, field-for-field (see common/ex_archive.c). */
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

static const char fw_usage[] =
    "usage: firmextr [-C DIR] [-f ARCHIVE] [-l] MEMBER...\n"
    "  -f FILE  archive to read (default " DEFAULT_ARCHIVE ")\n"
    "  -C DIR   extract under DIR (default /)\n"
    "  -l       list the members instead of extracting\n";

/* ── zstd stream ─────────────────────────────────────────────────────────── */

static void z_close(zstream *z) {
    free(z->ws);
    free(z->in);
    free(z->out);
    z->ws = NULL;
    z->in = NULL;
    z->out = NULL;
}

static int z_open(zstream *z, int fd) {
    memset(z, 0, sizeof(*z));
    z->fd = fd;

    size_t io = ZSTD_DStreamOutSize();
    if (io < 4096) io = 4096;
    z->in_cap = io;
    z->out_cap = io;
    z->in = malloc(z->in_cap);
    z->out = malloc(z->out_cap);
    if (!z->in || !z->out) { z->failed = 1; return -1; }

    /* The frame header (which may take a second read) carries the window size
     * the DStream must be sized for. */
    ZSTD_FrameHeader fh;
    for (;;) {
        if (z->in_len == z->in_cap) { z->failed = 1; return -1; }
        ssize_t n = read(z->fd, z->in + z->in_len, z->in_cap - z->in_len);
        if (n <= 0) { z->failed = 1; return -1; }
        z->in_len += (size_t)n;
        size_t r = ZSTD_getFrameHeader(&fh, z->in, z->in_len);
        if (ZSTD_isError(r)) { z->failed = 1; return -1; }
        if (r == 0) break;
    }

    size_t ws_size = ZSTD_estimateDStreamSize(fh.windowSize);
    z->ws = malloc(ws_size);
    if (!z->ws) { z->failed = 1; return -1; }
    z->ds = ZSTD_initStaticDStream(z->ws, ws_size);
    if (!z->ds) { z->failed = 1; return -1; }
    return 0;
}

/* Refill out[] with decompressed bytes.  1 = data, 0 = end, -1 = error. */
static int z_refill(zstream *z) {
    if (z->failed) return -1;
    for (;;) {
        if (z->in_pos == z->in_len) {
            if (z->ended) return 0;
            ssize_t n = read(z->fd, z->in, z->in_cap);
            if (n < 0) { z->failed = 1; return -1; }
            z->in_len = (size_t)n;
            z->in_pos = 0;
            if (n == 0) { z->ended = 1; return 0; }
        }
        ZSTD_inBuffer  in  = { z->in, z->in_len, z->in_pos };
        ZSTD_outBuffer out = { z->out, z->out_cap, 0 };
        size_t r = ZSTD_decompressStream(z->ds, &out, &in);
        if (ZSTD_isError(r)) {
            fprintf(stderr, "firmextr: zstd: %s\n", ZSTD_getErrorName(r));
            z->failed = 1;
            return -1;
        }
        z->in_pos   = in.pos;      /* absolute offset within the current input buffer */
        z->out_len  = out.pos;
        z->out_pos  = 0;
        if (z->out_len > 0) return 1;
        if (r == 0) { z->ended = 1; return 0; }   /* frame complete */
    }
}

static size_t z_read(zstream *z, void *buf, size_t n) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        if (z->out_pos == z->out_len && z_refill(z) <= 0) break;
        size_t avail = z->out_len - z->out_pos;
        size_t want  = n - got;
        if (want > avail) want = avail;
        memcpy(p + got, z->out + z->out_pos, want);
        z->out_pos += want;
        got        += want;
    }
    return got;
}

static int z_skip(zstream *z, unsigned long long n) {
    unsigned char buf[COPY_BUF];
    while (n) {
        size_t want = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        size_t got  = z_read(z, buf, want);
        if (!got) return -1;
        n -= got;
    }
    return 0;
}

/* ── tar ─────────────────────────────────────────────────────────────────── */

static unsigned long long tar_size(const char *f) {
    if ((unsigned char)f[0] & 0x80) {          /* base-256 (GNU) */
        unsigned long long v = 0;
        for (int i = 1; i < 12; i++) v = (v << 8) | (unsigned char)f[i];
        return v;
    }
    unsigned long long v = 0;
    for (int i = 0; i < 12; i++) {
        char c = f[i];
        if (c == ' ' || c == '\0') break;
        if (c < '0' || c > '7') break;
        v = v * 8 + (unsigned long long)(c - '0');
    }
    return v;
}

static int tar_is_zero(const unsigned char *b) {
    for (int i = 0; i < TAR_BLOCK; i++) if (b[i]) return 0;
    return 1;
}

static void tar_name(const tar_hdr *h, char *out, size_t n) {
    if (h->prefix[0]) snprintf(out, n, "%s/%s", h->prefix, h->name);
    else              snprintf(out, n, "%s", h->name);
}

static void tar_mode(const tar_hdr *h, unsigned long long *mode) {
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        char c = h->mode[i];
        if (c == ' ' || c == '\0') break;
        if (c < '0' || c > '7') break;
        v = v * 8 + (unsigned long long)(c - '0');
    }
    *mode = v & 07777;
}

/* ── extraction ──────────────────────────────────────────────────────────── */

static int mkparent(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return 0;
    *slash = '\0';
    return cact_ub_mkdir_p(tmp, 0755);
}

static int write_member(zstream *z, const char *path, unsigned long long size,
                        unsigned long long mode) {
    if (mkparent(path) != 0) {
        fprintf(stderr, "firmextr: %s: cannot create directory\n", path);
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)mode);
    if (fd < 0) {
        fprintf(stderr, "firmextr: %s: cannot create\n", path);
        return -1;
    }
    unsigned char buf[COPY_BUF];
    unsigned long long left = size;
    int rc = 0;
    while (left) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t got  = z_read(z, buf, want);
        if (!got) { rc = -1; break; }
        if (cact_ub_write_all(fd, buf, got) != 0) { rc = -1; break; }
        left -= got;
    }
    close(fd);
    if (rc != 0) fprintf(stderr, "firmextr: %s: write failed\n", path);
    return rc;
}

/* A pattern is either an exact member path, or — when it ends with '/' — a
 * subtree prefix that may match many members (the installer pulls the whole
 * usr/lib/cact-install/ tree this way). */
static const char *pat_norm(const char *p) {
    while (*p == '/') p++;
    return p;
}

static int pat_is_prefix(const char *p) {
    size_t l = strlen(p);
    return l > 0 && p[l - 1] == '/';
}

static int member_matches(const char *member, const char *p) {
    if (pat_is_prefix(p))
        return strncmp(p, member, strlen(p)) == 0;
    return strcmp(p, member) == 0;
}

/* Build the on-disk path for an archive member: DIR + member, or /member. */
static int dest_of(const char *root, const char *member, char *out, size_t n) {
    const char *r = root;
    size_t rl = r ? strlen(r) : 0;
    while (rl > 0 && r[rl - 1] == '/') rl--;     /* drop trailing slashes */
    if (rl == 0) return snprintf(out, n, "/%s", member) < (int)n ? 0 : -1;
    return snprintf(out, n, "%.*s/%s", (int)rl, r, member) < (int)n ? 0 : -1;
}

static int firmextr_run(zstream *z, const char *root, int list,
                        char **pat, int npat) {
    /* Listing takes no member arguments, and malloc(0) returns NULL in CactLibc
     * — allocating unconditionally made `firmextr -l` exit before it read a
     * single block, printing nothing. */
    int *satisfied = NULL;
    if (npat > 0) {
        satisfied = malloc((size_t)npat * sizeof(int));
        if (!satisfied) return -1;
    }
    int unbounded = 0;
    for (int i = 0; i < npat; i++) {
        satisfied[i] = 0;
        if (pat_is_prefix(pat_norm(pat[i]))) unbounded = 1;
    }

    unsigned char blk[TAR_BLOCK];
    int rc = 0;
    for (;;) {
        if (z_read(z, blk, TAR_BLOCK) != TAR_BLOCK) break;
        if (tar_is_zero(blk)) break;

        const tar_hdr *h = (const tar_hdr *)blk;
        char member[512];
        tar_name(h, member, sizeof(member));
        unsigned long long size = tar_size(h->size);
        char type = h->typeflag;

        if (list) printf("%s\n", member);

        int matched = 0;
        for (int i = 0; i < npat; i++) {
            if (member_matches(member, pat_norm(pat[i]))) {
                satisfied[i] = 1;
                matched = 1;
            }
        }

        int want = (!list && type != '5' && type != '2' && matched);
        if (want) {
            char dest[1200];
            unsigned long long mode;
            tar_mode(h, &mode);
            if (dest_of(root, member, dest, sizeof(dest)) != 0) {
                fprintf(stderr, "firmextr: %s: path too long\n", member);
                rc = 1;
                z_skip(z, size);
            } else if (write_member(z, dest, size, mode) != 0) {
                rc = 1;
            }
        } else {
            z_skip(z, size);
        }
        if (z_skip(z, (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK) != 0) break;

        if (list || unbounded) continue;   /* a subtree walk reads to the end */
        int all = 1;
        for (int i = 0; i < npat; i++)
            if (!satisfied[i]) { all = 0; break; }
        if (all) break;                    /* every requested member is out */
    }

    if (!list) {
        int missing = 0;
        for (int i = 0; i < npat; i++)
            if (!satisfied[i]) missing++;
        if (missing) {
            fprintf(stderr, "firmextr: %d of %d member(s) not found\n",
                    missing, npat);
            rc = 1;
        }
    }
    free(satisfied);
    return rc;
}

int cact_ub_firmextr(char **argv, int argc) {
    const char *archive = DEFAULT_ARCHIVE;
    const char *root = "/";
    int list = 0;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            write(STDOUT_FILENO, fw_usage, sizeof(fw_usage) - 1);
            return 0;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'l') list = 1;
            else if (*c == 'f') {
                if (c[1]) { archive = c + 1; break; }
                if (i + 1 < argc) { archive = argv[++i]; break; }
                fprintf(stderr, "firmextr: -f needs an archive name\n");
                return 2;
            } else if (*c == 'C') {
                if (c[1]) { root = c + 1; break; }
                if (i + 1 < argc) { root = argv[++i]; break; }
                fprintf(stderr, "firmextr: -C needs a directory\n");
                return 2;
            } else {
                fprintf(stderr, "firmextr: unknown option -%c\n", *c);
                return 2;
            }
        }
    }

    int npat = argc - i;
    if (!list && npat == 0) {
        write(STDERR_FILENO, fw_usage, sizeof(fw_usage) - 1);
        return 2;
    }

    int fd = STDIN_FILENO;
    if (strcmp(archive, "-") != 0) {
        fd = open(archive, O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "firmextr: %s: cannot open\n", archive);
            return 1;
        }
    }

    zstream z;
    if (z_open(&z, fd) != 0) {
        fprintf(stderr, "firmextr: %s: not a readable zstd archive\n", archive);
        z_close(&z);
        if (fd != STDIN_FILENO) close(fd);
        return 1;
    }

    int rc = firmextr_run(&z, root, list, &argv[i], npat);

    z_close(&z);
    if (fd != STDIN_FILENO) close(fd);
    return rc;
}
