/*
 * ex_admin.c — system and process administration utilities.
 *
 *   lscpu       summarise /proc/cpuinfo
 *   mountpoint  test whether a directory is a mount point (/proc/mounts)
 *   truncate    set or adjust the length of files
 *   pidof       find the PIDs whose command name matches
 *   killall     send a signal to every process with a given name
 *   xargs       build and run command lines from stdin items
 *   timeout     run a command with a time limit
 *
 * Everything here is userspace-only: process names come from /proc/<pid>/comm,
 * the mount table from /proc/mounts.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <wait.h>
#include <stdint.h>
#include <nodeio.h>

/* Iterate numeric /proc/<pid> directories and read each /proc/<pid>/comm.
 * The callback returns non-zero to stop the walk. */
typedef int (*proc_fn)(int pid, const char *comm, void *ud);

static int proc_each(proc_fn fn, void *ud) {
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) return -1;
    int stop = 0;
    struct dirent buf[32];
    int n;
    while (!stop && (n = getdents(fd, buf, sizeof(buf))) > 0) {
        int cnt = n / (int)sizeof(struct dirent);
        for (int i = 0; i < cnt && !stop; i++) {
            const char *nm = buf[i].d_name;
            if (!nm[0]) continue;
            int numeric = 1;
            for (const char *p = nm; *p; p++)
                if (*p < '0' || *p > '9') { numeric = 0; break; }
            if (!numeric) continue;
            char path[64], comm[64];
            snprintf(path, sizeof(path), "/proc/%s/comm", nm);
            int cn = nio_read_file(path, comm, sizeof(comm) - 1);
            if (cn <= 0) continue;
            comm[cn] = '\0';
            while (cn > 0 && (comm[cn - 1] == '\n' || comm[cn - 1] == '\r' ||
                              comm[cn - 1] == ' '))
                comm[--cn] = '\0';
            stop = fn(atoi(nm), comm, ud);
        }
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ lscpu */

static const char lscpu_usage[] =
    "usage: lscpu\n"
    "Summarise the CPUs from /proc/cpuinfo.\n";

static void lscpu_field(const char *buf, const char *key, char *out, size_t n) {
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char *p = buf; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && strncmp(p, key, kl) == 0) {
            const char *c = p + kl;
            while (c < p + ll && (*c == ' ' || *c == '\t' || *c == ':')) c++;
            size_t vl = (size_t)((p + ll) - c);
            while (vl > 0 && (c[vl - 1] == ' ' || c[vl - 1] == '\t')) vl--;
            if (vl >= n) vl = n - 1;
            memcpy(out, c, vl);
            out[vl] = '\0';
            return;
        }
        p = nl ? nl + 1 : p + ll;
    }
}

int cact_ub_lscpu(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, lscpu_usage, sizeof(lscpu_usage) - 1);
        return 0;
    }
    if (argc > 1) fprintf(stderr, "lscpu: ignoring extra arguments\n");

    size_t len = 0;
    char *buf = cact_ub_read_all("/proc/cpuinfo", &len);
    if (!buf) { fprintf(stderr, "lscpu: cannot read /proc/cpuinfo\n"); return 1; }

    int ncpu = 0;
    for (const char *p = buf; p && *p; ) {
        const char *nl = strchr(p, '\n');
        if (strncmp(p, "processor", 9) == 0) {
            const char *c = p + 9;
            while (nl && c < nl && (*c == ' ' || *c == '\t' || *c == ':')) c++;
            if (c < (nl ? nl : p + strlen(p)) && isdigit((unsigned char)*c)) ncpu++;
        }
        p = nl ? nl + 1 : NULL;
    }

    char vendor[128], model[192], family[32], modeln[32], stepping[32], flags[512];
    lscpu_field(buf, "vendor_id", vendor, sizeof(vendor));
    lscpu_field(buf, "model name", model, sizeof(model));
    if (!model[0]) lscpu_field(buf, "Processor", model, sizeof(model));
    lscpu_field(buf, "cpu family", family, sizeof(family));
    lscpu_field(buf, "model", modeln, sizeof(modeln));
    lscpu_field(buf, "stepping", stepping, sizeof(stepping));
    lscpu_field(buf, "flags", flags, sizeof(flags));
    if (!flags[0]) lscpu_field(buf, "Features", flags, sizeof(flags));

    int is64 = (strstr(flags, "lm") != NULL);
    printf("Architecture:          %s\n", is64 ? "x86_64" : "i686");
    printf("CPU(s):                %d\n", ncpu > 0 ? ncpu : 1);
    if (vendor[0])   printf("Vendor ID:             %s\n", vendor);
    if (model[0])    printf("Model name:            %s\n", model);
    if (family[0])   printf("CPU family:            %s\n", family);
    if (modeln[0])   printf("Model:                 %s\n", modeln);
    if (stepping[0]) printf("Stepping:              %s\n", stepping);
    if (flags[0])    printf("Flags:                 %s\n", flags);
    struct utsname u;
    if (uname(&u) == 0) printf("Machine:               %s\n", u.machine);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------- mountpoint */

