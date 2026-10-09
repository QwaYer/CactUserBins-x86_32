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

/* ------------------------------------------------------------------ */
/* AES-128 + RFC 3394 key unwrap                                       */
/*                                                                     */
/* The AP's message 3 carries the GTK inside key data wrapped with the */
/* KEK (AES-Key-Wrap; the Encrypted Key Data bit is set in the usual    */
/* case), so the group key can only be read after unwrapping it.        */
/* ------------------------------------------------------------------ */

static u8 aes_sbox[256];
static u8 aes_isbox[256];
static u8 aes_tables_ready;

static u8 gf_mul(u8 a, u8 b) {
    u8 p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = (u8)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b = (u8)(b >> 1);
    }
    return p;
}

static u8 rotl8(u8 x, int n) {
    return (u8)((x << n) | (x >> (8 - n)));
}

/* S-box = affine transform of the GF(2^8) inverse (x^254); the inverse box
 * falls out of the same pass.  Built once, on first unwrap. */
static void aes_build_tables(void) {
    if (aes_tables_ready)
        return;
    for (int i = 0; i < 256; i++) {
        u8 x = 0;
        if (i) {
            x = 1;
            for (int e = 0; e < 254; e++)
                x = gf_mul(x, (u8)i);
        }
        u8 s = (u8)(x ^ rotl8(x, 1) ^ rotl8(x, 2) ^ rotl8(x, 3) ^
                    rotl8(x, 4) ^ 0x63);
        aes_sbox[i] = s;
        aes_isbox[s] = (u8)i;
    }
    aes_tables_ready = 1;
}

static const u8 aes_rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static void aes_expand(const u8 key[16], u8 rk[176]) {
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        u8 t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if ((i & 3) == 0) {
            u8 b = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = b;
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
            t[0] ^= aes_rcon[(i >> 2) - 1];
        }
        for (int j = 0; j < 4; j++)
            rk[i * 4 + j] = (u8)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void aes_add_rk(u8 s[16], const u8 *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void aes_sub(u8 s[16], const u8 *box) {
    for (int i = 0; i < 16; i++) s[i] = box[s[i]];
}

static void aes_inv_shift_rows(u8 s[16]) {
    u8 o[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r + 4 * c] = s[r + 4 * ((c - r + 4) & 3)];
    memcpy(s, o, 16);
}

static void aes_inv_mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *p = s + 4 * c;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
        p[1] = (u8)(gf_mul(a0, 9)  ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
        p[2] = (u8)(gf_mul(a0, 13) ^ gf_mul(a1, 9)  ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
        p[3] = (u8)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9)  ^ gf_mul(a3, 14));
    }
}

static void aes_decrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);
    aes_add_rk(s, rk + 160);
    for (int r = 9; r >= 1; r--) {
        aes_inv_shift_rows(s);
        aes_sub(s, aes_isbox);
        aes_add_rk(s, rk + 16 * r);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_sub(s, aes_isbox);
    aes_add_rk(s, rk);
    memcpy(out, s, 16);
}

/* RFC 3394 key unwrap: KEK over n wrapped 64-bit blocks, returns the plaintext
 * length (8*n) or -1 (bad length or the 0xA6A6A6A6A6A6A6A6 integrity check). */
int wl_aes_unwrap(const u8 kek[16], const u8 *in, int in_len, u8 *out) {
    static const u8 iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
    u8 rk[176], a[8], r[32][8];
    int n;

    if (in_len < 24 || (in_len & 7))
        return -1;
    n = in_len / 8 - 1;
    if (n > 32)
        return -1;

    aes_build_tables();
    aes_expand(kek, rk);
    memcpy(a, in, 8);
    for (int i = 0; i < n; i++)
        memcpy(r[i], in + 8 + i * 8, 8);

    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            u8 blk[16], b[16];
            u8 t = (u8)(n * j + i);

            memcpy(blk, a, 8);
            blk[7] ^= t;                 /* A ^ (n*j + i), t fits one octet */
            memcpy(blk + 8, r[i - 1], 8);
            aes_decrypt_block(rk, blk, b);
            memcpy(a, b, 8);
            memcpy(r[i - 1], b + 8, 8);
        }
    }

    if (memcmp(a, iv, 8) != 0)
        return -1;
    for (int i = 0; i < n; i++)
        memcpy(out + i * 8, r[i], 8);
    return n * 8;
}
