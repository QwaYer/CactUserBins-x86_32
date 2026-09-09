#ifndef CACT_FDISK_PTAB_H
#define CACT_FDISK_PTAB_H

/*
 * ptab.h — MBR/GPT partition table read/build (pure, portable C).
 *
 * Only <stdint.h>/<string.h> is used so the file can be compiled both for
 * CactOS userland and for host-side tests. All on-disk values are
 * little-endian; the host and the target are both x86.
 */

#include <stdint.h>

#define PTAB_NONE      0
#define PTAB_MBR       1
#define PTAB_GPT       2

#define PTAB_MBR_MAX   4
#define PTAB_GPT_MAX   128
#define PTAB_NAME_MAX  36          /* GPT UTF-16 name cells */
#define PTAB_GUID_LEN  16

typedef struct {
    uint8_t  used;
    uint8_t  boot;                  /* MBR active flag 0x80 */
    uint8_t  mbr_type;              /* MBR partition type byte   */
    uint64_t first_lba;             /* first sector (LBA)        */
    uint64_t len;                   /* length in sectors         */
    uint8_t  type_guid[PTAB_GUID_LEN]; /* GPT type GUID          */
    uint8_t  uniq_guid[PTAB_GUID_LEN]; /* GPT unique GUID        */
    uint16_t name[PTAB_NAME_MAX];   /* GPT partition name (UTF-16LE) */
} ptab_part_t;

typedef struct {
    int       label;                /* PTAB_NONE / PTAB_MBR / PTAB_GPT */
    uint64_t  disk_sectors;
    int       nparts;               /* number of used entries          */
    ptab_part_t parts[PTAB_GPT_MAX];
} ptab_t;

/* Reset an empty table with the given label and disk geometry. */
void ptab_init(ptab_t *t, int label, uint64_t disk_sectors);

/* Parse an existing on-disk label into @t.
 * lba0 = first sector (MBR), lba1.. = GPT header sector.
 * ents/esize/num describe the GPT entry array (caller reads them). */
int ptab_load_mbr(ptab_t *t, const uint8_t lba0[512]);
int ptab_load_gpt(ptab_t *t, const uint8_t hdr[512],
                  const uint8_t *ents, uint32_t num, uint32_t esize);

/* Serialise the table back to disk images.
 *   mbr:  fills lba0.
 *   gpt:  fills lba0 (protective MBR), then *buf* with sectors
 *         1..(1 + GPT_ENTRY_SECTORS) = header + entry array, then the
 *         backup header in bak_hdr and backup entries in bak_ents. */
#define PTAB_GPT_ENTRIES    128
#define PTAB_GPT_ESIZE      128
#define PTAB_GPT_ENTRY_SECTORS  ((PTAB_GPT_ENTRIES * PTAB_GPT_ESIZE) / 512)

void ptab_build_mbr(ptab_t *t, uint8_t lba0[512]);
void ptab_build_gpt(ptab_t *t, uint8_t lba0[512],
                    uint8_t buf[(1 + PTAB_GPT_ENTRY_SECTORS) * 512],
                    uint8_t bak_hdr[512],
                    uint8_t bak_ents[PTAB_GPT_ENTRY_SECTORS * 512]);

/* Editing helpers.  Return 0 on success, -1 on error. */
int ptab_add(ptab_t *t, uint64_t first, uint64_t len);
int ptab_del(ptab_t *t, int n);          /* 1-based partition number */
int ptab_set_mbr_type(ptab_t *t, int n, uint8_t type);
int ptab_set_boot(ptab_t *t, int n, int on);
int ptab_set_gpt_type(ptab_t *t, int n, const uint8_t guid[16]);
int ptab_set_gpt_name(ptab_t *t, int n, const char *utf8);

/* Convert an MBR type byte to a GPT type GUID (best effort). */
void ptab_mbr_type_to_guid(uint8_t type, uint8_t out[16]);
/* 0 when @guid is the all-zero GUID (empty entry). */
int  ptab_guid_is_zero(const uint8_t guid[16]);
/* Names for humans; returns static strings. */
const char *ptab_mbr_type_name(uint8_t t);
const char *ptab_gpt_type_name(const uint8_t guid[16]);
/* Parse an MBR type given as hex string; returns -1 if invalid. */
int  ptab_parse_mbr_type(const char *s);
/* Parse a GPT type by alias ("linux","swap","efi","msdata") or
 * canonical GUID string; fills out[16]. Returns 0 on success. */
int  ptab_parse_gpt_type(const char *s, uint8_t out[16]);

/* Render a GUID to "XXXXXXXX-XXXX-..." (not NUL-terminated, 36 chars) */
void ptab_guid_to_str(const uint8_t guid[16], char out[37]);

/* Next free aligned sector (1 MiB alignment) after existing partitions. */
uint64_t ptab_first_free(ptab_t *t, uint64_t align);

#endif
