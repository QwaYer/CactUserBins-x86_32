/*
 * ex_view.c — interactive / display utilities.
 *
 *   more  page through files a screenful at a time
 *   cal   print a month or a whole year
 *   tree  list a directory tree with branch connectors
 *
 * No terminal modes are changed: the console has no line discipline, so more
 * just reads one byte per key, exactly like the editor does.
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
#include <signal.h>
#include <time.h>
#include <ioctl.h>

/* ------------------------------------------------------------------- more */

static const char more_usage[] = "usage: more [FILE...]\nPage through FILE (or stdin) one screen at a time.\n";

static void more_prompt(void) {
    write(STDOUT_FILENO, "--More--", 8);
}

int cact_ub_more(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, more_usage, sizeof(more_usage) - 1);
        return 0;
    }

    char *acc = NULL;
    size_t acclen = 0;
    int i = 1;
    if (i >= argc) {
        acc = cact_ub_read_all(NULL, &acclen);
        if (!acc) { fprintf(stderr, "more: cannot read stdin\n"); return 1; }
    } else {
        for (; i < argc; i++) {
            size_t n = 0;
            char *b = cact_ub_read_all(argv[i], &n);
            if (!b) { fprintf(stderr, "more: %s: cannot open\n", argv[i]); free(acc); return 1; }
            char *na = (char *)realloc(acc, acclen + n + 1);
            if (!na) { free(b); free(acc); return 1; }
            acc = na;
            memcpy(acc + acclen, b, n);
            acclen += n;
            acc[acclen] = '\0';
            free(b);
        }
    }

    /* Not an interactive terminal: behave as a filter. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        cact_ub_write_all(STDOUT_FILENO, acc, acclen);
        free(acc);
        return 0;
    }

    int rows = 24;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
        rows = ws.ws_row;
    int page = rows - 1;

    /* Split into lines. */
    size_t cap = 128, nlines = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    if (!lines) { free(acc); return 1; }
    for (size_t p = 0; p < acclen; ) {
        if (nlines == cap) {
            cap *= 2;
            char **nl = (char **)realloc(lines, cap * sizeof(char *));
            if (!nl) { free(lines); free(acc); return 1; }
            lines = nl;
        }
        lines[nlines++] = acc + p;
        while (p < acclen && acc[p] != '\n') p++;
        if (p < acclen) p++;
    }

    size_t start = 0;
    while (start < nlines) {
        size_t end = start + (size_t)page;
        if (end > nlines) end = nlines;
        for (size_t k = start; k < end; k++) {
            char *e = lines[k];
            while (*e && *e != '\n') e++;
            cact_ub_write_all(STDOUT_FILENO, lines[k], (size_t)(e - lines[k]));
            write(STDOUT_FILENO, "\n", 1);
        }
        if (end >= nlines) break;

        more_prompt();
        char c = 0;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        write(STDOUT_FILENO, "\r        \r", 10);
        if (r <= 0) break;
        if (c == 'q' || c == 'Q') { start = nlines; break; }
        if (c == '\n' || c == '\r') start = (start + 1 <= nlines) ? start + 1 : nlines;
        else if (c == 'b' || c == 'B') start = (start > (size_t)page) ? start - page : 0;
        else start = end;
    }

    free(lines);
    free(acc);
    return 0;
}

/* -------------------------------------------------------------------- cal */

static const char cal_usage[] =
    "usage: cal [MONTH YEAR]\n"
    "       cal [YEAR]\n"
    "Print a calendar for the given month or year (default: this month).\n";

static const char *cal_months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int cal_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int cal_mdays(int y, int m) {
    static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && cal_leap(y)) return 29;
    return d[m - 1];
}

/* day of week for the 1st of month m (1-12) in year y; 0 = Sunday. */
static int cal_first_wday(int y, int m) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y;
    if (m < 3) yy -= 1;
    return (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + 1) % 7;
}

static void cal_print_month(int y, int m) {
    char title[48];
    int tl = snprintf(title, sizeof(title), "%s %d", cal_months[m - 1], y);
    int pad = (20 - tl) / 2;
    if (pad < 0) pad = 0;
    for (int k = 0; k < pad; k++) putchar(' ');
    printf("%s\n", title);
    printf("Su Mo Tu We Th Fr Sa\n");
    int wd = cal_first_wday(y, m);
    int days = cal_mdays(y, m);
    for (int k = 0; k < wd * 3; k++) putchar(' ');
    for (int d = 1; d <= days; d++) {
        printf("%2d", d);
        if ((wd + d) % 7 == 0) putchar('\n');
        else putchar(' ');
    }
    if ((wd + days) % 7 != 0) putchar('\n');
    putchar('\n');
}

int cact_ub_cal(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, cal_usage, sizeof(cal_usage) - 1);
        return 0;
    }
    int month = -1, year = -1;
    if (argc == 2) year = atoi(argv[1]);
    else if (argc == 3) { month = atoi(argv[1]); year = atoi(argv[2]); }
    else if (argc > 3) { fprintf(stderr, "cal: too many arguments\n"); return 1; }

    if (year < 0) {
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        if (!tm) { fprintf(stderr, "cal: cannot read the clock\n"); return 1; }
        year = tm->tm_year + 1900;
        month = tm->tm_mon + 1;
    }
    if (year < 1 || year > 9999) { fprintf(stderr, "cal: bad year\n"); return 1; }

    if (month < 0) {
        for (int m = 1; m <= 12; m++) cal_print_month(year, m);
    } else {
        if (month < 1 || month > 12) { fprintf(stderr, "cal: bad month\n"); return 1; }
        cal_print_month(year, month);
    }
    return 0;
}

