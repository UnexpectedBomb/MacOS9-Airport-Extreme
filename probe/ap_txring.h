/* ap_txring.h -- the TRANSMIT RING's bookkeeping (k195): which ring position a frame goes in, which
 * buffer it uses, what TXINDEX to write, and which positions the engine has finished with.
 *
 * Pure computation, no MMIO, no Toolbox: included by ap_shim.c for the driver AND by txring_test.c on
 * the host, so what the host suite proves is the code the card runs (the ap_rate.h / rate_test.c
 * pattern).
 *
 * ★ WHY IT EXISTS. Through k194 ApShimWriteFrame posted EVERY frame at descriptors 0/1, from ONE
 * header buffer and ONE frame buffer, after resetting the TX DMA engine (ApShimTxArm). That is the
 * join's idiom -- one frame, then wait for its status -- carried into the data path, and it cost:
 *   1. the NEXT frame was encrypted into the one buffer while the engine could still be reading the
 *      previous one, and the reset then aborted it. The k194 AFP capture: 252 of the Mac's ~3284 IP
 *      datagrams (7.7%) never reached the Pi, 220 of them single; 98 of the 252 have IP ID = 28 mod 32
 *      (uniform would be ~8), because otm_wput sent a diagnostic frame right after every 32nd frame.
 *   2. every reset ran RxRecycle, which POISONS every unread receive slot -- any frame waiting for
 *      the interrupt pump was discarded each time we transmitted.
 *   3. DmaControllerTxReset spins >= 1 ms (up to 3), per frame, at whatever level OT called us from.
 *
 * ★ THE SHAPE IS b43's (dma.c): a 256-descriptor ring (B43_TXRING_SLOTS, DTABLEEND on 255, which is
 * what Op32FillDescriptorFull already does for K8_TX_SLOTS), TWO descriptors per frame
 * (TX_SLOTS_PER_FRAME: header, then body), and op32_poke_tx writing TXINDEX = the descriptor AFTER
 * the frame's body. The engine is reset ONCE (dma_init) and never per frame.
 *
 * ★ WHERE WE DIFFER, ON PURPOSE. b43 frees a slot when the frame's TX STATUS comes back. We free it
 * when the ENGINE'S OWN POINTER (TXSTATUS & TXDPTR) has moved past both of its descriptors: once the
 * engine has read a descriptor's data into the card's FIFO the host buffer is no longer needed --
 * retries are sent from the FIFO -- and that is exactly the question buffer reuse asks. It also keeps
 * the ISR's status drain (ApShimDrainTxStatQuiet) out of the ring entirely: the ring has ONE owner,
 * ApShimWriteFrame, serialised by gTxBusy.
 *
 * ★ 128 POSITIONS, 32 BUFFERS. Position p uses buffer p % 32. A frame is posted only while at most 31
 * are in flight; those are the 31 positions just before head, which use 31 DIFFERENT buffers, none of
 * them head % 32 -- so the buffer being filled is never one the engine can still read. That makes 32
 * the most ever in flight. head == tail could only mean "full" at 128 in flight, which cannot happen,
 * so it always means EMPTY and the ring never has to tell the two apart from a pointer comparison
 * (k20's lesson, on the receive side, in ap_wait.h). txring_test.c checks the buffer property against
 * a model engine; a cap of 33 fails it.
 */
#ifndef AP_TXRING_H
#define AP_TXRING_H

#define AP_TXR_DESCS   256u                   /* == K8_TX_SLOTS == b43's B43_TXRING_SLOTS */
#define AP_TXR_POS     (AP_TXR_DESCS / 2u)    /* 128 frame positions (TX_SLOTS_PER_FRAME = 2) */
#define AP_TXR_BUFS    32u                    /* header+frame buffers; position p uses p % 32 */
#define AP_TXR_MAXOUT  AP_TXR_BUFS            /* post only while < 32 in flight: 32 at most */
#define AP_TXR_DESC_BYTES 8u                  /* sizeof(struct b43_dmadesc32) */

/* Each buffer is half a page: the txhdr at +0 (TXH_SIZE_351 is 106 or 110), the frame at +128. */
#define AP_TXR_BUFBYTES 2048u
#define AP_TXR_HDRMAX   128u
#define AP_TXR_FRMMAX   (AP_TXR_BUFBYTES - AP_TXR_HDRMAX)    /* 1920 >= 24+8+1508+8 = 1548 */

typedef struct {
    UInt32 head;   /* the next position to fill */
    UInt32 tail;   /* the oldest position the engine may still be reading */
    UInt32 used;   /* posted and not yet seen consumed; never more than AP_TXR_MAXOUT (32) */
} ApTxRing;

/* Start the ring at the position the ENGINE says it is on -- after a controller reset that should be
 * descriptor 0, but the ring takes the engine's word for it rather than assuming. An odd descriptor
 * (half-way through a frame) or one past the end is refused: 0 = not started. */
static int ApTxrStartAt(ApTxRing *r, UInt32 curDesc)
{
    if(curDesc >= AP_TXR_DESCS || (curDesc & 1u)) return 0;
    r->head = r->tail = curDesc / 2u;
    r->used = 0;
    return 1;
}

/* Positions from a forward to b. */
static UInt32 ApTxrDist(UInt32 a, UInt32 b) { return (b + AP_TXR_POS - a) % AP_TXR_POS; }

/* The engine is on descriptor curDesc: every in-flight position before the one that descriptor
 * belongs to is finished. A frame whose header OR body descriptor the engine is on is NOT finished
 * (curDesc / 2 is that frame). Returns how many positions were freed, or -1 when curDesc is not
 * inside the in-flight window -- a pointer we cannot explain frees NOTHING. */
static int ApTxrReclaim(ApTxRing *r, UInt32 curDesc)
{
    UInt32 curPos, n;
    if(curDesc >= AP_TXR_DESCS) return -1;
    curPos = curDesc / 2u;
    n = ApTxrDist(r->tail, curPos);
    if(n > r->used) return -1;
    r->tail = curPos;
    r->used -= n;
    return (int)n;
}

static int    ApTxrHasRoom(const ApTxRing *r) { return r->used < AP_TXR_MAXOUT; }
static UInt32 ApTxrBuf(UInt32 pos)            { return pos % AP_TXR_BUFS; }
static UInt32 ApTxrHdrDesc(UInt32 pos)        { return 2u * pos; }          /* body is +1 */
/* op32_poke_tx(ring, next_slot(ring, bodySlot)): the descriptor after the body, in BYTES. */
static UInt32 ApTxrTxIndex(UInt32 pos)        { return ((2u * pos + 2u) % AP_TXR_DESCS) * AP_TXR_DESC_BYTES; }

/* Commit the head position once its descriptors are written. The caller checked ApTxrHasRoom. */
static UInt32 ApTxrCommit(ApTxRing *r)
{
    UInt32 p = r->head;
    r->head = (r->head + 1u) % AP_TXR_POS;
    r->used++;
    return p;
}

#endif /* AP_TXRING_H */
