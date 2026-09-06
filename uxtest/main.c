#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "socket.h"

/* uxtest — automated AF_UNIX smoke test.  Runs as /bin/init and reports
 * results through /dev/console (kernel printk -> serial). */

static int outfd = -1;
static int fail;

static void say(const char *s) {
    if (outfd < 0) return;
    write(outfd, s, strlen(s));
}

static void say_int(int v) {
    char b[16];
    int  i = 0;
    if (v < 0) { say("-"); v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) { char c = b[--i]; write(outfd, &c, 1); }
}

#define CHECK(cond, msg)                                        \
    do {                                                        \
        say(msg);                                               \
        if (cond) { say(": OK\n"); }                            \
        else      { say(": FAIL\n"); fail = 1; }                \
    } while (0)

int main(void) {
    int fd = open("/dev/console", O_WRONLY);
    outfd = (fd >= 0) ? fd : 1;
    say("\n== UXTEST AF_UNIX ==\n");

    /* 1) socketpair loopback */
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(r == 0, "socketpair create");
    if (r == 0) {
        const char ping[] = "ping";
        char buf[8];
        memset(buf, 0, sizeof(buf));
        CHECK(write(sv[1], ping, 4) == 4, "socketpair write");
        int n = read(sv[0], buf, 4);
        CHECK(n == 4 && memcmp(buf, ping, 4) == 0, "socketpair echo");
        close(sv[0]);
        close(sv[1]);
    }

    /* 2) pathname server, same-process loopback */
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/tmp/ux.sock");

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(srv >= 0, "server socket");
    r = bind(srv, (struct sockaddr *)&sun, sizeof(sun));
    if (r != 0) { say(" bind r="); say_int(r); say(" errno="); say_int(errno); say("\n"); }
    CHECK(r == 0, "server bind");
    r = listen(srv, 4);
    if (r != 0) { say(" listen r="); say_int(r); say(" errno="); say_int(errno); say("\n"); }
    CHECK(r == 0, "server listen");

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(cli >= 0, "client socket");
    r = connect(cli, (struct sockaddr *)&sun, sizeof(sun));
    if (r != 0) { say(" connect r="); say_int(r); say(" errno="); say_int(errno); say("\n"); }
    CHECK(r == 0, "client connect");
    int acc = accept(srv, 0, 0);
    if (acc < 0) { say(" accept r="); say_int(acc); say(" errno="); say_int(errno); say("\n"); }
    CHECK(acc >= 0, "server accept");

    if (acc >= 0) {
        const char abc[] = "abc";
        const char xyz[] = "xyz";
        char buf[8];
        memset(buf, 0, sizeof(buf));
        CHECK(write(cli, abc, 3) == 3, "c->s write");
        CHECK(read(acc, buf, 3) == 3 && memcmp(buf, abc, 3) == 0, "s recv");
        memset(buf, 0, sizeof(buf));
        CHECK(write(acc, xyz, 3) == 3, "s->c write");
        CHECK(read(cli, buf, 3) == 3 && memcmp(buf, xyz, 3) == 0, "c recv");

        shutdown(cli, SHUT_WR);
        int n = read(acc, buf, sizeof(buf));
        CHECK(n == 0, "s sees EOF");
        close(acc);
    }
    close(cli);
    close(srv);

    if (fail) say("UXFAIL\n");
    else      say("UXPASS\n");
    say("== UXTEST END ==\n");
    return fail;
}
