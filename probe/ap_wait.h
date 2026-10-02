/* ap_wait.h -- the PROMPT receive path: recycle a consumed slot, and wait for a specific frame.
 *
 * ★ THE SEVENTH EXTRACTION, and the one 8-3b needs. Authentication is an exchange: send a
 * frame, then wait for a SPECIFIC answer -- type 0, subtype 11, from the AP we addressed, to us.
 * "Poll the ring and see what turned up" is not that. RxWaitFor is the predicate, and it comes
 * across whole rather than being approximated in the driver.
 *
 * ⚠⚠ RxRecycle CAME WITH IT AND HAD TO. RxWaitFor re-arms consumed slots as it walks, so
 *   a wait longer than K3_RX_SLOTS frames does not run the ring dry -- and on a busy channel a
 *   500 ms wait sees far more than 32 frames. Taking RxWaitFor without RxRecycle would have
 *   produced a receiver that worked for the first 32 frames of every wait and then went deaf,
 *   which is the kind of fault that looks like a flaky AP.
 *
 * ⚠ BOTH REWRITE RXINDEX. That is deliberate and it is b43's behaviour -- the engine is never
 *   stopped and never reset here, which is exactly what the k46/k47 work established: b43
 *   recycles descriptors at runtime and resets a live RX ring only at dma_init and teardown.
 *
 * ⚠ gPromptBuf IS 2400 BYTES (k192) -- it was 512, sized for EAPOL-Key message 3 back when only the
 *   join used this path. Stage 8's data pump reuses it for EVERY data frame, and at 512 it silently
 *   truncated anything over a ~438-byte Ethernet frame (the MIC is at the end, so they failed decrypt).
 *   See AP_PROMPT_BUF below. A comment about "the largest frame this path catches" is a claim about
 *   its CALLERS; re-check it whenever a new caller appears.
 *
 * ★ HOW IT WAS MADE. Three spans lifted VERBATIM by line range, never retyped, proven by
 * reconstruction against the original file.
 */
#ifndef AP_WAIT_H
#define AP_WAIT_H

#include "ap_tx.h"   /* the transmit layer, and through it the ring, parser and formatters */
#include "ap_copy.h" /* k200: frame-length word copies out of DMA memory (host-tested: rxcopy_test.c) */

/* k200: a timing hook around RxWaitFor's copy. ap_shim.c defines it before including this chain;
 * every other includer gets a no-op. */
#ifndef AP_RXW_COPY_BEGIN
#define AP_RXW_COPY_BEGIN()   ((void)0)
#define AP_RXW_COPY_END()     ((void)0)
#endif
/* k202: a slot left for the engine (k201), and the moment it is finally taken -- so the snapshot can
 * say how long a held-back frame waited. ap_shim.c times them; everyone else gets no-ops. */
#ifndef AP_RXW_DEFERRED
#define AP_RXW_DEFERRED(slot) ((void)0)
#define AP_RXW_TAKEN(slot)    ((void)0)
#endif
/* ============================================================================
 * ★ THE RX RECYCLE, WHICH IS WHAT b43 ACTUALLY DOES.
 *
 * k13 reproduced the wedge and named it: gArmFails = 4, RXSTATUS 0x0C8010C0 --
 * RXSTATE ACTIVE, RXDPTR 192, and the engine never reaching DISABLED. It takes TX
 * down with it, which is why k13's antenna passes 2 and 3 never transmitted and
 * why that ladder proved nothing.
 *
 * The probe's reset-and-rebuild was never a supported operation. b43 resets an RX
 * controller exactly twice -- at dma_init and at dmacontroller_cleanup -- and never
 * in between. At runtime b43_dma_rx does this instead:
 *
 *     current_slot = ops->get_current_rxslot(ring);     // RXSTATUS & RXDPTR, / 8
 *     for (slot = ring->current_slot; slot != current_slot; slot = next_slot(...))
 *             dma_rx(ring, &slot);                      // consume, re-poison
 *     wmb();
 *     ops->set_current_rxslot(ring, slot);              // RXINDEX = slot * 8
 *
 * Consumed slots are re-poisoned and handed back by advancing RXINDEX. The engine
 * is never stopped. That is ported below, and the re-arm is gone from the passes.
 * ==========================================================================*/
static int gRxSlot = 0;

