/*
 * ptab.c — MBR/GPT partition table reader/builder (pure, portable C).
 *
 * Wire formats follow the UEFI / legacy MBR specs used by the kernel's
 * CactKernel partition layer (Cact/drivers/block/blkdev/part.c) and by
 * common partitioning tools.
 */

#include "ptab.h"

#include <string.h>

/* ---- little-endian helpers ------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = rd32(p);
    return v | ((uint64_t)rd32(p + 4) << 32);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}

/* ---- GUID constants (on-disk byte order, i.e. mixed-endian) --------------- */

static const uint8_t GUID_EFI[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
static const uint8_t GUID_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47, 0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4
};
static const uint8_t GUID_SWAP[16] = {
    0x6D,0xFD,0x57,0x06, 0xAB,0xA4, 0x43,0xC4, 0x84,0xE5, 0x09,0x33,0xC8,0x4B,0x4F,0x4F
};
static const uint8_t GUID_MSDATA[16] = {
    0xEB,0x0E,0xC9,0xA2, 0xB9,0xE5, 0x44,0x33, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};

int ptab_guid_is_zero(const uint8_t guid[16]) {
    for (int i = 0; i < 16; i++)
        if (guid[i]) return 0;
    return 1;
}

/* ---- CRC-32 (IEEE 802.3, reflected, used by GPT) -------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

/* ---- table lifecycle -------------------------------------------------------- */

void ptab_init(ptab_t *t, int label, uint64_t disk_sectors) {
    memset(t, 0, sizeof(*t));
    t->label        = label;
    t->disk_sectors = disk_sectors;
    t->nparts       = 0;
}


/* ---- MBR parse ------------------------------------------------------------- */

int ptab_load_mbr(ptab_t *t, const uint8_t lba0[512]) {
    if (lba0[510] != 0x55 || lba0[511] != 0xAA)
        return -1;

    ptab_t tmp;
    ptab_init(&tmp, PTAB_MBR, t->disk_sectors);

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = lba0 + 446 + i * 16;
        uint32_t start = rd32(e + 8);
        uint32_t len   = rd32(e + 12);
        uint8_t  type  = e[4];
        if (type == 0 || start == 0 || len == 0)
            continue;
        /* skip extended containers and protective GPT */
        if (type == 0x05 || type == 0x0F || type == 0x85 || type == 0xEE)
            continue;

        ptab_part_t *p = &tmp.parts[tmp.nparts++];
        p->used      = 1;
        p->boot      = (e[0] & 0x80) ? 1 : 0;
        p->mbr_type  = type;
        p->first_lba = start;
        p->len       = len;
    }

    *t = tmp;
    return 0;
}

/* ---- GPT parse ------------------------------------------------------------- */

int ptab_load_gpt(ptab_t *t, const uint8_t hdr[512],
                  const uint8_t *ents, uint32_t num, uint32_t esize) {
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return -1;

    ptab_t tmp;
    ptab_init(&tmp, PTAB_GPT, t->disk_sectors);
    tmp.disk_sectors = rd64(hdr + 32);   /* from backup_lba + 1 */

    if (!ents || esize < 128)
        esize = 128;
    if (num > 128)
        num = 128;

    for (uint32_t i = 0; i < num; i++) {
        const uint8_t *e = ents + (size_t)i * esize;
        uint64_t first = rd64(e + 32);
        uint64_t last  = rd64(e + 40);
        if (ptab_guid_is_zero(e) || first == 0 || last < first)
            continue;

        ptab_part_t *p = &tmp.parts[tmp.nparts++];
        p->used      = 1;
        p->first_lba = first;
        p->len       = last - first + 1;
        memcpy(p->type_guid, e, 16);
        memcpy(p->uniq_guid, e + 16, 16);
        /* UTF-16LE name (offset 56 in the entry) */
        for (int c = 0; c < PTAB_NAME_MAX; c++)
            p->name[c] = (uint16_t)(e[56 + c * 2] | (e[56 + c * 2 + 1] << 8));
    }

    *t = tmp;
    return 0;
}

