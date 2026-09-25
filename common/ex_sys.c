/*
 * builtins/sys.c — системные команды.
 *
 *   clear / date / uptime / kill / su / sleep / free / sysinfo / run
 *   modload / modunload  — PCI .cctk kmod (root); modunload [pci-index|name]
 *   poweroff / reboot / halt / suspend — через powerd (fallback: reboot(2))
 */

#include "version.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <socket.h>
#include <stdint.h>
#include <stdio.h>
#include <nodeio.h>
#include <drm.h>
#include <drm_mode.h>

static const char clear_usage[] = "usage: clear\n";

int cact_ub_clear(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, clear_usage, sizeof(clear_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}

static const char date_usage[] = "usage: date\n";

int cact_ub_date(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, date_usage, sizeof(date_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    struct timeval tv;
    gettimeofday(&tv, 0);
    long s = tv.tv_sec;
    int sec_v  = (int)(s % 60); s /= 60;
    int min_v  = (int)(s % 60); s /= 60;
    int hour_v = (int)(s % 24); s /= 24;
    long days  = s;
    int year   = 1970;
    while (1) {
        int ydays = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
        if (days < (long)ydays) break;
        days -= ydays;
        year++;
    }
    int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) mdays[1] = 29;
    int month = 0;
    while (month < 12 && days >= (long)mdays[month]) { days -= mdays[month]; month++; }
    int day = (int)days + 1;
    char buf[16];
    itoa(year, buf);        write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "-", 1);
    if (month + 1 < 10)     write(STDOUT_FILENO, "0", 1);
    itoa(month + 1, buf);   write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "-", 1);
    if (day < 10)           write(STDOUT_FILENO, "0", 1);
    itoa(day, buf);         write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, " ", 1);
    if (hour_v < 10)        write(STDOUT_FILENO, "0", 1);
    itoa(hour_v, buf);      write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, ":", 1);
    if (min_v < 10)         write(STDOUT_FILENO, "0", 1);
    itoa(min_v, buf);       write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, ":", 1);
    if (sec_v < 10)         write(STDOUT_FILENO, "0", 1);
    itoa(sec_v, buf);       write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, " UTC\n", 5);
    return 0;
}

static const char uptime_usage[] = "usage: uptime\n";

int cact_ub_uptime(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, uptime_usage, sizeof(uptime_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long total = ts.tv_sec;
    long d     = total / 86400; total %= 86400;
    int  h     = (int)(total / 3600); total %= 3600;
    int  m     = (int)(total / 60);
    char buf[32];
    write(STDOUT_FILENO, "up ", 3);
    if (d > 0) {
        itoa((int)d, buf); write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, " day(s), ", 9);
    }
    itoa(h, buf); write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, ":", 1);
    if (m < 10) write(STDOUT_FILENO, "0", 1);
    itoa(m, buf); write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

static int map_sig(int n) {
    switch (n) {
        case 1:  return (int)SIGHUP;
        case 2:  return (int)SIGINT;
        case 3:  return (int)SIGQUIT;
        case 9:  return (int)SIGKILL;
        case 15: return (int)SIGTERM;
        case 17: return (int)SIGCHLD;
        case 18: return (int)SIGCONT;
        case 19: return (int)SIGSTOP;
        default: return n;
    }
}

static const char kill_usage[] =
    "usage: kill [-SIG] PID...\n"
    "  -1 SIGKILL  -2 SIGTERM  -9 SIGKILL(posix)  -15 SIGTERM(posix)\n";

int cact_ub_kill(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, kill_usage, sizeof(kill_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        write(STDERR_FILENO, kill_usage, sizeof(kill_usage) - 1);
        return 1;
    }
    int sig   = (int)SIGTERM;
    int start = 1;
    if (argv[1][0] == '-') {
        sig   = map_sig(atoi(argv[1] + 1));
        start = 2;
    }
    int i, ret = 0;
    for (i = start; i < argc; i++) {
        pid_t pid = (pid_t)atoi(argv[i]);
        if (kill(pid, sig) != 0) {
            write(STDERR_FILENO, "kill: ", 6);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

static const char su_usage[] = "usage: su [UID [GID]]\n";

int cact_ub_su(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, su_usage, sizeof(su_usage) - 1);
        return 0;
    }
    uid_t uid = 0;
    gid_t gid = 0;
    if (argc >= 2) {
        uid = (uid_t)atoi(argv[1]);
        gid = (gid_t)uid;
        if (argc >= 3) gid = (gid_t)atoi(argv[2]);
    }
    if (setuid(uid) != 0 || setgid(gid) != 0) {
        write(STDERR_FILENO, "su: permission denied\n", 22);
        return 1;
    }
    char buf[16];
    write(STDOUT_FILENO, "switched to uid=", 16);
    itoa((int)uid, buf); write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, " gid=", 5);
    itoa((int)gid, buf); write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

static const char sleep_usage[] = "usage: sleep SECONDS\n";

int cact_ub_sleep(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sleep_usage, sizeof(sleep_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, sleep_usage, sizeof(sleep_usage) - 1); return 1; }
    sleep((unsigned int)atoi(argv[1]));
    return 0;
}

