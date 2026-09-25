/*
 * ex_text.c — text-processing utilities.
 *
 *   head  tail  wc  sort  uniq  cut  tr  tee  seq  yes  printf
 *
 * All of them read through cact_ub_read_all(), so a regular file is read up
 * to its st_size and a pipe/device is read to EOF.  Option syntax follows the
 * GNU tools closely enough for scripts: "-n N", "-nN", "--help".
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stat.h>
#include <stdint.h>

/* ------------------------------------------------------------------ shared */

/* Split buf in place at '\n', NUL-terminating each line.  The array holds
 * pointers into buf; one trailing empty line after a final '\n' is not
 * created.  Returns the number of lines (caller frees the array). */
static size_t split_lines(char *buf, size_t len, char ***outp) {
    char **arr = (char **)malloc(64 * sizeof(char *));
    if (!arr) { *outp = NULL; return 0; }
    size_t cap = 64, cnt = 0, i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        if (cnt == cap) {
            cap *= 2;
            char **na = (char **)realloc(arr, cap * sizeof(char *));
            if (!na) { free(arr); *outp = NULL; return 0; }
            arr = na;
        }
        arr[cnt++] = buf + start;
        if (i < len) buf[i++] = '\0';
    }
    *outp = arr;
    return cnt;
}

/* Concatenate the contents of argv[first..argc) (stdin when first >= argc)
 * into one buffer.  Returns NULL only if nothing could be read at all. */
static char *slurp_args(char **argv, int first, int argc, size_t *out_len,
                        int *err) {
    char *acc = NULL;
    size_t accn = 0;
    if (first >= argc) {
        acc = cact_ub_read_all(NULL, &accn);
        if (!acc) *err = 1;
        *out_len = accn;
        return acc;
    }
    for (int i = first; i < argc; i++) {
        size_t n = 0;
        char *b = cact_ub_read_all(argv[i], &n);
        if (!b) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            *err = 1;
            continue;
        }
        char *na = (char *)realloc(acc, accn + n + 1);
        if (!na) { free(b); free(acc); *err = 1; *out_len = 0; return NULL; }
        acc = na;
        memcpy(acc + accn, b, n);
        accn += n;
        acc[accn] = '\0';
        free(b);
    }
    *out_len = accn;
    return acc;
}

/* -------------------------------------------------------------------- head */

static const char head_usage[] =
    "usage: head [-n N] [-c N] [FILE...]\n"
    "Print the first 10 lines (or N lines / N bytes) of each FILE.\n";

static void head_lines(const char *buf, size_t len, long n) {
    size_t i = 0;
    long cnt = 0;
    while (i < len && cnt < n) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        if (i < len) i++;
        cact_ub_write_all(STDOUT_FILENO, buf + start, i - start);
        cnt++;
    }
}

static void head_bytes(const char *buf, size_t len, long n) {
    if (n <= 0) return;
    size_t take = (size_t)n < len ? (size_t)n : len;
    cact_ub_write_all(STDOUT_FILENO, buf, take);
}

int cact_ub_head(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, head_usage, sizeof(head_usage) - 1);
        return 0;
    }
    int by_bytes = 0;
    long count = 10;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == 'n' && (a[2] == '\0' || isdigit(a[2]) || a[2] == '-')) {
            if (a[2]) count = atol(a + 2);
            else if (i + 1 < argc) count = atol(argv[++i]);
            else { fprintf(stderr, "head: option -n needs a value\n"); return 1; }
            by_bytes = 0;
        } else if (a[1] == 'c' && (a[2] == '\0' || isdigit(a[2]) || a[2] == '-')) {
            if (a[2]) count = atol(a + 2);
            else if (i + 1 < argc) count = atol(argv[++i]);
            else { fprintf(stderr, "head: option -c needs a value\n"); return 1; }
            by_bytes = 1;
        } else if (isdigit(a[1])) {          /* head -20 */
            count = atol(a + 1);
            by_bytes = 0;
        } else {
            fprintf(stderr, "head: unknown option %s\n", a);
            return 1;
        }
    }

    int nfiles = argc - i;
    int ret = 0, firstfile = 1;
    if (nfiles == 0) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        if (by_bytes) head_bytes(buf, len, count);
        else head_lines(buf, len, count);
        free(buf);
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "head: %s: cannot open\n", argv[i]); ret = 1; continue; }
        if (nfiles > 1) {
            if (!firstfile) write(STDOUT_FILENO, "\n", 1);
            printf("==> %s <==\n", argv[i]);
        }
        firstfile = 0;
        if (by_bytes) head_bytes(buf, len, count);
        else head_lines(buf, len, count);
        free(buf);
    }
    return ret;
}

