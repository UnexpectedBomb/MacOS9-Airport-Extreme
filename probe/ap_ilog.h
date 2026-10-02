/* ap_ilog.h -- an INTERRUPT-SAFE log ring for the AirPort driver.
 *
 * ⚠⚠ THIS EXISTS BECAUSE THE ALTERNATIVE IS A SILENT HARD HANG.
 *
 * Every logging call in this probe today ends at Out() -> FSWrite + FlushVol. That is the File
 * Manager, and CLAUDE.md's first OS 9 rule is that the File Manager must never be called below
 * task level: from an ISR or a secondary interrupt handler it hangs the machine outright, with no
 * NMI and no debugger. The usb2-ehci project hit this three separate times (r18, n4, and the r95
 * bulk_xfer trace) before the rule was written down.
 *
 * Stage 8-2 moves receive into an interrupt handler. The moment that happens, ANY Say() reachable
 * from the new code is a machine-killer. So the ring lands first, before the interrupt code that
 * needs it, rather than after.
 *
 * ── DESIGN, taken from usb2-ehci/src/ehci_os.c, which paid for these lessons ──
 *
 *  1. STRING LITERALS ONLY. The ring stores the POINTER, never a copy. Literals live in the code
 *     fragment, which is resident, so the pointer is still valid when the drain reads it minutes
 *     later. Passing a stack buffer here is a use-after-return that will usually "work" in
 *     testing and corrupt the log under load.
 *  2. Single producer, lock-free. The OS guarantees at most one secondary interrupt handler runs
 *     at a time, and a primary ISR cannot be re-entered by itself, so head/tail need no lock --
 *     but they DO need `volatile`, because the drain runs at task level and the compiler must not
 *     cache them.
 *  3. NEVER BLOCK, NEVER WRAP OVER UNREAD DATA. A full ring counts a drop and returns. Dropping
 *     a line is a bounded, reported loss; overwriting unread entries loses the START of an event,
 *     which is the half that explains it.
 *  4. A TOKEN-BUCKET RATE CAP, not a fixed window. An interrupt storm can produce thousands of
 *     lines per second and drown the narrative. ⚠ usb2-ehci's m24 run dropped 458 ring lines to a
 *     rate cap and the log then read as an effect with no cause in front of it -- so the cap
 *     reports how many it threw away, and CRITICAL lines are exempt.
 *  5. Drops and throttles are COUNTED and printed. A silent lossy log is worse than no log: it
 *     invites conclusions drawn from a gap.
 *
 * ⚠ ApILogDrain() calls the File Manager. TASK LEVEL ONLY. That is the whole point of the split.
 */
#ifndef AP_ILOG_H
#define AP_ILOG_H

/* Publish-before-index barrier. Real eieio on PowerPC; a compiler barrier everywhere else, which
 * is all the host test build needs and all it can honestly claim. */
#if defined(__ppc__) || defined(__POWERPC__) || defined(powerpc)
#  define AP_ILOG_BARRIER() __asm__ __volatile__("eieio" ::: "memory")
#else
#  define AP_ILOG_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

#define AP_ILOG_N            256u   /* entries; power of two not required, the index is modular */
#define AP_ILOG_BURST         96u   /* token depth: a whole receive burst fits without throttling */
#define AP_ILOG_RATE_PER_SEC  30u   /* refill rate once the burst is spent */

typedef struct { const char *msg; unsigned long val; unsigned char kind; } ApILogRec;

static volatile ApILogRec gApILog[AP_ILOG_N];
static volatile unsigned long gApILogHead = 0, gApILogTail = 0;
static volatile unsigned long gApILogDropped = 0, gApILogThrottled = 0;
static unsigned long gApILogTokens = AP_ILOG_BURST, gApILogRefillTick = 0;
/* ⚠ A SEPARATE FLAG, NOT A SENTINEL VALUE. This started as `if(refillTick == 0) refillTick = now`,
 * which collides with a legitimate tick of 0: the anchor then re-arms to "now" on every call and
 * no time ever appears to have passed, so the bucket never refills. On the target that hides
 * forever, because TickCount() is never 0 by the time a driver runs -- it would only surface after
 * a ~828-day wrap. The host test caught it at tick 0 in seconds. */
static int gApILogRefillArmed = 0;

/* The token bucket. `nowTicks` is TickCount() -- 60/sec -- passed in so the ring never calls a
 * Toolbox routine itself; TickCount is a low-memory read and safe below task level, but keeping
 * the dependency out of the ring makes it host-testable. Returns 1 if a token was taken. */
static int ApILogTake(unsigned long nowTicks)
{
    unsigned long elapsed;
    if(!gApILogRefillArmed){ gApILogRefillTick = nowTicks; gApILogRefillArmed = 1; }
    elapsed = nowTicks - gApILogRefillTick;
    if(elapsed >= 60u){
      unsigned long add = (elapsed / 60u) * AP_ILOG_RATE_PER_SEC;
      gApILogTokens = (gApILogTokens + add > AP_ILOG_BURST) ? AP_ILOG_BURST
                                                            : gApILogTokens + add;
      gApILogRefillTick += (elapsed / 60u) * 60u; }
    if(gApILogTokens == 0){ gApILogThrottled++; return 0; }
    gApILogTokens--;
    return 1;
}

/* kind: 1 = message only, 2 = message + value. crit = exempt from the rate cap (never from the
 * ring bound -- nothing here is unbounded). */
static void ApILogPut(const char *s,unsigned long v,unsigned char kind,int crit,
                      unsigned long nowTicks)
{
    unsigned long i;
    if(!crit && !ApILogTake(nowTicks)) return;
    i = gApILogHead;
    if(i - gApILogTail >= AP_ILOG_N){ gApILogDropped++; return; }   /* full: count, never wrap */
    gApILog[i % AP_ILOG_N].msg  = s;
    gApILog[i % AP_ILOG_N].val  = v;
    gApILog[i % AP_ILOG_N].kind = kind;
    /* The record must be visible before the index that publishes it. eieio is what usb2-ehci uses
     * in twelve places for exactly this, commented "publish payload before the index".
     * ⚠ On the host test build there is no PowerPC and nothing to order across, but the COMPILER
     *   must still not sink the stores below the index write -- so the barrier is conditional on
     *   the instruction, never on the ordering guarantee. */
    AP_ILOG_BARRIER();
    gApILogHead = i + 1;
}

/* ★ The four entry points. Safe from ANY execution level. Literals only. */
static void ApILog  (const char *s,unsigned long t)               { ApILogPut(s,0,1,0,t); }
static void ApILogX (const char *s,unsigned long v,unsigned long t){ ApILogPut(s,v,2,0,t); }
static void ApILogC (const char *s,unsigned long t)               { ApILogPut(s,0,1,1,t); }
static void ApILogCX(const char *s,unsigned long v,unsigned long t){ ApILogPut(s,v,2,1,t); }

/* How many entries are waiting. Task level uses this to decide whether to drain. */
static unsigned long ApILogPending(void){ return gApILogHead - gApILogTail; }

#endif /* AP_ILOG_H */
