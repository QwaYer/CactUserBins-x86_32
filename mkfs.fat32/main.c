/*
 * mkfs.fat32 — format a block device / partition / image as FAT32.
 *
 *   mkfs.fat32 [-n label] [-q] <device|file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef CACTOS_TARGET
#include <stat.h>
#else
#include <sys/stat.h>
#endif

#include "fat32_fmt.h"

#define SECT 512
#define MAX_LBA 4194303u

static int dev_fd = -1;
static int quiet;
static char errbuf[128];

static int rd512(uint64_t lba, uint8_t out[512]) {
    if (lba > MAX_LBA) return -1;
    ssize_t n = pread(dev_fd, out, 512, (off_t)(lba * 512));
    return n == 512 ? 0 : -1;
}

static int wr512(uint64_t lba, const uint8_t in[512]) {
    if (lba > MAX_LBA) return -1;
    ssize_t n = pwrite(dev_fd, in, 512, (off_t)(lba * 512));
    return n == 512 ? 0 : -1;
}

static int io_read(void *ud, uint64_t sec, uint8_t out[512]) {
    (void)ud;
    return rd512(sec, out);
}

static int io_write(void *ud, uint64_t sec, const uint8_t in[512]) {
    (void)ud;
    return wr512(sec, in);
}

static uint64_t probe_capacity(void) {
    uint8_t tmp[512];
    if (rd512(0, tmp) != 0) return 0;
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
    return (lo + 1) * 512;
}

static void usage(void) {
    printf(
        "usage: mkfs.fat32 [-n label] [-q] <device|file>\n"
        "  formats the whole device as a FAT32 volume (mkfs.fat analog).\n");
}

int main(int argc, char **argv) {
    const char *dev = NULL;
    char label[12] = "";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            strncpy(label, argv[++i], 11);
            label[11] = '\0';
        } else if (!strcmp(argv[i], "-q")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-")) {
            fprintf(stderr, "mkfs.fat32: unknown option %s\n", argv[i]);
            return 1;
        } else {
            dev = argv[i];
        }
    }
    if (!dev) { usage(); return 1; }

    char node[300];
    node[0] = '\0';
    struct stat st;
    if (stat(dev, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(node, sizeof(node), "%s/data", dev);
        dev = node;
    }

    dev_fd = open(dev, O_RDWR);
    if (dev_fd < 0) {
        fprintf(stderr, "mkfs.fat32: cannot open %s\n", dev);
        return 1;
    }

    uint64_t bytes;
    if (stat(dev, &st) == 0 && S_ISREG(st.st_mode))
        bytes = (uint64_t)st.st_size;
    else
        bytes = probe_capacity();
    if (bytes < 512) {
        fprintf(stderr, "mkfs.fat32: cannot determine device size\n");
        close(dev_fd);
        return 1;
    }

    f32_opts_t o;
    memset(&o, 0, sizeof(o));
    strncpy(o.label, label, sizeof(o.label) - 1);
    o.label[sizeof(o.label) - 1] = '\0';
    o.serial = (uint32_t)(bytes ^ 0xF4700000u);

    f32_disk_t dk;
    dk.ud = NULL;
    dk.read_sec = io_read;
    dk.write_sec = io_write;

    if (!quiet)
        printf("mkfs.fat32: %llu bytes (%llu sectors)\n",
               (unsigned long long)bytes,
               (unsigned long long)(bytes / 512));

    int rc = f32_format(&dk, bytes / 512, &o, errbuf, sizeof(errbuf));
    if (rc != 0) {
        fprintf(stderr, "mkfs.fat32: %s\n", errbuf[0] ? errbuf : "format failed");
        close(dev_fd);
        return 1;
    }
    if (!quiet)
        printf("mkfs.fat32: done\n");
    close(dev_fd);
    return 0;
}
