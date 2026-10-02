/* link_test.c -- ap_link.h on the host: what the receive pump recognises, and when the link layer
 * declares a lost link and tries to join again. k204.
 *
 * What would make these fail (ask what an oracle would FAIL on):
 *   - a classifier that takes a neighbour's beacon for ours (a dead AP would look alive), a deauth sent
 *     to someone else for ours (a spurious rejoin), a protected or truncated frame's garbage for a
 *     reason code, or the reason bytes in the wrong order;
 *   - an EAPOL classifier that calls our own messages 2/4 "from the AP", or misses a group rekey;
 *   - a loss rule that fires one tick early or late, fires while the pump is not running or before any
 *     beacon was ever seen, or reads a beacon stamped a tick AFTER `now` as four billion ticks of silence;
 *   - a schedule that retries a silent AP at the fast rate (freezing the Finder every second while an AP
 *     reboots), never retries a silent AP at all (a replaced AP is never found), lets deauths received
 *     while down postpone the rejoin, or breaks when the tick counter wraps.
 * The world model at the end runs the driver's loop against a scripted AP -- restarts, a deauth, a busy
 * spell, a replaced BSSID -- and checks every attempt the link layer makes against when it was allowed.
 *
 * Build and run:  cc -Wall -o link_test link_test.c && ./link_test      (exit 0 = all passed) */
#include <stdio.h>
#include <string.h>
#include "ap_link.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{ gChecks++; if(!ok){ gFails++; printf("  [FAIL] %s\n", what); } }

static const unsigned char kBss[6]   = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const unsigned char kOther[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x56 };
static const unsigned char kMe[6]    = { 0x00, 0x0A, 0x95, 0x01, 0x02, 0x03 };
static const unsigned char kBcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* A management frame: frame control (type 0, subtype sub, flags), a1/a2/a3, sequence, then body. */
static unsigned MkMgmt(unsigned char *f, unsigned sub, unsigned flags, const unsigned char *a1,
                       const unsigned char *a2, const unsigned char *a3, unsigned reason)
{
    memset(f, 0, 64);
    f[0] = (unsigned char)(sub << 4);             /* version 0, type 0 (management) */
    f[1] = (unsigned char)flags;
    memcpy(f + 4, a1, 6); memcpy(f + 10, a2, 6); memcpy(f + 16, a3, 6);
    f[24] = (unsigned char)(reason & 0xFF); f[25] = (unsigned char)(reason >> 8);
    return 26u;
}

/* A converted Ethernet frame carrying an EAPOL-Key with the given Key Information. */
static unsigned MkEapol(unsigned char *e, unsigned type8021x, unsigned keyInfo)
{
    memset(e, 0, 128);
    memcpy(e, kMe, 6); memcpy(e + 6, kBss, 6);
    e[12] = 0x88; e[13] = 0x8E;
    e[14] = 2; e[15] = (unsigned char)type8021x; e[16] = 0; e[17] = 95;
    e[18] = 2;                                     /* RSN key descriptor */
    e[19] = (unsigned char)(keyInfo >> 8); e[20] = (unsigned char)keyInfo;
    return 14u + 99u;
}

