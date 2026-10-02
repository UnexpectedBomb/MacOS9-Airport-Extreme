/* known_test.c -- host test for ap_known.h (k210): the known-networks store, join selection, and the
 * on-disk serialization. All PMKs here are FAKE (byte patterns), never a real key.
 *
 *   cc -std=c99 -Wall -Wextra -Wno-unused-function -o known_test known_test.c && ./known_test
 */
#include <stdio.h>
#include <string.h>
#include "ap_known.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok) { gChecks++; if (!ok) { gFails++; printf("  FAIL: %s\n", what); } }

static ApStU8 *S(const char *s) { return (ApStU8 *)s; }
static ApStU8 L(const char *s) { return (ApStU8)strlen(s); }

/* a fake 32-byte PMK, distinct per seed */
static void FakePmk(ApStU8 *out, int seed) { int i; for (i = 0; i < 32; i++) out[i] = (ApStU8)(seed * 7 + i); }

/* a scan block with the given networks (name, security, dbm) */
static void MkScan(ApScanBlock *b, const char **names, const int *sec, const int *dbm, int n)
{
    int i, j;
    memset(b, 0, sizeof(*b));
    b->magic = kApScanMagic; b->version = kApScanVersion; b->size = sizeof(*b); b->count = (ApStU32)n;
    for (i = 0; i < n && i < (int)kApScanMax; i++) {
        ApScanItem *e = &b->items[i];
        e->ssidLen = (ApStU8)strlen(names[i]);
        for (j = 0; j < (int)e->ssidLen; j++) e->ssid[j] = (ApStU8)names[i][j];
        e->security = (ApStU8)sec[i];
        e->signalDbm = (ApStS32)dbm[i];
        e->flags = (ApStU8)(ApScanSecurityJoinable((unsigned)sec[i]) ? kApScanFJoinable : 0);
    }
}

static void TestAddFindRemove(void)
{
    static ApKnownDb db;
    ApStU8 pmk[32], got;
    int i;
    ApKnownClear(&db);
    Check("empty: count 0, find misses", db.count == 0 && ApKnownFind(&db, S("Home"), 4) == -1);
    FakePmk(pmk, 1);
    Check("add WPA2 returns index 0", ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, pmk) == 0 && db.count == 1);
    Check("find hits, the PMK stored", ApKnownFind(&db, S("Home"), 4) == 0
          && memcmp(db.net[0].pmk, pmk, 32) == 0 && db.net[0].security == kApSecWpa2Psk);
    Check("add open stores a ZERO pmk", ApKnownAdd(&db, S("Cafe"), 4, kApSecOpen, NULL) == 1
          && db.net[1].pmk[0] == 0 && db.net[1].pmk[31] == 0 && db.net[1].security == kApSecOpen);
    { ApStU8 p2[32]; FakePmk(p2, 9);
      Check("re-add same SSID updates in place (new password), no new row",
            ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, p2) == 0 && db.count == 2
            && memcmp(db.net[0].pmk, p2, 32) == 0); }
    Check("add too-long SSID rejected", ApKnownAdd(&db, S("0123456789012345678901234567890123"), 34, kApSecOpen, NULL) == -1);
    Check("add zero-length SSID rejected", ApKnownAdd(&db, S(""), 0, kApSecOpen, NULL) == -1);
    /* a name that is a PREFIX of another must not match it: find compares length, not just the prefix */
    FakePmk(pmk, 5); ApKnownAdd(&db, S("Home5"), 5, kApSecWpa2Psk, pmk);
    Check("find respects length: \"Home\" does not match \"Home5\"",
          ApKnownFind(&db, S("Home"), 4) == 0 && ApKnownFind(&db, S("Home5"), 5) == 2
          && ApKnownFind(&db, S("Hom"), 3) == -1);
    ApKnownRemove(&db, S("Home5"), 5);
    /* an OPEN network keeps a zero PMK even if the caller hands one in (open frames are never encrypted) */
    FakePmk(pmk, 7);
    { int oi = ApKnownAdd(&db, S("OpenWithKey"), 11, kApSecOpen, pmk);
      Check("open network stores a zero PMK even when one is passed",
            oi >= 0 && db.net[oi].pmk[0] == 0 && db.net[oi].pmk[5] == 0 && db.net[oi].pmk[31] == 0);
      ApKnownRemove(&db, S("OpenWithKey"), 11); }
    /* fill to the max */
    for (i = 2; i < (int)kApKnownMax; i++) { char nm[8]; sprintf(nm, "N%02d", i); FakePmk(pmk, i); ApKnownAdd(&db, S(nm), L(nm), kApSecWpa2Psk, pmk); }
    Check("full at kApKnownMax", db.count == kApKnownMax);
    FakePmk(pmk, 99);
    Check("add past full rejected", ApKnownAdd(&db, S("OneMore"), 7, kApSecWpa2Psk, pmk) == -1 && db.count == kApKnownMax);
    Check("update an existing one still works when full", ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, pmk) == 0);
    got = db.net[1].ssidLen;
    ApKnownRemove(&db, S("Home"), 4);
    Check("remove packs the table (Cafe slid to 0), count drops",
          db.count == kApKnownMax - 1 && ApKnownFind(&db, S("Home"), 4) == -1
          && ApKnownFind(&db, S("Cafe"), 4) == 0 && db.net[0].ssidLen == got);
    ApKnownRemove(&db, S("NotThere"), 8);
    Check("remove a missing SSID is a no-op", db.count == kApKnownMax - 1);
}

