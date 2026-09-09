/*
 * fdisk — MBR/GPT partition-table editor for CactOS (parted-analog).
 *
 * Reads/writes the first sectors of a whole block device or a raw disk
 * image.  After a successful write on a CactOS block node the kernel
 * partition layer is asked to re-scan the disk (/dev/sys ioctl), which
 * re-exposes /dev/<disk>N partitions without a reboot.
 *
 * Works on three device kinds:
 *   /dev/sda          -> CactOS whole-disk node (opened as /dev/sda/data)
 *   /dev/sda/data     -> raw CactOS node passed verbatim
 *   path/to/img       -> plain file (also handy for host-side testing)
 *
 * Command line:
 *   fdisk <dev>                 interactive
 *   fdisk <dev> <verb> [args]   one-shot (p|g|o|n|d|t|b|l|w)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef CACTOS_TARGET
#include <stat.h>
#include <ioctl_abi.h>
#else
#include <sys/stat.h>
#include <errno.h>
#endif

#include "ptab.h"

#define SECT 512
#define MAX_LBA 4194303u       /* (2^31-1)/512 — CactOS byte-offset limit */

static int      g_block;        /* 1 when writing a real CactOS node */
static char     g_devname[32];  /* kernel name, e.g. "sda"           */
static int      g_dirty;

/* ---- small helpers -------------------------------------------------------- */

static void usage(void) {
    printf(
        "usage: fdisk <device|image> [verb] [args]\n"
        "\n"
        "  device  block node (e.g. /dev/sda) or a raw disk image file\n"
        "\n"
        "verbs (one-shot; without a verb fdisk enters interactive mode):\n"
        "  p           print the partition table\n"
        "  o           create a new empty MBR (dos) label\n"
        "  g           create a new empty GPT label\n"
        "  n first end add a partition (numbers in sectors, '+' means size:\n"
        "              end='+100M', first='default' or a sector)\n"
        "  d <num>     delete partition number <num>\n"
        "  t <num> <t> change type: MBR '83'/'82'/'ef'/..., GPT linux/swap/efi/msdata\n"
        "  b <num>     toggle the MBR bootable flag\n"
        "  l           list known types\n"
        "  w           write the table and (on CactOS) rescan the disk\n");
}