static const char free_usage[] = "usage: free\n";

int cact_ub_free(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, free_usage, sizeof(free_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    void *brk = sbrk(0);
    char buf[32];
    write(STDOUT_FILENO, "heap brk: 0x", 12);
    hex_to_ascii((unsigned int)(size_t)brk, buf);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* Значение строки текстового /proc-файла, начинающейся с key: текст после
 * ':' и пробелов копируется в out.  Возвращает 0, если поле найдено. */
static int proc_field(const char *buf, const char *key, char *out, int cap) {
    int klen = (int)strlen(key);
    const char *p = buf;
    out[0] = '\0';
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0) {
            const char *v = p + klen;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ':') { v++; while (*v == ' ' || *v == '\t') v++; }
            int o = 0;
            while (*v && *v != '\n' && *v != '\r' && o < cap - 1)
                out[o++] = *v++;
            out[o] = '\0';
            return o > 0 ? 0 : -1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

/* Сколько строк текстового /proc-файла начинаются с key. */
static int proc_count(const char *buf, const char *key) {
    int klen = (int)strlen(key), cnt = 0;
    const char *p = buf;
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0) cnt++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return cnt;
}

/* struct drm_mode_get_connector::connection — ядро кладёт сюда DRM_MODE_* из
 * drm_drv.h; в uapi-заголовке этой пары констант нет. */
#define DRM_MODE_CONNECTED 1

/* /dev/fb0 — загрузочный фреймбуфер (multiboot).  Ядро держит
 * struct fb_var_screeninfo приватно (fs/vfs/devfs/devfs_devices.c), поэтому
 * форма ABI повторена здесь: те же 8 полей подряд. */
#define FBIOGET_VSCREENINFO 0x4600
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset, bits_per_pixel, grayscale;
};

/* Откуда взялась строка Display. */
enum display_src {
    DISP_NONE      = -1,  /* карты нет и фреймбуфера нет — строки не будет */
    DISP_CRTC      = 0,   /* активный CRTC: этот режим реально сканируется */
    DISP_CONNECTOR = 1,   /* preferred-режим коннектора: карта есть, modeset'а нет */
    DISP_FB0       = 2,   /* DRM-карты нет вовсе: размер загрузочного фреймбуфера */
};

/* определён ниже, рядом с modload */
static unsigned parse_u32(const char *s);

/* Карты нет: разрешение даёт загрузочный фреймбуфер — то, что выставил
 * загрузчик и на чём сейчас рисует консоль. */
static int fb0_mode(unsigned *w, unsigned *h) {
    struct fb_var_screeninfo vi;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return -1;

    memset(&vi, 0, sizeof(vi));
    int rc = ioctl(fd, FBIOGET_VSCREENINFO, &vi);
    close(fd);
    if (rc != 0 || vi.xres == 0 || vi.yres == 0) return -1;
    *w = vi.xres;
    *h = vi.yres;
    return 0;
}

/* Видеоадаптер с DRM-карты: /dev/dri/card0 существует, только когда драйвер
 * зарегистрировал устройство, а DRM_IOCTL_VERSION отдаёт ops->name и версию —
 * то же, что drmGetVersion().  -1, если карты нет. */
static int gpu_drm_name(char *out, int cap, int *major, int *minor, int *patch) {
    struct drm_version v;
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return -1;

    memset(&v, 0, sizeof(v));
    v.name = out;
    v.name_len = (size_t)(cap - 1);
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    /* name_len — полная длина имени: ядро копирует min(имя, буфер) и NUL не
     * пишет, так что терминатор ставим сами. */
    int len = (int)v.name_len;
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    out[len] = '\0';
    *major = v.version_major;
    *minor = v.version_minor;
    *patch = v.version_patchlevel;
    return 0;
}

