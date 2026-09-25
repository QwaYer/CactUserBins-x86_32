/*
 * Standalone misc utilities for CactUserBins (true, false, whoami, id,
 * chmod, chown, version).  exit/help stay in cactsole.
 */

#include "version.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stat.h>

#include "common/ex_util.h"

static const char true_usage[] = "usage: true\n";

int cact_ub_true(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, true_usage, sizeof(true_usage) - 1);
        return 0;
    }
    (void)argv;
    (void)argc;
    return 0;
}

static const char false_usage[] = "usage: false\n";

int cact_ub_false(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, false_usage, sizeof(false_usage) - 1);
        return 0;
    }
    (void)argv;
    (void)argc;
    return 1;
}

static const char whoami_usage[] = "usage: whoami\n";

int cact_ub_whoami(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, whoami_usage, sizeof(whoami_usage) - 1);
        return 0;
    }
    (void)argv;
    (void)argc;
    uid_t uid = getuid();
    char num[16];
    if (uid == 0) {
        write(STDOUT_FILENO, "root\n", 5);
    } else {
        write(STDOUT_FILENO, "user", 4);
        itoa((int)uid, num);
        write(STDOUT_FILENO, num, strlen(num));
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}

static const char id_usage[] =
    "usage: id [-u] [-g] [-G] [-n]\n"
    "  -u effective uid   -g effective gid   -G groups\n"
    "  -n print the numeric id instead of the name\n";

/* There is no user database yet: only uid 0 has a name. */
static void id_name(int id, char *out, size_t n) {
    if (id == 0) snprintf(out, n, "root");
    else snprintf(out, n, "%d", id);
}

int cact_ub_id(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, id_usage, sizeof(id_usage) - 1);
        return 0;
    }
    int f_u = 0, f_g = 0, f_G = 0, f_n = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            fprintf(stderr, "id: unexpected argument %s\n", a);
            return 1;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'u': f_u = 1; break;
                case 'g': f_g = 1; break;
                case 'G': f_G = 1; break;
                case 'n': f_n = 1; break;
                default:
                    fprintf(stderr, "id: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }

    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid(), egid = getegid();
    char name[32];

    if (f_u) {
        if (f_n) { id_name((int)euid, name, sizeof(name)); printf("%s\n", name); }
        else printf("%d\n", (int)euid);
        return 0;
    }
    if (f_g) {
        if (f_n) { id_name((int)egid, name, sizeof(name)); printf("%s\n", name); }
        else printf("%d\n", (int)egid);
        return 0;
    }
    if (f_G) {
        if (f_n) { id_name((int)egid, name, sizeof(name)); printf("%s\n", name); }
        else printf("%d\n", (int)egid);
        return 0;
    }

    char un[32], gn[32];
    id_name((int)uid, un, sizeof(un));
    id_name((int)gid, gn, sizeof(gn));
    printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)\n",
           (int)uid, un, (int)gid, gn, (int)gid, gn);
    if (euid != uid) printf("euid=%d\n", (int)euid);
    if (egid != gid) printf("egid=%d\n", (int)egid);
    return 0;
}

static const char chmod_usage[] =
    "usage: chmod [-R] MODE FILE...\n"
    "MODE is octal (755).  -R changes directories recursively.\n";

static int chmod_tree(const char *path, int mode);

static int chmod_entry(const char *full, const char *name, void *ud) {
    (void)name;
    return chmod_tree(full, *(int *)ud);
}

static int chmod_tree(const char *path, int mode) {
    if (chmod(path, mode) != 0) {
        fprintf(stderr, "chmod: %s: failed\n", path);
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        int m = mode;
        cact_ub_dir_foreach(path, chmod_entry, &m);
    }
    return 0;
}

int cact_ub_chmod(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, chmod_usage, sizeof(chmod_usage) - 1);
        return 0;
    }
    int recursive = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-R") == 0) { recursive = 1; i++; }
    if (argc - i < 2) {
        write(STDERR_FILENO, chmod_usage, sizeof(chmod_usage) - 1);
        return 1;
    }

    int mode = 0;
    const char *s = argv[i];
    while (*s >= '0' && *s <= '7') {
        mode = mode * 8 + (*s - '0');
        s++;
    }
    if (*s != '\0') {
        write(STDERR_FILENO, "chmod: invalid mode (use octal, e.g. 755)\n", 43);
        return 1;
    }

    int ret = 0;
    for (i = i + 1; i < argc; i++) {
        int rc = recursive ? chmod_tree(argv[i], mode)
                           : chmod(argv[i], mode);
        if (rc != 0) {
            if (!recursive) {
                write(STDERR_FILENO, "chmod: ", 7);
                write(STDERR_FILENO, argv[i], strlen(argv[i]));
                write(STDERR_FILENO, ": failed\n", 9);
            }
            ret = 1;
        }
    }
    return ret;
}

static const char chown_usage[] =
    "usage: chown [-R] OWNER[:GROUP] FILE...\n"
    "OWNER/GROUP are numeric.  -R changes directories recursively.\n";

typedef struct { int uid, gid; } chown_ctx_t;

static int chown_tree(const char *path, int uid, int gid);

static int chown_entry(const char *full, const char *name, void *ud) {
    (void)name;
    chown_ctx_t *c = (chown_ctx_t *)ud;
    return chown_tree(full, c->uid, c->gid);
}

static int chown_tree(const char *path, int uid, int gid) {
    if (chown(path, uid, gid) != 0) {
        fprintf(stderr, "chown: %s: failed\n", path);
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        chown_ctx_t c = { uid, gid };
        cact_ub_dir_foreach(path, chown_entry, &c);
    }
    return 0;
}

int cact_ub_chown(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, chown_usage, sizeof(chown_usage) - 1);
        return 0;
    }
    int recursive = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-R") == 0) { recursive = 1; i++; }
    if (argc - i < 2) {
        write(STDERR_FILENO, chown_usage, sizeof(chown_usage) - 1);
        return 1;
    }

    const char *spec = argv[i];
    int uid = 0, gid = -1;
    const char *p = spec;
    while (*p >= '0' && *p <= '9') {
        uid = uid * 10 + (*p - '0');
        p++;
    }
    if (*p == ':') {
        p++;
        gid = 0;
        while (*p >= '0' && *p <= '9') {
            gid = gid * 10 + (*p - '0');
            p++;
        }
    }
    if (*p != '\0') {
        write(STDERR_FILENO, "chown: invalid owner (use numeric uid[:gid])\n", 45);
        return 1;
    }

    int ret = 0;
    for (i = i + 1; i < argc; i++) {
        int rc = recursive ? chown_tree(argv[i], uid, gid)
                           : chown(argv[i], uid, gid);
        if (rc != 0) {
            if (!recursive) {
                write(STDERR_FILENO, "chown: ", 7);
                write(STDERR_FILENO, argv[i], strlen(argv[i]));
                write(STDERR_FILENO, ": failed\n", 9);
            }
            ret = 1;
        }
    }
    return ret;
}

static const char version_usage[] = "usage: version\n";

int cact_ub_version(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, version_usage, sizeof(version_usage) - 1);
        return 0;
    }
    (void)argv;
    (void)argc;
    static const char msg[] = "cactsole " CACTSOLE_VERSION "\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    return 0;
}