/* ---- serialisation: MBR ----------------------------------------------------- */

void ptab_build_mbr(ptab_t *t, uint8_t lba0[512]) {
    memset(lba0, 0, 512);

    int n = t->nparts < PTAB_MBR_MAX ? t->nparts : PTAB_MBR_MAX;
    for (int i = 0; i < n; i++) {
        const ptab_part_t *p = &t->parts[i];
        if (!p->used || p->first_lba == 0 || p->len == 0 ||
            p->first_lba > 0xFFFFFFFFull || p->len > 0xFFFFFFFFull)
            continue;
        uint8_t *e = lba0 + 446 + i * 16;
        e[0] = p->boot ? 0x80 : 0x00;
        e[4] = p->mbr_type;
        wr32(e + 8,  (uint32_t)p->first_lba);
        wr32(e + 12, (uint32_t)p->len);
    }

    lba0[510] = 0x55;
    lba0[511] = 0xAA;
}

/* ---- serialisation: GPT ------------------------------------------------------ */

static void gpt_make_header(uint8_t out[512], uint64_t cur_lba, uint64_t bak_lba,
                            uint64_t first_usable, uint64_t last_usable,
                            const uint8_t disk_guid[16],
                            uint32_t entries_crc) {
    memset(out, 0, 512);
    memcpy(out, "EFI PART", 8);
    wr32(out + 8,  0x00010000);          /* revision 1.0 */
    wr32(out + 12, 92);                  /* header size    */
    /* crc32 at +16, filled later        */
    wr64(out + 24, cur_lba);
    wr64(out + 32, bak_lba);
    wr64(out + 40, first_usable);
    wr64(out + 48, last_usable);
    memcpy(out + 56, disk_guid, 16);
    wr64(out + 72, 2);                    /* entries LBA */
    wr32(out + 80, PTAB_GPT_ENTRIES);
    wr32(out + 84, PTAB_GPT_ESIZE);
    wr32(out + 88, entries_crc);
    wr32(out + 16, crc32_update(0, out, 92));
}

static void gpt_make_entry(uint8_t out[128], const ptab_part_t *p) {
    memset(out, 0, 128);
    memcpy(out, p->type_guid, 16);
    memcpy(out + 16, p->uniq_guid, 16);
    wr64(out + 32, p->first_lba);
    wr64(out + 40, p->first_lba + p->len - 1);   /* last LBA; attrs at 48 */
    for (int c = 0; c < PTAB_NAME_MAX; c++) {    /* name at 56 */
        out[56 + c * 2]     = (uint8_t)p->name[c];
        out[56 + c * 2 + 1] = (uint8_t)(p->name[c] >> 8);
    }
}

