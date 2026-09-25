/*
 * devtest — userspace checks for the Unix-shaped /dev:
 *
 *   - the VT family behind /dev/tty0..ttyN (index ioctls, active/inactive
 *     state, and a VT_ACTIVATE round trip);
 *   - a full pseudo-terminal round trip through /dev/ptmx + /dev/pts/<n>,
 *     including the "slave stays locked until the master unlocks it" rule.
 *
 * One line per check of the form `devtest: <what> = ok|FAIL`, ending with a
 * summary line, so a headless boot log can be grepped for the result.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <ioctl_abi.h>

static int g_pass;
static int g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                         \
            printf("devtest: %-24s = ok\n", name);                            \
        } else {                                                               \
            g_fail++;                                                         \
            printf("devtest: %-24s = FAIL (errno=%d)\n", name, errno);        \
        }                                                                      \
    } while (0)

static void build_pts_path(char *out, int n) {
    const char *pfx = "/dev/pts/";
    int i = 0;
    while (pfx[i]) { out[i] = pfx[i]; i++; }
    char num[12];
    int k = 0;
    if (n == 0) num[k++] = '0';
    while (n > 0) { num[k++] = (char)('0' + n % 10); n /= 10; }
    while (k > 0) out[i++] = num[--k];
    out[i] = '\0';
}

static void test_vt(void) {
    int fd0 = open("/dev/tty0", O_RDWR);
    CHECK("open /dev/tty0", fd0 >= 0);
    if (fd0 < 0) return;

    int idx = -1;
    CHECK("tty0 index == 0",
          ioctl(fd0, CACT_TTYCTL_GET_INDEX, &idx) == 0 && idx == 0);

    cact_vt_state_t st;
    memset(&st, 0, sizeof(st));
    int st_ok = (ioctl(fd0, CACT_TTYCTL_VT_GETSTATE, &st) == 0);
    CHECK("VT_GETSTATE", st_ok);
    if (st_ok)
        printf("devtest: vt state active=%u count=%u\n", st.v_active, st.v_count);

    int fd2 = open("/dev/tty2", O_RDWR);
    CHECK("open /dev/tty2", fd2 >= 0);
    if (fd2 >= 0) {
        idx = -1;
        CHECK("tty2 index == 2",
              ioctl(fd2, CACT_TTYCTL_GET_INDEX, &idx) == 0 && idx == 2);
        close(fd2);
    }

    if (st_ok && st.v_active != 2) {
        /* Output written while VT 2 is active lands on VT 2 (and not on the
         * serial mirror), so run the checks first and report them only after
         * switching back to the original terminal. */
        int target = 2;
        int act_rc = ioctl(fd0, CACT_TTYCTL_VT_ACTIVATE, &target);
        cact_vt_state_t st2;
        memset(&st2, 0, sizeof(st2));
        ioctl(fd0, CACT_TTYCTL_VT_GETSTATE, &st2);
        int became2 = (st2.v_active == 2);

        int back = st.v_active;
        int back_rc = ioctl(fd0, CACT_TTYCTL_VT_ACTIVATE, &back);

        CHECK("VT_ACTIVATE(2)", act_rc == 0);
        CHECK("active became 2", became2);
        CHECK("VT_ACTIVATE(back)", back_rc == 0);
    }

    close(fd0);
}

static void test_pty(void) {
    int m = open("/dev/ptmx", O_RDWR);
    CHECK("open /dev/ptmx", m >= 0);
    if (m < 0) return;

    int ptn = -1;
    CHECK("PTYCTL_GET_NUMBER",
          ioctl(m, CACT_PTYCTL_GET_NUMBER, &ptn) == 0 && ptn >= 0);
    if (ptn < 0) { close(m); return; }
    printf("devtest: pts number = %d\n", ptn);

    char path[32];
    build_pts_path(path, ptn);

    /* Slave is locked until the master releases it: open() still yields an fd
     * (the kernel has no per-open failure for device nodes), but I/O on it must
     * fail rather than connect. */
    int sl = open(path, O_RDWR);
    CHECK("open locked slave", sl >= 0);
    if (sl >= 0) {
        int w = write(sl, "x", 1);
        CHECK("locked slave write fails", w < 0);
        close(sl);
    }

    int zero = 0;
    CHECK("PTYCTL_LOCK(0)", ioctl(m, CACT_PTYCTL_LOCK, &zero) == 0);

    int s = open(path, O_RDWR);
    CHECK("open unlocked slave", s >= 0);
    if (s >= 0) {
        char buf[8];

        memset(buf, 0, sizeof(buf));
        int w = write(m, "ping", 4);
        int r = (w == 4) ? read(s, buf, 4) : -1;
        CHECK("master -> slave", r == 4 && memcmp(buf, "ping", 4) == 0);
        printf("devtest: m2s '%s'\n", buf);

        memset(buf, 0, sizeof(buf));
        w = write(s, "pong", 4);
        r = (w == 4) ? read(m, buf, 4) : -1;
        CHECK("slave -> master", r == 4 && memcmp(buf, "pong", 4) == 0);
        printf("devtest: s2m '%s'\n", buf);

        close(s);
    }
    close(m);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    test_vt();
    test_pty();

    printf("devtest: summary pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
