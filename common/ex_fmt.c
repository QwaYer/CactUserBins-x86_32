/*
 * ex_fmt.c — binary / text formatting and comparison utilities.
 *
 *   od        octal / hex / char dump (GNU-ish option syntax)
 *   hexdump   canonical hex dump
 *   strings   extract printable runs from a binary
 *   base64    encode / decode
 *   cmp       byte-by-byte compare of two files
 *   diff      line diff, normal and unified (-u) output
 *   nl        number lines
 *   rev       reverse the characters of each line
 *   file      identify a file from a small built-in signature table
 *
 * Everything reads files through cact_ub_read_all(), or a bounded head for the
 * signature sniffers, and writes with cact_ub_write_all().
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stat.h>
#include <errno.h>
#include <stdint.h>
#include <ioctl_abi.h>

/* Read at most cap bytes from the start of path (or stdin when path is NULL).
 * Used by the sniffers so a multi-megabyte file is never slurped whole. */
static int fmt_read_head(const char *path, unsigned char *buf, int cap) {
    int fd = path ? open(path, O_RDONLY, 0) : STDIN_FILENO;
    if (fd < 0) return -1;
    int n = 0;
    while (n < cap) {
        ssize_t r = read(fd, buf + n, (size_t)(cap - n));
        if (r < 0) { n = -1; break; }
        if (r == 0) break;
        n += (int)r;
    }
    if (path) close(fd);
    return n;
}

/* --------------------------------------------------------------------- nl */

static const char nl_usage[] =
    "usage: nl [-b a|t] [-w N] [-s SEP] [-v N] [-i N] [FILE...]\n"
    "Number lines of FILE (or stdin).  -b a numbers every line, -b t (default)\n"
    "numbers only non-empty lines.  -w width (6), -s separator (TAB), -v start\n"
    "(1), -i increment (1).\n";

static void nl_emit(const char *buf, size_t len, int all, int width,
                    const char *sep, long *nump, long incr) {
    size_t i = 0;
    while (i <= len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t llen = i - start;
        int blank = (llen == 0);
        if (i < len) i++;               /* consume the newline */
        else {
            if (start == len) break;    /* nothing after the final newline */
        }
        if (!blank || all) {
            /* Right-align the number in `width` columns.  The libc printf does
             * not implement the '*' width, so pad by hand. */
            char num[32];
            int nl = snprintf(num, sizeof(num), "%ld", *nump);
            for (int k = nl; k < width; k++) putchar(' ');
            fputs(num, stdout);
            fputs(sep, stdout);
            *nump += incr;
        } else {
            for (int k = 0; k < width; k++) putchar(' ');
        }
        cact_ub_write_all(STDOUT_FILENO, buf + start, llen);
        write(STDOUT_FILENO, "\n", 1);
        if (i >= len) break;
    }
}

int cact_ub_nl(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, nl_usage, sizeof(nl_usage) - 1);
        return 0;
    }
    int all = 0, width = 6;
    const char *sep = "\t";
    long start = 1, incr = 1;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-b") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (v[0] == 'a') all = 1;
            else if (v[0] == 't') all = 0;
            else { fprintf(stderr, "nl: bad -b %s\n", v); return 1; }
        } else if (strncmp(a, "-b", 2) == 0 && a[2]) {
            if (a[2] == 'a') all = 1;
            else if (a[2] == 't') all = 0;
            else { fprintf(stderr, "nl: bad -b %s\n", a); return 1; }
        } else if (strcmp(a, "-w") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strncmp(a, "-w", 2) == 0 && a[2]) width = atoi(a + 2);
        else if (strcmp(a, "-s") == 0 && i + 1 < argc) sep = argv[++i];
        else if (strncmp(a, "-s", 2) == 0 && a[2]) sep = a + 2;
        else if (strcmp(a, "-v") == 0 && i + 1 < argc) start = atol(argv[++i]);
        else if (strcmp(a, "-i") == 0 && i + 1 < argc) incr = atol(argv[++i]);
        else { fprintf(stderr, "nl: unknown option %s\n", a); return 1; }
    }
    if (width < 0) width = 0;

    long num = start;
    int ret = 0;
    if (i >= argc) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) { fprintf(stderr, "nl: cannot read stdin\n"); return 1; }
        nl_emit(buf, len, all, width, sep, &num, incr);
        free(buf);
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "nl: %s: cannot open\n", argv[i]); ret = 1; continue; }
        nl_emit(buf, len, all, width, sep, &num, incr);
        free(buf);
    }
    return ret;
}

/* -------------------------------------------------------------------- rev */

static const char rev_usage[] = "usage: rev [FILE...]\nReverse each line.\n";

static void rev_emit(const char *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t llen = i - start;
        for (size_t k = llen; k > 0; k--)
            write(STDOUT_FILENO, buf + start + k - 1, 1);
        if (i < len) { write(STDOUT_FILENO, "\n", 1); i++; }
    }
}

