/* scan_test.c -- host test for ap_scan.h and ap_scanlist.h (k209): the beacon parser and its security
 * classification, the scan table, the power-management null frame, and the published block's seq protocol.
 *
 *   cc -std=c99 -Wall -o scan_test scan_test.c && ./scan_test
 *
 * The frames below are built by hand from the standard (802.11-2016 9.3.3.3 beacon, 9.4.2.25 RSN element);
 * the names and addresses are made up. */
#include <stdio.h>
#include <string.h>

static int gHookBumps = 0, gHookCalls = 0;
#define AP_SCAN_READ_HOOK(b) do { gHookCalls++; if (gHookBumps > 0) { gHookBumps--; ((volatile ApScanBlock *)(b))->seq += 2u; } } while (0)
#include "ap_scan.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{
    gChecks++;
    if (!ok) { gFails++; printf("  FAIL: %s\n", what); }
}

/* ---- a beacon, element by element ---- */
static ApStU8 gF[512];
static ApStU32 gN;
static const ApStU8 kBssid[6] = { 0x02, 0x00, 0x00, 0xAA, 0xBB, 0x01 };

static void Begin(unsigned subtype, unsigned cap)
{
    memset(gF, 0, sizeof(gF));
    gF[0] = (ApStU8)(subtype << 4);                   /* type 0 management */
    memset(gF + 4, 0xFF, 6);                          /* addr1: broadcast */
    memcpy(gF + 10, kBssid, 6);                       /* addr2 */
    memcpy(gF + 16, kBssid, 6);                       /* addr3: the BSSID */
    gF[24 + 8] = 0x64;                                /* beacon interval 100 TU */
    gF[24 + 10] = (ApStU8)(cap & 0xFF); gF[24 + 11] = (ApStU8)(cap >> 8);
    gN = 36;
}
static void Ie(ApStU8 id, const void *body, ApStU8 len) { gF[gN++] = id; gF[gN++] = len; memcpy(gF + gN, body, len); gN += len; }
static void Ssid(const char *s) { Ie(0, s, (ApStU8)strlen(s)); }
static void Ds(ApStU8 ch) { Ie(3, &ch, 1); }

/* An RSN element: group cipher, pairwise list, AKM list, capabilities (-1 = omit). */
static void Rsn(int group, const int *pw, int npw, const int *akm, int nakm, int caps)
{
    ApStU8 b[64]; int n = 0, k;
    b[n++] = 1; b[n++] = 0;                                                          /* version 1 */
    b[n++] = 0x00; b[n++] = 0x0F; b[n++] = 0xAC; b[n++] = (ApStU8)group;
    b[n++] = (ApStU8)npw; b[n++] = 0;
    for (k = 0; k < npw; k++) { b[n++] = 0x00; b[n++] = 0x0F; b[n++] = 0xAC; b[n++] = (ApStU8)pw[k]; }
    b[n++] = (ApStU8)nakm; b[n++] = 0;
    for (k = 0; k < nakm; k++) { b[n++] = 0x00; b[n++] = 0x0F; b[n++] = 0xAC; b[n++] = (ApStU8)akm[k]; }
    if (caps >= 0) { b[n++] = (ApStU8)(caps & 0xFF); b[n++] = (ApStU8)(caps >> 8); }
    Ie(48, b, (ApStU8)n);
}
static void WpaIe(void)
{
    static const ApStU8 w[] = { 0x00, 0x50, 0xF2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xF2, 0x02,
                                0x01, 0x00, 0x00, 0x50, 0xF2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xF2, 0x02 };
    Ie(221, w, sizeof(w));
}

static unsigned Classify(void)                   /* parse gF and return the security, or 99 if it did not parse */
{
    ApScanItem it;
    return ApScanParse(gF, gN, 6, &it) ? it.security : 99u;
}

