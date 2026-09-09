/*
 * ex_df.c — df: свободное место на смонтированных ext4.
 *
 *   df                     — показать всё из /proc/mounts
 *   df PATH                — показать одно устройство/точку монтирования
 *
 * Список монтирований берётся из /proc/mounts (формат Linux: dev target
 * fstype ...), а статистика — напрямую из суперблока ext4 первичного
 * суперблока (смещение 1024) устройства /dev/<name>: каждая запись счётчика
 * блоков s_blocks_count_lo / s_free_blocks_count_lo пересчитывается в байты
 * через s_log_block_size.  Это тот же путь, которым пользуется mkfs.ext4,
 * поэтому работает на блочных узлах CactOS (/dev/sda1 и т.п.).
 *
 * FAT32 и прочие ФС не опрашиваются (пока только ext4).
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <stdint.h>

#define DF_MAX_MNTS  32
#define DF_SB_OFF    1024u
#define E4_MAGIC     0xEF53u

struct df_mnt {
    char dev[128];
    char target[128];
    char fstype[32];
};

static const char *df_mount_files[] = {
    "/proc/mounts",
    "/etc/mounts",
    "/etc/mnts",
    NULL
};

/* Прочитать весь файл (до 64 КБ) в буфер; вернуть длину или -1. */
static long df_read_small(const char *path, char *buf, long cap) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, buf + n, (size_t)(cap - 1 - n));
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        n += (long)r;
    }
    close(fd);
    buf[n] = '\0';
    return n;
}

/* Загрузить таблицу монтирований из первого существующего файла. */
static int df_load_mnts(struct df_mnt *ms, int max) {
    char buf[65536];
    int count = 0;
    for (int fi = 0; df_mount_files[fi]; fi++) {
        long n = df_read_small(df_mount_files[fi], buf, (long)sizeof(buf));
        if (n < 0) continue;
        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        while (line && count < max) {
            char *lsp = NULL;
            char *d = strtok_r(line, " \t", &lsp);
            char *t = strtok_r(NULL, " \t", &lsp);
            char *f = strtok_r(NULL, " \t", &lsp);
            if (d && d[0] != '#') {
                struct df_mnt *m = &ms[count];
                memset(m, 0, sizeof(*m));
                strncpy(m->dev, d, sizeof(m->dev) - 1);
                if (t) strncpy(m->target, t, sizeof(m->target) - 1);
                if (f) strncpy(m->fstype, f, sizeof(m->fstype) - 1);
                count++;
            }
            line = strtok_r(NULL, "\n", &save);
        }
        if (count > 0) return count;   /* первый файл с записями */
    }
    return count;
}

/* /dev/sda1 или sda1 -> путь узла для открытия. */
static void df_dev_path(const char *dev, char *out, size_t n) {
    if (dev[0] == '/') {
        strncpy(out, dev, n - 1);
        out[n - 1] = '\0';
    } else {
        snprintf(out, n, "/dev/%s", dev);
    }
}

/* Прочитать ext4-суперблок и вычислить total/used/free в байтах. */
static int df_ext4_query(const char *devpath,
                         unsigned long long *total,
                         unsigned long long *used,
                         unsigned long long *free_bytes) {
    int fd = open(devpath, O_RDONLY, 0);
    if (fd < 0) return -1;

    unsigned char sb[DF_SB_OFF + 256];
    ssize_t r = pread(fd, sb, sizeof(sb), (off_t)DF_SB_OFF);
    close(fd);
    if (r < (ssize_t)256) return -1;

    /* magic 0xEF53 at superblock+0x38 */
    if (sb[0x38] != (E4_MAGIC & 0xFF) || sb[0x39] != ((E4_MAGIC >> 8) & 0xFF))
        return -2;

    uint32_t blocks_lo = (uint32_t)sb[0x04] | ((uint32_t)sb[0x05] << 8) |
                         ((uint32_t)sb[0x06] << 16) | ((uint32_t)sb[0x07] << 24);
    uint32_t free_lo   = (uint32_t)sb[0x0C] | ((uint32_t)sb[0x0D] << 8) |
                         ((uint32_t)sb[0x0E] << 16) | ((uint32_t)sb[0x0F] << 24);
    uint32_t log_bs    = (uint32_t)sb[0x18] | ((uint32_t)sb[0x19] << 8) |
                         ((uint32_t)sb[0x1A] << 16) | ((uint32_t)sb[0x1B] << 24);

    unsigned long long bs = 1024ULL << log_bs;
    if (!bs) bs = 1024;
    unsigned long long total_b = (unsigned long long)blocks_lo * bs;
    unsigned long long free_b  = (unsigned long long)free_lo   * bs;

    *total      = total_b;
    *free_bytes = free_b;
    *used       = total_b > free_b ? total_b - free_b : 0;
    return 0;
}