void ptab_build_gpt(ptab_t *t, uint8_t lba0[512],
                    uint8_t buf[(1 + PTAB_GPT_ENTRY_SECTORS) * 512],
                    uint8_t bak_hdr[512],
                    uint8_t bak_ents[PTAB_GPT_ENTRY_SECTORS * 512]) {
    uint64_t ns = t->disk_sectors;
    if (ns < 34)
        ns = 34;

    uint8_t disk_guid[16];
    {
        /* deterministic pseudo-GUID from geometry + time-ish seed */
        uint32_t x = (uint32_t)(ns ^ (ns >> 32));
        x ^= 0x9E3779B9u;
        for (int i = 0; i < 16; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            disk_guid[i] = (uint8_t)(x >> (i & 7) * 4);
        }
        disk_guid[7] = (uint8_t)((disk_guid[7] & 0x0F) | 0x40); /* version 4 */
        disk_guid[8] = (uint8_t)((disk_guid[8] & 0x3F) | 0x80);
    }

    /* protective MBR */
    memset(lba0, 0, 512);
    lba0[446 + 4] = 0xEE;
    wr32(lba0 + 446 + 8,  1);
    {
        uint64_t cap = ns - 1;
        if (cap > 0xFFFFFFFFull) cap = 0xFFFFFFFFull;
        wr32(lba0 + 446 + 12, (uint32_t)cap);
    }
    lba0[510] = 0x55;
    lba0[511] = 0xAA;

    /* entry array (sectors 2..33 of buf) */
    uint8_t *entry_arr = buf + 512;
    memset(entry_arr, 0, PTAB_GPT_ENTRY_SECTORS * 512);
    for (int i = 0; i < t->nparts && i < PTAB_GPT_MAX; i++)
        gpt_make_entry(entry_arr + (size_t)i * PTAB_GPT_ESIZE, &t->parts[i]);

    uint32_t entries_crc = crc32_update(0, entry_arr, PTAB_GPT_ENTRY_SECTORS * 512);

    uint64_t last_usable = ns - 1 - 1 - PTAB_GPT_ENTRY_SECTORS;
    gpt_make_header(buf, 1, ns - 1, 34, last_usable, disk_guid,
                    entries_crc);

    /* backup copies at the end of the disk */
    memcpy(bak_ents, entry_arr, PTAB_GPT_ENTRY_SECTORS * 512);
    gpt_make_header(bak_hdr, ns - 1, 1, 34, last_usable, disk_guid,
                    entries_crc);
}

/* ---- editing ------------------------------------------------------------------ */

int ptab_add(ptab_t *t, uint64_t first, uint64_t len) {
    int max = (t->label == PTAB_MBR) ? PTAB_MBR_MAX : PTAB_GPT_MAX;
    if (t->nparts >= max || len == 0 || first == 0)
        return -1;
    if (first + len > t->disk_sectors && t->disk_sectors)
        return -1;

    ptab_part_t *p = &t->parts[t->nparts];
    memset(p, 0, sizeof(*p));
    p->used      = 1;
    p->first_lba = first;
    p->len       = len;
    p->mbr_type  = 0x83;

    if (t->label == PTAB_MBR) {
        memcpy(p->type_guid, GUID_LINUX, 16);
    } else {
        ptab_mbr_type_to_guid(0x83, p->type_guid);
        /* small deterministic unique GUID per partition */
        uint32_t x = (uint32_t)first ^ (uint32_t)(first >> 32);
        for (int i = 0; i < 16; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            p->uniq_guid[i] = (uint8_t)(x ^ (uint32_t)i);
        }
        p->uniq_guid[7] = (uint8_t)((p->uniq_guid[7] & 0x0F) | 0x40);
        p->uniq_guid[8] = (uint8_t)((p->uniq_guid[8] & 0x3F) | 0x80);
    }

    t->nparts++;
    return t->nparts;   /* 1-based number */
}

int ptab_del(ptab_t *t, int n) {
    if (n < 1 || n > t->nparts)
        return -1;
    for (int i = n - 1; i < t->nparts - 1; i++)
        t->parts[i] = t->parts[i + 1];
    memset(&t->parts[t->nparts - 1], 0, sizeof(ptab_part_t));
    t->nparts--;
    return 0;
}

int ptab_set_mbr_type(ptab_t *t, int n, uint8_t type) {
    if (n < 1 || n > t->nparts) return -1;
    t->parts[n - 1].mbr_type = type;
    return 0;
}

int ptab_set_boot(ptab_t *t, int n, int on) {
    if (n < 1 || n > t->nparts) return -1;
    t->parts[n - 1].boot = on ? 1 : 0;
    return 0;
}

int ptab_set_gpt_type(ptab_t *t, int n, const uint8_t guid[16]) {
    if (n < 1 || n > t->nparts) return -1;
    memcpy(t->parts[n - 1].type_guid, guid, 16);
    return 0;
}