static void TestSecurity(void)
{
    static const int ccmp[] = { 4 }, ccmpTkip[] = { 4, 2 }, tkip[] = { 2 };
    static const int psk[] = { 2 }, pskSae[] = { 2, 8 }, sae[] = { 8 }, dot1x[] = { 1 }, psk256[] = { 6 }, owe[] = { 18 };
    ApScanItem it;

    Begin(8, 0x0001); Ssid("TestOpen"); Ds(6);
    Check("open: parses", ApScanParse(gF, gN, 6, &it) == 1);
    Check("open: security Open, joinable, channel 6, named",
          it.security == kApSecOpen && (it.flags & kApScanFJoinable) && it.channel == 6 && it.ssidLen == 8
          && memcmp(it.ssid, "TestOpen", 8) == 0 && !(it.flags & kApScanFHidden));
    Check("open: the BSSID comes from addr3", memcmp(it.bssid, kBssid, 6) == 0);

    Begin(8, 0x0011); Ssid("Home"); Ds(1); Rsn(4, ccmp, 1, psk, 1, 0);
    Check("WPA2-Personal (PSK, CCMP/CCMP) is joinable", Classify() == kApSecWpa2Psk);
    Begin(8, 0x0011); Ssid("Home"); Ds(1); Rsn(4, ccmp, 1, pskSae, 2, 0x0080);
    Check("WPA2/WPA3 transition (PSK+SAE, MFP capable) joins as WPA2", Classify() == kApSecWpa2Wpa3);
    Begin(8, 0x0011); Ssid("Home"); Ds(1); Rsn(4, ccmp, 1, sae, 1, 0x00C0);
    Check("WPA3 only (SAE, MFP required) is not joinable", Classify() == kApSecWpa3);
    Begin(8, 0x0011); Ssid("Office"); Ds(1); Rsn(4, ccmp, 1, dot1x, 1, 0);
    Check("802.1X is Enterprise", Classify() == kApSecEnterprise);
    Begin(8, 0x0011); Ssid("Mixed"); Ds(1); Rsn(2, ccmpTkip, 2, psk, 1, 0); WpaIe();
    Check("WPA/WPA2 mixed (TKIP group) is not joinable: broadcasts would not decrypt", Classify() == kApSecTkipGroup);
    Begin(8, 0x0011); Ssid("Old"); Ds(1); WpaIe();
    Check("WPA only (vendor element, no RSN)", Classify() == kApSecWpaTkip);
    Begin(8, 0x0011); Ssid("Older"); Ds(1);
    Check("privacy with no RSN or WPA element is WEP", Classify() == kApSecWep);
    Begin(8, 0x0011); Ssid("Strict"); Ds(1); Rsn(4, ccmp, 1, psk, 1, 0x00C0);
    Check("PSK with PMF required is not joinable", Classify() == kApSecPmfRequired);
    Begin(8, 0x0011); Ssid("Sha"); Ds(1); Rsn(4, ccmp, 1, psk256, 1, 0x0080);
    Check("PSK-SHA256 only (an 802.11w AKM) is not joinable", Classify() == kApSecPmfRequired);
    Begin(8, 0x0011); Ssid("Owe"); Ds(1); Rsn(4, ccmp, 1, owe, 1, 0x00C0);
    Check("OWE is Enhanced Open", Classify() == kApSecOwe);
    Begin(8, 0x0011); Ssid("Wpa2Tkip"); Ds(1); Rsn(2, tkip, 1, psk, 1, 0);
    Check("WPA2 with TKIP only", Classify() == kApSecWpaTkip);
    Begin(8, 0x0011); Ssid("NoCaps"); Ds(1); Rsn(4, ccmp, 1, psk, 1, -1);
    Check("capabilities omitted: still WPA2-Personal", Classify() == kApSecWpa2Psk);

    { static const ApStU8 onlyVer[] = { 1, 0 };
      Begin(8, 0x0011); Ssid("Defaults"); Ds(1); Ie(48, onlyVer, 2);
      Check("RSN with only a version: the defaults (CCMP, 802.1X) -> Enterprise", Classify() == kApSecEnterprise); }
    { static const ApStU8 grpOnly[] = { 1, 0, 0x00, 0x0F, 0xAC, 4 };
      Begin(8, 0x0011); Ssid("Defaults2"); Ds(1); Ie(48, grpOnly, sizeof(grpOnly));
      Check("RSN ending after the group cipher: AKM default 802.1X", Classify() == kApSecEnterprise); }
    { static const ApStU8 lies[] = { 1, 0, 0x00, 0x0F, 0xAC, 4, 9, 0, 0x00, 0x0F, 0xAC, 4 };
      Begin(8, 0x0011); Ssid("Liar"); Ds(1); Ie(48, lies, sizeof(lies));
      Check("a pairwise count the element cannot hold -> unknown, not a guess", Classify() == kApSecUnknown); }
    { static const ApStU8 v2[] = { 2, 0, 0x00, 0x0F, 0xAC, 4 };
      Begin(8, 0x0011); Ssid("Future"); Ds(1); Ie(48, v2, sizeof(v2));
      Check("RSN version 2 -> unknown (and not WEP, despite the privacy bit)", Classify() == kApSecUnknown); }
    Begin(8, 0x0011); Ssid("Two"); Ds(1); Rsn(4, ccmp, 1, psk, 1, 0); Rsn(4, ccmp, 1, dot1x, 1, 0);
    Check("two RSN elements: the first counts", Classify() == kApSecWpa2Psk);
    { int k; for (k = 0; k < kApSecCount; k++)
        Check(ApScanSecurityName((unsigned)k),
              ApScanSecurityJoinable((unsigned)k) == (k == kApSecOpen || k == kApSecWpa2Psk || k == kApSecWpa2Wpa3)); }
}

