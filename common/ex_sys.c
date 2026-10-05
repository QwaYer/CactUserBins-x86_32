/*
 * builtins/sys.c — system commands.
 *
 *   clear / date / uptime / kill / su / sleep / free / sysinfo / run
 *   modload / modunload  — PCI .cctk kmod (root); modunload [pci-index|name]
 *   poweroff / reboot / halt / suspend — via powerd (fallback: reboot(2))
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

static const char sleep_usage[] =
    "usage: sleep NUMBER...\n"
    "Pause for the total of the given durations (fractions allowed).\n";

int cact_ub_sleep(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sleep_usage, sizeof(sleep_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, sleep_usage, sizeof(sleep_usage) - 1); return 1; }

    double total = 0.0;
    for (int i = 1; i < argc; i++) {
        char *end = NULL;
        double v = strtod(argv[i], &end);
        if (end == argv[i] || (end && *end != '\0') || v < 0.0) {
            fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[i]);
            return 1;
        }
        total += v;
    }
    while (total > 0.0) {
        if (total > 3600.0) {
            sleep(3600);
            total -= 3600.0;
        } else {
            usleep((unsigned int)(total * 1000000.0 + 0.5));
            total = 0.0;
        }
    }
    return 0;
}

static int proc_field(const char *buf, const char *key, char *out, int cap);

static const char free_usage[] =
    "usage: free [-b|-k|-m|-g|-h] [-t] [-s N] [--help]\n"
    "Show memory use from /proc/meminfo.\n"
    "  -b/-k/-m/-g  units: bytes / KiB / MiB / GiB\n"
    "  -h           human-readable units (default)\n"
    "  -t           show a total row\n"
    "  -s N         repeat every N seconds\n";

/* Format a byte count as a column value.  unit < 0 selects human-readable
 * units (Ki/Mi/Gi/Ti, one decimal when the integer part is below 10); unit
 * 0..4 forces B/Ki/Mi/Gi/Ti. */
static void free_fmt(unsigned long long b, int unit, char *out, int cap) {
    static const char *sfx[5] = { "B", "Ki", "Mi", "Gi", "Ti" };
    if (unit >= 0) {
        unsigned long long d = 1ULL << (10 * unit);
        snprintf(out, cap, "%llu%s", b / d, sfx[unit]);
        return;
    }
    int u = 0;
    unsigned long long d = 1ULL;
    while (u < 4 && b >= d * 1024ULL) { d *= 1024ULL; u++; }
    if (u == 0) { snprintf(out, cap, "%lluB", b); return; }
    unsigned long long w = b / d;
    unsigned long long t = (b % d) * 10ULL / d;
    if (w < 10 && t != 0) snprintf(out, cap, "%llu.%llu%s", w, t, sfx[u]);
    else                  snprintf(out, cap, "%llu%s", w, sfx[u]);
}

/* Value of a /proc/meminfo field in bytes (the kernel reports kB). */
static unsigned long long free_kb_field(const char *buf, const char *key) {
    char v[24];
    if (proc_field(buf, key, v, sizeof(v)) != 0) return 0;
    return (unsigned long long)atoll(v) * 1024ULL;
}

