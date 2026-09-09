/*
 * cact-rootfs — install the root filesystem skeleton onto a target mount
 * point (phase-2 "root install layer").
 *
 * Given a mounted target directory it creates the base tree, deploys boot
 * files (kernel + initramfs/module archive + optional grub.cfg) under
 * boot/, and can copy the running userspace (/bin /sbin /lib) from a source
 * root so the target becomes self-contained.
 *
 *   cact-rootfs [options] TARGET
 *     -s DIR     source root to copy /bin,/sbin,/lib from (default "/")
 *     -b         also copy /bin,/sbin,/lib into TARGET
 *     -k FILE    copy FILE -> TARGET/boot/kernel.bin
 *     -m FILE    copy FILE -> TARGET/boot/cctkfs.img
 *     -g FILE    copy FILE -> TARGET/boot/grub/grub.cfg (default: generated)
 *     -q         quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef CACTOS_TARGET
#include <stat.h>
#include <dirent.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif

#define MODE_DIR  0755
#define MODE_FILE 0644

static int quiet;
static void msg(const char *fmt, ...) {
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

static void fail(const char *s) {
    fprintf(stderr, "cact-rootfs: %s\n", s);
    exit(1);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p */
static void mkdir_p(const char *path) {
    char tmp[512];
    if (strlen(path) >= sizeof(tmp)) fail("path too long");
    strcpy(tmp, path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!is_dir(tmp)) {
                if (mkdir(tmp, MODE_DIR) != 0 && !is_dir(tmp))
                    fail("cannot create directory");
            }
            *p = '/';
        }
    }
    if (!is_dir(tmp)) {
        if (mkdir(tmp, MODE_DIR) != 0 && !is_dir(tmp))
            fail("cannot create directory");
    }
}

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, MODE_FILE);
    if (fd < 0) fail("cannot create file");
    size_t len = strlen(data);
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) { close(fd); fail("write failed"); }
        data += n; len -= (size_t)n;
    }
    close(fd);
    chmod(path, MODE_FILE);
}

static void copy_file(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) fail("source file missing");
    int in = open(src, O_RDONLY);
    if (in < 0) fail("cannot open source");
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, MODE_FILE);
    if (out < 0) { close(in); fail("cannot create target"); }

    char buf[4096];
    uint32_t left = (uint32_t)st.st_size;
    while (left) {
        uint32_t want = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
        ssize_t n = read(in, buf, want);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { close(in); close(out); fail("copy failed"); }
            off += w;
        }
        left -= (uint32_t)n;
    }
    close(in);
    close(out);
    chmod(dst, st.st_mode & 07777);   /* keep exec bits (bin/sbin copies) */
}

/* directory entry names excluding . and .. ; returns count, -1 on error */
static int list_dir(const char *path, char names[][256], int max) {
    int count = 0;
#ifdef CACTOS_TARGET
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct dirent de[8];
    for (;;) {
        int n = getdents(fd, de, sizeof(de));
        if (n <= 0) break;
        int entries = n / (int)sizeof(struct dirent);
        for (int i = 0; i < entries; i++) {
            const char *nm = de[i].d_name;
            if (!nm[0] || !strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            if (count < max) {
                strncpy(names[count], nm, 255);
                names[count][255] = '\0';
                count++;
            }
        }
    }
    close(fd);
#else
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (count < max) {
            strncpy(names[count], nm, 255);
            names[count][255] = '\0';
            count++;
        }
    }
    closedir(d);
#endif
    return count;
}

static void copy_tree(const char *src, const char *dst) {
    if (!is_dir(src)) return;
    mkdir_p(dst);

    char names[64][256];
    int n = list_dir(src, names, 64);
    for (int i = 0; i < n; i++) {
        char sp[512], dp[512];
        snprintf(sp, sizeof(sp), "%s/%s", src, names[i]);
        snprintf(dp, sizeof(dp), "%s/%s", dst, names[i]);
        struct stat st;
        if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode)) {
            copy_tree(sp, dp);
        } else {
            copy_file(sp, dp);
        }
    }
}