/* -------------------------------------------------------------------- tail */

static const char tail_usage[] =
    "usage: tail [-n N] [-c N] [-f] [FILE...]\n"
    "Print the last 10 lines (or N lines / N bytes) of each FILE.\n"
    "  -n N  last N lines (default 10)   -c N  last N bytes\n"
    "  -f    keep the file open and print data appended to it\n";

static void tail_lines(const char *buf, size_t len, long n) {
    if (n <= 0 || len == 0) return;
    if ((size_t)n > len) n = (long)len;
    size_t *ring = (size_t *)malloc(sizeof(size_t) * (size_t)n);
    if (!ring) return;
    size_t cap = (size_t)n;
    size_t count = 1;                 /* line 0 starts at 0 */
    ring[0] = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n') {
            ring[count % cap] = i + 1;
            count++;
        }
    }
    size_t show = count < cap ? count : cap;
    size_t first = count - show;
    for (size_t j = first; j < count; j++) {
        size_t s = ring[j % cap];
        size_t e = (j + 1 < count) ? ring[(j + 1) % cap] : len;
        cact_ub_write_all(STDOUT_FILENO, buf + s, e - s);
    }
    free(ring);
}

static void tail_bytes(const char *buf, size_t len, long n) {
    if (n <= 0) return;
    size_t take = (size_t)n < len ? (size_t)n : len;
    cact_ub_write_all(STDOUT_FILENO, buf + len - take, take);
}

/* -f: poll the file size and print whatever grew since the last look.  This
 * needs no file-notification facility, only stat()/read(); a truncated or
 * rotated file restarts from offset 0. */
static int tail_follow(const char *path) {
    long long pos = 0;
    struct stat st;
    if (stat(path, &st) == 0) pos = (long long)st.st_size;

    for (;;) {
        usleep(500000);
        if (stat(path, &st) != 0) continue;          /* rotation gap */
        long long size = (long long)st.st_size;
        if (size < pos) pos = 0;                     /* truncated */
        if (size <= pos) continue;

        int fd = open(path, O_RDONLY, 0);
        if (fd < 0) continue;
        if (lseek(fd, (off_t)pos, SEEK_SET) < 0) { close(fd); continue; }

        char buf[4096];
        long long want = size - pos;
        while (want > 0) {
            int chunk = want > (long long)sizeof(buf) ? (int)sizeof(buf) : (int)want;
            ssize_t r = read(fd, buf, chunk);
            if (r <= 0) break;
            cact_ub_write_all(STDOUT_FILENO, buf, (size_t)r);
            pos += r;
            want -= r;
        }
        close(fd);
    }
}

