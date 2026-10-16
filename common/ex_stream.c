/*
 * ex_stream.c — stream-shaping utilities.
 *
 *   tac     reverse the order of lines
 *   shuf    random permutation of lines / arguments / a numeric range
 *   split   split a file into pieces by lines or bytes
 *   fold    wrap long lines to a width
 *   paste   merge lines of several files side by side (or serially)
 *
 * All of them work on whole buffers read through cact_ub_read_all().
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stat.h>
#include <time.h>

/* -------------------------------------------------------------------- tac */

static const char tac_usage[] = "usage: tac [FILE...]\nPrint lines in reverse order.\n";

static void tac_emit(const char *buf, size_t len) {
    /* Record each line with its terminating newline (the last line may have
     * none); then print the records back to front. */
    size_t cap = 64, cnt = 0, i = 0;
    size_t *start = (size_t *)malloc(cap * sizeof(size_t));
    size_t *end = (size_t *)malloc(cap * sizeof(size_t));
    if (!start || !end) { free(start); free(end); return; }
    while (i < len) {
        size_t s = i, e;
        while (i < len && buf[i] != '\n') i++;
        e = i;
        if (i < len) i++;               /* include the newline */
        else e = i;
        if (cnt == cap) {
            cap *= 2;
            size_t *ns = (size_t *)realloc(start, cap * sizeof(size_t));
            size_t *ne = (size_t *)realloc(end, cap * sizeof(size_t));
            if (!ns || !ne) { free(ns ? ns : start); free(ne ? ne : end); return; }
            start = ns; end = ne;
        }
        start[cnt] = s;
        end[cnt] = e;
        cnt++;
    }
    for (size_t k = cnt; k > 0; k--) {
        size_t s = start[k - 1], e = end[k - 1];
        cact_ub_write_all(STDOUT_FILENO, buf + s, e - s);
        if (s == e || buf[e - 1] != '\n') write(STDOUT_FILENO, "\n", 1);
    }
    free(start);
    free(end);
}

int cact_ub_tac(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tac_usage, sizeof(tac_usage) - 1);
        return 0;
    }
    int ret = 0;
    if (argc < 2) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        tac_emit(buf, len);
        free(buf);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "tac: %s: cannot open\n", argv[i]); ret = 1; continue; }
        tac_emit(buf, len);
        free(buf);
    }
    return ret;
}

/* ------------------------------------------------------------------- shuf */

static const char shuf_usage[] =
    "usage: shuf [-n N] [-e] [-i LO-HI] [FILE]\n"
    "Write a random permutation of the input lines (or -e arguments, or the\n"
    "-i numeric range).  -n limits the number of output lines.\n";

static unsigned int shuf_rand(void) {
    return (unsigned int)rand();
}

static void shuf_emit(char **items, size_t n, long count) {
    for (size_t i = n; i > 1; i--) {
        size_t j = shuf_rand() % i;
        char *t = items[i - 1];
        items[i - 1] = items[j];
        items[j] = t;
    }
    size_t out = (count >= 0 && (size_t)count < n) ? (size_t)count : n;
    for (size_t i = 0; i < out; i++) {
        fputs(items[i], stdout);
        putchar('\n');
    }
}