static const char mountpoint_usage[] =
    "usage: mountpoint [-q] DIR\n"
    "Exit 0 if DIR is a mount point, 1 if not, 32 on error.\n";

int cact_ub_mountpoint(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mountpoint_usage, sizeof(mountpoint_usage) - 1);
        return 0;
    }
    int quiet = 0, i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) { quiet = 1; continue; }
        break;
    }
    if (i >= argc) { fprintf(stderr, "mountpoint: missing operand\n"); return 32; }
    const char *dir = argv[i];

    struct stat st;
    if (stat(dir, &st) != 0) {
        if (!quiet) fprintf(stderr, "mountpoint: %s: no such file or directory\n", dir);
        return 32;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (!quiet) fprintf(stderr, "mountpoint: %s: not a directory\n", dir);
        return 32;
    }

    char norm[512];
    snprintf(norm, sizeof(norm), "%s", dir);
    size_t nl = strlen(norm);
    while (nl > 1 && norm[nl - 1] == '/') norm[--nl] = '\0';

    size_t len = 0;
    char *mnts = cact_ub_read_all("/proc/mounts", &len);
    if (!mnts) {
        if (!quiet) fprintf(stderr, "mountpoint: cannot read /proc/mounts\n");
        return 32;
    }
    int found = 0;
    for (char *p = mnts; p && *p; ) {
        char *nlp = strchr(p, '\n');
        if (nlp) *nlp = '\0';
        /* fields: device mountpoint fstype ... */
        char *sp = strchr(p, ' ');
        if (sp) {
            char *mp = sp + 1;
            while (*mp == ' ') mp++;
            char *e = mp;
            while (*e && *e != ' ') e++;
            *e = '\0';
            size_t ml = strlen(mp);
            while (ml > 1 && mp[ml - 1] == '/') mp[--ml] = '\0';
            if (strcmp(mp, norm) == 0) { found = 1; break; }
        }
        p = nlp ? nlp + 1 : NULL;
    }
    free(mnts);
    if (!quiet) printf("%s is %sa mountpoint\n", dir, found ? "" : "not ");
    return found ? 0 : 1;
}

/* -------------------------------------------------------------- truncate */

static const char truncate_usage[] =
    "usage: truncate [-c] -s SIZE FILE...\n"
    "  -s SIZE  new length (K/M/G suffixes); +N / -N adjust relative to the\n"
    "           current size.  -c does not create a missing file.\n";

static int truncate_one(const char *path, const char *spec, int no_create) {
    int relative = (spec[0] == '+' || spec[0] == '-');
    long val = cact_ub_parse_size(relative ? spec + 1 : spec);
    if (val < 0) val = -val;

    struct stat st;
    int exists = (stat(path, &st) == 0);
    if (!exists && no_create) return 0;    /* -c: do not create the file */

    long target;
    if (relative) {
        long cur = exists ? (long)st.st_size : 0;
        target = (spec[0] == '-') ? cur - val : cur + val;
    } else {
        target = val;
    }
    if (target < 0) target = 0;
    if (truncate(path, (off_t)target) != 0) {
        fprintf(stderr, "truncate: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int cact_ub_truncate(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, truncate_usage, sizeof(truncate_usage) - 1);
        return 0;
    }
    const char *spec = NULL;
    int no_create = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-c") == 0) no_create = 1;
        else if (strcmp(a, "-s") == 0 && i + 1 < argc) spec = argv[++i];
        else if (strncmp(a, "-s", 2) == 0 && a[2]) spec = a + 2;
        else { fprintf(stderr, "truncate: unknown option %s\n", a); return 1; }
    }
    if (!spec || i >= argc) { fprintf(stderr, "truncate: -s SIZE and a FILE are required\n"); return 1; }
    int ret = 0;
    for (; i < argc; i++) ret |= truncate_one(argv[i], spec, no_create);
    return ret;
}

/* ------------------------------------------------------------ pidof/killall */

static const char pidof_usage[] =
    "usage: pidof [-s] NAME...\n"
    "Print PIDs whose command name matches.  -s prints only the first.\n";