int cact_ub_tail(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tail_usage, sizeof(tail_usage) - 1);
        return 0;
    }
    int by_bytes = 0;
    long count = 10;
    int follow = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == 'n' && (a[2] == '\0' || isdigit(a[2]) || a[2] == '-')) {
            if (a[2]) count = atol(a + 2);
            else if (i + 1 < argc) count = atol(argv[++i]);
            else { fprintf(stderr, "tail: option -n needs a value\n"); return 1; }
            by_bytes = 0;
        } else if (a[1] == 'c' && (a[2] == '\0' || isdigit(a[2]) || a[2] == '-')) {
            if (a[2]) count = atol(a + 2);
            else if (i + 1 < argc) count = atol(argv[++i]);
            else { fprintf(stderr, "tail: option -c needs a value\n"); return 1; }
            by_bytes = 1;
        } else if (isdigit(a[1])) {
            count = atol(a + 1);
            by_bytes = 0;
        } else if (strcmp(a, "-f") == 0) {
            follow = 1;
        } else {
            fprintf(stderr, "tail: unknown option %s\n", a);
            return 1;
        }
    }

    int nfiles = argc - i;
    int ret = 0, firstfile = 1;
    if (follow) {
        if (nfiles != 1) {
            fprintf(stderr, "tail: -f needs exactly one FILE\n");
            return 1;
        }
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) {
            fprintf(stderr, "tail: %s: cannot open\n", argv[i]);
            return 1;
        }
        if (by_bytes) tail_bytes(buf, len, count);
        else tail_lines(buf, len, count);
        free(buf);
        return tail_follow(argv[i]);
    }
    if (nfiles == 0) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        if (by_bytes) tail_bytes(buf, len, count);
        else tail_lines(buf, len, count);
        free(buf);
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "tail: %s: cannot open\n", argv[i]); ret = 1; continue; }
        if (nfiles > 1) {
            if (!firstfile) write(STDOUT_FILENO, "\n", 1);
            printf("==> %s <==\n", argv[i]);
        }
        firstfile = 0;
        if (by_bytes) tail_bytes(buf, len, count);
        else tail_lines(buf, len, count);
        free(buf);
    }
    return ret;
}

/* ---------------------------------------------------------------------- wc */

static const char wc_usage[] =
    "usage: wc [-c] [-l] [-w] [-m] [FILE...]\n"
    "Count newlines, words and bytes for each FILE (default: lines words bytes).\n"
    "  -c bytes   -l lines   -w words   -m characters\n";

static void wc_count(const char *buf, size_t len, unsigned long long *lines,
                     unsigned long long *words, unsigned long long *bytes) {
    unsigned long long l = 0, w = 0;
    int in_word = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') l++;
        if (isspace((unsigned char)buf[i])) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            w++;
        }
    }
    *lines = l;
    *words = w;
    *bytes = (unsigned long long)len;
}

int cact_ub_wc(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, wc_usage, sizeof(wc_usage) - 1);
        return 0;
    }
    int f_c = 0, f_l = 0, f_w = 0, f_m = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--help") == 0) {
            write(STDOUT_FILENO, wc_usage, sizeof(wc_usage) - 1);
            return 0;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'c': f_c = 1; break;
                case 'l': f_l = 1; break;
                case 'w': f_w = 1; break;
                case 'm': f_m = 1; break;
                default:
                    fprintf(stderr, "wc: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }
    if (!f_c && !f_l && !f_w && !f_m) { f_l = f_w = f_c = 1; }

    unsigned long long tl = 0, tw = 0, tb = 0;
    int nfiles = argc - i, ret = 0;
    if (nfiles == 0) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        unsigned long long l, w, b;
        wc_count(buf, len, &l, &w, &b);
        free(buf);
        if (f_l) printf("%8llu", l);
        if (f_w) printf("%8llu", w);
        if (f_c || f_m) printf("%8llu", b);
        printf("\n");
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "wc: %s: cannot open\n", argv[i]); ret = 1; continue; }
        unsigned long long l, w, b;
        wc_count(buf, len, &l, &w, &b);
        free(buf);
        tl += l; tw += w; tb += b;
        if (f_l) printf("%8llu", l);
        if (f_w) printf("%8llu", w);
        if (f_c || f_m) printf("%8llu", b);
        printf(" %s\n", argv[i]);
    }
    if (nfiles > 1) {
        if (f_l) printf("%8llu", tl);
        if (f_w) printf("%8llu", tw);
        if (f_c || f_m) printf("%8llu", tb);
        printf(" total\n");
    }
    return ret;
}

/* -------------------------------------------------------------------- sort */

static int sort_numeric, sort_fold, sort_reverse;

static int sort_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++; b++;
    }
    return (*a != '\0') - (*b != '\0');
}