int cact_ub_shuf(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, shuf_usage, sizeof(shuf_usage) - 1);
        return 0;
    }
    long count = -1;
    int use_args = 0;
    const char *range = NULL;
    const char *file = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-e") == 0 || strcmp(a, "--echo") == 0) use_args = 1;
        else if (strcmp(a, "-n") == 0 && i + 1 < argc) count = atol(argv[++i]);
        else if (strncmp(a, "-n", 2) == 0 && a[2]) count = atol(a + 2);
        else if (strcmp(a, "-i") == 0 && i + 1 < argc) range = argv[++i];
        else { fprintf(stderr, "shuf: unknown option %s\n", a); return 1; }
    }

    struct timeval tv;
    gettimeofday(&tv, 0);
    srand((unsigned)(tv.tv_sec ^ (tv.tv_usec << 11) ^ (getpid() * 2654435761u)));

    char *buf = NULL;
    size_t len = 0;
    char **items = NULL;
    size_t n = 0;

    if (range) {
        long lo = 0, hi = 0;
        const char *dash = strchr(range, '-');
        if (dash) {
            lo = atol(range);
            hi = atol(dash + 1);
        } else {
            lo = 1; hi = atol(range);
        }
        if (hi < lo) { fprintf(stderr, "shuf: empty range\n"); return 1; }
        size_t total = (size_t)(hi - lo + 1);
        items = (char **)malloc((total ? total : 1) * sizeof(char *));
        if (!items) return 1;
        for (long v = lo; v <= hi; v++) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%ld", v);
            items[n++] = strdup(tmp);
        }
    } else if (use_args) {
        if (i >= argc) { fprintf(stderr, "shuf: -e needs arguments\n"); return 1; }
        items = (char **)malloc((size_t)(argc - i) * sizeof(char *));
        if (!items) return 1;
        for (; i < argc; i++) items[n++] = argv[i];   /* borrowed, not freed */
    } else {
        if (i < argc) file = argv[i];
        buf = cact_ub_read_all(file, &len);
        if (!buf) { fprintf(stderr, "shuf: cannot read %s\n", file ? file : "stdin"); return 1; }
        char **arr = NULL;
        {
            char *p = buf, *end = buf + len;
            size_t cap = 64;
            arr = (char **)malloc(cap * sizeof(char *));
            if (!arr) { free(buf); return 1; }
            while (p < end) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                char *q;
                if (nl) { *nl = '\0'; q = nl + 1; }
                else q = end;
                if (n == cap) {
                    cap *= 2;
                    char **na = (char **)realloc(arr, cap * sizeof(char *));
                    if (!na) { free(arr); free(buf); return 1; }
                    arr = na;
                }
                arr[n++] = p;
                p = q;
            }
        }
        items = arr;
    }

    shuf_emit(items, n, count);

    if (range) for (size_t k = 0; k < n; k++) free(items[k]);
    free(items);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ split */

static const char split_usage[] =
    "usage: split [-l N] [-b N] [-a N] [FILE [PREFIX]]\n"
    "Split FILE (default stdin) into pieces of N lines (-l, default 1000) or N\n"
    "bytes (-b).  Pieces are PREFIX (default x) plus a suffix of -a (default 2)\n"
    "letters.\n";

static void split_suffix(char *out, int width, unsigned long idx) {
    for (int k = width - 1; k >= 0; k--) {
        out[k] = (char)('a' + (idx % 26));
        idx /= 26;
    }
    out[width] = '\0';
}

