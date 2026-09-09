#ifndef CACT_MKFS_FAT32_FMT_H
#define CACT_MKFS_FAT32_FMT_H

/*
 * fat32_fmt.h — mkfs.fat-style FAT32 formatter (pure, portable C).
 *
 * Writes a standard FAT32 volume compatible with the CactOS FAT32 module
 * (FAT32-for-Cact-x86_32): BPB at sector 0, two FATs, FSInfo + backup
 * boot sectors.  Validated by host fsck.fat/mtools.
 */

#include <stdint.h>
#include <stddef.h>

typedef struct f32_opts {
    char     label[12];      /* 11-char uppercase volume label, NUL allowed */
    uint32_t serial;         /* volume id (0 = derive)                     */
} f32_opts_t;

typedef struct f32_disk {
    void *ud;
    int (*read_sec)(void *ud, uint64_t sec, uint8_t out[512]);
    int (*write_sec)(void *ud, uint64_t sec, const uint8_t in[512]);
} f32_disk_t;

/* Format @sectors (512B) of @d as FAT32.  0 ok, -1 fail (err filled). */
int f32_format(f32_disk_t *d, uint64_t sectors, const f32_opts_t *o,
               char *err, size_t err_len);

/* Derived geometry, useful for printing. */
typedef struct f32_geom {
    uint32_t sectors_per_cluster;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t data_start;
    uint32_t cluster_count;
} f32_geom_t;

/* Compute geometry only (no writes). Returns 0 ok. */
int f32_geometry(uint64_t sectors, f32_geom_t *g);

#endif
