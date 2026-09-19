/* pwd — standalone (cd remains cactsole builtin). */

#include <unistd.h>
#include <string.h>

static const char pwd_usage[] = "usage: pwd\n";

int cact_ub_pwd(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, pwd_usage, sizeof(pwd_usage) - 1);
        return 0;
    }
    (void)argv;
    (void)argc;
    char buf[512];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}
