/* ap_otmodl.c -- OTModl$AirPortBCM: a NATIVE Open Transport DLPI Ethernet STREAMS module for the
 * BCM4306 AirPort card, replacing the USBEnet-shim path (EnetShimLib) that OT's IP would never
 * drain on our boot-present PCI card.
 *
 * ★★★ WHY. Proven 2026-09-25 (10+ HW runs + RE of Apple's binaries): our radio HAL (ap_shim.c)
 * associates/decrypts/TX/RX and a RAW OT client drains 100/100 unicast IP -- but OT's own TCP/IP
 * won't drain the port EnetShimInstallDriver registers, because that is the USB *runtime* path; a
 * boot-present PCI card must be a native module. Apple's AirPort is seamless because it IS one
 * (OTModl$radio: install_info -> streamtab -> read-srv delivers RX via allocb/canputnext/putnext;
 * write-put dispatches DLPI). This file is our own OTModl$, reusing ap_shim for all radio work.
 *
 * ⚠ RUNTIME LINKING: Retro68 has the OT headers but not the kernel link libs, so allocb/putnext/...
 *   are resolved once (InitStreamModule/qopen) via GetSharedLibrary("OTKernelLib")+FindSymbol.
 *
 * STATUS: DLPI logic implemented (DL_INFO_ACK/DL_BIND_ACK/DL_ERROR_ACK, UNITDATA TX/RX). Still to
 *   do before HW: the ApOtmHal* bridges in ap_shim.c, the CMake PEF target + cfrg "OTModl$AirPortBCM"
 *   + TheDriverDescription, and the OTScanPorts scanner (OTRegisterPort) that makes OT load us.
 */

#include <OpenTransportKernel.h>
#include <OpenTransportProtocol.h>
#include <CodeFragments.h>
#include <NameRegistry.h>
#include "ap_tbclock.h"   /* k199: the per-frame stopwatch; gApTbPerMs is defined in ap_shim.c */

/* Freshness witness for THIS part of the deliverable: the init beacon carries this number, so
 * reading airport-otmodl-init == AP_OTMODL_BUILD proves this exact module loaded (not a stale one).
 * [[feedback_freshness_witness_outside_the_test]] -- one witness per part of a multi-part ship. */
#define AP_OTMODL_BUILD  219

/* ★★★ 8-61: k163 SAFE DIAGNOSTIC. Hold RX delivery OFF -- count the whole path but do NOT hand
 * frames to OT, so there is no corruption and no crash. With the module stable, the boot vehicle's
 * snapshot pump can finally resolve it and dump the [8-60] counters. Answers the branching question:
 * does OT bind our stream (boundSAP) and does RX reach us (enq)? Flip to 1 once the delivery is fixed. */
/* 8-76 k182: RX BACK ON. k181 (RX off) COLD-booted survived cleanly -- but OT then did only INFO + the
 * 0x4f02 ioctl and never bound (DL_BIND_REQ 0), i.e. with the card not armed to receive OT won't proceed.
 * And the cold boot is what stopped the k181 bring-up MMIO crash (bar0+0x14) -- warm restart was the
 * culprit there. So re-enable RX and require a COLD boot: this run separates the two crash theories --
 * if OTStrLength returns on a COLD boot with RX on, the crash is genuinely our RX/interrupt path (rewrite
 * it exactly like Apple's OTModl$radio: ISR->OTScheduleDeferredTask direct, allocb/putq only in the DT,
 * putnext only in the read-srv under canputnext); if it stays clean, warm restart was the whole story. */
#define AP_OTM_RX_DELIVER 1
#define AP_OTM_BIND_ONLY  0

/* Radio HAL (thin wrappers to add in ap_shim.c over the existing ApShim* functions). */
extern int  ApOtmHalOpen(void);                      /* bring-up + join; 0 = ok */
extern void ApOtmHalClose(void);
extern int  ApOtmHalGetMac(unsigned char out[6]);    /* 0 = ok */
extern int  ApOtmHalTx(const unsigned char *frame, unsigned long len);  /* full Ethernet frame */

#define AP_MAC_LEN    6
#define AP_SAP_LEN    2
#define AP_DLSAP_LEN  8                 /* T8022: 6-byte MAC + 2-byte SAP */
#define AP_ETH_HDR    14                /* dst(6)+src(6)+ethertype(2) */

/* ============================================================================================
 * Runtime-resolved OT/STREAMS primitives.
 * ==========================================================================================*/
typedef Ptr      (*allocb_t)(long size, unsigned long pri);
typedef void     (*freemsg_t)(mblk_t *mp);
typedef Boolean  (*canputnext_t)(queue *q);
typedef void     (*putnext_t)(queue *q, mblk_t *mp);
typedef mblk_t * (*getq_t)(queue *q);
typedef Boolean  (*putq_t)(queue *q, mblk_t *mp);
typedef Boolean  (*putbq_t)(queue *q, mblk_t *mp);       /* k191: back-off requeue -- see otm_rsrv */
typedef void     (*qreply_t)(queue *q, mblk_t *mp);
typedef void     (*qenable_t)(queue *q);                  /* k202: wake our write service routine */

typedef long     (*createDT_t)(void *proc, void *arg);   /* OTCreateDeferredTask (OTKernelUtilLib) */
typedef Boolean  (*scheduleDT_t)(long cookie);           /* OTScheduleDeferredTask */
typedef UInt32   (*mkPortRef_t)(UInt8 bus, UInt16 dev, UInt16 slot, UInt16 other); /* OTCreatePortRef */
typedef OSStatus (*chgPortState_t)(UInt32 ref, UInt32 event, OSStatus result);     /* OTChangePortState */
typedef void *   (*findPortByDev_t)(UInt32 dev);   /* 8-80 k186: OTFindPortByDev (OTKernelLib) */

static struct {
    int          ok;
    allocb_t     allocb;
    freemsg_t    freemsg;
    canputnext_t canputnext;
    putnext_t    putnext;
    getq_t       getq;
    putq_t       putq;
    putbq_t      putbq;         /* k191: Apple's OTModl$radio imports it too (measured) */
    qreply_t     qreply;
    qenable_t    qenable;       /* k202: OPTIONAL -- without it, a full TX ring drops as through k201 */
    createDT_t   createDT;      /* 8-59: deferred-task RX delivery -- allocb/putnext are NOT */
    scheduleDT_t scheduleDT;    /* interrupt-safe; the ISR only enqueues+schedules, this drains */
    mkPortRef_t    mkPortRef;   /* 8-63: OTCreatePortRef -- recompute our port ref (matches ap_boot) */
    chgPortState_t chgPortState;/* 8-63: OTChangePortState -- announce kOTPortOnline, like OTModl$radio */
    findPortByDev_t findPortByDev; /* 8-80 k186: OTFindPortByDev -- get OT's internal port record + its ref */
} gOT;

static unsigned char gMyMac[AP_MAC_LEN];

/* ★★★ 9-29 k190: PER-STREAM DLPI STATE. The old comment here said "single card => a single context is
 * fine" -- but one CARD is not one STREAM. TCP/IP over Ethernet opens SEVERAL streams on the port at
 * once (IP binds SAP 0x0800, ARP binds 0x0806). k189 run 2 proved it from the driver's own counters:
 * qopen count was already 2 at the FIRST qclose, so two streams were open together. Through k189 the
 * module kept ONE gReadQ/gState/gBoundSap: every qopen overwrote the read queue, every bind overwrote the
 * SAP, all 930 received frames went up whichever stream opened LAST (the final bind was ARP, 0x0806),
 * any qclose cut receive off for BOTH, and doInfoReq told a fresh stream it was already bound -- the
 * exact INFO_ACK mis-advertisement k183 showed makes OT refuse to bind. Apple's OTModl$radio qopen
 * "allocates per-stream state" (k177 RE); we never did.
 *
 * Each open stream now owns a slot. qopen stores slot+1 in BOTH queues' q_ptr (the partner found with
 * OTHERQ -- the pair is adjacent, WR(q) = &q[1], OpenTransportKernel.h:87-93), so wput/qclose find their
 * own stream. Receive is
 * routed by protocol: an Ethernet-II frame (type >= 0x600) goes up every BOUND stream whose SAP equals
 * its type; an 802.3 frame (length < 0x600) goes up streams bound to an 802.2 SAP (< 0x600).
 * Plain parallel arrays (not a struct) so ap_shim.c's snapshot can read them with simple externs.
 * gState/gBoundSap survive only as "last touched" MIRRORS for the diag frame + snapshot. */
#define AP_MAX_STREAMS 8
static queue *gStrRq[AP_MAX_STREAMS];            /* this stream's READ queue; 0 = slot free */
UInt32 gStrOpen [AP_MAX_STREAMS];                /* 1 while the stream is open (snapshot) */
UInt32 gStrSap  [AP_MAX_STREAMS];                /* bound SAP, 0 until DL_BIND_REQ */
UInt32 gStrState[AP_MAX_STREAMS];                /* DL_UNBOUND / DL_IDLE */
UInt32 gStrRxUp [AP_MAX_STREAMS];                /* frames delivered up THIS stream */
UInt32 gStrQd   [AP_MAX_STREAMS];                /* k191: frames queued on this stream, not yet passed up */
static unsigned long gStrPutqTb[AP_MAX_STREAMS]; /* k199: timebase of the oldest putq the read-srv has not
                                                  * yet run for (0 = none waiting) */
