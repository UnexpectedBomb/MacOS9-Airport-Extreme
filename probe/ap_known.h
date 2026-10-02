/* ap_known.h -- k210: THE KNOWN NETWORKS the driver may join, and the on-disk form of them. Shared, pure:
 * the driver reads them at boot and joins without any UI running; the control panel adds one when the user
 * chooses a network and types its password. The File Manager glue (FindFolder, FSRead/FSWrite, the atomic
 * temp-file swap) lives in each binary; everything HERE is pure and host-tested (known_test.c).
 *
 * ★ WHAT IS STORED, AND WHAT IS NOT. Each entry is an SSID, its security kind, and its PMK -- the 32-byte
 * key PBKDF2 derives from the passphrase and the SSID. THE PASSPHRASE ITSELF IS NEVER STORED AND NEVER
 * REACHES THE DRIVER. The panel runs PBKDF2 once when the user types the password (ap_wpa_kdf.h, ~0.3 s),
 * writes the PMK, and forgets the passphrase. This is what retires the compiled-in secret: the driver holds
 * no SSID and no passphrase, only what this file gives it at runtime.
 *
 * ⚠ THE PMK IS PASSWORD-EQUIVALENT for its SSID: anyone holding it can join that network and decrypt its
 * traffic. It lives in System Folder:Preferences, readable like any preferences file. This is the same
 * posture the Bluetooth stack already ships -- bluetooth/src/bt_keyfile.c stores link keys, equally
 * sensitive, the same way -- so it is a considered, existing tradeoff, not a new one. It is never written
 * to a log and never committed (the file is the user's, on their machine); known_test.c uses FAKE PMKs.
 * A future hardening is the OS 9 Keychain, as Apple's own AirPort used; noted, not done here.
 *
 * ★ ON-DISK FORMAT (byte layout written out explicitly -- a struct's padding is the compiler's choice, and
 * this file must be read by a future build; bt_keyfile.c's rule):
 *     header 16 bytes:  'APK1'(4) | version(2) | count(2) | nextUseOrder(4) | pad(4)
 *     record 72 bytes:  ssidLen(1) | security(1) | flags(1) | pad(1) | ssid(32) | pmk(32) | useOrder(4)
 * All multi-byte fields big-endian, as on the wire.
 *
 * Pure: no Toolbox, no File Manager, no allocation. Includes ap_scanlist.h for the security kinds and to
 * pick a join target against a scan.
 */
#ifndef AP_KNOWN_H
#define AP_KNOWN_H

#include "ap_scanlist.h"          /* kApSec*, ApScanBlock/ApScanItem, ApScanSecurityJoinable; ApStU8/ApStU32 */

#define kApKnownMax      16u
#define kApKnownMagic    0x41504B31UL   /* 'APK1' */
#define kApKnownVersion  1u
#define kApKnownHdrSize  16u
#define kApKnownRecSize  72u
#define kApKnownFileSize (kApKnownHdrSize + kApKnownRecSize * kApKnownMax)   /* 1168 bytes, fixed */

#define kApKnownPmkLen   32u

enum { kApKnownFAutoJoin = 0x01 };      /* join this one automatically when it is the best in range */

typedef struct {
    ApStU8  ssid[32];
    ApStU8  ssidLen;
    ApStU8  security;                   /* kApSec* -- open or WPA2-Personal (the only two joinable) */
    ApStU8  flags;                      /* kApKnownF* */
    ApStU8  pmk[32];                    /* zero for an open network */
    ApStU32 useOrder;                   /* higher = used more recently; 0 = never joined */
} ApKnownNet;

typedef struct {
    ApKnownNet net[kApKnownMax];
    ApStU32    count;
    ApStU32    nextUseOrder;            /* the value the next "mark used" assigns, then increments */
} ApKnownDb;

/* ---- helpers ------------------------------------------------------------------------------------------ */

