/*
 * ex_dd.c — dd: блоковый копировальщик для CactOS.
 *
 *   dd if=/dev/zero of=/dev/sda1 bs=1M count=64
 *   dd if=/dev/random of=/dev/sda1 bs=512 count=1024 conv=notrunc
 *
 * Словарь опций GNU-dd (подмножество): if= of= bs= ibs= obs= count= skip=
 * seek= conv=notrunc status=none.  skip/seek измеряются в блоках ibs/obs.
 *
 * Работает с блочными узлами CactOS (/dev/sda1 — простой байтовый узел,
 * vfsdev), с обычными файлами и с символьными узлами /dev/zero,/dev/random.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

static unsigned long long dd_parse_num(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long long)(*s - '0');
        s++;
    }
    unsigned long long mult = 1;
    switch (*s) {
        case 'k': case 'K': mult = 1024ULL; break;
        case 'm': case 'M': mult = 1024ULL * 1024ULL; break;
        case 'g': case 'G': mult = 1024ULL * 1024ULL * 1024ULL; break;
        default: break;
    }
    return v * mult;
}

static void dd_usage(void) {
    fprintf(stderr,
            "usage: dd [OPERAND]...\n"
            "  if=FILE     input file        (default stdin)\n"
            "  of=FILE     output file       (default stdout)\n"
            "  bs=N        block size (bytes, K/M/G suffixes ok)\n"
            "  ibs=N obs=N input/output block sizes\n"
            "  count=N     copy N input blocks\n"
            "  skip=N      skip N input blocks\n"
            "  seek=N      seek N output blocks\n"
            "  conv=notrunc  do not truncate the output file\n"
            "  status=none suppress the final summary\n");
}

int cact_ub_dd(char **argv, int argc) {
    const char *ifile = NULL;
    const char *ofile = NULL;
    unsigned long long bsv = 0, ibs = 0, obs = 0;
    unsigned long long count = 0, skip = 0, seek = 0;
    int have_count = 0;
    int quiet = 0, notrunc = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *eq = strchr(a, '=');
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ||
            strcmp(a, "help") == 0) {
            dd_usage();
            return 0;
        }
        if (!eq) {
            dd_usage();
            return 1;
        }
        char key[24];
        int klen = (int)(eq - a);
        if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
        memcpy(key, a, (size_t)klen);
        key[klen] = '\0';
        const char *val = eq + 1;

        if      (strcmp(key, "if") == 0)         ifile = val;
        else if (strcmp(key, "of") == 0)         ofile = val;
        else if (strcmp(key, "bs") == 0)         bsv = dd_parse_num(val);
        else if (strcmp(key, "ibs") == 0)        ibs = dd_parse_num(val);
        else if (strcmp(key, "obs") == 0)        obs = dd_parse_num(val);
        else if (strcmp(key, "count") == 0)      { count = dd_parse_num(val); have_count = 1; }
        else if (strcmp(key, "skip") == 0)       skip = dd_parse_num(val);
        else if (strcmp(key, "seek") == 0)       seek = dd_parse_num(val);
        else if (strcmp(key, "status") == 0)     { if (strcmp(val, "none") == 0) quiet = 1; }
        else if (strcmp(key, "conv") == 0) {
            if (strstr(val, "notrunc")) notrunc = 1;
        } else {
            fprintf(stderr, "dd: unknown operand %s\n", a);
            dd_usage();
            return 1;
        }
    }

    if (bsv) { ibs = bsv; obs = bsv; }
    if (!ibs) ibs = 512;
    if (!obs) obs = 512;

    if (ibs > 16u * 1024u * 1024u || obs > 16u * 1024u * 1024u) {
        fprintf(stderr, "dd: block size too large\n");
        return 1;
    }
    if (!ibs || !obs) {
        fprintf(stderr, "dd: bad block size\n");
        return 1;
    }

    int in  = STDIN_FILENO;
    int out = STDOUT_FILENO;
    int close_in = 0, close_out = 0;

    if (ifile) {
        in = open(ifile, O_RDONLY, 0);
        if (in < 0) {
            fprintf(stderr, "dd: cannot open input %s\n", ifile);
            return 1;
        }
        close_in = 1;
    }
    if (ofile) {
        int oflags = O_WRONLY | O_CREAT;
        if (!notrunc) oflags |= O_TRUNC;
        out = open(ofile, oflags, 0644);
        if (out < 0) {
            fprintf(stderr, "dd: cannot open output %s\n", ofile);
            if (close_in) close(in);
            return 1;
        }
        close_out = 1;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)(ibs > obs ? ibs : obs));
    if (!buf) {
        fprintf(stderr, "dd: out of memory\n");
        goto fail;
    }

    int rc = 0;
    unsigned long long full = 0, part = 0, total = 0;

    /* skip на входе: lseek если можно, иначе чтение-в-никуда */
    if (skip > 0) {
        off_t off = (off_t)(skip * ibs);
        if (in != STDIN_FILENO && off >= 0 && lseek(in, off, SEEK_SET) >= 0) {
            /* ok */
        } else {
            unsigned long long left = skip * ibs;
            while (left > 0) {
                int want = left > (unsigned long long)ibs
                               ? (int)ibs : (int)left;
                int n = read(in, buf, (size_t)want);
                if (n <= 0) break;
                left -= (unsigned long long)n;
            }
        }
    }

    /* seek на выходе: позиционируемся */
    if (seek > 0) {
        off_t off = (off_t)(seek * obs);
        if (off >= 0 && lseek(out, off, SEEK_SET) < 0) {
            fprintf(stderr, "dd: cannot seek output\n");
            rc = 1;
            goto done;
        }
    }

    {
        unsigned long long done = 0;
        while (!have_count || done < count) {
            int n = read(in, buf, (size_t)ibs);
            if (n < 0) {
                fprintf(stderr, "dd: input read error (errno=%d)\n", errno);
                rc = 1;
                break;
            }
            if (n == 0) break;
            if (n == (int)ibs) full++;
            else part++;

            int off = 0;
            while (off < n) {
                int w = write(out, buf + off, (size_t)(n - off));
                if (w < 0) {
                    fprintf(stderr, "dd: output write error (errno=%d)\n", errno);
                    rc = 1;
                    goto done;
                }
                if (w == 0) break;
                off += w;
            }
            total += (unsigned long long)n;
            done++;
            if (have_count && done >= count) break;
        }
    }

    if (!quiet) {
        fprintf(stderr, "%llu+%llu records in\n", full, part);
        fprintf(stderr, "%llu+%llu records out\n", full, part);
        fprintf(stderr, "%llu bytes copied\n", total);
    }

done:
    free(buf);
    if (close_in)  close(in);
    if (close_out) close(out);
    return rc;

fail:
    if (close_in)  close(in);
    if (close_out) close(out);
    return 1;
}