/* ★★★ k202: TRANSMIT BACK-PRESSURE. Through k201 doUnitdataReq did `(void)ApOtmHalTx(frame, n)` and
 * freed the message: a full TX ring DROPPED the frame, silently. k201's capture: a 25 ms upload burst
 * reached the Pi with a dozen holes (TX ring full 23x); OT's TCP (Reno, no SACK) fast-retransmitted
 * one and waited a 699 ms RTO for the next -- the upload warm-up crawled at 160 KB/s. mac80211/b43
 * never drop for a full ring: they stop the queue until TX completions free slots. The STREAMS form of
 * that: a refused frame stays on OUR write queue (putq; putbq from otm_wsrv), q_count above mi_hiwat
 * (16 KB) makes canputnext fail upstream so IP/TCP hold their data, and each TX completion (seen in
 * the secondary) schedules the deferred task, which qenables the waiting streams. */
static queue *gStrWq[AP_MAX_STREAMS];            /* this stream's WRITE queue, recorded when it waits */
static int    gStrTxWait[AP_MAX_STREAMS];        /* 1 = frames wait on this stream's write queue */
volatile UInt32 gOtmTxWaiting = 0;               /* any stream waiting -- the secondary reads it (extern) */
UInt32 gOtmTxQueued = 0;     /* frames kept on a write queue because the ring was full (were dropped) */
UInt32 gOtmTxWsrv = 0;       /* otm_wsrv runs */
UInt32 gOtmTxQenable = 0;    /* wakeups issued by the deferred task */
UInt32 gOtmTxQDrop = 0;      /* dropped anyway: the write queue was past AP_TXQ_CAP_BYTES */
UInt32 gOtmTxDropped = 0;    /* permanent refusals (not associated, no key, bad frame) -- dropped */
UInt32 gOtmTxQMaxBytes = 0;  /* deepest write queue seen, bytes */
#define AP_TXQ_CAP_BYTES 262144UL   /* a stalled radio must not drain OT's memory: 16x the high-water mark */
/* k199: OT's side of the per-frame cost -- non-static, ap_shim.c's snapshot prints them */
ApTbHist gTbRxQWait;                             /* ISR enqueue -> the deferred task picks the frame up */
ApTbHist gTbDtRun;                               /* one deferred-task run */
ApTbHist gTbRsrvWait;                            /* putq -> otm_rsrv runs for that stream */
UInt32 gOtmRxQFull = 0;                          /* k191: frames dropped because a stream's queue was at its cap */
#define AP_RXQ_PER_STREAM_MAX 64                 /* k191: ~96 KB of 1500-byte frames per stream, then drop */
UInt32 gStrMax = AP_MAX_STREAMS;                 /* the snapshot loops to THIS, never a copied constant */
UInt32 gOtmNoStream = 0;                         /* received frames no bound stream wanted (dropped) */
UInt32 gOtmNoStreamType = 0;                     /* ethertype/length of the last such frame */
static UInt32        gBoundSap;                  /* MIRROR: last SAP bound on any stream */
static UInt32        gState = DL_UNBOUND;        /* MIRROR: last state set on any stream */

/* The slot<->queue link rides in q_ptr, so pin OT's queue layout the way module_info is pinned: if
 * q_ptr's offset or the pair stride ever differed from OT's, WR(q) and q_ptr would silently point at
 * the wrong bytes. All fields before q_ptr are pointer-sized, and the one short packs with two chars,
 * so mac68k and power agree -- these asserts keep it that way. */
_Static_assert(__builtin_offsetof(queue, q_ptr) == 20, "queue.q_ptr must sit at +20 (OT layout)");
_Static_assert(sizeof(queue) == 64, "queue must be 64 bytes so WR(q) = &q[1] matches OT's pair");
_Static_assert(__builtin_offsetof(queue, q_flag) == 48, "queue.q_flag must sit at +48 (OTHERQ reads QREADR)");

static int apSlotOf(queue *q)                    /* -1 if q carries no stream */
{
    long s = q ? (long)q->q_ptr : 0;
    return (s >= 1 && s <= AP_MAX_STREAMS) ? (int)(s - 1) : -1;
}

/* ★★★ 8-60: BLACK-BOX RECORDER (non-static so ap_shim.c's ApOtmSnapshot can read them). Records
 * exactly where the RX path stops and what OT does, dumped to the driver log by the boot vehicle's
 * NM pump. enq -> defer -> inj -> rsrv/putnext is the RX chain; a break tells us where. */
UInt32 gApOtmodlBuild = AP_OTMODL_BUILD;   /* 8-63: freshness witness, stamped into the HAL log by ap_shim */
UInt32 gOtmLastType = 0, gOtmLastPrim = 0xFFFFFFFFUL;  /* 8-66 k170: last wput db_type + DLPI primitive */
/* k178: identify the gate. gOtmLastOtherPrim = the last DLPI primitive we did NOT handle (the "other
 * req"); the ioctl ring records {cmd,in,rval} of the last few M_IOCTLs so the reliable qclose snapshot
 * names exactly what OT sent (the lossy 0xD1A6 diag only kept the LAST ioc_cmd). gFraming/gMacType
 * model Apple's OTModl$radio 0x4f02 framing state (context[0x20]/[0x1c]). */
UInt32 gOtmLastOtherPrim = 0xFFFFFFFFUL;
UInt32 gFraming = 0;
UInt32 gMacType = DL_ETHER;
#define AP_IOCRING 6
UInt32 gIocCmd[AP_IOCRING], gIocIn[AP_IOCRING], gIocRval[AP_IOCRING], gIocRingN = 0;
/* k179: ground truth for the post-bind stall. gBind* = the DL_BIND_REQ OT actually sent (explains the
 * odd bound SAP); gEvtRing = the ORDER of every wput event ('P'|prim, 'IC'|ioctl-cmd, 'D'=M_DATA). */
UInt32 gBindMaxConind = 0xFFFFFFFFUL, gBindSvcMode = 0xFFFFFFFFUL;
#define AP_EVTRING 16
UInt32 gEvtRing[AP_EVTRING], gEvtRingN = 0;
UInt32 gOtmIoctls = 0, gOtmLastIoc = 0;                /* 8-67 k171: M_IOCTL count + last ioc_cmd */
UInt32 gOtmLastIocCount = 0;                            /* 8-68 k172: ioc_count (0xFFFFFFFF=transparent) */
UInt32 gOtmLastIocArg = 0;                              /* 8-72 k176: 4-byte value OT passed in 0x4f02, echoed in ioc_rval */
UInt32 gOtmEnq = 0, gOtmDefer = 0, gOtmInj = 0, gOtmInjDrop = 0;
UInt32 gOtmRsrv = 0, gOtmPutnext = 0, gOtmCanputF = 0;
UInt32 gOtmInfoReqs = 0, gOtmBindReqs = 0, gOtmUdataReqs = 0, gOtmOtherReqs = 0;
UInt32 gOtmBoundSap = 0, gOtmStateSnap = 0, gOtmQDepthMax = 0;
/* 8-80 k186: OT's internal port ref from OTFindPortByDev(dev) in qopen. Apple uses port->0x2c as the
 * OTChangePortState ref; we used a recomputed OTCreatePortRef fRef. Discriminator: log both. */
UInt32 gPortRefFound = 0, gPortRefFromFind = 0, gPortRefFromCreate = 0, gPortFlag24 = 0;
static void ApOtmAnnounceOnline(void);   /* 8-80 k186: now called from qopen; defined below */

/* 8-82 k188: TheDriverDescription + ValidateHardware are defined in ap_ndrv.h (included by ap_shim.c,
 * same fragment) with the correct 'mtej' / driverRuntime 0x04 / 'otan' 0x000A0B01 values. Through k187
 * they were never EXPORTED and ValidateHardware deliberately declined; both are fixed now (see ap_ndrv.h
 * and ap_otmodl.exp) so OT installs OTModl$AirPortBCM into its module registry via the OTKrnl$ scanner. */

/* ★ 8-58 WITNESS. A Name Registry property is outside every CFM copy, so it answers "did OT load
 * and open OUR module" unambiguously -- the same idiom ap_boot.c's Beacon uses. Written at task
 * level (InitStreamModule and qopen are both task time). Milestone A = airport-otmodl-qopen appears. */
static void apBeacon(const char *prop, UInt32 value)
{
    RegEntryID root;
    if (RegistryCStrEntryLookup(NULL, "Devices:device-tree", &root) != noErr) return;
    if (RegistryPropertyCreate(&root, (char *)prop, (void *)&value,
                               (RegPropertyValueSize)sizeof(value)) != noErr)
        (void)RegistryPropertySet(&root, (char *)prop, (void *)&value,
                                  (RegPropertyValueSize)sizeof(value));
    (void)RegistryEntryIDDispose(&root);
}