/* Фоллбек без DRM-карты: дисплей-контроллер (class_code 0x03) среди PCI-функций
 * /dev/modinfo.  Там у каждого узла "[pci N]" напечатаны vendor_id, device_id и
 * class_code, поэтому берём их из одного блока; секцию "[drv N]" пропускаем —
 * это драйверы, а не найденное железо. */
static int gpu_pci_ids(unsigned *vendor, unsigned *device) {
    static char buf[16384];
    int got = nio_read_file("/dev/modinfo", buf, sizeof(buf) - 1);
    if (got <= 0) return -1;
    buf[got] = '\0';

    unsigned v = 0, d = 0, cls = 0xFFFFFFFFu;
    int have_v = 0, have_d = 0, in_pci = 0;
    const char *p = buf;

    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;

        const char *s = p;
        while (*s == ' ' || *s == '\t') s++;

        if (strncmp(s, "[pci ", 5) == 0) {
            in_pci = 1;
            v = d = 0;
            cls = 0xFFFFFFFFu;
            have_v = have_d = 0;
        } else if (strncmp(s, "[drv ", 5) == 0) {
            in_pci = 0;
        } else if (in_pci && strncmp(s, "vendor_id:", 10) == 0) {
            v = parse_u32(s + 10);
            have_v = 1;
        } else if (in_pci && strncmp(s, "device_id:", 10) == 0) {
            d = parse_u32(s + 10);
            have_d = 1;
        } else if (in_pci && strncmp(s, "class_code:", 11) == 0) {
            cls = parse_u32(s + 11);
        }

        if (in_pci && have_v && have_d && cls == 0x03) {
            *vendor = v;
            *device = d;
            return 0;
        }

        p = *e ? e + 1 : e;
    }
    return -1;
}

/* Display mode: the active CRTC first (GETRESOURCES + GETCRTC — the same
 * legacy-DRM ioctls any DRM client uses), and if nothing has modeset yet, the
 * preferred mode of a connected connector.  Any failure simply means there is
 * no Display line: sysinfo must work without a GPU. */
static enum display_src display_mode(unsigned *w, unsigned *h, unsigned *hz) {
    uint32_t crtcs[8], conns[8];
    struct drm_mode_card_res res;
    struct drm_mode_crtc gc;
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return DISP_NONE;

    memset(&res, 0, sizeof(res));
    memset(crtcs, 0, sizeof(crtcs));
    memset(conns, 0, sizeof(conns));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.count_crtcs = sizeof(crtcs) / sizeof(crtcs[0]);
    res.count_connectors = sizeof(conns) / sizeof(conns[0]);

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        close(fd);
        return DISP_NONE;
    }

    for (uint32_t i = 0; i < res.count_crtcs && i < sizeof(crtcs) / sizeof(crtcs[0]); i++) {
        memset(&gc, 0, sizeof(gc));
        gc.crtc_id = crtcs[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gc) != 0) continue;
        if (!gc.mode_valid || gc.mode.hdisplay == 0 || gc.mode.vdisplay == 0) continue;
        *w  = gc.mode.hdisplay;
        *h  = gc.mode.vdisplay;
        *hz = gc.mode.vrefresh;
        close(fd);
        return DISP_CRTC;
    }

    /* CRTC ещё не включён: карта есть, поэтому спрашиваем у неё самой режим,
     * которым она умеет сканировать. */
    for (uint32_t i = 0; i < res.count_connectors && i < sizeof(conns) / sizeof(conns[0]); i++) {
        struct drm_mode_get_connector cc;
        struct drm_mode_modeinfo modes[8];
        int pick = -1;

        memset(&cc, 0, sizeof(cc));
        memset(modes, 0, sizeof(modes));
        cc.connector_id = conns[i];
        cc.modes_ptr = (uint64_t)(uintptr_t)modes;
        cc.count_modes = sizeof(modes) / sizeof(modes[0]);
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &cc) != 0) continue;
        if (cc.connection != DRM_MODE_CONNECTED) continue;

        for (uint32_t k = 0; k < cc.count_modes && k < sizeof(modes) / sizeof(modes[0]); k++) {
            if (modes[k].hdisplay == 0 || modes[k].vdisplay == 0) continue;
            if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { pick = (int)k; break; }
            if (pick < 0) pick = (int)k;
        }
        if (pick < 0) continue;

        *w  = modes[pick].hdisplay;
        *h  = modes[pick].vdisplay;
        *hz = modes[pick].vrefresh;
        close(fd);
        return DISP_CONNECTOR;
    }

    close(fd);
    return DISP_NONE;
}

