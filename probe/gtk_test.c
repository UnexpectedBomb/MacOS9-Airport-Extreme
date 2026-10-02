/* gtk_test.c -- host test for the simple parts of ap_gtk.h (k208):
 *   - the replay counter comparison (big-endian, 64-bit)
 *   - the group-key table, by key index (a renewal keeps the old index usable; a retried renewal is
 *     recognised as the same key)
 *   - reading the key index a protected data frame names in its CCMP header
 * The group key handshake itself is proven on the MDD, against the access point's own renewals.
 *
 *   cc -std=c99 -Wall -o gtk_test gtk_test.c && ./gtk_test
 */
#include <stdio.h>
#include <string.h>
#include "ap_gtk.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{
    gChecks++;
    if (!ok) { gFails++; printf("  FAIL: %s\n", what); }
}

static void TestReplay(void)
{
    ApU8 a[8] = { 0 }, b[8] = { 0 };
    Check("equal is not newer", !ApGkReplayNewer(a, b));
    a[7] = 1;
    Check("+1 in the low byte is newer, and not the other way round", ApGkReplayNewer(a, b) && !ApGkReplayNewer(b, a));
    memset(a, 0, 8); a[0] = 1; memset(b, 0xFF, 8); b[0] = 0;
    Check("big-endian: the high byte decides", ApGkReplayNewer(a, b) && !ApGkReplayNewer(b, a));
    memset(a, 0, 8); memset(b, 0, 8); a[3] = 2; b[3] = 1; b[7] = 0xFF;
    Check("a middle byte outranks every byte after it", ApGkReplayNewer(a, b));
}

static void TestTable(void)
{
    static ApGtkTable t;
    static const ApU8 k1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    static const ApU8 k2[16] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };
    ApGtkClear(&t);
    Check("a cleared table has no key at any index",
          !ApGtkFor(&t, 0) && !ApGtkFor(&t, 1) && !ApGtkFor(&t, 2) && !ApGtkFor(&t, 3));
    ApGtkInstall(&t, 1, k1, 16);
    Check("installed at 1: found at 1, not at 2", ApGtkFor(&t, 1) && memcmp(ApGtkFor(&t, 1), k1, 16) == 0 && !ApGtkFor(&t, 2));
    ApGtkInstall(&t, 2, k2, 16);
    Check("a renewal at 2 keeps 1 usable (frames under the old index still decrypt)",
          ApGtkFor(&t, 1) && memcmp(ApGtkFor(&t, 1), k1, 16) == 0 && ApGtkFor(&t, 2) && memcmp(ApGtkFor(&t, 2), k2, 16) == 0);
    ApGtkInstall(&t, 1, k2, 16);
    Check("the next renewal replaces 1 and leaves 2", memcmp(ApGtkFor(&t, 1), k2, 16) == 0 && memcmp(ApGtkFor(&t, 2), k2, 16) == 0);
    ApGtkInstall(&t, 4, k1, 16); ApGtkInstall(&t, -1, k1, 16);
    Check("an index outside 0..3 is ignored", !ApGtkFor(&t, 4) && !ApGtkFor(&t, -1) && !ApGtkFor(&t, 0));
    ApGtkInstall(&t, 0, k1, 5);
    Check("a key shorter than CCMP's 16 bytes is not usable", !ApGtkFor(&t, 0));
    ApGtkInstall(&t, 3, k1, 0);
    Check("a zero-length install is ignored", !ApGtkFor(&t, 3));
    ApGtkInstall(&t, 3, k1, 33);
    Check("an over-long install is ignored", !ApGtkFor(&t, 3));
    ApGtkInstall(&t, 3, k2, 16); ApGtkInstall(&t, 3, k1, 0); ApGtkInstall(&t, 3, k1, 33);
    Check("... and neither disturbs the key already there", ApGtkFor(&t, 3) && memcmp(ApGtkFor(&t, 3), k2, 16) == 0);
    { static const ApU8 k32[32] = { 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9 };
      ApGtkInstall(&t, 3, k32, 32); ApGtkInstall(&t, 3, k1, 16);
      Check("a shorter key replacing a longer one leaves no tail of the old", t.len[3] == 16 && t.key[3][16] == 0 && t.key[3][31] == 0); }
    ApGtkClear(&t);
    Check("clear empties every slot (a new association starts clean)", !ApGtkFor(&t, 1) && !ApGtkFor(&t, 2));
    ApGtkInstall(&t, 1, k1, 16);
    Check("same: the key already at that index", ApGtkSame(&t, 1, k1, 16));
    Check("same: a different key at that index is not", !ApGtkSame(&t, 1, k2, 16));
    Check("same: the right key at another index is not", !ApGtkSame(&t, 2, k1, 16));
    Check("same: a length that differs is not (15 of the same bytes)", !ApGtkSame(&t, 1, k1, 15));
    Check("same: an index outside 0..3 is not", !ApGtkSame(&t, 4, k1, 16) && !ApGtkSame(&t, -1, k1, 16));
    { ApU8 k3[16]; memcpy(k3, k1, 16); k3[15] ^= 1;
      Check("same: one bit of difference in the last byte is not", !ApGtkSame(&t, 1, k3, 16)); }
    ApGtkClear(&t);
    Check("same: nothing matches an empty slot", !ApGtkSame(&t, 1, k1, 16));
}