static void apMemcpy(void *d, const void *s, unsigned long n)
{ unsigned char *dp=d; const unsigned char *sp=s; while(n--) *dp++=*sp++; }
static void apMemset(void *d, int v, unsigned long n)
{ unsigned char *dp=d; while(n--) *dp++=(unsigned char)v; }

static int ResolveOne(CFragConnectionID cid, const char *sym, void **out)
{
    Str255 p; int i = 0; CFragSymbolClass cls; Ptr addr = 0;
    while (sym[i] && i < 254) { p[i+1] = (unsigned char)sym[i]; i++; }
    p[0] = (unsigned char)i;
    if (FindSymbol(cid, p, &addr, &cls) != noErr || cls != kTVectorCFragSymbol) return 0;
    *out = (void *)addr; return 1;
}

static int ResolveOTPrimitives(void)
{
    CFragConnectionID cid; Ptr a; Str255 em; int ok = 1;
    if (gOT.ok) return 1;
    if (GetSharedLibrary("\pOTKernelLib", kPowerPCCFragArch, kLoadCFrag, &cid, &a, em) != noErr)
        return 0;
    ok &= ResolveOne(cid, "allocb",     (void **)&gOT.allocb);
    ok &= ResolveOne(cid, "freemsg",    (void **)&gOT.freemsg);
    ok &= ResolveOne(cid, "canputnext", (void **)&gOT.canputnext);
    ok &= ResolveOne(cid, "putnext",    (void **)&gOT.putnext);
    ok &= ResolveOne(cid, "getq",       (void **)&gOT.getq);
    ok &= ResolveOne(cid, "putq",       (void **)&gOT.putq);
    ok &= ResolveOne(cid, "putbq",      (void **)&gOT.putbq);   /* k191: required -- otm_rsrv's back-off */
    ok &= ResolveOne(cid, "qreply",     (void **)&gOT.qreply);
    /* k202: optional. OTKernelLib exports it (Apple's OTModl$radio imports it -- FEASIBILITY.md's
     * import table); if it ever fails to resolve, apTxOrQueue falls back to k201's drop. */
    (void)ResolveOne(cid, "qenable",    (void **)&gOT.qenable);
    /* 8-80 k186: OTFindPortByDev (OTKernelLib). Non-fatal: absent -> fall back to the recomputed
     * OTCreatePortRef ref. This is the ref OT keys port-state/config on (OTModl$radio uses port->0x2c
     * from here); ours recomputed a different fRef, likely why OT's config for our port is malformed. */
    (void)ResolveOne(cid, "OTFindPortByDev", (void **)&gOT.findPortByDev);
    /* 8-59: the deferred-task pair lives in OTKernelUtilLib (measured off OTModl$radio's imports),
     * NOT OTKernelLib. Resolve it from a second connection. If this fails we degrade to no RX
     * delivery rather than crash -- ApOtmScheduleRx no-ops when scheduleDT is null. */
    { CFragConnectionID uid; Ptr ua; Str255 ue;
      if (GetSharedLibrary("\pOTKernelUtilLib", kPowerPCCFragArch, kLoadCFrag, &uid, &ua, ue) == noErr) {
        ok &= ResolveOne(uid, "OTCreateDeferredTask",   (void **)&gOT.createDT);
        ok &= ResolveOne(uid, "OTScheduleDeferredTask", (void **)&gOT.scheduleDT);
        /* 8-63: link-up primitives, resolved from the SAME library. Not folded into `ok` -- if OT
         * lacks them we degrade (no online announce) rather than fail every RX primitive. */
        (void)ResolveOne(uid, "OTCreatePortRef",   (void **)&gOT.mkPortRef);
        (void)ResolveOne(uid, "OTChangePortState", (void **)&gOT.chgPortState);
      } else ok = 0; }
    gOT.ok = ok;
    return ok;
}

/* Allocate a reply message of `size` bytes, db_type set, b_wptr advanced, zeroed. NULL on fail. */
static mblk_t *apAllocMsg(long size, unsigned char dbtype)
{
    mblk_t *mp = (mblk_t *)gOT.allocb(size, 0);
    if (!mp) return 0;
    mp->b_datap->db_type = dbtype;
    apMemset(mp->b_rptr, 0, (unsigned long)size);
    mp->b_wptr = mp->b_rptr + size;
    return mp;
}

/* ============================================================================================
 * read-srv: deliver received frames UP to OT's IP (modeled on OTModl$radio read-srv).
 * ==========================================================================================*/
/* ★★★ 9-29 k191: THE FREEZE ON THE FIRST REAL TCP TRAFFIC. k190 got the first DHCP lease ever
 * (192.168.1.225); the first AFP connect to the Pi then froze the machine, and so did the next boot at
 * the desktop. The back-pressure branch below requeued with putq. putq appends at the TAIL *and
 * re-enables this queue*, so the service routine was rescheduled at once, found upstream still full
 * (nothing had had a chance to drain), requeued, rescheduled... a livelock in the STREAMS scheduler.
 * The branch only runs when canputnext fails -- which never happened before k190, because until then
 * every frame went up the ARP stream, which discards non-ARP instantly (every snapshot: canputnext=NO
 * 0). k190 routed real IP to the IP stream; the first sustained transfer filled it.
 * putbq is the STREAMS back-off: it puts the frame back at the HEAD (order kept) and does NOT
 * re-enable -- canputnext's failure already asked upstream to back-enable us once it drains. Apple's
 * OTModl$radio imports putbq (measured off its import table). */
static OTInt32 otm_rsrv(queue *q)
{
    mblk_t *mp;
    int s = apSlotOf(q);
    gOtmRsrv++;
    if (s >= 0 && gStrPutqTb[s]) {               /* k199: how long OT took to schedule us */
        ApTbHistAdd(&gTbRsrvWait, ApTbNow() - gStrPutqTb[s]);
        gStrPutqTb[s] = 0;
    }
    while ((mp = gOT.getq(q)) != 0) {
        if (mp->b_datap->db_type == M_CTL) { gOT.freemsg(mp); continue; }
        if (gOT.canputnext(q)) {
            gOT.putnext(q, mp);                 /* -> OT's IP */
            gOtmPutnext++;
            if (s >= 0 && gStrQd[s]) gStrQd[s]--;
        } else {
            gOtmCanputF++;
            gOT.putbq(q, mp);                   /* k191: back-off -- HEAD, no self-reschedule (was putq) */
            break;
        }
    }
    return 0;
}

/* ---- DLPI request handlers (on the write queue; reply via qreply) ---- */

static void doErrorAck(queue *q, UInt32 badPrim, UInt32 errno_)
{
    mblk_t *mp = apAllocMsg(sizeof(dl_error_ack_t), M_PCPROTO);
    dl_error_ack_t *e;
    if (!mp) return;
    e = (dl_error_ack_t *)mp->b_rptr;
    e->dl_primitive       = DL_ERROR_ACK;
    e->dl_error_primitive = badPrim;
    e->dl_errno           = errno_;
    e->dl_unix_errno      = 0;
    gOT.qreply(q, mp);
}

static void doInfoReq(queue *q)
{
    /* 8-77 k183: Apple-exact DL_INFO_ACK (RE'd OTModl$radio handler 0x80009ba0). addr_length/sap_length
     * VARY BY BIND STATE -- UNBOUND -> 6/0 (MAC only, NO SAP); BOUND Ethernet-II -> 8/-2 (MAC + 2-byte
     * SAP). k182 (cold, 8/-2-always) proved OT will NOT bind on a fresh cold boot when we advertise a
     * SAP while unbound. (k179 tried this and "crashed", but that was the WARM-RESTART OTStrLength
     * crash, not this change -- cold boots don't crash, so it's safe and is the bind gate.)
     * dl_addr_offset=76=sizeof; broadcast follows the addr; apAllocMsg zeroes QoS/reserved/growth.
     * k190: answer with THIS stream's state and SAP -- through k189 every stream got the global, so a
     * freshly opened stream was told it was already bound to another stream's SAP. */
    int    s     = apSlotOf(q);
    UInt32 st    = (s >= 0) ? gStrState[s] : DL_UNBOUND;
    UInt32 ssap  = (s >= 0) ? gStrSap[s]   : 0;
    int    bound = (st == DL_IDLE);
    long alen  = bound ? AP_DLSAP_LEN : AP_MAC_LEN;  /* 8 bound, 6 unbound */
    long sz    = (long)(sizeof(dl_info_ack_t) + alen + AP_MAC_LEN);
    mblk_t *mp = apAllocMsg(sz, M_PCPROTO);
    dl_info_ack_t *a;
    unsigned char *addr, *bcst;
    if (!mp) return;
    a = (dl_info_ack_t *)mp->b_rptr;
    a->dl_primitive         = DL_INFO_ACK;
    a->dl_max_sdu           = 1500;
    a->dl_min_sdu           = 0;
    a->dl_addr_length       = (UInt32)alen;
    a->dl_mac_type          = gMacType;
    a->dl_current_state     = st;
    a->dl_sap_length        = bound ? -AP_SAP_LEN : 0;   /* -2 bound (SAP after addr), 0 unbound */
    a->dl_service_mode      = DL_CLDLS;              /* connectionless */
    a->dl_provider_style    = DL_STYLE1;            /* single card = implicit PPA */
    a->dl_version           = DL_VERSION_2;
    a->dl_addr_offset       = sizeof(dl_info_ack_t);
    a->dl_brdcst_addr_length= AP_MAC_LEN;
    a->dl_brdcst_addr_offset= (UInt32)(sizeof(dl_info_ack_t) + alen);
    addr = mp->b_rptr + sizeof(dl_info_ack_t);
    apMemcpy(addr, gMyMac, AP_MAC_LEN);
    if (bound) { addr[6] = (unsigned char)(ssap >> 8); addr[7] = (unsigned char)ssap; }
    bcst = mp->b_rptr + sizeof(dl_info_ack_t) + alen;
    apMemset(bcst, 0xff, AP_MAC_LEN);
    gOT.qreply(q, mp);
}

