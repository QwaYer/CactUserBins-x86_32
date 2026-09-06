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

    /* 2) SCM_RIGHTS fd passing over a socketpair */
    {
        int sp[2];
        r = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        CHECK(r == 0, "scm socketpair");
        if (r == 0) {
            int pf[2];
            CHECK(pipe(pf) == 0, "scm pipe");
            const char data[] = "HIFD";
            int sendfd = pf[1];
            unsigned char cbuf[CMSG_SPACE(sizeof(int))];
            struct iovec iov;
            struct msghdr mh;
            struct cmsghdr *c;

            iov.iov_base = (void *)data;
            iov.iov_len  = 4;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov        = &iov;
            mh.msg_iovlen     = 1;
            mh.msg_control    = cbuf;
            mh.msg_controllen = sizeof(cbuf);
            c = CMSG_FIRSTHDR(&mh);
            c->cmsg_len   = CMSG_LEN(sizeof(int));
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type  = SCM_RIGHTS;
            memcpy(CMSG_DATA(c), &sendfd, sizeof(sendfd));
            CHECK(sendmsg(sp[1], &mh, 0) == 4, "scm sendmsg data");

            char buf[8];
            unsigned char rbuf[CMSG_SPACE(4 * sizeof(int))];
            struct iovec riov;
            struct msghdr rmh;
            memset(buf, 0, sizeof(buf));
            riov.iov_base = buf;
            riov.iov_len  = sizeof(buf);
            memset(&rmh, 0, sizeof(rmh));
            rmh.msg_iov        = &riov;
            rmh.msg_iovlen     = 1;
            rmh.msg_control    = rbuf;
            rmh.msg_controllen = sizeof(rbuf);
            int got = (int)recvmsg(sp[0], &rmh, 0);
            CHECK(got == 4 && memcmp(buf, data, 4) == 0, "scm recvmsg data");
            int nfd = -1;
            for (c = CMSG_FIRSTHDR(&rmh); c; c = CMSG_NXTHDR(&rmh, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    memcpy(&nfd, CMSG_DATA(c), sizeof(nfd));
                    break;
                }
            }
            CHECK(nfd >= 0, "scm got fd");

            close(pf[1]);              /* drop the original write end */
            const char via[] = "viafd";
            if (nfd >= 0)
                CHECK(write(nfd, via, 5) == 5, "scm write via fd");
            {
                char r2[8];
                memset(r2, 0, sizeof(r2));
                CHECK(read(pf[0], r2, 5) == 5 && memcmp(r2, via, 5) == 0,
                      "scm pipe delivery");
            }
            if (nfd >= 0) close(nfd);
            close(pf[0]);
            close(sp[0]);
            close(sp[1]);
        }
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
