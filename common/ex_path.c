/*
 * ex_path.c — path-name utilities.
 *
 *   basename  dirname  realpath  which  mktemp  tty
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>

/* ----------------------------------------------------------------- basename */

static const char basename_usage[] =
    "usage: basename NAME [SUFFIX]\n"
    "Print NAME with any leading directory removed and an optional SUFFIX stripped.\n";

int cact_ub_basename(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, basename_usage, sizeof(basename_usage) - 1);
        return 0;
    }
    if (argc < 2 || argc > 3) {
        write(STDERR_FILENO, basename_usage, sizeof(basename_usage) - 1);
        return 1;
    }
    const char *name = argv[1];
    const char *suffix = (argc == 3) ? argv[2] : NULL;

    if (!name[0]) { write(STDOUT_FILENO, "\n", 1); return 0; }

    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;

    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/') start = i + 1;

    size_t blen = n - start;
    if (blen == 0) {                     /* the name was all slashes */
        write(STDOUT_FILENO, "/\n", 2);
        return 0;
    }
    if (suffix && suffix[0]) {
        size_t sl = strlen(suffix);
        if (sl < blen && strncmp(name + start + blen - sl, suffix, sl) == 0)
            blen -= sl;
    }
    cact_ub_write_all(STDOUT_FILENO, name + start, blen);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* ------------------------------------------------------------------ dirname */

static const char dirname_usage[] =
    "usage: dirname NAME\n"
    "Print NAME with its last component removed.\n";

int cact_ub_dirname(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, dirname_usage, sizeof(dirname_usage) - 1);
        return 0;
    }
    if (argc != 2) {
        write(STDERR_FILENO, dirname_usage, sizeof(dirname_usage) - 1);
        return 1;
    }
    const char *name = argv[1];
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;

    size_t i = n;
    while (i > 0 && name[i - 1] != '/') i--;
    if (i == 0) {
        write(STDOUT_FILENO, ".\n", 2);
        return 0;
    }
    size_t end = i;
    while (end > 1 && name[end - 1] == '/') end--;
    cact_ub_write_all(STDOUT_FILENO, name, end);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* ----------------------------------------------------------------- realpath */

static const char realpath_usage[] =
    "usage: realpath PATH...\n"
    "Print the canonical absolute path of each PATH.\n";

int cact_ub_realpath(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, realpath_usage, sizeof(realpath_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        write(STDERR_FILENO, realpath_usage, sizeof(realpath_usage) - 1);
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char *r = realpath(argv[i], NULL);
        if (!r) {
            fprintf(stderr, "realpath: %s: cannot resolve\n", argv[i]);
            ret = 1;
            continue;
        }
        printf("%s\n", r);
        free(r);
    }
    return ret;
}

/* -------------------------------------------------------------------- which */

static const char which_usage[] =
    "usage: which [-a] NAME...\n"
    "  -a print every match in PATH, not only the first\n";

static int which_one(const char *name, int all) {
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) { printf("%s\n", name); return 0; }
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !path[0]) path = "/bin:/sbin";

    char buf[512];
    const char *p = path;
    int found = 0;
    while (*p) {
        const char *e = strchr(p, ':');
        size_t dlen = e ? (size_t)(e - p) : strlen(p);
        if (dlen == 0)
            snprintf(buf, sizeof(buf), "./%s", name);
        else
            snprintf(buf, sizeof(buf), "%.*s/%s", (int)dlen, p, name);
        if (access(buf, X_OK) == 0) {
            printf("%s\n", buf);
            found = 1;
            if (!all) return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return found ? 0 : 1;
}

int cact_ub_which(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, which_usage, sizeof(which_usage) - 1);
        return 0;
    }
    int all = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) { all = 1; i++; }
    if (i >= argc) {
        write(STDERR_FILENO, which_usage, sizeof(which_usage) - 1);
        return 1;
    }
    int ret = 0;
    for (; i < argc; i++)
        if (which_one(argv[i], all) != 0) ret = 1;
    return ret;
}

/* ------------------------------------------------------------------- mktemp */

static const char mktemp_usage[] =
    "usage: mktemp [-d] [TEMPLATE]\n"
    "TEMPLATE must end in XXXXXX (default /tmp/tmp.XXXXXX).\n"
    "  -d create a directory instead of a file\n";

int cact_ub_mktemp(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mktemp_usage, sizeof(mktemp_usage) - 1);
        return 0;
    }
    int as_dir = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) { as_dir = 1; i++; }
    const char *tmpl_arg = (i < argc) ? argv[i] : "/tmp/tmp.XXXXXX";

    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s", tmpl_arg);
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        fprintf(stderr, "mktemp: TEMPLATE must end in XXXXXX\n");
        return 1;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "mktemp: cannot create %s\n", tmpl);
        return 1;
    }
    close(fd);

    if (as_dir) {
        unlink(tmpl);
        if (mkdir(tmpl, 0700) != 0) {
            fprintf(stderr, "mktemp: cannot create directory %s\n", tmpl);
            return 1;
        }
    }
    printf("%s\n", tmpl);
    return 0;
}

/* ---------------------------------------------------------------------- tty */

static const char tty_usage[] = "usage: tty\n";

int cact_ub_tty(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tty_usage, sizeof(tty_usage) - 1);
        return 0;
    }
    if (isatty(STDIN_FILENO) != 1) {
        write(STDOUT_FILENO, "not a tty\n", 10);
        return 1;
    }
    char buf[512];
    ssize_t n = readlink("/proc/self/fd/0", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s\n", buf);
    } else {
        write(STDOUT_FILENO, "/dev/tty\n", 9);
    }
    return 0;
}
