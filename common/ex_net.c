/*
 * builtins/net.c — simplified cactsole network commands.
 *
 * Only a basic TCP client is kept:
 *   nconn IP PORT [TEXT...]
 *   net   IP PORT [TEXT...]   (short alias)
 *
 * The complex low-level commands (nsock/nopt/nlisten/nudp) have been removed,
 * to keep the set minimal and easy to understand.
 */


#include <stdint.h>
#include <stddef.h>

#include <socket.h>
#include <tls.h>
#include <nodeio.h>
#include <ioctl_abi.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  Output utilities                                                          */
/* ────────────────────────────────────────────────────────────────────────── */

static void w(const char *s)  { write(STDOUT_FILENO, s, strlen((char *)s)); }
static void we(const char *s) { write(STDERR_FILENO, s, strlen((char *)s)); }

static void wn(int v)  { char b[16]; itoa(v, b); w(b); }
static void wne(int v) { char b[16]; itoa(v, b); we(b); }

/* Print IPv4 in host byte order (most significant octet in bits 24..31). */
static void print_ipv4_h(uint32_t ip) {
    char b[8];
    int p[4] = {(int)((ip >> 24) & 0xFF),
                (int)((ip >> 16) & 0xFF),
                (int)((ip >>  8) & 0xFF),
                (int)( ip        & 0xFF)};
    for (int i = 0; i < 4; i++) {
        itoa(p[i], b); w(b);
        if (i < 3) w(".");
    }
}

/* Parser for "10.0.2.15" -> host byte order. 0 = OK, -1 = error. */
static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t r = 0;
    int dots = 0, val = 0, has_digit = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return -1;
            has_digit = 1;
        } else if (*s == '.') {
            if (!has_digit) return -1;
            r = (r << 8) | (uint32_t)val;
            val = 0; has_digit = 0;
            if (++dots > 3) return -1;
        } else {
            return -1;
        }
        s++;
    }
    if (!has_digit || dots != 3) return -1;
    r = (r << 8) | (uint32_t)val;
    *out = r;
    return 0;
}

static int parse_port(const char *s, uint16_t *out) {
    int v = 0;
    if (!*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
        s++;
    }
    *out = (uint16_t)v;
    return 0;
}

static void perr(const char *who, const char *what, int rc) {
    we(who); we(": "); we(what); we(": rc=");
    wne(rc); we("\n");
}

static void fill_sin(struct sockaddr_in *a, uint32_t ip_h, uint16_t port_h) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port   = htons(port_h);
    a->sin_addr   = htonl(ip_h);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  nsock [tcp|udp]  — bare SYS_SOCKET demonstration                          */
/* ────────────────────────────────────────────────────────────────────────── */

/* ────────────────────────────────────────────────────────────────────────── */
/*  nconn IP PORT [TEXT...]  — simple TCP client                              */
/* ────────────────────────────────────────────────────────────────────────── */

static const char nconn_usage[] =
    "usage: nconn HOST PORT [TEXT...]\n"
    "       HOST — an address or a name (DNS)\n"
    "       sends TEXT (or 'PING\\n' by default) and reads the response\n";

int cact_ub_nconn(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        w(nconn_usage);
        return 0;
    }
    if (argc < 3) {
        we(nconn_usage);
        return 1;
    }
    uint32_t ip;
    uint16_t port;
    if (parse_ipv4(argv[1], &ip) != 0) {
        w("nconn: resolve "); w(argv[1]); w(" ... ");
        if (dns_resolve(argv[1], &ip) != 0) { we("failed\n"); return 1; }
        print_ipv4_h(ip); w("\n");
    }
    if (parse_port(argv[2], &port) < 0) { we("nconn: bad port\n"); return 1; }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { perr("nconn", "socket", fd); return 1; }

    struct sockaddr_in dst;
    fill_sin(&dst, ip, port);

    w("nconn: connect "); print_ipv4_h(ip); w(":"); wn(port); w(" ... ");
    int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (rc < 0) {
        w("FAILED rc="); wn(rc); w("\n");
        int err = 0; uint32_t l = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0) {
            w("nconn: SO_ERROR="); wn(err); w("\n");
        }
        close(fd);
        return 1;
    }
    w("OK\n");

    char  outbuf[1024];
    int   outlen = 0;
    if (argc >= 4) {
        for (int i = 3; i < argc; i++) {
            int l = strlen(argv[i]);
            if (outlen + l + 2 > (int)sizeof(outbuf)) break;
            memcpy(outbuf + outlen, argv[i], l); outlen += l;
            if (i < argc - 1) outbuf[outlen++] = ' ';
        }
        outbuf[outlen++] = '\n';
    } else {
        const char *p = "PING\n";
        outlen = strlen(p);
        memcpy(outbuf, p, outlen);
    }

    int sent = send(fd, outbuf, (uint32_t)outlen, 0);
    w("nconn: send -> "); wn(sent); w(" bytes\n");

    char in[1024];
    int n = recv(fd, in, sizeof(in) - 1, 0);
    if (n < 0) {
        perr("nconn", "recv", n);
    } else if (n == 0) {
        w("nconn: peer closed without data\n");
    } else {
        in[n] = '\0';
        w("nconn: recv "); wn(n); w(" bytes:\n---\n");
        write(STDOUT_FILENO, in, (size_t)n);
        if (in[n - 1] != '\n') w("\n");
        w("---\n");
    }

    close(fd);
    return (n < 0) ? 1 : 0;
}

static const char net_usage[] = "usage: net IP PORT [TEXT...]\n";

int cact_ub_net(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        w(net_usage);
        return 0;
    }
    if (argc < 3) {
        we(net_usage);
        return 1;
    }
    return cact_ub_nconn(argv, argc);
}

static const char ping_usage[] =
    "usage: ping [-c COUNT] [-i SEC] [-W MS] HOST\n"
    "       HOST — an address or a name (DNS); -c 0 — until Ctrl-C\n"
    "       -W MS — how long to wait for the response (2000 by default)\n";

/* Prints a value in milliseconds with three digits after the decimal point. */
static void w_ms_from_us(uint32_t us) {
    char b[4];
    uint32_t frac = us % 1000u;
    wn((int)(us / 1000u));
    w(".");
    b[0] = (char)('0' + (int)((frac / 100u) % 10u));
    b[1] = (char)('0' + (int)((frac / 10u) % 10u));
    b[2] = (char)('0' + (int)(frac % 10u));
    b[3] = '\0';
    w(b);
}