static void doBindReq(queue *q, mblk_t *in)
{
    dl_bind_req_t *req = (dl_bind_req_t *)in->b_rptr;
    UInt32 sap = req->dl_sap;
    long sz = (long)(sizeof(dl_bind_ack_t) + AP_DLSAP_LEN);
    mblk_t *mp; dl_bind_ack_t *a; unsigned char *addr;
    int s = apSlotOf(q);
    if (s >= 0) { gStrSap[s] = sap; gStrState[s] = DL_IDLE; }   /* k190: bind THIS stream only */
    gBoundSap = sap; gState = DL_IDLE; gOtmBoundSap = sap;       /* mirrors for the diag/snapshot */
    gBindMaxConind = req->dl_max_conind; gBindSvcMode = (UInt32)req->dl_service_mode;  /* k179: capture */
    mp = apAllocMsg(sz, M_PCPROTO);
    if (!mp) return;
    a = (dl_bind_ack_t *)mp->b_rptr;
    a->dl_primitive   = DL_BIND_ACK;
    a->dl_sap         = sap;
    a->dl_addr_length = AP_DLSAP_LEN;
    a->dl_addr_offset = sizeof(dl_bind_ack_t);
    a->dl_max_conind  = 0;
    a->dl_xidtest_flg = 0;
    addr = mp->b_rptr + sizeof(dl_bind_ack_t);
    apMemcpy(addr, gMyMac, AP_MAC_LEN);
    addr[6] = (unsigned char)(sap >> 8); addr[7] = (unsigned char)sap;
    gOT.qreply(q, mp);
}

/* 8-73 k178: the standard DLPI primitives Apple's OTModl$radio answers that we did not -- OT sends
 * one of these after bind and our DL_ERROR_ACK (DL_UNSUPPORTED) may be why it stops. Model Apple:
 * DL_OK_ACK for the state/filter primitives, DL_PHYS_ADDR_ACK (our MAC) for DL_PHYS_ADDR_REQ. */
static void doOkAck(queue *q, UInt32 prim)
{
    mblk_t *mp = apAllocMsg((long)sizeof(dl_ok_ack_t), M_PCPROTO);
    dl_ok_ack_t *a;
    if (!mp) return;
    a = (dl_ok_ack_t *)mp->b_rptr;
    a->dl_primitive         = DL_OK_ACK;
    a->dl_correct_primitive = prim;
    gOT.qreply(q, mp);
}
static void doPhysAddrReq(queue *q)
{
    mblk_t *mp = apAllocMsg((long)(sizeof(dl_phys_addr_ack_t) + AP_MAC_LEN), M_PCPROTO);
    dl_phys_addr_ack_t *a; unsigned char *addr;
    if (!mp) return;
    a = (dl_phys_addr_ack_t *)mp->b_rptr;
    a->dl_primitive   = DL_PHYS_ADDR_ACK;
    a->dl_addr_length = AP_MAC_LEN;
    a->dl_addr_offset = sizeof(dl_phys_addr_ack_t);
    addr = mp->b_rptr + sizeof(dl_phys_addr_ack_t);
    apMemcpy(addr, gMyMac, AP_MAC_LEN);              /* both DL_FACT and DL_CURR -> our SPROM MAC */
    gOT.qreply(q, mp);
}

/* DL_UNITDATA_REQ: M_PROTO (dest DLSAP addr) + M_DATA chain (the SDU). Build the Ethernet frame
 * [dst MAC][our MAC][ethertype=SAP][SDU] and hand it to the radio. */
/* Returns what happened to `in`: AP_TX_SENT (freed), AP_TX_DROPPED (freed, counted), or AP_TX_LATER --
 * the ring is full, `in` is NOT freed, and the caller keeps it on the write queue. */
#define AP_TX_SENT     0
#define AP_TX_LATER    1
#define AP_TX_DROPPED -1
static int doUnitdataReq(queue *q, mblk_t *in)
{
    static unsigned char frame[1600];
    dl_unitdata_req_t *req = (dl_unitdata_req_t *)in->b_rptr;
    unsigned char *dst = in->b_rptr + req->dl_dest_addr_offset;
    unsigned long sap = ((unsigned long)dst[6] << 8) | dst[7];
    unsigned long n = AP_ETH_HDR;
    mblk_t *d;
    int r;
    (void)q;
    apMemcpy(frame, dst, AP_MAC_LEN);                 /* dest MAC */
    apMemcpy(frame + 6, gMyMac, AP_MAC_LEN);          /* src MAC  */
    frame[12] = (unsigned char)(sap >> 8); frame[13] = (unsigned char)sap;  /* ethertype */
    for (d = in->b_cont; d; d = d->b_cont) {          /* gather the SDU */
        unsigned long seg = (unsigned long)(d->b_wptr - d->b_rptr);
        if (n + seg > sizeof(frame)) break;
        apMemcpy(frame + n, d->b_rptr, seg); n += seg;
    }
    /* ⛔ k202: THE RETURN VALUE IS THE POINT. It was cast away here through k201 -- CLAUDE.md: never
     *   discard what hardware says about whether it did what it was told. 1 = the ring is full: keep
     *   the message, it goes out when a TX completion frees a slot. */
    r = ApOtmHalTx(frame, n);
    if (r == 1) return AP_TX_LATER;
    if (r != 0) gOtmTxDropped++;
    gOT.freemsg(in);
    return (r == 0) ? AP_TX_SENT : AP_TX_DROPPED;
}

/* k202: park a refused frame on its stream's write queue and mark the stream as waiting. */
static void apQueueTx(queue *q, mblk_t *mp)
{
    int s = apSlotOf(q);
    if (q->q_count > AP_TXQ_CAP_BYTES) { gOtmTxQDrop++; gOT.freemsg(mp); return; }
    if (s >= 0) { gStrWq[s] = q; gStrTxWait[s] = 1; }
    gOtmTxWaiting = 1;
    gOtmTxQueued++;
    gOT.putq(q, mp);           /* q_count past mi_hiwat -> canputnext fails upstream -> IP/TCP hold data */
    if (q->q_count > gOtmTxQMaxBytes) gOtmTxQMaxBytes = q->q_count;
}

/* k202: transmit now, or keep it. ORDER: once anything waits on this queue, everything behind it waits
 * too -- a frame must never overtake one OT handed us first. */
static void apTxOrQueue(queue *q, mblk_t *mp)
{
    if (gOT.qenable && q->q_first) { apQueueTx(q, mp); return; }
    if (doUnitdataReq(q, mp) == AP_TX_LATER) {
        if (gOT.qenable) apQueueTx(q, mp);
        else { gOtmTxDropped++; gOT.freemsg(mp); }     /* no wakeup available: k201's behaviour */
    }
}

/* ★ k202: THE WRITE SERVICE ROUTINE. OT runs it after putq (first message) and after our qenable (a TX
 * completion freed ring slots). Sends until the ring refuses, then putbq -- HEAD, order kept, and no
 * self-reschedule ([[reference_os9_ot_streams_putbq_backoff]]): the next TX completion's wakeup is what
 * runs it again, so a still-full ring cannot livelock it. */
static OTInt32 otm_wsrv(queue *q)
{
    mblk_t *mp;
    int s = apSlotOf(q);
    gOtmTxWsrv++;
    while ((mp = gOT.getq(q)) != 0) {
        if (doUnitdataReq(q, mp) == AP_TX_LATER) {
            if (s >= 0) { gStrWq[s] = q; gStrTxWait[s] = 1; }
            gOtmTxWaiting = 1;
            gOT.putbq(q, mp);
            break;
        }
    }
    return 0;
}