static int RxRecycle(UInt32 base,DmaBlock *ring,UInt8 **bufLog,UInt32 *bufPhys,
                     UInt32 frameoffset,UInt32 bufSize,
                     UInt8 *keepFirst,int *keptOut)
{
    UInt32 rxs = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
    int current = (int)((rxs & B43_DMA32_RXDPTR) / B43_DMADESC32_BYTES);
    int slot = gRxSlot, n = 0, kept = 0;

    if(current < 0 || current >= K3_RX_SLOTS) return -1;   /* refuse a nonsense pointer */

    /* ⚠⚠ k20's DEFECT, AND IT VOIDED ORACLE I FOR ELEVEN PROBES.
     *
     * `while (slot != current)` is b43's loop, and for b43 it is unambiguous: b43 recycles
     * from the RX interrupt, so it is never more than a few slots behind the engine. We poll
     * once a second, with a 32-slot ring, on a channel carrying about thirty beacons a
     * second. The engine LAPS the ring between polls and comes back to the exact slot we
     * left it on -- so `slot == current` on entry, the loop runs ZERO times, nothing is
     * poisoned and nothing is handed back. Every later call finds the same thing. The
     * receiver is starved for the rest of the run.
     *
     * That is not a theory; it is what k20 measured, and every number agrees:
     *   RXSTATUS 0x05802058 at all four reads  -- RXDPTR frozen at 88 (slot 11), IDLEWAIT
     *   "frames heard" = 32 = the WHOLE ring   -- the engine had lapped it once and stopped
     *   both passes identical: 32 / 32 / 6     -- two one-second windows cannot match
     * and the scan section of the same run, which never calls this function, stayed ACTIVE
     * on all three channels.
     *
     * So the cursor is not trustworthy: poison every slot that holds data, not just the ones
     * between two pointers that cannot tell "empty" from "full". */
    for(slot=0; slot<K3_RX_SLOTS; slot++){
      if(!RxBufferIsPoisoned(bufLog[slot],frameoffset)){
        if(!kept && keepFirst){                            /* keep one for the decoder */
          UInt32 q; for(q=0;q<128UL;q++) keepFirst[q] = bufLog[slot][q];
          kept = 1; }
        n++;
        /* the descriptor still points at the same page and its bytecount has not changed,
         * so unlike b43 -- which re-maps a fresh skb -- there is nothing to rewrite here. */
        PoisonRxBuffer(bufLog[slot],frameoffset); } }

    /* ★ AND HAND BACK THE WHOLE RING, USING THE VALUE THIS RUN PROVED KEEPS THE ENGINE
     * RUNNING. k20 wrote RXINDEX = slot*8 here, faithful to b43, and the engine stopped dead
     * after one lap. RxRingArm writes K3_RX_SLOTS*8 -- one descriptor past the end -- and the
     * scan section stayed ACTIVE for three 2-second dwells on the strength of it. Between a
     * value measured to work on this part and a value that is faithful to a driver whose
     * recycle runs off an interrupt, take the measurement.
     *
     * The cost is that a free-running engine may overwrite a buffer we have not read. For a
     * probe asking "did a probe response arrive in the last second" that is the right trade,
     * and it is exactly what the scan already does. */
    DmaPublish();                                          /* poison visible before the index */
    ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,
            (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES);
    gRxSlot = current;
    if(keptOut) *keptOut = kept;
    return n;
}

/* ============================================================================
 * ★★★★★ STAGE 6 PREREQUISITE: A PROMPT RECEIVE PATH.
 *
 * WHY THIS EXISTS, MEASURED RATHER THAN ASSUMED. k28 ran twice on identical code and firmware.
 * Run 1 caught 4 probe responses on the broadcast pass and 1 on the directed; run 2 caught
 * ZERO on both -- while ACKing in both runs, so the AP demonstrably received and answered us
 * each time. The difference is not the radio, it is us:
 *
 *     transmit  ->  ~600 ms draining TX status  ->  ~500 ms settling TSSI  ->  THEN scan
 *
 * and since k25 the RX engine free-runs over all 32 slots. On a channel carrying about thirty
 * beacons a second the ring WRAPS IN ROUGHLY ONE SECOND, so a reply that arrives promptly --
 * which is the only kind an auth or assoc response ever is -- has usually been overwritten
 * before RxScan looks. Run 1's five were timing luck.
 *
 * An 802.11 authentication response follows the request by single-digit milliseconds. An
 * association response is not much slower. Polling a wrapped ring a second later cannot see
 * either, so Stage 6 would have been unreadable and I would have spent runs deciding whether
 * the AP was refusing us or we were simply deaf. That is the k9/k20 failure again, and the
 * fix belongs before Stage 6 rather than after it.
 *
 * WHAT THIS DOES INSTEAD. It walks the ring as the engine fills it: poll RXDPTR on a short
 * period, parse only the slots that are genuinely new, test each against a predicate, and hand
 * the first match straight back. That is what a real driver does on an RX interrupt, minus the
 * interrupt -- and b43's own b43_dma_rx walks exactly this way, from ring->current_slot to the
 * hardware pointer.
 *
 * ⚠ IT RETURNS THE ELAPSED TIME TOO. "Did it arrive?" and "how fast?" are different questions,
 *   and the second one tells us whether the old 1.1 s path was ever going to work. */