int ptab_set_gpt_name(ptab_t *t, int n, const char *utf8) {
    if (n < 1 || n > t->nparts) return -1;
    size_t l = 0;
    while (utf8[l] && l < 255) l++;
    /* naive UTF-8 -> UTF-16LE for ASCII-only caller convenience */
    for (int c = 0; c < PTAB_NAME_MAX; c++)
        t->parts[n - 1].name[c] = 0;
    for (size_t c = 0; c < l && (int)c < PTAB_NAME_MAX; c++)
        t->parts[n - 1].name[c] = (uint16_t)(uint8_t)utf8[c];
    return 0;
}

uint64_t ptab_first_free(ptab_t *t, uint64_t align) {
    if (align == 0) align = 1;
    uint64_t start = (t->label == PTAB_MBR) ? 2048 : 34;
    uint64_t best  = start;

    for (int i = 0; i < t->nparts; i++) {
        uint64_t end = t->parts[i].first_lba + t->parts[i].len;
        if (end > best)
            best = end;
    }
    if (best < start) best = start;
    if (align > 1) {
        uint64_t m = best % align;
        if (m) best += align - m;
    }
    return best;
}

/* ---- type tables ----------------------------------------------------------- */

void ptab_mbr_type_to_guid(uint8_t type, uint8_t out[16]) {
    switch (type) {
    case 0x82: memcpy(out, GUID_SWAP, 16); break;
    case 0xEF: memcpy(out, GUID_EFI, 16); break;
    case 0x07: memcpy(out, GUID_MSDATA, 16); break;
    default:   memcpy(out, GUID_LINUX, 16); break;
    }
}

const char *ptab_mbr_type_name(uint8_t t) {
    switch (t) {
    case 0x00: return "Empty";
    case 0x07: return "NTFS / exFAT";
    case 0x0B: return "W95 FAT32";
    case 0x0C: return "W95 FAT32 (LBA)";
    case 0x0E: return "W95 FAT16 (LBA)";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "EFI GPT protective";
    case 0xEF: return "EFI System (FAT)";
    default:   return "Unknown";
    }
}

const char *ptab_gpt_type_name(const uint8_t guid[16]) {
    if (ptab_guid_is_zero(guid)) return "Unused";
    if (!memcmp(guid, GUID_EFI, 16))     return "EFI System";
    if (!memcmp(guid, GUID_LINUX, 16))   return "Linux filesystem";
    if (!memcmp(guid, GUID_SWAP, 16))    return "Linux swap";
    if (!memcmp(guid, GUID_MSDATA, 16))  return "Microsoft basic data";
    return "Unknown";
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ptab_parse_mbr_type(const char *s) {
    int hi = hexval(s[0]);
    int lo = hexval(s[1]);
    if (hi < 0 || lo < 0)
        return -1;
    return (hi << 4) | lo;
}

int ptab_parse_gpt_type(const char *s, uint8_t out[16]) {
    if (!strcmp(s, "linux") || !strcmp(s, "83")) {
        memcpy(out, GUID_LINUX, 16);
        return 0;
    }
    if (!strcmp(s, "swap") || !strcmp(s, "82")) {
        memcpy(out, GUID_SWAP, 16);
        return 0;
    }
    if (!strcmp(s, "efi") || !strcmp(s, "esp") || !strcmp(s, "EF")) {
        memcpy(out, GUID_EFI, 16);
        return 0;
    }
    if (!strcmp(s, "msdata") || !strcmp(s, "07")) {
        memcpy(out, GUID_MSDATA, 16);
        return 0;
    }
    return -1;
}

void ptab_guid_to_str(const uint8_t guid[16], char out[37]) {
    static const char hx[] = "0123456789ABCDEF";
    int p = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[p++] = '-';
        out[p++] = hx[guid[i] >> 4];
        out[p++] = hx[guid[i] & 0x0F];
    }
    out[p] = '\0';
}