int cact_ub_split(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, split_usage, sizeof(split_usage) - 1);
        return 0;
    }
    long lines = 1000, bytes = -1;
    int swidth = 2;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-l") == 0 && i + 1 < argc) { lines = atol(argv[++i]); bytes = -1; }
        else if (strncmp(a, "-l", 2) == 0 && a[2]) { lines = atol(a + 2); bytes = -1; }
        else if (strcmp(a, "-b") == 0 && i + 1 < argc) { bytes = atol(argv[++i]); }
        else if (strncmp(a, "-b", 2) == 0 && a[2]) { bytes = atol(a + 2); }
        else if (strcmp(a, "-a") == 0 && i + 1 < argc) swidth = atoi(argv[++i]);
        else if (strncmp(a, "-a", 2) == 0 && a[2]) swidth = atoi(a + 2);
        else { fprintf(stderr, "split: unknown option %s\n", a); return 1; }
    }
    const char *file = (i < argc) ? argv[i++] : NULL;
    const char *prefix = (i < argc) ? argv[i] : "x";
    if (lines <= 0) lines = 1;
    if (swidth < 1) swidth = 2;

    size_t len = 0;
    char *buf = cact_ub_read_all(file, &len);
    if (!buf) { fprintf(stderr, "split: cannot read %s\n", file ? file : "stdin"); return 1; }

    unsigned long part = 0;
    size_t pos = 0;
    if (bytes > 0) {
        while (pos < len) {
            size_t n = len - pos;
            if ((size_t)bytes < n) n = (size_t)bytes;
            char path[512];
            char suf[16];
            split_suffix(suf, swidth, part++);
            snprintf(path, sizeof(path), "%s%s", prefix, suf);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { fprintf(stderr, "split: %s: cannot create\n", path); free(buf); return 1; }
            int rc = cact_ub_write_all(fd, buf + pos, n);
            close(fd);
            if (rc != 0) { fprintf(stderr, "split: %s: write failed\n", path); free(buf); return 1; }
            pos += n;
        }
    } else {
        /* line mode: emit `lines` input records per piece */
        while (pos < len) {
            size_t start = pos;
            long cnt = 0;
            while (pos < len && cnt < lines) {
                while (pos < len && buf[pos] != '\n') pos++;
                if (pos < len) pos++;
                cnt++;
            }
            char path[512];
            char suf[16];
            split_suffix(suf, swidth, part++);
            snprintf(path, sizeof(path), "%s%s", prefix, suf);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { fprintf(stderr, "split: %s: cannot create\n", path); free(buf); return 1; }
            int rc = cact_ub_write_all(fd, buf + start, pos - start);
            close(fd);
            if (rc != 0) { fprintf(stderr, "split: %s: write failed\n", path); free(buf); return 1; }
        }
    }
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------- fold */

static const char fold_usage[] =
    "usage: fold [-w N] [-s] [FILE...]\n"
    "Wrap input lines at N columns (default 80).  -s breaks at the last blank\n"
    "within the width instead of mid-word.\n";

static void fold_emit(const char *buf, size_t len, int width, int break_spaces) {
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && buf[i] != '\n') i++;
        size_t llen = i - s;
        if (i < len) i++;
        size_t off = 0;
        while (off < llen) {
            size_t chunk = (size_t)width;
            if (chunk > llen - off) chunk = llen - off;
            if (break_spaces && off + chunk < llen) {
                size_t lastspace = chunk;
                for (size_t k = chunk; k > 0; k--) {
                    if (buf[s + off + k - 1] == ' ' || buf[s + off + k - 1] == '\t') {
                        lastspace = k;
                        break;
                    }
                }
                if (lastspace != chunk) chunk = lastspace;
                else {
                    /* no blank within the width: extend to the next blank */
                    size_t k = chunk;
                    while (off + k < llen && buf[s + off + k] != ' ' && buf[s + off + k] != '\t') k++;
                    chunk = k;
                }
            }
            cact_ub_write_all(STDOUT_FILENO, buf + s + off, chunk);
            write(STDOUT_FILENO, "\n", 1);
            off += chunk;
        }
        if (llen == 0) write(STDOUT_FILENO, "\n", 1);
    }
}

int cact_ub_fold(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, fold_usage, sizeof(fold_usage) - 1);
        return 0;
    }
    int width = 80, break_spaces = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-s") == 0) break_spaces = 1;
        else if (strcmp(a, "-w") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strncmp(a, "-w", 2) == 0 && a[2]) width = atoi(a + 2);
        else { fprintf(stderr, "fold: unknown option %s\n", a); return 1; }
    }
    if (width < 1) width = 1;

    int ret = 0;
    if (i >= argc) {
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        fold_emit(buf, len, width, break_spaces);
        free(buf);
        return 0;
    }
    for (; i < argc; i++) {
        size_t len = 0;
        char *buf = cact_ub_read_all(argv[i], &len);
        if (!buf) { fprintf(stderr, "fold: %s: cannot open\n", argv[i]); ret = 1; continue; }
        fold_emit(buf, len, width, break_spaces);
        free(buf);
    }
    return ret;
}

/* ------------------------------------------------------------------ paste */

static const char paste_usage[] =
    "usage: paste [-d LIST] [-s] [FILE...]\n"
    "Merge lines of the inputs side by side (TAB between columns by default).\n"
    "-s pastes one file at a time.  -d sets the delimiter list (\\t, \\n, \\0).\n";