#define RXW_ANY_TYPE  (-1)
#define RXW_ANY_SUB   (-1)

/* File scope rather than a local: 256 bytes of stack per pass is avoidable, and keeping the
 * caught frame around lets the decoder below work on it after the measurements have run. */
/* ⚠ 512, NOT 256. EAPOL-Key message 3 is the largest frame this probe catches: 24 + 8 + 4 + 95
 * plus encrypted Key Data carrying the AP's RSNE and the wrapped GTK, which lands near 200
 * bytes. RxWaitFor's copy length must cover it or the GTK would be silently truncated.
 *
 * ★★★ 9-29 k192: AND NOW 2400, BECAUSE "the largest frame this probe catches" STOPPED BEING TRUE.
 * That sentence was right while only the join used RxWaitFor. Stage 8's data pump (ApShimPumpLocked)
 * then reused RxWaitFor + gPromptBuf for EVERY data frame, and nobody resized it. The copy took the
 * first 512 bytes; the frame starts at 30 (RX header) + 6 (PLCP), so any 802.11 frame over ~476
 * bytes lost its tail -- including the CCMP MIC -- and ApRxToEnet, which reads raw frame_len bytes
 * with no bound against this buffer, read garbage past the end and dropped it as a decrypt failure.
 * The cutoff is a ~438-byte Ethernet frame. Everything that ever worked was under it (SYN-ACK 62,
 * pings, DHCP offers ~350); the Pi's first AFP reply was 467, resent 39 times, never received --
 * the "Finder freeze" after the first DHCP lease was a TCP connection starving on those drops.
 * 2400 holds the largest frame the radio can place in an RX buffer (30 + 6 + a 2304-byte MSDU with
 * QoS/CCMP headers, MIC and FCS = ~2386); each RX buffer is a full 4 KB page, so the copy never
 * leaves it. ApShimPumpLocked now also refuses (TOO_BIG, counted) any frame whose header claims more
 * than this buffer holds, so the over-read cannot recur. */
#define AP_PROMPT_BUF 2400
static UInt8 gPromptBuf[AP_PROMPT_BUF] __attribute__((aligned(16)));   /* k200: word copies need 4 */

/* ⚠ MacEq came with RxWaitFor because RxWaitFor compares addresses -- seventh extraction,
 * and the compiler caught it the same way it caught the constants blocks in the previous
 * two. A header that compiles standalone in a driver context is what makes that cheap. */
static int MacEq(const UInt8 *a,const UInt8 *b)
{ int i; for(i=0;i<6;i++) if(a[i]!=b[i]) return 0; return 1; }

/* ★★★ k197: FRAMES THAT FAILED THEIR FCS. They reach the ring only while MACCTL KEEP_BAD is set, which
 * ap_shim.c does for the data path after the join. RxWaitFor counts each one here and steps past it --
 * garbage is never parsed, matched, copied or delivered, on the join's waits or the pump's. The counts
 * measure the RECEPTION: a corrupted 1500-byte OFDM frame from our AP is the radio failing to take
 * what the AP sent, which is what k196's size-dependent outage looks like from the air side.
 * ⚠ addr2 of a corrupted frame may itself be corrupted, so "from our AP" is a lower bound. */