/* qsort comparator: the elements are char* (pointers to lines). */
static int sort_cmp(const void *pa, const void *pb) {
    const char *x = *(const char * const *)pa;
    const char *y = *(const char * const *)pb;
    int r;
    if (sort_numeric) {
        double dx = strtod(x, NULL), dy = strtod(y, NULL);
        r = (dx < dy) ? -1 : (dx > dy) ? 1 : 0;
    } else if (sort_fold) {
        r = sort_casecmp(x, y);
    } else {
        r = strcmp(x, y);
    }
    return sort_reverse ? -r : r;
}

static const char sort_usage[] =
    "usage: sort [-n] [-r] [-u] [-f] [FILE...]\n"
    "  -n numeric  -r reverse  -u unique  -f ignore case\n";

int cact_ub_sort(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, sort_usage, sizeof(sort_usage) - 1);
        return 0;
    }
    int uniq = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'n': sort_numeric = 1; break;
                case 'r': sort_reverse = 1; break;
                case 'u': uniq = 1; break;
                case 'f': sort_fold = 1; break;
                default:
                    fprintf(stderr, "sort: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }

    int err = 0;
    size_t len = 0;
    char *buf = slurp_args(argv, i, argc, &len, &err);
    if (!buf) return err ? 1 : 0;

    char **lines = NULL;
    size_t n = split_lines(buf, len, &lines);
    if (n > 0) qsort(lines, n, sizeof(char *), sort_cmp);

    for (size_t k = 0; k < n; k++) {
        if (uniq && k > 0 && sort_cmp(&lines[k - 1], &lines[k]) == 0) continue;
        cact_ub_write_all(STDOUT_FILENO, lines[k], strlen(lines[k]));
        write(STDOUT_FILENO, "\n", 1);
    }
    free(lines);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------- uniq */

static const char uniq_usage[] =
    "usage: uniq [-c] [-d] [-u] [INPUT [OUTPUT]]\n"
    "  -c prefix lines with a count  -d only duplicated  -u only unique\n";

int cact_ub_uniq(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, uniq_usage, sizeof(uniq_usage) - 1);
        return 0;
    }
    int show_count = 0, only_dup = 0, only_uniq = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'c': show_count = 1; break;
                case 'd': only_dup = 1; break;
                case 'u': only_uniq = 1; break;
                default:
                    fprintf(stderr, "uniq: unknown option -%c\n", *p);
                    return 1;
            }
        }
    }

    const char *inpath = (i < argc) ? argv[i++] : NULL;
    const char *outpath = (i < argc) ? argv[i++] : NULL;

    size_t len = 0;
    char *buf = cact_ub_read_all(inpath, &len);
    if (!buf) { fprintf(stderr, "uniq: %s: cannot open\n", inpath ? inpath : "(stdin)"); return 1; }

    FILE *out = stdout;
    if (outpath) {
        out = fopen(outpath, "w");
        if (!out) { fprintf(stderr, "uniq: %s: cannot create\n", outpath); free(buf); return 1; }
    }

    char **lines = NULL;
    size_t n = split_lines(buf, len, &lines);
    int ret = 0;
    for (size_t k = 0; k < n; ) {
        size_t j = k + 1;
        while (j < n && strcmp(lines[j], lines[k]) == 0) j++;
        unsigned long count = (unsigned long)(j - k);
        int emit = 1;
        if (only_dup && count < 2) emit = 0;
        if (only_uniq && count > 1) emit = 0;
        if (only_dup && only_uniq) emit = 0;
        if (emit) {
            if (show_count) fprintf(out, "%7lu %s\n", count, lines[k]);
            else fprintf(out, "%s\n", lines[k]);
        }
        k = j;
    }
    if (out != stdout) fclose(out);
    free(lines);
    free(buf);
    return ret;
}

/* --------------------------------------------------------------------- cut */

typedef struct { int lo, hi; } cut_range_t;