static uint64_t parse_num(const char *s, int *ok) {
    uint64_t v = 0;
    *ok = 1;
    if (!s || !*s) { *ok = 0; return 0; }
    const char *p = s;
    if (*p == '+') p++;
    if (!*p) { *ok = 0; return 0; }
    while (*p) {
        if (*p < '0' || *p > '9') { *ok = 0; return 0; }
        if (v > (uint64_t)-1 / 10) { *ok = 0; return 0; }
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    return v;
}

static uint64_t human_to_sectors(const char *s) {
    /* accepts plain sector numbers or 'K'/'M'/'G' (powers of 1024 bytes) */
    size_t l = strlen(s);
    uint64_t mult = 1;
    char tmp[32];
    strncpy(tmp, s, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    if (l) {
        char u = s[l - 1];
        if (u == 'K' || u == 'k') mult = 1024 / SECT;
        else if (u == 'M' || u == 'm') mult = (1024 * 1024) / SECT;
        else if (u == 'G' || u == 'g') mult = (1024 * 1024 * 1024) / SECT;
        if (mult != 1)
            tmp[l - 1] = '\0';
    }
    int ok = 0;
    uint64_t v = parse_num(tmp, &ok);
    if (!ok || v == (uint64_t)-1)
        return (uint64_t)-1;
    return v * mult;
}

static const char *fmt_size(uint64_t bytes, char *out, size_t n) {
    if (bytes >= (1024ull * 1024 * 1024))
        snprintf(out, n, "%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes >= (1024ull * 1024))
        snprintf(out, n, "%llu MiB", (unsigned long long)(bytes >> 20));
    else if (bytes >= 1024)
        snprintf(out, n, "%llu KiB", (unsigned long long)(bytes >> 10));
    else
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
    return out;
}

/* ---- device I/O ------------------------------------------------------------ */

static int g_fd = -1;

static int rd512(uint64_t lba, uint8_t out[512]) {
    if (lba > MAX_LBA) return -1;
    ssize_t n = pread(g_fd, out, 512, (off_t)(lba * 512));
    return n == 512 ? 0 : -1;
}

static int wr512(uint64_t lba, const uint8_t in[512]) {
    if (lba > MAX_LBA) return -1;
    ssize_t n = pwrite(g_fd, in, 512, (off_t)(lba * 512));
    return n == 512 ? 0 : -1;
}

static int rd_multi(uint64_t lba, uint8_t *buf, int sectors) {
    for (int i = 0; i < sectors; i++)
        if (rd512(lba + (uint64_t)i, buf + (size_t)i * 512) != 0)
            return -1;
    return 0;
}

static int wr_multi(uint64_t lba, const uint8_t *buf, int sectors) {
    for (int i = 0; i < sectors; i++)
        if (wr512(lba + (uint64_t)i, buf + (size_t)i * 512) != 0)
            return -1;
    return 0;
}

/* Probe the last readable sector of a whole-disk node (no size ioctl). */
static uint64_t probe_capacity(void) {
    uint8_t tmp[512];
    if (rd512(0, tmp) != 0)
        return 0;

    uint64_t lo = 0, hi = 1;
    while (hi <= MAX_LBA && rd512(hi, tmp) == 0) {
        lo = hi;
        if (hi > MAX_LBA / 2) { hi = MAX_LBA; break; }
        hi *= 2;
    }
    if (hi <= MAX_LBA && rd512(hi, tmp) != 0) {
        while (hi - lo > 1) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (rd512(mid, tmp) == 0) lo = mid;
            else hi = mid;
        }
    }
    return lo + 1;   /* sectors */
}

static int try_stat(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

static int path_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* Resolve the argument to an open fd. Returns sector capacity or 0. */
static uint64_t open_device(const char *arg) {
    char data_path[256];

    g_block = 0;
    g_devname[0] = '\0';

    if (arg[0] && strlen(arg) + 6 < sizeof(data_path)) {
        snprintf(data_path, sizeof(data_path), "%s/data", arg);
        if (path_is_dir(arg) && try_stat(data_path)) {
            g_block = 1;
            const char *slash = strrchr(arg, '/');
            const char *base  = slash ? slash + 1 : arg;
            strncpy(g_devname, base, sizeof(g_devname) - 1);
            g_devname[sizeof(g_devname) - 1] = '\0';
            g_fd = open(data_path, O_RDWR);
            if (g_fd < 0) { printf("fdisk: cannot open %s\n", data_path); return 0; }
            return probe_capacity();
        }
    }

    g_fd = open(arg, O_RDWR);
    if (g_fd < 0) { printf("fdisk: cannot open %s\n", arg); return 0; }

    struct stat st;
    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size <= 0) { printf("fdisk: image is empty\n"); close(g_fd); return 0; }
        return (uint64_t)(st.st_size / 512);
    }
    /* raw node passed directly (e.g. /dev/sda/data) */
    if (arg[0] && strstr(arg, "/dev/")) {
        const char *slash = strrchr(arg, '/');
        const char *base  = slash ? slash + 1 : arg;
        size_t name_len   = strlen(base);
        if (name_len > 5 && strcmp(arg + strlen(arg) - 5, "/data") == 0)
            name_len -= 5;
        if (name_len >= sizeof(g_devname))
            name_len = sizeof(g_devname) - 1;
        memcpy(g_devname, base, name_len);
        g_devname[name_len] = '\0';
        g_block = 1;
    }
    return probe_capacity();
}

static void rescan_kernel(void) {
#ifdef CACTOS_TARGET
    if (!g_block) return;
    int sfd = open("/dev/sys", O_RDWR);
    if (sfd < 0) { printf("fdisk: no /dev/sys (are you root?)\n"); return; }
    int n = ioctl(sfd, CACT_SYSCTL_BLKDEV_RESCAN, g_devname);
    close(sfd);
    if (n >= 0)
        printf("kernel: re-scanned %s, %d partition(s) exposed\n", g_devname, n);
    else
        printf("kernel: rescan failed (mounted partitions? not root?)\n");
#else
    (void)0;
#endif
}

/* ---- table I/O -------------------------------------------------------------- */

static int load_table(ptab_t *t) {
    uint8_t lba0[512], lba1[512];
    ptab_init(t, PTAB_NONE, 0);

    uint64_t cap = probe_capacity();
    t->disk_sectors = cap;

    if (rd512(0, lba0) != 0) return -1;
    if (rd512(1, lba1) == 0 && memcmp(lba1, "EFI PART", 8) == 0) {
        uint8_t entries[32 * 512];
        if (rd_multi(2, entries, 32) != 0) return -1;
        ptab_load_gpt(t, lba1, entries, PTAB_GPT_ENTRIES, PTAB_GPT_ESIZE);
        t->disk_sectors = cap;
        return 0;
    }
    if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        ptab_load_mbr(t, lba0);
        t->disk_sectors = cap;
        if (t->label == PTAB_MBR && t->nparts == 0)
            t->label = PTAB_MBR;   /* empty but present */
        return 0;
    }
    return 0;   /* no label yet */
}