static UInt32 gRxFcsAny = 0, gRxFcsFromAp = 0;     /* all FCS failures walked; those whose addr2 reads as the AP */
static UInt32 gRxFcsOfdm[16], gRxFcsCck[5];        /* by rate: OFDM code nibble; CCK 1, 2, 5.5, 11, other */
static UInt32 gRxFcsLen[3];                        /* by length: <= 200, 201-1000, > 1000 bytes */
static UInt32 gRxFcsAnt[2];                        /* by the antenna it arrived on (phy_status0 ANT) */
static UInt32 gRxJssiBad[8];                       /* JSSI in eight 32-wide buckets */
/* k198: the antenna of EVERY frame the walk sees -- beacons, data, FCS failures, other BSSs alike --
 * from phy_status0's ANT bit. Sampled per second by the timeline, it says which antenna the receiver
 * was on through a deaf spell, which the data-frame counts cannot (a deaf spell has no data frames). */
static UInt32 gRxWalkAnt[2];

static UInt32 RxMacStatus(const UInt8 *buf)
{ return (UInt32)buf[RXH_MAC_STATUS] | ((UInt32)buf[RXH_MAC_STATUS+1] << 8) |
         ((UInt32)buf[RXH_MAC_STATUS+2] << 16) | ((UInt32)buf[RXH_MAC_STATUS+3] << 24); }

static void RxFcsCount(const UInt8 *buf,UInt32 frameoffset,const UInt8 *fromAddr)
{
    const UInt8 *f = buf + frameoffset + K6_HDR_PLCP6;
    UInt16 len = le16at(buf + RXH_FRAME_LEN);
    UInt16 ps0 = le16at(buf + RXH_PHY_STATUS0);
    UInt8  p0  = buf[frameoffset];                  /* PLCP byte 0: OFDM rate nibble / CCK SIGNAL */
    gRxFcsAny++;
    if(fromAddr && MacEq(f + 10, fromAddr)) gRxFcsFromAp++;
    if((ps0 & 0x0003u) == 0x0001u) gRxFcsOfdm[p0 & 0x0Fu]++;
    else gRxFcsCck[p0 == 0x0Au ? 0 : p0 == 0x14u ? 1 : p0 == 0x37u ? 2 : p0 == 0x6Eu ? 3 : 4]++;
    gRxFcsLen[len <= 200u ? 0 : len <= 1000u ? 1 : 2]++;
    gRxFcsAnt[(ps0 & B43_RX_PHYST0_ANT) ? 1 : 0]++;
    gRxJssiBad[buf[RXH_JSSI] >> 5]++;
}

/* ============================================================================
 * ★★★ THE CENSUS -- k84.
 *
 * k83 printed "frames walked = 26" and that number is a QUESTION, not an answer. It cannot
 * separate the only two explanations that matter:
 *
 *     the frame WAS in the ring and this function rejected it   -- a filter bug, ours
 *     the frame never reached memory at all                     -- a ring or a silent AP
 *
 * CLAUDE.md: "never log a status register as a bare number ... a hex value in a log is a
 * question; a decoded field is an answer." A bare COUNT is the same defect wearing different
 * clothes, and it cost this run. So the walk now records what it saw, and the caller prints it
 * on failure. gRxNear* is the sharpest of these: a frame of exactly the type and subtype we
 * wanted, rejected ONLY on an address, names the AP that answered us from a BSSID we were not
 * filtering for -- the base station also beacons under several locally-administered BSSIDs from one radio.
 *
 * gRxBacklogIn is the lap detector. k81 entered its auth wait with the ring caught up and found
 * the response after walking ONE frame; k83 entered 26 behind. If a failing run reports a
 * backlog at or near K3_RX_SLOTS, the engine lapped the cursor and the walk below is the fix. */
static UInt16 gRxCensusType[4];      /* every frame walked, by 802.11 type */
static UInt16 gRxCensusMgmt[16];     /* type 0 only, by subtype */
static UInt8  gRxNearFrom[4][6];     /* right type+subtype, WRONG address: who sent it */
static UInt8  gRxNearTo[4][6];       /* ... and who it was addressed to */
static int    gRxNearN;
static int    gRxBacklogIn;          /* unpoisoned slots when the wait STARTED */
static int    gRxBacklogOut;         /* ... and when it gave up */
static UInt32 gRxStatusIn, gRxStatusOut;
static int    gRxSlotIn;

