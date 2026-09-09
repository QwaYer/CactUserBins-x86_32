/*
 * fat32_fmt.c — mkfs.fat-style FAT32 formatter for CactOS.
 */

#include "fat32_fmt.h"

#include <string.h>
#include <stdlib.h>

#define F32_RESERVED 32
#define F32_NUM_FATS 2
#define F32_BPS      512

static void set_err(char *err, size_t n, const char *msg) {
    if (!err || !n) return;
    size_t l = strlen(msg);
    if (l >= n) l = n - 1;
    memcpy(err, msg, l);
    err[l] = '\0';
}

static uint32_t ceil_div(uint64_t a, uint32_t b) {
    return (uint32_t)((a + b - 1) / b);
}


static void fat_label_entry(uint8_t *e, const char *label) {
    memset(e, 0, 32);
    memset(e, ' ', 11);
    size_t l = 0;
    while (label[l] && l < 11) {
        char c = label[l];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        e[l] = (uint8_t)c;
        l++;
    }
    e[11] = 0x08;                 /* ATTR_VOLUME_ID */
}

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static void fat_set_entry(uint8_t *fat, uint32_t idx, uint32_t val) {
    put32(fat + (size_t)idx * 4, val);
}

int f32_geometry(uint64_t sectors, f32_geom_t *g) {
    if (!g || sectors < 64) return -1;

    uint32_t spc = 0, fs = 0, cc = 0;
    for (uint32_t cand = 1; cand <= 128; cand <<= 1) {
        if (F32_BPS * cand > 32768) break;
        /* find a FAT size that can hold the cluster count it implies */
        uint32_t nfs = 1, ncc = 0;
        for (int it = 0; it < 100; it++) {
            if ((int64_t)sectors - F32_RESERVED - (int64_t)F32_NUM_FATS * nfs <= 0) {
                nfs = 0;
                break;
            }
            ncc = (uint32_t)((sectors - F32_RESERVED - (uint64_t)F32_NUM_FATS * nfs) / cand);
            uint32_t need = ceil_div(((uint64_t)ncc + 2) * 4, F32_BPS);
            if (nfs >= need)
                break;
            nfs = need;
        }
        if (!nfs) continue;
        if (ncc >= 65525 && ncc <= 0x0FFFFFF5u) {
            spc = cand;
            cc  = ncc;
            fs  = nfs;
            break;
        }
    }
    if (!spc)
        return -1;

    g->sectors_per_cluster = spc;
    g->fat_sectors         = fs;
    g->root_cluster        = 2;
    g->data_start          = F32_RESERVED + F32_NUM_FATS * fs;
    g->cluster_count       = cc;
    return 0;
}