/* 8-68 k172: CAPTURE the ioctl, do NOT answer it yet. k171 answered M_IOCTL with M_IOCNAK and OT
 * FROZE (livelock) -- rejecting it is worse than ignoring it (k170 merely stalled, stayed responsive).
 * So OT REQUIRES this ioctl; we must ANSWER it correctly, which means first knowing which one it is.
 * Put ioc_cmd + ioc_count on the wire NOW (5x, before anything that can hang), then DROP the message
 * (k170's responsive behavior). k173 will ACK it properly once the wire names it. */
static void ApOtmTxDiag(UInt32 seq);   /* fwd decl -- defined below */
static void doIoctl(queue *q, mblk_t *mp)
{
    struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
    gOtmIoctls++;
    gOtmLastIoc      = (UInt32)ioc->ioc_cmd;
    gOtmLastIocCount = (UInt32)ioc->ioc_count;
    gOtmLastIocArg   = 0;
    /* 8-72 k176: MODEL Apple's OTModl$radio ioctl handler. k175 proved definitively: OT sends ONE
     * M_IOCTL (0x4f02), we ACK'd it (k174), but OT still did NOT bind -- a bare ACK is not enough.
     * Apple's 0x4f02/0x4f03 handler reads a 4-byte value passed INLINE in b_cont and ECHOES it back
     * in ioc_rval before ACKing (iocblk+0x14). k174 returned ioc_rval=0, so OT saw the value unhonored
     * and refused DL_BIND_REQ. Echo it now. (These are in-kernel ioctls: the data is inline in b_cont,
     * not a user pointer, so no M_COPYIN is needed -- Apple reads *b_rptr directly, and so do we.) */
    ioc->ioc_error = 0;
    ioc->ioc_rval  = 0;
    {
        UInt32 cmd = (UInt32)ioc->ioc_cmd, inVal = 0xFFFFFFFFUL, slot;
        if (mp->b_cont && (unsigned long)(mp->b_cont->b_wptr - mp->b_cont->b_rptr) >= 4)
            inVal = *(UInt32 *)mp->b_cont->b_rptr;
        /* 8-73 k178: model Apple's OTModl$radio ioctl handler (0x8000b020) exactly. 0x4f02 is get/set
         * framing: value 0xFFFFFFFF is a QUERY (return the STORED framing, do NOT store); any other
         * value is a SET (store it; mac_type = 0 if value==8 else DL_ETHER). ioc_rval is ALWAYS the
         * driver's CURRENT framing, NEVER the raw input -- k176 echoed the input, which hands OT
         * 0xFFFFFFFF back on a query and it reads that as a garbage framing type. */
        if (cmd == 0x4f02) {
            if (inVal != 0xFFFFFFFFUL) { gFraming = inVal; gMacType = (inVal == 8) ? 0 : DL_ETHER; }
            ioc->ioc_rval = (SInt32)gFraming;
        } else if (cmd == 0x4f03) {
            ioc->ioc_rval = (SInt32)((inVal != 0xFFFFFFFFUL) ? inVal : gFraming);
        }
        gOtmLastIocArg = (UInt32)ioc->ioc_rval;
        /* record the ioctl so the reliable qclose snapshot names exactly what OT sent (0x6c0a? 0x4f03?) */
        slot = gIocRingN % AP_IOCRING;
        gIocCmd[slot] = cmd; gIocIn[slot] = inVal; gIocRval[slot] = (UInt32)ioc->ioc_rval; gIocRingN++;
    }
    if (mp->b_cont) { gOT.freemsg(mp->b_cont); mp->b_cont = 0; }
    ioc->ioc_count = 0;
    mp->b_datap->db_type = M_IOCACK;   /* ACK all (never NAK -- k171: NAK of a required ioctl froze OT) */
    gOT.qreply(q, mp);
}

/* 8-65 k169: SAFE counter visibility. k168 crashed because ApOtmSnapshot does File Manager I/O, and
 * that is illegal from an OT STREAMS put procedure (qopen is an open routine and may; wput may NOT --
 * CurrentExecutionLevel()==0 is necessary but not sufficient in OT's put context). TRANSMITTING is
 * proven safe from wput (the XID went out exactly this way), so pack the DLPI/RX counters into a
 * broadcast frame (ethertype 0xD1A6) and read them off the wire capture. No File Manager, no NR. */
static void ApOtmPutBE32(unsigned char *d, UInt32 v)
{ d[0]=(unsigned char)(v>>24); d[1]=(unsigned char)(v>>16); d[2]=(unsigned char)(v>>8); d[3]=(unsigned char)v; }
static void ApOtmTxDiag(UInt32 seq)
{
    static unsigned char f[80];
    apMemset(f, 0xff, 6); apMemcpy(f + 6, gMyMac, 6);
    f[12] = 0xD1; f[13] = 0xA6;                       /* ethertype: diagnostic (not real traffic) */
    ApOtmPutBE32(f + 14, 0x41506467UL);              /* 'APdg' magic */
    ApOtmPutBE32(f + 18, seq);
    ApOtmPutBE32(f + 22, gOtmInfoReqs);
    ApOtmPutBE32(f + 26, gOtmBindReqs);              /* did OT bind now? */
    ApOtmPutBE32(f + 30, gOtmBoundSap);
    ApOtmPutBE32(f + 34, gOtmUdataReqs);
    ApOtmPutBE32(f + 38, gOtmOtherReqs);
    ApOtmPutBE32(f + 42, gOtmIoctls);                /* 8-67 k171: M_IOCTL count */
    ApOtmPutBE32(f + 46, gOtmLastIoc);               /* 8-67 k171: last ioc_cmd (which ioctl OT sent) */
    ApOtmPutBE32(f + 50, gOtmLastType);
    ApOtmPutBE32(f + 54, gOtmLastPrim);
    ApOtmPutBE32(f + 58, gOtmLastIocCount);          /* 8-68 k172: ioc_count (0xFFFFFFFF=transparent) */
    ApOtmPutBE32(f + 62, gState);
    (void)ApOtmHalTx(f, 66);
}

static OTInt32 otm_wput(queue *q, mblk_t *mp)
{
    static UInt32 sSnapN = 0;
    unsigned char t = mp->b_datap->db_type;
    gOtmStateSnap = gState;                         /* 8-60: keep the snapshot's state fresh */
    gOtmLastType = t;                               /* 8-66 k170: capture what OT actually sends */
    {   /* 8-74 k179: record the event ORDER so the qclose snapshot shows the whole OT<->driver
         * conversation (does OT go idle right after BIND, or re-INFO first?). */
        UInt32 ev;
        if (t == M_IOCTL)                        ev = 0x49430000UL | ((UInt32)((struct iocblk *)mp->b_rptr)->ioc_cmd & 0xFFFFUL);
        else if (t == M_PROTO || t == M_PCPROTO) ev = 0x50000000UL | (*(UInt32 *)mp->b_rptr & 0xFFFFUL);
        else if (t == M_DATA)                    ev = 0x44000000UL;
        else                                     ev = 0x3F000000UL | (UInt32)t;
        gEvtRing[gEvtRingN % AP_EVTRING] = ev; gEvtRingN++;
    }
    if (t == M_PROTO || t == M_PCPROTO) {
        UInt32 prim = *(UInt32 *)mp->b_rptr;
        gOtmLastPrim = prim;
        switch (prim) {
            case DL_INFO_REQ:      gOtmInfoReqs++;  doInfoReq(q);            gOT.freemsg(mp); break;
            case DL_BIND_REQ:      gOtmBindReqs++;  doBindReq(q, mp);        gOT.freemsg(mp); break;
            case DL_UNITDATA_REQ:  gOtmUdataReqs++; apTxOrQueue(q, mp);      break;  /* k202: sends, keeps or frees */
            /* 8-73 k178: primitives Apple's OTModl$radio handles (were falling through to DL_UNSUPPORTED) */
            case DL_UNBIND_REQ:  { int s = apSlotOf(q);                     /* k190: this stream only */
                                   if (s >= 0) { gStrState[s] = DL_UNBOUND; gStrSap[s] = 0; } }
                                   gState = DL_UNBOUND; gBoundSap = 0; doOkAck(q, prim); gOT.freemsg(mp); break;
            case DL_PHYS_ADDR_REQ: doPhysAddrReq(q);                        gOT.freemsg(mp); break;
            case DL_ENABMULTI_REQ:
            case DL_DISABMULTI_REQ:
            case DL_PROMISCON_REQ:
            case DL_PROMISCOFF_REQ: doOkAck(q, prim);                       gOT.freemsg(mp); break;
            default:               gOtmOtherReqs++; gOtmLastOtherPrim = prim; doErrorAck(q, prim, DL_UNSUPPORTED); gOT.freemsg(mp); break;
        }
    } else if (t == M_DATA) { gOtmUdataReqs++; apTxOrQueue(q, mp); }   /* never seen: OT sends DL_UNITDATA_REQ */
    else if (t == M_IOCTL) { doIoctl(q, mp); }     /* 8-67 k171: reply (qreply's mp itself) */
    else { gOtmOtherReqs++; gOT.freemsg(mp); }
    /* 8-65 k169: report the counters by TRANSMITTING them (safe from wput). Read the 0xD1A6 frames off
     * the wire: bound SAP, DL_UD_REQ(TX) count, RX enqueue/inject/putnext.
     * ★ k195: the FIRST 8 only. k169 also sent one after every 32nd wput, and on k194's one-buffer
     *   transmitter that frame was encrypted over the frame OT had just handed us, before the card
     *   had read it: in the k194 AFP capture 98 of the 252 datagrams the Mac lost have IP ID = 28
     *   mod 32. The ring makes the overwrite impossible, but the frame is still a broadcast on the
     *   user's network every 32 packets for a diagnostic the qclose snapshot replaced long ago. */
    sSnapN++;
    if (sSnapN <= 8) ApOtmTxDiag(sSnapN);
    return 0;
}