static void TestFraming(void)
{
    ApScanItem it;
    static const ApStU8 zeros[8] = { 0 };
    static const char long40[] = "0123456789012345678901234567890123456789";

    Begin(8, 0x0001); Ie(0, "", 0); Ds(6);
    Check("hidden: an empty name", ApScanParse(gF, gN, 6, &it) && it.ssidLen == 0 && (it.flags & kApScanFHidden));
    Begin(8, 0x0001); Ie(0, zeros, 8); Ds(6);
    Check("hidden: a name of zero bytes is hidden too", ApScanParse(gF, gN, 6, &it) && it.ssidLen == 0
          && (it.flags & kApScanFHidden) && it.ssid[0] == 0);
    Begin(8, 0x0001); Ie(0, long40, 40); Ds(6);
    Check("a 40-byte name is cut to 32", ApScanParse(gF, gN, 6, &it) && it.ssidLen == 32
          && memcmp(it.ssid, long40, 32) == 0);
    Begin(8, 0x0001); Ssid("NoDs");
    Check("no DS element: the channel heard on", ApScanParse(gF, gN, 9, &it) && it.channel == 9);
    Begin(8, 0x0001); Ssid("Leak"); Ds(11);
    Check("heard on 9 but the beacon says 11 (overlapping channels): 11", ApScanParse(gF, gN, 9, &it) && it.channel == 11);
    Begin(8, 0x0001); Ssid("Bad"); Ds(15);
    Check("a DS channel outside 1..14 is ignored", ApScanParse(gF, gN, 4, &it) && it.channel == 4);
    Begin(8, 0x0002); Ssid("Adhoc"); Ds(6);
    Check("an IBSS (ad hoc) beacon is not listed", !ApScanParse(gF, gN, 6, &it));
    Begin(5, 0x0001); Ssid("Resp"); Ds(6);
    Check("a probe response parses, flagged as one", ApScanParse(gF, gN, 6, &it) && (it.flags & kApScanFProbeResp));
    Begin(4, 0x0001); Ssid("Req"); Ds(6);
    Check("a probe REQUEST is not a network", !ApScanParse(gF, gN, 6, &it));
    Begin(8, 0x0001); Ssid("Data"); gF[0] = 0x08;
    Check("a data frame is not a network", !ApScanParse(gF, gN, 6, &it));
    Begin(8, 0x0001); Ds(6);
    Check("no SSID element: not listed", !ApScanParse(gF, gN, 6, &it));
    Begin(8, 0x0001); Ssid("Cut"); gF[gN++] = 3; gF[gN++] = 5; gF[gN++] = 6;
    Check("a truncated element after the name: the name still counts, the walk stops",
          ApScanParse(gF, gN, 7, &it) && it.ssidLen == 3 && it.channel == 7);
    Begin(8, 0x0001); gF[gN++] = 0; gF[gN++] = 10; gF[gN++] = 'x';
    Check("the SSID element itself truncated: not listed", !ApScanParse(gF, gN, 6, &it));
    Begin(8, 0x0001);
    Check("shorter than header + fixed fields", !ApScanParse(gF, 35, 6, &it) && !ApScanParse(NULL, 64, 6, &it));
}

static ApScanItem Item(ApStU8 last, const char *ssid, int dbm)
{
    ApScanItem it;
    memset(&it, 0, sizeof(it));
    memcpy(it.bssid, kBssid, 6); it.bssid[5] = last;
    it.ssidLen = (ApStU8)strlen(ssid); memcpy(it.ssid, ssid, it.ssidLen);
    if (!it.ssidLen) it.flags |= kApScanFHidden;
    it.signalDbm = dbm; it.quality = (ApStU8)ApStatQuality(dbm, 5);
    return it;
}