int cact_ub_rev(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, rev_usage, sizeof(rev_usage) - 1);
        return 0;
    }
    int ret = 0;
    if (argc < 2) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        rev_emit(buf, len);
        free(buf);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "rev: %s: cannot open\n", argv[i]); ret = 1; continue; }
        rev_emit(buf, len);
        free(buf);
    }
    return ret;
}

/* ---------------------------------------------------------------- strings */

static const char strings_usage[] =
    "usage: strings [-a] [-n N] [-t d|o|x] [FILE...]\n"
    "Print runs of at least N (default 4) printable characters.  -t prints each\n"
    "run's offset in the given radix.\n";

static int strings_min_len(const char *s) {
    long v = atol(s);
    return v > 0 ? (int)v : 1;
}

static void strings_fmt_off(unsigned long off, char radix, char *out) {
    if (radix == 'x')      snprintf(out, 16, "%lx", off);
    else if (radix == 'o') snprintf(out, 16, "%lo", off);
    else                   snprintf(out, 16, "%lu", off);
}

static void strings_emit(const unsigned char *buf, size_t len, int minlen,
                         int want_off, char radix) {
    size_t i = 0;
    while (i < len) {
        if (!(isprint(buf[i]) || buf[i] == '\t')) { i++; continue; }
        size_t start = i;
        while (i < len && (isprint(buf[i]) || buf[i] == '\t')) i++;
        size_t rl = i - start;
        if ((int)rl < minlen) continue;
        if (want_off) {
            char ob[24];
            strings_fmt_off((unsigned long)start, radix, ob);
            printf("%s ", ob);
        }
        cact_ub_write_all(STDOUT_FILENO, buf + start, rl);
        write(STDOUT_FILENO, "\n", 1);
    }
}

int cact_ub_strings(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, strings_usage, sizeof(strings_usage) - 1);
        return 0;
    }
    int minlen = 4, want_off = 0;
    char radix = 'd';
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "-a") == 0) continue;                 /* accepted, default */
        if (strcmp(a, "--") == 0) { i++; break; }
        if ((strcmp(a, "-n") == 0) && i + 1 < argc) minlen = strings_min_len(argv[++i]);
        else if (strncmp(a, "-n", 2) == 0 && a[2]) minlen = strings_min_len(a + 2);
        else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            want_off = 1;
            radix = (v[0] == 'o' || v[0] == 'x') ? v[0] : 'd';
        } else { fprintf(stderr, "strings: unknown option %s\n", a); return 1; }
    }

    int ret = 0;
    if (i >= argc) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        strings_emit((const unsigned char *)buf, len, minlen, want_off, radix);
        free(buf);
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "strings: %s: cannot open\n", argv[i]); ret = 1; continue; }
        strings_emit((const unsigned char *)buf, len, minlen, want_off, radix);
        free(buf);
    }
    return ret;
}

/* ----------------------------------------------------------------- base64 */

static const char b64_usage[] =
    "usage: base64 [-d|--decode] [-w N] [FILE]\n"
    "Encode FILE (or stdin) to base64, or decode with -d.  -w sets the wrap\n"
    "column (default 76, 0 disables wrapping).\n";

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t n, int wrap, int fd) {
    char out[4];
    int col = 0;
    size_t i = 0;
    while (i + 2 < n) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[0] = b64_tab[(v >> 18) & 0x3f];
        out[1] = b64_tab[(v >> 12) & 0x3f];
        out[2] = b64_tab[(v >>  6) & 0x3f];
        out[3] = b64_tab[v & 0x3f];
        for (int k = 0; k < 4; k++) {
            cact_ub_write_all(fd, &out[k], 1);
            if (wrap > 0 && ++col == wrap) { cact_ub_write_all(fd, "\n", 1); col = 0; }
        }
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        unsigned v = (unsigned)in[i] << 16;
        out[0] = b64_tab[(v >> 18) & 0x3f];
        out[1] = b64_tab[(v >> 12) & 0x3f];
        out[2] = '='; out[3] = '=';
    } else if (rem == 2) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8;
        out[0] = b64_tab[(v >> 18) & 0x3f];
        out[1] = b64_tab[(v >> 12) & 0x3f];
        out[2] = b64_tab[(v >>  6) & 0x3f];
        out[3] = '=';
    }
    if (rem) {
        for (int k = 0; k < 4; k++) {
            cact_ub_write_all(fd, &out[k], 1);
            if (wrap > 0 && ++col == wrap) { cact_ub_write_all(fd, "\n", 1); col = 0; }
        }
    }
    if (wrap > 0 && col != 0) cact_ub_write_all(fd, "\n", 1);
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const unsigned char *in, size_t n) {
    unsigned acc = 0;
    int nbits = 0;
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        int v = b64_val(c);
        if (v < 0) { bad = 1; continue; }
        acc = (acc << 6) | (unsigned)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            unsigned char b = (unsigned char)((acc >> nbits) & 0xff);
            cact_ub_write_all(STDOUT_FILENO, &b, 1);
        }
    }
    if (bad) { fprintf(stderr, "base64: invalid input\n"); return 1; }
    return 0;
}

