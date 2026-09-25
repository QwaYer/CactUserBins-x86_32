/*
 * ex_proc.c — process / device / log utilities built on existing kernel nodes.
 *
 *   ps        — /proc/<pid>/info (no process names exist in the ABI)
 *   mount     — /dev/sys CACT_SYSCTL_MOUNT (no /proc/mounts: listing is limited)
 *   umount    — /dev/sys CACT_SYSCTL_UMOUNT
 *   lsmod     — parse /dev/modinfo section A (loadable PCI drivers only)
 *   lspci     — parse /dev/modinfo section B
 *   dmesg     — read the kernel log from /dev/kmsg
 *   sha256sum / sha384sum — /dev/crypto one-shot hashes
 *
 * Everything here uses interfaces the kernel already exposes; no kernel change
 * is involved.  Where an interface is missing (mount table, USB list, process
 * names) the tool says so instead of inventing one.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>

#include <ioctl_abi.h>
#include <nodeio.h>
#include <crypto.h>

/* ---------------------------------------------------------------------- ps */

static const char ps_usage[] =
    "usage: ps\n"
    "List processes from /proc (PID PPID PGID SID UID GID STAT COMMAND).\n";

static const char *ps_state(uint32_t s) {
    switch (s) {
        case 0: return "R";   /* ready */
        case 1: return "R";   /* running */
        case 2: return "S";   /* sleeping */
        case 3: return "Z";   /* zombie */
        case 4: return "D";   /* waiting */
        case 5: return "T";   /* stopped */
        default: return "?";
    }
}

int cact_ub_ps(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, ps_usage, sizeof(ps_usage) - 1);
        return 0;
    }
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "ps: cannot open /proc\n");
        return 1;
    }

    printf("%5s %5s %5s %5s %5s %5s %4s %s\n",
           "PID", "PPID", "PGID", "SID", "UID", "GID", "STAT", "COMMAND");

    struct dirent buf[32];
    int n;
    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int cnt = n / (int)sizeof(struct dirent);
        for (int i = 0; i < cnt; i++) {
            const char *nm = buf[i].d_name;
            if (!nm[0]) continue;
            int numeric = 1;
            for (const char *p = nm; *p; p++)
                if (*p < '0' || *p > '9') { numeric = 0; break; }
            if (!numeric) continue;

            char path[64];
            snprintf(path, sizeof(path), "/proc/%s/info", nm);
            cact_proc_info_t pi;
            if (nio_read_file(path, &pi, sizeof(pi)) != (int)sizeof(pi))
                continue;

            char comm[32];
            snprintf(path, sizeof(path), "/proc/%s/comm", nm);
            int cn = nio_read_file(path, comm, sizeof(comm) - 1);
            if (cn > 0) {
                comm[cn] = '\0';
                while (cn > 0 && (comm[cn - 1] == '\n' || comm[cn - 1] == '\r' ||
                                  comm[cn - 1] == ' '))
                    comm[--cn] = '\0';
            } else {
                snprintf(comm, sizeof(comm), "-");
            }

            printf("%5u %5u %5u %5u %5u %5u %4s %s%s\n",
                   pi.pid, pi.ppid, pi.pgid, pi.sid, pi.uid, pi.gid,
                   ps_state(pi.state), comm, (pi.flags & 1u) ? " (kernel)" : "");
        }
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------- mount */

static const char mount_usage[] =
    "usage: mount [-t TYPE] SRC DIR\n"
    "       mount\n"
    "  -t TYPE  filesystem: ext4, fat32 or auto (default auto for /dev/* SRC)\n"
    "Without arguments print /etc/mounts (the kernel exposes no mount table,\n"
    "so mounts started by the kernel cannot be listed).\n";

/* Read the first readable mount table; returns the byte count or -1. */
static long mount_table(char *buf, long cap) {
    static const char *files[] = { "/proc/mounts", "/etc/mounts", "/etc/mnts" };
    for (int i = 0; i < 3; i++) {
        int fd = open(files[i], O_RDONLY, 0);
        if (fd < 0) continue;
        long n = 0;
        while (n < cap - 1) {
            ssize_t r = read(fd, buf + n, (size_t)(cap - 1 - n));
            if (r < 0) { n = -1; break; }
            if (r == 0) break;
            n += (long)r;
        }
        close(fd);
        if (n >= 0) { buf[n] = '\0'; return n; }
    }
    return -1;
}

