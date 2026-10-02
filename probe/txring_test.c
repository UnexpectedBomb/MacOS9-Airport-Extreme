/* txring_test.c -- host suite for ap_txring.h (k195): the TX ring's positions, buffers, TXINDEX and
 * reclaim, driven against a MODEL of the DMA engine.
 *
 * Build:  cc -Wall -o txring_test txring_test.c && ./txring_test      (exit 0 = every check passed)
 *
 * ★ THE ORACLE IS THE MODEL ENGINE, NOT THE RING'S OWN COUNTERS. The model walks descriptors one at a
 * time from its current pointer toward the last TXINDEX written, wrapping after descriptor 255 (the one
 * carrying DTABLEEND), and records which POSITIONS it has finished -- a frame is finished when the
 * engine moves off its body descriptor. The one property that matters on the card is checked on every
 * post: the buffer being filled belongs to NO position the engine has not finished. The ring could
 * keep perfectly consistent counters and still fail that; this test would see it.
 *
 * Expected TXINDEX values are written out by hand from b43's op32_poke_tx(next_slot(body)).
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;

#include "ap_txring.h"

static int gFails = 0, gChecks = 0;
static void Check(const char *what, int ok)
{
    gChecks++;
    if(!ok) gFails++;
    printf("  %-70s %s\n", what, ok ? "[ok]" : "[FAIL]");
}

/* ---- the model engine ------------------------------------------------------------------------ */
static UInt32 mCur, mLast;                      /* descriptors */
static UInt32 mFifo[AP_TXR_POS], mFh, mFn;      /* positions posted and not yet finished, in order */
static void   ModelReset(UInt32 at){ mCur = mLast = at; mFh = mFn = 0; }
static void   ModelPoke(UInt32 txIndexBytes){ mLast = txIndexBytes / AP_TXR_DESC_BYTES; }
static void   ModelPost(UInt32 pos){ mFifo[(mFh + mFn) % AP_TXR_POS] = pos; mFn++; }
/* One descriptor. Leaving a body descriptor (odd) finishes the oldest posted position. */
static int ModelStep(void)
{
    if(mCur == mLast) return 0;
    if(mCur & 1u){
        if(!mFn || mFifo[mFh] != mCur / 2u){ printf("    model: engine finished position %u out of order\n", mCur / 2u); gFails++; }
        mFh = (mFh + 1u) % AP_TXR_POS; mFn--;
    }
    mCur = (mCur + 1u) % AP_TXR_DESCS;          /* DTABLEEND on 255 wraps to 0 */
    return 1;
}
static int ModelOwnsBuf(UInt32 buf)
{
    UInt32 i;
    for(i = 0; i < mFn; i++) if(ApTxrBuf(mFifo[(mFh + i) % AP_TXR_POS]) == buf) return 1;
    return 0;
}

/* One post, exactly as ApShimWriteFrame does it: reclaim from the engine's pointer, check room,
 * fill the head position's buffer, poke TXINDEX, commit. Returns 1 posted, 0 refused (ring full). */
static UInt32 gBufOverwrites = 0, gReclaimMismatch = 0, gBadPtr = 0;
static int Post(ApTxRing *r)
{
    UInt32 pos, buf;
    int n = ApTxrReclaim(r, mCur);
    if(n < 0) gBadPtr++;
    if(r->used != mFn) gReclaimMismatch++;     /* after a reclaim the ring must agree with the engine */
    if(!ApTxrHasRoom(r)) return 0;
    pos = r->head; buf = ApTxrBuf(pos);
    if(ModelOwnsBuf(buf)) gBufOverwrites++;    /* ★ the property: never refill a buffer in flight */
    ModelPost(pos);
    ModelPoke(ApTxrTxIndex(pos));
    (void)ApTxrCommit(r);
    return 1;
}