static int cut_list_parse(const char *s, cut_range_t *rs, int max) {
    int n = 0;
    while (*s && n < max) {
        if (!isdigit((unsigned char)*s)) return -1;
        int lo = atoi(s);
        while (isdigit((unsigned char)*s)) s++;
        int hi = lo;
        if (*s == '-') {
            s++;
            if (isdigit((unsigned char)*s)) { hi = atoi(s); while (isdigit((unsigned char)*s)) s++; }
            else hi = 1 << 30;                 /* "3-" means 3 to end */
        }
        if (hi < lo) return -1;
        rs[n].lo = lo; rs[n].hi = hi; n++;
        if (*s == ',' || *s == ' ') { s++; continue; }
        break;
    }
    /* a trailing comma is harmless */
    return n;
}

static int cut_selected(const cut_range_t *rs, int n, int idx) {
    for (int i = 0; i < n; i++)
        if (idx >= rs[i].lo && idx <= rs[i].hi) return 1;
    return 0;
}

static const char cut_usage[] =
    "usage: cut -d DELIM -f LIST [-s] [FILE...]\n"
    "       cut -c LIST [FILE...]\n"
    "  -d char  field delimiter   -f LIST  fields (1,3-5)\n"
    "  -c LIST  characters        -s       skip lines without the delimiter\n";

int cact_ub_cut(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cut_usage, sizeof(cut_usage) - 1);
        return 0;
    }
    char delim = '\t';
    const char *field_list = NULL, *char_list = NULL;
    int suppress = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] == 'd') {
            if (a[2]) delim = a[2];
            else if (i + 1 < argc) delim = argv[++i][0];
            else { fprintf(stderr, "cut: -d needs a delimiter\n"); return 1; }
        } else if (a[1] == 'f') {
            if (a[2]) field_list = a + 2;
            else if (i + 1 < argc) field_list = argv[++i];
            else { fprintf(stderr, "cut: -f needs a list\n"); return 1; }
        } else if (a[1] == 'c') {
            if (a[2]) char_list = a + 2;
            else if (i + 1 < argc) char_list = argv[++i];
            else { fprintf(stderr, "cut: -c needs a list\n"); return 1; }
        } else if (a[1] == 's') {
            suppress = 1;
        } else {
            fprintf(stderr, "cut: unknown option %s\n", a);
            return 1;
        }
    }
    if (!field_list && !char_list) {
        fprintf(stderr, "cut: specify -f or -c\n");
        return 1;
    }
    if (field_list && char_list) {
        fprintf(stderr, "cut: -f and -c are mutually exclusive\n");
        return 1;
    }

    cut_range_t rs[64];
    int nr = cut_list_parse(field_list ? field_list : char_list, rs, 64);
    if (nr <= 0) { fprintf(stderr, "cut: bad list\n"); return 1; }

    int err = 0;
    size_t len = 0;
    char *buf = slurp_args(argv, i, argc, &len, &err);
    if (!buf) return err ? 1 : 0;
    char **lines = NULL;
    size_t n = split_lines(buf, len, &lines);

    for (size_t k = 0; k < n; k++) {
        const char *line = lines[k];
        if (char_list) {
            int clen = (int)strlen(line);
            for (int c = 0; c < clen; c++)
                if (cut_selected(rs, nr, c + 1))
                    write(STDOUT_FILENO, &line[c], 1);
            write(STDOUT_FILENO, "\n", 1);
            continue;
        }
        if (!strchr(line, delim)) {
            if (!suppress) { cact_ub_write_all(STDOUT_FILENO, line, strlen(line)); write(STDOUT_FILENO, "\n", 1); }
            continue;
        }
        int field = 0, first = 1;
        const char *p = line;
        for (;;) {
            const char *e = strchr(p, delim);
            field++;
            if (cut_selected(rs, nr, field)) {
                if (!first) write(STDOUT_FILENO, &delim, 1);
                size_t flen = e ? (size_t)(e - p) : strlen(p);
                cact_ub_write_all(STDOUT_FILENO, p, flen);
                first = 0;
            }
            if (!e) break;
            p = e + 1;
        }
        write(STDOUT_FILENO, "\n", 1);
    }
    free(lines);
    free(buf);
    return 0;
}