int cact_ub_mount(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mount_usage, sizeof(mount_usage) - 1);
        return 0;
    }
    const char *fstype = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == 't') {
            if (a[2]) fstype = a + 2;
            else if (i + 1 < argc) fstype = argv[++i];
            else { fprintf(stderr, "mount: option -t needs a type\n"); return 1; }
        } else {
            fprintf(stderr, "mount: unknown option %s\n", a);
            return 1;
        }
    }

    if (i >= argc) {
        static char buf[8192];
        long n = mount_table(buf, sizeof(buf));
        if (n > 0) { cact_ub_write_all(STDOUT_FILENO, buf, (size_t)n); return 0; }
        fprintf(stderr,
                "mount: no mount table (/proc/mounts is not provided by the kernel)\n");
        return 1;
    }
    if (argc - i != 2) {
        write(STDERR_FILENO, mount_usage, sizeof(mount_usage) - 1);
        return 1;
    }
    const char *src = argv[i];
    const char *dir = argv[i + 1];
    if (!fstype)
        fstype = (strncmp(src, "/dev/", 5) == 0) ? "auto" : "";

    if (mount(src, dir, fstype, 0, NULL) != 0) {
        fprintf(stderr, "mount: %s on %s: %s\n", src, dir, strerror(errno));
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ umount */

static const char umount_usage[] =
    "usage: umount TARGET\n"
    "Unmount a mount point or a device name.\n";

int cact_ub_umount(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, umount_usage, sizeof(umount_usage) - 1);
        return 0;
    }
    if (argc != 2) {
        write(STDERR_FILENO, umount_usage, sizeof(umount_usage) - 1);
        return 1;
    }
    if (umount(argv[1]) != 0) {
        fprintf(stderr, "umount: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- lsmod */

static const char lsmod_usage[] =
    "usage: lsmod\n"
    "List loaded modules from /proc/modules (filesystem modules and resident\n"
    "PCI kernel modules).\n";

static const char *after_colon(const char *line) {
    const char *c = strchr(line, ':');
    if (!c) return "";
    c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}

int cact_ub_lsmod(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, lsmod_usage, sizeof(lsmod_usage) - 1);
        return 0;
    }
    /* Preferred source: /proc/modules (Linux shape). */
    size_t plen = 0;
    char *pbuf = cact_ub_read_all("/proc/modules", &plen);
    if (pbuf) {
        int printed = 0;
        const char *q = pbuf;
        while (*q) {
            const char *e = q;
            while (*e && *e != '\n') e++;
            char line[256];
            size_t ll = (size_t)(e - q);
            if (ll > sizeof(line) - 1) ll = sizeof(line) - 1;
            memcpy(line, q, ll);
            line[ll] = '\0';
            char mname[64];
            unsigned msize = 0, mused = 0;
            if (line[0] && sscanf(line, "%63s %u %u", mname, &msize, &mused) >= 1) {
                if (!printed)
                    printf("%-20s %8s %6s\n", "Module", "Size", "Used");
                printf("%-20s %8u %6u\n", mname, msize, mused);
                printed = 1;
            }
            q = *e ? e + 1 : e;
        }
        free(pbuf);
        if (printed) return 0;
        /* empty /proc/modules: fall back to the /dev/modinfo view below */
    }

    /* Fallback: parse /dev/modinfo section A. */
    size_t len = 0;
    char *buf = cact_ub_read_all("/dev/modinfo", &len);
    if (!buf) {
        fprintf(stderr, "lsmod: cannot read /proc/modules or /dev/modinfo\n");
        return 1;
    }

    struct { char name[64]; char path[192]; } mods[32];
    int nm = 0;
    char name[64] = "", path[192] = "";
    const char *p = buf;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        char line[256];
        size_t ll = (size_t)(e - p);
        if (ll > sizeof(line) - 1) ll = sizeof(line) - 1;
        memcpy(line, p, ll);
        line[ll] = '\0';

        if (strncmp(line, "[drv ", 5) == 0) {
            const char *rb = strchr(line, ']');
            const char *nmstr = rb ? rb + 1 : "";
            while (*nmstr == ' ') nmstr++;
            snprintf(name, sizeof(name), "%s", nmstr);
            path[0] = '\0';
        } else if (strncmp(line, "module_path:", 12) == 0) {
            snprintf(path, sizeof(path), "%s", after_colon(line));
        } else if (strncmp(line, "kmod unload:", 12) == 0) {
            if (strncmp(after_colon(line), "yes", 3) == 0 && name[0] && nm < 32) {
                snprintf(mods[nm].name, sizeof(mods[nm].name), "%s", name);
                snprintf(mods[nm].path, sizeof(mods[nm].path), "%s", path);
                nm++;
            }
        }
        p = *e ? e + 1 : e;
    }
    free(buf);

    if (nm == 0) {
        printf("(no loadable modules)\n");
        return 0;
    }
    printf("%-20s %s\n", "Module", "Path");
    for (int i = 0; i < nm; i++)
        printf("%-20s %s\n", mods[i].name, mods[i].path[0] ? mods[i].path : "-");
    return 0;
}

