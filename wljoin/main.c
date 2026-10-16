/*
 * wljoin — WPA2 supplicant for CactOS.
 *
 * The 802.11 station engine lives in the kernel (/dev/wlan0, cact80211): scan,
 * open-system auth/association and the 802.11<->802.3 datapath with CCMP.  This
 * utility is the supplicant: it reads the scan list, asks the kernel to join a
 * BSS, runs the WPA2 4-way handshake over EAPOL and installs the keys.
 *
 *   wljoin                  scan and list access points
 *   wljoin <ssid> <pass>    connect (empty/missing pass = open network)
 *   wljoin --selftest       verify the WPA2 crypto vectors
 *   wljoin --status         print the link state and datapath counters
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "wljoin.h"
#include "wlan.h"

/* Bring-up trace: the last line printed names the step the tool is in when it
 * wedges.  A trailing newline plus fflush forces it out before a blocking ioctl
 * (the kernel console interleaves its own "wl: ..." lines). */
#define WL_TRACE(...) do { printf("wljoin: " __VA_ARGS__); fflush(stdout); } while (0)

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static int wl_fd = -1;
static u8  our_mac[6];

/* Connection target and 4-way handshake state. */
static u8  cur_bssid[6];
static u8  cur_ssid[33];
static u8  cur_ssid_len;
static u8  cur_rsn[64];
static u8  cur_rsn_len;

static int wpa_state;             /* 0 idle, 1 have PMK/SNonce, 2 after msg2, 3 done */
static u8  pmk[32];
static u8  ptk[48];               /* KCK[16] KEK[16] TK[16] */
static u8  anonce[32];
static u8  snonce[32];
static u8  replay[8];
static u8  aa[6];                 /* authenticator (AP) */
static u8  spa[6];                /* supplicant (us) */

/* Group key (GTK), taken from msg3's key data and installed alongside the TK. */
static u8  gtk[32];
static int gtk_len;
static u8  gtk_id;

/* The engine reports failures as an ioctl errno (the RT_E* codes in
 * rt2800_local.h) and prints nothing itself; name the ones that matter. */
static const char *wl_error(void) {
    switch (errno) {
    case ECOMM:     return "transmitted, but the AP did not acknowledge";
    case ETIMEDOUT: return "the radio reported no result";
    case EIO:       return "radio I/O error (driver or dongle down?)";
    case ENODEV:    return "the device is not usable";
    case ENOENT:    return "no such BSS (rescan?)";
    case EINVAL:    return "the request was rejected";
    case EFAULT:    return "bad buffer";
    case ENOMEM:    return "out of memory";
    case EAGAIN:    return "not associated yet";
    }
    return "failed";
}

/* ------------------------------------------------------------------ */
/* EAPOL transport                                                     */
/* ------------------------------------------------------------------ */

static int tx_eapol(const u8 *eapol, int len) {
    return wl_eapol_tx(wl_fd, cur_bssid, eapol, (u32)len);
}

/* Fetch at most one EAPOL payload and process it.  Returns 1 when one arrived,
 * 0 when the radio was idle for one poll (the engine's EAPOL_RX ioctl drives a
 * bulk-IN that times out after ~20 ms, so an idle poll is a real wait). */
static void wpa_rx(const u8 *e, int len);