static void df_fmt_size(unsigned long long v, char *out, size_t n) {
    static const char *u[] = { "K", "M", "G", "T" };
    int ui = -1;
    unsigned long long x = v;
    while (x >= 1024ULL && ui < 3) { x /= 1024ULL; ui++; }
    if (ui < 0) {
        snprintf(out, n, "%lluB", v);
    } else {
        snprintf(out, n, "%llu%s", x, u[ui]);
    }
}

static void df_print_row(const char *devdisp, const char *mounted_on,
                         unsigned long long total,
                         unsigned long long used,
                         unsigned long long free_bytes) {
    char st[32], su[32], sa[32];
    df_fmt_size(total, st, sizeof(st));
    df_fmt_size(used, su, sizeof(su));
    df_fmt_size(free_bytes, sa, sizeof(sa));

    int pct = 0;
    if (total > 0)
        pct = (int)((used * 100ULL) / total);

    char buf[512];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%-14s ", devdisp);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%5s ", st);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%5s ", su);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%5s ", sa);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%3d%% ", pct);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s\n", mounted_on);
    write(STDOUT_FILENO, buf, (size_t)n);
}

static int df_show_device(const char *devarg, const char *mounted_on) {
    char devpath[160];
    df_dev_path(devarg, devpath, sizeof(devpath));

    unsigned long long total = 0, used = 0, free_b = 0;
    int rc = df_ext4_query(devpath, &total, &used, &free_b);
    if (rc != 0) {
        fprintf(stderr, "df: %s: %s\n", devpath,
                rc == -2 ? "not an ext4 superblock" : "cannot read device");
        return 1;
    }
    if (!mounted_on[0]) mounted_on = devpath;
    df_print_row(devpath, mounted_on, total, used, free_b);
    return 0;
}

int cact_ub_df(char **argv, int argc) {
    int human = 1;   /* единицы K/M/G включены по умолчанию */
    (void)human;
    int args_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--human-readable") == 0) {
            args_start = i + 1;
            continue;
        }
        if (strcmp(argv[i], "-k") == 0) {
            args_start = i + 1;
            continue;
        }
        if (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--si") == 0) {
            args_start = i + 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "df: unknown option %s\n", argv[i]);
            return 1;
        }
        break;
    }

    printf("%-14s %5s %5s %5s %4s  %s\n",
           "Filesystem", "Size", "Used", "Avail", "Use%", "Mounted on");

    if (args_start >= argc) {
        struct df_mnt mnts[DF_MAX_MNTS];
        int nm = df_load_mnts(mnts, DF_MAX_MNTS);
        if (nm == 0) {
            fprintf(stderr,
                    "df: no mount table (/proc/mounts absent) — use `df /dev/<part>`\n");
            return 1;
        }
        int ok = 0;
        for (int i = 0; i < nm; i++) {
            if (mnts[i].fstype[0] && strcmp(mnts[i].fstype, "ext4") != 0)
                continue;   /* пока умеем только ext4 */
            char devpath[160];
            df_dev_path(mnts[i].dev, devpath, sizeof(devpath));
            unsigned long long total = 0, used = 0, free_b = 0;
            int rc = df_ext4_query(devpath, &total, &used, &free_b);
            if (rc != 0) {
                fprintf(stderr, "df: %s: %s\n", devpath,
                        rc == -2 ? "not an ext4 superblock" : "cannot read device");
                continue;
            }
            const char *target = mnts[i].target[0] ? mnts[i].target : devpath;
            df_print_row(devpath, target, total, used, free_b);
            ok = 1;
        }
        return ok ? 0 : 1;
    }

    int rc_all = 0;
    for (int i = args_start; i < argc; i++) {
        const char *arg = argv[i];

        struct stat st;
        int is_dir = (stat(arg, &st) == 0) && S_ISDIR(st.st_mode);

        if (is_dir) {
            /* точка монтирования: ищем в таблице точное совпадение target */
            struct df_mnt mnts[DF_MAX_MNTS];
            int nm = df_load_mnts(mnts, DF_MAX_MNTS);
            int found = 0;
            for (int j = 0; j < nm; j++) {
                if (strcmp(mnts[j].target, arg) == 0) {
                    char devpath[160];
                    df_dev_path(mnts[j].dev, devpath, sizeof(devpath));
                    unsigned long long total = 0, used = 0, free_b = 0;
                    int rc = df_ext4_query(devpath, &total, &used, &free_b);
                    if (rc != 0) {
                        fprintf(stderr, "df: %s: %s\n", devpath,
                                rc == -2 ? "not an ext4 superblock"
                                         : "cannot read device");
                        rc_all = 1;
                    } else {
                        df_print_row(devpath, arg, total, used, free_b);
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "df: %s: not found in mount table\n", arg);
                rc_all = 1;
            }
            continue;
        }

        if (df_show_device(arg, "") != 0)
            rc_all = 1;
    }
    return rc_all;
}