int cact_ub_ping(char **argv, int argc) {
    const char *host = NULL;
    int count = 4, interval_s = 1, timeout_ms = 2000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
            if (count < 0) count = 0;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atoi(argv[++i]);
            if (interval_s < 0) interval_s = 0;
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
            if (timeout_ms <= 0) timeout_ms = 2000;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            w(ping_usage);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            we("ping: unknown option "); we(argv[i]); we("\n");
            we(ping_usage);
            return 1;
        } else if (!host) {
            host = argv[i];
        }
    }
    if (!host) {
        we(ping_usage);
        return 1;
    }

    uint32_t ip_h;
    if (parse_ipv4(host, &ip_h) != 0) {
        w("ping: resolve "); w(host); w(" ... ");
        if (dns_resolve(host, &ip_h) != 0) {
            we("failed\n");
            return 1;
        }
        print_ipv4_h(ip_h);
        w("\n");
    }

    w("PING "); w(host); w(" ("); print_ipv4_h(ip_h); w("): 56 data bytes\n");

    uint16_t id = (uint16_t)getpid();
    int sent = 0, recvd = 0;
    for (int seq = 1; count == 0 || seq <= count; seq++) {
        cact_ping_wait_arg_t a;
        memset(&a, 0, sizeof(a));
        a.dst_ip     = ip_h;
        a.id         = id;
        a.seq        = (uint32_t)seq;
        a.timeout_ms = (uint32_t)timeout_ms;

        int rtt_us = nio_dev_cmd("net", CACT_NETCTL_PING_WAIT, &a);
        sent++;
        if (rtt_us < 0) {
            w("Request timeout for icmp_seq "); wn(seq); w("\n");
        } else {
            recvd++;
            wn((int)a.bytes_out); w(" bytes from ");
            print_ipv4_h(a.src_ip_out);
            w(": icmp_seq="); wn(seq);
            w(" time="); w_ms_from_us((uint32_t)rtt_us); w(" ms\n");
        }
        if (count == 0 || seq < count) sleep(interval_s > 0 ? interval_s : 1);
    }

    w("\n--- "); w(host); w(" ping statistics ---\n");
    wn(sent); w(" packets transmitted, "); wn(recvd);
    w(" received, ");
    wn(sent ? (100 * (sent - recvd)) / sent : 0);
    w("% packet loss\n");
    return recvd > 0 ? 0 : 1;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  dhcp  — simple DHCP discover/offer client (UDP 68 -> 67)                */
/* ────────────────────────────────────────────────────────────────────────── */

#define DHCP_MAGIC 0x63825363u
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_PARAM_REQ 55
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_REQ_IP 50
#define DHCP_OPT_SUBNET 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_END 255

struct dhcp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  opts[312];
} __attribute__((packed));

static int dhcp_get_opt_u32_host(const uint8_t *opts, int opts_len, uint8_t key, uint32_t *out) {
    int i = 0;
    while (i < opts_len) {
        uint8_t t = opts[i++];
        if (t == DHCP_OPT_END) break;
        if (t == 0) continue;
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (i + l > opts_len) break;
        if (t == key && l >= 4) {
            uint32_t netv = ((uint32_t)opts[i] << 24) |
                            ((uint32_t)opts[i + 1] << 16) |
                            ((uint32_t)opts[i + 2] << 8) |
                            (uint32_t)opts[i + 3];
            *out = ntohl(netv);
            return 0;
        }
        i += l;
    }
    return -1;
}

static int dhcp_get_opt_u8(const uint8_t *opts, int opts_len, uint8_t key, uint8_t *out) {
    int i = 0;
    while (i < opts_len) {
        uint8_t t = opts[i++];
        if (t == DHCP_OPT_END) break;
        if (t == 0) continue;
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (i + l > opts_len) break;
        if (t == key && l >= 1) {
            *out = opts[i];
            return 0;
        }
        i += l;
    }
    return -1;
}

static uint32_t g_dns_ip_h = 0x08080808u;

static const char dhcp_usage[] =
    "usage: dhcp\n"
    "       DHCP discover/request, applies IP/netmask/gateway/DNS to the network card\n";

