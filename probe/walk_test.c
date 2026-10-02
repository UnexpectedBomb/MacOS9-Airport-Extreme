/* walk_test.c -- does walking the POISON find frames a CURSOR walk misses?
 * Models both walks over the same 32-slot ring and the same arrival trace. */
#include <stdio.h>
#define SLOTS 32

static int poisoned[SLOTS];      /* 1 = we poisoned it, 0 = engine wrote a frame */
static int content[SLOTS];       /* frame id, -1 = none */
static int engine;               /* RXDPTR */
static int cursor;               /* gRxSlot */

static void arrive(int id){ content[engine]=id; poisoned[engine]=0;
                            engine=(engine+1)%SLOTS; }

/* the OLD walk: while(cursor != engine) */
static int walk_cursor(int want){
    int found=0;
    while(cursor != engine){
        if(!poisoned[cursor] && content[cursor]==want) found=1;
        poisoned[cursor]=1; cursor=(cursor+1)%SLOTS; }
    return found; }

/* the NEW walk: advance while the slot holds data, bounded by one ring */
static int walk_poison(int want){
    int step,found=0;
    for(step=0; step<SLOTS; step++){
        if(poisoned[cursor]) break;
        if(content[cursor]==want) found=1;
        poisoned[cursor]=1; cursor=(cursor+1)%SLOTS; }
    return found; }

static void reset(void){ int i; for(i=0;i<SLOTS;i++){poisoned[i]=1;content[i]=-1;}
                         engine=0; cursor=0; }

int main(void){
    int i,fails=0;

    /* CASE 1: caught up -- 1 frame arrives, we want it. Both must find it. (k81's shape) */
    reset(); arrive(7);
    if(!walk_cursor(7)){ printf("  cursor walk FAILED case 1\n"); fails++; }
    reset(); arrive(7);
    if(!walk_poison(7)){ printf("  poison walk FAILED case 1\n"); fails++; }
    printf("case 1  caught up, 1 frame          cursor=find  poison=find\n");

    /* CASE 2: EXACT lap -- 32 frames arrive with nobody walking, engine returns to cursor.
     * The wanted frame is in there. This is k20's measured shape. */
    reset(); for(i=0;i<SLOTS;i++) arrive(i==17?99:i);
    { int c,p;
      c = walk_cursor(99);
      reset(); for(i=0;i<SLOTS;i++) arrive(i==17?99:i);
      p = walk_poison(99);
      printf("case 2  exact lap (32 frames)       cursor=%s  poison=%s\n",
             c?"find":"MISS", p?"find":"MISS");
      if(c) { printf("  note: cursor unexpectedly found it\n"); }
      if(!p){ printf("  FAIL: poison walk MISSED -- the fix does not work\n"); fails++; } }

    /* CASE 3: partial lap then the wanted frame arrives last (k83's shape:
     * a long gap fills most of the ring, then the auth response lands). */
    reset(); for(i=0;i<20;i++) arrive(i); arrive(99);
    { int c,p;
      c = walk_cursor(99);
      reset(); for(i=0;i<20;i++) arrive(i); arrive(99);
      p = walk_poison(99);
      printf("case 3  21 backlog + wanted last    cursor=%s  poison=%s\n",
             c?"find":"MISS", p?"find":"MISS");
      if(!p){ printf("  FAIL: poison walk MISSED case 3\n"); fails++; } }

    /* CASE 4: over-lap -- 40 frames into 32 slots, wanted frame is #35 (survives). */
    reset(); for(i=0;i<40;i++) arrive(i==35?99:i);
    { int c,p;
      c = walk_cursor(99);
      reset(); for(i=0;i<40;i++) arrive(i==35?99:i);
      p = walk_poison(99);
      printf("case 4  over-lap 40 into 32         cursor=%s  poison=%s\n",
             c?"find":"MISS", p?"find":"MISS");
      if(!p){ printf("  FAIL: poison walk MISSED case 4 -- frame WAS in memory\n"); fails++; } }

    /* CASE 5: empty ring must not spin or find anything. */
    reset();
    if(walk_poison(5)){ printf("  FAIL: poison walk found a frame in an empty ring\n"); fails++; }
    printf("case 5  empty ring                  poison=correctly finds nothing\n");

    printf("\n%s\n", fails? "FAILURES" : "[ok] the poison walk finds every frame that is in memory");
    return fails!=0; }
