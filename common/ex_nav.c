/* pwd — standalone (cd remains cactsole builtin). */

#include <unistd.h>
#include <string.h>

int cact_ub_pwd(char **argv, int argc) {
    (void)argv;
    (void)argc;
    char buf[512];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}
