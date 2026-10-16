/*
 * ex_shell.c — the cactsole bridge tools.
 *
 *   cd  export  unset  exit  jobs  fg  bg  help
 *
 * These were cactsole builtins once.  They are separate ELFs now, and a child
 * process cannot change the shell's own state (its cwd, environment, job
 * table, or the shell itself), so each one sends a request over the control
 * pipe named by $CACT_CTL_FD; cactsole applies it as soon as the command
 * exits.  `help` needs no channel: it only reads PATH.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stat.h>
#include <fcntl.h>
#include <dirent.h>

#define CTL_REQ_MAX 1024

/* Send one control request, reporting uniformly when there is no channel
 * (the tool was run outside cactsole). */
static int ctl_send(const char *req, const char *tool) {
    if (cact_ub_shell_ctl(req) != 0) {
        fprintf(stderr, "%s: no shell control channel (run under cactsole)\n", tool);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- cd */

static const char cd_usage[] = "usage: cd [DIR]\n";

int cact_ub_cd(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cd_usage, sizeof(cd_usage) - 1);
        return 0;
    }
    const char *path = (argc >= 2) ? argv[1] : "";
    char expanded[512];
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/";

    if (path[0] == '\0') {
        path = home;
    } else if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
        path = expanded;
    }

    /* Resolve here, so a bad path is reported by cd and the shell only ever
     * receives an existing absolute directory. */
    char *abs = realpath(path, NULL);
    struct stat st;
    if (!abs || stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cd: %s: not a directory\n", (argc >= 2) ? argv[1] : path);
        free(abs);
        return 1;
    }
    char req[CTL_REQ_MAX];
    snprintf(req, sizeof(req), "cd\t%s", abs);
    free(abs);
    return ctl_send(req, "cd");
}

/* ------------------------------------------------------------ export/unset */

static const char export_usage[] =
    "usage: export [NAME[=VALUE]]...\n"
    "With no arguments print the environment; otherwise add or update variables.\n";

int cact_ub_export(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, export_usage, sizeof(export_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        if (environ)
            for (int k = 0; environ[k]; k++)
                printf("export %s\n", environ[k]);
        return 0;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char req[CTL_REQ_MAX];
        char *eq = strchr(argv[i], '=');
        if (eq) {
            snprintf(req, sizeof(req), "setenv\t%.*s\t%s",
                     (int)(eq - argv[i]), argv[i], eq + 1);
        } else {
            if (getenv(argv[i])) continue;   /* export NAME: only if unset */
            snprintf(req, sizeof(req), "setenv\t%s\t", argv[i]);
        }
        if (ctl_send(req, "export") != 0) ret = 1;
    }
    return ret;
}

static const char unset_usage[] = "usage: unset NAME...\n";

int cact_ub_unset(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, unset_usage, sizeof(unset_usage) - 1);
        return 0;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char req[CTL_REQ_MAX];
        snprintf(req, sizeof(req), "unsetenv\t%s", argv[i]);
        if (ctl_send(req, "unset") != 0) ret = 1;
    }
    return ret;
}

/* ------------------------------------------------------------- exit/jobs */

static const char exit_usage[] = "usage: exit [CODE]\n";

int cact_ub_exit(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, exit_usage, sizeof(exit_usage) - 1);
        return 0;
    }
    char req[64];
    snprintf(req, sizeof(req), "exit\t%d", (argc >= 2) ? atoi(argv[1]) : 0);
    return ctl_send(req, "exit");
}

static const char jobs_usage[] = "usage: jobs\n";

int cact_ub_jobs(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, jobs_usage, sizeof(jobs_usage) - 1);
        return 0;
    }
    return ctl_send("jobs", "jobs");
}

static const char fg_usage[] = "usage: fg [JOB]\n";

int cact_ub_fg(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, fg_usage, sizeof(fg_usage) - 1);
        return 0;
    }
    char req[64];
    snprintf(req, sizeof(req), "fg\t%d", (argc >= 2) ? atoi(argv[1]) : 1);
    return ctl_send(req, "fg");
}

static const char bg_usage[] = "usage: bg [JOB]\n";

int cact_ub_bg(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, bg_usage, sizeof(bg_usage) - 1);
        return 0;
    }
    char req[64];
    snprintf(req, sizeof(req), "bg\t%d", (argc >= 2) ? atoi(argv[1]) : 1);
    return ctl_send(req, "bg");
}

/* -------------------------------------------------------------------- help */

static const char help_usage[] =
    "usage: help [COMMAND]\n"
    "With no argument list the programs on PATH; with COMMAND run it with\n"
    "--help so the program documents itself.\n";