/* ---------------------------------------------------------------------- tr */

/* Expand a SET (ranges a-z, escapes) into bytes; returns the length or -1. */
static int tr_expand(const char *s, unsigned char *out, int cap) {
    int n = 0;
    while (*s) {
        unsigned char c;
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n': c = '\n'; s++; break;
                case 't': c = '\t'; s++; break;
                case 'r': c = '\r'; s++; break;
                case 'a': c = '\a'; s++; break;
                case 'b': c = '\b'; s++; break;
                case 'f': c = '\f'; s++; break;
                case 'v': c = '\v'; s++; break;
                case '\\': c = '\\'; s++; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    int v = 0, k = 0;
                    while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; k++; }
                    c = (unsigned char)v;
                    break;
                }
                case '\0': c = '\\'; break;
                default: c = (unsigned char)*s; s++; break;
            }
        } else {
            c = (unsigned char)*s;
            if (s[0] != '\0' && s[1] == '-' && s[2] != '\0') {
                unsigned char hi = (unsigned char)s[2];
                for (unsigned char x = c; x <= hi && n < cap; x++) out[n++] = x;
                s += 3;
                continue;
            }
            s++;
        }
        if (n < cap) out[n++] = c;
    }
    return n;
}

static const char tr_usage[] =
    "usage: tr [-d] [-s] SET1 [SET2]\n"
    "Translate characters from stdin to stdout.\n"
    "  -d delete SET1   -s squeeze repeats in SET2   SETs support a-z, \\n, \\t\n";

int cact_ub_tr(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tr_usage, sizeof(tr_usage) - 1);
        return 0;
    }
    int del = 0, squeeze = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'd') del = 1;
            else if (*p == 's') squeeze = 1;
            else { fprintf(stderr, "tr: unknown option -%c\n", *p); return 1; }
        }
    }
    if (i >= argc) { fprintf(stderr, "tr: missing SET1\n"); return 1; }
    const char *set1 = argv[i++];
    const char *set2 = (i < argc) ? argv[i++] : NULL;
    if (!del && !set2) { fprintf(stderr, "tr: SET2 is required unless -d is used\n"); return 1; }

    unsigned char map1[256], map2[256];
    int n1 = tr_expand(set1, map1, 256);
    int n2 = set2 ? tr_expand(set2, map2, 256) : 0;
    if (n1 < 0 || n2 < 0) { fprintf(stderr, "tr: bad SET\n"); return 1; }

    int in1[256];
    memset(in1, 0, sizeof(in1));
    for (int k = 0; k < n1; k++) in1[map1[k]] = 1;

    /* in_set2 marks bytes that -s squeezes (SET2, or SET1 with -d). */
    int sq[256];
    memset(sq, 0, sizeof(sq));
    if (squeeze) {
        if (del || !n2) for (int k = 0; k < n1; k++) sq[map1[k]] = 1;
        else             for (int k = 0; k < n2; k++) sq[map2[k]] = 1;
    }

    size_t len = 0;
    char *buf = cact_ub_read_all(NULL, &len);
    if (!buf) return 1;

    int have_last = 0;
    unsigned char last = 0;
    for (size_t k = 0; k < len; k++) {
        unsigned char c = (unsigned char)buf[k];
        if (in1[c]) {
            if (del) continue;
            int idx = -1;
            for (int m = 0; m < n1; m++) if (map1[m] == c) { idx = m; break; }
            if (n2 > 0 && idx >= 0) c = map2[idx < n2 ? idx : n2 - 1];
        }
        if (squeeze && have_last && c == last && sq[c]) continue;
        write(STDOUT_FILENO, &c, 1);
        have_last = 1;
        last = c;
    }
    free(buf);
    return 0;
}

/* --------------------------------------------------------------------- tee */

static const char tee_usage[] = "usage: tee [-a] [FILE...]\n";

