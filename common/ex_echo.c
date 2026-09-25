/* echo — standalone (shell keeps export/unset/env as builtins). */

#include <unistd.h>
#include <string.h>

static const char echo_usage[] =
    "usage: echo [-n] [-e] [-E] [ARG...]\n"
    "  -n no trailing newline   -e expand \\n \\t \\\\ \\r \\0NNN \\xHH\n";

static void echo_escaped(const char *s) {
    while (*s) {
        if (*s != '\\') {
            write(STDOUT_FILENO, s, 1);
            s++;
            continue;
        }
        s++;
        char c;
        switch (*s) {
            case 'n': c = '\n'; s++; break;
            case 't': c = '\t'; s++; break;
            case 'r': c = '\r'; s++; break;
            case 'a': c = '\a'; s++; break;
            case 'b': c = '\b'; s++; break;
            case 'f': c = '\f'; s++; break;
            case 'v': c = '\v'; s++; break;
            case '\\': c = '\\'; s++; break;
            case 'e': c = '\033'; s++; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                int v = 0, k = 0;
                while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; k++; }
                c = (char)v;
                break;
            }
            case 'x': {
                int v = 0, k = 0;
                s++;
                while (k < 2 && ((*s >= '0' && *s <= '9') ||
                                 (*s >= 'a' && *s <= 'f') ||
                                 (*s >= 'A' && *s <= 'F'))) {
                    int d = (*s <= '9') ? *s - '0'
                          : (*s >= 'a') ? *s - 'a' + 10 : *s - 'A' + 10;
                    v = v * 16 + d; s++; k++;
                }
                c = (char)v;
                break;
            }
            case '\0': return;
            default: c = '\\'; break;
        }
        write(STDOUT_FILENO, &c, 1);
    }
}

int cact_ub_echo(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, echo_usage, sizeof(echo_usage) - 1);
        return 0;
    }
    int newline = 1, escapes = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        int all_flags = 1;
        for (const char *p = a + 1; *p; p++)
            if (*p != 'n' && *p != 'e' && *p != 'E') { all_flags = 0; break; }
        if (!all_flags) break;
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'n') newline = 0;
            else if (*p == 'e') escapes = 1;
            else if (*p == 'E') escapes = 0;
        }
    }

    int first = 1;
    for (; i < argc; i++) {
        if (!first) write(STDOUT_FILENO, " ", 1);
        first = 0;
        if (escapes) echo_escaped(argv[i]);
        else write(STDOUT_FILENO, argv[i], strlen(argv[i]));
    }
    if (newline) write(STDOUT_FILENO, "\n", 1);
    return 0;
}
