/*
 * ex_math.c — condition, expression and integer utilities.
 *
 *   test    POSIX-ish conditional evaluator (also works as "[ EXPR ]")
 *   expr    expression evaluator: integer arithmetic, string ops, comparisons
 *   factor  prime factorisation of the numbers given (or read from stdin)
 *
 * No floating point: everything works on long integers.
 */

#include "common/ex_util.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stat.h>
#include <errno.h>
#include <stdint.h>

/* ------------------------------------------------------------------- test */

static const char test_usage[] =
    "usage: test EXPR\n"
    "  -e -f -d -s -r -w -x -L FILE   file tests\n"
    "  -z STR   -n STR                string is empty / non-empty\n"
    "  S1 = S2  S1 != S2              string comparison\n"
    "  N1 -eq -ne -lt -le -gt -ge N2  integer comparison\n"
    "  ! EXPR   EXPR -a EXPR   EXPR -o EXPR   ( EXPR )\n"
    "Exit status 0 if true, 1 if false, 2 on a syntax error.\n";

static int t_is_int(const char *s, long *out) {
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    *out = atol(s);
    return 1;
}

typedef struct { char **av; int ac; int pos; } tst;

static int tst_or(tst *t, int *err);

static int tst_file_test(const char *op, const char *file) {
    struct stat st;
    if (strcmp(op, "-e") == 0) return stat(file, &st) == 0;
    if (strcmp(op, "-s") == 0) return stat(file, &st) == 0 && st.st_size > 0;
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        return lstat(file, &st) == 0 && S_ISLNK(st.st_mode);
    }
    if (strcmp(op, "-r") == 0) return access(file, R_OK) == 0;
    if (strcmp(op, "-w") == 0) return access(file, W_OK) == 0;
    if (strcmp(op, "-x") == 0) return access(file, X_OK) == 0;
    if (stat(file, &st) != 0) return 0;
    if (strcmp(op, "-f") == 0) return S_ISREG(st.st_mode);
    if (strcmp(op, "-d") == 0) return S_ISDIR(st.st_mode);
    if (strcmp(op, "-p") == 0) return S_ISFIFO(st.st_mode);
    if (strcmp(op, "-b") == 0) return S_ISBLK(st.st_mode);
    if (strcmp(op, "-c") == 0) return S_ISCHR(st.st_mode);
    return 0;
}

static int tst_primary(tst *t, int *err) {
    if (t->pos >= t->ac) { *err = 1; return 0; }
    const char *a = t->av[t->pos];

    if (strcmp(a, "(") == 0) {
        t->pos++;
        int r = tst_or(t, err);
        if (*err) return 0;
        if (t->pos < t->ac && strcmp(t->av[t->pos], ")") == 0) t->pos++;
        else *err = 1;
        return r;
    }

    static const char *file_ops[] = { "-e", "-f", "-d", "-s", "-r", "-w", "-x",
                                      "-L", "-h", "-p", "-b", "-c" };
    for (size_t k = 0; k < sizeof(file_ops) / sizeof(file_ops[0]); k++) {
        if (strcmp(a, file_ops[k]) == 0) {
            if (t->pos + 1 >= t->ac) { *err = 1; return 0; }
            const char *f = t->av[t->pos + 1];
            t->pos += 2;
            return tst_file_test(a, f);
        }
    }
    if (strcmp(a, "-z") == 0 || strcmp(a, "-n") == 0) {
        if (t->pos + 1 >= t->ac) { *err = 1; return 0; }
        const char *s = t->av[t->pos + 1];
        t->pos += 2;
        size_t l = strlen(s);
        return strcmp(a, "-z") == 0 ? (l == 0) : (l != 0);
    }

    /* Binary operator: STR OP STR */
    if (t->pos + 2 <= t->ac - 1) {
        const char *op = t->av[t->pos + 1];
        const char *rhs = t->av[t->pos + 2];
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
            int eq = (strcmp(a, rhs) == 0);
            t->pos += 3;
            return strcmp(op, "!=") == 0 ? !eq : eq;
        }
        int numeric = strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
                      strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
                      strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0;
        if (numeric) {
            long x, y;
            if (!t_is_int(a, &x) || !t_is_int(rhs, &y)) { *err = 1; return 0; }
            t->pos += 3;
            if (strcmp(op, "-eq") == 0) return x == y;
            if (strcmp(op, "-ne") == 0) return x != y;
            if (strcmp(op, "-lt") == 0) return x < y;
            if (strcmp(op, "-le") == 0) return x <= y;
            if (strcmp(op, "-gt") == 0) return x > y;
            return x >= y;
        }
    }

    /* Single non-empty string. */
    t->pos++;
    return a[0] != '\0';
}

static int tst_not(tst *t, int *err) {
    if (t->pos < t->ac && strcmp(t->av[t->pos], "!") == 0) {
        t->pos++;
        int r = tst_not(t, err);
        return *err ? 0 : !r;
    }
    return tst_primary(t, err);
}