static const char sysinfo_usage[] = "usage: sysinfo\n";

int cact_ub_sysinfo(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sysinfo_usage, sizeof(sysinfo_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;

    static const char *logo[] = {
        "     |_|_|           ",
        "     \\_|||;;_/       ",
        "    \\d||%||%:b/      ",
        "   \\d~|dO%|i::b/     ",
        "  ._H||dSf|||%::H_.  ",
        "  ._H@|dLF|}|;::H_.  ",
        "  ._H||dXFt||;.:H_.  ",
        "  ._?|{|P|||/;:.P_.  ",
        "   ._Hy||t|||;:H_.   ",
        "   ._?|x||T|;i:P_.   ",
        "    ._H||i||;:H_.    ",
        "    ._H|\"|||;:H_.    ",
        " .=================.  ",
        "|;;|#H#|;;;;;;;;: |  ",
        ".=================.   ",
        " |;|#H#|;;;;;;;: |   ",
        "  |;|#H#|;;;;;: |    ",
        "  |;|#H#|;;;;;: |    ",
        "   |;|#H#|;;;: |     ",
        "   |;|#H#|;;;: |     ",
        "    |;|#H#|;: |      ",
        "     =========       ",
        NULL
    };

    char lines[16][128];
    int n = 0;

    struct utsname un;
    int have_uname = (uname(&un) == 0);

    if (have_uname)
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mOS\033[0m: %s (%s)",
                 un.sysname, un.machine);
    else
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mOS\033[0m: Cact OS");
    snprintf(lines[n++], sizeof(lines[0]), "\033[33mKernel\033[0m: %s",
             have_uname ? un.release : "?");

    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long total = ts.tv_sec;
        long d = total / 86400; total %= 86400;
        int  h = (int)(total / 3600); total %= 3600;
        int  m = (int)(total / 60);
        if (d > 0)
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mUptime\033[0m: %ldd %d:%02d", d, h, m);
        else
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mUptime\033[0m: %d:%02d", h, m);
    }

    /* Модель и частоту ядра ядро отдаёт в /proc/cpuinfo. */
    {
        static char cpubuf[4096];
        int got = nio_read_file("/proc/cpuinfo", cpubuf, sizeof(cpubuf) - 1);
        if (got > 0) {
            char model[80];
            cpubuf[got] = '\0';
            if (proc_field(cpubuf, "model name", model, sizeof(model)) == 0)
                snprintf(lines[n++], sizeof(lines[0]), "\033[33mCPU\033[0m: %s", model);

            int cores = proc_count(cpubuf, "processor");
            char mhz[24];
            if (cores > 0 &&
                proc_field(cpubuf, "cpu MHz", mhz, sizeof(mhz)) == 0 &&
                strcmp(mhz, "0") != 0)
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mCores\033[0m: %d @ %s MHz", cores, mhz);
            else if (cores > 0)
                snprintf(lines[n++], sizeof(lines[0]), "\033[33mCores\033[0m: %d", cores);
        }
    }

    /* Память и swap — /proc/meminfo (значения в kB). */
    {
        static char membuf[512];
        int got = nio_read_file("/proc/meminfo", membuf, sizeof(membuf) - 1);
        if (got > 0) {
            char tot[24], used[24], swap[24];
            membuf[got] = '\0';
            if (proc_field(membuf, "MemTotal", tot, sizeof(tot)) == 0 &&
                proc_field(membuf, "MemUsed", used, sizeof(used)) == 0) {
                int t = atoi(tot), u = atoi(used);
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mMemory\033[0m: %d MiB / %d MiB (%d%%)",
                         u / 1024, t / 1024, t > 0 ? (u * 100) / t : 0);
            }
            if (proc_field(membuf, "SwapTotal", swap, sizeof(swap)) == 0) {
                int s = atoi(swap);
                if (s > 0)
                    snprintf(lines[n++], sizeof(lines[0]),
                             "\033[33mSwap\033[0m: %d MiB", s / 1024);
                else
                    snprintf(lines[n++], sizeof(lines[0]), "\033[33mSwap\033[0m: none");
            }
        }
    }

    /* Видеоадаптер: карта есть — спрашиваем её драйвер; карты нет — показываем
     * дисплей-контроллер, найденный по PCI. */
    {
        char gname[64];
        int gmaj = 0, gmin = 0, gpat = 0;
        if (gpu_drm_name(gname, sizeof(gname), &gmaj, &gmin, &gpat) == 0) {
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mGPU\033[0m: %s %d.%d.%d", gname, gmaj, gmin, gpat);
        } else {
            unsigned gv = 0, gd = 0;
            if (gpu_pci_ids(&gv, &gd) == 0)
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mGPU\033[0m: PCI %04x:%04x (no DRM card)", gv, gd);
        }
    }

    /* Дисплей: карта есть — активный CRTC, иначе preferred-режим коннектора;
     * карты нет — загрузочный фреймбуфер /dev/fb0. */
    {
        unsigned dw = 0, dh = 0, dhz = 0;
        enum display_src src = display_mode(&dw, &dh, &dhz);
        if (src == DISP_NONE && fb0_mode(&dw, &dh) == 0)
            src = DISP_FB0;

        if (src == DISP_FB0) {
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mDisplay\033[0m: %ux%u (fb0)", dw, dh);
        } else if (src != DISP_NONE) {
            const char *tag = (src == DISP_CONNECTOR) ? " (preferred)" : "";
            if (dhz > 0)
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mDisplay\033[0m: %ux%u @ %u Hz%s", dw, dh, dhz, tag);
            else
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mDisplay\033[0m: %ux%u%s", dw, dh, tag);
        }
    }

    /* Сеть — тот же ioctl, которым пользуется `ip addr`.  nio_dev_cmd сам
     * добавляет "/dev/", поэтому имя узла здесь без префикса. */
    {
        cact_netcfg_get_t g;
        memset(&g, 0, sizeof(g));
        if (nio_dev_cmd("net", CACT_NETCTL_NETCFG_GET, &g) >= 0 && g.ip_host)
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mNet\033[0m: eth0 %u.%u.%u.%u",
                     (unsigned)((g.ip_host >> 24) & 0xFFu),
                     (unsigned)((g.ip_host >> 16) & 0xFFu),
                     (unsigned)((g.ip_host >> 8) & 0xFFu),
                     (unsigned)(g.ip_host & 0xFFu));
        else
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mNet\033[0m: (no IPv4 address)");
    }

    /* Программы: считаем записи в каталогах, куда ставится CactUserBins. */
    {
        static const char *pkgdirs[] = {"/bin", "/sbin", NULL};
        int pkg = 0;
        for (int d = 0; pkgdirs[d]; d++) {
            int fd = open(pkgdirs[d], O_RDONLY, 0);
            if (fd < 0) continue;
            struct dirent dbuf[32];
            int r;
            while ((r = getdents(fd, dbuf, sizeof(dbuf))) > 0) {
                int cnt = r / (int)sizeof(struct dirent);
                for (int i = 0; i < cnt; i++)
                    if (dbuf[i].d_name[0] != '.') pkg++;
            }
            close(fd);
        }
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mPackages\033[0m: %d", pkg);
    }

    snprintf(lines[n++], sizeof(lines[0]), "\033[33mShell\033[0m: cactsole %s",
             CACTSOLE_VERSION);
    snprintf(lines[n++], sizeof(lines[0]), "\033[33mUser\033[0m: uid=%d",
             (int)getuid());

    int lw = 0;
    for (int k = 0; logo[k]; k++) {
        int sl = (int)strlen(logo[k]);
        if (sl > lw) lw = sl;
    }

    int i = 0;
    while (logo[i]) {
        char buf[224];
        int pos = 0;
        const char *col = (i < 12) ? "\033[32m" : "\033[33m";
        memcpy(buf + pos, col, 5); pos += 5;
        int llen = (int)strlen(logo[i]);
        memcpy(buf + pos, logo[i], llen); pos += llen;
        memcpy(buf + pos, "\033[0m", 4); pos += 4;
        for (int s = llen; s < lw; s++)
            buf[pos++] = ' ';
        if (i < n) {
            buf[pos++] = ' ';
            buf[pos++] = ' ';
            int sl = (int)strlen(lines[i]);
            memcpy(buf + pos, lines[i], sl); pos += sl;
        }
        buf[pos++] = '\r';
        buf[pos++] = '\n';
        write(STDOUT_FILENO, buf, pos);
        i++;
    }
    while (i < n) {
        char buf[224];
        int pos = 0;
        for (int s = 0; s < lw + 2; s++)
            buf[pos++] = ' ';
        int sl = (int)strlen(lines[i]);
        memcpy(buf + pos, lines[i], sl); pos += sl;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
        write(STDOUT_FILENO, buf, pos);
        i++;
    }

    return 0;
}

static unsigned parse_u32(const char *s) {
    if (!s || !s[0]) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        unsigned v = 0;
        while (*s) {
            char c = *s;
            unsigned d;
            if (c >= '0' && c <= '9')
                d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = 10u + (unsigned)(c - 'a');
            else if (c >= 'A' && c <= 'F')
                d = 10u + (unsigned)(c - 'A');
            else
                break;
            v = (v << 4) | d;
            s++;
        }
        return v;
    }
    return (unsigned)atoi(s);
}

static const char modload_usage[] =
    "usage: modload PATH [VENDOR_ID DEVICE_ID]\n"
    "  IDs omitted: use cact_pci_* manifest inside the .cctk\n"
    "  example: modload virtio_net.cctk\n"
    "  example: modload /lib/virtio_net.cctk 0x1AF4 0x1041\n";

int cact_ub_modload(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, modload_usage, sizeof(modload_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        write(STDERR_FILENO, modload_usage, sizeof(modload_usage) - 1);
        return 1;
    }
    unsigned vid, did;
    if (argc == 2) {
        vid = 0xFFFFFFFFu;
        did = 0xFFFFFFFFu;
    } else if (argc == 4) {
        vid = parse_u32(argv[2]);
        did = parse_u32(argv[3]);
    } else {
        write(STDERR_FILENO, modload_usage, sizeof(modload_usage) - 1);
        return 1;
    }
    int rc = module_load(argv[1], vid, did);
    if (rc == 0) {
        write(STDOUT_FILENO, "modload: ok\n", 12);
        return 0;
    }
    // module_load() folds the kernel's error into errno (the /dev/sys ioctl
    // returns -errno); decode that, never the collapsed -1.
    const char *m;
    char        num[16];
    switch (errno) {
    case EPERM:  m = "modload: permission denied (need root)\n";            break;
    case ENOENT: m = "modload: no such module (not in cctkfs/VFS)\n";       break;
    case EACCES: m = "modload: module signature rejected (bad HMAC)\n";     break;
    case EBUSY:  m = "modload: module slot busy (modunload first)\n";       break;
    case EEXIST: m = "modload: already loaded (modunload first)\n";          break;
    case ENODEV: m = "modload: module did not bind: no matching PCI device "
                     "(see the kernel log)\n";                              break;
    case EINVAL: m = "modload: invalid module image or PCI id\n";           break;
    case ENOSPC: m = "modload: no free module slot\n";                      break;
    case ENOMEM: m = "modload: out of memory\n";                            break;
    default:
        m = "modload: failed (errno ";
        write(STDERR_FILENO, m, strlen(m));
        itoa(errno, num);
        write(STDERR_FILENO, num, strlen(num));
        write(STDERR_FILENO, ")\n", 2);
        return 1;
    }
    write(STDERR_FILENO, m, strlen(m));
    return 1;
}

static const char modunload_usage[] =
    "usage: modunload [PCI_INDEX|DRIVER_NAME]\n"
    "  no args: unload every loaded module\n"
    "  name:    module name, e.g. ahci for ahci.cctk\n"
    "  number:  same as [pci N] in /dev/modinfo\n";

int cact_ub_modunload(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, modunload_usage, sizeof(modunload_usage) - 1);
        return 0;
    }
    if (argc > 2) {
        write(STDERR_FILENO, modunload_usage, sizeof(modunload_usage) - 1);
        return 1;
    }
    const char *target = (argc == 2) ? argv[1] : NULL;
    int rc = module_unload(target);
    if (rc == 0) {
        write(STDOUT_FILENO, "modunload: ok\n", 14);
        return 0;
    }
    // Same as modload: the reason is in errno, not in the collapsed return.
    const char *m;
    char        num[16];
    switch (errno) {
    case EPERM:  m = "modunload: permission denied (need root)\n";          break;
    case EINVAL: m = "modunload: invalid argument\n";                       break;
    case ENOENT: m = "modunload: no driver with that name\n";               break;
    case ENODEV: m = "modunload: no .cctk driver for that PCI function\n";  break;
    case EOPNOTSUPP: m = "modunload: not a relocatable module (built-in)\n"; break;
    case EBUSY:  m = "modunload: module busy (still mounted)\n";            break;
    default:
        m = "modunload: failed (errno ";
        write(STDERR_FILENO, m, strlen(m));
        itoa(errno, num);
        write(STDERR_FILENO, num, strlen(num));
        write(STDERR_FILENO, ")\n", 2);
        return 1;
    }
    write(STDERR_FILENO, m, strlen(m));
    return 1;
}

static const char run_usage[] = "usage: run /path/to/program [args...]\n";

int cact_ub_run(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, run_usage, sizeof(run_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        write(STDERR_FILENO, run_usage, sizeof(run_usage) - 1);
        return 1;
    }

    const char *prog = argv[1];

    pid_t pid = fork();
    if (pid < 0) {
        write(STDERR_FILENO, "run: fork failed\n", 17);
        return 1;
    }
    if (pid == 0) {
        char **child_argv = &argv[1];
        execve((char *)prog, child_argv, NULL);

        if (prog[0] == '/' &&
            prog[1] == 'b' &&
            prog[2] == 'i' &&
            prog[3] == 'n' &&
            prog[4] == '/') {
            /* Fallback for current kernel exec path normalization issues. */
            child_argv[0] = (char *)(prog + 5);
            execve((char *)(prog + 5), child_argv, NULL);
        }
        if (prog[0] == '/' &&
            prog[1] == 's' &&
            prog[2] == 'b' &&
            prog[3] == 'i' &&
            prog[4] == 'n' &&
            prog[5] == '/') {
            child_argv[0] = (char *)(prog + 6);
            execve((char *)(prog + 6), child_argv, NULL);
        }

        write(STDERR_FILENO, "run: exec failed: ", 18);
        write(STDERR_FILENO, (char *)prog, strlen((char *)prog));
        write(STDERR_FILENO, "\n", 1);
        exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        write(STDERR_FILENO, "run: waitpid failed\n", 20);
        return 1;
    }
    return (status >> 8) & 0xff;
}

/* ── Питание ────────────────────────────────────────────────────────────────
 *
 * Команды не трогают /dev/sys напрямую: они просят системный демон powerd по
 * его AF_UNIX-сокету — так же, как systemctl просит logind.  Если демон не
 * поднят, запрос уходит в ядро напрямую через reboot(2), чтобы команда не
 * «повисала» без сервиса.  suspend() возвращается только после пробуждения. */

#define POWERD_SOCK "/run/powerd.sock"

static int powerd_ask(const char *verb) {
    struct sockaddr_un sa;
    char line[64];
    int fd, n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, POWERD_SOCK, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }

    n = 0;
    for (const char *p = verb; *p && n < (int)sizeof(line) - 2; p++)
        line[n++] = *p;
    line[n++] = '\n';
    if (write(fd, line, (uint32_t)n) != n) {
        close(fd);
        return -1;
    }

    char rbuf[64];
    int r = (int)read(fd, rbuf, sizeof(rbuf));
    close(fd);
    if (r > 0)
        write(STDOUT_FILENO, rbuf, (uint32_t)r);
    return 0;
}

static int power_cmd(const char *verb, int fallback_cmd) {
    if (powerd_ask(verb) == 0)
        return 0;

    if (reboot(fallback_cmd) == 0)
        return 0;

    write(STDERR_FILENO, "power: powerd unreachable and reboot() failed\n", 45);
    return 1;
}

static const char poweroff_usage[] = "usage: poweroff\n";

int cact_ub_poweroff(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, poweroff_usage, sizeof(poweroff_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    return power_cmd("poweroff", RB_POWER_OFF);
}

static const char reboot_usage[] = "usage: reboot\n";

int cact_ub_reboot(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, reboot_usage, sizeof(reboot_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    return power_cmd("reboot", RB_AUTOBOOT);
}

static const char halt_usage[] = "usage: halt\n";

int cact_ub_halt(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, halt_usage, sizeof(halt_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    return power_cmd("halt", RB_HALT_SYSTEM);
}

static const char suspend_usage[] = "usage: suspend\n";

int cact_ub_suspend(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, suspend_usage, sizeof(suspend_usage) - 1);
        return 0;
    }
    (void)argv; (void)argc;
    return power_cmd("suspend", RB_SUSPEND);
}