/* ------------------------------------------------------------------- tree */

static const char tree_usage[] =
    "usage: tree [-a] [-d] [-f] [-L LEVEL] [DIR]\n"
    "List DIR (default .) as a tree.  -a shows hidden entries, -d lists\n"
    "directories only, -f prints the full path of each entry, -L limits depth.\n";

typedef struct {
    int show_all;
    int dirs_only;
    int full_path;
    long maxdepth;
    unsigned long dirs;
    unsigned long files;
    /* Display prefix per depth, on the heap: the walk is iterative
     * (cact_ub_tree_walk), so tree keeps no prefix string per level on its own
     * 16 KiB stack either. */
    char **prefix;
    int   *last;
    int    cap;
} tree_ctx;

#define TREE_PREFIX_SZ 256

static int tree_reserve(tree_ctx *c, int depth) {
    int ncap, i;

    if (depth < c->cap) return 0;
    ncap = c->cap ? c->cap * 2 : 16;
    while (ncap <= depth) ncap *= 2;
    {
        char **np = realloc(c->prefix, (size_t)ncap * sizeof(char *));
        int   *nl = realloc(c->last,   (size_t)ncap * sizeof(int));
        if (!np || !nl) return -1;
        c->prefix = np;
        c->last   = nl;
    }
    for (i = c->cap; i < ncap; i++) {
        c->prefix[i] = malloc(TREE_PREFIX_SZ);
        if (!c->prefix[i]) return -1;
        c->prefix[i][0] = '\0';
        c->last[i] = 0;
    }
    c->cap = ncap;
    return 0;
}

static void tree_free(tree_ctx *c) {
    int i;
    for (i = 0; i < c->cap; i++) free(c->prefix[i]);
    free(c->prefix);
    free(c->last);
    c->prefix = NULL;
    c->last = NULL;
    c->cap = 0;
}

/* Hidden entries stay hidden unless -a; -d leaves files out.  Filtering here
 * (rather than while printing) is what puts the "`--" on the last *shown*
 * entry. */
static int tree_sel(const char *name, const struct stat *st, void *ud) {
    const tree_ctx *c = (const tree_ctx *)ud;
    if (!c->show_all && name[0] == '.') return 0;
    if (c->dirs_only && !S_ISDIR(st->st_mode)) return 0;
    return 1;
}

static int tree_enter(const char *full, const char *name, int depth, int is_last,
                      const struct stat *st, void *ud) {
    tree_ctx *c = (tree_ctx *)ud;
    int isdir = S_ISDIR(st->st_mode);

    if (tree_reserve(c, depth) != 0) return 1;
    /* This level's prefix = the parent's prefix + the parent's own "last" mark. */
    if (depth <= 1) c->prefix[depth][0] = '\0';
    else snprintf(c->prefix[depth], TREE_PREFIX_SZ, "%s%s",
                  c->prefix[depth - 1], c->last[depth - 1] ? "    " : "|   ");
    c->last[depth] = is_last;

    printf("%s%s%s\n", c->prefix[depth], is_last ? "`-- " : "|-- ",
           c->full_path ? full : name);
    if (isdir) c->dirs++; else c->files++;
    return 0;
}

int cact_ub_tree(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, tree_usage, sizeof(tree_usage) - 1);
        return 0;
    }
    tree_ctx c = { 0, 0, 0, -1, 0, 0, NULL, NULL, 0 };
    const char *dir = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (strcmp(a, "--") == 0) { i++; break; }
            if (strcmp(a, "-a") == 0) c.show_all = 1;
            else if (strcmp(a, "-d") == 0) c.dirs_only = 1;
            else if (strcmp(a, "-f") == 0) c.full_path = 1;
            else if (strcmp(a, "-L") == 0 && i + 1 < argc) c.maxdepth = atol(argv[++i]);
            else if (strncmp(a, "-L", 2) == 0 && a[2]) c.maxdepth = atol(a + 2);
            else { fprintf(stderr, "tree: unknown option %s\n", a); return 1; }
        } else {
            dir = a;
        }
    }
    if (!dir) dir = ".";
    if (!cact_ub_is_dir(dir)) { fprintf(stderr, "tree: %s: not a directory\n", dir); return 1; }

    printf("%s\n", dir);
    if (tree_reserve(&c, 0) != 0) { fprintf(stderr, "tree: out of memory\n"); return 1; }
    c.prefix[0][0] = '\0';
    c.last[0] = 1;
    {
        cact_ub_walk_opts_t o;
        o.max_depth = (c.maxdepth >= 0) ? (int)c.maxdepth : -1;
        o.sel       = tree_sel;
        o.sort      = 1;
        (void)cact_ub_tree_walk(dir, 0, &o, tree_enter, NULL, &c);
    }
    printf("\n%lu directories, %lu files\n", c.dirs, c.files);
    tree_free(&c);
    return 0;
}
