#ifndef CACT_MKFS_EXT4_FMT_H
#define CACT_MKFS_EXT4_FMT_H

/*
 * ext4_fmt.h — minimal mke2fs-style ext4 formatter (pure, portable C).
 *
 * Produces an ext4 filesystem compatible with the CactOS ext4 module
 * (EXT4-for-Cact-x86_32): 4096/2048-byte blocks, 32/64-byte group
 * descriptors, no journal, no 64-bit, no metadata checksums, no flex_bg,
 * extents on directories.  Validated by host e2fsck -fn.
 */

#include <stdint.h>
#include <stddef.h>

#define E4_BLOCK_DEFAULT 4096
#define E4_INODE_DEFAULT 256

typedef struct e4_opts {
    uint32_t block_size;         /* 2048 or 4096 */
    uint32_t inode_size;         /* >= 256 is safest for the Cact module  */
    char     label[17];          /* volume label (NUL-terminated, <=16)   */
    uint32_t now;                /* unix-ish timestamp (0 = 1700000000)   */
    uint32_t uuid_seed;          /* seed for deterministic filesystem UUID */
} e4_opts_t;

/* 512-byte sector I/O callback pair.  Both return 0 on success. */
typedef struct e4_disk {
    void *ud;
    int (*read_sec)(void *ud, uint64_t sec, uint8_t out[512]);
    int (*write_sec)(void *ud, uint64_t sec, const uint8_t in[512]);
} e4_disk_t;

/* Format @bytes bytes of @d as ext4.  Returns 0 on success, -1 on failure
 * (err holds a short message). */
int e4_format(e4_disk_t *d, uint64_t bytes, const e4_opts_t *o, char *err,
              size_t err_len);

#endif