/* pidof prints all matches on one line, so collect them first. */
typedef struct { const char **names; int n; int count; pid_t pids[256]; } pidof_col;
static int pidof_collect(int pid, const char *comm, void *ud) {
    pidof_col *c = (pidof_col *)ud;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(comm, c->names[i]) == 0) {
            if (c->count < 256) c->pids[c->count++] = pid;
            break;
        }
    }
    return 0;
}

int cact_ub_pidof(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, pidof_usage, sizeof(pidof_usage) - 1);
        return 0;
    }
    int single = 0, i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) { single = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "pidof: unknown option %s\n", argv[i]); return 1; }
        break;
    }
    if (i >= argc) { fprintf(stderr, "pidof: missing program name\n"); return 1; }

    pidof_col c;
    c.names = (const char **)&argv[i];
    c.n = argc - i;
    c.count = 0;
    proc_each(pidof_collect, &c);
    if (c.count == 0) return 1;
    int out = single ? 1 : c.count;
    for (int k = 0; k < out; k++) printf("%s%d", k ? " " : "", c.pids[k]);
    printf("\n");
    return 0;
}

static int kill_sig_by_name(const char *s) {
    static const struct { const char *n; int v; } tab[] = {
        { "HUP",  SIGHUP  }, { "INT",  SIGINT  }, { "QUIT", SIGQUIT },
        { "KILL", SIGKILL }, { "TERM", SIGTERM }, { "STOP", SIGSTOP },
        { "CONT", SIGCONT }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
        { "CHLD", SIGCHLD }, { "SEGV", SIGSEGV }, { "FPE",  SIGFPE  },
        { "WINCH", SIGWINCH },
    };
    if (strncasecmp(s, "SIG", 3) == 0 && s[3]) s += 3;
    if (isdigit((unsigned char)s[0])) {
        /* Linux-style numbers mapped to the kernel's signal masks, so both
         * "TERM" and "15" mean the same thing. */
        switch (atoi(s)) {
            case 1:  return SIGHUP;
            case 2:  return SIGINT;
            case 3:  return SIGQUIT;
            case 9:  return SIGKILL;
            case 13: return SIGPIPE;
            case 14: return SIGALRM;
            case 15: return SIGTERM;
            case 17: return SIGCHLD;
            case 18: return SIGCONT;
            case 19: return SIGSTOP;
            default: return atoi(s);
        }
    }
    for (size_t k = 0; k < sizeof(tab) / sizeof(tab[0]); k++)
        if (strcasecmp(s, tab[k].n) == 0) return tab[k].v;
    return -1;
}

static const char killall_usage[] =
    "usage: killall [-s SIG|--signal SIG] [-q] NAME...\n"
    "Send a signal (default TERM) to every process whose command name matches.\n";

typedef struct { const char **names; int n; int sig; int quiet; int killed; int failed; } killall_ctx;

static int killall_cb(int pid, const char *comm, void *ud) {
    killall_ctx *c = (killall_ctx *)ud;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(comm, c->names[i]) == 0) {
            if (kill(pid, c->sig) == 0) c->killed++;
            else c->failed++;
            break;
        }
    }
    return 0;
}