static void TestPickJoin(void)
{
    static ApKnownDb db;
    static ApScanBlock scan;
    ApStU8 pmk[32];
    int sc;
    const char *names[4]; int sec[4], dbm[4];

    ApKnownClear(&db);
    FakePmk(pmk, 1); ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, pmk);
    FakePmk(pmk, 2); ApKnownAdd(&db, S("Work"), 4, kApSecWpa2Psk, pmk);
    ApKnownAdd(&db, S("Cafe"), 4, kApSecOpen, NULL);

    /* all three in range, none ever used -> strongest signal wins */
    names[0]="Home"; sec[0]=kApSecWpa2Psk; dbm[0]=-70;
    names[1]="Work"; sec[1]=kApSecWpa2Psk; dbm[1]=-55;
    names[2]="Cafe"; sec[2]=kApSecOpen;    dbm[2]=-80;
    MkScan(&scan, names, sec, dbm, 3);
    Check("never used: strongest in range (Work)", ApKnownPickJoin(&db, &scan, &sc) == ApKnownFind(&db, S("Work"), 4)
          && sc == 1);

    /* mark Home used -> last-used wins even though Work is stronger */
    ApKnownTouch(&db, ApKnownFind(&db, S("Home"), 4));
    Check("last used wins over stronger (Home)", ApKnownPickJoin(&db, &scan, &sc) == ApKnownFind(&db, S("Home"), 4));

    /* then mark Cafe used more recently -> Cafe wins */
    ApKnownTouch(&db, ApKnownFind(&db, S("Cafe"), 4));
    Check("most-recently used wins (Cafe)", ApKnownPickJoin(&db, &scan, &sc) == ApKnownFind(&db, S("Cafe"), 4));

    /* Cafe (last used) drops out of range -> fall back to the other candidates' last-used (Home) */
    names[0]="Home"; sec[0]=kApSecWpa2Psk; dbm[0]=-70;
    names[1]="Work"; sec[1]=kApSecWpa2Psk; dbm[1]=-55;
    MkScan(&scan, names, sec, dbm, 2);
    Check("last-used out of range: next most-recently-used in range (Home)",
          ApKnownPickJoin(&db, &scan, &sc) == ApKnownFind(&db, S("Home"), 4));

    /* none of our known nets in range */
    names[0]="Stranger"; sec[0]=kApSecWpa2Psk; dbm[0]=-40;
    MkScan(&scan, names, sec, dbm, 1);
    Check("nothing known in range -> -1", ApKnownPickJoin(&db, &scan, &sc) == -1 && sc == -1);

    /* k212: an EXPLICIT choice (join command) honors the most-recently-used network even when it is NOT in the
     * current scan -- it must never substitute a different in-range network (the wrong-saved-network bug). */
    ApKnownTouch(&db, ApKnownFind(&db, S("Work"), 4));
    Check("k212 most-recent-joinable = Work (ignores scan)",
          ApKnownMostRecentJoinable(&db) == ApKnownFind(&db, S("Work"), 4));
    ApKnownTouch(&db, ApKnownFind(&db, S("Home"), 4));
    Check("k212 most-recent-joinable follows the latest touch (Home)",
          ApKnownMostRecentJoinable(&db) == ApKnownFind(&db, S("Home"), 4));
    names[0]="Home"; sec[0]=kApSecWpa2Psk; dbm[0]=-70;
    names[1]="Work"; sec[1]=kApSecWpa2Psk; dbm[1]=-55;
    MkScan(&scan, names, sec, dbm, 2);
    Check("k212 scan-index-for: Home is at 0",
          ApKnownScanIndexFor(&db, ApKnownFind(&db, S("Home"), 4), scan.items, scan.count) == 0);
    MkScan(&scan, names, sec, dbm, 1);               /* only Home in range now */
    Check("k212 scan-index-for: Work not in scan -> -1 (join by SSID instead)",
          ApKnownScanIndexFor(&db, ApKnownFind(&db, S("Work"), 4), scan.items, scan.count) == -1);

    /* a known net heard but as a security we cannot join (its scan entry not joinable) */
    ApKnownClear(&db);
    FakePmk(pmk, 5); ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, pmk);
    names[0]="Home"; sec[0]=kApSecEnterprise; dbm[0]=-40;    /* same name, but the AP now advertises enterprise */
    MkScan(&scan, names, sec, dbm, 1);
    Check("known SSID present but not joinable in the scan -> -1", ApKnownPickJoin(&db, &scan, &sc) == -1);

    /* a known entry whose STORED security is not joinable (e.g. loaded from an older/odd file) is excluded
     * even if the scan heard the SSID as something joinable -- the guard is on the known net, not only the scan */
    ApKnownClear(&db);
    FakePmk(pmk, 8); ApKnownAdd(&db, S("Legacy"), 6, kApSecWep, pmk);   /* WEP: not joinable */
    names[0]="Legacy"; sec[0]=kApSecOpen; dbm[0]=-40;                   /* scan hears it as open (joinable) */
    MkScan(&scan, names, sec, dbm, 1);
    Check("known net with non-joinable stored security -> excluded", ApKnownPickJoin(&db, &scan, &sc) == -1);

    /* auto-join cleared -> excluded */
    ApKnownClear(&db);
    FakePmk(pmk, 6); ApKnownAdd(&db, S("Home"), 4, kApSecWpa2Psk, pmk);
    db.net[0].flags &= (ApStU8)~kApKnownFAutoJoin;
    names[0]="Home"; sec[0]=kApSecWpa2Psk; dbm[0]=-40;
    MkScan(&scan, names, sec, dbm, 1);
    Check("auto-join off -> not a candidate", ApKnownPickJoin(&db, &scan, &sc) == -1);

    /* empty scan */
    memset(&scan, 0, sizeof(scan)); scan.magic = kApScanMagic; scan.version = kApScanVersion; scan.size = sizeof(scan);
    Check("empty scan -> -1", ApKnownPickJoin(&db, &scan, &sc) == -1);
}

