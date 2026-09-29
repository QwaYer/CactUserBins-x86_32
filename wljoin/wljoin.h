#ifndef WLJOIN_H
#define WLJOIN_H

#include <stdint.h>
#include "ioctl_abi.h"

/*
 * wljoin — the Wi-Fi connection utility for CactOS.
 *
 * The rt2800usb driver is a dumb radio (/dev/wlan0): it moves raw 802.11 frames
 * and runs the datapath with installed keys.  Everything that establishes a
 * connection — scan, open-system auth/assoc, the WPA2 4-way handshake and key
 * installation — happens here, in userspace.
 *
 *   wljoin                 scan and list access points
 *   wljoin <ssid> <pass>   connect (pass empty = open network)
 */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;

/* 802.11 / LLC constants (no FCS: the chip strips it). */
#define DOT11_HDR       24
#define DOT11_QOS_HDR   26
#define LLC_SNAP_LEN    8
#define EAPOL_ETHERTYPE 0x888e

#define MAX_AP          64

/* Poll budgets.  /dev/wlan0's RX ioctl performs one driver bulk-IN itself when
 * nothing is queued (RT_RX_TIMEOUT_MS = 20 ms), so a loop on it is paced by the
 * air; these are the resulting wall-clock windows. */
#define AUTH_POLLS      40        /* ~0.8 s */
#define ASSOC_POLLS     40        /* ~0.8 s */
#define WPA_POLLS       400       /* ~8 s for the 4-way handshake */

/* Scan dwell.  A channel is left after SCAN_QUIET_POLLS consecutive idle
 * polls.  One poll is one driver bulk-IN, so an idle poll costs
 * RT_RX_TIMEOUT_MS (20 ms): a channel with nothing on it costs ~100 ms, while a
 * channel with APs is dwelled only as long as beacons keep arriving (up to
 * SCAN_POLLS_MAX). */
#define SCAN_QUIET_POLLS     5    /* ~100 ms of silence ends the channel */
#define SCAN_POLLS_MAX      50    /* safety cap: at most ~1 s per channel */
#define SCAN_REPROBE        10    /* re-send the probe request every ~200 ms */
#define SCAN_MAX_PROBES      3    /* probe requests per channel */

struct ap_info {
    u8  bssid[6];
    u8  ssid[33];
    u8  ssid_len;
    u8  channel;
    u8  rsn[64];
    u8  rsn_len;
    u8  privacy;
    /* From the beacon's information elements, handed to the driver with
     * CACT_WLANCTL_SET_RATES: the rates the AP declares basic (bit0 = 1 Mbit/s,
     * … bit11 = 54) and the CACT_WLAN_ERP_* timings. */
    u16 basic_rates;
    u16 erp_flags;
};

/* ---- crypto (wpa.c) --------------------------------------------------- */

void wl_pmk(const char *passphrase, const u8 *ssid, u8 ssid_len, u8 pmk[32]);
void wl_hmac_sha1(const u8 *key, u32 key_len, const u8 *msg, u32 msg_len,
                  u8 out[20]);
void wl_prf512(const u8 *key, const char *label, const u8 *data, u32 data_len,
               u8 *out, u32 out_len);

#endif /* WLJOIN_H */