int cact_ub_tee(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tee_usage, sizeof(tee_usage) - 1);
        return 0;
    }
    int append = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) { append = 1; i++; }
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);

    int nf = argc - i;
    int *fds = (int *)malloc(sizeof(int) * (nf > 0 ? nf : 1));
    if (!fds) return 1;
    int ret = 0, opened = 0;
    for (int k = 0; k < nf; k++) {
        int fd = open(argv[i + k], flags, 0644);
        if (fd < 0) { fprintf(stderr, "tee: %s: cannot open\n", argv[i + k]); ret = 1; continue; }
        fds[opened++] = fd;
    }

    char chunk[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (r < 0) { ret = 1; break; }
        if (r == 0) break;
        cact_ub_write_all(STDOUT_FILENO, chunk, (size_t)r);
        for (int k = 0; k < opened; k++)
            if (cact_ub_write_all(fds[k], chunk, (size_t)r) != 0) ret = 1;
    }
    for (int k = 0; k < opened; k++) close(fds[k]);
    free(fds);
    return ret;
}

/* --------------------------------------------------------------------- seq */

static const char seq_usage[] =
    "usage: seq [FIRST [STEP]] LAST\n"
    "Print numbers from FIRST to LAST (inclusive) with STEP.\n";

int cact_ub_seq(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, seq_usage, sizeof(seq_usage) - 1);
        return 0;
    }
    if (argc < 2 || argc > 4) {
        write(STDERR_FILENO, seq_usage, sizeof(seq_usage) - 1);
        return 1;
    }
    double a[3];
    int n = argc - 1;
    int decimals = 0;
    for (int k = 0; k < n; k++) {
        char *end = NULL;
        a[k] = strtod(argv[k + 1], &end);
        if (end == argv[k + 1] || (end && *end != '\0')) {
            fprintf(stderr, "seq: invalid number: %s\n", argv[k + 1]);
            return 1;
        }
        const char *dot = strchr(argv[k + 1], '.');
        if (dot) {
            int d = (int)strlen(dot + 1);
            if (d > decimals) decimals = d;
        }
    }
    double first, step, last;
    if (n == 1) { first = 1.0; step = 1.0; last = a[0]; }
    else if (n == 2) { first = a[0]; step = 1.0; last = a[1]; }
    else { first = a[0]; step = a[1]; last = a[2]; }

    if (step == 0.0) { fprintf(stderr, "seq: zero step\n"); return 1; }
    if (decimals > 6) decimals = 6;

    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%df", decimals);

    if ((step > 0 && first > last) || (step < 0 && first < last)) return 0;

    /* Count the steps first so the loop bound is not defeated by rounding. */
    long long count = (long long)((last - first) / step + 1e-9);
    for (long long k = 0; k <= count; k++) {
        double v = first + step * (double)k;
        printf(fmt, v);
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}

/* --------------------------------------------------------------------- yes */

static const char yes_usage[] = "usage: yes [STRING...]\n";

int cact_ub_yes(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, yes_usage, sizeof(yes_usage) - 1);
        return 0;
    }
    char line[1024];
    int n = 0;
    if (argc < 2) {
        memcpy(line, "y", 1); n = 1;
    } else {
        for (int i = 1; i < argc && n < (int)sizeof(line) - 2; i++) {
            if (i > 1 && n < (int)sizeof(line) - 2) line[n++] = ' ';
            size_t l = strlen(argv[i]);
            if (l > (size_t)(sizeof(line) - 2 - n)) l = (size_t)(sizeof(line) - 2 - n);
            memcpy(line + n, argv[i], l); n += (int)l;
        }
    }
    line[n++] = '\n';
    for (;;) {
        if (cact_ub_write_all(STDOUT_FILENO, line, (size_t)n) != 0) return 1;
    }
}

/* ------------------------------------------------------------------ printf */

