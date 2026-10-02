/* ap_scan.h -- k209: THE PURE HALF OF A SCAN. Read one beacon or probe response into an ApScanItem, keep a
 * fixed table of what a scan heard, and build the power-management null frame that tells the base station
 * we are leaving the channel (and back again).
 *
 * The driver half -- switching channels, probing, the excursions, publishing -- is ApScanStep in ap_shim.c,
 * at task level. This file is what scan_test.c can prove on the host.
 *
 * PRIOR ART, read for this:
 *   IEEE 802.11-2016 9.4.2.25 (the RSN element: version, group cipher, pairwise and AKM suite lists,
 *     RSN Capabilities; every field after the version may be absent, and an absent field takes its
 *     default -- CCMP-128 for the ciphers, 00-0F-AC:1 (802.1X) for the AKM), 9.4.2.2 (SSID), 9.4.2.4 (DS
 *     Parameter Set), 9.4.1.4 (the capability field's Privacy bit)
 *   WPA's vendor element: 00-50-F2, type 1 (hostapd, wpa_supplicant's wpa_parse_wpa_ie_wpa)
 *   mac80211 scan.c: channels are heard for their beacons AND probe responses; a passive channel
 *     (IEEE80211_CHAN_NO_IR) is only heard
 *   The driver's own ParseRxBuffer (ap_tx.h): addr2 is the transmitter, the SSID's length is capped at 32,
 *     a truncated element ends the walk ("stop, do not guess"). ParseRxBuffer stays the join's parser;
 *     this one adds what a LIST needs -- the Privacy bit and the RSN/WPA elements.
 *
 * ⚠ Every read is bounded by the frame's length: a malformed or truncated frame off the air is the normal
 *   case (ParseRxBuffer's own rule), and the scan reads frames from every network in range.
 */
#ifndef AP_SCAN_H
#define AP_SCAN_H

#include "ap_scanlist.h"

/* ---- the security of one network ---------------------------------------------------------------------- */
#define AP_RSN_CIPHER_TKIP   2u
#define AP_RSN_CIPHER_CCMP   4u
#define AP_RSN_CAP_MFPR      0x0040u   /* RSN Capabilities bit 6: Management Frame Protection Required */

/* Suite types seen in the RSN element, as bit sets: bit t = 00-0F-AC:t present (types 0..31). */
typedef struct { ApStU32 pairwise, akm; unsigned group; unsigned caps; int valid; } ApRsnInfo;