static int rx_one(void) {
    u8  buf[CACT_WLAN_EAPOL_MAX];
    u32 n = 0;

    if (wl_eapol_rx(wl_fd, buf, &n, 0) == 1 && n > 0) {
        wpa_rx(buf, (int)n);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* WPA2 4-way handshake                                                */
/* ------------------------------------------------------------------ */

static void derive_ptk(void) {
    u8 data[76];
    u32 n = 0;

    if (memcmp(aa, spa, 6) < 0) {
        memcpy(data + n, aa, 6);  n += 6;
        memcpy(data + n, spa, 6); n += 6;
    } else {
        memcpy(data + n, spa, 6); n += 6;
        memcpy(data + n, aa, 6);  n += 6;
    }
    if (memcmp(anonce, snonce, 32) < 0) {
        memcpy(data + n, anonce, 32); n += 32;
        memcpy(data + n, snonce, 32); n += 32;
    } else {
        memcpy(data + n, snonce, 32); n += 32;
        memcpy(data + n, anonce, 32); n += 32;
    }
    wl_prf512(pmk, "Pairwise key expansion", data, n, ptk, 48);
}

static int send_msg2(void) {
    u8 f[256];
    int n = 0, mic_off, kd_off;
    u16 kinfo = 0x0002 | 0x0008 | 0x0100;   /* ver2 | pairwise | MIC */
    u16 kd_len;

    f[n++] = 2;                 /* EAPOL version */
    f[n++] = 3;                 /* EAPOL-Key */
    f[n++] = 0; f[n++] = 0;     /* body length, filled below */
    f[n++] = 2;                 /* RSN key descriptor */
    f[n++] = (u8)(kinfo >> 8); f[n++] = (u8)kinfo;
    f[n++] = 0; f[n++] = 0;     /* key length */
    memcpy(f + n, replay, 8); n += 8;
    memcpy(f + n, snonce, 32); n += 32;
    memset(f + n, 0, 16); n += 16;          /* IV */
    memset(f + n, 0, 8);  n += 8;           /* RSC */
    memset(f + n, 0, 8);  n += 8;           /* key ID */
    mic_off = n; memset(f + n, 0, 16); n += 16;
    kd_off = n;
    f[n++] = 0; f[n++] = 0;                 /* key data length */

    kd_len = cur_rsn_len;
    memcpy(f + n, cur_rsn, kd_len); n += kd_len;
    f[kd_off] = (u8)(kd_len >> 8);
    f[kd_off + 1] = (u8)kd_len;

    f[2] = (u8)((n - 4) >> 8); f[3] = (u8)(n - 4);

    u8 mic[20];
    wl_hmac_sha1(ptk, 16, f, (u32)n, mic);
    memcpy(f + mic_off, mic, 16);

    return tx_eapol(f, n);
}

static int send_msg4(void) {
    u8 f[128];
    int n = 0, mic_off;
    u16 kinfo = 0x0002 | 0x0008 | 0x0100 | 0x0200;   /* + secure */

    f[n++] = 2;
    f[n++] = 3;
    f[n++] = 0; f[n++] = 0;
    f[n++] = 2;
    f[n++] = (u8)(kinfo >> 8); f[n++] = (u8)kinfo;
    f[n++] = 0; f[n++] = 0;
    memcpy(f + n, replay, 8); n += 8;
    memset(f + n, 0, 32); n += 32;          /* nonce */
    memset(f + n, 0, 16); n += 16;
    memset(f + n, 0, 8);  n += 8;
    memset(f + n, 0, 8);  n += 8;
    mic_off = n; memset(f + n, 0, 16); n += 16;
    f[n++] = 0; f[n++] = 0;                 /* key data length */

    f[2] = (u8)((n - 4) >> 8); f[3] = (u8)(n - 4);

    u8 mic[20];
    wl_hmac_sha1(ptk, 16, f, (u32)n, mic);
    memcpy(f + mic_off, mic, 16);

    return tx_eapol(f, n);
}

static void wpa_start(const u8 *ssid, u8 ssid_len, const char *pass,
                      const u8 *bssid) {
    u32 seed = 0x12345678u;

    memcpy(aa, bssid, 6);
    memcpy(spa, our_mac, 6);
    wl_pmk(pass, ssid, ssid_len, pmk);

    for (int i = 0; i < 6; i++)
        seed = seed * 31u + our_mac[i];
    for (int i = 0; i < 32; i += 4) {
        seed = seed * 1103515245u + 12345u;
        u32 r = (seed >> 16) ^ seed;
        snonce[i]     = (u8)(r >> 24); snonce[i + 1] = (u8)(r >> 16);
        snonce[i + 2] = (u8)(r >> 8);  snonce[i + 3] = (u8)r;
    }
    wpa_state = 1;
}

/* Pull the GTK out of an EAPOL-Key frame's key data (a key data encapsulation:
 * 0xDD, length, OUI 00-0F-AC, type 1 (GTK), key-info byte, reserved byte, then
 * the key — a six-byte prefix).  Returns 0 when a GTK was found. */
static int gtk_from_keydata(const u8 *kd, int kd_len) {
    for (int i = 0; i + 6 <= kd_len; ) {
        u8 type = kd[i];
        u8 klen = kd[i + 1];

        if (klen == 0 || i + 2 + klen > kd_len)
            break;
        if (type == 0xdd && klen >= 6 + 16 &&
            kd[i + 2] == 0x00 && kd[i + 3] == 0x0f && kd[i + 4] == 0xac &&
            kd[i + 5] == 0x01) {
            gtk_id  = (u8)(kd[i + 6] & 0x03);
            gtk_len = klen - 6;
            if (gtk_len > (int)sizeof(gtk))
                gtk_len = (int)sizeof(gtk);
            memcpy(gtk, kd + i + 8, (size_t)gtk_len);
            return 0;
        }
        i += 2 + klen;
    }
    return -1;
}

static void wpa_rx(const u8 *e, int len) {
    u16 kinfo;
    int ack, kd_len;

    if (len < 99 || e[1] != 3)
        return;

    kinfo   = (u16)((e[5] << 8) | e[6]);
    ack     = (kinfo & 0x0080) != 0;
    kd_len  = (e[97] << 8) | e[98];

    if (!ack)
        return;
    memcpy(replay, e + 9, 8);

    if (wpa_state == 1) {
        memcpy(anonce, e + 17, 32);
        derive_ptk();
        send_msg2();
        wpa_state = 2;
        return;
    }

    if (wpa_state == 2) {
        u8 tmp[512];
        u8 got[16], calc[20];

        if (len > (int)sizeof(tmp))
            return;
        memcpy(tmp, e, len);
        memcpy(got, tmp + 81, 16);
        memset(tmp + 81, 0, 16);

        wl_hmac_sha1(ptk, 16, tmp, (u32)len, calc);
        if (memcmp(calc, got, 16) != 0) {
            printf("WPA2: msg3 MIC mismatch (bad passphrase?)\n");
            return;
        }
        /* msg3 carries the group key.  With the Encrypted Key Data bit set the
         * key data is AES-Key-Wrapped with the KEK (the usual case); the MIC
         * above already covered the wrapped bytes. */
        const u8 *kd = e + 99;
        int kdl = kd_len;
        u8 plain[512];
        if ((kinfo & 0x1000) && kd_len >= 24 && kd_len <= (int)sizeof(plain)) {
            int u = wl_aes_unwrap(ptk + 16, e + 99, kd_len, plain);
            if (u > 0) {
                kd = plain;
                kdl = u;
            } else {
                printf("WPA2: msg3 key data did not unwrap (bad KEK?)\n");
            }
        }
        if (gtk_from_keydata(kd, kdl) != 0)
            printf("WPA2: no GTK in msg3 — group traffic will not decode\n");

        send_msg4();
        printf("WPA2: handshake done\n");
        wpa_state = 3;
    }
}

/* ------------------------------------------------------------------ */
/* connect                                                             */
/* ------------------------------------------------------------------ */

static int do_scan(int *count) {
    int n = wl_scan(wl_fd);

    if (n < 0) {
        printf("scan failed: %s\n", wl_error());
        return -1;
    }
    *count = n;
    return 0;
}

/* Look up an SSID in the BSS list the caller already scanned.  The engine keeps
 * the list until the next scan, so search it in place — re-running the scan here
 * would repeat the whole radio sweep (the join hung on that second sweep). */
static int find_ap(const char *ssid, int n, cact_wlan_bss_t *out) {
    size_t sl = strlen(ssid);

    for (int i = 0; i < n; i++) {
        cact_wlan_bss_t b;
        memset(&b, 0, sizeof(b));
        b.index = (uint32_t)i;
        if (wl_get_bss(wl_fd, &b) != 0)
            continue;
        if (b.ssid_len == sl && memcmp(b.ssid, ssid, sl) == 0) {
            *out = b;
            return 1;
        }
    }
    return 0;
}

static int connect_ap(const cact_wlan_bss_t *b, const char *pass) {
    printf("connecting to '%.*s' %02x:%02x:%02x:%02x:%02x:%02x ch=%u privacy=%u\n",
           (int)b->ssid_len, (const char *)b->ssid,
           b->bssid[0], b->bssid[1], b->bssid[2],
           b->bssid[3], b->bssid[4], b->bssid[5],
           (unsigned)b->channel, (unsigned)b->privacy);

    memcpy(cur_bssid, b->bssid, 6);
    memcpy(cur_ssid, b->ssid, 33);
    cur_ssid_len = b->ssid_len;
    cur_rsn_len  = b->rsn_len;
    memcpy(cur_rsn, b->rsn, b->rsn_len);

    /* The engine runs open-system auth + association. */
    WL_TRACE("joining (auth/assoc)...\n");
    if (wl_connect(wl_fd, b->bssid, b->ssid, b->ssid_len) != 0) {
        printf("join failed: %s\n", wl_error());
        return -1;
    }
    printf("associated\n");

    if (!b->privacy) {
        printf("open network — link up\n");
        return 0;
    }
    if (cur_rsn_len == 0) {
        printf("WPA2 AP but no RSN IE in the beacon; giving up\n");
        return -1;
    }
    if (!pass || !pass[0]) {
        printf("WPA2 AP needs a passphrase\n");
        return -1;
    }

    wpa_start(cur_ssid, cur_ssid_len, pass, cur_bssid);
    WL_TRACE("WPA2 handshake, waiting for EAPOL...\n");
    for (int i = 0; i < WPA_POLLS && wpa_state != 3; i++) {
        if (i && i % 50 == 0)
            WL_TRACE("WPA2 handshake, still waiting (state=%d)...\n",
                     wpa_state);
        rx_one();
    }
    if (wpa_state != 3) {
        printf("WPA2 handshake did not complete (state=%d)\n", wpa_state);
        return -1;
    }

    if (wl_set_key(wl_fd, CACT_WLAN_KEY_PAIRWISE, 0, ptk + 32, 16) != 0) {
        printf("failed to install the pairwise key: %s\n", wl_error());
        return -1;
    }
    if (gtk_len > 0) {
        if (wl_set_key(wl_fd, CACT_WLAN_KEY_GROUP, gtk_id, gtk,
                       (u32)gtk_len) != 0)
            printf("failed to install the group key: %s\n", wl_error());
        else
            printf("WPA2: GTK installed (id=%u)\n", (unsigned)gtk_id);
    }
    printf("WPA2: link up\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int selftest(void) {
    static const u8 expect[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
        0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
        0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e,
    };
    u8 got[32];

    wl_pmk("password", (const u8 *)"IEEE", 4, got);
    int pmk_ok = memcmp(got, expect, 32) == 0;
    printf("WPA2 PMK self-test: %s\n", pmk_ok ? "OK" : "FAILED");

    /* RFC 3394 §4.6 key-unwrap vector. */
    static const u8 kek[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const u8 wrapped[24] = {
        0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47,
        0xae, 0xf3, 0x4b, 0xd8, 0xfb, 0x5a, 0x7b, 0x82,
        0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5,
    };
    static const u8 key[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    u8 unwrapped[16];
    int kw_ok = wl_aes_unwrap(kek, wrapped, (int)sizeof(wrapped), unwrapped) == 16 &&
                memcmp(unwrapped, key, 16) == 0;
    printf("AES key-unwrap self-test: %s\n", kw_ok ? "OK" : "FAILED");

    return (pmk_ok && kw_ok) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    wl_fd = open("/dev/wlan0", O_RDWR);
    if (wl_fd < 0) {
        printf("wljoin: cannot open /dev/wlan0 (errno=%d)\n", errno);
        return 1;
    }

    /* Diagnostics: read the engine's link state and datapath counters without
     * touching the radio, e.g. right after dhcpd has tried to get a lease. */
    if (argc >= 2 && strcmp(argv[1], "--status") == 0) {
        cact_wlan_status_t st;
        if (wl_status(wl_fd, &st) != 0) {
            printf("wljoin: status request failed (errno=%d)\n", errno);
            return 1;
        }
        printf("wljoin: link=%d last_error=%d ccmp_selftest=%d beacon=%u deauth=%u\n",
               (int)st.linked, (int)st.last_error, (int)st.ccmp_selftest,
               (unsigned)st.beacon_rx, (unsigned)st.deauth_rx);
        printf("wljoin: tx data=%u group=%u fail=%u drop(noassoc=%u nokey=%u)\n",
               (unsigned)st.tx_data, (unsigned)st.tx_group,
               (unsigned)st.tx_fail, (unsigned)st.tx_drop_noassoc,
               (unsigned)st.tx_drop_nokey);
        printf("wljoin: rx data=%u group=%u bad=%u nokey=%u other=%u\n",
               (unsigned)st.rx_data, (unsigned)st.rx_group,
               (unsigned)st.rx_bad, (unsigned)st.rx_nokey,
               (unsigned)st.rx_other);
        return 0;
    }

    cact_wlan_status_t st;
    if (wl_status(wl_fd, &st) == 0) {
        memcpy(our_mac, st.mac, 6);
        /* Bring-up failures have no ioctl to come back from: the engine parks
         * them here instead of printing. */
        if (st.last_error)
            printf("wljoin: the engine reports error %d\n", st.last_error);
    }
    printf("wljoin: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           our_mac[0], our_mac[1], our_mac[2],
           our_mac[3], our_mac[4], our_mac[5]);

    int n = 0;
    WL_TRACE("scanning radio...\n");
    if (do_scan(&n) != 0)
        return 1;
    printf("scan: %d AP(s)\n", n);
    for (int i = 0; i < n; i++) {
        cact_wlan_bss_t b;
        memset(&b, 0, sizeof(b));
        b.index = (uint32_t)i;
        if (wl_get_bss(wl_fd, &b) != 0)
            continue;
        printf("  %02x:%02x:%02x:%02x:%02x:%02x ch=%u %s '%.*s'\n",
               b.bssid[0], b.bssid[1], b.bssid[2],
               b.bssid[3], b.bssid[4], b.bssid[5],
               (unsigned)b.channel, b.privacy ? "WPA2" : "open",
               (int)b.ssid_len, (const char *)b.ssid);
    }

    if (argc < 2) {
        printf("usage: wljoin <ssid> [passphrase]   "
               "(or --selftest / --status)\n");
        return 0;
    }

    cact_wlan_bss_t ap;
    if (find_ap(argv[1], n, &ap) <= 0) {
        printf("SSID not found in the scan: %s\n", argv[1]);
        return 1;
    }
    const char *pass = (argc >= 3) ? argv[2] : "";
    return connect_ap(&ap, pass) == 0 ? 0 : 1;
}
