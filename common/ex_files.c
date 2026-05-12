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

int cact_ub_ls(char **argv, int argc) {
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

int cact_ub_mkdir(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: mkdir DIR...\n", 20); return 1; }
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

int cact_ub_rmdir(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: rmdir DIR...\n", 20); return 1; }
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

int cact_ub_tch(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: tch FILE...\n", 19); return 1; }
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

int cact_ub_rm(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: rm FILE...\n", 18); return 1; }
    int i, ret = 0;
    for (i = 1; i < argc; i++) {
        if (__syscall1(SYS_DELETE, (int)argv[i]) != 0) {
            write(STDERR_FILENO, "rm: ", 4);
            write(STDERR_FILENO, argv[i], strlen(argv[i]));
            write(STDERR_FILENO, ": failed\n", 9);
            ret = 1;
        }
    }
    return ret;
}

int cact_ub_cat(char **argv, int argc) {
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

int cact_ub_wrt(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: wrt FILE [text...]\n", 26); return 1; }
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

int cact_ub_stat(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: stat FILE...\n", 20); return 1; }
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

int cact_ub_mv(char **argv, int argc) {
    if (argc < 3) { write(STDERR_FILENO, "usage: mv SRC DST\n", 18); return 1; }
    if (rename(argv[1], argv[2]) != 0) {
        write(STDERR_FILENO, "mv: rename failed\n", 18);
        return 1;
    }
    return 0;
}

int cact_ub_ln(char **argv, int argc) {
    int soft = 0, base = 1;
    if (argc >= 2 && strcmp(argv[1], "-s") == 0) { soft = 1; base = 2; }
    if (argc < base + 2) {
        write(STDERR_FILENO, "usage: ln [-s] SRC DST\n", 23);
        return 1;
    }
    int r = soft
        ? __syscall2(SYS_SYMLINK, (int)argv[base], (int)argv[base + 1])
        : __syscall2(SYS_LINK,    (int)argv[base], (int)argv[base + 1]);
    if (r != 0) {
        write(STDERR_FILENO, "ln: failed\n", 11);
        return 1;
    }
    return 0;
}

int cact_ub_readlink(char **argv, int argc) {
    if (argc < 2) { write(STDERR_FILENO, "usage: readlink PATH\n", 21); return 1; }
    char buf[512];
    int n = __syscall3(SYS_READLINK, (int)argv[1], (int)buf, (int)(sizeof(buf) - 1));
    if (n < 0) {
        write(STDERR_FILENO, "readlink: failed\n", 17);
        return 1;
    }
    buf[n] = '\0';
    write(STDOUT_FILENO, buf, (size_t)n);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
