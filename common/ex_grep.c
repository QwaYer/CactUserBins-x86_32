/*
 * ex_grep.c — grep: поиск текста в файлах/каталогах.
 *
 *   grep [-i] [-n] [-v] [-c] [-l] [-r] PATTERN [FILE...]
 *
 * Паттерн — простая подстрока (без регулярных выражений).  При нескольких
 * файлах (или -r) вывод предваряется именем файла; без файлов читается
 * stdin.  Возврат: 0 — найдено, 1 — не найдено, 2 — ошибка.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stat.h>
#include <dirent.h>
#include <stdint.h>

#define GREP_MAX_DEPTH 24

typedef struct {
    int icase;
    int lineno;
    int invert;
    int count;
    int list_only;
    int recurse;
    const char *pat;
    int patlen;
} grep_opt_t;

typedef struct {
    unsigned char *b;
    int cap;
    int len;
} gline_t;

static int g_low(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int g_match(const unsigned char *h, int hl,
                   const unsigned char *n, int nl, int icase) {
    if (nl == 0) return 1;
    if (!icase) return memmem(h, (size_t)hl, n, (size_t)nl) != NULL;
    if (nl > hl) return 0;
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (g_low(h[i + j]) != g_low(n[j])) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void g_write_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

static void g_write_num(int fd, unsigned long v) {
    char buf[24];
    int n = 0;
    if (v == 0) { write(fd, "0", 1); return; }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) write(fd, &buf[--n], 1);
}

static void g_print_line(const grep_opt_t *o, int out, const char *path,
                         int show_name, unsigned long ln,
                         const unsigned char *line, int len) {
    if (show_name) {
        g_write_str(out, path);
        write(out, ":", 1);
    }
    if (o->lineno) {
        g_write_num(out, ln);
        write(out, ":", 1);
    }
    if (len > 0) write(out, line, (size_t)len);
    write(out, "\n", 1);
}

/* Обработать один открытый fd. limit>0 — читать не больше limit байт
 * (обычные файлы: их размер из stat), limit==0 — читать до EOF (stdin,
 * псевдо-файлы). Возвращает 0 = есть совпадения, 1 = нет, 2 = ошибка. */
static int g_run_fd(int fd, const grep_opt_t *o, const char *path,
                    int show_name, unsigned long long limit) {
    gline_t L;
    L.b = (unsigned char *)malloc(128);
    if (!L.b) return 2;
    L.cap = 128;
    L.len = 0;

    unsigned long ln = 0, cnt = 0;
    int stop = 0, any = 0;
    int rc = 1;
    unsigned long long got = 0;

    unsigned char chunk[4096];
    for (;;) {
        unsigned long long want = sizeof(chunk);
        if (limit > 0 && limit - got < want) want = limit - got;
        ssize_t r = read(fd, chunk, (size_t)want);
        if (r < 0) { rc = 2; break; }
        if (r == 0) break;
        got += (unsigned long long)r;
        for (ssize_t i = 0; i < r; i++) {
            unsigned char c = chunk[i];
            if (c == '\n') {
                ln++;
                int m = g_match(L.b, L.len,
                                (const unsigned char *)o->pat, o->patlen,
                                o->icase);
                if (o->invert) m = !m;
                if (m) {
                    cnt++;
                    any = 1;
                    if (o->count || o->list_only) {
                        /* посчитано/список — печатаем в конце/сразу */
                        if (o->list_only && !stop) {
                            if (path && path[0]) {
                                g_write_str(STDOUT_FILENO, path);
                                write(STDOUT_FILENO, "\n", 1);
                            }
                            stop = 1;
                        }
                    } else {
                        g_print_line(o, STDOUT_FILENO, path, show_name,
                                     ln, L.b, L.len);
                    }
                }
                L.len = 0;
            } else {
                if (L.len + 1 >= L.cap) {
                    int ncap = L.cap * 2;
                    unsigned char *nb = (unsigned char *)realloc(L.b,
                                                                 (size_t)ncap);
                    if (!nb) {
                        free(L.b);
                        return 2;
                    }
                    L.b = nb;
                    L.cap = ncap;
                }
                L.b[L.len++] = c;
            }
        }
        if (limit > 0 && got >= limit) break;
        if (stop) break;
    }

    if (!stop && L.len > 0) {
        /* последняя строка без перевода каретки */
        ln++;
        int m = g_match(L.b, L.len,
                        (const unsigned char *)o->pat, o->patlen, o->icase);
        if (o->invert) m = !m;
        if (m) {
            cnt++;
            any = 1;
            if (o->count || o->list_only) {
                if (o->list_only && path && path[0]) {
                    g_write_str(STDOUT_FILENO, path);
                    write(STDOUT_FILENO, "\n", 1);
                }
            } else {
                g_print_line(o, STDOUT_FILENO, path, show_name,
                             ln, L.b, L.len);
            }
        }
    }
    free(L.b);

    if (rc == 2) return 2;

    if (o->count) {
        if (show_name) {
            g_write_str(STDOUT_FILENO, path);
            write(STDOUT_FILENO, ":", 1);
        }
        g_write_num(STDOUT_FILENO, cnt);
        write(STDOUT_FILENO, "\n", 1);
    }
    return any ? 0 : 1;
}