/* Emit one \xNN or \NNN escape; *pp points at the backslash. */
static void pf_escape(const char **pp) {
    const char *p = *pp + 1;
    unsigned char c;
    switch (*p) {
        case 'n': c = '\n'; p++; break;
        case 't': c = '\t'; p++; break;
        case 'r': c = '\r'; p++; break;
        case 'a': c = '\a'; p++; break;
        case 'b': c = '\b'; p++; break;
        case 'f': c = '\f'; p++; break;
        case 'v': c = '\v'; p++; break;
        case '\\': c = '\\'; p++; break;
        case '"': c = '"'; p++; break;
        case '\'': c = '\''; p++; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int v = 0, k = 0;
            while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
            c = (unsigned char)v;
            break;
        }
        case 'x': {
            int v = 0, k = 0;
            p++;
            while (k < 2 && isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0'
                        : tolower((unsigned char)*p) - 'a' + 10;
                v = v * 16 + d; p++; k++;
            }
            c = (unsigned char)v;
            break;
        }
        default:
            write(STDOUT_FILENO, "\\", 1);   /* leave the char for the next round */
            *pp = p;
            return;
    }
    write(STDOUT_FILENO, &c, 1);
    *pp = p;
}

static void pf_spec(char *spec, int si, char conv, const char *arg) {
    spec[si++] = conv;
    spec[si] = '\0';
    char buf[4096];
    switch (conv) {
        case 's': snprintf(buf, sizeof(buf), spec, arg); break;
        case 'c': {
            int c = arg[0] ? (unsigned char)arg[0] : 0;
            snprintf(buf, sizeof(buf), spec, c);
            break;
        }
        case 'd': case 'i':
            snprintf(buf, sizeof(buf), spec, (int)strtol(arg, NULL, 10));
            break;
        case 'u': case 'o': case 'x': case 'X': {
            int base = (conv == 'o') ? 8 : (conv == 'u') ? 10 : 16;
            snprintf(buf, sizeof(buf), spec, (unsigned)strtoul(arg, NULL, base));
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof(buf), spec, strtod(arg, NULL));
            break;
        default: {
            buf[0] = '%';
            int k = 0;
            for (; k < si - 1 && k < (int)sizeof(buf) - 2; k++) buf[k + 1] = spec[k];
            buf[k + 1] = '\0';
            break;
        }
    }
    cact_ub_write_all(STDOUT_FILENO, buf, strlen(buf));
}

/* One pass over fmt; *ai is the next argument index, *used set when an
 * argument was consumed. */
static void pf_pass(const char *fmt, char **argv, int argc, int *ai, int *used) {
    const char *p = fmt;
    while (*p) {
        if (*p == '\\') { pf_escape(&p); continue; }
        if (*p != '%') {
            write(STDOUT_FILENO, p, 1);
            p++;
            continue;
        }
        char spec[32];
        int si = 0;
        spec[si++] = '%';
        p++;
        while (*p && strchr("-+ #0", *p) && si < (int)sizeof(spec) - 3) spec[si++] = *p++;
        while (*p && isdigit((unsigned char)*p) && si < (int)sizeof(spec) - 3) spec[si++] = *p++;
        if (*p == '.') {
            spec[si++] = '.';
            p++;
            while (*p && isdigit((unsigned char)*p) && si < (int)sizeof(spec) - 3) spec[si++] = *p++;
        }
        char conv = *p;
        if (conv == '%') { write(STDOUT_FILENO, "%", 1); p++; continue; }
        if (!conv) { write(STDOUT_FILENO, "%", 1); break; }
        p++;
        const char *arg = (*ai < argc) ? argv[*ai] : "";
        if (*ai < argc) { (*ai)++; *used = 1; }
        pf_spec(spec, si, conv, arg);
    }
}

static const char printf_usage[] =
    "usage: printf FORMAT [ARG...]\n"
    "Write FORMAT with %s %c %d %u %o %x %f escapes (\\n \\t \\\\ \\xHH) expanded,\n"
    "reusing FORMAT while arguments remain.\n";

int cact_ub_printf(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, printf_usage, sizeof(printf_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        write(STDERR_FILENO, printf_usage, sizeof(printf_usage) - 1);
        return 1;
    }
    const char *fmt = argv[1];
    int ai = 2;
    for (;;) {
        int used = 0;
        pf_pass(fmt, argv, argc, &ai, &used);
        if (!used || ai >= argc) break;
    }
    return 0;
}
