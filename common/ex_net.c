/*
 * builtins/net.c — упрощенные сетевые команды cactsole.
 *
 * Держим только базовый TCP-клиент:
 *   nconn IP PORT [TEXT...]
 *   net   IP PORT [TEXT...]   (короткий алиас)
 *
 * Сложные низкоуровневые команды (nsock/nopt/nlisten/nudp) удалены,
 * чтобы оставить минимальный и понятный набор.
 */


#include <stdint.h>
#include <stddef.h>

#include <socket.h>
#include <nodeio.h>
#include <ioctl_abi.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  Утилиты вывода                                                            */
/* ────────────────────────────────────────────────────────────────────────── */

static void w(const char *s)  { write(STDOUT_FILENO, s, strlen((char *)s)); }
static void we(const char *s) { write(STDERR_FILENO, s, strlen((char *)s)); }

static void wn(int v)  { char b[16]; itoa(v, b); w(b); }
static void wne(int v) { char b[16]; itoa(v, b); we(b); }

/* Печать IPv4 в host byte order (старший октет в битах 24..31). */
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

/* Парсер «10.0.2.15» -> host byte order. 0 = OK, -1 = ошибка. */
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
/*  nsock [tcp|udp]  — голая демонстрация SYS_SOCKET                          */
/* ────────────────────────────────────────────────────────────────────────── */

/* ────────────────────────────────────────────────────────────────────────── */
/*  nconn IP PORT [TEXT...]  — простой TCP-клиент                             */
/* ────────────────────────────────────────────────────────────────────────── */

int cact_ub_nconn(char **argv, int argc) {
    if (argc < 3) {
        we("usage: nconn IP PORT [TEXT...]\n"
           "       шлёт TEXT (или 'PING\\n' по умолчанию) и читает ответ\n");
        return 1;
    }
    uint32_t ip;
    uint16_t port;
    if (parse_ipv4(argv[1], &ip)   < 0) { we("nconn: bad ip\n");   return 1; }
    if (parse_port(argv[2], &port) < 0) { we("nconn: bad port\n"); return 1; }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { perr("nconn", "socket", fd); return 1; }

    struct sockaddr_in dst;
    fill_sin(&dst, ip, port);

    w("nconn: connect "); print_ipv4_h(ip); w(":"); wn(port); w(" ... ");
    int rc = connect(fd, &dst, sizeof(dst));
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

int cact_ub_net(char **argv, int argc) {
    if (argc < 3) {
        we("usage: net IP PORT [TEXT...]\n");
        return 1;
    }
    return cact_ub_nconn(argv, argc);
}

int cact_ub_ping(char **argv, int argc) {
    if (argc < 2) {
        we("usage: ping IP [-c COUNT]\n");
        return 1;
    }

    uint32_t ip_h;
    if (parse_ipv4(argv[1], &ip_h) < 0) {
        we("ping: bad ip\n");
        return 1;
    }

    int count = 4;
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            count = atoi(argv[i + 1]);
            if (count <= 0) count = 1;
        }
    }

    w("PING "); print_ipv4_h(ip_h); w(":\n");
    uint16_t id = (uint16_t)getpid();
    for (int i = 0; i < count; i++) {
        cact_ping_arg_t pa = { .dst_ip = ip_h, .id = id, .seq = (uint32_t)(i + 1) };
        int rc = nio_dev_cmd("/dev/net", CACT_NETCTL_PING, &pa);
        if (rc < 0) {
            we("ping: send failed\n");
            return 1;
        }
        w("ping: echo request sent seq="); wn(i + 1); w("\n");
    }
    w("ping: done (echo replies will appear from kernel ICMP logs)\n");
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  dhcp  — простой DHCP discover/offer клиент (UDP 68 -> 67)               */
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

typedef struct {
    uint32_t ip_host;
    uint32_t netmask_host;
    uint32_t gateway_host;
    uint32_t dns_host;
    uint32_t dhcp_server_host;
    uint32_t lease_s;
    uint32_t t1_s;
    uint32_t t2_s;
} netcfg_args_t;

static uint32_t g_dns_ip_h = 0x08080808u;

int cact_ub_dhcp(char **argv, int argc) {
    (void)argv; (void)argc;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { perr("dhcp", "socket", fd); return 1; }

    struct sockaddr_in local;
    fill_sin(&local, 0, 68);
    if (bind(fd, &local, sizeof(local)) < 0) {
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
    int sret = sendto(fd, &req, sizeof(req), 0, &bcast, sizeof(bcast));
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
        int n = recvfrom(fd, &rep, sizeof(rep), 0, &from, &fromlen);
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

    sret = sendto(fd, &req, sizeof(req), 0, &bcast, sizeof(bcast));
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
        int n = recvfrom(fd, &rep, sizeof(rep), 0, &from, &fromlen);
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

    netcfg_args_t cfg = {
        .ip_host = yiaddr_h,
        .netmask_host = subnet ? subnet : 0xFFFFFF00u,
        .gateway_host = router,
        .dns_host = dns ? dns : g_dns_ip_h,
        .dhcp_server_host = server_id,
        .lease_s = lease_s,
        .t1_s = t1_s,
        .t2_s = t2_s,
    };
    int rc = nio_dev_cmd("/dev/net", CACT_NETCTL_NETCFG, &cfg);
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
    if (cfg.lease_s) { w("dhcp: lease="); wn((int)cfg.lease_s); w("s\n"); }
    if (cfg.t1_s) { w("dhcp: t1="); wn((int)cfg.t1_s); w("s\n"); }
    if (cfg.t2_s) { w("dhcp: t2="); wn((int)cfg.t2_s); w("s\n"); }

    close(fd);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  dns HOST [DNS_IP]  — простой DNS A query (UDP/53)                        */
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

int cact_ub_dns(char **argv, int argc) {
    if (argc < 2) {
        we("usage: dns HOST [DNS_IP]\n");
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
    int sret = sendto(fd, pkt, (uint32_t)off, 0, &dst, sizeof(dst));
    if (sret < 0) {
        perr("dns", "sendto", sret);
        close(fd);
        return 1;
    }

    for (int attempt = 0; attempt < 5; attempt++) {
        struct sockaddr_in from;
        uint32_t fromlen = sizeof(from);
        int n = recvfrom(fd, pkt, sizeof(pkt), 0, &from, &fromlen);
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