static void TestSerialize(void)
{
    static ApKnownDb a, b;
    static ApStU8 buf[kApKnownFileSize];
    ApStU8 pmk[32];
    int i;
    ApKnownClear(&a);
    FakePmk(pmk, 3); ApKnownAdd(&a, S("Alpha"), 5, kApSecWpa2Psk, pmk);
    ApKnownAdd(&a, S("Bravo"), 5, kApSecOpen, NULL);
    FakePmk(pmk, 4); ApKnownAdd(&a, S("Charlie"), 7, kApSecWpa2Psk, pmk);
    ApKnownTouch(&a, 2); ApKnownTouch(&a, 0);          /* give useOrder some non-trivial values */

    ApKnownSave(&a, buf);
    Check("file size constant is header + 16 records", kApKnownFileSize == 16u + 72u * 16u);
    Check("load round-trips", ApKnownLoad(&b, buf) && b.count == a.count && b.nextUseOrder == a.nextUseOrder);
    Check("every field survives (incl. the PMK and useOrder)", memcmp(a.net, b.net, sizeof(a.net)) == 0);
    Check("byte layout: magic, version, count, ssid at +20, pmk at +52",
          ApKnownGet32(buf) == kApKnownMagic && ApKnownGet16(buf + 4) == kApKnownVersion
          && ApKnownGet16(buf + 6) == 3 && buf[16 + 4] == 'A' && buf[16 + 36] == pmk[0] - (4 * 7 - 3 * 7));
    /* the "Alpha" record's pmk is FakePmk(3); check its first byte lands at record0 offset 36 */
    { ApStU8 p3[32]; FakePmk(p3, 3); Check("Alpha's PMK at record 0 offset 36", buf[16 + 36] == p3[0]); }

    /* rejections */
    { ApStU8 bad[kApKnownFileSize]; memcpy(bad, buf, sizeof(bad));
      bad[0] ^= 0xFF; Check("bad magic rejected, db cleared", !ApKnownLoad(&b, bad) && b.count == 0); }
    { ApStU8 bad[kApKnownFileSize]; memcpy(bad, buf, sizeof(bad));
      ApKnownPut16(bad + 4, 999); Check("future version rejected", !ApKnownLoad(&b, bad)); }
    /* an over-count must be rejected by the COUNT guard, not incidentally by an out-of-bounds record read.
     * Use a buffer with room for the extra (zeroed) record so nothing but the guard can reject it. */
    { static ApStU8 big[kApKnownFileSize + kApKnownRecSize];
      memcpy(big, buf, kApKnownFileSize);
      ApKnownPut16(big + 6, kApKnownMax + 1);
      Check("impossible count rejected by the count guard", !ApKnownLoad(&b, big) && b.count == 0); }
    { ApStU8 bad[kApKnownFileSize]; memcpy(bad, buf, sizeof(bad));
      bad[16] = 33; Check("a record with ssidLen > 32 rejects the whole file", !ApKnownLoad(&b, bad) && b.count == 0); }
    { static ApStU8 zeros[kApKnownFileSize];
      for (i = 0; i < (int)kApKnownFileSize; i++) zeros[i] = 0;
      Check("an all-zero buffer loads as empty (first run)", !ApKnownLoad(&b, zeros) && b.count == 0); }

    /* save clamps a corrupt over-count */
    a.count = 999; ApKnownSave(&a, buf);
    Check("save clamps count to the max", ApKnownGet16(buf + 6) == kApKnownMax);
}

int main(void)
{
    TestAddFindRemove();
    TestPickJoin();
    TestSerialize();
    printf(gFails ? "  [!!] %d of %d checks FAILED\n" : "  [ok] %d checks, %d failed\n",
           gFails ? gFails : gChecks, gFails ? gChecks : 0);
    return gFails ? 1 : 0;
}