static int tst_and(tst *t, int *err) {
    int r = tst_not(t, err);
    if (*err) return 0;
    while (t->pos < t->ac && strcmp(t->av[t->pos], "-a") == 0) {
        t->pos++;
        int r2 = tst_not(t, err);
        if (*err) return 0;
        r = r && r2;
    }
    return r;
}

static int tst_or(tst *t, int *err) {
    int r = tst_and(t, err);
    if (*err) return 0;
    while (t->pos < t->ac && strcmp(t->av[t->pos], "-o") == 0) {
        t->pos++;
        int r2 = tst_and(t, err);
        if (*err) return 0;
        r = r || r2;
    }
    return r;
}

int cact_ub_test(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, test_usage, sizeof(test_usage) - 1);
        return 0;
    }
    int ac = argc;
    char **av = argv;
    if (argc >= 1 && argv[0] && strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        ac = argc - 1;
    }
    if (ac <= 1) return 1;             /* no expression: false */
    tst t = { av, ac, 1 };
    int err = 0;
    int r = tst_or(&t, &err);
    if (err || t.pos != ac) {
        fprintf(stderr, "test: invalid expression\n");
        return 2;
    }
    return r ? 0 : 1;
}

/* ------------------------------------------------------------------- expr */

static const char expr_usage[] =
    "usage: expr EXPR...\n"
    "  integer: + - * / %      comparison: = != < <= > >=\n"
    "  logic: | &             parentheses: ( )\n"
    "  strings: length S | substr S POS LEN | index S CHARS\n"
    "Exit status 0 if the result is neither empty nor 0, 1 if it is, 2 on error.\n";

typedef struct { int is_int; long i; const char *s; } val;

static val v_int(long v) { val r; r.is_int = 1; r.i = v; r.s = NULL; return r; }
static val v_str(const char *s) { val r; r.is_int = 0; r.i = 0; r.s = s; return r; }

static int ex_is_int(const char *s, long *out) {
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    *out = atol(s);
    return 1;
}

static int ex_true(val v) { return v.is_int ? (v.i != 0) : (v.s && v.s[0]); }

static val ex_or(char **av, int ac, int *pos, int *err);

static val ex_prim(char **av, int ac, int *pos, int *err) {
    if (*pos >= ac) { *err = 1; return v_int(0); }
    const char *a = av[(*pos)++];

    if (strcmp(a, "(") == 0) {
        val v = ex_or(av, ac, pos, err);
        if (*err) return v_int(0);
        if (*pos < ac && strcmp(av[*pos], ")") == 0) (*pos)++;
        else *err = 1;
        return v;
    }
    if (strcmp(a, "length") == 0) {
        if (*pos >= ac) { *err = 1; return v_int(0); }
        return v_int((long)strlen(av[(*pos)++]));
    }
    if (strcmp(a, "substr") == 0) {
        if (*pos + 2 >= ac) { *err = 1; return v_int(0); }
        const char *s = av[(*pos)++];
        long p = atol(av[(*pos)++]);
        long l = atol(av[(*pos)++]);
        long sl = (long)strlen(s);
        if (p < 1) p = 1;
        if (l < 0) l = 0;
        long start = p - 1;
        if (start > sl) start = sl;
        long avail = sl - start;
        if (l > avail) l = avail;
        char *out = (char *)malloc((size_t)l + 1);
        if (!out) { *err = 1; return v_int(0); }
        memcpy(out, s + start, (size_t)l);
        out[l] = '\0';
        return v_str(out);
    }
    if (strcmp(a, "index") == 0) {
        if (*pos + 1 >= ac) { *err = 1; return v_int(0); }
        const char *s = av[(*pos)++];
        const char *chars = av[(*pos)++];
        for (long k = 0; s[k]; k++)
            if (strchr(chars, s[k])) return v_int(k + 1);
        return v_int(0);
    }

    long iv;
    if (ex_is_int(a, &iv)) return v_int(iv);
    return v_str(a);
}

static val ex_mul(char **av, int ac, int *pos, int *err) {
    val l = ex_prim(av, ac, pos, err);
    while (!*err && *pos < ac) {
        const char *op = av[*pos];
        if (strcmp(op, "*") != 0 && strcmp(op, "/") != 0 && strcmp(op, "%") != 0) break;
        (*pos)++;
        val r = ex_prim(av, ac, pos, err);
        if (*err) break;
        if (!l.is_int || !r.is_int) { *err = 1; break; }
        if ((strcmp(op, "/") == 0 || strcmp(op, "%") == 0) && r.i == 0) {
            fprintf(stderr, "expr: division by zero\n");
            *err = 1; break;
        }
        if (strcmp(op, "*") == 0) l = v_int(l.i * r.i);
        else if (strcmp(op, "/") == 0) l = v_int(l.i / r.i);
        else l = v_int(l.i % r.i);
    }
    return l;
}

