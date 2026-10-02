/* txbp_test.c -- k202's transmit back-pressure, modelled: a write queue in front of a 32-frame ring.
 *
 * Models the decision logic of ap_otmodl.c (apTxOrQueue / apQueueTx / otm_wsrv / the deferred-task
 * wakeup) against a ring that drains at the air's pace, fed by TCP-like bursts. It cannot model OT's
 * STREAMS scheduler itself (putq auto-enabling, canputnext) -- that is the hardware run's job -- but it
 * checks the logic that decides what happens to each frame.
 *
 * What would make it fail (ask what an oracle would FAIL on):
 *   - any frame dropped while the queue is under its cap (the k201 behaviour: the rule's whole point);
 *   - any frame sent out of order (a new frame overtaking one already waiting);
 *   - a frame left waiting forever (a missed wakeup) once the ring drains;
 *   - the cap not holding when the radio stalls.
 * And it must SEE the bug it replaces: the k201 policy (drop when full), same trace, has to drop.
 *
 * Build and run:  cc -Wall -o txbp_test txbp_test.c && ./txbp_test      (exit 0 = all passed) */
#include <stdio.h>
#include <string.h>

#define RING    32
#define QMAX    4096
#define CAPB    262144UL
#define FRAME   1514UL

static int  ring[RING], ringUsed, ringHead, ringTail;      /* frames in flight, FIFO */
static int  q[QMAX], qHead, qTail;                         /* the write queue */
static unsigned long qBytes;
static int  waiting, nextSend, sentSeq[200000], nSent, dropped, capDrops, wsrvRuns, wakeups;
static unsigned int rng = 99u;
static unsigned int rnd(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void reset(void)
{ ringUsed = ringHead = ringTail = qHead = qTail = 0; qBytes = 0;
  waiting = nextSend = nSent = dropped = capDrops = wsrvRuns = wakeups = 0; }

static int  qEmpty(void){ return qHead == qTail; }
static void qPut(int f){ q[qTail++ % QMAX] = f; qBytes += FRAME; }
static int  qGet(void){ qBytes -= FRAME; return q[qHead++ % QMAX]; }
static void qPutBack(int f){ qHead--; q[qHead % QMAX] = f; qBytes += FRAME; }

/* ApShimWriteFrame: 0 = on the ring, 1 = ring full ("later") */
static int hwTx(int f)
{ if(ringUsed >= RING) return 1;
  ring[ringHead++ % RING] = f; ringUsed++; sentSeq[nSent++] = f; return 0; }
static void airDrain(int n){ while(n-- > 0 && ringUsed > 0){ ringTail++; ringUsed--; } }

static int radioStalled;
static void queueTx(int f)
{ if(qBytes > CAPB){ capDrops++; return; }
  waiting = 1; qPut(f); }

/* policy 1 = k202 (keep), policy 0 = k201 (drop when full) */
static void txOrQueue(int f, int policy)
{ if(policy == 1 && !qEmpty()){ queueTx(f); return; }
  if(hwTx(f) == 1){ if(policy == 1) queueTx(f); else dropped++; } }

static void wsrv(void)
{ wsrvRuns++;
  while(!qEmpty()){
    int f = qGet();
    if(hwTx(f) == 1){ waiting = 1; qPutBack(f); break; } } }

/* the secondary: TX completions (the air drained some frames) -> schedule the deferred task, which
 * clears `waiting` and qenables -> OT runs wsrv */
static void interrupt(void)
{ if(!radioStalled) airDrain(1 + (int)(rnd() % 3u));
  if(waiting){ waiting = 0; wakeups++; wsrv(); } }

#define HIWAT 16384UL      /* gModInfo.mi_hiwat: past it canputnext fails upstream and TCP holds its data */
static void run(int policy, int frames, int burstMax)
{ while(nextSend < frames){
    int b = 1 + (int)(rnd() % (unsigned)burstMax), i;
    for(i = 0; i < b && nextSend < frames; i++){                  /* TCP hands a burst ...           */
      if(policy == 1 && qBytes >= HIWAT) break;                  /* ... until canputnext says stop  */
      txOrQueue(nextSend++, policy); }
    interrupt(); }
  { int guard = 0; while((!qEmpty() || ringUsed) && guard++ < 1000000) interrupt(); } }

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok){ gChecks++; if(!ok){ gFails++; printf("  [FAIL] %s\n", what); } }

int main(void)
{
    int i, inOrder;

    reset(); rng = 7u; run(0, 100000, 60);
    printf("  k201 policy (drop when full): %d of 100000 dropped\n", dropped);
    Check("k201's drop-when-full DOES drop frames on bursty input (the bug reproduces)", dropped > 0);

    { int bursts[3] = { 8, 60, 200 }, k;
      for(k = 0; k < 3; k++){
        char msg[160];
        reset(); rng = 11u + (unsigned)k; run(1, 100000, bursts[k]);
        for(inOrder = 1, i = 0; i < nSent; i++) if(sentSeq[i] != i){ inOrder = 0; break; }
        printf("  k202 policy, bursts up to %3d: sent %d, dropped %d, cap drops %d, wsrv %d, wakeups %d\n",
               bursts[k], nSent, dropped, capDrops, wsrvRuns, wakeups);
        sprintf(msg, "k202, bursts up to %d: every frame sent exactly once, in order, none dropped", bursts[k]);
        Check(msg, nSent == 100000 && dropped == 0 && capDrops == 0 && inOrder);
        sprintf(msg, "k202, bursts up to %d: nothing left waiting once the ring drains", bursts[k]);
        Check(msg, qEmpty() && ringUsed == 0); } }

    /* a stalled radio (the air stops draining) must not grow the queue without bound */
    reset(); radioStalled = 1;
    for(i = 0; i < 5000; i++){ txOrQueue(i, 1); interrupt(); }
    printf("  stalled radio: queue %lu bytes, cap drops %d\n", qBytes, capDrops);
    Check("a stalled radio: the write queue stops at its cap", qBytes <= CAPB + FRAME && capDrops > 0);
    radioStalled = 0;
    { int guard = 0; while((!qEmpty() || ringUsed) && guard++ < 1000000) interrupt(); }
    Check("...and drains completely once the radio recovers", qEmpty() && ringUsed == 0);

    printf("%s  %d checks, %d failed\n", gFails ? "[FAIL]" : "[ok] ", gChecks, gFails);
    return gFails ? 1 : 0;
}