static const char *GEN_GRUB =
    "set timeout=5\n"
    "set default=0\n"
    "menuentry \"CactOS\" {\n"
    "    multiboot2 /boot/kernel.bin\n"
    "    module2 /boot/cctkfs.img cctkfs\n"
    "}\n";

int main(int argc, char **argv) {
    const char *target = NULL;
    const char *srcroot = "/";
    const char *kb = NULL, *mb = NULL, *gc = NULL;
    int copy_base = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)      srcroot = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) kb = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mb = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gc = argv[++i];
        else if (!strcmp(argv[i], "-b"))                 copy_base = 1;
        else if (!strcmp(argv[i], "-q"))                 quiet = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: cact-rootfs [-s SRCDIR] [-b] [-k kernel] "
                   "[-m cctkfs] [-g grub.cfg] TARGET\n");
            return 0;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-")) {
            fprintf(stderr, "cact-rootfs: unknown option %s\n", argv[i]);
            return 1;
        } else {
            target = argv[i];
        }
    }
    if (!target) { fprintf(stderr, "cact-rootfs: target directory required\n"); return 1; }
    if (!is_dir(target)) { fprintf(stderr, "cact-rootfs: %s is not a directory\n", target); return 1; }

    char p[512];
    snprintf(p, sizeof(p), "%s/boot/grub", target);   mkdir_p(p);
    snprintf(p, sizeof(p), "%s/etc", target);         mkdir_p(p);
    snprintf(p, sizeof(p), "%s/var/log", target);     mkdir_p(p);
    snprintf(p, sizeof(p), "%s/var/run", target);     mkdir_p(p);
    snprintf(p, sizeof(p), "%s/var/tmp", target);     mkdir_p(p);
    snprintf(p, sizeof(p), "%s/root", target);        mkdir_p(p);
    snprintf(p, sizeof(p), "%s/tmp", target);         mkdir_p(p);
    snprintf(p, sizeof(p), "%s/dev", target);         mkdir_p(p);
    snprintf(p, sizeof(p), "%s/proc", target);        mkdir_p(p);
    if (copy_base) {
        const char *base[] = { "/bin", "/sbin", "/lib", "/usr" };
        for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
            char s[512], d[512];
            snprintf(s, sizeof(s), "%s%s", srcroot, base[i]);
            snprintf(d, sizeof(d), "%s%s", target, base[i]);
            if (is_dir(s)) {
                msg("copying %s -> %s\n", s, d);
                copy_tree(s, d);
            }
        }
    }

    if (kb || mb) {
        snprintf(p, sizeof(p), "%s/boot", target);
        mkdir_p(p);
        if (kb) {
            snprintf(p, sizeof(p), "%s/boot/kernel.bin", target);
            msg("installing %s -> %s\n", kb, p);
            copy_file(kb, p);
        }
        if (mb) {
            snprintf(p, sizeof(p), "%s/boot/cctkfs.img", target);
            msg("installing %s -> %s\n", mb, p);
            copy_file(mb, p);
        }
    }

    /* grub.cfg: user file or a generated Multiboot2 menu */
    {
        snprintf(p, sizeof(p), "%s/boot/grub/grub.cfg", target);
        if (gc) {
            copy_file(gc, p);
        } else if (kb && mb) {
            write_file(p, GEN_GRUB);
        }
    }

    /* skeleton files */
    snprintf(p, sizeof(p), "%s/etc/hostname", target);
    write_file(p, "cact\n");

    snprintf(p, sizeof(p), "%s/etc/cgoct.conf", target);
    write_file(p,
        "# cgoct supervisor configuration\n"
        "#services=logd devd netd resolved powerd\n");

    snprintf(p, sizeof(p), "%s/etc/mnts", target);
    write_file(p, "");

    msg("cact-rootfs: skeleton deployed under %s\n", target);
    return 0;
}