int cact_ub_killall(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, killall_usage, sizeof(killall_usage) - 1);
        return 0;
    }
    const char *signame_opt = "TERM";
    int quiet = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-q") == 0) quiet = 1;
        else if (strcmp(a, "-s") == 0 && i + 1 < argc) signame_opt = argv[++i];
        else if (strcmp(a, "--signal") == 0 && i + 1 < argc) signame_opt = argv[++i];
        else if (strncmp(a, "-s", 2) == 0 && a[2]) signame_opt = a + 2;
        else { fprintf(stderr, "killall: unknown option %s\n", a); return 1; }
    }
    if (i >= argc) { fprintf(stderr, "killall: missing process name\n"); return 1; }
    int sig = kill_sig_by_name(signame_opt);
    if (sig < 0) { fprintf(stderr, "killall: unknown signal %s\n", signame_opt); return 1; }

    killall_ctx c;
    c.names = (const char **)&argv[i];
    c.n = argc - i;
    c.sig = sig;
    c.quiet = quiet;
    c.killed = 0;
    c.failed = 0;
    proc_each(killall_cb, &c);
    if (c.failed && !quiet) fprintf(stderr, "killall: %d process(es) could not be signalled\n", c.failed);
    if (c.killed == 0) {
        if (!quiet) fprintf(stderr, "killall: no process found\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ xargs */

static const char xargs_usage[] =
    "usage: xargs [-0] [-n N] [-I REPL] [-t] [-r] [CMD [ARG...]]\n"
    "Read items from stdin (whitespace-separated, or NUL with -0) and run CMD\n"
    "(default echo) with the items appended, at most N at a time.  -I runs CMD\n"
    "once per item, replacing REPL.  -r does not run CMD on empty input.\n";

/* Resolve a command name through PATH and exec it.  The kernel resolves exec
 * paths against the CWD/absolute only (no PATH search) and libc execvp is a
 * bare execve, so a shell-style PATH lookup has to be done here.  Never
 * returns on success; returns -1 if the program could not be executed. */
static int adm_exec(char *const argv[]) {
    const char *prog = argv[0];
    if (strchr(prog, '/')) {
        execve((char *)prog, argv, NULL);
        return -1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/usr/sbin:/bin:/sbin";
    char buf[1024];
    size_t pl = (size_t)strlen(prog);
    const char *p = path;
    while (p && *p) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : (size_t)strlen(p);
        if (dl > 0 && dl + 1 + pl + 1 < sizeof(buf)) {
            memcpy(buf, p, dl);
            buf[dl] = '/';
            memcpy(buf + dl + 1, prog, pl + 1);
            execve(buf, argv, NULL);
        }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

static int xargs_run(char **cmdv, int cmdlen, int trace) {
    if (trace) {
        fputs("+ ", stderr);
        for (int i = 0; i < cmdlen; i++) fputs(cmdv[i], stderr), fputc(' ', stderr);
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "xargs: fork failed\n"); return -1; }
    if (pid == 0) {
        adm_exec(cmdv);
        fprintf(stderr, "xargs: cannot execute %s\n", cmdv[0]);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 0;
}

int cact_ub_xargs(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, xargs_usage, sizeof(xargs_usage) - 1);
        return 0;
    }
    int nul = 0, trace = 0, run_empty = 1;
    long per = 500;
    const char *repl = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-0") == 0) nul = 1;
        else if (strcmp(a, "-t") == 0) trace = 1;
        else if (strcmp(a, "-r") == 0) run_empty = 0;
        else if (strcmp(a, "-n") == 0 && i + 1 < argc) per = atol(argv[++i]);
        else if (strncmp(a, "-n", 2) == 0 && a[2]) per = atol(a + 2);
        else if (strcmp(a, "-I") == 0 && i + 1 < argc) repl = argv[++i];
        else if (strncmp(a, "-I", 2) == 0 && a[2]) repl = a + 2;
        else { fprintf(stderr, "xargs: unknown option %s\n", a); return 1; }
    }
    if (per <= 0) per = 1;

    const char *cmd = (i < argc) ? argv[i] : "echo";
    int base_len = (i < argc) ? argc - i : 1;

    size_t len = 0;
    char *input = cact_ub_read_all(NULL, &len);
    if (!input) { fprintf(stderr, "xargs: cannot read stdin\n"); return 1; }

    char **items = (char **)malloc(64 * sizeof(char *));
    size_t nitems = 0, icap = 64;
    if (!items) { free(input); return 1; }
    {
        char *p = input, *end = input + len;
        while (p < end) {
            if (nul) {
                char *z = memchr(p, '\0', (size_t)(end - p));
                char *q = z ? z : end;
                if (q == p) { p = z ? z + 1 : end; continue; }
                if (z) *z = '\0';
                if (nitems == icap) { icap *= 2; char **ni = realloc(items, icap * sizeof(char *)); if (!ni) { free(items); free(input); return 1; } items = ni; }
                items[nitems++] = p;
                p = q;
            } else {
                while (p < end && isspace((unsigned char)*p)) p++;
                if (p >= end) break;
                char *s = p;
                while (p < end && !isspace((unsigned char)*p)) p++;
                if (p < end) *p++ = '\0';
                if (nitems == icap) { icap *= 2; char **ni = realloc(items, icap * sizeof(char *)); if (!ni) { free(items); free(input); return 1; } items = ni; }
                items[nitems++] = s;
            }
        }
    }

    int rc = 0;
    if (nitems == 0) {
        if (run_empty) {
            char **cmdv = (char **)malloc((size_t)(base_len + 1) * sizeof(char *));
            if (!cmdv) { free(items); free(input); return 1; }
            for (int k = 0; k < base_len; k++) cmdv[k] = (i < argc) ? argv[i + k] : (char *)cmd;
            cmdv[base_len] = NULL;
            rc = xargs_run(cmdv, base_len, trace);
            free(cmdv);
        }
        free(items); free(input);
        return rc;
    }

    if (repl) {
        for (size_t k = 0; k < nitems; k++) {
            char **cmdv = (char **)malloc((size_t)(base_len + 1) * sizeof(char *));
            if (!cmdv) { free(items); free(input); return 1; }
            for (int b = 0; b < base_len; b++) {
                char *arg = (i < argc) ? argv[i + b] : (char *)cmd;
                char *sub = (char *)malloc(strlen(arg) * 2 + strlen(items[k]) + 1);
                if (!sub) { free(cmdv); free(items); free(input); return 1; }
                char *d = sub;
                for (const char *s = arg; *s; ) {
                    if (strncmp(s, repl, strlen(repl)) == 0) {
                        size_t rl = strlen(items[k]);
                        memcpy(d, items[k], rl); d += rl; s += strlen(repl);
                    } else *d++ = *s++;
                }
                *d = '\0';
                cmdv[b] = sub;
            }
            cmdv[base_len] = NULL;
            int r = xargs_run(cmdv, base_len, trace);
            for (int b = 0; b < base_len; b++) free(cmdv[b]);
            free(cmdv);
            if (r > rc) rc = r;
        }
    } else {
        size_t k = 0;
        while (k < nitems) {
            size_t take = (size_t)per;
            if (take > nitems - k) take = nitems - k;
            int total = base_len + (int)take;
            char **cmdv = (char **)malloc(((size_t)total + 1) * sizeof(char *));
            if (!cmdv) { free(items); free(input); return 1; }
            for (int b = 0; b < base_len; b++) cmdv[b] = (i < argc) ? argv[i + b] : (char *)cmd;
            for (size_t t = 0; t < take; t++) cmdv[base_len + t] = items[k + t];
            cmdv[total] = NULL;
            int r = xargs_run(cmdv, total, trace);
            free(cmdv);
            if (r > rc) rc = r;
            k += take;
        }
    }

    free(items);
    free(input);
    return rc;
}

/* ---------------------------------------------------------------- timeout */

static const char timeout_usage[] =
    "usage: timeout [-s SIG] DURATION CMD [ARG...]\n"
    "Run CMD, and signal it (TERM by default) if it is still running after\n"
    "DURATION seconds (fractions allowed).  Exit status is 124 on timeout.\n";

/* Parse DURATION as seconds with an optional fraction, as milliseconds.
 * Integer only: the timeout path must not depend on userspace FP. */
static long timeout_parse_ms(const char *s) {
    long sec = 0, ms = 0;
    const char *p = s;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) sec = sec * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        int scale = 100;
        while (isdigit((unsigned char)*p) && scale > 0) {
            ms += (*p++ - '0') * scale;
            scale /= 10;
        }
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 's' && p[1] == '\0') p++;
    if (*p != '\0') return -1;
    return sec * 1000 + ms;
}