/* ============================================================================================
 * RX inject: ap_shim's receive path calls this with a decrypted full Ethernet frame; we wrap it
 * as DL_UNITDATA_IND (M_PROTO addresses + M_DATA SDU) and putq it for otm_rsrv to deliver up.
 * ==========================================================================================*/
/* The DL_UNITDATA_IND for one received frame -- M_PROTO (dest + src DLSAP) + M_DATA (the SDU). This is
 * k189's builder moved out unchanged, so the delivered format stays byte-identical; k190 only changes
 * WHICH stream receives it. 0 if OT could not allocate. */
static mblk_t *apUnitdataInd(const unsigned char *frame, unsigned long len)
{
    mblk_t *mp, *dat;
    dl_unitdata_ind_t *ind;
    unsigned char *da, *sa;
    unsigned long payload = len - AP_ETH_HDR;
    mp = apAllocMsg((long)(sizeof(dl_unitdata_ind_t) + 2 * AP_DLSAP_LEN), M_PROTO);
    if (!mp) return 0;
    ind = (dl_unitdata_ind_t *)mp->b_rptr;
    ind->dl_primitive        = DL_UNITDATA_IND;
    ind->dl_dest_addr_length = AP_DLSAP_LEN;
    ind->dl_dest_addr_offset = sizeof(dl_unitdata_ind_t);
    ind->dl_src_addr_length  = AP_DLSAP_LEN;
    ind->dl_src_addr_offset  = sizeof(dl_unitdata_ind_t) + AP_DLSAP_LEN;
    ind->dl_group_address    = (frame[0] & 0x01) ? 1 : 0;
    da = mp->b_rptr + sizeof(dl_unitdata_ind_t);
    apMemcpy(da, frame, AP_MAC_LEN);               /* dest MAC */
    da[6] = frame[12]; da[7] = frame[13];          /* SAP = ethertype */
    sa = da + AP_DLSAP_LEN;
    apMemcpy(sa, frame + 6, AP_MAC_LEN);           /* src MAC */
    sa[6] = frame[12]; sa[7] = frame[13];
    dat = (mblk_t *)gOT.allocb((long)payload, 0);
    if (!dat) { gOT.freemsg(mp); return 0; }
    dat->b_datap->db_type = M_DATA;
    apMemcpy(dat->b_wptr, frame + AP_ETH_HDR, payload);
    dat->b_wptr += payload;
    mp->b_cont = dat;
    return mp;
}

/* k190: deliver a received frame up EVERY bound stream that asked for its protocol. Through k189 the
 * frame went up the single global read queue -- the last stream opened -- whatever its protocol.
 * 8-66 k170's rule is kept per stream: DL_UNITDATA_IND goes only to a BOUND stream (DL_IDLE). */
void ApOtmRxInject(const unsigned char *frame, unsigned long len)
{
    UInt32 etype;
    int    i, wanted = 0;
    if (!gOT.ok || len < AP_ETH_HDR) { gOtmInjDrop++; return; }
    etype = ((UInt32)frame[12] << 8) | frame[13];   /* Ethernet-II type, or an 802.3 length */
    for (i = 0; i < AP_MAX_STREAMS; i++) {
        queue *rq  = gStrRq[i];                      /* read once: qclose clears state, then rq */
        UInt32 sap = gStrSap[i];
        if (!rq || gStrState[i] != DL_IDLE) continue;
        if (etype >= 0x0600UL ? (sap != etype) : (sap >= 0x0600UL)) continue;
        wanted++;
#if !AP_OTM_RX_DELIVER
        /* 8-61: diagnostic -- count the frame we WOULD deliver, but do not touch OT (no crash). */
        gOtmInj++; gStrRxUp[i]++;
#else
        {   mblk_t *mp;
            /* k191: cap what waits on a stream. If OT stops draining (upstream stalled), keep dropping
             * the newest -- TCP recovers from loss -- instead of allocating until OT's pool is gone. */
            if (gStrQd[i] >= AP_RXQ_PER_STREAM_MAX) { gOtmRxQFull++; continue; }
            mp = apUnitdataInd(frame, len);
            if (!mp) { gOtmInjDrop++; continue; }
            gOT.putq(rq, mp);                        /* enabled read queue -> otm_rsrv */
            if (!gStrPutqTb[i]) gStrPutqTb[i] = ApTbNow();   /* k199: the oldest frame waiting */
            gOtmInj++; gStrRxUp[i]++; gStrQd[i]++;
        }
#endif /* AP_OTM_RX_DELIVER */
    }
    if (!wanted) { gOtmNoStream++; gOtmNoStreamType = etype; }
}

/* ============================================================================================
 * ★★★ 8-59: DEFERRED-TASK RX DELIVERY. THE k160 CRASH FIX.
 *
 * k160 called ApOtmRxInject (allocb/putq) straight from the secondary-interrupt handler and OS 9
 * face-planted in OTStrLength on a corrupted pool: OT's STREAMS primitives are NOT safe at
 * interrupt level. Apple's own OTModl$radio proves the right shape -- it imports
 * OTCreateDeferredTask/OTScheduleDeferredTask and does allocb/putnext from the deferred task, not
 * the ISR. So: the ISR only ENQUEUES the already-converted Ethernet frame (a bounded memcpy, no OT
 * calls) and SCHEDULES the deferred task; the deferred task drains the ring UP the DLPI stream at a
 * safe level.
 * ==========================================================================================*/
/* k195: 64 slots, was 24. The secondary now drains the whole receive ring (up to K3_RX_SLOTS = 32)
 * per interrupt instead of 4, and a second interrupt can land before the deferred task runs, so 24
 * would drop inside one burst. gRxQDrops (exported for the snapshot) says whether 64 is enough. */
#define AP_RXQ_SLOTS 64
#define AP_RXQ_BUFSZ 1600
static unsigned char   gRxQ[AP_RXQ_SLOTS][AP_RXQ_BUFSZ];
static unsigned short  gRxQLen[AP_RXQ_SLOTS];
static unsigned long   gRxQTb[AP_RXQ_SLOTS];   /* k199: timebase when the ISR enqueued the slot */
static volatile UInt32 gRxQWr = 0;     /* producer: the ISR, via ApOtmRxEnqueue */
static volatile UInt32 gRxQRd = 0;     /* consumer: ApOtmRxDeferred */
UInt32                 gRxQDrops = 0;  /* k195: non-static -- ApOtmSnapshot prints it */
static UInt32          gRxDeferRuns = 0;
static long            gRxDtCookie = 0;

/* Called FROM the ISR (ap_shim's pump). Interrupt-safe: a bounded memcpy into the ring, NO OT
 * calls. Single producer (the non-re-entrant secondary handler), single consumer, volatile
 * indices; publish the slot only after the copy completes. Drops when the ring is full. */
void ApOtmRxEnqueue(const unsigned char *frame, unsigned long len)
{
    UInt32 depth = gRxQWr - gRxQRd, s;
    gOtmEnq++;
    if (depth + 1 > gOtmQDepthMax) gOtmQDepthMax = depth + 1;
    if (depth >= AP_RXQ_SLOTS) { gRxQDrops++; return; }
    if (len > AP_RXQ_BUFSZ) len = AP_RXQ_BUFSZ;
    s = gRxQWr % AP_RXQ_SLOTS;
    apMemcpy(gRxQ[s], frame, len);
    gRxQLen[s] = (unsigned short)len;
    gRxQTb[s]  = ApTbNow();                 /* k199: stamped before the slot is published */
    gRxQWr++;
}

/* Called FROM the ISR after enqueuing. OTScheduleDeferredTask is the one OT call that IS
 * interrupt-safe (it just posts the task). No-op until the cookie exists. */
void ApOtmScheduleRx(void)
{
    if (gOT.scheduleDT && gRxDtCookie) (void)(*gOT.scheduleDT)(gRxDtCookie);
}

/* The deferred task. Runs at OT deferred-task level, where allocb/putq ARE safe. Drains every
 * queued frame up the DLPI stream. */