static int write_table(const ptab_t *t) {
    uint8_t lba0[512];

    if (t->label == PTAB_MBR) {
        ptab_build_mbr((ptab_t *)t, lba0);
        if (wr512(0, lba0) != 0) return -1;
        return 0;
    }

    if (t->label == PTAB_GPT) {
        uint64_t ns = t->disk_sectors;
        if (ns < 34) { printf("fdisk: disk too small for GPT\n"); return -1; }
        uint8_t buf[(1 + PTAB_GPT_ENTRY_SECTORS) * 512];
        uint8_t bak_hdr[512];
        uint8_t bak_ents[PTAB_GPT_ENTRY_SECTORS * 512];

        ptab_build_gpt((ptab_t *)t, lba0, buf, bak_hdr, bak_ents);
        if (wr512(0, lba0) != 0) return -1;
        /* header at LBA1, entries 2..33; backups at the end */
        if (wr_multi(1, buf, 1 + PTAB_GPT_ENTRY_SECTORS) != 0) return -1;
        if (wr_multi(ns - 1 - PTAB_GPT_ENTRY_SECTORS, bak_ents,
                     PTAB_GPT_ENTRY_SECTORS) != 0) return -1;
        if (wr512(ns - 1, bak_hdr) != 0) return -1;
        return 0;
    }

    printf("fdisk: no label selected (create one with 'o' or 'g')\n");
    return -1;
}

/* ---- printing ---------------------------------------------------------------- */

static void print_table(const ptab_t *t) {
    char size_buf[32];
    fmt_size(t->disk_sectors * 512, size_buf, sizeof(size_buf));

    const char *lbl = t->label == PTAB_MBR ? "MBR" :
                      t->label == PTAB_GPT ? "GPT" : "none";
    printf("Disk: %u sectors, %s, label: %s\n",
           (unsigned)(t->disk_sectors > 0xFFFFFFFFull ? 0xFFFFFFFFull
                                                      : t->disk_sectors),
           size_buf, lbl);
    if (!t->disk_sectors) return;

    printf("Number  Start        End          Sectors      Size   Type\n");
    for (int i = 0; i < t->nparts; i++) {
        const ptab_part_t *p = &t->parts[i];
        if (!p->used) continue;
        uint64_t start = p->first_lba;
        uint64_t end   = p->first_lba + p->len - 1;
        char s1[24], s2[24];
        snprintf(s1, sizeof(s1), "%llu", (unsigned long long)start);
        snprintf(s2, sizeof(s2), "%llu", (unsigned long long)end);

        const char *type;
        char guid_buf[37], name_buf[64];
        if (t->label == PTAB_GPT) {
            type = ptab_gpt_type_name(p->type_guid);
            /* show ASCII part of the GPT name if any */
            name_buf[0] = '\0';
            for (int c = 0; c < PTAB_NAME_MAX; c++) {
                uint16_t ch = p->name[c];
                if (ch == 0) break;
                if (ch < 32 || ch > 126) continue;
                size_t l = strlen(name_buf);
                if (l + 1 < sizeof(name_buf)) { name_buf[l] = (char)ch; name_buf[l + 1] = 0; }
            }
        } else {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "0x%02X", p->mbr_type);
            type = ptab_mbr_type_name(p->mbr_type);
            snprintf(name_buf, sizeof(name_buf), "%s%s", tmp,
                     p->boot ? " *" : "");
            (void)guid_buf;
        }
        (void)type; (void)guid_buf;

        char size_buf2[32];
        fmt_size(p->len * 512, size_buf2, sizeof(size_buf2));
        printf("%-6d  %-11s %-11s %-10llu %-6s %s%s%s\n",
               i + 1, s1, s2,
               (unsigned long long)p->len, size_buf2,
               type, name_buf[0] ? " " : "", name_buf);
    }
}

static void print_types(void) {
    printf("MBR (hex): 07 NTFS/exFAT  0B FAT32  0C FAT32(LBA)  82 swap  "
           "83 Linux  EF EFI-System\n");
    printf("GPT (alias): linux  swap  efi/esp  msdata\n");
}

/* ---- interactive -------------------------------------------------------------- */