static unsigned ApScanLe16(const ApStU8 *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

/* One suite: 00-0F-AC:type -> type (0..31), anything else -> 32 (a vendor's suite; recorded as unknown). */
static unsigned ApScanSuite(const ApStU8 *s)
{
    if (s[0] == 0x00u && s[1] == 0x0Fu && s[2] == 0xACu && s[3] < 32u) return s[3];
    return 32u;
}

/* The RSN element's body (after ID and length). Absent trailing fields keep their defaults. */
static ApRsnInfo ApScanParseRsn(const ApStU8 *b, ApStU32 len)
{
    ApRsnInfo r;
    ApStU32 at = 2, n, k;
    r.pairwise = 1u << AP_RSN_CIPHER_CCMP;               /* the defaults (9.4.2.25.1) */
    r.akm = 1u << 1;
    r.group = AP_RSN_CIPHER_CCMP;
    r.caps = 0;
    r.valid = 0;
    if (len < 2u || ApScanLe16(b) != 1u) return r;       /* version 1 or nothing */
    r.valid = 1;
    if (at + 4u > len) return r;
    r.group = ApScanSuite(b + at); at += 4u;
    if (at + 2u > len) return r;
    n = ApScanLe16(b + at); at += 2u;
    if (at + 4u * n > len) { r.valid = 0; return r; }     /* a count the element cannot hold: malformed */
    r.pairwise = 0;
    for (k = 0; k < n; k++) { unsigned t = ApScanSuite(b + at + 4u * k); if (t < 32u) r.pairwise |= 1u << t; }
    at += 4u * n;
    if (at + 2u > len) return r;
    n = ApScanLe16(b + at); at += 2u;
    if (at + 4u * n > len) { r.valid = 0; return r; }
    r.akm = 0;
    for (k = 0; k < n; k++) { unsigned t = ApScanSuite(b + at + 4u * k); if (t < 32u) r.akm |= 1u << t; }
    at += 4u * n;
    if (at + 2u <= len) r.caps = ApScanLe16(b + at);
    return r;
}

/* Joinable = PSK (AKM 2) with CCMP for unicast AND broadcast, and PMF not required: the only WPA2 this
 * driver speaks (HMAC-SHA1 KDF, CCMP, no 802.11w). */
static unsigned ApScanClassify(int privacy, const ApRsnInfo *rsn, int hasWpaIe)
{
    if (rsn && !rsn->valid) return kApSecUnknown;         /* an RSN element we cannot read: say so, do not guess */
    if (rsn) {
        int psk    = (rsn->akm & (1u << 2)) != 0;
        int psk256 = (rsn->akm & (1u << 6)) != 0;
        int sae    = (rsn->akm & ((1u << 8) | (1u << 24))) != 0;
        int owe    = (rsn->akm & (1u << 18)) != 0;
        int dot1x  = (rsn->akm & ((1u << 1) | (1u << 3) | (1u << 5) | (1u << 11) | (1u << 12) | (1u << 13)
                                   | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17))) != 0;
        int pairCcmp  = (rsn->pairwise & (1u << AP_RSN_CIPHER_CCMP)) != 0;
        int groupCcmp = rsn->group == AP_RSN_CIPHER_CCMP;
        int mfpr      = (rsn->caps & AP_RSN_CAP_MFPR) != 0;
        if (psk && pairCcmp && groupCcmp && !mfpr) return sae ? kApSecWpa2Wpa3 : kApSecWpa2Psk;
        if (psk && pairCcmp && rsn->group == AP_RSN_CIPHER_TKIP && !mfpr) return kApSecTkipGroup;
        if ((psk || psk256) && (mfpr || !psk)) return kApSecPmfRequired;
        if (psk && !pairCcmp) return kApSecWpaTkip;
        if (sae) return kApSecWpa3;
        if (owe) return kApSecOwe;
        if (dot1x) return kApSecEnterprise;
        return kApSecUnknown;
    }
    if (hasWpaIe) return kApSecWpaTkip;
    return privacy ? kApSecWep : kApSecOpen;
}

/* ---- one beacon or probe response ---------------------------------------------------------------------
 * f: the 802.11 frame, flen bytes WITHOUT the FCS. heardOn: the channel the radio was tuned to. Fills
 * everything but signalDbm/quality (the RX header's business) and returns 1, or 0 for anything else. */