int cact_ub_base64(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, b64_usage, sizeof(b64_usage) - 1);
        return 0;
    }
    int decode = 0, wrap = 76;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--decode") == 0) decode = 1;
        else if (strcmp(a, "-w") == 0 && i + 1 < argc) wrap = atoi(argv[++i]);
        else if (strncmp(a, "-w", 2) == 0 && a[2]) wrap = atoi(a + 2);
        else if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-") != 0) {
            fprintf(stderr, "base64: unknown option %s\n", a);
            return 1;
        } else file = a;
    }

    size_t len = 0;
    char *buf = cact_ub_read_all(file, &len);
    if (!buf) { fprintf(stderr, "base64: cannot read %s\n", file ? file : "stdin"); return 1; }
    int rc = decode ? b64_decode((const unsigned char *)buf, len)
                    : (b64_encode((const unsigned char *)buf, len, wrap, STDOUT_FILENO), 0);
    free(buf);
    return rc;
}

/* -------------------------------------------------------------------- cmp */

static const char cmp_usage[] =
    "usage: cmp [-l] [-s] FILE1 FILE2 [SKIP1 [SKIP2]]\n"
    "Compare two files byte by byte.  -l lists every differing byte; -s prints\n"
    "nothing and only sets the exit status (0 same, 1 differ, 2 error).\n";

int cact_ub_cmp(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cmp_usage, sizeof(cmp_usage) - 1);
        return 0;
    }
    int list = 0, silent = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-l") == 0) list = 1;
        else if (strcmp(a, "-s") == 0) silent = 1;
        else if (strcmp(a, "-ls") == 0 || strcmp(a, "-sl") == 0) { list = 1; silent = 1; }
        else { fprintf(stderr, "cmp: unknown option %s\n", a); return 2; }
    }
    if (argc - i < 2) { fprintf(stderr, "cmp: two files are required\n"); return 2; }

    long skip1 = (argc - i >= 3) ? cact_ub_parse_size(argv[i + 2]) : 0;
    long skip2 = (argc - i >= 4) ? cact_ub_parse_size(argv[i + 3]) : 0;
    size_t len1 = 0, len2 = 0;
    char *b1 = cact_ub_read_all(argv[i], &len1);
    if (!b1) { if (!silent) fprintf(stderr, "cmp: %s: cannot open\n", argv[i]); return 2; }
    char *b2 = cact_ub_read_all(argv[i + 1], &len2);
    if (!b2) { if (!silent) fprintf(stderr, "cmp: %s: cannot open\n", argv[i + 1]); free(b1); return 2; }
    if (skip1 < 0) skip1 = 0;
    if (skip2 < 0) skip2 = 0;
    if ((size_t)skip1 > len1) skip1 = (long)len1;
    if ((size_t)skip2 > len2) skip2 = (long)len2;

    const unsigned char *p1 = (const unsigned char *)b1 + skip1;
    const unsigned char *p2 = (const unsigned char *)b2 + skip2;
    size_t n1 = len1 - (size_t)skip1, n2 = len2 - (size_t)skip2;
    size_t nmin = n1 < n2 ? n1 : n2;

    int differ = 0;
    for (size_t k = 0; k < nmin; k++) {
        if (p1[k] == p2[k]) continue;
        differ = 1;
        if (silent) break;
        if (list) {
            printf("%lu %o %o\n", (unsigned long)(skip1 + (long)k + 1), p1[k], p2[k]);
        } else {
            printf("%s %s differ: byte %lu\n", argv[i], argv[i + 1],
                   (unsigned long)(skip1 + (long)k + 1));
            break;
        }
    }
    if (!differ && n1 != n2) {
        differ = 1;
        if (!silent && !list) {
            const char *shortf = n1 < n2 ? argv[i] : argv[i + 1];
            printf("cmp: EOF on %s after byte %lu\n", shortf,
                   (unsigned long)(nmin + (n1 < n2 ? skip1 : skip2)));
        }
    }
    free(b1);
    free(b2);
    return differ ? 1 : 0;
}

/* ------------------------------------------------------------------- diff */

static const char diff_usage[] =
    "usage: diff [-u] [-q] FILE1 FILE2\n"
    "Compare two text files line by line.  Default output is the normal diff\n"
    "format; -u prints unified hunks (3 lines of context), -q only reports\n"
    "whether the files differ.\n";

typedef struct {
    char t;          /* ' ' equal, '-' only in file1, '+' only in file2 */
    size_t idx;      /* line index into a[] or b[] */
} diff_op;

