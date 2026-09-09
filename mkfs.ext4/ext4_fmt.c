/*
 * ext4_fmt.c — minimal mke2fs-style ext4 formatter for CactOS.
 *
 * Layout written (all values little-endian):
 *   - primary superblock at byte 1024 of the volume
 *   - group 0: boot block (block 0), group-descriptor table, block bitmap,
 *     inode bitmap, inode table, then data (root + lost+found dirs)
 *   - every group with a superblock copy (group 0 plus powers of 3/5/7)
 *     also carries a group-descriptor copy; sparse_super is enabled.
 *   - no journal, no flex_bg, no resize_inode, no metadata_csum/64bit.
 *
 * Group bitmaps and free counters are fully initialised so that e2fsck -fn
 * passes and the CactOS ext4 module can allocate blocks/inodes immediately.
 */

#include "ext4_fmt.h"

#include <string.h>
#include <stdlib.h>

#define E4_SUPER_MAGIC     0xEF53
#define E4_EXTENT_MAGIC    0xF30A
#define E4_EXTENTS_FL      0x00080000u
#define E4_INODE_ROOT      2
#define E4_INODE_LOSTFOUND 11

#define FEATURE_COMPAT_JOURNAL 0x0004u
#define FEATURE_INCOMPAT_FILETYPE 0x0002u
#define FEATURE_INCOMPAT_EXTENTS  0x0040u
#define FEATURE_RO_SPARSE_SUPER   0x0001u
#define FEATURE_RO_EXTRA_ISIZE    0x0040u

/* On-disk superblock — offsets match Linux/ext4 and the CactOS module. */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t  s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint32_t s_last_error_line;
    uint64_t s_last_error_block;
    uint8_t  s_last_error_func[32];
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_clusters;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint32_t s_reserved[98];
    uint32_t s_checksum;
} __attribute__((packed)) e4_sb_t;

/* 64-byte group descriptor. */
typedef struct {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) e4_gd_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) e4_extent_header_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) e4_extent_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed)) e4_dirent_t;   /* name[] follows */


static void set_err(char *err, size_t n, const char *msg) {
    if (!err || !n) return;
    size_t l = strlen(msg);
    if (l >= n) l = n - 1;
    memcpy(err, msg, l);
    err[l] = '\0';
}

/* ---- little endian writers ------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* ---- sector / block I/O ---------------------------------------------------- */

typedef struct {
    e4_disk_t *d;
    uint32_t   bs;
    uint32_t   spb;               /* sectors per block */
    uint8_t   *buf;               /* one block scratch  */
} io_t;

static int wr_block(io_t *io, uint32_t block, const uint8_t *data) {
    for (uint32_t i = 0; i < io->spb; i++)
        if (io->d->write_sec(io->d->ud, (uint64_t)block * io->spb + i,
                             data + (size_t)i * 512) != 0)
            return -1;
    return 0;
}

/* ---- helpers ---------------------------------------------------------------- */

