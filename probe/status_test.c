/* status_test.c -- ap_status.h on the host: the status block's layout and reader, b43's RSSI conversion,
 * and Apple's five signal levels. k205.
 *
 * What would make these fail (ask what an oracle would FAIL on):
 *   - a conversion that drifts from b43_rssi_postprocess: the wrong phy-status bit for OFDM or for the
 *     2050 adjustment, no clamp at 63 on the table index, rounding instead of C's truncation, or a
 *     missing (s8) wrap -- every expected value below is worked by hand from b43's arithmetic;
 *   - levels without hysteresis (the strip would flicker at a boundary), hysteresis that sticks (a
 *     real drop never shows), or boundaries in the wrong order;
 *   - a reader that returns a torn copy while the writer is inside (seq odd), trusts a copy during
 *     which seq moved, or loops forever on a writer that died mid-rewrite;
 *   - a validity check that accepts another version's or another size's block.
 *
 * Build and run:  cc -Wall -o status_test status_test.c && ./status_test      (exit 0 = all passed) */
#include <stdio.h>
#include <string.h>
/* The writer the reader must survive: each time the hook runs (between the reader's copy and its second
 * look at seq) it completes one more rewrite -- new linkState, seq += 2 -- while gRewrites lasts. */
static int gRewrites = 0;
#define AP_STAT_READ_HOOK(b) do { if (gRewrites > 0) { volatile ApStatBlock *w_ = (volatile ApStatBlock *)(b); \
    gRewrites--; w_->linkState++; w_->seq += 2; } } while (0)
#include "ap_status.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{ gChecks++; if (!ok) { gFails++; printf("  [FAIL] %s\n", what); } }

static short gLinear[64];                        /* nrssi_lt[i] = i: makes the arithmetic checkable */

static void TestRssi(void)
{
    int i;
    for (i = 0; i < 64; i++) gLinear[i] = (short)i;
    /* OFDM: in>127 -> -256; *73; /64 (truncating); -3, or +25 with the TR-state adjustment. */
    Check("OFDM jssi 200 -> -66 dBm (200-256=-56; -56*73/64=-63; -3)", ApRssiDbm(200, 0x0001, 0x0000, gLinear) == -66);
    Check("OFDM jssi 200 + TRSTATE -> -38 (-63 + 25)",                ApRssiDbm(200, 0x0001, 0x0400, gLinear) == -38);
    Check("OFDM jssi 10 -> 8 (730/64=11; -3)",                         ApRssiDbm(10, 0x0001, 0x0000, gLinear) == 8);
    Check("OFDM ignores the gain-control bit (adjust_2053, unused for a 2050)",
          ApRssiDbm(200, 0x4001, 0x0000, gLinear) == -66);
    /* CCK via the table: 31-lt; *-131; /128 (truncating); -57; +25 with TRSTATE. */
    Check("CCK jssi 20 -> -68 (31-20=11; -1441/128=-11; -57)",       ApRssiDbm(20, 0x0000, 0x0000, gLinear) == -68);
    Check("CCK jssi 20 + TRSTATE -> -43",                             ApRssiDbm(20, 0x0000, 0x0400, gLinear) == -43);
    Check("CCK jssi 0 -> -88 (31*-131/128=-31; -57)",                ApRssiDbm(0, 0x0000, 0x0000, gLinear) == -88);
    Check("CCK index clamps at 63 (jssi 100 == jssi 63 == -25)",
          ApRssiDbm(100, 0x0000, 0x0000, gLinear) == -25 && ApRssiDbm(63, 0x0000, 0x0000, gLinear) == -25);
    Check("CCK reads the TABLE, not the raw value",
          (gLinear[20] = 5, ApRssiDbm(20, 0x0000, 0x0000, gLinear)) == (31 - 5) * -131 / 128 - 57);
    gLinear[20] = 20;
    Check("the result wraps like b43's (s8): OFDM jssi 127 + TRSTATE = 169 -> -87",
          ApRssiDbm(127, 0x0001, 0x0400, gLinear) == -87);
}