static size_t diff_split(char *buf, size_t len, char ***outp) {
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

static void diff_print_range(size_t from1, size_t to1) {  /* 1-based inclusive */
    if (from1 == to1) printf("%lu", (unsigned long)from1);
    else printf("%lu,%lu", (unsigned long)from1, (unsigned long)to1);
}

static int diff_normal(const diff_op *ops, size_t k, char **a, char **b) {
    unsigned long *pa = (unsigned long *)malloc((k + 1) * sizeof(unsigned long));
    unsigned long *pb = (unsigned long *)malloc((k + 1) * sizeof(unsigned long));
    if (!pa || !pb) { free(pa); free(pb); return 0; }
    pa[0] = 0; pb[0] = 0;
    for (size_t t = 0; t < k; t++) {
        pa[t + 1] = pa[t] + (ops[t].t != '+');   /* a-lines consumed */
        pb[t + 1] = pb[t] + (ops[t].t != '-');   /* b-lines consumed */
    }

    size_t i = 0;
    while (i < k) {
        if (ops[i].t == ' ') { i++; continue; }
        size_t start = i;
        while (i < k && ops[i].t != ' ') i++;
        size_t afirst = 0, alast = 0, acount = 0;
        size_t bfirst = 0, blast = 0, bcount = 0;
        for (size_t t = start; t < i; t++) {
            if (ops[t].t == '-') {
                if (!acount) afirst = ops[t].idx;
                alast = ops[t].idx;
                acount++;
            } else {
                if (!bcount) bfirst = ops[t].idx;
                blast = ops[t].idx;
                bcount++;
            }
        }
        if (acount && bcount) {
            diff_print_range(afirst + 1, alast + 1);
            printf("c");
            diff_print_range(bfirst + 1, blast + 1);
            printf("\n");
            for (size_t t = start; t < i; t++) if (ops[t].t == '-') printf("< %s\n", a[ops[t].idx]);
            printf("---\n");
            for (size_t t = start; t < i; t++) if (ops[t].t == '+') printf("> %s\n", b[ops[t].idx]);
        } else if (acount) {
            diff_print_range(afirst + 1, alast + 1);
            printf("d%lu\n", pb[start]);
            for (size_t t = start; t < i; t++) if (ops[t].t == '-') printf("< %s\n", a[ops[t].idx]);
        } else {
            printf("%lua", pa[start]);
            diff_print_range(bfirst + 1, blast + 1);
            printf("\n");
            for (size_t t = start; t < i; t++) if (ops[t].t == '+') printf("> %s\n", b[ops[t].idx]);
        }
    }
    free(pa);
    free(pb);
    return 1;
}

static void diff_unified(const diff_op *ops, size_t K, char **a, char **b,
                         const char *f1, const char *f2, unsigned long lo1, unsigned long lo2) {
    const size_t ctx = 3;
    unsigned long *pa = (unsigned long *)malloc((K + 1) * sizeof(unsigned long));
    unsigned long *pb = (unsigned long *)malloc((K + 1) * sizeof(unsigned long));
    if (!pa || !pb) { free(pa); free(pb); return; }
    pa[0] = 0; pb[0] = 0;
    for (size_t t = 0; t < K; t++) {
        pa[t + 1] = pa[t] + (ops[t].t != '+');
        pb[t + 1] = pb[t] + (ops[t].t != '-');
    }
    printf("--- %s\n+++ %s\n", f1, f2);

    size_t k = 0;
    while (k < K) {
        if (ops[k].t == ' ') { k++; continue; }
        size_t start = (k >= ctx) ? k - ctx : 0;
        size_t end = k;
        while (end < K) {
            if (ops[end].t != ' ') { end++; continue; }
            size_t s = end;
            while (s < K && ops[s].t == ' ') s++;
            if (s < K && (s - end) <= 2 * ctx) { end = s; continue; }
            break;
        }
        size_t hend = (end + ctx <= K) ? end + ctx : K;
        unsigned long acount = pa[hend] - pa[start];
        unsigned long bcount = pb[hend] - pb[start];
        printf("@@ -");
        if (acount == 0) printf("0,0");
        else printf("%lu,%lu", pa[start] + lo1, acount);
        printf(" +");
        if (bcount == 0) printf("0,0");
        else printf("%lu,%lu", pb[start] + lo2, bcount);
        printf(" @@\n");
        for (size_t t = start; t < hend; t++) {
            char c = ops[t].t;
            putchar(c);
            puts(c == '+' ? b[ops[t].idx] : a[ops[t].idx]);
        }
        k = hend;
    }
    free(pa);
    free(pb);
}

static int diff_file(const char *p1, const char *p2, int unified, int brief) {
    size_t l1 = 0, l2 = 0;
    char *r1 = cact_ub_read_all(p1, &l1);
    if (!r1) { fprintf(stderr, "diff: %s: cannot open\n", p1); return 2; }
    char *r2 = cact_ub_read_all(p2, &l2);
    if (!r2) { fprintf(stderr, "diff: %s: cannot open\n", p2); free(r1); return 2; }

    int same = (l1 == l2) && (l1 == 0 || memcmp(r1, r2, l1) == 0);
    if (same) { free(r1); free(r2); return 0; }
    if (brief) { printf("Files %s and %s differ\n", p1, p2); free(r1); free(r2); return 1; }

    char **a = NULL, **b = NULL;
    size_t n = diff_split(r1, l1, &a);
    size_t m = diff_split(r2, l2, &b);
    if (!a || !b) { free(r1); free(r2); free(a); free(b); return 2; }

    size_t cells = (n + 1) * (m + 1);
    if (cells > 4000000u) {
        fprintf(stderr, "diff: files too large to diff (%lu x %lu)\n",
                (unsigned long)n, (unsigned long)m);
        free(r1); free(r2); free(a); free(b);
        return 2;
    }
    unsigned char *dir = (unsigned char *)malloc(cells ? cells : 1);
    int *cur = (int *)malloc((m + 1) * sizeof(int));
    int *nxt = (int *)calloc(m + 1, sizeof(int));
    if (!dir || !cur || !nxt) { free(dir); free(cur); free(nxt); free(r1); free(r2); free(a); free(b); return 2; }

    for (size_t ii = n; ii-- > 0; ) {
        cur[m] = 0;
        for (size_t jj = m; jj-- > 0; ) {
            unsigned char d;
            int v;
            if (strcmp(a[ii], b[jj]) == 0) {
                d = 0; v = nxt[jj + 1] + 1;
            } else if (nxt[jj] >= cur[jj + 1]) {
                d = 1; v = nxt[jj];
            } else {
                d = 2; v = cur[jj + 1];
            }
            dir[ii * (m + 1) + jj] = d;
            cur[jj] = v;
        }
        memcpy(nxt, cur, (m + 1) * sizeof(int));
    }

    /* Walk the alignment into a linear op stream. */
    diff_op *ops = (diff_op *)malloc((n + m + 1) * sizeof(diff_op));
    if (!ops) { free(dir); free(cur); free(nxt); free(r1); free(r2); free(a); free(b); return 2; }
    size_t ii = 0, jj = 0, K = 0;
    while (ii < n && jj < m) {
        unsigned char d = dir[ii * (m + 1) + jj];
        if (d == 0)      { ops[K].t = ' '; ops[K].idx = ii; ii++; jj++; }
        else if (d == 1) { ops[K].t = '-'; ops[K].idx = ii; ii++; }
        else             { ops[K].t = '+'; ops[K].idx = jj; jj++; }
        K++;
    }
    while (ii < n) { ops[K].t = '-'; ops[K].idx = ii++; K++; }
    while (jj < m) { ops[K].t = '+'; ops[K].idx = jj++; K++; }

    int all_eq = 1;
    for (size_t t = 0; t < K; t++) if (ops[t].t != ' ') { all_eq = 0; break; }
    if (all_eq) {
        /* Every line matched; the only remaining difference is the trailing
         * newline (byte-equality was already ruled out above). */
        printf("diff: %s and %s differ only in the trailing newline\n", p1, p2);
        free(ops); free(dir); free(cur); free(nxt);
        free(r1); free(r2); free(a); free(b);
        return 1;
    }

    if (unified) diff_unified(ops, K, a, b, p1, p2, 1, 1);
    else         diff_normal(ops, K, a, b);

    free(ops); free(dir); free(cur); free(nxt);
    free(r1); free(r2); free(a); free(b);
    return 1;
}

int cact_ub_diff(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, diff_usage, sizeof(diff_usage) - 1);
        return 0;
    }
    int unified = 0, brief = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-u") == 0 || strcmp(a, "-U") == 0) unified = 1;
        else if (strcmp(a, "-q") == 0) brief = 1;
        else if (strcmp(a, "-uq") == 0 || strcmp(a, "-qu") == 0) { unified = 1; brief = 1; }
        else { fprintf(stderr, "diff: unknown option %s\n", a); return 2; }
    }
    if (argc - i != 2) { fprintf(stderr, "diff: two files are required\n"); return 2; }
    return diff_file(argv[i], argv[i + 1], unified, brief);
}

