/* ap_ring.h -- the DRIVER'S logging sink: a byte ring of length-prefixed lines, Stage 8-2d-1.
 *
 * ★ WHAT THIS IS FOR. ap_log.h's Out() does FSWrite + FlushVol. That is correct for the probe
 * application and fatal for the driver: the File Manager below task level hangs OS 9 with no NMI
 * and no debugger, and this project has been bitten by that three times (r18, n4, and the r95
 * bulk_xfer trace on the EHCI work). The driver therefore defines its own Out() over this ring,
 * includes the same ap_fmt.h, and all 736 Say/SayH/Say1/Out call sites in ap_bringup.h and
 * ap_phy_initg.h come along unmodified. The app drains the ring afterwards and writes it into the
 * ordinary log, so the narration reads exactly as it always has.
 *
 * ⚠ IT FILLS AND STOPS. IT DOES NOT WRAP, and that is the opposite of ap_ilog.h on purpose.
 *   ap_ilog.h serves the receive ISR, where the interesting lines are the MOST RECENT ones and
 *   overwriting old ones is correct. This serves BRING-UP, where the interesting lines are the
 *   FIRST ones -- a PHY init that fails does so early, and the hundreds of lines that follow are
 *   consequences. A wrapping ring would discard precisely the lines that explain the failure and
 *   would hand back a log that looks complete. So: fill, stop, and COUNT what was refused.
 *
 * ⚠ A NON-ZERO ApRingDropped() INVALIDATES THE TAIL OF THE NARRATION, NOT THE HEAD. Whoever
 *   reads it must say so. Silence about a truncation is how a reader mistakes a short log for a
 *   PHY that stopped early -- exactly the misreading 8-2d-1 exists to prevent.
 *
 * ⚠ EXECUTION LEVEL, HONESTLY. There is no allocation, no trap and no File Manager here, so the
 *   *operations* are safe anywhere. But the cursor update is three non-atomic stores, so two
 *   writers at different levels can interleave and corrupt a line. Today there is exactly one
 *   writer, at task level, inside EnetHAL_Open. When the RX ISR arrives it must NOT simply start
 *   calling this -- it wants ap_ilog.h's discipline (single-word records, modular index) instead.
 *   That is a real fork in the road and it is written down here rather than discovered later.
 *
 * ⚠ SIZING IS MEASURED, NOT GUESSED -- and the guess was wrong. The first version of this header
 *   reasoned "400-700 lines at ~45 bytes each" and set 48000 bytes. Counting the ApPhyInitG
 *   narration in a real k69 log gives **848 lines and 49010 bytes**, mean 57.8 bytes per line.
 *   With the 888 bytes the driver already emits, 8-2d-3 needs 49898 -- so the original size would
 *   have truncated by about 1900 bytes and produced precisely the failure this header warns
 *   about two paragraphs up: a log that stops mid-PHY and reads like a PHY that stopped.
 *
 *   Measure the span with the section markers in a banked log ("=== b43_" to "b43_phy_initg IS
 *   COMPLETE. STAGE 4 IS DONE"), not from the call-site count -- 711 call sites in
 *   ap_phy_initg.h produce 848 lines, because loops emit repeatedly and branches do not fire.
 *
 * ⚠ THE OFFSETS ARE 32-BIT AS OF 8-2d-3. They were 16-bit, which capped the buffer at 65535 and
 *   left only 1.3x headroom over a measured 49898 -- close enough that one more instrumented
 *   branch in the PHY code would have silently re-armed the truncation. Widening them costs
 *   AP_RING_LINES * 2 extra bytes of static data and removes the ceiling entirely. A limit that
 *   a measurement is already within 30% of is not a limit worth defending.
 */
#ifndef AP_RING_H
#define AP_RING_H

/* ⚠⚠ k196: RE-MEASURED, AND THE OLD LIMIT WAS ONE SNAPSHOT FROM TRUNCATING. The k195 log (bring-up +
 *   join + two qclose snapshots) came to 72,360 bytes and 1,568 LINES against 80,000 / 1,600 -- 98% of
 *   the line limit. k196 adds a ~500-line receive timeline and ~40 counter lines per snapshot, about
 *   2,150 lines / ~120 KB in all. Doubled, for the same 1.5x-plus headroom the note above asks for. */
#define AP_RING_BYTES 160000u   /* k196: 1.3x a measured-and-projected ~120 KB (was 80000) */
#define AP_RING_LINES  3200u    /* k196: 1.5x a projected ~2150 (was 1600; k195 used 1568) */

static unsigned char gApRingBuf[AP_RING_BYTES];
static unsigned long gApRingOff[AP_RING_LINES];
static unsigned long  gApRingN = 0, gApRingUsed = 0, gApRingDropped = 0;

static void ApRingReset(void)
{
    gApRingN = 0; gApRingUsed = 0; gApRingDropped = 0;
}

/* Copy a Pascal string in. p[0] is the length, so a line is p[0]+1 bytes and a zero-length line
 * still costs one. Refuses rather than truncates: a half-written line is worse than a missing
 * one, because it reads as real. */
static void ApRingPut(const unsigned char *p)
{
    unsigned long need, i;
    if(!p) { gApRingDropped++; return; }
    need = (unsigned long)p[0] + 1u;
    if(gApRingN >= AP_RING_LINES || gApRingUsed + need > AP_RING_BYTES) { gApRingDropped++; return; }
    gApRingOff[gApRingN] = gApRingUsed;   /* 32-bit since 8-2d-3; see the sizing note above */
    for(i = 0; i < need; i++) gApRingBuf[gApRingUsed + i] = p[i];
    gApRingUsed += need;
    gApRingN++;
}

static unsigned long ApRingCount(void)   { return gApRingN; }
static unsigned long ApRingDropped(void) { return gApRingDropped; }
static unsigned long ApRingBytes(void)   { return gApRingUsed; }

/* Copy line idx out as a Pascal string. Returns 1 if it copied, 0 if idx is out of range or out
 * is NULL. The caller supplies a Str255; nothing here can exceed 256 bytes because nothing
 * longer can be put in. */
static int ApRingGet(unsigned long idx, unsigned char *out)
{
    const unsigned char *p;
    unsigned long i, n;
    if(!out || idx >= gApRingN) return 0;
    p = &gApRingBuf[gApRingOff[idx]];
    n = (unsigned long)p[0];
    for(i = 0; i <= n; i++) out[i] = p[i];
    return 1;
}

#endif /* AP_RING_H */