#define AP_SCAN_HDR    24u
#define AP_SCAN_FIXED  12u               /* timestamp 8, beacon interval 2, capability 2 */
static int ApScanParse(const ApStU8 *f, ApStU32 flen, ApStU8 heardOn, ApScanItem *out)
{
    unsigned fc, cap;
    ApStU32 at, k;
    ApRsnInfo rsn;
    int sawRsn = 0, haveWpa = 0, sawSsid = 0, allZero = 1;
    ApStU8 dsChan = 0;

    for (k = 0; k < sizeof(*out); k++) ((ApStU8 *)out)[k] = 0;
    if (!f || flen < AP_SCAN_HDR + AP_SCAN_FIXED) return 0;
    fc = ApScanLe16(f);
    if (((fc >> 2) & 3u) != 0u) return 0;                         /* management frames only */
    if (((fc >> 4) & 0xFu) != 8u && ((fc >> 4) & 0xFu) != 5u) return 0;  /* beacon or probe response */
    for (k = 0; k < 6u; k++) out->bssid[k] = f[16 + k];          /* addr3 = the BSSID */
    cap = ApScanLe16(f + AP_SCAN_HDR + 10u);
    if (!(cap & 0x0001u)) return 0;                              /* not an ESS: an ad hoc (IBSS) network */
    rsn.valid = 0;
    for (at = AP_SCAN_HDR + AP_SCAN_FIXED; at + 2u <= flen; ) {
        ApStU8 id = f[at], ln = f[at + 1];
        const ApStU8 *b = f + at + 2u;
        if (at + 2u + ln > flen) break;                          /* truncated: stop, do not guess */
        if (id == 0u && !sawSsid) {
            ApStU8 n = (ln > 32u) ? 32u : ln;
            sawSsid = 1;
            for (k = 0; k < n; k++) { out->ssid[k] = b[k]; if (b[k]) allZero = 0; }
            out->ssidLen = allZero ? 0u : n;                    /* an all-zero name hides it too */
            if (allZero) for (k = 0; k < n; k++) out->ssid[k] = 0;
        } else if (id == 3u && ln >= 1u) {
            dsChan = b[0];
        } else if (id == 48u && !sawRsn) {
            rsn = ApScanParseRsn(b, ln);
            sawRsn = 1;
        } else if (id == 221u && ln >= 4u && b[0] == 0x00u && b[1] == 0x50u && b[2] == 0xF2u && b[3] == 1u) {
            haveWpa = 1;
        }
        at += 2u + ln;
    }
    if (!sawSsid) return 0;                                      /* no SSID element at all: malformed */
    out->channel  = (dsChan >= 1u && dsChan <= 14u) ? dsChan : heardOn;
    out->security = (ApStU8)ApScanClassify((cap & 0x0010u) != 0, sawRsn ? &rsn : 0, haveWpa);
    out->flags    = (ApStU8)((out->ssidLen ? 0 : kApScanFHidden)
                             | (ApScanSecurityJoinable(out->security) ? kApScanFJoinable : 0)
                             | ((((fc >> 4) & 0xFu) == 5u) ? kApScanFProbeResp : 0));
    return 1;
}

/* ---- the table a scan fills -------------------------------------------------------------------------- */
/* k216: how many sweeps a network may go unheard before it is dropped. Apple's AirPort driver MERGES scan
 * results into a rolling list (WirelessScanMerge) rather than rebuilding from scratch, so a network that is
 * briefly missed does not flicker out of the UI. We do the same: a sweep updates what it hears and ages out
 * what it has not heard for this many sweeps. 3 tolerates two missed sweeps before a gone network disappears. */
#define kApScanAgeSweeps 3u

typedef struct {
    ApScanItem e[kApScanMax];
    ApStU32    n;
    ApStU32    dropped;          /* a new network that did not fit and was weaker than everything held */
    ApStU32    lastSeen[kApScanMax];  /* k216: the sweep number each entry was last heard in (merge age-out) */
    ApStU32    sweepNo;          /* k216: bumped once per merging sweep (ApScanMergeBegin) */
} ApScanTable;

static void ApScanReset(ApScanTable *t)
{
    ApStU32 k;
    for (k = 0; k < sizeof(*t); k++) ((ApStU8 *)t)[k] = 0;
}

/* k216: begin a MERGING sweep -- keep the entries already held and bump the sweep counter, instead of wiping
 * the table (ApScanReset). ApScanAdd stamps every entry it hears with this sweep number; ApScanMergeEnd then
 * drops the ones that have gone unheard. Call ApScanMergeEnd after listening, before sorting/publishing. */
static void ApScanMergeBegin(ApScanTable *t) { t->sweepNo++; }

/* k216: end a merging sweep -- drop entries not heard within the last `maxAge` sweeps, compacting e[] and
 * lastSeen[] together so they stay aligned. (sweepNo - lastSeen) is unsigned and lastSeen <= sweepNo always. */