static void TestQuality(void)
{
    Check("no history: -45 is Strong",        ApStatQuality(-45, 99) == kApQualStrong);
    Check("no history: -64 is Good",          ApStatQuality(-64, 99) == kApQualGood);
    Check("no history: -70 is Average",       ApStatQuality(-70, 99) == kApQualAverage);
    Check("no history: -80 is Weak",          ApStatQuality(-80, 99) == kApQualWeak);
    Check("no history: -90 is Out of Range",  ApStatQuality(-90, 99) == kApQualOutOfRange);
    Check("boundaries are inclusive (-60 Strong, -85 Weak)",
          ApStatQuality(-60, 99) == kApQualStrong && ApStatQuality(-85, 99) == kApQualWeak);
    /* hysteresis, up */
    Check("Average -> -66: stays Average (Good needs -65)",   ApStatQuality(-66, kApQualAverage) == kApQualAverage);
    Check("Average -> -65: Good",                             ApStatQuality(-65, kApQualAverage) == kApQualGood);
    Check("Good -> -59: stays Good (Strong needs -58)",       ApStatQuality(-59, kApQualGood) == kApQualGood);
    Check("Weak -> -59: jumps to Good, stops short of Strong", ApStatQuality(-59, kApQualWeak) == kApQualGood);
    Check("Out of Range -> -40: straight to Strong",          ApStatQuality(-40, kApQualOutOfRange) == kApQualStrong);
    /* hysteresis, down */
    Check("Strong -> -61: stays Strong (drop needs < -62)",   ApStatQuality(-61, kApQualStrong) == kApQualStrong);
    Check("Strong -> -62: still Strong",                      ApStatQuality(-62, kApQualStrong) == kApQualStrong);
    Check("Strong -> -63: Good",                              ApStatQuality(-63, kApQualStrong) == kApQualGood);
    Check("Strong -> -80: straight to Weak",                  ApStatQuality(-80, kApQualStrong) == kApQualWeak);
    Check("Weak -> -87: stays Weak",                          ApStatQuality(-87, kApQualWeak) == kApQualWeak);
    Check("Weak -> -88: Out of Range",                        ApStatQuality(-88, kApQualWeak) == kApQualOutOfRange);
    /* no flicker: a +/-1 dB wobble around -60 from Strong never leaves Strong */
    {   ApStU32 q = kApQualStrong; int i, moved = 0;
        for (i = 0; i < 20; i++) { ApStU32 n = ApStatQuality((i & 1) ? -59 : -61, q); if (n != q) moved++; q = n; }
        Check("a 1 dB wobble at a boundary never flickers", moved == 0); }
    /* and a real fall is followed all the way */
    {   ApStU32 q = kApQualStrong; int d;
        for (d = -40; d >= -95; d--) q = ApStatQuality(d, q);
        Check("a steady fall from -40 to -95 ends Out of Range", q == kApQualOutOfRange); }
}

static void TestPercent(void)
{
    Check("-95 dBm -> 0%",           ApStatPercent(-95) == 0);
    Check("-35 dBm -> 100%",         ApStatPercent(-35) == 100);
    Check("-65 dBm -> 50%",          ApStatPercent(-65) == 50);
    Check("no reading (0) -> 0%",    ApStatPercent(0) == 0);
    Check("clamped above and below", ApStatPercent(-20) == 100 && ApStatPercent(-120) == 0);
}

static void TestBlock(void)
{
    static ApStatBlock b, c;
    memset(&b, 0, sizeof b);
    b.magic = kApStatMagic; b.version = kApStatVersion; b.size = sizeof b;
    Check("a well-formed block is valid", ApStatValid(&b));
    b.version = kApStatVersion + 1;
    Check("another version is refused", !ApStatValid(&b));
    b.version = kApStatVersion; b.size = sizeof b - 4;
    Check("another size is refused", !ApStatValid(&b));
    b.size = sizeof b; b.magic ^= 1;
    Check("another magic is refused", !ApStatValid(&b));
    Check("NULL is refused", !ApStatValid(NULL));
    b.magic = kApStatMagic;

    b.seq = 4; b.linkState = kApLinkUp; b.ssidLen = 3; memcpy(b.ssid, "abc", 3);
    memset(&c, 0xEE, sizeof c);
    Check("seq even: the copy is consistent", ApStatRead(&b, &c) == 1 && c.linkState == kApLinkUp && c.ssidLen == 3
          && memcmp(c.ssid, "abc", 3) == 0);
    b.seq = 5;
    Check("seq odd (writer inside): gives up after its bounded retries, reports not-consistent",
          ApStatRead(&b, &c) == 0);
    /* a writer that finishes ONE rewrite during the copy: the first copy is stale and must be discarded;
     * the retry sees the new values */
    b.seq = 6; b.linkState = kApLinkSearching; gRewrites = 1;
    Check("seq moved during the copy: the stale copy is thrown away and the retry is consistent",
          ApStatRead(&b, &c) == 1 && c.linkState == kApLinkJoining && c.seq == 8);
    /* a writer that never stops: bounded, reports not-consistent instead of spinning */
    b.seq = 6; gRewrites = 1000;
    Check("a writer rewriting on every attempt: the reader gives up (bounded)", ApStatRead(&b, &c) == 0);
    Check("  after exactly its 8 attempts", gRewrites == 1000 - 8);
    gRewrites = 0;
}