int cact_ub_free(char **argv, int argc) {
    int unit = -1;         /* < 0 = human-readable */
    int show_total = 0;
    int interval = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            write(STDOUT_FILENO, free_usage, sizeof(free_usage) - 1);
            return 0;
        }
        if (a[0] != '-' || a[1] == '\0') {
            fprintf(stderr, "free: unexpected argument '%s'\n", a);
            return 1;
        }
        for (int j = 1; a[j]; j++) {
            switch (a[j]) {
            case 'b': unit = 0; break;
            case 'k': unit = 1; break;
            case 'm': unit = 2; break;
            case 'g': unit = 3; break;
            case 'h': unit = -1; break;
            case 't': show_total = 1; break;
            case 's': {
                const char *v = a[j + 1] ? &a[j + 1]
                                         : (i + 1 < argc ? argv[++i] : NULL);
                if (!v || *v == '\0') {
                    fprintf(stderr, "free: option -s requires a delay\n");
                    return 1;
                }
                interval = atoi(v);
                j = (int)strlen(a);   /* the rest of this option was the delay */
                break;
            }
            default:
                fprintf(stderr, "free: invalid option -- '%c'\n", a[j]);
                return 1;
            }
        }
    }

    static char mem[768];
    for (;;) {
        int got = nio_read_file("/proc/meminfo", mem, sizeof(mem) - 1);
        if (got <= 0) {
            void *brk = sbrk(0);
            printf("heap brk: 0x%08x\n", (unsigned)(size_t)brk);
            return 0;
        }
        mem[got] = '\0';

        unsigned long long m_total = free_kb_field(mem, "MemTotal");
        unsigned long long m_free  = free_kb_field(mem, "MemFree");
        unsigned long long m_avail = free_kb_field(mem, "MemAvailable");
        unsigned long long m_buf   = free_kb_field(mem, "Buffers");
        unsigned long long m_cache = free_kb_field(mem, "Cached");
        unsigned long long m_slab  = free_kb_field(mem, "Slab");
        unsigned long long m_shm   = free_kb_field(mem, "Shmem");

        unsigned long long s_total = free_kb_field(mem, "SwapTotal");
        unsigned long long s_free  = free_kb_field(mem, "SwapFree");
        unsigned long long s_used  = free_kb_field(mem, "SwapUsed");
        if (s_used == 0 && s_total > s_free) s_used = s_total - s_free;

        unsigned long long k_total = free_kb_field(mem, "HeapTotal");
        unsigned long long k_free  = free_kb_field(mem, "HeapFree");
        unsigned long long k_used  = free_kb_field(mem, "HeapUsed");
        if (k_used == 0 && k_total > k_free) k_used = k_total - k_free;

        if (m_avail == 0) m_avail = m_free;

        /* Linux semantics: used = total - free - buff/cache. */
        unsigned long long buffcache = m_buf + m_cache + m_slab;
        unsigned long long m_used = (m_total >= m_free + buffcache)
                                        ? m_total - m_free - buffcache : 0;

        char ct[16], cu[16], cf[16], cs[16], cb[16], ca[16];
        char kt[16], ku[16], kf[16];
        char st[16], su[16], sf[16];

        free_fmt(m_total,   unit, ct, sizeof(ct));
        free_fmt(m_used,    unit, cu, sizeof(cu));
        free_fmt(m_free,    unit, cf, sizeof(cf));
        free_fmt(m_shm,     unit, cs, sizeof(cs));
        free_fmt(buffcache, unit, cb, sizeof(cb));
        free_fmt(m_avail,   unit, ca, sizeof(ca));
        free_fmt(k_total,   unit, kt, sizeof(kt));
        free_fmt(k_used,    unit, ku, sizeof(ku));
        free_fmt(k_free,    unit, kf, sizeof(kf));
        free_fmt(s_total,   unit, st, sizeof(st));
        free_fmt(s_used,    unit, su, sizeof(su));
        free_fmt(s_free,    unit, sf, sizeof(sf));

        printf("%-8s%12s%12s%12s%12s%12s%12s\n",
               "", "total", "used", "free", "shared", "buff/cache", "available");
        printf("%-8s%12s%12s%12s%12s%12s%12s\n",
               "Mem:", ct, cu, cf, cs, cb, ca);
        if (k_total > 0)
            printf("%-8s%12s%12s%12s%12s%12s%12s\n",
                   "Kernel:", kt, ku, kf, "", "", "");
        if (show_total) {
            char tt[16], tu[16], tf[16];
            free_fmt(m_total + s_total, unit, tt, sizeof(tt));
            free_fmt(m_used + s_used,   unit, tu, sizeof(tu));
            free_fmt(m_free + s_free,   unit, tf, sizeof(tf));
            printf("%-8s%12s%12s%12s%12s%12s%12s\n",
                   "Total:", tt, tu, tf, "", "", "");
        }
        printf("%-8s%12s%12s%12s%12s%12s%12s\n",
               "Swap:", st, su, sf, "", "", "");

        if (interval <= 0) break;
        sleep((unsigned)interval);
    }
    return 0;
}