static void TestClassifiers(void)
{
    unsigned char f[64], e[128];
    unsigned short r = 0xBEEF; unsigned ki = 0; unsigned n;

    n = MkMgmt(f, 8, 0, kBcast, kBss, kBss, 0);
    Check("our beacon -> BEACON", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_BEACON);
    Check("a beacon leaves the reason at 0", r == 0);
    n = MkMgmt(f, 8, 0, kBcast, kOther, kOther, 0);
    Check("a neighbour's beacon is not ours", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    n = MkMgmt(f, 8, 0, kBcast, kOther, kBss, 0);
    Check("a beacon whose a2 is someone else is not ours", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    n = MkMgmt(f, 5, 0, kMe, kBss, kBss, 0);
    Check("a probe response is not a beacon", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);

    n = MkMgmt(f, 12, 0, kMe, kBss, kBss, 7);
    Check("deauth to us -> DEAUTH", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_DEAUTH);
    Check("  with its reason (7)", r == 7);
    n = MkMgmt(f, 10, 0, kBcast, kBss, kBss, 3);
    Check("disassoc to everyone -> DISASSOC", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_DISASSOC);
    Check("  with its reason (3)", r == 3);
    n = MkMgmt(f, 12, 0, kMe, kBss, kBss, 0x0110);
    Check("the reason is little-endian (0x0110 = 272, not 0x1001)",
          ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_DEAUTH && r == 0x0110);
    n = MkMgmt(f, 12, 0, kOther, kBss, kBss, 7);
    Check("a deauth to another station is not ours", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    n = MkMgmt(f, 12, 0, kMe, kOther, kOther, 7);
    Check("a deauth from another BSS is not ours", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    n = MkMgmt(f, 12, 0x40, kMe, kBss, kBss, 7);
    Check("a PROTECTED deauth is not read", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER && r == 0);
    n = MkMgmt(f, 12, 0, kMe, kBss, kBss, 7);
    Check("a deauth with no room for a reason is refused", ApLinkClassifyMgmt(f, 25, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    f[0] = (unsigned char)((12u << 4) | (2u << 2));                 /* same bytes, type 2 */
    Check("a data frame is not management", ApLinkClassifyMgmt(f, n, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    Check("a runt (< 24 bytes) is refused", ApLinkClassifyMgmt(f, 23, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);
    Check("NULL frame is refused", ApLinkClassifyMgmt(NULL, 64, kBss, kMe, &r) == AP_LINK_MGMT_OTHER);

    n = MkEapol(e, 3, 0x0382);
    Check("group message 1 (ACK|MIC|Secure) -> GROUP_M1", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_GROUP_M1);
    Check("  and its Key Information", ki == 0x0382);
    n = MkEapol(e, 3, 0x008A);
    Check("pairwise message 1 (pairwise|ACK) -> PAIR_M1", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_PAIR_M1);
    n = MkEapol(e, 3, 0x13CA);
    Check("pairwise message 3 (pairwise|ACK|MIC|install) -> PAIR_M3", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_PAIR_M3);
    n = MkEapol(e, 3, 0x010A);
    Check("our message 2 (no ACK) is not from the AP", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_OTHER);
    n = MkEapol(e, 3, 0x0302);
    Check("our group message 2 (MIC|Secure, no ACK) is not from the AP", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_OTHER);
    n = MkEapol(e, 1, 0x0382);                     /* not a key frame, whatever bytes sit where key info would */
    Check("EAPOL-Start is not a key frame", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_OTHER);
    n = MkEapol(e, 3, 0x0382); e[13] = 0x00;
    Check("not 0x888E -> NONE", ApLinkClassifyEapol(e, n, &ki) == AP_LINK_EAPOL_NONE);
    n = MkEapol(e, 3, 0x0382);
    Check("a key frame too short for Key Information -> OTHER", ApLinkClassifyEapol(e, 20, &ki) == AP_LINK_EAPOL_OTHER);
    Check("shorter than an Ethernet header -> NONE", ApLinkClassifyEapol(e, 13, &ki) == AP_LINK_EAPOL_NONE);
}

static void TestClock(void)
{
    Check("age 10-5 = 5", ApLinkAge(10u, 5u) == 5u);
    Check("a beacon stamped AFTER now has age 0, not ~4e9", ApLinkAge(5u, 6u) == 0u);
    Check("age across the wrap (3 - 0xFFFFFFFE = 5)", ApLinkAge(3u, 0xFFFFFFFEu) == 5u);
    Check("silent needs a beacon ever seen", !ApLinkSilent(1000u, 0u, 0));
    Check("silent at exactly the loss threshold", ApLinkSilent(100u + AP_LINK_LOSS_TICKS, 100u, 1));
    Check("not silent one tick before it", !ApLinkSilent(99u + AP_LINK_LOSS_TICKS, 100u, 1));
    Check("retry after 1 failure = 1 s", ApLinkRetryTicks(1) == 4u);
    Check("retry schedule 1,2,5,10,30 s then stays", ApLinkRetryTicks(2) == 8u && ApLinkRetryTicks(3) == 20u
          && ApLinkRetryTicks(4) == 40u && ApLinkRetryTicks(5) == 120u && ApLinkRetryTicks(99) == 120u);
    Check("silent-AP schedule 20 s doubling to 5 min", ApLinkFallbackTicks(0) == 80u && ApLinkFallbackTicks(1) == 160u
          && ApLinkFallbackTicks(4) == 1200u && ApLinkFallbackTicks(50) == 1200u);

    /* the one place a signed comparison matters: `now` just before the wrap, the next attempt just after */
    {   ApLink L; ApLinkFacts f;
        ApLinkInit(&L, 0, 0xFFFFFFFEu, 0);                          /* boot wait -> nextTick = 6 */
        memset(&f, 0, sizeof f);
        f.now = 0xFFFFFFFFu; f.lastBeacon = 0xFFFFFFFFu; f.beaconSeen = 1; f.monitoring = 1;
        Check("an attempt due just after the wrap is not due just before it", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
        f.now = 6u; f.lastBeacon = 6u;
        Check("  and is due once the counter wraps to it", ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);
    }

    /* the words the log prints */
    Check("reason 16 is named as the unanswered group rekey", strstr(ApLinkReasonName(16), "group key") != NULL);
    Check("reason 7 is the class 3 frame", strstr(ApLinkReasonName(7), "class 3") != NULL);
    Check("an unknown reason says so", strcmp(ApLinkReasonName(99), "(not in this table)") == 0);
    Check("the beacon-loss outage is named", strstr(ApLinkWhyName(AP_LINK_WHY_BEACONS), "beacon") != NULL);
    Check("a group rekey is named", strstr(ApLinkEapolName(AP_LINK_EAPOL_GROUP_M1), "GROUP") != NULL);
}

static ApLinkFacts Facts(ApLinkU32 now, ApLinkU32 lastB, int seen, ApLinkU32 kick, int monitoring)
{
    ApLinkFacts f;
    memset(&f, 0, sizeof f);
    f.now = now; f.lastBeacon = lastB; f.beaconSeen = seen; f.kickSeq = kick;
    f.kickKind = AP_LINK_MGMT_DEAUTH; f.kickReason = 16; f.monitoring = monitoring;
    return f;
}

static void TestStateMachine(ApLinkU32 base)
{
    ApLink L; ApLinkFacts f; ApLinkU32 t; int a, bad;
    char what[160];

    /* up, beacons every tick: nothing happens */
    ApLinkInit(&L, 1, base, 0);
    bad = 0;
    for (t = base; t != base + 400u; t++) { f = Facts(t, t, 1, 0, 1); if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    snprintf(what, sizeof what, "[base %08x] up with beacons: no action for 100 s", base); Check(what, !bad && L.up);

    /* beacons stop at base+400: lost exactly LOSS ticks later */
    bad = 0;
    for (t = base + 400u; t != base + 399u + AP_LINK_LOSS_TICKS; t++) {
        f = Facts(t, base + 399u, 1, 0, 1); if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    f = Facts(base + 399u + AP_LINK_LOSS_TICKS, base + 399u, 1, 0, 1);
    snprintf(what, sizeof what, "[base %08x] silent: nothing before %u ticks", base, AP_LINK_LOSS_TICKS); Check(what, !bad);
    a = ApLinkStep(&L, &f);
    snprintf(what, sizeof what, "[base %08x] silent: WENT_DOWN at exactly %u ticks, why BEACONS", base, AP_LINK_LOSS_TICKS);
    Check(what, a == AP_LINK_ACT_WENT_DOWN && !L.up && L.why == AP_LINK_WHY_BEACONS && L.downs == 1u);

    /* silence while the pump is not running, or before any beacon, proves nothing */
    ApLinkInit(&L, 1, base, 0);
    f = Facts(base + 10000u, base, 1, 0, 0);
    Check("no loss while the pump is not running", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);
    f = Facts(base + 10000u, 0u, 0, 0, 1);
    Check("no loss before any beacon was ever seen", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);
    f = Facts(base + 50u, base + 51u, 1, 0, 1);
    Check("a beacon stamped after now is not silence", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);

    /* a kick: down at once, reason kept, first attempt after the kick wait */
    ApLinkInit(&L, 1, base, 5);
    f = Facts(base + 100u, base + 100u, 1, 5, 1);
    Check("a kick already seen at init does nothing", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);
    f = Facts(base + 101u, base + 101u, 1, 6, 1);
    a = ApLinkStep(&L, &f);
    Check("a new kick -> WENT_DOWN, why DEAUTH, reason 16",
          a == AP_LINK_ACT_WENT_DOWN && L.why == AP_LINK_WHY_DEAUTH && L.reason == 16);
    f = Facts(base + 101u, base + 101u, 1, 6, 1); f.kickKind = AP_LINK_MGMT_DISASSOC;
    Check("the kick is consumed (no second WENT_DOWN)", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
    {   ApLink D; ApLinkFacts g;
        ApLinkInit(&D, 1, base, 0);
        g = Facts(base + 5u, base + 5u, 1, 1, 1); g.kickKind = AP_LINK_MGMT_DISASSOC; g.kickReason = 8;
        Check("a disassociation is recorded as one, with its reason",
              ApLinkStep(&D, &g) == AP_LINK_ACT_WENT_DOWN && D.why == AP_LINK_WHY_DISASSOC && D.reason == 8); }
    f = Facts(base + 101u + AP_LINK_KICK_WAIT_TICKS - 1u, base + 102u, 1, 6, 1);
    Check("no attempt inside the kick wait", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
    f = Facts(base + 101u + AP_LINK_KICK_WAIT_TICKS, base + 102u, 1, 6, 1);
    Check("attempt when the kick wait ends (AP audible)", ApLinkStep(&L, &f) == AP_LINK_ACT_TRY && L.tries == 1u);

    /* failures while audible back off 1, 2, 5, 10, 30 s; kicks while down change nothing */
    {   static const ApLinkU32 gap[6] = { 4u, 8u, 20u, 40u, 120u, 120u };
        ApLinkU32 now = base + 101u + AP_LINK_KICK_WAIT_TICKS, k = 6; int i;
        bad = 0;
        for (i = 0; i < 6; i++) {
            ApLinkAttempted(&L, now, 0);
            f = Facts(now + gap[i] - 1u, now + gap[i] - 1u, 1, ++k, 1);      /* audible, AND a fresh kick */
            if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad |= 1;
            f = Facts(now + gap[i], now + gap[i], 1, ++k, 1);
            if (ApLinkStep(&L, &f) != AP_LINK_ACT_TRY) bad |= 2;
            now += gap[i];
        }
        Check("audible retries back off 1,2,5,10,30,30 s", !(bad & 2));
        Check("and never early, even with deauths arriving while down", !(bad & 1));
        ApLinkAttempted(&L, now, 1);
        Check("success -> up, fails cleared, one rejoin", L.up && L.fails == 0u && L.rejoins == 1u);
        f = Facts(now + 1u, now + 1u, 1, k, 1);
        Check("kicks received while down are not replayed after the rejoin", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);
    }

    /* a SILENT AP: slow schedule only */
    ApLinkInit(&L, 1, base, 0);
    f = Facts(base + 1000u, base + 1000u - AP_LINK_LOSS_TICKS, 1, 0, 1);
    (void)ApLinkStep(&L, &f);                                       /* lost by silence at base+1000 */
    bad = 0;
    for (t = base + 1000u; t != base + 1000u + 80u; t++) {
        f = Facts(t, base + 1000u - AP_LINK_LOSS_TICKS, 1, 0, 1); if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    Check("silent AP: no attempt for 20 s", !bad);
    f = Facts(base + 1080u, base + 1000u - AP_LINK_LOSS_TICKS, 1, 0, 1);
    Check("silent AP: one attempt at 20 s", ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);
    ApLinkAttempted(&L, base + 1080u, 0);
    f = Facts(base + 1080u + 159u, base + 1000u - AP_LINK_LOSS_TICKS, 1, 0, 1);
    Check("silent AP: not again before 40 s", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
    f = Facts(base + 1080u + 160u, base + 1000u - AP_LINK_LOSS_TICKS, 1, 0, 1);
    Check("silent AP: again at 40 s", ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);
    ApLinkAttempted(&L, base + 1240u, 0);
    f = Facts(base + 1250u, base + 1249u, 1, 0, 1);                 /* the AP is back: audible */
    Check("an AP that returns is tried on the audible schedule, not the slow one",
          ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);

    /* boot */
    ApLinkInit(&L, 0, base, 3);
    Check("boot failure: down, why BOOT, one outage counted", !L.up && L.why == AP_LINK_WHY_BOOT && L.downs == 1u);
    f = Facts(base + AP_LINK_BOOT_WAIT_TICKS - 1u, base + AP_LINK_BOOT_WAIT_TICKS - 1u, 1, 3, 1);
    Check("boot: no attempt inside the 2 s wait, even with the AP audible", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
    f = Facts(base + AP_LINK_BOOT_WAIT_TICKS, base + AP_LINK_BOOT_WAIT_TICKS, 1, 3, 1);
    Check("boot: attempt at 2 s when the AP is audible", ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);
    ApLinkInit(&L, 0, base, 0);
    f = Facts(base + 79u, 0u, 0, 0, 1);
    Check("boot, no BSSID ever heard: not before 20 s", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE);
    f = Facts(base + 80u, 0u, 0, 0, 1);
    Check("boot, no BSSID ever heard: tried at 20 s (a deaf boot still gets its chances)",
          ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);
}

/* ---- the world model ------------------------------------------------------------------------- */
/* A scripted AP over 30 minutes of ticks, and the driver's loop around ap_link.h:
 *   every tick   the AP beacons if it is on (the pump stamps it only while monitoring);
 *   every tick   the task runs ApLinkStep; on TRY the join succeeds iff the AP is on, is not busy,
 *                and our BSSID is current (a replaced AP must be re-learned by a fallback attempt).
 * Script: up at 0; AP restart 60 s at t=120 s; deauth at t=400 s; busy for 20 s at t=700 s (it answers
 * but refuses) then a deauth; BSSID change at t=1100 s (new BSSID; our beacon tracking goes deaf to it). */
static void TestWorld(void)
{
    ApLink L; ApLinkU32 t, lastB = 0, kick = 0; int seen = 1, monitoring = 1, bssOk = 1;
    unsigned tries = 0, triesWhileOff = 0, fastWhileOff = 0; ApLinkU32 lastTry = 0, lastDown = 0;
    ApLinkU32 maxOutage = 0; int up = 1;
    ApLinkInit(&L, 1, 0, 0);
    for (t = 0; t < 30u * 60u * 4u; t++) {
        int apOn   = !(t >= 480u && t < 720u);                      /* restart: 60 s off */
        int busy   = (t >= 2800u && t < 2880u);                     /* refuses for 20 s */
        ApLinkFacts f; int a;
        if (t == 1600u || t == 2800u) kick++;                       /* deauths */
        if (t == 4400u) bssOk = 0;                                  /* AP replaced: new BSSID */
        if (apOn && monitoring && bssOk) { lastB = t; seen = 1; }
        memset(&f, 0, sizeof f);
        f.now = t; f.lastBeacon = lastB; f.beaconSeen = seen; f.kickSeq = kick;
        f.kickKind = AP_LINK_MGMT_DEAUTH; f.kickReason = 7; f.monitoring = monitoring;
        a = ApLinkStep(&L, &f);
        if (a == AP_LINK_ACT_WENT_DOWN) { up = 0; lastDown = t; }
        if (a == AP_LINK_ACT_TRY) {
            int ok = apOn && !busy;
            tries++;
            if (!apOn) { triesWhileOff++; if (lastTry && t - lastTry < 80u) fastWhileOff++; }
            if (ok && !bssOk) { bssOk = 1; lastB = t; }             /* the fallback attempt re-learned it */
            ApLinkAttempted(&L, t, ok);
            if (ok) { up = 1; if (t - lastDown > maxOutage) maxOutage = t - lastDown; lastB = t; }
            lastTry = t;
        }
    }
    Check("world: ends up", up && L.up);
    Check("world: never retried a SILENT AP faster than every 20 s", fastWhileOff == 0u);
    Check("world: at most 3 attempts during the 60 s AP restart", triesWhileOff <= 3u);
    Check("world: every outage ended within 90 s (restart 60 s + one audible retry)", maxOutage <= 360u);
    Check("world: the replaced BSSID was found by a fallback attempt", L.rejoins >= 4u);
    Check("world: bounded effort (under 40 attempts in 30 min)", tries < 40u);
    printf("  world: %u attempts, %u rejoins, longest outage %u s, %u attempts while the AP was off\n",
           tries, (unsigned)L.rejoins, (unsigned)(maxOutage / 4u), triesWhileOff);
}

/* ★ k207: AirPort off and on -- the user's switch, not an outage. */
static void TestOffOn(ApLinkU32 base)
{
    ApLink L; ApLinkFacts f; ApLinkU32 t; int bad;
    char what[160];

    /* up, then off: nothing is ever attempted, whatever the air does, and it is not an outage */
    ApLinkInit(&L, 1, base, 0);
    ApLinkTurnOff(&L, base + 10u);
    snprintf(what, sizeof what, "[base %08x] off: down, why OFF, not counted as an outage", base);
    Check(what, L.off && !L.up && L.why == AP_LINK_WHY_OFF && L.downs == 0u);
    bad = 0;
    for (t = base + 10u; t != base + 10u + 4800u; t++) {          /* 20 minutes */
        f = Facts(t, t, 1, 0, 1);                                  /* the AP audible every tick */
        if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    snprintf(what, sizeof what, "[base %08x] off: 20 min with the AP audible -> no attempt", base);
    Check(what, !bad && L.tries == 0u);
    bad = 0;
    for (t = base + 5000u; t != base + 5000u + 4800u; t++) {       /* the AP silent too */
        f = Facts(t, base, 1, 0, 1);
        if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    Check("off: 20 min silent -> no attempt, not even the slow fallback", !bad && L.tries == 0u);
    f = Facts(base + 9900u, base + 9900u, 1, 3, 1);                /* the AP answering our goodbye */
    Check("off: a deauth is consumed, not a WENT_DOWN", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.kickSeen == 3u);

    /* on again, the AP audible: the attempt comes at once, and success is a rejoin */
    ApLinkTurnOn(&L, base + 10000u);
    Check("on: no longer off, still down, why OFF", !L.off && !L.up && L.why == AP_LINK_WHY_OFF);
    f = Facts(base + 10000u, base + 10000u, 1, 3, 1);
    snprintf(what, sizeof what, "[base %08x] on + AP audible -> TRY on the same tick", base);
    Check(what, ApLinkStep(&L, &f) == AP_LINK_ACT_TRY && L.tries == 1u);
    ApLinkAttempted(&L, base + 10000u, 1);
    Check("on: the attempt succeeds -> up, one rejoin, still no outage", L.up && L.rejoins == 1u && L.downs == 0u);
    f = Facts(base + 10001u, base + 10000u, 1, 3, 1);
    Check("on: the kick seen while off does not bring the new link down", ApLinkStep(&L, &f) == AP_LINK_ACT_NONE && L.up);

    /* on again with the AP silent (the last beacon from before the off): the ordinary 20 s fallback */
    ApLinkInit(&L, 1, base, 0);
    ApLinkTurnOff(&L, base + 1u);
    ApLinkTurnOn(&L, base + 100u);
    bad = 0;
    for (t = base + 100u; t != base + 100u + 80u; t++) {
        f = Facts(t, base + 1u, 1, 0, 1); if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    f = Facts(base + 180u, base + 1u, 1, 0, 1);
    snprintf(what, sizeof what, "[base %08x] on + AP silent: nothing for 20 s, then the fallback TRY", base);
    Check(what, !bad && ApLinkStep(&L, &f) == AP_LINK_ACT_TRY);

    /* off while already down and retrying stops the retries */
    ApLinkInit(&L, 0, base, 0);                                    /* the boot join failed */
    ApLinkTurnOff(&L, base + 2u);
    bad = 0;
    for (t = base + 2u; t != base + 2u + 4800u; t++) {
        f = Facts(t, t, 1, 0, 1); if (ApLinkStep(&L, &f) != AP_LINK_ACT_NONE) bad = 1; }
    Check("off while down: the retry schedule stops", !bad && L.tries == 0u);
    Check("the why has words", strcmp(ApLinkWhyName(AP_LINK_WHY_OFF), "AirPort was turned off") == 0);
}

int main(void)
{
    TestClassifiers();
    TestClock();
    TestStateMachine(0u);
    TestStateMachine(0xFFFFFF00u);          /* the same scenarios straddling the 32-bit tick wrap: the
                                             * beacon loop, the loss and the retry schedule all cross it */
    TestOffOn(0u);
    TestOffOn(0xFFFFE000u);                 /* and the off/on scenarios across the wrap */
    TestWorld();
    printf(gFails ? "  [!!] %d of %d checks FAILED\n" : "  [ok] %d checks, %d failed\n",
           gFails ? gFails : gChecks, gFails ? gChecks : 0);
    return gFails ? 1 : 0;
}