static int g_process_file(const char *path, const grep_opt_t *o,
                          int show_name) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "grep: %s: cannot open\n", path);
        return 2;
    }
    int rc;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size == 0) {
            rc = 1;          /* пустой файл — совпадений нет, не читаем */
        } else {
            /* читаем ровно st_size байт: если драйвер ФС не отдаёт EOF
             * на read() за концом файла, не упираемся в вечное ожидание */
            rc = g_run_fd(fd, o, path, show_name,
                          (unsigned long long)st.st_size);
        }
    } else {
        rc = g_run_fd(fd, o, path, show_name, 0);
    }
    close(fd);
    return rc;
}

static int g_process_dir(const char *path, const grep_opt_t *o, int depth);

static int g_process_path(const char *path, const grep_opt_t *o, int depth,
                          int show_name) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "grep: %s: not found\n", path);
        return 2;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!o->recurse) {
            fprintf(stderr, "grep: %s: is a directory (use -r)\n", path);
            return 2;
        }
        if (depth >= GREP_MAX_DEPTH) return 0;
        return g_process_dir(path, o, depth + 1);
    }
    if (!S_ISREG(st.st_mode)) return 0;
    return g_process_file(path, o, show_name);
}

static int g_process_dir(const char *path, const grep_opt_t *o, int depth) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "grep: %s: cannot read directory\n", path);
        return 2;
    }
    int any = 0;
    struct dirent de[8];
    for (;;) {
        int n = getdents(fd, de, sizeof(de));
        if (n <= 0) break;
        int entries = n / (int)sizeof(struct dirent);
        for (int i = 0; i < entries; i++) {
            const char *nm = de[i].d_name;
            if (!nm[0] || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, nm);
            int r = g_process_path(child, o, depth, 1);
            if (r == 0) any = 1;
            else if (r == 2) any = 2;
        }
    }
    close(fd);
    return any;
}

static void grep_usage(void) {
    fprintf(stderr,
            "usage: grep [-i] [-n] [-v] [-c] [-l] [-r] PATTERN [FILE...]\n"
            "  PATTERN is a plain substring (no regexes)\n"
            "  -i ignore case, -n line numbers, -v invert match\n"
            "  -c print count, -l list files with matches, -r recurse dirs\n");
}

int cact_ub_grep(char **argv, int argc) {
    grep_opt_t o;
    memset(&o, 0, sizeof(o));
    int pat_idx = -1;
    int file_count = 0;
    const char *files[64];
    int options_done = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!options_done && a[0] == '-' && a[1]) {
            if (strcmp(a, "--") == 0) {
                options_done = 1;
                continue;
            }
            if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
                grep_usage();
                return 0;
            }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                    case 'i': o.icase = 1; break;
                    case 'n': o.lineno = 1; break;
                    case 'v': o.invert = 1; break;
                    case 'c': o.count = 1; break;
                    case 'l': o.list_only = 1; break;
                    case 'r': o.recurse = 1; break;
                    default:
                        fprintf(stderr, "grep: unknown option -%c\n", *p);
                        grep_usage();
                        return 2;
                }
            }
            continue;
        }
        if (pat_idx < 0) {
            pat_idx = i;
        } else if (file_count < (int)(sizeof(files) / sizeof(files[0]))) {
            files[file_count++] = a;
        }
    }

    if (pat_idx < 0) {
        grep_usage();
        return 2;
    }

    o.pat = argv[pat_idx];
    o.patlen = (int)strlen(o.pat);

    int show_name = (file_count > 1) || o.recurse;

    if (file_count == 0) {
        int r = g_run_fd(STDIN_FILENO, &o, NULL, 0, 0);
        return r;
    }

    int found = 0, err = 0;
    for (int i = 0; i < file_count; i++) {
        int r = g_process_path(files[i], &o, 0, show_name);
        if (r == 0) found = 1;
        else if (r == 2) err = 1;
    }
    if (err) return 2;
    if (found) return 0;
    return 1;
}