static void ApOtmRxDeferred(void *arg)
{
    unsigned long tbDt = ApTbNow();         /* k199 */
    (void)arg;
    gRxDeferRuns++; gOtmDefer++;
    while (gRxQRd != gRxQWr) {
        UInt32 s = gRxQRd % AP_RXQ_SLOTS;
        ApTbHistAdd(&gTbRxQWait, ApTbNow() - gRxQTb[s]);   /* k199: ISR enqueue -> picked up here */
        ApOtmRxInject(gRxQ[s], gRxQLen[s]);
        gRxQRd++;
    }
    /* ★ k202: THE TRANSMIT WAKEUP. The secondary schedules this task whenever a stream is waiting (a
     * TX completion may have freed ring slots); qenable is safe HERE, at deferred-task level, not in the
     * secondary. Cleared BEFORE the qenables: a wsrv that finds the ring still full sets it again, and
     * the next interrupt (a TX completion, or at worst a beacon ~100 ms on) brings us back. */
    if (gOtmTxWaiting && gOT.qenable) {
        int i;
        gOtmTxWaiting = 0;
        for (i = 0; i < AP_MAX_STREAMS; i++)
            if (gStrTxWait[i] && gStrWq[i] && gStrRq[i]) {
                gStrTxWait[i] = 0;
                gOT.qenable(gStrWq[i]);
                gOtmTxQenable++;
            }
    }
    ApTbHistAdd(&gTbDtRun, ApTbNow() - tbDt);
}

/* Create the deferred task once, at task level (qopen), before the ISR is armed. */
static void ApOtmSetupRx(void)
{
    if (!gRxDtCookie && gOT.createDT)
        gRxDtCookie = (*gOT.createDT)((void *)ApOtmRxDeferred, 0);
}

/* ============================================================================================
 * Stream open/close.
 * ==========================================================================================*/
UInt32 gQopenCount = 0;    /* 8-81 k187: non-static so the qclose snapshot can report it */
UInt32 gQcloseCount = 0;   /* 8-81 k187: pairs with gQopenCount -- lets us tell enumeration (open/close, no bind)
                            * from a real activation attempt. If OT only ever opens ONCE per boot, it enumerates
                            * our port and never comes back to activate it; >1 means it re-opened to bind. */
static int    gHalUp      = 0;   /* k177: set by InitStreamModule's bring-up; qopen checks it */

static OTInt32 otm_qopen(queue *q, dev_t *dev, OTInt32 flag, OTInt32 sflag, cred_t *cred)
{
    (void)flag; (void)sflag; (void)cred;   /* 8-80 k186: dev is now used (OTFindPortByDev) */
    /* ★ MILESTONE A: OT opened OUR stream. k177: THIS ROUTINE IS NOW LIGHTWEIGHT, matching Apple's
     * OTModl$radio qopen (disassembled: 62 instructions, zero loops -- OTFindPortByDev + allocate
     * per-stream state + lazy timer; NO MMIO, delay, firmware, join or interrupt install). The full
     * ~20s bring-up + WPA2 join + ISR arm moved to InitStreamModule, because Apple does hardware in
     * InitStreamModule/ValidateHardware, NEVER on the STREAMS open path. Doing it here blocked OT's
     * async open-endpoint thread for 20s; OT then crashed in OTStrLength while completing the open
     * (k160/k166/k176 were all the same OpenEndpoint->OTStrLength fault). Beacon FIRST regardless. */
    gQopenCount++;
    apBeacon("airport-otmodl-qopen", 0x6F510000UL | (gQopenCount & 0xFFFF));
    if (!gOT.ok && !ResolveOTPrimitives()) { apBeacon("airport-otmodl-qopen", 0x6F51E001UL); return -1; }
    if (!gHalUp) { apBeacon("airport-otmodl-qopen", 0x6F51E002UL); return -1; }  /* radio not up at init */
    /* k190: THIS stream gets its own slot (k189 overwrote one global read queue on every open, and set
     * it before the gHalUp check -- a failed open left it pointing at a queue OT was tearing down). */
    {   int s;
        for (s = 0; s < AP_MAX_STREAMS && gStrRq[s]; s++) ;
        if (s == AP_MAX_STREAMS) { apBeacon("airport-otmodl-qopen", 0x6F51E003UL); return -1; }  /* table full */
        gStrSap[s] = 0; gStrState[s] = DL_UNBOUND; gStrRxUp[s] = 0; gStrOpen[s] = 1; gStrQd[s] = 0;
        gStrPutqTb[s] = 0;                            /* k199 */
        gStrWq[s] = 0; gStrTxWait[s] = 0;             /* k202 */
        q->q_ptr = OTHERQ(q)->q_ptr = (char *)(long)(s + 1); /* wput/qclose find their stream from either queue.
                                                              * OTHERQ, not WR: it checks QREADR, so it is right
                                                              * whichever half of the pair OT hands us. */
        gStrRq[s] = q;                 /* publish last -- RX may flow once THIS stream binds (DL_IDLE) */
    }
    gState = DL_UNBOUND;               /* mirror */
    /* ★★★ 8-80 k186: capture OT's OWN port ref the way OTModl$radio's qopen does. OTFindPortByDev(dev)
     * returns OT's internal port record; port->0x2c is the ref OT keys port-state (and its config for
     * the port) on, and port->0x24 |= 0x10 marks the port as having an open provider stream. We had
     * been announcing ONLINE with a RECOMPUTED OTCreatePortRef fRef (a different field) at InitStreamModule
     * -- so OT never saw our port go up on the ref it watches. Now: capture the ref here, then announce. */
    if (gOT.findPortByDev && dev) {
        void *portp = (*gOT.findPortByDev)((UInt32)*dev);
        if (portp) {
            gPortFlag24      = ((volatile UInt32 *)portp)[0x24/4];   /* read BEFORE, for the discriminator */
            ((volatile UInt32 *)portp)[0x24/4] |= 0x10UL;            /* mark: a provider stream is open */
            gPortRefFromFind = ((volatile UInt32 *)portp)[0x2c/4];   /* the ref OT actually wants */
            gPortRefFound    = 1;
        }
    }
    ApOtmAnnounceOnline();          /* announce ONLINE + network-change here, post-open, with OT's own ref */
    apBeacon("airport-otmodl-qopen", 0x6F510000UL | 0x8000UL | (gQopenCount & 0xFFF)); /* light-open ok */
    return 0;
}

static OTInt32 otm_qclose(queue *q, OTInt32 flag, cred_t *cred)
{
    extern void ApOtmSnapshot(void);   /* ap_shim: reliable File-Manager dump of the DLPI counters */
    (void)flag; (void)cred;
    gQcloseCount++;                    /* 8-81 k187: count closes to read the open/close lifecycle */
    /* 8-71 k175: qclose is a STREAMS close routine (task level, like qopen) -- File Manager is SAFE
     * here, unlike wput. Dump the full counter set BEFORE resetting, so switching the TCP/IP port after
     * a test writes the whole OT<->driver conversation (ioctls, bind, boundSAP, udata) to the driver
     * log reliably -- the lossy 0xD1A6 TX diag can't be trusted on a marginal join. */
    ApOtmSnapshot();                   /* before release, so it still shows this stream */
    {   int s = apSlotOf(q);           /* k190: release THIS stream only -- the others keep receiving */
        if (s >= 0) {
            /* k202: FIRST, so the deferred task's transmit wakeup (which can preempt us here) can never
             * qenable a queue that is closing -- it also checks gStrRq, cleared next, belt and braces. */
            gStrTxWait[s] = 0; gStrWq[s] = 0;
            gStrState[s] = DL_UNBOUND;     /* first: the RX router skips it from here on */
            gStrRq[s]    = 0;
            gStrSap[s]   = 0; gStrOpen[s] = 0; gStrQd[s] = 0;   /* OT flushes the queue on close */
            gStrPutqTb[s] = 0;                                   /* k199 */
        }
        q->q_ptr = OTHERQ(q)->q_ptr = 0;   /* OTHERQ: WR() on a write queue would hit the NEXT stream */
    }
    gState = DL_UNBOUND; gBoundSap = 0;   /* mirrors */
    /* k177: do NOT quiesce the radio here. Apple's qclose is lightweight; the hardware stays up for
     * the module's lifetime and is torn down in TerminateStreamModule. Quiescing per stream-close
     * would also force the ~20s bring-up to re-run on the next open. */
    return 0;
}

/* ============================================================================================
 * The module contract Open Transport reads.
 * ==========================================================================================*/