/* ------------------------------------------------------------------- lspci */

static const char lspci_usage[] =
    "usage: lspci\n"
    "List PCI functions from /dev/modinfo (bus:dev.fn class vendor:device).\n";

int cact_ub_lspci(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, lspci_usage, sizeof(lspci_usage) - 1);
        return 0;
    }
    size_t len = 0;
    char *buf = cact_ub_read_all("/dev/modinfo", &len);
    if (!buf) {
        fprintf(stderr, "lspci: cannot read /dev/modinfo\n");
        return 1;
    }

    unsigned bus = 0, dev = 0, fn = 0;
    unsigned vendor = 0, device = 0, cls = 0, rev = 0, irq = 0;
    int have = 0;
    const char *p = buf;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        char line[256];
        size_t ll = (size_t)(e - p);
        if (ll > sizeof(line) - 1) ll = sizeof(line) - 1;
        memcpy(line, p, ll);
        line[ll] = '\0';

        if (strncmp(line, "[pci ", 5) == 0) {
            if (have)
                printf("%02x:%02x.%x Class %04x [%04x:%04x] rev %02x irq %u\n",
                       bus, dev, fn, cls, vendor, device, rev, irq);
            unsigned idx = 0;
            sscanf(line, "[pci %u] bus %u dev %u fn %u", &idx, &bus, &dev, &fn);
            vendor = device = cls = rev = irq = 0;
            have = 1;
        } else if (have) {
            const char *v = after_colon(line);
            if      (strncmp(line, "vendor_id:", 10) == 0) vendor = (unsigned)strtoul(v, NULL, 0);
            else if (strncmp(line, "device_id:", 10) == 0) device = (unsigned)strtoul(v, NULL, 0);
            else if (strncmp(line, "class_code:", 11) == 0) cls   = (unsigned)strtoul(v, NULL, 0);
            else if (strncmp(line, "revision:", 9) == 0)   rev   = (unsigned)strtoul(v, NULL, 0);
            else if (strncmp(line, "irq_line:", 9) == 0)   irq   = (unsigned)strtoul(v, NULL, 0);
        }
        p = *e ? e + 1 : e;
    }
    if (have)
        printf("%02x:%02x.%x Class %04x [%04x:%04x] rev %02x irq %u\n",
               bus, dev, fn, cls, vendor, device, rev, irq);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------- lsusb */

static const char lsusb_usage[] =
    "usage: lsusb\n"
    "List USB devices from /proc/usb.\n";

int cact_ub_lsusb(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, lsusb_usage, sizeof(lsusb_usage) - 1);
        return 0;
    }
    size_t len = 0;
    char *buf = cact_ub_read_all("/proc/usb", &len);
    if (!buf) {
        fprintf(stderr, "lsusb: cannot read /proc/usb\n");
        return 1;
    }

    int any = 0;
    const char *p = buf;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        char line[160];
        size_t ll = (size_t)(e - p);
        if (ll > sizeof(line) - 1) ll = sizeof(line) - 1;
        memcpy(line, p, ll);
        line[ll] = '\0';

        unsigned addr = 0, cls = 0, sub = 0, proto = 0, speed = 0, port = 0;
        unsigned vend = 0, prod = 0;
        if (line[0] && sscanf(line, "%u %x %x %x %x %x %u %u",
                              &addr, &vend, &prod, &cls, &sub, &proto,
                              &speed, &port) == 8) {
            printf("Bus 001 Device %03u: ID %04x:%04x\n", addr, vend, prod);
            any = 1;
        }
        p = *e ? e + 1 : e;
    }
    free(buf);
    if (!any) printf("(no USB devices)\n");
    return 0;
}

/* ------------------------------------------------------------------ dmesg */

static const char dmesg_usage[] =
    "usage: dmesg [-r] [-n N]\n"
    "Print the kernel log from /dev/kmsg.\n"
    "  -r raw records (level,seq,usec,flags;message)\n"
    "  -n N last N records\n";