static int read_line(char *buf, int size) {
    int n = 0;
    while (n < size - 1) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* last sector for a new partition; returns 0 if invalid */
static int parse_last(const char *tok, uint64_t first, uint64_t cap,
                      uint64_t *out_end) {
    if (!tok || !*tok) return 0;
    if (tok[0] == '+') {
        char tmp[40];
        strncpy(tmp, tok + 1, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        uint64_t sz = human_to_sectors(tmp);
        if (sz == (uint64_t)-1 || sz == 0) return 0;
        *out_end = first + sz - 1;
        if (*out_end < first || *out_end >= cap) return 0;
        return 1;
    }
    int ok = 0;
    uint64_t v = human_to_sectors(tok);
    if (v == (uint64_t)-1) return 0;
    if (v <= first) return 0;
    if (v >= cap) return 0;
    *out_end = v;
    (void)ok;
    return 1;
}

static int do_new(ptab_t *t, const char *a_first, const char *a_end) {
    uint64_t first, last, len;

    if (t->label == PTAB_NONE) {
        printf("fdisk: no label yet (create one with 'o' or 'g')\n");
        return -1;
    }

    if (!a_first || strcmp(a_first, "default") == 0 || strcmp(a_first, "d") == 0) {
        first = ptab_first_free(t, 2048);
    } else {
        first = human_to_sectors(a_first);
        if (first == (uint64_t)-1) { printf("fdisk: bad start sector\n"); return -1; }
        uint64_t align = 2048;
        if (t->disk_sectors && first + align > t->disk_sectors) align = 1;
        if (first % align) { printf("fdisk: start not %llu-aligned\n",
                                    (unsigned long long)align); return -1; }
    }

    if (!a_end) {
        printf("fdisk: no end sector given\n");
        return -1;
    }
    if (!parse_last(a_end, first, t->disk_sectors, &last)) {
        printf("fdisk: bad end sector (want >%llu, <%llu)\n",
               (unsigned long long)first,
               (unsigned long long)t->disk_sectors);
        return -1;
    }

    /* do not let a new partition overlap existing ones */
    for (int i = 0; i < t->nparts; i++) {
        uint64_t s = t->parts[i].first_lba;
        uint64_t e = s + t->parts[i].len - 1;
        if (!(last < s || first > e)) {
            printf("fdisk: overlaps partition %d (%llu-%llu)\n", i + 1,
                   (unsigned long long)s, (unsigned long long)e);
            return -1;
        }
    }

    len = last - first + 1;
    int n = ptab_add(t, first, len);
    if (n < 0) {
        printf("fdisk: no free partition slot\n");
        return -1;
    }
    g_dirty = 1;
    printf("Partition %d: %llu-%llu (%llu sectors)\n", n,
           (unsigned long long)first, (unsigned long long)last,
           (unsigned long long)len);
    return 0;
}

static int run_verb(ptab_t *t, const char *verb,
                    const char *a1, const char *a2) {
    if (!strcmp(verb, "p") || !strcmp(verb, "print")) {
        print_table(t);
    } else if (!strcmp(verb, "l") || !strcmp(verb, "list")) {
        print_types();
    } else if (!strcmp(verb, "o") || !strcmp(verb, "label-mbr")) {
        ptab_init(t, PTAB_MBR, t->disk_sectors);
        g_dirty = 1;
        printf("Created a new empty MBR (dos) label.\n");
    } else if (!strcmp(verb, "g") || !strcmp(verb, "label-gpt")) {
        ptab_init(t, PTAB_GPT, t->disk_sectors);
        g_dirty = 1;
        printf("Created a new empty GPT label.\n");
    } else if (!strcmp(verb, "n") || !strcmp(verb, "new")) {
        return do_new(t, a1, a2);
    } else if (!strcmp(verb, "d") || !strcmp(verb, "del")) {
        int ok = 0;
        uint64_t n = a1 ? parse_num(a1, &ok) : 0;
        if (!ok || !n) { printf("fdisk: d <num>\n"); return -1; }
        if (ptab_del(t, (int)n) != 0) { printf("fdisk: no partition %d\n", (int)n); return -1; }
        g_dirty = 1;
    } else if (!strcmp(verb, "t") || !strcmp(verb, "type")) {
        int ok = 0;
        uint64_t n = a1 ? parse_num(a1, &ok) : 0;
        if (!ok || !n || !a2) { printf("fdisk: t <num> <type>\n"); return -1; }
        if ((int)n > t->nparts) { printf("fdisk: no partition %d\n", (int)n); return -1; }
        if (t->label == PTAB_MBR) {
            int code = ptab_parse_mbr_type(a2);
            if (code < 0) { printf("fdisk: unknown MBR type '%s'\n", a2); return -1; }
            ptab_set_mbr_type(t, (int)n, (uint8_t)code);
        } else if (t->label == PTAB_GPT) {
            uint8_t guid[16];
            if (ptab_parse_gpt_type(a2, guid) != 0) {
                printf("fdisk: unknown GPT type '%s'\n", a2);
                return -1;
            }
            ptab_set_gpt_type(t, (int)n, guid);
        } else {
            printf("fdisk: no label\n");
            return -1;
        }
        g_dirty = 1;
    } else if (!strcmp(verb, "b") || !strcmp(verb, "boot")) {
        int ok = 0;
        uint64_t n = a1 ? parse_num(a1, &ok) : 0;
        if (!ok || !n) { printf("fdisk: b <num>\n"); return -1; }
        if ((int)n > t->nparts) { printf("fdisk: no partition %d\n", (int)n); return -1; }
        ptab_part_t *p = &t->parts[n - 1];
        p->boot = p->boot ? 0 : 1;
        g_dirty = 1;
    } else if (!strcmp(verb, "w") || !strcmp(verb, "write")) {
        if (write_table(t) != 0) { printf("fdisk: write failed\n"); return -1; }
        g_dirty = 0;
        printf("Table written.\n");
        rescan_kernel();
    } else if (!strcmp(verb, "q") || !strcmp(verb, "quit")) {
        return -2;
    } else if (!strcmp(verb, "h") || !strcmp(verb, "help") || !strcmp(verb, "?")) {
        usage();
    } else {
        printf("fdisk: unknown verb '%s' (try 'h')\n", verb);
        return -1;
    }
    return 0;
}

static int interactive(ptab_t *t) {
    printf("fdisk: interactive mode, 'h' for help, 'q' to quit\n");
    for (;;) {
        char line[128], *args[4];
        printf("Command (m for help): ");
        fflush(stdout);
        if (read_line(line, sizeof(line)) <= 0)
            return g_dirty ? 1 : 0;

        int nargs = 0;
        char *save = NULL;
        for (char *tok = strtok_r(line, " \t", &save);
             tok && nargs < 4; tok = strtok_r(NULL, " \t", &save))
            args[nargs++] = tok;
        if (nargs == 0) continue;

        int r = run_verb(t, args[0], nargs > 1 ? args[1] : NULL,
                         nargs > 2 ? args[2] : NULL);
        if (r == -2) {
            if (g_dirty) printf("Warning: unsaved changes.  Use 'w' to write.\n");
            return g_dirty ? 1 : 0;
        }
        if (r < 0 && r != -2)
            printf("(failed)\n");
    }
}

static int is_verb(const char *s) {
    static const char *verbs[] = {
        "p","print","l","list","o","label-mbr","g","label-gpt",
        "n","new","d","del","t","type","b","boot","w","write",
        "q","quit","h","help","?"
    };
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
        if (!strcmp(s, verbs[i])) return 1;
    return 0;
}

/* One-shot: run every verb from argv[2..] in order. */
static int run_argv(ptab_t *t, int argc, char **argv) {
    const char *cur = NULL;
    const char *a1 = NULL, *a2 = NULL;
    int rc = 0;

    for (int i = 2; i < argc; i++) {
        if (is_verb(argv[i])) {
            if (cur) {
                int r = run_verb(t, cur, a1, a2);
                if (r == -2) return rc;
                if (r < 0) rc = r;
                a1 = a2 = NULL;
            }
            cur = argv[i];
            continue;
        }
        if (!cur) { printf("fdisk: stray argument '%s'\n", argv[i]); return -1; }
        if (!a1) a1 = argv[i];
        else if (!a2) a2 = argv[i];
        else { printf("fdisk: too many arguments for '%s'\n", cur); return -1; }
    }
    if (cur) {
        int r = run_verb(t, cur, a1, a2);
        if (r == -2) return rc;
        if (r < 0) rc = r;
    }
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    uint64_t cap = open_device(argv[1]);
    if (!cap) return 1;

    ptab_t tbl;
    load_table(&tbl);

    if (argc >= 3) {
        int r = run_argv(&tbl, argc, argv);
        close(g_fd);
        return r < 0 ? 1 : 0;
    }

    int r = interactive(&tbl);
    close(g_fd);
    return r ? 1 : 0;
}