static void TestTable(void)
{
    static ApScanTable t;
    ApScanItem a;
    int k;
    ApScanReset(&t);
    a = Item(1, "A", -70); ApScanAdd(&t, &a);
    a = Item(2, "B", -50); ApScanAdd(&t, &a);
    a = Item(3, "C", -80); ApScanAdd(&t, &a);
    Check("three BSSIDs, three entries", t.n == 3);
    a = Item(1, "A", -60); ApScanAdd(&t, &a);
    Check("the same BSSID stronger: the signal rises, no new entry", t.n == 3 && t.e[0].signalDbm == -60
          && t.e[0].quality == ApStatQuality(-60, 5));
    a = Item(1, "A", -90); ApScanAdd(&t, &a);
    Check("the same BSSID weaker: the strongest reading stays", t.e[0].signalDbm == -60);
    a = Item(4, "", -65); ApScanAdd(&t, &a);
    a = Item(4, "Secret", -66); a.flags |= kApScanFProbeResp; ApScanAdd(&t, &a);
    Check("a hidden network's name, learned from a probe response",
          t.n == 4 && t.e[3].ssidLen == 6 && memcmp(t.e[3].ssid, "Secret", 6) == 0
          && !(t.e[3].flags & kApScanFHidden) && (t.e[3].flags & kApScanFProbeResp) && t.e[3].signalDbm == -65);
    ApScanSort(&t);
    Check("sorted strongest first", t.e[0].signalDbm == -50 && t.e[1].signalDbm == -60 && t.e[2].signalDbm == -65
          && t.e[3].signalDbm == -80);

    ApScanReset(&t);
    for (k = 0; k < (int)kApScanMax; k++) { a = Item((ApStU8)(10 + k), "N", -40 - k); ApScanAdd(&t, &a); }
    Check("32 fit", t.n == kApScanMax && t.dropped == 0);
    a = Item(200, "Weak", -95); ApScanAdd(&t, &a);
    Check("full, and weaker than everything: dropped, counted", t.n == kApScanMax && t.dropped == 1);
    for (k = 0; k < (int)t.n; k++) Check("... and not stored", t.e[k].bssid[5] != 200);
    a = Item(201, "Strong", -30); ApScanAdd(&t, &a);
    { int found = 0, weakestGone = 1;
      for (k = 0; k < (int)t.n; k++) { if (t.e[k].bssid[5] == 201) found = 1; if (t.e[k].signalDbm == -71) weakestGone = 0; }
      Check("full, and stronger than the weakest: it replaces the weakest", found && weakestGone && t.dropped == 2); }

    ApScanReset(&t);
    a = Item(1, "X", -60); ApScanAdd(&t, &a);
    a = Item(2, "Y", -60); ApScanAdd(&t, &a);
    a = Item(3, "Z", -40); ApScanAdd(&t, &a);
    ApScanSort(&t);
    Check("the sort is stable for equal signals", t.e[0].bssid[5] == 3 && t.e[1].bssid[5] == 1 && t.e[2].bssid[5] == 2);
}

/* k216: the scan-merge rolling list (Apple's WirelessScanMerge): a network briefly missed must not vanish,
 * and lastSeen must ride along with the sort so a later age-out drops the right row. */
static void TestMerge(void)
{
    static ApScanTable t;
    ApScanItem a;
    unsigned s;

    ApScanReset(&t);
    ApScanMergeBegin(&t);
    a = Item(1, "A", -50); ApScanAdd(&t, &a);
    a = Item(2, "B", -60); ApScanAdd(&t, &a);
    a = Item(3, "C", -70); ApScanAdd(&t, &a);
    ApScanMergeEnd(&t, kApScanAgeSweeps);
    Check("merge: three heard, three held", t.n == 3);
    /* miss C for one sweep short of the age limit: it must still be there */
    for (s = 1; s < kApScanAgeSweeps; s++) {
        ApScanMergeBegin(&t);
        a = Item(1, "A", -50); ApScanAdd(&t, &a);
        a = Item(2, "B", -60); ApScanAdd(&t, &a);
        ApScanMergeEnd(&t, kApScanAgeSweeps);
    }
    Check("merge: a network missed a few sweeps does NOT vanish from the list", t.n == 3);
    /* one more unheard sweep and it ages out */
    ApScanMergeBegin(&t);
    a = Item(1, "A", -50); ApScanAdd(&t, &a);
    a = Item(2, "B", -60); ApScanAdd(&t, &a);
    ApScanMergeEnd(&t, kApScanAgeSweeps);
    Check("merge: a gone network ages out after kApScanAgeSweeps unheard sweeps", t.n == 2);
    /* hearing it again revives it */
    ApScanMergeBegin(&t);
    a = Item(3, "C", -70); ApScanAdd(&t, &a);
    ApScanMergeEnd(&t, kApScanAgeSweeps);
    Check("merge: a network heard again is held again", t.n == 3);

    /* lastSeen rides along with the sort: 'weak' is a sweep older than 'strong', so after sorting (which
     * reorders the rows) the age-out must still drop 'weak' first. If the stamp did not move with its row,
     * the age-out would drop 'strong' instead and this check would fail. */
    ApScanReset(&t);
    ApScanMergeBegin(&t); a = Item(1, "weak",   -90); ApScanAdd(&t, &a); ApScanMergeEnd(&t, kApScanAgeSweeps);
    ApScanMergeBegin(&t); a = Item(2, "strong", -30); ApScanAdd(&t, &a); ApScanMergeEnd(&t, kApScanAgeSweeps);
    ApScanSort(&t);
    Check("merge+sort: strongest first", t.e[0].signalDbm == -30 && t.e[1].signalDbm == -90);
    for (s = 0; s + 1 < kApScanAgeSweeps; s++) { ApScanMergeBegin(&t); ApScanMergeEnd(&t, kApScanAgeSweeps); }
    Check("merge+sort: age-out kept 'strong' (newer) and dropped 'weak' (older), via the row's own stamp",
          t.n == 1 && t.e[0].signalDbm == -30);
}