/* --------------------------------------------------------------------- od */

static const char od_usage[] =
    "usage: od [-A d|o|x|n] [-t x1|o1|d1|u1|c] [-N BYTES] [-j SKIP] [FILE...]\n"
    "Dump a file in octal (default), hex, decimal or character form.  -x is\n"
    "shorthand for -A x -t x1, -c for -t c, -b for -A o -t o1.\n";

typedef struct { int size; char kind; } od_fmt;

static void od_char_c(unsigned char c, char out[5]) {
    switch (c) {
        case '\0': memcpy(out, "  \\0", 5); return;
        case '\a': memcpy(out, "  \\a", 5); return;
        case '\b': memcpy(out, "  \\b", 5); return;
        case '\t': memcpy(out, "  \\t", 5); return;
        case '\n': memcpy(out, "  \\n", 5); return;
        case '\v': memcpy(out, "  \\v", 5); return;
        case '\f': memcpy(out, "  \\f", 5); return;
        case '\r': memcpy(out, "  \\r", 5); return;
        default:
            if (c >= 0x20 && c < 0x7f) snprintf(out, 5, "   %c", c);
            else                       snprintf(out, 5, " %03o", c);
            return;
    }
}

static void od_value(const unsigned char *b, int size, od_fmt f) {
    unsigned long v = 0;
    for (int k = 0; k < size; k++) v |= (unsigned long)b[k] << (8 * k);
    if (f.kind == 'x') {
        if (size == 1) printf(" %02lx", v & 0xff);
        else if (size == 2) printf(" %04lx", v & 0xffff);
        else printf(" %08lx", v);
    } else if (f.kind == 'o') {
        if (size == 1) printf(" %03lo", v & 0xff);
        else if (size == 2) printf(" %06lo", v & 0xffff);
        else printf(" %011lo", v);
    } else if (f.kind == 'd') {
        long sv = (long)(int32_t)v;
        if (size == 1) sv = (long)(signed char)(v & 0xff);
        else if (size == 2) sv = (long)(int16_t)(v & 0xffff);
        printf(" %ld", sv);
    } else { /* 'u' */
        if (size == 1) printf(" %3lu", v & 0xff);
        else if (size == 2) printf(" %5lu", v & 0xffff);
        else printf(" %10lu", v);
    }
}