int cact_ub_dhcp(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        w(dhcp_usage);
        return 0;
    }
    (void)argv; (void)argc;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { perr("dhcp", "socket", fd); return 1; }

    struct sockaddr_in local;
    fill_sin(&local, 0, 68);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perr("dhcp", "bind(68)", -1);
        close(fd);
        return 1;
    }

    struct dhcp_hdr req;
    memset(&req, 0, sizeof(req));
    req.op = 1;
    req.htype = 1;
    req.hlen = 6;
    req.xid = htonl(((uint32_t)getpid() << 16) ^ 0xCA7CD00Du);
    req.flags = htons(0x8000); /* broadcast */
    req.magic = htonl(DHCP_MAGIC);

    int oi = 0;
    req.opts[oi++] = DHCP_OPT_MSG_TYPE; req.opts[oi++] = 1; req.opts[oi++] = 1; /* DISCOVER */
    req.opts[oi++] = DHCP_OPT_PARAM_REQ; req.opts[oi++] = 3;
    req.opts[oi++] = DHCP_OPT_SUBNET;
    req.opts[oi++] = DHCP_OPT_ROUTER;
    req.opts[oi++] = DHCP_OPT_DNS;
    req.opts[oi++] = DHCP_OPT_END;

    struct sockaddr_in bcast;
    fill_sin(&bcast, 0xFFFFFFFFu, 67);
    int sret = sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&bcast, sizeof(bcast));
    if (sret < 0) {
        perr("dhcp", "sendto(discover)", sret);
        close(fd);
        return 1;
    }
    w("dhcp: discover sent, waiting offer...\n");

    uint32_t yiaddr_h = 0, server_id = 0, subnet = 0, router = 0, dns = 0;
    uint32_t lease_s = 0, t1_s = 0, t2_s = 0;
    int got_offer = 0;

    for (int attempt = 0; attempt < 10; attempt++) {
        struct dhcp_hdr rep;
        struct sockaddr_in from;
        uint32_t fromlen = sizeof(from);
        int n = recvfrom(fd, &rep, sizeof(rep), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            sleep(1);
            continue;
        }
        if ((size_t)n < offsetof(struct dhcp_hdr, opts) + 4) continue;
        if (rep.op != 2) continue;
        if (rep.xid != req.xid) continue;
        if (ntohl(rep.magic) != DHCP_MAGIC) continue;

        int opts_len = n - (int)offsetof(struct dhcp_hdr, opts);
        uint8_t msg_type = 0;
        (void)dhcp_get_opt_u8(rep.opts, opts_len, DHCP_OPT_MSG_TYPE, &msg_type);
        if (msg_type != 2) continue; /* only DHCPOFFER */

        yiaddr_h = ntohl(rep.yiaddr);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_SERVER_ID, &server_id);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_SUBNET, &subnet);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_ROUTER, &router);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_DNS, &dns);
        got_offer = 1;
        break;
    }

    if (!got_offer) {
        we("dhcp: no offer received\n");
        close(fd);
        return 1;
    }

    w("dhcp: offer ip="); print_ipv4_h(yiaddr_h); w("\n");
    if (server_id) { w("dhcp: server="); print_ipv4_h(server_id); w("\n"); }

    /* REQUEST offered lease. */
    memset(&req, 0, sizeof(req));
    req.op = 1;
    req.htype = 1;
    req.hlen = 6;
    req.xid = htonl(((uint32_t)getpid() << 16) ^ 0xCA7CD00Du);
    req.flags = htons(0x8000);
    req.magic = htonl(DHCP_MAGIC);

    oi = 0;
    req.opts[oi++] = DHCP_OPT_MSG_TYPE; req.opts[oi++] = 1; req.opts[oi++] = 3; /* REQUEST */
    req.opts[oi++] = DHCP_OPT_REQ_IP; req.opts[oi++] = 4;
    {
        uint32_t req_ip_n = htonl(yiaddr_h);
        req.opts[oi++] = (uint8_t)((req_ip_n >> 24) & 0xFF);
        req.opts[oi++] = (uint8_t)((req_ip_n >> 16) & 0xFF);
        req.opts[oi++] = (uint8_t)((req_ip_n >> 8) & 0xFF);
        req.opts[oi++] = (uint8_t)(req_ip_n & 0xFF);
    }
    if (server_id) {
        uint32_t sid_n = htonl(server_id);
        req.opts[oi++] = DHCP_OPT_SERVER_ID; req.opts[oi++] = 4;
        req.opts[oi++] = (uint8_t)((sid_n >> 24) & 0xFF);
        req.opts[oi++] = (uint8_t)((sid_n >> 16) & 0xFF);
        req.opts[oi++] = (uint8_t)((sid_n >> 8) & 0xFF);
        req.opts[oi++] = (uint8_t)(sid_n & 0xFF);
    }
    req.opts[oi++] = DHCP_OPT_PARAM_REQ; req.opts[oi++] = 3;
    req.opts[oi++] = DHCP_OPT_SUBNET;
    req.opts[oi++] = DHCP_OPT_ROUTER;
    req.opts[oi++] = DHCP_OPT_DNS;
    req.opts[oi++] = DHCP_OPT_END;

    sret = sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&bcast, sizeof(bcast));
    if (sret < 0) {
        perr("dhcp", "sendto(request)", sret);
        close(fd);
        return 1;
    }
    w("dhcp: request sent, waiting ack...\n");

    int got_ack = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        struct dhcp_hdr rep;
        struct sockaddr_in from;
        uint32_t fromlen = sizeof(from);
        int n = recvfrom(fd, &rep, sizeof(rep), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            sleep(1);
            continue;
        }
        if ((size_t)n < offsetof(struct dhcp_hdr, opts) + 4) continue;
        if (rep.op != 2) continue;
        if (rep.xid != req.xid) continue;
        if (ntohl(rep.magic) != DHCP_MAGIC) continue;

        int opts_len = n - (int)offsetof(struct dhcp_hdr, opts);
        uint8_t msg_type = 0;
        (void)dhcp_get_opt_u8(rep.opts, opts_len, DHCP_OPT_MSG_TYPE, &msg_type);
        if (msg_type != 5) continue; /* DHCPACK */

        yiaddr_h = ntohl(rep.yiaddr);
        subnet = 0; router = 0; dns = 0; lease_s = 0; t1_s = 0; t2_s = 0;
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_SUBNET, &subnet);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_ROUTER, &router);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, DHCP_OPT_DNS, &dns);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, 51, &lease_s);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, 58, &t1_s);
        (void)dhcp_get_opt_u32_host(rep.opts, opts_len, 59, &t2_s);
        got_ack = 1;
        break;
    }

    if (!got_ack) {
        we("dhcp: no ack received\n");
        close(fd);
        return 1;
    }

    cact_netcfg_arg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ip_host       = yiaddr_h;
    cfg.netmask_host  = subnet ? subnet : 0xFFFFFF00u;
    cfg.gateway_host  = router;
    cfg.dns_host      = dns ? dns : g_dns_ip_h;
    int rc = nio_dev_cmd("net", CACT_NETCTL_NETCFG, &cfg);
    if (rc < 0) {
        we("dhcp: failed to apply config in kernel\n");
        close(fd);
        return 1;
    }
    g_dns_ip_h = cfg.dns_host;

    w("dhcp: applied ip="); print_ipv4_h(cfg.ip_host); w("\n");
    w("dhcp: mask="); print_ipv4_h(cfg.netmask_host); w("\n");
    if (cfg.gateway_host) { w("dhcp: gw="); print_ipv4_h(cfg.gateway_host); w("\n"); }
    if (cfg.dns_host) { w("dhcp: dns="); print_ipv4_h(cfg.dns_host); w("\n"); }
    if (lease_s) { w("dhcp: lease="); wn((int)lease_s); w("s\n"); }
    if (t1_s) { w("dhcp: t1="); wn((int)t1_s); w("s\n"); }
    if (t2_s) { w("dhcp: t2="); wn((int)t2_s); w("s\n"); }

    close(fd);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  dns HOST [DNS_IP]  — simple DNS A query (UDP/53)                         */
/* ────────────────────────────────────────────────────────────────────────── */

struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

static int dns_write_qname(uint8_t *dst, int cap, const char *host) {
    int p = 0;
    const char *s = host;
    while (*s) {
        const char *dot = s;
        int labellen = 0;
        while (*dot && *dot != '.') { dot++; labellen++; }
        if (labellen <= 0 || labellen > 63 || p + 1 + labellen >= cap) return -1;
        dst[p++] = (uint8_t)labellen;
        memcpy(dst + p, s, (size_t)labellen);
        p += labellen;
        s = (*dot == '.') ? dot + 1 : dot;
    }
    if (p >= cap) return -1;
    dst[p++] = 0;
    return p;
}

static int dns_skip_name(const uint8_t *pkt, int pkt_len, int off) {
    if (off < 0 || off >= pkt_len) return -1;
    while (off < pkt_len) {
        uint8_t c = pkt[off++];
        if (c == 0) return off;
        if ((c & 0xC0) == 0xC0) {
            if (off >= pkt_len) return -1;
            return off + 1; /* pointer: one more byte */
        }
        if ((int)c > pkt_len - off) return -1;
        off += c;
    }
    return -1;
}

static const char dns_usage[] = "usage: dns HOST [DNS_IP]\n";