/* ★★★ 9-29 k189: ROOT CAUSE of the OpenEndpoint->OTStrLength crash -- a module_info struct ALIGNMENT
 * mismatch, proven three ways (runtime dump ConfigDump2 + the OT header's own comment + the toolchain).
 *
 * OpenTransportProtocol.h force-aligns `struct module_info` to `power` with `#pragma options align=power`
 * -- a CodeWarrior/MPW pragma that Retro68's gcc SILENTLY IGNORES (it is the ONLY struct in the whole OT
 * header set given that treatment; the header comment even apologises for needing it). Under gcc's mac68k
 * default the `char* mi_idname` after `unsigned short mi_idnum` packs at offset +0x02, and
 * sizeof(module_info)==22. Apple's OT, built with MPW, reads mi_idname at +0x04 -- so it reads
 * (low16 of our name pointer | high16 of mi_minpsz==0) = 0xXXXX0000, a wild pointer, and OTStrLength
 * walks it and dies. The garbage varied every crash (D78C/FABC/AB0C/FA9C 0000) because it is our PEF's
 * per-boot load address; the low 16 bits were ALWAYS 0000 because that half is mi_minpsz. The crash dump
 * showed the module_info at R5 as `424D <pad> <nameLo>0000 | 0000 0000 | 05EA(1514) | ...4000 | ...1000`
 * -- i.e. the longs at +0x0A/+0x0E/+0x12: mac68k packing, byte for byte.
 *
 * FIX: hand OT a struct with the EXPLICIT pad the power pragma would have inserted, so mi_idname sits at
 * +0x04 and the four longs at +0x08/+0x0C/+0x10/+0x14 (sizeof 24) -- byte-for-byte the layout OT reads --
 * independent of the compiler's alignment mode. The _Static_asserts turn a future toolchain regression
 * into a BUILD failure, not a hardware boot. Field VALUES are unchanged from the k176 baseline so this
 * build isolates the single variable (alignment); Apple's radio sizing {60,1500,0x8000,..} stays a later
 * parity step and never touched this crash. See reference_os9_usb_ddk_struct_alignment (one pad byte...). */
typedef struct {
    unsigned short  mi_idnum;
    unsigned short  mi_pad;     /* the pad `#pragma options align=power` inserts; gcc won't, so we do */
    char *          mi_idname;
    long            mi_minpsz;
    long            mi_maxpsz;
    unsigned long   mi_hiwat;
    unsigned long   mi_lowat;
} ap_ot_module_info;
_Static_assert(__builtin_offsetof(ap_ot_module_info, mi_idname) == 4,  "mi_idname must sit at +4 (OT power ABI)");
_Static_assert(__builtin_offsetof(ap_ot_module_info, mi_maxpsz) == 12, "mi_maxpsz must sit at +12 (OT power ABI)");
_Static_assert(sizeof(ap_ot_module_info) == 24, "module_info must be 24 bytes (OT power ABI)");
static ap_ot_module_info gModInfo = { 0x424d, 0, "AirPortBCM", 0, 1514, 0x4000, 0x1000 };
static qinit gRdInit = { 0,        otm_rsrv, otm_qopen, otm_qclose, 0, (module_info*)&gModInfo, 0 };
static qinit gWrInit = { otm_wput, otm_wsrv, otm_qopen, otm_qclose, 0, (module_info*)&gModInfo, 0 };   /* k202: + wsrv */
static streamtab gStreamtab = { &gRdInit, &gWrInit, 0, 0 };

static install_info gInstallInfo = {
    &gStreamtab,
    kOTModIsDriver | kOTModUpperIsDLPI | kOTModUsesInterrupts,   /* 0x08002001, per OTModl$radio */
    3,                          /* install_sqlvl */
    0, 0, 0
};

/* ★★★ 8-63 k166: ANNOUNCE THE PORT ONLINE, exactly as Apple's OTModl$radio does.
 * RE of the shipping AirPort Driver (OTModl$radio) shows .Reset calls
 *     OTChangePortState(fRef, kOTPortOnline=0x25000004, 0)
 * after bring-up (and kOTPortOffline on teardown; .TimerTick pushes ongoing link changes). Our native
 * module registered the port (ap_boot) but NEVER told OT it came online -- so when TCP/IP activates the
 * port on Save, OT operates on a port stuck in its initial state and dies in OTSMOpenStream->OTStrLength.
 * fRef is recomputed with the SAME recipe ap_boot.c registered with, so OT finds the same port. */
#define AP_kOTPortOnline         0x25000004UL   /* OpenTransport.h kOTPortOnline */
#define AP_kOTPortNetworkChange  0x25000007UL   /* 8-79 k185: OTModl$radio's .TimerTick link-state push */
UInt32 gOtmOnlineResult = 0;             /* 0=uncalled; 0xDEAD0000=unresolved; 0xAA55|rr=called */
UInt32 gOtmNetChangeResult = 0;          /* 8-79 k185: 0xBB66|rr = link-up NETWORK-CHANGE pushed */
static void ApOtmAnnounceOnline(void)
{
    UInt32 ref; OSStatus rr;
    if (!gOT.chgPortState) { gOtmOnlineResult = 0xDEAD0000UL; apBeacon("airport-otmodl-online", gOtmOnlineResult); return; }
    /* 8-80 k186: prefer OT's OWN ref (port->0x2c, captured in qopen via OTFindPortByDev) over the
     * recomputed OTCreatePortRef fRef. Record both -- if they differ, the recompute was the bug. */
    if (gOT.mkPortRef) gPortRefFromCreate = (*gOT.mkPortRef)(3, 10, 0x0E, 0);
    ref = gPortRefFound ? gPortRefFromFind : gPortRefFromCreate;
    rr  = (*gOT.chgPortState)(ref, AP_kOTPortOnline, 0);
    gOtmOnlineResult = 0xAA550000UL | ((UInt32)(rr) & 0xFFFFUL);
    apBeacon("airport-otmodl-online", gOtmOnlineResult);
    /* 8-79 k185: model OTModl$radio's .TimerTick. We only ever announced the port ONLINE once; Apple
     * ALSO pushes a NETWORK-CHANGE port-state when the link comes up. OT probes our port (DL_INFO_REQ +
     * 0x4f02) but never binds or sends DHCP -- consistent with OT not knowing the link is actually
     * connected. After a successful join, tell OT the network state changed so it brings IP up. */
    if (gHalUp) {
        OSStatus rr2 = (*gOT.chgPortState)(ref, AP_kOTPortNetworkChange, 0);
        gOtmNetChangeResult = 0xBB660000UL | ((UInt32)(rr2) & 0xFFFFUL);
        apBeacon("airport-otmodl-netchg", gOtmNetChangeResult);
    }
}

/* k215: push a NETWORK-CHANGE to OT after a later join to a DIFFERENT network, so OT renews its DHCP lease for
 * the network we just switched to. ApOtmAnnounceOnline (above) does this once at boot/open; Apple's OTModl$radio
 * ALSO does it from .TimerTick on every link change. Without it, switching networks left OT holding the first
 * network's IP, so the second network had no LAN (the k214 "no LAN access after a network switch" report). Task level
 * only -- ap_shim.c calls it from the link-watch task after a network-changing rejoin, the same context Apple's
 * timer task uses. Uses OT's own captured port ref, exactly as the boot announce does. */
UInt32 gOtmNetChangeSwitches = 0;           /* how many times a network switch pushed the change */
void ApOtmNotifyNetworkChange(void)
{
    UInt32 ref;
    OSStatus rr;
    if (!gOT.chgPortState) return;          /* OT primitive never resolved: nothing to push */
    ref = gPortRefFound ? gPortRefFromFind : gPortRefFromCreate;
    rr  = (*gOT.chgPortState)(ref, AP_kOTPortNetworkChange, 0);
    gOtmNetChangeResult = 0xBB660000UL | ((UInt32)(rr) & 0xFFFFUL);
    gOtmNetChangeSwitches++;
    apBeacon("airport-otmodl-netchg", gOtmNetChangeResult);
}

install_info *GetOTInstallInfo(void)      { apBeacon("airport-otmodl-getinfo", 0x67494E46UL); return &gInstallInfo; }  /* 'gINF' -- OT read our install_info */
Boolean InitStreamModule(void *pi)
{
    (void)pi;
    apBeacon("airport-otmodl-init", (UInt32)AP_OTMODL_BUILD);   /* value = build # (freshness) */
    if (!ResolveOTPrimitives()) return false;
    /* ★★★ k177: APPLE'S ARCHITECTURE (disassembled from OTModl$radio). Apple does hardware bring-up
     * at module init (PrepFirmware/LoadMyMacAdrs) and ValidateHardware(main:FindCard) -- NEVER in the
     * STREAMS qopen, which is a 62-instruction connect. We were doing the whole ~20s bring-up+join+arm
     * in qopen, on OT's async open-endpoint thread, and OT crashed in OTStrLength completing that open
     * (k160/k166/k176). Do bring-up HERE, once, before OT ever opens the stream. */
    (void)ApOtmHalGetMac(gMyMac);          /* maps the card + reads the SPROM MAC */
#if !AP_OTM_BIND_ONLY
    ApOtmSetupRx();                        /* create the deferred RX task (task level) before the arm */
#endif
    gHalUp = (ApOtmHalOpen() == 0);        /* bring-up + WPA2 join + arm the ISR, ONCE */
    apBeacon("airport-otmodl-init", gHalUp ? 0x494E4B01UL : 0x494E4B00UL);  /* 'INK' | halUp? */
    ApOtmAnnounceOnline();                 /* announce the port ONLINE only after the radio is up */
    return true;                           /* the port/module exists regardless; qopen reports the link */
}
void TerminateStreamModule(void)
{
    apBeacon("airport-otmodl-term", 0x5445524DUL);   /* 'TERM' */
    ApOtmHalClose();                       /* k177: tear the radio down at module unload, not stream close */
}