static void ApScanMergeEnd(ApScanTable *t, ApStU32 maxAge)
{
    ApStU32 r, w = 0;
    for (r = 0; r < t->n; r++) {
        if ((t->sweepNo - t->lastSeen[r]) < maxAge) {
            if (w != r) { t->e[w] = t->e[r]; t->lastSeen[w] = t->lastSeen[r]; }
            w++;
        }
    }
    t->n = w;
}

static int ApScanSameBssid(const ApStU8 *a, const ApStU8 *b)
{
    int k;
    for (k = 0; k < 6; k++) if (a[k] != b[k]) return 0;
    return 1;
}

/* One reading. The same BSSID again: keep the strongest signal, learn a name a probe response revealed,
 * gather the flags. A new BSSID: append; or, full, replace the weakest if this one is stronger. */
static void ApScanAdd(ApScanTable *t, const ApScanItem *in)
{
    ApStU32 k, weakest = 0;
    for (k = 0; k < t->n; k++) {
        ApScanItem *e = &t->e[k];
        if (!ApScanSameBssid(e->bssid, in->bssid)) continue;
        if (in->signalDbm > e->signalDbm) { e->signalDbm = in->signalDbm; e->quality = in->quality; }
        if (e->ssidLen == 0u && in->ssidLen != 0u) {
            ApStU32 j;
            for (j = 0; j < 32u; j++) e->ssid[j] = in->ssid[j];
            e->ssidLen = in->ssidLen;
            e->flags = (ApStU8)(e->flags & ~kApScanFHidden);
        }
        e->flags = (ApStU8)(e->flags | (in->flags & kApScanFProbeResp));
        t->lastSeen[k] = t->sweepNo;                      /* k216: heard again this sweep */
        return;
    }
    if (t->n < kApScanMax) { t->lastSeen[t->n] = t->sweepNo; t->e[t->n++] = *in; return; }
    for (k = 1; k < t->n; k++) if (t->e[k].signalDbm < t->e[weakest].signalDbm) weakest = k;
    if (in->signalDbm > t->e[weakest].signalDbm) { t->e[weakest] = *in; t->lastSeen[weakest] = t->sweepNo; }
    t->dropped++;
}

/* Strongest first (stable, so equal signals keep the order they were heard in). k216: lastSeen[] moves with
 * its entry, so the merge age-out (ApScanMergeEnd, next sweep) still reads the right stamp per row. */
static void ApScanSort(ApScanTable *t)
{
    ApStU32 i, j;
    for (i = 1; i < t->n; i++) {
        ApScanItem x = t->e[i]; ApStU32 xs = t->lastSeen[i];
        for (j = i; j > 0 && t->e[j - 1].signalDbm < x.signalDbm; j--) { t->e[j] = t->e[j - 1]; t->lastSeen[j] = t->lastSeen[j - 1]; }
        t->e[j] = x; t->lastSeen[j] = xs;
    }
}

/* ---- "I am dozing" / "I am back" ------------------------------------------------------------------------
 * A null-function data frame to the AP with the Power Management bit set before we leave the channel, so it
 * holds our frames; clear on returning, so it sends them (mac80211 ieee80211_offchannel_ps_enable/disable,
 * ieee80211_send_nullfunc). 24 bytes, no body: type 2 subtype 4 (0x48), ToDS, PwrMgt (0x10 in byte 1).
 * Duration 0 and a zero sequence control, like every management frame this driver sends (HWSEQ fills it). */
static ApStU32 ApBuildNullFunc(ApStU8 *f, const ApStU8 *me, const ApStU8 *bssid, int pm)
{
    int k;
    f[0] = 0x48u;
    f[1] = (ApStU8)(0x01u | (pm ? 0x10u : 0x00u));
    f[2] = 0; f[3] = 0;
    for (k = 0; k < 6; k++) { f[4 + k] = bssid[k]; f[10 + k] = me[k]; f[16 + k] = bssid[k]; }
    f[22] = 0; f[23] = 0;
    return 24u;
}

#endif /* AP_SCAN_H */