/* Value of the line of a text /proc file that starts with key: the text after
 * ':' and spaces is copied into out.  Returns 0 if the field is found. */
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

/* How many lines of a text /proc file start with key. */
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

/* struct drm_mode_get_connector::connection — the kernel puts DRM_MODE_* from
 * drm_drv.h here; this pair of constants is absent from the uapi header. */
#define DRM_MODE_CONNECTED 1

/* /dev/fb0 — the boot framebuffer (multiboot).  The kernel keeps
 * struct fb_var_screeninfo private (fs/vfs/devfs/devfs_devices.c), so the
 * ABI shape is repeated here: the same 8 fields in a row. */
#define FBIOGET_VSCREENINFO 0x4600
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset, bits_per_pixel, grayscale;
};

/* Where the Display line comes from. */
enum display_src {
    DISP_NONE      = -1,  /* no card and no framebuffer — there will be no line */
    DISP_CRTC      = 0,   /* active CRTC: this mode is actually scanned out */
    DISP_CONNECTOR = 1,   /* connector preferred mode: card present, no modeset yet */
    DISP_FB0       = 2,   /* no DRM card at all: size of the boot framebuffer */
};

/* defined below, next to modload */
static unsigned parse_u32(const char *s);

/* No card: the resolution comes from the boot framebuffer — what the
 * bootloader set up and what the console is currently drawing on. */
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

/* Video adapter from the DRM card: /dev/dri/card0 exists only once the driver
 * has registered the device, and DRM_IOCTL_VERSION returns ops->name and the
 * version — the same as drmGetVersion().  -1 if there is no card. */
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

    /* name_len — the full length of the name: the kernel copies min(name, buffer)
     * and does not write a NUL, so we place the terminator ourselves. */
    int len = (int)v.name_len;
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    out[len] = '\0';
    *major = v.version_major;
    *minor = v.version_minor;
    *patch = v.version_patchlevel;
    return 0;
}

/* Fallback without a DRM card: the display controller (class_code 0x03) among the
 * PCI functions of /dev/modinfo.  There each "[pci N]" node prints vendor_id,
 * device_id and class_code, so we take them from one block; the "[drv N]"
 * section is skipped — those are drivers, not discovered hardware. */
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

/* DRM-style label for a connector ("DP-1", "HDMI-A-1", "Virtual-1"), matching
 * how the kernel names them. */
static void drm_connector_name(uint32_t type, uint32_t id, char *out, int cap) {
    const char *base;
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA:         base = "VGA";       break;
    case DRM_MODE_CONNECTOR_DVII:        base = "DVI-I";     break;
    case DRM_MODE_CONNECTOR_DVID:        base = "DVI-D";     break;
    case DRM_MODE_CONNECTOR_DVIA:        base = "DVI-A";     break;
    case DRM_MODE_CONNECTOR_Composite:   base = "Composite"; break;
    case DRM_MODE_CONNECTOR_SVIDEO:      base = "SVIDEO";    break;
    case DRM_MODE_CONNECTOR_LVDS:        base = "LVDS";      break;
    case DRM_MODE_CONNECTOR_Component:   base = "Component"; break;
    case DRM_MODE_CONNECTOR_9PinDIN:     base = "DIN";       break;
    case DRM_MODE_CONNECTOR_DisplayPort: base = "DP";        break;
    case DRM_MODE_CONNECTOR_HDMIA:       base = "HDMI-A";    break;
    case DRM_MODE_CONNECTOR_HDMIB:       base = "HDMI-B";    break;
    case DRM_MODE_CONNECTOR_TV:          base = "TV";        break;
    case DRM_MODE_CONNECTOR_eDP:         base = "eDP";       break;
    case DRM_MODE_CONNECTOR_VIRTUAL:     base = "Virtual";   break;
    case DRM_MODE_CONNECTOR_DSI:         base = "DSI";       break;
    case DRM_MODE_CONNECTOR_DPI:         base = "DPI";       break;
    default:                             base = "Unknown";   break;
    }
    snprintf(out, (size_t)cap, "%s-%u", base, (unsigned)id);
}