static void od_dump(const unsigned char *buf, size_t len, char aradix, od_fmt f) {
    size_t per = 16;
    size_t pos = 0;
    while (pos < len) {
        size_t n = len - pos;
        if (n > per) n = per;
        if (aradix == 'd')      printf("%07lu", (unsigned long)pos);
        else if (aradix == 'o') printf("%07lo", (unsigned long)pos);
        else if (aradix == 'x') printf("%06lx", (unsigned long)pos);
        for (size_t k = 0; k < per; k += (size_t)f.size) {
            if (k >= n) break;
            if (k == 8 && f.size == 1) putchar(' ');
            if (f.kind == 'c') {
                char col[5];
                od_char_c(buf[pos + k], col);
                fputs(col, stdout);
            } else {
                od_value(buf + pos + k, f.size, f);
            }
        }
        putchar('\n');
        pos += n;
    }
    if (aradix == 'd')      printf("%07lu\n", (unsigned long)len);
    else if (aradix == 'o') printf("%07lo\n", (unsigned long)len);
    else if (aradix == 'x') printf("%06lx\n", (unsigned long)len);
}

int cact_ub_od(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, od_usage, sizeof(od_usage) - 1);
        return 0;
    }
    char aradix = 'o';
    od_fmt f = { 2, 'o' };
    long limit = -1, skip = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-v") == 0) continue;
        if (strcmp(a, "-x") == 0) { aradix = 'x'; f.size = 1; f.kind = 'x'; }
        else if (strcmp(a, "-c") == 0) { f.size = 1; f.kind = 'c'; }
        else if (strcmp(a, "-b") == 0) { aradix = 'o'; f.size = 1; f.kind = 'o'; }
        else if (strcmp(a, "-A") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            aradix = (v[0] == 'd' || v[0] == 'o' || v[0] == 'x') ? v[0] : 'n';
        } else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (v[0] == 'c') { f.size = 1; f.kind = 'c'; }
            else {
                f.kind = v[0];
                f.size = (v[1] >= '1' && v[1] <= '4') ? v[1] - '0' : 1;
                if (f.kind != 'x' && f.kind != 'o' && f.kind != 'd' && f.kind != 'u') f.kind = 'o';
            }
        } else if (strcmp(a, "-N") == 0 && i + 1 < argc) limit = cact_ub_parse_size(argv[++i]);
        else if (strcmp(a, "-j") == 0 && i + 1 < argc) skip = cact_ub_parse_size(argv[++i]);
        else { fprintf(stderr, "od: unknown option %s\n", a); return 1; }
    }
    if (aradix != 'd' && aradix != 'o' && aradix != 'x' && aradix != 'n') aradix = 'o';

    char *acc = NULL;
    size_t acclen = 0;
    if (i >= argc) {
        acc = cact_ub_read_all(NULL, &acclen);
        if (!acc) { fprintf(stderr, "od: cannot read stdin\n"); return 1; }
    } else {
        for (; i < argc; i++) {
            size_t n = 0;
            char *b = cact_ub_read_all(argv[i], &n);
            if (!b) { fprintf(stderr, "od: %s: cannot open\n", argv[i]); free(acc); return 1; }
            char *na = (char *)realloc(acc, acclen + n + 1);
            if (!na) { free(b); free(acc); return 1; }
            acc = na;
            memcpy(acc + acclen, b, n);
            acclen += n;
            acc[acclen] = '\0';
            free(b);
        }
    }
    if (!acc) return 1;

    size_t start = skip > 0 ? (size_t)skip : 0;
    if (start > acclen) start = acclen;
    size_t n = acclen - start;
    if (limit >= 0 && (size_t)limit < n) n = (size_t)limit;
    od_dump((const unsigned char *)acc + start, n, aradix, f);
    free(acc);
    return 0;
}