static void TestNullFunc(void)
{
    static const ApStU8 me[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    ApStU8 f[32];
    memset(f, 0xEE, sizeof(f));
    Check("null frame: 24 bytes", ApBuildNullFunc(f, me, kBssid, 1) == 24u);
    Check("PM=1: data/null function (0x48), ToDS + PwrMgt (0x11)", f[0] == 0x48 && f[1] == 0x11);
    Check("addr1 = addr3 = the AP, addr2 = us, duration and sequence zero",
          memcmp(f + 4, kBssid, 6) == 0 && memcmp(f + 10, me, 6) == 0 && memcmp(f + 16, kBssid, 6) == 0
          && f[2] == 0 && f[3] == 0 && f[22] == 0 && f[23] == 0 && f[24] == 0xEE);
    ApBuildNullFunc(f, me, kBssid, 0);
    Check("PM=0: ToDS only (0x01)", f[0] == 0x48 && f[1] == 0x01);
}

static void TestBlock(void)
{
    static ApScanBlock b, c;
    static ApScanItem items[kApScanMax + 4];
    int k;
    memset(&b, 0, sizeof(b));
    b.magic = kApScanMagic; b.version = kApScanVersion; b.size = sizeof(ApScanBlock);
    Check("a valid block is valid", ApScanValid(&b));
    b.version = 99; Check("a wrong version is refused", !ApScanValid(&b)); b.version = kApScanVersion;
    for (k = 0; k < (int)kApScanMax + 4; k++) items[k] = Item((ApStU8)k, "Net", -50 - k);
    memset(b.items, 0xCD, sizeof(b.items));
    b.state = kApScanRunning;
    ApScanPublish(&b, items, 3, 5, 1234);
    Check("published: count 3, dropped 5, tick, scanSeq 1, idle, seq even",
          b.count == 3 && b.dropped == 5 && b.scanTick == 1234 && b.scanSeq == 1 && b.state == kApScanIdle && !(b.seq & 1u));
    Check("the items beyond the count are cleared", b.items[3].ssidLen == 0 && b.items[kApScanMax - 1].bssid[0] == 0);
    Check("the items themselves", memcmp(&b.items[2], &items[2], sizeof(ApScanItem)) == 0);
    ApScanPublish(&b, items, kApScanMax + 4, 0, 5678);
    Check("more than 32 are cut to 32", b.count == kApScanMax && b.scanSeq == 2);
    Check("a consistent read", ApScanRead(&b, &c) && c.count == kApScanMax && memcmp(&c, &b, sizeof(b)) == 0);
    b.seq |= 1u;
    Check("a writer that never finishes: the reader gives up (bounded)", !ApScanRead(&b, &c));
    b.seq += 1u;
    gHookBumps = 1; gHookCalls = 0;
    Check("a rewrite completed mid-copy: the reader copies AGAIN and succeeds",
          ApScanRead(&b, &c) && gHookBumps == 0 && gHookCalls == 2);
}

int main(void)
{
    TestSecurity();
    TestFraming();
    TestTable();
    TestMerge();
    TestNullFunc();
    TestBlock();
    printf(gFails ? "  [!!] %d of %d checks FAILED\n" : "  [ok] %d checks, %d failed\n",
           gFails ? gFails : gChecks, gFails ? gChecks : 0);
    return gFails ? 1 : 0;
}