int cact_ub_dns(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        w(dns_usage);
        return 0;
    }
    if (argc < 2) {
        we(dns_usage);
        return 1;
    }
    const char *host = argv[1];
    uint32_t dns_ip_h = g_dns_ip_h; /* default from last DHCP run */
    if (argc >= 3 && parse_ipv4(argv[2], &dns_ip_h) < 0) {
        we("dns: bad DNS_IP\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { perr("dns", "socket", fd); return 1; }

    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));
    struct dns_hdr *h = (struct dns_hdr *)pkt;
    h->id = htons((uint16_t)((getpid() ^ 0xBEEF) & 0xFFFF));
    h->flags = htons(0x0100); /* recursion desired */
    h->qdcount = htons(1);

    int off = sizeof(struct dns_hdr);
    int qname_len = dns_write_qname(pkt + off, (int)sizeof(pkt) - off, host);
    if (qname_len < 0) {
        we("dns: invalid host\n");
        close(fd);
        return 1;
    }
    off += qname_len;
    if (off + 4 > (int)sizeof(pkt)) {
        close(fd);
        return 1;
    }
    pkt[off++] = 0; pkt[off++] = 1; /* QTYPE A */
    pkt[off++] = 0; pkt[off++] = 1; /* QCLASS IN */

    struct sockaddr_in dst;
    fill_sin(&dst, dns_ip_h, 53);
    int sret = sendto(fd, pkt, (uint32_t)off, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (sret < 0) {
        perr("dns", "sendto", sret);
        close(fd);
        return 1;
    }

    for (int attempt = 0; attempt < 5; attempt++) {
        struct sockaddr_in from;
        uint32_t fromlen = sizeof(from);
        int n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            sleep(1);
            continue;
        }
        if (n < (int)sizeof(struct dns_hdr)) continue;
        struct dns_hdr *rh = (struct dns_hdr *)pkt;
        if (rh->id != h->id) continue;
        int qd = ntohs(rh->qdcount);
        int an = ntohs(rh->ancount);
        int roff = sizeof(struct dns_hdr);
        for (int i = 0; i < qd; i++) {
            roff = dns_skip_name(pkt, n, roff);
            if (roff < 0 || roff + 4 > n) { roff = -1; break; }
            roff += 4;
        }
        if (roff < 0) continue;

        int found = 0;
        for (int i = 0; i < an; i++) {
            roff = dns_skip_name(pkt, n, roff);
            if (roff < 0 || roff + 10 > n) break;
            uint16_t type = ((uint16_t)pkt[roff] << 8) | pkt[roff + 1];
            uint16_t cls  = ((uint16_t)pkt[roff + 2] << 8) | pkt[roff + 3];
            uint16_t rdlen = ((uint16_t)pkt[roff + 8] << 8) | pkt[roff + 9];
            roff += 10;
            if (roff + rdlen > n) break;
            if (type == 1 && cls == 1 && rdlen == 4) {
                uint32_t ip_h = ((uint32_t)pkt[roff] << 24) |
                                ((uint32_t)pkt[roff + 1] << 16) |
                                ((uint32_t)pkt[roff + 2] << 8) |
                                (uint32_t)pkt[roff + 3];
                w("dns: "); w(host); w(" -> ");
                print_ipv4_h(ip_h);
                w("\n");
                found = 1;
            }
            roff += rdlen;
        }
        close(fd);
        if (found) return 0;
        we("dns: no A records\n");
        return 1;
    }

    we("dns: timeout\n");
    close(fd);
    return 1;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  ip  — Linux-like utility for the addresses/routes of one network card    */
/*                                                                           */
/*    ip addr show [dev IF]     ip addr [dev IF]                             */
/*    ip addr add A[/P] dev IF  ip addr del A[/P] dev IF                     */
/*    ip addr flush [dev IF]                                                 */
/*    ip link show [dev IF]                                                  */
/*    ip route show                                                          */
/*    ip route add default via GW   ip route del default                     */
/*                                                                           */
/* Control goes through /dev/net (CACT_NETCTL_NETCFG / _GET).                */
/* ────────────────────────────────────────────────────────────────────────── */

#define IP_IFACE "eth0"

static int ip_prefix_to_mask(int prefix, uint32_t *mask) {
    if (prefix < 0 || prefix > 32) return -1;
    *mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    return 0;
}

static int ip_mask_to_prefix(uint32_t mask) {
    int n = 0;
    while (mask) {
        if (mask & 1) n++;
        mask >>= 1;
    }
    return n;
}

static void ip_print_mac(const uint8_t *mac) {
    char b[4];
    for (int i = 0; i < 6; i++) {
        uint8_t hi = (uint8_t)(mac[i] >> 4), lo = (uint8_t)(mac[i] & 0xF);
        b[0] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        b[1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
        b[2] = '\0';
        w(b);
        if (i < 5) w(":");
    }
}

/* Network number (host order) -> address in buf. */
static void ip_fmt_ipv4(uint32_t v, char *buf, int cap) {
    int p[4] = {(int)((v >> 24) & 0xFF), (int)((v >> 16) & 0xFF),
                (int)((v >> 8) & 0xFF), (int)(v & 0xFF)};
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i) buf[n++] = '.';
        char t[4];
        itoa(p[i], t);
        for (char *s = t; *s && n < cap - 1; s++) buf[n++] = *s;
    }
    buf[n] = '\0';
}

static int ip_get_cfg(cact_netcfg_get_t *g) {
    int rc = nio_dev_cmd("net", CACT_NETCTL_NETCFG_GET, g);
    if (rc < 0) {
        we("ip: NETCFG_GET failed rc=");
        wne(rc);
        we("\n");
        return -1;
    }
    return 0;
}

static int ip_set_cfg(const cact_netcfg_arg_t *a) {
    int rc = nio_dev_cmd("net", CACT_NETCTL_NETCFG, (void *)a);
    if (rc < 0) {
        we("ip: NETCFG failed rc=");
        wne(rc);
        we(" (need root?)\n");
        return -1;
    }
    return 0;
}

static int ip_show_addr(cact_netcfg_get_t *g, int show_link_only) {
    if (ip_get_cfg(g) < 0) return 1;

    w(IP_IFACE);
    w(": <");
    w(g->link_up ? "UP" : "DOWN");
    w("> mtu 1500\n");

    w("    link/ether ");
    if (g->link_up) {
        ip_print_mac(g->mac);
    } else {
        w("00:00:00:00:00:00");
    }
    w("\n");

    if (show_link_only) return 0;

    char buf[20];
    if (g->ip_host && g->netmask_host) {
        int prefix = ip_mask_to_prefix(g->netmask_host);
        uint32_t bcast = (g->ip_host & g->netmask_host) | ~g->netmask_host;
        w("    inet ");
        ip_fmt_ipv4(g->ip_host, buf, sizeof(buf));
        w(buf);
        w("/");
        itoa(prefix, buf);
        w(buf);
        w(" brd ");
        ip_fmt_ipv4(bcast, buf, sizeof(buf));
        w(buf);
        w(" scope global ");
        w(IP_IFACE);
        w("\n");
        if (g->gateway_host) {
            w("    default via ");
            ip_fmt_ipv4(g->gateway_host, buf, sizeof(buf));
            w(buf);
            w(" dev ");
            w(IP_IFACE);
            w("\n");
        }
        if (g->dns_host) {
            w("    dns ");
            ip_fmt_ipv4(g->dns_host, buf, sizeof(buf));
            w(buf);
            w("\n");
        }
    } else {
        w("    (no IPv4 address configured)\n");
    }
    return 0;
}

/* ip helpers: there is only one card (eth0), so "dev IF" is ignored when
 * parsing, but if a foreign card is named we complain. */
static int ip_warn_foreign_dev(char **argv, int argc) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "dev") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], IP_IFACE) != 0) {
                we("ip: unknown interface `");
                we(argv[i + 1]);
                we("` (only ");
                we(IP_IFACE);
                we(" exists)\n");
                return 1;
            }
        }
    }
    return 0;
}

static int ip_cmd_addr_show(char **argv, int argc) {
    if (ip_warn_foreign_dev(argv, argc)) return 1;
    cact_netcfg_get_t g;
    return ip_show_addr(&g, 0);
}

static int ip_cmd_link_show(char **argv, int argc) {
    if (ip_warn_foreign_dev(argv, argc)) return 1;
    cact_netcfg_get_t g;
    return ip_show_addr(&g, 1);
}

static int ip_parse_addr_prefix(const char *s, uint32_t *ip, int *prefix) {
    char tmp[32];
    int n = 0;
    const char *slash = 0;
    for (const char *p = s; *p && n < (int)sizeof(tmp) - 1; p++) {
        if (*p == '/') slash = tmp + n;
        tmp[n++] = *p;
    }
    tmp[n] = '\0';
    uint32_t a;
    if (parse_ipv4(tmp, &a) < 0) return -1;
    int pre = 32;
    if (slash) {
        pre = atoi(slash + 1);
        if (pre < 0 || pre > 32) return -1;
    }
    *ip = a;
    *prefix = pre;
    return 0;
}