/* ---------------------------------------------------------------- hexdump */

static const char hexdump_usage[] =
    "usage: hexdump [-C|-b|-c] [-n LEN] [-s SKIP] [FILE...]\n"
    "Dump a file as hex + ASCII (-C, default), octal bytes (-b) or characters\n"
    "(-c).  -n limits the length, -s skips bytes.\n";

static void hd_canon(const unsigned char *b, size_t n, unsigned long off) {
    printf("%08lx  ", off);
    for (int j = 0; j < 16; j++) {
        if (j == 8) putchar(' ');
        if ((size_t)j < n) printf("%02x ", b[j]);
        else               fputs("   ", stdout);
    }
    putchar(' ');
    putchar('|');
    for (size_t j = 0; j < n; j++) putchar(isprint(b[j]) ? b[j] : '.');
    putchar('|');
    putchar('\n');
}

static void hd_oct(const unsigned char *b, size_t n, unsigned long off) {
    printf("%07lo", off);
    for (size_t j = 0; j < n; j++) printf(" %03o", b[j]);
    putchar('\n');
}

static void hd_char(const unsigned char *b, size_t n, unsigned long off) {
    printf("%07lo", off);
    for (size_t j = 0; j < n; j++) {
        char col[5];
        od_char_c(b[j], col);
        fputs(col, stdout);
    }
    putchar('\n');
}

int cact_ub_hexdump(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, hexdump_usage, sizeof(hexdump_usage) - 1);
        return 0;
    }
    char mode = 'C';
    long limit = -1, skip = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-v") == 0) continue;
        if (strcmp(a, "-C") == 0) mode = 'C';
        else if (strcmp(a, "-b") == 0) mode = 'b';
        else if (strcmp(a, "-c") == 0) mode = 'c';
        else if (strcmp(a, "-n") == 0 && i + 1 < argc) limit = cact_ub_parse_size(argv[++i]);
        else if (strncmp(a, "-n", 2) == 0 && a[2]) limit = cact_ub_parse_size(a + 2);
        else if (strcmp(a, "-s") == 0 && i + 1 < argc) skip = cact_ub_parse_size(argv[++i]);
        else if (strncmp(a, "-s", 2) == 0 && a[2]) skip = cact_ub_parse_size(a + 2);
        else { fprintf(stderr, "hexdump: unknown option %s\n", a); return 1; }
    }

    char *acc = NULL;
    size_t acclen = 0;
    if (i >= argc) {
        acc = cact_ub_read_all(NULL, &acclen);
        if (!acc) { fprintf(stderr, "hexdump: cannot read stdin\n"); return 1; }
    } else {
        for (; i < argc; i++) {
            size_t n = 0;
            char *b = cact_ub_read_all(argv[i], &n);
            if (!b) { fprintf(stderr, "hexdump: %s: cannot open\n", argv[i]); free(acc); return 1; }
            char *na = (char *)realloc(acc, acclen + n + 1);
            if (!na) { free(b); free(acc); return 1; }
            acc = na;
            memcpy(acc + acclen, b, n);
            acclen += n;
            acc[acclen] = '\0';
            free(b);
        }
    }
    if (!acc) return 1;

    size_t start = skip > 0 ? (size_t)skip : 0;
    if (start > acclen) start = acclen;
    size_t n = acclen - start;
    if (limit >= 0 && (size_t)limit < n) n = (size_t)limit;

    const unsigned char *p = (const unsigned char *)acc + start;
    for (size_t pos = 0; pos < n; pos += 16) {
        size_t l = n - pos < 16 ? n - pos : 16;
        if (mode == 'C')      hd_canon(p + pos, l, (unsigned long)(start + pos));
        else if (mode == 'b') hd_oct(p + pos, l, (unsigned long)(start + pos));
        else                  hd_char(p + pos, l, (unsigned long)(start + pos));
    }
    free(acc);
    return 0;
}

/* ------------------------------------------------------------------- file */

static const char file_usage[] =
    "usage: file [-b] FILE...\n"
    "Identify each file from its leading bytes (built-in signatures only).\n";