int cact_ub_timeout(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, timeout_usage, sizeof(timeout_usage) - 1);
        return 0;
    }
    int sig = SIGTERM;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-s") == 0 && i + 1 < argc) {
            int s = kill_sig_by_name(argv[++i]);
            if (s < 0) { fprintf(stderr, "timeout: unknown signal\n"); return 1; }
            sig = s;
        } else if (strcmp(a, "--signal") == 0 && i + 1 < argc) {
            int s = kill_sig_by_name(argv[++i]);
            if (s < 0) { fprintf(stderr, "timeout: unknown signal\n"); return 1; }
            sig = s;
        } else { fprintf(stderr, "timeout: unknown option %s\n", a); return 1; }
    }
    if (argc - i < 2) { fprintf(stderr, "timeout: DURATION and CMD are required\n"); return 1; }

    long dead_ms = timeout_parse_ms(argv[i]);
    if (dead_ms < 0) { fprintf(stderr, "timeout: bad duration %s\n", argv[i]); return 1; }
    int cmd_i = i + 1;

    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "timeout: fork failed\n"); return 1; }
    if (pid == 0) {
        adm_exec(&argv[cmd_i]);
        fprintf(stderr, "timeout: cannot execute %s\n", argv[cmd_i]);
        _exit(127);
    }

    int st = 0, timed_out = 0;
    for (long waited = 0;; waited += 20) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) break;
        if (waited >= dead_ms) {
            kill(pid, sig);
            usleep(100 * 1000);
            if (waitpid(pid, &st, WNOHANG) != pid) {
                kill(pid, SIGKILL);
                /* Reap without risking an unbounded block. */
                for (int k = 0; k < 100; k++) {
                    if (waitpid(pid, &st, WNOHANG) == pid) break;
                    usleep(20 * 1000);
                }
            }
            timed_out = 1;
            break;
        }
        usleep(20 * 1000);
    }
    if (timed_out) return 124;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 0;
}