/* ★ k207: the command mailbox. */
static void TestMailbox(void)
{
    static ApStatBlock b;
    ApStU32 s1, s2, seq = 0, cmd = 0, arg = 0;
    memset(&b, 0, sizeof b);
    Check("a fresh block has no command", !ApStatBusy(&b) && !ApStatTake(&b, &seq, &cmd, &arg));

    s1 = ApStatPost(&b, kApCmdPower, 0);
    Check("post: sequence 1, busy, the words written", s1 == 1u && ApStatBusy(&b)
          && b.cmd == kApCmdPower && b.cmdArg == 0u && b.cmdSeq == 1u);
    Check("post: not yet acknowledged", !ApStatAcked(&b, s1));
    Check("take: the command, its argument and its number",
          ApStatTake(&b, &seq, &cmd, &arg) == 1 && seq == 1u && cmd == kApCmdPower && arg == 0u);
    Check("take does not acknowledge (still busy until the driver acks)", ApStatBusy(&b) && !ApStatAcked(&b, s1));
    ApStatAck(&b, seq, kApAckDone);
    Check("ack: done, not busy, the result published", ApStatAcked(&b, s1) && !ApStatBusy(&b)
          && b.ackResult == kApAckDone && b.ackSeq == 1u);
    Check("nothing left to take", !ApStatTake(&b, &seq, &cmd, &arg));

    /* two posts before the driver looks: the newest wins, and the older one counts as settled with it */
    s1 = ApStatPost(&b, kApCmdPower, 0);
    s2 = ApStatPost(&b, kApCmdPower, 1);
    Check("two posts: consecutive numbers", s1 == 2u && s2 == 3u);
    Check("take gets the NEWEST command", ApStatTake(&b, &seq, &cmd, &arg) == 1 && seq == 3u && arg == 1u);
    Check("  the older one is not settled before the ack", !ApStatAcked(&b, s1));
    ApStatAck(&b, seq, kApAckDone);
    Check("  after the ack both are settled (the older superseded)", ApStatAcked(&b, s1) && ApStatAcked(&b, s2));

    /* a newer command posted by someone else, not yet taken: ours (acked) stays settled; theirs is pending */
    s1 = ApStatPost(&b, kApCmdPower, 0);
    ApStatAck(&b, s1, kApAckDone);
    s2 = ApStatPost(&b, kApCmdPower, 1);
    Check("ours acked, theirs pending: ours settled, theirs not", ApStatAcked(&b, s1) && !ApStatAcked(&b, s2));

    /* the number never becomes 0, even across the 32-bit wrap */
    b.cmdSeq = 0xFFFFFFFFu; b.ackSeq = 0xFFFFFFFFu;
    s1 = ApStatPost(&b, kApCmdPower, 1);
    Check("the wrap skips 0 (0 is a fresh block's 'nothing yet')", s1 == 1u && ApStatBusy(&b));
    Check("  and is taken normally", ApStatTake(&b, &seq, &cmd, &arg) == 1 && seq == 1u);

    /* an unknown command is still acknowledged, with its result */
    s1 = ApStatPost(&b, 99u, 0);
    (void)ApStatTake(&b, &seq, &cmd, &arg);
    ApStatAck(&b, seq, kApAckUnknown);
    Check("an unknown command: acknowledged, result Unknown", ApStatAcked(&b, s1) && b.ackResult == kApAckUnknown);
}

int main(void)
{
    TestRssi();
    TestQuality();
    TestPercent();
    TestBlock();
    TestMailbox();
    printf(gFails ? "  [!!] %d of %d checks FAILED\n" : "  [ok] %d checks, %d failed\n",
           gFails ? gFails : gChecks, gFails ? gChecks : 0);
    return gFails ? 1 : 0;
}
