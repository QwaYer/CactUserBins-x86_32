/*
 * builtins/sys.c — системные команды.
 *
 *   clear / date / uptime / kill / su / sleep / free / fetch / run
 *   modload / modunload  — PCI .cctk kmod (root); modunload [pci-index|name]
 */

#include "version.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

int cact_ub_clear(char **argv, int argc) {
    (void)argv; (void)argc;
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}

int cact_ub_date(char **argv, int argc) {
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

int cact_ub_uptime(char **argv, int argc) {
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

int cact_ub_kill(char **argv, int argc) {
    if (argc < 2) {
        write(STDERR_FILENO, "usage: kill [-SIG] PID...\n", 26);
        write(STDERR_FILENO, "  -1 SIGKILL  -2 SIGTERM  -9 SIGKILL(posix)  -15 SIGTERM(posix)\n", 65);
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

int cact_ub_su(char **argv, int argc) {
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

int cact_ub_sleep(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: sleep SECONDS\n", 21); return 1; }
    sleep((unsigned int)atoi(argv[1]));
    return 0;
}

int cact_ub_free(char **argv, int argc) {
    (void)argv; (void)argc;
    void *brk = sbrk(0);
    char buf[32];
    write(STDOUT_FILENO, "heap brk: 0x", 12);
    hex_to_ascii((unsigned int)(size_t)brk, buf);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

int cact_ub_fetch(char **argv, int argc) {
    (void)argv; (void)argc;
    static const char logo[] =
        "\033[36m"
        "  _                 \n"
        " | |   _   ___  __  \n"
        " | |  | | | \\ \\/ /  \n"
        " | |__| |_| |>  <   \n"
        " |_____\\__,_/_/\\_\\  \n"
        "\033[0m\n";
    static const char shell_line[] =
        "\033[33mShell :\033[0m cactsole " CACTSOLE_VERSION "\n";
    static const char kernel_line[] =
        "\033[33mKernel:\033[0m Cact x86_32\n";
    static const char arch_line[] =
        "\033[33mArch  :\033[0m i686\n";
    static const char user_label[] =
        "\033[33mUser  :\033[0m uid=";
    write(STDOUT_FILENO, logo, sizeof(logo) - 1);
    write(STDOUT_FILENO, kernel_line, sizeof(kernel_line) - 1);
    write(STDOUT_FILENO, shell_line, sizeof(shell_line) - 1);
    write(STDOUT_FILENO, arch_line, sizeof(arch_line) - 1);
    char buf[16];
    uid_t uid = getuid();
    write(STDOUT_FILENO, user_label, sizeof(user_label) - 1);
    itoa((int)uid, buf); write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
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

int cact_ub_modload(char **argv, int argc) {
    if (argc < 2) {
        static const char usage[] =
            "usage: modload PATH [VENDOR_ID DEVICE_ID]\n"
            "  IDs omitted: use cact_pci_* manifest inside the .cctk\n"
            "  example: modload virtio_net.cctk\n"
            "  example: modload /lib/virtio_net.cctk 0x1AF4 0x1041\n";
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
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
        static const char usage[] =
            "usage: modload PATH [VENDOR_ID DEVICE_ID]\n";
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }
    int rc = module_load(argv[1], vid, did);
    if (rc == 0) {
        write(STDOUT_FILENO, "modload: ok\n", 12);
        return 0;
    }
    if (rc == -1)
        write(STDERR_FILENO, "modload: permission denied (need root)\n", 41);
    else if (rc == -2)
        write(STDERR_FILENO, "modload: invalid path or PCI id\n", 32);
    else if (rc == -3)
        write(STDERR_FILENO, "modload: module slot busy (modunload first)\n", 44);
    else if (rc == -4) {
        static const char e4[] =
            "modload: module did not bind (VID/DID mismatch or load error — see kernel log)\n";
        write(STDERR_FILENO, e4, sizeof(e4) - 1);
    }
    else {
        write(STDERR_FILENO, "modload: failed (", 17);
        char b[16];
        itoa(rc, b);
        write(STDERR_FILENO, b, strlen(b));
        write(STDERR_FILENO, ")\n", 2);
    }
    return 1;
}

int cact_ub_modunload(char **argv, int argc) {
    if (argc > 2) {
        write(STDERR_FILENO, "usage: modunload [PCI_INDEX|DRIVER_NAME]\n", 41);
        write(STDERR_FILENO, "  no args: unload usermod slot\n", 31);
        write(STDERR_FILENO, "  number:  same as [pci N] in /dev/modinfo\n", 43);
        return 1;
    }
    const char *target = (argc == 2) ? argv[1] : NULL;
    int rc = module_unload(target);
    if (rc == 0) {
        write(STDOUT_FILENO, "modunload: ok\n", 14);
        return 0;
    }
    if (rc == -1)
        write(STDERR_FILENO, "modunload: permission denied (need root)\n", 41);
    else if (rc == -2)
        write(STDERR_FILENO, "modunload: invalid argument\n", 28);
    else if (rc == -5)
        write(STDERR_FILENO, "modunload: no driver with that name\n", 36);
    else if (rc == -6)
        write(STDERR_FILENO, "modunload: not a relocatable module (built-in)\n", 47);
    else if (rc == -7)
        write(STDERR_FILENO, "modunload: no PCI function at that index (see /dev/modinfo)\n", 61);
    else if (rc == -8)
        write(STDERR_FILENO, "modunload: no .cctk driver for that PCI function\n", 49);
    else {
        write(STDERR_FILENO, "modunload: failed\n", 18);
    }
    return 1;
}

int cact_ub_run(char **argv, int argc) {
    if (argc < 2) {
        write(STDERR_FILENO, "usage: run /path/to/program [args...]\n", 38);
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