static int ApKnownSsidEq(const ApStU8 *a, ApStU8 an, const ApStU8 *b, ApStU8 bn)
{
    ApStU8 i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void ApKnownClear(ApKnownDb *db)
{
    ApStU32 i;
    for (i = 0; i < sizeof(*db); i++) ((ApStU8 *)db)[i] = 0;
    db->nextUseOrder = 1;               /* 0 means "never used", so real orders start at 1 */
}

/* Index of the SSID, or -1. */
static int ApKnownFind(const ApKnownDb *db, const ApStU8 *ssid, ApStU8 len)
{
    ApStU32 i;
    for (i = 0; i < db->count; i++)
        if (ApKnownSsidEq(db->net[i].ssid, db->net[i].ssidLen, ssid, len)) return (int)i;
    return -1;
}

/* Add a network, or update the one already there (a changed password rewrites its PMK). Open networks pass
 * a NULL or zero pmk. Returns the index, or -1 if the SSID is unusable or the table is full. The new or
 * updated entry is NOT marked used -- ApKnownTouch does that when a join actually succeeds. */
static int ApKnownAdd(ApKnownDb *db, const ApStU8 *ssid, ApStU8 len, unsigned security, const ApStU8 *pmk)
{
    int idx;
    ApKnownNet *e;
    ApStU8 i;
    if (len == 0 || len > 32) return -1;
    idx = ApKnownFind(db, ssid, len);
    if (idx < 0) {
        if (db->count >= kApKnownMax) return -1;
        idx = (int)db->count++;
        for (i = 0; i < sizeof(ApKnownNet); i++) ((ApStU8 *)&db->net[idx])[i] = 0;
        for (i = 0; i < len; i++) db->net[idx].ssid[i] = ssid[i];
        db->net[idx].ssidLen = len;
        db->net[idx].flags = kApKnownFAutoJoin;
    }
    e = &db->net[idx];
    e->security = (ApStU8)security;
    for (i = 0; i < kApKnownPmkLen; i++) e->pmk[i] = (security == kApSecOpen || !pmk) ? 0 : pmk[i];
    return idx;
}

/* Remove a network, keeping the table packed. */
static void ApKnownRemove(ApKnownDb *db, const ApStU8 *ssid, ApStU8 len)
{
    int idx = ApKnownFind(db, ssid, len);
    ApStU32 j, k;
    if (idx < 0) return;
    for (j = (ApStU32)idx; j + 1 < db->count; j++)
        for (k = 0; k < sizeof(ApKnownNet); k++)
            ((ApStU8 *)&db->net[j])[k] = ((ApStU8 *)&db->net[j + 1])[k];
    db->count--;
    for (k = 0; k < sizeof(ApKnownNet); k++) ((ApStU8 *)&db->net[db->count])[k] = 0;
}

/* Mark a network as the most recently joined, so it wins the auto-join tie next time. */
static void ApKnownTouch(ApKnownDb *db, int idx)
{
    if (idx < 0 || (ApStU32)idx >= db->count) return;
    db->net[idx].useOrder = db->nextUseOrder++;
    if (db->nextUseOrder == 0) db->nextUseOrder = 1;   /* the wrap is astronomically distant; still, no 0 */
}

/* ---- pick a network to join -------------------------------------------------------------------------- *
 * The rule (COMMUNITY-RELEASE.md): the last network used, else the strongest known one in range. "In range"
 * means its SSID appears in the scan and the scan entry is joinable; a known network the scan did not hear,
 * or heard as a security this build cannot join, is not a candidate. Returns the known index to join and,
 * through *scanOut, the scan entry it matched (for its channel/BSSID/signal); -1 if nothing is joinable. */
static int ApKnownPickJoinItems(const ApKnownDb *db, const ApScanItem *items, ApStU32 count, int *scanOut)
{
    ApStU32 i, s;
    int best = -1, bestScan = -1;
    ApStS32 bestSig = 0;
    ApStU32 bestOrder = 0;
    if (scanOut) *scanOut = -1;
    for (i = 0; i < db->count; i++) {
        const ApKnownNet *k = &db->net[i];
        if (!(k->flags & kApKnownFAutoJoin)) continue;
        if (!ApScanSecurityJoinable(k->security)) continue;
        for (s = 0; s < count && s < kApScanMax; s++) {
            const ApScanItem *e = &items[s];
            if (!(e->flags & kApScanFJoinable)) continue;
            if (!ApKnownSsidEq(e->ssid, e->ssidLen, k->ssid, k->ssidLen)) continue;
            /* A candidate. Prefer the most recently used; among equal use (e.g. all never-used), the
             * strongest signal. useOrder dominates, signal breaks the tie. */
            if (best < 0 || k->useOrder > bestOrder
                || (k->useOrder == bestOrder && e->signalDbm > bestSig)) {
                best = (int)i; bestScan = (int)s; bestOrder = k->useOrder; bestSig = e->signalDbm;
            }
            break;                      /* one scan entry per known SSID is enough */
        }
    }
    if (scanOut) *scanOut = bestScan;
    return best;
}

/* The same, against a published scan block (the UI's view). */
static int ApKnownPickJoin(const ApKnownDb *db, const ApScanBlock *scan, int *scanOut)
{
    return ApKnownPickJoinItems(db, scan->items, scan->count, scanOut);
}

/* k212: the network the user most recently chose (the panel marks it used on every pick). Unlike
 * ApKnownPickJoin, this does NOT require the network to be in the current scan -- an EXPLICIT choice must be
 * honored even when that SSID is momentarily out of the last scan (a multi-SSID AP drops in and out), rather
 * than silently joining a different saved network. Returns -1 if there is no joinable, auto-join network. */
static int ApKnownMostRecentJoinable(const ApKnownDb *db)
{
    int best = -1;
    ApStU32 bestOrder = 0, i;
    for (i = 0; i < db->count; i++) {
        const ApKnownNet *k = &db->net[i];
        if (!(k->flags & kApKnownFAutoJoin)) continue;
        if (!ApScanSecurityJoinable(k->security)) continue;
        if (best < 0 || k->useOrder > bestOrder) { best = (int)i; bestOrder = k->useOrder; }
    }
    return best;
}

/* The scan index whose (joinable) SSID matches known network k, or -1 if that SSID is not in this scan. */
static int ApKnownScanIndexFor(const ApKnownDb *db, int k, const ApScanItem *items, ApStU32 count)
{
    const ApKnownNet *e = &db->net[k];
    ApStU32 s;
    for (s = 0; s < count && s < kApScanMax; s++) {
        if (!(items[s].flags & kApScanFJoinable)) continue;
        if (ApKnownSsidEq(items[s].ssid, items[s].ssidLen, e->ssid, e->ssidLen)) return (int)s;
    }
    return -1;
}

/* ---- serialize to / from the on-disk bytes ----------------------------------------------------------- *
 * buf must hold kApKnownFileSize bytes. Explicit big-endian layout; no struct is ever written to disk. */
static void ApKnownPut32(ApStU8 *p, ApStU32 v)
{ p[0] = (ApStU8)(v >> 24); p[1] = (ApStU8)(v >> 16); p[2] = (ApStU8)(v >> 8); p[3] = (ApStU8)v; }
static void ApKnownPut16(ApStU8 *p, unsigned v) { p[0] = (ApStU8)(v >> 8); p[1] = (ApStU8)v; }
static ApStU32 ApKnownGet32(const ApStU8 *p)
{ return ((ApStU32)p[0] << 24) | ((ApStU32)p[1] << 16) | ((ApStU32)p[2] << 8) | p[3]; }
static unsigned ApKnownGet16(const ApStU8 *p) { return ((unsigned)p[0] << 8) | p[1]; }

static void ApKnownSave(const ApKnownDb *db, ApStU8 *buf)
{
    ApStU32 i, j;
    ApStU32 n = (db->count > kApKnownMax) ? kApKnownMax : db->count;
    for (i = 0; i < kApKnownFileSize; i++) buf[i] = 0;
    ApKnownPut32(buf + 0, kApKnownMagic);
    ApKnownPut16(buf + 4, kApKnownVersion);
    ApKnownPut16(buf + 6, (unsigned)n);
    ApKnownPut32(buf + 8, db->nextUseOrder);
    for (i = 0; i < n; i++) {
        ApStU8 *r = buf + kApKnownHdrSize + i * kApKnownRecSize;
        const ApKnownNet *e = &db->net[i];
        r[0] = e->ssidLen; r[1] = e->security; r[2] = e->flags; r[3] = 0;
        for (j = 0; j < 32u; j++) r[4 + j] = e->ssid[j];
        for (j = 0; j < 32u; j++) r[36 + j] = e->pmk[j];
        ApKnownPut32(r + 68, e->useOrder);
    }
}

/* Returns 1 if buf is a known-networks file this build understands, filling *db; 0 (and a cleared db)
 * otherwise -- a bad magic, a future version, or a count that does not fit. A zero-length or absent file is
 * the caller's to detect (it never calls this); an all-zero buffer fails the magic and clears, which is the
 * right "start with no known networks" outcome. */
static int ApKnownLoad(ApKnownDb *db, const ApStU8 *buf)
{
    ApStU32 i, j, n;
    ApKnownClear(db);
    if (ApKnownGet32(buf + 0) != kApKnownMagic) return 0;
    if (ApKnownGet16(buf + 4) != kApKnownVersion) return 0;
    n = ApKnownGet16(buf + 6);
    if (n > kApKnownMax) return 0;
    db->nextUseOrder = ApKnownGet32(buf + 8);
    if (db->nextUseOrder == 0) db->nextUseOrder = 1;
    for (i = 0; i < n; i++) {
        const ApStU8 *r = buf + kApKnownHdrSize + i * kApKnownRecSize;
        ApKnownNet *e = &db->net[i];
        e->ssidLen = r[0]; e->security = r[1]; e->flags = r[2];
        if (e->ssidLen > 32) { ApKnownClear(db); return 0; }   /* a record we cannot trust: refuse the file */
        for (j = 0; j < 32u; j++) e->ssid[j] = r[4 + j];
        for (j = 0; j < 32u; j++) e->pmk[j] = r[36 + j];
        e->useOrder = ApKnownGet32(r + 68);
    }
    db->count = n;
    return 1;
}

#endif /* AP_KNOWN_H */
