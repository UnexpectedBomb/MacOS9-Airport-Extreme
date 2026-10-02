/* ap_scanlist.h -- k209: THE NETWORK LIST, as the driver publishes it and the control panel and the Control
 * Strip module read it. ap_status.h's model exactly: one header included by all three binaries, the layout
 * pinned by _Static_asserts, a block in the SYSTEM heap found through Gestalt (kApScanSelector), never
 * freed, so a reader can never touch freed memory and a reload of the module finds and reuses it.
 *
 * ★ WHO WRITES WHAT.
 *   the driver, task level   everything, at the end of a scan, under the seq word (odd while it rewrites
 *                            the items, even when they are consistent); and `state` alone, as one word,
 *                            when a scan starts
 *   the UI                   reads only. A scan is REQUESTED through ap_status.h's command mailbox
 *                            (kApCmdScan); scanSeq moving is how the UI learns it finished.
 * ⇒ A reader copies the block with ApScanRead (retry while seq is odd or moved), exactly like ApStatRead.
 *
 * ★ WHAT A SCAN IS (ap_shim.c, ApScanStep): channels 1-11 heard AND probed, 12-13 only heard -- a passive
 *   channel transmits nothing, which is lawful everywhere -- in short excursions from the home channel
 *   (mac80211's rule: never more than ~125 ms away while joined, then home again), telling the base
 *   station we are dozing first so it holds our traffic. The table is built from every beacon and probe
 *   response heard; one entry per BSSID, so one network name can appear more than once (the UIs show one
 *   line per name, the strongest).
 *
 * ⚠ NOTHING SECRET GOES HERE -- any process can read the block. Names and addresses of the networks around
 *   the machine are what it is FOR, which is also why the driver log's copy of the list is private data.
 *
 * Pure: no Toolbox, no MMIO. scan_test.c includes it on the host.
 */
#ifndef AP_SCANLIST_H
#define AP_SCANLIST_H

#include "ap_status.h"          /* ApStU32 / ApStS32 / ApStU8, AP_STAT_FN, AP_STAT_BARRIER, kApQual* */

#define kApScanSelector  0x41505873UL        /* Gestalt 'APXs' -> the ApScanBlock's address */
#define kApScanMagic     0x41505853UL        /* 'APXS' */
#define kApScanVersion   1u                  /* bump on ANY layout change; readers refuse others */
#define kApScanMax       32u                 /* entries; one per BSSID */

/* What the network's beacon advertises. JOINABLE by this driver: open, WPA2-Personal, and the WPA2/WPA3
 * transition mode through its WPA2 half (the user's decision, 2026-09-30: nothing below WPA2-Personal; the
 * rest are listed so a user can see the network and learn why it cannot be chosen). Open networks are
 * joinable in principle; the driver joins them from k210 on. */
enum {
    kApSecOpen        = 0,   /* no protection */
    kApSecWpa2Psk     = 1,   /* RSN, PSK, CCMP for both unicast and broadcast */
    kApSecWpa2Wpa3    = 2,   /* the same plus SAE: a WPA3 transition network, joined as WPA2 */
    kApSecWpa3        = 3,   /* SAE only */
    kApSecEnterprise  = 4,   /* 802.1X: a username and a certificate, not a password */
    kApSecWpaTkip     = 5,   /* the original WPA, or WPA2 with TKIP only */
    kApSecTkipGroup   = 6,   /* WPA2 with TKIP broadcasts (a WPA/WPA2 mixed network) */
    kApSecWep         = 7,
    kApSecPmfRequired = 8,   /* requires Protected Management Frames (802.11w) */
    kApSecOwe         = 9,   /* "Enhanced Open" */
    kApSecUnknown     = 10,
    kApSecCount       = 11
};

enum { kApScanFJoinable = 0x01,   /* a security this driver can join */
       kApScanFOurs     = 0x02,   /* the network the driver is joined to right now (BSSID match) */
       kApScanFHidden   = 0x04,   /* no name broadcast (a closed network) */
       kApScanFProbeResp= 0x08 }; /* answered our probe request (else heard by its beacon alone) */
enum { kApScanIdle = 0, kApScanRunning = 1 };

typedef struct {
    ApStU8  bssid[6];           /*  0 */
    ApStU8  channel;            /*  6  from the network's own DS Parameter Set, else the channel heard on */
    ApStU8  security;           /*  7  kApSec* */
    ApStU8  ssidLen;            /*  8  0 = hidden */
    ApStU8  ssid[32];           /*  9 */
    ApStU8  quality;            /* 41  kApQual*, from signalDbm (ap_status.h ApStatQuality) */
    ApStU8  flags;              /* 42  kApScanF* */
    ApStU8  pad;                /* 43 */
    ApStS32 signalDbm;          /* 44  the strongest reading in the scan */
} ApScanItem;                   /* 48 */

typedef struct {
    ApStU32    magic;           /*   0  kApScanMagic */
    ApStU32    version;         /*   4  kApScanVersion */
    ApStU32    size;            /*   8  sizeof(ApScanBlock) */
    ApStU32    seq;             /*  12  even = the items are consistent */
    ApStU32    scanSeq;         /*  16  completed scans since the block was made; 0 = none yet */
    ApStU32    state;           /*  20  kApScan* */
    ApStU32    scanTick;        /*  24  the driver's heartbeat when the latest scan completed */
    ApStU32    count;           /*  28  valid items, strongest first */
    ApStU32    dropped;         /*  32  networks heard that did not fit */
    ApStU32    reserved[3];     /*  36 */
    ApScanItem items[kApScanMax]; /* 48 */
} ApScanBlock;                  /* 1584 */