static void make_boot_sector(uint8_t s[512], uint64_t tot_sectors,
                             const f32_geom_t *g, const f32_opts_t *o) {
    memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(s + 3, "CACTOS  ", 8);
    put16(s + 11, 512);                 /* byts_per_sec */
    s[13] = (uint8_t)g->sectors_per_cluster;
    put16(s + 14, F32_RESERVED);        /* rsvd_sec_cnt */
    s[16] = F32_NUM_FATS;
    put16(s + 17, 0);                   /* root_ent_cnt */
    put16(s + 19, 0);                   /* tot_sec16 */
    s[21] = 0xF8;                       /* media */
    put16(s + 22, 0);                   /* fatsz16 */
    put16(s + 24, 63);                  /* sec_per_trk */
    put16(s + 26, 255);                 /* num_heads */
    put32(s + 28, 0);                   /* hidd_sec */
    put32(s + 32, (uint32_t)tot_sectors); /* tot_sec32 */
    put32(s + 36, g->fat_sectors);
    put16(s + 40, 0);                   /* ext_flags */
    put16(s + 42, 0);                   /* fs_ver */
    put32(s + 44, g->root_cluster);
    put16(s + 48, 1);                   /* fsinfo_sec */
    put16(s + 50, 6);                   /* bkboot_sec */
    memset(s + 52, 0, 12);              /* reserved */
    s[64] = 0x80;                       /* drv_num */
    s[66] = 0x29;                       /* boot_sig */
    put32(s + 67, o->serial);
    {
        char lbl[12];
        memset(lbl, ' ', 11);
        lbl[11] = '\0';
        if (o->label[0]) {
            size_t l = strlen(o->label);
            if (l > 11) l = 11;
            memcpy(lbl, o->label, l);
            for (size_t i = 0; i < l; i++)
                if (lbl[i] >= 'a' && lbl[i] <= 'z') lbl[i] -= 32;
        }
        memcpy(s + 71, lbl, 11);
    }
    memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void make_fsinfo(uint8_t s[512], uint32_t free_clusters) {
    memset(s, 0, 512);
    put32(s + 0,   0x41615252u);   /* lead signature */
    put32(s + 484, 0x61417272u);   /* struct signature */
    put32(s + 488, free_clusters);
    put32(s + 492, 2);
    put32(s + 508, 0xAA550000u);
}

int f32_format(f32_disk_t *d, uint64_t sectors, const f32_opts_t *o,
               char *err, size_t err_len) {
    f32_geom_t g;
    f32_opts_t opts;

    if (!d || !d->write_sec) { set_err(err, err_len, "bad args"); return -1; }
    if (f32_geometry(sectors, &g) != 0) {
        set_err(err, err_len, "volume too small/large for FAT32");
        return -1;
    }
    memset(&opts, 0, sizeof(opts));
    if (o) opts = *o;
    if (!opts.serial)
        opts.serial = (uint32_t)sectors ^ 0xF4700000u;

    uint8_t boot[512], fsinfo[512];
    uint64_t ns = sectors;

    make_boot_sector(boot, ns, &g, &opts);
    make_fsinfo(fsinfo, g.cluster_count > 1 ? g.cluster_count - 1 : 0);

    /* sector 0: boot; sector 1: FSInfo; sector 2: reserved zero */
    if (d->write_sec(d->ud, 0, boot) != 0) goto ioerr;
    if (d->write_sec(d->ud, 1, fsinfo) != 0) goto ioerr;
    memset(boot, 0, 512);
    if (d->write_sec(d->ud, 2, boot) != 0) goto ioerr;

    /* backup boot at 6, backup FSInfo at 7 */
    make_boot_sector(boot, ns, &g, &opts);
    make_fsinfo(fsinfo, g.cluster_count > 1 ? g.cluster_count - 1 : 0);
    if (d->write_sec(d->ud, 6, boot) != 0) goto ioerr;
    if (d->write_sec(d->ud, 7, fsinfo) != 0) goto ioerr;

    /* FATs: build one full FAT in memory, then mirror it twice */
    {
        uint8_t *fb = (uint8_t *)malloc((size_t)g.fat_sectors * 512);
        if (!fb) { set_err(err, err_len, "oom"); return -1; }
        memset(fb, 0, (size_t)g.fat_sectors * 512);
        fat_set_entry(fb, 0, 0x0FFFFFF8u);
        fat_set_entry(fb, 1, 0x0FFFFFFFu);
        fat_set_entry(fb, g.root_cluster, 0x0FFFFFFFu);

        for (uint32_t copy = 0; copy < F32_NUM_FATS; copy++) {
            uint64_t base = F32_RESERVED + (uint64_t)copy * g.fat_sectors;
            for (uint32_t i = 0; i < g.fat_sectors; i++)
                if (d->write_sec(d->ud, base + i, fb + (size_t)i * 512) != 0) {
                    free(fb);
                    goto ioerr;
                }
        }
        free(fb);
    }

    /* root directory cluster (cluster 2) — volume label + empty */
    {
        uint64_t start = g.data_start;   /* root cluster start sector */
        uint8_t zero[512];
        memset(zero, 0, 512);
        if (opts.label[0]) {
            fat_label_entry(zero, opts.label);
        }
        for (uint32_t i = 0; i < g.sectors_per_cluster; i++) {
            if (d->write_sec(d->ud, start + i, zero) != 0)
                goto ioerr;
            if (i == 0)
                memset(zero, 0, 512);   /* only the first sector carries it */
        }
    }

    return 0;

ioerr:
    set_err(err, err_len, "I/O error");
    return -1;
}
