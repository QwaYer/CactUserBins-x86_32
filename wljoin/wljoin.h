#ifndef WLJOIN_H
#define WLJOIN_H

#include <stdint.h>
#include "ioctl_abi.h"

/*
 * wljoin — the WPA2 supplicant for CactOS.
 *
 * The 802.11 station engine lives in the kernel (/dev/wlan0, cact80211): it
 * scans, runs open-system auth/association and the datapath with CCMP.  wljoin
 * is what establishes the security association on top: it picks a BSS from the
 * scan, asks the kernel to join it, runs the WPA2 4-way handshake over EAPOL
 * and installs the pairwise/group keys.
 *
 *   wljoin                 scan and list access points
 *   wljoin <ssid> <pass>   connect (pass empty = open network)
 */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;

#define MAX_AP          64

/* Poll budget for the 4-way handshake: each EAPOL_RX ioctl drives one radio
 * read (the driver's bulk-IN times out after ~20 ms when idle), so this is the
 * wall-clock window (~8 s). */
#define WPA_POLLS       400

/* ---- crypto (wpa.c) --------------------------------------------------- */

void wl_pmk(const char *passphrase, const u8 *ssid, u8 ssid_len, u8 pmk[32]);
void wl_hmac_sha1(const u8 *key, u32 key_len, const u8 *msg, u32 msg_len,
                  u8 out[20]);
void wl_prf512(const u8 *key, const char *label, const u8 *data, u32 data_len,
               u8 *out, u32 out_len);

/* AES-Key-Unwrap (RFC 3394) of WPA2 key data with the KEK.  Returns the
 * plaintext length, or -1 when the length or the integrity check fails. */
int wl_aes_unwrap(const u8 kek[16], const u8 *in, int in_len, u8 *out);

#endif /* WLJOIN_H */