_Static_assert(sizeof(ApScanItem) == 48,                           "ApScanItem size changed");
_Static_assert(__builtin_offsetof(ApScanItem, ssid) == 9,          "ApScanItem.ssid moved");
_Static_assert(__builtin_offsetof(ApScanItem, signalDbm) == 44,    "ApScanItem.signalDbm moved");
_Static_assert(__builtin_offsetof(ApScanBlock, count) == 28,       "ApScanBlock.count moved");
_Static_assert(__builtin_offsetof(ApScanBlock, items) == 48,       "ApScanBlock.items moved");
_Static_assert(sizeof(ApScanBlock) == 48 + 48 * kApScanMax,        "ApScanBlock size changed");

AP_STAT_FN int ApScanValid(const ApScanBlock *b)
{
    return b && b->magic == kApScanMagic && b->version == kApScanVersion && b->size == sizeof(ApScanBlock);
}

/* A consistent copy: ApStatRead's loop. 1 = known-consistent. */
#ifndef AP_SCAN_READ_HOOK
#define AP_SCAN_READ_HOOK(b) ((void)0)
#endif
AP_STAT_FN int ApScanRead(const volatile ApScanBlock *b, ApScanBlock *out)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        ApStU32 s0 = b->seq, s1;
        const volatile ApStU8 *src = (const volatile ApStU8 *)b;
        ApStU8 *dst = (ApStU8 *)out;
        unsigned k;
        if (s0 & 1u) continue;
        for (k = 0; k < sizeof(ApScanBlock); k++) dst[k] = src[k];
        AP_SCAN_READ_HOOK(b);
        s1 = b->seq;
        if (s1 == s0) return 1;
    }
    return 0;
}

/* The driver: publish n items (already sorted) as one consistent rewrite. */
AP_STAT_FN void ApScanPublish(volatile ApScanBlock *b, const ApScanItem *items, ApStU32 n, ApStU32 dropped,
                              ApStU32 tick)
{
    const ApStU8 *src = (const ApStU8 *)items;
    volatile ApStU8 *dst = (volatile ApStU8 *)b->items;
    unsigned k;
    if (n > kApScanMax) n = kApScanMax;
    b->seq = b->seq + 1u;                                /* odd: readers retry */
    AP_STAT_BARRIER();
    for (k = 0; k < n * sizeof(ApScanItem); k++) dst[k] = src[k];
    for (; k < kApScanMax * sizeof(ApScanItem); k++) dst[k] = 0;
    b->count = n;
    b->dropped = dropped;
    b->scanTick = tick;
    b->scanSeq = b->scanSeq + 1u;
    b->state = kApScanIdle;
    AP_STAT_BARRIER();
    b->seq = b->seq + 1u;                                /* even: consistent */
}

/* Short names, for the UIs and the driver log. */
AP_STAT_FN const char *ApScanSecurityName(unsigned sec)
{
    switch (sec) {
    case kApSecOpen:        return "Open";
    case kApSecWpa2Psk:     return "WPA2 Personal";
    case kApSecWpa2Wpa3:    return "WPA2/WPA3 Personal";
    case kApSecWpa3:        return "WPA3 Personal";
    case kApSecEnterprise:  return "WPA2 Enterprise";
    case kApSecWpaTkip:     return "WPA (TKIP)";
    case kApSecTkipGroup:   return "WPA/WPA2 mixed (TKIP)";
    case kApSecWep:         return "WEP";
    case kApSecPmfRequired: return "Protected Management Frames required";
    case kApSecOwe:         return "Enhanced Open";
    default:                return "unknown security";
    }
}

AP_STAT_FN int ApScanSecurityJoinable(unsigned sec)
{
    return sec == kApSecOpen || sec == kApSecWpa2Psk || sec == kApSecWpa2Wpa3;
}

/* k212: a UI de-dup. A multi-SSID access point (one UniFi radio broadcasting several networks) beacons the
 * same SSID from several BSSIDs, so the scan list holds several entries with identical names. Apple's own
 * AirPort list shows each SSID once; the network lists call this to skip an entry whose SSID already appeared
 * earlier. The list is sorted strongest-first, so the kept (first) entry is the strongest BSSID of that SSID.
 * Hidden entries (ssidLen 0) are never folded together -- they have no name to match on. */
AP_STAT_FN int ApScanDupEarlier(const ApScanItem *items, int idx)
{
    int j, k;
    const ApScanItem *e = &items[idx];
    if (e->ssidLen == 0) return 0;
    for (j = 0; j < idx; j++) {
        const ApScanItem *p = &items[j];
        if (p->ssidLen != e->ssidLen) continue;
        for (k = 0; k < e->ssidLen; k++) if (p->ssid[k] != e->ssid[k]) break;
        if (k == e->ssidLen) return 1;
    }
    return 0;
}

#endif /* AP_SCANLIST_H */