/* Display mode: the active CRTC first (GETRESOURCES + GETCRTC — the same
 * legacy-DRM ioctls any DRM client uses), and if nothing has modeset yet, the
 * preferred mode of a connected connector.  The CRTC does not name the
 * connector it drives, so the connected connector is resolved first and its
 * label is handed back in `conn` ("none" when the card reports none).  Any
 * failure simply means there is no card: sysinfo must work without a GPU. */
static enum display_src display_mode(unsigned *w, unsigned *h, unsigned *hz,
                                     char *conn, int conncap) {
    uint32_t crtcs[8], conns[8];
    struct drm_mode_card_res res;
    struct drm_mode_crtc gc;
    char name[32] = "";
    unsigned cw = 0, chh = 0, chz = 0;
    enum display_src src = DISP_NONE;
    int found = 0;

    if (conncap > 0) conn[0] = '\0';

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

    /* Connected connector: its name (for the label) and preferred mode (used
     * when no CRTC is active yet). */
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

        if (!name[0])
            drm_connector_name(cc.connector_type, cc.connector_type_id,
                               name, sizeof(name));

        for (uint32_t k = 0; k < cc.count_modes && k < sizeof(modes) / sizeof(modes[0]); k++) {
            if (modes[k].hdisplay == 0 || modes[k].vdisplay == 0) continue;
            if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { pick = (int)k; break; }
            if (pick < 0) pick = (int)k;
        }
        if (pick >= 0 && !found) {
            cw  = modes[pick].hdisplay;
            chh = modes[pick].vdisplay;
            chz = modes[pick].vrefresh;
            found = 1;
            src = DISP_CONNECTOR;
        }
    }

    /* An enabled CRTC wins: that is what is actually being scanned out. */
    for (uint32_t i = 0; i < res.count_crtcs && i < sizeof(crtcs) / sizeof(crtcs[0]); i++) {
        memset(&gc, 0, sizeof(gc));
        gc.crtc_id = crtcs[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gc) != 0) continue;
        if (!gc.mode_valid || gc.mode.hdisplay == 0 || gc.mode.vdisplay == 0) continue;
        cw  = gc.mode.hdisplay;
        chh = gc.mode.vdisplay;
        chz = gc.mode.vrefresh;
        found = 1;
        src = DISP_CRTC;
        break;
    }

    close(fd);
    if (!found) return DISP_NONE;

    *w = cw;
    *h = chh;
    *hz = chz;
    if (conncap > 0)
        snprintf(conn, (size_t)conncap, "%s", name[0] ? name : "none");
    return src;
}

/* "a.b.c.d" for a host-order IPv4 address; returns the length written. */
static int sysinfo_ipv4(char *out, int cap, uint32_t v) {
    return snprintf(out, (size_t)cap, "%u.%u.%u.%u",
                    (unsigned)((v >> 24) & 0xFFu), (unsigned)((v >> 16) & 0xFFu),
                    (unsigned)((v >> 8) & 0xFFu), (unsigned)(v & 0xFFu));
}

/* Prefix length of a network mask (255.255.255.0 -> 24). */
static int sysinfo_prefix(uint32_t mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1u); mask >>= 1; }
    return n;
}

/* Human byte size ("12 B", "512.00 MiB", "7.17 GiB"). */
static void sysinfo_size(unsigned long long b, char *out, int cap) {
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    unsigned long long d = 1;
    while (i < 4 && b >= d * 1024ULL) { d *= 1024ULL; i++; }
    if (i == 0) {
        snprintf(out, (size_t)cap, "%llu B", b);
        return;
    }
    snprintf(out, (size_t)cap, "%llu.%02llu %s",
             b / d, (b % d) * 100ULL / d, u[i]);
}

