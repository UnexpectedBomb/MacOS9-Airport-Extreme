/* ap_tbclock.h -- the G4 timebase as a per-frame stopwatch, and a histogram to keep what it sees. k199.
 *
 * ★ WHY. k198's capture said the Mac spends ~3-4 ms per frame somewhere; a capture on the Pi cannot say
 * WHERE. This times each stage of a frame's trip through the driver, on the machine, so the next build
 * is aimed at a measured cost instead of a theory. Stages (the snapshot's PER-FRAME COST section):
 *   transmit  ApShimWriteFrame entry -> doorbell, and the CCMP encrypt inside it
 *   receive   the ring walk + copy (RxWaitFor), ApRxToEnet, the CCMP decrypt inside it, the wait in the
 *             ISR->deferred-task queue, the deferred task's run, and the wait from putq to the read
 *             service routine
 *   secondary the whole secondary-interrupt run
 *
 * ★ mftb, NOT Microseconds(). One instruction, legal in user mode (TBL is not a privileged SPR --
 *   contrast [[reference_os9_usermode_privileged_sprs]]), safe at any execution level, no trap. The
 *   conversion to microseconds needs the timebase rate, which ApTbCalibrate measures ONCE at task level
 *   against Microseconds(); until then gApTbPerMs is 0 and every sample reads 0 us -- visibly
 *   "uncalibrated", never silently wrong. 32-bit TBL wraps in ~100 s at ~42 MHz; unsigned subtraction
 *   gives the right delta for anything shorter, and nothing timed here is longer.
 *
 * ⚠ ApTbHistAdd runs at interrupt level: integer arithmetic on the caller's struct, no calls, no
 *   File Manager. Printing is task level only (the snapshot). */
#ifndef AP_TBCLOCK_H
#define AP_TBCLOCK_H

#define AP_TBH_NB 9    /* buckets: <25 <50 <100 <200 <500 <1000 <2000 <5000 >=5000 us */

typedef struct {
    unsigned long n;
    unsigned long sumUs;          /* saturates rather than wraps */
    unsigned long maxUs;
    unsigned long b[AP_TBH_NB];
} ApTbHist;

extern unsigned long gApTbPerMs;  /* timebase ticks per millisecond; defined in ap_shim.c, 0 = uncalibrated */

static __inline__ unsigned long ApTbNow(void)
{
#if defined(__ppc__) || defined(__POWERPC__)
    unsigned long t;
    __asm__ __volatile__("mftb %0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}

/* ticks -> microseconds, exactly, in 32 bits: (ticks % perMs) < perMs (~42 000), so * 1000 fits. */
static __inline__ unsigned long ApTbToUs(unsigned long ticks)
{
    unsigned long pm = gApTbPerMs;
    if(pm == 0) return 0;
    return (ticks / pm) * 1000UL + ((ticks % pm) * 1000UL) / pm;
}

static __inline__ void ApTbHistAdd(ApTbHist *h, unsigned long ticks)
{
    unsigned long us = ApTbToUs(ticks);
    int k = (us < 25UL) ? 0 : (us < 50UL) ? 1 : (us < 100UL) ? 2 : (us < 200UL) ? 3 : (us < 500UL) ? 4 :
            (us < 1000UL) ? 5 : (us < 2000UL) ? 6 : (us < 5000UL) ? 7 : 8;
    h->n++;
    h->b[k]++;
    if(h->sumUs + us >= h->sumUs) h->sumUs += us;
    if(us > h->maxUs) h->maxUs = us;
}

#endif /* AP_TBCLOCK_H */