static void bm_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bm_clear(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static int is_sparse_group(uint32_t g) {
    if (g == 0 || g == 1) return 1;      /* group 1 always carries a copy */
    static const uint32_t primes[3] = { 3, 5, 7 };
    for (int i = 0; i < 3; i++) {
        uint64_t x = primes[i];
        while (x <= g) {
            if (x == g) return 1;
            x *= primes[i];
        }
    }
    return 0;
}

static void mk_uuid(uint32_t seed, uint8_t out[16]) {
    uint32_t x = seed ? seed : 0xCAC7012Bu;
    for (int i = 0; i < 16; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = (uint8_t)(x ^ (uint32_t)(i * 2654435761u));
    }
    out[6] = (uint8_t)((out[6] & 0x0F) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

/* Write a 256-byte inode image at @in (isize >= 256 recommended). */
static void make_dir_inode(uint8_t *in, uint32_t isz, uint32_t blk,
                           uint32_t size_bytes, uint32_t now, uint32_t gen,
                           uint16_t links) {
    memset(in, 0, isz);
    put16(in + 0,  0x41ED);                    /* S_IFDIR | 0755 */
    put16(in + 2,  0);                          /* uid */
    put32(in + 4,  size_bytes);                 /* i_size_lo */
    put32(in + 8,  now); put32(in + 12, now); put32(in + 16, now);
    put32(in + 20, 0);                          /* dtime */
    put16(in + 24, 0);                          /* gid */
    put16(in + 26, links);
    put32(in + 28, size_bytes / 512);           /* i_blocks_lo (512B units) */
    put32(in + 32, E4_EXTENTS_FL);
    /* i_block[15] at offset 40 */
    uint8_t *ib = in + 40;
    put16(ib + 0,  E4_EXTENT_MAGIC);
    put16(ib + 2,  1);                          /* eh_entries */
    put16(ib + 4,  4);                          /* eh_max */
    put16(ib + 6,  0);                          /* depth */
    put32(ib + 8,  0);                          /* generation */
    put32(ib + 12, 0);                          /* ee_block  */
    put16(ib + 16, 1);                          /* ee_len    */
    put16(ib + 18, 0);                          /* ee_start_hi */
    put32(ib + 20, blk);                        /* ee_start_lo */
    /* generation */
    put32(in + 100, gen);
    if (isz >= 160)
        put16(in + 128, 32);                    /* i_extra_isize */
}

static void dir_put_entry(uint8_t *p, uint32_t ino, uint16_t reclen,
                          const char *name, uint8_t ft) {
    e4_dirent_t *de = (e4_dirent_t *)p;
    de->inode     = ino;
    de->rec_len   = reclen;
    de->name_len  = (uint8_t)strlen(name);
    de->file_type = ft;
    memcpy(p + 8, name, de->name_len);
}

/* Fill a directory block: "." + ".." (+ trailing entry with a fat rec_len). */
static void make_dir_block(uint8_t *blk, uint32_t bs, uint32_t self_ino,
                           uint32_t parent_ino, const char *extra_name,
                           uint32_t extra_ino) {
    memset(blk, 0, bs);
    size_t off = 0;

    dir_put_entry(blk + off, self_ino, 12, ".", 2);
    off += 12;
    if (extra_name && extra_name[0]) {
        dir_put_entry(blk + off, parent_ino, 12, "..", 2);
        off += 12;
        dir_put_entry(blk + off, extra_ino,
                      (uint16_t)(bs - off), extra_name, 2);
    } else {
        dir_put_entry(blk + off, parent_ino, (uint16_t)(bs - off), "..", 2);
    }
}

int e4_format(e4_disk_t *d, uint64_t bytes, const e4_opts_t *o, char *err,
              size_t err_len) {
    if (!d || !d->write_sec || !o) {
        if (err) set_err(err, err_len, "bad args");
        return -1;
    }
    uint32_t bs  = o->block_size ? o->block_size : E4_BLOCK_DEFAULT;
    uint32_t isz = o->inode_size ? o->inode_size : E4_INODE_DEFAULT;
    if (bs != 2048 && bs != 4096) {
        if (err) set_err(err, err_len, "block size must be 2048 or 4096");
        return -1;
    }
    if (isz < 128 || isz > bs) {
        if (err) set_err(err, err_len, "bad inode size");
        return -1;
    }

    uint64_t total_bytes = (bytes / 512) * 512;
    uint32_t total_blocks = (uint32_t)(total_bytes / bs);
    if (total_blocks < 64) {
        if (err) set_err(err, err_len, "volume too small");
        return -1;
    }

    io_t io;
    io.d = d; io.bs = bs; io.spb = bs / 512;
    io.buf = (uint8_t *)malloc(bs);
    if (!io.buf) { if (err) set_err(err, err_len, "oom"); return -1; }

    uint32_t bpg   = 8 * bs;                     /* blocks per group */
    uint32_t groups = (total_blocks + bpg - 1) / bpg;
    uint32_t desc_size = 32;
    uint32_t gdt_blocks = (groups * desc_size + bs - 1) / bs;

    /* inode ratio 16 KiB/ inode, spread evenly, 8-aligned, >= 12 per group */
    uint64_t want = (bytes / 16384) + 1;
    if (want < 16) want = 16;
    uint32_t ipg = (uint32_t)((want + groups - 1) / groups);
    ipg = (ipg + 7u) & ~7u;
    if (ipg < 12) ipg = 12;
    uint32_t max_ipg = bs * 8u;
    if (ipg > max_ipg) ipg = max_ipg;
    uint32_t it_blocks = (ipg * isz + bs - 1) / bs;
    uint32_t total_inodes = groups * ipg;

    /* ---- per-group geometry ---- */
    /* recompute for writes: bb/ib/it absolute, has_sb, free blocks */
    uint32_t *bb    = (uint32_t *)malloc(groups * sizeof(uint32_t));
    uint32_t *ibm   = (uint32_t *)malloc(groups * sizeof(uint32_t));
    uint32_t *itbl  = (uint32_t *)malloc(groups * sizeof(uint32_t));
    uint32_t *meta_len = (uint32_t *)malloc(groups * sizeof(uint32_t));
    uint32_t *real  = (uint32_t *)malloc(groups * sizeof(uint32_t));
    uint8_t  *has_sb = (uint8_t *)malloc(groups);
    if (!bb || !ibm || !itbl || !meta_len || !real || !has_sb) {
        free(bb); free(ibm); free(itbl); free(meta_len); free(real); free(has_sb);
        free(io.buf);
        if (err) set_err(err, err_len, "oom");
        return -1;
    }

    uint64_t free_blocks_total = 0;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t base = g * bpg;
        uint32_t r = (base + bpg <= total_blocks) ? bpg : (total_blocks - base);
        real[g] = r;
        uint8_t sb = (uint8_t)is_sparse_group(g);
        has_sb[g] = sb;
        uint32_t c = sb ? (1 + gdt_blocks) : 0;   /* sb block + gdt copy */
        bb[g]   = base + c;
        ibm[g]  = bb[g] + 1;
        itbl[g] = ibm[g] + 1;
        meta_len[g] = c + 2 + it_blocks;

        if (meta_len[g] + (g == 0 ? 2u : 0u) > r) {
            free(bb); free(ibm); free(itbl); free(meta_len); free(real); free(has_sb);
            free(io.buf);
            if (err) set_err(err, err_len, "volume too small for metadata");
            return -1;
        }
        uint64_t free_g = r - meta_len[g];
        if (g == 0) free_g -= 2;                  /* root + lost+found dirs */
        free_blocks_total += free_g;
    }

    /* ---- superblock ---- */
    e4_sb_t sb;
    memset(&sb, 0, sizeof(sb));
    uint32_t now = o->now ? o->now : 1700000000u;
    sb.s_inodes_count          = total_inodes;
    sb.s_blocks_count_lo       = total_blocks;
    sb.s_free_blocks_count_lo  = (uint32_t)free_blocks_total;
    sb.s_free_inodes_count     = total_inodes - E4_INODE_LOSTFOUND; /* 1..11 */
    sb.s_first_data_block      = 0;
    sb.s_log_block_size        = (bs == 4096) ? 2 : 1;
    sb.s_log_cluster_size      = sb.s_log_block_size;
    sb.s_blocks_per_group      = bpg;
    sb.s_clusters_per_group    = bpg;
    sb.s_inodes_per_group      = ipg;
    sb.s_mtime = now; sb.s_wtime = now;
    sb.s_mnt_count = 0;
    sb.s_max_mnt_count = 0xFFFF;
    sb.s_magic = E4_SUPER_MAGIC;
    sb.s_state = 1;
    sb.s_errors = 1;
    sb.s_lastcheck = now;
    sb.s_creator_os = 0;
    sb.s_rev_level = 1;                            /* dynamic rev */
    sb.s_first_ino = E4_INODE_LOSTFOUND;
    sb.s_inode_size = (uint16_t)isz;
    sb.s_desc_size = 0;   /* 32-byte descriptors (mke2fs convention) */
    sb.s_feature_compat     = 0;                   /* no journal */
    sb.s_feature_incompat   = FEATURE_INCOMPAT_FILETYPE | FEATURE_INCOMPAT_EXTENTS;
    sb.s_feature_ro_compat  = FEATURE_RO_SPARSE_SUPER | FEATURE_RO_EXTRA_ISIZE;
    sb.s_mkfs_time = now;
    sb.s_min_extra_isize = 32;
    sb.s_want_extra_isize = 32;
    if (isz < 256)
        sb.s_feature_ro_compat &= ~FEATURE_RO_EXTRA_ISIZE;
    mk_uuid(o->uuid_seed, sb.s_uuid);
    if (o->label[0]) {
        memset(sb.s_volume_name, 0, sizeof(sb.s_volume_name));
        strncpy(sb.s_volume_name, o->label, sizeof(sb.s_volume_name) - 1);
    }

    /* ---- group descriptor table buffer ---- */
    size_t gdt_bytes = (size_t)gdt_blocks * bs;
    uint8_t *gdt = (uint8_t *)malloc(gdt_bytes);
    if (!gdt) {
        free(bb); free(ibm); free(itbl); free(meta_len); free(real); free(has_sb);
        free(io.buf);
        if (err) set_err(err, err_len, "oom");
        return -1;
    }
    memset(gdt, 0, gdt_bytes);

    for (uint32_t g = 0; g < groups; g++) {
        e4_gd_t gd;
        memset(&gd, 0, sizeof(gd));
        gd.bg_block_bitmap_lo  = bb[g];
        gd.bg_inode_bitmap_lo  = ibm[g];
        gd.bg_inode_table_lo   = itbl[g];
        uint32_t used_inodes = (g == 0) ? E4_INODE_LOSTFOUND : 0;
        gd.bg_free_blocks_count_lo  = (uint16_t)(real[g] - meta_len[g] -
                                                 (g == 0 ? 2u : 0u));
        gd.bg_free_inodes_count_lo  = (uint16_t)(ipg - used_inodes);
        gd.bg_used_dirs_count_lo    = (g == 0) ? 2 : 0;
        memcpy(gdt + (size_t)g * desc_size, &gd, 32); /* 32-byte rows */
    }

    /* ---- write each group ---- */
    uint8_t *block = io.buf;
    uint8_t *bm    = (uint8_t *)malloc(bs);
    uint8_t *ibm_p = (uint8_t *)malloc(bs);
    if (!bm || !ibm_p) goto oom;

    for (uint32_t g = 0; g < groups; g++) {
        uint32_t base = g * bpg;
        uint32_t r    = real[g];
        uint32_t c    = has_sb[g] ? (1 + gdt_blocks) : 0;

        /* block bitmap: mark sb/gdt copy + bb/ib/it of this group */
        memset(bm, 0, bs);
        if (has_sb[g]) {
            for (uint32_t i = 0; i <= gdt_blocks && i < r; i++)
                bm_set(bm, i);
        }
        for (uint32_t i = 0; i < it_blocks + 2; i++) {
            uint32_t idx = c + i;                 /* bb, ib, it blocks */
            if (idx < r) bm_set(bm, idx);
        }
        if (g == 0) {
            /* root + lost+found dir blocks follow the inode table */
            uint32_t d0 = c + 2 + it_blocks;
            if (d0 < r) bm_set(bm, d0);
            if (d0 + 1 < r) bm_set(bm, d0 + 1);
        }
        if (r < bpg) {
            /* padding bits beyond the partial group must read as used */
            for (uint32_t i = r; i < bpg; i++)
                bm_set(bm, i);
        }
        if (wr_block(&io, bb[g], bm) != 0) goto ioerr;

        /* inode bitmap: set (used) plus padding to 1, free inodes to 0 */
        memset(ibm_p, 0xFF, bs);
        {
            uint32_t used = (g == 0) ? E4_INODE_LOSTFOUND : 0;
            for (uint32_t i = used; i < ipg; i++)
                bm_clear(ibm_p, i);            /* free inodes */
        }
        if (wr_block(&io, ibm[g], ibm_p) != 0) goto ioerr;

        /* zero inode table, then plant root / lost+found in group 0 */
        memset(block, 0, bs);
        for (uint32_t i = 0; i < it_blocks; i++)
            if (wr_block(&io, itbl[g] + i, block) != 0) goto ioerr;

        if (g == 0) {
            uint32_t d0 = c + 2 + it_blocks;
            uint32_t root_blk   = base + d0;
            uint32_t lost_blk   = base + d0 + 1;

            uint8_t *inode = (uint8_t *)malloc(isz);
            if (!inode) goto oom;

            /* first inode-table block: inode 2 = / and inode 11 = /lost+found */
            memset(block, 0, bs);
            make_dir_inode(inode, isz, root_blk, bs, now, o->uuid_seed, 3);
            memcpy(block + (size_t)(E4_INODE_ROOT - 1) * isz, inode, isz);
            make_dir_inode(inode, isz, lost_blk, bs, now, o->uuid_seed, 2);
            memcpy(block + (size_t)(E4_INODE_LOSTFOUND - 1) * isz, inode, isz);
            free(inode);

            if (wr_block(&io, itbl[g], block) != 0) goto ioerr;

            /* root directory contents */
            memset(block, 0, bs);
            make_dir_block(block, bs, E4_INODE_ROOT, E4_INODE_ROOT,
                           "lost+found", E4_INODE_LOSTFOUND);
            if (wr_block(&io, root_blk, block) != 0) goto ioerr;

            /* lost+found contents */
            memset(block, 0, bs);
            make_dir_block(block, bs, E4_INODE_LOSTFOUND, E4_INODE_ROOT,
                           NULL, 0);
            if (wr_block(&io, lost_blk, block) != 0) goto ioerr;
        }
    }

    /* ---- write primary superblock (block 0, byte 1024) ---- */
    memset(block, 0, bs);
    memcpy(block + 1024, &sb, sizeof(sb) < 1024 ? sizeof(sb) : 1024);
    if (wr_block(&io, 0, block) != 0) goto ioerr;

    /* ---- group descriptor table + backups ---- */
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t base = g * bpg;
        if (!has_sb[g]) continue;

        uint32_t gd_abs = (g == 0) ? 1 : base + 1;
        for (uint32_t i = 0; i < gdt_blocks; i++)
            if (wr_block(&io, gd_abs + i, gdt + (size_t)i * bs) != 0)
                goto ioerr;

        if (g != 0) {   /* backup superblock in the group's first block */
            memset(block, 0, bs);
            memcpy(block + 1024, &sb, sizeof(sb) < 1024 ? sizeof(sb) : 1024);
            if (wr_block(&io, base, block) != 0) goto ioerr;
        }
    }

    free(bm); free(ibm_p);
    free(gdt); free(bb); free(ibm); free(itbl); free(meta_len); free(real);
    free(has_sb); free(io.buf);
    return 0;

ioerr:
    if (err) set_err(err, err_len, "I/O error");
    free(bm); free(ibm_p);
    free(gdt); free(bb); free(ibm); free(itbl); free(meta_len); free(real);
    free(has_sb); free(io.buf);
    return -1;
oom:
    if (err) set_err(err, err_len, "oom");
    free(bm); free(ibm_p);
    free(gdt); free(bb); free(ibm); free(itbl); free(meta_len); free(real);
    free(has_sb); free(io.buf);
    return -1;
}