static int ip_cmd_addr_add(char **argv, int argc) {
    if (argc < 1) {
        we("usage: ip addr add IP[/PREFIX] dev eth0\n");
        return 1;
    }
    uint32_t ip_h;
    int prefix;
    if (ip_parse_addr_prefix(argv[0], &ip_h, &prefix) < 0) {
        we("ip: bad address\n");
        return 1;
    }
    uint32_t mask;
    if (ip_prefix_to_mask(prefix, &mask) < 0) {
        we("ip: bad prefix\n");
        return 1;
    }
    cact_netcfg_get_t g;
    if (ip_get_cfg(&g) < 0) return 1;

    cact_netcfg_arg_t a;
    a.ip_host       = ip_h;
    a.netmask_host  = mask;
    a.gateway_host  = g.gateway_host;   /* routes are left untouched */
    a.dns_host      = g.dns_host;
    return ip_set_cfg(&a);
}

static int ip_cmd_addr_del(char **argv, int argc) {
    if (argc < 1) {
        we("usage: ip addr del IP[/PREFIX] dev eth0\n");
        return 1;
    }
    uint32_t ip_h;
    int prefix;
    if (ip_parse_addr_prefix(argv[0], &ip_h, &prefix) < 0) {
        we("ip: bad address\n");
        return 1;
    }
    cact_netcfg_get_t g;
    if (ip_get_cfg(&g) < 0) return 1;

    cact_netcfg_arg_t a;
    a.ip_host       = 0;                 /* remove the address */
    a.netmask_host  = 0;
    a.gateway_host  = g.gateway_host;
    a.dns_host      = g.dns_host;
    return ip_set_cfg(&a);
}

static int ip_cmd_addr_flush(char **argv, int argc) {
    (void)argv; (void)argc;
    cact_netcfg_arg_t a;
    a.ip_host       = 0;
    a.netmask_host  = 0;
    a.gateway_host  = 0;
    a.dns_host      = 0;
    return ip_set_cfg(&a);
}

static int ip_cmd_route_show(char **argv, int argc) {
    (void)argv; (void)argc;
    cact_netcfg_get_t g;
    if (ip_get_cfg(&g) < 0) return 1;
    char buf[20];
    if (g.gateway_host) {
        w("default via ");
        ip_fmt_ipv4(g.gateway_host, buf, sizeof(buf));
        w(buf);
        w(" dev ");
        w(IP_IFACE);
        w("\n");
    } else {
        w("default via <none> dev ");
        w(IP_IFACE);
        w("\n");
    }
    return 0;
}

static int ip_cmd_route_add(char **argv, int argc) {
    /* ip route add default via GW */
    if (argc < 3 || strcmp(argv[0], "default") != 0 ||
        strcmp(argv[1], "via") != 0) {
        we("usage: ip route add default via GW\n");
        return 1;
    }
    uint32_t gw;
    if (parse_ipv4(argv[2], &gw) < 0) {
        we("ip: bad gateway\n");
        return 1;
    }
    cact_netcfg_get_t g;
    if (ip_get_cfg(&g) < 0) return 1;
    cact_netcfg_arg_t a;
    a.ip_host       = g.ip_host;
    a.netmask_host  = g.netmask_host;
    a.gateway_host  = gw;
    a.dns_host      = g.dns_host;
    return ip_set_cfg(&a);
}

static int ip_cmd_route_del(char **argv, int argc) {
    if (argc < 1 || strcmp(argv[0], "default") != 0) {
        we("usage: ip route del default\n");
        return 1;
    }
    cact_netcfg_get_t g;
    if (ip_get_cfg(&g) < 0) return 1;
    cact_netcfg_arg_t a;
    a.ip_host       = g.ip_host;
    a.netmask_host  = g.netmask_host;
    a.gateway_host  = 0;
    a.dns_host      = g.dns_host;
    return ip_set_cfg(&a);
}

static const char ip_usage[] =
    "usage: ip [ addr | link | route ] ...\n"
    "  ip addr show [dev IF] | ip addr add IP[/P] dev IF | "
    "ip addr del IP[/P] dev IF | ip addr flush [dev IF]\n"
    "  ip link show [dev IF]\n"
    "  ip route show | ip route add default via GW | ip route del default\n";

int cact_ub_ip(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        w(ip_usage);
        return 0;
    }
    if (argc < 2) {
        we(ip_usage);
        return 1;
    }
    int base = 1;
    /* "ip -4 addr show" — we simply skip the options */
    while (base < argc && argv[base][0] == '-') base++;
    if (base >= argc) {
        we(ip_usage);
        return 1;
    }
    const char *obj = argv[base];
    int sub = base + 1;

    if (strcmp(obj, "addr") == 0 || strcmp(obj, "address") == 0) {
        if (sub >= argc || strcmp(argv[sub], "show") == 0) {
            return ip_cmd_addr_show(argv + sub + 1, argc - (sub + 1));
        }
        if (strcmp(argv[sub], "add") == 0) {
            return ip_cmd_addr_add(argv + sub + 1, argc - (sub + 1));
        }
        if (strcmp(argv[sub], "del") == 0) {
            return ip_cmd_addr_del(argv + sub + 1, argc - (sub + 1));
        }
        if (strcmp(argv[sub], "flush") == 0) {
            return ip_cmd_addr_flush(argv + sub + 1, argc - (sub + 1));
        }
        we(ip_usage);
        return 1;
    }

    if (strcmp(obj, "link") == 0) {
        if (sub >= argc || strcmp(argv[sub], "show") == 0) {
            return ip_cmd_link_show(argv + sub + 1, argc - (sub + 1));
        }
        we(ip_usage);
        return 1;
    }

    if (strcmp(obj, "route") == 0) {
        if (sub >= argc || strcmp(argv[sub], "show") == 0) {
            return ip_cmd_route_show(argv + sub + 1, argc - (sub + 1));
        }
        if (sub + 1 < argc && strcmp(argv[sub], "add") == 0) {
            return ip_cmd_route_add(&argv[sub + 1], argc - (sub + 1));
        }
        if (sub + 1 < argc && strcmp(argv[sub], "del") == 0) {
            return ip_cmd_route_del(&argv[sub + 1], argc - (sub + 1));
        }
        we(ip_usage);
        return 1;
    }

    we("ip: unknown object `");
    we(obj);
    we("`\n");
    we(ip_usage);
    return 1;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  nc  — simple netcat: bidirectional relay stdin <-> socket.                */
/*                                                                           */
/*    nc HOST PORT            — TCP client (connect and relay)               */
/*    nc -l [-p] PORT         — listen, accept one connection, relay         */
/*                                                                           */
/*  Implementation: after connect()/accept() the process forks: the child    */
/*  drives stdin -> socket, the parent drives socket -> stdout. This needs no */
/*  poll() on the sockets and works fine with blocking read().                */
/* ────────────────────────────────────────────────────────────────────────── */

