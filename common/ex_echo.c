/* echo — standalone (shell keeps export/unset/env as builtins). */

#include <unistd.h>
#include <string.h>

static const char echo_usage[] = "usage: echo [ARG...]\n";

int cact_ub_echo(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, echo_usage, sizeof(echo_usage) - 1);
        return 0;
    }
    int i;
    for (i = 1; i < argc; i++) {
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        if (i < argc - 1) write(STDOUT_FILENO, " ", 1);
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