/* Used/total bytes of the ext4 filesystem mounted at `target`, read from the
 * primary superblock of the device named in /proc/mounts — the same source
 * ex_df.c uses.  Returns 0, or -1 when the mount or the superblock is not
 * available. */
static int sysinfo_disk_usage(const char *target,
                              unsigned long long *total,
                              unsigned long long *used) {
    static char mnts[8192];
    char dev[128] = "";
    int fd = open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0) return -1;
    ssize_t got = read(fd, mnts, sizeof(mnts) - 1);
    close(fd);
    if (got <= 0) return -1;
    mnts[got] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(mnts, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *lsp = NULL;
        char *d = strtok_r(line, " \t", &lsp);
        char *t = strtok_r(NULL, " \t", &lsp);
        if (d && t && strcmp(t, target) == 0) {
            strncpy(dev, d, sizeof(dev) - 1);
            dev[sizeof(dev) - 1] = '\0';
            break;
        }
    }
    if (!dev[0]) return -1;

    char path[160];
    if (dev[0] == '/') snprintf(path, sizeof(path), "%s", dev);
    else               snprintf(path, sizeof(path), "/dev/%s", dev);

    unsigned char sb[1024 + 256];
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, sb, sizeof(sb), (off_t)1024);
    close(fd);
    if (r < 256) return -1;
    if (sb[0x38] != 0x53 || sb[0x39] != 0xEF) return -1;   /* ext4 magic 0xEF53 */

    uint32_t blocks = (uint32_t)sb[0x04] | ((uint32_t)sb[0x05] << 8) |
                      ((uint32_t)sb[0x06] << 16) | ((uint32_t)sb[0x07] << 24);
    uint32_t freeb  = (uint32_t)sb[0x0C] | ((uint32_t)sb[0x0D] << 8) |
                      ((uint32_t)sb[0x0E] << 16) | ((uint32_t)sb[0x0F] << 24);
    uint32_t log_bs = (uint32_t)sb[0x18] | ((uint32_t)sb[0x19] << 8) |
                      ((uint32_t)sb[0x1A] << 16) | ((uint32_t)sb[0x1B] << 24);

    unsigned long long bs = 1024ULL << log_bs;
    if (!bs) bs = 1024;
    unsigned long long t = (unsigned long long)blocks * bs;
    unsigned long long f = (unsigned long long)freeb * bs;
    *total = t;
    *used  = t > f ? t - f : 0;
    return 0;
}

/* Console font cell size — the PSF2 header of the boot font.  The kernel loads
 * exactly this file as the console font (see the font driver), so its cell
 * geometry is the system's font identity.  Returns 0, or -1 when absent. */
static int sysinfo_font_cell(char *out, int cap) {
    int fd = open("/usr/share/consolefont.psf", O_RDONLY, 0);
    if (fd < 0) return -1;
    unsigned char h[32];
    ssize_t r = read(fd, h, sizeof(h));
    close(fd);
    if (r < 32) return -1;
    if (h[0] != 0x72 || h[1] != 0xB5 || h[2] != 0x4A || h[3] != 0x86)
        return -1;   /* PSF2 magic 0x72 0xB5 0x4A 0x86 */
    uint32_t height = (uint32_t)h[24] | ((uint32_t)h[25] << 8) |
                      ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
    uint32_t width  = (uint32_t)h[28] | ((uint32_t)h[29] << 8) |
                      ((uint32_t)h[30] << 16) | ((uint32_t)h[31] << 24);
    if (!width || !height) return -1;
    snprintf(out, (size_t)cap, "%ux%u", width, height);
    return 0;
}

/* Uptime worded like fastfetch: "20 mins", "3 hours, 20 mins",
 * "2 days, 1 hour, 5 mins".  Higher units are dropped when zero; minutes are
 * always shown. */