static val ex_add(char **av, int ac, int *pos, int *err) {
    val l = ex_mul(av, ac, pos, err);
    while (!*err && *pos < ac) {
        const char *op = av[*pos];
        if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0) break;
        (*pos)++;
        val r = ex_mul(av, ac, pos, err);
        if (*err) break;
        if (!l.is_int || !r.is_int) { *err = 1; break; }
        l = v_int(strcmp(op, "+") == 0 ? l.i + r.i : l.i - r.i);
    }
    return l;
}

static val ex_cmp(char **av, int ac, int *pos, int *err) {
    val l = ex_add(av, ac, pos, err);
    if (*err || *pos >= ac) return l;
    const char *op = av[*pos];
    if (strcmp(op, "=") != 0 && strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 &&
        strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 &&
        strcmp(op, ">") != 0 && strcmp(op, ">=") != 0) return l;
    (*pos)++;
    val r = ex_add(av, ac, pos, err);
    if (*err) return v_int(0);
    int t;
    if (l.is_int && r.is_int) {
        long x = l.i, y = r.i;
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) t = (x == y);
        else if (strcmp(op, "!=") == 0) t = (x != y);
        else if (strcmp(op, "<") == 0)  t = (x < y);
        else if (strcmp(op, "<=") == 0) t = (x <= y);
        else if (strcmp(op, ">") == 0)  t = (x > y);
        else                            t = (x >= y);
    } else {
        char lb[32], rb[32];
        const char *ls = l.s;
        const char *rs = r.s;
        if (!ls) { snprintf(lb, sizeof(lb), "%ld", l.i); ls = lb; }
        if (!rs) { snprintf(rb, sizeof(rb), "%ld", r.i); rs = rb; }
        int c = strcmp(ls, rs);
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) t = (c == 0);
        else if (strcmp(op, "!=") == 0) t = (c != 0);
        else if (strcmp(op, "<") == 0)  t = (c < 0);
        else if (strcmp(op, "<=") == 0) t = (c <= 0);
        else if (strcmp(op, ">") == 0)  t = (c > 0);
        else                            t = (c >= 0);
    }
    return v_int(t);
}

static val ex_and(char **av, int ac, int *pos, int *err) {
    val l = ex_cmp(av, ac, pos, err);
    while (!*err && *pos < ac && strcmp(av[*pos], "&") == 0) {
        (*pos)++;
        val r = ex_cmp(av, ac, pos, err);
        if (*err) break;
        if (!ex_true(l) || !ex_true(r)) l = v_int(0);
    }
    return l;
}

static val ex_or(char **av, int ac, int *pos, int *err) {
    val l = ex_and(av, ac, pos, err);
    while (!*err && *pos < ac && strcmp(av[*pos], "|") == 0) {
        (*pos)++;
        val r = ex_and(av, ac, pos, err);
        if (*err) break;
        if (!ex_true(l)) l = r;
    }
    return l;
}

int cact_ub_expr(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, expr_usage, sizeof(expr_usage) - 1);
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "expr: missing operand\n");
        return 2;
    }
    int pos = 1, err = 0;
    val r = ex_or(argv, argc, &pos, &err);
    if (err || pos != argc) {
        if (!err) fprintf(stderr, "expr: syntax error\n");
        return 2;
    }
    if (r.is_int) {
        printf("%ld\n", r.i);
        return r.i != 0 ? 0 : 1;
    }
    printf("%s\n", r.s ? r.s : "");
    return (r.s && r.s[0]) ? 0 : 1;
}

/* ----------------------------------------------------------------- factor */

static const char factor_usage[] =
    "usage: factor [NUMBER...]\n"
    "Print the prime factorisation of each NUMBER (read from stdin if none).\n";

static void factor_one(unsigned long long n) {
    printf("%llu:", n);
    if (n < 2) { printf("\n"); return; }
    unsigned long long x = n;
    for (unsigned long long d = 2; d <= x / d; ) {
        if (x % d == 0) {
            printf(" %llu", d);
            x /= d;
        } else {
            d = (d == 2) ? 3 : d + 2;
        }
    }
    if (x > 1) printf(" %llu", x);
    printf("\n");
}

int cact_ub_factor(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, factor_usage, sizeof(factor_usage) - 1);
        return 0;
    }
    if (argc >= 2) {
        for (int i = 1; i < argc; i++) factor_one(strtoull(argv[i], NULL, 10));
        return 0;
    }
    size_t len = 0;
    char *buf = cact_ub_read_all(NULL, &len);
    if (!buf) return 1;
    char *p = buf;
    while (p && *p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *end = p;
        while (*end && !isspace((unsigned char)*end)) end++;
        char save = *end;
        *end = '\0';
        factor_one(strtoull(p, NULL, 10));
        *end = save;
        p = end;
    }
    free(buf);
    return 0;
}
