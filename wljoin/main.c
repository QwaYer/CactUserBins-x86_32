/*
 * wljoin — Wi-Fi connection utility for CactOS.
 *
 * The rt2800usb driver is a dumb radio (/dev/wlan0): it moves raw 802.11 frames
 * and runs the datapath with keys it is given.  Everything that establishes a
 * connection lives here:
 *
 *   scan   — walk channels, send probe requests, collect beacons / probe
 *            responses and parse SSID / channel / RSN / privacy;
 *   join   — open-system auth, association, then (for a WPA2 AP) the 4-way
 *            handshake, and finally install the pairwise key and bring the
 *            datapath up.
 *
 *   wljoin                  scan and list access points
 *   wljoin <ssid> <pass>    connect (empty/missing pass = open network)
 *   wljoin --selftest       verify the WPA2 crypto vectors
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "wljoin.h"
#include "wlan.h"

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static int wl_fd = -1;
static u8  our_mac[6];

static struct ap_info aps[MAX_AP];
static int ap_count;

static int auth_ok;
static int assoc_ok;

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

static u8  rxbuf[CACT_WLAN_FRAME_MAX];

/* The driver reports failures as an ioctl errno (the RT_E* codes in
 * rt2800_local.h) and prints nothing itself; name the ones that matter. */
static const char *wl_error(void) {
    switch (errno) {
    case ECOMM:     return "transmitted, but the AP did not acknowledge";
    case ETIMEDOUT: return "the radio reported no result";
    case EIO:       return "radio I/O error (driver or dongle down?)";
    case ENODEV:    return "the device is not usable";
    case EINVAL:    return "the request was rejected";
    case EFAULT:    return "bad buffer";
    case ENOMEM:    return "out of memory";
    }
    return "failed";
}

/* ------------------------------------------------------------------ */
/* raw frame I/O                                                       */
/* ------------------------------------------------------------------ */

static int tx_frame(const u8 *f, int len) {
    return wl_tx(wl_fd, f, (u32)len);
}

static void handle_rx(const u8 *f, int len);
static void scan_frame(const u8 *f, int len);
static void wpa_rx(const u8 *eapol, int len);

/* Pull at most one frame and dispatch it.  Returns 1 when a frame arrived, 0
 * when the radio was idle for one poll — the driver's RX ioctl performs a
 * bulk-IN that times out after RT_RX_TIMEOUT_MS (~20 ms), so an idle poll is a
 * real wait and the missed-poll count doubles as the dwell timer. */
static int rx_one(void) {
    u32 n = 0;

    if (wl_rx(wl_fd, rxbuf, &n) == 1 && n > 0) {
        handle_rx(rxbuf, (int)n);
        return 1;
    }
    return 0;
}

/* Throw away whatever the channel walk left queued.  A stale auth/assoc
 * response from an earlier attempt must not set the success flags before this
 * attempt has sent anything, and the scan's backlog must not surface later.
 * Bounded: an AP's channel keeps producing beacons, so stop after a fixed
 * number of frames rather than waiting for silence that never comes. */
