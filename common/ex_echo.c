/* echo — standalone (shell keeps export/unset/env as builtins). */

#include <unistd.h>
#include <string.h>

int cact_ub_echo(char **argv, int argc) {
    int i;
    for (i = 1; i < argc; i++) {
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        if (i < argc - 1) write(STDOUT_FILENO, " ", 1);
    }
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