#define PROG_MAX      256
#define PROG_NAME_MAX 64

static char help_progs[PROG_MAX][PROG_NAME_MAX];
static int  help_prog_count;

static void prog_add(const char *name) {
    int len = (int)strlen(name);
    if (len == 0 || len >= PROG_NAME_MAX || help_prog_count >= PROG_MAX) return;
    for (int i = 0; i < help_prog_count; i++)
        if (strcmp(help_progs[i], name) == 0) return;
    memcpy(help_progs[help_prog_count], name, (size_t)len + 1);
    help_prog_count++;
}

static void prog_scan_dir(const char *dir, int dlen) {
    char path[300];
    if (dlen <= 0 || dlen >= (int)sizeof(path) - 2) return;
    memcpy(path, dir, (size_t)dlen);
    if (path[dlen - 1] != '/') path[dlen++] = '/';
    path[dlen] = '\0';

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;
    struct dirent buf[32];
    int n;
    while ((n = getdents(fd, buf, (unsigned int)sizeof(buf))) > 0) {
        int cnt = n / (int)sizeof(struct dirent);
        for (int i = 0; i < cnt; i++) {
            if (buf[i].d_name[0] == '.') continue;
            prog_add(buf[i].d_name);
        }
    }
    close(fd);
}

static int prog_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void prog_collect(void) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/usr/sbin";
    const char *seg = path;
    while (*seg) {
        const char *end = seg;
        while (*end && *end != ':') end++;
        prog_scan_dir(seg, (int)(end - seg));
        if (!*end) break;
        seg = end + 1;
    }
    if (help_prog_count > 1)
        qsort(help_progs, (size_t)help_prog_count, PROG_NAME_MAX, prog_cmp);
}

static void print_programs(void) {
    if (help_prog_count == 0) {
        printf("  (no directory on PATH could be read)\n");
        return;
    }
    int w = 0;
    for (int i = 0; i < help_prog_count; i++) {
        int l = (int)strlen(help_progs[i]);
        if (l > w) w = l;
    }
    w += 2;
    int cols = 76 / w;
    if (cols < 1) cols = 1;
    int rows = (help_prog_count + cols - 1) / cols;

    char line[160];
    for (int r = 0; r < rows; r++) {
        int pos = 0;
        line[pos++] = ' ';
        line[pos++] = ' ';
        for (int c = 0; c < cols; c++) {
            int idx = c * rows + r;
            if (idx >= help_prog_count) break;
            int l = (int)strlen(help_progs[idx]);
            memcpy(line + pos, help_progs[idx], (size_t)l);
            pos += l;
            if (idx + rows < help_prog_count)
                for (int s = l; s < w; s++) line[pos++] = ' ';
        }
        line[pos++] = '\n';
        write(STDOUT_FILENO, line, pos);
    }
}

/* PATH lookup, like cactsole's shell_path_find (execvp does no PATH search on
 * CactOS).  0 on success, -1 when not found. */
static int help_find(const char *cmd, char *out, int outsz) {
    if (strchr(cmd, '/')) {
        struct stat st;
        if (stat(cmd, &st) == 0) {
            int i = (int)strlen(cmd);
            if (i >= outsz) i = outsz - 1;
            memcpy(out, cmd, (size_t)i);
            out[i] = '\0';
            return 0;
        }
        return -1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/usr/sbin";
    struct stat st;
    int cmdlen = (int)strlen(cmd);
    const char *seg = path;
    while (*seg) {
        const char *end = seg;
        while (*end && *end != ':') end++;
        if (end - seg + 1 + cmdlen < outsz) {
            memcpy(out, seg, (size_t)(end - seg));
            out[end - seg] = '/';
            memcpy(out + (end - seg) + 1, cmd, (size_t)cmdlen + 1);
            if (stat(out, &st) == 0) return 0;
        }
        if (!*end) break;
        seg = end + 1;
    }
    return -1;
}

int cact_ub_help(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, help_usage, sizeof(help_usage) - 1);
        return 0;
    }

    if (argc >= 2) {
        char path[512];
        if (help_find(argv[1], path, sizeof(path)) != 0) {
            fprintf(stderr, "help: %s: not found on PATH\n", argv[1]);
            return 1;
        }
        char *hargv[3] = { argv[1], (char *)"--help", NULL };
        execve(path, hargv, environ);
        fprintf(stderr, "help: cannot run %s\n", path);
        return 1;
    }

    printf("  Programs on PATH (each documents itself with --help):\n\n");
    prog_collect();
    print_programs();
    printf("\n");
    return 0;
}
