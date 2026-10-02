/* ilog_test.c -- known-answer tests for ap_ilog.h, run on the Mac, not the G4.
 *
 * The ring's failure modes are all SILENT ones: a dropped line, a throttled line, a wrapped index
 * that overwrites the start of an event. None of them announces itself on the target, and each
 * produces a log that reads as a complete narrative with a hole in it -- which is how usb2-ehci's
 * m24 run came to look like an effect with no cause. So the properties are pinned here, for free,
 * before any of it runs below task level.
 *
 * Build and run:  cc -Wall -o ilog_test ilog_test.c && ./ilog_test
 */
#include <stdio.h>
#include <string.h>

/* The header uses a PowerPC eieio; on the host there is nothing to order, so stub it out. */
#ifndef __ppc__
#define eieio_stub
#endif
#include "ap_ilog.h"

static int gFail = 0;

static void checkb(const char *what,int ok)
{
    if(ok){ printf("  [ok]   %s\n",what); return; }
    printf("  [FAIL] %s\n",what);
    gFail++;
}

static void reset(void)
{
    gApILogHead = gApILogTail = 0;
    gApILogDropped = gApILogThrottled = 0;
    gApILogTokens = AP_ILOG_BURST; gApILogRefillTick = 0; gApILogRefillArmed = 0;
}

int main(void)
{
    printf("=== ap_ilog: ordering and payload ===\n");
    reset();
    ApILog("first",0); ApILogX("second",0xBEEF,0); ApILog("third",0);
    checkb("three entries pending", ApILogPending()==3);
    checkb("entry 0 is the first message",  !strcmp((const char*)gApILog[0].msg,"first"));
    checkb("entry 1 carries its value",     gApILog[1].val==0xBEEF && gApILog[1].kind==2);
    checkb("entry 2 is the third message",  !strcmp((const char*)gApILog[2].msg,"third"));
    checkb("a message-only entry has kind 1", gApILog[0].kind==1);

    printf("=== the ring is BOUNDED and never wraps over unread data ===\n");
    { unsigned long i;
      reset();
      /* Use critical puts so the rate cap is not what stops us -- we are testing the ring bound. */
      for(i=0;i<AP_ILOG_N+50;i++) ApILogC("flood",0);
      checkb("pending never exceeds the ring size", ApILogPending()==AP_ILOG_N);
      checkb("the overflow is COUNTED, not silent", gApILogDropped==50);
      /* ★ The point of the bound: the FIRST entry must still be the first one written. A ring that
       * wrapped would have lost the start of the event, which is the half that explains it. */
      checkb("the oldest entry survived (the start of the event is intact)",
             !strcmp((const char*)gApILog[0].msg,"flood")); }

    printf("=== the token bucket throttles, reports, and refills ===\n");
    { unsigned long i;
      reset();
      for(i=0;i<AP_ILOG_BURST;i++) ApILog("burst",0);
      checkb("a full burst passes without throttling", gApILogThrottled==0);
      checkb("the burst is all in the ring", ApILogPending()==AP_ILOG_BURST);
      ApILog("one too many",0);
      checkb("the next line IS throttled",   gApILogThrottled==1);
      checkb("and it did NOT enter the ring", ApILogPending()==AP_ILOG_BURST);
      /* One second later (60 ticks) the bucket refills by AP_ILOG_RATE_PER_SEC. */
      ApILog("after a second",60);
      checkb("a refill lets lines through again", ApILogPending()==AP_ILOG_BURST+1); }

    printf("=== CRITICAL lines are exempt from the rate cap ===\n");
    { unsigned long i;
      reset();
      for(i=0;i<AP_ILOG_BURST+20;i++) ApILog("ordinary",0);
      checkb("ordinary lines were throttled once the burst ran out", gApILogThrottled==20);
      { unsigned long before = ApILogPending();
        ApILogC("CRITICAL",0);
        checkb("a critical line gets through with zero tokens left",
               ApILogPending()==before+1); } }

    printf("=== but critical lines are NOT exempt from the ring bound ===\n");
    { unsigned long i;
      reset();
      for(i=0;i<AP_ILOG_N+10;i++) ApILogC("crit flood",0);
      checkb("nothing is unbounded, even critical", ApILogPending()==AP_ILOG_N);
      checkb("critical overflow is counted too", gApILogDropped==10); }

    printf("=== draining frees space, and order is preserved across the wrap ===\n");
    { unsigned long i;
      reset();
      for(i=0;i<AP_ILOG_N;i++) ApILogC("a",0);
      gApILogTail += 10;                       /* simulate a task-level drain of 10 */
      checkb("draining reduces the pending count", ApILogPending()==AP_ILOG_N-10);
      for(i=0;i<10;i++) ApILogC("b",0);
      checkb("the freed slots accept new entries", ApILogPending()==AP_ILOG_N);
      checkb("no drops once space was freed", gApILogDropped==0);
      checkb("the new entries landed in the reused slots",
             !strcmp((const char*)gApILog[0].msg,"b")); }

    printf("\n");
    if(gFail==0) printf("ALL ILOG TESTS PASSED.\n");
    else         printf("%d TEST(S) FAILED -- do not build this into a probe.\n",gFail);
    return gFail ? 1 : 0;
}