static int RxCountUnpoisoned(UInt8 **bufLog,UInt32 frameoffset)
{ int i,n=0; for(i=0;i<K3_RX_SLOTS;i++) if(!RxBufferIsPoisoned(bufLog[i],frameoffset)) n++;
  return n; }

/* ★★★ k201: NEVER CONSUME THE SLOT THE ENGINE IS STILL WRITING.
 *
 * k200 made this walk ~30x faster (565 -> 19 us a frame) and downloads COLLAPSED, 1152 -> 135 KB/s:
 * the Pi resent 356 of 2629 segments and the Mac sent 998 duplicate ACKs, while the driver counted 351
 * frames "too short" (k199: 17) and 25 pairwise MIC failures (k199: 0). The walk now reaches a slot
 * while the AP's next frame is still streaming into it: the poison (the frame's first 8 bytes) is
 * already overwritten, but the RX header's frame_len is not written yet (-> too short) or the body is
 * not complete (-> MIC fail). It read the partial frame, dropped it, and POISONED the slot, so the
 * frame was gone even after the engine finished it. The slow walk had hidden this: at 565 us a frame
 * the engine was nearly always done first. Uploads survived because the Pi's ACKs are tiny.
 *
 * b43 never has this: b43_dma_rx walks from its cursor up to, but NOT INCLUDING, the engine's current
 * descriptor (op32_get_current_rxslot = RXSTATUS & RXDPTR / sizeof desc32), and dma_rx still waits
 * 5 x 2 us for a frame_len of 0. This keeps the poison walk (k20's lesson: the free-running engine can
 * lap, so pointers alone cannot say empty from full) and adds b43's boundary: an unpoisoned slot that
 * IS the engine's current descriptor is in progress -- leave it, unpoisoned, for the interrupt its
 * completion raises. The pointer can trail a completion by a moment, so it settles 5 x 2 us first.
 * One RXSTATUS read per frame (only for unpoisoned slots). */
static UInt32 gRxBeingWritten = 0;      /* slots left for later because the engine was writing them */
static UInt32 gRxBeingWrittenMoved = 0; /* ...where the pointer moved off within the settle */
static UInt32 gRxLenZeroSettled = 0;    /* frame_len read 0, then appeared within 5 x 2 us */
static UInt32 gRxLenZeroStill = 0;      /* ...and still 0 after it: copied whole, as through k199 */
static UInt32 gRxLapSeen = 0;           /* deferred while the NEXT slot held an unread frame: lapped */

/* ⚠ ONLY AN ACTIVE ENGINE IS WRITING. If it has STOPPED or been DISABLED (an error, a reset), RXDPTR
 *   is frozen -- possibly at a finished, unread slot -- and deferring to it would stall the ring for
 *   good ([[guards must not latch]]). This engine reads ACTIVE while waiting for frames too (good boots
 *   log RXSTATUS 0x06801060), so ACTIVE alone says nothing; ACTIVE *and* RXDPTR == slot is the test. */
static int RxSlotBeingWritten(UInt32 base,int slot)
{
    int tries;
    for(tries = 0; tries < 6; tries++){
        UInt32 st  = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
        UInt32 cur = (st & B43_DMA32_RXDPTR) / B43_DMADESC32_BYTES;
        if((st & B43_DMA32_RXSTATE) != B43_DMA32_RXSTAT_ACTIVE) return 0;   /* not writing anything */
        if((int)cur != slot){ if(tries) gRxBeingWrittenMoved++; return 0; }
        if(tries < 5) SsbSpinUs(2);
    }
    gRxBeingWritten++;
    return 1;
}

