/*
 * WPA2-PSK crypto for wljoin: SHA-1, HMAC-SHA1, PBKDF2-SHA1 (passphrase -> PMK)
 * and PRF-512 (PMK -> PTK).  Ported from the kernel module's rt2800_wpa.c when
 * the supplicant moved to userspace.
 *
 * Verified by construction against the standard vector: passphrase "password"
 * with SSID "IEEE" gives f42c6fc5…9710a12e (checked by `wljoin --selftest`).
 */

#include <string.h>
#include "wljoin.h"

/* ------------------------------------------------------------------ */
/* SHA-1                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 h[5];
    u64 len;
    u8  buf[64];
    u32 buflen;
} sha1_ctx;

static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const u8 *p) {
    u32 w[80];

    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else             { f = b ^ cc ^ d;                    k = 0xca62c1d6; }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
    c->len = 0; c->buflen = 0;
}

static void sha1_update(sha1_ctx *c, const u8 *data, u32 len) {
    c->len += len;
    while (len) {
        u32 n = 64 - c->buflen;
        if (n > len) n = len;
        memcpy(c->buf + c->buflen, data, n);
        c->buflen += n; data += n; len -= n;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, u8 out[20]) {
    u64 bits = c->len * 8;

    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    u8 zero = 0;
    while (c->buflen != 56)
        sha1_update(c, &zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (u8)(c->h[i] >> 24);
        out[i * 4 + 1] = (u8)(c->h[i] >> 16);
        out[i * 4 + 2] = (u8)(c->h[i] >> 8);
        out[i * 4 + 3] = (u8)c->h[i];
    }
}

static void sha1(const u8 *data, u32 len, u8 out[20]) {
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA1                                                           */
/* ------------------------------------------------------------------ */

void wl_hmac_sha1(const u8 *key, u32 key_len, const u8 *msg, u32 msg_len,
                  u8 out[20]) {
    u8 k[64];

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        sha1(key, key_len, k);
    else
        memcpy(k, key, key_len);

    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    sha1_ctx c;
    u8 inner[20];
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, msg_len);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* PBKDF2-SHA1: passphrase -> PMK (4096 iterations, 2 blocks)          */
/* ------------------------------------------------------------------ */

void wl_pmk(const char *passphrase, const u8 *ssid, u8 ssid_len, u8 pmk[32]) {
    u32 pass_len = 0;
    while (passphrase[pass_len])
        pass_len++;

    for (u32 block = 1; block <= 2; block++) {
        u8 msg[128];
        u32 n = 0;
        memcpy(msg + n, ssid, ssid_len); n += ssid_len;
        msg[n++] = (u8)(block >> 24); msg[n++] = (u8)(block >> 16);
        msg[n++] = (u8)(block >> 8);  msg[n++] = (u8)block;

        u8 u[20], acc[20];
        wl_hmac_sha1((const u8 *)passphrase, pass_len, msg, n, u);
        memcpy(acc, u, 20);

        for (int iter = 1; iter < 4096; iter++) {
            wl_hmac_sha1((const u8 *)passphrase, pass_len, u, 20, u);
            for (int i = 0; i < 20; i++)
                acc[i] ^= u[i];
        }
        memcpy(pmk + (block - 1) * 20, acc, block == 1 ? 20 : 12);
    }
}

/* ------------------------------------------------------------------ */
/* PRF-512: PMK + label + data -> PTK (KCK|KEK|TK)                     */
/* ------------------------------------------------------------------ */

void wl_prf512(const u8 *key, const char *label, const u8 *data, u32 data_len,
               u8 *out, u32 out_len) {
    u32 label_len = 0;
    while (label[label_len])
        label_len++;

    u32 done = 0;
    u8  it = 0;
    while (done < out_len) {
        u8 msg[128];
        u32 n = 0;
        memcpy(msg + n, label, label_len); n += label_len;
        msg[n++] = 0x00;
        memcpy(msg + n, data, data_len); n += data_len;
        msg[n++] = it;

        u8 h[20];
        wl_hmac_sha1(key, 32, msg, n, h);

        u32 take = out_len - done;
        if (take > 20) take = 20;
        memcpy(out + done, h, take);
        done += take;
        it++;
    }
}
