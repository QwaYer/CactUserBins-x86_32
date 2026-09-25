/*
 * ex_hash.c — md5sum / sha1sum.
 *
 * MD5 and SHA-1 are not offered by /dev/crypto (only SHA-256/384 are), so they
 * are implemented here in portable C.  Both hash the file in fixed-size chunks,
 * so there is no size limit and no need to hold the file in memory.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>

/* ---------------------------------------------------------------------- MD5 */

typedef struct {
    uint32_t h[4];
    uint64_t len;
    unsigned char buf[64];
    uint32_t have;
} md5_ctx;

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define MD5_STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); (a) = MD5_ROL((a), (s)); (a) += (b)

static void md5_block(md5_ctx *ctx, const unsigned char *p) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];

    MD5_STEP(MD5_F, a, b, c, d, x[0],  0xd76aa478, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[1],  0xe8c7b756, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[2],  0x242070db, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[3],  0xc1bdceee, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[4],  0xf57c0faf, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[5],  0x4787c62a, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[6],  0xa8304613, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[7],  0xfd469501, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[8],  0x698098d8, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[9],  0x8b44f7af, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[10], 0xffff5bb1, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[11], 0x895cd7be, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[12], 0x6b901122, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[13], 0xfd987193, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[14], 0xa679438e, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[15], 0x49b40821, 22);

    MD5_STEP(MD5_G, a, b, c, d, x[1],  0xf61e2562, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[6],  0xc040b340, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[11], 0x265e5a51, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[0],  0xe9b6c7aa, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[5],  0xd62f105d, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[10], 0x02441453, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[15], 0xd8a1e681, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[4],  0xe7d3fbc8, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[9],  0x21e1cde6, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[14], 0xc33707d6, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[3],  0xf4d50d87, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[8],  0x455a14ed, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[13], 0xa9e3e905, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[2],  0xfcefa3f8, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[7],  0x676f02d9, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

    MD5_STEP(MD5_H, a, b, c, d, x[5],  0xfffa3942, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[8],  0x8771f681, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[11], 0x6d9d6122, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[14], 0xfde5380c, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[1],  0xa4beea44, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[4],  0x4bdecfa9, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[7],  0xf6bb4b60, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[10], 0xbebfbc70, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[13], 0x289b7ec6, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[0],  0xeaa127fa, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[3],  0xd4ef3085, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[6],  0x04881d05, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[9],  0xd9d4d039, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[12], 0xe6db99e5, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[15], 0x1fa27cf8, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[2],  0xc4ac5665, 23);

    MD5_STEP(MD5_I, a, b, c, d, x[0],  0xf4292244, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[7],  0x432aff97, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[14], 0xab9423a7, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[5],  0xfc93a039, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[12], 0x655b59c3, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[3],  0x8f0ccc92, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[10], 0xffeff47d, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[1],  0x85845dd1, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[8],  0x6fa87e4f, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[15], 0xfe2ce6e0, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[6],  0xa3014314, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[13], 0x4e0811a1, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[4],  0xf7537e82, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[11], 0xbd3af235, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[2],  0x2ad7d2bb, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[9],  0xeb86d391, 21);

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
}

static void md5_init(md5_ctx *ctx) {
    ctx->h[0] = 0x67452301; ctx->h[1] = 0xefcdab89;
    ctx->h[2] = 0x98badcfe; ctx->h[3] = 0x10325476;
    ctx->len = 0; ctx->have = 0;
}

static void md5_update(md5_ctx *ctx, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    ctx->len += n;
    while (n > 0) {
        size_t take = 64 - ctx->have;
        if (take > n) take = n;
        memcpy(ctx->buf + ctx->have, p, take);
        ctx->have += (uint32_t)take;
        p += take; n -= take;
        if (ctx->have == 64) { md5_block(ctx, ctx->buf); ctx->have = 0; }
    }
}

static void md5_final(md5_ctx *ctx, unsigned char out[16]) {
    uint64_t bits = ctx->len * 8;
    unsigned char pad = 0x80;
    md5_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->have != 56) md5_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (8 * i));
    md5_update(ctx, lenb, 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (unsigned char)(ctx->h[i] >> (8 * j));
}

