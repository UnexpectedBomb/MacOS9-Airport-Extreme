/* rxwalk_test.c -- the k201 receive-walk boundary, modelled against an engine that takes TIME to write
 * a frame. k201.
 *
 * k200's walk (poison only) was correct only while it was slow. The model's engine writes a frame in
 * three observable steps, as the card does: START (the frame's first bytes overwrite the poison, the
 * frame is incomplete), LAND (the whole frame is in memory), ADVANCE (RXDPTR moves to the next
 * descriptor -- which can trail LAND a moment; k201 settles 10 us for it). Walks run at random points
 * in between, the way interrupts and the pump do.
 *
 * What would make it fail (ask what an oracle would FAIL on):
 *   - the k201 rule consuming a slot whose frame has not LANDED (the k200 bug: "too short"/MIC fail);
 *   - losing, duplicating or reordering a frame when the walk keeps up (no lap);
 *   - a deferred frame never being consumed once the engine moves on (a stuck ring);
 *   - a lap happening without the lap counter noticing.
 * And the model must SEE the bug it is named for: the k200 rule, same trace, has to read partial frames.
 *
 * Build and run:  cc -Wall -o rxwalk_test rxwalk_test.c && ./rxwalk_test      (exit 0 = all passed) */
#include <stdio.h>
#include <string.h>

#define SLOTS 32

static int  poisoned[SLOTS], content[SLOTS], landed[SLOTS];
static int  cur, writing;              /* engine: RXDPTR, and whether a frame is in progress at cur */
static int  cursor;                    /* gRxSlot */
static int  nextId, lastSeen;          /* frames produced; last id the walk delivered */
static long partial, lost, dup, reordered, lapCount, overwrites;
static unsigned int rng = 12345u;
static unsigned int rnd(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void reset(void)
{ int i; for(i = 0; i < SLOTS; i++){ poisoned[i] = 1; content[i] = -1; landed[i] = 0; }
  cur = 0; writing = 0; cursor = 0; nextId = 0; lastSeen = -1;
  partial = lost = dup = reordered = lapCount = overwrites = 0; }

static void engineStart(void)          /* the frame's first bytes land: the poison is gone */
{ if(!poisoned[cur]) overwrites++;     /* the engine is overwriting a frame nobody read: a lap */
  content[cur] = nextId++; landed[cur] = 0; poisoned[cur] = 0; writing = 1; }
static void engineLand(void)   { landed[cur] = 1; }
static void engineAdvance(void){ cur = (cur + 1) % SLOTS; writing = 0; }

static void deliver(int slot)
{ int id = content[slot];
  if(!landed[slot]) partial++;
  if(id == lastSeen) dup++;
  else if(id < lastSeen) reordered++;
  else if(id > lastSeen + 1) lost += id - lastSeen - 1;
  if(id > lastSeen) lastSeen = id; }

/* rule 0 = k200 (poison only); rule 1 = k201 (poison, and never the engine's current descriptor) */
static void walk(int rule)
{ int step;
  for(step = 0; step < SLOTS; step++){
    if(poisoned[cursor]) break;
    if(rule == 1 && cursor == cur){
      if(!poisoned[(cursor + 1) % SLOTS]) lapCount++;
      break; }
    deliver(cursor);
    poisoned[cursor] = 1; landed[cursor] = 0;
    cursor = (cursor + 1) % SLOTS; } }

/* one random trace: the engine cycles START -> LAND -> ADVANCE; a walk runs with probability pWalk at
 * every step (a larger pWalk = a faster walk relative to the air) */
static void run(int rule, int frames, unsigned int pWalkPct)
{ int phase = 0;
  while(nextId < frames || phase != 0){
    if(phase == 0){ if(nextId < frames){ engineStart(); phase = 1; } }
    else if(phase == 1){ engineLand(); phase = 2; }
    else { engineAdvance(); phase = 0; }
    while((rnd() % 100u) < pWalkPct) walk(rule); }
  walk(rule); walk(rule);                  /* the ring drains once the engine is idle */
}

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok){ gChecks++; if(!ok){ gFails++; printf("  [FAIL] %s\n", what); } }

int main(void)
{
    long k200partial;

    /* the model must SEE the k200 bug: a fast walk over the same kind of trace reads partial frames */
    reset(); rng = 777u; run(0, 20000, 70u); k200partial = partial;
    printf("  k200 rule, fast walk: %ld partial frames read of 20000\n", k200partial);
    Check("k200's poison-only walk DOES read half-written frames when fast (the bug reproduces)", k200partial > 0);

    /* k201: slow walk, fast walk, in between. NEVER a partial frame. Where the walk kept up (no lap: the
     * engine never overwrote an unread frame) nothing may be lost, duplicated or reordered; where it
     * fell a full ring behind (the 5% walk pauses long enough), the loss is the free-running engine's
     * and the lap counter must have seen it. */
    { unsigned int p[5] = { 5u, 30u, 70u, 95u, 99u }; int i;
      for(i = 0; i < 5; i++){
        char msg[200];
        reset(); rng = 777u + (unsigned int)i; run(1, 20000, p[i]);
        printf("  k201 rule, walk %2u%%: partial %ld  lost %ld  dup %ld  reordered %ld  overwrites %ld  laps counted %ld\n",
               p[i], partial, lost, dup, reordered, overwrites, lapCount);
        sprintf(msg, "k201 rule, walk %u%%: never a partial frame", p[i]);
        Check(msg, partial == 0);
        if(overwrites == 0){
          sprintf(msg, "k201 rule, walk %u%%, no lap: all 20000 delivered once, in order", p[i]);
          Check(msg, lost == 0 && dup == 0 && reordered == 0 && lastSeen == 19999); }
        else {
          sprintf(msg, "k201 rule, walk %u%%, lapped: the lap counter saw it", p[i]);
          Check(msg, lapCount > 0); } } }

    /* a deferred frame is not stuck: engine LANDs + ADVANCEs, the next walk takes it */
    reset(); engineStart(); walk(1);
    Check("a frame still being written is left alone", lastSeen == -1 && !poisoned[0]);
    engineLand(); walk(1);
    Check("...even once it has landed, while RXDPTR still points at it (the lag k201 settles)", lastSeen == -1);
    engineAdvance(); walk(1);
    Check("...and taken by the first walk after the pointer moves on", lastSeen == 0 && poisoned[0]);

    /* a lap: walks stop for more than a ring's worth of frames -> the engine overwrites unread frames;
     * the lap counter must notice (the loss itself is unavoidable with a free-running engine) */
    { int f;
      reset();
      for(f = 0; f < SLOTS + 4; f++){ engineStart(); engineLand(); engineAdvance(); }
      engineStart();                                 /* mid-frame, a full ring behind */
      walk(1);
      Check("a lap overwrites unread frames (the model produced one)", overwrites > 0);
      Check("...and the k201 walk counts it when it defers at the engine's slot", lapCount > 0); }

    printf("%s  %d checks, %d failed\n", gFails ? "[FAIL]" : "[ok] ", gChecks, gFails);
    return gFails ? 1 : 0;
}