static void sysinfo_uptime_str(long total, char *out, int cap) {
    long d = total / 86400; total %= 86400;
    long h = total / 3600;  total %= 3600;
    long m = total / 60;
    int  n = 0;

    if (d > 0)
        n += snprintf(out + n, (size_t)(cap - n), "%ld day%s", d, d == 1 ? "" : "s");
    if (h > 0) {
        if (n) n += snprintf(out + n, (size_t)(cap - n), ", ");
        n += snprintf(out + n, (size_t)(cap - n), "%ld hour%s", h, h == 1 ? "" : "s");
    }
    if (m > 0 || n == 0) {
        if (n) n += snprintf(out + n, (size_t)(cap - n), ", ");
        n += snprintf(out + n, (size_t)(cap - n), "%ld min%s", m, m == 1 ? "" : "s");
    }
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    out[n] = '\0';
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

    static char lines[32][160];
    int n = 0;

    struct utsname un;
    int have_uname = (uname(&un) == 0);

    if (have_uname)
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mOS\033[0m: %s (%s)",
                 un.sysname, un.machine);
    else
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mOS\033[0m: none");

    /* Host — the SMBIOS system identity that /proc/dmi relays from firmware. */
    {
        static char dmibuf[256];
        char vendor[64] = "", product[64] = "";
        int got = nio_read_file("/proc/dmi", dmibuf, sizeof(dmibuf) - 1);
        if (got > 0) {
            dmibuf[got] = '\0';
            proc_field(dmibuf, "vendor", vendor, sizeof(vendor));
            proc_field(dmibuf, "product", product, sizeof(product));
        }
        if (vendor[0] || product[0])
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mHost\033[0m: %s%s%s",
                     vendor, (vendor[0] && product[0]) ? " " : "", product);
        else
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mHost\033[0m: none");
    }

    snprintf(lines[n++], sizeof(lines[0]), "\033[33mKernel\033[0m: %s",
             have_uname ? un.release : "none");

    {
        struct timespec ts;
        char up[64];
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sysinfo_uptime_str(ts.tv_sec, up, sizeof(up));
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mUptime\033[0m: %s", up);
    }

    /* Programs: count the entries in the directories CactUserBins is installed into. */
    {
        static const char *pkgdirs[] = {"/usr/bin", "/usr/sbin", NULL};
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

    /* Display: the active CRTC (otherwise the connector preferred mode) labelled
     * with its DRM connector; without a card the boot framebuffer /dev/fb0. */
    {
        unsigned dw = 0, dh = 0, dhz = 0;
        char dconn[32] = "";
        enum display_src src = display_mode(&dw, &dh, &dhz, dconn, sizeof(dconn));
        if (src == DISP_NONE && fb0_mode(&dw, &dh) == 0)
            src = DISP_FB0;

        if (src == DISP_FB0) {
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mDisplay\033[0m: %ux%u (fb0)", dw, dh);
        } else if (src != DISP_NONE) {
            if (dhz > 0)
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mDisplay\033[0m (%s): %ux%u @ %u Hz",
                         dconn[0] ? dconn : "none", dw, dh, dhz);
            else
                snprintf(lines[n++], sizeof(lines[0]),
                         "\033[33mDisplay\033[0m (%s): %ux%u",
                         dconn[0] ? dconn : "none", dw, dh);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mDisplay\033[0m: none");
        }
    }

    /* Desktop-environment descriptors: CactOS has no DE/theming stack, so each
     * is reported as "none" rather than dropped. */
    {
        static const char *none_items[] = {
            "DE", "WM", "WM Theme", "Theme", "Icons", "Cursor"
        };
        for (unsigned k = 0; k < sizeof(none_items) / sizeof(none_items[0]); k++)
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33m%s\033[0m: none", none_items[k]);
    }

    /* Font — the console font the kernel loaded; its PSF2 header gives the cell. */
    {
        char cell[24];
        if (sysinfo_font_cell(cell, sizeof(cell)) == 0)
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mFont\033[0m: consolefont.psf %s", cell);
        else
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mFont\033[0m: none");
    }

    /* Terminal this command is attached to. */
    {
        char term[128] = "none";
        if (isatty(STDIN_FILENO) == 1) {
            ssize_t tn = readlink("/proc/self/fd/0", term, sizeof(term) - 1);
            if (tn > 0) term[tn] = '\0';
            else        strcpy(term, "/dev/tty");
        }
        snprintf(lines[n++], sizeof(lines[0]), "\033[33mTerminal\033[0m: %s", term);
    }

    /* CPU model, core count and frequency — /proc/cpuinfo. */
    {
        static char cpubuf[4096];
        int got = nio_read_file("/proc/cpuinfo", cpubuf, sizeof(cpubuf) - 1);
        char model[80] = "";
        char mhz[24] = "";
        int have_model = 0, cores = 0;

        if (got > 0) {
            cpubuf[got] = '\0';
            have_model = (proc_field(cpubuf, "model name", model, sizeof(model)) == 0);
            cores = proc_count(cpubuf, "processor");
            if (proc_field(cpubuf, "cpu MHz", mhz, sizeof(mhz)) != 0 ||
                strcmp(mhz, "0") == 0)
                mhz[0] = '\0';
        }

        if (have_model && cores > 0 && mhz[0]) {
            int m = atoi(mhz);
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mCPU\033[0m: %s (%d) @ %d.%02d GHz",
                     model, cores, m / 1000, (m % 1000) / 10);
        } else if (have_model && cores > 0) {
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mCPU\033[0m: %s (%d)", model, cores);
        } else if (have_model) {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mCPU\033[0m: %s", model);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mCPU\033[0m: none");
        }
    }

    /* GPU: the DRM driver name, or the PCI display controller when no card is
     * registered.  [Discrete]/[Integrated]/[Virtual] is a PCI-vendor heuristic. */
    {
        char gname[64];
        int gmaj = 0, gmin = 0, gpat = 0;
        unsigned gv = 0, gd = 0;
        int have_pci = (gpu_pci_ids(&gv, &gd) == 0);
        const char *tag = "none";

        if (have_pci) {
            switch (gv) {
            case 0x10DE: tag = "Discrete"; break;
            case 0x8086: case 0x1002: case 0x1022: case 0x1A03:
                tag = "Integrated"; break;
            case 0x1AF4: case 0x1234: case 0x15AD:
                tag = "Virtual"; break;
            default: break;
            }
        }

        if (gpu_drm_name(gname, sizeof(gname), &gmaj, &gmin, &gpat) == 0)
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mGPU\033[0m: %s %d.%d.%d [%s]",
                     gname, gmaj, gmin, gpat, tag);
        else if (have_pci)
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mGPU\033[0m: PCI %04x:%04x [%s]", gv, gd, tag);
        else
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mGPU\033[0m: none");
    }

    /* Memory and swap — /proc/meminfo (values in kB). */
    {
        static char membuf[512];
        int got = nio_read_file("/proc/meminfo", membuf, sizeof(membuf) - 1);
        char tot[24] = "", used[24] = "", swap[24] = "";
        int have_mem = 0, have_swap = 0;
        unsigned long long st = 0;

        if (got > 0) {
            membuf[got] = '\0';
            have_mem = (proc_field(membuf, "MemTotal", tot, sizeof(tot)) == 0 &&
                        proc_field(membuf, "MemUsed", used, sizeof(used)) == 0);
            have_swap = (proc_field(membuf, "SwapTotal", swap, sizeof(swap)) == 0);
            if (have_swap) st = (unsigned long long)atoll(swap) * 1024ULL;
        }

        if (have_mem) {
            unsigned long long t = (unsigned long long)atoll(tot) * 1024ULL;
            unsigned long long u = (unsigned long long)atoll(used) * 1024ULL;
            char su_s[24], st_s[24];
            sysinfo_size(u, su_s, sizeof(su_s));
            sysinfo_size(t, st_s, sizeof(st_s));
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mMemory\033[0m: %s / %s (%d%%)",
                     su_s, st_s, t > 0 ? (int)((u * 100ULL) / t) : 0);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mMemory\033[0m: none");
        }

        if (st > 0) {
            char ss[24];
            sysinfo_size(st, ss, sizeof(ss));
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mSwap\033[0m: %s", ss);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mSwap\033[0m: none");
        }
    }

    /* Disk (/) — used/total of the ext4 filesystem mounted at the root. */
    {
        unsigned long long total = 0, used = 0;
        if (sysinfo_disk_usage("/", &total, &used) == 0) {
            char su_s[24], st_s[24];
            sysinfo_size(used, su_s, sizeof(su_s));
            sysinfo_size(total, st_s, sizeof(st_s));
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mDisk (/)\033[0m: %s / %s (%d%%)",
                     su_s, st_s, total > 0 ? (int)((used * 100ULL) / total) : 0);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mDisk (/)\033[0m: none");
        }
    }

    /* Local IP — the same ioctl `ip addr` uses.  The interface name comes from
     * the registered NIC, so a Wi-Fi card reports wlan0 rather than eth0. */
    {
        cact_netcfg_get_t g;
        memset(&g, 0, sizeof(g));
        if (nio_dev_cmd("net", CACT_NETCTL_NETCFG_GET, &g) >= 0 && g.link_up) {
            char ifname[CACT_IFNAME_MAX] = "";
            char ip4[16], addr[24];
            (void)nio_dev_cmd("net", CACT_NETCTL_IFNAME, ifname);
            if (g.ip_host && g.netmask_host) {
                sysinfo_ipv4(ip4, sizeof(ip4), g.ip_host);
                snprintf(addr, sizeof(addr), "%s/%d",
                         ip4, sysinfo_prefix(g.netmask_host));
            } else {
                strcpy(addr, "none");
            }
            snprintf(lines[n++], sizeof(lines[0]),
                     "\033[33mLocal IP\033[0m (%s): %s",
                     ifname[0] ? ifname : "none", addr);
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mLocal IP\033[0m: none");
        }
    }

    /* Locale — LC_ALL/LANG from the environment, else /etc/locale.conf. */
    {
        static char locbuf[256];
        char *loc = getenv("LC_ALL");
        if (!loc || !loc[0]) loc = getenv("LANG");
        if (!loc || !loc[0]) {
            int got = nio_read_file("/etc/locale.conf", locbuf, sizeof(locbuf) - 1);
            if (got > 0) {
                locbuf[got] = '\0';
                for (const char *p = locbuf; *p; ) {
                    if (strncmp(p, "LANG", 4) == 0 && p[4] == '=') {
                        loc = (char *)p + 5;
                        char *e = loc;
                        while (*e && *e != '\n' && *e != '\r') e++;
                        *e = '\0';
                        break;
                    }
                    while (*p && *p != '\n') p++;
                    if (*p) p++;
                }
            }
        }
        if (loc && loc[0])
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mLocale\033[0m: %s", loc);
        else
            snprintf(lines[n++], sizeof(lines[0]), "\033[33mLocale\033[0m: none");
    }

    snprintf(lines[n++], sizeof(lines[0]), "\033[33mUser\033[0m: uid=%d",
             (int)getuid());

    int lw = 0;
    for (int k = 0; logo[k]; k++) {
        int sl = (int)strlen(logo[k]);
        if (sl > lw) lw = sl;
    }

    int i = 0;
    while (logo[i]) {
        char buf[320];
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
        char buf[320];
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
    "  example: modload /usr/lib/modules/virtio_net.cctk 0x1AF4 0x1041\n";

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

/* ── Power ──────────────────────────────────────────────────────────────────
 *
 * The commands do not touch /dev/sys directly: they ask the powerd system daemon
 * over its AF_UNIX socket — just as systemctl asks logind.  If the daemon is not
 * running, the request goes straight to the kernel via reboot(2) so that the
 * command does not hang without the service.  suspend() returns only after wakeup. */

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