int main(void)
{
    ApTxRing r;
    char msg[160];
    UInt32 i;

    printf("== geometry ==\n");
    Check("256 descriptors = b43's B43_TXRING_SLOTS (DTABLEEND lands on 255)", AP_TXR_DESCS == 256u);
    Check("128 positions: two descriptors (header, body) per frame", AP_TXR_POS == 128u);
    Check("positions are a whole number of buffer laps (p % 32 stays consistent across the wrap)",
          (AP_TXR_POS % AP_TXR_BUFS) == 0u);
    Check("at most 32 in flight: a post needs <= 31 in flight, so head % 32 is free",
          AP_TXR_MAXOUT == AP_TXR_BUFS);
    Check("'full' would need 128 in flight, so head == tail can only mean EMPTY",
          AP_TXR_MAXOUT < AP_TXR_POS);
    Check("a frame fits its buffer: 24 hdr + 8 CCMP + 8 SNAP + 1500 + 8 MIC = 1548 <= frame area",
          24u + 8u + 8u + 1500u + 8u <= AP_TXR_FRMMAX);
    Check("the txhdr fits its area (TXH_SIZE_351 is 106, or 110 with the rev>=410 shift)", 110u <= AP_TXR_HDRMAX);
    Check("two buffers per 4 KB page, so no buffer straddles a page", 2u * AP_TXR_BUFBYTES == 4096u);

    printf("== TXINDEX = the descriptor after the body, in bytes (op32_poke_tx) ==\n");
    { static const struct { UInt32 pos, want; } t[] = {
          {0, 16}, {1, 32}, {2, 48}, {63, 1024}, {64, 1040}, {126, 2032}, {127, 0} };
      for(i = 0; i < sizeof t / sizeof t[0]; i++){
          sprintf(msg, "position %3u (descriptors %3u,%3u) -> TXINDEX %4u", t[i].pos, 2*t[i].pos, 2*t[i].pos+1, t[i].want);
          Check(msg, ApTxrTxIndex(t[i].pos) == t[i].want); } }
    Check("header descriptor of position 5 is 10 (body 11)", ApTxrHdrDesc(5) == 10u);
    Check("position 37 uses buffer 5; position 127 uses buffer 31", ApTxrBuf(37) == 5u && ApTxrBuf(127) == 31u);

    printf("== start ==\n");
    Check("start at descriptor 0 -> head = tail = 0, empty", ApTxrStartAt(&r, 0) && r.head == 0 && r.tail == 0 && r.used == 0);
    Check("start at descriptor 40 -> position 20", ApTxrStartAt(&r, 40) && r.head == 20 && r.tail == 20);
    Check("start REFUSED at an odd descriptor (half-way through a frame)", !ApTxrStartAt(&r, 41));
    Check("start REFUSED past the end (256)", !ApTxrStartAt(&r, 256));

    printf("== reclaim, by hand ==\n");
    ApTxrStartAt(&r, 0);
    for(i = 0; i < 3; i++) (void)ApTxrCommit(&r);                     /* positions 0,1,2 in flight */
    Check("engine still on descriptor 0 (frame 0's header): frees nothing", ApTxrReclaim(&r, 0) == 0 && r.used == 3);
    Check("engine on descriptor 1 (frame 0's BODY): still frees nothing", ApTxrReclaim(&r, 1) == 0 && r.used == 3);
    Check("engine on descriptor 2: frame 0 done -> frees 1", ApTxrReclaim(&r, 2) == 1 && r.used == 2 && r.tail == 1);
    Check("engine on descriptor 6 (= TXINDEX, idle): frees the other 2", ApTxrReclaim(&r, 6) == 2 && r.used == 0 && r.tail == 3);
    (void)ApTxrCommit(&r);                                              /* position 3 */
    Check("a pointer BEHIND the tail is refused and frees nothing", ApTxrReclaim(&r, 2) == -1 && r.used == 1 && r.tail == 3);
    Check("a pointer AHEAD of the head is refused and frees nothing", ApTxrReclaim(&r, 12) == -1 && r.used == 1);
    Check("a pointer past the ring (256) is refused", ApTxrReclaim(&r, 256) == -1 && r.used == 1);

    printf("== the wrap ==\n");
    ApTxrStartAt(&r, 250);                                              /* position 125 */
    for(i = 0; i < 5; i++) (void)ApTxrCommit(&r);                       /* 125,126,127,0,1 */
    Check("head wrapped: 125 + 5 = position 2", r.head == 2u && r.used == 5u);
    Check("engine wrapped to descriptor 0: 125,126,127 done (3 freed), tail = 0", ApTxrReclaim(&r, 0) == 3 && r.tail == 0 && r.used == 2);
    Check("engine on descriptor 4 (= TXINDEX of position 1): all freed", ApTxrReclaim(&r, 4) == 2 && r.used == 0);

    printf("== room ==\n");
    ApTxrStartAt(&r, 0);
    for(i = 0; i < AP_TXR_MAXOUT - 1u; i++) (void)ApTxrCommit(&r);
    Check("31 in flight: room for one more", ApTxrHasRoom(&r));
    (void)ApTxrCommit(&r);
    Check("32 in flight: no room", !ApTxrHasRoom(&r));
    Check("... and head != tail, so 'full' never looks like 'empty'", r.head != r.tail);
    Check("engine finishes one: room again", ApTxrReclaim(&r, 2) == 1 && ApTxrHasRoom(&r));

    printf("== the model engine: random posts vs random engine progress ==\n");
    { struct { const char *name; int postPct; int maxSteps; } sc[] = {
          { "engine faster than the driver (ring mostly empty)",  30, 12 },
          { "balanced",                                           50,  3 },
          { "driver faster than the engine (ring runs full)",     90,  1 },
          { "engine stalls for long stretches",                   70,  0 } };
      UInt32 s;
      for(s = 0; s < 4; s++){
          UInt32 step, posted = 0, refused = 0, laps, maxUsed = 0;
          srand(1234u + s);
          ApTxrStartAt(&r, 0); ModelReset(0);
          gBufOverwrites = gReclaimMismatch = gBadPtr = 0;
          for(step = 0; step < 400000u; step++){
              if(rand() % 100 < sc[s].postPct){ if(Post(&r)) posted++; else refused++; }
              { int k, n = sc[s].maxSteps ? rand() % (sc[s].maxSteps + 1) : ((rand() % 50) == 0 ? 40 : 0);
                for(k = 0; k < n; k++) (void)ModelStep(); }
              if(r.used > maxUsed) maxUsed = r.used;
          }
          laps = posted / AP_TXR_POS;
          sprintf(msg, "%s: %u posts, %u ring laps", sc[s].name, posted, laps);
          Check(msg, posted > 0 && laps > 100);
          sprintf(msg, "  no buffer refilled while the engine owned it (overwrites = %u)", gBufOverwrites);
          Check(msg, gBufOverwrites == 0);
          sprintf(msg, "  after every reclaim the ring agrees with the engine (mismatches = %u)", gReclaimMismatch);
          Check(msg, gReclaimMismatch == 0);
          sprintf(msg, "  the engine's pointer was never refused (bad pointers = %u)", gBadPtr);
          Check(msg, gBadPtr == 0);
          sprintf(msg, "  never more than 32 in flight (max seen %u; refused when full %u)", maxUsed, refused);
          Check(msg, maxUsed <= 32u);                /* written out, not the macro under test */
      }
      /* the full-ring scenario must actually have been full, or it did not test the cap */
      srand(99u); ApTxrStartAt(&r, 0); ModelReset(0);
      { UInt32 refusedFull = 0, step;
        for(step = 0; step < 20000u; step++){ if(!Post(&r)) refusedFull++; if(step % 3 == 0) (void)ModelStep(); }
        sprintf(msg, "the cap is exercised: a slow engine fills the ring and posts are refused (%u)", refusedFull);
        Check(msg, refusedFull > 0 && gBufOverwrites == 0); }
    }

    printf("\n%d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