static const char *file_kind(const unsigned char *b, int n, const char *path,
                             struct stat *st) {
    if (st && S_ISDIR(st->st_mode))  return "directory";
    if (st && S_ISCHR(st->st_mode))  return "character special";
    if (st && S_ISBLK(st->st_mode))  return "block special";
    if (st && S_ISFIFO(st->st_mode)) return "fifo";
    if (st && S_ISLNK(st->st_mode))  return "symbolic link";
    if (n <= 0)                      return "empty";
    (void)path;

    if (n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        int class32 = b[4] == 1;
        int type = n >= 18 ? (b[16] | (b[17] << 8)) : 0;
        if (type == 2) return "ELF shared object";
        if (type == 3) return class32 ? "ELF 32-bit LSB pie executable"
                                      : "ELF 64-bit LSB pie executable";
        return class32 ? "ELF 32-bit LSB executable" : "ELF 64-bit LSB executable";
    }
    if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b) return "gzip compressed data";
    if (n >= 3 && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h') return "bzip2 compressed data";
    if (n >= 6 && b[0] == 0xfd && b[1] == '7' && b[2] == 'z' && b[3] == 'X' &&
        b[4] == 'Z' && b[5] == 0x00) return "XZ compressed data";
    if (n >= 4 && b[0] == 'P' && b[1] == 'K' && (b[2] == 3 || b[2] == 5)) return "Zip archive data";
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "PNG image data";
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return "JPEG image data";
    if (n >= 4 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return "GIF image data";
    if (n >= 4 && b[0] == '%' && b[1] == 'P' && b[2] == 'D' && b[3] == 'F') return "PDF document";
    if (n >= 4 && b[0] == 0x04 && b[1] == 0x22 && b[2] == 0x4d && b[3] == 0x18) return "LZ4 compressed data";
    if (n >= 512 && memcmp(b + 257, "ustar", 5) == 0) return "POSIX tar archive";
    if (n >= 512 && b[510] == 0x55 && b[511] == 0xaa) {
        if (n >= 520 && memcmp(b + 512, "EFI PART", 8) == 0)
            return "GPT partition table";
        return "DOS/MBR boot sector";
    }
    if (n >= 1082 && b[1080] == 0x53 && b[1081] == 0xef)
        return "ext2/ext3/ext4 filesystem data";
    if (n >= 90 && (memcmp(b + 54, "FAT", 3) == 0 || memcmp(b + 82, "FAT32", 5) == 0))
        return "FAT filesystem data";
    if (n >= 2 && b[0] == '#' && b[1] == '!') return "script text executable";
    if (n >= 6 && (memcmp(b, "<?xml", 5) == 0)) return "XML document";
    if (n >= 9 && memcmp(b, "SQLite f", 8) == 0) return "SQLite 3.x database";

    int ascii_ok = 1, utf8_ok = 1;
    for (int k = 0; k < n; k++) {
        unsigned char c = b[k];
        if (!(isprint(c) || c == '\n' || c == '\t' || c == '\r' || c == '\f' || c == '\v'))
            ascii_ok = 0;
        if (c >= 0x80) {
            int extra = 0;
            if ((c & 0xe0) == 0xc0) extra = 1;
            else if ((c & 0xf0) == 0xe0) extra = 2;
            else if ((c & 0xf8) == 0xf0) extra = 3;
            else { utf8_ok = 0; break; }
            if (k + extra >= n) { utf8_ok = 0; break; }
            for (int e = 1; e <= extra; e++)
                if ((b[k + e] & 0xc0) != 0x80) { utf8_ok = 0; break; }
            k += extra;
        }
    }
    if (ascii_ok) return "ASCII text";
    if (utf8_ok)  return "UTF-8 Unicode text";
    return "data";
}

int cact_ub_file(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, file_usage, sizeof(file_usage) - 1);
        return 0;
    }
    int brief = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0) brief = 1;
        else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "file: unknown option %s\n", argv[i]); return 1; }
        else break;
    }
    if (i >= argc) { fprintf(stderr, "usage: file [-b] FILE...\n"); return 1; }

    int ret = 0;
    for (; i < argc; i++) {
        struct stat st;
        int have_st = (lstat(argv[i], &st) == 0);
        const char *name = file_kind((const unsigned char *)"", 0, argv[i], have_st ? &st : NULL);
        if (!have_st) {
            fprintf(stderr, "file: %s: cannot stat\n", argv[i]);
            ret = 1;
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            unsigned char head[520];
            int n = fmt_read_head(argv[i], head, (int)sizeof(head));
            if (n < 0) { fprintf(stderr, "file: %s: cannot read\n", argv[i]); ret = 1; continue; }
            name = file_kind(head, n, argv[i], &st);
        }
        if (brief) printf("%s\n", name);
        else       printf("%s: %s\n", argv[i], name);
    }
    return ret;
}