int cact_ub_dmesg(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, dmesg_usage, sizeof(dmesg_usage) - 1);
        return 0;
    }
    int raw = 0;
    long last = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-r") == 0) raw = 1;
        else if (a[0] == '-' && a[1] == 'n') {
            if (a[2]) last = atol(a + 2);
            else if (i + 1 < argc) last = atol(argv[++i]);
            else { fprintf(stderr, "dmesg: -n needs a value\n"); return 1; }
        } else {
            fprintf(stderr, "dmesg: unknown option %s\n", a);
            return 1;
        }
    }

    size_t len = 0;
    char *buf = cact_ub_read_all("/dev/kmsg", &len);
    if (!buf) {
        fprintf(stderr, "dmesg: cannot read /dev/kmsg\n");
        return 1;
    }
    char **lines = NULL;
    size_t n = 0;
    {
        /* split in place, keeping the record text */
        n = 0;
        char **arr = (char **)malloc(256 * sizeof(char *));
        size_t cap = 256, i = 0;
        while (i < len) {
            size_t start = i;
            while (i < len && buf[i] != '\n') i++;
            if (n == cap) {
                cap *= 2;
                char **na = (char **)realloc(arr, cap * sizeof(char *));
                if (!na) { free(arr); arr = NULL; break; }
                arr = na;
            }
            arr[n++] = buf + start;
            if (i < len) buf[i++] = '\0';
        }
        lines = arr;
    }

    size_t start = 0;
    if (last >= 0 && (size_t)last < n) start = n - (size_t)last;

    for (size_t k = start; k < n; k++) {
        const char *s = lines[k];
        if (raw || !*s) {
            printf("%s\n", s);
            continue;
        }
        char *e1 = NULL;
        (void)strtoul(s, &e1, 10);              /* level */
        if (e1 && *e1 == ',') {
            char *e2 = NULL;
            (void)strtoul(e1 + 1, &e2, 10);     /* seq */
            if (e2 && *e2 == ',') {
                char *e3 = NULL;
                unsigned long usec = strtoul(e2 + 1, &e3, 10);
                const char *msg = e3 ? strchr(e3, ';') : NULL;
                printf("[%5lu.%06lu] %s\n", usec / 1000000UL, usec % 1000000UL,
                       msg ? msg + 1 : s);
                continue;
            }
        }
        printf("%s\n", s);
    }
    free(lines);
    free(buf);
    return 0;
}

/* ---------------------------------------------------------------- sha*sum */

static int sha_show(const char *path, int is384) {
    size_t len = 0;
    char *buf = cact_ub_read_all(path, &len);
    if (!buf) {
        fprintf(stderr, "%s: %s: cannot read\n",
                is384 ? "sha384sum" : "sha256sum", path ? path : "-");
        return 1;
    }
    if (len > 1024u * 1024u) {
        fprintf(stderr, "%s: %s: exceeds the 1 MiB /dev/crypto limit\n",
                is384 ? "sha384sum" : "sha256sum", path ? path : "-");
        free(buf);
        return 1;
    }

    uint8_t out[CACT_SHA384_LEN];
    int rc = is384 ? cact_sha384(buf, len, out) : cact_sha256(buf, len, out);
    free(buf);
    if (rc != 0) {
        fprintf(stderr, "%s: hashing failed: %s\n",
                is384 ? "sha384sum" : "sha256sum", strerror(errno));
        return 1;
    }

    static const char hd[] = "0123456789abcdef";
    int nb = is384 ? CACT_SHA384_LEN : CACT_SHA256_LEN;
    char hex[CACT_SHA384_LEN * 2 + 1];
    for (int i = 0; i < nb; i++) {
        hex[2 * i]     = hd[out[i] >> 4];
        hex[2 * i + 1] = hd[out[i] & 0x0F];
    }
    hex[2 * nb] = '\0';
    printf("%s  %s\n", hex, path ? path : "-");
    return 0;
}

static const char sha256_usage[] = "usage: sha256sum [FILE...]\n";

int cact_ub_sha256sum(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sha256_usage, sizeof(sha256_usage) - 1);
        return 0;
    }
    if (argc < 2) return sha_show(NULL, 0);
    int ret = 0;
    for (int i = 1; i < argc; i++)
        if (sha_show(strcmp(argv[i], "-") == 0 ? NULL : argv[i], 0) != 0) ret = 1;
    return ret;
}

static const char sha384_usage[] = "usage: sha384sum [FILE...]\n";

int cact_ub_sha384sum(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sha384_usage, sizeof(sha384_usage) - 1);
        return 0;
    }
    if (argc < 2) return sha_show(NULL, 1);
    int ret = 0;
    for (int i = 1; i < argc; i++)
        if (sha_show(strcmp(argv[i], "-") == 0 ? NULL : argv[i], 1) != 0) ret = 1;
    return ret;
}
