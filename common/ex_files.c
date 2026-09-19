/*
 * builtins/files.c — операции над файлами и каталогами.
 *
 *   ls / mkdir / rmdir / tch / rm / cat / wrt / stat / mv / ln / readlink
 */


#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <syscall.h>

static const char ls_usage[] = "usage: ls [PATH]\n";

int cact_ub_ls(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, ls_usage, sizeof(ls_usage) - 1);
        return 0;
    }
    const char *path = (argc >= 2) ? argv[1] : ".";
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        write(STDERR_FILENO, "ls: cannot open ", 16);
        write(STDERR_FILENO, path, strlen(path));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    struct dirent buf[32];
    int n, any = 0;
    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int i, count = n / (int)sizeof(struct dirent);
        for (i = 0; i < count; i++) {
            if (buf[i].d_name[0] == '\0') continue;
            write(STDOUT_FILENO, buf[i].d_name, strlen(buf[i].d_name));
            write(STDOUT_FILENO, "\n", 1);
            any = 1;
        }
    }
    if (!any) write(STDOUT_FILENO, "(empty)\n", 8);
    close(fd);
    return 0;
}

static const char mkdir_usage[] = "usage: mkdir DIR...\n";

int cact_ub_mkdir(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mkdir_usage, sizeof(mkdir_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, mkdir_usage, sizeof(mkdir_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) != 0) {
            write(STDERR_FILENO, "mkdir: ", 7);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

static const char rmdir_usage[] = "usage: rmdir DIR...\n";

int cact_ub_rmdir(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, rmdir_usage, sizeof(rmdir_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, rmdir_usage, sizeof(rmdir_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        if (rmdir(argv[i]) != 0) {
            write(STDERR_FILENO, "rmdir: ", 7);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

static const char tch_usage[] = "usage: tch FILE...\n";

int cact_ub_tch(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tch_usage, sizeof(tch_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, tch_usage, sizeof(tch_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            write(STDERR_FILENO, "tch: ", 5);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        } else {
            close(fd);
        }
    }
    return ret;
}

static const char rm_usage[] = "usage: rm FILE...\n";

int cact_ub_rm(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, rm_usage, sizeof(rm_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, rm_usage, sizeof(rm_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        if (unlink(argv[i]) != 0) {
            write(STDERR_FILENO, "rm: ", 4);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

static const char cat_usage[] = "usage: cat [FILE...]\n";

int cact_ub_cat(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cat_usage, sizeof(cat_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        char buf[512];
        int n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, (size_t)n);
        return 0;
    }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            write(STDERR_FILENO, "cat: ", 5);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": not found\n", 12);
            ret = 1;
            continue;
        }
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, (size_t)n);
        close(fd);
    }
    return ret;
}

static const char wrt_usage[] = "usage: wrt FILE [text...]\n";

int cact_ub_wrt(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, wrt_usage, sizeof(wrt_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, wrt_usage, sizeof(wrt_usage) - 1); return 1; }
    int fd = open(argv[1], O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        write(STDERR_FILENO, "wrt: file not found: ", 21);
        write(STDERR_FILENO, argv[1], strlen(argv[1]));
        write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    if (argc >= 3) {
        int i;
        for (i = 2; i < argc; i++) {
            write(fd, argv[i], strlen(argv[i]));
            if (i < argc - 1) write(fd, " ", 1);
        }
        write(fd, "\n", 1);
    }
    close(fd);
    return 0;
}

static const char stat_usage[] = "usage: stat FILE...\n";

int cact_ub_stat(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, stat_usage, sizeof(stat_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, stat_usage, sizeof(stat_usage) - 1); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            write(STDERR_FILENO, "stat: ", 6);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": not found\n", 12);
            ret = 1;
            continue;
        }
        char buf[32];
        write(STDOUT_FILENO, "File:  ", 7);
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Size:  ", 7);
        itoa((int)st.st_size, buf); write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Inode: ", 7);
        itoa((int)st.st_ino, buf);  write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Mode:  ", 7);
        itoa((int)st.st_mode, buf); write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
        write(STDOUT_FILENO, "Type:  ", 7);
        if      (S_ISDIR(st.st_mode))  write(STDOUT_FILENO, "directory\n",    10);
        else if (S_ISREG(st.st_mode))  write(STDOUT_FILENO, "regular file\n",  13);
        else if (S_ISCHR(st.st_mode))  write(STDOUT_FILENO, "char device\n",   12);
        else if (S_ISBLK(st.st_mode))  write(STDOUT_FILENO, "block device\n",  13);
        else if (S_ISFIFO(st.st_mode)) write(STDOUT_FILENO, "fifo/pipe\n",     10);
        else                           write(STDOUT_FILENO, "other\n",          6);
    }
    return ret;
}

static const char mv_usage[] = "usage: mv SRC DST\n";

int cact_ub_mv(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, mv_usage, sizeof(mv_usage) - 1);
        return 0;
    }
    if (argc < 3) { write(STDERR_FILENO, mv_usage, sizeof(mv_usage) - 1); return 1; }
    if (rename(argv[1], argv[2]) != 0) {
        write(STDERR_FILENO, "mv: rename failed\n", 18);
        return 1;
    }
    return 0;
}

static const char ln_usage[] = "usage: ln [-s] SRC DST\n";

int cact_ub_ln(char **argv, int argc) {
    int soft = 0, base = 1;
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, ln_usage, sizeof(ln_usage) - 1);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "-s") == 0) { soft = 1; base = 2; }
    if (argc < base + 2) {
        write(STDERR_FILENO, ln_usage, sizeof(ln_usage) - 1);
        return 1;
    }
    int r = soft
        ? symlink(argv[base], argv[base + 1])
        : link(argv[base], argv[base + 1]);
    if (r != 0) {
        write(STDERR_FILENO, "ln: failed\n", 11);
        return 1;
    }
    return 0;
}

static const char readlink_usage[] = "usage: readlink PATH\n";

int cact_ub_readlink(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, readlink_usage, sizeof(readlink_usage) - 1);
        return 0;
    }
    if (argc < 2) { write(STDERR_FILENO, readlink_usage, sizeof(readlink_usage) - 1); return 1; }
    char buf[512];
    int n = (int)readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        write(STDERR_FILENO, "readlink: failed\n", 17);
        return 1;
    }
    buf[n] = '\0';
    write(STDOUT_FILENO, buf, (size_t)n);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