static void nc_usage(void) {
    fprintf(stderr,
            "usage: nc HOST PORT\n"
            "       nc -l [-p PORT]\n"
            "  relays stdin to the network and the network to stdout\n");
}

static int nc_xfer_all(int fd, const char *buf, int len) {
    while (len > 0) {
        int n = write(fd, buf, (size_t)len);
        if (n < 0) return -1;
        if (n == 0) {
            struct pollfd p;
            p.fd = fd; p.events = POLLOUT; p.revents = 0;
            poll(&p, 1, -1);
            continue;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

static int nc_relay(int sock) {
    pid_t pid = fork();
    if (pid < 0) {
        we("nc: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        /* child: stdin -> socket */
        signal(SIGPIPE, SIG_IGN);
        char buf[1024];
        for (;;) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            if (nc_xfer_all(sock, buf, n) != 0) break;
        }
        shutdown(sock, SHUT_WR);
        _exit(0);
    }
    /* parent: socket -> stdout */
    signal(SIGPIPE, SIG_IGN);
    char buf[1024];
    int rc = 0;
    for (;;) {
        int n = read(sock, buf, sizeof(buf));
        if (n < 0) { rc = 1; break; }
        if (n == 0) break;
        if (nc_xfer_all(STDOUT_FILENO, buf, n) != 0) { rc = 1; break; }
    }
    kill(pid, SIGKILL);
    {
        int st;
        waitpid(pid, &st, 0);
    }
    close(sock);
    return rc;
}

int cact_ub_nc(char **argv, int argc) {
    int listen_mode = 0;
    const char *host = NULL;
    int port = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            listen_mode = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            nc_usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            we("nc: unknown option ");
            we(argv[i]);
            we("\n");
            nc_usage();
            return 1;
        } else if (!host) {
            host = argv[i];
        } else {
            port = atoi(argv[i]);
        }
    }

    if (listen_mode) {
        if (port < 0 || port > 65535) { nc_usage(); return 1; }
        int sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sfd < 0) { perr("nc", "socket", sfd); return 1; }
        int one = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in a;
        fill_sin(&a, 0, (uint16_t)port);
        if (bind(sfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            we("nc: bind port "); wn(port); we(" failed\n");
            close(sfd);
            return 1;
        }
        if (listen(sfd, 1) < 0) {
            we("nc: listen failed\n");
            close(sfd);
            return 1;
        }
        struct sockaddr_in peer;
        uint32_t peerlen = sizeof(peer);
        int cs = accept(sfd, (struct sockaddr *)&peer, &peerlen);
        close(sfd);
        if (cs < 0) { we("nc: accept failed\n"); return 1; }
        return nc_relay(cs);
    }

    if (!host || port < 0) { nc_usage(); return 1; }
    if (port > 65535) { we("nc: bad port\n"); return 1; }

    uint32_t ip;
    if (parse_ipv4(host, &ip) != 0 && dns_resolve(host, &ip) != 0) {
        we("nc: cannot resolve ");
        we(host);
        we("\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { perr("nc", "socket", fd); return 1; }
    struct sockaddr_in dst;
    fill_sin(&dst, ip, (uint16_t)port);
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        we("nc: connect to ");
        print_ipv4_h(ip);
        we(":");
        wn(port);
        we(" failed\n");
        close(fd);
        return 1;
    }
    return nc_relay(fd);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  wget  — micro HTTP client: GET over TCP, response body into a file.       */
/*                                                                           */
/*    wget [-o FILE] http[s]://HOST[:PORT][/PATH]                            */
/*                                                                           */
/*  Handles Content-Length, chunked and read-to-close bodies (HTTP/1.0) and    */
/*  simple redirects (Location as an absolute URL or a path).  HTTPS is done   */
/*  by libc: after connect() a TLS 1.3 session is opened with                  */
/*  cact_tls_connect(), the keys stay in the process, the kernel verifies the  */
/*  certificate chain against the system CA bundle                             */
/*  (/etc/ca-certificates.crt, else /lib/ca-certificates.crt), and data flows  */
/*  through cact_tls_read()/cact_tls_write().                                  */
/* ────────────────────────────────────────────────────────────────────────── */

#define W_UA "cact-wget/0.1"
#define W_REDIR_MAX 5

static int wget_parse_url(const char *s, char *host, size_t hostsz,
                          int *port, int *is_tls, char *path, size_t pathsz) {
    if (!s || !s[0]) return -1;
    int tls = 0;
    const char *p = s;
    if (strncmp(p, "https://", 8) == 0) {
        tls = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    size_t hn = 0;
    while (*p && *p != ':' && *p != '/' && hn < hostsz - 1)
        host[hn++] = *p++;
    host[hn] = '\0';
    if (hn == 0) return -1;

    *is_tls = tls;
    *port = tls ? 443 : 80;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        if (v <= 0 || v > 65535) return -1;
        *port = v;
    }
    if (*p == '/') {
        size_t pn = 0;
        while (*p && pn < pathsz - 1)
            path[pn++] = *p++;
        path[pn] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}

/* Default file name = the last path component (without ? and #). */
static void wget_default_name(const char *path, char *out, size_t outsz) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    size_t n = 0;
    for (const char *p = base; *p && *p != '?' && *p != '#' && n < outsz - 1; p++)
        out[n++] = *p;
    out[n] = '\0';
    if (n == 0) {
        strncpy(out, "index.html", outsz - 1);
        out[outsz - 1] = '\0';
    }
}

/* Transport: a plain socket or a TLS session on top of it.  The TLS keys live
   here, in the process; the kernel only provides primitives and certificate checks. */
typedef struct {
    int fd;
    cact_tls_t *tls;
} tr_t;

static int tr_read(tr_t *t, void *buf, int len) {
    if (t->tls) {
        long n = cact_tls_read(t->tls, buf, (size_t)len);
        return n < 0 ? -1 : (int)n;
    }
    return recv(t->fd, buf, (size_t)len, 0);
}

static int tr_write_all(tr_t *t, const char *buf, int len) {
    while (len > 0) {
        long n = t->tls ? cact_tls_write(t->tls, buf, (size_t)len)
                        : write(t->fd, buf, (size_t)len);
        if (n < 0) return -1;
        if (n == 0) {
            struct pollfd p;
            p.fd = t->fd; p.events = POLLOUT; p.revents = 0;
            poll(&p, 1, -1);
            continue;
        }
        buf += n;
        len -= (int)n;
    }
    return 0;
}

static void tr_close(tr_t *t) {
    if (!t) return;
    if (t->tls) { cact_tls_close(t->tls); t->tls = NULL; }
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
}

/* Slice buffer for the response: the body surplus is hidden behind the end of
   the headers.  The size is not fixed: on real sites the headers (CSP, cookies,
   report-to) easily outgrow any reasonable stack array — github
   returns more than two kilobytes — so the buffer lives on the heap. */
#define W_BUF_SIZE (16 * 1024)

typedef struct {
    tr_t *tr;
    unsigned char *buf;
    int cap;
    int start;
    int end;
} wbuf_t;

static int wbuf_fill(wbuf_t *b) {
    if (b->start < b->end) return 1;
    b->start = b->end = 0;
    int n = tr_read(b->tr, b->buf, b->cap);
    if (n <= 0) return n;
    b->end = n;
    return 1;
}

/* Pull in more data: move the unread part to the front and do a recv. */
static int wbuf_append(wbuf_t *b) {
    if (b->start > 0) {
        memmove(b->buf, b->buf + b->start, (size_t)(b->end - b->start));
        b->end -= b->start;
        b->start = 0;
    }
    if (b->end >= b->cap) return -1;   /* header too long */
    int n = tr_read(b->tr, b->buf + b->end, b->cap - b->end);
    if (n <= 0) return n;
    b->end += n;
    return 1;
}

static void wbuf_free(wbuf_t *b) { free(b->buf); b->buf = 0; }

/* Read a byte; 1 = ok, 0 = EOF, -1 = error. */
static int wbuf_get(wbuf_t *b, unsigned char *out) {
    if (b->start == b->end) {
        int rc = wbuf_fill(b);
        if (rc <= 0) return rc;
    }
    *out = b->buf[b->start++];
    return 1;
}

/* Read a line (up to \n), return the length without \r\n, -1 on error. */
static int wbuf_line(wbuf_t *b, char *out, int cap) {
    int n = 0;
    for (;;) {
        unsigned char c;
        int rc = wbuf_get(b, &c);
        if (rc <= 0) return -1;
        if (c == '\n') return n;
        if (c != '\r' && n < cap - 1) out[n++] = (char)c;
    }
}


/* ── download progress ─────────────────────────────────────────────────── */

static void wget_size_str(unsigned long long n, char *out, int cap)
{
    if (n >= (1ULL << 20))      snprintf(out, (size_t)cap, "%.1f MiB", (double)n / (double)(1ULL << 20));
    else if (n >= (1ULL << 10)) snprintf(out, (size_t)cap, "%.1f KiB", (double)n / (double)(1ULL << 10));
    else                        snprintf(out, (size_t)cap, "%llu B", n);
}

/* One progress line on stderr, redrawn via '\r'.  When the size
   is unknown (chunked or body until close), we only show the counter. */
static void wget_progress(const char *label, unsigned long long done, long total)
{
    char line[128], ds[24], ts[28], bar[27];

    wget_size_str(done, ds, (int)sizeof ds);
    if (total > 0) {
        unsigned long long t = (unsigned long long)total;
        int pct = (int)(done * 100ULL / t);
        if (pct > 100) pct = 100;
        int fill = pct * 24 / 100;
        for (int i = 0; i < 24; i++)
            bar[i] = (i < fill) ? '#' : (i == fill ? '>' : '.');
        bar[24] = '\0';
        wget_size_str(t, ts, (int)sizeof ts);
        snprintf(line, sizeof line, "\r%-18.18s [%s] %3d%%  %s / %s",
                 label, bar, pct, ds, ts);
    } else {
        snprintf(line, sizeof line, "\r%-18.18s %s", label, ds);
    }
    we(line);
    fflush(stderr);
}

/* Read N bytes of the body. */
static int wbuf_readn(wbuf_t *b, unsigned char *out, int n) {
    while (n > 0) {
        if (b->start == b->end) {
            int rc = wbuf_fill(b);
            if (rc <= 0) return -1;
        }
        int take = b->end - b->start;
        if (take > n) take = n;
        memcpy(out, b->buf + b->start, (size_t)take);
        b->start += take;
        out += take;
        n -= take;
    }
    return 0;
}

static int wget_write_all(int fd, const unsigned char *buf, int len) {
    while (len > 0) {
        int n = write(fd, buf, (size_t)len);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

static int wget_send_request(tr_t *tr, const char *host, int port, int tls,
                             const char *path) {
    char portstr[8];
    char req[1600];
    int n;
    if (port != (tls ? 443 : 80)) {
        itoa(port, portstr);
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, portstr, W_UA);
    } else {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, W_UA);
    }
    if (n <= 0 || n >= (int)sizeof(req)) return -1;
    if (tr_write_all(tr, req, n) != 0) return -1;
    return 0;
}

/* Read the response headers. Fills code/clen/chunked/location.
 * Right after the headers the buffered body remains. 0 = ok, -1 = error. */
static int wget_read_headers(wbuf_t *b, int *code, long *clen,
                             int *chunked, char *loc, int loccap) {
    *code = 0; *clen = -1; *chunked = 0; loc[0] = '\0';
    for (;;) {
        /* look for the end of the headers in the current buffer */
        int found = -1, term = 0;
        for (int i = b->start; i + 3 < b->end; i++) {
            if (b->buf[i] == '\r' && b->buf[i + 1] == '\n' &&
                b->buf[i + 2] == '\r' && b->buf[i + 3] == '\n') {
                found = i; term = 4; break;
            }
        }
        if (found < 0) {
            for (int i = b->start; i + 1 < b->end; i++) {
                if (b->buf[i] == '\n' && b->buf[i + 1] == '\n') {
                    found = i; term = 2; break;
                }
            }
        }
        if (found >= 0) {
            int p = b->start;
            int first = 1;
            while (p < found) {
                int q = p;
                while (q < found && b->buf[q] != '\n') q++;
                int len = q - p;
                if (len > 0 && b->buf[q - 1] == '\r') len--;
                if (len < 0) len = 0;
                if (first) {
                    /* "HTTP/1.x CODE ..." */
                    int sp = p;
                    while (sp < p + len && b->buf[sp] != ' ') sp++;
                    if (sp < p + len) {
                        const char *cp = (const char *)b->buf + sp + 1;
                        *code = atoi(cp);
                    }
                    first = 0;
                } else {
                    /* copy the line into a NUL-terminated buffer and parse it */
                    char tmp[256];
                    int tl = len < (int)sizeof(tmp) - 1 ? len
                                                         : (int)sizeof(tmp) - 1;
                    memcpy(tmp, b->buf + p, (size_t)tl);
                    tmp[tl] = '\0';
                    char *colonp = strchr(tmp, ':');
                    if (colonp) {
                        *colonp = '\0';
                        char *valp = colonp + 1;
                        while (*valp == ' ') valp++;
                        if (strcasecmp(tmp, "content-length") == 0) {
                            *clen = atol(valp);
                        } else if (strcasecmp(tmp, "transfer-encoding") == 0) {
                            if (strcasestr(valp, "chunked")) *chunked = 1;
                        } else if (strcasecmp(tmp, "location") == 0) {
                            strncpy(loc, valp, (size_t)loccap - 1);
                            loc[loccap - 1] = '\0';
                            int ll = (int)strlen(loc);
                            while (ll > 0 && (loc[ll - 1] == '\r' ||
                                              loc[ll - 1] == ' '))
                                loc[--ll] = '\0';
                        }
                    }
                }
                p = q + 1;
            }
            b->start = found + term;
            return 0;
        }
        int rc = wbuf_append(b);
        if (rc < 0) return -1;
        if (rc == 0) return -1;                  /* EOF before the end of the headers */
    }
}

/* Skip the CRLF after a chunk. */
static int wbuf_skip_crlf(wbuf_t *b) {
    unsigned char c;
    if (wbuf_get(b, &c) <= 0) return -1;
    if (c == '\r') {
        if (wbuf_get(b, &c) <= 0) return -1;
    }
    return 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int wget_save_body(wbuf_t *b, int outfd, int chunked, long clen,
                          unsigned long long *written, const char *label) {
    *written = 0;
    if (chunked) {
        for (;;) {
            char line[32];
            int ln = wbuf_line(b, line, sizeof(line));
            if (ln < 0) return -1;
            int sz = 0;
            for (int i = 0; i < ln; i++) {
                int v = hexval((unsigned char)line[i]);
                if (v < 0) break;
                if (sz > 0x0FFFFFFF) return -1;
                sz = sz * 16 + v;
            }
            if (sz == 0) break;
            unsigned char tmp[1024];
            while (sz > 0) {
                int take = sz > (int)sizeof(tmp) ? (int)sizeof(tmp) : sz;
                if (wbuf_readn(b, tmp, take) != 0) return -1;
                if (wget_write_all(outfd, tmp, take) != 0) return -1;
                *written += (unsigned long long)take;
                sz -= take;
                if (label && ((*written >> 14) != ((*written - (unsigned long long)take) >> 14)))
                    wget_progress(label, *written, -1);
            }
            if (wbuf_skip_crlf(b) != 0) return -1;
        }
        if (label) { wget_progress(label, *written, -1); we("\n"); }
        return 0;
    }

    if (clen == 0) return 0;                     /* body is empty */
    long total = clen;
    unsigned char tmp[1024];
    for (;;) {
        if (b->start == b->end) {
            int rc = wbuf_fill(b);
            if (rc < 0) return -1;
            if (rc == 0) break;
        }
        int take = b->end - b->start;
        if (clen >= 0 && take > clen) take = (int)clen;
        memcpy(tmp, b->buf + b->start, (size_t)take);
        b->start += take;
        if (wget_write_all(outfd, tmp, take) != 0) return -1;
        *written += (unsigned long long)take;
        if (label && ((*written >> 14) != ((*written - (unsigned long long)take) >> 14)))
            wget_progress(label, *written, total);
        if (clen >= 0) {
            clen -= take;
            if (clen == 0) break;
        }
    }
    if (label) { wget_progress(label, *written, total); we("\n"); }
    return 0;
}

/* Connect and, if needed, bring up a TLS session.  NULL on error. */
static tr_t *wget_open_conn(const char *host, int port, int use_tls) {
    uint32_t ip;
    if (parse_ipv4(host, &ip) != 0 && dns_resolve(host, &ip) != 0) return NULL;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return NULL;
    struct sockaddr_in dst;
    fill_sin(&dst, ip, (uint16_t)port);
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        close(fd);
        return NULL;
    }

    tr_t *tr = (tr_t *)calloc(1, sizeof(*tr));
    if (!tr) {
        close(fd);
        return NULL;
    }
    tr->fd = fd;
    tr->tls = NULL;

    if (use_tls) {
        tr->tls = cact_tls_connect(fd, host);
        if (!tr->tls) {
            we("wget: TLS with ");
            we(host);
            we(" failed: ");
            we(cact_tls_error());
            we("\n");
            tr_close(tr);
            free(tr);
            return NULL;
        }
    }
    return tr;
}

int cact_ub_wget(char **argv, int argc) {
    const char *url = NULL;
    const char *outfile = NULL;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "-O") == 0) &&
            i + 1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "usage: wget [-o FILE] http[s]://HOST[:PORT][/PATH]\n"
                    "  micro HTTP/1.1 GET client (TLS 1.3, certificate verified)\n"
                    "  default output: basename of PATH (index.html for '/')\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            we("wget: unknown option ");
            we(argv[i]);
            we("\n");
            return 1;
        } else {
            url = argv[i];
        }
    }
    if (!url) {
        we("usage: wget [-o FILE] http[s]://HOST[:PORT][/PATH]\n");
        return 1;
    }

    char host[160];
    char path[768];
    int  port = 80;
    int  tls = 0;
    if (wget_parse_url(url, host, sizeof(host), &port, &tls,
                       path, sizeof(path)) != 0) {
        we("wget: bad URL\n");
        return 1;
    }

    for (int attempt = 0; attempt < W_REDIR_MAX; attempt++) {
        tr_t *tr = wget_open_conn(host, port, tls);
        if (!tr) {
            we("wget: cannot connect to ");
            we(host);
            we("\n");
            return 1;
        }
        if (wget_send_request(tr, host, port, tls, path) != 0) {
            we("wget: send failed\n");
            tr_close(tr);
            free(tr);
            return 1;
        }

        wbuf_t buf;
        buf.tr = tr;
        buf.buf = (unsigned char *)malloc(W_BUF_SIZE);
        buf.cap = W_BUF_SIZE;
        buf.start = buf.end = 0;
        if (!buf.buf) {
            we("wget: out of memory\n");
            tr_close(tr);
            free(tr);
            return 1;
        }

        int code, chunked;
        long clen;
        char loc[1024];
        if (wget_read_headers(&buf, &code, &clen, &chunked, loc, sizeof(loc)) != 0) {
            we("wget: malformed response\n");
            free(buf.buf);
            tr_close(tr);
            free(tr);
            return 1;
        }

        if (code == 301 || code == 302 || code == 303 ||
            code == 307 || code == 308) {
            tr_close(tr);
            free(tr);
            wbuf_free(&buf);
            if (!loc[0]) {
                we("wget: redirect without Location\n");
                return 1;
            }
            if (strncmp(loc, "http://", 7) == 0 ||
                strncmp(loc, "https://", 8) == 0) {
                if (wget_parse_url(loc, host, sizeof(host), &port, &tls,
                                   path, sizeof(path)) != 0) {
                    we("wget: bad redirect Location\n");
                    return 1;
                }
            } else if (loc[0] == '/') {
                strncpy(path, loc, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            } else {
                we("wget: unsupported redirect (relative URL)\n");
                return 1;
            }
            fprintf(stderr, "wget: %d redirect -> %s\n", code, loc);
            continue;
        }

        if (code < 200 || code >= 300) {
            fprintf(stderr, "wget: HTTP %d\n", code);
            wbuf_free(&buf);
            tr_close(tr);
            free(tr);
            return 1;
        }

        /* open the file only on a successful response */
        int outfd = STDOUT_FILENO;
        char defname[128];
        const char *name = outfile;
        if (!name) {
            wget_default_name(path, defname, sizeof(defname));
            name = defname;
        }
        int opened = 0;
        if (strcmp(name, "-") != 0) {
            outfd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (outfd < 0) {
                we("wget: cannot create ");
                we(name);
                we("\n");
                wbuf_free(&buf);
                tr_close(tr);
                free(tr);
                return 1;
            }
            opened = 1;
        }

        unsigned long long written = 0;
        const char *label = (outfd == STDOUT_FILENO) ? NULL : name;
        int rc = wget_save_body(&buf, outfd, chunked, clen, &written, label);
        if (opened) close(outfd);
        wbuf_free(&buf);
        tr_close(tr);
        free(tr);

        if (rc != 0) {
            we("wget: body read failed\n");
            return 1;
        }
        fprintf(stderr, "wget: HTTP %d, %llu bytes -> %s\n",
                code, written, name);
        return 0;
    }

    we("wget: too many redirects\n");
    return 1;
}