/* --------------------------------------------------------------------- SHA1 */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    uint32_t have;
} sha1_ctx;

#define SHA1_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(sha1_ctx *ctx, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
        uint32_t t = SHA1_ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

static void sha1_init(sha1_ctx *ctx) {
    ctx->h[0] = 0x67452301; ctx->h[1] = 0xefcdab89; ctx->h[2] = 0x98badcfe;
    ctx->h[3] = 0x10325476; ctx->h[4] = 0xc3d2e1f0;
    ctx->len = 0; ctx->have = 0;
}

static void sha1_update(sha1_ctx *ctx, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    ctx->len += n;
    while (n > 0) {
        size_t take = 64 - ctx->have;
        if (take > n) take = n;
        memcpy(ctx->buf + ctx->have, p, take);
        ctx->have += (uint32_t)take;
        p += take; n -= take;
        if (ctx->have == 64) { sha1_block(ctx, ctx->buf); ctx->have = 0; }
    }
}

static void sha1_final(sha1_ctx *ctx, unsigned char out[20]) {
    uint64_t bits = ctx->len * 8;
    unsigned char pad = 0x80;
    sha1_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->have != 56) sha1_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (8 * (7 - i)));
    sha1_update(ctx, lenb, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (unsigned char)(ctx->h[i] >> (8 * (3 - j)));
}

/* -------------------------------------------------------------------- tool */

static int hash_show(const char *path, int is_md5) {
    const char *prog = is_md5 ? "md5sum" : "sha1sum";
    int fd = path ? open(path, O_RDONLY, 0) : STDIN_FILENO;
    if (fd < 0) {
        fprintf(stderr, "%s: %s: cannot open\n", prog, path);
        return 1;
    }

    struct stat st;
    long long budget = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        budget = (long long)st.st_size;   /* the FS may not report EOF */

    md5_ctx  m; sha1_ctx s;
    md5_init(&m); sha1_init(&s);

    unsigned char chunk[16384];
    for (;;) {
        size_t want = sizeof(chunk);
        if (budget >= 0) {
            if (budget == 0) break;
            if ((long long)want > budget) want = (size_t)budget;
        }
        ssize_t r = read(fd, chunk, want);
        if (r < 0) {
            fprintf(stderr, "%s: %s: read error\n", prog, path ? path : "-");
            if (path) close(fd);
            return 1;
        }
        if (r == 0) break;
        if (budget >= 0) budget -= r;
        if (is_md5) md5_update(&m, chunk, (size_t)r);
        else        sha1_update(&s, chunk, (size_t)r);
    }
    if (path) close(fd);

    unsigned char digest[20];
    int nb;
    if (is_md5) { md5_final(&m, digest); nb = 16; }
    else        { sha1_final(&s, digest); nb = 20; }

    static const char hd[] = "0123456789abcdef";
    char hex[41];
    for (int i = 0; i < nb; i++) {
        hex[2 * i]     = hd[digest[i] >> 4];
        hex[2 * i + 1] = hd[digest[i] & 0x0F];
    }
    hex[2 * nb] = '\0';
    printf("%s  %s\n", hex, path ? path : "-");
    return 0;
}

static const char md5_usage[] = "usage: md5sum [FILE...]\n";

int cact_ub_md5sum(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, md5_usage, sizeof(md5_usage) - 1);
        return 0;
    }
    if (argc < 2) return hash_show(NULL, 1);
    int ret = 0;
    for (int i = 1; i < argc; i++)
        if (hash_show(strcmp(argv[i], "-") == 0 ? NULL : argv[i], 1) != 0) ret = 1;
    return ret;
}

static const char sha1_usage[] = "usage: sha1sum [FILE...]\n";

int cact_ub_sha1sum(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sha1_usage, sizeof(sha1_usage) - 1);
        return 0;
    }
    if (argc < 2) return hash_show(NULL, 0);
    int ret = 0;
    for (int i = 1; i < argc; i++)
        if (hash_show(strcmp(argv[i], "-") == 0 ? NULL : argv[i], 0) != 0) ret = 1;
    return ret;
}