/* Return the next delimiter character, cycling the -d list when exhausted.
 * Understands the \t, \n, \0 and \\ escapes.  Updates *pp. */
static int paste_next(const char **pp, const char *base) {
    const char *p = *pp;
    if (!p || !*p) p = base;
    int c;
    if (p[0] == '\\' && p[1]) {
        switch (p[1]) {
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case '0': c = '\0'; break;
            case '\\': c = '\\'; break;
            default:  c = p[1]; break;
        }
        p += 2;
    } else {
        c = (unsigned char)p[0];
        p += 1;
    }
    *pp = p;
    return c;
}

int cact_ub_paste(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, paste_usage, sizeof(paste_usage) - 1);
        return 0;
    }
    const char *delims = "\t";
    int serial = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-s") == 0) serial = 1;
        else if (strcmp(a, "-d") == 0 && i + 1 < argc) delims = argv[++i];
        else if (strncmp(a, "-d", 2) == 0 && a[2]) delims = a + 2;
        else { fprintf(stderr, "paste: unknown option %s\n", a); return 1; }
    }
    if (delims[0] == '\0') delims = "\t";

    int nfiles = argc - i;
    if (nfiles <= 0) {
        /* single stdin stream */
        size_t len = 0;
        char *buf = cact_ub_read_all(NULL, &len);
        if (!buf) return 1;
        if (serial) cact_ub_write_all(STDOUT_FILENO, buf, len);
        else        cact_ub_write_all(STDOUT_FILENO, buf, len);
        free(buf);
        return 0;
    }

    char **bufs = (char **)malloc((size_t)nfiles * sizeof(char *));
    size_t *lens = (size_t *)malloc((size_t)nfiles * sizeof(size_t));
    if (!bufs || !lens) { free(bufs); free(lens); return 1; }
    for (int f = 0; f < nfiles; f++) {
        bufs[f] = cact_ub_read_all(argv[i + f], &lens[f]);
        if (!bufs[f]) { fprintf(stderr, "paste: %s: cannot open\n", argv[i + f]); lens[f] = 0; }
    }

    if (serial) {
        for (int f = 0; f < nfiles; f++) {
            if (!bufs[f]) continue;
            const char *p = bufs[f], *end = bufs[f] + lens[f];
            const char *dl = delims;
            int first = 1;
            while (p < end) {
                if (!first) {
                    int c = paste_next(&dl, delims);
                    if (c) { char cc = (char)c; write(STDOUT_FILENO, &cc, 1); }
                }
                first = 0;
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                const char *q = nl ? nl + 1 : end;
                cact_ub_write_all(STDOUT_FILENO, p, (size_t)(q - p));
                p = q;
            }
            if (end == bufs[f] || end[-1] != '\n') write(STDOUT_FILENO, "\n", 1);
        }
    } else {
        size_t *pos = (size_t *)calloc((size_t)nfiles, sizeof(size_t));
        if (!pos) { free(bufs); free(lens); return 1; }
        int any = 1;
        while (any) {
            any = 0;
            const char *dl = delims;
            for (int f = 0; f < nfiles; f++) {
                if (f > 0) {
                    int c = paste_next(&dl, delims);
                    if (c) { char cc = (char)c; write(STDOUT_FILENO, &cc, 1); }
                }
                if (!bufs[f] || pos[f] >= lens[f]) continue;
                any = 1;
                const char *p = bufs[f] + pos[f];
                const char *end = bufs[f] + lens[f];
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t seg = nl ? (size_t)(nl - p) : (size_t)(end - p);
                cact_ub_write_all(STDOUT_FILENO, p, seg);
                pos[f] += seg;
                if (nl) pos[f] += 1;
            }
            write(STDOUT_FILENO, "\n", 1);
        }
        free(pos);
    }

    for (int f = 0; f < nfiles; f++) free(bufs[f]);
    free(bufs);
    free(lens);
    return 0;
}