/* An 802.11 data frame header with a CCMP header after it: fc (little-endian), the key index and ExtIV bit
 * in the CCMP header's 4th byte. Returns a length with room for the CCMP header and a little body. */
static ApU32 MkHdr(ApU8 *f, unsigned fc, int keyIndex, int extIv)
{
    ApU32 hdr = 24u + (((fc & 0x0300u) == 0x0300u) ? 6u : 0u) + ((((fc >> 4) & 0x0Fu) == 8u) ? 2u : 0u);
    memset(f, 0, 96);
    f[0] = (ApU8)(fc & 0xFFu); f[1] = (ApU8)(fc >> 8);
    f[hdr + 3] = (ApU8)(((unsigned)keyIndex << 6) | (extIv ? 0x20u : 0u));
    return hdr + 8u + 24u;
}

static void TestFrameKeyId(void)
{
    ApU8 f[96];
    ApU32 n;
    n = MkHdr(f, 0x4208u, 2, 1);                  /* data, FromDS, Protected: the AP to us */
    Check("AP->STA protected data: key index 2", ApGtkFrameKeyId(f, n) == 2);
    n = MkHdr(f, 0x4288u, 1, 1);                  /* QoS data (subtype 8): the header grows by 2 */
    Check("QoS data: the CCMP header is found after the QoS field (index 1)", ApGtkFrameKeyId(f, n) == 1);
    n = MkHdr(f, 0x4308u, 3, 1);                  /* ToDS and FromDS: 4-address, +6 */
    Check("4-address data: after the fourth address (index 3)", ApGtkFrameKeyId(f, n) == 3);
    n = MkHdr(f, 0x4388u, 1, 1);                  /* 4-address QoS: +6 +2 */
    Check("4-address QoS data (index 1)", ApGtkFrameKeyId(f, n) == 1);
    n = MkHdr(f, 0x4108u, 2, 1);                  /* ToDS only (STA->AP): still a 3-address header */
    Check("ToDS-only data: 3-address, no +6 (index 2)", ApGtkFrameKeyId(f, n) == 2);
    n = MkHdr(f, 0x0208u, 2, 1);                  /* not Protected */
    Check("an unprotected frame names no key", ApGtkFrameKeyId(f, n) == -1);
    n = MkHdr(f, 0x4080u, 2, 1);                  /* management (beacon subtype 8, type 0) */
    Check("a management frame names no key", ApGtkFrameKeyId(f, n) == -1);
    n = MkHdr(f, 0x4208u, 2, 0);                  /* no ExtIV: not CCMP */
    Check("no ExtIV bit (not CCMP) names no key", ApGtkFrameKeyId(f, n) == -1);
    (void)MkHdr(f, 0x4208u, 2, 1);
    Check("too short to hold the CCMP header", ApGtkFrameKeyId(f, 24u + 7u) == -1);
    Check("just long enough", ApGtkFrameKeyId(f, 24u + 8u) == 2);
    Check("shorter than a MAC header", ApGtkFrameKeyId(f, 23u) == -1);
    Check("NULL", ApGtkFrameKeyId(NULL, 64u) == -1);
    n = MkHdr(f, 0x42C8u, 1, 1);                  /* QoS NULL (subtype 12) is not QoS data: no +2, as ApCcmpDecap */
    Check("the header rule is ApCcmpDecap's (QoS means subtype 8 only)",
          ApGtkFrameKeyId(f, n) == ((f[24 + 3] >> 6) & 3) && ApGtkFrameKeyId(f, n) == 1);
}

int main(void)
{
    TestReplay();
    TestTable();
    TestFrameKeyId();
    printf(gFails ? "  [!!] %d of %d checks FAILED\n" : "  [ok] %d checks, %d failed\n",
           gFails ? gFails : gChecks, gFails ? gChecks : 0);
    return gFails ? 1 : 0;
}
