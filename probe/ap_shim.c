/* ap_shim.c -- STAGE 8-5b: the AirPort Ethernet Shim driver, first increment.
 *
 * ★ WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT.
 *
 * k52 established that `EnetShimLib` is present on the target and exports all three of its entry
 * points. That settles the Stage 8 architecture: we do NOT write a STREAMS/DLPI driver. We export
 * ONE function, `EnetHAL_Entry`, dispatch eleven selectors, and Apple's shim provides all of
 * STREAMS and DLPI on top of it.
 *
 * This increment answers exactly one question: **will the shim accept a registration from a
 * fragment that is neither a USB class driver nor a PCI-matched ndrv, and will it then call us?**
 * Everything below records what happened and returns an honest answer. NO RADIO CODE. The card is
 * not touched, no BAR is mapped, no firmware is loaded. A driver that talks to hardware before it
 * knows the OS will talk to IT is a driver debugging two things at once.
 *
 * ── WHY A SEPARATE FRAGMENT AND NOT CODE IN THE PROBE APP ──
 *
 * `InstallShimDrvr` in Apple's sample is commented "CFrag Connection ID (mine)": the shim takes a
 * connection to the DRIVER'S OWN fragment and FindSymbols `EnetHAL_Entry` inside it. So the code
 * OT calls must live in a fragment with a lifetime the OS controls, not in an application heap.
 * That is also why this is the right thing to build before the interrupt handler: the same
 * residency argument that made an app-resident ISR a hard-hang risk applies here, and a shared
 * library is the container that answers it.
 *
 * ── THE ENTRY POINT CONTRACT, confirmed three ways ──
 *   usb-ddk/Examples/USBEnetSample/ShimEnetHAL.h:23   OSErr EnetHAL_Entry(UInt16, USBEnetPtr, UInt32)
 *   usb-ddk/Examples/USBEnetSample/USBEnet.exp        exports EnetHAL_Entry
 *   the sample's Readme                               "the EnetShimLib locates and calls the
 *                                                      EnetHAL_Entry point which the USB driver
 *                                                      must have exported"
 * ⚠ EnetShim.h also defines Shim_Entry = "EnetMac_ShimEntry". That name is NOT what the sample
 *   exports and is not what the readme describes; it appears to be a leftover. Following the
 *   three sources that agree rather than the one that does not.
 *
 * ⚠⚠ NO FILE MANAGER ANYWHERE IN THIS FILE. Only EnetHAL_Open is documented as task time; the
 *    rest may be called below task level, and the File Manager below task level hangs OS 9 with
 *    no NMI and no debugger. So this driver does not log -- it COUNTS, into resident globals, and
 *    the probe app reads them out afterwards through AirPortShimStats(). Counters are safe from
 *    any execution level; FSWrite is not.
 */
#include <MacTypes.h>
#include <MacErrors.h>
#include <MacMemory.h>
#include <NameRegistry.h>
#include <DriverServices.h>
#include <Folders.h>          /* k190: FindFolder -- every log goes to the System Folder */
#include <Notification.h>     /* k204: the link layer's task-level pump (NMInstall, nmStr = 0) */
#include <OSUtils.h>          /* k204: nmType */
#include "ap_link.h"          /* k204: pure -- what the pump recognises, and when to rejoin */
#include <Gestalt.h>          /* k205: the status block is published as a Gestalt value */
#include "ap_status.h"        /* k205: the status block the control panel and strip module read */

/* ★★★ STAGE 8-5g: THE DRIVER NOW FINDS ITS OWN CARD.
 *
 * k58 got the whole way: OT plumbed the stream and drove this fragment sixteen times. It stopped
 * at the second selector -- GetMACAddress -- because this driver had never touched the hardware
 * and could not answer. Start never arrived, because OT will not start a link whose address it
 * cannot read.
 *
 * ⚠ THE SHORTCUT NOT TAKEN. Returning a plausible MAC would have got OT to call Start, and we
 *   would have had a port advertising a link on an address the card does not have. That is
 *   precisely the lie the honest-refusal design exists to prevent, and it would have been
 *   discovered three increments later as "the AP ignores us".
 *
 * So the driver reads the real address out of the card's SPROM. This is the first step of the
 * migration that has to happen anyway: the probe application does not exist in the shipped
 * product, so the driver must find its own hardware. It is deliberately the SMALLEST such step --
 * find the node, map BAR0, read six bytes. No PHY, no firmware, no DMA, no core resets.
 *
 * ⚠ WHY THIS CANNOT DISTURB THE APP, which is driving the same card at the same time:
 *   - SsbFindAndMap only walks the Name Registry, reads assigned-addresses and sets the PCI
 *     memory-decode bit. It does not reset, enumerate or select a core.
 *   - The SPROM shadow is at BAR0 + 0x1000 and, as airport_stage2.c:92 records, is "NOT in a core
 *     window". Reading it needs no SsbSelectCore, so the app's window is never moved underneath
 *     it -- which would have been a genuine way to corrupt a run in progress.
 *
 * ⚠ AND IT HAPPENS AT Open, NOT AT GetMACAddress. Apple's readme says EnetHAL_Open is called at
 *   task time and memory allocation is safe there; the other selectors carry no such promise.
 *   Mapping and Name Registry work below task level is how this project has hung machines before,
 *   so the address is read ONCE at Open and cached, and GetMACAddress only copies six bytes. */
#include "ssb_core.h"

/* ★★★ STAGE 8-2d-1: THE DRIVER CAN NARRATE NOW.
 *
 * Until this increment the driver COUNTED and the app read the counters out, because the only
 * Out() in the project did FSWrite + FlushVol and the File Manager below task level hangs OS 9
 * with no NMI. The counters were honest but thin: "Open: 1" does not say whether the firmware
 * upload got as far as the second blob.
 *
 * Three pieces make the narration possible, and the order of these four lines is the whole trick:
 *   ap_ring.h   a sink that only ever memcpy's into static storage -- no traps, no File Manager
 *   Out()       OUR sink, defined BEFORE ap_fmt.h because everything in it calls Out()
 *   ap_fmt.h    PCat/PCatHex/PCatDec/Say/SayH/Say1, the SAME copy the application uses
 *
 * ⚠ THE APP AND THE DRIVER NOW SHARE ONE SET OF FORMATTERS, WHICH IS THE POINT. When 8-2d-2 and
 *   8-2d-3 move ApplyIvs and ApPhyInitG in here, their 736 call sites come across unmodified and
 *   emit character-for-character what the app emits today. That is what makes the migration
 *   checkable: the driver's narration can be diffed against the app's banked logs, and a
 *   divergence is then a real behavioural difference rather than a reformatting artefact.
 *
 * ⚠ TASK LEVEL ONLY, TODAY. Everything below Open is safe because Open is the one selector Apple
 *   documents as task-time. The RX ISR must NOT start calling Say() -- see the execution-level
 *   note in ap_ring.h. */
#include "ap_ring.h"

static void Out(Str255 s) { ApRingPut(s); }

#include "ap_fmt.h"

/* ★ k199: THE PER-FRAME STOPWATCH (ap_tbclock.h). Declared before ap_eapol.h (which brings ap_wait.h)
 * and ap_enet.h, because RxWaitFor's copy (k200) and the receive decrypt report through the hooks
 * below; every other includer of those headers gets no-ops. ap_otmodl.c keeps OT's side (queue and
 * service-routine waits), and the snapshot prints them all together as PER-FRAME COST.
 * ⚠ gTbDecT0/gTbCopyT0 are single globals: two receive passes cannot overlap (while the ISR is armed
 *   the secondary owns the ring and the task-level pump bows out), and a collision would spoil one
 *   sample, nothing more. */
#include "ap_tbclock.h"
unsigned long gApTbPerMs = 0;                 /* extern in ap_tbclock.h; ApTbCalibrate sets it */
static ApTbHist gTbTxTotal, gTbTxEncap;       /* ApShimWriteFrame entry -> doorbell; CCMP encrypt */
static ApTbHist gTbTxCopy;                    /* k200: the encrypted frame, staged -> the ring buffer */
static ApTbHist gTbRxWalk, gTbRxConv, gTbRxDecrypt; /* RxWaitFor; ApRxToEnet; CCMP decrypt */
static ApTbHist gTbRxCopy;                    /* k200: RxWaitFor's copy out of the DMA page */
static ApTbHist gTbSecRun;                    /* one whole secondary-interrupt run */
static unsigned long gTbDecT0, gTbCopyT0;
#define AP_ENET_DECRYPT_BEGIN()   (gTbDecT0 = ApTbNow())
#define AP_ENET_DECRYPT_END(ok)   do{ if(ok) ApTbHistAdd(&gTbRxDecrypt, ApTbNow() - gTbDecT0); }while(0)
#define AP_RXW_COPY_BEGIN()       (gTbCopyT0 = ApTbNow())
#define AP_RXW_COPY_END()         ApTbHistAdd(&gTbRxCopy, ApTbNow() - gTbCopyT0)
/* k202: how long a slot left for the engine (k201) waited before it was taken. Stamped at the FIRST
 * deferral (| 1 so a timebase of 0 still reads as "stamped"); a frame held ~100 ms would say the
 * completion's interrupt was missed and only the next beacon brought the walk back. */
static ApTbHist      gTbRxDeferWait;
static unsigned long gRxDeferTb[32];
#define AP_RXW_DEFERRED(slot)     do{ if(!gRxDeferTb[(slot) & 31]) gRxDeferTb[(slot) & 31] = ApTbNow() | 1UL; }while(0)
#define AP_RXW_TAKEN(slot)        do{ unsigned long t_ = gRxDeferTb[(slot) & 31]; \
                                      if(t_){ ApTbHistAdd(&gTbRxDeferWait, ApTbNow() - t_); gRxDeferTb[(slot) & 31] = 0; } }while(0)

/* 8-2d-2a: the hardware half, taken whole rather than transcribed. See the note below.
 * 8-2d-3: ap_phy_initg.h includes ap_bringup.h itself, so taking the PHY header gets both. */
#include "ap_eapol.h"   /* 8-4; brings ap_wait.h, ap_tx.h and the whole stack below it.
                         * ⚠⚠ AND THE PASSPHRASE. See the header -- this binary is now secret-bearing. */
#include "ap_ndrv.h"    /* 8-11: TheDriverDescription + DoDriverIO. Shape only -- the cfrg
                         * still declares one kImportLibraryCFrag member, so the Device
                         * Manager cannot see us and CANNOT load us at boot. */
#include "ap_enet.h"    /* 8-6a: 802.11 in, Ethernet out. Brings ap_ccmp.h with it.
                         * ⚠ MUST FOLLOW ap_eapol.h, and the compiler is what said so: ap_enet.h
                         * uses RXH_FRAME_LEN and K6_HDR_PLCP6 from ap_tx.h, le16at from ap_dma.h
                         * and MacEq from ap_wait.h. It does not include them itself, because
                         * enet_test.c compiles this header on the HOST against small stubs and
                         * pulling the ring layer in would end that. Tenth header, same lesson as
                         * the other nine: the dependencies follow the code. */
#include "ap_txring.h"  /* k195: the TX ring's bookkeeping -- pure, host-tested by txring_test.c */
#include "ap_gtk.h"     /* k208: group message 1 and the group keys by key index -- pure, gtk_test.c */
#include "ap_scan.h"    /* k209: the beacon parser, the scan table, the null frame -- pure, scan_test.c */
#include "ap_known.h"   /* k210: the known networks + the Preferences-file form -- pure, known_test.c */

/* k208: ap_gtk.h carries its own copy of the EAPOL-Key layout, because ap_eapol.h cannot be compiled on the
 * host. A copy is exactly the hazard this project keeps paying for, so the two are pinned equal here, on
 * every build: a drift is a compile error, not a MIC that silently never verifies. */
_Static_assert(AP_GK_O_TYPE == EAPOL_O_TYPE && AP_GK_O_BODYLEN == EAPOL_O_BODYLEN &&
               AP_GK_O_DESCTYPE == EAPOL_O_DESCTYPE && AP_GK_O_KEYINFO == EAPOL_O_KEYINFO &&
               AP_GK_O_KEYLEN == EAPOL_O_KEYLEN && AP_GK_O_REPLAY == EAPOL_O_REPLAY &&
               AP_GK_O_MIC == EAPOL_O_MIC && AP_GK_O_DATALEN == EAPOL_O_DATALEN &&
               AP_GK_O_DATA == EAPOL_O_DATA, "ap_gtk.h's EAPOL-Key offsets differ from ap_eapol.h's");
_Static_assert(AP_GK_KI_VERSION == KEYINFO_VERSION && AP_GK_KI_PAIRWISE == KEYINFO_PAIRWISE &&
               AP_GK_KI_ACK == KEYINFO_ACK && AP_GK_KI_MIC == KEYINFO_MIC &&
               AP_GK_KI_SECURE == KEYINFO_SECURE && AP_GK_KI_ENCRYPTED == KEYINFO_ENCRYPTED,
               "ap_gtk.h's Key Info bits differ from ap_eapol.h's");

/* The shim's own definitions. Copied rather than #included: usb-ddk's EnetShim.h is not on this
 * project's include path, and vendoring three enums is better than a fragile relative path. The
 * values are transcribed from usb-ddk/Examples/USBEnetSample/EnetShim.h. */
typedef UInt32 ShimRefNum;

typedef struct USBEnet {
    ShimRefNum  ioRefNum;
    void       *ioBuffer;
    UInt32      ioReqCount;
    UInt32      ioActCount;
    ProcPtr     ioCompletion;
    void       *ioMisc;
    OSErr       ioResult;
} USBEnet;
typedef USBEnet *USBEnetPtr;

enum {
    EnetHAL_RegisterPorts       = 0,
    EnetHAL_Open                = 1,
    EnetHAL_Close               = 2,
    EnetHAL_Start               = 3,
    EnetHAL_Stop                = 4,
    EnetHAL_Read                = 5,
    EnetHAL_Write               = 6,
    EnetHAL_GetMACAddress       = 7,
    EnetHAL_SetMACAddress       = 8,
    EnetHAL_SetMulticastFilters = 9,
    EnetHAL_Status              = 10
};
/* ⚠⚠ 16, NOT 11, AND THE REASON IS A DOCUMENTATION CONFLICT WE CANNOT RESOLVE ON PAPER.
 *
 * usb-ddk/Examples/USBEnetSample/EnetShim.h:55-70 -- the header the sample compiles against --
 * ends the selector enum at EnetHAL_Status = 10, with no SetPacketFilter. EthernetShim.pdf,
 * Apple's spec for the same library, pages 5-6, lists EnetHAL_SetPacketFilter = 10 and
 * EnetHAL_Status = 11. They disagree, the PDF is marked DRAFT (1.0d1, July 2000), and the
 * EnetShimLib actually installed on the target is 1.5.9 from 2002 -- newer than both.
 *
 * Choosing between two documents is guessing. The binary is the only authority, and it will tell
 * us: if OT ever sends selector 11, the PDF numbering is right and our Status arm has been
 * answering SetPacketFilter this whole time. At 11 the old bound silently DROPPED the count
 * (selector < AP_SHIM_NSEL), so the one observation that settles it was the one we could not
 * make. */
#define AP_SHIM_NSEL 16

/* ── THE EVIDENCE. Resident globals, written from any execution level, read by the probe. ──
 *
 * gSelCount[s]  how many times selector s was called
 * gSelOrder[]   the FIRST few selectors in the order they arrived, which matters more than the
 *               counts: it shows the sequence OT actually drives, and that sequence is the thing
 *               8-5c has to implement against. A count of "Open: 1, Start: 1" does not say which
 *               came first or whether GetMACAddress was demanded in between.
 * gFirstBad     the first selector we had to refuse, so a refusal that aborts the sequence is
 *               attributable rather than inferred. */
#define AP_SHIM_ORDER_N 24
static UInt32 gSelCount[AP_SHIM_NSEL];
static UInt8  gSelOrder[AP_SHIM_ORDER_N];
static UInt32 gSelOrderN   = 0;
static UInt32 gTotalCalls  = 0;
static SInt32 gFirstBad    = -1;
static UInt32 gLastRefCon  = 0;

/* A signature the probe checks, so "the counters are all zero" can be told apart from "I am
 * reading the wrong memory". Without it, a failed FindSymbol and a driver that was never called
 * produce identical evidence. */
#define AP_SHIM_MAGIC 0x41505348UL   /* 'APSH' */
static UInt32 gMagic = AP_SHIM_MAGIC;

/* ── 8-5g: the card, and the address read out of it ── */
#define SSB_SPROM1_IL0MAC  0x0048UL          /* 6 bytes, the 802.11b/g MAC */
static UInt8   gShimMac[6];
static int     gShimMacOk   = 0;             /* the six bytes are real */
static OSStatus gShimMapErr = 1;             /* 1 = never attempted, so it can be told from noErr */

/* Called from EnetHAL_Open only. Task time: Name Registry, PCI config and a mapping are all legal
 * here and nowhere else in this file. */
static void ApShimFindCard(void)
{
    UInt16 w0,w1,w2;
    if(gShimMacOk) return;                   /* once per load */
    gShimMapErr = SsbFindAndMap(&gBus);
    if(gShimMapErr != noErr) return;
    if(gBus.bar0 == 0) { gShimMapErr = paramErr; return; }

    /* SPROM shadow, BAR0 + 0x1000, outside every core window. ssb writes each SPROM word
     * big-endian into il0mac, so the HIGH byte of each word comes first -- getting this backwards
     * yields a MAC with the octets pairwise swapped, which looks almost right and is not. */
    w0 = ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC);
    w1 = ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC+2);
    w2 = ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC+4);
    gShimMac[0]=(UInt8)(w0>>8); gShimMac[1]=(UInt8)(w0&0xFF);
    gShimMac[2]=(UInt8)(w1>>8); gShimMac[3]=(UInt8)(w1&0xFF);
    gShimMac[4]=(UInt8)(w2>>8); gShimMac[5]=(UInt8)(w2&0xFF);

    /* ⚠ SANITY-CHECK IT RATHER THAN TRUSTING IT. An unmapped or undecoded BAR reads as all-ones
     * or all-zeros, and either would be handed to OT as a valid unicast address. A multicast bit
     * in byte 0, or an all-zero / all-FF address, means the read did not work -- and refusing is
     * still better than lying. */
    { int i, allz = 1, allf = 1;
      for(i=0;i<6;i++){ if(gShimMac[i]!=0x00) allz=0; if(gShimMac[i]!=0xFF) allf=0; }
      if(allz || allf || (gShimMac[0] & 0x01)) { gShimMacOk = 0; gShimMapErr = paramErr; return; } }
    gShimMacOk = 1;
}

/* ── 8-2a: the backplane, enumerated by the driver itself ──
 *
 * The first step of moving the radio into the driver, and deliberately the least interesting one:
 * no firmware, no DMA, no interrupts, no core resets. It answers the question the rest of Stage
 * 8-2 rests on -- can two fragments own this card in sequence without fighting? -- while a mistake
 * still costs a wrong number in a log rather than a hung machine.
 *
 * ⚠⚠ SCANNING MOVES THE BAR0 WINDOW, AND THE APPLICATION READS THROUGH THE SAME WINDOW.
 *
 * SsbScanCores is read-only on the cores themselves -- it reads IDHIGH and CHIPID and identifies
 * ChipCommon and the 802.11 core -- but it gets to each one via SsbSelectCore, which writes the
 * PCI config register SSB_BAR0_WIN. It also leaves the window on the LAST core scanned rather than
 * where it found it. If the app read a register across that, it would get "core N believing it is
 * core M", which ssb_core.h's own comment describes as yielding plausible nonsense rather than an
 * obvious failure -- the worst kind of corruption to chase.
 *
 * Two defences, because one of them is an ordering argument and ordering arguments expire:
 *   1. This runs from EnetHAL_Open, which the probe triggers BEFORE its own ApBringUp
 *      (airport_rx.c: the 8-5 block is at ~2553, ApBringUp at ~3022). So today the app has not
 *      touched the card yet.
 *   2. The window is SAVED and RESTORED around the scan anyway. Defence 1 is a claim about two
 *      line numbers in another file and will silently stop being true the moment someone reorders
 *      the probe; defence 2 holds regardless. */
static UInt32 gShimChipId = 0, gShimChipRev = 0, gShimNCores = 0;
static SInt32 gShimIdx80211 = -1, gShimIdxCC = -1;
static OSStatus gShimScanErr = 1;          /* 1 = never attempted */

static void ApShimScanBackplane(void)
{
    UInt32 savedWin = 0;
    int haveSaved;

    if(gShimScanErr == noErr) return;      /* once per load */
    if(gBus.bar0 == 0){ gShimScanErr = paramErr; return; }

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    gShimScanErr = SsbScanCores(&gBus);
    if(gShimScanErr == noErr){
      gShimChipId   = gBus.chipId;
      gShimChipRev  = gBus.chipRev;
      gShimNCores   = gBus.nCores;
      gShimIdx80211 = (SInt32)gBus.idx80211;
      gShimIdxCC    = (SInt32)gBus.idxChipCommon;
      /* k218 belt-and-braces: SsbFindAndMap already binds only PCI 14e4:4320/4325 (BCM4306), so a
       * BCM4318 (e.g. a later A1026 or an A1126/A1127 combo) is declined before this -- but assert the
       * ChipCommon id is 0x4306 too, so any unexpected chip is declined cleanly rather than driven on
       * BCM4306 assumptions. On the supported card chipId == 0x4306, so this never fires. */
      if(gShimChipId != 0x4306UL) gShimScanErr = paramErr; }

    /* ⚠ Restore on EVERY path, including the failure one. A scan that gave up halfway has still
     * moved the window, and leaving it parked on an arbitrary core is exactly how a later,
     * unrelated read returns believable rubbish. */
    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* ── 8-2b: THE FIRST STEP THAT WRITES, AND THE INTERLOCK THAT MAKES IT SAFE ──
 *
 * Everything the driver has done so far is read-only: walk the registry, map BAR0, read the
 * SPROM, enumerate cores (restoring the window). Enabling a core is different. SsbCoreEnable
 * calls SsbCoreDisable first and then drives TMSLOW through a reset sequence -- it is a real
 * teardown of the 802.11 core. Done at the wrong moment it would destroy a run in progress.
 *
 * ⚠⚠ AND "THE WRONG MOMENT" IS NOT HYPOTHETICAL. Every safe step so far has leaned on an ordering
 *   argument: EnetHAL_Open happens before the app's ApBringUp. That is true of how the probe
 *   drives things TODAY, and it is a claim about line numbers in another file. Open Transport
 *   decides when to open a port; nothing guarantees it will only ever do so at a moment of our
 *   choosing, and a port left registered could be opened by anything.
 *
 * So the destructive step is gated on an EXPLICIT ARM rather than on timing. The application --
 * which is the thing that actually knows whether it is using the card -- calls AirPortShimArm(1)
 * when the card is idle and AirPortShimArm(0) when it is not. Unarmed, Open still does all the
 * read-only work and simply declines to reset anything.
 *
 * ⇒ This interlock is TRANSITIONAL. It exists because two owners share one card during the
 *   migration. When the app is gone the driver is the only owner and arms itself -- and the
 *   right time to delete this is when that happens, not before. */
static int      gShimArmed    = 0;
static int      gShimCoreUp   = 0;
static int      gShimCoreWasUp = 0;   /* 8-2d-3: the void-run predictor ApBringUp records */
static OSStatus gShimCoreErr  = 1;          /* 1 = never attempted */
static UInt32   gShimTmsLow   = 0;

/* Called by the application at task level. Returns the previous state. */
UInt32 AirPortShimArm(UInt32 allow)
{
    UInt32 was = (UInt32)gShimArmed;
    gShimArmed = (allow != 0);
    return was;
}

/* Enable the 802.11 core. Task time only, and only when armed. */
static void ApShimEnableCore(void)
{
    UInt32 savedWin = 0;
    int haveSaved;

    if(gShimCoreUp) return;                  /* once per load */
    if(!gShimArmed){ gShimCoreErr = permErr; return; }   /* declined, not failed */
    if(gShimScanErr != noErr || gShimIdx80211 < 0){ gShimCoreErr = paramErr; return; }

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      gShimCoreErr = paramErr;
    } else {
      /* ⚠⚠ 8-2d-3: THE FLAGS WERE 0 UNTIL NOW, AND 8-2b's ORACLE COULD NOT SEE IT.
       * ApBringUp passes GMODE|PHYCLKEN|PHYRESET and then calls PhyTakeOutOfReset(); this
       * driver passed 0 and skipped the reset dance entirely. 8-2b scored "the core came up"
       * on TMSLOW's CLOCK and RESET bits, which those flags do not touch -- so a core with no
       * GMODE, and a PHY still held in reset, passed a test named for exactly that.
       * The firmware upload and the initvals did not care, which is why 8-2c and 8-2d-2 both
       * passed on top of it. ApPhyInitG cares enormously, and would have looked broken.
       * ⚠ An oracle that cannot see the thing it is named after is this project's recurring
       *   lesson, and this is a fresh instance of it rather than an old one. */
      { UInt32 pre = ssb_r32(gBus.bar0,SSB_TMSLOW);
        gShimCoreWasUp = ((pre & SSB_TMSLOW_CLOCK) != 0) && ((pre & SSB_TMSLOW_RESET) == 0); }
      SsbCoreEnable(&gBus,B43_TMSLOW_GMODE|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET);
      SsbSpinUs(2500);
      PhyTakeOutOfReset();
      /* ⚠ READ BACK RATHER THAN ASSUMING. SsbCoreEnable returns void -- it cannot report failure,
       * and "we wrote the registers" is not "the core came up". TMSLOW should show CLOCK set and
       * RESET clear; anything else means the sequence did not take, and a driver that believed it
       * had a running core would fail later, somewhere unrelated. */
      gShimTmsLow = ssb_r32(gBus.bar0,SSB_TMSLOW);
      if((gShimTmsLow & SSB_TMSLOW_CLOCK) && !(gShimTmsLow & SSB_TMSLOW_RESET)){
        gShimCoreUp = 1; gShimCoreErr = noErr;
        /* ⚠ 8-2d-3: these two were missing, and check-prefix-order.py is what found them.
         * ApBringUp reads the radio ID in minimal state here -- "cheap, driver-sanctioned, and
         * it fails loudly if the teardown left the core wrong" -- and then does the FIRST
         * switch_analog(1). There are two switch_analog calls in chip_init's prefix, not one;
         * keeping only the later one looked complete and was not. */
        Say("");
        Say("  [8-2d-3] radio ID control in minimal state (b43_phy_versioning's path)");
        (void)RadioIdControl();
        SwitchAnalog(1);
        SayH("  [8-2d-3] switch_analog(1) done; PHY_VER = ",
             (unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);
      } else {
        gShimCoreErr = paramErr; } }

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* ── 8-2c: THE DRIVER UPLOADS ITS OWN FIRMWARE ──
 *
 * The blobs are compiled in rather than read as resources, and that is a deliberate choice with a
 * cost. See the long note in embed-fw.sh: an application's resource fork is open, a CFM shared
 * library's is not, so the shim would need its own FSSpec from a CFM init routine -- and MakePEF
 * in this toolchain exposes no way to declare one. Calling GetResource anyway would have read the
 * APPLICATION'S resources from inside the driver: it would have worked today, looked like
 * independence, and kept exactly the coupling this migration exists to remove.
 *
 * ⚠ The 8-byte header is validated before a single word is uploaded, the same way the app does it.
 *   A truncated or wrong-generation blob is then caught here rather than halfway into the chip's
 *   microcode RAM, where the symptom would be a dead radio with no explanation.
 */
#include "firmware_blobs_c.h"

/* ★★★ 8-2d-2a: THE TRANSCRIBED COPIES ARE GONE; THE DRIVER TAKES ap_bringup.h DIRECTLY.
 *
 * This file used to carry its own copies of a dozen b43 register constants and two SHM
 * accessors, and the comment here argued for that at length: ap_bringup.h "also drags in the
 * Toolbox, QuickDraw, windows and the File Manager", and the surest way to honour the
 * no-File-Manager rule was for those symbols not to exist in the fragment at all.
 *
 * ⚠ THAT ARGUMENT EXPIRED WITH 8-2d-0, AND THE COMMENT WOULD HAVE OUTLIVED IT SILENTLY. The
 *   logging split moved every Toolbox dependency out; ap_bringup.h now includes exactly
 *   <Resources.h> and "ssb_core.h" and reaches no File Manager at all. That old comment was a
 *   claim about a call graph, and this project's rule is that such a claim expires when the
 *   callers change -- audit, do not trust. Audited: expired.
 *
 * ⚠ THE DUPLICATION WAS A REAL LIABILITY, not a theoretical one. The note said so itself: "if
 *   the app's ever change, these are a second place to change." Two divergent copies of
 *   B43_MMIO_MACCTL would have produced a driver writing a plausible value to the wrong
 *   register -- silent, and attributable to nothing.
 *
 * The include now sits next to ap_fmt.h, above, because it must come after Out()/Say() exist
 * and before anything here touches the bus. */

/* 8-2c's results, read back by the app through AirPortShimGetFw. ⚠ gShimFwErr starts at 1 rather
 * than noErr so that "never attempted" is distinguishable from "attempted and succeeded" -- k66
 * turned on exactly that distinction when the accessor failed to resolve and the sentinel was
 * what the oracle saw. */
static UInt16   gShimFwRev = 0;
static UInt32   gShimFwIrq = 0;
static OSStatus gShimFwErr = 1;

static UInt32 ApBe32(const unsigned char *p)
{ return ((UInt32)p[0]<<24)|((UInt32)p[1]<<16)|((UInt32)p[2]<<8)|(UInt32)p[3]; }

/* Validate one blob's 8-byte header: {u8 type; u8 ver; u8 pad[2]; be32 size}. Returns the payload
 * and its length, or 0. `want` is 'u', 'p' or 'i'. */
static int ApShimFwCheck(const unsigned char *b, UInt32 len, unsigned char want,
                         const unsigned char **payload, UInt32 *plen)
{
    UInt32 declared;
    if(len < 9) return 0;
    if(b[0] != want || b[1] != 1) return 0;
    declared = ApBe32(b+4);
    /* 'i' declares an initval COUNT, not a byte length -- the app's LoadFw makes the same
     * exception, and checking it as a length would reject every good initvals blob. */
    if(want != 'i' && declared != (len - 8)) return 0;
    *payload = b + 8;
    *plen    = len - 8;
    return 1;
}

static void ApShimLoadFirmware(void)
{
    const unsigned char *uc, *pc;
    UInt32 ucLen, pcLen, w, words, macctl, irq, savedWin = 0;
    int haveSaved, havePcm;

    if(gShimFwRev != 0) return;                       /* once per load */
    if(!gShimCoreUp){ gShimFwErr = paramErr; return; } /* needs the core enabled first */

    if(!ApShimFwCheck(kApFwUcode,kApFwUcode_LEN,'u',&uc,&ucLen)){ gShimFwErr = paramErr; return; }
    havePcm = ApShimFwCheck(kApFwPcm,kApFwPcm_LEN,'p',&pc,&pcLen);

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      gShimFwErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    /* ⚠ INFRA IS NOT OPTIONAL HERE. ap_bringup.h records that it was missing from Stage 3b through
     * i4 and that its absence was a real defect -- b43_chip_init sets IHR|SHM|GMODE|INFRA before
     * b43_upload_microcode ORs in PSM_JMP0. Copying the working sequence, not a subset of it. */
    macctl = B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_INFRA;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl); (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL)|B43_MACCTL_PSM_JMP0;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl); (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);

    for(w=0;w<64;w++){ ShmControl(2UL,w); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,0); }
    for(w=0;w<4096;w+=4){ ShmControl(1UL,w>>2); ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,0); }

    words = ucLen/4;
    ShmControl(0UL|0x0100UL,0x0000);                  /* SHM_UCODE | AUTOINC_W */
    for(w=0;w<words;w++){
      ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,ApBe32(uc+w*4)); SsbSpinUs(10); }

    if(havePcm){
      words = pcLen/4;
      ShmControl(3UL,0x01EA); ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,0x00004000UL);
      ShmControl(3UL,0x01EB);
      for(w=0;w<words;w++){
        ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,ApBe32(pc+w*4)); SsbSpinUs(10); } }

    /* ⚠ 8-2d-3: CLEAR GEN_IRQ_REASON FIRST. ApBringUp writes 0xFFFFFFFF here before starting
     * the PSM, and this driver did not. The readiness test below waits for GEN_IRQ_REASON == 1,
     * so a stale bit from an earlier run could satisfy it before the microcode said anything --
     * a pass that means nothing, which is the worst kind. */
    ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
    macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    macctl &= ~B43_MACCTL_PSM_JMP0; macctl |= B43_MACCTL_PSM_RUN;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl); (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);

    /* The microcode signals readiness by raising GEN_IRQ_REASON == 1. */
    irq = 0;
    for(w=0;w<100;w++){
      irq = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
      if(irq == 0x00000001UL) break;
      SsbSpinUs(10000); }
    gShimFwIrq = irq;
    gShimFwRev = ShmRead16Shared(0x0000UL);

    /* ⚠ b43's rule, not bwi's: reject rev <= 0x128. ap_bringup.h documents at length that this
     * guard was backwards for twenty probes and cost Stage 5 most of its schedule. The driver
     * inherits the CORRECTED form, and inherits it explicitly rather than by having copied code
     * that happened to be right. */
    if(irq != 0x00000001UL || gShimFwRev == 0)      gShimFwErr = paramErr;
    else if(gShimFwRev <= 0x128)                    gShimFwErr = paramErr;
    else                                            gShimFwErr = noErr;

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* ★★★ STAGE 8-2d-2b: THE DRIVER APPLIES THE INITVALS.
 *
 * b43_chip_init steps 4 and 5. ApplyIvs comes from ap_bringup.h now rather than being transcribed
 * a third time -- that is what 8-2d-2a bought, and it is the same routine, against the same
 * register names, that the application has been running since Stage 3b.
 *
 * ★ THE ORACLE IS IN THE BLOB, WHICH IS WHY THIS IS WORTH A RUN. An 'i' blob's 8-byte header
 *   declares an initval COUNT rather than a byte length (LoadFw makes the same exception, and
 *   ap_bringup.h says why). So the number of records ApplyIvs reports can be checked against a
 *   figure written into the firmware years ago and derived from nothing this code does.
 *   applied == declared is a content oracle with a pre-known answer, not a return code agreeing
 *   with itself.
 *
 * ⚠ IT NARRATES AS IT GOES, not afterwards. ApShimNarrateOpen reports results once everything has
 *   returned; if the second table faulted, its "applied" line would simply not exist and the
 *   reader would be guessing whether it ran. Progress lines make a PARTIAL failure legible.
 *
 * ⚠ THAT IS A NARROWER CLAIM THAN I MADE WHEN PLANNING THIS INCREMENT, and the correction matters
 *   because it changes what this buys. I wrote that narrating inline would let us "diagnose a
 *   hang inside" the bring-up. It will not. The ring is drained by the APPLICATION after Open
 *   returns, so a hard hang inside Open yields nothing either way -- the machine is gone and
 *   nobody reads the ring. What inline narration actually buys is granularity when the driver
 *   RETURNS having done part of the work. Worth having; not the thing I claimed.
 *
 * ⚠ THE BAR0 WINDOW IS SAVED AND RESTORED, same as the firmware upload. ApplyIvs writes core
 *   registers below 0x1000, so the 802.11 core has to be selected -- and the application is using
 *   the same window, so leaving it moved would corrupt a run in progress rather than fail. */
static UInt32 gShimIvApplied = 0, gShimIvDeclared = 0;
static UInt32 gShimBsApplied = 0, gShimBsDeclared = 0;
static OSStatus gShimIvErr = 1;                   /* 1 = never attempted */

static void ApShimApplyInitvals(void)
{
    const unsigned char *iv, *bs;
    UInt32 ivLen, bsLen, savedWin = 0;
    int haveSaved, haveBs;
    Str255 L;

    if(gShimIvErr != 1) return;                   /* once per load */

    Say("");
    Say("  [8-2d-2] INITVALS (b43_chip_init steps 4 and 5)");

    /* ⚠ ORDER. The initvals must land AFTER the microcode is running -- b43_chip_init uploads
     * ucode, waits for GEN_IRQ_REASON, and only then writes the tables. Refusing here rather
     * than writing them into a dead core keeps a sequencing mistake from looking like a bad
     * firmware blob three stages downstream. */
    if(gShimFwErr != noErr){
      Say("    [--] DECLINED: the microcode is not up, so there is nothing to configure.");
      Say("      ⚠ Not a failure of the initvals -- they were never written. Read the 8-2c");
      Say("        result above; this is downstream of it.");
      gShimIvErr = paramErr;
      return; }

    if(!ApShimFwCheck(kApFwIv,kApFwIv_LEN,'i',&iv,&ivLen)){
      Say("    [!!] the b0g0initvals5 blob failed its header check -- re-run embed-fw.sh");
      gShimIvErr = paramErr;
      return; }
    gShimIvDeclared = ApBe32(kApFwIv+4);           /* an 'i' header declares a RECORD COUNT */
    haveBs = ApShimFwCheck(kApFwBsIv,kApFwBsIv_LEN,'i',&bs,&bsLen);
    if(haveBs) gShimBsDeclared = ApBe32(kApFwBsIv+4);

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core -- nothing was written.");
      gShimIvErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    Say("    applying b0g0initvals5...");
    gShimIvApplied = ApplyIvs(iv, ivLen);
    L[0]=0; PCat(L,"    b0g0initvals5   applied "); PCatDec(L,gShimIvApplied);
    PCat(L," of ");                                 PCatDec(L,gShimIvDeclared);
    PCat(L," declared");                            Out(L);

    if(haveBs){
      Say("    applying b0g0bsinitvals5...");
      gShimBsApplied = ApplyIvs(bs, bsLen);
      L[0]=0; PCat(L,"    b0g0bsinitvals5 applied "); PCatDec(L,gShimBsApplied);
      PCat(L," of ");                                 PCatDec(L,gShimBsDeclared);
      PCat(L," declared");                            Out(L);
    } else {
      Say("    ⚠ b0g0bsinitvals5 ABSENT from this build -- re-run embed-fw.sh"); }

    /* PHY_VER is readable only once the tables are in; the app prints it at the same point. */
    SayH("    PHY_VER after = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ⚠ SCORED ON THE COUNT, NOT ON "ApplyIvs RETURNED". ApplyIvs stops early and silently on a
     * malformed record or an out-of-range offset -- it range-checks BEFORE the write, which is
     * right, but it means a short count is the ONLY evidence that anything was rejected. A test
     * that accepted any non-zero return would pass on a table that applied one record of three
     * hundred. */
    if(gShimIvApplied != gShimIvDeclared)                       gShimIvErr = paramErr;
    else if(haveBs && gShimBsApplied != gShimBsDeclared)        gShimIvErr = paramErr;
    else if(!haveBs)                                            gShimIvErr = paramErr;
    else                                                        gShimIvErr = noErr;

    Say(gShimIvErr == noErr ? "    [ok] both tables applied in full."
                            : "    [!!] a table was short -- see the counts above.");

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

UInt32 AirPortShimGetFw(UInt32 *fwRev, UInt32 *irq, SInt32 *fwErr)
{
    if(fwRev) *fwRev = (UInt32)gShimFwRev;
    if(irq)   *irq   = gShimFwIrq;
    if(fwErr) *fwErr = (SInt32)gShimFwErr;
    return (UInt32)(gShimFwErr == noErr);
}

UInt32 AirPortShimGetCoreState(UInt32 *tmsLow, SInt32 *coreErr, UInt32 *armed)
{
    if(tmsLow)  *tmsLow  = gShimTmsLow;
    if(coreErr) *coreErr = (SInt32)gShimCoreErr;
    if(armed)   *armed   = (UInt32)gShimArmed;
    return (UInt32)gShimCoreUp;
}

/* Exported so the probe can compare the DRIVER's backplane inventory against its own. */
UInt32 AirPortShimGetCores(UInt32 *chipId, UInt32 *chipRev, UInt32 *nCores,
                           SInt32 *idx80211, SInt32 *idxCC, SInt32 *scanErr)
{
    if(chipId)   *chipId   = gShimChipId;
    if(chipRev)  *chipRev  = gShimChipRev;
    if(nCores)   *nCores   = gShimNCores;
    if(idx80211) *idx80211 = gShimIdx80211;
    if(idxCC)    *idxCC    = gShimIdxCC;
    if(scanErr)  *scanErr  = (SInt32)gShimScanErr;
    return (UInt32)(gShimScanErr == noErr);
}

/* Exported so the probe can report what the DRIVER saw, independently of what the app reads. Two
 * separate paths to the same six bytes is a cross-check worth having: if they disagree, one of
 * the two mappings is wrong and that is far better learned here than three stages later. */
UInt32 AirPortShimGetMac(UInt8 *out, SInt32 *mapErr)
{
    int i;
    if(out) for(i=0;i<6;i++) out[i] = gShimMac[i];
    if(mapErr) *mapErr = (SInt32)gShimMapErr;
    return (UInt32)gShimMacOk;
}

/* Exported accessor. The probe calls this at task level after giving OT a chance to drive us.
 * Returns the magic; fills the caller's buffers. */
UInt32 AirPortShimStats(UInt32 *totalCalls, UInt32 *counts, UInt8 *order, UInt32 *orderN,
                        SInt32 *firstBad, UInt32 *lastRefCon)
{
    UInt32 i;
    if(totalCalls) *totalCalls = gTotalCalls;
    if(counts) for(i=0;i<AP_SHIM_NSEL;i++) counts[i] = gSelCount[i];
    if(order)  for(i=0;i<AP_SHIM_ORDER_N;i++) order[i] = gSelOrder[i];
    if(orderN) *orderN = gSelOrderN;
    if(firstBad) *firstBad = gFirstBad;
    if(lastRefCon) *lastRefCon = gLastRefCon;
    return gMagic;
}

/* ── 8-2d-1: hand the driver's narration back to the app, one line per call ──
 *
 * Returns the number of lines the driver has narrated, ALWAYS -- so the caller reads the count
 * first with out == NULL, then loops. If out is non-NULL and idx is in range, line idx is copied
 * in as a Pascal string; otherwise out[0] is set to 0 so a caller that ignores the count still
 * gets an empty line rather than whatever was on its stack.
 *
 * ⚠ dropped IS NOT OPTIONAL READING. A non-zero value means the TAIL of the narration is missing
 *   -- the ring fills and stops rather than wrapping, see ap_ring.h. A reader who does not print
 *   it will mistake a truncated log for a bring-up that stopped early, which is the exact
 *   misreading this increment exists to prevent. The app prints it unconditionally.
 *
 * One line per call is deliberately the dumb interface. The alternative -- handing back a pointer
 * into the driver's data section -- would let the app read the ring while the driver is still
 * writing it, and would tie the app's log format to the driver's storage layout. Copying 256
 * bytes a few hundred times at task level costs nothing anyone can measure. */
/* ★★★ STAGE 8-2d-3: THE REST OF chip_init's PREFIX, THEN THE G-PHY.
 *
 * ⚠⚠ WHAT THIS INCREMENT ACTUALLY FOUND. It was planned as "include ap_phy_initg.h and call
 *   ApPhyInitG()". Reading ApBringUp() first -- which ap_phy_initg.h's own header tells you to
 *   do, because it says in its first twenty lines that it does NOT call ApBringUp and the caller
 *   must -- showed the driver had been running a SUBSET of chip_init's prefix all along:
 *
 *     SsbCoreEnable flags      0            should be GMODE|PHYCLKEN|PHYRESET
 *     SsbSpinUs(2500)          absent
 *     PhyTakeOutOfReset()      absent       the PHY was still held in reset
 *     SwitchAnalog(1) (early)  absent
 *     GEN_IRQ_REASON clear     absent       readiness could be satisfied by a stale bit
 *     GpioInit()               absent       chip_init step 3
 *     SwitchAnalog(1)+RadioOn  absent       chip_init steps 6 and 7
 *
 *   Calling ApPhyInitG() on that would have initialised a PHY in reset with an unpowered radio,
 *   and the failure would have been attributed to ApPhyInitG rather than to its preconditions.
 *
 * ⚠ THESE ARE CALLS, NOT COPIES. Every step below is a call into ap_bringup.h -- PhyTakeOutOfReset,
 *   GpioInit, SwitchAnalog, RadioOn, RadioIdControl. The only hand-written thing is the ORDER, and
 *   check-prefix-order.py diffs that order against ApBringUp's on every build, because "a
 *   hand-copied sequence drifts, a called function cannot" and an ORDER is the one part of this
 *   that is still hand-copied.
 *
 * ⚠ WHAT IS STILL NOT DONE, and is the caller's per ap_phy_initg.h's header: chip_init's
 *   statements AFTER phy_init -- interference mitigation, antenna selection, PRMAXTIME,
 *   b43_adjust_opmode, mac_phy_clock_set. Those belong to 8-2e, not here. Stopping at a function
 *   boundary is the habit; guessing at what is required is not. */
static int      gShimGpioOk   = 0;
static UInt32   gShimBoardFlags = 0;
static int      gShimRadioOn  = 0;
static int      gShimPhyOk    = 0;
static OSStatus gShimPhyErr   = 1;              /* 1 = never attempted */

/* chip_init step 3, between the microcode and the initvals -- exactly where ApBringUp puts it. */
static void ApShimGpioInit(void)
{
    UInt32 gm=0, gs=0;
    int ccOk=0;
    if(gShimFwErr != noErr) return;             /* downstream of the microcode */
    Say("");
    Say("  [8-2d-3] GPIO INIT (b43_gpio_init) -- chip_init step 3");
    GpioInit(&gShimBoardFlags,&gm,&gs,&ccOk);
    gShimGpioOk = ccOk;
    SayH("    board flags (SPROM +0x72) = ",gShimBoardFlags,4);
    Say((gShimBoardFlags&B43_BFL_PACTRL)
        ? "    PACTRL set -> GPIO 9 enabled so the ucode can drive the PA"
        : "    PACTRL clear -> GPIO 9 branch not taken");
    Say(ccOk ? "    ChipCommon GPIO_CONTROL holds the masked bits"
             : "    ⚠ ChipCommon GPIO_CONTROL did NOT hold the masked bits");
    SayH("    window back on the 802.11 core, TMSLOW = ",ssb_r32(gBus.bar0,SSB_TMSLOW),8);
}

/* chip_init steps 6 and 7, after the initvals. */
static void ApShimRadioOn(void)
{
    if(gShimIvErr != noErr) return;             /* downstream of the initvals */
    Say("");
    Say("  [8-2d-3] switch_analog(1), then b43_software_rfkill(dev,false)");
    SwitchAnalog(1);
    RadioOn(1);                                 /* b43_gphy_op_get_default_chan() returns 1 */
    gShimRadioOn = 1;
    SayH("    PGACTL after the 0x8000 / 0xCC00 / 0x00C0 strobe = ",
         (unsigned long)PhyRead((UInt16)B43_PHY_PGACTL),4);
    SayH("    CHANNEL_EXT (0x3F4) = ",
         (unsigned long)ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT),4);
    Say("");
    Say("  [8-2d-3] radio ID control, AFTER the full prefix");
    (void)RadioIdControl();
}

/* ★★★★★ AND THEN THE G-PHY ITSELF.
 *
 * ApPhyInitG() is b43_phy_init's body, extracted mechanically from the j7 commit whose log passed
 * oracles A through O. It returns 1 only if all fifteen of those oracles pass, so this single
 * call re-runs the whole of Stage 4 as a regression from inside the driver.
 *
 * ★ THAT IS THE ORACLE, and it is far stronger than anything this increment could invent: fifteen
 *   independent checks, written months ago against measured hardware identity, none of which knows
 *   it is being run from a driver rather than an application.
 *
 * ⚠ ~848 LINES OF NARRATION LAND IN THE RING HERE. ap_ring.h was raised to 80000 bytes for
 *   exactly this, after MEASURING a k69 log rather than reasoning about call-site counts. If the
 *   drop counter is non-zero in the log, the ring is too small again and the tail is missing --
 *   not the PHY stopping early. */
static void ApShimPhyInit(void)
{
    if(gShimPhyErr != 1) return;                /* once per load */
    if(!gShimRadioOn){
      Say("");
      Say("  [8-2d-3] PHY INIT DECLINED -- the prefix did not complete.");
      Say("    ⚠ Not a PHY failure. ApPhyInitG's own header says the caller must run the");
      Say("      chip_init prefix first; read the steps above for which one stopped.");
      gShimPhyErr = paramErr;
      return; }
    Say("");
    Say("  [8-2d-3] ★★★ ApPhyInitG() -- b43_phy_init, all fifteen Stage 4 oracles");
    gShimPhyOk  = ApPhyInitG();
    gShimPhyErr = gShimPhyOk ? noErr : paramErr;
    Say(gShimPhyOk ? "  [8-2d-3] ★★★★★ ApPhyInitG returned 1 -- every Stage 4 oracle passed"
                   : "  [8-2d-3] [!!] ApPhyInitG returned 0 -- an oracle failed, see above");
}

/* 8-2d-3: the prefix and PHY results, for the app's oracle. */
UInt32 AirPortShimGetPhy(UInt32 *gpioOk, UInt32 *boardFlags, UInt32 *radioOn,
                         SInt32 *phyErr, UInt32 *coreWasUp)
{
    if(gpioOk)     *gpioOk     = (UInt32)gShimGpioOk;
    if(boardFlags) *boardFlags = gShimBoardFlags;
    if(radioOn)    *radioOn    = (UInt32)gShimRadioOn;
    if(phyErr)     *phyErr     = (SInt32)gShimPhyErr;
    if(coreWasUp)  *coreWasUp  = (UInt32)gShimCoreWasUp;
    return (UInt32)gShimPhyOk;
}

/* ★★★ STAGE 8-2e: b43_chip_init's TAIL, statements 8 through 13.
 *
 * ChipInitTail comes from ap_chipinit_tail.h, extracted verbatim from airport_rx.c rather than
 * hand-assembled here. That choice is 8-2d-3's lesson applied before it could cost anything: the
 * prefix WAS hand-assembled, six steps went missing, and three oracles passed on top of the gap.
 *
 * ★ THE ORACLES ARE READ-BACKS OF SPECIFIC WRITES, not "it returned".
 *     TMSLOW MACPHYCLKEN   MacPhyClockSet(1) sets bit 20; read it back
 *     SHM_SH_PRMAXTIME     statement [10] writes 0; read it back
 *     MACCTL INFRA         cleared then set again; must end SET
 *   Each names a register the function demonstrably touched. 8-2b's oracle failed because it
 *   watched bits its operation never wrote -- see the note above ApShimEnableCore. */
static UInt32 gShimTailMacctl = 0, gShimTailTmsLow = 0;
static UInt16 gShimTailPretbtt = 0, gShimTailPrmaxtime = 0xFFFF;
static UInt32 gShimCoreRev = 0, gShimPhyRev = 0;
static OSStatus gShimTailErr = 1;               /* 1 = never attempted */

static void ApShimChipInitTail(void)
{
    UInt32 savedWin = 0;
    int haveSaved;

    if(gShimTailErr != 1) return;               /* once per load */
    if(!gShimPhyOk){
      Say("");
      Say("  [8-2e] chip_init TAIL DECLINED -- the G-PHY is not initialised.");
      Say("    ⚠ Not a tail failure. b43 runs statements 8-13 AFTER phy_init, and running them");
      Say("      on an uninitialised PHY would write antenna and opmode state into a core that");
      Say("      has not been configured. Read the 8-2d-3 result above.");
      gShimTailErr = paramErr;
      return; }

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("  [8-2e] [!!] could not select the 802.11 core -- nothing was written.");
      gShimTailErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    Say("");
    Say("  [8-2e] b43_chip_init statements 8 through 13");
    gShimCoreRev = IDHIGH_REV(ssb_r32(gBus.bar0,SSB_IDHIGH));
    gShimPhyRev  = (UInt32)(ssb_r16(gBus.bar0,B43_MMIO_PHY_VER) & 0x000F);
    Say1("    core rev = ",(unsigned long)gShimCoreRev);
    Say1("    phy rev  = ",(unsigned long)gShimPhyRev);

    ChipInitTail(gShimCoreRev,(UInt16)gShimPhyRev,gBus.chipId,gBus.chipRev,
                 &gShimTailMacctl,&gShimTailPretbtt);

    /* ── the read-backs ── */
    gShimTailTmsLow    = ssb_r32(gBus.bar0,SSB_TMSLOW);
    gShimTailPrmaxtime = ShmRead16Shared(B43_SHM_SH_PRMAXTIME);
    SayH("    MACCTL after opmode = ",gShimTailMacctl,8);
    Say1("    PRETBTT             = ",(unsigned long)gShimTailPretbtt);
    SayH("    TMSLOW after tail   = ",gShimTailTmsLow,8);
    Say1("    PRMAXTIME read back = ",(unsigned long)gShimTailPrmaxtime);

    if(!(gShimTailTmsLow & B43_TMSLOW_MACPHYCLKEN)){
      Say("    [!!] MACPHYCLKEN is CLEAR -- MacPhyClockSet(1) did not take.");
      gShimTailErr = paramErr;
    } else if(gShimTailPrmaxtime != 0){
      Say("    [!!] PRMAXTIME did not read back as 0 -- statement [10] did not take.");
      gShimTailErr = paramErr;
    } else if(!(gShimTailMacctl & B43_MACCTL_INFRA)){
      Say("    [!!] INFRA is CLEAR in MACCTL -- it is cleared then set again, and must end set.");
      gShimTailErr = paramErr;
    } else {
      Say("    [ok] MACPHYCLKEN set, PRMAXTIME 0, INFRA set -- all three read back.");
      gShimTailErr = noErr; }

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* ★★★ 9-29 k194: b43_wireless_core_init's MIDDLE -- the statements between b43_chip_init and
 * b43_dma_init that this port never had (main.c:4870-4933). ApShimChipInitTail carries chip_init plus
 * PRMAXTIME; ApShimCoreInitTail carries the five statements AFTER dma_init. Everything in between was
 * skipped, and none of it mattered while every frame was 1 Mbps CCK:
 *   WLCOREREV                    the microcode's copy of the 802.11 core revision
 *   b43_set_retry_limits(7, 4)   short/long retry limits (SHM scratch)
 *   SFFBLIM 3, LFFBLIM 2         attempts at the main rate before the FALLBACK rate -- k193 now sends
 *                                OFDM with a CCK 1 fallback, so this decides when that fallback fires
 *   b43_rate_memory_init         per-rate entries the ucode uses for ACK/CTS; G-PHY: 8 OFDM then 4 CCK
 *   b43_set_phytxctl_defaults    PHY word (CCK | ANT01AUTO | TXPWR) for beacon, ACK/CTS, probe resp
 *   MINCONT 0xF, MAXCONT 0x3FF   the contention window (G-PHY; a B-PHY would be 0x1F)
 *   PHYTYPE G, PHYVER            which PHY the ucode drives -- only a G-PHY does OFDM
 * THE EVIDENCE (k193 run 2): once we advertised 802.11g the AP sent to us at OFDM for the FIRST time,
 * and the Pi->Mac direction went lossy -- 5% of the Pi's segments resent, a 67 s silence opening the
 * share -- while Mac->Pi stayed at 1%. Without the OFDM rate entries the ucode has no correct way to
 * ACK a frame it received at OFDM, so the AP retries, gives up, and TCP backs off.
 * ⚠ Scratch offsets are WORD offsets used as-is (b43_shm_write16 only shifts SHARED byte offsets); the
 *   routing enum is UCODE 0, SHARED 1, SCRATCH 2, HW 3. Not once-per-load: it runs after every
 *   successful chip_init tail, so a re-bring-up that re-uploads the initvals gets it re-applied. */
#define B43_SHM_SCRATCH          2UL
#define B43_SHM_SH_WLCOREREV     0x0016UL
#define B43_SHM_SH_SFFBLIM       0x0044UL
#define B43_SHM_SH_LFFBLIM       0x0046UL
#define B43_SHM_SH_PHYVER        0x0050UL
#define B43_SHM_SH_PHYTYPE       0x0052UL
#define B43_SHM_SH_BEACPHYCTL    0x0054UL
#define B43_SHM_SC_MINCONT       0x0003UL
#define B43_SHM_SC_MAXCONT       0x0004UL
#define B43_SHM_SC_SRLIMIT       0x0006UL
#define B43_SHM_SC_LRLIMIT       0x0007UL
#define B43_TXH_PHY_TXPWR        0xFC00
#define B43_PHYTYPE_G_K194       0x02
static OSStatus gShimMidErr = 1;                 /* 1 = never attempted */

static void ShmWrite16Scratch(UInt32 wordOffset,UInt16 v)
{ ShmControl(B43_SHM_SCRATCH,wordOffset); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,v); }
static UInt16 ShmRead16Scratch(UInt32 wordOffset)
{ ShmControl(B43_SHM_SCRATCH,wordOffset); return ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA); }

/* b43_rate_memory_write: copy the initvals' entry for this rate to the +0x20 slot. */
static void RateMemoryWrite(UInt32 base,UInt8 code)
{
    UInt32 off = base + ((UInt32)(code & 0x0F)) * 2UL;
    ShmWrite16Shared(off + 0x20UL, ShmRead16Shared(off));
}

static void ApShimCoreInitMid(void)
{
    static const UInt8 kOfdm[8] = { 0xB,0xF,0xA,0xE,0x9,0xD,0x8,0xC };   /* 6 9 12 18 24 36 48 54 */
    static const UInt8 kCck[4]  = { 0x0A,0x14,0x37,0x6E };               /* 1 2 5.5 11 */
    static const UInt8 kOfdmMb[8] = { 6,9,12,18,24,36,48,54 };
    UInt32 savedWin = 0;
    int haveSaved, i, bad = 0;
    UInt16 ctl = (UInt16)(B43_TXH_PHY_ENC_CCK | B43_TXH_PHY_ANT01AUTO | B43_TXH_PHY_TXPWR);

    if(gShimTailErr != noErr) return;            /* b43 runs these only after a completed chip_init */
    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,&savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("  [8-2e2] [!!] could not select the 802.11 core -- nothing was written.");
      gShimMidErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    Say("");
    Say("  [8-2e2] b43_wireless_core_init's middle (k194) -- the statements this port had skipped");
    ShmWrite16Shared(B43_SHM_SH_WLCOREREV,(UInt16)gShimCoreRev);
    ShmWrite16Scratch(B43_SHM_SC_SRLIMIT,7);                 /* b43_set_retry_limits(dev, 7, 4) */
    ShmWrite16Scratch(B43_SHM_SC_LRLIMIT,4);
    ShmWrite16Shared(B43_SHM_SH_SFFBLIM,3);
    ShmWrite16Shared(B43_SHM_SH_LFFBLIM,2);
    for(i = 0; i < 8; i++) RateMemoryWrite(0x480UL,kOfdm[i]);  /* b43_rate_memory_init, G-PHY: OFDM... */
    for(i = 0; i < 4; i++) RateMemoryWrite(0x4C0UL,kCck[i]);   /* ...falls through to CCK */
    ShmWrite16Shared(B43_SHM_SH_BEACPHYCTL,ctl);             /* b43_set_phytxctl_defaults */
    ShmWrite16Shared(B43_SHM_SH_ACKCTSPHYCTL,ctl);
    ShmWrite16Shared(B43_SHM_SH_PRPHYCTL,ctl);
    ShmWrite16Scratch(B43_SHM_SC_MINCONT,0xF);               /* not a B-PHY */
    ShmWrite16Scratch(B43_SHM_SC_MAXCONT,0x3FF);
    ShmWrite16Shared(B43_SHM_SH_PHYTYPE,B43_PHYTYPE_G_K194);
    ShmWrite16Shared(B43_SHM_SH_PHYVER,(UInt16)gShimPhyRev);

    /* ── read-backs, every one decoded (CLAUDE.md: a bare hex value is a question) ── */
    { UInt16 v;
      v = ShmRead16Shared(B43_SHM_SH_WLCOREREV); Say1("    WLCOREREV      = ",(unsigned long)v); if(v != (UInt16)gShimCoreRev) bad++;
      v = ShmRead16Scratch(B43_SHM_SC_SRLIMIT);  Say1("    short retries  = ",(unsigned long)v); if(v != 7) bad++;
      v = ShmRead16Scratch(B43_SHM_SC_LRLIMIT);  Say1("    long retries   = ",(unsigned long)v); if(v != 4) bad++;
      v = ShmRead16Shared(B43_SHM_SH_SFFBLIM);   Say1("    short fb limit = ",(unsigned long)v); if(v != 3) bad++;
      v = ShmRead16Shared(B43_SHM_SH_LFFBLIM);   Say1("    long fb limit  = ",(unsigned long)v); if(v != 2) bad++;
      v = ShmRead16Scratch(B43_SHM_SC_MINCONT);  Say1("    CWmin (MINCONT)= ",(unsigned long)v); if(v != 0xF) bad++;
      v = ShmRead16Scratch(B43_SHM_SC_MAXCONT);  Say1("    CWmax (MAXCONT)= ",(unsigned long)v); if(v != 0x3FF) bad++;
      v = ShmRead16Shared(B43_SHM_SH_PHYTYPE);   Say1("    PHYTYPE (2=G)  = ",(unsigned long)v); if(v != B43_PHYTYPE_G_K194) bad++;
      v = ShmRead16Shared(B43_SHM_SH_PHYVER);    Say1("    PHYVER         = ",(unsigned long)v); if(v != (UInt16)gShimPhyRev) bad++;
      v = ShmRead16Shared(B43_SHM_SH_ACKCTSPHYCTL); SayH("    ACK/CTS phyctl = ",(unsigned long)v,4); if(v != ctl) bad++;
      /* The rate entries: the copy's SOURCE comes from the initvals. If a source reads 0 the copy
       * carried nothing, and "no OFDM ACK entries" would still be the state -- so print both. */
      for(i = 0; i < 8; i++){
        UInt32 off = 0x480UL + ((UInt32)kOfdm[i]) * 2UL;
        UInt16 src = ShmRead16Shared(off), dst = ShmRead16Shared(off + 0x20UL);
        Say1("    OFDM rate entry Mbps ",(unsigned long)kOfdmMb[i]);
        SayH("      initvals  = ",(unsigned long)src,4);
        SayH("      ACK entry = ",(unsigned long)dst,4);
        if(dst != src) bad++; } }
    if(bad){ Say1("    [!!] read-backs that did not match: ",(unsigned long)bad); gShimMidErr = paramErr; }
    else   { Say("    [ok] every value read back as written"); gShimMidErr = noErr; }

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* 8-2e: the chip_init tail result. */
UInt32 AirPortShimGetTail(UInt32 *macctl, UInt32 *tmsLow, UInt32 *pretbtt,
                          UInt32 *prmaxtime, SInt32 *err)
{
    if(macctl)    *macctl    = gShimTailMacctl;
    if(tmsLow)    *tmsLow    = gShimTailTmsLow;
    if(pretbtt)   *pretbtt   = (UInt32)gShimTailPretbtt;
    if(prmaxtime) *prmaxtime = (UInt32)gShimTailPrmaxtime;
    if(err)       *err       = (SInt32)gShimTailErr;
    return (UInt32)(gShimTailErr == noErr);
}

/* ★★★ STAGE 8-2f: b43_dma_init's RX HALF -- the driver builds and arms a receive ring.
 *
 * First increment where the driver touches MEMORY rather than registers, and the first where it
 * hands the card a physical address of its own.
 *
 * ⚠⚠ IT ARMS, VERIFIES, THEN STOPS THE ENGINE AGAIN -- deliberately, and this is the whole
 *   coexistence argument. The probe application runs on the same card moments later and builds
 *   its own ring. If the driver returned from Open with the RX engine live and pointing at
 *   gShimRxPool, the card would be writing into driver memory while the app reprogrammed the
 *   same engine underneath it. Leaving DISABLED costs nothing the increment claims -- RXSTATUS
 *   is read back in the ARMED state before the reset, which is the evidence -- and it removes a
 *   window where two owners share a live DMA target. Receiving frames is 8-2g, and 8-2g is where
 *   the ownership question has to be answered properly rather than avoided.
 *
 * ⚠ ALLOCATION HAPPENS AT Open AND NOWHERE ELSE. Apple documents Open as task time and says
 *   allocation is safe there; no other selector carries that promise. NewPtrSysClear from Read or
 *   Start would be allocating below task level -- the same class of error as the File Manager in
 *   an ISR. The build's static audit now checks this BY SELECTOR.
 *
 * ⚠ AND IT FREES AT Close. NewPtrSysClear draws on the SYSTEM heap, which OS 9 never compacts and
 *   never reclaims from a fragment that forgets. A driver that leaks 160 KB per Open would degrade
 *   the machine silently across a day of testing. The free is also guarded: memory is released
 *   only after the RX engine has been reset, because DisposePtr on a buffer the card still holds
 *   a physical address for is memory corruption with no message attached. */
#define AP_SHIM_RX_BUFSZ  (K3_PAGE - B43_DMA0_RX_FW351_FO)

static DmaBlock gShimRxRing, gShimRxPool;
static UInt8   *gShimRxBufLog[K3_RX_SLOTS];
static UInt32   gShimRxBufPhys[K3_RX_SLOTS];
static UInt32   gShimDmaBase = 0, gShimNBufOk = 0;
/* 8-35/8-36: the RX engine's four registers at TWO points in a join attempt. Declared here
 * rather than beside CaptureRxRegs because the census that prints them appears earlier in
 * this file than the join that fills them.
 *
 * ⚠ k128 took ONE snapshot, at the top of the attempt, and its diff therefore spanned the
 *   scan AND the probe AND the auth. k125 proves the scan works when the attempt works -- it
 *   heard a beacon and learned the AP's BSSID -- and that RXSTATUS reads
 *   0x00000000 DISABLED by the time the auth response is waited for. A single snapshot can
 *   say "something changed" across three steps; two snapshots bisect it in one run.
 *
 *   slot 0 = entry to the attempt, before the scan
 *   slot 1 = the scan has just SUCCEEDED, before the first transmit */
static UInt32   gRxRegPre[2][4];
static int      gRxRegPreOk[2] = { 0, 0 };

/* ★★★ 8-36: THE RETURN VALUES THIS FILE HAS BEEN THROWING AWAY ON THE FAILING PATH.
 *
 * CLAUDE.md's own table records this defect once already -- "the RX ring re-arms fine" was
 * believed because RxRingArm's result was cast to void. It is still cast to void at three
 * sites, and all three sit on the path where the receiver is dying:
 *
 *   RxRingArm       says whether the reset-and-reprogram actually took
 *   RxRecycle       returns -1 when the RX pointer is nonsense, and it runs before EVERY
 *                   transmit -- so it is the earliest possible witness to the engine dying,
 *                   and it has been silently discarded every time
 *
 * ⚠ "0 frames recycled" and "refused, the pointer was garbage" are different facts and both
 *   came back as nothing. gRxRecycleLast keeps them apart. */
static UInt32   gRxArmFail     = 0;   /* RxRingArm reported the arm did not take */
static UInt32   gRxReArmed     = 0;   /* 8-44: RX engine was DISABLED at scan; we re-armed it */
static UInt32   gRxReArmStuck  = 0;   /* 8-44: ...and after re-arming it was ACTIVE (the fix took) */
static UInt32   gRxRecycleBad  = 0;   /* RxRecycle refused a nonsense RX pointer */
static SInt32   gRxRecycleLast = 0;   /* and its last return, verbatim */
/* ⚠⚠ TX DOES NOT USE THE RX CONTROLLER, AND k77 LOST A RUN TO THAT. The receive ring lives on
 * DMA controller 0; the transmitter does not. b43 maps controllers 0..3 to AC_BK, AC_BE, AC_VI
 * and AC_VO, and this project settled the question at k9 -- the constant's own comment says
 * "AC_BE is controller 1, and only 1 and 3 moved". Posting to controller 0 produced exactly what
 * k77 saw: descriptors written, TXINDEX rung, and ZERO TX status reports, because nothing on that
 * controller was ever going to transmit.
 *
 * ⚠ It was in the repository the whole time. "Grep the project before theorising about the
 *   platform" found it in one step, after a hardware run had already been spent. */
static UInt32   gShimTxBase = 0;
static UInt32   gShimRingPhys = 0, gShimRingReadback = 0;
static UInt32   gShimRxStatArmed = 0, gShimRxStatFinal = 0;
static int      gShimDmaArmed = 0;
static OSStatus gShimDmaErr = 1;                /* 1 = never attempted */

/* ⚠⚠ STOPPING AND FREEING ARE SEPARATE, AND THE SPLIT IS NOT COSMETIC.
 *
 * The first version of this called DmaBlockFree from EnetHAL_Close, and check-exec-level.py
 * refused the build: Close reaches UnlockMemory and DisposePtr, and Apple documents ONLY Open as
 * task time. Freeing from Close would have been an allocator call at an undocumented execution
 * level -- which on OS 9 does not return an error, it corrupts or hangs.
 *
 * So Close does the part that is safe anywhere -- a register write that stops the engine -- and
 * the release happens at the START of the next Open, which is task time by documentation. At most
 * one allocation is ever outstanding, and the card is never left with a physical address for
 * memory that is about to be released. */
/* Forward-declared: the TX allocations are released here too, but the TX globals are declared
 * with the transmit code further down, where they belong. */
static void ApShimTxFree(void);
static void ApShimTxArm(void);   /* re-arm before every post -- see the note at its definition */

static void ApShimDmaStop(void)
{
    if(gShimDmaBase && gBus.bar0) (void)DmaControllerRxReset(gShimDmaBase);
    gShimDmaArmed = 0;
}

static void ApShimDmaFree(void)
{
    if(!gShimRxRing.raw && !gShimRxPool.raw) return;
    ApShimDmaStop();                 /* the engine must not hold these addresses */
    DmaBlockFree(&gShimRxRing);
    DmaBlockFree(&gShimRxPool);
    ApShimTxFree();                  /* the TX blocks, defined with the TX code below */
    gShimDmaErr = 1;                 /* so a later Open may build a fresh ring */
}

static void ApShimDmaInit(void)
{
    UInt32 i;
    UInt32 savedWin = 0;
    int haveSaved;

    if(gShimDmaErr != 1) return;                /* once per load */
    if(gShimTailErr != noErr){
      Say("");
      Say("  [8-2f] DMA INIT DECLINED -- chip_init did not complete.");
      Say("    ⚠ b43 runs dma_init AFTER chip_init. Read the 8-2e result above.");
      gShimDmaErr = paramErr;
      return; }

    Say("");
    Say("  [8-2f] b43_dma_init -- the RX ring");

    /* ---- memory, at task time, from the system heap ---- */
    if(!DmaBlockAlloc(&gShimRxRing,B43_DMA32_RINGMEMSIZE)){
      Say("    [!!] RX ring allocation FAILED -- system heap exhausted or LockMemory refused.");
      gShimDmaErr = memFullErr; return; }
    if(!DmaBlockAlloc(&gShimRxPool,(UInt32)K3_RX_SLOTS*K3_PAGE)){
      Say("    [!!] RX buffer pool allocation FAILED.");
      DmaBlockFree(&gShimRxRing);
      gShimDmaErr = memFullErr; return; }

    gShimRingPhys = gShimRxRing.phys;
    SayH("    RX ring phys = ",gShimRingPhys,8);

    /* ⚠ EVERY BUFFER IS CHECKED, not assumed. One page each, so each is physically contiguous by
     * construction -- but page-alignment and the 1 GB DMA window are properties of the PHYSICAL
     * address, which GetPhysical is the only thing that knows. A buffer outside the window would
     * be silently un-DMA-able and would look like a dead receiver. */
    gShimNBufOk = 0;
    for(i=0;i<K3_RX_SLOTS;i++){
      gShimRxBufLog[i] = gShimRxPool.base + i*K3_PAGE;
      gShimRxBufPhys[i] = 0;
      if(PhysOfPage(gShimRxBufLog[i],&gShimRxBufPhys[i])
         && (gShimRxBufPhys[i] & 0xFFFUL) == 0
         && InWindow(gShimRxBufPhys[i],AP_SHIM_RX_BUFSZ)) gShimNBufOk++; }
    Say1("    RX buffer pages usable = ",(unsigned long)gShimNBufOk);

    if(gShimNBufOk != K3_RX_SLOTS){
      Say1("    [!!] not every buffer is usable; expected ",(unsigned long)K3_RX_SLOTS);
      Say("      A page that is not 4K-aligned physically, or sits above the 1 GB PCI DMA");
      Say("      window, cannot be a receive target. Partial rings are refused rather than");
      Say("      armed -- an engine with a bad descriptor fails in a way that reads as a dead");
      Say("      receiver three increments later.");
      ApShimDmaFree();
      gShimDmaErr = paramErr; return; }

    /* ---- arm ---- */
    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      ApShimDmaFree(); gShimDmaErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    gShimDmaBase = B43_MMIO_DMA32_BASE0;
    /* ⚠ 8-36: KEPT, not cast away. The read-back below is a good oracle and stays, but this
     * is the function's own verdict on whether the reset reached DISABLED before it
     * reprogrammed -- and believing a re-arm that never reported is precisely the defect
     * CLAUDE.md's prior-art table records against this call. */
    if(!RxRingArm(gShimDmaBase,&gShimRxRing,gShimRxBufLog,gShimRxBufPhys,
                  B43_DMA0_RX_FW351_FO,AP_SHIM_RX_BUFSZ)){
      gRxArmFail++;
      Say("    [!!] ⚠⚠ RxRingArm REPORTED FAILURE -- the RX engine never reached DISABLED");
      Say("         before it was reprogrammed. The read-back below may still look right."); }

    gShimRingReadback = ssb_r32(gBus.bar0,gShimDmaBase+B43_DMA32_RXRING);
    gShimRxStatArmed  = ssb_r32(gBus.bar0,gShimDmaBase+B43_DMA32_RXSTATUS);
    SayH("    RXRING reads back  = ",gShimRingReadback,8);
    SayH("    RXSTATUS armed     = ",gShimRxStatArmed,8);

    /* ⚠ THE READ-BACK IS THE ORACLE, and it is compared against what we WROTE, not against a
     * constant: B43DmaAddressLow applies the SSB translation, so the expected value is derived
     * from the ring's own physical address. A test against a literal would pass on the wrong
     * card. */
    if(gShimRingReadback != B43DmaAddressLow(gShimRingPhys)){
      Say("    [!!] RXRING did not read back as the address we wrote.");
      gShimDmaErr = paramErr;
    } else if((gShimRxStatArmed & B43_DMA32_RXSTATE) == B43_DMA32_RXSTAT_DISABLED){
      Say("    [!!] the engine is still DISABLED after arming.");
      gShimDmaErr = paramErr;
    } else {
      gShimDmaArmed = 1; gShimDmaErr = noErr;
      Say("    [ok] ring armed: RXRING matches the physical address written, engine not"); 
      Say("         DISABLED."); }

    /* ⚠⚠ 8-2g: THE RING NOW STAYS ARMED. 8-2f reset the engine here so the probe application
     * could build its own ring on the same card. It cannot any more -- a receiver has to stay
     * armed with the MAC enabled, so the app has become a pure reporter and the driver keeps the
     * radio. The teardown moved to AirPortShimQuiesce(), which the app calls before it quits.
     *
     * ⚠ THAT CALL IS NOT OPTIONAL AND ITS ABSENCE IS DANGEROUS. If the app exits without it, CFM
     *   may release this fragment while the card is still writing into gShimRxPool -- DMA into
     *   freed system heap, which corrupts silently. Close does it too, so OT tearing the stream
     *   down covers the ordinary path; the explicit call covers the rest. */
    gShimRxStatFinal = gShimRxStatArmed;

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* 8-2f: the RX ring result. */
UInt32 AirPortShimGetDma(UInt32 *ringPhys, UInt32 *ringReadback, UInt32 *nBufOk,
                         UInt32 *rxStatArmed, SInt32 *err)
{
    if(ringPhys)     *ringPhys     = gShimRingPhys;
    if(ringReadback) *ringReadback = gShimRingReadback;
    if(nBufOk)       *nBufOk       = gShimNBufOk;
    if(rxStatArmed)  *rxStatArmed  = gShimRxStatArmed;
    if(err)          *err          = (SInt32)gShimDmaErr;
    return (UInt32)(gShimDmaErr == noErr);
}

/* ★★★ STAGE 8-2g: b43_wireless_core_init's TAIL, THEN THE MAC. The driver becomes a receiver.
 *
 * ⚠⚠ THIS IS WHERE CARD OWNERSHIP CHANGES HANDS, and it is a one-way door. Every increment up to
 *   8-2f could configure the card and hand it back: the probe application then re-ran the whole
 *   bring-up from scratch, which is why both could share one run. A receiver cannot be handed
 *   back -- it has to stay armed with the MAC enabled. So from here the application does not
 *   drive the radio at all. It reports.
 *
 * ⚠ WHAT THAT COSTS, STATED PLAINLY: the app's own association and CCMP oracles do not run in
 *   this build. Fifteen of its thirty-one oracles survive, because ApPhyInitG re-runs them from
 *   inside the driver and the narration carries them. The rest -- authentication, association,
 *   the four-way handshake, decryption -- are unavailable until that work migrates too. This
 *   increment's oracle has to stand on its own, and it does: a frame either arrives in our
 *   buffers with a decodable header or it does not.
 *
 * The five statements are b43_wireless_core_init's, in b43's order, after dma_init. */
static UInt32 gShimPuDelay = 0, gShimKtp = 0, gShimFilter = 0;
static int    gShimNKeys = 0, gShimMacOn = 0;
/* ⚠ STICKY. gShimMacOn goes back to 0 at quiesce, which is correct -- the receiver really is off.
 * But it made a LATE read indistinguishable from a receiver that never started: k73 polled after
 * OT's Close and reported "never attempted" while the driver's own narration showed the MAC had
 * been enabled perfectly. This one is never cleared, so the log can always tell the two apart. */
static int    gShimMacEverOn = 0;
static UInt8  gShimChannel = 1;
static OSStatus gShimRxErr = 1;                 /* 1 = never attempted */

static void ApShimCoreInitTail(void)
{
    UInt32 boardflags;

    if(gShimDmaErr != noErr) return;            /* downstream of the ring */

    Say("");
    Say("  [8-2g] b43_wireless_core_init's remaining five statements");

    QosInitDisabled();                                      /* [1] */
    gShimPuDelay = SetSynthPuDelay();                       /* [2] */
    Say1("    SPUWKUP written = ",(unsigned long)gShimPuDelay);

    boardflags = (UInt32)ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SPROM_BFLLO);
    (void)BluetoothCoextEnable(boardflags);                 /* [3] */

    UploadCardMacAddress(gShimMac);                         /* [4] */
    { Str255 L; L[0]=0; PCat(L,"    card MAC uploaded: ");
      { int i; for(i=0;i<6;i++){ if(i) PCat(L,":"); PCatHex(L,(unsigned long)gShimMac[i],2); } }
      Out(L); }

    SecurityInit(&gShimKtp,&gShimNKeys);                    /* [5] */
    Say1("    key table pointer = ",(unsigned long)gShimKtp);
    Say1("    key slots cleared = ",(unsigned long)gShimNKeys);

    PowerSavingCtlBits();
    Say("    power saving OFF, awake forced -- the chip must never sleep on us");
}

/* ★★★ And the MAC. This is the statement that turns the receiver on. */
static void ApShimMacOn(void)
{
    if(gShimRxErr != 1) return;
    if(gShimDmaErr != noErr){
      Say("");
      Say("  [8-2g] MAC ENABLE DECLINED -- there is no ring to receive into.");
      gShimRxErr = paramErr; return; }

    Say("");
    Say("  [8-2g] channel, filter, then b43_mac_enable");
    SwitchChannel(gShimChannel);
    Say1("    channel = ",(unsigned long)gShimChannel);

    /* ⚠ BEACPROMISC, level 1 -- what mac80211 sets to scan. Level 0 is what b43 leaves behind and
     * hears only frames addressed to us, which on a card that has never associated is nothing at
     * all. Level 2 adds PROMISC and KEEP_BAD and would make "we received something" nearly
     * unfalsifiable. Level 1 is the weakest setting that can possibly succeed, which is the one
     * an honest first receive test wants. */
    gShimFilter = ApplyFilter(B43_MACCTL_BEACPROMISC);
    SayH("    MACCTL after filter = ",gShimFilter,8);

    MacEnable();
    gShimMacOn = 1; gShimMacEverOn = 1;
    gShimRxErr = noErr;
    SayH("    MACCTL after enable = ",(unsigned long)ssb_r32(gBus.bar0,B43_MMIO_MACCTL),8);
    Say("    ★ the receiver is live and the ring stays armed. The app is a reporter now.");
}

/* ── 8-2g: the app asks the driver what it heard ──
 *
 * ⚠ POLLED FROM THE APPLICATION, AT TASK LEVEL, BY DESIGN. This driver still has no interrupt
 *   handler -- DriverServicesLib is deliberately absent -- so nothing here may run below task
 *   level. The app calls this after Open has returned; RxScan only reads our own buffers and
 *   touches no allocator and no trap, so it is safe from where it is called.
 *
 * Returns the number of slots whose poison is gone. firstSlot is the first such slot, or -1. */
UInt32 AirPortShimRxPoll(SInt32 *firstSlot, UInt32 *rxStatus, UInt32 *rxIndex)
{
    int first = -1;
    UInt32 n;
    if(gShimRxErr != noErr){
      if(firstSlot) *firstSlot = -1;
      if(rxStatus)  *rxStatus  = 0;
      if(rxIndex)   *rxIndex   = 0;
      return 0; }
    n = (UInt32)RxScan(gShimRxBufLog,B43_DMA0_RX_FW351_FO,&first);
    if(firstSlot) *firstSlot = (SInt32)first;
    if(rxStatus)  *rxStatus  = ssb_r32(gBus.bar0,gShimDmaBase+B43_DMA32_RXSTATUS);
    if(rxIndex)   *rxIndex   = ssb_r32(gBus.bar0,gShimDmaBase+B43_DMA32_RXINDEX);
    return n;
}

/* Copy one received buffer out, so the app can decode it without reaching into driver memory. */
UInt32 AirPortShimGetRxBuf(UInt32 slot, UInt8 *out, UInt32 max)
{
    UInt32 i, n;
    if(gShimRxErr != noErr || slot >= (UInt32)K3_RX_SLOTS || !out) return 0;
    n = (max < K3_PAGE) ? max : K3_PAGE;
    for(i=0;i<n;i++) out[i] = gShimRxBufLog[slot][i];
    return n;
}

/* ⚠⚠ THE TEARDOWN THE APP USED TO DO FOR ITSELF. It must happen before this fragment can be
 *   released, because the card holds physical addresses for gShimRxPool: MAC suspended, both
 *   engines reset, ring registers ZEROED. b43's own comment on the equivalent path is that the
 *   card must not be left pointing at pages that are about to go away. */
UInt32 AirPortShimQuiesce(void)
{
    if(!gShimMacOn && gShimDmaErr != noErr) return 0;
    if(gShimMacOn){ (void)MacSuspend(); gShimMacOn = 0; }
    if(gShimDmaBase && gBus.bar0){
      /* ⚠ the TX engine is on its own controller -- see the note above gShimTxBase */
      if(gShimTxBase){
        (void)DmaControllerTxReset(gShimTxBase);
        ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXRING,0); }
      (void)DmaControllerRxReset(gShimDmaBase);
      ssb_w32(gBus.bar0,gShimDmaBase+B43_DMA32_RXRING,0); }
    gShimDmaArmed = 0;
    gShimRxErr = 1;
    return 1;
}

/* 8-2g: the receive state, for the app's oracle. */
UInt32 AirPortShimGetRx(UInt32 *channel, UInt32 *filter, UInt32 *macOn,
                        UInt32 *everOn, SInt32 *err)
{
    if(channel) *channel = (UInt32)gShimChannel;
    if(filter)  *filter  = gShimFilter;
    if(macOn)   *macOn   = (UInt32)gShimMacOn;
    if(everOn)  *everOn  = (UInt32)gShimMacEverOn;   /* survives quiesce; see the note above */
    if(err)     *err     = (SInt32)gShimRxErr;
    return (UInt32)(gShimRxErr == noErr);
}

/* ★★★ STAGE 8-3a: b43_dma_init's TX HALF. The driver transmits.
 *
 * ⚠ THE ORACLE IS AN ACK FROM A REAL ACCESS POINT, not a status register. DrainTxStatus decodes
 *   the microcode's own report -- acked, retries, supp_reason -- and `acked` means some AP on the
 *   air received our frame, checked its FCS and answered within a SIFS. TXSTATUS alone would only
 *   say "the MAC believes it transmitted", which Stage 5 learned the hard way is compatible with
 *   nothing leaving the antenna: k18 found the PHY raising TXERR on every transmission while the
 *   TXSTAT bits looked textbook.
 *
 * ⚠⚠ IT AIMS AT A BSSID IT HEARD ITSELF. A broadcast frame is never acked, so the oracle needs a
 *   unicast destination -- and the only address the driver legitimately has is one it pulled out
 *   of a beacon it received in 8-2g. That is why ParseRxBuffer came across with the TX layer.
 *   No BSSID is compiled in and none is guessed.
 *
 * ⚠ gTxhShift IS SET FROM THE FIRMWARE THE CARD LOADED, not assumed. rev >= 410 means the
 *   FW_HDR_410 layout and four extra bytes ahead of the cookie. Getting this wrong is what cost
 *   Stage 5 most of its schedule -- the microcode read a 106-byte header as its own 82-byte
 *   layout and nothing decodable ever left the antenna. */
static DmaBlock gShimTxRing, gShimTxPool;
static UInt8   *gShimTxHdr = 0, *gShimTxFrm = 0;
static UInt32   gShimTxHdrPhys = 0, gShimTxFrmPhys = 0;
static UInt8    gShimTgtBssid[6];
static int      gShimHaveBssid = 0, gShimTxAcked = 0, gShimTxRetries = 0;

/* ★★★ k210: THE RUNTIME TARGET -- the network to join, set from the known-networks store (ap_known.h), not
 * compiled in. gTargetSsid replaces the old K10_TARGET_SSID everywhere the driver matches or names its
 * network; gTargetPmk replaces the PBKDF2-of-a-compiled-passphrase. Without a target (gHaveTarget = 0) the
 * driver runs but joins nothing -- it scans, publishes the list, and waits for the panel to choose one.
 * ⚠ gTargetPmk is password-equivalent for gTargetSsid; it is never logged and never leaves this fragment. */
static UInt8    gTargetSsid[32];
static UInt8    gTargetLen  = 0;
static UInt8    gTargetPmk[32];
static UInt8    gTargetSec  = 0;            /* kApSec* (only kApSecWpa2Psk joined in k210) */
static int      gHaveTarget = 0;
static ApKnownDb gKnownDb;                  /* loaded from Preferences at boot, and on a join command */
static int      gKnownLoaded = 0;
static UInt32   gKnownCount = 0, gKnownLoadErr = 0, gKnownNoFile = 0;   /* snapshot: bt_keyfile's gKfNoFile lesson */
static UInt32   gShimTxRingReadback = 0;
static SInt32   gShimTxSupp = -1;
static OSStatus gShimTxErr = 1;                 /* 1 = never attempted */

/* ★★★ k195: THE DATA-PATH TX RING -- positions, buffers and reclaim live in ap_txring.h (host-tested
 * by txring_test.c). The JOIN keeps gShimTxHdr/gShimTxFrm and its one-frame reset idiom (ApShimTxArm,
 * which also clears gTxrUp so the ring restarts after any join frame); DATA frames go through these.
 * Allocated in ApShimTxInit (task time, with the other TX blocks), freed in ApShimTxFree. */
_Static_assert(106UL + TXH_FMT_410_EXTRA <= AP_TXR_HDRMAX, "k195: the largest txhdr must fit a ring buffer's header area");
_Static_assert(AP_TXR_DESCS == K8_TX_SLOTS, "k195: the ring's DTABLEEND must land where Op32FillDescriptorFull puts it");
static DmaBlock gTxrPool;                                   /* AP_TXR_BUFS/2 pages, two buffers per page */
static UInt8    gTxStage[AP_TXR_FRMMAX] __attribute__((aligned(16)));   /* k200: encrypt here, then copy */
static UInt8   *gTxrHdr[AP_TXR_BUFS], *gTxrFrm[AP_TXR_BUFS];
static UInt32   gTxrHdrPhys[AP_TXR_BUFS], gTxrFrmPhys[AP_TXR_BUFS];
static ApTxRing gTxr;
static int      gTxrReady = 0;       /* the pool is allocated and every page is inside the DMA window */
static int      gTxrUp    = 0;       /* the engine was set up for ring use since the last join frame */
/* Counters, printed by the snapshot. gTxrOccHist is the k194 discriminator: a frame posted while
 * another was still in flight is one k194 would have overwritten (one buffer, reset per frame). */
static UInt32   gTxrStarts = 0, gTxrStartFail = 0, gTxrResetFail = 0, gTxrPosts = 0, gTxrFull = 0;
static UInt32   gTxrBadPtr = 0, gTxrStopped = 0, gTxrUsedMax = 0;
static UInt32   gTxrOccHist[6];      /* frames already in flight at each post: 0, 1, 2-3, 4-7, 8-15, 16+ */
static UInt32   gTxrStartStatus = 0, gTxrBadStatus = 0, gTxrStopStatus = 0, gTxrErrStatus = 0;

/* Two buffers per page, each page's physical address looked up on its own: the pages of one block
 * need not be physically contiguous, which is why the RX buffers get a page each. */
static int ApShimTxrAlloc(void)
{
    UInt32 b;
    gTxrReady = 0; gTxrUp = 0;
    if(!DmaBlockAlloc(&gTxrPool,(AP_TXR_BUFS / 2UL) * K3_PAGE)){ DmaBlockFree(&gTxrPool); return 0; }
    for(b = 0; b < AP_TXR_BUFS; b++){
      UInt8 *pg  = gTxrPool.base + (b / 2UL) * K3_PAGE;
      UInt32 off = (b % 2UL) * AP_TXR_BUFBYTES, pphys = 0;
      if(!PhysOfPage(pg,&pphys) || !InWindow(pphys,K3_PAGE)){ DmaBlockFree(&gTxrPool); return 0; }
      gTxrHdr[b] = pg + off;                  gTxrHdrPhys[b] = pphys + off;
      gTxrFrm[b] = pg + off + AP_TXR_HDRMAX;  gTxrFrmPhys[b] = pphys + off + AP_TXR_HDRMAX;
    }
    gTxrReady = 1;
    return 1;
}

/* ⚠⚠ MATCH THE TARGET SSID. DO NOT TAKE THE FIRST BEACON THAT PARSES.
 *
 * k79 authenticated against whichever BSSID happened to decode first, and the app's own code
 * refuses to do exactly that:
 *
 *     ⛔ SKIPPED. The scan did not find the target SSID, so we have no BSSID to
 *        authenticate against and would be guessing at the address.
 *
 * A probe request survives the guess -- any AP acknowledges a unicast frame addressed to it -- so
 * 8-3a passed on a neighbour's BSSID and told us nothing was wrong. Authentication does not: the
 * BSSID changed between runs (the radio nearby advertises several), and asking a stranger's
 * access point to authenticate us is not a test of this driver.
 */
static int ApShimPickBssid(void)
{
    UInt32 i;
    UInt8 ssid[34], chan = 0, jssi = 0, ssidLen = 0;
    UInt8 bssid[6];
    UInt16 fc = 0, flen = 0;
    if(gShimHaveBssid) return 1;
    if(gTargetLen == 0) return 0;      /* ⚠ k210: no target -> match nothing. Without this, gTargetLen 0
                                        * would match a HIDDEN network (ssidLen 0) and try to join it. */
    for(i = 0; i < (UInt32)K3_RX_SLOTS; i++){
      if(RxBufferIsPoisoned(gShimRxBufLog[i],B43_DMA0_RX_FW351_FO)) continue;
      ssidLen = 0;
      if(!ParseRxBuffer(gShimRxBufLog[i],B43_DMA0_RX_FW351_FO,
                        bssid,ssid,&ssidLen,&chan,&fc,&flen,&jssi)) continue;
      if(ssidLen != gTargetLen) continue;
      { int k, same = 1;
        for(k = 0; k < (int)gTargetLen; k++)
          if(ssid[k] != gTargetSsid[k]){ same = 0; break; }
        if(!same) continue; }
      { int k; for(k=0;k<6;k++) gShimTgtBssid[k] = bssid[k]; }
      gShimHaveBssid = 1;
      return 1; }
    return 0;
}

static void ApShimTxInit(void)
{
    UInt32 savedWin = 0, ae, v;
    int haveSaved;
    UInt16 flen;

    if(gShimTxErr != 1) return;                 /* once per load */
    if(gShimRxErr != noErr){
      Say("");
      Say("  [8-3a] TX DECLINED -- the receiver is not up.");
      gShimTxErr = paramErr; return; }

    Say("");
    Say("  [8-3a] b43_dma_init -- the TX half (allocation and engine only)");

    if(!DmaBlockAlloc(&gShimTxRing,B43_DMA32_RINGMEMSIZE)){
      Say("    [!!] TX ring allocation FAILED."); gShimTxErr = memFullErr; return; }
    /* Two pages: the txhdr on one, the frame on the other, so each has its own guaranteed
     * contiguous physical address -- the same reason the RX buffers get a page each. */
    if(!DmaBlockAlloc(&gShimTxPool,2UL*K3_PAGE)){
      Say("    [!!] TX buffer allocation FAILED.");
      DmaBlockFree(&gShimTxRing); gShimTxErr = memFullErr; return; }
    gShimTxHdr = gShimTxPool.base;
    gShimTxFrm = gShimTxPool.base + K3_PAGE;
    if(!PhysOfPage(gShimTxHdr,&gShimTxHdrPhys) || !PhysOfPage(gShimTxFrm,&gShimTxFrmPhys)
       || !InWindow(gShimTxHdrPhys,K3_PAGE) || !InWindow(gShimTxFrmPhys,K3_PAGE)){
      Say("    [!!] a TX page is outside the 1 GB DMA window -- refusing to arm.");
      DmaBlockFree(&gShimTxRing); DmaBlockFree(&gShimTxPool);
      gShimTxErr = paramErr; return; }
    /* ★ k195: the data-path ring's 32 buffers (16 pages). Failing here is failing to arm, like the
     * two blocks above: there is no one-buffer data path to fall back to any more. */
    if(!ApShimTxrAlloc()){
      Say("    [!!] TX ring buffers: allocation FAILED or a page is outside the DMA window -- refusing to arm.");
      DmaBlockFree(&gShimTxRing); DmaBlockFree(&gShimTxPool);
      gShimTxErr = memFullErr; return; }
    Say1("    TX ring buffers    = ",(unsigned long)AP_TXR_BUFS);
    Say1("    TX ring positions  = ",(unsigned long)AP_TXR_POS);

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      DmaBlockFree(&gShimTxRing); DmaBlockFree(&gShimTxPool);
      DmaBlockFree(&gTxrPool); gTxrReady = 0;                  /* k195 */
      gShimTxErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    /* ⚠ The txhdr layout follows the firmware the card actually loaded. */
    gTxhShift = (gShimFwRev >= 410) ? TXH_FMT_410_EXTRA : 0;
    Say1("    ucode rev = ",(unsigned long)gShimFwRev);
    Say1("    txhdr shift = ",(unsigned long)gTxhShift);

    gShimTxBase = B43_MMIO_DMA32_BASE0 + (UInt32)K10_TXC*0x20UL;
    SayH("    TX controller = ",gShimTxBase,3);
    (void)DmaControllerTxReset(gShimTxBase);
    ae = B43DmaAddressExt(gShimTxRing.phys);
    v  = B43_DMA32_TXENABLE | ((ae<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXCTL,v);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXRING,B43DmaAddressLow(gShimTxRing.phys));
    gShimTxRingReadback = ssb_r32(gBus.bar0,gShimTxBase+B43_DMA32_TXRING);
    SayH("    TX ring phys      = ",gShimTxRing.phys,8);
    SayH("    TXRING reads back = ",gShimTxRingReadback,8);

    if(gShimTxRingReadback != B43DmaAddressLow(gShimTxRing.phys)){
      Say("    [!!] TXRING did not read back as the address we wrote.");
      gShimTxErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return; }

    /* ⚠⚠ 2, NOT 0. noErr IS 0, and "armed" must not be the same value as "an AP acknowledged
     * our frame" -- AirPortShimGetTx returns (gShimTxErr == noErr), so a 0 here would have
     * reported success for a frame that was never sent. A sentinel that collides with a real
     * result is how a false pass gets manufactured; this project has the rule written down for
     * gShimFwErr and it applies just as much here.
     *   1 = never attempted   2 = armed, waiting for TxProbe   noErr = ACKed   else = failed */
    gShimTxErr = 2;
    Say("    [ok] TX ring armed. The frame itself waits for the app to call TxProbe.");

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
}

/* ★★★ 8-3a-b: THE EXCHANGE ITSELF, CALLED BY THE APP AFTER ITS DWELL.
 *
 * ⚠⚠ k76 PUT THIS INSIDE Open AND IT WAS STRUCTURALLY IMPOSSIBLE. ApShimTxInit ran microseconds
 *   after ApShimMacOn, so every RX buffer was still poisoned and there was no BSSID to aim at.
 *   The driver declined correctly and the log said "no beacon has been parsed yet" -- while the
 *   app, three seconds later, reported 32 filled slots and a frame that parses. Both true; the
 *   question was asked before the answer could exist.
 *
 * ⚠ SO THE TIMING BELONGS TO THE APP AND THE WORK BELONGS TO THE DRIVER. The app knows when it
 *   has dwelled; the driver knows how to transmit. Same split as AirPortShimRxPoll.
 *
 * ⚠ AND THE ALLOCATION STAYED AT Open, deliberately. Apple documents only Open as task time, so
 *   NewPtrSysClear and LockMemory must happen there -- check-exec-level.py enforces it by
 *   selector. What moved here is register work and a memcpy, which are safe from a task-level
 *   call like this one. */
UInt32 AirPortShimTxProbe(void)
{
    UInt32 savedWin = 0;
    int haveSaved;
    UInt16 flen;

    if(gShimTxErr != 2) return 0;               /* not armed, or already run */
    if(!ApShimPickBssid()){
      Say("");
      Say("  [8-3a] no beacon has parsed yet, so there is still no BSSID to aim at.");
      Say("    ⚠ RECEIVE result, not a transmit one -- read the 8-2g frame count.");
      gShimTxErr = paramErr; return 0; }

    Say("");
    Say("  [8-3a] transmitting a unicast probe request");
    { Str255 L; int k; L[0]=0; PCat(L,"    target BSSID (from a beacon we heard) = ");
      for(k=0;k<6;k++){ if(k) PCat(L,":"); PCatHex(L,(unsigned long)gShimTgtBssid[k],2); }
      Out(L); }

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      gShimTxErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    Say("    building a unicast probe request...");
    flen = BuildProbeRequest(gShimTxFrm,gShimMac,gShimTgtBssid,
                             (const char *)gTargetSsid,gTargetLen);
    /* ⚠ SIGNATURE READ FROM THE SOURCE, NOT GUESSED: (txh, frame, frameLen, channel, cookie,
     * antSel, ackReq). antSel 0 is b43's default antenna; ackReq 1 is what makes the AP answer,
     * and an ACK is this increment's entire oracle -- a unicast frame with ackReq 0 would be
     * transmitted and never acknowledged, which would look exactly like a broken transmitter. */
    /* ⚠ B43_TXH_PHY_ANT01AUTO, not 0. The app's comment on the identical call says this is
     * "exactly what the directed probe request used, which we know draws an ACK from this AP". */
    GenerateTxHdr351(gShimTxHdr,gShimTxFrm,flen,gShimChannel,0xC000,B43_TXH_PHY_ANT01AUTO,1);
    ApShimTxArm();
    Say1("    frame length = ",(unsigned long)flen);
    DmaPublish();
    PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                  gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,flen);

    /* ★ THE ORACLE. An ACK means a real AP received this frame and answered within a SIFS. */
    gShimTxAcked = DrainTxStatus(0xC000,600);
    gShimTxRetries = gTxStatCount;
    gShimTxSupp = gTxStatSupp;
    Say1("    TX status reports seen = ",(unsigned long)gTxStatCount);
    Say1("    ACKED                  = ",(unsigned long)gShimTxAcked);
    Say1("    supp_reason            = ",(unsigned long)(gShimTxSupp < 0 ? 0 : gShimTxSupp));
    gShimTxErr = gShimTxAcked ? noErr : paramErr;
    Say(gShimTxAcked ? "    [ok] ★ AN ACCESS POINT ACKNOWLEDGED OUR FRAME."
                     : "    [!!] no ACK -- see the status decode above.");

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);

    /* ⚠⚠ 8-36: THIS FUNCTION FELL OFF THE END WITHOUT A RETURN, AND IT IS EXPORTED.
     *
     * UInt32, three early returns, and the success path returned whatever happened to be in
     * r3 -- here, ExpMgrConfigWriteLong's result. The app resolves this symbol by name and
     * reads the answer (airport_rx.c prints "ARMED BUT NEVER FIRED" when it does not like
     * it), so a probe that transmitted perfectly could report failure, and one that never
     * transmitted could report success, depending on a PCI config write.
     *
     * ⚠ gcc has printed "control reaches end of non-void function" for this on every build.
     *   It sat one line above the noise floor of the unused-variable warnings. Same family as
     *   the (void) casts this build removes: a status that nobody actually returns. */
    return (UInt32)(gShimTxErr == noErr);
}

static void ApShimTxFree(void)
{
    if(!gShimTxRing.raw && !gShimTxPool.raw && !gTxrPool.raw) return;
    DmaBlockFree(&gShimTxRing);
    DmaBlockFree(&gShimTxPool);
    DmaBlockFree(&gTxrPool); gTxrReady = 0; gTxrUp = 0;       /* k195: the data-path ring's buffers */
    gShimTxErr = 1; gShimHaveBssid = 0;
}

/* ⚠⚠ RE-ARM BEFORE EVERY POST, AND k79 LOST A RUN TO NOT DOING IT.
 *
 * PostTxFrameAt rings the doorbell by writing TXINDEX = (slot+2)*8. Posting a second frame at
 * slot 0 writes the SAME value, which is not a change, so the engine never sees new work. k79's
 * probe request went out (TXINDEX 0 -> 16) and its auth request did not (16 -> 16): one frame
 * ACKed, the next not acknowledged and never answered, on the same path to the same address.
 *
 * ⚠ The app resets the TX controller before EVERY post -- four call sites, four resets -- and
 *   re-programs TXCTL and TXRING afterwards because the reset clears them. That is the sequence
 *   copied here, by calling the same functions.
 *
 * ⚠ RxRecycle comes first, as it does in the app: hand back consumed receive slots BEFORE
 *   transmitting, so the answer has somewhere to land. */
static void ApShimTxArm(void)
{
    UInt32 ax, v;
    /* ⚠ 8-36: THE RESULT IS KEPT NOW. This call reads RXSTATUS and refuses with -1 when the
     * RX descriptor pointer is out of range, which makes it the first thing in the whole
     * driver to notice the receive engine going away -- and it ran before every transmit for
     * eight builds with its answer thrown on the floor. */
    gRxRecycleLast = (SInt32)RxRecycle(gShimDmaBase,&gShimRxRing,gShimRxBufLog,gShimRxBufPhys,
                                       B43_DMA0_RX_FW351_FO,AP_SHIM_RX_BUFSZ,0,0);
    if(gRxRecycleLast < 0) gRxRecycleBad++;
    (void)DmaControllerTxReset(gShimTxBase);
    ax = B43DmaAddressExt(gShimTxRing.phys);
    v  = B43_DMA32_TXENABLE | ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXCTL,v);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXRING,B43DmaAddressLow(gShimTxRing.phys));
    /* ★ k195: this reset just threw away the data ring's engine state (and this post is about to
     * write descriptors 0/1), so the next DATA frame must start the ring again. The join is the only
     * caller since k195 -- ApShimWriteFrame no longer comes through here. */
    gTxrUp = 0;
}

/* ★★★ k195: START THE DATA RING -- b43's dma_init for the TX half, run once per join instead of once
 * per frame. The same reset / enable / ring-base sequence ApShimTxArm has always used (and
 * DmaControllerTxReset waits for the engine to go quiet first, so a join frame still in flight
 * completes), WITHOUT ApShimTxArm's RxRecycle: that poisons every unread receive slot, and running it
 * per data frame threw away whatever the interrupt pump had not reached yet. The ring then starts
 * where the ENGINE says it is (TXDPTR), not where we assume a reset leaves it. */
static int ApShimTxrStart(void)
{
    UInt32 ax, v, st;
    if(!gTxrReady){ gTxrStartFail++; return 0; }
    gTxrStarts++;
    if(!DmaControllerTxReset(gShimTxBase)) gTxrResetFail++;   /* counted; the join has always carried on */
    ax = B43DmaAddressExt(gShimTxRing.phys);
    v  = B43_DMA32_TXENABLE | ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXCTL,v);
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXRING,B43DmaAddressLow(gShimTxRing.phys));
    st = ssb_r32(gBus.bar0,gShimTxBase+B43_DMA32_TXSTATUS);
    gTxrStartStatus = st;
    if(!ApTxrStartAt(&gTxr,(st & B43_DMA32_TXDPTR) / B43_DMADESC32_BYTES)){ gTxrStartFail++; return 0; }
    gTxrUp = 1;
    return 1;
}

/* ★★★ k195: IS THERE A RING POSITION FOR THIS FRAME? Reclaim what the engine has finished -- from its
 * own pointer -- then check room. Two conditions restart the ring, each counted, and the frames it
 * held are lost: the engine STOPPED (a DMA error halts it), or its pointer lies outside the frames we
 * posted (which only a reset we did not make could cause). Failing open: a ring that cannot explain
 * the engine re-synchronises instead of refusing every later frame. */
static int ApShimTxrReady(void)
{
    UInt32 st, occ;
    if(!gTxrUp && !ApShimTxrStart()) return 0;
    st = ssb_r32(gBus.bar0,gShimTxBase+B43_DMA32_TXSTATUS);
    if((st & B43_DMA32_TXERROR) && !gTxrErrStatus) gTxrErrStatus = st;       /* the first one, decoded later */
    if((st & B43_DMA32_TXSTATE) == B43_DMA32_TXSTAT_STOPPED){
      gTxrStopped++; gTxrStopStatus = st;
      if(!ApShimTxrStart()) return 0;
      st = ssb_r32(gBus.bar0,gShimTxBase+B43_DMA32_TXSTATUS); }
    if(ApTxrReclaim(&gTxr,(st & B43_DMA32_TXDPTR) / B43_DMADESC32_BYTES) < 0){
      gTxrBadPtr++; gTxrBadStatus = st;
      if(!ApShimTxrStart()) return 0; }
    if(!ApTxrHasRoom(&gTxr)){ gTxrFull++; return 0; }
    occ = gTxr.used;
    gTxrOccHist[occ == 0 ? 0 : occ == 1 ? 1 : occ < 4 ? 2 : occ < 8 ? 3 : occ < 16 ? 4 : 5]++;
    return 1;
}

/* ★ k195: the ring's doorbell barrier. DmaPublish is an eieio, which the architecture defines as
 * ordering accesses to CACHING-INHIBITED storage; the txhdr, the frame and the descriptors are
 * cacheable stores the card reads by snooped DMA. Through k194 a >= 1 ms TX-engine reset sat between
 * those stores and the TXINDEX write, so their order never came up; the ring removes that gap. `sync`
 * orders every prior store, cacheable or not, ahead of the MMIO store -- it is what Linux's PPC32
 * out_le32 emits in front of the same stwbrx. Strictly stronger than eieio; the TX doorbell only. */
static inline void ApTxDoorbellBarrier(void) { __asm__ __volatile__("sync" ::: "memory"); }

/* 8-3a: the transmit result. */
UInt32 AirPortShimGetTx(UInt32 *ringPhys, UInt32 *ringReadback, UInt32 *acked,
                        UInt32 *statCount, SInt32 *err)
{
    if(ringPhys)     *ringPhys     = gShimTxRing.phys;
    if(ringReadback) *ringReadback = gShimTxRingReadback;
    if(acked)        *acked        = (UInt32)gShimTxAcked;
    if(statCount)    *statCount    = (UInt32)gShimTxRetries;
    if(err)          *err          = (SInt32)gShimTxErr;
    return (UInt32)(gShimTxErr == noErr);
}

/* The BSSID the driver aimed at, so the app can report it. */
UInt32 AirPortShimGetBssid(UInt8 *out)
{
    int i;
    if(!out || !gShimHaveBssid) return 0;
    for(i=0;i<6;i++) out[i] = gShimTgtBssid[i];
    return 1;
}

/* ★★★ STAGE 8-3b: AUTHENTICATION. The driver's first two-way exchange.
 *
 * ★ THE ORACLE IS THE AP'S OWN ANSWER: an authentication frame back, type 0 subtype 11, from the
 *   BSSID we addressed, to our MAC, with SEQUENCE 2 and STATUS 0. Every one of those is a field
 *   the access point chose -- nothing in this driver can manufacture them.
 *
 * ⚠ SEQUENCE 2 IS CHECKED SEPARATELY FROM STATUS, and that is not pedantry. An auth frame from
 *   the target addressed to us that is NOT sequence 2 is some other exchange, and reading its
 *   status as our answer would be reading someone else's mail. The app's own code carries that
 *   warning and it comes across with the logic.
 *
 * ⚠ AND AN ACK IS NOT AN ANSWER. 8-3a proved the AP acknowledges our frames at the MAC layer;
 *   that is a different thing from agreeing to authenticate. An ACKed auth request that draws no
 *   response means the AP received it and chose silence -- MAC filtering is the usual reason --
 *   and the oracle says so rather than blaming the transmitter.
 *
 * ⚠ THE CONSTANTS WERE ALREADY THERE. I wrote a private AP_AUTH_SEQ_RESPONSE before noticing
 *   that ap_tx.h already carries AUTH_ALG_OPEN, AUTH_SEQ_REQUEST, AUTH_SEQ_RESPONSE and
 *   AUTH_STATUS_SUCCESS -- they came across in the 8-3a extraction. That is the same miss as
 *   k77, where K10_TXC's own comment named the TX controller and I used the RX base anyway, and
 *   it cost a hardware run. Reading what you move is part of moving it. */

static UInt16 gShimAuthAlg = 0, gShimAuthSeq = 0, gShimAuthStatus = 0xFFFF;
static UInt32 gShimAuthMs = 0, gShimAuthSeen = 0;
static int    gShimAuthAcked = 0, gShimAuthGot = 0;
static OSStatus gShimAuthErr = 1;               /* 1 = never attempted */

/* ★★★ WHAT THE WAIT ACTUALLY SAW -- k84.
 *
 * Printed on every FAILED RxWaitFor, and it exists because "frames walked = 26" was the whole
 * of k83's evidence and it could not answer the only question worth asking: was the frame we
 * wanted in the ring, or was it never in memory at all? Each line below turns a count into a
 * decoded fact, which is CLAUDE.md's first rule applied to something that is not a register.
 *
 * ⚠ READ backlog-at-entry FIRST. It is the discriminator between the two live theories:
 *     near K3_RX_SLOTS  -- the engine lapped our cursor; the walk fix in ap_wait.h is the cause
 *                          and this run is its control
 *     small, and a NEAR MISS is listed -- the AP answered from a BSSID we filtered out
 *     small, and no near miss, census shows beacons only -- the AP genuinely stayed silent,
 *                          and the ring is exonerated for the first time */
static void ApShimSayRxCensus(const char *what)
{
    Str255 L;
    int i, n;
    Say("");
    Say1("      [census] what the wait saw -- frames walked = ",
         (unsigned long)(gRxCensusType[0]+gRxCensusType[1]+gRxCensusType[2]+gRxCensusType[3]));
    L[0]=0; PCat(L,"      ring: backlog at entry = "); PCatDec(L,(unsigned long)gRxBacklogIn);
    PCat(L," of "); PCatDec(L,(unsigned long)K3_RX_SLOTS);
    PCat(L,", at exit = "); PCatDec(L,(unsigned long)gRxBacklogOut); Out(L);
    L[0]=0; PCat(L,"      cursor slot in = "); PCatDec(L,(unsigned long)gRxSlotIn);
    PCat(L,", out = "); PCatDec(L,(unsigned long)gRxSlot); Out(L);
    /* RXSTATUS decoded, never bare -- the k20 note in ap_dma.h is about this exact register. */
    for(i=0;i<2;i++){
      UInt32 v = i ? gRxStatusOut : gRxStatusIn;
      UInt32 st = v & B43_DMA32_RXSTATE;
      L[0]=0; PCat(L,i ? "      RXSTATUS out = " : "      RXSTATUS in  = ");
      PCat(L,"0x"); PCatHex(L,v,8);
      PCat(L,"  state ");
      PCat(L, st==B43_DMA32_RXSTAT_DISABLED ? "DISABLED" :
              st==B43_DMA32_RXSTAT_ACTIVE   ? "ACTIVE"   :
              st==B43_DMA32_RXSTAT_IDLEWAIT ? "IDLEWAIT" :
              st==B43_DMA32_RXSTAT_STOPPED  ? "STOPPED"  : "?");
      PCat(L,", RXDPTR slot ");
      PCatDec(L,(unsigned long)((v & B43_DMA32_RXDPTR)/B43_DMADESC32_BYTES));
      if(v & B43_DMA32_RXERROR){ PCat(L,"  ⚠ RXERROR 0x");
        PCatHex(L,(v & B43_DMA32_RXERROR)>>16,1); }
      Out(L); }
    /* ★★★★★ 8-32: MACCTL, READ LIVE, AT THE MOMENT THE WAIT ENDS.
     *
     * k122's census is the reason this is here. The engine read ACTIVE, the whole ring was handed
     * back, BEACPROMISC is set once at 8-2g and nothing in this driver ever clears it -- and the
     * wait still walked ONE frame in 500 ms, a probe response, with no beacons at all. Beacons
     * arrive every ~100 ms and BEACPROMISC bypasses the BSSID match entirely, so five or so
     * should have been walked. The AP heard us well enough to ACK twice. Transmit works and
     * receive does not, which no filter setting in this driver explains.
     *
     * "Nothing in this driver clears it" is a claim about the source; MACCTL is a claim about the
     * chip. Firmware, a PHY operation or the MAC itself can move bits underneath us, and the two
     * claims have never been compared. One register read settles it -- and reports the whole
     * word decoded rather than a bare number, which is this project's own rule about status
     * registers. */
    /* ★ 8-36: the RX engine at THREE points, decoded, so a change can be LOCATED and not
     * merely noticed. k128 printed two columns across three steps and could only have said
     * "something moved it". */
    if(gRxRegPreOk[0] || gRxRegPreOk[1]){
      static const char *kRxRegName[4] = { "RXCTL   ", "RXRING  ", "RXINDEX ", "RXSTATUS" };
      static const UInt32 kRxRegOff[4] = { B43_DMA32_RXCTL, B43_DMA32_RXRING,
                                           B43_DMA32_RXINDEX, B43_DMA32_RXSTATUS };
      int r;
      Say("      RX engine at three points of this attempt:");
      Say("        entry = before the scan | scan-ok = a beacon was just heard | now = wait ended");
      for(r=0;r<4;r++){
        UInt32 nowv = ssb_r32(gBus.bar0, gShimDmaBase + kRxRegOff[r]);
        L[0]=0; PCat(L,"        "); PCat(L,(char*)kRxRegName[r]);
        PCat(L,"  entry ");
        if(gRxRegPreOk[0]){ PCat(L,"0x"); PCatHex(L,gRxRegPre[0][r],8); } else PCat(L,"----------");
        PCat(L,"  scan-ok ");
        if(gRxRegPreOk[1]){ PCat(L,"0x"); PCatHex(L,gRxRegPre[1][r],8); } else PCat(L,"----------");
        PCat(L,"  now 0x"); PCatHex(L,nowv,8);
        if(gRxRegPreOk[0] && gRxRegPreOk[1] && gRxRegPre[0][r] != gRxRegPre[1][r])
          PCat(L,"  ⚠ THE SCAN CHANGED IT");
        if(gRxRegPreOk[1]){
          PCat(L, (nowv == gRxRegPre[1][r]) ? "  same across the transmit"
                                            : "  ⚠⚠ THE TRANSMIT CHANGED IT"); }
        Out(L); }
      Say("        ⇒ changed at scan-ok : the scan moved it and transmitting is innocent.");
      Say("        ⇒ changed at now     : transmitting moved the receive engine underneath us,");
      Say("                               and the defect is in ApShimTxArm or PostTxFrameAt.");
      Say("        ⇒ all the same       : the DMA is configured exactly as it was while it was");
      Say("                               working, and the fault is above it -- PHY or MAC."); }
    { UInt32 mc = ssb_r32(gBus.bar0, B43_MMIO_MACCTL);
      L[0]=0; PCat(L,"      MACCTL now = 0x"); PCatHex(L,mc,8); PCat(L,"  ");
      PCat(L, (mc & B43_MACCTL_ENABLED)     ? "ENABLED "    : "⚠not-enabled ");
      PCat(L, (mc & B43_MACCTL_AWAKE)       ? "AWAKE "      : "⚠not-awake ");
      PCat(L, (mc & B43_MACCTL_BEACPROMISC) ? "BEACPROMISC " : "⚠BEACPROMISC-CLEAR ");
      PCat(L, (mc & B43_MACCTL_PROMISC)     ? "PROMISC "    : "");
      PCat(L, (mc & B43_MACCTL_KEEP_BAD)    ? "KEEP_BAD "   : "");
      Out(L);
      L[0]=0; PCat(L,"        we asked for 0x"); PCatHex(L,(unsigned long)gShimFilter,8);
      PCat(L," at 8-2g; a difference here means the CHIP changed it, not us."); Out(L); }
    L[0]=0; PCat(L,"      by type: mgmt "); PCatDec(L,(unsigned long)gRxCensusType[0]);
    PCat(L,", ctrl "); PCatDec(L,(unsigned long)gRxCensusType[1]);
    PCat(L,", data "); PCatDec(L,(unsigned long)gRxCensusType[2]);
    PCat(L,", rsvd "); PCatDec(L,(unsigned long)gRxCensusType[3]); Out(L);
    L[0]=0; PCat(L,"      mgmt subtypes seen:");
    n = 0;
    for(i=0;i<16;i++) if(gRxCensusMgmt[i]){
      PCat(L," "); PCatDec(L,(unsigned long)i); PCat(L,"x");
      PCatDec(L,(unsigned long)gRxCensusMgmt[i]); n++; }
    if(!n) PCat(L," none");
    Out(L);
    Say("        (subtype 8 = beacon, 5 = probe resp, 11 = auth, 1 = assoc resp, 12 = deauth)");
    if(gRxNearN){
      Say("      ⚠⚠ NEAR MISS -- right type AND subtype, rejected on ADDRESS only:");
      for(i=0;i<gRxNearN;i++){
        int k;
        L[0]=0; PCat(L,"          from ");
        for(k=0;k<6;k++){ if(k) PCat(L,":"); PCatHex(L,(unsigned long)gRxNearFrom[i][k],2); }
        PCat(L,"  to ");
        for(k=0;k<6;k++){ if(k) PCat(L,":"); PCatHex(L,(unsigned long)gRxNearTo[i][k],2); }
        Out(L); }
      L[0]=0; PCat(L,"        we were filtering for a sender of ");
      { int k; for(k=0;k<6;k++){ if(k) PCat(L,":");
          PCatHex(L,(unsigned long)gShimTgtBssid[k],2); } }
      Out(L);
      Say("        ⇒ The AP answered from a DIFFERENT BSSID. It is one radio with several, and");
      Say("          this driver picked the one that beacons, not the one that replies."); }
    else if(gRxCensusMgmt[11] || gRxCensusMgmt[1]){
      Say("      the right subtype WAS walked but failed type or address -- read the near-miss"); }
    else {
      Say("      no frame of the wanted subtype reached memory at all.");
      Say("        ⇒ Either the AP stayed silent, or the ring dropped it before we looked."); }
    L[0]=0; PCat(L,"      (census for: "); PCat(L,what); PCat(L,")"); Out(L);
}

/* ★ THE ONE LINE A PASSING RUN NEEDS -- k84's lesson.
 *
 * k84 fixed RxWaitFor's walk, and the run PASSED at the exact point k83 failed. Good -- except
 * the census above prints only on failure, so the discriminator built to name the mechanism
 * never ran, and the backlog at entry is unmeasured for BOTH runs. The fix is strongly indicated
 * and not proven, and it will stay that way until a passing run reports its own ring state.
 *
 * ⚠ The rule this violates is one this project already wrote down: ask what an oracle would FAIL
 *   on. I asked that, and stopped there. The other half is "what will I know if it PASSES?" An
 *   instrument that only speaks on failure can complain about a fix but never confirm one.
 *
 * So the ring reports itself on the success path too. One line, always, free. */
static void ApShimSayRxRing(const char *what)
{
    Str255 L;
    UInt32 st = gRxStatusIn & B43_DMA32_RXSTATE;
    L[0]=0;
    PCat(L,"        [ring] "); PCat(L,what);
    PCat(L,": backlog at entry "); PCatDec(L,(unsigned long)gRxBacklogIn);
    PCat(L,"/"); PCatDec(L,(unsigned long)K3_RX_SLOTS);
    PCat(L,", cursor "); PCatDec(L,(unsigned long)gRxSlotIn);
    PCat(L,"->"); PCatDec(L,(unsigned long)gRxSlot);
    PCat(L,", engine ");
    PCat(L, st==B43_DMA32_RXSTAT_DISABLED ? "DISABLED" :
            st==B43_DMA32_RXSTAT_ACTIVE   ? "ACTIVE"   :
            st==B43_DMA32_RXSTAT_IDLEWAIT ? "IDLEWAIT" :
            st==B43_DMA32_RXSTAT_STOPPED  ? "STOPPED"  : "?");
    PCat(L,"@"); PCatDec(L,(unsigned long)((gRxStatusIn & B43_DMA32_RXDPTR)/B43_DMADESC32_BYTES));
    Out(L);
    /* ⚠ A backlog at or near K3_RX_SLOTS on a run that PASSED is the proof k84 could not get:
     * it means the walk crossed a lapped ring and still found the frame. A small backlog means
     * the lap never happened here and the cursor was never the cause. Either way, it is an
     * answer, and it costs nothing. */
}

UInt32 AirPortShimAuth(void)
{
    UInt32 savedWin = 0;
    int haveSaved, got;
    UInt16 flen;
    UInt32 ams = 0; int aseen = 0;

    if(gShimAuthErr != 1) return 0;             /* once per load */
    if(gShimTxErr != noErr){
      Say("");
      Say("  [8-3b] AUTH DECLINED -- the transmitter is not proven.");
      Say("    ⚠ Read the 8-3a result above; this is downstream of it.");
      gShimAuthErr = paramErr; return 0; }

    Say("");
    Say("  [8-3b] AUTHENTICATION -- open system, sequence 1");

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      gShimAuthErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    /* ★★★★★ 8-31: CLEAR THE AP'S STALE STATE FIRST.
     *
     * k122 sent an auth request, the AP ACKed it, and then said nothing for 500 ms -- the
     * census saw one management frame and it was a probe response, not an auth response. This
     * driver has never sent a deauthentication in any build, so the access point has been left
     * holding association state for our MAC after every single session, and a reboot cannot
     * clean that up because a reboot never reaches EnetHAL_Close.
     *
     * One frame, sent before we ask to authenticate, and correct no matter how the last session
     * ended. We do NOT wait for or check a response: a deauth is a notification, the standard
     * requires no reply, and an AP with no state for us will simply drop it. The short settle
     * below is so the AP processes it before our auth request arrives.
     *
     * ⚠ Deliberately not gated on "did we associate last time" -- this copy of the driver has no
     *   memory of a previous boot, which is the entire point. */
    { UInt16 dlen = BuildDeauth(gShimTxFrm,gShimMac,gShimTgtBssid);
      GenerateTxHdr351(gShimTxHdr,gShimTxFrm,dlen,gShimChannel,0xC00D,
                       B43_TXH_PHY_ANT01AUTO,1);
      ApShimTxArm();
      Say1("    [8-31] deauth first, to clear stale AP state. length = ",(unsigned long)dlen);
      DmaPublish();
      PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                    gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,dlen);
      /* Drain our own status so the deauth's entry cannot be mistaken for the auth request's
       * a moment later -- k90 lost a run to exactly that confusion, reporting a stale status
       * as the current frame's ACK. The 50 ms doubles as the AP's settle time. */
      (void)DrainTxStatus(0xC00D, 50); }

    /* ⚠ A FRESH TXHDR FOR THIS FRAME. GenerateTxHdr351 writes the PLCP, which carries the length
     * the PHY transmits. Reusing 8-3a's header would send the probe request's 42 bytes for an
     * auth frame of a different size -- k49 lost a run to exactly that. */
    flen = BuildAuthRequest(gShimTxFrm,gShimMac,gShimTgtBssid);
    GenerateTxHdr351(gShimTxHdr,gShimTxFrm,flen,gShimChannel,0xC001,B43_TXH_PHY_ANT01AUTO,1);
    ApShimTxArm();                 /* ⚠ without this the doorbell does not ring -- see k79 */
    Say1("    auth request length = ",(unsigned long)flen);
    DmaPublish();
    PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                  gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,flen);

    gShimAuthAcked = DrainTxStatus(0xC001,600);
    Say1("    the AP ACKed our auth request = ",(unsigned long)gShimAuthAcked);

    /* subtype 11 = authentication, from the target, addressed to us */
    got = RxWaitFor(gShimDmaBase,gShimRxBufLog,B43_DMA0_RX_FW351_FO,0,11,
                    gShimTgtBssid,gShimMac,500,gPromptBuf,&ams,&aseen);
    gShimAuthMs = ams; gShimAuthSeen = (UInt32)aseen;

    if(!got){
      Say1("    [!!] NO AUTH RESPONSE within 500 ms. frames walked = ",(unsigned long)aseen);
      Say("      ⚠ k29 measured a probe response at 5-14 ms, so 500 ms is not a timing problem.");
      Say("        The ACK line above separates the two causes: an ACKed auth request that draws");
      Say("        no answer means the AP received it and chose silence -- MAC filtering first.");
      ApShimSayRxCensus("8-3b authentication");
      gShimAuthErr = paramErr;
    } else {
      const UInt8 *bf = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
      gShimAuthAlg    = (UInt16)(bf[24] | ((UInt16)bf[25]<<8));
      gShimAuthSeq    = (UInt16)(bf[26] | ((UInt16)bf[27]<<8));
      gShimAuthStatus = (UInt16)(bf[28] | ((UInt16)bf[29]<<8));
      gShimAuthGot = 1;
      Say1("    ★★★ AUTH RESPONSE after ms = ",(unsigned long)ams);
      Say1("        frames walked while waiting = ",(unsigned long)aseen);
      ApShimSayRxRing("8-3b auth");
      SayH("        algorithm = ",(unsigned long)gShimAuthAlg,4);
      SayH("        sequence  = ",(unsigned long)gShimAuthSeq,4);
      SayH("        status    = ",(unsigned long)gShimAuthStatus,4);
      if(gShimAuthSeq != AUTH_SEQ_RESPONSE){
        Say("    ⚠ SEQUENCE IS NOT 2. An auth frame from the target addressed to us, but not the");
        Say("      second frame of OUR exchange -- do not read its status as our answer.");
        gShimAuthErr = paramErr;
      } else if(gShimAuthStatus != AUTH_STATUS_SUCCESS){
        Say1("    [!!] the AP REFUSED us. status = ",(unsigned long)gShimAuthStatus);
        gShimAuthErr = paramErr;
      } else {
        Say("    [ok] ★★★★★ SEQUENCE 2, STATUS 0 -- the access point authenticated this driver.");
        gShimAuthErr = noErr; } }

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
    return (UInt32)(gShimAuthErr == noErr);
}

/* 8-3b: the authentication result. */
UInt32 AirPortShimGetAuth(UInt32 *alg, UInt32 *seq, UInt32 *status,
                          UInt32 *ms, UInt32 *acked, UInt32 *got, SInt32 *err)
{
    /* ⚠⚠ "DID A FRAME ARRIVE" IS ITS OWN FIELD, NOT A MAGIC VALUE IN ms.
     *
     * k79's oracle used ms == 0 to mean "no response" and was wrong: RxWaitFor reports the
     * elapsed time even on timeout, so a silent AP came back as ms = 501. I fixed that by folding
     * the answer into ms -- and k80 then reported ms = 0 on a SUCCESSFUL exchange, because the
     * response was already in the ring and arrived on the first poll.
     *
     * So the fold replaced one collision with another: 0 meant both "nothing came" and "it came
     * immediately". The oracle passed only because it checks err before it looks at ms. A
     * diagnostic that is ambiguous exactly when it is needed is not a diagnostic. */
    if(alg)    *alg    = (UInt32)gShimAuthAlg;
    if(seq)    *seq    = (UInt32)gShimAuthSeq;
    if(status) *status = (UInt32)gShimAuthStatus;
    if(ms)     *ms     = gShimAuthMs;
    if(acked)  *acked  = (UInt32)gShimAuthAcked;
    if(got)    *got    = (UInt32)gShimAuthGot;   /* 1 = a frame arrived, whatever it said */
    if(err)    *err    = (SInt32)gShimAuthErr;
    return (UInt32)(gShimAuthErr == noErr);
}

/* ★★★ STAGE 8-3c: ASSOCIATION. The exchange that yields an AID.
 *
 * ★ THE ORACLE IS status == 0 AND AN AID THE AP ASSIGNED. Both come out of a frame the access
 *   point built. The AID is the stronger half: it is a number the AP allocates from its own
 *   association table, and a driver cannot invent a plausible one.
 *
 * ⚠ MASK THE AID WITH 0x3FFF. The top two bits are always set by the AP and are NOT part of the
 *   value -- the app's comment says so at the same line. An unmasked AID reads as ~49000 and
 *   would look like garbage from a working association.
 *
 * ⚠ THE RSNE IS STATED, NOT DISCOVERED, and that is a scope decision with its defence named:
 *   we never parse the AP's beacon, and the downgrade protection is the RSNE comparison in EAPOL
 *   message 3 -- Stage 7, and not skipped. Carried across from the app rather than re-argued.
 *
 * ⚠ SILENCE HERE MEANS SOMETHING DIFFERENT FROM SILENCE AT 8-3b. Authentication succeeded moments
 *   earlier, so the AP knows this MAC and is willing to talk to it. No association response points
 *   at the request's CONTENTS -- the RSNE first -- rather than at addressing or the transmitter. */
static UInt16 gShimAssocCap = 0, gShimAssocStatus = 0xFFFF, gShimAssocAid = 0;
static UInt32 gShimAssocMs = 0, gShimAssocSeen = 0;
static int    gShimAssocAcked = 0, gShimAssocGot = 0;
static OSStatus gShimAssocErr = 1;              /* 1 = never attempted */

UInt32 AirPortShimAssoc(void)
{
    UInt32 savedWin = 0;
    int haveSaved, got;
    UInt16 blen;
    UInt32 bms = 0; int bseen = 0;

    if(gShimAssocErr != 1) return 0;            /* once per load */
    if(gShimAuthErr != noErr){
      Say("");
      Say("  [8-3c] ASSOCIATION DECLINED -- authentication did not succeed.");
      Say("    ⚠ An association request from an unauthenticated station is refused by any sane");
      Say("      AP, so sending one would test nothing. Read the 8-3b result above.");
      gShimAssocErr = paramErr; return 0; }

    Say("");
    Say("  [8-3c] ASSOCIATION");

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      gShimAssocErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    blen = BuildAssocRequest(gShimTxFrm,gShimMac,gShimTgtBssid,
                             (const char *)gTargetSsid,gTargetLen,
                             gTargetSec != kApSecOpen);   /* k217: open -> no RSNE, Privacy bit clear */
    Say1("    assoc request bytes = ",(unsigned long)blen);
    if(gTargetSec == kApSecOpen){
      Say ("    capability = ESS | SHORT PREAMBLE,  listen interval 1  (open: no Privacy)");
      Say ("    no RSNE -- open network");
    } else {
      Say ("    capability = ESS | PRIVACY | SHORT PREAMBLE,  listen interval 1");
      Say ("    RSNE: group CCMP-128, pairwise CCMP-128, AKM PSK, RSN caps 0x0000");
    }

    GenerateTxHdr351(gShimTxHdr,gShimTxFrm,blen,gShimChannel,0xC020,
                     B43_TXH_PHY_ANT01AUTO,1);
    ApShimTxArm();
    DmaPublish();
    PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                  gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,blen);

    gShimAssocAcked = DrainTxStatus(0xC020,600);
    Say1("    the AP ACKed our assoc request = ",(unsigned long)gShimAssocAcked);

    /* subtype 1 = association response. 800 ms, as the app uses -- an AP does more work here
     * than for an auth frame and k29's 5-14 ms figure is not the right yardstick. */
    got = RxWaitFor(gShimDmaBase,gShimRxBufLog,B43_DMA0_RX_FW351_FO,0,1,
                    gShimTgtBssid,gShimMac,800,gPromptBuf,&bms,&bseen);
    gShimAssocMs = bms; gShimAssocSeen = (UInt32)bseen;

    if(!got){
      Say1("    [!!] NO ASSOC RESPONSE within 800 ms. frames walked = ",(unsigned long)bseen);
      Say("      ⚠ Authentication succeeded moments ago, so the AP knows this MAC. Silence here");
      Say("        points at the request's CONTENTS rather than at addressing -- the RSNE first.");
      ApShimSayRxCensus("8-3c association");
      gShimAssocErr = paramErr;
    } else {
      const UInt8 *cf = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
      UInt16 caid;
      gShimAssocGot = 1;
      gShimAssocCap    = (UInt16)(cf[24] | ((UInt16)cf[25]<<8));
      gShimAssocStatus = (UInt16)(cf[26] | ((UInt16)cf[27]<<8));
      caid             = (UInt16)(cf[28] | ((UInt16)cf[29]<<8));
      gShimAssocAid    = (UInt16)(caid & 0x3FFF);   /* ⚠ the top two bits are the AP's, not ours */
      Say1("    ★★★ ASSOC RESPONSE after ms = ",(unsigned long)bms);
      ApShimSayRxRing("8-3c assoc");
      SayH("        capability = ",(unsigned long)gShimAssocCap,4);
      SayH("        status     = ",(unsigned long)gShimAssocStatus,4);
      SayH("        AID raw    = ",(unsigned long)caid,4);
      Say1("        AID        = ",(unsigned long)gShimAssocAid);
      if(gShimAssocStatus != ASSOC_STATUS_SUCCESS){
        Say1("    [!!] the AP REFUSED the association. status = ",
             (unsigned long)gShimAssocStatus);
        gShimAssocErr = paramErr;
      } else if(gShimAssocAid == 0){
        Say("    [!!] status 0 but the AID is ZERO. An associated station always has a non-zero");
        Say("      AID, so this is not a completed association -- suspect the field offsets.");
        gShimAssocErr = paramErr;
      } else {
        Say("    [ok] ★★★★★ STATUS 0 AND AN AID. The access point associated this driver.");
        gShimAssocErr = noErr;

        /* ★★★★★★ 8-6d: THE SECOND MACFILTER_BSSID WRITE. THIS IS THE ONE THAT WAS MISSING.
         *
         * ⚠⚠ k87's histogram is the diagnostic signature, and this project has a memo about it
         *   that cost 43 probe increments the first time:
         *
         *       144 ring frames walked, 105 of them beacons, ZERO data frames --
         *       while unicast worked perfectly (auth, assoc and all four EAPOL-Key frames).
         *
         *   Unicast and beacons arriving while group-addressed data never does is an ADDRESS
         *   FILTER, not a radio, a PHY or a crypto problem. With MACFILTER_BSSID left at
         *   00:00:00:00:00:00 the card still delivers anything unicast to us (MACFILTER_SELF
         *   matches addr1) and still delivers beacons (MACCTL's BEACPROMISC bit deliberately
         *   BYPASSES the BSSID match). Group-addressed data is the only class that needs the
         *   match -- and it is exactly the class carried by the GTK.
         *
         * ⚠ AND THE HEADER SAID SO. ap_rx.h:172, above UploadCardMacAddress: "The real one goes
         *   in after association; see WriteMacBssidTemplates above." The only caller that ever
         *   did it is in airport_rx.c, inside the AP_APP_DRIVES_RADIO block that has been dark
         *   since 8-2g -- the same shape as the GTK gap found at 8-6a. Two steps that lived
         *   only in the app's gated code and vanished silently when the driver took over.
         *
         * ⚠ NO MAC SUSPEND. b43 does this at main.c:4140 on BSS_CHANGED_BSSID with the MAC
         *   still running; its own b43_mac_suspend is three lines later, for other work. */
        WriteMacBssidTemplates(gShimMac,gShimTgtBssid);
        { Str255 L; int q;
          L[0]=0; PCat(L,"    ★ MACFILTER_BSSID <- ");
          for(q=0;q<6;q++){ if(q) PCat(L,":"); PCatHex(L,(unsigned long)gShimTgtBssid[q],2); }
          Out(L); }
        Say("      (group-addressed data cannot arrive until this is written; beacons and");
        Say("       unicast always could, which is why this hid for so long.)"); } }

    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
    return (UInt32)(gShimAssocErr == noErr);
}

/* 8-3c: the association result. */
UInt32 AirPortShimGetAssoc(UInt32 *cap, UInt32 *status, UInt32 *aid,
                           UInt32 *ms, UInt32 *acked, UInt32 *got, SInt32 *err)
{
    if(cap)    *cap    = (UInt32)gShimAssocCap;
    if(status) *status = (UInt32)gShimAssocStatus;
    if(aid)    *aid    = (UInt32)gShimAssocAid;
    if(ms)     *ms     = gShimAssocMs;
    if(acked)  *acked  = (UInt32)gShimAssocAcked;
    if(got)    *got    = (UInt32)gShimAssocGot;   /* its own field -- see the k80 note on GetAuth */
    if(err)    *err    = (SInt32)gShimAssocErr;
    return (UInt32)(gShimAssocErr == noErr);
}

/* ★★★★★★ STAGE 8-4: THE WPA2 FOUR-WAY HANDSHAKE.
 *
 * ⚠⚠ THIS IS WHERE THE DRIVER BINARY BECOMES SECRET-BEARING. ap_eapol.h pulls in
 *   ap_psk_config.h, so the passphrase is compiled into this fragment and everything derived from
 *   it -- PMK, PTK, KCK, KEK, TK, GTK -- lives in its data section. NOTHING derived from the PSK
 *   is printed: only presence, timing and counts. The PMK is password-equivalent for this SSID.
 *
 * ★ THE ORACLE IS MESSAGE 3, AND IT IS THE STRONGEST IN THE PROJECT. When the AP sends message 3
 *   with MIC, ACK and INSTALL set, it has recomputed the MIC on OUR message 2 using ITS OWN PTK
 *   and got our value. Both sides derived the same 48 bytes independently from the same PMK, so
 *   the PMK, the PRF, the min||max address ordering, the label's NUL and the KCK are ALL correct
 *   at once. Nothing in this driver can fake that -- it is a cryptographic agreement with a
 *   third party.
 *
 * ⚠ THE PMK IS COMPUTED BEFORE ASSOCIATING, in its own call. Message 1 arrives UNPROMPTED about
 *   100 ms after the association response, and PBKDF2 at 4096 iterations is 0.3-1 s on this G4.
 *   Deriving it afterwards loses message 1 every time.
 *
 * ⚠ THE SNonce IS NOT CRYPTOGRAPHICALLY SOUND. It is built from the tick counter and the ANonce,
 *   exactly as the probe does, and the probe's own comment says this is "adequate to prove the
 *   derivation runs, NOT adequate for a driver anyone relies on". That remains true here and is
 *   carried forward as an open item, not quietly inherited. */
static int      gShimPmkReady = 0, gShimPtkReady = 0;
static UInt32   gShimPmkMs = 0;
static UInt32   gShimPmkNz = 0, gShimPtkNz = 0;
static int      gShimM1 = 0, gShimM2Sent = 0, gShimM3 = 0, gShimM4Sent = 0;
/* k213: wrong-password detection. A WPA2 passphrase is only proven by the 4-way handshake -- auth and assoc
 * succeed with any key -- so "we transmitted message 2 but message 3 never came" is the wrong-key signature.
 * gShimBadKeyTries counts those in a row (a failure that never reached message 2 is range/contact, not the
 * key, and resets it); at AP_BADKEY_TRIES the driver LATCHES gShimKeyBad, publishes kApLinkBadKey, and stops
 * retrying so it does not hammer the AP with a key it already rejected. Both clear when a new target is chosen
 * (ApShimSetTarget*), which is the only way in -- the panel removes the saved network and asks again. */
#define AP_BADKEY_TRIES 2
static int      gShimBadKeyTries = 0;
static int      gShimKeyBad = 0;
/* k215: the SSID OT was last told about, for the DHCP-renew push (ApOtmNotifyNetworkChange). A (re)join to a
 * DIFFERENT network pushes a network-change so OT renews its lease; a same-network recovery does not, so a
 * flaky link does not churn the IP. Set at the boot join and on every network-changing rejoin. */
static UInt8    gOtmLastNetSsid[32];
static UInt32   gOtmLastNetLen = 0;
/* 8-6a: the group key, taken out of message 3. ⚠ gShimKdBuf holds UNWRAPPED key data and so
 * the GTK in the clear. Neither is ever printed and neither leaves this fragment. */
static UInt8    gShimKdBuf[256];
static UInt8    gShimGtkLen = 0, gShimGtkId = 0;
/* ★★★ k208: THE GROUP KEYS BY KEY INDEX, and the replay counter every renewal is checked against (ap_gtk.h
 * has the why). The 4-way handshake below clears and fills them -- at task level, with the receive interrupt
 * quiesced -- and from then on the receive pump is their only reader and writer (ApGkRenew, secondary
 * interrupt level, never re-entered). gGtk keeps the key the 4-way handshake delivered, as before; nothing
 * on the receive path reads it any more. */
static ApGtkTable gGtkTab;
static ApU8       gGkReplay[8];          /* the newest counter accepted: message 3's, then each renewal's */
static volatile int gGkReady = 0;        /* 1 = a 4-way handshake completed: gGkReplay and the PTK are this association's */
static volatile int gGkM1Pending = 0;    /* a message 1 the pump queued for ApGkService, below */
static volatile int gGkM2Pending = 0;    /* a message 2 built and not yet sent ("try later"), below */
static UInt32       gGkLastKid = 4;      /* the key index most recently installed (4 = none yet) */
static UInt16   gShimM1Info = 0, gShimM3Info = 0;
static UInt32   gShimM1Ms = 0, gShimM3Ms = 0;
static OSStatus gShimHsErr = 1;                 /* 1 = never attempted */

/* ★★ k203: MESSAGE 2, AS A STEP THAT CAN BE REPEATED -- and whose delivery is finally checked.
 * Built from gReplay / gSNonce / gPtk as they stand, so answering a retried message 1 (new replay
 * counter, maybe a new ANonce and PTK) is just calling this again. Through k202 message 2 was posted
 * and never checked: the failed k202 boot could not say whether the AP even ACKed it. Now its TX
 * status is drained like auth's and assoc's: ACKED 0 = lost on the air; ACKED 1 and message 1 again =
 * received but not accepted (MIC, or the AP's retry had already moved the replay counter on). */
#define AP_M1_RETRY_ROUNDS 3
static UInt32 gShimM1Retries = 0, gShimM2Posts = 0, gShimM2AckedN = 0;
static void ApShimSendM2(void)
{
    UInt16 m2len = BuildEapolKeyFrame(gShimTxFrm,gShimMac,gShimTgtBssid,gSNonce,gReplay,
                        gPtk+AP_KCK_OFF,
                        (UInt16)((gShimM1Info & KEYINFO_VERSION)|KEYINFO_PAIRWISE|KEYINFO_MIC),1);
    int acked;
    Say1("    message 2 bytes = ",(unsigned long)m2len);
    Say ("      Key Data is the RSNE, byte-identical to the association request's -- the AP");
    Say ("      compares them and aborts if they differ at all.");
    GenerateTxHdr351(gShimTxHdr,gShimTxFrm,m2len,gShimChannel,0xC030,
                     B43_TXH_PHY_ANT01AUTO,1);
    ApShimTxArm();
    DmaPublish();
    PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                  gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,m2len);
    gShimM2Sent = 1;
    gShimM2Posts++;
    acked = DrainTxStatus(0xC030,600);
    if(acked) gShimM2AckedN++;
    Say1("    the AP ACKed message 2 (802.11 ACK: it was delivered, not that it was accepted) = ",
         (unsigned long)acked);
}

/* ⚠ CALLED BEFORE AirPortShimAssoc. Message 1 will not wait.
 *
 * ★★★ k210: THE PMK COMES FROM THE RUNTIME TARGET, NOT FROM A COMPILED PASSPHRASE. The panel ran PBKDF2
 * once when the user typed the password and stored the 32-byte PMK in the Preferences file; the driver read
 * it into gTargetPmk (ApShimSetTargetFromKnown). So this now just copies it -- the driver holds no
 * passphrase and runs no PBKDF2, which is the whole point of k210. The PMK stays password-equivalent, so
 * nothing derived from it is printed; only whether one is present. */
UInt32 AirPortShimPrepPmk(void)
{
    UInt32 z;
    if(gShimPmkReady) return 1;
    Say("");
    Say("  [8-4] THE PMK -- taken from the runtime target (the panel derived it; the driver has no passphrase)");
    if(!gHaveTarget || gTargetSec != kApSecWpa2Psk){
      Say("    [!!] no WPA2-Personal target PMK -- cannot do the handshake.");
      gShimPmkReady = 0; gPmkReady = 0; return 0; }
    for(z=0;z<32;z++) gPmk[z] = gTargetPmk[z];
    gShimPmkNz = 0;
    for(z=0;z<32;z++) if(gPmk[z]) gShimPmkNz++;
    gShimPmkReady = (gShimPmkNz > 0);
    gPmkReady = gShimPmkReady;
    gShimPmkMs = 0;
    /* k214: name WHICH network this PMK is for, so a stale-PMK recurrence (the second-network bug) is visible
     * in the log instead of silent. The SSID length is a free discriminator between two targets and reveals no
     * key material; the PMK bytes themselves are still never printed. */
    Say1("    PMK re-read for THIS target -- SSID length = ",(unsigned long)gTargetLen);
    Say1("    non-zero bytes in the 32-byte PMK = ",(unsigned long)gShimPmkNz);
    Say(gShimPmkReady ? "    [ok] the PMK is present (from the chosen network)."
                      : "    [!!] the stored PMK is all zeros -- re-enter the password in the panel.");
    return (UInt32)gShimPmkReady;
}

UInt32 AirPortShimHandshake(void)
{
    UInt32 savedWin = 0;
    int haveSaved, got, z;
    UInt32 ms = 0; int seen = 0;
    const UInt8 *eap;

    if(gShimHsErr != 1) return 0;
    if(gShimAssocErr != noErr){
      Say("");
      Say("  [8-4] HANDSHAKE DECLINED -- not associated.");
      Say("    ⚠ The four-way handshake runs over a BSS membership. Read 8-3c above.");
      gShimHsErr = 2; return 0; }        /* ⚠ 2 = DECLINED, not paramErr -- see the k82 note */
    if(!gShimPmkReady){
      Say("");
      Say("  [8-4] HANDSHAKE DECLINED -- no PMK.");
      Say("    ⚠ AirPortShimPrepPmk must be called BEFORE associating; message 1 arrives about");
      Say("      100 ms after the association response and will not wait for PBKDF2.");
      gShimHsErr = 2; return 0; }        /* ⚠ 2 = DECLINED */

    Say("");
    Say("  [8-4] THE FOUR-WAY HANDSHAKE");

    /* k208: a new association starts with no group keys and no accepted replay counter. The receive
     * interrupt is quiesced for every join (the boot join runs before the first arm), so the pump cannot
     * be looking at any of this. */
    ApGtkClear(&gGtkTab);
    gGkReady = 0; gGkM1Pending = 0; gGkM2Pending = 0; gGkLastKid = 4;
    for(z=0;z<8;z++) gGkReplay[z] = 0;

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core.");
      gShimHsErr = paramErr;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    /* ── message 1: unprompted, type 2 (data), any subtype ── */
    got = RxWaitFor(gShimDmaBase,gShimRxBufLog,B43_DMA0_RX_FW351_FO,2,RXW_ANY_SUB,
                    gShimTgtBssid,gShimMac,2000,gPromptBuf,&ms,&seen);
    gShimM1Ms = ms;
    if(!got){
      Say1("    [!!] no data frame within 2 s of associating. frames walked = ",
           (unsigned long)seen);
      Say("      ⚠ Message 1 is unprompted. Silence means the AP did not start the handshake --");
      Say("        check that the association above really completed with a non-zero AID.");
      ApShimSayRxCensus("8-4 message 1");
      gShimHsErr = paramErr; goto done; }
    { const UInt8 *mf = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
      UInt16 mflen = le16at(gPromptBuf + RXH_FRAME_LEN);
      mflen = (UInt16)(mflen > K6_HDR_PLCP6 ? mflen - K6_HDR_PLCP6 : 0);
      eap = EapolBody(mf,mflen); }
    if(!eap){
      Say1("    [!!] a data frame arrived after ms = ",(unsigned long)ms);
      Say("      but it is not EAPOL. The handshake did not start.");
      gShimHsErr = paramErr; goto done; }
    gShimM1Info = be16at(eap+EAPOL_O_KEYINFO);
    SayH("    message 1 key info = ",(unsigned long)gShimM1Info,4);
    /* Message 1: PAIRWISE and ACK set, MIC clear. */
    if(!((gShimM1Info & KEYINFO_PAIRWISE) && (gShimM1Info & KEYINFO_ACK)
         && !(gShimM1Info & KEYINFO_MIC))){
      Say("    [!!] NOT message 1 -- PAIRWISE and ACK set with MIC clear is the signature.");
      Say("      ⚠ Do not derive a PTK from another message's nonce.");
      gShimHsErr = paramErr; goto done; }
    gShimM1 = 1;
    for(z=0;z<32;z++) gANonce[z] = eap[EAPOL_O_NONCE+z];
    for(z=0;z<8;z++)  gReplay[z] = eap[EAPOL_O_REPLAY+z];
    Say1("    ★ message 1 after ms = ",(unsigned long)ms);

    /* ⚠ SNonce from the tick counter and the ANonce. Enough to exercise the derivation; NOT an
     * acceptable entropy source for a driver anyone relies on. Carried forward as an open item. */
    { UInt32 seed = TickCount();
      for(z=0;z<32;z++)
        gSNonce[z] = (UInt8)((seed >> ((z&3)*8)) ^ gANonce[31-z] ^ (UInt8)(z*37+11)); }

    ApPmkToPtk(gPmk,32UL,gShimTgtBssid,gShimMac,gANonce,gSNonce,gPtk);
    gShimPtkNz = 0;
    for(z=0;z<AP_PTK_LEN;z++) if(gPtk[z]) gShimPtkNz++;
    gShimPtkReady = (gShimPtkNz > 0);
    gPtkReady = gShimPtkReady;
    Say1("    PTK derived; non-zero bytes of 48 = ",(unsigned long)gShimPtkNz);
    Say ("    ⚠ KCK, KEK and TK are NOT printed. The MIC on message 2 is what proves them right.");
    if(!gShimPtkReady){ gShimHsErr = paramErr; goto done; }

    /* ── message 2 ── */
    ApShimSendM2();

    /* ── message 3: THE ORACLE ──
     * ★★ k203: AND A RETRIED MESSAGE 1 IS ANSWERED, NOT FATAL. The second k202 boot heard every frame,
     * authenticated, associated, got message 1 in 4 ms -- and then the AP sent message 1 AGAIN: it
     * never accepted our message 2 (lost on the air, or it arrived after the AP's retry moved the
     * replay counter on). This code gave up there, and with one join attempt and no rejoin in the
     * native module, that boot had no network until a reboot. wpa_supplicant answers every message 1
     * with a message 2 carrying THAT message's replay counter (and re-derives the PTK if the ANonce
     * changed). So does this, up to AP_M1_RETRY_ROUNDS times, within the same attempt. */
    { int round;
      for(round = 0; ; round++){
        got = RxWaitFor(gShimDmaBase,gShimRxBufLog,B43_DMA0_RX_FW351_FO,2,RXW_ANY_SUB,
                        gShimTgtBssid,gShimMac,2000,gPromptBuf,&ms,&seen);
        gShimM3Ms = ms;
        if(!got){
          Say1("    [!!] nothing within 2 s of message 2. frames walked = ",(unsigned long)seen);
          Say("      ⚠ The AP verifies our MIC before answering. Silence means it did NOT verify --");
          Say("        the PTK, the PRF ordering or the KCK offset are the suspects, in that order.");
          ApShimSayRxCensus("8-4 message 3");
          gShimHsErr = paramErr; goto done; }
        { const UInt8 *mf = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
          UInt16 mflen = le16at(gPromptBuf + RXH_FRAME_LEN);
          mflen = (UInt16)(mflen > K6_HDR_PLCP6 ? mflen - K6_HDR_PLCP6 : 0);
          eap = EapolBody(mf,mflen); }
        if(!eap){
          Say("    [!!] a data frame arrived but it is not EAPOL.");
          gShimHsErr = paramErr; goto done; }
        gShimM3Info = be16at(eap+EAPOL_O_KEYINFO);
        SayH("    message 3 key info = ",(unsigned long)gShimM3Info,4);
        /* Message 3 carries INSTALL and SECURE alongside ACK and MIC. A retried message 1 has ACK and
         * PAIRWISE with MIC clear, which is why INSTALL is the discriminator. */
        if((gShimM3Info & KEYINFO_MIC) && (gShimM3Info & KEYINFO_ACK)
           && (gShimM3Info & KEYINFO_INSTALL)) break;                      /* message 3: onward */
        if((gShimM3Info & KEYINFO_PAIRWISE) && (gShimM3Info & KEYINFO_ACK)
           && !(gShimM3Info & KEYINFO_MIC) && round < AP_M1_RETRY_ROUNDS){
          int changed = 0;
          gShimM1Retries++;
          Say1("    ★ the AP RETRIED message 1 -- it did not accept our message 2. Answering, round ",
               (unsigned long)(round + 1));
          for(z=0;z<32;z++) if(gANonce[z] != eap[EAPOL_O_NONCE+z]){ changed = 1; break; }
          for(z=0;z<8;z++)  gReplay[z] = eap[EAPOL_O_REPLAY+z];      /* THIS message's counter */
          if(changed){                                                /* a new ANonce: a new PTK */
            for(z=0;z<32;z++) gANonce[z] = eap[EAPOL_O_NONCE+z];
            ApPmkToPtk(gPmk,32UL,gShimTgtBssid,gShimMac,gANonce,gSNonce,gPtk);
            Say("      the ANonce changed, so the PTK was derived again"); }
          ApShimSendM2();
          continue; }
        Say("    [!!] not message 3 -- MIC, ACK and INSTALL together are the signature.");
        Say("      ⚠ A retried message 1 has ACK and no MIC, and means the AP rejected message 2.");
        gShimHsErr = paramErr; goto done; } }
    gShimM3 = 1;
    Say1("    ★★★★★★ MESSAGE 3 after ms = ",(unsigned long)ms);
    Say ("      The AP recomputed the MIC on OUR message 2 with ITS PTK and got our value.");
    Say ("      Both sides derived the same 48 bytes independently.");

    /* ★★★ 8-6a NEEDS THE GTK, so the driver now takes it out of message 3.
     *
     * ⚠ WHY THIS APPEARED ONLY AT 8-6. The handshake has been "complete" since k84 -- M3 verified,
     *   M4 sent, the AP satisfied. But gGtkLen was only ever assigned in airport_rx.c, inside the
     *   app's AP_APP_DRIVES_RADIO block, which has been dark since 8-2g. So the DRIVER has never
     *   held a group key, and nothing noticed because nothing had asked for one.
     *
     *   8-6 asks. We are associated with no IP address, so essentially everything the AP sends us
     *   is group-addressed -- ARP, mDNS, broadcast. Without the GTK this build would have
     *   converted ZERO frames and reported "protected, key not installed" for all of them, and
     *   the DLPI contract -- the actual question -- would have gone untested for a reason that
     *   has nothing to do with DLPI. The control must exercise the change.
     *
     * ⛔ AND THE MIC IS CHECKED FIRST, WHICH IS NOT OPTIONAL. Unwrapping an unauthenticated
     *   message 3 installs a group key chosen by whoever sent the frame. The app's Stage 7 code
     *   refuses on exactly this ground and says so; the driver refuses for the same reason. */
    {
        UInt16 m3BodyLen = be16at(eap + EAPOL_O_BODYLEN);
        UInt16 m3DataLen = be16at(eap + EAPOL_O_DATALEN);
        int micOk = 0;
        gShimGtkLen = 0;

        if(m3BodyLen >= EAPOL_O_DATALEN - 4 + 2 && m3BodyLen <= AP_PROMPT_BUF - 8){
          static UInt8 scratch[AP_PROMPT_BUF];
          UInt8 calc[20], rxMic[16];
          UInt32 z2;
          for(z2=0;z2<16;z2++) rxMic[z2] = eap[EAPOL_O_MIC+z2];
          for(z2=0;z2<(UInt32)(4+m3BodyLen);z2++) scratch[z2] = eap[z2];
          /* ⚠ THE MIC COVERS FROM THE EAPOL HEADER WITH THE MIC FIELD ZEROED -- not from the
           * EAPOL-Key header. The published standard is wrong about this; hostap's
           * wpa_common.c:186 documents it, and Stage 7 measured it against a live AP. */
          for(z2=0;z2<16;z2++) scratch[EAPOL_O_MIC+z2] = 0;
          ApHmacSha1((const ApU8*)(gPtk+AP_KCK_OFF),16UL,
                     (const ApU8*)scratch,(ApU32)(4+m3BodyLen),calc);
          micOk = 1;
          for(z2=0;z2<16;z2++) if(calc[z2] != rxMic[z2]) micOk = 0; }

        Say(micOk ? "      [ok] message 3's OWN MIC verifies against our KCK"
                  : "      [!!] message 3's own MIC does NOT verify -- NOT unwrapping it.");

        if(micOk && (gShimM3Info & KEYINFO_ENCRYPTED) && m3DataLen >= 24
           && m3DataLen <= sizeof(gShimKdBuf) + 8){
          if(ApAesUnwrap((const ApU8*)(gPtk+AP_KEK_OFF),
                         (const ApU8*)(eap+EAPOL_O_DATA),
                         (ApU32)m3DataLen,(ApU8*)gShimKdBuf)){
            UInt16 plainLen = (UInt16)(m3DataLen - 8);
            UInt16 q = 0;
            Say1("      key data unwrapped (RFC 3394), plaintext bytes = ",
                 (unsigned long)plainLen);
            while(q + 2 <= plainLen){
              UInt8 id = gShimKdBuf[q], ln = gShimKdBuf[q+1];
              if(q + 2 + ln > plainLen) break;
              if(id == 0xDD && ln >= 6 &&
                 gShimKdBuf[q+2]==0x00 && gShimKdBuf[q+3]==0x0F &&
                 gShimKdBuf[q+4]==0xAC && gShimKdBuf[q+5]==0x01){
                /* GTK KDE: OUI(3) + type(1) + keyid(1) + reserved(1) + the key. So the key is
                 * ln-6 bytes and starts at q+8. ⚠ k34 subtracted keyid and reserved TWICE and
                 * copied 14 of 16 bytes -- a group key that silently decrypts nothing. */
                UInt16 glen = (UInt16)(ln - 6);
                if(glen >= 1 && glen <= 32){
                  UInt16 g;
                  gShimGtkId  = (UInt8)(gShimKdBuf[q+6] & 0x03);
                  gShimGtkLen = (UInt8)glen;
                  for(g=0; g<glen; g++) gGtk[g] = gShimKdBuf[q+8+g];
                  gGtkLen = (UInt8)glen;
                  gGtkId  = gShimGtkId;
                  /* k208: and into the table, under the index the AP named -- the receive path now picks
                   * a group frame's key by the index in its own CCMP header. */
                  ApGtkInstall(&gGtkTab,(int)gShimGtkId,(const ApU8*)gGtk,(ApU32)glen);
                  gGkLastKid = (UInt32)gShimGtkId; } }
              q = (UInt16)(q + 2 + ln); }
            /* ⛔⛔ 8-8: THE TRANSMIT PN, RESET HERE BECAUSE THE PTK IS FRESH.
             *
             * The driver has never touched gTxPn -- the compiler has been warning "defined but
             * not used" on it since ap_eapol.h was extracted, and the only code that ever set it
             * is in airport_rx.c's AP_APP_DRIVES_RADIO block, dark since 8-2g. That is the
             * FOURTH gap of exactly this shape, after the GTK, the post-association BSSID write
             * and the bind address.
             *
             * This one is not a missing feature, it is a security defect waiting for a
             * transmitter: CCMP's nonce is (PN, address), so sending two frames with the same PN
             * under one key hands an observer two ciphertexts under one keystream and lets the
             * AP silently discard the second as a replay. A PN left at zeros from BSS would do
             * exactly that on frame two.
             *
             * It starts at 1, not 0, on every fresh PTK -- the value Stage 8-1 used to put an
             * encrypted ARP on the air, and the value the standard expects. */
            { int pz; for(pz=0;pz<5;pz++) gTxPn[pz]=0; gTxPn[5]=1; }
            Say("      transmit PN reset to 1 for this PTK");

            Say1("      ★ GTK length = ",(unsigned long)gShimGtkLen);
            Say1("        key id     = ",(unsigned long)gShimGtkId);
            Say ("        ⚠ The GTK itself is NOT printed. It decrypts this network's");
            Say ("          broadcast traffic for every station on it.");
            if(!gShimGtkLen)
              Say("      ⚠ No GTK KDE in the key data. Group traffic will not decrypt.");
          } else {
            Say("      [!!] the RFC 3394 integrity check FAILED -- the KEK is wrong or the data");
            Say("        was altered. ApAesUnwrap leaves the output alone rather than handing");
            Say("        back a partial result, so there is nothing to salvage."); } }
    }

    /* ── message 4 ── */
    { UInt16 m4info = (UInt16)((gShimM3Info & KEYINFO_SECURE)|(gShimM3Info & KEYINFO_VERSION)
                               |KEYINFO_PAIRWISE|KEYINFO_MIC);
      UInt16 m4len;
      /* ⚠ the replay counter comes from MESSAGE 3, not message 1. Echoing message 1's is
       * rejected. */
      for(z=0;z<8;z++) gReplay[z] = eap[EAPOL_O_REPLAY+z];
      m4len = BuildEapolKeyFrame(gShimTxFrm,gShimMac,gShimTgtBssid,0,gReplay,
                                 gPtk+AP_KCK_OFF,m4info,0);
      SayH("    message 4 key info = ",(unsigned long)m4info,4);
      Say1("    message 4 bytes = ",(unsigned long)m4len);
      Say ("      Zero nonce and no Key Data, both by the standard.");
      GenerateTxHdr351(gShimTxHdr,gShimTxFrm,m4len,gShimChannel,0xC040,
                       B43_TXH_PHY_ANT01AUTO,1);
      ApShimTxArm();
      DmaPublish();
      PostTxFrameAt(gShimTxBase,gShimTxRing.base,0,
                    gShimTxHdrPhys,(UInt16)TXH_SIZE_351,gShimTxFrmPhys,m4len);
      gShimM4Sent = 1;
      Say("    [ok] ★★★★★★ THE FOUR-WAY HANDSHAKE COMPLETED.");
      /* k208: message 3's replay counter is the floor every group renewal must be above. */
      for(z=0;z<8;z++) gGkReplay[z] = gReplay[z];
      gGkReady = 1;
      gShimHsErr = noErr; }

done:
    if(haveSaved)
      (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
    return (UInt32)(gShimHsErr == noErr);
}

/* 8-4: the handshake result. ⚠ No key material crosses this boundary -- only presence and counts. */
UInt32 AirPortShimGetHs(UInt32 *pmkMs, UInt32 *pmkNz, UInt32 *ptkNz,
                        UInt32 *m1, UInt32 *m3, UInt32 *m1ms, UInt32 *m3ms, SInt32 *err)
{
    if(pmkMs) *pmkMs = gShimPmkMs;
    if(pmkNz) *pmkNz = gShimPmkNz;
    if(ptkNz) *ptkNz = gShimPtkNz;
    if(m1)    *m1    = (UInt32)gShimM1;
    if(m3)    *m3    = (UInt32)gShimM3;
    if(m1ms)  *m1ms  = gShimM1Ms;
    if(m3ms)  *m3ms  = gShimM3Ms;
    if(err)   *err   = (SInt32)gShimHsErr;
    return (UInt32)(gShimHsErr == noErr);
}

/* ── 8-2d-1b: WHICH BUILD OF THIS FRAGMENT IS ACTUALLY RUNNING? ──
 *
 * ⚠ THIS EXISTS BECAUSE k65 WAS LOST TO EXACTLY THE AMBIGUITY IT REMOVES. The app resolved every
 *   old export and failed on the one new one, the driver ran flawlessly, and the log said "the
 *   driver produced no lines" -- which reads as a code defect and was actually a stale .shlb.
 *   CFM builds its fragment registry at boot, so a shim copied into Extensions without a restart
 *   is simply not the shim that runs.
 *
 * ⚠ THE FRESHNESS WITNESS MUST NOT LIVE INSIDE THE THING BEING TESTED. k65's witness was an Open
 *   sequence number carried in the narration -- unreachable precisely when the narration is what
 *   went missing. This is reachable through one plain call that depends on nothing else.
 *
 * ⚠ THE CONSTANT IS DELIBERATELY DUPLICATED IN airport_rx.c, which is normally the exact hazard
 *   this project warns about -- a value copied across a boundary goes stale silently. Here the
 *   MISMATCH IS THE PRODUCT. A disagreement is the answer, not a bug.
 *
 * Bump this on every change to the shim. Absence of the export at all means a fragment older
 * than 8-2d-1b, which is its own, equally decisive answer.
 *
 *   1  8-2d-1b  first build carrying a stamp; ring + Out() + ap_fmt.h narration
 *   2  8-2d-1c  export list cut to nine -- see the hash-table note below
 *   3  8-2d-1d  MakePEF patched; ten exports again, ValidateHW restored
 *   4  8-2d-2   takes ap_bringup.h whole; applies the initvals itself
 *   5  8-2d-3   completes chip_init's prefix and runs ApPhyInitG
 *   6  8-2e     runs chip_init's tail, statements 8-13
 *   7  8-2f     builds and arms its own RX ring; allocates at Open only
 *   8  8-2g     enables the MAC and RECEIVES; owns the card outright
 *   9  8-2g-b   sticky MacEverOn, so a late read is still interpretable
 *  10  8-3a     b43_dma_init's TX half; transmits a unicast probe request
 *  11  8-3a-b   the exchange moved out of Open -- see AirPortShimTxProbe
 *  12  8-3a-c   TX moved to DMA controller 1 (AC_BE), per k9
 *  13  8-3b     authentication -- the driver's first two-way exchange
 *  14  8-3b-b   re-arm per post; match the target SSID; ANT01AUTO
 *  15  8-3c     association -- status 0 and an AID the AP assigned
 *  16  8-4      the WPA2 four-way handshake. ⚠ SECRET-BEARING from here
 *  17  8-4-b    DECLINED is 2, not paramErr; PMK moved before auth
 *  18  8-4-c    RxWaitFor walks the poison, not the cursor (k20's lesson, again)
 *  19  8-6a     Start is ACCEPTED; frames convert to Ethernet and go up via the isr
 *  20  8-6b     the driver SAYS when Start lands, so it can be placed in time
 *  21  8-6d     ★ the SECOND MACFILTER_BSSID write, after association
 *  22  8-7      ★★ the INTERRUPT SOURCE -- ISR + secondary handler, armed on request
 *  23  8-8      ★★ EnetHAL_Write -- OT transmits: Ethernet -> 802.11 + CCMP -> the air
 *  24  8-8-b    DrainTxStatus returns OUR frame's ACK, not a count of ANY status
 *  25  8-9      ★★ the driver JOINS in Open and ARMS at Start -- no app required
 *  26  8-9b     ⛔ k92 GREY-SCREENED. Arm-in-Open OFF, join hard-bounded. One variable.
 *  27  8-9c     the arm SPLIT: INSTALL in Open (device masked), ENABLE still from the app
 *  28  8-9d     ★ the k92 crash FOUND: an unguarded NULL isr call. Guarded; arm back in Open
 *  29  8-9e     ★ a receive QUEUE, not a single slot -- the shim Reads long after the isr
 *  30  8-11     the ndrv SHAPE: DoDriverIO + TheDriverDescription. Not yet loadable.
 *  31  8-11b    nameInfoStr := "pci80211", read off the machine. Still not loadable.
 *  32  8-12     file type 'ndrv' + a Name Registry beacon. Does the DM load us at boot?
 *  33  8-13     ⛔ 8-12 REFUTED: type 'ndrv' breaks CFM by-name. cfrg 'ndrv' member instead.
 *  34  8-14     ⛔ 8-13 REFUTED too. Stop guessing: enumerate the node's properties.
 *  35  8-15     ⛔ 8-14 REFUTED. Node-named fragments are the ROM route; an EXTENSION uses
 *                the OTKrnl$ PORT SCANNER. OTScanPorts added, plus the property VALUES.
 *  36  8-16     ⛔ 8-15 REFUTED too, and the prior-art sweep says there is no discovery to
 *                join: the DRIVER registers itself. The boot vehicle (68K INIT -> PPC
 *                fragment -> NM) ships as a second extension; the shim is unchanged.
 *  37  8-16b    ⛔ k104 CRASHED at Finder time: the INIT handed CFM the RESOURCE HANDLE's
 *                memory, which the System freed when it closed the extension's resource
 *                file. NewPtrSys copy now, per bootmain.c:1840. Shim unchanged again.
 *  38  8-17     ★★★ the boot vehicle now CALLS EnetShimInstallDriver + AsyncStatus(Link).
 *                The port is registered before any application runs. Shim unchanged.
 *  39  8-18     ★★★★★★ k106 PUT THE PORT IN TCP/IP. Renamed to "AirPort Extreme"; the
 *                app's driver oracles are witness-aware. Shim unchanged.
 *  40  8-19     ⚠ k107: OT never drove the port and the app could not tell whether our
 *                driver was called in ANOTHER COPY. EnetHAL_Open now beacons to the Name
 *                Registry, outside every fragment instance.
 *  41  8-21     ★★★ k110: the port OPENS and EnetHAL_Open RUNS -- but in a copy the app
 *                cannot see. The driver now drains its ring to "AirPort Driver Log" at
 *                the end of Open, guarded by CurrentExecutionLevel()==0.
 *  42  8-22     ⚠ 8-21 dumped at the END OF OPEN ONLY, so the log could never contain
 *                EnetHAL_Start -- which arrives after it. Dump at Start, Status and
 *                Close too; the execution-level guard is what makes that safe.
 *  43  8-23     ⛔ OT never calls Status (0 polls) and never Closes, so 8-22 still could
 *                not see past Start. AirPortShimSnapshot(), pumped by the boot vehicle's
 *                NM on a TickCount schedule -- the only task-level clock we own.
 *  44  8-24     ⚠ k114 anchored that schedule at REGISTRATION -- i.e. during boot, before
 *                anyone could touch TCP/IP. AirPortShimGetPhase() lets the pump wait for
 *                the BIND and anchor there instead.
 *  45  8-25     ★ k116: the data path WORKS (10,920 bytes up, 29 writes down) and DHCP
 *                still fails. Histogram what we deliver -- IP/DHCP/ARP/EAPOL -- and move
 *                the snapshot to its own appended file; the ring dropped two of four.
 *  46  8-26     ★ k117: DHCP-port frames DO reach OT -- but all broadcast, and the TX
 *                outcome has never been in a snapshot. Report TX sent/ACKed/refusals, and
 *                whether a delivered DHCP frame's chaddr is actually OUR MAC.
 *  47  8-27     ★★ k118: the server ANSWERS US (20 OFFERs, our chaddr) and OT ignores
 *                them. TX ACKed==0 was an artefact -- nothing drains the status. Capture
 *                the first three delivered DHCP frames verbatim and print them.
 *  48  8-27b    ⚠ EnetShim.h says Status=10; EthernetShim.pdf says SetPacketFilter=10,
 *                Status=11. Widen the selector table to 16 and PRINT the histogram --
 *                a count at 11 settles which document the shipping library follows.
 *  49  8-28     ★★★ THE FIX. k120 captured our own DHCP DISCOVER arriving as a received
 *                frame -- the AP relays a station's broadcast back to the originator. OT
 *                saw its own source MAC on the wire and called Stop x10. Drop on SA.
 *  50  8-30     ⚠ k121 VOID: clean bring-up, zero beacons, joined=0 -- the ~30%
 *                intermittent. The join-failure line could not tell a deaf radio from an
 *                absent network; it now prints the receive census.
 *  51  8-31     ★★ k122: auth request ACKed, no auth response. This driver has NEVER sent
 *                a deauth, so the AP holds stale state for our MAC after every session.
 *                Send one before authenticating.
 *  52  8-32     ⚠ prior art WEAKENED the deauth theory (hostapd accepts a re-auth). The
 *                real anomaly is zero beacons in 500 ms with BEACPROMISC set. Read MACCTL
 *                live in the census: source claims vs chip state.
 *  53  8-33     ★ AirPortShimRejoin(): the pump re-runs the WHOLE bring-up when the radio
 *                came up deaf. Three runs in a row were lost to intermittents that a
 *                TCP/IP re-select clears by hand.
 *  54  8-35     ★ THREE runs show the receiver working during the scan and stopping once
 *                we transmit. Capture the RX engine's four registers before the first TX
 *                and print them beside the post-wait values.
 *  55  8-35b    ⛔ k127 CRASHED: those four reads were unguarded and OT opened the driver
 *                from a background app's copy, where gBus.bar0 is zero. Null test first,
 *                then the window check -- ApShimWindowIsOurs reads through bar0 itself.
 *  56  8-36     ★★★ THE PRIOR-ART SWEEP, ANSWERED IN INSTRUMENTS RATHER THAN THEORIES.
 *                Decoding k120's captured frames by hand found that every DHCP reply the
 *                server sends us carries yiaddr = 0.0.0.0 -- so "OT ignores them" may be
 *                OT behaving correctly. Three measurements settle it at the point of
 *                delivery: the UDP checksum (the only thing covering the payload, never
 *                computed here before), the DHCP message type by name, and yiaddr.
 *                Plus: a SECOND RX-register snapshot the moment the scan succeeds, which
 *                bisects k128's three-step span; RxRingArm's and RxRecycle's return values
 *                kept instead of cast away; and the snapshot line that printed gShimMacOk
 *                under a caption claiming it was the chaddr match is corrected.
 *  57  8-36b    ⛔⛔ k129 CRASHED ON MY OWN UNBOUNDED READ, and the real defect was that the
 *                parser was INLINE IN A SELECTOR where no host test could reach it. The BOOTP
 *                length came off the wire and only one of the three reads using it was
 *                bounded. Moved to ap_enet.h as ApIpInspect() with the bound computed ONCE,
 *                and enet_test.c now runs it on mmap'd guard pages -- verified by deleting
 *                the clamp and watching the suite take SIGBUS. Same shape as k127: a new
 *                read on a hot path, inline, without the guard its neighbours already had.
 *                ★ Tiger, same card and MAC on this AP: yiaddr 192.168.1.225, type ACK.
 *                  The server allocates to us. "It is refusing us" is dead.
 *  58  8-37     ★★★ THE k130 RUN, READ BY AN INDEPENDENT SNIFFER, NAMED THE FAILURE. Our
 *                driver transmits DHCP, the server answers, and every "yiaddr=0" reply we
 *                puzzled over for weeks was a NAK (OT asked for the Ethernet's .90 lease on
 *                the card). OT then DISCOVERs and gets no completing offer. The driver's own
 *                snapshot -- which would show whether WE deliver the offer -- never ran,
 *                because the boot vehicle cached a CFM connection across the boot->task
 *                boundary (FindSymbol -2801). This build: the snapshot now also reads the
 *                RX engine LIVE, so "OFFER=0" separates "RX alive, none delivered" from
 *                "RX DISABLED (k125)". The connection fix is in ap_boot.c (8-37). And it dumps
 *                the per-reason drop histogram: the NAKs we received are BROADCAST (group/GTK),
 *                a DHCP OFFER is UNICAST (pairwise/PTK) -- a distinct key path never proven on
 *                received traffic. DECRYPT_FAILED(pairwise)>0 with GROUP==0 names it at once.
 *  59  8-38     ⛔ k131's snapshot STILL did not run: FindSymbol from the NM pump fails -2801
 *                intermittently because GetSharedLibrary itself fails in a foreign CFM context.
 *                Stop depending on the boot vehicle reaching across fragments -- the DRIVER
 *                dumps its own snapshot from EnetHAL_Entry, guarded task-level and throttled,
 *                on whatever selector OT calls during the DHCP phase (Write). No cross-fragment
 *                call. (k131 was also a deaf-radio run -- the ~30% intermittent -- so it had no
 *                DHCP to capture regardless; this makes the NEXT good-radio run yield the data.)
 *  60  8-39     ⛔ FOUR deaf runs in a row (k131, k132 x3), incl. one after a 60s unplugged
 *                cold-off -- so it is NOT power-off timing and NOT the ~30% intermittent alone.
 *                PROVEN not-hardware (Tiger got a DHCP lease on this card the same day) and
 *                not-a-regression (bring-up is byte-identical to k130, the one run that joined).
 *                The differing input must be what the bring-up MEASURES from the radio, and the
 *                fifteen PHY oracles all check WRITES, not those measured results. So print the
 *                RX-sensitivity calibration -- NRSSI slope, RX gain chain, the NRSSI-restore
 *                flag -- on every join attempt, to diff against the j7 reference (max_lb_gain 31,
 *                slope ~97541) and name the one value that is wrong.
 *  61  8-40     ⛔ SEVEN deaf runs, incl. a 40-min cold rest -- thermal ruled out; a persistent
 *                regime change on identical code. k133 showed the receiver WORKS (the MAC hears
 *                the AP's ACK) but frames never reach the DMA ring, and the RX-register census
 *                read all zeros. That census trusted ApShimWindowIsOurs() rather than selecting
 *                the core, so a zero could be the instrument, not the engine. k134 makes it
 *                save/select/read/restore like the snapshot, so the next run says DEFINITIVELY
 *                whether the RX DMA is running during the join or genuinely off.
 *  62  8-41     ★★★★★★ THE FIX. k134 proved (wire + our own count, one good boot) that the
 *                server answers ~1.1 ms after each DHCP send, broadcast, and the interrupt path
 *                delivers NONE of them -- while the JOIN catches its post-send responses every
 *                time because it POLLS the ring right after transmitting. So ApShimWriteFrame
 *                now does the same: after posting, spin briefly and pump (task-level guarded,
 *                bounded ~20 ms), so the reply is delivered instead of missed. OT then gets the
 *                NAK, drops the stale .90, DISCOVERs, gets the OFFER, and pulls a real address.
 */
#define AP_SHIM_BUILD 100
/* ⚠ k203: KEPT AT 1 DELIBERATELY -- no longer a diagnostic ([[feedback_sweep_concluded_experiments]]).
 * Born at 8-61 (k164) as "qopen succeeds without a radio join (diagnostic only; 0 for real)" and left
 * on for ~38 builds, which is why the failed k202 boot showed TCP/IP on a self-assigned 169.254
 * address instead of "port unavailable". Decided now, on purpose: the port stays openable when the
 * join fails, as Ethernet does with no cable, because the background rejoin that comes next needs
 * the stream open to deliver into when the link comes up. With 0, the failure is louder but no more
 * recoverable. Revisit if the rejoin is never built.
 * ★ k204: the rejoin IS built (ApLinkStart / ApLinkTaskBody) and depends on this: a boot whose join
 *   failed keeps its stream, and TCP/IP gets its lease once the link layer joins. */
#define AP_OTM_DIAG_NOJOIN 1
#define AP_OTM_BIND_ONLY   0   /* 8-76 k182: RX/ISR back ON (COLD boot required). k181 cold-boot proved the bring-up MMIO crash was warm-restart; and OT won't bind with the card unarmed. Re-arm; a cold+RX-on OTStrLength would confirm the RX path needs the Apple-exact rewrite. */

/* ============================================================================================
 * ★★★★★★ STAGE 8-6a — ACCEPT Start, AND DELIVER FRAMES UP THE DLPI STACK.
 *
 * ★ THE CONTRACT, READ FROM APPLE'S OWN SAMPLE RATHER THAN GUESSED.
 * usb-ddk/Examples/USBEnetSample/ShimEnetHAL.c:66 is four lines long and is the whole of it:
 *
 *     case EnetHAL_Start:
 *         USBSetisr(enet->ioCompletion, enet->ioMisc);
 *         USBStartupRead();
 *
 * So Start hands us a ProcPtr and a refcon. When a frame arrives the driver calls
 * `(*isr)(cookie)` (USBEnetDriver.c:1167) and the shim comes back down through EnetHAL_Read to
 * collect it (ShimEnetHAL.c:84). Delivery is PUSH-notify, PULL-collect. OT never calls Read
 * unprompted -- which is exactly why k84's log shows Read x0: we had never called an isr.
 *
 * ★ WHY THE APP PUMPS THIS, AND WHY THAT IS NOT A CHEAT.
 * Apple's sample calls the isr from a USB completion routine -- interrupt context. We have no
 * interrupt handler yet; 8-2's receiver is polled. Installing one is the single most dangerous
 * thing this driver could do (a hang at interrupt level on OS 9 has no NMI, and this project has
 * hard-locked machines three times on exactly that), so it is NOT bundled with the first run of
 * an unproven contract.
 *
 * Instead AirPortShimPump() is exported and the app calls it AT TASK LEVEL. Every part of the
 * contract is exercised -- convert, stage, call the isr, serve the shim's Read -- and the only
 * thing deferred is what wakes it. 8-6b replaces the pump with a real interrupt source, and by
 * then the contract above is either proven or has already been corrected.
 *
 * ⚠ THE ORACLE IS gDlpiConsumed, NOT gDlpiIsrCalls. Calling the isr proves we can reach the
 *   shim. It does NOT prove the shim believes us. The shim is expected to call Read back
 *   SYNCHRONOUSLY from inside the isr, so Pump checks whether the staged frame was taken by the
 *   time the isr returns. Consumed > 0 is the AP-equivalent evidence here: a subsystem we do not
 *   control acted on what we handed it.
 */
static ProcPtr gShimIsr        = NULL;
static UInt32  gShimIsrCookie  = 0;
static int     gShimStarted    = 0;
static UInt32  gShimOpens      = 0;      /* completed Opens; read via AirPortShimGetPhase */
static UInt32  gRejoinRuns     = 0;      /* 8-33: pump-driven re-bring-up attempts */
static int     gShimStartSeen  = 0;      /* Start arrived, even if we later Stop */

/* ★★★ THE RECEIVE QUEUE — 8-9e, AND IT IS APPLE'S DESIGN, FINALLY COPIED PROPERLY.
 *
 * ⛔⛔ k95 CALLED THE isr 19 TIMES, THE SHIM CAME BACK DOWN 15 TIMES, AND EVERY ONE OF THOSE
 *   READS WAS EMPTY. The staging area was a SINGLE SLOT, set just before the isr call and
 *   cleared the instant it returned:
 *
 *       gShimEnetPending = 1;  (*isr)(cookie);  gShimEnetPending = 0;
 *
 *   That is only correct if the shim reads SYNCHRONOUSLY from inside the notification. At task
 *   level it did -- k88 through k94 collected 10 and 12 frames and the design looked sound.
 *   From SECONDARY INTERRUPT level it does not: the shim defers Read to task time, and by the
 *   time it arrives the slot has been cleared and reused. Safe by ordering again, and the
 *   ordering changed again.
 *
 * ★ AND APPLE'S SAMPLE SAYS SO IN THE CODE I ALREADY READ. USBEnetDriver.c:1165 is:
 *
 *       AddToRcvList(pb->usbRefcon);       // queue it
 *       if (gGlobals->isr) (*gGlobals->isr)(gGlobals->cookie);
 *
 *   and USBReadData() calls RemoveFromRcvList() whenever the shim gets round to it. A LIST,
 *   not a slot -- precisely because the producer is an interrupt and the consumer is not. I
 *   copied the notify and the Read and left the queue between them behind.
 *
 * ⚠ IT DROPS THE OLDEST ON OVERFLOW, and counts it. A receive queue that blocks its producer
 *   would be blocking an interrupt handler. */
#define AP_ENET_Q   8

static UInt8   gEnetQBuf[AP_ENET_Q][AP_ENET_MAXFRAME];
static UInt32  gEnetQLen[AP_ENET_Q];
static volatile UInt32 gEnetQHead = 0, gEnetQTail = 0;   /* head = next to write, tail = read */
static volatile UInt32 gEnetQDrops = 0, gEnetQMaxDepth = 0;

static UInt32 ApEnetQDepth(void)
{ return (gEnetQHead - gEnetQTail); }

/* Producer: secondary interrupt level. */
static UInt8 *ApEnetQReserve(void)
{
    if(ApEnetQDepth() >= (UInt32)AP_ENET_Q){
      gEnetQTail++;                 /* drop the OLDEST, never stall the interrupt */
      gEnetQDrops++; }
    return gEnetQBuf[gEnetQHead % AP_ENET_Q];
}
static void ApEnetQCommit(UInt32 len)
{
    UInt32 d;
    gEnetQLen[gEnetQHead % AP_ENET_Q] = len;
    gEnetQHead++;
    d = ApEnetQDepth();
    if(d > gEnetQMaxDepth) gEnetQMaxDepth = d;
}
/* Consumer: EnetHAL_Read, whenever the shim gets round to it. */
static int ApEnetQPop(UInt8 *dst, UInt32 max, UInt32 *lenOut)
{
    UInt32 i, n;
    if(gEnetQTail == gEnetQHead) return 0;
    n = gEnetQLen[gEnetQTail % AP_ENET_Q];
    if(max && n > max) n = max;
    for(i=0;i<n;i++) dst[i] = gEnetQBuf[gEnetQTail % AP_ENET_Q][i];
    gEnetQTail++;
    if(lenOut) *lenOut = n;
    return 1;
}

static UInt32  gDlpiPumped = 0, gDlpiIsrCalls = 0, gDlpiConsumed = 0;
static UInt32  gRxDhcpFlagCleared = 0;   /* 8-50 (k145): server replies whose broadcast flag we reset to 0 */
static UInt32  gRxDhcpDstRewrit   = 0;   /* 8-55 (k152): broadcast DHCP replies whose L2 dst we made unicast */
static UInt32  gRxArpDstRewrit    = 0;   /* 8-56 (k153): broadcast who-has-.225 ARP requests made unicast */
/* 8-52 (k147): the ping test proved the driver joins + moves frames but never completes a
 * round-trip. These capture WHAT we drop and WHO is talking, to pinpoint the receive/decrypt fault.
 * gDropCap: the plaintext 802.11 header (FC + addr1/2/3, always unencrypted) of the first frames we
 * DROP for a decrypt/format reason -- shows broadcast(GTK) vs unicast-to-us(PTK) and the sender.
 * gRxArp*: decode of received ARP (op / sender-IP / target-IP) -- did the Pi's who-has-.225 arrive?
 * gTxArp*: decode of OT's outgoing ARP -- its sender-IP is OT's ACTUAL address (.225 vs 169.254). */
static UInt8   gDropCap[6][32];
static UInt32  gDropCapWhy[6], gDropCapN = 0;
static UInt32  gRxArpN = 0, gRxArpWhoHasUs = 0, gRxArpLastOp = 0, gRxArpLastSpa = 0, gRxArpLastTpa = 0;
static UInt32  gTxArpN = 0, gTxArpLastOp = 0, gTxArpLastSpa = 0, gTxArpLastTpa = 0;
/* 8-53 (k148): does the AP ACK our UNICAST data frames? k147 showed OT replies to the ARP but the
 * reply never reaches the Pi. Sample the transmit status (XMITSTAT, cookie 0xC0E0) of the first few
 * unicast data frames -- the ARP reply among them. ACKed = it reached the AP; not-acked = our
 * unicast uplink never gets there. */
static UInt32  gTxUniSampled = 0, gTxUniAcked = 0, gTxUniNoAck = 0, gTxUniLastAck = 0;
static volatile UInt32 gDlpiNoIsr = 0;  /* converted with no notify proc to take it */
static UInt32  gDlpiReadCalls = 0, gDlpiReadBytes = 0, gDlpiReadEmpty = 0;
/* 8-25: an EtherType histogram of what actually reaches Open Transport. */
static UInt32  gRxUpIp = 0, gRxUpArp = 0, gRxUpEapol = 0, gRxUpOther = 0;
static UInt32  gRxUpDhcp = 0, gRxUpUdp = 0, gRxUpIcmp = 0, gRxUpBcast = 0;
static UInt32  gRxUpDhcpForUs = 0;   /* BOOTP chaddr matches our own MAC */
static UInt8   gDhcpCap[3][352];     /* 8-27/8-54: first three verbatim; k151 widened 96->352 to
                                      * reach the OPTIONS (they start ~byte 280) -- a 342-byte offer
                                      * now fits whole, so the raw dump backs up the decoded fields. */
static UInt32  gDhcpCapLen[3], gDhcpCapFull[3], gDhcpCapN = 0;

/* ★★★ 8-36: WHY OPEN TRANSPORT IGNORES THE REPLIES, MEASURED INSTEAD OF ARGUED.
 *
 * k120's three captured frames were decoded by hand this session and they are not what the
 * commit message of the day claimed. Frames 2 and 3 are genuine BOOTREPLYs -- our xid
 * e6d8bd00, our chaddr, and an IP header checksum that folds to zero, so the header arrived
 * byte for byte. And both carry yiaddr = 0.0.0.0. No DHCP client on earth accepts an offer
 * with no address in it, so "OT ignores them" may be OT behaving CORRECTLY, and the defect
 * may not be in this driver at all.
 *
 * ⚠ 96 captured bytes of a 342-byte frame cannot tell an OFFER from a NAK. Option 53 sits at
 *   roughly byte 282. Four sessions have been spent reasoning about a field nobody read.
 *
 * ⇒ THE IP HEADER CHECKSUM COVERS TWENTY BYTES AND SAYS NOTHING ABOUT yiaddr. The UDP
 *   checksum is the only thing that covers the payload, and it has never been computed here.
 *   A bad one means our receive path corrupts the payload and OT is right to drop the frame.
 *   A good one means those bytes are the server's own and the search moves off this machine.
 *   That single bit is worth more than the last four builds. */
static UInt32  gIpCkOk = 0, gIpCkBad = 0;
static UInt32  gUdpCkOk = 0, gUdpCkBad = 0, gUdpCkNone = 0;
static UInt32  gDhcpType[9];         /* by RFC 2132 option 53; [0] = no option 53 present */
static UInt32  gDhcpYiSet = 0, gDhcpYiZero = 0;
static UInt32  gDhcpYiaddr = 0;      /* the last NON-ZERO yiaddr a reply actually offered */
/* ★ 8-54 (k151): the decoded options of the last OFFER (msgType 2). opt 54 (server-id) is the one
 * OT MUST have to build a SELECTING REQUEST -- if it is 0 here while offers arrive, that is the
 * missing piece we have never been able to see (it lives past the 96-byte capture). */
static UInt32  gOffServerId = 0, gOffMask = 0, gOffRouter = 0, gOffDns = 0, gOffLease = 0;
static UInt32  gOffOptCount = 0, gOffSawEnd = 0;

/* ⚠ 8-36b: the one's-complement sum moved to ap_enet.h as ApOnesSum, with the parser that
 * uses it, so enet_test.c can reach both. It was here, inline beside the counters, for
 * exactly one build -- the one that crashed. */
static UInt32  gDlpiWalked = 0;
static UInt32  gDlpiWhy[AP_ENET_NREASON];
/* k192: frame-size witnesses for the 512-byte-copy fix. gRxBigDelivered counts delivered frames over
 * 438 bytes -- every one of them was dropped before k192. gRxOverBuf counts frames refused because
 * their header claimed more than gPromptBuf holds (should stay ~0 now it is 2400). */
static UInt32  gRxOverBuf = 0, gRxMaxLen = 0, gRxBigDelivered = 0;
/* k193: delivered frames by PHY -- does the AP send OFDM once we advertise 802.11g, and do we decode it? */
static UInt32  gRxOfdm = 0, gRxCck = 0, gRxOfdmRate[16];
static UInt32  gRxCckRate[5];     /* k196: CCK data frames at 1, 2, 5.5, 11 Mbps, and anything else */
/* ★★★ k197: RECEIVE QUALITY. k196's capture pinned the outage down: for 4.8 minutes the Pi's 1460-byte
 * retransmissions never reached the Mac while the SAME link carried file sharing's 16-byte keep-alives
 * every 30 s, both ways -- radio, ring, interrupts and OT all alive. Size-dependent loss is what a
 * marginal link does (a 1500-byte frame fails ~20x as often as a 70-byte one), so this measures the
 * reception itself. MACCTL KEEP_BAD (b43_adjust_opmode's FIF_FCSFAIL arm) makes the MAC hand us frames
 * that failed their FCS; RxWaitFor (ap_wait.h) counts them -- by PHY, rate and size, and whether addr2
 * reads as our AP -- and steps past them, so garbage is never parsed or delivered on any path. This
 * side counts the GOOD frames the same way. RX_MAC_RESP ("a response frame was transmitted") says our
 * chip ACKed a good unicast frame. JSSI (the RX header's signal byte) is histogrammed both sides. */
static UInt32  gRxGoodLen[3];                 /* good data frames by length: <= 200, 201-1000, > 1000 */
static UInt32  gRxGoodAnt[2];                 /* good data frames by the antenna they arrived on */
static UInt32  gRxRespSet = 0, gRxRespClear = 0;     /* good unicast-to-us data frames: we ACKed / did not */
static UInt32  gRxJssiGoodOfdm[8], gRxJssiGoodCck[8];  /* JSSI in eight 32-wide buckets */
static int     gRxKeepBadOn = 0;              /* MACCTL KEEP_BAD was set for the data path */
/* ⛔ k199: KEEP_BAD IS OFF AGAIN -- the experiment is concluded ([[feedback_sweep_concluded_experiments]]).
 * Its question ("are the big frames arriving CORRUPTED?") was answered by k198: the outage was our own
 * Bluetooth driver paging, and with Bluetooth unplugged the counter read FCS FAILED = 0 across ~8200
 * frames. It never saw one failed frame in any run, so it is also unproven as an oracle (maybe this
 * ucode drops them regardless). Either way it adds nothing, and b43 runs with it clear. The counters
 * and the snapshot section stay, reading KEEP_BAD 0; set this to 1 to measure again. */
#define AP_RX_KEEP_BAD 0

/* ⚠ A GUARD THAT CANNOT LATCH. The shim calls Read from inside the isr, so Read re-enters this
 * fragment while Pump is still on the stack. That is expected and fine -- Read touches only the
 * staging buffer. What must never happen is two Pumps interleaving, so Pump refuses re-entry --
 * and releases the flag on EVERY exit path, including the early ones. A guard that latches is
 * the prime suspect for its own symptom; see the project's own rule. */
static int gPumpBusy = 0;

/* Forward declarations: the pump's body and the interrupt state live below, because 8-7's block
 * reads better next to the ISR it belongs to than spliced in here. */
static UInt32 ApShimPumpLocked(UInt32 maxFrames);
static int    gIrqArmed;

/* ★★★ THE PUMP. Task level only -- the app calls it. Returns frames delivered this call. */
UInt32 AirPortShimPump(UInt32 maxFrames)
{
    UInt32 delivered = 0;
    UInt32 savedWin = 0;
    int haveSaved;

    /* ⚠ GATED ON Start HAVING BEEN SEEN, NOT ON STILL BEING STARTED -- and that is deliberate.
     *
     * k84's selector trace ends `... Start Stop Stop Close`: OT issues a Stop after every Start
     * it does not like. If this gated on gShimStarted, and OT keeps that habit now that Start
     * succeeds, the pump would silently do nothing and the run would report "0 frames" for a
     * reason indistinguishable from a broken receiver. That is the sentinel-collision mistake
     * this stage has already made three times -- "we did not try" wearing the same clothes as
     * "we tried and it did not work".
     *
     * So the pump runs on gShimStartSeen and the CURRENT state is reported separately, through
     * the startSeen out-parameter. The log then says which of the two situations it was in. */
    if(gPumpBusy) return 0;
    if(!gShimStartSeen || !gShimIsr) return 0;
    if(gShimDmaErr != noErr || !gShimMacEverOn) return 0;
    gPumpBusy = 1;

    /* ⚠ WHILE THE INTERRUPT IS ARMED, THE SECONDARY HANDLER OWNS THE RING. Two pumps walking
     * the same cursor from two execution levels is the kind of race that produces a log nobody
     * can read. The OS runs at most one secondary handler at a time, so the secondary needs no
     * lock against itself -- it needs one against THIS. */
    if(gIrqArmed){ gPumpBusy = 0; return 0; }

    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      gPumpBusy = 0;                                  /* ⚠ released on the early path too */
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    delivered = ApShimPumpLocked(maxFrames);

    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                              (LogicalAddress)SSB_BAR0_WIN,savedWin);
    gPumpBusy = 0;
    return delivered;
}

/* ★★★ THE PUMP'S BODY, WITH NO CONFIG-SPACE ACCESS OF ITS OWN -- split out for 8-7.
 *
 * ⚠⚠ THE SPLIT IS THE POINT: the caller owns the BAR0 window, because the two callers cannot
 *   manage it the same way.
 *
 *     AirPortShimPump (task level)     may save / select / restore through PCI config space
 *     ApShimSecondaryHandler (IRQ)     MAY NOT -- config space is not safe there. It VERIFIES
 *                                      the window instead and refuses if it has moved.
 *
 * Everything below is MMIO and computation: ring walk, AES-CCM, the address rules, and a call
 * into the shim's notify proc. Apple's own sample calls that proc from a USB completion routine
 * -- secondary interrupt level -- so this is the context it was designed for.
 *
 * ⛔ NO ALLOCATION, NO FILE MANAGER, NO NAME REGISTRY may ever appear on this path.
 *   check-exec-level.py audits it from both roots. */
/* ★★★ 8-58: NATIVE-MODULE RX. When OT drives our own OTModl$AirPortBCM (ApOtmHalOpen set this),
 * the pump hands each converted Ethernet frame UP our DLPI read stream via ApOtmRxInject instead of
 * committing to the EnetShim queue -- the inbound-IP path Apple's shim never provided.
 *
 * ⚠ gOtmRxScratch is a single static build buffer, and that is safe HERE for a specific reason: in
 *   native mode there is no application, so nothing calls the TASK-level AirPortShimPump; the ONLY
 *   caller of ApShimPumpLocked is ApShimSecondaryHandler, and OS 9 does not re-enter a secondary
 *   handler. One pump, one buffer. If a task-level pump is ever re-introduced in native mode this
 *   assumption breaks -- use ApEnetQReserve's ring then. */
static int     gOtmNativeMode   = 0;
static UInt32  gDlpiOtmInjected = 0;
static UInt8   gOtmRxScratch[1600];
/* 8-59: the ISR does NOT call OT STREAMS primitives (that crashed k160 in OTStrLength). It only
 * ENQUEUES the converted frame (bounded memcpy) and SCHEDULES a deferred task; ap_otmodl.c's
 * deferred proc does the allocb/putnext at a safe level. Both of these are interrupt-safe. */
extern void    ApOtmRxEnqueue(const unsigned char *frame, unsigned long len);  /* ap_otmodl.c */
extern void    ApOtmScheduleRx(void);                                          /* ap_otmodl.c */

/* ★★★ k204: THE LINK LAYER'S FACTS, recorded by the receive pump (ap_link.h has the why).
 *
 * The pump is the one place every frame from our BSSID passes, and through k203 it dropped every
 * management frame -- beacons, and the deauthentication an AP sends when it throws us off -- as "not
 * data". It now RECORDS two facts and decides nothing: the tick of the latest beacon from our BSSID, and
 * each deauth/disassoc addressed to us (kind + reason written BEFORE the sequence number, so a reader
 * that sees the new number sees the new reason). EAPOL frames after the join -- the AP re-keying -- are
 * counted and kept from Open Transport, which has no stream bound to 0x888E and dropped them unseen.
 * Every DECISION is made at task level (ApLinkTaskBody), so no link state is written from two levels.
 *
 * ⚠ SECONDARY INTERRUPT LEVEL -- in native mode the pump's only caller is ApShimSecondaryHandler, which
 *   OS 9 never re-enters. Word stores and ap_link.h's bounded reads, nothing else. NOT Say(): ap_ring.h's
 *   cursor is three non-atomic stores with exactly one writer, task level. Events go to their own small
 *   ring instead -- one writer (here), one reader (the task body), published by the index. */
static volatile UInt32 gLinkTick        = 0;   /* THE link clock: heartbeat runs, one per AP_LINK_TICK_MS */
static volatile UInt32 gLinkLastBcnTick = 0;   /* gLinkTick when our BSSID's latest beacon was walked */
static volatile int    gLinkBcnSeen     = 0;
static volatile UInt32 gLinkBcnCount    = 0;
static volatile UInt32 gLinkKickSeq     = 0, gLinkKickReason = 0;
static volatile int    gLinkKickKind    = 0;
static volatile UInt32 gLinkKicks[2]    = { 0, 0 };            /* deauth, disassoc */
static volatile UInt32 gLinkEapolN[5]   = { 0, 0, 0, 0, 0 };   /* by AP_LINK_EAPOL_* */
/* Set while the link is down: ApShimWriteFrame answers "try later", so OT's frames wait on the write
 * queue (k202's back-pressure) instead of being encrypted with a dead key into an association the AP has
 * forgotten. 0 outside native mode, so the shim-era paths are untouched. */
static volatile int    gLinkTxBlocked   = 0;
/* The task body's gLink.up, mirrored in one word: the heartbeat's hint reads it, and (k205) so does
 * ApStatBeacon, which must not paint a signal for a network we are not associated with. */
static volatile int    gLinkUpFlag      = 0;
/* k207: the user's AirPort switch, as the task body last set it (ApLinkSetPower). One word, so the
 * heartbeat can pace itself by it. */
static volatile int    gRadioPowerOn    = 1;

#define AP_LINK_EVT_KICK   1
#define AP_LINK_EVT_EAPOL  2
#define AP_LINK_EVT_GTK    3     /* k208: a group message 1 answered or refused: sub = AP_GK_*, code below */
#define AP_LINK_EVT_GTK_M2 4     /* k208: a held message 2's fate: sub = AP_GK_M2_*, code = tries */
/* k208: the receive pump's group frames, by the key index each one named (ap_gtk.h ApGtkFrameKeyId):
 * delivered, dropped for no key at that index, and failing to decrypt under the key that is there. A
 * renewal shows as the count moving from one index to the other. Written only by the pump. */
static UInt32 gRxGkOk[4], gRxGkNoKey[4], gRxGkBad[4];
static void   ApGkQueueM1(const UInt8 *eth, UInt32 len); /* k208: below ApShimTxFromPump, with the rest */
static void   ApGkService(void);
typedef struct { UInt32 tick; UInt8 kind, sub; UInt16 code; } ApLinkEvt;
#define AP_LINK_EVT_N 32u
static ApLinkEvt       gLinkEvt[AP_LINK_EVT_N];
static volatile UInt32 gLinkEvtW = 0;          /* written ONLY by the pump */
static UInt32          gLinkEvtR = 0;          /* written ONLY by the task body */

static void ApLinkEvtPut(UInt8 kind, UInt8 sub, UInt16 code)
{
    ApLinkEvt *e = &gLinkEvt[gLinkEvtW % AP_LINK_EVT_N];
    e->tick = gLinkTick; e->kind = kind; e->sub = sub; e->code = code;
    gLinkEvtW++;                                 /* publish only after the slot is complete */
}

/* ★★ k205: THE STATUS BLOCK's live half (ap_status.h has the contract). Written HERE, at secondary level,
 * as single aligned words only -- signalDbm and quality -- from each beacon of OUR AP, and only while the
 * link is up: during an outage the task body has already set Out of Range, and a beacon heard then must
 * not paint a signal for a network we are not on. The EWMA state below is touched only by this path
 * (the task body just asks for a restart through gStatReset, one word, after a rejoin). */
static ApStatBlock  *gApStat       = 0;        /* set once at task level (ApStatPublish), before the arm */
static ApStS32       gStatSmooth16 = 0;        /* smoothed dBm x16 */
static volatile int  gStatReset    = 1;        /* 1 = next beacon starts the average afresh */
#define RXH_PHY_STATUS3  10                    /* b43_rxhdr_fw4.phy_status3 (format 351: 0 fl, 4 ps0, 6 jssi, 10 ps3) */

static void ApStatBeacon(const UInt8 *rxbuf)
{
    ApStS32 dbm;
    if(!gApStat || !gLinkUpFlag) return;
    dbm = ApRssiDbm(rxbuf[RXH_JSSI], le16at(rxbuf + RXH_PHY_STATUS0), le16at(rxbuf + RXH_PHY_STATUS3),
                    (const short *)gNrssiLt);
    if(gStatReset){ gStatSmooth16 = dbm * 16; gStatReset = 0; }
    else          gStatSmooth16 += (dbm * 16 - gStatSmooth16) / 8;   /* ~8 beacons (~0.8 s) of memory */
    gApStat->signalDbm = gStatSmooth16 / 16;
    gApStat->quality   = ApStatQuality(gStatSmooth16 / 16, gApStat->quality);
}

/* A frame ApRxToEnet refused as NOT DATA: our beacon, or a kick. ApRxToEnet's own length rule
 * (frame_len includes the PLCP and the FCS); the pump has already refused anything longer than the
 * buffer it copied (TOO_BIG), so the frame is wholly inside gPromptBuf. */
static void ApLinkNoteMgmt(const UInt8 *rxbuf)
{
    UInt32 raw = (UInt32)le16at(rxbuf + RXH_FRAME_LEN);
    unsigned short reason = 0;
    int k;
    if(raw <= K6_HDR_PLCP6 + 4UL) return;
    k = ApLinkClassifyMgmt(rxbuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6,
                           (ApLinkU32)(raw - K6_HDR_PLCP6 - 4UL), gShimTgtBssid, gShimMac, &reason);
    if(k == AP_LINK_MGMT_BEACON){
        gLinkLastBcnTick = gLinkTick; gLinkBcnSeen = 1; gLinkBcnCount++;
        ApStatBeacon(rxbuf);                     /* k205: the signal the strip module draws */
    } else if(k == AP_LINK_MGMT_DEAUTH || k == AP_LINK_MGMT_DISASSOC){
        gLinkKickKind = k; gLinkKickReason = reason;
        gLinkKickSeq++;                          /* after kind + reason -- see the block above */
        gLinkKicks[(k == AP_LINK_MGMT_DEAUTH) ? 0 : 1]++;
        ApLinkEvtPut(AP_LINK_EVT_KICK, (UInt8)k, (UInt16)reason);
    }
}

/* A DELIVERED frame that is EAPOL: count it, log it, keep it from OT. 1 = it was EAPOL (swallowed).
 * k208: a group message 1 is QUEUED for an answer -- ApGkService answers it once the pump returns (the
 * renewal block below says why there) -- and logs through its own events; the other kinds are still
 * counted and logged only. */
static int ApLinkNoteEapol(const UInt8 *eth, UInt32 len)
{
    unsigned ki = 0;
    int k = ApLinkClassifyEapol(eth, (ApLinkU32)len, &ki);
    if(k == AP_LINK_EAPOL_NONE) return 0;
    gLinkEapolN[k]++;
    if(k == AP_LINK_EAPOL_GROUP_M1) ApGkQueueM1(eth, len);
    else ApLinkEvtPut(AP_LINK_EVT_EAPOL, (UInt8)k, (UInt16)ki);
    return 1;
}

static UInt32 ApShimPumpLocked(UInt32 maxFrames)
{
    UInt32 delivered = 0;

    while(delivered < maxFrames){
      UInt32 ms = 0; int seen = 0; int why = AP_ENET_OK; UInt32 convLen = 0; UInt8 *rbuf = 0;
      unsigned long tbWalk = ApTbNow(), tbConv;       /* k199: stopwatch */
      /* One pass over the ring for ANY data frame from our BSS. toAddr is NULL on purpose:
       * broadcast and multicast are addressed to a group, not to us, and ApRxToEnet applies
       * the real "is this ours" rule where the address roles are already decoded. */
      if(!RxWaitFor(gShimDmaBase,gShimRxBufLog,B43_DMA0_RX_FW351_FO,
                    RXW_ANY_TYPE,RXW_ANY_SUB,gShimTgtBssid,NULL,
                    0,gPromptBuf,&ms,&seen)) break;
      ApTbHistAdd(&gTbRxWalk, ApTbNow() - tbWalk);    /* the walk to this frame + its copy out of DMA */
      gDlpiWalked += (UInt32)seen;

      /* ★ k192: ApRxToEnet reads raw frame_len bytes out of gPromptBuf with no bound of its own. At
       * 512 bytes that over-read cost EVERY frame above a ~438-byte Ethernet frame (tail + MIC were
       * garbage -> "decrypt failed"). Refuse any frame whose RX header claims more than RxWaitFor
       * copied, so it cannot come back whatever size the buffer is. Counted as TOO_BIG. */
      if(B43_DMA0_RX_FW351_FO + (UInt32)le16at(gPromptBuf + RXH_FRAME_LEN) > (UInt32)AP_PROMPT_BUF){
        gDlpiWhy[AP_ENET_TOO_BIG]++; gRxOverBuf++; continue; }

      { const UInt8 *gk; int gkId = -1, gkGroup = 0;
        rbuf = gOtmNativeMode ? gOtmRxScratch : ApEnetQReserve();  /* native: scratch; shim: queue slot */
        /* ★★★ k208: THE GROUP KEY BY THE FRAME'S OWN KEY INDEX. After a renewal the AP moves to the new
         * index while frames under the old one are still arriving, so one "current" group key is wrong for
         * one of the two. ApRxToEnet uses this key only for a group-addressed frame; a unicast frame names
         * index 0 and is decrypted with the TK whatever is passed here. The frame is wholly inside
         * gPromptBuf (TOO_BIG above), and ApGtkFrameKeyId bounds every read by the length it is given. */
        {   const UInt8 *fh = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
            UInt32 raw = (UInt32)le16at(gPromptBuf + RXH_FRAME_LEN);
            if(raw > K6_HDR_PLCP6 + 4UL + 24UL){
                gkId    = ApGtkFrameKeyId(fh, raw - K6_HDR_PLCP6 - 4UL);
                gkGroup = ApEnetIsGroup(fh + 4); } }            /* addr1, the receiver, is a group address */
        gk = ApGtkFor(&gGtkTab, gkId);
        tbConv = ApTbNow();
        if(!ApRxToEnet(gPromptBuf,B43_DMA0_RX_FW351_FO,
                       gPtk + AP_TK_OFF, gPtkReady,
                       gk ? gk : (const UInt8*)gGtk, (gk != 0),
                       gShimMac, rbuf, &convLen, &why)){
          if(why >= 0 && why < AP_ENET_NREASON) gDlpiWhy[why]++;
          if(gkGroup && gkId >= 0){                       /* k208: which index the dropped group frame named */
            if(why == AP_ENET_NO_KEY) gRxGkNoKey[gkId]++;
            else if(why == AP_ENET_DECRYPT_FAILED_GROUP) gRxGkBad[gkId]++; }
          if(why == AP_ENET_NOT_DATA) ApLinkNoteMgmt(gPromptBuf);   /* k204: our beacon, or a kick */
          /* 8-52 (k147): capture the plaintext 802.11 header of a frame we DROP for a
           * decrypt/format reason, so we can see whether it was broadcast (GTK) or unicast-to-us
           * (PTK) and who sent it. The 802.11 header is never encrypted. */
          if((why == AP_ENET_DECRYPT_FAILED_GROUP || why == AP_ENET_NOT_SNAP ||
              why == AP_ENET_DECRYPT_FAILED) && gDropCapN < 6u){
            const UInt8 *fh = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
            UInt32 k; for(k = 0; k < 32u; k++) gDropCap[gDropCapN][k] = fh[k];
            gDropCapWhy[gDropCapN] = (UInt32)why;
            gDropCapN++; }
          continue; }
        ApTbHistAdd(&gTbRxConv, ApTbNow() - tbConv);    /* k199: a DELIVERED frame's decrypt + conversion */
        if(gkGroup && gkId >= 0) gRxGkOk[gkId]++;         /* k208: delivered under the index it named */
        /* ⚠ THE k145/k152/k153 REWRITES ARE SHIM-ONLY. They smuggle broadcast past Apple's shim,
         * which drops L2-broadcast. Our native module delivers broadcast up correctly
         * (ApOtmRxInject sets dl_group_address), so in native mode OT gets the frame UNTOUCHED. */
        if(!gOtmNativeMode){
          if(ApRxClearDhcpBroadcast(rbuf, convLen)) gRxDhcpFlagCleared++;
          if(ApRxDhcpDstToUnicast(rbuf, convLen, gShimMac)) gRxDhcpDstRewrit++;
          if(ApRxArpToUnicast(rbuf, convLen, gShimMac)) gRxArpDstRewrit++;
        } }

      /* ★ k204: EAPOL after the join is the AP re-keying. Counted and logged (ApLinkNoteEapol), not handed
       * to OT -- which has no stream bound to 0x888E and has been dropping these unseen. */
      if(gOtmNativeMode && ApLinkNoteEapol(rbuf, convLen)) continue;

      gDlpiWhy[AP_ENET_OK]++;
      gDlpiPumped++;
      {   /* ★ k197: the GOOD side of the receive-quality picture (RxWaitFor counts the FCS failures). */
          const UInt8 *fh = gPromptBuf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6;
          UInt16 flen = le16at(gPromptBuf + RXH_FRAME_LEN);
          UInt16 ps0g = le16at(gPromptBuf + RXH_PHY_STATUS0);
          UInt8  js   = gPromptBuf[RXH_JSSI];
          gRxGoodLen[flen <= 200u ? 0 : flen <= 1000u ? 1 : 2]++;
          gRxGoodAnt[(ps0g & B43_RX_PHYST0_ANT) ? 1 : 0]++;
          if((ps0g & 0x0003u) == 0x0001u) gRxJssiGoodOfdm[js >> 5]++; else gRxJssiGoodCck[js >> 5]++;
          if(MacEq(fh + 4, gShimMac)){               /* unicast to us: the MAC owes the AP an ACK */
              if(RxMacStatus(gPromptBuf) & B43_RX_MAC_RESP) gRxRespSet++; else gRxRespClear++; }
      }
      if(convLen > gRxMaxLen) gRxMaxLen = convLen;       /* k192: largest frame delivered */
      if(convLen > 438u)      gRxBigDelivered++;         /* k192: each of these was DROPPED before */
      {   /* k193: which PHY delivered it. RX header phy_status0 bits 0-1 are the frame type (b43:
           * 0 CCK, 1 OFDM); an OFDM PLCP's low nibble is the same rate code the TX side writes. */
          UInt16 ps0 = le16at(gPromptBuf + RXH_PHY_STATUS0);
          if((ps0 & 0x0003u) == 0x0001u){ gRxOfdm++; gRxOfdmRate[gPromptBuf[B43_DMA0_RX_FW351_FO] & 0x0Fu]++; }
          else {                         /* k196: which CCK rate -- b43_plcp_get_bitrate_idx_cck's SIGNAL */
              UInt8 sig = gPromptBuf[B43_DMA0_RX_FW351_FO];
              gRxCck++;
              gRxCckRate[sig == 0x0Au ? 0 : sig == 0x14u ? 1 : sig == 0x37u ? 2 : sig == 0x6Eu ? 3 : 4]++; } }

      if(gOtmNativeMode){
        /* ★★★ 8-59: NATIVE DELIVERY, DEFERRED. Enqueue the converted Ethernet frame (bounded
         * memcpy, NO OT calls -- interrupt-safe) and let the ISR schedule the deferred task after
         * this loop. The deferred task does the allocb/putnext up our DLPI stream at a safe level.
         * k160 called allocb here at interrupt level and OS 9 crashed in OTStrLength. */
        ApOtmRxEnqueue(rbuf, convLen);
        gDlpiOtmInjected++;
      } else {
        ApEnetQCommit(convLen);
        /* ⛔⛔ THE gShimIsr NULL CHECK IS WHAT GREY-SCREENED THE MACHINE AT k92. gShimIsr is NULL
         * until EnetHAL_Start hands us one; jumping to 0 at secondary interrupt level kills the
         * machine instantly. The frame is still walked+poisoned when there is no isr, because
         * dropping out would leave RX_DONE (a level condition) to re-raise forever -- a storm. */
        if(gShimIsr){
          gDlpiIsrCalls++;
          (*(void(*)(UInt32))gShimIsr)(gShimIsrCookie);
        } else {
          gDlpiNoIsr++;                          /* converted with nowhere to put it -- counted */
        }
      }
      delivered++; }

    return delivered;
}

/* ============================================================================================
 * ★★★★★★ STAGE 8-7 — THE INTERRUPT SOURCE. The driver wakes itself.
 *
 * Everything up to 8-6 was pumped by the application at task level. This is what replaces it,
 * and it is the most dangerous code in the project: a mistake at hardware interrupt level on
 * OS 9 hangs the machine with no NMI and no debugger, and this project has done exactly that
 * three times (r18, n4, and the r95 bulk_xfer trace on the EHCI work).
 *
 * ⛔ SO IT IS ARMED BY THE APPLICATION, NEVER BY Open. THIS IS THE WHOLE SAFETY DESIGN.
 *
 *   If the ISR were installed from EnetHAL_Open, a handler that hangs would hang at EVERY BOOT
 *   with the extension in place -- the machine would need booting from another volume to remove
 *   it. That is close to bricking the user's OS 9 install, over a driver increment.
 *
 *   Installing only on an explicit AirPortShimArmIrq() call means the next boot is always clean.
 *   It is the same gate 8-2b used for the driver's first WRITE to the card, for the same reason.
 *
 * ★ THE ABI IS NOT GUESSED. Apple's "Designing PCI Cards and Drivers" Ch.11 Listing 11-3, cross
 *   checked against Apple's shipping OHCI UIM disassembly:
 *     1. RegistryPropertyGet(node, "driver-ist", ist, &size)  -> ist[kISTChipInterruptSource]
 *     2. GetInterruptFunctions(...)    -- SAVE the parent's, we must put them back
 *     3. InstallInterruptFunctions(setID, member, refCon, myISR, NULL, NULL)
 *        NULL enabler/disabler = inherit the parent's. Task level only.
 *     4. Install does NOT enable. Call the inherited enabler once.
 *
 * ★ AND THE SPLIT IS b43's OWN. b43_do_interrupt (main.c) reads GEN_IRQ_REASON, returns early
 *   if it is 0xFFFFFFFF (shared line, card gone) or masks to nothing, ACKs the reason registers,
 *   writes GEN_IRQ_MASK = 0 to silence the device, and hands off to a thread. That maps exactly
 *   onto the OS 9 storm-safe pattern: mask in the primary, queue a secondary, let the secondary
 *   do the work and re-unmask. QueueSecondaryInterruptHandler is legal from hardware level;
 *   CallSecondaryInterruptHandler2 is NOT, and is not used here.
 *
 * ⚠ THE OS GUARANTEES AT MOST ONE SECONDARY HANDLER RUNS AT A TIME, so the ring state shared
 *   between the secondary and the app needs no additional lock -- but the app must not pump
 *   while armed, and AirPortShimPump refuses to when it is.
 */
#define B43_MMIO_GEN_IRQ_MASK       0x12CUL
#define B43_MMIO_DMA0_REASON        0x020UL
#define B43_IRQ_DMA                 0x00008000UL
#define B43_DMAIRQ_RX_DONE          0x00010000UL
#define B43_DMA0_REASON_MASK        0x0001FC00UL

static int      gIrqArmed = 0;
static int      gIrqInstalled = 0;
static InterruptSetID        gIstSet = 0;
static InterruptMemberNumber gIstMember = 0;
static void               *gParentRefCon   = NULL;
static InterruptHandler    gParentHandler  = NULL;
static InterruptEnabler    gParentEnabler  = NULL;
static InterruptDisabler   gParentDisabler = NULL;
static UInt32  gIrqMaskWanted = 0;
/* Counters. Every one of these is read at task level by the app; none is ever printed from
 * interrupt level, because the driver's ring Out() is safe there but noisy at IRQ rates. */
static volatile UInt32 gIrqHard = 0, gIrqNotOurs = 0, gIrqQueued = 0, gIrqQueueFail = 0;
static volatile UInt32 gIrqSecondary = 0, gIrqSecFrames = 0, gIrqWindowBad = 0;
static volatile UInt32 gIrqLastReason = 0, gIrqLastDma0 = 0, gIrqRxDone = 0;
/* 8-54 (k149): counts for the ISR's transmit-status drain. If gTxStatIsrDrained climbs, the FIFO
 * is being emptied and unicast transmit can complete on its own (no app needed). */
static UInt32 gTxStatIsrDrained = 0, gTxStatIsrAcked = 0;
/* k195: the receive drain's bound and its witnesses. gRxFireHist counts frames delivered per
 * secondary fire -- 0, 1, 2-4, 5-8, 9-16, 17+; every fire past the 2-4 bucket is one in which k194's
 * budget of 4 left frames stranded. gTxStatFireMax is the most TX statuses one drain popped: the ring
 * puts more frames in flight between drains, and a status FIFO that fills stalls transmit (k148). */
#define AP_RX_ISR_BUDGET  ((UInt32)K3_RX_SLOTS)
static UInt32 gRxFireHist[6], gRxFireMax = 0, gTxStatFireMax = 0;
static volatile int    gIrqInSecondary = 0;

/* ★★★ k196: THE RECEIVE TIMELINE. k195 had a 14 s receive outage in NetBench's DOWN #2: the Pi resent
 * one segment at 1.9, 3.8 and 8.1 s and the Mac's TCP never saw the first three copies -- its IP IDs
 * run on unbroken across the gap, so it sent nothing and lost nothing. The snapshot's totals cannot
 * say WHICH layer went quiet. This samples, every AP_TS_PERIOD_US, from the secondary handler:
 * secondary fires, data frames delivered, non-data frames (beacons), OFDM frames, TX posts, TX
 * statuses, TX acked, the TX rate and ring occupancy, RXSTATUS, the RX cursor and backlog, and the
 * most frames one fire delivered in the interval. Reading an outage off it:
 *     samples keep coming, beacons count, data stops   -> the radio hears the AP's 1 Mbps beacons
 *                                                         but not its data frames
 *     samples keep coming, nothing counts, backlog > 0 -> frames sit in the ring and the walk is not
 *                                                         taking them (a hole in the poison walk)
 *     samples keep coming, nothing counts, backlog 0   -> the receiver itself heard nothing
 *     a GAP in the sample times                        -> the secondary never ran: the interrupt
 *                                                         path went quiet
 * ⚠ Written ONLY here, at secondary-interrupt level (one writer; the OS serialises secondaries), read
 *   at task level by the snapshot. MMIO reads and statics only -- audited with the handler. */
typedef struct {
    UInt32 us;                              /* Microseconds().lo */
    UInt32 fires, rxData, rxNonData, rxOfdm, txPosts, txStat, txAcked;
    UInt32 rxFcs;                           /* k197: FCS failures walked (gRxFcsAny) */
    UInt32 ant0, ant1;                      /* k198: every frame walked, by antenna (gRxWalkAnt) */
    UInt32 rxStatus;                        /* RXSTATUS, raw */
    UInt16 cursor, backlog;                 /* gRxSlot; unpoisoned RX slots */
    UInt8  rate, used, fireMax, pad;        /* TX rate index; TX ring in flight; most frames in one fire */
} ApTsSample;
/* k197: 1 s, was 0.5 s. k196's NetBench took over three minutes (a 4.8-minute outage), so the stream
 * close that prints this came too late for a ~4-minute window. 512 x 1 s = the last ~8.5 minutes. */
#define AP_TS_N          512u
#define AP_TS_PERIOD_US  1000000UL
static ApTsSample gTs[AP_TS_N];
static UInt32     gTsN = 0, gTsLastUs = 0, gTsFireMax = 0, gTsPrinted = 0;

static void ApTsMaybeSample(UInt32 delivered)
{
    UnsignedWide now;
    ApTsSample *s;
    if(delivered > gTsFireMax) gTsFireMax = delivered;
    Microseconds(&now);
    if(gTsN && (UInt32)(now.lo - gTsLastUs) < AP_TS_PERIOD_US) return;
    gTsLastUs = now.lo;
    s = &gTs[gTsN % AP_TS_N];
    s->us        = now.lo;
    s->fires     = gIrqSecondary;
    s->rxData    = gDlpiWhy[AP_ENET_OK];
    s->rxNonData = gDlpiWhy[AP_ENET_NOT_DATA];
    s->rxOfdm    = gRxOfdm;
    s->txPosts   = gTxrPosts;
    s->txStat    = gTxStatIsrDrained;
    s->txAcked   = gTxStatIsrAcked;
    s->rxFcs     = gRxFcsAny;
    s->ant0      = gRxWalkAnt[0];
    s->ant1      = gRxWalkAnt[1];
    s->rxStatus  = ssb_r32(gBus.bar0, gShimDmaBase + B43_DMA32_RXSTATUS);
    s->cursor    = (UInt16)gRxSlot;
    s->backlog   = (UInt16)RxCountUnpoisoned(gShimRxBufLog, B43_DMA0_RX_FW351_FO);
    s->rate      = (UInt8)gTxRateIdx;
    s->used      = (UInt8)gTxr.used;
    s->fireMax   = (UInt8)(gTsFireMax > 255u ? 255u : gTsFireMax);
    s->pad       = 0;
    gTsFireMax = 0;
    gTsN++;
}
static volatile UInt32 gIrqIdleRuns = 0, gIrqBraked = 0;

/* ★★★ k204: THE LINK HEARTBEAT, AND THE TASK-LEVEL PUMP IT FEEDS.
 *
 * The join spins and logs, so it runs only at task level -- and nothing in the native module gets task
 * time after InitStreamModule. The EHCI driver solved exactly this (usb2-ehci/src/ehci_vhub.c, "h26
 * APP-LESS"), from the RE of Apple's own USBMassStorageSupport: NMInstall with nmStr = 0 displays nothing
 * and exists only to have nmResp called at task level, inside whatever process runs its event loop
 * (normally the Finder). NMInstall only ENQUEUES, so it is legal at interrupt level; NMRemove is not.
 *
 * WHO POSTS: this one-shot SetInterruptTimer, re-armed from its own handler -- EHCI's vhub_heartbeat, the
 * same primitive k144 proved here. It also IS the link clock (gLinkTick): stamps and checks both read it
 * (EHCI h83). It posts when there is work: an event to print or act on, a silence that looks like a lost
 * link, or the keepalive (every 5 s up, every 1 s down) that lets the task body run the schedule.
 * ⚠ The receive pump alone could not be the poster: a radio that has gone completely quiet raises no
 *   interrupt, so the one outage that most needs noticing would also stop the noticing.
 *
 * ⚠ TEARDOWN (EHCI lc1): the handler checks gLinkStop FIRST and then does not re-arm, so ApLinkStop's
 *   CancelTimer is final even when the timer fires during it. A secondary handler that has expired runs
 *   before task level resumes, so none can run after TerminateStreamModule returns.
 * ⚠ SECONDARY INTERRUPT LEVEL: counters, word reads, NMInstall, SetInterruptTimer. check-exec-level.py
 *   audits ApLinkTimer as an interrupt root. */
#define AP_LINK_KEEPALIVE_UP_TICKS    20u     /* 5 s: the body's heartbeat and log flush run on this */
#define AP_LINK_KEEPALIVE_DOWN_TICKS   4u     /* 1 s: while down, the rejoin schedule is checked often */
static TimerID         gLinkTimerId      = 0;
static volatile int    gLinkRunning      = 0, gLinkStop = 0, gLinkTimerArmed = 0;
static volatile UInt32 gLinkTimerRuns    = 0, gLinkTimerFails = 0, gLinkTimerRevived = 0;
static NMRec           gLinkNm;                     /* the pump's own request -- nothing else ever uses it */
static NMUPP           gLinkNmUpp        = 0;
static volatile int    gLinkNmPosted     = 0;
static volatile UInt32 gLinkNmArmed      = 0, gLinkNmFired = 0, gLinkNmFailed = 0;
static volatile UInt32 gLinkLastPostTick = 0;
static volatile unsigned long gLinkNmArmTb = 0;     /* timebase at the post, for the latency */

static OSStatus ApLinkTimer(void *p1, void *p2);

static void ApLinkArmTimer(void)
{
    AbsoluteTime when = AddDurationToAbsolute((Duration)AP_LINK_TICK_MS, UpTime());
    if(SetInterruptTimer(&when, ApLinkTimer, NULL, &gLinkTimerId) == noErr) gLinkTimerArmed = 1;
    else gLinkTimerFails++;
}

static void ApLinkPost(void)
{
    if(gLinkNmPosted || !gLinkNmUpp || gLinkStop) return;   /* one request in flight, never two */
    gLinkNm.qType   = nmType;
    gLinkNm.nmMark  = 0;
    gLinkNm.nmIcon  = 0;
    gLinkNm.nmSound = 0;
    gLinkNm.nmStr   = 0;                               /* ★ displays NOTHING: this is the trampoline */
    gLinkNm.nmResp  = gLinkNmUpp;
    gLinkNm.nmRefCon = 0;
    gLinkLastPostTick = gLinkTick;
    gLinkNmArmTb = ApTbNow();
    if(NMInstall(&gLinkNm) == noErr){ gLinkNmPosted = 1; gLinkNmArmed++; }
    else gLinkNmFailed++;
}

/* Is there task-level work? A HINT: the task body re-decides everything with ApLinkStep. */
static int ApLinkWanted(void)
{
    if(gLinkEvtW != gLinkEvtR) return 1;                         /* a kick or a rekey to act on / print */
    if(gApStat && gApStat->cmdSeq != gApStat->ackSeq) return 1;  /* k207: a command from the UI */
    if(gLinkUpFlag && gIrqArmed && ApLinkSilent(gLinkTick, gLinkLastBcnTick, gLinkBcnSeen)) return 1;
    return (UInt32)(gLinkTick - gLinkLastPostTick) >=
           ((gLinkUpFlag || !gRadioPowerOn) ? AP_LINK_KEEPALIVE_UP_TICKS   /* off: nothing to retry */
                                            : AP_LINK_KEEPALIVE_DOWN_TICKS);
}

static OSStatus ApLinkTimer(void *p1, void *p2)
{
#pragma unused (p1, p2)
    gLinkTimerArmed = 0;
    if(gLinkStop) return noErr;          /* EHCI lc1: FIRST, and no re-arm -- this is what makes it stop */
    gLinkTick++;
    gLinkTimerRuns++;
    if(gApStat){                         /* k205: single words -- the UI's "is the driver alive" + the rate */
        UInt32 r = gTxRateIdx;
        gApStat->heartbeat  = gLinkTick;
        gApStat->txRateKbps = (r < AP_TX_NRATES) ? (UInt32)kTxRates[r].mbps * 1000UL : 0;
    }
    if(ApLinkWanted()) ApLinkPost();
    ApLinkArmTimer();
    return noErr;
}

/* ⚠⚠ THE WINDOW CHECK, AND IT IS THE DIFFERENCE BETWEEN A COUNTER AND A HANG.
 *
 * BAR0 on this card is a SLIDING WINDOW selected through PCI config space: SsbSelectCore writes
 * SSB_BAR0_WIN and every entry point in this driver saves and restores it. An interrupt handler
 * cannot do that -- config-space access is not safe at hardware interrupt level -- so the ISR
 * has to assume the window is already on the 802.11 core.
 *
 * "Assume" is exactly the word this project keeps getting hurt by. So instead of assuming, read
 * the chip ID through the window and check it. If the window has moved, we are about to read
 * some other core's registers and interpret them as interrupt state: the ISR returns
 * "not mine", counts it, and touches nothing. Missed interrupts are recoverable. Writing
 * GEN_IRQ_MASK into a stranger's register file is not.
 */
static int ApShimWindowIsOurs(void)
{
    /* ⚠ IDHIGH_CC, NOT a hand-rolled shift. ssb_core.h:9 records that "IDHIGH_CC was written as
     * ((v) >> 4) & 0xFFF in every probe" and that the real mask is 0x00008FF0 -- bits 4-11 plus
     * bit 15. Re-deriving it here would reintroduce a bug the header exists to document. */
    return IDHIGH_CC(ssb_r32(gBus.bar0, SSB_IDHIGH)) == SSB_DEV_80211;
}

/* ★★★ 9-29 k193: the data rate and its ARF (kTxRates, gTxRateIdx, ApTxRateFeedback) live in ap_rate.h
 * -- pure code shared with the host suite rate_test.c. The drain below feeds ApTxRateFeedback. */

/* ★★★★★★ 8-54 (k149): DRAIN THE TRANSMIT-STATUS FIFO, QUIETLY. THE FIX.
 *
 * k148 proved the whole data path stalls because nothing empties XMITSTAT. Apple's model has the
 * APP drain it (AirPortShimTxEthernet); this is an EXTENSION with no app, so after join the FIFO
 * fills and a full status FIFO stalls the transmit engine -- the DHCP REQUEST, the ARP reply and
 * every later unicast frame never complete (uni-TX ACKed 6/6 the moment k148's diagnostic drained
 * it, 0 replies for the frames it did not). This pops every pending entry so the FIFO stays empty
 * and transmit runs on its own. The ISR calls it every fire, so it keeps up asynchronously with no
 * per-frame spin.
 *
 * ⚠ MMIO reads only, a bounded loop, no Say / no File Manager / no allocation -- safe at
 *   secondary-interrupt level. Reading XMITSTAT_0 then XMITSTAT_1 pops one entry, exactly as
 *   DrainTxStatus does; bit 0 is "entry valid", bit 1 is "the AP ACKed it". The 64 cap cannot spin
 *   the handler even if the microcode wedged the valid bit high. */
static void ApShimDrainTxStatQuiet(void)
{
    int guard;
    for(guard = 0; guard < 64; guard++){
        UInt32 v0 = ssb_r32(gBus.bar0, B43_MMIO_XMITSTAT_0);
        if(!(v0 & 0x00000001UL)) break;                    /* nothing pending */
        (void)ssb_r32(gBus.bar0, B43_MMIO_XMITSTAT_1);     /* read _1 to pop the entry */
        gTxStatIsrDrained++;
        if(v0 & 0x00000002UL) gTxStatIsrAcked++;
        ApTxRateFeedback(v0);                              /* k193: per-rate stats + the ARF vote */
    }
    if((UInt32)guard > gTxStatFireMax) gTxStatFireMax = (UInt32)guard;   /* k195: entries this drain popped */
}

/* ★ THE SECONDARY. Runs at secondary interrupt level: no File Manager, no allocation, no
 * Name Registry, no config space. Computation and MMIO only -- which is all the pump needs. */
static OSStatus ApShimSecondaryHandler(void *p1, void *p2)
{
#pragma unused (p1, p2)
    UInt32 delivered = 0;
    unsigned long tbSec = ApTbNow();           /* k199: the whole run, sampled at the bottom */
    gIrqSecondary++;

    /* ★★★ k195: DRAIN THE RING, BOUNDED BY IT. Through k194 this was a budget of 4 -- "at 11
     * frames/sec four is already more than one interrupt's worth" -- written when the link carried
     * beacons. With TCP on it a window of segments lands in one burst: frames 5..n waited in the ring
     * for the NEXT frame's interrupt (at the end of a burst, often a beacon ~100 ms later), and until
     * k195 the per-frame TX reset's RxRecycle discarded them outright if we transmitted first.
     * b43_dma_rx walks to the hardware pointer on every interrupt. One ring's worth bounds the loop,
     * and a frame that lands during it re-raises the latched reason the moment the mask is restored
     * below, so nothing is stranded.
     * ⚠ k199: this note used to say "~70 us of AES-CCM per full frame, ~2 ms worst case". That was
     *   an estimate for OPTIMISED code, never measured -- and this driver was built at -O0, where the
     *   byte-wise AES looks more like milliseconds per frame, so a full ring could hold interrupt
     *   level for tens of ms. PER-FRAME COST in the snapshot (gTbSecRun, gTbRxDecrypt) now measures
     *   both instead of asserting them. */
    if(gIrqArmed && ApShimWindowIsOurs()){
      gIrqInSecondary = 1;
      delivered = ApShimPumpLocked(AP_RX_ISR_BUDGET);
      gRxFireHist[delivered == 0 ? 0 : delivered == 1 ? 1 : delivered <= 4 ? 2 :
                  delivered <= 8 ? 3 : delivered <= 16 ? 4 : 5]++;
      if(delivered > gRxFireMax) gRxFireMax = delivered;
      /* 8-59: in native mode the pump only ENQUEUED frames; post the deferred task ONCE per fire
       * to drain them up the DLPI stream at a safe level (OTScheduleDeferredTask is interrupt-safe). */
      if(gOtmNativeMode && delivered) ApOtmScheduleRx();
      ApShimDrainTxStatQuiet();     /* 8-54 (k149): keep XMITSTAT empty so unicast transmit completes */
      ApGkService();                /* k208: a queued group message 1 answered, a queued message 2 sent --
                                     * here, after the pump, where the stack is shallow */
      /* ★ k202: a stream is holding frames the ring refused. Any interrupt -- a TX completion freeing a
       * slot, or at worst a beacon -- posts the deferred task, which qenables the waiting write queues.
       * (OTScheduleDeferredTask is interrupt-safe and coalesces if it is already posted.) */
      /* k204: not while the link is down -- every attempt would be refused, and the wakeup would spin the
       * write queues once per beacon for nothing. The rejoin posts the wakeup itself when it succeeds. */
      {   extern volatile UInt32 gOtmTxWaiting;
          if(gOtmNativeMode && gOtmTxWaiting && !gLinkTxBlocked) ApOtmScheduleRx(); }
      /* ★ k204: the heartbeat's second source. Secondaries are serialised, so if it reads "not armed" here
       * its own re-arm genuinely failed; a dead heartbeat would silently stop all link recovery. */
      if(gLinkRunning && !gLinkStop && !gLinkTimerArmed){ gLinkTimerRevived++; ApLinkArmTimer(); }
      ApTsMaybeSample(delivered);   /* k196: the receive timeline, every 0.5 s */
      gIrqInSecondary = 0;
      gIrqSecFrames += delivered;

      /* ⛔ THE STORM BRAKE. Insurance, not a design element -- with the null-isr call guarded
       * this should never fire, and that is exactly why it is cheap to carry.
       *
       * If we keep being interrupted while there is still no notify proc to deliver into, the
       * assumption that Start is coming is wrong and re-unmasking forever would hang the
       * machine. Stop instead: leave the device masked and count it. The run then reports a
       * silent receiver, which says everything; a hang says nothing.
       *
       * ⚠ AND IT DOES NOT LATCH. The idle count resets the moment a notify proc exists, so a
       *   slow Start costs nothing. A guard that cannot release is the prime suspect for its
       *   own symptom -- this project's own rule. */
      /* ⚠ 8-58: native mode delivers via ApOtmRxInject, not gShimIsr, so it counts as "delivering"
       * too -- otherwise the brake would fire after 3000 fires on a working native link. */
      if(gShimIsr || gOtmNativeMode) gIrqIdleRuns = 0; else gIrqIdleRuns++;
      if(gIrqIdleRuns > 3000UL){
        gIrqBraked = 1;                        /* deliberately NOT re-unmasked */
      } else {
        /* Re-enable the device's interrupt output. The primary set it to 0 to stop a storm
         * while this ran; b43's thread handler does the same at the same point. */
        ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, gIrqMaskWanted); }
    } else {
      gIrqWindowBad++;
      /* Do NOT re-unmask into a window we do not own. Staying silent is the safe failure. */
    }
    ApTbHistAdd(&gTbSecRun, ApTbNow() - tbSec);
    return noErr;
}

/* ★ THE PRIMARY. Hardware interrupt level. Resident, non-faulting, no allocation, and it must
 * not call CallSecondaryInterruptHandler2 -- QueueSecondaryInterruptHandler is the legal one. */
static InterruptMemberNumber ApShimIsr(InterruptSetMember m, void *refCon, UInt32 cnt)
{
#pragma unused (refCon, cnt)
    UInt32 reason, dma0;

    if(!gIrqArmed) return kIsrIsNotComplete;
    if(!ApShimWindowIsOurs()){ gIrqWindowBad++; return kIsrIsNotComplete; }

    reason = ssb_r32(gBus.bar0, B43_MMIO_GEN_IRQ_REASON);
    /* 0xFFFFFFFF on a shared line means the card is not answering -- it is not ours. b43 checks
     * this first for the same reason (main.c, b43_do_interrupt). */
    if(reason == 0xFFFFFFFFUL){ gIrqNotOurs++; return kIsrIsNotComplete; }
    reason &= gIrqMaskWanted;
    if(!reason){ gIrqNotOurs++; return kIsrIsNotComplete; }

    gIrqHard++;
    dma0 = ssb_r32(gBus.bar0, B43_MMIO_DMA0_REASON) & B43_DMA0_REASON_MASK;
    gIrqLastReason = reason; gIrqLastDma0 = dma0;
    if(dma0 & B43_DMAIRQ_RX_DONE) gIrqRxDone++;

    /* ACK, in b43's order: the general reason first, then the DMA reason. Both are RW1C. */
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_REASON, reason);
    ssb_w32(gBus.bar0, B43_MMIO_DMA0_REASON,    dma0);
    /* Silence the device until the secondary has drained. Without this a level-triggered line
     * re-asserts the instant we return and the machine livelocks in the ISR. */
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, 0);

    if(QueueSecondaryInterruptHandler(ApShimSecondaryHandler, NULL, NULL, NULL) == noErr)
      gIrqQueued++;
    else
      gIrqQueueFail++;     /* ⚠ counted, not ignored: the mask stays 0 and RX stalls if this
                            * ever happens, which is exactly the symptom to recognise. */
    (void)m;
    return kIsrIsComplete;
}

/* ★★★ ARM. Task level only -- the app calls it, deliberately, and nothing else does. */
/* ★★★ INSTALL -- HALF THE ARM, AND THE HALF THAT CANNOT STORM.
 *
 * ⛔⛔ THE SPLIT EXISTS BECAUSE k92 GREY-SCREENED THE MACHINE. Arming from inside EnetHAL_Open
 *   hung it; k93 removed that one variable and ran clean, so the arm is the culprit and the
 *   self-join is not. What the arm does is two separable things, and only ONE of them can make
 *   the card raise an interrupt:
 *
 *     INSTALL   read driver-ist, save the parent's functions, InstallInterruptFunctions
 *               -- puts our handler on the interrupt set member. The DEVICE stays silent.
 *     ENABLE    clear the reason registers, write GEN_IRQ_MASK, call the inherited enabler
 *               -- this is what lets the card assert the line.
 *
 *   With only INSTALL done, GEN_IRQ_MASK is still 0 and our device physically cannot assert
 *   its interrupt output. An interrupt storm from this card is impossible BY CONSTRUCTION, not
 *   by argument. That is the whole point: it tests whether installing a handler inside Open is
 *   survivable, with the dangerous half still switched off.
 *
 * ⚠ gIrqArmed stays 0, so ApShimIsr declines every interrupt it is offered and returns
 *   kIsrIsNotComplete -- which is the documented way to let shared-line siblings be polled. */
static int ApShimIrqInstall(void)
{
    ISTProperty ist;
    UInt32 size = (UInt32)sizeof(ist);
    OSStatus err;

    if(gIrqInstalled) return 1;
    if(!gShimMacEverOn){
      Say("  [8-7] INSTALL REFUSED -- the MAC has never been enabled."); return 0; }

    Say("");
    Say("  [8-7] ★ INSTALLING THE INTERRUPT HANDLER (device output stays MASKED)");

    /* ⚠ The window is left on the 802.11 core from here on and is NOT restored: an ISR cannot
     * touch config space, so the window has to be ours whenever an interrupt can land.
     * ApShimWindowIsOurs() verifies it on every entry rather than trusting this comment. */
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      Say("    [!!] could not select the 802.11 core. NOT installing."); return 0; }

    err = RegistryPropertyGet(&gBus.node, kISTPropertyName, (void*)ist, &size);
    if(err != noErr || size < sizeof(InterruptSetMember)){
      Say1("    [!!] no \"driver-ist\" property on our node. err = ",(unsigned long)err);
      return 0; }
    gIstSet    = ist[kISTChipInterruptSource].setID;
    gIstMember = ist[kISTChipInterruptSource].member;
    Say1("    driver-ist setID  = ",(unsigned long)gIstSet);
    Say1("    driver-ist member = ",(unsigned long)gIstMember);

    /* SAVE the parent's functions. A shared line must chain, and Close must put these back. */
    err = GetInterruptFunctions(gIstSet,gIstMember,&gParentRefCon,&gParentHandler,
                                &gParentEnabler,&gParentDisabler);
    if(err != noErr){
      Say1("    [!!] GetInterruptFunctions failed, err = ",(unsigned long)err); return 0; }

    err = InstallInterruptFunctions(gIstSet,gIstMember,NULL,ApShimIsr,NULL,NULL);
    if(err != noErr){
      Say1("    [!!] InstallInterruptFunctions failed, err = ",(unsigned long)err); return 0; }
    gIrqInstalled = 1;
    Say("    [ok] handler installed. GEN_IRQ_MASK is still 0 -- the card cannot interrupt yet.");
    return 1;
}

/* ★★★ ENABLE -- the other half, and the one that lets the card raise a line. */
static int ApShimIrqEnable(void)
{
    if(!gIrqInstalled) return 0;
    if(gIrqArmed) return 1;
    if(!ApShimWindowIsOurs()){
      Say("  [8-7] ENABLE REFUSED -- the BAR0 window is not on the 802.11 core."); return 0; }

    /* Clear anything stale, then choose what we want to hear about: DMA only. TX-done and the
     * PHY/MAC error interrupts are deliberately NOT enabled -- nothing handles them yet and an
     * unhandled level interrupt is a livelock, not a missed feature. */
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_REASON, 0xFFFFFFFFUL);
    ssb_w32(gBus.bar0, B43_MMIO_DMA0_REASON,    0xFFFFFFFFUL);
    gIrqMaskWanted = B43_IRQ_DMA;
    gIrqArmed = 1;
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, gIrqMaskWanted);

    /* Install does not enable. Call the enabler we inherited, once. */
    if(gParentEnabler){
      InterruptSetMember sm; sm.setID = gIstSet; sm.member = gIstMember;
      (void)gParentEnabler(sm,gParentRefCon);
      Say("    inherited enabler called"); }
    else Say("    (no parent enabler -- the slot is presumably already enabled)");

    SayH("    GEN_IRQ_MASK <- ",(unsigned long)gIrqMaskWanted,8);
    Say("    [ok] ★★★ ARMED. From here the card can interrupt this machine.");

    /* ★ k197: keep FCS-failed frames for the receive-quality counters -- b43_adjust_opmode's
     * FIF_FCSFAIL arm, a supported MACCTL bit b43 sets at runtime for monitor mode. Set HERE, after the
     * join, and RxWaitFor steps past every such frame anyway, so nothing downstream ever sees one. The
     * MAC does not ACK a bad frame whatever this bit says: nothing changes on the air. */
#if AP_RX_KEEP_BAD
    {   UInt32 mc = ssb_r32(gBus.bar0, B43_MMIO_MACCTL);
        ssb_w32(gBus.bar0, B43_MMIO_MACCTL, mc | B43_MACCTL_KEEP_BAD);
        gRxKeepBadOn = ((ssb_r32(gBus.bar0, B43_MMIO_MACCTL) & B43_MACCTL_KEEP_BAD) != 0);
        Say1("    MACCTL KEEP_BAD (receive-quality counters; 1 = on) = ", (unsigned long)gRxKeepBadOn); }
#else
    /* k199: left clear (b43's normal mode). Read back rather than assumed, so the log says what the MAC has. */
    gRxKeepBadOn = ((ssb_r32(gBus.bar0, B43_MMIO_MACCTL) & B43_MACCTL_KEEP_BAD) != 0);
    Say1("    MACCTL KEEP_BAD (k199: left clear; 0 expected) = ", (unsigned long)gRxKeepBadOn);
#endif
    return 1;
}

/* ★★★ ARM = INSTALL + ENABLE. Task level only. Unchanged for every existing caller. */
UInt32 AirPortShimArmIrq(void)
{
    if(gIrqArmed) return 1;
    if(!ApShimIrqInstall()) return 0;
    return (UInt32)ApShimIrqEnable();
}

/* ★ QUIESCE -- SAFE AT ANY EXECUTION LEVEL, and this split was forced by the audit.
 *
 * ⚠⚠ check-exec-level.py flagged EnetHAL_Close reaching InstallInterruptFunctions, which Apple's
 *   PCI book documents as TASK LEVEL ONLY. Close is not documented as task time -- only Open is
 *   -- so calling it from there was a latent defect of exactly the kind this audit exists for.
 *   In practice Close arrives from OTCloseProvider at task level, and it would probably never
 *   have bitten. "Probably never" is what the other three hangs were.
 *
 * So the work splits by what each part actually needs:
 *
 *   quiesce (here)   clear the flag, mask the device output.   Pure memory + MMIO. Any level.
 *                    After this the ISR returns immediately and the card raises nothing --
 *                    the system is SAFE, which is the part that cannot wait.
 *   restore (below)  put the parent's interrupt functions back. Task level only, so it is done
 *                    from the app's explicit disarm.
 *
 * ⚠ Leaving our handler installed but disarmed is not a leak: it no-ops and returns
 *   kIsrIsNotComplete, so a shared line still reaches its other owners. The restore matters
 *   before the fragment is UNLOADED, not before the card is closed. */
static void ApShimIrqQuiesce(void)
{
    gIrqArmed = 0;                       /* the ISR bails immediately from here */
    if(gIrqInstalled && ApShimWindowIsOurs())
      ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, 0);
}

/* ★ FULL DISARM. TASK LEVEL ONLY -- the app calls it. Puts the parent's functions back, because
 * a line left pointing at a fragment that has been unloaded is a crash on the next interrupt. */
UInt32 AirPortShimDisarmIrq(void)
{
    ApShimIrqQuiesce();
    if(!gIrqInstalled) return 1;
    (void)InstallInterruptFunctions(gIstSet,gIstMember,gParentRefCon,
                                    gParentHandler,gParentEnabler,gParentDisabler);
    gIrqInstalled = 0;
    return 1;
}

/* ★ k204: RESUME -- undo ApShimIrqQuiesce after a rejoin. The rejoin quiesces because the join polls the
 * receive ring itself (the boot join runs before the first arm for the same reason). The handler is still
 * installed and the parent's enabler was never undone, so only the device side is redone: clear stale
 * reasons, then the flag, then the mask -- ApShimIrqEnable's order, without its install-time extras.
 * Frames that landed meanwhile are walked on the next interrupt; beacons guarantee one within ~100 ms.
 * MMIO + flags, any level; the rejoin calls it at task level. */
static UInt32 gIrqResumes = 0, gIrqResumeRefused = 0;
static void ApShimIrqResume(void)
{
    if(!gIrqInstalled || gIrqArmed) return;
    if(!ApShimWindowIsOurs()){ gIrqResumeRefused++; return; }
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_REASON, 0xFFFFFFFFUL);
    ssb_w32(gBus.bar0, B43_MMIO_DMA0_REASON,    0xFFFFFFFFUL);
    gIrqArmed = 1;
    ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, gIrqMaskWanted);
    gIrqResumes++;
}

UInt32 AirPortShimGetIrq(UInt32 *hard, UInt32 *notOurs, UInt32 *queued, UInt32 *queueFail,
                         UInt32 *secondary, UInt32 *secFrames, UInt32 *windowBad,
                         UInt32 *rxDone, UInt32 *lastReason, UInt32 *lastDma0)
{
    if(hard)       *hard       = gIrqHard;
    if(notOurs)    *notOurs    = gIrqNotOurs;
    if(queued)     *queued     = gIrqQueued;
    if(queueFail)  *queueFail  = gIrqQueueFail;
    if(secondary)  *secondary  = gIrqSecondary;
    if(secFrames)  *secFrames  = gIrqSecFrames;
    if(windowBad)  *windowBad  = gIrqWindowBad;
    if(rxDone)     *rxDone     = gIrqRxDone;
    /* ⚠ windowBad now carries the BRAKE too: a non-zero value with zero window faults means
     * the storm brake fired, which is a different story and the app prints both. */
    if(lastReason) *lastReason = gIrqLastReason;
    if(lastDma0)   *lastDma0   = gIrqLastDma0;
    return (UInt32)(gIrqArmed ? 2 : (gIrqInstalled ? 1 : 0));
}

/* ============================================================================================
 * ★★★★★★ STAGE 8-8 — EnetHAL_Write. Open Transport transmits through this driver.
 *
 * The last piece of the data path. An Ethernet frame arrives from OT; it leaves as an
 * 802.11 ToDS data frame, CCMP-encrypted under the pairwise TK, on the air.
 *
 * ★ EVERY PART OF THIS ALREADY EXISTED AND WAS ALREADY PROVEN. Nothing here is new
 *   cryptography or new framing:
 *     BuildDataHeaderToDs   ap_eapol.h   -- built the header for Stage 8-1's encrypted ARP
 *     ApCcmpEncap           ap_ccmp.h    -- checked against RFC 3610 vectors in kdf_test.c
 *     GenerateTxHdr351 /
 *     ApShimTxArm / PostTxFrameAt        -- the path every management frame since 8-3a used
 *   What is new is the join: Ethernet in, and a PN that advances.
 *
 * ⚠⚠ EXECUTION LEVEL. Write may be called below task level -- OT decides, not us. So, exactly
 *   like EnetHAL_Read and the ISR: no allocation, no File Manager, no Name Registry, and NO PCI
 *   CONFIG SPACE. It cannot re-point the BAR0 window, so it verifies the window and refuses if
 *   it has moved. check-exec-level.py audits this selector.
 *
 * ⚠ AND IT REFUSES RATHER THAN LYING. A Write that returns noErr having transmitted nothing
 *   tells OT the frame is gone. Every refusal below returns an error and increments a counter
 *   that says which one, because "the link is up and every packet vanishes" is the single most
 *   expensive failure mode this interface can have.
 */
/* ⚠⚠ THE ENCAP OUTPUT GOES INTO A RING BUFFER (k195: gTxrFrm[], was gShimTxFrm), NOT INTO A
 * STATIC OF OUR OWN. Each is half of a locked page out of gTxrPool with a physical address the
 * card was given (PhysOfPage + InWindow, checked at Open). A plain static has no physical mapping,
 * is not locked, and may not even be in the DMA window -- handing its address to the engine would
 * DMA from whatever happens to live at that physical address. AP_TXR_FRMMAX is the size bound.
 * gTxEthBody is plaintext and never reaches the card, so it can be an ordinary static. */
static UInt8  gTxEthBody[AP_ENET_MAXFRAME + 16];
static UInt8  gTxEthHdr[32];
static UInt32 gTxCalls = 0, gTxSent = 0, gTxAcked = 0;
static UInt32 gTxNoKey = 0, gTxNotAssoc = 0, gTxBadFrame = 0, gTxWindowBad = 0, gTxPnWrap = 0;
static UInt32 gTxOpenFrames = 0;    /* k217: open-network plaintext data frames sent (no CCMP) */
static UInt32 gTxDhcpBcast = 0;   /* 8-42: times we forced the DHCP broadcast flag on a request */
static UInt32 gTxDhcpPolled = 0;  /* 8-43: times we polled the ring after a DHCP send */
static UInt32 gTxPollGot = 0;     /* 8-43: frames that post-send poll delivered (the reply, we hope) */
static UInt32 gTxNoStatus = 0;   /* posted, but no TX status with OUR cookie ever came back */
static UInt32 gTxLastLen = 0;
/* 8-47 (k142): what OT actually TRANSMITS as DHCP, decoded from our own outgoing body. gDhcpType
 * is RX-only; these answer "what is OT asking for, that the server NAKs". */
static UInt32 gTxDhcpType[9];                    /* outgoing DHCP by option 53 (0 = no opt 53) */
static UInt32 gTxReqIpSet = 0, gTxSrvIdSet = 0;  /* how many requests carried opt 50 / opt 54 */
static UInt32 gTxLastReqIp = 0, gTxLastSrvId = 0, gTxLastCiaddr = 0;
static UInt8  gTxDhcpCap[2][96];                 /* first two outgoing DHCP requests, verbatim */
static UInt32 gTxDhcpCapLen[2], gTxDhcpCapFull[2], gTxDhcpCapN = 0;
/* 8-57 (k154): what OT TRANSMITS as ICMP. Does it answer the Pi's ping (echo reply, type 0), and
 * to which dst MAC -- the Pi (correct) or elsewhere? Distinguishes "OT won't reply" from "the AP
 * won't bridge OT's reply". */
static UInt32 gTxIcmpReply = 0, gTxIcmpReq = 0, gTxIcmpOther = 0;
static UInt32 gTxIcmpLastDstIp = 0;
static UInt8  gTxIcmpLastDstMac[6] = {0,0,0,0,0,0};
static int    gTxBusy = 0;
static UInt32 gTxBusyRefused = 0;   /* k196: frames refused because a call was already inside (re-entry) */
static UInt32 gTxLinkDown = 0;      /* k204: frames held ("try later") because the link was down */

/* ★ THE CORE. Returns noErr when the frame is on the air, an error otherwise -- never noErr
 * for a frame that was not transmitted. */
/* k202: "not now" -- the ring is full or a transmit is already in progress. The caller KEEPS the frame
 * (ap_otmodl.c's write queue) instead of dropping it; every other failure is permanent and drops. */
#define kApTxTryLater ((OSStatus)1)

/* ★★ k208: THE CORE -- checks, encrypt, post -- and the transmitter's owner is its CALLER. Two wrappers
 * below take gTxBusy around it: ApShimWriteFrame (OT, and the shim era's callers) and ApShimTxFromPump
 * (the receive pump's group message 2). They differ in one thing only: the shim era's after-send probes
 * (ApShimTxShimProbes), which spin and write the log ring, and so may never be reachable from the pump --
 * check-exec-level.py holds the secondary handler to that. The core never touches gTxBusy. */
static OSStatus ApShimTxCore(const UInt8 *eth, UInt32 ethLen, int *isDhcpOut, const UInt8 **daOut)
{
    UInt32 bodyLen = 0, encLen;
    UInt32 txPos, txBuf;               /* k195: this frame's ring position and buffer */
    int    txIsDhcp = 0;               /* 8-42/8-43: this frame is a BOOTP client request */
    const UInt8 *da = NULL;
    UInt16 hdrLen;
    int why = AP_ENET_OK;
    unsigned long tbEntry = ApTbNow(), tbEnc;   /* k199: stopwatch; sampled only for a posted frame */

    /* ★ k204: the link is down (or a rejoin is running and owns the transmitter). KEEP the frame: it waits
     * on OT's write queue and goes out after the rejoin, which posts the wakeup. Through k203 a rejoin could
     * not happen, and JoinReset's assoc state would have made this an ioErr -- a silent drop. */
    if(gLinkTxBlocked){ gTxLinkDown++; return kApTxTryLater; }
    if(gShimAssocErr != noErr){ gTxNotAssoc++; return ioErr; }
    if(!gPtkReady && gTargetSec != kApSecOpen){ gTxNoKey++; return ioErr; }  /* k217: open is keyless */
    if(!ApShimWindowIsOurs()) { gTxWindowBad++; return ioErr; }
    if(!ApEnetToBody(eth,ethLen,gTxEthBody,&bodyLen,&da,&why)){ gTxBadFrame++; return paramErr; }

    /* ★★★ 8-42: force the DHCP broadcast flag so the server BROADCASTS its OFFER/ACK. k136 proved
     * OT drops the UNICAST offer (wrong dst IP for an unconfigured interface) but acts on broadcast
     * DHCP -- see ApTxForceDhcpBroadcast. Operates on our own gTxEthBody, before encryption; a
     * no-op for everything that is not a BOOTP client request. The result also tells 8-43 below
     * whether to poll for the reply. */
    /* ★★★ 8-47 (k142): decode what OT is actually asking for, BEFORE ApTxForceDhcpBroadcast touches
     * the broadcast flag, so bcastFlag records OT's own choice. Read-only and bounded; a no-op for
     * anything that is not a BOOTP client request. See ApTxDhcpInspect -- this is the measurement
     * that turns "the server NAKs us" into "the server NAKs us because OT requests X". */
    { ApTxDhcpInfo tx;
      if(ApTxDhcpInspect(gTxEthBody, bodyLen, &tx)){
        UInt32 mt = (tx.msgType >= 0 && tx.msgType <= 8) ? (UInt32)tx.msgType : 0u;
        gTxDhcpType[mt]++;
        gTxLastCiaddr = tx.ciaddr;
        if(tx.hasReqIp)   { gTxReqIpSet++; gTxLastReqIp  = tx.requestedIp; }
        if(tx.hasServerId){ gTxSrvIdSet++; gTxLastSrvId = tx.serverId;   }
        if(gTxDhcpCapN < 2u){
          UInt32 k, lim = (bodyLen < 96u) ? bodyLen : 96u;
          for(k = 0; k < lim; k++) gTxDhcpCap[gTxDhcpCapN][k] = gTxEthBody[k];
          gTxDhcpCapLen[gTxDhcpCapN]  = lim;
          gTxDhcpCapFull[gTxDhcpCapN] = bodyLen;
          gTxDhcpCapN++; }
        txIsDhcp = 1;    /* 8-51 (k146): the post-TX poll gates on the DHCP detection itself now */
      } }

    /* 8-52 (k147): decode OT's OUTGOING ARP. Its sender-IP is OT's ACTUAL address -- .225 means the
     * static config applied, 169.254.x means OT fell to link-local. The target shows what OT is
     * resolving (the router .1, the Pi .207, or a probe). This settles "is OT even on .225". */
    if(bodyLen >= 36u && gTxEthBody[6] == 0x08 && gTxEthBody[7] == 0x06){
      const UInt8 *a = gTxEthBody + 8;    /* the ARP, after the SNAP header */
      UInt16 aop = ((UInt16)a[6] << 8) | a[7];
      UInt32 spa = ((UInt32)a[14]<<24)|((UInt32)a[15]<<16)|((UInt32)a[16]<<8)|a[17];
      UInt32 tpa = ((UInt32)a[24]<<24)|((UInt32)a[25]<<16)|((UInt32)a[26]<<8)|a[27];
      gTxArpN++; gTxArpLastOp = aop; gTxArpLastSpa = spa; gTxArpLastTpa = tpa;
    }

    /* ★★★ 8-57 (k154): decode OUR outgoing ICMP. When the Pi pings static .225, does OT emit an
     * echo REPLY (type 0), and to what dst? The wire shows OT's ARP reply crossing but its ICMP
     * replies not -- this says whether OT even generates the reply and whether da is the Pi. */
    { ApTxIcmpInfo ic;
      if(ApTxIcmpInspect(gTxEthBody, bodyLen, &ic) && ic.isIcmp){
        if(ic.icmpType == 0)      gTxIcmpReply++;
        else if(ic.icmpType == 8) gTxIcmpReq++;
        else                      gTxIcmpOther++;
        gTxIcmpLastDstIp = ic.dstIp;
        if(da){ int k; for(k = 0; k < 6; k++) gTxIcmpLastDstMac[k] = da[k]; }
      } }

    /* ★★★★★★ 8-53 (k150): RESTORE forcing the DHCP broadcast flag -- the k136 fix, retired by k146
     * on evidence the pre-k149 TX-status-FIFO stall had corrupted, now reinstated on top of the k149
     * drain. THE k149 DHCP BOOT REPRODUCED k136 EXACTLY: the snapshot showed OFFER = 14, IP+UDP
     * checksum ok 14/14, yiaddr = c0a801e1 (.225), DHCP bcast set = 0, and TX REQUEST = 0 -> a
     * link-local fallback. Reception is flawless; OT drops every offer because a UNICAST datagram to
     * .225 is addressed to no IP the interface holds yet, and OT's IP input discards it before the
     * bootp client ever sees it. Forcing the flag makes the server BROADCAST the offer and the ack
     * onto the one path k136 proved OT accepts. This is the TX half of a matched pair:
     * ApRxClearDhcpBroadcast (8-50, still live at the hand-up) resets the echoed 0x8000 the server
     * returns, so OT sees a flags field matching the 0x0000 it believes it sent. gTxDhcpBcast is the
     * witness that we are forcing again.
     *
     * ⚠ k146's own restore condition was "if RX OFFER -> 0"; that was the wrong witness. The offers
     *   never stopped arriving -- OT stopped ACTING on them. The correct witness is OFFER > 0 with
     *   REQUEST == 0, which is precisely what the k149 boot showed. Do not retire this again without
     *   a boot that shows OT REQUESTing off a UNICAST offer -- which k136 and k149 both refute. */
    if(ApTxForceDhcpBroadcast(gTxEthBody, bodyLen)) gTxDhcpBcast++;

    /* ★★★ k195: A RING POSITION FIRST -- see ap_txring.h for why the one-buffer post had to go. The
     * position's buffer is free by construction (the engine has finished every frame that used it),
     * so encrypting straight into it cannot touch a frame still on its way to the card. Checked
     * before the PN advances, so a refused frame does not spend one. */
    if(!ApShimTxrReady()) return kApTxTryLater;   /* k202: ring full -- the caller keeps it */
    txPos = gTxr.head; txBuf = ApTxrBuf(txPos);

    /* ⛔ ADVANCE THE PN BEFORE EVERY FRAME, AND STOP DEAD IF IT WRAPS. Reusing a PN under one
     * key is not a degraded mode, it is the end of CCMP's guarantees for this session. There is
     * no recovery here short of a new PTK, so the transmitter refuses rather than continuing. */
    if(gTargetSec != kApSecOpen && !ApCcmpPnIncrement(gTxPn)){ gTxPnWrap++; return ioErr; }  /* k217: PN is CCMP-only */

    hdrLen = BuildDataHeaderToDs(gTxEthHdr,gShimMac,gShimTgtBssid,da);
    /* hdr(24) + CCMP hdr(8) + body + MIC(8) must fit the ring buffer's frame area (k195: 1920 bytes,
     * was the whole 4 KB page), and the check is BEFORE the write, not after: ApCcmpEncap has no
     * length limit of its own. */
    /* k217: WPA2 adds a CCMP header(8) + MIC(8); an open frame adds neither. */
    if((gTargetSec == kApSecOpen ? (UInt32)hdrLen + bodyLen
                                 : (UInt32)hdrLen + 8UL + bodyLen + 8UL) > (UInt32)AP_TXR_FRMMAX){
      gTxBadFrame++; return paramErr; }
    /* ★ k200: ENCRYPT INTO CACHED MEMORY, THEN COPY WORDS INTO THE RING. k199 measured the live
     * encrypt at 150 us against 73 us for the same work into a static buffer -- the difference is the
     * ~1500 single-byte stores CCM's CTR loop made straight into the DMA page. gTxStage is ordinary
     * memory; ApCopyWords moves the finished frame in ~380 word stores. The ring buffer is still
     * free by construction (ApShimTxrReady above), so the copy cannot touch a frame in flight. */
    if(gTargetSec == kApSecOpen){
      /* ★ k217 OPEN: plaintext -- copy the (unprotected) 802.11 header and the LLC/SNAP body into the
       * stage buffer. No PN, no CCMP header, no MIC. The common tail below posts gTxStage exactly as it
       * posts a CCMP frame; the RX path (ApRxToEnet) already passes unprotected frames straight through. */
      UInt32 c;
      for(c = 0; c < (UInt32)hdrLen; c++) gTxStage[c] = gTxEthHdr[c];
      for(c = 0; c < bodyLen;        c++) gTxStage[(UInt32)hdrLen + c] = gTxEthBody[c];
      encLen = (UInt32)hdrLen + bodyLen;
      gTxOpenFrames++;
    } else {
      tbEnc = ApTbNow();
      encLen = ApCcmpEncap(gTxEthHdr,(ApU32)hdrLen,gTxEthBody,(ApU32)bodyLen,
                           (const ApU8*)(gPtk+AP_TK_OFF),gTxPn,0,
                           0,0,0,0,(ApU8*)gTxStage);
      if(!encLen){ gTxBadFrame++; return ioErr; }
      ApTbHistAdd(&gTbTxEncap, ApTbNow() - tbEnc);
    }

    /* ⚠ A FRESH TXHDR FOR THIS FRAME. GenerateTxHdr351 writes the PLCP, which carries the
     * length the PHY transmits -- reusing a previous header sends the previous length. k49 lost
     * a run to exactly that. */
    /* ★ k193: the ARF's current rate (read ONCE -- the ISR may step it), carried in the cookie so the
     * status comes back naming it. k203: the fallback is the next rate down (fb = r - 1, floor CCK 1)
     * -- see GenerateTxHdr351Rate; through k202 every retry went out at CCK 1. */
    {   UInt32 r = gTxRateIdx, fb;
        if(r >= AP_TX_NRATES) r = 0;
        fb = r ? r - 1 : 0;
        gTxLastCookie = (UInt16)(AP_TX_COOKIE_DATA + r);
        /* k200: the header is built from the STAGED frame (GenerateTxHdr351Rate only READS it: frame
         * control, duration, addr1) -- the same bytes the ring buffer is about to receive. */
        GenerateTxHdr351Rate(gTxrHdr[txBuf],gTxStage,(UInt16)encLen,gShimChannel,gTxLastCookie,
                             B43_TXH_PHY_ANT01AUTO,1,kTxRates[r].ofdm,kTxRates[r].code,
                             kTxRates[fb].ofdm,kTxRates[fb].code);
        gTxRateSent[r]++;
    }
    {   unsigned long tbCopy = ApTbNow();
        ApCopyWords(gTxrFrm[txBuf], gTxStage, (unsigned long)encLen);   /* k200: staged -> the ring */
        ApTbHistAdd(&gTbTxCopy, ApTbNow() - tbCopy); }
    /* ★★★ k195: POST AT THE RING POSITION -- b43's dma_tx_fragment: the header descriptor
     * (FRAMESTART), the body descriptor (FRAMEEND + IRQ), then op32_poke_tx(next_slot(body)).
     * Through k194 this was ApShimTxArm (a TX-engine reset + RxRecycle) and a post at descriptors
     * 0/1: k79's fix for "posting slot 0 twice writes the same TXINDEX", which a ring answers by
     * never writing the same TXINDEX twice. The engine keeps running; nothing is reset per frame. */
    Op32FillDescriptorFull(gShimTxRing.base,(int)ApTxrHdrDesc(txPos),(int)AP_TXR_DESCS,
                           gTxrHdrPhys[txBuf],(UInt16)TXH_SIZE_351,1,0,0);
    Op32FillDescriptorFull(gShimTxRing.base,(int)ApTxrHdrDesc(txPos) + 1,(int)AP_TXR_DESCS,
                           gTxrFrmPhys[txBuf],(UInt16)encLen,0,1,1);
    ApTxDoorbellBarrier();            /* txhdr, frame and both descriptors visible before the index */
    ssb_w32(gBus.bar0,gShimTxBase+B43_DMA32_TXINDEX,ApTxrTxIndex(txPos));
    ApTxrCommit(&gTxr);
    ApTbHistAdd(&gTbTxTotal, ApTbNow() - tbEntry);   /* k199: OT's call -> the card owns the frame */
    gTxrPosts++;
    if(gTxr.used > gTxrUsedMax) gTxrUsedMax = gTxr.used;
    gTxSent++;
    gTxLastLen = encLen;
    /* The AP's ACK is the oracle and it is not ours to fake -- but draining for it here would
     * spin at whatever level OT called us from. The count is collected by the app instead. */
    if(isDhcpOut) *isDhcpOut = txIsDhcp;
    if(daOut)     *daOut     = da;
    return noErr;
}

/* ★ k208: THE SHIM ERA'S AFTER-SEND PROBES, moved out of the core unchanged. Both are gated on gShimIsr,
 * which only EnetHAL_Start sets -- Apple's EnetShim path -- so neither runs in native (OTModl) mode, the
 * one that ships. They stay for the shim-era callers, and out of the receive pump's reach: the second
 * drains the TX status with DrainTxStatus, which spins and prints through Out(). Run with gTxBusy held,
 * as they always were, by ApShimWriteFrame below. */
static void ApShimTxShimProbes(int txIsDhcp, const UInt8 *da)
{
    /* ★★★★★★ 8-43 (k138): CATCH THE REPLY BY POLLING RIGHT AFTER A DHCP SEND -- the join's idiom.
     *
     * k137 proved the last gap from both sides. With the broadcast flag set, the server broadcasts
     * OFFERs of 192.168.1.225 that show up on the wire -- yet the driver delivered only 3 of 14, and
     * ZERO of the NAKs, while the ISR's own why-histogram showed every one of the 140 frames it saw
     * delivered with no drops. So the missed replies never ENTERED the ISR's view: they land in the
     * ~1 ms window right after we transmit and are gone before the interrupt services the ring. The
     * JOIN never has this problem because it POLLS the ring immediately after each transmit
     * (RxWaitFor, 8-4); this does the same for the DHCP send path. (k135 had this instinct and I was
     * wrong to drop it -- but k135 was gated to task level, so on OT's below-task send path it never
     * ran, and it walked the ring past the armed ISR. Both are fixed here.)
     *
     * ⚠⚠ THE ISR RACE IS CLOSED BY MASKING, NOT BY LUCK. While the interrupt is armed the secondary
     *   handler owns the ring (ap_shim.c:2703 is why the task-level pump bows out). Here we instead
     *   MASK the device's interrupt output (GEN_IRQ_MASK <- 0) for the duration of the poll: on this
     *   single-core machine a masked device cannot assert, so no primary/secondary can fire and the
     *   poll owns the ring alone. Frames that arrive while masked are delivered by the poll itself;
     *   the mask is restored the instant it ends. The mask writes are MMIO, safe from any level, and
     *   guarded by ApShimWindowIsOurs() so we never write GEN_IRQ_MASK into a stranger's core.
     * ⚠ Scoped to DHCP client requests only (ApTxForceDhcpBroadcast returned 1), so a steady data
     *   path never eats a 20 ms masked spin -- only the handful of DHCP sends during association do. */
    if(txIsDhcp && gShimStartSeen && gShimIsr){
        int masked = 0;
        if(gBus.bar0 && ApShimWindowIsOurs()){
            ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, 0);
            (void)ssb_r32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK);   /* flush the mask before we spin */
            masked = 1;
        }
        gTxDhcpPolled++;
        { UInt32 i; for(i = 0; i < 20u; i++){ SsbSpinUs(1000); gTxPollGot += ApShimPumpLocked(8); } }
        if(masked) ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, gIrqMaskWanted);
    }

    /* ★★★ 8-53 (k148): SAMPLE THE TRANSMIT STATUS OF A UNICAST DATA FRAME -- the crux from k147.
     * OT replies to the Pi's ARP but the reply never arrives. Read XMITSTAT (cookie 0xC0E0) for the
     * first few unicast frames (the ARP reply is one) to learn whether the AP ACKed them: ACKed ->
     * the frame reached the AP (chase forwarding/framing); not-acked -> the unicast uplink itself is
     * not getting to the AP. DHCP requests are broadcast (group da), so they take the poll above,
     * not this -- the two never both run for one frame. Masked + window-guarded like the poll. */
    if(da && !ApEnetIsGroup(da) && gTxUniSampled < 6u && gShimStartSeen && gShimIsr){
        int masked = 0;
        if(gBus.bar0 && ApShimWindowIsOurs()){
            ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, 0);
            (void)ssb_r32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK);
            masked = 1;
        }
        { int a = DrainTxStatus(gTxLastCookie, 12);        /* k193: the cookie this frame carried */
          gTxUniSampled++;
          if(a) gTxUniAcked++; else gTxUniNoAck++;
          gTxUniLastAck = (UInt32)a; }
        if(masked) ssb_w32(gBus.bar0, B43_MMIO_GEN_IRQ_MASK, gIrqMaskWanted);
    }
}

/* ⛔ k208: THE TRANSMITTER IS TAKEN BEFORE ANYTHING SHARED IS TOUCHED, and held for the whole call.
 * Through k207 the flag was set halfway down, after ApEnetToBody had already written this frame into
 * gTxEthBody. A call preempted in that window by a call from a higher level -- OT's deferred task over a
 * task-level OT send, or now the receive pump over either -- had its body overwritten, and then encrypted
 * and sent the OTHER frame's body a second time, with a fresh PN: a duplicate on the air and a silent loss
 * of its own frame. Taken right after the test, the window holds nothing shared: a preempting call runs to
 * completion before the preempted level resumes, and one arriving after the take is refused with "try
 * later" and keeps its frame (k202's write queue; the pump's held message 2). */
static OSStatus ApShimWriteFrame(const UInt8 *eth, UInt32 ethLen)
{
    OSStatus e;
    int isDhcp = 0;
    const UInt8 *da = NULL;
    gTxCalls++;
    if(gTxBusy){ gTxBusyRefused++; return kApTxTryLater; }   /* no re-entrancy; k202: kept, not dropped */
    gTxBusy = 1;
    e = ApShimTxCore(eth, ethLen, &isDhcp, &da);
    if(e == noErr) ApShimTxShimProbes(isDhcp, da);
    gTxBusy = 0;
    return e;
}

/* k208: the receive pump's transmit -- the core alone. See ApShimTxCore for why there are two. */
static OSStatus ApShimTxFromPump(const UInt8 *eth, UInt32 ethLen)
{
    OSStatus e;
    gTxCalls++;
    if(gTxBusy){ gTxBusyRefused++; return kApTxTryLater; }
    gTxBusy = 1;
    e = ApShimTxCore(eth, ethLen, 0, 0);
    gTxBusy = 0;
    return e;
}

/* ★★★ k208: A GROUP KEY RENEWAL, ANSWERED FROM THE SECONDARY INTERRUPT HANDLER (ap_gtk.h has the protocol and
 * the why).
 *
 * ⚠ SECONDARY INTERRUPT LEVEL, ON PURPOSE -- not the task body, where COMMUNITY-RELEASE.md puts
 *   "everything radio-side". The AP gives a station about 3.5 s (hostapd: 500 ms, then 1 s per resend, 4
 *   tries) before it disconnects it with reason 16, and the task body runs off the Notification Manager,
 *   which k207's log measured starved for 13.5 s by a Control Strip menu held open. And this is not radio
 *   control: it is data-path work, like the CCMP decrypt the pump already does per frame -- a MIC, an
 *   unwrap, one encrypted frame; well under a millisecond, no allocation, no File Manager, no spin. The
 *   transmit is ApShimTxFromPump: ApShimTxCore, the encrypt-and-post OT's deferred task already runs at
 *   this level through ApShimWriteFrame, without the shim era's probes (they spin and log).
 * ⚠ QUEUED BY THE PUMP, ANSWERED AFTER IT. The receive pump only copies message 1 (ApGkQueueM1); ApGkService,
 *   which the secondary handler calls once the pump has returned, does the MIC, the unwrap, message 2 and the
 *   send. Measured with gcc's -fstack-usage at -O0: doing it all inside the pump took the handler's worst-case
 *   stack from 1248 bytes (k207: the CCMP decrypt) to 1824. Queued, and with the unwrap's scratch static, the
 *   worst case stays the CCMP decrypt's. The driver's own pattern: the ISR queues, the deferred task works.
 * ⚠ MESSAGE 2 GOES OUT ENCRYPTED under the TK: the pairwise key is installed, and wpa_supplicant's
 *   wpa_ether_send encrypts every EAPOL frame once it is (wpas_glue.c). BuildEapolKeyFrame builds it -- the
 *   one EAPOL-Key builder -- in its 802.11 form; the EAPOL part becomes an Ethernet frame to the AP, which
 *   ApShimTxCore encrypts and posts like any data frame.
 * ⚠ "Try later" (the ring is full, or a task-level send holds the transmitter) is not a failure: message 2
 *   stays queued and ApGkService tries again on each interrupt -- a TX completion, or at worst the next
 *   beacon. hostapd accepts a message 2 carrying the counter of ANY of its recent resends, so a late one
 *   still counts.
 * ⚠ This level may not call Say(): every outcome is an event, which the task body logs. */
#define AP_GK_M2_SENT      0     /* AP_LINK_EVT_GTK_M2's sub: what became of a queued message 2 */
#define AP_GK_M2_FAILED    1
#define AP_GK_M2_ABANDONED 2     /*   held past the AP's whole budget; the AP resends message 1 anyway */
#define AP_GK_M2_QUEUED    0     /* AP_LINK_EVT_GTK's code bits 4-5: message 2 built and queued */
#define AP_GK_M2_NOTBUILT  1     /*   message 2 could not be built */
#define AP_GK_M2_NONE      3     /*   nothing to send: message 1 was refused */
#define AP_GK_M2_MAX_TRIES 40u   /* ~4 s of beacons at worst, past the AP's whole budget */
static UInt8    gGkM1Buf[AP_ENET_HDR + AP_GK_MAX_EAPOL];  /* a queued message 1, as the pump converted it */
static UInt32   gGkM1Len = 0, gGkM1Replaced = 0;
static UInt8    gGkM2Frm[24 + EAPOL_SNAP_LEN + 128];   /* BuildEapolKeyFrame's 802.11 form */
static UInt8    gGkM2Eth[AP_ENET_HDR + 128];           /* the same EAPOL frame, as the Ethernet frame sent */
static UInt32   gGkM2Len = 0, gGkM2Tries = 0;
static UInt32   gGkM1N = 0, gGkOk = 0, gGkSame = 0, gGkWhy[AP_GK_NRESULT];
static UInt32   gGkM2Sent = 0, gGkM2Held = 0, gGkM2Failed = 0, gGkM2Retries = 0, gGkM2Abandoned = 0;
static OSStatus gGkM2LastErr = noErr;
static UInt32   gGkLastTick = 0, gGkKidChanges = 0;

/* The pump's half: copy message 1 for ApGkService. A newer one replaces one still queued (its replay counter
 * is newer, so the older would be refused anyway). Bounded by the buffer: a longer frame is cut, and
 * ApGkParseM1 then refuses it as SHORT, which is the right answer for a message 1 of that size. */
static void ApGkQueueM1(const UInt8 *eth, UInt32 len)
{
    UInt32 i, n = (len > (UInt32)sizeof(gGkM1Buf)) ? (UInt32)sizeof(gGkM1Buf) : len;
    if(gGkM1Pending) gGkM1Replaced++;
    for(i = 0; i < n; i++) gGkM1Buf[i] = eth[i];
    gGkM1Len = n;
    gGkM1Pending = 1;
}

/* One attempt at the queued message 2. "Try later" leaves it queued; anything else settles it, as an event. */
static void ApGkSendM2(void)
{
    OSStatus e;
    gGkM2Tries++;
    e = ApShimTxFromPump(gGkM2Eth, gGkM2Len);
    if(e == kApTxTryLater){ gGkM2Held++; return; }
    gGkM2Pending = 0;
    if(e == noErr) gGkM2Sent++;
    else { gGkM2Failed++; gGkM2LastErr = e; }
    ApLinkEvtPut(AP_LINK_EVT_GTK_M2, (UInt8)(e == noErr ? AP_GK_M2_SENT : AP_GK_M2_FAILED), (UInt16)gGkM2Tries);
}

static void ApGkRenew(const UInt8 *eth, UInt32 len)
{
    ApGkM1 m;
    int r, fate = AP_GK_M2_NONE, same = 0;
    UInt32 i;
    m.keyId = -1;
    gGkM1N++;
    if(!gGkReady || !gPtkReady) r = AP_GK_NO_PTK;
    else if(len <= AP_ENET_HDR)  r = AP_GK_SHORT;
    else r = ApGkParseM1(eth + AP_ENET_HDR, len - AP_ENET_HDR, (const ApU8*)(gPtk + AP_KCK_OFF),
                         (const ApU8*)(gPtk + AP_KEK_OFF), gGkReplay, &m);
    if(r >= 0 && r < AP_GK_NRESULT) gGkWhy[r]++;
    if(r == AP_GK_OK){
        UInt16 flen;
        UInt32 eapAt = 24UL + EAPOL_SNAP_LEN, n;
        for(i = 0; i < 8u; i++) gGkReplay[i] = m.replay[i];    /* accepted: nothing older is answered again */
        same = ApGtkSame(&gGtkTab, m.keyId, m.gtk, m.gtkLen);
        if(same) gGkSame++;                                    /* the AP resent it: answered, not reinstalled */
        else     ApGtkInstall(&gGtkTab, m.keyId, m.gtk, m.gtkLen);
        for(i = 0; i < 32u; i++) m.gtk[i] = 0;                 /* the clear copy does not outlive the call */
        if((UInt32)m.keyId != gGkLastKid && gGkLastKid < 4u) gGkKidChanges++;
        gGkLastKid = (UInt32)m.keyId; gGkLastTick = gLinkTick;
        gGkOk++;
        /* Message 2 = message 4 without PAIRWISE: version | MIC | SECURE, message 1's replay counter, zero
         * nonce, no Key Data. A newer message 1 replaces any message 2 still held. */
        flen = BuildEapolKeyFrame(gGkM2Frm, gShimMac, gShimTgtBssid, 0, m.replay,
                                  gPtk + AP_KCK_OFF,
                                  (UInt16)((m.keyInfo & KEYINFO_VERSION) | KEYINFO_MIC | KEYINFO_SECURE), 0);
        n = ((UInt32)flen > eapAt) ? (UInt32)flen - eapAt : 0;
        if(n == 0 || n > 128u){ gGkM2Failed++; fate = AP_GK_M2_NOTBUILT; }
        else {
            for(i = 0; i < 6u; i++){ gGkM2Eth[i] = gShimTgtBssid[i]; gGkM2Eth[6 + i] = gShimMac[i]; }
            gGkM2Eth[12] = 0x88; gGkM2Eth[13] = 0x8E;          /* EAPOL */
            for(i = 0; i < n; i++) gGkM2Eth[AP_ENET_HDR + i] = gGkM2Frm[eapAt + i];
            gGkM2Len = AP_ENET_HDR + n;
            gGkM2Tries = 0;
            gGkM2Pending = 1;
            fate = AP_GK_M2_QUEUED; }
    }
    ApLinkEvtPut(AP_LINK_EVT_GTK, (UInt8)r,
                 (UInt16)(((m.keyId >= 0) ? (unsigned)m.keyId : 7u) | ((unsigned)fate << 4) | (same ? 0x100u : 0u)));
}

/* The secondary handler's k208 step, once the pump has returned: answer a queued message 1, then one attempt
 * at a queued message 2 -- until it goes, fails, or has been held past the AP's whole budget. */
static void ApGkService(void)
{
    if(gGkM1Pending){ gGkM1Pending = 0; ApGkRenew(gGkM1Buf, gGkM1Len); }
    if(!gGkM2Pending) return;
    if(gGkM2Tries >= AP_GK_M2_MAX_TRIES){
        gGkM2Pending = 0; gGkM2Abandoned++;
        ApLinkEvtPut(AP_LINK_EVT_GTK_M2, (UInt8)AP_GK_M2_ABANDONED, (UInt16)gGkM2Tries);
        return; }
    if(gGkM2Tries) gGkM2Retries++;
    ApGkSendM2();
}

/* ★ A WAY TO EXERCISE THE PATH WITHOUT WAITING FOR OT TO DECIDE TO SEND.
 *
 * ⚠ This is not a workaround for Write, it is a CONTROL for it. If OT never calls Write, the run
 *   says nothing about whether the transmit path works -- and "OT did not send anything" and
 *   "our transmitter is broken" would be indistinguishable. This separates them: the app feeds a
 *   frame through the identical code path, so a failure here is ours and a silent OT is not. */
UInt32 AirPortShimTxEthernet(const UInt8 *eth, UInt32 ethLen)
{
    UInt32 savedWin = 0; int haveSaved; OSStatus e;
    if(!gShimMacEverOn) return 0;
    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }
    /* ⚠ SNAPSHOT gTxStatOurs ACROSS THE DRAIN. Three outcomes have to stay distinguishable and
     * k90 collapsed them into one number:
     *     no status with our cookie at all   -- the frame never reported; did it transmit?
     *     our status arrived, not acked      -- it transmitted and the AP did not answer
     *     our status arrived, acked          -- the AP received it
     * Only the middle one is "the radio spoke and nobody replied"; the first is a different
     * failure entirely and in k90 it was silently reported as the third. */
    e = ApShimWriteFrame(eth,ethLen);
    if(e == noErr){
      UInt32 oursBefore = gTxStatOurs;
      gTxAcked += (UInt32)DrainTxStatus(gTxLastCookie,600);   /* k193: data cookies now carry the rate */
      if(gTxStatOurs == oursBefore) gTxNoStatus++;
    }
    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                              (LogicalAddress)SSB_BAR0_WIN,savedWin);
    return (UInt32)(e == noErr);
}

UInt32 AirPortShimGetTxEth(UInt32 *calls, UInt32 *sent, UInt32 *acked, UInt32 *noKey,
                           UInt32 *notAssoc, UInt32 *badFrame, UInt32 *windowBad,
                           UInt32 *pnWrap, UInt32 *noStatus)
{
    if(calls)     *calls     = gTxCalls;
    if(sent)      *sent      = gTxSent;
    if(acked)     *acked     = gTxAcked;
    if(noKey)     *noKey     = gTxNoKey;
    if(notAssoc)  *notAssoc  = gTxNotAssoc;
    if(badFrame)  *badFrame  = gTxBadFrame;
    if(windowBad) *windowBad = gTxWindowBad;
    if(pnWrap)    *pnWrap    = gTxPnWrap;
    if(noStatus)  *noStatus  = gTxNoStatus;
    return gTxSent;
}

/* ★★★ 8-11: WHAT IS THIS CARD ACTUALLY CALLED IN THE NAME REGISTRY?
 *
 * TheDriverDescription's nameInfoStr must equal the node's name for the Device Manager to match
 * a driver to it, and a wrong name is a SILENT no-op -- the DM simply never loads us, which is
 * indistinguishable from every other reason nothing happened. ApShimFindCard has always matched
 * on the vendor/device ID out of config space and never looked at the name, so we have shipped
 * eleven stages without ever knowing it.
 *
 * ⚠ Reported, not acted on. ap_ndrv.h currently carries the CONVENTION ("pci14e4,4320") and
 *   says so; the next increment sets it from whatever this prints. */
static char gNodeName[64];
static UInt32 gNodeNameLen = 0;

static void ApShimReadNodeName(void)
{
    RegPropertyValueSize sz = (RegPropertyValueSize)(sizeof(gNodeName) - 1);
    OSStatus e;
    UInt32 i;
    for(i=0;i<sizeof(gNodeName);i++) gNodeName[i] = 0;
    e = RegistryPropertyGet(&gBus.node, "name", (void*)gNodeName, &sz);
    if(e != noErr){
      Say1("  [8-11] could not read the node's \"name\" property, err = ",(unsigned long)e);
      return; }
    gNodeNameLen = (UInt32)sz;
    { Str255 L; L[0]=0;
      PCat(L,"  [8-11] ★ Name Registry node name = \"");
      PCat(L,gNodeName); PCat(L,"\"");
      Out(L); }
    Say1("          length = ",(unsigned long)gNodeNameLen);

    /* ★★★ 8-14: WHAT DOES THIS NODE ACTUALLY CARRY? ENUMERATE IT, DO NOT GUESS AGAIN.
     *
     * Two guesses have now been spent on how the Device Manager decides to load a driver:
     *   k100  the FILE TYPE -- refuted, 'ndrv' breaks CFM's by-name registration outright
     *   k101  a cfrg member with architecture 'ndrv' -- built, shipped, verified in the file,
     *         and the DM still never called DoDriverIO
     *
     * Both were reasonable readings of Apple's driver and both were wrong, which is the signal
     * to stop reasoning about the mechanism and read the node instead. The property list says
     * what keys exist to match on and whether anything has already claimed this node -- a
     * "driver-ref" would mean it is spoken for, and a "compatible" would mean nameInfoStr may
     * need to match THAT rather than "name". Neither is knowable from the headers.
     *
     * ⚠ Names only. Values are a second step, and dumping arbitrary property values into a log
     *   that gets published is how a MAC address or worse ends up somewhere it should not. */
    { RegPropertyIter it;
      OSStatus ie = RegistryPropertyIterateCreate(&gBus.node, &it);
      if(ie != noErr){
        Say1("  [8-14b] could not iterate this node's properties, err = ",(unsigned long)ie);
      } else {
        Boolean done = false;
        /* ⚠⚠ RegPropertyNameBuf, NOT RegPropertyName. The header types the out-parameter as
         *   `RegPropertyName *`, and `typedef char RegPropertyName` -- so a `RegPropertyName nm;`
         *   is ONE BYTE and `&nm` type-checks perfectly while the Name Registry writes up to 32
         *   into it. Silent stack smash at Open time, on every boot, with no diagnostic. The
         *   prototype cannot warn because char* carries no size; only the header says so.
         *   Same family as [[reference_os9_logbuf_vsprintf_smash]]. */
        RegPropertyNameBuf nm;
        int n = 0;
        Say("  [8-14b] ★ every property on our Name Registry node:");
        while(!done && n < 40){
          nm[0] = 0; nm[sizeof(nm)-1] = 0;
          ie = RegistryPropertyIterate(&it, nm, &done);
          if(ie != noErr || done) break;
          nm[sizeof(nm)-1] = 0;                       /* belt and braces on the terminator */
          { Str255 L; L[0]=0; PCat(L,"           "); PCat(L,nm); Out(L); }
          n++; }
        (void)RegistryPropertyIterateDispose(&it);
        Say1("           total = ",(unsigned long)n);
        (void)0; } }

    /* ★★★ 8-15b: THE VALUES, not just the names.
     *
     * k102 listed 24 properties and that alone answered two things -- there is no "driver-ref",
     * so nothing has claimed this node, and there IS a "category" and a "network-type", which
     * are what a port scanner would look at. But a name is not a value, and the four that decide
     * matching are all strings we have never read.
     *
     * ⚠ DELIBERATELY NOT "every property". "local-mac-address", "AAPL,address" and
     *   "assigned-addresses" are either personal or useless here, and these logs go on a file
     *   share -- [[feedback_airport_scrub_before_public]]. Named properties only. */
    { static const char *kWant[] = { "name", "compatible", "category", "network-type",
                                     "device_type", "model", 0 };
      int i;
      Say("  [8-15b] ★ the values that a scanner would match on:");
      for(i = 0; kWant[i]; i++){
        char v[128]; RegPropertyValueSize sz = (RegPropertyValueSize)sizeof(v) - 1;
        OSStatus e = RegistryPropertyGet(&gBus.node, (char*)kWant[i], v, &sz);
        Str255 L; L[0] = 0;
        PCat(L, "           "); PCat(L, (char*)kWant[i]); PCat(L, " = ");
        if(e != noErr){ PCat(L, "(absent)"); Out(L); continue; }
        if(sz > (RegPropertyValueSize)sizeof(v) - 1) sz = (RegPropertyValueSize)sizeof(v) - 1;
        v[sz] = 0;
        /* ⚠ An Open Firmware string property can be a NUL-SEPARATED LIST -- "compatible" almost
         *   always is. Printing to the first NUL would silently show one entry of several, and
         *   the one we need could be any of them. */
        { RegPropertyValueSize k = 0; int first = 1;
          while(k < sz){
            int len = 0; while(k + len < sz && v[k+len]) len++;
            if(len){
              if(!first) PCat(L, " | ");
              PCat(L, "\""); PCat(L, &v[k]); PCat(L, "\"");
              first = 0; }
            k += len + 1; } }
        Out(L); }
      { UInt32 ids[3]; static const char *kIds[] = { "vendor-id", "device-id", "class-code", 0 };
        for(i = 0; kIds[i]; i++){
          RegPropertyValueSize sz = (RegPropertyValueSize)sizeof(UInt32);
          ids[i] = 0xFFFFFFFFUL;
          (void)RegistryPropertyGet(&gBus.node, (char*)kIds[i], &ids[i], &sz);
          { Str255 L; L[0]=0; PCat(L,"           "); PCat(L,(char*)kIds[i]); PCat(L," = ");
            PCatHex(L,(unsigned long)ids[i],8); Out(L); } } } }
    /* ★ 8-12: did ANY copy of this fragment get DoDriverIO(kInitialize)? The property is
     * outside every copy's globals, so this answers even when the Device Manager loaded a
     * copy the application cannot see. */
    { UInt32 mark = 0; RegPropertyValueSize ms = (RegPropertyValueSize)sizeof(mark);
      OSStatus me = RegistryPropertyGet(&gBus.node,"airport-ndrv-init",(void*)&mark,&ms);
      if(me == noErr && mark == 0x4E445256UL)
        Say("  [8-12] ★★★ THE DEVICE MANAGER CALLED DoDriverIO(kInitialize) -- the beacon is set.");
      else
        Say1("  [8-12] no ndrv beacon on our node (DM never loaded us as a driver). err = ",
             (unsigned long)me); }
    { Str255 L; int k; L[0]=0; PCat(L,"          our descriptor currently claims = \"");
      PCat(L,AP_NDRV_NODE_NAME); PCat(L,"\"");
      Out(L);
      k = 1;
      { UInt32 q; const char *g = AP_NDRV_NODE_NAME;
        for(q=0; g[q] || gNodeName[q]; q++) if(g[q] != gNodeName[q]){ k = 0; break; } }
      /* ⚠ The first version of this line said "the convention was right" on a match. It was
       * not -- the convention said "pci14e4,4320" and k98 proved it wrong; the descriptor
       * matches because it was SET from what k98 read. A log line that credits a guess for a
       * fact it was corrected by is exactly the kind a future reader believes. */
      Say(k ? "          [ok] they MATCH -- the descriptor is set from what this machine reports."
            : "          ⚠ they DIFFER. The descriptor must be set from the line above."); }
}

/* ============================================================================================
 * ★★★★★★ STAGE 8-9 — THE STEADY RUNNING STATE. The driver joins the network by itself.
 *
 * Until now every step past bring-up was requested by the test application: it called
 * TxProbe, PrepPmk, Auth, Assoc, Handshake and ArmIrq in order, and the link existed only
 * inside the window the app held open. That is a probe, not a driver.
 *
 * ⭐ AND IT CONTRADICTS THE STANDING REQUIREMENT FOR THIS PROJECT: the deliverable is
 *   extension-only, with no helper application. A driver that needs an app to join a network is
 *   not the thing being built, however well it works.
 *
 * So the join moves in here and runs from EnetHAL_Open -- the ONE selector Apple documents as
 * task time, which is what makes this legal: it allocates nothing new, but it does spin, and
 * spinning below task level would be a different conversation.
 *
 * ⚠ IT RETRIES, BECAUSE THE FAILURE IT MUST SURVIVE IS MEASURED, NOT IMAGINED. k84-k90
 *   authenticated eight times running; k91's first attempt was met with silence from an AP that
 *   had ACKed the request, and the same build passed on the immediate re-run. About one in ten.
 *   A single-shot join would therefore fail to bring the interface up roughly every tenth boot,
 *   and the symptom -- "no network this time" -- is exactly the kind a user learns to shrug at
 *   and we never hear about. Three attempts, and the count is reported.
 *
 * ⚠ THE FIRST THING IT NEEDS IS A BEACON, and beacons arrive about every 100 ms. ApShimFindBssid
 *   reads the ring, so the ring has to have been filling for a moment first. That wait is the
 *   only new delay in Open and it is bounded.
 */
/* ⛔⛔ 8-9b — WHY THE ARM IS SWITCHED OFF, AND WHAT I DID WRONG.
 *
 * k92 grey-screened the machine the moment the application opened the endpoint. It changed TWO
 * things at once, and that is the whole problem with the report I can write about it:
 *
 *   1. the join moved INSIDE EnetHAL_Open, where it SPINS -- worst case 8.3 s per attempt and
 *      three attempts, so up to ~25 SECONDS inside a call OT makes from OTOpenEndpoint, on a
 *      cooperatively scheduled OS, without yielding once
 *   2. the interrupt arm moved inside Open too, where k89 and k91 had armed it from the
 *      application, late, and ran clean twice
 *
 * Either could hang the machine and I cannot attribute it, because I shipped them together.
 * "One change per run" is a rule this project already has and I broke it on the single most
 * dangerous surface in the driver.
 *
 * ⚠ 25 SECONDS IS INDEFENSIBLE INDEPENDENT OF THE CRASH. An Ethernet driver may negotiate a
 *   link in Open, and that costs a second or two; it does not get to block the Open Transport
 *   stack for half a minute. Even if the arm turns out to be the culprit, the join budget was
 *   wrong and is fixed below regardless.
 *
 * So k93 separates them: the join stays, hard-bounded; the arm goes back to the explicit
 * application call that has two clean runs behind it. If k93 is clean, the join is exonerated
 * and the arm-in-Open is the suspect; if k93 hangs, it is the join. One variable. */
#define AP_ARM_IRQ_IN_OPEN 1

/* ★★★ 8-9c: INSTALL THE HANDLER IN Open, BUT DO NOT ENABLE THE DEVICE.
 *
 * k93 attributed the k92 grey screen to arming inside Open -- the self-join ran clean there and
 * everything downstream passed. The arm is two separable things and only one of them lets the
 * card raise a line, so this run does the harmless half and nothing else:
 *
 *     ON   InstallInterruptFunctions, with GEN_IRQ_MASK left at 0
 *     OFF  the enable -- the app still calls AirPortShimArmIrq, which installs (already done,
 *          returns immediately) and then enables, exactly as it did in k93
 *
 * ⚠ AN INTERRUPT STORM FROM THIS CARD IS IMPOSSIBLE BY CONSTRUCTION WHILE THIS IS THE ONLY
 *   CHANGE, not merely unlikely: with its output masked the device cannot assert the line. If
 *   the machine still hangs, the cause is installing a handler inside Open -- which would be a
 *   genuinely surprising and much more interesting result than an interrupt we mishandled. */
#define AP_INSTALL_IRQ_IN_OPEN 1

/* ⚠ HARD BUDGET. One attempt inside Open, and a beacon wait measured in hundreds of
 * milliseconds rather than seconds. The ~1-in-10 retry that motivated three attempts does not
 * justify holding OTOpenEndpoint for 25 s -- a retry belongs in a later re-Open, or in a
 * periodic task once one exists, not in a spin the stack is waiting on.
 *
 * ★★ k203: THREE ATTEMPTS AGAIN, BECAUSE THE PREMISE IS GONE. That note is from the shim era, when
 * the join ran inside OTOpenEndpoint and the boot vehicle's Notification Manager pump retried it
 * (8-33). The native module has neither: the join runs ONCE, in InitStreamModule at boot, and
 * nothing ever retries it -- so the second k202 boot, which heard everything and lost only the
 * handshake, stayed offline until a reboot. A boot-time InitStreamModule can afford a few more
 * seconds on the rare failure; a boot with no network cannot be recovered at all. ApShimJoinReset
 * (below) runs between attempts, as it did when this was 3 before 8-9. The real fix -- a background
 * rejoin that also covers a link lost mid-session -- is the next robustness step. */
#define AP_JOIN_ATTEMPTS   3
#define AP_JOIN_BEACON_MS  600UL

static UInt32 gJoinTries = 0, gJoinBeaconMs = 0;
static int    gJoined = 0;

/* ★★★★★ 8-35: THE RX ENGINE, BEFORE WE EVER TRANSMIT.
 *
 * Three runs now show the same shape and it is far more specific than "the radio came up deaf":
 *
 *   k122  scan heard beacons -> BSSID picked -> probe ACKED -> auth ACKED -> 0 frames in 500 ms
 *   k124  same, 0 frames of any type, MACCTL ENABLED|AWAKE|BEACPROMISC
 *   k125  two full re-bring-ups from the pump, both reached AUTH (so beacons WERE heard), both
 *         then walked 0 frames
 *
 * The receiver demonstrably works during the scan and stops once we transmit. MACCTL is innocent
 * (read live, k124), the BSSID filter and templates match b43 field for field, the DMA engine
 * reads ACTIVE, and the TX and RX rings are separate allocations on separate engine blocks.
 *
 * So capture the RX engine's four registers BEFORE the first transmit of the join, and let the
 * census print them beside the post-wait values. If transmitting moves RXCTL, RXRING or RXINDEX,
 * the difference is the defect and it will be sitting in one line. If all four are identical,
 * the engine is configured exactly as it was while it was working, and the fault is in the PHY
 * or the MAC rather than the DMA -- which is a different search entirely.
 *
 * ⚠ A snapshot taken before the FIRST transmit, not before the auth: the probe request goes out
 *   first, and blaming the auth for something the probe did would be the same mistake as blaming
 *   the join for something the bring-up did.
 *
 * ★ 8-36: AND A SECOND ONE THE MOMENT THE SCAN SUCCEEDS. With only the entry snapshot, a
 *   CHANGED register means "the scan, the probe or the auth did it" and the next run has to
 *   bisect that by hand at the cost of a reboot. k125 already tells us the scan WORKS on the
 *   attempt that gets this far -- it heard a beacon and learned a BSSID -- so the second
 *   reading splits the span at the one point we know the receiver was alive. */
static void CaptureRxRegs(int which, UInt32 base)
{
    /* ⛔⛔ GUARDED, BECAUSE k127 SHIPPED THIS UNGUARDED AND TOOK THE MACHINE TO MACSBUG.
     *
     *     PowerPC access exception at 5E9E5398
     *     Current application is "ShareWay IP Personal Bgnd"
     *     Address 5E9E5398 is in the CFM fragment "AirPortShim" at 5E9E52F0
     *     ... EnetMac_ShimEntry+000D0 -> EnetHAL_Entry+001E4 -> (inside the shim)
     *
     * Open Transport opened the driver from a BACKGROUND APPLICATION's context -- a third copy
     * of this fragment, in the Process Manager heap, whose globals are its own and start at
     * zero. These four reads were the FIRST statement in ApShimJoinOnce with no SsbSelectCore
     * ahead of them, no window check and no null test, while every other hardware path in this
     * file opens with SsbSelectCore(&gBus, ...) and checks the result. A null gBus.bar0 is then
     * an access exception, and that is exactly what it was.
     *
     * ⚠ THE NULL TEST MUST COME FIRST. ApShimWindowIsOurs() reads SSB_IDHIGH through gBus.bar0
     *   itself, so using it as the only guard would fault in precisely the case being guarded
     *   against.
     *
     * ⚠ And gRxRegPreOk is cleared up front: a stale 1 from an earlier attempt would make the
     *   census print last attempt's registers as if they were this one's, which is the kind of
     *   quietly-wrong instrument this session has already produced three times. */
    if(which < 0 || which > 1) return;
    gRxRegPreOk[which] = 0;
    if(!gBus.bar0)            return;   /* this copy never mapped the card */

    /* ★★★★★ 8-40: SELECT THE CORE, DO NOT MERELY TRUST THE WINDOW. Seven deaf runs, and this
     * census kept printing RXCTL/RXRING/RXINDEX/RXSTATUS = 0x00000000 across all three points --
     * which is EITHER the RX DMA genuinely being off (the frame-delivery fault, since the MAC
     * still hears ACKs but delivers no frames) OR this read landing on the wrong core because
     * ApShimWindowIsOurs() passed when the window was not actually ours. The snapshot's own
     * read, which does an explicit SsbSelectCore first, saw RXSTATUS ACTIVE -- so the two
     * disagree, and the difference is exactly the select. Do what the snapshot does: save the
     * BAR0 window, select the 802.11 core, read, restore. Now a zero is the hardware's answer,
     * not the instrument's. This is the one measurement that tells us whether the receive engine
     * is running at all during the join.
     *
     * ⚠ Task level only -- SsbSelectCore writes PCI config space. CaptureRxRegs is reached only
     *   from ApShimJoinOnce, inside EnetHAL_Open, which is documented task time. */
    { UInt32 savedWin = 0;
      int haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                            &savedWin) == noErr);
      if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
        if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                  (LogicalAddress)SSB_BAR0_WIN,savedWin);
        return; }
      gRxRegPre[which][0] = ssb_r32(gBus.bar0, base + B43_DMA32_RXCTL);
      gRxRegPre[which][1] = ssb_r32(gBus.bar0, base + B43_DMA32_RXRING);
      gRxRegPre[which][2] = ssb_r32(gBus.bar0, base + B43_DMA32_RXINDEX);
      gRxRegPre[which][3] = ssb_r32(gBus.bar0, base + B43_DMA32_RXSTATUS);
      gRxRegPreOk[which]  = 1;
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin); }
}

/* ★★★★★★ 8-44: THE DEAF RADIO, NAMED AND FIXED. Ensure the RX DMA engine is ACTIVE at the moment
 * we scan for beacons -- re-arming it in place if it is not.
 *
 * k138 proved the deaf radio from the driver's own registers. The RX ring is armed early, in the
 * bring-up (RxRingArm -> RXSTATUS state ACTIVE, 0x1000). Then b43_mac_enable and the TX-half of
 * b43_dma_init run, and by the time we reach the beacon scan the engine reads RXSTATUS = 0x00000000
 * (state DISABLED) with RXDPTR still on slot 0 -- it halted, without ever processing a descriptor,
 * and heard zero frames of any kind. A good boot reads ACTIVE here and hears beacons. Nothing in
 * our code explicitly resets RX in that gap (the two RxReset calls are teardown), so the engine is
 * self-halting across the steps that follow an over-early arm -- exactly the ordering b43 avoids by
 * setting the ring up LAST. We can't reorder the whole bring-up cheaply, but we can do the
 * equivalent: re-arm the ring HERE, at the point of use, if it is not live. Nothing runs between
 * this and RxWaitFor, so a re-arm that takes stays taken through the scan.
 *
 * ⚠ Own window save/select/restore, exactly like CaptureRxRegs (8-40): SsbSelectCore writes PCI
 *   config space and is task-level only, which this is (JoinOnce runs inside EnetHAL_Open). Reads
 *   and RxRingArm use gBus.bar0+base and require the 802.11 core selected. Returns 1 iff ACTIVE. */
static int ApShimEnsureRxLive(void)
{
    int live = 0;
    UInt32 savedWin = 0;
    int haveSaved;

    if(!gBus.bar0 || !gShimDmaBase) return 0;
    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,&savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }

    {
      UInt32 rxs = ssb_r32(gBus.bar0, gShimDmaBase + B43_DMA32_RXSTATUS);
      if((rxs & B43_DMA32_RXSTATE) == B43_DMA32_RXSTAT_ACTIVE){
        live = 1;                                    /* already live -- do not disturb a good boot */
      } else {
        gRxReArmed++;
        SayH("  [8-44] RX engine NOT active before the scan -- RXSTATUS = ", (unsigned long)rxs, 8);
        (void)RxRingArm(gShimDmaBase,&gShimRxRing,gShimRxBufLog,gShimRxBufPhys,
                        B43_DMA0_RX_FW351_FO,AP_SHIM_RX_BUFSZ);
        rxs = ssb_r32(gBus.bar0, gShimDmaBase + B43_DMA32_RXSTATUS);
        live = ((rxs & B43_DMA32_RXSTATE) == B43_DMA32_RXSTAT_ACTIVE);
        if(live){ gRxReArmStuck++;
          SayH("         ★ re-armed and it TOOK -- RXSTATUS now = ", (unsigned long)rxs, 8); }
        else
          SayH("         ⚠ re-arm did NOT stick -- RXSTATUS still = ", (unsigned long)rxs, 8);
      }
    }

    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,savedWin);
    return live;
}

/* k209: channel agility -- defined with the scan code below, forward-declared for the join. */
static UInt32 ApScanSweep(void);
static int    ApScanAdoptOurs(void);
static int    ApShimBootTarget(void);      /* k210: pick a known network in range, set the runtime target */

static int ApShimJoinOnce(void)
{
    UInt32 waited = 0;                  /* ⚠ declaration first: this file builds as C89 */

    CaptureRxRegs(0, gShimDmaBase);     /* 8-35: before ANY transmit in this attempt */

    /* ★★★★★ 8-45: THE RX DMA STATE AT THE SCAN, PRINTED -- as found, before any re-arm. k139's
     * re-arm never fired, which left the deaf radio ambiguous: was the engine ACTIVE here (so the
     * fault is upstream in the RF/PHY and the earlier DISABLED reading was later, post-failure), or
     * did the core-select bail? These four registers, from the capture just done, say it outright
     * every boot -- no more inference. RXSTATUS state field: 0x1000 = ACTIVE, 0x0000 = DISABLED.
     * RXINDEX is the hardware's write pointer; if it is 0 here AND still 0 after a failed scan
     * (8-45b below), the MAC delivered NOTHING to the ring -- an RF/PHY deafness, not a DMA one. */
    if(gRxRegPreOk[0]){
      SayH("  [8-45] RX DMA at scan (as found) -- RXCTL   = ",(unsigned long)gRxRegPre[0][0],8);
      SayH("                                      RXRING  = ",(unsigned long)gRxRegPre[0][1],8);
      SayH("                                      RXINDEX = ",(unsigned long)gRxRegPre[0][2],8);
      SayH("                                      RXSTATUS= ",(unsigned long)gRxRegPre[0][3],8);
    } else
      Say("  [8-45] RX DMA at scan -- CaptureRxRegs could NOT select the core (window bailed).");

    /* ★★★ 8-44: the receiver MUST be live before we listen -- see ApShimEnsureRxLive. The re-arm
     * only triggers if the engine is not ACTIVE; 8-45 above records the as-found state first. */
    (void)ApShimEnsureRxLive();

    /* 1. Wait for a beacon carrying our SSID, so the BSSID can be learned rather than guessed.
     *    ⚠ The app used to do this by dwelling for three seconds. A second is plenty for a
     *    100 ms beacon interval, and Open should not cost more than it must. */
    while(waited < AP_JOIN_BEACON_MS && !ApShimPickBssid()){ SsbSpinUs(50000); waited += 50UL; }
    gJoinBeaconMs = waited;

    /* ★★★★★ 8-39: THE RX-SENSITIVITY CALIBRATION, MEASURED, ON EVERY ATTEMPT -- good or deaf.
     *
     * The deaf radio is now PROVEN to be our software, not the hardware or a regression: Tiger
     * received DHCP on this card and antenna the same day, and this bring-up is byte-identical
     * to k130, the one run that joined. Same code + a freshly power-cycled card + a different
     * result means the differing input is what the bring-up MEASURES from the radio -- the RX
     * gain and NRSSI calibration -- and every one of the fifteen PHY oracles checks a WRITE, not
     * these measured results. Printed BEFORE the beacon-found branch so a GOOD run logs them too
     * and a deaf run can finally be diffed against it:
     *
     *   gNrssiSlope / gNrssi0 / gNrssi1  -- a wrong slope means a wrong RSSI THRESHOLD, and the
     *     MAC then rejects every frame as below-threshold: a silent, total deaf.
     *   gMaxLbGain / gTrswRxGain / gLnaGain / gPgaGain -- the RX gain chain; low or zero is a
     *     receiver that hears nothing while transmit still works, which is exactly the symptom.
     *   gNrssiRestoreBad -- the header's retracted block named "NRSSI restore" as the section
     *     that went void ~half the time on repeat runs; 1 here is that fault, caught.
     *
     * ⚠ Globals in ap_phy_initg.h, reached through this file's include chain; a memory load,
     *   legal at any level, and the join runs at task time regardless. */
    Say("  [8-39] RX sensitivity calibration (measured this attempt):");
    Say1("          gNrssiSlope               = ",(unsigned long)(SInt32)gNrssiSlope);
    Say1("          gNrssi0 (SInt16)          = ",(unsigned long)(UInt16)gNrssi0);
    Say1("          gNrssi1 (SInt16)          = ",(unsigned long)(UInt16)gNrssi1);
    Say1("          gNrssiRestoreBad (1=bad)  = ",(unsigned long)gNrssiRestoreBad);
    Say1("          gMaxLbGain                = ",(unsigned long)(UInt16)gMaxLbGain);
    Say1("          gTrswRxGain               = ",(unsigned long)(UInt16)gTrswRxGain);
    Say1("          gLnaGain                  = ",(unsigned long)(UInt16)gLnaGain);
    Say1("          gPgaGain                  = ",(unsigned long)(UInt16)gPgaGain);

    /* ★★★ k209: CHANNEL AGILITY. Our network was not on the home channel within the budget -- so before
     * giving up, SCAN every channel for it, and adopt the channel it is really on. This is the whole point
     * of the scan for the join: the compiled-in channel (1) is right only for the one network it was built
     * for, and a stranger's base station is on 6 or 11 as often as not. Listen-only, no probe requests; the
     * receive interrupt is not armed yet this early in the join, so the sweep owns the ring. A miss still
     * falls through to the same diagnostics below. */
    if(!gShimHaveBssid){
      int adopted = 0;
      UInt32 scanWin = 0;
      int scanHaveWin = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,&scanWin) == noErr);
      Say("  [8-9] our network not on the home channel -- scanning every channel for it");
      /* ⚠ SELECT THE 802.11 CORE FIRST. ApShimEnsureRxLive and CaptureRxRegs above each RESTORE the window
       * when they return, so it is not the 802.11 core's here -- and SwitchChannel writes MMIO. The sweep
       * hops channels, so it leaves the core selected; put the window back afterward, as every entry does. */
      if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) == noErr){
        (void)ApScanSweep();
        adopted = ApScanAdoptOurs();
        SwitchChannel(gShimChannel);     /* home again: gShimChannel is the adopted channel, or unchanged */
      } else Say("  [8-9] could not select the 802.11 core for the scan -- skipped");
      if(scanHaveWin) (void)ExpMgrConfigWriteLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,scanWin);
      (void)ApShimEnsureRxLive();        /* re-arm the receiver on the (possibly new) home channel */
      if(adopted)
        Say1("  [8-9] ★ found our network by scanning; channel is now ",(unsigned long)gShimChannel);
    }

    if(!gShimHaveBssid){
      Say1("  [8-9] no beacon for our SSID within ms = ",(unsigned long)AP_JOIN_BEACON_MS);
      /* ★★★ AND SAY WHETHER THE RECEIVER HEARD ANYTHING AT ALL, because "our network was not
       * there" and "this radio is not receiving" are completely different failures and this
       * line has been reporting them identically since 8-9.
       *
       * k121 lost a run to exactly that: core up, firmware rev 478, ApPhyInitG returned 1, DMA
       * and receiver both status 0 -- a textbook clean bring-up -- and then zero beacons. With
       * only the line above, that is indistinguishable from the network being switched off.
       * The census counters have existed since 8-4-c and were never printed here.
       *
       * ⇒ mgmt/beacon counts NON-ZERO means the receiver works and our SSID was not among what
       *   it heard -- look at the channel, the SSID match, the scan budget.
       * ⇒ ALL ZERO means the radio initialised cleanly and receives nothing, which is the
       *   ~30% intermittent this project has carried since Stage 5, and a reboot is the only
       *   known clearing action. Not a join problem at all. */
      Say("        what the receiver actually heard while scanning:");
      Say1("          frames walked, any type   = ",(unsigned long)(gRxCensusType[0]+gRxCensusType[1]
                                                                   +gRxCensusType[2]+gRxCensusType[3]));
      Say1("          management (type 0)       = ",(unsigned long)gRxCensusType[0]);
      Say1("            of which beacons (sub 8)= ",(unsigned long)gRxCensusMgmt[8]);
      Say1("            probe responses (sub 5) = ",(unsigned long)gRxCensusMgmt[5]);
      Say1("          data (type 2)             = ",(unsigned long)gRxCensusType[2]);
      Say1("          channel we tuned to       = ",(unsigned long)gShimChannel);
      Say("          (RX cal values are in the [8-39] block just above)");
      if((gRxCensusType[0]+gRxCensusType[1]+gRxCensusType[2]+gRxCensusType[3]) == 0)
        Say("        ⇒ ZERO frames of ANY kind: the receiver is not receiving. This is the");
      else
        Say("        ⇒ the receiver IS working; our SSID was simply not among what it heard.");
      /* ★★★★★ 8-45b: THE RX DMA STATE AFTER THE FAILED SCAN. Compare RXINDEX here against the
       * 8-45 (as-found) value: if it MOVED, the MAC delivered frames to the ring during the scan
       * and the deafness is in our ring-walk/delivery (software); if it is still 0 with RXSTATUS
       * ACTIVE, the MAC delivered NOTHING -- the RF/PHY heard nothing, and no re-arm can help. */
      CaptureRxRegs(1, gShimDmaBase);
      if(gRxRegPreOk[1]){
        SayH("  [8-45b] RX DMA after failed scan -- RXINDEX = ",(unsigned long)gRxRegPre[1][2],8);
        SayH("                                      RXSTATUS= ",(unsigned long)gRxRegPre[1][3],8);
      }
      return 0; }

    /* ★ 8-36: THE SECOND READING, HERE, WHERE THE RECEIVER IS PROVABLY ALIVE. We only reach
     * this line because ApShimPickBssid matched a beacon carrying our SSID, so the engine was
     * delivering frames a moment ago. Everything after this point is transmit. */
    CaptureRxRegs(1, gShimDmaBase);

    /* 2. The PMK first (WPA2 only). PBKDF2 is 316 ms on this machine and message 1 of the handshake
     *    arrives unprompted about 100 ms after the association response -- it will not wait for us. An
     *    OPEN network has no PMK; PrepPmk would only log a misleading "no WPA2 target", so skip it. */
    if(gTargetSec != kApSecOpen) (void)AirPortShimPrepPmk();
    (void)AirPortShimTxProbe();
    (void)AirPortShimAuth();
    if(gShimAuthErr != noErr) return 0;
    (void)AirPortShimAssoc();
    if(gShimAssocErr != noErr) return 0;
    if(gTargetSec == kApSecOpen){
      /* ★ k217 OPEN NETWORK: Open-System auth + association ARE the whole join. There is no 4-way
       * handshake (the AP sends no unprompted EAPOL message 1, which would otherwise stall the 2 s
       * RxWaitFor in AirPortShimHandshake every attempt) and no PMK/PTK/GTK. gShimHsErr stays at
       * "never attempted" -- truthful, and link-up is gated on gLink.up, not on it. */
      Say("");
      Say("  [8-4] OPEN NETWORK -- no 4-way handshake; the association completes the join.");
      return 1;
    }
    (void)AirPortShimHandshake();
    return (gShimHsErr == noErr);
}

/* ⚠ THE PER-ATTEMPT STATE HAS TO BE RELEASED OR A RETRY IS NOT A RETRY.
 *
 * Auth, Assoc and Handshake each open with `if(gShimXxxErr != 1) return 0;` -- once per load,
 * which was right when the app drove them exactly once. Leaving that in place would make
 * attempts 2 and 3 return instantly without transmitting anything, and the log would show three
 * tries and one transmission. That is the "never entered" reading of a retry count, and this
 * project has already been caught by it once. */
static void ApShimJoinReset(void)
{
    gShimAuthErr  = 1;  gShimAuthGot   = 0;  gShimAuthAcked  = 0;
    gShimAssocErr = 1;  gShimAssocGot  = 0;  gShimAssocAcked = 0;
    gShimHsErr    = 1;  gShimM1 = 0; gShimM3 = 0; gShimM2Sent = 0; gShimM4Sent = 0;
    gShimHaveBssid = 0;                      /* re-learn it: the AP may have moved BSSID */
    gGkReady = 0; gGkM1Pending = 0; gGkM2Pending = 0;   /* k208: the old association's renewals are over (the handshake clears the keys) */
    /* ⚠⚠ k214: the SAME "once per load" trap the comment above warns of -- AirPortShimPrepPmk opens with
     * `if(gShimPmkReady) return 1;` and copies gTargetPmk into gPmk only when it runs. That was right when
     * one network was compiled in, but since k210 the target is chosen at RUNTIME: joining a SECOND network
     * in one boot left gPmk holding the FIRST network's PMK, so the PTK (ApPmkToPtk reads gPmk) and its KCK
     * were wrong, the AP rejected our message 2's MIC, and message 3 never came -- read exactly like a wrong
     * password (and k213 duly reported one). Release it here so every attempt re-reads the CURRENT target's
     * PMK. The PTK is re-derived each handshake from the fresh ANonce anyway; clear its flag for the same
     * reason. The home-survey log now also prints which network each PMK was loaded for (AirPortShimPrepPmk). */
    gShimPmkReady = 0; gShimPtkReady = 0;
}

static void ApShimJoin(void)
{
    int attempt;
    if(gJoined) return;
    /* ★★★ k210: WHAT DO WE JOIN? There is no compiled network any more. Pick a known one that is in range
     * (ApShimBootTarget: load Preferences, scan, choose). No known network in range -> the driver simply
     * runs, scanning and publishing the list, until the panel chooses one (kApCmdJoin). This is the
     * extension-only, no-secret boot: it connects on its own when it has been told a network before, and
     * waits quietly when it has not. */
    if(!gHaveTarget && !ApShimBootTarget()){
      Say("  [8-9] no known network is in range -- not joining. Choose one in the AirPort Extreme panel.");
      return; }
    for(attempt = 0; attempt < AP_JOIN_ATTEMPTS && !gJoined; attempt++){
      if(attempt) ApShimJoinReset();
      gJoinTries++;
      Say("");
      Say1("  [8-9] ★★★ JOINING THE NETWORK, attempt ",(unsigned long)gJoinTries);
      if(ApShimJoinOnce()) gJoined = 1; }
    /* k215: record the boot network as the one OT already knows about (ApOtmAnnounceOnline pushes its
     * network-change). The link watch then re-pushes only when a LATER join lands on a different network. */
    if(gJoined){ UInt32 z, tl = (gTargetLen > 32) ? 32 : gTargetLen;
                 gOtmLastNetLen = tl; for(z = 0; z < tl; z++) gOtmLastNetSsid[z] = gTargetSsid[z]; }
    Say(gJoined ? "  [8-9] ★★★★★★ JOINED. The driver did this without an application."
                : "  [8-9] [!!] the join did not complete within its budget.");
}

/* ★★★★ k204: THE LINK LAYER, TASK LEVEL -- the body the heartbeat's Notification Manager request runs.
 *
 * Everything that DECIDES lives here (ApLinkStep, in ap_link.h, host-tested), and everything that must
 * run at task level: the join (spins, logs), the log flush (File Manager). One join attempt per body,
 * never a loop: the body borrows the foreground process's event loop, and EHCI h84 is the standing
 * warning -- "a body that does not return IS a wedged desktop". So the body times itself, keeps its
 * maxima for the snapshot, and refuses to nest.
 *
 * ⚠ THE LOG FLUSH WAITS WHILE THE LINK IS DOWN. The classic File Manager serves one request at a time,
 *   and with the link down the AppleShare client's requests to the Pi sit until they time out. A
 *   synchronous write to the boot disk queued behind them would freeze the Finder -- whose thread this
 *   is -- for that long, and delay the rejoin that would unstick them. Lines stay in the ring and reach
 *   the disk on the first body after the rejoin. */
static void ApShimDumpRingToFile(void);   /* below: guarded, and appends since k204 */
static int  ApShimLogHasNew(void);        /* below */
static ApLink gLink;                      /* ⚠ TASK LEVEL ONLY: the body is its sole writer */
static UInt32 gLinkInBody = 0, gLinkNested = 0, gLinkNotTask = 0;
static UInt32 gLinkLatMaxUs = 0, gLinkBodyMaxUs = 0, gLinkRejoinMaxUs = 0, gLinkRejoinLastUs = 0;
static UInt32 gLinkLastHbTick = 0, gLinkLastFlushTick = 0;
static UInt32 gLinkHbBcn = 0, gLinkHbRx = 0, gLinkHbTx = 0;
static UInt32 gLinkEvtLost = 0, gLinkFlushHeld = 0;
#define AP_LINK_HB_UP_TICKS    120u   /* a status line every 30 s while up ... */
#define AP_LINK_HB_DOWN_TICKS   40u   /* ... every 10 s while down */
#define AP_LINK_FLUSH_TICKS     40u   /* new lines reach the disk within 10 s, at once after an event */

/* ★★ k205: THE STATUS BLOCK's task-level half (ap_status.h). Published once, from ApLinkStart; the
 * identity -- state, network name, base station ID, channel -- rewritten only here, under seq. */
static UInt32 gStatPrevState = kApLinkNoDriver;

static void ApStatPublish(void)
{
    extern UInt32 gApOtmodlBuild;              /* AP_OTMODL_BUILD, ap_otmodl.c */
    long v = 0;
    if(!gApStat){
        /* A reload of the module in the same boot finds its own block and reuses it -- never leaks one,
         * and a reader holding the old address keeps reading live memory. */
        if(Gestalt((OSType)kApStatSelector, &v) == noErr && v && ApStatValid((const ApStatBlock *)v))
            gApStat = (ApStatBlock *)v;
    }
    if(!gApStat){
        ApStatBlock *b = (ApStatBlock *)NewPtrSysClear((Size)sizeof(ApStatBlock));
        if(!b){ Say("  [status] no system-heap memory for the status block -- the UI will show no driver"); return; }
        b->magic = kApStatMagic; b->version = kApStatVersion; b->size = (ApStU32)sizeof(ApStatBlock);
        if(NewGestaltValue((OSType)kApStatSelector, (long)b) != noErr &&
           ReplaceGestaltValue((OSType)kApStatSelector, (long)b) != noErr){
            DisposePtr((Ptr)b);
            Say("  [status] Gestalt refused the status block -- the UI will show no driver");
            return; }
        gApStat = b;
    }
    gApStat->driverBuild = gApOtmodlBuild;
    gApStat->alive = 1;
    SayH("  [status] k205 status block (Gestalt 'APXe') at ", (unsigned long)gApStat, 8);
}

/* ★★★ k209: THE NETWORK LIST BLOCK (ap_scanlist.h), published exactly as the status block is -- system
 * heap, Gestalt 'APXs', found and reused across a reload, never freed. The table the scan fills lives here
 * in the fragment (it holds names and addresses of networks around the machine, private data); only the
 * published block, which the UIs read, leaves it, and the driver log prints counts alone. */
static ApScanBlock  *gApScan = 0;              /* set once at task level, before any scan runs */
static ApScanTable   gScanTab;                 /* task level only: built by a scan, then published */
static volatile int  gScanning = 0;            /* 1 while a scan is off the home channel (the link watch reads it) */
static UInt32 gScanRuns = 0, gScanHeard = 0, gScanPublished = 0, gScanFail = 0;
static UInt32 gScanLastMs = 0, gScanFoundOurs = 0, gScanChanAdopted = 0;

static void ApScanPublish2(void)
{
    long v = 0;
    if(!gApScan){
        if(Gestalt((OSType)kApScanSelector, &v) == noErr && v && ApScanValid((const ApScanBlock *)v))
            gApScan = (ApScanBlock *)v;
    }
    if(!gApScan){
        ApScanBlock *b = (ApScanBlock *)NewPtrSysClear((Size)sizeof(ApScanBlock));
        if(!b){ Say("  [scan] no system-heap memory for the network-list block"); return; }
        b->magic = kApScanMagic; b->version = kApScanVersion; b->size = (ApStU32)sizeof(ApScanBlock);
        if(NewGestaltValue((OSType)kApScanSelector, (long)b) != noErr &&
           ReplaceGestaltValue((OSType)kApScanSelector, (long)b) != noErr){
            DisposePtr((Ptr)b);
            Say("  [scan] Gestalt refused the network-list block");
            return; }
        gApScan = b;
    }
    SayH("  [scan] k209 network-list block (Gestalt 'APXs') at ", (unsigned long)gApScan, 8);
}

/* One beacon from an RX-ring buffer into the scan table. The buffer is wholly ours (the ring buffers are a
 * page each); every read is bounded by the frame length the header reports, and ApScanParse bounds the
 * rest. Listen-only: this looks at the ring, it never transmits. */
static void ApScanTakeBeacon(const UInt8 *buf, UInt8 heardOn)
{
    ApScanItem it;
    UInt32 raw = (UInt32)le16at(buf + RXH_FRAME_LEN);
    UInt32 flen;
    if(raw <= K6_HDR_PLCP6 + 4UL) return;                    /* too short to hold a frame + FCS */
    flen = raw - K6_HDR_PLCP6 - 4UL;                         /* strip PLCP and FCS, as ApRxToEnet does */
    if(!ApScanParse(buf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6, flen, heardOn, &it)) return;
    it.signalDbm = ApRssiDbm(buf[RXH_JSSI], le16at(buf + RXH_PHY_STATUS0), le16at(buf + RXH_PHY_STATUS3),
                             (const short *)gNrssiLt);
    it.quality   = (ApStU8)ApStatQuality(it.signalDbm, 5);   /* prev > 4: the plain mapping, no hysteresis */
    ApScanAdd(&gScanTab, &it);
    gScanHeard++;
}

/* Tune to a channel and listen for dwellMs, walking the RX ring for beacons and probe responses of EVERY
 * network (no address filter). The receive interrupt MUST be quiesced by the caller, so this is the ring's
 * only reader and nothing decrypts a foreign frame. Reuses the join's ring walk: poison a slot once read,
 * so the free-running engine's lap cannot re-count it. Returns beacons taken. */
static UInt32 ApScanListenChannel(UInt8 chan, UInt32 dwellMs)
{
    UInt32 waited = 0, took = 0;
    SwitchChannel(chan);                                     /* b43_switch_channel: SHM cookie + PHY + 8 ms settle */
    while(waited < dwellMs){
        int slot;
        for(slot = 0; slot < K3_RX_SLOTS; slot++){
            UInt8 *buf = gShimRxBufLog[slot];
            if(RxBufferIsPoisoned(buf, B43_DMA0_RX_FW351_FO)) continue;
            { UInt16 fc = le16at(buf + B43_DMA0_RX_FW351_FO + K6_HDR_PLCP6);
              if(((fc >> 2) & 3u) == 0u){ ApScanTakeBeacon(buf, chan); took++; } }
            PoisonRxBuffer(buf, B43_DMA0_RX_FW351_FO);        /* consumed: hand it back to the engine */
        }
        /* Keep the engine's free window at the whole ring, exactly as RxWaitFor does -- a scan hears a lot
         * of beacons, and without this the free-running engine fills the ring and stalls. */
        DmaPublish();
        ssb_w32(gBus.bar0, gShimDmaBase + B43_DMA32_RXINDEX, (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES);
        SsbSpinUs(10000); waited += 10UL;                    /* 10 ms between sweeps; beacons arrive ~100 ms */
    }
    return took;
}

/* The channels a 2.4 GHz scan listens on. 1-11 are usable in every regulatory domain; 12-13 are left out
 * rather than guessed (they are lawful to LISTEN on, but this driver has no country information to know it,
 * and the omission only hides two channels almost nothing in North America uses). */
#define AP_SCAN_CHAN_LO   1u
#define AP_SCAN_CHAN_HI  11u
#define AP_SCAN_DWELL_MS 120UL          /* per channel: a beacon interval is ~100 ms, so one is almost sure */

/* Listen on every channel, building gScanTab. The caller has quiesced the receive interrupt (or it is not
 * armed yet) and will put the home channel back. Does NOT transmit. Returns the table's network count. */
static UInt32 ApScanSweep(void)
{
    UInt32 ch, i;
    unsigned long t0 = ApTbNow();
    /* k216: MERGE this sweep into the rolling list instead of wiping it (Apple's WirelessScanMerge). A network
     * briefly missed stays put; ApScanMergeEnd drops only ones unheard for kApScanAgeSweeps sweeps. This is the
     * real fix for "a network vanished from the list between sweeps" -- the old ApScanReset rebuilt from zero. */
    ApScanMergeBegin(&gScanTab);
    for(ch = AP_SCAN_CHAN_LO; ch <= AP_SCAN_CHAN_HI; ch++) (void)ApScanListenChannel((UInt8)ch, AP_SCAN_DWELL_MS);
    ApScanMergeEnd(&gScanTab, kApScanAgeSweeps);
    gScanLastMs = (UInt32)(ApTbToUs(ApTbNow() - t0) / 1000UL);
    ApScanSort(&gScanTab);
    /* Flag the entry we are joined to, so a UI shows it as the current one. ⚠ k216: clear it across the WHOLE
     * merged list first -- with merge, a network we left would otherwise keep a stale "ours" flag from the sweep
     * when it WAS the target (the k215 panel bug's root, now impossible by construction, not just ignored). */
    for(i = 0; i < gScanTab.n; i++) gScanTab.e[i].flags &= (ApStU8)~kApScanFOurs;
    if(gJoined)
        for(i = 0; i < gScanTab.n; i++)
            if(MacEq(gScanTab.e[i].bssid, gShimTgtBssid)) gScanTab.e[i].flags |= kApScanFOurs;
    return gScanTab.n;
}

/* k209: CHANNEL AGILITY. After a sweep, look in the table for OUR network by SSID; if it is there, adopt
 * its channel and BSSID so the join aims at it wherever it actually is. This is what frees the driver from
 * the compiled-in channel: the home channel is tried first (cheap), and only a miss pays for a sweep.
 * Returns 1 if our network was found and adopted. */
static int ApScanAdoptOurs(void)
{
    UInt32 i;
    for(i = 0; i < gScanTab.n; i++){
        ApScanItem *e = &gScanTab.e[i];
        int k, same;
        if(e->ssidLen != gTargetLen) continue;
        for(k = 0, same = 1; k < (int)gTargetLen; k++)
            if(e->ssid[k] != gTargetSsid[k]){ same = 0; break; }
        if(!same) continue;
        gScanFoundOurs++;
        if(e->channel >= 1 && e->channel <= 14 && e->channel != gShimChannel){
            gShimChannel = e->channel; gScanChanAdopted++; }
        for(k = 0; k < 6; k++) gShimTgtBssid[k] = e->bssid[k];
        gShimHaveBssid = 1;
        return 1;
    }
    return 0;
}

/* ★★★ k210: THE KNOWN-NETWORKS FILE -- System Folder:Preferences:AirPort Extreme Known Networks.
 * Read only from the driver (the panel writes it); ap_known.h has the format, bt_keyfile.c the pattern.
 * TASK LEVEL (File Manager); called at boot and when a join command arrives. Every path is counted, so a
 * log can tell "no file yet" (first run) from "the file would not open" (bt_keyfile's gKfNoFile lesson). */
static Boolean ApShimKnownFileSpec(FSSpec *spec)
{
    short vRefNum; long dirID;
    if(FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr)
        return false;
    { OSErr e = FSMakeFSSpec(vRefNum, dirID, "\pAirPort Extreme Known Networks", spec);
      if(e != noErr && e != fnfErr) return false;
      return (spec->name[0] != 0); }
}

static int ApShimLoadKnown(void)
{
    static UInt8 buf[kApKnownFileSize];        /* task level only; 1168 bytes off the stack */
    FSSpec spec; short ref; OSErr e; long n = (long)kApKnownFileSize;
    /* ⚠ FILE MANAGER: TASK LEVEL ONLY, like the snapshot writer. Every caller is task level (boot's
     * EnetHAL_Open, and the command handler, which the NM response gates on CurrentExecutionLevel()==0),
     * but the guard is kept here too so the claim holds by construction and check-exec-level.py can treat
     * this as a guarded File Manager entry (FM_GUARDED_BY). Below task level it loads nothing, safely. */
    if(CurrentExecutionLevel() != 0) return 0;
    ApKnownClear(&gKnownDb);
    gKnownLoaded = 1;
    if(!ApShimKnownFileSpec(&spec)){ gKnownLoadErr++; gKnownCount = 0; return 0; }
    e = FSpOpenDF(&spec, fsRdPerm, &ref);
    if(e != noErr){ if(e == fnfErr) gKnownNoFile++; else gKnownLoadErr++; gKnownCount = 0; return 0; }
    e = FSRead(ref, &n, buf);                  /* eofErr with a short read is fine: a smaller file */
    (void)FSClose(ref);
    if(e != noErr && e != eofErr){ gKnownLoadErr++; gKnownCount = 0; return 0; }
    { long i; for(i = n; i < (long)kApKnownFileSize; i++) buf[i] = 0; }   /* pad a short file with zeros */
    if(!ApKnownLoad(&gKnownDb, buf)){ gKnownLoadErr++; gKnownCount = 0; return 0; }
    gKnownCount = gKnownDb.count;
    return 1;
}

/* Set the runtime target from known entry k, and the channel/BSSID from scan item s (of gScanTab). */
static void ApShimSetTargetFromKnown(int k, int s)
{
    const ApKnownNet *e = &gKnownDb.net[k];
    const ApScanItem *si = &gScanTab.e[s];
    UInt32 i;
    for(i = 0; i < 32u; i++){ gTargetSsid[i] = e->ssid[i]; gTargetPmk[i] = e->pmk[i]; }
    gTargetLen = e->ssidLen;
    gTargetSec = e->security;
    gHaveTarget = 1;
    if(si->channel >= 1 && si->channel <= 14) gShimChannel = si->channel;
    for(i = 0; i < 6u; i++) gShimTgtBssid[i] = si->bssid[i];
    gShimHaveBssid = 1;                          /* the pick already found the BSSID: the join need not re-scan */
    gShimBadKeyTries = 0; gShimKeyBad = 0;       /* k213: a fresh choice gets a fresh wrong-password budget */
}

/* k212: aim at known network k by SSID only (no scan entry). The user chose a network that is not in the last
 * scan; target the SSID and let the link watch find its BSSID (ApShimPickBssid, channel agility) rather than
 * join a DIFFERENT saved network that happens to be in range (the wrong-saved-network-in-range bug). */
static void ApShimSetTargetSsidOnly(int k)
{
    const ApKnownNet *e = &gKnownDb.net[k];
    UInt32 i;
    for(i = 0; i < 32u; i++){ gTargetSsid[i] = e->ssid[i]; gTargetPmk[i] = e->pmk[i]; }
    gTargetLen = e->ssidLen;
    gTargetSec = e->security;
    gHaveTarget = 1;
    gShimHaveBssid = 0;                          /* no BSSID yet: the join searches for this SSID */
    gShimBadKeyTries = 0; gShimKeyBad = 0;       /* k213: a fresh choice gets a fresh wrong-password budget */
}

/* Boot (and command) target selection: load the file, scan, choose the best known network in range. Returns
 * 1 and sets the target, or 0 (nothing known is reachable). Task level; the receive interrupt is not armed
 * at boot, so ApScanSweep owns the ring. */
static int ApShimBootTarget(void)
{
    int k = -1, s = -1, sel;
    UInt32 win = 0; int haveWin;
    (void)ApShimLoadKnown();
    if(gKnownDb.count == 0){ Say("  [k210] no known networks in Preferences yet -- nothing to auto-join."); return 0; }
    /* The sweep needs the receiver live and the 802.11 core selected, and it leaves us on the last channel
     * swept -- so the core stays selected through the pick and the SwitchChannel back to the target's
     * channel, and only then is the BAR0 window put back (as every entry point does). This runs before the
     * receive interrupt is armed, so the sweep owns the ring. */
    (void)ApShimEnsureRxLive();
    haveWin = (ExpMgrConfigReadLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, &win) == noErr);
    sel = (SsbSelectCore(&gBus, (UInt32)gShimIdx80211) == noErr);
    if(sel){
        (void)ApScanSweep();
        k = ApKnownPickJoinItems(&gKnownDb, gScanTab.e, gScanTab.n, &s);
        if(k >= 0 && s >= 0){ ApShimSetTargetFromKnown(k, s); SwitchChannel(gShimChannel); }
    }
    if(haveWin) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, win);
    if(!sel){ Say("  [k210] could not select the 802.11 core for the auto-join scan."); return 0; }
    if(k < 0 || s < 0){
      Say1("  [k210] known networks, but none in range. count = ", (unsigned long)gKnownDb.count);
      return 0; }
    /* ⚠ the SSID name is NOT logged (the log is a neighbourhood survey; k209's "counts only" rule). The
     * known-list index and the channel identify the choice for debugging without naming the network. */
    { Str255 L; L[0] = 0; PCat(L, "  [k210] ★ auto-join: known #"); PCatDec(L, (unsigned long)k);
      PCat(L, " of "); PCatDec(L, (unsigned long)gKnownDb.count);
      PCat(L, ", channel "); PCatDec(L, (unsigned long)gShimChannel); Out(L); }
    return 1;
}

/* Publish the table gScanSweep built, and log COUNTS only -- never the names or addresses it holds. */
static void ApScanPublishTable(void)
{
    if(!gApScan) return;
    ApScanPublish(gApScan, gScanTab.e, gScanTab.n, gScanTab.dropped, gLinkTick);
    gScanPublished++;
}

/* ★★★ k209: A FULL NETWORK-LIST SCAN WHILE JOINED -- the kApCmdScan path, at task level.
 *
 * ⚠ It leaves our channel, so: tell the base station we are dozing (a null frame, Power Management set) so
 *   it BUFFERS our traffic; gate the link-loss detector (gScanning) so a scan is not mistaken for an
 *   outage; quiesce the receive interrupt so the sweep owns the ring; put the home channel back and clear
 *   Power Management before resuming. Everything transmitted here is a null data frame -- no probe
 *   requests -- so the scan is a listener that only announces its own doze, which is the conservative form
 *   the user chose (2026-09-30).
 * ⚠ The whole sweep is ~1.2 s off the home channel. mac80211 returns home every ~125 ms; this does not,
 *   which is why the doze frame and the link-watch gate matter. A shorter, home-returning sweep is a later
 *   refinement (k209 keeps it simple and proven first). */
static void ApShimTxNull(int pm)
{
    UInt8 nf[24];
    UInt16 n = (UInt16)ApBuildNullFunc(nf, gShimMac, gShimTgtBssid, pm);
    int k;
    for(k = 0; k < (int)n; k++) gShimTxFrm[k] = nf[k];
    GenerateTxHdr351(gShimTxHdr, gShimTxFrm, n, gShimChannel, 0xC00F, B43_TXH_PHY_ANT01AUTO, 1);
    ApShimTxArm();
    DmaPublish();
    PostTxFrameAt(gShimTxBase, gShimTxRing.base, 0, gShimTxHdrPhys, (UInt16)TXH_SIZE_351, gShimTxFrmPhys, n);
    (void)DrainTxStatus(0xC00F, 50);
}

static int ApScanRunCommand(void)
{
    UInt32 savedWin = 0;
    int haveSaved, joined = (gLink.up && gJoined);
    int wasArmed = gIrqArmed;                    /* k210: the pump may be armed even when not associated */
    gScanRuns++;
    haveSaved = (ExpMgrConfigReadLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, &savedWin) == noErr);
    if(SsbSelectCore(&gBus, (UInt32)gShimIdx80211) != noErr){
        gScanFail++;
        if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, savedWin);
        Say("  [scan] [!!] could not select the 802.11 core -- scan abandoned");
        return 0; }
    gScanning = 1;
    /* Hold OT's transmit while we are off the home channel: its frames wait on the write queue (k202's
     * back-pressure) instead of going out on a scan channel, and flush on the first interrupt after we
     * resume. Restored below; joined implies the link was up, so it was 0. */
    if(joined) gLinkTxBlocked = 1;
    /* ⚠ k210: QUIESCE WHENEVER THE INTERRUPT IS ARMED, not only when joined. On a first boot with no
     * network, the pump is armed (it hears beacons promiscuously) though we are not associated -- so the
     * sweep must still own the ring, or it and the secondary handler walk it at once. The DOZE null frame,
     * on the other hand, is only meaningful when associated (there is an AP to tell). */
    if(wasArmed) ApShimIrqQuiesce();
    if(joined) ApShimTxNull(1);                              /* dozing; tell the AP to buffer our traffic */
    (void)ApScanSweep();
    SwitchChannel(gShimChannel);                             /* home again */
    if(joined){ ApShimTxNull(0); gLinkTxBlocked = 0; }       /* awake; the AP sends buffered frames */
    if(wasArmed && gIrqInstalled) ApShimIrqResume();
    gScanning = 0;
    gStatReset = 1;                                          /* the signal average restarts on our channel */
    if(joined){
        extern volatile UInt32 gOtmTxWaiting;
        gLinkLastBcnTick = gLinkTick;                        /* the off-channel time does not count as a beacon gap */
        if(gOtmNativeMode && gOtmTxWaiting) ApOtmScheduleRx();   /* flush any frames OT held, now, not on the next beacon */
    }
    ApScanPublishTable();
    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, savedWin);
    { Str255 L; L[0] = 0; PCat(L, "  [scan] ★ found ");
      PCatDec(L, (unsigned long)gScanTab.n); PCat(L, " network(s) in ");
      PCatDec(L, (unsigned long)gScanLastMs); PCat(L, " ms");
      if(gScanTab.dropped){ PCat(L, " (+"); PCatDec(L, (unsigned long)gScanTab.dropped); PCat(L, " over 32)"); }
      Out(L); }
    return 1;
}

/* Task level only: seq odd while rewriting, even when consistent (the readers' contract). Volatile, so the
 * compiler cannot move a field store outside the two seq stores that bracket it. */
static void ApStatIdentity(UInt32 state)
{
    volatile ApStatBlock *b = gApStat;
    UInt32 k, n = (gTargetLen > 32) ? 32 : (UInt32)gTargetLen;   /* k210: the runtime target's name, 0 if none */
    if(!b) return;
    b->seq = b->seq + 1u;                      /* odd: readers retry */
    b->linkState = state;
    b->linkWhy   = (ApStU32)gLink.why;
    b->channel   = (ApStU32)gShimChannel;
    for(k = 0; k < 6; k++){ b->airportId[k] = gShimMac[k]; b->bssid[k] = gShimTgtBssid[k]; }
    b->ssidLen = (ApStU8)n;
    for(k = 0; k < n; k++) b->ssid[k] = gTargetSsid[k];
    if(state == kApLinkUp && gStatPrevState != kApLinkUp) b->upSince = gLinkTick;
    b->outages = gLink.downs;
    b->rejoins = gLink.rejoins;
    b->seq = b->seq + 1u;                      /* even: consistent */
    gStatPrevState = state;
}

/* Out of Range the moment the link is down -- set AFTER gLinkUpFlag is cleared, so a beacon arriving in
 * between (ApStatBeacon checks the flag) cannot paint it back. Single words. */
static void ApStatNoSignal(void)
{
    if(!gApStat) return;
    gApStat->quality   = kApQualOutOfRange;
    gApStat->signalDbm = 0;
}

static void ApLinkCatTick(Str255 L, UInt32 tick)     /* "+123.25 s" since the heartbeat started */
{
    static const char *kQ[4] = { ".00", ".25", ".50", ".75" };
    PCat(L, "+"); PCatDec(L, (unsigned long)(tick / 4u)); PCat(L, kQ[tick % 4u]); PCat(L, " s");
}

static int    gGkLogPrevOk = 0;         /* k208: the previous renewal the log saw -- task level only */
static UInt32 gGkLogPrevTick = 0;
static int ApLinkDrainEvents(void)
{
    UInt32 w = gLinkEvtW;
    int n = 0;
    if((UInt32)(w - gLinkEvtR) > AP_LINK_EVT_N){          /* lapped: keep the newest, count the rest */
        gLinkEvtLost += (w - gLinkEvtR) - AP_LINK_EVT_N;
        gLinkEvtR = w - AP_LINK_EVT_N; }
    while(gLinkEvtR != w){
        const ApLinkEvt *e = &gLinkEvt[gLinkEvtR % AP_LINK_EVT_N];
        Str255 L;
        L[0] = 0; PCat(L, "  [link] "); ApLinkCatTick(L, e->tick); PCat(L, "  ");
        if(e->kind == AP_LINK_EVT_KICK){
            PCat(L, (e->sub == AP_LINK_MGMT_DEAUTH) ? "the AP DEAUTHENTICATED us" : "the AP DISASSOCIATED us");
            PCat(L, ", reason "); PCatDec(L, (unsigned long)e->code);
            PCat(L, ": "); PCat(L, ApLinkReasonName(e->code));
        } else if(e->kind == AP_LINK_EVT_GTK){          /* k208 */
            unsigned kid = e->code & 7u, fate = (e->code >> 4) & 3u;
            if(e->sub == AP_GK_OK){
                PCat(L, "★ GROUP KEY RENEWAL -- key index "); PCatDec(L, (unsigned long)kid);
                PCat(L, (e->code & 0x100u) ? ": the same key again (the AP resent), not reinstalled"
                                           : " installed");
                PCat(L, fate == AP_GK_M2_QUEUED ? "; message 2 queued" : "; message 2 could NOT be built");
                /* The AP resends message 1 only when our message 2 did not arrive or did not verify. */
                if(gGkLogPrevOk && ApLinkAge(e->tick, gGkLogPrevTick) <= 12u)
                    PCat(L, "  ⚠ within 3 s of the last: the AP did not accept that message 2");
                gGkLogPrevOk = 1; gGkLogPrevTick = e->tick;
            } else {
                PCat(L, "group key message 1 REFUSED: "); PCat(L, ApGkResultName(e->sub));
            }
        } else if(e->kind == AP_LINK_EVT_GTK_M2){       /* k208 */
            PCat(L, e->sub == AP_GK_M2_SENT ? "group message 2 sent, encrypted (attempt "
                  : e->sub == AP_GK_M2_FAILED ? "group message 2 FAILED to send (attempt "
                                              : "group message 2 ABANDONED, held too long (attempts ");
            PCatDec(L, (unsigned long)e->code); PCat(L, ")");
        } else {
            PCat(L, "EAPOL from the AP: "); PCat(L, ApLinkEapolName(e->sub));
            PCat(L, "  (key info 0x"); PCatHex(L, (unsigned long)e->code, 4); PCat(L, ")");
        }
        Out(L);
        gLinkEvtR++; n++;
    }
    return n;
}

static void ApLinkSayDown(void)
{
    Str255 L;
    Say("");
    L[0] = 0; PCat(L, "  [link] ★★ LINK LOST "); ApLinkCatTick(L, gLink.downTick);
    PCat(L, ": "); PCat(L, ApLinkWhyName(gLink.why));
    if(gLink.why == AP_LINK_WHY_DEAUTH || gLink.why == AP_LINK_WHY_DISASSOC){
        PCat(L, " (reason "); PCatDec(L, (unsigned long)gLink.reason); PCat(L, ")"); }
    Out(L);
    Say("         transmit held -- OT's frames wait on the write queue; rejoin once the AP is heard");
}

/* One attempt. The same ApShimJoinOnce the boot runs (JoinReset first, exactly as between boot attempts),
 * with the receive interrupt quiesced because the join polls the ring itself -- the boot join runs before
 * the first arm for the same reason. Resumed afterwards whether or not it worked: the pump's beacons are
 * how the next attempt knows the AP is there. PBKDF2 is cached (gShimPmkReady) and TxProbe runs once per
 * load, so an attempt costs the scan + auth + assoc + handshake: ~0.1-0.3 s with the AP present. */
/* k213: the link state to publish when a join attempt did NOT bring the link up -- kApLinkBadKey once the
 * wrong-password latch is set (ap_shim.c globals), kApLinkSearching otherwise. Used by every post-attempt
 * ApStatIdentity so the UI says "incorrect password" instead of spinning on "Looking for ..." forever. */
static UInt32 ApLinkDownIdentity(void)
{
    return gShimKeyBad ? (UInt32)kApLinkBadKey : (UInt32)kApLinkSearching;
}

static int ApLinkRejoinOnce(void)
{
    unsigned long tb0 = ApTbNow();
    int ok;
    Str255 L;
    Say("");
    L[0] = 0; PCat(L, "  [link] ★★ REJOIN attempt "); PCatDec(L, (unsigned long)gLink.tries);
    PCat(L, " at "); ApLinkCatTick(L, gLinkTick);
    PCat(L, " (down "); PCatDec(L, (unsigned long)(ApLinkAge(gLinkTick, gLink.downTick) / 4u));
    PCat(L, " s; "); PCat(L, ApLinkWhyName(gLink.why)); PCat(L, ")");
    Out(L);
    gLinkTxBlocked = 1;                  /* the join owns the transmitter until this returns */
    ApStatIdentity(kApLinkJoining);      /* k205: the UI can say so for the ~0.1-0.7 s it takes */
    ApShimIrqQuiesce();
    ApShimJoinReset();
    gJoined = 0;
    gJoinTries++;
    ok = ApShimJoinOnce();
    /* k213: classify a failed attempt. assoc OK + our message 2 sent + no message 3 back = the AP rejected our
     * key (a wrong passphrase); count those in a row. A failure that never reached message 2 is range/contact,
     * not the key, so it resets the count. At AP_BADKEY_TRIES, latch it: the panel will remove the saved network
     * and ask again, and ApLinkDownIdentity now publishes kApLinkBadKey. Success proves the key -> clear. */
    if(ok)                             { gShimBadKeyTries = 0; gShimKeyBad = 0; }
    else if(gShimM2Sent && !gShimM3)   { if(++gShimBadKeyTries >= AP_BADKEY_TRIES) gShimKeyBad = 1; }
    else                               { gShimBadKeyTries = 0; }
    if(ok) gJoined = 1;
    if(gIrqInstalled) ApShimIrqResume();
    else if(ok) (void)AirPortShimArmIrq();   /* the boot never managed to install it: do it now */
    gLinkRejoinLastUs = ApTbToUs(ApTbNow() - tb0);
    if(gLinkRejoinLastUs > gLinkRejoinMaxUs) gLinkRejoinMaxUs = gLinkRejoinLastUs;
    if(ok){
        extern volatile UInt32 gOtmTxWaiting;
        gStatReset = 1;                  /* k205: the signal average starts afresh on the new association */
        gLinkLastBcnTick = gLinkTick; gLinkBcnSeen = 1;   /* the join just heard the AP */
        gLinkTxBlocked = 0;
        if(gOtmNativeMode && gOtmTxWaiting) ApOtmScheduleRx();   /* wake the held write queues */
        /* k215: if this landed on a DIFFERENT network than the one OT last knew, tell OT the network changed so
         * it renews DHCP for it -- without this, a switch kept the first network's IP and the second had no LAN.
         * The boot association is covered by ApOtmAnnounceOnline; a same-network recovery does not re-push. */
        if(gOtmNativeMode){
            UInt32 z, tl = (gTargetLen > 32) ? 32 : gTargetLen;
            int same = (gOtmLastNetLen == tl);
            if(same) for(z = 0; z < tl; z++) if(gOtmLastNetSsid[z] != gTargetSsid[z]){ same = 0; break; }
            if(!same){
                extern void ApOtmNotifyNetworkChange(void);
                ApOtmNotifyNetworkChange();
                gOtmLastNetLen = tl;
                for(z = 0; z < tl; z++) gOtmLastNetSsid[z] = gTargetSsid[z];
                Say("  [link] k215: joined a different network -> told OT (it renews DHCP for the new one)");
            }
        }
    }
    Say1(ok ? "  [link] ★★★ REJOINED -- transmit released. attempt took ms = "
            : "  [link] rejoin attempt did not complete. took ms = ", (unsigned long)(gLinkRejoinLastUs / 1000u));
    return ok;
}

/* ★★★ k207: AIRPORT OFF AND ON -- the control panel's "Turn AirPort Off" and the strip module's menu item,
 * through the status block's command mailbox (ap_status.h). Task level: the body below runs it.
 *
 * PRIOR ART, read for this:
 *   b43_software_rfkill (phy_common.c)  b43_mac_suspend; phy->ops->software_rfkill; b43_mac_enable
 *   b43_gphy_op_software_rfkill (phy_g.c)
 *     off: save RFOVER and RFOVERVAL, then force the radio off through them: RFOVER | 0x008C,
 *          RFOVERVAL & 0xFF73 (the "radio_off_context")
 *     on:  the PGACTL strobe 0x8000 / 0xCC00 / 0x00C0 (gmode), RESTORE the saved override pair, then
 *          switch to channel 6 with the synth power-up workaround and back to the channel
 *   mac80211, taking an interface down while associated: a deauthentication, reason 3 ("leaving") --
 *     BuildDeauth's AP_DEAUTH_REASON_LEAVING already, the same frame the join sends first.
 * ⚠ The bring-up's RadioOn (ap_bringup.h) omits the restore on purpose -- its header says nothing has run
 *   the OFF arm at first init. Now something does, so the runtime ON arm below carries the restore, and the
 *   bring-up's copy stays as it was proven.
 * ⚠ ORDER, off: stop transmit and the receive interrupt, say goodbye while the radio can still speak, THEN
 *   the radio. On: the radio, the receive interrupt (its beacons are how ApLinkStep knows the AP is back),
 *   then the link layer rejoins by itself on the next beacon. */
static UInt16 gRadioOffRfover = 0, gRadioOffRfoverval = 0;
static int    gRadioOffValid  = 0;
static UInt32 gPowerOffs = 0, gPowerOns = 0, gPowerMacStuck = 0, gPowerByeAcked = 0, gPowerSelFails = 0;
static UInt32 gCmdTaken = 0, gCmdUnknown = 0, gCmdLast = 0, gCmdLastArg = 0, gCmdLastResult = 0;
static UInt32 gRfoverOn = 0, gRfoverOff = 0;    /* the override pair as read back after each arm */

static void ApShimRadioSoftOff(void)             /* b43_gphy_op_software_rfkill, blocked = true */
{
    UInt16 rfover = PhyRead((UInt16)B43_PHY_RFOVER), rfoverval = PhyRead((UInt16)B43_PHY_RFOVERVAL);
    gRadioOffRfover = rfover; gRadioOffRfoverval = rfoverval; gRadioOffValid = 1;
    PhyWrite((UInt16)B43_PHY_RFOVER, (UInt16)(rfover | 0x008Cu));
    PhyWrite((UInt16)B43_PHY_RFOVERVAL, (UInt16)(rfoverval & 0xFF73u));
    gRfoverOff = ((UInt32)PhyRead((UInt16)B43_PHY_RFOVER) << 16) | PhyRead((UInt16)B43_PHY_RFOVERVAL);
}

static void ApShimRadioSoftOn(void)              /* b43_gphy_op_software_rfkill, blocked = false */
{
    PhyWrite((UInt16)B43_PHY_PGACTL, 0x8000);
    PhyWrite((UInt16)B43_PHY_PGACTL, 0xCC00);
    PhyWrite((UInt16)B43_PHY_PGACTL, 0x00C0);   /* gmode is true for this card */
    if(gRadioOffValid){
        PhyWrite((UInt16)B43_PHY_RFOVER, gRadioOffRfover);
        PhyWrite((UInt16)B43_PHY_RFOVERVAL, gRadioOffRfoverval);
        gRadioOffValid = 0; }
    GphyChannelSwitch(6, 1);
    GphyChannelSwitch(gShimChannel, 0);
    gRfoverOn = ((UInt32)PhyRead((UInt16)B43_PHY_RFOVER) << 16) | PhyRead((UInt16)B43_PHY_RFOVERVAL);
}

/* The goodbye: the join's own deauth (reason 3, "leaving"), sent the join's way -- one buffer, the engine
 * armed for it -- which is why the caller has quiesced the receive interrupt first, exactly as the rejoin
 * does. The next join sets the data path's ring up afresh. 1 = the AP ACKed it (informative only: a deauth
 * needs no answer, and an AP that already forgot us simply drops it). */
static int ApShimTxGoodbye(void)
{
    UInt16 dlen = BuildDeauth(gShimTxFrm, gShimMac, gShimTgtBssid);
    GenerateTxHdr351(gShimTxHdr, gShimTxFrm, dlen, gShimChannel, 0xC00E, B43_TXH_PHY_ANT01AUTO, 1);
    ApShimTxArm();
    DmaPublish();
    PostTxFrameAt(gShimTxBase, gShimTxRing.base, 0, gShimTxHdrPhys, (UInt16)TXH_SIZE_351, gShimTxFrmPhys, dlen);
    return DrainTxStatus(0xC00E, 50);
}

/* The user's switch. 1 = in the requested state now (including "already was"); 0 = the 802.11 core could
 * not be selected, and nothing was changed. */
static void ApLinkMaybeFlush(int event);
static int ApLinkSetPower(int on)
{
    UInt32 savedWin = 0;
    int    haveSaved, bye = 0, macOk;
    Str255 L;
    if(on == gRadioPowerOn) return 1;
    haveSaved = (ExpMgrConfigReadLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, &savedWin) == noErr);
    if(SsbSelectCore(&gBus, (UInt32)gShimIdx80211) != noErr){
        gPowerSelFails++;
        if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, savedWin);
        Say("  [power] [!!] could not select the 802.11 core -- AirPort left as it was");
        return 0; }
    if(!on){
        int wasUp = gLink.up;
        ApLinkMaybeFlush(1);                     /* the log so far reaches the disk while the link is still up */
        gLinkTxBlocked = 1; gLinkUpFlag = 0;     /* OT's frames wait on the write queue, as in an outage */
        ApStatNoSignal();                        /* after the flag, so no beacon can repaint it */
        ApShimIrqQuiesce();
        if(wasUp){ bye = ApShimTxGoodbye(); if(bye) gPowerByeAcked++; }
        macOk = MacSuspend();
        ApShimRadioSoftOff();
        MacEnable();
        gRadioPowerOn = 0; gPowerOffs++; gJoined = 0;
        ApLinkTurnOff(&gLink, gLinkTick);
        ApStatIdentity(kApLinkOff);
        Say("");
        L[0] = 0; PCat(L, "  [power] ★★ AirPort turned OFF at "); ApLinkCatTick(L, gLinkTick);
        PCat(L, wasUp ? (bye ? " -- said goodbye (deauth, reason 3), the AP ACKed it"
                             : " -- said goodbye (deauth, reason 3), no ACK")
                      : " -- was not joined, so no goodbye");
        Out(L);
    } else {
        macOk = MacSuspend();
        ApShimRadioSoftOn();
        MacEnable();
        gRadioPowerOn = 1; gPowerOns++;
        ApLinkTurnOn(&gLink, gLinkTick);
        ApStatIdentity(gShimKeyBad ? (UInt32)kApLinkBadKey
                                   : (gHaveTarget ? kApLinkSearching : kApLinkIdle));   /* k211; k213: a power cycle does not fix a wrong key */
        if(gIrqInstalled) ApShimIrqResume();     /* the pump hears the AP again: that is the rejoin's cue */
        Say("");
        L[0] = 0; PCat(L, "  [power] ★★ AirPort turned ON at "); ApLinkCatTick(L, gLinkTick);
        PCat(L, gHaveTarget ? " -- rejoining on the next beacon from our AP"
                            : " -- no network chosen yet; waiting for the panel to pick one");
        Out(L);
    }
    if(!macOk){ gPowerMacStuck++; Say("  [power] ⚠ the MAC did not report suspended (b43 carries on too)"); }
    SayH("  [power] RFOVER / RFOVERVAL now = ", (unsigned long)(on ? gRfoverOn : gRfoverOff), 8);
    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, savedWin);
    return 1;
}

/* ★★★ k210: JOIN THE NETWORK THE PANEL CHOSE. The panel has already written the choice to the Preferences
 * file (its SSID, security, and the PMK it derived) and marked it most-recently-used, then posted this. So:
 * re-read the file, pick the best known network in range from the CURRENT scan (the panel refreshes the
 * scan when its menu opens, and useOrder makes the just-chosen one win), point the runtime target at it, and
 * rejoin. gStatCounters aside, this reuses the k204 rejoin machinery -- quiesce, JoinReset, JoinOnce, resume.
 * Returns 1 if a target was found and the join ran (joined or not), 0 if nothing to join. */
static UInt32 gJoinCmds = 0, gJoinCmdNoPick = 0, gJoinCmdOk = 0;
static int ApLinkJoinCommand(void)
{
    int k, s;
    gJoinCmds++;
    (void)ApShimLoadKnown();                  /* the panel just wrote it */
    if(gKnownDb.count == 0){ gJoinCmdNoPick++; Say("  [k210] join command, but no known networks on file."); return 0; }
    /* ⚠ k212: join the network the USER chose, not the strongest one in range. The panel marked the chosen
     * network most-recently-used; take THAT (ApKnownMostRecentJoinable). The old code re-ran strongest-in-scan
     * and, when the chosen SSID was momentarily out of the last scan (a multi-SSID AP drops in and out), joined
     * a DIFFERENT saved network -- the "wrong saved network in range" report. If the chosen SSID is not
     * in the last scan, aim by SSID and let the link watch find it (channel agility), rather than substitute. */
    k = ApKnownMostRecentJoinable(&gKnownDb);
    if(k < 0){ gJoinCmdNoPick++; Say("  [k212] join command, but no joinable known network on file."); return 0; }
    s = ApKnownScanIndexFor(&gKnownDb, k, gScanTab.e, gScanTab.n);
    if(s >= 0) ApShimSetTargetFromKnown(k, s);        /* in range now: use its channel and BSSID */
    else       ApShimSetTargetSsidOnly(k);            /* not in the last scan: aim by SSID, the link watch finds it */
    Say1("  [k212] ★ join command (the user's choice) -> known #", (unsigned long)k);
    Say1("        that SSID was in the last scan (1=yes) = ", (unsigned long)(s >= 0 ? 1u : 0u));
    /* Tune to the chosen network's channel (the core selected for the MMIO write) before the rejoin, so its
     * ring walk sees that network's beacons -- only when the scan gave us one; otherwise the link watch's
     * channel agility finds it. JoinReset inside the rejoin re-learns the BSSID on this channel. */
    if(s >= 0){ UInt32 win = 0; int haveWin = (ExpMgrConfigReadLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, &win) == noErr);
      if(SsbSelectCore(&gBus, (UInt32)gShimIdx80211) == noErr) SwitchChannel(gShimChannel);
      if(haveWin) (void)ExpMgrConfigWriteLong(&gBus.node, (LogicalAddress)SSB_BAR0_WIN, win); }
    gJoined = 0;
    gLink.up = 0;                             /* force the rejoin path to actually run */
    (void)ApLinkRejoinOnce();
    ApLinkAttempted(&gLink, gLinkTick, gJoined);
    gLinkUpFlag = gLink.up;
    ApStatIdentity(gLink.up ? kApLinkUp : ApLinkDownIdentity());   /* k213: kApLinkBadKey if the key was rejected */
    if(gJoined) gJoinCmdOk++;
    return 1;
}

/* The mailbox: the newest command, carried out and acknowledged. Returns 1 if there was one (an event). */
static int ApLinkCommands(void)
{
    ApStU32 seq = 0, cmd = 0, arg = 0, res;
    Str255 L;
    if(!gApStat || !ApStatTake(gApStat, &seq, &cmd, &arg)) return 0;
    gCmdTaken++; gCmdLast = cmd; gCmdLastArg = arg;
    switch(cmd){
    case kApCmdPower:
        res = ApLinkSetPower(arg != 0) ? kApAckDone : kApAckFailed;
        break;
    case kApCmdScan:                          /* k209: rebuild the network list, then resume normal service */
        res = (gRadioPowerOn && ApScanRunCommand()) ? kApAckDone : kApAckFailed;
        break;
    case kApCmdJoin:                          /* k210: join the network the panel chose (PMK from the file) */
        res = (gRadioPowerOn && ApLinkJoinCommand() && gJoined) ? kApAckDone : kApAckFailed;
        break;
    default:
        gCmdUnknown++;
        res = kApAckUnknown;
        break;
    }
    gCmdLastResult = res;
    ApStatAck(gApStat, seq, res);
    L[0] = 0; PCat(L, "  [cmd] #"); PCatDec(L, (unsigned long)seq); PCat(L, " ");
    PCat(L, (cmd == kApCmdPower) ? (arg ? "AirPort on" : "AirPort off")
          : (cmd == kApCmdScan) ? "scan" : (cmd == kApCmdJoin) ? "join" : "(unknown command)");
    PCat(L, (res == kApAckDone) ? ": done" : (res == kApAckUnknown) ? ": unknown, refused" : ": FAILED");
    Out(L);
    return 1;
}

static void ApLinkHeartbeat(UInt32 now)
{
    UInt32 bcn = gLinkBcnCount, rx = gDlpiWhy[AP_ENET_OK], tx = gTxrPosts;
    Str255 L;
    if(ApLinkAge(now, gLinkLastHbTick) < ((gLink.up || gLink.off) ? AP_LINK_HB_UP_TICKS : AP_LINK_HB_DOWN_TICKS))
        return;                              /* k207: off is not an outage -- the slow up cadence */
    L[0] = 0; PCat(L, "  [link] "); ApLinkCatTick(L, now);
    PCat(L, gLink.off ? "  OFF" : gLink.up ? "  UP" : "  DOWN");
    PCat(L, "  bcn "); PCatDec(L, (unsigned long)(bcn - gLinkHbBcn));
    PCat(L, " (last "); PCatDec(L, (unsigned long)(ApLinkAge(now, gLinkLastBcnTick) * AP_LINK_TICK_MS));
    PCat(L, " ms ago)  rx "); PCatDec(L, (unsigned long)(rx - gLinkHbRx));
    PCat(L, "  tx "); PCatDec(L, (unsigned long)(tx - gLinkHbTx));
    PCat(L, "  lost "); PCatDec(L, (unsigned long)gLink.downs);
    PCat(L, " tries "); PCatDec(L, (unsigned long)gLink.tries);
    PCat(L, " rejoins "); PCatDec(L, (unsigned long)gLink.rejoins);
    PCat(L, "  kicks "); PCatDec(L, (unsigned long)(gLinkKicks[0] + gLinkKicks[1]));
    PCat(L, "  rekeys g"); PCatDec(L, (unsigned long)gLinkEapolN[AP_LINK_EAPOL_GROUP_M1]);
    PCat(L, " (ok "); PCatDec(L, (unsigned long)gGkOk); PCat(L, ")");   /* k208: answered */
    PCat(L, "/p"); PCatDec(L, (unsigned long)gLinkEapolN[AP_LINK_EAPOL_PAIR_M1]);
    PCat(L, "  nm "); PCatDec(L, (unsigned long)gLinkNmFired);
    PCat(L, " lat<="); PCatDec(L, (unsigned long)(gLinkLatMaxUs / 1000u));
    PCat(L, "ms body<="); PCatDec(L, (unsigned long)(gLinkBodyMaxUs / 1000u));
    PCat(L, "ms  ring "); PCatDec(L, (unsigned long)(ApRingCount() * 100u / AP_RING_LINES)); PCat(L, "%");
    Out(L);
    gLinkHbBcn = bcn; gLinkHbRx = rx; gLinkHbTx = tx; gLinkLastHbTick = now;
}

static void ApLinkMaybeFlush(int event)
{
    if(!ApShimLogHasNew()) return;
    if(gLinkTxBlocked){ gLinkFlushHeld++; return; }        /* see the block above: never behind AppleShare */
    if(!event && ApLinkAge(gLinkTick, gLinkLastFlushTick) < AP_LINK_FLUSH_TICKS) return;
    ApShimDumpRingToFile();
    gLinkLastFlushTick = gLinkTick;
}

/* k209: one automatic scan a few seconds after the link first comes up, so the network list is ready the
 * moment a UI asks for it, and so a hardware run PROVES the scan from the log without a UI to trigger it.
 * gScanBootTick is the heartbeat when the link came up; the scan runs once, ~5 s later, when the link is
 * settled and idle. After that, scans are the UI's to request (kApCmdScan). */
static UInt32 gScanBootTick = 0;
static int    gScanBootDone = 0;

static void ApLinkTaskBody(void)
{
    ApLinkFacts f;
    int act, event = ApLinkDrainEvents();
    if(ApLinkCommands()) event = 1;          /* k207: the UI's command first -- the step below sees its effect */
    f.now        = gLinkTick;
    f.lastBeacon = gLinkLastBcnTick;
    f.beaconSeen = gLinkBcnSeen;
    f.kickSeq    = gLinkKickSeq;
    f.kickKind   = gLinkKickKind;
    f.kickReason = (unsigned short)gLinkKickReason;
    f.monitoring = gIrqArmed;
    act = ApLinkStep(&gLink, &f);
    if(act == AP_LINK_ACT_WENT_DOWN){
        gLinkTxBlocked = 1; gLinkUpFlag = 0;
        ApStatNoSignal();                 /* k205: after the flag, so no beacon can repaint it */
        ApStatIdentity(kApLinkSearching);
        ApLinkSayDown();
        event = 1;
    } else if(act == AP_LINK_ACT_TRY){
        /* k210: only rejoin if we actually have a network to rejoin to. Before the panel chooses one
         * (gHaveTarget = 0) there is nothing to attempt, and the join would match nothing.
         * k213: and not once the key has been proven wrong -- hold kApLinkBadKey, but stop hammering the AP with
         * a key it rejected. A new choice (ApShimSetTarget* via kApCmdJoin) clears the latch and resumes here. */
        if(gHaveTarget && !gShimKeyBad){
            int ok = ApLinkRejoinOnce();
            ApLinkAttempted(&gLink, gLinkTick, ok);
            ApStatIdentity(gLink.up ? kApLinkUp : ApLinkDownIdentity());   /* k205; k213 bad-key aware */
            gLinkUpFlag = gLink.up;
            event = 1;
        } else if(gShimKeyBad){
            ApStatIdentity((UInt32)kApLinkBadKey);   /* k213: keep saying "incorrect password", do not spin */
        }
    }
    /* k209: the one-shot boot scan. Only while genuinely up and not already scanning; ~5 s after the link
     * came up, so the join and its first traffic are done. gRadioPowerOn guards it against running with the
     * radio switched off. */
    if(!gScanBootDone && gLink.up && gRadioPowerOn){
        if(gScanBootTick == 0) gScanBootTick = gLinkTick;
        else if((UInt32)(gLinkTick - gScanBootTick) >= 20u){    /* 20 ticks = 5 s */
            gScanBootDone = 1;
            (void)ApScanRunCommand();
            event = 1;
        }
    }
    if(!gLink.up) gScanBootTick = 0;         /* an outage before the first scan: time it from the next up */
    ApLinkHeartbeat(gLinkTick);
    ApLinkMaybeFlush(event);
}

/* The Notification Manager calls this at task level, in whatever process runs its event loop. Dequeue
 * FIRST (NMRemove is task-level only, and until it runs the heartbeat must not re-post), then measure,
 * then check the level rather than trusting this comment, then refuse to nest. */
static pascal void ApLinkNmResp(NMRecPtr nm)
{
    unsigned long tb0 = ApTbNow();
    UInt32 us;
    (void)NMRemove(nm);
    gLinkNmPosted = 0;
    gLinkNmFired++;
    us = ApTbToUs(tb0 - gLinkNmArmTb);
    if(us > gLinkLatMaxUs) gLinkLatMaxUs = us;
    if(gLinkStop) return;
    if(CurrentExecutionLevel() != 0){ gLinkNotTask++; return; }
    if(gLinkInBody){ gLinkNested++; return; }
    gLinkInBody = 1;
    ApLinkTaskBody();
    gLinkInBody = 0;
    us = ApTbToUs(ApTbNow() - tb0);
    if(us > gLinkBodyMaxUs) gLinkBodyMaxUs = us;
}

/* At the end of ApOtmHalOpen (InitStreamModule: task level, boot). ⚠ EHCI h96: NewNMUPP allocates in
 * TheZone, and a boot-era zone need not outlive boot -- a descriptor left in one was recycled into a font
 * and called, the 2026-08-14 desktop-load crash. This one is called every few seconds, forever, so it is
 * forced into the system zone. The timer is armed BEFORE gLinkRunning is set, so the secondary handler's
 * revive can never start a second chain while this one is being armed. */
static void ApLinkStart(int joined)
{
    /* No MAC, no rings: a rejoin would walk receive buffers that were never allocated. (Open then returns
     * -1 anyway -- gShimMacEverOn gates NOJOIN -- so OT never opens a stream on this path.) */
    if(!gShimMacEverOn){ Say("  [link] link watch NOT started: the MAC never came up this boot"); return; }
    if(!gLinkNmUpp){
        THz z = GetZone();
        SetZone(SystemZone());
        gLinkNmUpp = NewNMUPP((NMProcPtr)ApLinkNmResp);
        SetZone(z);
    }
    gLinkStop = 0;
    ApLinkInit(&gLink, joined, gLinkTick, gLinkKickSeq);
    ApStatPublish();                      /* k205: before the flag goes up, so the first beacon has a block */
    ApScanPublish2();                     /* k209: the network-list block, so a UI finds it even before the first scan */
    gStatReset = 1;
    /* k211: with no network chosen yet (gHaveTarget = 0, an empty known-networks file) the driver is not
     * searching -- there is nothing to look for. Say so (kApLinkIdle -> the UI's "No AirPort network is
     * selected") instead of spinning forever (the k210 "scanned continually and never stopped" report).
     * joined implies a target (the boot join found one), so the ternary order is consistent. */
    ApStatIdentity(joined ? kApLinkUp : (gHaveTarget ? kApLinkSearching : kApLinkIdle));
    gLinkUpFlag = joined;
    if(joined){ gLinkLastBcnTick = gLinkTick; gLinkBcnSeen = 1; }
    gLinkTxBlocked = !joined;
    gLinkLastHbTick = gLinkTick; gLinkLastFlushTick = gLinkTick; gLinkLastPostTick = gLinkTick;
    ApLinkArmTimer();
    gLinkRunning = 1;
    Say("");
    Say("  [link] ★ k204 LINK WATCH -- notices a lost link, and joins again by itself");
    Say1("    heartbeat every ms            = ", (unsigned long)AP_LINK_TICK_MS);
    Say1("    heartbeat armed (1 = ok)      = ", (unsigned long)gLinkTimerArmed);
    SayH("    task pump UPP (system zone)   = ", (unsigned long)gLinkNmUpp, 8);
    Say(joined ? "    link UP at start"
               : "    link DOWN at start (the boot join did not complete): rejoin when the AP is heard");
}

/* In ApOtmHalClose (TerminateStreamModule: task level), BEFORE the radio goes down. */
static void ApLinkStop(void)
{
    gLinkStop = 1;                       /* first: a timer firing during this does not re-arm */
    gLinkRunning = 0;
    if(gLinkTimerId){ (void)CancelTimer(gLinkTimerId, NULL); gLinkTimerId = 0; }
    gLinkTimerArmed = 0;
    if(gLinkNmPosted){ (void)NMRemove(&gLinkNm); gLinkNmPosted = 0; }
    if(gLinkNmUpp){ DisposeNMUPP(gLinkNmUpp); gLinkNmUpp = 0; }
    gLinkTxBlocked = 0;
    gLinkUpFlag = 0;                     /* k205: then the block says so -- it outlives this module */
    ApStatNoSignal();
    ApStatIdentity(kApLinkNoDriver);
    if(gApStat) gApStat->alive = 0;
}

/* ★★★★★ 8-33: RETRY THE WHOLE BRING-UP, FROM THE PUMP, WHEN THE RADIO CAME UP DEAF.
 *
 * ⚠ AP_JOIN_ATTEMPTS STAYS AT 1, DELIBERATELY. Its comment at 8-9 is right and this does not
 *   override it: "a retry belongs in a later re-Open, OR IN A PERIODIC TASK ONCE ONE EXISTS, not
 *   in a spin the stack is waiting on." Holding OTOpenEndpoint for 25 seconds would be worse
 *   than failing. What has changed is that the periodic task now exists -- the boot vehicle's
 *   Notification Manager pump, at task level, already polling AirPortShimGetPhase.
 *
 * ★ AND RETRYING THE JOIN ALONE WOULD NOT HELP THE CASE THAT KEEPS COSTING RUNS. k121 and k124
 *   both failed with ZERO frames of any type walked -- MACCTL read ENABLED|AWAKE|BEACPROMISC,
 *   the DMA engine read ACTIVE, and nothing arrived. ApShimJoinReset only clears auth/assoc
 *   state; a receiver that is not receiving needs the core reset, the firmware and the PHY
 *   brought up again. So this re-runs the same sequence EnetHAL_Open runs, which is the sequence
 *   a TCP/IP re-select already exercises by hand -- and that manual re-select is what suggested
 *   automating it.
 *
 * ⛔ QUIESCE FIRST. If the interrupt is armed, a core reset underneath a live ISR is the
 *   k92 class of fault. Open arms only AFTER a successful join, so on this path there is no link
 *   to disturb -- but "there should be no interrupt" is an assumption, and disarming is one call.
 *
 * ⚠ Task level only, measured not assumed, for the same reason every other File-Manager-adjacent
 *   entry point in this file measures it. */
static void ApShimDumpRingToFile(void);   /* defined below; the ring drain, guarded */

UInt32 AirPortShimRejoin(void)
{
    if(CurrentExecutionLevel() != 0) return 0;      /* not task level: do nothing, safely */
    if(gJoined)                      return 1;      /* already up; never disturb a live link */
    if(!gShimMacEverOn)              return 0;      /* Open has never run; nothing to redo */

    gRejoinRuns++;
    Say("");
    Say1("  [8-33] ★★★ RE-BRING-UP from the pump, attempt ",(unsigned long)gRejoinRuns);

    ApShimIrqQuiesce();              /* level-safe half; see 8-8 */
    (void)AirPortShimQuiesce();      /* MAC down, engines reset, ring regs zeroed */

    ApShimFindCard();
    ApShimScanBackplane();
    ApShimEnableCore();
    ApShimLoadFirmware();
    ApShimGpioInit();
    ApShimApplyInitvals();
    ApShimRadioOn();
    ApShimPhyInit();
    ApShimChipInitTail();
    ApShimCoreInitMid();              /* k194: b43_wireless_core_init's middle, b43's position */
    ApShimDmaInit();
    ApShimCoreInitTail();
    ApShimMacOn();
    ApShimTxInit();
    ApShimJoinReset();
    ApShimJoin();

    if(gJoined && !gIrqArmed) (void)AirPortShimArmIrq();
    Say1("  [8-33] re-bring-up finished. joined = ",(unsigned long)gJoined);
    ApShimDumpRingToFile();          /* guarded; the attempt's full narration to disk */
    return (UInt32)gJoined;
}

UInt32 AirPortShimGetJoin(UInt32 *tries, UInt32 *beaconMs, UInt32 *joined, UInt32 *armed)
{
    if(tries)    *tries    = gJoinTries;
    if(beaconMs) *beaconMs = gJoinBeaconMs;
    if(joined)   *joined   = (UInt32)gJoined;
    if(armed)    *armed    = (UInt32)(gIrqArmed ? 2 : (gIrqInstalled ? 1 : 0));
    return (UInt32)gJoined;
}

/* ★★★ 8-6d: SET THE BSSID FILTER TO ZEROS OR TO THE REAL BSSID, ON DEMAND.
 *
 * ⚠⚠ THIS EXISTS SO THE CONTROL CAN GO INSIDE THE RUN, and that is not a stylistic preference --
 *   it is how this exact fault was proved on 2026-09-20 (k44). A cross-run before/after was
 *   tried first and was worthless, because the receiver has a ~30% intermittent and three
 *   hypotheses had already died on it. Comparing k87 (no filter write) against k88 (filter
 *   write) across a reboot would inherit that same 30% chance of being confounded.
 *
 *   With this, one run does both: window A with the filter at 00:00:00:00:00:00, window B with
 *   the real BSSID, same association, same minute, same air. Group data in B and none in A is
 *   the filter. Nothing in A or B is "the network was quiet", and that answer is worth having
 *   too -- it just is not a pass.
 *
 * ⚠ Safe with the MAC running: b43 main.c:4140 writes this on BSS_CHANGED_BSSID without a
 *   suspend, and its own b43_mac_suspend three lines later is for unrelated work. */
UInt32 AirPortShimSetBssidFilter(UInt32 useReal)
{
    static const UInt8 zeroBssid[6] = {0,0,0,0,0,0};
    UInt32 savedWin = 0;
    int haveSaved;

    if(!gShimMacEverOn) return 0;
    haveSaved = (ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)SSB_BAR0_WIN,
                                      &savedWin) == noErr);
    if(SsbSelectCore(&gBus,(UInt32)gShimIdx80211) != noErr){
      if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                                (LogicalAddress)SSB_BAR0_WIN,savedWin);
      return 0; }
    WriteMacBssidTemplates(gShimMac, useReal ? gShimTgtBssid : zeroBssid);
    if(haveSaved) (void)ExpMgrConfigWriteLong(&gBus.node,
                                              (LogicalAddress)SSB_BAR0_WIN,savedWin);
    return 1;
}

/* The counters, for the app to report. Nothing here touches the card. */
UInt32 AirPortShimGetDlpi(UInt32 *pumped, UInt32 *isrCalls, UInt32 *consumed,
                          UInt32 *readCalls, UInt32 *readBytes, UInt32 *readEmpty,
                          UInt32 *walked, UInt32 *why, UInt32 *startSeen)
{
    int i;
    if(pumped)    *pumped    = gDlpiPumped;
    if(isrCalls)  *isrCalls  = gDlpiIsrCalls;
    if(consumed)  *consumed  = gDlpiConsumed;
    if(readCalls) *readCalls = gDlpiReadCalls;
    if(readBytes) *readBytes = gDlpiReadBytes;
    if(readEmpty) *readEmpty = gDlpiReadEmpty;
    if(walked)    *walked    = gDlpiWalked;
    /* 0 = Start never arrived; 1 = Start arrived but OT has since Stopped us; 2 = started now.
     * Three states, three values -- "never tried" must never share a value with "tried". */
    if(startSeen) *startSeen = gShimStartSeen ? (gShimStarted ? 2UL : 1UL) : 0UL;
    if(why) for(i=0;i<AP_ENET_NREASON;i++) why[i] = gDlpiWhy[i];
    return (UInt32)(gShimIsr != NULL);
}

/* 8-2d-2b: the initvals result. Returns 1 if both tables applied in full.
 *
 * ⚠ FOUR COUNTS, NOT A BOOLEAN. "it worked" is what a return code says; applied-vs-declared is
 *   what lets the app score a PARTIAL application, which is the failure ApplyIvs actually has --
 *   it stops early and silently when a record is malformed or out of range. */
UInt32 AirPortShimGetIvs(UInt32 *applied, UInt32 *declared,
                         UInt32 *bsApplied, UInt32 *bsDeclared, SInt32 *err)
{
    if(applied)    *applied    = gShimIvApplied;
    if(declared)   *declared   = gShimIvDeclared;
    if(bsApplied)  *bsApplied  = gShimBsApplied;
    if(bsDeclared) *bsDeclared = gShimBsDeclared;
    if(err)        *err        = (SInt32)gShimIvErr;
    return (UInt32)(gShimIvErr == noErr);
}

UInt32 AirPortShimGetBuild(void)
{
    return AP_SHIM_BUILD;
}

/* ★★★★★ 8-21: DRAIN THE RING TO A FILE, BECAUSE THE APPLICATION IS WATCHING THE WRONG COPY.
 *
 * ⚠⚠ k110 PROVED THERE ARE TWO INSTANCES OF THIS FRAGMENT, and the evidence is two lines of one
 *   log disagreeing:
 *
 *       ★★★★★★ EnetHAL_Open RAN. times = 1     <- the Name Registry beacon, outside every copy
 *       selector calls our driver received = 0  <- the app's own connection to AirPortShim
 *
 *   The boot vehicle's GetSharedLibrary and the application's returned SEPARATE copies with
 *   separate globals -- [[reference_os9_two_fragment_copies_own_globals]]. EnetShimLib calls the
 *   copy the boot vehicle registered; the app has been reading one that owns no hardware and has
 *   no state. Every app-side oracle since the boot vehicle landed has been pointed at the wrong
 *   instance, and AirPortShimGetLog can never replay the ring that matters.
 *
 *   So the driver has to speak for itself. This is the eSATA/EHCI idiom exactly: an
 *   interrupt-safe ring at run time, drained to disk at task level -- ap_ring.h exists for the
 *   first half, and this is the second.
 *
 * ⛔⛔ THE FILE MANAGER BELOW TASK LEVEL HANGS MAC OS 9 WITH NO NMI, AND IT HAS BITTEN THIS
 *   PROJECT THREE TIMES. Apple documents EnetHAL_Open as task time, and a comment claiming an
 *   execution level is a claim that expires silently when callers change. So this does not trust
 *   the documentation: it MEASURES, with the same call Apple's own sample uses for the same
 *   worry (USBEnetDriver.c:537, "Caution - Installing OT driver at non-task time"). Level 0 is
 *   task level. Anything else and we write nothing at all. */
/* ★ k190: EVERY driver log goes to the System Folder, whatever program happens to be running.
 * FSMakeFSSpec(0, 0, name) means "the CURRENT program's default folder": at boot that was the
 * Extensions folder, during TCP/IP's close it was the Control Panels folder. k189 run 2 left THREE
 * "AirPort Driver Log"s (System Folder, Control Panels, Extensions) and the one holding the snapshot
 * was not the one we read. Likely also the August "Snapshot Log can't be created from the NM
 * context" mystery. Same FindFolder idiom as the gpu-fix probes (vbl_kick.c:377); falls back to the
 * old default-folder behaviour only if FindFolder itself fails. Task level only (callers are gated). */
static OSErr apLogSpec(ConstStr255Param name, FSSpec *sp)
{
    short vRef; long dirID;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRef, &dirID) == noErr)
        return FSMakeFSSpec(vRef, dirID, name, sp);
    return FSMakeFSSpec(0, 0, name, sp);
}

/* ★★ k204: APPEND, AND RECYCLE THE RING. Through k203 every call deleted the file and rewrote the whole
 * ring -- fine while dumps came at bring-up and at a stream close, but the ring FILLS AND STOPS (ap_ring.h)
 * and a session that runs for hours needs the link layer's events at its END. So: the first dump of a load
 * creates the file (this boot's log, as before), each later one appends only the lines not yet written,
 * and once most of the ring is safely on disk it is emptied to take more. The file reads exactly as it
 * did; it just keeps growing. A file deleted mid-session (copied to the Pi and trashed) is recreated.
 * ⚠ FlushVol on the LOG's volume, not the default one: the link layer calls this from an NM response, in
 *   whatever process is current, and that process's default volume may well be the AppleShare volume. */
static int           gLogBegun = 0;       /* this load has created the file */
static unsigned long gLogFlushedN = 0;    /* ring lines already on disk */
static UInt32        gLogRecycles = 0;

static int ApShimLogHasNew(void) { return !gLogBegun || ApRingCount() > gLogFlushedN; }

static void ApShimDumpRingToFile(void)
{
    short ref = 0;
    FSSpec sp;
    unsigned long i, n;
    OSErr e;

    if(CurrentExecutionLevel() != 0) return;   /* not task level -- say nothing, safely */

    n = ApRingCount();
    if(gLogBegun && n <= gLogFlushedN) return;          /* nothing new */
    (void)apLogSpec("\pAirPort Driver Log", &sp);     /* k190: always the System Folder */
    if(!gLogBegun){
        (void)FSpDelete(&sp);
        if(FSpCreate(&sp, 'ttxt', 'TEXT', 0) != noErr) return;
        gLogFlushedN = 0;
    }
    e = FSpOpenDF(&sp, fsRdWrPerm, &ref);
    if(e == fnfErr && gLogBegun){                        /* deleted mid-session: start it again */
        if(FSpCreate(&sp, 'ttxt', 'TEXT', 0) != noErr) return;
        e = FSpOpenDF(&sp, fsRdWrPerm, &ref);
    }
    if(e != noErr) return;
    (void)SetFPos(ref, fsFromLEOF, 0);

    for(i = gLogFlushedN; i < n; i++){
        unsigned char line[256];
        long len, one = 1;
        if(!ApRingGet(i, line)) continue;
        len = (long)line[0];
        if(len > 255) len = 255;
        (void)FSWrite(ref, &len, (Ptr)&line[1]);
        (void)FSWrite(ref, &one, (Ptr)"\r");
    }
    (void)FSClose(ref);
    (void)FlushVol(NULL, sp.vRefNum);
    gLogBegun = 1;
    gLogFlushedN = n;
    if(n > (AP_RING_LINES * 3u) / 4u || ApRingBytes() > (AP_RING_BYTES * 3u) / 4u){
        ApRingReset();                                  /* everything in it is on disk */
        gLogFlushedN = 0;
        gLogRecycles++;
    }
}

/* ★★★★★ 8-23: A SNAPSHOT THE BOOT VEHICLE CAN ASK FOR, BECAUSE NOTHING ELSE WILL.
 *
 * ⚠⚠ THREE BUILDS OF LOGGING HAVE NOW LANDED TOO EARLY TO SEE WHAT MATTERS:
 *     8-21  dumped at the end of Open  -> could not contain EnetHAL_Start, which follows Open
 *     8-22  added Start, Status, Close -> Start dumps BEFORE any traffic; OT never calls
 *                                         Status (zero polls in k113); Close never came,
 *                                         because TCP/IP keeps the port open
 *   Every one of those is a moment defined by the DRIVER's lifecycle, and the question is about
 *   what happens in the quiet afterwards. There is no selector for "a while later".
 *
 * ★ But the boot vehicle has a Notification Manager response, it runs at TASK level, and it holds
 *   a connection to THIS copy -- the one Open Transport drives. It can simply ask, on a clock.
 *   That is the same NM-as-pump shape usb2-ehci/resident uses ("the NM pump is alive and polling
 *   at task level").
 *
 * ⚠ Guarded anyway. The caller is task level today; a future caller might not be, and the cost
 *   of being wrong about that is a hung machine with no NMI. */
/* ★ 8-24: a CHEAP phase read, so the snapshot pump can wait for the right moment.
 *
 * ⚠⚠ k114's schedule was anchored at REGISTRATION, which happens during boot. Its four
 *   snapshots would all have fired inside the first minute at the Finder -- before anyone could
 *   open the TCP/IP control panel, let alone save it -- and every one would have read zero. A
 *   correct instrument pointed at the wrong moment measures nothing, and it would have cost a
 *   reboot to find that out.
 *
 * So the pump polls THIS instead: no file, no ring write, nothing but three bits. It anchors its
 * schedule on the bind, which is the event the question is actually about.
 *
 *   bit 0  EnetHAL_Open has completed at least once
 *   bit 1  the driver joined the network
 *   bit 2  EnetHAL_Start has arrived (OT bound the port and gave us a notify proc) */
UInt32 AirPortShimGetPhase(void)
{
    return (UInt32)((gShimOpens ? 1u : 0u) | (gJoined ? 2u : 0u) | (gShimStartSeen ? 4u : 0u));
}

/* ★★★ 8-25: THE SNAPSHOT GETS ITS OWN FILE, APPENDED, AND HERE IS WHY.
 *
 * k116 fired four snapshots and only TWO reached the disk. ap_ring.h DROPS when full rather than
 * wrapping (which is the right choice -- a half-written line reads as real), the bring-up
 * narration is ~1140 lines of a 1600-line ring, and the last two snapshots were simply refused.
 * The dump then rewrote the same file it had written before, so nothing said anything was lost.
 *
 * A periodic status does not belong in a fixed-size narration ring at all. It appends to its own
 * file, so a busy driver cannot crowd it out, and the ring's drop counter is reported alongside
 * so saturation can never again be silent. */
/* ⚠ k117 FIRED FOUR SNAPSHOTS AND ONLY ONE REACHED THE FILE. The NM log shows all four calls;
 *   the file held exactly one block. The refnum was held open across calls and never closed, so
 *   the directory entry was only ever updated by FlushVol -- and whatever the precise reason,
 *   "held open and flushed" is the clever version and it lost three quarters of the data.
 *   Open, seek to EOF, write, CLOSE, every time. Slower, and it cannot lose anything. */
static short gSnapRef = 0;
static void SnapW(const char *label, unsigned long v)
{
    char b[96]; int i = 0, j; long n, one = 1;
    static const char hx[] = "0123456789abcdef";
    if(!gSnapRef) return;
    while(label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for(j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    n = i;
    (void)FSWrite(gSnapRef, &n, (Ptr)b);
    (void)FSWrite(gSnapRef, &one, (Ptr)"\r");
}

void AirPortShimSnapshot(void)
{
    if(CurrentExecutionLevel() != 0) return;
    {   /* open, append, CLOSE -- every time */
        static int sSnapMade = 0;
        FSSpec sp; long eof = 0;
        (void)apLogSpec("\pAirPort Snapshot Log", &sp);   /* k190: always the System Folder */
        if(!sSnapMade){ sSnapMade = 1; (void)FSpDelete(&sp);
                        (void)FSpCreate(&sp, 'ttxt', 'TEXT', 0); }
        gSnapRef = 0;
        if(FSpOpenDF(&sp, fsRdWrPerm, &gSnapRef) != noErr) gSnapRef = 0;
        if(gSnapRef){
            long one = 1, n;
            const char *hdr = "--- snapshot ---";
            (void)GetEOF(gSnapRef, &eof); (void)SetFPos(gSnapRef, fsFromStart, eof);
            n = 16; (void)FSWrite(gSnapRef, &n, (Ptr)hdr);
            (void)FSWrite(gSnapRef, &one, (Ptr)"\r");
            SnapW("  hardware IRQs     ", (unsigned long)gIrqHard);
            SnapW("    frames from IRQ ", (unsigned long)gIrqSecFrames);
            SnapW("  isr calls made    ", (unsigned long)gDlpiIsrCalls);
            SnapW("  shim Read calls   ", (unsigned long)gDlpiReadCalls);
            SnapW("    bytes handed up ", (unsigned long)gDlpiReadBytes);
            SnapW("  frames converted  ", (unsigned long)gDlpiConsumed);
            SnapW("  EnetHAL_Write sel ", (unsigned long)gSelCount[EnetHAL_Write]);
            SnapW("  ★ delivered IP    ", (unsigned long)gRxUpIp);
            SnapW("    ...of which DHCP", (unsigned long)gRxUpDhcp);
            SnapW("    ...other UDP    ", (unsigned long)gRxUpUdp);
            SnapW("    ...ICMP         ", (unsigned long)gRxUpIcmp);
            SnapW("  ★ delivered ARP   ", (unsigned long)gRxUpArp);
            SnapW("  delivered EAPOL   ", (unsigned long)gRxUpEapol);
            SnapW("  delivered other   ", (unsigned long)gRxUpOther);
            SnapW("  of those, bcast   ", (unsigned long)gRxUpBcast);
            /* ★★★ 8-26: THE TRANSMIT SIDE, WHICH HAS BEEN INVISIBLE ALL ALONG.
             *
             * k117 counted only gSelCount[EnetHAL_Write] -- how often OT ASKED us to send. It
             * never reported whether anything left the aerial. Every one of these counters has
             * existed since 8-8 and none of them was ever in a snapshot, so "OT wrote 2 frames"
             * has been carrying an unexamined assumption that 2 frames were transmitted.
             *
             * If calls > 0 and acked == 0, our DHCP requests never reached the AP, and every
             * DHCP-port frame we have been delivering upward is somebody else's broadcast
             * traffic -- which would explain the whole thing, including why the delivered
             * frames are all broadcast. */
            SnapW("  ★ TX calls        ", (unsigned long)gTxCalls);
            SnapW("    TX sent         ", (unsigned long)gTxSent);
            SnapW("    ★ TX ACKed      ", (unsigned long)gTxAcked);
            SnapW("    TX no key       ", (unsigned long)gTxNoKey);
            SnapW("    TX not assoc    ", (unsigned long)gTxNotAssoc);
            SnapW("    TX bad frame    ", (unsigned long)gTxBadFrame);
            SnapW("    TX window bad   ", (unsigned long)gTxWindowBad);
            SnapW("  ★ DHCP bcast set  ", (unsigned long)gTxDhcpBcast);
            SnapW("  ★ DHCP post-poll  ", (unsigned long)gTxDhcpPolled);
            SnapW("  ★ ...poll delivered", (unsigned long)gTxPollGot);
            /* ★★★★★ 8-47 (k142): WHAT WE TRANSMIT, not just what we receive. The histogram above
             * (gDhcpType) is RX-only; these are OUR OWN outgoing client requests. A REQUEST (type 3)
             * whose requested-IP (opt50) is not 192.168.1.225 with NO server-id (opt54) is the
             * INIT-REBOOT-for-a-stale-address signature that a NAK answers -- the whole point of k142. */
            { UInt32 q;
              static const char *kTxT[9] = {
                "  TX DHCP (no opt53)", "  ★ TX DHCP DISCOVER", "  ⚠ TX DHCP OFFER?  ",
                "  ★ TX DHCP REQUEST ", "  TX DHCP DECLINE   ", "  ⚠ TX DHCP ACK?    ",
                "  ⚠ TX DHCP NAK?    ", "  TX DHCP RELEASE   ", "  TX DHCP INFORM    " };
              for(q = 0; q < 9u; q++)
                if(gTxDhcpType[q]) SnapW(kTxT[q], (unsigned long)gTxDhcpType[q]); }
            SnapW("  ★ TX req-IP (opt50)", (unsigned long)gTxLastReqIp);
            SnapW("    TX opt50 seen    ", (unsigned long)gTxReqIpSet);
            SnapW("  ★ TX srv-id (opt54)", (unsigned long)gTxLastSrvId);
            SnapW("    TX opt54 seen    ", (unsigned long)gTxSrvIdSet);
            SnapW("  ★ TX ciaddr        ", (unsigned long)gTxLastCiaddr);
            SnapW("  ★ DHCP for US     ", (unsigned long)gRxUpDhcpForUs);
            /* ⚠⚠ THIS LINE READ "(chaddr==our MAC)" AND PRINTED gShimMacOk, WHICH IS THE MAC
             * READ-BACK FLAG FROM BRING-UP -- a different fact entirely, and a constant 1. The
             * count on the line ABOVE is the chaddr match. Every snapshot since 8-27 has
             * carried this, and reading "1" as "one frame matched our chaddr" next to "20" was
             * available to be misread on every one of them. */
            SnapW("    MAC readback ok ", (unsigned long)gShimMacOk);

            /* ★★★★★ 8-36: THE THREE MEASUREMENTS THAT SETTLE "OT IGNORES THEM".
             *
             * UDP BAD, non-zero   ⇒ our receive path corrupts the payload and Open Transport is
             *                       right to drop these frames. The defect is ours and it is in
             *                       the decrypt or the 802.11 -> Ethernet conversion.
             * UDP ok and yiaddr ZERO ⇒ the bytes are the server's own and it is genuinely
             *                       offering no address. The defect is not in this driver, and
             *                       the message type below says whether we are being NAKed.
             * IP BAD, non-zero    ⇒ worse and simpler: the header itself is not surviving. */
            SnapW("  ★ IP hdr cksum ok ", (unsigned long)gIpCkOk);
            SnapW("    ⚠ IP hdr BAD    ", (unsigned long)gIpCkBad);
            SnapW("  ★ UDP cksum ok    ", (unsigned long)gUdpCkOk);
            SnapW("    ⚠⚠ UDP cksum BAD", (unsigned long)gUdpCkBad);
            SnapW("    UDP cksum absent", (unsigned long)gUdpCkNone);
            SnapW("  ★ yiaddr non-zero ", (unsigned long)gDhcpYiSet);
            SnapW("    ⚠⚠ yiaddr ZERO  ", (unsigned long)gDhcpYiZero);
            SnapW("    last yiaddr     ", (unsigned long)gDhcpYiaddr);
            { UInt32 q;
              static const char *kDhcpT[9] = {
                "    (no option 53)  ", "    DHCP DISCOVER   ", "  ★ DHCP OFFER      ",
                "    DHCP REQUEST    ", "    DHCP DECLINE    ", "  ★ DHCP ACK        ",
                "  ⚠⚠ DHCP NAK       ", "    DHCP RELEASE    ", "    DHCP INFORM     " };
              for(q = 0; q < 9u; q++)
                if(gDhcpType[q]) SnapW(kDhcpT[q], (unsigned long)gDhcpType[q]); }
            /* ★ 8-36: the answers this driver used to throw away. */
            SnapW("  ⚠ RxRingArm FAILED", (unsigned long)gRxArmFail);
            SnapW("  ★ RX re-armed at scan", (unsigned long)gRxReArmed);
            SnapW("  ★ ...re-arm took (live)", (unsigned long)gRxReArmStuck);
            SnapW("  ⚠ RxRecycle refused", (unsigned long)gRxRecycleBad);
            SnapW("    RxRecycle last   ", (unsigned long)gRxRecycleLast);

            /* ★★★★★ 8-37: WHY EACH DROPPED FRAME WAS DROPPED -- the discriminator for the OFFER.
             *
             * The whole DHCP question now turns on a UNICAST frame. The NAKs we demonstrably
             * received are BROADCAST, decrypted with the GROUP key (GTK); a DHCP OFFER is
             * UNICAST, decrypted with the PAIRWISE key (PTK) -- a key path this driver has never
             * once proven on RECEIVED traffic, only on transmit and on host vectors. If a data
             * frame arrives, is addressed to us, and fails PTK decryption, it lands in
             * AP_ENET_DECRYPT_FAILED (kept DISTINCT from the group case at ap_enet.h:43/50), and
             * nothing above ever printed that histogram. So:
             *   DECRYPT_FAILED (pairwise) > 0, DECRYPT_FAILED_GROUP == 0  ⇒ the offer arrived
             *     and we could not read it: unicast/pairwise decryption is the fault.
             *   NOT_FOR_US > 0 on data frames                            ⇒ an address-role bug
             *     dropped a frame that was ours.
             *   all-zero but OFFER == 0 with RX ACTIVE                   ⇒ the offer never
             *     reached the ring: look at the radio, not the software.
             * This is exactly the "an oracle must SEE what it names" rule -- OFFER=0 alone names
             * nothing. */
            { UInt32 w;
              for(w = 0; w < (UInt32)AP_ENET_NREASON; w++)
                if(gDlpiWhy[w]){
                  char nm[80]; int z = 0; const char *t = ApEnetWhy((int)w);
                  nm[z++]=' '; nm[z++]=' '; nm[z++]='w'; nm[z++]='h'; nm[z++]='y'; nm[z++]=' ';
                  nm[z++]=(char)('0'+(w/10u)); nm[z++]=(char)('0'+(w%10u)); nm[z++]=' ';
                  while(*t && z < 76) nm[z++] = *t++;
                  nm[z] = 0;
                  SnapW(nm, (unsigned long)gDlpiWhy[w]); } }

            /* ★★★★★ 8-37: THE RECEIVE ENGINE, LIVE, AT SNAPSHOT TIME. Without this "★ DHCP
             * OFFER = 0" has two completely different meanings and no way to tell them apart:
             *   RXSTATUS reads ACTIVE  ⇒ the engine is alive and simply received no offer --
             *                            look above it (server, filter, ApRxToEnet), or the
             *                            offer came unicast and something dropped it.
             *   RXSTATUS reads DISABLED ⇒ the k125 fault: transmitting killed the receiver, so
             *                            the offer could not have been heard at all. A wholly
             *                            different search, and this one line names which.
             * The 2s/10s/30s/60s cadence means the 60s reading lands well after OT's DHCP.
             *
             * ⚠ Guarded exactly like CaptureRxRegs: null bar0 first (a copy that never mapped
             *   the card), then the window check -- ApShimWindowIsOurs reads through bar0. */
            if(gBus.bar0 && ApShimWindowIsOurs() && gShimDmaBase){
              UInt32 rxs = ssb_r32(gBus.bar0, gShimDmaBase + B43_DMA32_RXSTATUS);
              UInt32 mc  = ssb_r32(gBus.bar0, B43_MMIO_MACCTL);
              UInt32 st  = rxs & B43_DMA32_RXSTATE;
              SnapW("  ★ RXSTATUS now    ", (unsigned long)rxs);
              SnapW(st==B43_DMA32_RXSTAT_DISABLED ? "    ⚠⚠ RX state DISABLED (k125)" :
                    st==B43_DMA32_RXSTAT_ACTIVE   ? "    RX state ACTIVE (alive)"    :
                                                    "    RX state (see RXSTATUS)",
                    (unsigned long)(st >> 12));
              SnapW("  ★ MACCTL now      ", (unsigned long)mc);
            } else
              SnapW("  ⚠ RX read skipped (bar0/window)", 0);

            SnapW("  joined            ", (unsigned long)gJoined);
            /* ⚠ gTxAcked is ONLY incremented by the app's drain path (AirPortShimTxEthernet).
             * ApShimWriteFrame returns noErr as soon as it posts -- draining there would spin at
             * whatever level OT called us from. So a zero here means "nobody drained", NOT "the
             * frame failed", and k118 was very nearly read the wrong way round because of it. */
            SnapW("  (ACKed==0 means UNDRAINED, not failed)", 0);
            /* ★★★ 8-27: THE SELECTOR HISTOGRAM, which settles the EnetShim.h vs EthernetShim.pdf
             * conflict by observation. A non-zero count at 11 means the PDF is right and our
             * enum is off by one from selector 10 onward -- i.e. every "Status" we have answered
             * was really SetPacketFilter, and OT's real Status calls were refused with paramErr.
             * gFirstBad names the first selector we ever refused, which is the other half. */
            { UInt32 q;
              for(q = 0; q < AP_SHIM_NSEL; q++)
                if(gSelCount[q]){
                  char nm[40]; int z = 0;
                  const char *t = (q==0)?"RegisterPorts":(q==1)?"Open":(q==2)?"Close":
                                  (q==3)?"Start":(q==4)?"Stop":(q==5)?"Read":(q==6)?"Write":
                                  (q==7)?"GetMAC":(q==8)?"SetMAC":(q==9)?"SetMulticast":
                                  (q==10)?"Status-or-SetPacketFilter":
                                  (q==11)?"★ ELEVEN -- the PDF is right":"unknown";
                  nm[z++]=' '; nm[z++]=' '; nm[z++]='s'; nm[z++]='e'; nm[z++]='l';
                  nm[z++]=' '; nm[z++]=(char)('0'+(q/10)); nm[z++]=(char)('0'+(q%10));
                  nm[z++]=' ';
                  while(*t && z < 38) nm[z++] = *t++;
                  nm[z] = 0;
                  SnapW(nm, (unsigned long)gSelCount[q]); } }
            SnapW("  ★ first REFUSED selector (-1 = none)", (unsigned long)gFirstBad);
            /* ★ and the frames themselves, written at task level from bytes captured earlier */
            { UInt32 f;
              for(f = 0; f < gDhcpCapN; f++){
                char line[112]; int i = 0, c; long ln, one = 1;
                static const char hx[] = "0123456789abcdef";
                SnapW("  --- DHCP frame, full length", (unsigned long)gDhcpCapFull[f]);
                for(c = 0; c < (int)gDhcpCapLen[f]; c++){
                  if((c & 15) == 0 && c){ ln = i; (void)FSWrite(gSnapRef,&ln,(Ptr)line);
                                          (void)FSWrite(gSnapRef,&one,(Ptr)"\r"); i = 0; }
                  if(i == 0){ line[i++]=' '; line[i++]=' '; line[i++]=' '; line[i++]=' '; }
                  line[i++] = hx[(gDhcpCap[f][c] >> 4) & 0xF];
                  line[i++] = hx[gDhcpCap[f][c] & 0xF];
                  line[i++] = ' '; }
                if(i){ ln = i; (void)FSWrite(gSnapRef,&ln,(Ptr)line);
                       (void)FSWrite(gSnapRef,&one,(Ptr)"\r"); } } }
            /* ★ 8-47: and our OWN outgoing DHCP requests, verbatim (SNAP+IP+UDP+BOOTP head). The
             * option data (opt50/54) is past 96 bytes and lives in the counters above; this head
             * cross-checks op/xid/flags/ciaddr the way the RX capture cross-checks the replies. */
            { UInt32 f;
              for(f = 0; f < gTxDhcpCapN; f++){
                char line[112]; int i = 0, c; long ln, one = 1;
                static const char hx[] = "0123456789abcdef";
                SnapW("  --- TX DHCP frame, full length", (unsigned long)gTxDhcpCapFull[f]);
                for(c = 0; c < (int)gTxDhcpCapLen[f]; c++){
                  if((c & 15) == 0 && c){ ln = i; (void)FSWrite(gSnapRef,&ln,(Ptr)line);
                                          (void)FSWrite(gSnapRef,&one,(Ptr)"\r"); i = 0; }
                  if(i == 0){ line[i++]=' '; line[i++]=' '; line[i++]=' '; line[i++]=' '; }
                  line[i++] = hx[(gTxDhcpCap[f][c] >> 4) & 0xF];
                  line[i++] = hx[gTxDhcpCap[f][c] & 0xF];
                  line[i++] = ' '; }
                if(i){ ln = i; (void)FSWrite(gSnapRef,&ln,(Ptr)line);
                       (void)FSWrite(gSnapRef,&one,(Ptr)"\r"); } } }
            SnapW("  ⚠ ring lines      ", (unsigned long)ApRingCount());
            SnapW("  ⚠ ring DROPPED    ", (unsigned long)ApRingDropped());
            (void)FSClose(gSnapRef);
            gSnapRef = 0;
            (void)FlushVol(NULL, 0); } }
    Say("");
    Say("  [8-23] --- snapshot ---");
    Say1("    hardware IRQs      = ",(unsigned long)gIrqHard);
    Say1("      secondary runs   = ",(unsigned long)gIrqSecondary);
    Say1("      frames from IRQ  = ",(unsigned long)gIrqSecFrames);
    Say1("      not ours         = ",(unsigned long)gIrqNotOurs);
    Say1("    isr calls made     = ",(unsigned long)gDlpiIsrCalls);
    Say1("    shim Read calls    = ",(unsigned long)gDlpiReadCalls);
    Say1("      bytes handed up  = ",(unsigned long)gDlpiReadBytes);
    Say1("      empty Reads      = ",(unsigned long)gDlpiReadEmpty);
    Say1("    frames converted   = ",(unsigned long)gDlpiConsumed);
    Say1("    EnetHAL_Write sel  = ",(unsigned long)gSelCount[EnetHAL_Write]);
    Say1("    joined             = ",(unsigned long)gJoined);
    /* ★★★★★ 8-48 (k143): THE DHCP PICTURE IN THE DRIVER LOG, WHICH ALWAYS WRITES.
     * The rich Snapshot Log is created ONLY by AirPortShimSnapshot in the NM-response context,
     * where FSpCreate fails -- so once that file leaves the System Folder it never returns, and
     * k142 boots 3 & 4 (both HEARD, both joined) lost their DHCP answer to it. Say1 goes through
     * the ring ApShimDumpRingToFile flushes to the driver log every boot, so these cannot be lost.
     * This is the whole k141/k142 question, in the log that always writes:
     *   TX REQUEST > 0 with req-IP != 192.168.1.225 and opt54 seen == 0  => INIT-REBOOT for a
     *     stale address, which is what the server NAKs (leading hypothesis, CONFIRMED);
     *   req-IP == c0a801e1 and opt54 seen > 0                            => a proper SELECTING
     *     request being NAKed anyway (refuted -> look elsewhere);
     *   TX DISCOVER only, REQUEST == 0                                   => OT never accepts the
     *     offers (a third fault). */
    Say1("    -- RX DHCP for us   = ",(unsigned long)gRxUpDhcpForUs);
    Say1("       RX OFFER         = ",(unsigned long)gDhcpType[2]);
    Say1("       RX ACK           = ",(unsigned long)gDhcpType[5]);
    Say1("       RX NAK           = ",(unsigned long)gDhcpType[6]);
    Say1("       RX last yiaddr   = ",(unsigned long)gDhcpYiaddr);
    /* ★ 8-54 (k151): the OFFER's decoded options. opt54 == 0 while RX OFFER > 0 is the answer we
     * have never had -- OT cannot REQUEST without a server-id. opt count / saw-end witness a whole,
     * untruncated option list; a low count or saw-end == 0 means our hand-up clipped the tail. */
    Say1("       OFF opt54 srv-id = ",(unsigned long)gOffServerId);
    Say1("       OFF opt1 mask    = ",(unsigned long)gOffMask);
    Say1("       OFF opt3 router  = ",(unsigned long)gOffRouter);
    Say1("       OFF opt6 dns     = ",(unsigned long)gOffDns);
    Say1("       OFF opt51 lease  = ",(unsigned long)gOffLease);
    Say1("       OFF opt count    = ",(unsigned long)gOffOptCount);
    Say1("       OFF saw end 0xFF = ",(unsigned long)gOffSawEnd);
    Say1("    -- TX DISCOVER      = ",(unsigned long)gTxDhcpType[1]);
    Say1("       TX REQUEST       = ",(unsigned long)gTxDhcpType[3]);
    Say1("       TX req-IP(opt50) = ",(unsigned long)gTxLastReqIp);
    Say1("       TX opt50 seen    = ",(unsigned long)gTxReqIpSet);
    Say1("       TX srv-id(opt54) = ",(unsigned long)gTxLastSrvId);
    Say1("       TX opt54 seen    = ",(unsigned long)gTxSrvIdSet);
    Say1("       TX ciaddr        = ",(unsigned long)gTxLastCiaddr);
    Say1("       TX bcast flag set= ",(unsigned long)gTxDhcpBcast);
    Say1("    -- RX flag cleared   = ",(unsigned long)gRxDhcpFlagCleared);
    Say1("    -- RX dst->unicast    = ",(unsigned long)gRxDhcpDstRewrit);
    Say1("    -- RX ARP dst->unicast= ",(unsigned long)gRxArpDstRewrit);
    /* ★★★★★ 8-52 (k147): THE DATA-PATH DIAGNOSTIC. TX ARP sender-IP is OT's REAL address; RX ARP
     * who-has-US counts the Pi's ping arriving; the dropped-frame headers show what we cannot read. */
    Say1("    -- TX ARP count      = ",(unsigned long)gTxArpN);
    Say1("       TX ARP sender-IP  = ",(unsigned long)gTxArpLastSpa);
    Say1("       TX ARP target-IP  = ",(unsigned long)gTxArpLastTpa);
    Say1("       TX ARP op 1req2rep= ",(unsigned long)gTxArpLastOp);
    /* ★★★ 8-57 (k154): does OT answer the Pi's ping? echo-reply > 0 = OT generates the reply; then
     * dstMAC tells us if it is aimed at the Pi (correct -> AP/bridge/uplink) or elsewhere. */
    Say1("    -- TX ICMP echo-reply= ",(unsigned long)gTxIcmpReply);
    Say1("       TX ICMP echo-req  = ",(unsigned long)gTxIcmpReq);
    Say1("       TX ICMP last dstIP= ",(unsigned long)gTxIcmpLastDstIp);
    { Str255 L; int i; L[0]=0;
      PCat(L,"       TX ICMP dstMAC   = ");
      for(i=0;i<6;i++){ if(i) PCat(L,":"); PCatHex(L,(unsigned long)gTxIcmpLastDstMac[i],2); }
      Out(L); }
    Say1("    -- RX ARP count      = ",(unsigned long)gRxArpN);
    Say1("       RX ARP who-has US = ",(unsigned long)gRxArpWhoHasUs);
    Say1("       RX ARP last op    = ",(unsigned long)gRxArpLastOp);
    Say1("       RX ARP last spa   = ",(unsigned long)gRxArpLastSpa);
    Say1("       RX ARP last tpa   = ",(unsigned long)gRxArpLastTpa);
    Say1("    -- uni-TX sampled     = ",(unsigned long)gTxUniSampled);
    Say1("       uni-TX AP-ACKed    = ",(unsigned long)gTxUniAcked);
    Say1("       uni-TX NO-ACK      = ",(unsigned long)gTxUniNoAck);
    Say1("       uni-TX last ack    = ",(unsigned long)gTxUniLastAck);
    Say1("    -- ISR TX-stat drained= ",(unsigned long)gTxStatIsrDrained);
    Say1("       ISR TX-stat ACKed  = ",(unsigned long)gTxStatIsrAcked);
    Say1("    -- dropped-frame caps= ",(unsigned long)gDropCapN);
    { UInt32 f; int i;
      for(f = 0; f < gDropCapN; f++){
        Str255 L; const UInt8 *d = gDropCap[f];
        UInt16 fc = (UInt16)d[0] | ((UInt16)d[1] << 8);   /* FC is little-endian on the wire */
        L[0]=0; PCat(L,"      drop["); PCatDec(L,(unsigned long)f);
        PCat(L,"] why="); PCatDec(L,(unsigned long)gDropCapWhy[f]);
        PCat(L," FC="); PCatHex(L,(unsigned long)fc,4);
        PCat(L," A1="); for(i=0;i<6;i++){ if(i)PCat(L,":"); PCatHex(L,(unsigned long)d[4+i],2); }
        Out(L);
        L[0]=0; PCat(L,"            A2="); for(i=0;i<6;i++){ if(i)PCat(L,":"); PCatHex(L,(unsigned long)d[10+i],2); }
        PCat(L," A3=");                    for(i=0;i<6;i++){ if(i)PCat(L,":"); PCatHex(L,(unsigned long)d[16+i],2); }
        Out(L); } }
    /* ⇒ THE READING. Write climbing with everything else flat means TCP/IP is sending DHCP into
     *   a driver that delivers nothing back -- a receive-path problem. IRQs climbing with Read
     *   flat means we take interrupts and never hand anything up. All flat after a good join
     *   means the MAC is not receiving at all. Each is a different next build. */
    ApShimDumpRingToFile();
}

UInt32 AirPortShimGetLog(UInt32 idx, UInt8 *out, UInt32 *dropped, UInt32 *bytesUsed)
{
    if(dropped)   *dropped   = (UInt32)ApRingDropped();
    if(bytesUsed) *bytesUsed = (UInt32)ApRingBytes();
    if(out && !ApRingGet((unsigned long)idx, (unsigned char *)out)) out[0] = 0;
    return (UInt32)ApRingCount();
}

/* ── 8-2d-1: the driver says what it just did ──
 *
 * This narrates the results the 8-5g/8-2a/8-2b/8-2c code already recorded in globals rather than
 * instrumenting those four functions. That keeps this increment to what it claims to be -- a
 * change of SINK, not of hardware code -- so a regression here cannot be confused with a
 * regression in the bring-up. 8-2d-2 and 8-2d-3 move the real narration across wholesale.
 *
 * ⚠ IT EXERCISES ALL FOUR PATHS ON PURPOSE: Say, SayH, Say1, and a composite built with
 *   PCat/PCatHex/PCatDec and handed to Out() directly. The composite path is the one an
 *   ap_ilog.h-style pointer ring would have silently dropped, and it is 34 of the call sites
 *   waiting in ap_phy_initg.h. Proving it here costs one line and settles the design choice on
 *   hardware rather than on my reading of it.
 *
 * ⚠ THE OPEN COUNTER IS A STALENESS GUARD, NOT DECORATION. These globals live in the fragment's
 *   data section, and CFM keeps the fragment prepared as long as anything holds a connection. Run
 *   the app twice without a restart and the second run can read the FIRST run's lines. A log that
 *   says "Open #2" when the tester expected one Open is the signal that happened. This project
 *   has been burned by a stale log before; the counter is cheaper than the reboot. */
static UInt32 gShimOpenSeq = 0;

static void ApShimNarrateOpen(void)
{
    Str255 L;
    int i;

    gShimOpenSeq++;

    Say("");
    Say1("=== AirPortShim narration, EnetHAL_Open #", (unsigned long)gShimOpenSeq);

    /* ── 8-5g: find the card and read its address ── */
    Say1("  [8-5g] SPROM address read back ok: ", (unsigned long)gShimMacOk);
    SayH("  [8-5g] map status ", (unsigned long)gShimMapErr, 8);
    L[0] = 0; PCat(L, "  [8-5g] MAC ");
    for(i = 0; i < 6; i++){ if(i) PCat(L, ":"); PCatHex(L, (unsigned long)gShimMac[i], 2); }
    Out(L);                                  /* ★ the composite path */

    /* ── 8-2a: enumerate the backplane ── */
    SayH("  [8-2a] scan status ", (unsigned long)gShimScanErr, 8);
    L[0] = 0; PCat(L, "  [8-2a] chip 0x"); PCatHex(L, gShimChipId, 4);
    PCat(L, " rev "); PCatDec(L, gShimChipRev);
    PCat(L, ", cores "); PCatDec(L, gShimNCores);
    Out(L);                                  /* ★ the composite path, with decimals */
    if(gShimIdx80211 >= 0) Say1("  [8-2a] 802.11 core at index ", (unsigned long)gShimIdx80211);
    else                   Say("  [8-2a] [!!] no 802.11 core found");
    if(gShimIdxCC >= 0)    Say1("  [8-2a] chipcommon core at index ", (unsigned long)gShimIdxCC);

    /* ── 8-2b: bring the core up ── */
    Say1("  [8-2b] armed by the app: ", (unsigned long)gShimArmed);
    SayH("  [8-2b] core-enable status ", (unsigned long)gShimCoreErr, 8);
    Say1("  [8-2b] core is up: ", (unsigned long)gShimCoreUp);
    SayH("  [8-2b] TMSLOW ", gShimTmsLow, 8);

    /* ── 8-2c: upload our own microcode ── */
    SayH("  [8-2c] firmware status ", (unsigned long)gShimFwErr, 8);
    L[0] = 0; PCat(L, "  [8-2c] ucode rev "); PCatDec(L, (unsigned long)gShimFwRev);
    PCat(L, " (0x"); PCatHex(L, (unsigned long)gShimFwRev, 4); PCat(L, ")");
    Out(L);
    SayH("  [8-2c] MACCTL/irq ", gShimFwIrq, 8);

    /* ── 8-2d-2: the initvals ──
     * The per-table lines were already emitted INLINE by ApShimApplyInitvals, above this in the
     * ring. This is the summary line; the detail is where it happened. */
    SayH("  [8-2d-2] initvals status ", (unsigned long)gShimIvErr, 8);
    SayH("  [8-2d-3] gpio/radio/phy status ", (unsigned long)gShimPhyErr, 8);
    Say1("  [8-2d-3] ApPhyInitG returned ", (unsigned long)gShimPhyOk);
    SayH("  [8-2e] chip_init tail status ", (unsigned long)gShimTailErr, 8);
    SayH("  [8-2f] dma/rx-ring status ", (unsigned long)gShimDmaErr, 8);
    SayH("  [8-2g] receiver status ", (unsigned long)gShimRxErr, 8);

    /* ⚠ b43 rejects rev <= 0x128 outright, and Stage 5-4 was stuck for weeks on a v3 blob that
     * passed every upload check and then read our 106-byte txhdr as its own 82-byte layout. The
     * threshold is decoded HERE, at the point of reading, rather than left as a number for
     * whoever reads the log to remember -- see "never log a status register as a bare number". */
    if(gShimFwErr == noErr)
        Say(gShimFwRev > 0x128 ? "  [8-2c] rev is ABOVE b43's 0x128 floor -- usable"
                               : "  [8-2c] [!!] rev is AT OR BELOW 0x128 -- b43 would reject this");

    Say1("=== end of narration, Open #", (unsigned long)gShimOpenSeq);
}

/* ── ValidateHardware ──
 * "called only once, at system boot time, before installing your driver into the Open Transport
 * module registry" (OpenTransportKernel.h:2514). We are not installed at boot in this increment
 * -- the probe registers us by hand -- so this may never run. It is exported anyway because its
 * absence is one of the things that could make the shim refuse us, and an unexercised export
 * costs nothing while a missing one costs a reboot to discover. */
/* Exported again as of k68. It was dropped from ap_shim.exp in k67 only to get the export count
 * under ten while MakePEF was still writing broken hash tables; the tool is fixed now, so the
 * workaround is gone and the original intent is restored. See the note below.
 *
 * ⚠ Incidentally MEASURED to be uncalled: in k66 CFM could not find this symbol at all (it was
 *   one of the three the bug stranded) and registration, Open and removal all completed normally.
 *   Apple calls ValidateHardware only when a driver is installed into the OT module registry AT
 *   BOOT, and we register by hand through EnetShimInstallDriver. */
OSErr EnetMac_ValidateHW(void *param)
{
    (void)param;          /* not #pragma unused -- that is CodeWarrior's, and gcc warns on it */
    return noErr;
}

/* ⚠⚠ A NOTE ABOUT THE EXPORT LIST, because it cost two hardware cycles to learn.
 *
 * Retro68's MakePEF wrote export hash tables that CFM could not read. It placed a symbol at
 * `table[key % sz]` (MakePEF.cc:212) while CFM looks in `(word ^ (word >> power)) & (size-1)`.
 * The two agree only while bit `power` of the hash word is clear, and MakePEF keeps ONE bucket
 * until a fragment has ten exports -- so the bug was invisible for years and appeared the moment
 * this file crossed ten.
 *
 * ⚠ THE SYMPTOM IS A LIE IN BOTH DIRECTIONS. It reports symbols as "not exported" when they are
 *   exported, and it strands whichever symbols happen to have the bit set rather than the one
 *   just added. k66 added AirPortShimGetBuild and lost AirPortShimGetFw and AirPortShimStats,
 *   untouched for weeks. Nothing in that diff points at them.
 *
 * ★ MakePEF WAS PATCHED ON 2026-09-21 and the fix is verified: the same ten-export fragment fails
 *   the checker when built with the old binary and passes with the new one, and rebuilding an
 *   existing nine-export fragment produced a BYTE-IDENTICAL .pef, so nothing that already worked
 *   was disturbed. The patch is kept at airport-extreme/tools/makepef-export-hash-fix.patch.
 *
 * ⚠⚠ THE PATCH IS NOT SAFE FROM THE TOOLCHAIN. build-toolchain.bash rebuilds MakePEF from source
 *   and will silently restore the buggy version. check-pef-exports.py is a HARD gate in
 *   package-probe.sh precisely so that a reverted toolchain is caught at packaging time rather
 *   than on the G4 -- it reads where each symbol ACTUALLY sits in the table and compares that
 *   against CFM's rule, so it stays honest no matter which MakePEF built the fragment.
 *
 * ⚠ THE .exp TAKES NO COMMENTS. The linker tries to export each token in it: a '#' line produces
 *   `attempt to export undefined symbol '#'`. Keep it a bare list; the explanation lives here.
 *
 * Even so, prefer the 8-2d-1 NARRATION over another bespoke accessor when more driver facts are
 * needed. One export per fact does not scale to ap_phy_initg.h, which is the whole reason the
 * narration exists -- and it is why AirPortShimGetFw's data already appears in the D| lines. */

/* ── The one entry point OT drives everything through ── */
OSErr EnetHAL_Entry(UInt16 selector, USBEnetPtr enet, UInt32 refCon)
{
    gTotalCalls++;
    gLastRefCon = refCon;
    if(selector < AP_SHIM_NSEL) gSelCount[selector]++;
    if(gSelOrderN < AP_SHIM_ORDER_N) gSelOrder[gSelOrderN++] = (UInt8)selector;

    /* ★★★★★ 8-38: THE DRIVER SNAPSHOTS ITSELF. The boot vehicle's NM pump cannot reach this
     * fragment: k130 and k131 both proved FindSymbol from the NM response proc fails
     * intermittently with -2801, because GetSharedLibrary itself fails whenever the NM fires in
     * a foreign CFM context, and the snapshot the whole DHCP decode feeds never ran on either
     * run. The driver has no such problem -- OT calls it in a valid context and it already
     * writes its own logs from here. So it dumps the snapshot ITSELF, from whatever selector OT
     * calls at task level during the DHCP phase (Write fires every time TCP/IP sends a DISCOVER).
     * No cross-fragment call, no context to be wrong. This is the fix the NM pump was a
     * workaround for.
     *
     * ⚠ AirPortShimSnapshot self-checks CurrentExecutionLevel()==0, so it is silent at interrupt
     *   level; the outer check here is only to skip the TickCount/throttle work in that case.
     *   Throttled to once per ~2 s so a busy Write path cannot flood the file, and only after
     *   Start, when there is a DHCP phase worth capturing. */
    if(gShimStartSeen && CurrentExecutionLevel() == 0){
        static UInt32 sLastSelfSnap = 0;
        UInt32 nowT = (UInt32)TickCount();
        if(sLastSelfSnap == 0 || (nowT - sLastSelfSnap) >= 120u){
            sLastSelfSnap = nowT;
            AirPortShimSnapshot(); } }

    switch(selector){

      /* Harmless to accept, and accepting them is what lets the sequence continue far enough to
       * be worth reading. None of them touches the card. */
      /* ★ Open is the ONLY task-time selector Apple documents, so it is the only place this
       * driver is allowed to map anything or walk the Name Registry. */
      case EnetHAL_Open:
        /* ★★★ 8-36: SAY WHICH DRIVER THIS IS, IN THE DRIVER'S OWN LOG, ON EVERY BOOT.
         *
         * ⚠⚠ AP_SHIM_BUILD was readable ONLY through AirPortShimGetBuild -- an exported getter
         *   that the APPLICATION calls. The deliverable is two extensions and no application,
         *   so an ordinary boot run produced a driver log with nothing in it naming the build.
         *   The startup extension stamps "k129" in the INIT log; the driver stamped nothing.
         *
         *   That is the k65 defect with the parts swapped: a two-part deliverable needs a
         *   freshness witness PER PART, and this one has been missing since the app stopped
         *   being required at k112. It matters more than usual here because several builds
         *   ship a fragment named "AirPortShim" and which one CFM picks among duplicates in
         *   Extensions is undefined -- so "is the driver I installed the driver that ran" has
         *   been an unanswerable question for seventeen builds. */
        Say1("  [8-36] ★ AirPort Extreme Driver, build ", (unsigned long)AP_SHIM_BUILD);

        /* ★★★★★ 8-19: SAY, OUTSIDE THIS FRAGMENT, THAT OPEN HAPPENED.
         *
         * ⚠⚠ k107 COULD NOT ANSWER "DID OT CALL US", AND THAT IS A WITNESS DEFECT, NOT A RESULT.
         *   The app read "EnetHAL_Open calls = 0" from ITS OWN connection to AirPortShim. The
         *   boot vehicle made a separate GetSharedLibrary call, and an INIT-loaded fragment can
         *   get its own instance with its own globals --
         *   [[reference_os9_two_fragment_copies_own_globals]]. So a zero in the app's copy is
         *   consistent with BOTH "OT never called us" and "OT called a copy you cannot see", and
         *   those demand completely different next moves.
         *
         *   A counter in this fragment's data cannot settle it. A Name Registry property can:
         *   it is outside every copy, exactly as ap_boot.c's beacons are, and that is the same
         *   fix k100 taught and this is the second place it was needed.
         *
         * ⚠ Task level only, which EnetHAL_Open is -- Apple documents it as task time and it is
         *   the one selector this project's audit already exempts for allocation. Nothing else
         *   in the dispatch may do this. */
        { RegEntryID root;
          gShimOpens++;
          if(RegistryCStrEntryLookup(NULL,"Devices:device-tree",&root) == noErr){
            UInt32 v = 0x4F500000UL | (gShimOpens & 0xFFFF);   /* 'OP' | count */
            if(RegistryPropertyCreate(&root,"airport-hal-open",(void*)&v,
                                      (RegPropertyValueSize)sizeof(v)) != noErr)
              (void)RegistryPropertySet(&root,"airport-hal-open",(void*)&v,
                                        (RegPropertyValueSize)sizeof(v));
            (void)RegistryEntryIDDispose(&root); } }
        ApShimFindCard();
        ApShimScanBackplane();   /* 8-2a: enumerate the backplane from inside the driver */
        ApShimEnableCore();      /* 8-2b: the first WRITE -- gated on AirPortShimArm() */
        ApShimLoadFirmware();    /* 8-2c: upload our own microcode, needs the core up */
        ApShimGpioInit();        /* 8-2d-3: chip_init step 3, BEFORE the initvals */
        ApShimApplyInitvals();   /* 8-2d-2: chip_init steps 4 and 5 */
        ApShimRadioOn();         /* 8-2d-3: chip_init steps 6 and 7 */
        ApShimPhyInit();         /* 8-2d-3: ★ b43_phy_init, fifteen Stage 4 oracles */
        ApShimChipInitTail();    /* 8-2e: chip_init statements 8-13 */
        ApShimCoreInitMid();     /* k194: b43_wireless_core_init's middle (rate memory, CW, PHYTYPE...) */
        ApShimDmaInit();         /* 8-2f: b43_dma_init, the RX ring */
        ApShimCoreInitTail();    /* 8-2g: core_init's remaining five statements */
        ApShimMacOn();           /* 8-2g: ★ channel, filter, b43_mac_enable */
        ApShimTxInit();          /* 8-3a: ★ b43_dma_init's TX half, and one frame */
        ApShimReadNodeName();    /* 8-11: what does the DM actually call this card? */
        ApShimJoin();            /* 8-9: ★★★ find the BSSID, authenticate, associate, handshake */
#if AP_ARM_IRQ_IN_OPEN
        /* ⛔⛔ DISABLED AFTER k92 GREY-SCREENED THE MACHINE. See ap_shim.c's 8-9b block.
         * ★★★ 8-9: AND ARM, HERE, BECAUSE THIS IS THE ONLY SELECTOR WHERE IT IS LEGAL.
         *
         * ⚠⚠ THE 8-7 SAFETY GATE IS BEING DELIBERATELY RELAXED and it deserves saying plainly.
         *   8-7 installed the ISR only on an explicit call from the test application, so a hang
         *   could never repeat at boot. It has since run twice -- 527 and 503 interrupts, zero
         *   not-ours, zero queue failures, zero window faults -- and an extension-only
         *   deliverable cannot require an application to arm it.
         *
         *   The residual risk is narrow and worth naming: Open arrives when something opens this
         *   provider, and at boot that happens only if AirPort is SELECTED IN THE TCP/IP CONTROL
         *   PANEL. Until several clean runs accumulate, leave it unselected -- the test
         *   application opens the provider by name, which is a deliberate act with a human at
         *   the keyboard.
         *
         * ⚠ Only over a successful join. Arming for a link that does not exist would take
         *   interrupts and hand the secondary handler a ring nothing is filling. */
        if(gJoined && !gIrqArmed) (void)AirPortShimArmIrq();
#endif
#if AP_INSTALL_IRQ_IN_OPEN
        /* 8-9c: the harmless half only. See AP_INSTALL_IRQ_IN_OPEN above. */
        if(gJoined && !gIrqInstalled) (void)ApShimIrqInstall();
#endif
        ApShimNarrateOpen();     /* 8-2d-1: say what just happened, into the ring */
        ApShimDumpRingToFile();  /* ★★★ 8-21: and out to disk, because nobody can read the ring */
        if(enet) enet->ioResult = noErr;
        return noErr;

      /* ⚠ 8-2f: Close is where the DMA memory goes back. It is a separate case from the
       * other harmless selectors now, because it is no longer harmless -- it releases system
       * heap the card was given physical addresses for, and it stops the engine first. */
      case EnetHAL_Close:
        /* ⛔ DISARM BEFORE ANYTHING ELSE, AND UNCONDITIONALLY. Quiesce resets the DMA engines
         * and zeroes the ring registers; an interrupt landing in the middle of that would send
         * the secondary handler walking a ring that is being dismantled underneath it. And if
         * this fragment is ever unloaded with our ISR still installed, the next interrupt on
         * that line calls into freed memory. Restoring the parent's functions is not cleanup,
         * it is the difference between a clean close and a crash minutes later. */
        ApShimIrqQuiesce();           /* 8-8: the level-safe half. See its header for why the
                                       * InstallInterruptFunctions restore is NOT done here. */
        (void)AirPortShimQuiesce();   /* 8-2g: MAC down, engines reset, ring regs zeroed */
        ApShimDumpRingToFile();       /* 8-22: end-of-session snapshot, guarded like the others */
        if(enet) enet->ioResult = noErr;
        return noErr;

      case EnetHAL_RegisterPorts:
      case EnetHAL_Status:
        /* ⛔ k113: OT NEVER CALLS THIS. Zero status polls in a run where TCP/IP bound the port
         * and Start arrived, so it is not the rolling picture I hoped for. Left in -- it costs
         * nothing and would be the cheapest source if some other client ever does poll -- but
         * the snapshot pump in the boot vehicle is what actually produces one. */
        AirPortShimSnapshot();
        if(enet) enet->ioResult = noErr;
        return noErr;

      /* ⚠ Stop arrives ELEVEN times in a k84 run, seven of them before Start is ever tried, so
       * it cannot be treated as "tear everything down". It clears delivery and nothing else. */
      case EnetHAL_Stop:
        gShimStarted = 0;
        if(enet) enet->ioResult = noErr;
        return noErr;

      /* ★★★★★★ 8-6a: START. Save the notify proc and its refcon -- ShimEnetHAL.c:66 does
       * exactly this and nothing more. No hardware is touched: the receiver has been running
       * since Open, and Apple's USBStartupRead() equivalent is already done. */
      case EnetHAL_Start:
        if(!enet){ if(gFirstBad < 0) gFirstBad = (SInt32)selector; return paramErr; }
        gShimIsr       = enet->ioCompletion;
        gShimIsrCookie = (UInt32)enet->ioMisc;
        gShimStarted   = 1;
        gShimStartSeen = 1;
        /* ⚠⚠ THE ARM IS **NOT** HERE, AND THE AUDIT IS WHY.
         *
         * The first draft of 8-9 armed the interrupt from this selector, which reads naturally
         * -- Start is when OT wants delivery. check-exec-level.py refused it:
         *
         *     ⛔ EnetHAL_Start reaches SsbSelectCore -- PCI config space below task level
         *
         * Arming needs PCI config space, the Name Registry and InstallInterruptFunctions, and
         * all three are task level only. Start is NOT documented as task time -- only Open is.
         * That is the SAME defect the audit caught at EnetHAL_Close an hour earlier, made again
         * immediately, in a selector where it looked even more reasonable.
         *
         * So the arm happens at the end of Open, where those calls are legal, and Start does
         * only what Apple's own sample does here: record the proc and the refcon. */
        /* ★ 8-6b: SAY SO, IN THE RING, WHERE IT LANDS AMONG THE DRIVER'S OWN NARRATION.
         * k85 accepted Start and the app still reported "NEVER ARRIVED", because the app pumped
         * before OT got there -- and nothing in the log could place Start in time, only in the
         * selector ORDER. One line here puts it on the same timeline as the handshake. The ring
         * is a memory sink (ap_ring.h), never the File Manager, so this is safe from a selector
         * that may run below task level. */
        Say("");
        Say("  [8-6] ★★★ EnetHAL_Start ARRIVED -- the shim handed us a notify proc.");
        SayH("    ioCompletion = ",(unsigned long)enet->ioCompletion,8);
        SayH("    ioMisc       = ",(unsigned long)enet->ioMisc,8);
        /* ★★★ 8-22: AND DUMP, BECAUSE THE FILE COULD NEVER CONTAIN THIS LINE BEFORE.
         *
         * 8-21 drained the ring at the end of EnetHAL_Open only. Start arrives AFTER Open, so
         * "did Start arrive" -- the single most important question once TCP/IP binds -- was
         * physically unanswerable from the log written to answer it. The ring HAD the line since
         * 8-6; nothing ever wrote it out.
         *
         * ⚠ Start is NOT documented as task time, and that is exactly why this is safe to call
         *   here: ApShimDumpRingToFile measures CurrentExecutionLevel() and writes nothing at
         *   all if it is not zero. The guard is what lets the dump go in the places that matter
         *   rather than only the one place that was provably safe. */
        ApShimDumpRingToFile();
        enet->ioResult = noErr;
        return noErr;

      /* ★★★ 8-6a: READ -- the shim collecting what the isr told it about.
       *
       * ⚠ THIS RUNS RE-ENTRANTLY, inside our own call to the isr, and it may run below task
       *   level. It therefore touches ONE thing: the staging buffer. No ring walking, no BAR0
       *   window change, no allocation. check-exec-level.py now audits this selector through
       *   ap_enet.h and ap_ccmp.h as well, because the old audit could not see into them.
       *
       * Apple's sample returns readErr with ioActCount 0 when the queue is empty, so an empty
       * Read is a normal event and is counted separately rather than scored as a failure. */
      case EnetHAL_Read:
        if(!enet) return paramErr;
        gDlpiReadCalls++;
        if(!enet->ioBuffer || gEnetQTail == gEnetQHead){
          enet->ioActCount = 0;
          enet->ioResult   = readErr;
          gDlpiReadEmpty++;
          return readErr; }
        { UInt32 n = 0;
          if(!ApEnetQPop((UInt8*)enet->ioBuffer,(UInt32)enet->ioReqCount,&n)){
            enet->ioActCount = 0; enet->ioResult = readErr;
            gDlpiReadEmpty++; return readErr; }
          enet->ioActCount = n;
          enet->ioResult   = noErr;
          gDlpiReadBytes  += n;
          gDlpiConsumed++;            /* counted HERE: the shim really took a frame */
          /* ★★★ 8-25: WHAT, exactly, ARE WE HANDING UP? k116 proved the path works -- 10,920
           * bytes delivered, 29 writes down -- and DHCP still fails. "Frames are flowing" and
           * "the frames DHCP needs are flowing" are different claims, and only one of them has
           * been measured. The EtherType is two bytes at offset 12 of an Ethernet frame and
           * costs nothing to count. If no 0x0800 ever arrives, the reply is being dropped
           * before us; if plenty do, the problem is above us. */
          if(n >= 14){
            const UInt8 *eh = (const UInt8*)enet->ioBuffer;
            UInt32 et = ((UInt32)eh[12] << 8) | eh[13];
            int bcast = (eh[0] & 0x01) != 0;
            if(bcast) gRxUpBcast++;
            switch(et){
              case 0x0800: gRxUpIp++;
                /* ★★★ 8-36b: ONE CALL, INTO A FUNCTION THE HOST SUITE EXERCISES.
                 *
                 * ⛔ Everything below used to be written out inline here, and on its first boot
                 *   it walked 65 KB off the end of a buffer and took the machine to MacsBug --
                 *   because the BOOTP length came off the wire and only one of the three reads
                 *   that used it was bounded. Inline in a selector meant enet_test.c could not
                 *   reach it, so the first execution of that code anywhere was on the G4. The
                 *   parser now lives in ap_enet.h with the bound computed once, and the
                 *   malformed-length frame is a host case. See ApIpInspect's header. */
                { ApIpInfo inf;
                  ApIpInspect(eh, n, gShimMac, &inf);
                  if(inf.ipCkOk == 1)      gIpCkOk++;
                  else if(inf.ipCkOk == 0) gIpCkBad++;

                  if(inf.isDhcp){
                    gRxUpDhcp++;
                    if(inf.chaddrIsOurs) gRxUpDhcpForUs++;
                    if(inf.udpCk == AP_UDPCK_OK)          gUdpCkOk++;
                    else if(inf.udpCk == AP_UDPCK_BAD)    gUdpCkBad++;
                    else if(inf.udpCk == AP_UDPCK_ABSENT) gUdpCkNone++;
                    gDhcpType[(inf.msgType >= 0 && inf.msgType <= 8)
                              ? (UInt32)inf.msgType : 0u]++;
                    /* ★★★★★ TIGER, 2026-09-23, SAME CARD AND SAME MAC ON THIS AP:
                     *   yiaddr = 192.168.1.225, siaddr = 192.168.1.1, message type ACK.
                     * So the server allocates to our card's MAC without hesitation, and
                     * "it is refusing us" is dead. k120's reply had yiaddr AND siaddr both
                     * zero -- the two fields Tiger shows non-zero, eight adjacent bytes at
                     * Ethernet offsets 58..65. Either we blank them after the MIC verified,
                     * or the reply really is a NAK. The message type above is what separates
                     * those, and it is the only thing that can. */
                    if(inf.isReply){
                      if(inf.yiaddr){ gDhcpYiSet++; gDhcpYiaddr = inf.yiaddr; }
                      else            gDhcpYiZero++; }
                    /* ★ 8-54 (k151): stash the OFFER's decoded options. opt 54 == 0 while offers
                     * arrive is the smoking gun -- OT cannot build a SELECTING REQUEST without it. */
                    if(inf.isReply && inf.msgType == 2){
                      gOffServerId = inf.serverId;  gOffMask  = inf.subnetMask;
                      gOffRouter   = inf.router;    gOffDns   = inf.dns1;
                      gOffLease    = inf.leaseSecs;
                      gOffOptCount = (UInt32)inf.optCount;
                      gOffSawEnd   = (UInt32)inf.sawEnd; }
                    /* 8-27: keep the head of the first three, verbatim, for the snapshot.
                     * ⚠ Real MAC addresses from the user's own network; the log files are in
                     *   PRE-PUBLICATION-SCRUB.md for exactly this. */
                    if(gDhcpCapN < 3){
                      UInt32 k, lim = (n < 352u) ? n : 352u;
                      for(k = 0; k < lim; k++) gDhcpCap[gDhcpCapN][k] = eh[k];
                      gDhcpCapLen[gDhcpCapN]  = lim;
                      gDhcpCapFull[gDhcpCapN] = n;
                      gDhcpCapN++; } }
                  else if(inf.isUdp) gRxUpUdp++;
                  else if(n >= 14u + 20u && eh[14 + 9] == 1u) gRxUpIcmp++; }
                break;

              /* ---- the old inline decode, removed at 8-36b ---- */
              case 0x0806: gRxUpArp++;
                /* 8-52 (k147): decode a received ARP. If its target IP is our .225, someone (the
                 * Pi's ping) is asking for us -- proving the request reached OT's stack. */
                if(n >= 42u){
                  UInt16 aop = ((UInt16)eh[20] << 8) | eh[21];
                  UInt32 spa = ((UInt32)eh[28]<<24)|((UInt32)eh[29]<<16)|((UInt32)eh[30]<<8)|eh[31];
                  UInt32 tpa = ((UInt32)eh[38]<<24)|((UInt32)eh[39]<<16)|((UInt32)eh[40]<<8)|eh[41];
                  gRxArpN++; gRxArpLastOp = aop; gRxArpLastSpa = spa; gRxArpLastTpa = tpa;
                  if(aop == 1u && tpa == 0xC0A801E1UL) gRxArpWhoHasUs++; }
                break;
              case 0x888E: gRxUpEapol++; break;
              default:     gRxUpOther++; break; } } }
        return noErr;

      /* ★ 8-5h: ACCEPTED, AND THIS ONE IS NOT A LIE -- which is worth arguing rather than
       * assuming, because everything else here is refused on exactly that principle.
       *
       * Accepting a multicast filter we do not program means the receiver delivers a SUPERSET of
       * what was asked for: more frames than necessary, never fewer, and never the wrong ones.
       * Real drivers do this routinely when the hardware filter table is exhausted -- they fall
       * back to receiving all multicast and let the stack discard. It costs throughput, not
       * correctness.
       *
       * Contrast GetMACAddress, which was refused until the card could actually answer it: a
       * wrong address is wrong DATA, and OT would have built a link on it. The distinction is
       * whether the caller can be misled about something it cannot check. */
      case EnetHAL_SetMulticastFilters:
        if(enet){ enet->ioActCount = 0; enet->ioResult = noErr; }
        return noErr;

      /* ★★★ 8-5g: THE SELECTOR THAT STOPPED k58, NOW ANSWERED FROM THE CARD ITSELF.
       * Served from the cache filled at Open -- no hardware access here, because this may be
       * called below task level. Still refuses if the read did not produce a sane address:
       * a wrong MAC is worse than no MAC, because OT would go on to Start on it. */
      case EnetHAL_GetMACAddress:
        if(!gShimMacOk){
          if(gFirstBad < 0) gFirstBad = (SInt32)selector;
          if(enet) enet->ioResult = paramErr;
          return paramErr; }
        if(enet && enet->ioBuffer){
          int i; UInt8 *d = (UInt8*)enet->ioBuffer;
          for(i=0;i<6;i++) d[i] = gShimMac[i];
          enet->ioActCount = 6;
          enet->ioResult = noErr; }
        return noErr;

      /* ⚠ STILL REFUSED ON PURPOSE, and the principle is unchanged even though Start and Read
       * have now left this list.
       *
       * Write moves a frame OUT, and the transmit path in this fragment sends management frames
       * it builds itself -- it cannot yet take an Ethernet frame from OT, wrap it in 802.11 and
       * CCMP-encrypt it. SetMACAddress would program hardware nothing here programs. Returning
       * noErr would tell OT those worked, which is the kind of lie that costs a day to unpick.
       *
       * ★ Start and Read moved out at 8-6a because they stopped being lies: Start saves a real
       *   notify proc and Read serves a real Ethernet frame built from a real decrypted 802.11
       *   frame. That is the ONLY reason a selector may leave this list -- not because refusing
       *   it was inconvenient.
       *
       * ⇒ If the sequence stops at one of these, that is a RESULT: it names the next selector
       *   OT insists on, which is precisely the thing worth knowing next. */
      /* ★★★★★★ 8-8: WRITE. OT hands us an Ethernet frame; it leaves as CCMP-encrypted 802.11.
       *
       * ⚠ It returns an ERROR whenever it did not transmit, and never noErr for a frame that
       *   went nowhere. "The link is up and every packet vanishes" is the most expensive lie
       *   this interface can tell, and the counters behind ApShimWriteFrame say which refusal
       *   it was: not associated, no key, the BAR0 window moved, or the frame was malformed. */
      case EnetHAL_Write:
        if(!enet) return paramErr;
        if(!enet->ioBuffer || !enet->ioReqCount){
          if(enet) enet->ioResult = paramErr;
          return paramErr; }
        { OSStatus we = ApShimWriteFrame((const UInt8*)enet->ioBuffer,
                                         (UInt32)enet->ioReqCount);
          if(we == kApTxTryLater) we = ioErr;   /* k202: Apple's shim has no "later"; its old answer */
          enet->ioActCount = (we == noErr) ? enet->ioReqCount : 0;
          enet->ioResult   = we;
          return we; }

      case EnetHAL_SetMACAddress:
      default:
        if(gFirstBad < 0) gFirstBad = (SInt32)selector;
        if(enet) enet->ioResult = paramErr;
        return paramErr;
    }
}

/* ============================================================================================
 * ★★★ 8-58: NATIVE-MODULE BRIDGE (for ap_otmodl.c / OTModl$AirPortBCM).
 *
 * The native DLPI module and this radio HAL ship in ONE fragment, so the DLPI code drives the
 * radio through these thin, exported wrappers -- no cross-fragment call and no two-copy globals
 * ([[reference_os9_two_fragment_copies_own_globals]]).
 *
 * ⚠ ApOtmHalOpen's bring-up list is INTENTIONALLY identical to the sequence EnetHAL_Open runs
 *   (see the case above, "ApShimFindCard()" through "ApShimJoin()"). It is duplicated rather than
 *   shared because the native path does not enter the EnetHAL dispatch at all; the two are the
 *   same contract and must change together. The EnetHAL path is left byte-for-byte untouched so
 *   the app-driven witness builds keep working.
 * ==========================================================================================*/
/* ★★★ 8-69 k173: HARDEN BRING-UP against the intermittent (~30%) MMIO fault. k168 and k172 both crashed
 * on COLD boots reading a core register during bring-up -- a PCI target-abort on a core not yet out of
 * reset/fully powered reads back to MacsBug as an unmapped-memory exception (the SPROM at bar0+0x1000
 * never aborts, which is why it reads fine while a core register at bar0+0x14 faults). Model Apple's
 * OTModl$radio, which imports NativePowerMgrLib: force PCI power state D0 before touching registers,
 * re-assert memory+bus-master decode, and LOG the base/command/power state via NON-FAULTING config
 * space so the NEXT fault is finally diagnosable. Task level (Open) only. */
/* 8-69 k204: name every PCI status (config 0x06) error bit. Through k203 only 0x1000 was decoded, and it
 * was called Signaled Target Abort; that is 0x0800 (0x1000 is Received Target Abort), so the very bit a
 * bring-up target-abort latches would have printed nothing. A bare 0 means the read never happened: this
 * card always has bit 4 (capability list) set. */
static void ApShimSayPciStatErrs(unsigned long s)
{
    if(s == 0UL){ Say("      0x0000: status read failed (this card always sets bit 4)"); return; }
    if(s & 0x0800UL) Say("      \xE2\x9A\xA0 0x0800 Signaled Target Abort: the card aborted a read of ours (a bring-up fault latches this)");
    if(s & 0x1000UL) Say("      \xE2\x9A\xA0 0x1000 Received Target Abort: a DMA cycle the card started was target-aborted");
    if(s & 0x2000UL) Say("      \xE2\x9A\xA0 0x2000 Received Master Abort: a DMA cycle the card started found no target");
    if(s & 0x4000UL) Say("      \xE2\x9A\xA0 0x4000 Signaled System Error");
    if(s & 0x8000UL) Say("      \xE2\x9A\xA0 0x8000 Detected Parity Error");
    if(s & 0x0100UL) Say("      \xE2\x9A\xA0 0x0100 Master Data Parity Error");
    if(!(s & 0xF900UL)) Say("      no abort or error bit latched");
}
static void ApShimEnsurePowerAndLog(void)
{
    UInt16 cmd=0, stat=0, pm=0; UInt8 cp=0; int guard=0;
    Say("");
    Say("  [8-69] ★ PRE-BRING-UP HARDWARE STATE (config space, non-faulting)");
    SayH("    gBus.bar0     = ", (unsigned long)gBus.bar0, 8);
    SayH("    gBus.bar0Size = ", (unsigned long)gBus.bar0Size, 8);
    Say1("    FindCard MACok= ", (unsigned long)gShimMacOk);
    SayH("    gShimMapErr   = ", (unsigned long)(long)gShimMapErr, 8);
    /* 8-78 k184: backplane crystal/PLL power-up witness (SsbPowerOn ran at FindCard, before this). */
    SayH("    GPIO_IN @entry= ", (unsigned long)gBus.gpioIn, 8);
    if(gBus.gpioIn & 0x40UL) Say("      xtal bit 0x40 was ALREADY set -> power dance SKIPPED (this boot's clock was up)");
    else                     Say("      xtal bit 0x40 was CLEAR -> ran xtal+PLL dance + 6ms settle (this boot needed it)");
    SayH("    PCI stat pre-scrub = ", (unsigned long)gBus.stabortWas, 4);
    ApShimSayPciStatErrs((unsigned long)gBus.stabortWas);
    if(gBus.stabortWas & SSB_PCI_STABORT) Say("      -> 0x0800 scrubbed (write-1-to-clear); the next PCI status line shows whether it cleared");
    if(ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)0x04,&cmd)==noErr){
        SayH("    PCI command   = ", (unsigned long)cmd, 4);
        if((cmd & 0x0006)!=0x0006){
            Say("    \xE2\x9A\xA0 mem/master not both enabled -- asserting");
            (void)ExpMgrConfigWriteWord(&gBus.node,(LogicalAddress)0x04,(UInt16)(cmd|0x0006)); } }
    if(ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)0x06,&stat)!=noErr) stat=0;
    SayH("    PCI status    = ", (unsigned long)stat, 4);
    ApShimSayPciStatErrs((unsigned long)stat);
    if(stat & 0x0010){                              /* capabilities list present */
        UInt16 w=0;
        if(ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)0x34,&w)==noErr){
            cp=(UInt8)(w & 0xFC);
            while(cp && guard++ < 48){
                UInt16 c=0;
                if(ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)cp,&c)!=noErr) break;
                if((c & 0xFF)==0x01){                /* PCI Power Management capability */
                    UInt16 pmAddr=(UInt16)(cp+4);
                    if(ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)pmAddr,&pm)==noErr){
                        SayH("    PMCSR         = ", (unsigned long)pm, 4);
                        if((pm & 0x0003)!=0){
                            Say("    \xE2\x9A\xA0 card NOT in D0 -- forcing D0 (models Apple NativePowerMgrLib)");
                            (void)ExpMgrConfigWriteWord(&gBus.node,(LogicalAddress)pmAddr,(UInt16)(pm & ~0x0003U));
                            SsbSpinUs(12000);        /* PCI PM: >=10 ms to settle after a D-state change */
                            pm=0; (void)ExpMgrConfigReadWord(&gBus.node,(LogicalAddress)pmAddr,&pm);
                            SayH("    PMCSR after   = ", (unsigned long)pm, 4);
                        } else Say("    card already in D0"); }
                    break; }
                cp=(UInt8)((c>>8)&0xFC); } }
    } else Say("    no PCI capability list -- no PM cap to check");
}

/* ★★★ k199: CALIBRATE THE STOPWATCH, TEST THE FAST CIPHER ON THIS G4, AND TIME BOTH CIPHERS.
 * Called from ApOtmHalOpen at task level, BEFORE the join -- no key exists yet, so no frame can be
 * encrypted by an untested path. Once per load: the self-test is deterministic, and a re-open would
 * only repeat its answer (the result stays in gApAesSelfTest and is re-logged).
 *
 * The benchmark is the "before and after" in one boot: the same 1500-byte payload through
 * ap_aes.h's cipher (k198's, still unoptimised) and ap_aes_fast.h's. Best of N runs, so an interrupt
 * landing in one run does not count. ⚠ The CCM loops around the cipher are O2 in BOTH measurements
 * now, so "reference" slightly UNDERSTATES what k198 paid per frame; the cipher is the bulk of it. */
static unsigned long gAesBenchRefUs = 0, gAesBenchFastUs = 0;
static int           gCryptoProbed = 0;

static void ApTbCalibrate(void)
{
    UnsignedWide a, b;
    unsigned long t0, t1, us;
    Microseconds(&a); t0 = ApTbNow();
    do { Microseconds(&b); } while ((unsigned long)(b.lo - a.lo) < 20000UL);   /* ~20 ms */
    t1 = ApTbNow();
    us = (unsigned long)(b.lo - a.lo);
    gApTbPerMs = ((t1 - t0) * 1000UL) / us;     /* ~833k ticks * 1000 < 2^32 at ~42 MHz */
}

static unsigned long ApShimBenchCcm(const ApAesCtx *k, int runs)
{
    unsigned long best = 0xFFFFFFFFUL, t;
    ApU8 mic[CCMP_M];
    int i;
    for (i = 0; i < runs; i++) {
        t = ApTbNow();
        ApAesCcmEncryptCtx(k, gApAesStNonce, gApAesStAad, 22, gApAesStPlain, AP_AES_ST_LEN,
                           gApAesStOutB, mic);
        t = ApTbNow() - t;
        if (t < best) best = t;
    }
    return ApTbToUs(best);
}

static void ApShimCryptoProbe(void)
{
    ApAesCtx k;
    if (!gCryptoProbed) {
        gCryptoProbed = 1;
        ApTbCalibrate();
        (void)ApAesFastSelfTest();              /* sets gApAesSelfTest and gApAesFast; logged below */
        ApAesCtxInitMode(&k, gApAesStKey, 0);
        gAesBenchRefUs = ApShimBenchCcm(&k, 3);
        if (gApAesFast) { ApAesCtxInitMode(&k, gApAesStKey, 1); gAesBenchFastUs = ApShimBenchCcm(&k, 8); }
    }
    Say ("  [k199] \xE2\x98\x85 CRYPTO + STOPWATCH (task level, before the join installs a key)");
    Say1("    timebase ticks per ms (vs Microseconds; 0 = uncalibrated) ", gApTbPerMs);
    Say1("    fast AES self-test on THIS G4 (0 = pass; 1-6 = failed check) ",
         (unsigned long)(gApAesSelfTest < 0 ? 99 : gApAesSelfTest));
    Say (gApAesFast ? "    data path cipher: FAST (T-table, O2)"
                    : "    data path cipher: REFERENCE (byte-wise, as k198) -- the fast one did NOT pass here");
    Say1("    one 1500-byte CCMP encrypt, reference cipher (k198's), us ", gAesBenchRefUs);
    Say1("    one 1500-byte CCMP encrypt, fast cipher, us              ", gAesBenchFastUs);
}

/* ★★ k200: IS DMA MEMORY SLOW, AND BY HOW MUCH? k199 implied ~190 ns per access to the ring pages
 * (the 565 us walk; the 150-vs-73 us encrypt) though the pools are ordinary NewPtrSysClear +
 * LockMemory, which should be cacheable. This measures it instead of inferring it: the SAME loop over
 * an RX page (read only -- the engine may be filling it; nothing is written) and over a static buffer,
 * by byte and by word; and writes to the LAST TX buffer, only while nothing is in flight (before the
 * ring's first post), against the static buffer. Each figure is ns per access, loop overhead included
 * (the static figure IS the overhead). Task level, once, after the join and before the IRQ is armed.
 * READ: DMA ~ static -> cacheable, and k199's cost was instructions; DMA >> static -> every access is a
 * bus transaction, and every byte k200 no longer touches is the saving. */
static UInt8         gMemProbeStatic[2048] __attribute__((aligned(16)));
static unsigned long gMemRdDmaB, gMemRdStaB, gMemRdDmaW, gMemRdStaW;   /* ns per access */
static unsigned long gMemWrDmaB, gMemWrStaB, gMemWrDmaW, gMemWrStaW;
static int           gMemProbed = 0, gMemWrSkipped = 0;

static unsigned long ApMemTimeRd(const volatile UInt8 *p, int words)
{
    unsigned long t, sum = 0, i, pass;
    for (pass = 0; pass < 3; pass++) {               /* pass 0 warms; passes 1-2 are timed */
        t = ApTbNow();
        if (words) { const volatile ApCopyW *w = (const volatile ApCopyW *)p;
                     for (i = 0; i < 512UL; i++) sum += w[i]; }
        else       { for (i = 0; i < 2048UL; i++) sum += p[i]; }
        t = ApTbNow() - t;
        if (pass == 2) { gMemProbeStatic[0] ^= (UInt8)sum;  /* keep sum live */
                         return (ApTbToUs(t) * 1000UL) / (words ? 512UL : 2048UL); }
    }
    return 0;
}

static unsigned long ApMemTimeWr(volatile UInt8 *p, int words, unsigned long n)
{
    unsigned long t, i, pass;
    for (pass = 0; pass < 3; pass++) {
        t = ApTbNow();
        if (words) { volatile ApCopyW *w = (volatile ApCopyW *)p;
                     for (i = 0; i < n / 4UL; i++) w[i] = (ApCopyW)i; }
        else       { for (i = 0; i < n; i++) p[i] = (UInt8)i; }
        t = ApTbNow() - t;
        if (pass == 2) return (ApTbToUs(t) * 1000UL) / (words ? n / 4UL : n);
    }
    return 0;
}

static void ApShimDmaMemProbe(void)
{
    if (!gMemProbed && gApTbPerMs && gShimRxBufLog[0]) {
        gMemProbed = 1;
        gMemRdDmaB = ApMemTimeRd(gShimRxBufLog[0], 0);  gMemRdStaB = ApMemTimeRd(gMemProbeStatic, 0);
        gMemRdDmaW = ApMemTimeRd(gShimRxBufLog[0], 1);  gMemRdStaW = ApMemTimeRd(gMemProbeStatic, 1);
        if (gTxrReady && !gTxrUp && gTxr.used == 0) {   /* nothing in flight: the last buffer is ours */
            gMemWrDmaB = ApMemTimeWr(gTxrFrm[AP_TXR_BUFS - 1], 0, (unsigned long)AP_TXR_FRMMAX);
            gMemWrStaB = ApMemTimeWr(gMemProbeStatic, 0, (unsigned long)AP_TXR_FRMMAX);
            gMemWrDmaW = ApMemTimeWr(gTxrFrm[AP_TXR_BUFS - 1], 1, (unsigned long)AP_TXR_FRMMAX);
            gMemWrStaW = ApMemTimeWr(gMemProbeStatic, 1, (unsigned long)AP_TXR_FRMMAX);
        } else gMemWrSkipped = 1;
    }
    Say ("  [k200] \xE2\x98\x85 DMA MEMORY PROBE -- ns per access, loop included (static = the loop's own cost)");
    Say1("    read  byte: RX DMA page ", gMemRdDmaB); Say1("                static     ", gMemRdStaB);
    Say1("    read  word: RX DMA page ", gMemRdDmaW); Say1("                static     ", gMemRdStaW);
    if (gMemWrSkipped) Say("    write: SKIPPED (a TX frame was in flight)");
    else {
        Say1("    write byte: TX DMA buf  ", gMemWrDmaB); Say1("                static     ", gMemWrStaB);
        Say1("    write word: TX DMA buf  ", gMemWrDmaW); Say1("                static     ", gMemWrStaW);
    }
}

int ApOtmHalOpen(void)
{
    gOtmNativeMode = 1;      /* ★ 8-58: the RX pump now delivers up our DLPI stream, not the shim queue */
    Say1("  [8-58] \xE2\x98\x85 NATIVE OTModl$AirPortBCM -- HAL build ", (unsigned long)AP_SHIM_BUILD);
    {   /* 8-63 k166 freshness+result witness in the log the user reads */
        extern UInt32 gApOtmodlBuild;    /* AP_OTMODL_BUILD, set in ap_otmodl.c */
        extern UInt32 gOtmOnlineResult;  /* OTChangePortState(online) outcome, set in InitStreamModule */
        Say1("  [8-63] OTMODL build ", (unsigned long)gApOtmodlBuild);
        Say1("  [8-63] port-online result (0xAA55xx=called, 0xDEAD=unresolved, 0=uncalled) ", (unsigned long)gOtmOnlineResult);
    }
    ApShimCryptoProbe();     /* k199: the data path's cipher is chosen HERE, before any key exists */
    /* ⚠ SELF-ARM. gShimArmed gates the first hardware write (ApShimEnableCore -> permErr when
     * clear). In the shim path the boot vehicle arms the copy OT drives; in the native path OT
     * loads THIS fragment fresh and calls qopen here, so we arm our own copy. Correct because the
     * arm interlock is transitional -- it existed only while an app and the driver shared one card
     * (ap_shim.c:292); under the native module OT's copy is the sole owner. */
    (void)AirPortShimArm(1);
    ApShimFindCard();
    ApShimEnsurePowerAndLog();          /* 8-69 k173: D0 + re-assert decode + log the pre-bring-up state */
    ApShimDumpRingToFile();             /* flush it to disk BEFORE any risky core MMIO -- so a target-abort
                                         * (the ~30% crash) leaves the base/command/PMCSR on disk to read */
    if(!gShimMacOk){                     /* FindCard rejected the BAR (all-ones/all-zero MAC) -- the mapping
                                          * is bad; touching a core register now is the crash. Bail clean. */
        Say("  [8-69] \xE2\x9B\x94 bad BAR/MAC from FindCard -- aborting bring-up (no core MMIO, no crash)");
        ApShimDumpRingToFile();
        return -1;
    }
    ApShimScanBackplane();
    ApShimEnableCore();
    ApShimLoadFirmware();
    ApShimGpioInit();
    ApShimApplyInitvals();
    ApShimRadioOn();
    ApShimPhyInit();
    ApShimChipInitTail();
    ApShimCoreInitMid();              /* k194: b43_wireless_core_init's middle, b43's position */
    ApShimDmaInit();
    ApShimCoreInitTail();
    ApShimMacOn();
    ApShimTxInit();
    ApShimReadNodeName();
    ApShimJoin();
    ApShimDmaMemProbe();              /* k200: task level, before the IRQ is armed and before any data post */
    /* ★ 8-58: ARM THE INTERRUPT -- RX is wired now (the pump delivers via ApOtmRxInject). The ISR
     * machinery (ApShimSecondaryHandler) is proven safe (k89: 527 IRQs, machine survived); the only
     * new interrupt-reachable code is ApOtmRxInject (allocb+putq+memcpy -- no File Manager, no
     * NewPtr allocation, no config space), which the static audit covers. Without this nothing
     * delivers the DHCP reply that lands ~1 ms after OT transmits.
     * ⚠ ARMING AT OPEN = boot-time if TCP/IP is already set to our port. That is the SAME accepted
     *   risk the shim path carries (it arms at EnetHAL_Open); recovery is a Shift-boot to disable
     *   extensions. The alternative -- never arming -- cannot receive, so there is no safer option
     *   that still works. */
#if !AP_OTM_BIND_ONLY
    /* ★ k204: armed whenever the MAC is up, not only after a join. With no key the pump drops every data
     * frame it walks (no PTK: decrypt fails, counted), exactly as before the join -- but it also stamps our
     * AP's beacons, and those are how the link layer knows when a rejoin can succeed. */
    if(gShimMacEverOn && !gIrqArmed) (void)AirPortShimArmIrq();
#endif
    ApLinkStart(gJoined);             /* k204: the heartbeat + task pump; transmit held until joined */
    /* ★ FILE WITNESS. The native path does not go through EnetHAL_Open, which is where the shim
     * path flushes the in-memory log ring to "AirPort Driver Log". Flush it here so an ordinary
     * boot leaves a readable driver log proving the module ran and how far bring-up got (build 82,
     * [8-2b]..[8-9]). Task level (qopen), which is what ApShimDumpRingToFile requires. */
    ApShimDumpRingToFile();
#if AP_OTM_DIAG_NOJOIN
    /* ★ 8-61 (k164): DIAGNOSTIC -- succeed even if the radio did not JOIN this boot, so OT proceeds
     * to bind our stream and the snapshot pump can capture boundSAP regardless of the flaky radio
     * join intermittent. Safe because RX delivery is held off (AP_OTM_RX_DELIVER 0): OT may TX into
     * an unassociated radio (frames ignored by the AP) but nothing is handed up to corrupt OT.
     * ⚠ MUST be 0 for a real build -- there, no join means no link and qopen should fail. */
    return gShimMacEverOn ? 0 : -1;
#else
    return (gShimMacEverOn && gJoined) ? 0 : -1;
#endif
}

void ApOtmHalClose(void)
{
    ApLinkStop();                   /* k204: FIRST -- no heartbeat, no pending NM request into this fragment */
    gOtmNativeMode = 0;             /* stop native RX delivery before the radio goes down */
    (void)AirPortShimQuiesce();
    /* ★ k202: AND TAKE THE INTERRUPT BACK. Found reading the k200 shutdown crash (not its cause -- that
     * was ShareWay's SLP): through k201 this left ApShimIsr installed AND armed with GEN_IRQ_MASK set, so
     * once OT unloaded the fragment the next card interrupt would jump into freed code. The disarm
     * already existed for the app era ("a line left pointing at a fragment that has been unloaded is a
     * crash on the next interrupt"); the native path never called it. Mask first, then put the parent's
     * functions back. Task level: TerminateStreamModule is OT's unload call. */
    (void)AirPortShimDisarmIrq();
}

int ApOtmHalGetMac(unsigned char out[6])
{
    SInt32 e = 0;
    ApShimFindCard();
    (void)AirPortShimGetMac((UInt8 *)out, &e);
    return gShimMacOk ? 0 : -1;
}

/* 0 = on the ring; 1 = not now (ring full / transmit in progress: KEEP the frame, k202); -1 = dropped. */
int ApOtmHalTx(const unsigned char *frame, unsigned long len)
{
    OSStatus e = ApShimWriteFrame((const UInt8 *)frame, (UInt32)len);
    return (e == noErr) ? 0 : (e == kApTxTryLater) ? 1 : -1;
}

/* k196: a decimal right-aligned in a w-wide column, for the timeline's rows. */
static void ApCatW(Str255 L, unsigned long v, int w)
{
    Str255 t; int n, pad;
    t[0] = 0; PCatDec(t, v);
    pad = w - (int)t[0];
    while (pad-- > 0 && L[0] < 255) { L[0]++; L[L[0]] = ' '; }
    for (n = 1; n <= (int)t[0] && L[0] < 255; n++) { L[0]++; L[L[0]] = t[n]; }
}

/* ★ k196: print the receive timeline (ApTsMaybeSample) -- every sample not yet printed, oldest first;
 * counters as per-interval deltas. Task level (the snapshot). A second snapshot moments later (one per
 * stream close) prints only what is new, so the timeline appears once. */
static void ApTsPrint(void)
{
    UInt32 n = gTsN;              /* the secondary keeps sampling while this prints: stop at a fixed n */
    UInt32 first = gTsPrinted, k, t0us;
    if (n > AP_TS_N - 8u && first < n - (AP_TS_N - 8u)) first = n - (AP_TS_N - 8u);   /* 8 slots of slack */
    Say1("    RX timeline samples (1 s each) ", (unsigned long)n);
    if (first >= n) { Say("    (timeline: nothing new since the last snapshot)"); return; }
    Say("    RX TIMELINE (a gap in t = the secondary never ran; bcn = non-data frames, mostly beacons;");
    Say("                 fcs = frames that FAILED their FCS -- the radio took them in corrupted;");
    Say("                 a0/a1 = EVERY frame received, by the antenna it arrived on)");
    Say("      t(s)  fires  data   bcn  ofdm   fcs    a0    a1   txp   txs   ack Mbps in mx rxstatus cur bl");
    t0us = gTs[first % AP_TS_N].us;
    for (k = first; k < n; k++) {
        const ApTsSample *s = &gTs[k % AP_TS_N];
        const ApTsSample *p = (k > first) ? &gTs[(k - 1) % AP_TS_N] : s;
        UInt32 ms = (UInt32)(s->us - t0us) / 1000UL;
        Str255 L;
        L[0] = 0; PCat(L, "  ");
        ApCatW(L, ms / 1000UL, 6); PCat(L, "."); PCatDec(L, (ms % 1000UL) / 100UL);
        ApCatW(L, s->fires - p->fires, 7);
        ApCatW(L, s->rxData - p->rxData, 6);
        ApCatW(L, s->rxNonData - p->rxNonData, 6);
        ApCatW(L, s->rxOfdm - p->rxOfdm, 6);
        ApCatW(L, s->rxFcs - p->rxFcs, 6);
        ApCatW(L, s->ant0 - p->ant0, 6);
        ApCatW(L, s->ant1 - p->ant1, 6);
        ApCatW(L, s->txPosts - p->txPosts, 6);
        ApCatW(L, s->txStat - p->txStat, 6);
        ApCatW(L, s->txAcked - p->txAcked, 6);
        ApCatW(L, (s->rate < AP_TX_NRATES) ? kTxRates[s->rate].mbps : 0, 5);
        ApCatW(L, s->used, 3);
        ApCatW(L, s->fireMax, 3);
        PCat(L, " "); PCatHex(L, s->rxStatus, 8);
        ApCatW(L, s->cursor, 4);
        ApCatW(L, s->backlog, 3);
        Out(L);
    }
    gTsPrinted = n;
}

/* ★★★ 8-60: BLACK-BOX SNAPSHOT. The boot vehicle's NM pump calls this (task level) to dump the
 * native module's live RX/DLPI counters (ap_otmodl.c, same fragment) into "AirPort Driver Log".
 * It answers, without a debugger, exactly where the RX path stops and whether OT bound IP:
 *   enq>0 defer>0 inj>0 but rsrv==0  -> putq isn't scheduling our read-srv (OT never gets frames)
 *   inj>0 rsrv>0 putnext==0 canputNO>0 -> OT is flow-controlling / not accepting (upstream not ready)
 *   enq==0 -> the ISR never saw a frame (RX not reaching us); boundSAP==0 -> OT never bound IP. */
/* k199: one stopwatch histogram as two log lines -- count / average / max, then the buckets. */
static void ApTbHistSay(const char *name, const ApTbHist *h)
{
    static const char *kB[AP_TBH_NB] = { "<25:", "  <50:", "  <100:", "  <200:", "  <500:",
                                         "  <1ms:", "  <2ms:", "  <5ms:", "  >=5ms:" };
    Str255 L;
    int k;
    L[0] = 0; PCat(L, name); PCat(L, "  n "); PCatDec(L, h->n);
    PCat(L, "  avg "); PCatDec(L, h->n ? h->sumUs / h->n : 0UL);
    PCat(L, " us  max "); PCatDec(L, h->maxUs); PCat(L, " us");
    Out(L);
    L[0] = 0; PCat(L, "        ");
    for (k = 0; k < AP_TBH_NB; k++) { PCat(L, kB[k]); PCatDec(L, h->b[k]); }
    Out(L);
}

void ApOtmSnapshot(void)
{
    extern UInt32 gOtmEnq, gOtmDefer, gOtmInj, gOtmInjDrop, gOtmRsrv, gOtmPutnext, gOtmCanputF;
    extern UInt32 gOtmInfoReqs, gOtmBindReqs, gOtmUdataReqs, gOtmOtherReqs;
    extern UInt32 gOtmBoundSap, gOtmStateSnap, gOtmQDepthMax;
    extern UInt32 gOtmIoctls, gOtmLastIoc, gOtmLastIocCount, gOtmLastIocArg;   /* 8-71 k175 / 8-72 k176 */
    extern UInt32 gOtmLastOtherPrim, gFraming;                                 /* 8-73 k178 */
    extern UInt32 gIocCmd[6], gIocIn[6], gIocRval[6], gIocRingN;               /* 8-73 k178: ioctl ring */
    extern UInt32 gBindMaxConind, gBindSvcMode, gEvtRing[16], gEvtRingN;       /* 8-74 k179 */
    extern UInt32 gOtmNetChangeResult;                                         /* 8-79 k185 */
    extern UInt32 gOtmNetChangeSwitches;                                        /* k215: switch re-pushes */
    extern UInt32 gPortRefFound, gPortRefFromFind, gPortRefFromCreate, gPortFlag24; /* 8-80 k186 */
    extern UInt32 gQopenCount, gQcloseCount;                                   /* 8-81 k187: open/close lifecycle */
    Say ("  [8-60] ==== OTModl$AirPortBCM live snapshot ====");
    Say1("    qopen count   ", (unsigned long)gQopenCount);   /* 8-81 k187: 1 = OT only ENUMERATED our port; */
    Say1("    qclose count  ", (unsigned long)gQcloseCount);  /*           >1 = it re-opened to activate/bind */
    Say1("    RX enqueue    ", (unsigned long)gOtmEnq);
    Say1("    RX defer runs ", (unsigned long)gOtmDefer);
    Say1("    RX inject     ", (unsigned long)gOtmInj);
    Say1("    RX injDrop    ", (unsigned long)gOtmInjDrop);
    Say1("    RX qDepthMax  ", (unsigned long)gOtmQDepthMax);
    {   extern UInt32 gRxQDrops;                           /* k195: ISR->deferred-task queue overflow */
        Say1("    RX q drops    ", (unsigned long)gRxQDrops); }
    Say1("    read-srv runs ", (unsigned long)gOtmRsrv);
    Say1("    putnext UP    ", (unsigned long)gOtmPutnext);
    Say1("    canputnext=NO ", (unsigned long)gOtmCanputF);
    {   /* k190: the per-stream table -- which protocol each open stream bound and how many received
         * frames went up IT. k189's bug in one picture: every frame went up one stream, whatever its
         * protocol. A healthy DHCP boot shows an 0800 (IP) stream and an 0806 (ARP) stream, both > 0. */
        extern UInt32 gStrOpen[], gStrSap[], gStrState[], gStrRxUp[], gStrQd[], gStrMax;
        extern UInt32 gOtmNoStream, gOtmNoStreamType, gOtmRxQFull;
        UInt32 s;
        for (s = 0; s < gStrMax; s++) if (gStrOpen[s] || gStrRxUp[s]) {
            Say1("    stream slot   ", (unsigned long)s);
            SayH("      bound SAP   ", (unsigned long)gStrSap[s], 4);
            Say1("      DLPI state  ", (unsigned long)gStrState[s]);
            Say1("      open        ", (unsigned long)gStrOpen[s]);
            Say1("      RX up       ", (unsigned long)gStrRxUp[s]);
            Say1("      RX waiting  ", (unsigned long)gStrQd[s]);        /* k191: queued, not yet passed up */
        }
        Say1("    RX cap drops  ", (unsigned long)gOtmRxQFull);        /* k191: >0 = OT fell behind; we shed */
        Say1("    RX no-stream  ", (unsigned long)gOtmNoStream);
        SayH("    ...last type  ", (unsigned long)gOtmNoStreamType, 4);
    }
    {   /* k192: WHY frames were dropped inside the driver, by name -- the pump always counted this
         * (gDlpiWhy) but the native snapshot never printed it, which is how a 512-byte truncation hid
         * behind "RX works, 930 delivered". A healthy AFP run: "decrypt failed (pairwise)" ~0 and
         * "RX big delivered" > 0. */
        int w;
        Say ("    RX drop reasons (driver-side, before OT):");
        for (w = 0; w < AP_ENET_NREASON; w++)
            if (gDlpiWhy[w]) { Say(ApEnetWhy(w)); Say1("        frames    ", (unsigned long)gDlpiWhy[w]); }
        Say1("    RX max frame  ", (unsigned long)gRxMaxLen);         /* bytes, Ethernet */
        Say1("    RX big (>438) ", (unsigned long)gRxBigDelivered);   /* every one was dropped pre-k192 */
        Say1("    RX over-buffer", (unsigned long)gRxOverBuf);        /* header claimed > 2400: refused */
    }
    {   /* k193: the data-rate story, both directions. A good run: "TX rate now" well above 1, OFDM
         * rates with 1st-try ok >> failed, and "RX OFDM frames" > 0 (the AP sends OFDM to us now). */
        static const UInt8 kOfdmMbps[16] = { 0,0,0,0,0,0,0,0, 48,24,12,6, 54,36,18,9 };
        UInt32 r, c, now = (gTxRateIdx < AP_TX_NRATES) ? gTxRateIdx : 0;
        Say1("    TX rate now Mb", (unsigned long)kTxRates[now].mbps);
        Say1("    TX steps up   ", (unsigned long)gTxRateUp);
        Say1("    TX steps down ", (unsigned long)gTxRateDown);
        for (r = 0; r < AP_TX_NRATES; r++)
            if (gTxRateSent[r] | gTxRateOk1[r] | gTxRateRetryOk[r] | gTxRateFail[r]) {
                Say1("    TX at Mbps    ", (unsigned long)kTxRates[r].mbps);
                Say1("      sent        ", (unsigned long)gTxRateSent[r]);
                Say1("      1st-try ok  ", (unsigned long)gTxRateOk1[r]);
                Say1("      retried ok  ", (unsigned long)gTxRateRetryOk[r]);
                Say1("      failed      ", (unsigned long)gTxRateFail[r]);
            }
        Say1("    TX tries=1    ", (unsigned long)gTxFcHist[1]);
        Say1("    TX tries=2    ", (unsigned long)gTxFcHist[2]);
        Say1("    TX tries=3    ", (unsigned long)gTxFcHist[3]);
        Say1("    TX tries>=4   ", (unsigned long)gTxFcHist[4]);
        Say1("    TX never sent ", (unsigned long)gTxFcHist[0]);
        /* ★ k195: the TX ring. A good run: posts ~= TX sent, full / bad-ptr / stopped all 0, restarts
         * = 1 (the one after the join), and occupancy past "0" -- which is k194's overwrite, measured:
         * each post that found a frame still in flight is one k194 would have destroyed. */
        {   static const char *kOcc[6] = { "      0 in flight ", "      1 in flight ", "      2-3        ",
                                           "      4-7        ", "      8-15       ", "      16+        " };
            UInt32 k;
            Say1("    TX ring posts ", (unsigned long)gTxrPosts);
            Say1("    TX ring starts", (unsigned long)gTxrStarts);    /* 1 per join is normal */
            Say1("    ...start fail ", (unsigned long)gTxrStartFail);
            Say1("    ...reset fail ", (unsigned long)gTxrResetFail);
            SayH("    ...start stat ", (unsigned long)gTxrStartStatus, 8);   /* TXDPTR should be 0 */
            Say1("    TX ring full  ", (unsigned long)gTxrFull);      /* frames refused: 32 in flight */
            Say1("    TX bad pointer", (unsigned long)gTxrBadPtr);
            SayH("    ...bad status ", (unsigned long)gTxrBadStatus, 8);
            Say1("    TX engine stop", (unsigned long)gTxrStopped);
            SayH("    ...stop status", (unsigned long)gTxrStopStatus, 8);
            SayH("    ...first error", (unsigned long)gTxrErrStatus, 8);    /* TXERROR bits 16-19 */
            Say1("    TX most in fly", (unsigned long)gTxrUsedMax);
            Say ("    TX occupancy at post (frames already in flight):");
            for (k = 0; k < 6; k++) Say1(kOcc[k], (unsigned long)gTxrOccHist[k]);
        }
        {   static const char *kFire[6] = { "      0 frames   ", "      1          ", "      2-4        ",
                                            "      5-8        ", "      9-16       ", "      17+        " };
            UInt32 k;
            Say ("    RX frames per interrupt (5+ = k194 would have stranded some):");
            for (k = 0; k < 6; k++) Say1(kFire[k], (unsigned long)gRxFireHist[k]);
            Say1("    RX most in one", (unsigned long)gRxFireMax);
            Say1("    TX stat most/drain", (unsigned long)gTxStatFireMax);
        }
        /* ★ k196: interrupt-path health and every way a transmit is refused -- k195's outage could not
         * be placed because none of these reached the native snapshot. */
        Say1("    IRQ hard      ", (unsigned long)gIrqHard);
        Say1("    IRQ not ours  ", (unsigned long)gIrqNotOurs);
        Say1("    IRQ queued    ", (unsigned long)gIrqQueued);
        Say1("    IRQ queue FAIL", (unsigned long)gIrqQueueFail);      /* > 0: mask left at 0, RX stalls */
        Say1("    IRQ secondary ", (unsigned long)gIrqSecondary);
        Say1("    IRQ window BAD", (unsigned long)gIrqWindowBad);      /* > 0: the handler found a foreign core */
        Say1("    IRQ braked    ", (unsigned long)gIrqBraked);
        Say1("    TX calls      ", (unsigned long)gTxCalls);
        Say1("    TX sent       ", (unsigned long)gTxSent);
        Say1("    TX open (plain)", (unsigned long)gTxOpenFrames);   /* k217: open-network frames */
        Say1("    TX refused: re-entry  ", (unsigned long)gTxBusyRefused);
        Say1("    TX refused: window    ", (unsigned long)gTxWindowBad);
        Say1("    TX refused: not assoc ", (unsigned long)gTxNotAssoc);
        Say1("    TX refused: no key    ", (unsigned long)gTxNoKey);
        Say1("    TX refused: bad frame ", (unsigned long)gTxBadFrame);
        Say1("    RX CCK frames ", (unsigned long)gRxCck);
        {   static const char *kCck[5] = { "      CCK 1 Mbps  ", "      CCK 2 Mbps  ", "      CCK 5.5 Mbps",
                                           "      CCK 11 Mbps ", "      CCK other   " };
            UInt32 k;
            for (k = 0; k < 5; k++) if (gRxCckRate[k]) Say1(kCck[k], (unsigned long)gRxCckRate[k]);
        }
        Say1("    RX OFDM frames", (unsigned long)gRxOfdm);
        for (c = 0; c < 16; c++)
            if (gRxOfdmRate[c] && kOfdmMbps[c]) {
                Say1("      OFDM Mbps   ", (unsigned long)kOfdmMbps[c]);
                Say1("        frames    ", (unsigned long)gRxOfdmRate[c]);
            }
        /* ★ k197: RECEIVE QUALITY. Read it against the good-frame counts above: many FCS failures at
         * the AP's OFDM rates, concentrated in the > 1000-byte bucket, is the radio failing to take
         * big frames the AP really sent -- the air side of k196's size-dependent outage. Few or none,
         * with an outage in the timeline, means the frames never reached our receiver at all. */
        {   static const char *kLen[3] = { "      <= 200 B    ", "      201-1000 B  ", "      > 1000 B    " };
            static const char *kCckN[5] = { "      CCK 1 Mbps  ", "      CCK 2 Mbps  ", "      CCK 5.5 Mbps",
                                            "      CCK 11 Mbps ", "      CCK other   " };
            UInt32 k;
            Say ("    RECEIVE QUALITY (k197)");
            Say1("    KEEP_BAD on   ", (unsigned long)gRxKeepBadOn);
            Say1("    FCS FAILED    ", (unsigned long)gRxFcsAny);
            Say1("    ...from our AP", (unsigned long)gRxFcsFromAp);     /* addr2 may be corrupt: a floor */
            for (c = 0; c < 16; c++)
                if (gRxFcsOfdm[c] && kOfdmMbps[c]) {
                    Say1("      bad OFDM Mbps ", (unsigned long)kOfdmMbps[c]);
                    Say1("        frames      ", (unsigned long)gRxFcsOfdm[c]);
                }
            for (k = 0; k < 5; k++) if (gRxFcsCck[k]) { Say(kCckN[k]); Say1("        bad frames  ", (unsigned long)gRxFcsCck[k]); }
            Say ("    FCS failed by length / good data by length:");
            for (k = 0; k < 3; k++) {
                Str255 L; L[0] = 0; PCat(L, kLen[k]); PCat(L, " bad "); PCatDec(L, gRxFcsLen[k]);
                PCat(L, "   good "); PCatDec(L, gRxGoodLen[k]); Out(L); }
            {   Str255 L; L[0] = 0; PCat(L, "    antenna 0/1: good "); PCatDec(L, gRxGoodAnt[0]); PCat(L, "/");
                PCatDec(L, gRxGoodAnt[1]); PCat(L, "   bad "); PCatDec(L, gRxFcsAnt[0]); PCat(L, "/");
                PCatDec(L, gRxFcsAnt[1]); PCat(L, "   every frame "); PCatDec(L, gRxWalkAnt[0]); PCat(L, "/");
                PCatDec(L, gRxWalkAnt[1]); Out(L); }                           /* k198 */
            Say1("    unicast good, we ACKed     ", (unsigned long)gRxRespSet);
            Say1("    unicast good, NO ACK sent  ", (unsigned long)gRxRespClear);
            Say ("    JSSI (raw signal byte) in 32-wide buckets, 0-31 .. 224-255:");
            {   Str255 L; L[0] = 0; PCat(L, "      good OFDM ");
                for (k = 0; k < 8; k++) { PCatDec(L, gRxJssiGoodOfdm[k]); PCat(L, " "); } Out(L); }
            {   Str255 L; L[0] = 0; PCat(L, "      good CCK  ");
                for (k = 0; k < 8; k++) { PCatDec(L, gRxJssiGoodCck[k]); PCat(L, " "); } Out(L); }
            {   Str255 L; L[0] = 0; PCat(L, "      FCS-bad   ");
                for (k = 0; k < 8; k++) { PCatDec(L, gRxJssiBad[k]); PCat(L, " "); } Out(L); }
        }
    }
    Say1("    DL_INFO_REQ   ", (unsigned long)gOtmInfoReqs);
    Say1("    DL_BIND_REQ   ", (unsigned long)gOtmBindReqs);
    Say1("    bound SAP     ", (unsigned long)gOtmBoundSap);
    Say1("    DL_UD_REQ(TX) ", (unsigned long)gOtmUdataReqs);
    Say1("    other req     ", (unsigned long)gOtmOtherReqs);
    Say1("    M_IOCTL count ", (unsigned long)gOtmIoctls);          /* 8-71 k175 */
    SayH("    last ioc_cmd  ", (unsigned long)gOtmLastIoc, 8);      /* 8-71 k175: 0x4f02 etc. */
    SayH("    last ioc_count", (unsigned long)gOtmLastIocCount, 8); /* 0xFFFFFFFF = transparent */
    SayH("    ioc rval echo ", (unsigned long)gOtmLastIocArg, 8);   /* 8-72 k176: value echoed to OT */
    Say1("    DLPI state    ", (unsigned long)gOtmStateSnap);
    /* 8-73 k178: name the exact gate. last OTHER prim = the DLPI primitive we answered with
     * DL_ERROR_ACK (the unhandled "other req"); the ring lists every M_IOCTL cmd/in/rval so we can
     * see if OT sent 0x6c0a (DL_IOC_HDR_INFO) or 0x4f03 that we are not handling like Apple. */
    SayH("    last OTHER prim ", (unsigned long)gOtmLastOtherPrim, 8);
    SayH("    framing 0x4f02  ", (unsigned long)gFraming, 8);
    SayH("    net-change push ", (unsigned long)gOtmNetChangeResult, 8);   /* 8-79 k185: 0xBB66|rr = link-up signaled */
    Say1("    net-change switches (k215) ", (unsigned long)gOtmNetChangeSwitches);   /* re-pushes on a network switch */
    /* 8-80 k186: the port-ref discriminator. If findRef != createRef, our old recomputed ref was wrong. */
    Say1("    portRef found?  ", (unsigned long)gPortRefFound);
    SayH("    portRef (find)  ", (unsigned long)gPortRefFromFind, 8);
    SayH("    portRef (create)", (unsigned long)gPortRefFromCreate, 8);
    SayH("    port flag 0x24  ", (unsigned long)gPortFlag24, 8);
    Say1("    ioctls seen     ", (unsigned long)gIocRingN);
    {
        UInt32 i, n = (gIocRingN < 6) ? gIocRingN : 6;
        UInt32 base = (gIocRingN < 6) ? 0 : gIocRingN;   /* walk oldest -> newest */
        for (i = 0; i < n; i++) {
            UInt32 s = (base + i) % 6;
            SayH("      ioc cmd/in/rval ", (unsigned long)gIocCmd[s], 8);
            SayH("              in      ", (unsigned long)gIocIn[s], 8);
            SayH("              rval    ", (unsigned long)gIocRval[s], 8);
        }
    }
    /* 8-74 k179: DL_BIND_REQ contents (explains the bound SAP) + the full event ORDER. Event codes:
     * 0x50xx xxxx = 'P'|DLPI prim, 0x4943 xxxx = 'IC'|ioctl cmd, 0x4400 0000 = M_DATA, 0x3Fxx = other. */
    SayH("    bind max_conind ", (unsigned long)gBindMaxConind, 8);
    SayH("    bind svc_mode   ", (unsigned long)gBindSvcMode, 8);
    Say1("    events seen     ", (unsigned long)gEvtRingN);
    {
        UInt32 i, n = (gEvtRingN < 16) ? gEvtRingN : 16;
        UInt32 base = (gEvtRingN < 16) ? 0 : gEvtRingN;   /* oldest -> newest */
        for (i = 0; i < n; i++) SayH("      evt ", (unsigned long)gEvtRing[(base + i) % 16], 8);
    }
    {   /* ★★★ k199: PER-FRAME COST. Where the ~3-4 ms per frame k198's capture implied actually goes.
         * Read TX total against TX encrypt, and RX convert against RX decrypt: the difference is
         * everything that is not the cipher. The two waits are OT's scheduling, not our code. */
        extern ApTbHist gTbRxQWait, gTbDtRun, gTbRsrvWait;   /* ap_otmodl.c */
        Say ("    PER-FRAME COST (k199/k200; microseconds, from the G4 timebase)");
        Say1("    timebase ticks/ms  ", gApTbPerMs);
        Say1("    fast AES self-test (0 = pass, 99 = never ran) ",
             (unsigned long)(gApAesSelfTest < 0 ? 99 : gApAesSelfTest));
        Say (gApAesFast ? "    data path cipher: FAST" : "    data path cipher: REFERENCE");
        Say1("    bench, 1500-byte CCMP encrypt: reference us ", gAesBenchRefUs);
        Say1("    bench, 1500-byte CCMP encrypt: fast us      ", gAesBenchFastUs);
        Say1("    JOIN (k203): attempts this boot            ", (unsigned long)gJoinTries);
        Say1("      message 2 posted / ACKed by the AP       ", (unsigned long)gShimM2Posts);
        Say1("                                               ", (unsigned long)gShimM2AckedN);
        Say1("      retried message 1s answered              ", (unsigned long)gShimM1Retries);
        {   extern UInt32 gOtmTxQueued, gOtmTxWsrv, gOtmTxQenable, gOtmTxQDrop, gOtmTxDropped, gOtmTxQMaxBytes;
            Say ("    TX BACK-PRESSURE (k202): a full ring now KEEPS the frame on the write queue");
            Say1("      frames kept (were dropped through k201) ", (unsigned long)gOtmTxQueued);
            Say1("      write-service runs                     ", (unsigned long)gOtmTxWsrv);
            Say1("      wakeups (qenable from the DT)          ", (unsigned long)gOtmTxQenable);
            Say1("      deepest write queue, bytes             ", (unsigned long)gOtmTxQMaxBytes);
            Say1("      dropped: queue past its cap            ", (unsigned long)gOtmTxQDrop);
            Say1("      dropped: permanent refusal             ", (unsigned long)gOtmTxDropped);
            Say1("      ring-full refusals (all now kept)      ", (unsigned long)gTxrFull); }
        ApTbHistSay("    RX held for the engine -> taken", &gTbRxDeferWait);
        Say1("    RX slot left: engine still writing (k201) ", (unsigned long)gRxBeingWritten);
        Say1("      ...pointer moved off within the settle  ", (unsigned long)gRxBeingWrittenMoved);
        Say1("      ...ring LAPPED (next slot unread)       ", (unsigned long)gRxLapSeen);
        Say1("    RX frame_len 0: appeared within 10 us     ", (unsigned long)gRxLenZeroSettled);
        Say1("                    still 0 (copied whole)    ", (unsigned long)gRxLenZeroStill);
        Say1("    DMA probe ns/access: RX page read byte ", gMemRdDmaB);
        Say1("                         static read byte  ", gMemRdStaB);
        Say1("                         RX page read word ", gMemRdDmaW);
        Say1("                         TX buf write byte ", gMemWrDmaB);
        Say1("                         static write byte ", gMemWrStaB);
        ApTbHistSay("    TX total (OT call -> doorbell)", &gTbTxTotal);
        ApTbHistSay("    TX CCMP encrypt (into cache)  ", &gTbTxEncap);
        ApTbHistSay("    TX copy staged -> ring (k200) ", &gTbTxCopy);
        ApTbHistSay("    RX ring walk + copy           ", &gTbRxWalk);
        ApTbHistSay("    RX copy out of DMA (k200)     ", &gTbRxCopy);
        ApTbHistSay("    RX convert (incl. decrypt)    ", &gTbRxConv);
        ApTbHistSay("    RX CCMP decrypt               ", &gTbRxDecrypt);
        ApTbHistSay("    RX ISR -> deferred-task wait  ", &gTbRxQWait);
        ApTbHistSay("    RX deferred-task run          ", &gTbDtRun);
        ApTbHistSay("    RX putq -> read-srv wait      ", &gTbRsrvWait);
        ApTbHistSay("    secondary-interrupt run       ", &gTbSecRun);
    }
    {   /* ★★★ k204: THE LINK LAYER -- did it notice anything, and did its machinery run? */
        Str255 L;
        Say ("    LINK (k204): lost-link detection and automatic rejoin");
        L[0] = 0; PCat(L, "      state now                               "); PCat(L, gLink.up ? "UP" : "DOWN"); Out(L);
        L[0] = 0; PCat(L, "      latest outage                           "); PCat(L, ApLinkWhyName(gLink.why));
        if(gLink.why == AP_LINK_WHY_DEAUTH || gLink.why == AP_LINK_WHY_DISASSOC){
            PCat(L, ", reason "); PCatDec(L, (unsigned long)gLink.reason); }
        Out(L);
        Say1("      outages                                 ", (unsigned long)gLink.downs);
        Say1("      rejoin attempts / rejoins               ", (unsigned long)gLink.tries);
        Say1("                                              ", (unsigned long)gLink.rejoins);
        Say1("      deauths / disassocs received            ", (unsigned long)gLinkKicks[0]);
        Say1("                                              ", (unsigned long)gLinkKicks[1]);
        Say1("      beacons from our AP                     ", (unsigned long)gLinkBcnCount);
        Say1("        latest, ms ago                        ",
             (unsigned long)(ApLinkAge(gLinkTick, gLinkLastBcnTick) * AP_LINK_TICK_MS));
        Say1("      EAPOL after the join: group rekey msg 1 ", (unsigned long)gLinkEapolN[AP_LINK_EAPOL_GROUP_M1]);
        Say1("                            pairwise msg 1    ", (unsigned long)gLinkEapolN[AP_LINK_EAPOL_PAIR_M1]);
        Say1("                            pairwise msg 3    ", (unsigned long)gLinkEapolN[AP_LINK_EAPOL_PAIR_M3]);
        Say1("                            other             ", (unsigned long)gLinkEapolN[AP_LINK_EAPOL_OTHER]);
        Say1("      heartbeat runs (4/s)                    ", (unsigned long)gLinkTimerRuns);
        Say1("        re-arm failures / revived by the RX   ", (unsigned long)gLinkTimerFails);
        Say1("                                              ", (unsigned long)gLinkTimerRevived);
        Say1("      task pump posted / ran / post failed    ", (unsigned long)gLinkNmArmed);
        Say1("                                              ", (unsigned long)gLinkNmFired);
        Say1("                                              ", (unsigned long)gLinkNmFailed);
        Say1("        still posted now (1 = waiting)        ", (unsigned long)gLinkNmPosted);
        Say1("        posted, ms ago                        ",
             (unsigned long)(ApLinkAge(gLinkTick, gLinkLastPostTick) * AP_LINK_TICK_MS));
        Say1("        not task level / nested (both 0)      ", (unsigned long)gLinkNotTask);
        Say1("                                              ", (unsigned long)gLinkNested);
        Say1("        post -> run, max ms                   ", (unsigned long)(gLinkLatMaxUs / 1000u));
        Say1("        body, max ms                          ", (unsigned long)(gLinkBodyMaxUs / 1000u));
        Say1("      rejoin attempt, max / latest ms         ", (unsigned long)(gLinkRejoinMaxUs / 1000u));
        Say1("                                              ", (unsigned long)(gLinkRejoinLastUs / 1000u));
        Say1("      frames held while down (try later)      ", (unsigned long)gTxLinkDown);
        Say1("      receive interrupt resumes / refused     ", (unsigned long)gIrqResumes);
        Say1("                                              ", (unsigned long)gIrqResumeRefused);
        Say1("      log: flushes held while down            ", (unsigned long)gLinkFlushHeld);
        Say1("           ring recycled (all on disk)        ", (unsigned long)gLogRecycles);
        Say1("           link events lost (ring lapped)     ", (unsigned long)gLinkEvtLost);
        if(gApStat){                      /* k205: exactly what the control panel and strip module read */
            SayH("      STATUS BLOCK (k205, Gestalt 'APXe') at   ", (unsigned long)gApStat, 8);
            Say1("        alive / linkState / seq             ", (unsigned long)gApStat->alive);
            Say1("                                              ", (unsigned long)gApStat->linkState);
            Say1("                                              ", (unsigned long)gApStat->seq);
            Say1("        quality (0 out of range .. 4 strong) ", (unsigned long)gApStat->quality);
            Say1("        signal, -dBm                        ", (unsigned long)(-(long)gApStat->signalDbm));
            Say1("        heartbeat                           ", (unsigned long)gApStat->heartbeat);
            Say1("        command seq / ack seq / result      ", (unsigned long)gApStat->cmdSeq);
            Say1("                                              ", (unsigned long)gApStat->ackSeq);
            Say1("                                              ", (unsigned long)gApStat->ackResult);
        } else Say("      STATUS BLOCK (k205): NOT PUBLISHED -- the UI shows no driver");
        Say ("    POWER (k207): AirPort off / on, from the panel and the strip through the block's commands");
        Say1("      AirPort now (1 = on)                    ", (unsigned long)gRadioPowerOn);
        Say1("      turned off / turned on                  ", (unsigned long)gPowerOffs);
        Say1("                                              ", (unsigned long)gPowerOns);
        Say1("      goodbyes (deauth, reason 3) ACKed       ", (unsigned long)gPowerByeAcked);
        Say1("      MAC not reported suspended / no core    ", (unsigned long)gPowerMacStuck);
        Say1("                                              ", (unsigned long)gPowerSelFails);
        SayH("      RFOVER:RFOVERVAL after off (| 008C / & FF73) ", (unsigned long)gRfoverOff, 8);
        SayH("      RFOVER:RFOVERVAL after on (restored)          ", (unsigned long)gRfoverOn, 8);
        Say1("      commands taken / unknown                ", (unsigned long)gCmdTaken);
        Say1("                                              ", (unsigned long)gCmdUnknown);
        Say1("      latest command / argument / result      ", (unsigned long)gCmdLast);
        Say1("                                              ", (unsigned long)gCmdLastArg);
        Say1("                                              ", (unsigned long)gCmdLastResult);
        {   /* ★★★ k208: the group key renewals. Lengths and counts only -- never a key. */
            int k;
            Say ("    GROUP KEYS (k208): the AP's renewals, answered in the receive pump");
            Say1("      message 1 received / accepted           ", (unsigned long)gGkM1N);
            Say1("                                              ", (unsigned long)gGkOk);
            Say1("        replaced while queued / still queued  ", (unsigned long)gGkM1Replaced);
            Say1("                                              ", (unsigned long)gGkM1Pending);
            Say1("        accepted again, same key (AP resent)  ", (unsigned long)gGkSame);
            Say1("        key index changes                     ", (unsigned long)gGkKidChanges);
            Say1("        latest key index (4 = none yet)       ", (unsigned long)gGkLastKid);
            Say1("        latest accepted, s after heartbeat 0  ", (unsigned long)(gGkLastTick / 4u));
            for(k = 1; k < AP_GK_NRESULT; k++) if(gGkWhy[k]){
                L[0] = 0; PCat(L, "        REFUSED "); PCatDec(L, (unsigned long)gGkWhy[k]);
                PCat(L, "x: "); PCat(L, ApGkResultName(k)); Out(L); }
            Say1("      message 2 sent / held / failed          ", (unsigned long)gGkM2Sent);
            Say1("                                              ", (unsigned long)gGkM2Held);
            Say1("                                              ", (unsigned long)gGkM2Failed);
            Say1("        retries / abandoned / still held      ", (unsigned long)gGkM2Retries);
            Say1("                                              ", (unsigned long)gGkM2Abandoned);
            Say1("                                              ", (unsigned long)gGkM2Pending);
            SayH("        last send error                       ", (unsigned long)gGkM2LastErr, 8);
            L[0] = 0; PCat(L, "      key slots 0/1/2/3, length (0 = empty)   ");
            for(k = 0; k < 4; k++){ if(k) PCat(L, " / "); PCatDec(L, (unsigned long)gGtkTab.len[k]); }
            Out(L);
            L[0] = 0; PCat(L, "      group frames by key index: delivered    ");
            for(k = 0; k < 4; k++){ if(k) PCat(L, " / "); PCatDec(L, (unsigned long)gRxGkOk[k]); }
            Out(L);
            L[0] = 0; PCat(L, "                                 no key there ");
            for(k = 0; k < 4; k++){ if(k) PCat(L, " / "); PCatDec(L, (unsigned long)gRxGkNoKey[k]); }
            Out(L);
            L[0] = 0; PCat(L, "                                 bad decrypt  ");
            for(k = 0; k < 4; k++){ if(k) PCat(L, " / "); PCatDec(L, (unsigned long)gRxGkBad[k]); }
            Out(L);
            Say1("      transmits refused, transmitter busy     ", (unsigned long)gTxBusyRefused);
        }
        {   /* ★★★ k209: the network-list scan. COUNTS ONLY -- the names and addresses the scan heard are
             * private ([[feedback_airport_scrub_before_public]]) and stay in the fragment; only the count. */
            Say ("    SCAN (k209): the network list, and channel agility for the join");
            Say1("      scans run / published                    ", (unsigned long)gScanRuns);
            Say1("                                              ", (unsigned long)gScanPublished);
            Say1("      could not select the core (abandoned)    ", (unsigned long)gScanFail);
            Say1("      networks in the latest list              ", (unsigned long)gScanTab.n);
            Say1("        heard, all channels (pre-dedup)        ", (unsigned long)gScanHeard);
            Say1("        over 32, dropped                       ", (unsigned long)gScanTab.dropped);
            Say1("      latest scan took ms                      ", (unsigned long)gScanLastMs);
            Say1("      join found our net by scanning           ", (unsigned long)gScanFoundOurs);
            Say1("        and adopted a new channel              ", (unsigned long)gScanChanAdopted);
            Say1("      home channel now                         ", (unsigned long)gShimChannel);
            if(gApScan){
                SayH("      LIST BLOCK (k209, Gestalt 'APXs') at     ", (unsigned long)gApScan, 8);
                Say1("        scanSeq / count published            ", (unsigned long)gApScan->scanSeq);
                Say1("                                              ", (unsigned long)gApScan->count);
            } else Say("      LIST BLOCK (k209): NOT PUBLISHED");
        }
        {   /* ★★★ k210: the known networks and runtime join. NO NAMES, NO KEYS -- counts and the target's
             * length only; the PMK and the SSID are the user's and never enter the log. */
            Say ("    KNOWN NETWORKS (k210): runtime join, no compiled passphrase");
            Say1("      Preferences file: loaded (1 = yes)        ", (unsigned long)gKnownLoaded);
            Say1("        known networks on file                  ", (unsigned long)gKnownCount);
            Say1("        file absent (first run) / read errors   ", (unsigned long)gKnownNoFile);
            Say1("                                              ", (unsigned long)gKnownLoadErr);
            Say1("      have a runtime target (1 = yes)           ", (unsigned long)gHaveTarget);
            Say1("        target SSID length / security kind      ", (unsigned long)gTargetLen);
            Say1("                                              ", (unsigned long)gTargetSec);
            Say1("      join commands / no pick / joined          ", (unsigned long)gJoinCmds);
            Say1("                                              ", (unsigned long)gJoinCmdNoPick);
            Say1("                                              ", (unsigned long)gJoinCmdOk);
            Say1("      auto-join found target by scan            ", (unsigned long)gScanFoundOurs);
        }
    }
    ApTsPrint();
    ApShimDumpRingToFile();
}