static void drain_rx(void) {
    u32 n;

    for (int i = 0; i < 32; i++) {
        n = 0;
        if (wl_rx(wl_fd, rxbuf, &n) != 1 || n == 0)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* 802.11 management frames                                            */
/* ------------------------------------------------------------------ */

static int send_probe_request(void) {
    u8 f[64];
    int n = 0;

    f[n++] = 0x40; f[n++] = 0x00;        /* probe request */
    f[n++] = 0x00; f[n++] = 0x00;        /* duration */
    memset(f + n, 0xff, 6); n += 6;      /* DA broadcast */
    memcpy(f + n, our_mac, 6); n += 6;   /* SA */
    memset(f + n, 0xff, 6); n += 6;      /* BSSID broadcast */
    f[n++] = 0x00; f[n++] = 0x00;        /* seq */
    f[n++] = 0;    f[n++] = 0;           /* SSID: wildcard */
    f[n++] = 1;    f[n++] = 4;           /* supported rates */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8b; f[n++] = 0x96;

    return tx_frame(f, n);
}

static int send_auth(const u8 *bssid) {
    u8 f[32];
    int n = 0;

    f[n++] = 0xb0; f[n++] = 0x00;        /* auth, management */
    f[n++] = 0x00; f[n++] = 0x00;
    memcpy(f + n, bssid, 6); n += 6;     /* DA */
    memcpy(f + n, our_mac, 6); n += 6;   /* SA */
    memcpy(f + n, bssid, 6); n += 6;     /* BSSID */
    f[n++] = 0x00; f[n++] = 0x00;        /* seq */
    f[n++] = 0x00; f[n++] = 0x00;        /* algorithm: open */
    f[n++] = 0x01; f[n++] = 0x00;        /* transaction seq 1 */
    f[n++] = 0x00; f[n++] = 0x00;        /* status 0 */

    return tx_frame(f, n);
}

static int send_assoc(const struct ap_info *ap) {
    static const u8 fallback_rates[4] = { 0x82, 0x84, 0x8b, 0x96 };
    u8 f[256];
    u16 capab = 0x0001;                  /* capability: ESS */
    const u8 *rates;
    u8 rates_len;
    int n = 0;

    /* An RSN (WPA2) STA sets the Privacy bit in its association request, as
     * mac80211 does when the BSS capability carries WLAN_CAPABILITY_PRIVACY. */
    if (ap->privacy)
        capab |= 0x0010;

    f[n++] = 0x00; f[n++] = 0x00;        /* assoc request */
    f[n++] = 0x00; f[n++] = 0x00;
    memcpy(f + n, ap->bssid, 6); n += 6; /* DA */
    memcpy(f + n, our_mac, 6);   n += 6; /* SA */
    memcpy(f + n, ap->bssid, 6); n += 6; /* BSSID */
    f[n++] = 0x00; f[n++] = 0x00;        /* seq */
    f[n++] = (u8)capab; f[n++] = (u8)(capab >> 8);
    f[n++] = 10;   f[n++] = 0x00;        /* listen interval */
    f[n++] = 0;    f[n++] = ap->ssid_len;
    memcpy(f + n, ap->ssid, ap->ssid_len); n += ap->ssid_len;

    /* Rates: echo the AP's own advertised set — IE 1 with the first up to eight,
     * then IE 50 with the rest, exactly the pair mac80211 emits
     * (ieee80211_put_srates_elem).  Some APs reject a request that advertises a
     * superset of their rates. */
    if (ap->supp_rates_len) {
        rates = ap->supp_rates;
        rates_len = ap->supp_rates_len;
    } else {
        rates = fallback_rates;
        rates_len = (u8)sizeof(fallback_rates);
    }
    u8 first = rates_len > 8 ? 8 : rates_len;
    f[n++] = 1;  f[n++] = first;
    memcpy(f + n, rates, first); n += first;
    if (rates_len > first) {
        f[n++] = 50; f[n++] = (u8)(rates_len - first);
        memcpy(f + n, rates + first, rates_len - first); n += rates_len - first;
    }

    /* RSN IE: an association request to a WPA2 (privacy) BSS must carry it —
     * without one hostapd rejects the station before the 4-way handshake, and
     * the status is a generic denial (not the rates code).  The STA echoes the
     * AP's RSNE, the same bytes the handshake later re-uses. */
    if (ap->privacy && cur_rsn_len) {
        f[n++] = 48; f[n++] = cur_rsn_len;
        memcpy(f + n, cur_rsn, cur_rsn_len); n += cur_rsn_len;
    }

    return tx_frame(f, n);
}

/* Wrap an EAPOL frame in an 802.11 data frame (ToDS) + LLC/SNAP and send it. */
static int tx_eapol(const u8 *eapol, int len) {
    u8 f[1600];
    int n = 0;

    if (len > (int)sizeof(f) - 32)
        return -1;

    f[n++] = 0x08; f[n++] = 0x01;          /* data, ToDS */
    f[n++] = 0x00; f[n++] = 0x00;
    memcpy(f + n, cur_bssid, 6); n += 6;   /* Addr1: BSSID */
    memcpy(f + n, our_mac, 6);   n += 6;   /* Addr2: us */
    memcpy(f + n, cur_bssid, 6); n += 6;   /* Addr3: DA = AP */
    f[n++] = 0x00; f[n++] = 0x00;          /* seq */
    f[n++] = 0xaa; f[n++] = 0xaa; f[n++] = 0x03;
    f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x00;
    f[n++] = 0x88; f[n++] = 0x8e;          /* ethertype EAPOL */
    memcpy(f + n, eapol, len); n += len;

    return tx_frame(f, n);
}

/* ------------------------------------------------------------------ */
/* RX dispatch                                                         */
/* ------------------------------------------------------------------ */

static void handle_rx(const u8 *f, int len) {
    u8 type, sub;

    if (len < 24)
        return;

    type = (u8)((f[0] >> 2) & 0x3);

    if (type == 0) {                       /* management */
        sub = (u8)((f[0] >> 4) & 0xf);

        if (sub == 11 && len >= 30) {
            u16 status = (u16)(f[28] | (f[29] << 8));
            printf("auth rx: status=%u\n", status);
            if (status == 0)
                auth_ok = 1;
            return;
        }
        if (sub == 1 && len >= 30) {
            u16 status = (u16)(f[26] | (f[27] << 8));
            printf("assoc rx: status=%u\n", status);
            if (status == 0)
                assoc_ok = 1;
            return;
        }
        if (sub == 12 || sub == 10) {
            u16 reason = (u16)(f[24] | (f[25] << 8));
            printf("%s from AP: reason=%u\n",
                   sub == 12 ? "deauth" : "disassoc", reason);
            return;
        }
        if (sub == 8 || sub == 5)
            scan_frame(f, len);
        return;
    }

    if (type == 2) {                       /* data: EAPOL belongs to us */
        u8 dsub;
        int hdr;
        const u8 *llc;

        if (!(f[1] & 0x02))                /* FromDS only */
            return;
        dsub = (u8)((f[0] >> 4) & 0xf);
        hdr = (dsub & 0x8) ? DOT11_QOS_HDR : DOT11_HDR;
        if (len < hdr + LLC_SNAP_LEN)
            return;
        llc = f + hdr;
        if (llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03 &&
            llc[6] == 0x88 && llc[7] == 0x8e)
            wpa_rx(llc + LLC_SNAP_LEN, len - hdr - LLC_SNAP_LEN);
    }
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

/* Rate bitmap index of an 802.11 "supported rates" element byte (the low seven
 * bits are the rate in units of 0.5 Mbit/s): 0..3 are CCK 1/2/5.5/11, 4..11 the
 * OFDM rates 6..54.  -1 for anything the driver cannot send. */
static int rate_bit(u8 half_mbit) {
    switch (half_mbit) {
    case 2:   return 0;
    case 4:   return 1;
    case 11:  return 2;
    case 22:  return 3;
    case 12:  return 4;
    case 18:  return 5;
    case 24:  return 6;
    case 36:  return 7;
    case 48:  return 8;
    case 72:  return 9;
    case 96:  return 10;
    case 108: return 11;
    }
    return -1;
}

static void scan_frame(const u8 *f, int len) {
    const u8 *bssid;
    const u8 *ssid = NULL;
    const u8 *rsn = NULL;
    u8 ssid_len = 0, rsn_len = 0, channel = 0, privacy;
    u16 basic = 0, erp = 0;
    u8 rates[16];
    u8 rates_len = 0;
    int off;

    if (len < 24 + 12)
        return;

    bssid = f + 16;
    privacy = (u8)((f[34] & 0x10) != 0);
    off = 24 + 12;

    while (off + 2 <= len) {
        u8 id = f[off];
        u8 ilen = f[off + 1];
        if (off + 2 + ilen > len)
            break;
        if (id == 0) {
            ssid = f + off + 2;
            ssid_len = ilen;
        } else if (id == 1 || id == 50) {
            /* Supported rates (bit 7 marks a basic rate) and the extended set,
             * which never marks basic rates.  Keep the bytes too: the
             * association request echoes the AP's own rates in the same order. */
            for (int k = 0; k < ilen; k++) {
                u8 b = f[off + 2 + k];
                int bit = rate_bit((u8)(b & 0x7f));
                if (bit >= 0 && id == 1 && (b & 0x80))
                    basic |= (u16)(1u << bit);
                if (rates_len < (u8)sizeof(rates))
                    rates[rates_len++] = b;
            }
        } else if (id == 3 && ilen >= 1) {
            channel = f[off + 2];
        } else if (id == 42 && ilen >= 1) {
            /* ERP information: protection, short preamble, short slot. */
            u8 e = f[off + 2];
            if (e & 0x02) erp |= CACT_WLAN_ERP_CTS_PROT;
            if (e & 0x04) erp |= CACT_WLAN_ERP_SHORT_PREAMBLE;
            if (e & 0x08) erp |= CACT_WLAN_ERP_SHORT_SLOT;
        } else if (id == 48) {
            rsn = f + off + 2;
            rsn_len = ilen;
        }
        off += 2 + ilen;
    }
    if (!ssid)
        return;
    if (ssid_len > 32)
        ssid_len = 32;
    if (rsn_len > 64)
        rsn_len = 64;

    for (int i = 0; i < ap_count; i++) {
        if (memcmp(aps[i].bssid, bssid, 6) == 0) {
            if (channel)
                aps[i].channel = channel;
            aps[i].privacy = privacy;
            if (basic)
                aps[i].basic_rates = basic;
            aps[i].erp_flags = erp;
            if (rates_len) {
                memcpy(aps[i].supp_rates, rates, rates_len);
                aps[i].supp_rates_len = rates_len;
            }
            /* Do not let a hidden-SSID sighting pin the entry to an empty
             * name: a later frame with the real SSID fills it in. */
            if (aps[i].ssid_len == 0 && ssid_len > 0) {
                memcpy(aps[i].ssid, ssid, ssid_len);
                aps[i].ssid[ssid_len] = 0;
                aps[i].ssid_len = ssid_len;
            }
            if (rsn_len) {
                memcpy(aps[i].rsn, rsn, rsn_len);
                aps[i].rsn_len = rsn_len;
            }
            return;
        }
    }
    if (ap_count >= MAX_AP)
        return;

    struct ap_info *a = &aps[ap_count++];
    memset(a, 0, sizeof(*a));
    memcpy(a->bssid, bssid, 6);
    memcpy(a->ssid, ssid, ssid_len);
    a->ssid_len = ssid_len;
    a->channel = channel ? channel : 1;
    a->privacy = privacy;
    a->basic_rates = basic;
    a->erp_flags = erp;
    memcpy(a->supp_rates, rates, rates_len);
    a->supp_rates_len = rates_len;
    if (rsn_len) {
        memcpy(a->rsn, rsn, rsn_len);
        a->rsn_len = rsn_len;
    }
}

static int do_scan(void) {
    ap_count = 0;
    int radio_failed = 0;

    /* Walk the channels and leave each once it has gone quiet.  The missed-poll
     * counter is the dwell timer: an idle poll blocks for ~20 ms in the driver,
     * so a channel with nothing on it costs SCAN_QUIET_POLLS * 20 ms (~100 ms)
     * and a channel with APs is dwelled only while beacons keep arriving, up to
     * SCAN_POLLS_MAX.  Re-asking rescues an AP that ignored the first probe. */
    for (int ch = 1; ch <= 13; ch++) {
        if (wl_set_channel(wl_fd, (u32)ch) != 0) {
            printf("cannot set channel %d: %s\n", ch, wl_error());
            return -1;
        }
        /* A broadcast probe request waits for no acknowledgement, so a failure
         * here is the radio itself; report it once and keep listening for
         * beacons. */
        if (send_probe_request() != 0 && !radio_failed) {
            radio_failed = 1;
            printf("probe request failed: %s\n", wl_error());
        }

        int quiet = 0, probes = 1;
        for (int i = 0; i < SCAN_POLLS_MAX && quiet < SCAN_QUIET_POLLS; i++) {
            if (i && (i % SCAN_REPROBE) == 0 && probes < SCAN_MAX_PROBES) {
                send_probe_request();
                probes++;
            }
            if (rx_one())
                quiet = 0;
            else
                quiet++;
        }
    }
    return ap_count;
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
    printf("WPA2: passphrase %u chars, deriving PMK...\n",
           (unsigned)strlen(pass));
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

/* Pull the GTK out of an EAPOL-Key frame's key data.  It arrives as a key data
 * encapsulation: 0xDD, length, OUI 00-0F-AC, type 1 (GTK), one byte of key
 * information (bits 0-1 = key id, bit 2 = Tx), one reserved byte, then the key
 * itself — a six-byte prefix, so length - 6 is the key size and the key starts
 * six bytes into the encapsulation body.  Returns 0 when a GTK was found. */
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
    int ack, mic, kd_len;
    const char *msg;

    if (len < 99 || e[1] != 3)
        return;

    kinfo   = (u16)((e[5] << 8) | e[6]);
    ack     = (kinfo & 0x0080) != 0;
    mic     = (kinfo & 0x0100) != 0;
    kd_len  = (e[97] << 8) | e[98];
    msg = (ack && !mic) ? "msg1" : (ack && mic) ? "msg3"
        : (!ack && mic) ? "msg2/4" : "?";

    printf("eapol-key rx: %s len=%d kinfo=0x%04x kd=%d state=%d\n",
           msg, len, kinfo, kd_len, wpa_state);

    if (!ack)
        return;
    memcpy(replay, e + 9, 8);

    if (wpa_state == 1) {
        memcpy(anonce, e + 17, 32);
        derive_ptk();
        printf("WPA2: msg1 -> PTK derived, msg2 tx rc=%d\n", send_msg2());
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
        /* msg3 carries the group key: take it before msg4 completes the
         * handshake (it is installed together with the TK afterwards).  With
         * the Encrypted Key Data bit set the key data is AES-Key-Wrapped with
         * the KEK — the usual case — so unwrap it first; the MIC above already
         * covered the wrapped bytes, so the check just performed still holds. */
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
        if (gtk_from_keydata(kd, kdl) == 0)
            printf("WPA2: msg3 carries the GTK, id=%u len=%d\n",
                   (unsigned)gtk_id, gtk_len);
        else
            printf("WPA2: no GTK in msg3 — group traffic will not decode\n");

        printf("WPA2: msg3 verified, msg4 tx rc=%d — handshake done\n", send_msg4());
        wpa_state = 3;
    }
}

/* ------------------------------------------------------------------ */
/* connect                                                             */
/* ------------------------------------------------------------------ */

static struct ap_info *find_ap(const char *ssid) {
    size_t sl = strlen(ssid);

    for (int i = 0; i < ap_count; i++) {
        if (aps[i].ssid_len == sl && memcmp(aps[i].ssid, ssid, sl) == 0)
            return &aps[i];
    }
    return NULL;
}

static int connect_ap(struct ap_info *ap, const char *pass) {
    printf("connecting to '%s' %02x:%02x:%02x:%02x:%02x:%02x ch=%u privacy=%u\n",
           ap->ssid, ap->bssid[0], ap->bssid[1], ap->bssid[2],
           ap->bssid[3], ap->bssid[4], ap->bssid[5],
           (unsigned)ap->channel, (unsigned)ap->privacy);

    memcpy(cur_bssid, ap->bssid, 6);
    memcpy(cur_ssid, ap->ssid, 33);
    cur_ssid_len = ap->ssid_len;
    cur_rsn_len = ap->rsn_len;
    memcpy(cur_rsn, ap->rsn, ap->rsn_len);

    if (wl_set_channel(wl_fd, ap->channel) != 0) {
        printf("cannot set channel %u: %s\n", (unsigned)ap->channel, wl_error());
        return -1;
    }

    /* The synthesizer needs a moment to settle after the channel change: the
     * scan left the radio hopping channels, and an auth frame sent while the RF
     * is still retuning is lost. */
    usleep(5000);

    /* Discard the frames the channel walk left queued so a stale response is
     * not read as this attempt's. */
    drain_rx();

    /* Hand the AP's beacon parameters to the driver before the first frame:
     * management frames and EAPOL then go out at the lowest basic rate and with
     * the AP's slot time and protection settings, as mac80211's bss_conf does. */
    cact_wlan_rates_t rates;
    rates.basic = ap->basic_rates;
    rates.flags = ap->erp_flags;
    if (wl_set_rates(wl_fd, &rates) != 0) {
        printf("cannot program the AP's rates: %s\n", wl_error());
        return -1;
    }
    printf("rates: basic=0x%03x erp=0x%x\n",
           (unsigned)ap->basic_rates, (unsigned)ap->erp_flags);

    auth_ok = 0;
    int rc = send_auth(ap->bssid);
    printf("auth tx: %s\n", rc == 0 ? "sent" : wl_error());
    for (int i = 0; i < AUTH_POLLS && !auth_ok; i++)
        rx_one();
    if (!auth_ok) {
        printf("authentication failed\n");
        return -1;
    }

    assoc_ok = 0;
    rc = send_assoc(ap);
    printf("assoc tx: %s\n", rc == 0 ? "sent" : wl_error());
    for (int i = 0; i < ASSOC_POLLS && !assoc_ok; i++)
        rx_one();
    if (!assoc_ok) {
        printf("association failed\n");
        return -1;
    }

    wl_set_bssid(wl_fd, ap->bssid);

    if (!ap->privacy) {
        wl_set_link(wl_fd, 1);
        printf("associated (open) — link up\n");
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

    wpa_start(ap->ssid, ap->ssid_len, pass, ap->bssid);
    for (int i = 0; i < WPA_POLLS && wpa_state != 3; i++)
        rx_one();
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
            printf("installed GTK id=%u — broadcast/multicast usable\n",
                   (unsigned)gtk_id);
    }
    wl_set_link(wl_fd, 1);
    printf("installed TK — link up\n");
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

    /* Diagnostics: read the driver's link state and data-path counters without
     * touching the radio, e.g. right after dhcpd has tried to get a lease. */
    if (argc >= 2 && strcmp(argv[1], "--status") == 0) {
        wl_fd = open("/dev/wlan0", O_RDWR);
        if (wl_fd < 0) {
            printf("wljoin: cannot open /dev/wlan0 (errno=%d)\n", errno);
            return 1;
        }
        cact_wlan_status_t st;
        if (wl_status(wl_fd, &st) != 0) {
            printf("wljoin: status request failed (errno=%d)\n", errno);
            return 1;
        }
        printf("wljoin: link=%d last_error=%d ccmp_selftest=%d\n",
               (int)st.linked, (int)st.last_error, (int)st.ccmp_selftest);
        printf("wljoin: tx data=%u group=%u fail=%u\n",
               (unsigned)st.tx_data, (unsigned)st.tx_group,
               (unsigned)st.tx_fail);
        printf("wljoin: rx data=%u group=%u bad=%u nokey=%u other=%u\n",
               (unsigned)st.rx_data, (unsigned)st.rx_group,
               (unsigned)st.rx_bad, (unsigned)st.rx_nokey,
               (unsigned)st.rx_other);
        return 0;
    }

    wl_fd = open("/dev/wlan0", O_RDWR);
    if (wl_fd < 0) {
        printf("wljoin: cannot open /dev/wlan0 (errno=%d)\n", errno);
        return 1;
    }

    cact_wlan_status_t st;
    if (wl_status(wl_fd, &st) == 0) {
        memcpy(our_mac, st.mac, 6);
        /* Bring-up failures have no ioctl to come back from: the driver parks
         * them here instead of printing. */
        if (st.last_error)
            printf("wljoin: the driver reports error %d\n", st.last_error);
    }
    printf("wljoin: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           our_mac[0], our_mac[1], our_mac[2],
           our_mac[3], our_mac[4], our_mac[5]);

    int n = do_scan();
    if (n < 0)
        return 1;
    printf("scan: %d AP(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("  %02x:%02x:%02x:%02x:%02x:%02x ch=%u %s '%s'\n",
               aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
               aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
               (unsigned)aps[i].channel, aps[i].privacy ? "WPA2" : "open",
               aps[i].ssid);

    if (argc < 2) {
        printf("usage: wljoin <ssid> [passphrase]   "
               "(or --selftest / --status)\n");
        return 0;
    }

    struct ap_info *ap = find_ap(argv[1]);
    if (!ap) {
        printf("SSID not found in the scan: %s\n", argv[1]);
        return 1;
    }
    const char *pass = (argc >= 3) ? argv[2] : "";
    return connect_ap(ap, pass) == 0 ? 0 : 1;
}