static int RxWaitFor(UInt32 base,UInt8 **bufLog,UInt32 frameoffset,
                     int wantType,int wantSub,
                     const UInt8 *fromAddr,const UInt8 *toAddr,
                     UInt32 timeoutMs,UInt8 *copyOut,UInt32 *msOut,int *seenOut)
{
    UInt32 waited = 0;
    int seen = 0;
    int i;
    if(msOut)   *msOut   = 0;
    if(seenOut) *seenOut = 0;

    for(i=0;i<4;i++)  gRxCensusType[i] = 0;
    for(i=0;i<16;i++) gRxCensusMgmt[i] = 0;
    gRxNearN      = 0;
    gRxSlotIn     = gRxSlot;
    /* ⚠ k200: THE ENTRY/EXIT CENSUS IS FOR THE JOIN'S WAITS ONLY (timeoutMs > 0: auth 500, assoc 800,
     * M1/M3 2000 -- the only readers, ap_shim.c's wait-failure diagnostics, print it after those).
     * The data path's pump calls with 0, once per frame and once more to find the ring empty, and
     * the census was two RXSTATUS reads plus two 32-slot poison scans -- ~520 loads from DMA memory,
     * ~95 us, on EVERY receive AND every TX-status interrupt (k199: 22438 secondary runs). */
    if(timeoutMs > 0){
      gRxStatusIn   = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
      gRxBacklogIn  = RxCountUnpoisoned(bufLog,frameoffset);
      gRxStatusOut  = gRxStatusIn;
      gRxBacklogOut = gRxBacklogIn; }

    while(waited <= timeoutMs){
      /* ⚠⚠ WALK BY POISON, NOT BY CURSOR -- and this is k20's lesson arriving a second time.
       *
       * This loop used to walk `while(gRxSlot != current)`, comparing our read cursor against
       * RXDPTR. RxRecycle, forty lines above, carries a header explaining at length why that
       * comparison cannot be trusted: the engine free-runs by design here (we hand back
       * K3_RX_SLOTS*8 rather than b43's slot*8, because k20 measured the faithful value
       * stopping the engine dead), so it LAPS the ring and comes back to the slot we left it
       * on. Two pointers that meet cannot say whether the ring is empty or FULL.
       *
       * RxRecycle was fixed for exactly this in k20 and walks the poison instead. RxWaitFor was
       * written later, as the seventh extraction, and reintroduced the cursor. It survived
       * because every earlier wait followed a transmit by a millisecond or two -- far too short
       * to lap 32 slots. 8-4 put 333 ms of PBKDF2 in front of one, and the lap became reachable:
       * the failure then followed the delay across k82 and k83, moving from the association to
       * the authentication when the PBKDF2 moved.
       *
       * The poison IS the has-data flag: we write it, only the engine's DMA clears it. Walking
       * forward while a slot is unpoisoned, bounded by one ring, cannot be fooled by a lap. */
      {
        int step;
        for(step=0; step<K3_RX_SLOTS; step++){
          UInt8 *buf = bufLog[gRxSlot];
          const UInt8 *f;
          UInt16 fc;
          int ty, sb, ok;
          if(RxBufferIsPoisoned(buf,frameoffset)) break;   /* nothing new here */
          /* ★ k201: and not while the engine is still writing it -- see RxSlotBeingWritten. BEFORE any
           * other look at the slot: the FCS-fail and non-matching arms below poison it too. */
          if(RxSlotBeingWritten(base,(int)gRxSlot)){
            /* the slot AFTER the engine's is normally empty; an unread frame there means the free-
             * running engine has lapped us (a full ring behind) -- a loss this rule cannot prevent,
             * counted so it cannot hide */
            if(!RxBufferIsPoisoned(bufLog[(gRxSlot + 1) % K3_RX_SLOTS],frameoffset)) gRxLapSeen++;
            AP_RXW_DEFERRED(gRxSlot);
            break; }
          AP_RXW_TAKEN(gRxSlot);             /* k202: every arm below consumes this slot */
          gRxWalkAnt[(le16at(buf + RXH_PHY_STATUS0) & B43_RX_PHYST0_ANT) ? 1 : 0]++;   /* k198 */
          /* ★ k197: an FCS failure (MACCTL KEEP_BAD) -- count it, step past it, never parse it. */
          if(RxMacStatus(buf) & B43_RX_MAC_FCSERR){
            RxFcsCount(buf,frameoffset,fromAddr);
            PoisonRxBuffer(buf,frameoffset);
            gRxSlot = (gRxSlot + 1) % K3_RX_SLOTS;
            continue; }
          f  = buf + frameoffset + K6_HDR_PLCP6;
          fc = le16at(f);
          ty = (int)((fc>>2)&3); sb = (int)((fc>>4)&0xF);
          ok = 1;
          seen++;
          gRxCensusType[ty]++;
          if(ty==0) gRxCensusMgmt[sb]++;
          if(wantType>=0 && ty!=wantType) ok = 0;
          if(wantSub >=0 && sb!=wantSub)  ok = 0;
          if(ok){
            /* ★ THE NEAR MISS. Right type, right subtype, rejected only on an address -- so
             * record WHO sent it before poisoning the slot. This is the one frame whose
             * identity a failed wait cannot afford to discard. */
            int addrOk = 1;
            if(fromAddr && !MacEq(f+10,fromAddr)) addrOk = 0;   /* addr2, the sender */
            if(toAddr   && !MacEq(f+4,toAddr))    addrOk = 0;   /* addr1, the target */
            if(!addrOk){
              if(gRxNearN < 4){
                int q;
                for(q=0;q<6;q++){ gRxNearFrom[gRxNearN][q] = f[10+q];
                                  gRxNearTo  [gRxNearN][q] = f[4+q]; }
                gRxNearN++; }
              ok = 0; } }
          if(ok){
            /* ★ k200: THE FRAME, NOT THE BUFFER, AND IN WORDS. Through k199 this copied all
             * AP_PROMPT_BUF (2400) bytes one load at a time whatever the frame's length -- ~2400
             * DMA-memory transactions, ~450 us of the 565 us k199 measured per frame. Now
             * frameoffset + frame_len + 8, as 32-bit words (buf is page-aligned, gPromptBuf aligned 16);
             * an unwritten or impossible frame_len still copies the whole buffer, so the pump's
             * TOO_BIG check and everything after it see exactly what they did. */
            if(copyOut){
              /* k201: b43's dma_rx wait -- a completed slot whose frame_len still reads 0 gets 5 x 2 us
               * to show it. Still 0 -> copy it whole, exactly as through k199 (the pump drops it). */
              UInt32 flen = (UInt32)le16at(buf+RXH_FRAME_LEN);
              if(flen == 0){
                int t;
                for(t = 0; t < 5 && flen == 0; t++){ SsbSpinUs(2); flen = (UInt32)le16at(buf+RXH_FRAME_LEN); }
                if(flen) gRxLenZeroSettled++; else gRxLenZeroStill++; }
              AP_RXW_COPY_BEGIN();
              ApCopyWords(copyOut,buf,ApRxCopyLen(frameoffset,(unsigned long)flen,(unsigned long)AP_PROMPT_BUF));
              AP_RXW_COPY_END(); }
            if(msOut)   *msOut   = waited;
            if(seenOut) *seenOut = seen;
            /* hand the consumed slots back before returning, or the engine starves exactly
             * the way k20's did -- see RxRecycle's header. */
            PoisonRxBuffer(buf,frameoffset);
            gRxSlot = (gRxSlot + 1) % K3_RX_SLOTS;
            if(timeoutMs > 0){                 /* k200: the join's census only -- see the entry */
              gRxStatusOut  = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
              gRxBacklogOut = RxCountUnpoisoned(bufLog,frameoffset); }
            DmaPublish();
            ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,
                    (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES);
            return 1; }
          PoisonRxBuffer(buf,frameoffset);
          gRxSlot = (gRxSlot + 1) % K3_RX_SLOTS; }
        /* ⚠ UNCONDITIONALLY, AND ON EVERY PASS -- which is what the old cursor version did, via
         * its `if(current in range)` arm that was true essentially always. Writing this only
         * when a slot was consumed would be a SECOND change riding along with the walk fix, and
         * the value is the one k20 measured as keeping the engine out of IDLEWAIT. One change
         * per run: the walk. The cadence stays exactly as it was. */
        DmaPublish();
        ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,
                (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES); }
      /* ★ k195: NO SPIN AFTER THE LAST PASS. This loop spun 1 ms after every pass, the last one
       * included, so a wait that ended empty cost one extra millisecond before returning. The join
       * never noticed (500-2000 ms waits). Stage 8's pump calls this with timeoutMs = 0 -- one pass
       * -- and ends EVERY interrupt with exactly that empty call, so every receive interrupt burnt a
       * millisecond at secondary-interrupt level, holding off OT's deferred task that delivers the
       * frame. A timed-out join wait now reports timeoutMs rather than timeoutMs + 1; the callers
       * only log it. */
      if(waited >= timeoutMs) break;
      SsbSpinUs(1000);
      waited++; }

    if(timeoutMs > 0){                         /* k200: the join's census only -- see the entry */
      gRxStatusOut  = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
      gRxBacklogOut = RxCountUnpoisoned(bufLog,frameoffset); }
    if(msOut)   *msOut   = waited;
    if(seenOut) *seenOut = seen;
    return 0;
}

#endif /* AP_WAIT_H */
