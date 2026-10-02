/* airport_rx.c -- STAGE 5-3 + 5-4 (k7): enable the MAC and catch a frame.
 *
 * ★★★★★ WHAT k3 GOT WRONG, BECAUSE IT IS THE WHOLE REASON k4 EXISTS.
 * k3 ran this same sweep and caught nothing on any channel, with RXERROR clean and RXDPTR frozen
 * at zero. Its own diagnosis list named the answer -- "the MAC is not handing frames to the DMA
 * engine, look at MACCTL and the PHY" -- and the cause was a scoping error in k3 itself:
 *
 *     ap_bringup.h runs b43_chip_init statements 1 through 6 AND STOPS.
 *     Statement 7 is b43_phy_init. Its own header says so in as many words.
 *
 * So k3 enabled a MAC whose PHY had never been initialised, whose RX antenna had never been
 * selected, and whose MAC/PHY clock had never been turned on. Nothing could have arrived. The
 * null result carried no information about the receiver.
 *
 * k4 closes both halves of that gap:
 *   1. ap_phy_initg.h -- Stage 4's entire b43_phy_initg, extracted MECHANICALLY from the j7
 *      probe and diff-verified, so it is callable instead of trapped inside another main().
 *   2. ChipInitTail() -- b43_chip_init statements 8 through 13, which no probe has ever run:
 *      interference mitigation, RX and TX antenna selection, PRMAXTIME, b43_adjust_opmode,
 *      the MAC/PHY clock, and the scalar writes between them.
 *
 * ★★★★★ WHAT k4 GOT WRONG -- THE SAME ERROR CLASS AS k3, ONE LEVEL DEEPER.
 * k4 ran Stage 4's PHY init and chip_init's tail and STILL heard nothing, with RXERROR clean
 * and RXDPTR frozen at zero across 1, 6 and 11. Two causes, both found by reading rather than
 * guessing, and neither of them a Stage 4 defect.
 *
 * 1. b43_phy_init DOES NOT END AT ops->init. k3 stopped at chip_init statement 6 when statement
 *    7 was b43_phy_init; k4 then ported ops->init when the ENCLOSING function was b43_phy_init.
 *    Same habit failure twice. phy_common.c:
 *        switch_analog(true);            <- ApBringUp
 *        b43_software_rfkill(false);     <- ApBringUp
 *        err = ops->init(dev);           <- ApPhyInitG  (all k4 had)
 *        err = b43_switch_channel(dev, phy->channel);   <- NEVER RUN BY ANY PROBE
 *    and b43_switch_channel is not b43_gphy_channel_switch. It adds two things:
 *        b43_shm_write16(SHARED, B43_SHM_SH_CHAN, cookie);  "to prevent the firmware from
 *                                                            sending ghost packets"
 *        msleep(8);                                        "wait for the radio to ... stabilize"
 *    ⚠ SHM 0x00A0 HAS NEVER BEEN WRITTEN BY ANY PROBE IN THIS PROJECT -- verified by grep. The
 *    microcode reads that word to know what channel it is on, and it has always read whatever
 *    the initvals left. k4's sweep also retuned with zero settle time.
 *
 * 2. THE MAC WAS FILTERING BEACONS OUT BY ADDRESS, and k4's own log said so:
 *    "core rev > 4, so PROMISC stays clear and the HW filter is used". The state was
 *    MACFILTER_BSSID = 00:00:00:00:00:00, BEACPROMISC clear, PROMISC clear -- and a beacon's
 *    addr3 is the AP's BSSID, which does not match zeros. In Linux, mac80211 sets
 *    FIF_BCN_PRBRESP_PROMISC the moment it starts scanning, and b43_adjust_opmode turns that
 *    into B43_MACCTL_BEACPROMISC: exactly the bit that exempts beacons and probe responses from
 *    BSSID filtering. Our port of adjust_opmode is FAITHFUL -- it ran with filter_flags = 0,
 *    which is right for the instant before mac80211 asks for anything. There is no mac80211
 *    here, so that decision is ours, and we had never made it.
 *
 * ★ k5 FIXES ONE AND TESTS THE OTHER, ON PURPOSE. The channel cookie is a missing ported
 * statement, so it is applied unconditionally -- faithfulness is not an experiment. The filter
 * is a policy we must choose, so it runs as a LADDER: nothing, then BEACPROMISC, then full
 * promiscuous. Fixing both blind would leave us unable to say which mattered, and "it works now"
 * without knowing why is how a driver acquires cargo cult.
 *
 * ⚠ THE ORDER IS b43's, NOT A CONVENIENT ONE. chip_init 1-6 (ApBringUp) -> phy_init (ApPhyInitG)
 * -> chip_init 8-13 (ChipInitTail) -> dma_init (the rings) -> core_init's last five -> mac_enable.
 * Running the tail before phy_init, or the rings before the tail, would be the same class of
 * error as k3's with a different statement.
 *
 * ★★★★★ THE MILESTONE THIS IS AIMED AT. Every stage so far has verified that the card accepted
 * something we wrote. This is the first one where the card must do something on its own and we
 * read the result: receive a real 802.11 frame off the air, DMA it into our memory, and leave it
 * there for us to decode. A single beacon proves, in one shot, that
 *     - the card READS our descriptor ring (it had to, to learn where to put the frame)
 *     - the card WRITES our buffers
 *     - PowerMac PCI DMA is cache-coherent, because we see what it wrote
 *     - Stage 4's PHY and radio work actually receives, end to end
 * None of those has ever been demonstrated on this hardware under OS 9.
 *
 * ★ THE ORACLE IS THE POISON, AND IT WAS PLANTED IN k2. b43_poison_rx_buffer fills the eight
 * bytes at the frame offset with 0xFF, and b43_rx_buffer_is_poisoned calls a buffer untouched
 * while `f[0] & f[1] & ... & f[7] == 0xFF`. That is the reference's own way of telling "the card
 * has not written here" from a genuinely short frame, so this probe does not have to invent a
 * detector. Poison gone + frame_len non-zero + RXDPTR advanced is three independent witnesses to
 * the same event.
 *
 * ★ WHAT IT RUNS, AND WHY ALL OF IT. k2 stopped after b43_dma_init. b43_wireless_core_init has
 * exactly five statements after that before it returns:
 *     b43_qos_init, b43_set_synth_pu_delay(dev,1), b43_bluetooth_coext_enable,
 *     b43_upload_card_macaddress, b43_security_init
 * and then b43_mac_enable turns the receiver on. All five are ported, in order, because guessing
 * which ones are preconditions is exactly the error that cost Stage 4 nine runs -- g2 dropped
 * three chip_init steps on the same reasoning and reported success anyway. Four of the five are
 * small; only b43_clear_keys has any bulk, and it is ported rather than skipped.
 *
 * ★ THE FIRMWARE REVISION DECIDES A BRANCH HERE TOO, AND IT IS THE OLD ONE.
 * b43_new_kidx_api (b43_xmit.h:372) is `fw.rev >= 351`. k1 measured 295, so it is FALSE:
 *     pairwise_keys_start = B43_NR_GROUP_KEYS * 2 = 8      (not 4)
 *     b43_clear_keys count = 4*2 + 50 = 58                 (not 54)
 *     b43_kidx_to_fw subtracts 4 for raw_kidx >= 4         (not identity)
 * All three are consequences of one measured number, and all three are wrong if you assume the
 * modern branch. This is the same shape as the FW_HDR_351 frame-offset finding in k2.
 *
 * ★ QoS IS DISABLED BY CHOICE, NOT BY MEASUREMENT, and that is legitimate. dev->qos_enabled is a
 * DRIVER-side setting (`dev->wl->hw->queues > 1`), not a hardware property, so it is ours to
 * decide -- and MINIMAL-STA-SCOPE.md already dropped QoS/WMM for this target. b43_qos_init's
 * disable arm is two operations: clear B43_HF_EDCF in the host flags, clear USE_EDCF in IFSCTL.
 * Taking that arm is running the function, not skipping it.
 *
 * ★ rx_tkip_phase1_write IS A NO-OP AND THAT IS CHECKED, NOT ASSUMED. It opens with
 * `if (!modparam_hwtkip) return;` and b43_main.c:92 declares `static int modparam_hwtkip;` with
 * no initialiser, so it is 0 unless someone passes hwtkip=1. The default build never enters it.
 *
 * ⚠ THE CHANNEL SWEEP EXISTS BECAUSE A TUNING MISS LOOKS EXACTLY LIKE A BROKEN RECEIVER.
 * ApBringUp tunes to channel 1. If the AP is on 6 or 11 we would see nothing and could easily
 * read that as "RX does not work". So this sweeps 1, 6 and 11 and reports each separately. Each
 * channel gets a CLEAN 32-slot budget -- the RX controller is reset, the buffers re-poisoned and
 * the descriptors rewritten between channels -- so one busy channel cannot starve the next of
 * descriptors and make it look empty.
 *
 * ⚠ THE CHANNEL SWITCH HAPPENS UNDER b43_mac_suspend, as b43 does it. That means mac_suspend is
 * ported here too, including its refcount: setup_struct_wldev_for_init leaves mac_suspended = 1,
 * so the FIRST b43_mac_enable is what takes it to 0 and actually sets MACCTL_ENABLED. Getting
 * that initial value wrong would leave the receiver off while every register readback looked
 * plausible.
 *
 * ⚠⚠ "NO FRESH BOOT REQUIRED" IS RETRACTED as of 2026-09-20 -- see ap_bringup.h. Half of the
 * runs taken without a reboot were void. Power down fully between runs.
 */

#define AP_LOG_NAME "\pAirPort RX Log"
/* ⚠ THE STAGE LIVES HERE, NOT INLINE IN THE BANNER. k29 shipped announcing itself as "STAGE 5-3
 * -- ENABLE THE MAC AND CATCH A FRAME" while doing Stage 6 work, because the stage was baked into
 * two string literals a thousand lines apart and only the version was ever bumped. Three adjacent
 * defines now, so a banner cannot drift from the work again. */
/* ★★★ 8-2g: THE APP IS A REPORTER, NOT A DRIVER.
 *
 * 0 = the driver owns the radio and this app resolves accessors, drains the driver's narration
 *     and runs oracles on what the driver reports.
 * 1 = the pre-8-2g behaviour, where the app brought the card up itself. ⚠ DO NOT SET THIS TO 1
 *     while the driver still enables the MAC at Open -- both would own the same DMA engine and
 *     the failure would not be attributable to either. */
#define AP_APP_DRIVES_RADIO 0

/* k85 = 8-6a, and it carries the two instrumentation changes that were deliberately held back
 * from a run of their own: the ring reports itself on the SUCCESS path (k84's census could only
 * speak on failure, so it never ran) and OT results are decoded at the point of reading. Both
 * ride along here rather than spending a reboot each. */
#define AP_RX_VER     "k158"
#define AP_RX_STAGE   "STAGE 8-58"
#define AP_RX_TITLE   "★ SAP-0x0800 RX ISOLATION (open-if-free)"

#include <Gestalt.h>
#include <MacMemory.h>
#include <CodeFragments.h>  /* 8-5a: GetSharedLibrary / FindSymbol, to see if EnetShimLib exists */
#include <OpenTransport.h>  /* 8-5d: InitOpenTransport + OTGetIndexedPort, client side only */
/* 8-6c: T8022Address, AF_8022, kSNAPSAP and the two address lengths. OTBind on an 802.2
 * provider needs a real address and this is where its shape is defined -- the header, not the
 * prose about the header. */
#include <OpenTransportProviders.h>

/* ── Stage 8-5b: Apple's Ethernet Shim interface, transcribed from
 *    usb-ddk/Examples/USBEnetSample/EnetShim.h. Vendored rather than #included because usb-ddk is
 *    not on this project's include path; three declarations are cheaper than a fragile path. ── */
typedef UInt32 ShimRefNum;
typedef struct EnetShimInterface {
    StringPtr         DRVName;
    CFragConnectionID ConnID;      /* a connection to the DRIVER'S OWN fragment */
    UInt32            RefCon;
    UInt32            theID;
} EnetShimInterface;
/* ⚠ The interface block goes BY VALUE, which is unusual enough to be worth stating: Apple's
 *   sample calls (*ShimInstall)(IntBlk, &Tref) with the struct itself, not a pointer. Passing a
 *   pointer here would compile and then hand the shim a stack address. */
typedef OSErr  (*ShimInstallProc)(EnetShimInterface,ShimRefNum*);
typedef OSErr  (*ShimRemoveProc)(ShimRefNum,Boolean);
/* 8-5e: the third shim entry point, resolved since k54 and never called until now.
 * usb-ddk USBEnet.h:134 -- OSErr (*ShimAsync)(ShimRefNum, UInt16 DvrSelector, UInt32, UInt32);
 * used in USBEnetDriver.c as (ref, EnetShim_Link, linkState, 0). */
typedef OSErr  (*ShimAsyncProc)(ShimRefNum,UInt16,UInt32,UInt32);
#define EnetShim_Link   0
#define EnetShim_Speed  1
#define EnetShim_Error  2
/* Our own accessor in ap_shim.c, not Apple's. */
typedef UInt32 (*ShimStatsProc)(UInt32*,UInt32*,UInt8*,UInt32*,SInt32*,UInt32*);
/* 8-5g: ours -- what the DRIVER read off the card, for cross-checking against the app's read. */
typedef UInt32 (*ShimGetMacProc)(UInt8*,SInt32*);
/* 8-2a: the backplane inventory the DRIVER enumerated, for comparison with the app's. */
typedef UInt32 (*ShimGetCoresProc)(UInt32*,UInt32*,UInt32*,SInt32*,SInt32*,SInt32*);
/* 8-2b: the arming interlock, and the core state the driver reached. */
typedef UInt32 (*ShimArmProc)(UInt32);
typedef UInt32 (*ShimCoreStateProc)(UInt32*,SInt32*,UInt32*);
/* 8-2c: the firmware revision the DRIVER's own upload produced. */
typedef UInt32 (*ShimGetFwProc)(UInt32*,UInt32*,SInt32*);
/* ★ 8-2d-1: drain the driver's own narration, one line per call.
 *   (idx, out, dropped, bytesUsed) -> total lines. out is a Str255; out[0]=0 if idx is past the
 *   end. Call it once with out == NULL to learn the count, then loop. See ap_ring.h. */
typedef UInt32 (*ShimGetLogProc)(UInt32,UInt8*,UInt32*,UInt32*);
/* ★ 8-2d-1b: which build of the shim is actually running.
 * ⚠ AP_SHIM_BUILD_EXPECTED IS DELIBERATELY A DUPLICATE of AP_SHIM_BUILD in ap_shim.c. Copying a
 *   constant across a fragment boundary is normally the silent-drift hazard this project keeps
 *   getting bitten by -- here the DISAGREEMENT IS THE PRODUCT. Bump both together. */
typedef UInt32 (*ShimGetBuildProc)(void);
/* ★ 8-2d-2b: the driver's initvals result.
 *   (applied, declared, bsApplied, bsDeclared, err) -> 1 if both tables applied IN FULL.
 * ⚠ Counts, not a boolean, so a PARTIAL application is visible -- ApplyIvs stops early and
 *   silently on a malformed or out-of-range record. */
typedef UInt32 (*ShimGetIvsProc)(UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★ 8-2d-3: the prefix + PHY result. (gpioOk, boardFlags, radioOn, phyErr, coreWasUp) -> 1 if
 * ApPhyInitG returned 1, i.e. all fifteen Stage 4 oracles passed from inside the driver. */
typedef UInt32 (*ShimGetPhyProc)(UInt32*,UInt32*,UInt32*,SInt32*,UInt32*);
/* ★ 8-2e: the chip_init tail. (macctl, tmsLow, pretbtt, prmaxtime, err) -> 1 if all three
 * read-backs held: MACPHYCLKEN set, PRMAXTIME 0, INFRA set. */
typedef UInt32 (*ShimGetTailProc)(UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★ 8-2f: the driver's RX ring. (ringPhys, ringReadback, nBufOk, rxStatArmed, err) -> 1 if the
 * ring armed and RXRING read back as the physical address written. */
typedef UInt32 (*ShimGetDmaProc)(UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★★★ 8-2g: the driver as a receiver. */
typedef UInt32 (*ShimRxPollProc)(SInt32*,UInt32*,UInt32*);
typedef UInt32 (*ShimGetRxBufProc)(UInt32,UInt8*,UInt32);
typedef UInt32 (*ShimQuiesceProc)(void);
typedef UInt32 (*ShimGetRxProc)(UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★★★ 8-3a: the driver as a transmitter.
 *   GetTx  -> (ringPhys, ringReadback, acked, statCount, err), 1 if an AP ACKed our frame.
 *   GetBssid -> the six bytes the driver aimed at, taken from a beacon IT received. */
typedef UInt32 (*ShimGetTxProc)(UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
typedef UInt32 (*ShimGetBssidProc)(UInt8*);
typedef UInt32 (*ShimTxProbeProc)(void);
/* ★★★★★ 8-3b: authentication. Auth() runs the exchange; GetAuth reports it.
 *   (alg, seq, status, ms, acked, err) -> 1 only if sequence 2 AND status 0. */
typedef UInt32 (*ShimAuthProc)(void);
typedef UInt32 (*ShimGetAuthProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★★★★★★ 8-3c: association. GetAssoc -> (cap, status, aid, ms, acked, got, err).
 * ⚠ The AID is the strong half of the oracle: a number the AP allocates from its own association
 *   table. status 0 with a ZERO aid is not a completed association. */
typedef UInt32 (*ShimAssocProc)(void);
typedef UInt32 (*ShimGetAssocProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* ★★★★★★ 8-4: the four-way handshake.
 * ⚠ PrepPmk must run BEFORE Assoc -- message 1 arrives ~100 ms after the association response
 *   and PBKDF2 is 0.3-1 s on this G4.
 * ⚠ GetHs returns counts and timings ONLY. No key material crosses the boundary. */
typedef UInt32 (*ShimPrepPmkProc)(void);
typedef UInt32 (*ShimHandshakeProc)(void);
typedef UInt32 (*ShimGetHsProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,SInt32*);
/* 8-6a: the DLPI delivery path. Pump takes a frame budget and returns frames delivered. */
typedef UInt32 (*ShimPumpProc)(UInt32);
/* 8-6d: force MACFILTER_BSSID to zeros (0) or the real BSSID (1), so the control can run
 * inside one run rather than across a reboot the intermittent can confound. */
typedef UInt32 (*ShimSetFilterProc)(UInt32);
/* 8-7: the interrupt source. ArmIrq installs the ISR and enables the card's output; it is
 * NEVER called from Open, so a hang here cannot repeat at the next boot. */
typedef UInt32 (*ShimArmIrqProc)(void);
/* 8-8: feed an Ethernet frame through the SAME path EnetHAL_Write uses. */
/* 8-9: did the DRIVER join the network by itself, inside Open? */
typedef UInt32 (*ShimGetJoinProc)(UInt32*,UInt32*,UInt32*,UInt32*);
typedef UInt32 (*ShimTxEthProc)(const UInt8*,UInt32);
typedef UInt32 (*ShimGetTxEthProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,
                                   UInt32*,UInt32*,UInt32*,UInt32*);
typedef UInt32 (*ShimGetIrqProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,
                                 UInt32*,UInt32*,UInt32*,UInt32*,UInt32*);
typedef UInt32 (*ShimGetDlpiProc)(UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,UInt32*,
                                  UInt32*,UInt32*,UInt32*);
/* ⚠ Keep equal to AP_SHIM_NSEL in ap_shim.c -- the shim writes this many selector
 * counters into arrays declared here, so a mismatch is a stack overflow, not a
 * compile error. The build-stamp check below is what makes the pairing enforceable. */
#define AP_SHIM_NSEL_APP 16
#define AP_SHIM_BUILD_EXPECTED 81
/* 8-5c: how long to hold the port registered while the tester selects it in a control panel.
 * 120 s in ticks. It breaks out early the instant a selector arrives, so this is a ceiling on a
 * run where nothing happens, not a cost every run pays. */
#define AP85C_WAIT_TICKS  (15L*60L)

/* ★ 8-2d-0: ap_log.h supplies Say/Out/PCat* and the Toolbox surface they need. It must come
 * BEFORE ap_bringup.h, which no longer includes them -- see the note at that file's old
 * logging block for why the dependency deliberately runs upward. */
#include "ap_log.h"
#include "ap_phy_initg.h"   /* brings ap_bringup.h with it */
/* ap_ilog.h is deliberately NOT included yet. Nothing below task level exists until 8-2, and an
 * unused header that emits five "defined but not used" warnings is exactly what hides a real one.
 * It is tested standalone by ilog_test.c and comes in with the interrupt handler that needs it. */

/* ★ STAGE 7. ap_psk_config.h is GITIGNORED and holds the user's own Wi-Fi password; it is never
 * read, never committed, and nothing derived from it is printed. ap_wpa_kdf.h is the C89 key
 * derivation verified off-hardware by kdf_test.c against RFC 6070, RFC 2202 and IEEE 802.11i
 * Annex H.4.2 -- fourteen known-answer tests, all passing, before any of it reached the G4. */
#include "ap_psk_config.h"
#include "ap_wpa_kdf.h"
#include "ap_aes.h"
#include "ap_ccmp.h"

/* ⚠⚠ PAINT THE WINDOW, AND VALIDATE THE UPDATE REGION.
 *
 * k55 presented the tester with a white box they could not click away from, and the cause was
 * this function's absence. The probe paints its window exactly once, at the END of the run, and
 * handles updateEvt only in the final event loop. That is harmless while the run is a straight
 * line of work -- nobody is looking -- but 8-5c introduced a two-minute wait that asks the tester
 * to go and do something.
 *
 * During that wait WaitNextEvent dequeued the update event and threw it away. Without
 * BeginUpdate/EndUpdate the update region is never validated, so the Window Manager re-posts it
 * immediately and forever: the window stays blank, the app churns, and the instructions the whole
 * experiment depends on are never drawn. The tester was asked to read a box that could not paint.
 *
 * ⇒ The lesson is not "call WaitNextEvent". It is that yielding is necessary and not sufficient:
 *   an app that wants the user to act must also be able to DRAW. */
static void ApDrawLog(WindowPtr w)
{
    short i, y, h, fit, start;
    if(!w) return;
    SetPort((GrafPtr)w);
    TextFont(kFontIDMonaco); TextSize(9);
    EraseRect(&w->portRect);
    /* ★ k156: AUTO-SCROLL TO THE TAIL. The old loop drew from line 0 and ran off the bottom, so
     * a prompt like "PING NOW" that prints late was invisible. Draw only the last `fit` lines. */
    h = (short)(w->portRect.bottom - w->portRect.top);
    fit = (short)((h - 12) / 11);
    if(fit < 1) fit = 1;
    start = (gN > fit) ? (short)(gN - fit) : 0;
    y = 12;
    for(i=start;i<gN;i++){ MoveTo(6,y); DrawString(gLines[i]); y += 11; }
}

/* ── 8-5d: dump Open Transport's port registry ────────────────────────────────────────────────
 * Decodes the flags rather than printing them raw. A hex value in a log is a question; a decoded
 * field is an answer, and three of this project's four worst detours came from a number nobody
 * decoded at the point of reading. */
static void ApListOtPorts(const char *whenLabel)
{
    OTPortRecord pr;
    OTItemCount  idx = 0;
    int          n = 0;
    Say(whenLabel);
    while(OTGetIndexedPort(&pr,idx)){
      Str255 L;
      L[0]=0;
      PCat(L,"    ["); PCatDec(L,(unsigned long)idx); PCat(L,"] ");
      PCat(L,(char*)pr.fPortName);
      PCat(L,"  module="); PCat(L,(char*)pr.fModuleName);
      Out(L);
      L[0]=0;
      PCat(L,"        ref=");   PCatHex(L,(unsigned long)pr.fRef,8);
      PCat(L,"  portFlags="); PCatHex(L,(unsigned long)pr.fPortFlags,8);
      PCat(L,"  infoFlags="); PCatHex(L,(unsigned long)pr.fInfoFlags,8);
      Out(L);
      L[0]=0; PCat(L,"        ");
      if(pr.fPortFlags & 0x00000001UL) PCat(L,"ACTIVE ");
      if(pr.fPortFlags & 0x00000002UL) PCat(L,"DISABLED ");
      if(pr.fPortFlags & 0x00000004UL) PCat(L,"UNAVAILABLE ");
      if(pr.fInfoFlags & 0x00004000UL) PCat(L,"SYSTEM-REGISTERED ");
      if(pr.fInfoFlags & 0x00008000UL) PCat(L,"⚠PRIVATE(hidden from the user) ");
      if(L[0] <= 8) PCat(L,"(no notable flags)");
      Out(L);
      idx++; n++;
      if(n > 24) { Say("    ...truncated at 24 ports"); break; } }
    if(n == 0) Say("    (OT reported NO ports at all -- suspect InitOpenTransport failed)");
    else       Say1("    total ports = ",(unsigned long)n);
}

/* ⚠ A DECLARED LENGTH THAT DISAGREES WITH THE LITERAL IS THE WORST KIND OF BUG HERE: PBKDF2
 * would hash the wrong number of bytes, every byte of the PMK would be wrong, and the only
 * symptom would be a failed MIC four frames later -- indistinguishable from a dozen other
 * faults. These make it a COMPILE error instead. A negative array size is the C89 way; there is
 * no static_assert in this dialect. Neither reveals anything about the values. */
typedef char ap_psk_passphrase_len_disagrees_with_the_literal
    [(sizeof(AP_PSK_PASSPHRASE) - 1 == (AP_PSK_PASSPHRASE_LEN)) ? 1 : -1];
typedef char ap_psk_ssid_len_disagrees_with_the_literal
    [(sizeof(AP_PSK_SSID) - 1 == (AP_PSK_SSID_LEN)) ? 1 : -1];




#define K3_DWELL_MS                 1200         /* ~12 beacon intervals per channel */
#define K5_NCHAN                    13           /* 1..13; 14 needs the Japan country-code arm */
#define K5_WIDE_DWELL_MS            600          /* ~6 beacon intervals, x13 channels = ~8 s */
#define K5_NARROW_DWELL_MS          1500         /* the re-test gets longer, it must not miss */
#define K6_NCHAN                    3            /* the three non-overlapping channels */
#define K6_DWELL_MS                 2000         /* ~20 beacon intervals: a real scan dwell */
/* k45's A/B. The outcome is binary and the counter saturates at K3_RX_SLOTS, so a full 2 s
 * dwell buys nothing here -- 600 ms is ample to tell 0 from "the ring filled", and it keeps
 * 5 rounds x 2 paths x 3 channels under 20 s. More rounds beat longer dwells: at a 23% failure
 * rate, 5 rounds give a 1 - 0.77^5 = 73% chance of catching at least one path-A failure. */
#define K45_ROUNDS                  5
#define K45_DWELL_MS                600
/* ★★★ k46: THE HAMMER. k45's five rounds gave five trials of a fault that fires ~23% of the
 * time, and unsurprisingly caught nothing (0.77^5 = 27%, so that outcome was the single most
 * likely one). Proving a fix by re-running would need ~12 consecutive clean reboots.
 *
 * So instead of sampling the fault, provoke it. 40 arms in one run: if the wedge is a per-arm
 * dice roll at anything like 23%, P(zero failures in 40) = 0.77^40 = 0.00004 -- it cannot hide.
 * And if 40 arms produce nothing, that is a STRONG refutation of the arm as the trigger, not
 * another inconclusive run. One reboot either way. */
#define K46_HAMMER                  40
#define K46_DWELL_MS                400          /* counts ran 6..32 at 600 ms; 400 ms still
                                                  * separates "delivered" from "zero" easily */
/* ★★★ k47: k46 answered its question -- the fault reproduces and the ring arm is innocent --
 * so the hammer shrinks to a detector and the run spends its time on a BISECT instead. Four
 * probe dwells are plenty: k46 saw 86 consecutive dead ones, so the state does not flicker. */
#define K47_PROBE                   4
#define K47_RUNGS                   9
#define K6_RX_SLOTS_USED            K3_RX_SLOTS

/* 8-9e: set when GetJoin reports the driver armed its own interrupt (state 2). */
static int irqArmedEarly = 0;
/* 8-10: set when the driver reports it has ALREADY been Opened by somebody else (TCP/IP).
 * In that state this app registers nothing and opens nothing -- it only reports. */
static int witnessMode = 0;
static void SayOk(const char*s,int ok){Str255 L;L[0]=0;PCat(L,ok?"  [ok] ":"  [!!] ");PCat(L,s);Out(L);}

/* ★★★ AN ORACLE THAT CANNOT RUN IS NOT AN ORACLE THAT FAILED, AND CONFLATING THEM COST A BUILD.
 *
 * k106 registered the port at BOOT, so the app correctly skipped its own registration -- and then
 * printed seven [!!] lines, including "the shim ACCEPTED our registration" and "The shim is there
 * and refused us", in a run where the shim had accepted the registration perfectly at startup.
 * Every one of them is an app-drives-the-card check on a path the app no longer takes.
 *
 * This is the SECOND time on this project: the legacy A..I VERDICT block did the same thing from
 * k99 to k102 and was banked as a PASS while printing nine confident failures. A report that
 * cries wolf on the expected path is worse than no report, because the one time it is right
 * nobody looks. [[feedback_guards_must_not_latch]]
 *
 * So: SayDrv is for oracles that only mean something when THIS APP drove the hardware. In
 * witness mode they say so, and say where the real answer lives. */
static void SayDrv(const char*s,int ok){
    Str255 L; L[0]=0;
    if(witnessMode){ PCat(L,"  [--] "); PCat(L,s); PCat(L," -- not exercised (witness mode)"); }
    else           { PCat(L,ok?"  [ok] ":"  [!!] "); PCat(L,s); }
    Out(L);
}

/* ★ OT RESULTS ARE DECODED AT THE POINT OF READING -- k84.
 *
 * k80 through k84 all logged `OTBind -> 4294964146` and nobody read it, through five runs. It is
 * `Say1`'s unsigned rendering of -3150 = kOTBadAddressErr. A bare number is a question; the name
 * is the answer. Same rule as RXSTATUS in ap_dma.h, same project, and this is the third time. */
static const char *OtErrName(OSStatus e)
{
    switch(e){
      case 0:     return "kOTNoError";
      case -3150: return "kOTBadAddressErr";
      case -3151: return "kOTBadOptionErr";
      case -3152: return "kOTAccessErr";
      case -3153: return "kOTBadReferenceErr";
      case -3154: return "kOTNoAddressErr";
      case -3155: return "kOTOutStateErr";
      case -3156: return "kOTBadSequenceErr";
      case -3157: return "kOTSysErrorErr";
      case -3158: return "kOTLookErr";
      case -3159: return "kOTBadDataErr";
      case -3160: return "kOTBufferOverflowErr";
      case -3161: return "kOTFlowErr";
      case -3162: return "kOTNoDataErr";
      case -3163: return "kOTNoDisconnectErr";
      case -3164: return "kOTNoUDErrErr";
      case -3165: return "kOTBadFlagErr";
      case -3166: return "kOTNoReleaseErr";
      case -3167: return "kOTNotSupportedErr";
      case -3168: return "kOTStateChangeErr";
      case -3169: return "kOTNoStructureTypeErr";
      case -3170: return "kOTBadNameErr";
      case -3171: return "kOTBadQLenErr";
      case -3172: return "kOTAddressBusyErr";
      default:    return "(not an OT error code)"; }
}

/* Prints the signed value AND the name. Say1 renders OSStatus unsigned, which is how -3150
 * reached five logs disguised as 4294964146. */
static void SayOtErr(const char *s,OSStatus e)
{
    Str255 L; L[0]=0;
    PCat(L,s);
    if(e < 0){ PCat(L,"-"); PCatDec(L,(unsigned long)(-(long)e)); }
    else       PCatDec(L,(unsigned long)e);
    PCat(L,"  "); PCat(L,OtErrName(e));
    Out(L);
}
/* ★ 8-2f: the DMA layer moved out so the DRIVER can build an RX ring too.
 * Verbatim move, proven by reconstruction. See ap_dma.h. */
#include "ap_dma.h"

/* ★ 8-2g: the receive layer moved out. The DRIVER owns the radio from here; this app is a
 * reporter. Verbatim move, proven by reconstruction. See ap_rx.h. */
#include "ap_rx.h"
/* ★ 8-3a: the transmit layer moved out, with the frame parser that aims it. See ap_tx.h. */
#include "ap_tx.h"

/* ============================================================================
 * ★ THE STAGE-BOUNDARY BISECT.
 *
 * k18 settled that the PHY TX path is faulty for EVERY transmission, not just
 * ours: b43_dummy_transmission uses template RAM and none of the Stage 5
 * machinery, and it raised B43_IRQ_PHY_TXERR having radiated (TSSI moved). So
 * the fault was already present in Stage 4, which never decoded this register --
 * its oracle was the TXSTAT bits, and those are textbook here.
 *
 * dummy_transmission runs with the MAC not yet enabled (Stage 4 called it
 * repeatedly during PHY init), so ONE pass can probe every boundary. The first
 * that errors narrows ~450 statements to a single stage. If the very first --
 * before any PHY init at all -- already errors, the fault predates
 * b43_phy_initg entirely and Stage 4 is not where to look either.
 * ==========================================================================*/
static int gTxProbeN = 0, gTxProbeFirstBad = -1, gTxProbeSilent = 0;

/* ⚠⚠ k19's BISECT WAS VACUOUS AND ITS OWN OUTPUT SAID SO. Boundaries 1 through 4 each printed
 * "(did not radiate)" -- IRQ 0x00000000, TSSI untouched -- because MACCTL_ENABLED is not set
 * until boundary 5. Four "clean" readings from four transmissions that never happened, scored
 * as evidence. A transmission that does not occur cannot raise a transmit error. That is a
 * check that cannot fail, which is the thing this project added a CLAUDE.md rule about three
 * hours ago, and the probe PRINTED the disproof while the verdict logic ignored it.
 *
 * Two fixes, and the second matters more than the first:
 *   1. force MACCTL_ENABLED|AWAKE for the duration of each probe and restore it after, so every
 *      boundary CAN transmit. Out of b43's order -- it enables the MAC after dma_init -- and
 *      labelled as the deliberate probe liberty it is. A bisect in which only one point can
 *      transmit is not a bisect.
 *   2. SCORE ON RADIATION. Clean now requires TSSI to have MOVED and no PHY_TXERR. "Nothing
 *      happened" is its own outcome, reported separately, and never counted as success.
 *
 * ★ IT ALSO ESTABLISHED SOMETHING REAL: dummy_transmission does not radiate without
 * MACCTL_ENABLED. Stage 4 ran it five times during PHY init with the MAC never enabled, and
 * its oracle F reported "the MAC drove a frame out of the TX engine" from the TXE0_STATUS
 * transitions. Those transitions are genuine -- the engine responds to the TXE0_AUX write
 * regardless -- but nothing left the antenna. Stage 4 never transmitted at all. */
static int TxProbeHere(const char *whereLabel)
{
    UInt32 ir,macctl0; UInt16 t0,t1,ov,ovv; int bad,radiated;
    Str255 L;

    macctl0 = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,
            macctl0 | B43_MACCTL_ENABLED | B43_MACCTL_AWAKE);
    (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);

    ShmClearTssi();
    ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
    (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);

    DummyTransmission(0);                    /* radio rev 2 -> the rev >= 2 variant */

    SsbSpinUs(200000);
    ir  = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
    t0  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK);
    t1  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK+2);
    ov  = PhyRead((UInt16)B43_PHY_RFOVER);
    ovv = PhyRead((UInt16)B43_PHY_RFOVERVAL);

    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl0);          /* put it back */
    (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);

    radiated = !(t0==0x7F7F && t1==0x7F7F);
    bad      = (ir & 0x00000800UL) ? 1 : 0;

    if(!radiated) gTxProbeSilent++;
    else if(bad && gTxProbeFirstBad < 0) gTxProbeFirstBad = gTxProbeN;
    gTxProbeN++;

    L[0]=0;
    PCat(L, !radiated ? "  [ SILENT   ] " : (bad ? "  [PHY_TXERR] " : "  [  RADIATED] "));
    PCat(L,whereLabel);
    PCat(L," IRQ 0x");PCatHex(L,ir,8);
    PCat(L," TSSI 0x");PCatHex(L,(unsigned long)t0,4);
    PCat(L,"  MACCTL was ");PCat(L,(macctl0&B43_MACCTL_ENABLED)?"on ":"OFF");
    Out(L);
    /* The T/R switch lives in RFOVERVAL. TSSI measures power at the PA, so a switch stuck in
     * receive would give exactly what we see: real measured power that never reaches the air. */
    L[0]=0;PCat(L,"                RFOVER 0x");PCatHex(L,(unsigned long)ov,4);
    PCat(L,"  RFOVERVAL 0x");PCatHex(L,(unsigned long)ovv,4);
    PCat(L,"  TRSWRX field 0x");PCatHex(L,(unsigned long)(ovv & B43_PHY_RFOVERVAL_TRSWRX),4);
    if(ov & B43_PHY_RFOVERVAL_TRSWRX) PCat(L,"  ⚠ TRSW OVERRIDDEN");
    Out(L);
    return bad;
}


/* ============================================================================
 * the RX ring, rebuilt clean for one channel
 * ==========================================================================*/
/* ⚠⚠ k10's SCAN READ 32 BUFFERS ON CHANNEL 1 AND ZERO ON 6 AND 11. The receiver works
 * exactly ONCE per run, and this function is why: it called
 *     (void)DmaControllerRxReset(base);
 * and threw the answer away. Every RXSTATUS in a failing pass reads 0x00801000 -- RXSTATE
 * ACTIVE, not DISABLED -- so the reset was failing and we programmed a ring into an engine
 * that had never stopped. The one value that would have said so was cast to void.
 *
 * k11 reports it, on every arm, with the state before and after. It also RETRIES with a much
 * longer wait when the faithful 10 ms reset fails, because "needs more time" and "will never
 * disable" want different fixes and one extra poll loop tells them apart. */
/* k48: how often the SHM_SCRATCH counter stood still across a dwell, split by whether that
 * dwell heard anything. The 2x2 is the whole point -- a frozen counter only means something
 * if it does NOT freeze on the dwells that work. */
static int gScrFrozen = 0, gScrMoved = 0, gScrFrozenDead = 0, gScrMovedDead = 0;


/* ★ 8-3b: the prompt receive path moved out -- the driver has to WAIT for an answer now.
 * Verbatim move, proven by reconstruction. See ap_wait.h. */
#include "ap_wait.h"

/* One channel, one filter level, one dwell. Suspends the MAC for the retune and the ring
 * rebuild, exactly as b43 wraps a channel change, then re-enables and waits. */
/* ★★★ k45: `rearm` SELECTS BETWEEN OUR PATH AND b43'S.
 *
 *   rearm = 1  MacSuspend, RxRingArm (a full DMA controller RESET + re-program), SwitchChannel,
 *              MacEnable. This is what every increment up to k44 did, once per channel.
 *   rearm = 0  MacSuspend, SwitchChannel, MacEnable, RxRecycle. No engine reset. This is what
 *              b43 actually does: b43_op_config (main.c) suspends, switches and enables and
 *              touches NO ring code, and b43_dmacontroller_rx_reset is reachable only from
 *              setup and teardown -- TWICE in a session, never per channel. We were calling it
 *              up to NINE times per run.
 *
 * Everything else about the two paths is identical, so the arm is the only variable. */
/* ★★★★★ k48: SHM_SCRATCH words 0 and 1, the first thing in 22 logs that separates a dead run
 * from a live one.
 *
 * Every one of the six dead runs read SCR[0]=0x000A SCR[1]=0x0000 at bring-up, and not one of
 * the sixteen runs with any other value has ever died. Necessary, not sufficient -- five live
 * runs share the value -- but P(all six landing in that bucket by chance) is about 1.6%.
 *
 * b43 calls offset 1 B43_WATCHDOG_REG: "the firmware will reset the watchdog counter to 0 in its
 * idle loop". ⚠ IT ONLY DOES SO FOR OPENSOURCE FIRMWARE and we run the proprietary v4 blob, so
 * the NAME is not evidence here and is not being claimed. What IS testable without knowing the
 * name: the pair reads like a counter (0x0000_000A = 10 in the dead runs, against 0x0001_000E =
 * 65550 and 0x0003_0043 = 196675 in live ones). A counter can be watched.
 *
 * ⇒ THE MEASUREMENT: sample it either side of every dwell. If it is FROZEN across a dead dwell
 *   and MOVING across a live one, the microcode has stalled and we are looking at the fault
 *   itself rather than another correlate. If it advances during a dead dwell, the microcode is
 *   fine and the fault is downstream of it -- which is just as useful and kills this lead. */
static UInt16 ScratchRead16(UInt32 wordIndex)
{ ShmControl(2UL,wordIndex); return ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA); }

static int DwellAndCount(UInt32 base,DmaBlock *ring,UInt8 **bufLog,UInt32 *bufPhys,
                         UInt32 frameoffset,UInt32 bufSize,
                         UInt8 ch,UInt32 filterBits,UInt32 dwellMs,
                         UInt32 *rxStatOut,int *firstOut,int rearm)
{
    UInt32 ms; int n=0;
    UInt16 scr0a,scr1a,scr0b,scr1b;
    scr0a = ScratchRead16(0); scr1a = ScratchRead16(1);
    (void)MacSuspend();
    if(rearm) (void)RxRingArm(base,ring,bufLog,bufPhys,frameoffset,bufSize);
    SwitchChannel(ch);                 /* cookie + PHY switch + 8 ms settle */
    (void)ApplyFilter(filterBits);
    MacEnable();
    if(!rearm) (void)RxRecycle(base,ring,bufLog,bufPhys,frameoffset,bufSize,0,0);

    /* ★★★★★ RE-WRITE RXINDEX AFTER MacEnable, AND KEEP RE-WRITING IT.
     *
     * This is the fix for the intermittent that voided k33a, k35 and k37. The shape, measured
     * twice and identical both times:
     *
     *     scan            0 buffers across SIX 2-second dwells on three channels
     *     RXSTATUS        0x00801000 -- state ACTIVE, RXDPTR parked at 0, never advancing
     *     transmit sect.  32 frames in one second, a probe reply caught in 6 ms
     *
     * ACTIVE with a pointer that never advances is an engine that is enabled and has NO
     * DESCRIPTORS -- not one that is switched off. And the difference between the two sections
     * is exactly where RXINDEX gets written:
     *
     *     DwellAndCount   RxRingArm writes RXINDEX with the MAC SUSPENDED, then the dwell loop
     *                     calls only RxScan, which reads buffer contents and never touches it
     *     transmit sect.  RxRecycle and RxWaitFor re-write RXINDEX on every call, AFTER
     *                     MacEnable, so a lost write is repaired within milliseconds
     *
     * So the scan had no path to recover from a write that did not stick, and the transmit
     * section could not fail to. That also explains why k36's 2-second retry did not help: the
     * retry repeats the same suspended write.
     *
     * ⚠ THIS IS A HYPOTHESIS WITH A CHEAP TEST, NOT A PROVEN CAUSE. Re-writing RXINDEX is
     *   harmless when it was already right, so if the scan starts working reliably that is the
     *   answer; if it still dies, the write is not what is being lost and the mid-dwell RXSTATUS
     *   below will show whether the pointer ever moves. */
    DmaPublish();
    ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,
            (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES);

    for(ms=0; ms<dwellMs; ms+=50){
      SsbSpinUs(50000);
      n = RxScan(bufLog,frameoffset,firstOut);
      if(n >= K3_RX_SLOTS) break;
      if((ms % 250) == 0){            /* four times a second, so one lost write costs 250 ms */
        DmaPublish();
        ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,
                (UInt32)K3_RX_SLOTS * B43_DMADESC32_BYTES); } }
    scr0b = ScratchRead16(0); scr1b = ScratchRead16(1);
    *rxStatOut = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
    n = RxScan(bufLog,frameoffset,firstOut);

    /* ★★★ THE DEAD-DWELL CENSUS. k39 REFUTED THE RXINDEX HYPOTHESIS, so stop guessing.
     *
     * k39 re-wrote RXINDEX after MacEnable and again every 250 ms, exactly as k38 proposed, and
     * the scan was still stone dead across six dwells with RXSTATUS 0x00801000. So the write is
     * NOT what is being lost, and that was worth finding out -- it was the leading explanation
     * and it is now eliminated rather than lingering.
     *
     * What is left is that something ELSE about the receive path is not in the state we think.
     * Rather than guess again, read every register that gates reception and print it, but only
     * on a dwell that heard nothing -- a healthy run stays quiet and a void run becomes a
     * diagnosis instead of another lost reboot.
     *
     * ⚠ RXINDEX IS READ BACK HERE FOR THE FIRST TIME. We have written it since k2 and never
     *   once checked what the register returns. If it does not read back as written, that is
     *   the answer and it has been sitting one register read away the entire time. */
    /* ⚠ THIS USED TO RUN ONLY ON A DEAD DWELL, AND THAT WAS A MISTAKE. Keeping healthy runs
     * quiet sounded tidy, but it meant the dead-dwell census had NO HEALTHY BASELINE to be
     * compared against -- a measurement with no control, which is the error this project has
     * made more than any other. It now prints every dwell, one compact line when all is well
     * and the full census when nothing was heard, so a good run and a bad one can be diffed
     * directly. */
    { UInt32 q_ctl = ssb_r32(gBus.bar0,base+B43_DMA32_RXCTL);
      UInt32 q_idx = ssb_r32(gBus.bar0,base+B43_DMA32_RXINDEX);
      UInt32 q_mac = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
      Str255 L; L[0]=0;
      PCat(L,"      [dwell] heard "); PCatDec(L,(unsigned long)n);
      PCat(L,"  RXCTL ");   PCatHex(L,q_ctl,8);
      PCat(L,"  RXINDEX "); PCatHex(L,q_idx,8);
      PCat(L,"  RXSTATUS ");PCatHex(L,*rxStatOut,8);
      PCat(L,"  MACCTL ");  PCatHex(L,q_mac,8);
      /* ★ k48: the scratch counter either side of the dwell. Frozen across a dead dwell means
       * the microcode stalled; moving means it did not and this lead is dead. */
      PCat(L,"  SCR ");     PCatHex(L,(unsigned long)scr1a,4);
      PCat(L,":");          PCatHex(L,(unsigned long)scr0a,4);
      PCat(L,"->");         PCatHex(L,(unsigned long)scr1b,4);
      PCat(L,":");          PCatHex(L,(unsigned long)scr0b,4);
      PCat(L,(scr0a==scr0b && scr1a==scr1b) ? "  FROZEN" : "  moving");
      Out(L);
      if(scr0a==scr0b && scr1a==scr1b) gScrFrozen++; else gScrMoved++;
      if(n==0){ if(scr0a==scr0b && scr1a==scr1b) gScrFrozenDead++; else gScrMovedDead++; } }

    if(n == 0){
      UInt32 rxctl  = ssb_r32(gBus.bar0,base+B43_DMA32_RXCTL);
      UInt32 rxring = ssb_r32(gBus.bar0,base+B43_DMA32_RXRING);
      UInt32 rxidx  = ssb_r32(gBus.bar0,base+B43_DMA32_RXINDEX);
      UInt32 macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
      Say("      ---- DEAD DWELL: every register that gates reception ----");
      SayH("        RXCTL    = ",rxctl,8);
      Say ("          bit0 RXENABLE, bits 1-7 frameoffset, bits 16-17 addrext");
      SayH("        RXRING   = ",rxring,8);
      SayH("        RXINDEX  = ",rxidx,8);
      Say1("          we wrote ",(unsigned long)(K3_RX_SLOTS*B43_DMADESC32_BYTES));
      if(rxidx != (UInt32)(K3_RX_SLOTS*B43_DMADESC32_BYTES))
        Say ("          ⚠⚠ IT DOES NOT READ BACK AS WRITTEN. That is the fault, and it has been");
      else
        Say ("          reads back correctly, so the write is landing -- look elsewhere.");
      SayRxStatus("        RXSTATUS = ",*rxStatOut);
      SayH("        MACCTL   = ",macctl,8);
      Say (((macctl & B43_MACCTL_ENABLED) && (macctl & B43_MACCTL_AWAKE))
           ? "          ENABLED and AWAKE, so the MAC is not the problem"
           : "          ⚠⚠ THE MAC IS NOT ENABLED|AWAKE -- that alone explains the silence");
      SayH("        PHY channel cookie SHM[0x00A0] = ",
           (unsigned long)ShmRead16Shared(B43_SHM_SH_CHAN),4);
      Say ("          the channel the FIRMWARE thinks it is on; a mismatch with the dwell's");
      Say ("          channel would mean b43_switch_channel's cookie did not take."); }
    return n;
}

static void PCatMac(Str255 L,const UInt8 *m)
{ int i; for(i=0;i<6;i++){ if(i) PCat(L,":"); PCatHex(L,(unsigned long)m[i],2); } }

/* Decode one received frame far enough to say what it is and who sent it. */
/* ============================================================================
 * THE FRAME DECODER, corrected. k5's version placed the 802.11 header at the
 * frame offset and produced addr2 = FF:FF:FF:FF:FF:FF -- an impossible source
 * address -- while Oracle G passed it anyway.
 *
 * ⚠ THE 802.11 HEADER IS AT frameoffset + padding + 6. b43_rx (b43_xmit.c) says
 * so in its own comment:
 *       /_ Skip PLCP and padding _/
 *       padding = (macstat & B43_RX_MAC_PADDING) ? 2 : 0;
 *       plcp = (struct b43_plcp_hdr6 *)(skb->data + padding);
 *       skb_pull(skb, sizeof(struct b43_plcp_hdr6) + padding);
 *       wlhdr = (struct ieee80211_hdr *)(skb->data);
 * A six-byte PLCP header sits between them, and k5 walked straight over it.
 * Re-decoding k5's own logged bytes at +6 produced five agreeing fields --
 * PLCP SIGNAL 0x0A (1 Mbps), frame control 0x0080 (beacon), duration 0,
 * addr1 broadcast, addr2 a locally-administered UniFi BSSID -- which is what
 * a correct offset looks like and what k5's decode could not produce.
 *
 * ⚠ frame_len INCLUDES the PLCP header. b43 does skb_put(len + frameoffset),
 * skb_pull(frameoffset), THEN skb_pull(6 + padding), so the 802.11 frame is
 * len - 6 - padding bytes long. Every bound below uses that, not len.
 *
 * ⚠ PADDING IS TREATED AS 0 AND NOT READ FROM mac_status, deliberately. k5's
 * mac_status came back 0x4A3F0000 with RXST_VALID clear and a scatter of
 * improbable flags, so its offset is not yet trustworthy -- that is exactly
 * what the hex dump is for. The five-field agreement above was obtained with
 * padding = 0, so padding = 0 is a MEASURED fact for that frame rather than an
 * assumption. If the dump shows otherwise this is the line to change.
 * ========================================================================= */

/* 802.11b PLCP SIGNAL values. A frame that is really there carries one of these. */
static int PlcpRateIsLegal(UInt8 sig)
{ return (sig==0x0A)||(sig==0x14)||(sig==0x37)||(sig==0x6E); }
static const char *PlcpRateName(UInt8 sig)
{
    if(sig==0x0A) return "1 Mbps";
    if(sig==0x14) return "2 Mbps";
    if(sig==0x37) return "5.5 Mbps";
    if(sig==0x6E) return "11 Mbps";
    return "not a legal 802.11b rate";
}

/* A raw hex + ASCII dump. The point of k6: settle the layout by MEASUREMENT rather than by
 * another round of me reading a struct definition and getting an offset wrong. */
static void HexDump(const UInt8 *p,int n,UInt32 frameoffset)
{
    int i,j;
    for(i=0;i<n;i+=16){
      Str255 L; L[0]=0;
      PCat(L,"    +"); PCatHex(L,(unsigned long)i,2); PCat(L,"  ");
      for(j=0;j<16;j++){
        if(i+j<n){ PCatHex(L,(unsigned long)p[i+j],2); PCat(L," "); } else PCat(L,"   ");
        if(j==7) PCat(L," "); }
      PCat(L," |");
      for(j=0;j<16 && i+j<n;j++){
        char c[2]; c[0]=(char)((p[i+j]>=32 && p[i+j]<127)?p[i+j]:'.'); c[1]=0; PCat(L,c); }
      PCat(L,"|");
      /* mark the two offsets that matter so the layout can be read off the dump directly.
       * ⚠ 0xFFFFFFFF IS THE "NO MARKER" SENTINEL AND IT USED TO WRAP. frameoffset + 6 on a
       * UInt32 sentinel is 5, so `i <= 5 && 5 < i+16` fired on line 0 of every dump and every
       * raw hexdump in the project was labelled "<- 802.11 hdr" whether or not it was one.
       * That is how k32's ANonce dump came out mislabelled. Same class as the Oracle E wrap in
       * k1: arithmetic on a sentinel. Test the sentinel first. */
      if(frameoffset != 0xFFFFFFFFUL){
        if((UInt32)i <= frameoffset && frameoffset < (UInt32)(i+16))           PCat(L,"  <- frameoffset");
        if((UInt32)i <= frameoffset+K6_HDR_PLCP6 && frameoffset+K6_HDR_PLCP6 < (UInt32)(i+16))
                                                                               PCat(L," <- 802.11 hdr"); }
      Out(L); }
}

/* One network, as seen in a beacon. */
typedef struct { UInt8 bssid[6]; UInt8 ssidLen; UInt8 ssid[33]; UInt8 chan; UInt8 jssi; } NetEntry;
#define K6_MAXNET 24
static NetEntry gNets[K6_MAXNET];
static int gNNets = 0;


static void NetAdd(const UInt8 *bssid,const UInt8 *ssid,UInt8 ssidLen,UInt8 chan,UInt8 jssi)
{
    int i,k;
    for(i=0;i<gNNets;i++) if(MacEq(gNets[i].bssid,bssid)){
      if(jssi>gNets[i].jssi) gNets[i].jssi=jssi;
      if(gNets[i].ssidLen==0 && ssidLen) {
        gNets[i].ssidLen=ssidLen; for(k=0;k<ssidLen;k++) gNets[i].ssid[k]=ssid[k]; }
      if(chan) gNets[i].chan=chan;
      return; }
    if(gNNets>=K6_MAXNET) return;
    for(k=0;k<6;k++) gNets[gNNets].bssid[k]=bssid[k];
    gNets[gNNets].ssidLen=ssidLen;
    for(k=0;k<ssidLen && k<32;k++) gNets[gNNets].ssid[k]=ssid[k];
    gNets[gNNets].chan=chan; gNets[gNNets].jssi=jssi;
    gNNets++;
}

/* ★ 8-4: the EAPOL layer moved out. ⚠ It carries KEY MATERIAL -- see ap_eapol.h. */
#include "ap_eapol.h"
/* 8-6a: the app reports the conversion's reason histogram, so it needs the same names the
 * driver counts against. Shared header, one list -- not a second copy that drifts. */
#include "ap_enet.h"
/* Post one frame: two descriptors, header then payload, exactly as dma_tx_fragment does,
 * then poke TXINDEX one slot past the last one used. */
/* ⚠ k7 PASSED K3_RX_SLOTS (32) AS THE TX RING'S SLOT COUNT. That is wrong, and it is fixed
 * here -- but it was NOT the blocker, and saying so matters. DTABLEEND is only written on the
 * LAST slot, and we write slots 0 and 1, so no descriptor got one either way. b43 behaves
 * identically: it fills TX descriptors lazily as frames are queued, so its TX ring also has no
 * DTABLEEND until the ring wraps. The bug was latent -- a frame posted at slot 31 would have
 * been handed a spurious end-of-table -- not active. */


int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    DmaBlock txRing={0},rxRing={0},rxPool={0},txPool={0};
    UInt32 base;
    UInt32 rxBufPhys[K3_RX_SLOTS];
    UInt8 *rxBufLog[K3_RX_SLOTS];
    UInt32 frameoffset = B43_DMA0_RX_FW351_FO;
    UInt32 rxBufSize   = B43_DMA0_RX_FW351_BUFSIZE;
    UInt32 boardflags  = 0;
    UInt8  mac[6];
    UInt32 ktp=0; int nKeys=0; UInt16 puDelay=0; int btDid=0;
    UInt32 macctlBefore=0,macctlAfter=0;
    static const UInt8 kChans[K6_NCHAN] = {1,6,11};
    int chanFrames[K6_NCHAN]; int chanFirst[K6_NCHAN]; UInt32 chanRxStat[K6_NCHAN];
    /* k5's ladder variables are gone with the ladder: the filter question is answered, and
     * carrying the machinery of a settled experiment is how dead code starts looking live. */
    int totalFrames=0, bestCh=-1, bestSlot=-1;
    int chanCookieOk=0;
    int nBufOk=0;
    int oracleA=0,oracleB=0,oracleC=0,oracleD=0,oracleE=0,oracleF=0,oracleG=0;
    int oracleH=0,oracleI=0,promptHits=0;
    int authGot=0,oracleJ=0; UInt16 authAlg=0,authSeq=0,authStatus=0xFFFF;
    int assocGot=0,oracleK=0; UInt16 assocStatus=0xFFFF,assocAid=0;
    int eapolMsg1=0,oracleL=0,eapolMsg2Sent=0,eapolMsg3=0,oracleM=0;
    int gtkUnwrapped=0,gtkOk=0,oracleN=0;
    int eapolMsg4Sent=0,protectedSeen=0,eapolRetried=0,oracleO=0;
    int ccmpTried=0,ccmpOk=0,ccmpSane=0,oracleP=0;
    /* k44's A/B, indexed by window: 0 = zero BSSID filter, 1 = real BSSID filter. */
    int winData[2]={0,0},winGroup[2]={0,0},winProt[2]={0,0},oracleQ=0;
    /* k46's hammer: dwells that read zero, per path. k47: the bisect rung that recovered. */
    int abDeadA=0,abDeadB=0,oracleR=0,bisectWinner=-1;
    /* Stage 8-1: the first encrypted frame this project has ever transmitted. */
    int arp81Encrypted=0,arp81Sent=0,arp81Replied=0,arp81Matched=0;
    int arp81Protected=0,arp81Decrypted=0,arp81Try=0,oracleT=0;
    int arp81Acked=0,arp81Group=0,arp81Unicast=0,arp81Ran=0;
    /* Stage 8-5a/8-5b: the shim library and our own driver fragment. */
    int shimLibOk=0, shim85bInstalled=0, shim85bCalls=0;
    int shim85fOpened=0; OTItemCount shimPortIdx=0;
    int shimMacOk=0; UInt8 shimMac[6]; int shim85hBound=0;
    int shimCoresOk=0; UInt32 shimChipId=0,shimChipRev=0,shimNCores=0;
    SInt32 shimIdx80211=-1,shimIdxCC=-1,shimScanErr=1;
    int shimCoreUp=0; UInt32 shimTmsLow=0,shimArmed=0; SInt32 shimCoreErr=1;
    int shimFwOk=0; UInt32 shimFwRev=0,shimFwIrq=0; SInt32 shimFwErr=1;
    /* 8-2d-1: what came back out of the driver's own log ring. shimLogN is the headline --
     * 0 means the narration pipe is dead, and that is a different failure from a bring-up that
     * went wrong. shimLogDropped != 0 means the TAIL is missing, not the head. */
    UInt32 shimLogN=0, shimLogDropped=0, shimLogBytes=0; int shimLogReplayed=0;
    /* ⚠ 8-2d-1 needs to tell "the narration is broken" from "the narration never ran". Only
     * EnetHAL_Open calls ApShimNarrateOpen, so if OT never drove Open there is nothing to find
     * and a zero line count means NOTHING. The selector counts are read in a block that closes
     * long before Oracle U, so the one number that matters is carried out here. */
    int shimOpenCalls=-1;     /* -1 = the counters were never read at all */
    /* 8-2d-1b: is the loaded fragment the one built alongside this app? shimStaleSig is the
     * inference that needs no cooperation from the driver -- old exports present, new absent. */
    UInt32 shimBuild=0; int shimHaveBuild=0, shimStaleSig=0;
    /* 8-2d-2b: the driver's initvals counts. shimIvDecl is the ORACLE -- it comes out of the
     * firmware blob's own header, not from anything this code computed. */
    UInt32 shimIvApp=0, shimIvDecl=0, shimBsApp=0, shimBsDecl=0; SInt32 shimIvErr=1;
    /* 8-2d-3: the driver's own G-PHY init. shimPhyOk is ApPhyInitG's return -- 1 only if all
     * fifteen Stage 4 oracles passed, which makes this the strongest oracle in the project. */
    UInt32 shimGpioOk=0, shimBoardFlags=0, shimRadioOn=0, shimCoreWasUp=0; SInt32 shimPhyErr=1;
    /* 8-2e: the chip_init tail. shimTailMacctl is comparable against the APP's own macctlTail --
     * two fragments running the same extracted function on the same card. */
    UInt32 shimTailMacctl=0, shimTailTms=0, shimTailPretbtt=0, shimTailPrmax=0xFFFF;
    /* 8-2f: the driver's RX ring. shimDmaNBuf is the one to read first -- a partial ring is
     * refused rather than armed, so anything less than K3_RX_SLOTS means a physical-address
     * problem, not a DMA-engine problem. */
    UInt32 shimDmaRingPhys=0, shimDmaReadback=0, shimDmaNBuf=0, shimDmaRxStat=0;
    /* 8-2g: what the DRIVER heard. shimRxFrames is the headline of the whole increment. */
    UInt32 shimRxFrames=0, shimRxChan=0, shimRxFilter=0, shimRxMacOn=0, shimRxEverOn=0;
    UInt32 shimRxStatus=0, shimRxIndex=0; SInt32 shimRxFirst=-1, shimRxErr=1;
    int shimHaveRx=0, shimRxDecoded=0; UInt8 shimRxBuf[K3_PAGE];
    /* 8-3a: the driver's transmitter. shimTxAcked is the headline -- an ACK means a real AP
     * received our frame, checked its FCS and answered within a SIFS. */
    UInt32 shimTxRing=0, shimTxReadback=0, shimTxAcked=0, shimTxStats=0;
    SInt32 shimTxErr=1; int shimHaveTx=0; UInt8 shimTxBssid[6];
    /* 8-3b: authentication. shimAuthSeq and shimAuthStatus are the oracle -- both are fields the
     * ACCESS POINT chose, and nothing in this driver can manufacture them. */
    UInt32 shimAuthAlg=0, shimAuthSeq=0, shimAuthStatus=0xFFFF, shimAuthMs=0, shimAuthAcked=0;
    SInt32 shimAuthErr=1; int shimHaveAuth=0; UInt32 shimAuthGot=0;
    /* 8-3c: association. shimAssocAid is the strong half -- the AP allocates it. */
    UInt32 shimAssocCap=0, shimAssocStatus=0xFFFF, shimAssocAid=0, shimAssocMs=0;
    UInt32 shimAssocAcked=0, shimAssocGot=0; SInt32 shimAssocErr=1; int shimHaveAssoc=0;
    /* 8-4: the handshake. shimHsM3 is the oracle -- the AP verified OUR MIC. */
    UInt32 shimPmkMs=0, shimPmkNz=0, shimPtkNz=0, shimHsM1=0, shimHsM3=0;
    UInt32 shimHsM1Ms=0, shimHsM3Ms=0; SInt32 shimHsErr=1; int shimHaveHs=0;
    UInt8 shimRxBssid[6]; UInt8 shimRxSsid[34]; UInt8 shimRxSsidLen=0;
    SInt32 shimDmaErr=1; int shimDmaOk=0, shimHaveDma=0;
    SInt32 shimTailErr=1; int shimTailOk=0, shimHaveTail=0;
    int shimPhyOk=0, shimHavePhy=0;
    int shimIvOk=0, shimHaveIvs=0;
    UInt8 arp81Mac[6],arp81Ip[4];
    static const char *kRungName[K47_RUNGS] = {
      "no-op, 500 ms only (TIME CONTROL)",
      "gRxSlot = 0 (software only)     ",
      "program TX DMA controller 1     ",
      "program TX DMA controller 2     ",
      "program TX DMA controller 3     ",
      "program TX DMA controller 4     ",
      "MacSuspend + MacEnable          ",
      "DummyTransmission               ",
      "full RxRingArm                  " };
    int txMoved=0, txError=0, gotReply=0, descBad=0, slowReplies=0;
    int rxControlOk=0, targetFound=0; UInt8 targetBssid[6]; UInt8 targetChan=0;
    int scanRetried=0, scanRetryWorked=0;
    int ackedOn=-1, repliedOn=-1, phyTxErr=0, macTxErr=0, tssiMoved=0;
    int quietPhyTxErr=0, quietTssiMoved=0; UInt32 quietIrq=0; int bcastReplies=0;
    int dummyPhyTxErr=0; UInt32 dummyIrq=0;
    UInt16 tssiC0=0, tssiC1=0;
    UInt32 coreRev=0; UInt16 phyRev=0; int phyOk=0;
    UInt32 macctlPreTail=0,macctlTail=0,tmslowPreTail=0,tmslowPostTail=0;
    UInt16 pretbtt=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+660;
    win=NewWindow(NULL,&bounds,"\pAirPort " AP_RX_VER " - " AP_RX_STAGE " " AP_RX_TITLE,
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT " AP_RX_STAGE " " AP_RX_VER " -- " AP_RX_TITLE " ===");

    /* ★★★★★ [8-14] DID OPEN TRANSPORT CALL ValidateHardware AT BOOT?
     *
     * ⚠⚠ THIS RUNS FIRST, AHEAD OF EVERY GetSharedLibrary, AND THAT PLACEMENT IS THE POINT.
     *
     * k100 asked the same question and could not answer it. Its beacon reader lived inside the
     * shim's own open narration -- so when the change under test (file type 'ndrv') broke CFM's
     * by-name lookup, the reader went with it and the run reported nothing either way. I called
     * the hypothesis refuted on a measurement that had never been taken.
     * [[feedback_freshness_witness_outside_the_test]] says a witness must sit OUTSIDE the thing
     * it witnesses, and this one now does: the Name Registry needs no CFM connection, no shim,
     * no bring-up and no hardware. Even a run that dies in the first second answers this.
     *
     * The property is written by ValidateHardware onto the RegEntryID that OPEN TRANSPORT hands
     * it, which is why we search the whole registry for the property rather than looking on the
     * node our own bring-up finds. If those two nodes differ, this prints the one OT used and
     * the difference is itself the finding. */
    Say("");
    Say("=== [8-15] ★★★ WHICH BOOT HOOK FIRED? (no CFM, no hardware) ===");
    /* ★ TWO hypotheses are armed at once, and they are distinguishable because each writes its
     * own property. That is deliberate: they are not variants of one change, they are two
     * DIFFERENT mechanisms, and the k102 run proved the cheap one costs nothing to leave in.
     *
     *   airport-ot-validate  ValidateHardware -- the node-named PCI driver route. k102 says no,
     *                        and the port list explains why (module "gmac" exists with no
     *                        fragment on disk, so that route starts in the ROM). Left armed
     *                        because disarming it would be a second variable and it is free.
     *   airport-ot-scan      OTScanPorts -- the OTKrnl$ PORT SCANNER, which is how an
     *                        EXTENSION-delivered Ethernet registers a port. This is the one
     *                        k103 exists to test. */
    { int hook;
      for(hook = 0; hook < 6; hook++){
        const char *prop  = (hook == 0) ? "airport-ot-validate"
                          : (hook == 1) ? "airport-ot-scan"
                          : (hook == 2) ? "airport-init-ran"
                          : (hook == 3) ? "airport-nm-ran"
                          : (hook == 4) ? "airport-ot-port"
                                        : "airport-hal-open";
        const char *label = (hook == 0) ? "ValidateHardware (the node-named PCI route)"
                          : (hook == 1) ? "OTScanPorts (the OTKrnl$ port scanner)"
                          : (hook == 2) ? "★ InstallMe -- the 68K INIT reached our PPC fragment"
                          : (hook == 3) ? "★ the NM response -- TASK level, post-boot, OT up"
                          : (hook == 4) ? "★★★ THE PORT REGISTERED AT BOOT, with no app running"
                                        : "★★★★★ OPEN TRANSPORT ACTUALLY OPENED OUR DRIVER";
        RegEntryIter it; RegEntryID found; Boolean done = false; OSStatus e;
        e = RegistryEntryIterateCreate(&it);
        if(e != noErr){
          Say1("  [8-15] could not iterate the Name Registry, err = ",(unsigned long)e);
          continue; }
        e = RegistryEntrySearch(&it, kRegIterContinue, &found, &done, prop, NULL, 0);
        if(e == noErr && !done){
          Str255 L; L[0]=0;
          PCat(L,"  [8-15] ★★★★★ FIRED: "); PCat(L,(char*)label); Out(L);
          if(hook == 1){
            Say("          ⇒ Open Transport ran our port scanner at boot.");
          } else if(hook == 2){
            Say("          ⇒ The boot vehicle works: a 68K INIT loaded our PowerPC fragment and");
            Say("            called it through Mixed Mode, at startup, with no application.");
          } else if(hook == 5){
            /* ⚠⚠ THE ONE THING k107 COULD NOT SAY. Its "EnetHAL_Open calls = 0" was read from
             * THIS app's connection to AirPortShim; the boot vehicle holds its own, and an
             * INIT-loaded fragment can have its own globals. So that zero was consistent with
             * both "OT never called us" and "OT called a copy you cannot see". This property is
             * written from inside EnetHAL_Open itself and lives outside every copy. */
            RegPropertyValueSize vs = (RegPropertyValueSize)sizeof(UInt32);
            UInt32 v = 0;
            if(RegistryPropertyGet(&found,"airport-hal-open",&v,&vs) == noErr)
              Say1("          EnetHAL_Open has run this boot, times = ",
                   (unsigned long)(v & 0xFFFFUL));
            Say("          ⇒ OT reached the driver. Whatever failed, failed AFTER this point.");
          } else if(hook == 4){
            /* ⚠⚠ THE VALUE CARRIES THE OUTCOME, and reading it as presence-only would call a
             * FAILED registration a success. ap_boot.c stores (ShimRefNum | 0x80000000) on
             * noErr and the raw OSErr otherwise, so the top bit is the verdict. */
            RegPropertyValueSize vs = (RegPropertyValueSize)sizeof(UInt32);
            UInt32 v = 0;
            if(RegistryPropertyGet(&found,"airport-ot-port",&v,&vs) == noErr){
              if(v & 0x80000000UL){
                Say1("          ★★★★★★ EnetShimInstallDriver SUCCEEDED at boot. ShimRefNum = ",
                     (unsigned long)(v & 0x7FFFFFFFUL));
                Say("          ⇒ the port was created before this app existed. Look at the port");
                Say("            listing below: if it is ACTIVE, \"AirPort\" is selectable in");
                Say("            TCP/IP with nothing running.");
                /* ⛔ AND DO NOT REGISTER A SECOND ONE. The boot vehicle owns this port now;
                 * a second EnetShimInstallDriver would put two drivers behind one card. */
                witnessMode = 1;
                Say("          [8-10] witness mode FORCED -- the boot vehicle owns the port.");
              } else {
                Say1("          ⛔ the boot vehicle tried and FAILED. OSErr = ",(unsigned long)v);
              }
            }
          } else if(hook == 3){
            /* ⚠ The VALUE matters here, not just presence: the NM response stores OT's Gestalt
             * attributes, so a zero means it ran but Open Transport was NOT up -- which would be
             * a real finding and the opposite of what the deferral exists to guarantee. */
            RegPropertyValueSize vs = (RegPropertyValueSize)sizeof(UInt32);
            UInt32 otAttr = 0;
            if(RegistryPropertyGet(&found,"airport-nm-ran",&otAttr,&vs) == noErr){
              Say1("          Open Transport attributes seen at NM time = ",
                   (unsigned long)otAttr);
              if(otAttr == 0)
                Say("          ⚠ ZERO -- the NM fired but OT was not up. Read 'AirPort NM Log'.");
              else
                Say("          ⇒ ★★★★★ THE WHOLE VEHICLE IS PROVEN. Boot-time execution, task-level");
            }
            Say("            deferral, and Open Transport up and ready. EnetShimInstallDriver");
            Say("            goes exactly here next, and the port appears in TCP/IP.");
          }
          (void)RegistryEntryIDDispose(&found);
        } else {
          Str255 L; L[0]=0;
          PCat(L,"  [8-15] no: "); PCat(L,(char*)label); Out(L);
          Say1("          nothing carries that property; search err = ",(unsigned long)e); }
        (void)RegistryEntryIterateDispose(&it); } }
    /* ★★★ THE ORACLE, AND IT RUNS UNCONDITIONALLY AND EARLY.
     *
     * Every previous port listing in this app sits inside the registration path, so a run that
     * skipped registration -- which witness mode does, and which is now the EXPECTED path --
     * printed no ports at all. The question "is AirPort in OT's port list before this app does
     * anything" is the whole objective of stage 8, and it must not be answerable only on the
     * branch where the app interferes. [[feedback_freshness_witness_outside_the_test]] */
    /* ⛔ OPEN TRANSPORT IS INITIALISED HERE, BEFORE THE FIRST OT CALL OF ANY KIND.
     *
     * k109 crashed in OTCfigNewConfiguration -> OTAllocSharedClientMem because [8-20] ran ahead
     * of the InitOpenTransport that used to live down in [8-5b]. The subtle part, and the reason
     * it is fixed HERE rather than at [8-20]: OTGetIndexedPort just below has ALWAYS run before
     * initialisation, in k107, k108 and k109, and always worked. Port enumeration reads a
     * registry; configuration allocates client memory. So three clean runs were evidence of
     * nothing, and "an OT call worked" never meant OT was initialised.
     *
     * Nesting is fine -- [8-5b] calls it again and OT reference-counts init/close pairs. */
    Say("");
    { OSStatus ie = InitOpenTransport();
      Say1("  InitOpenTransport (before ANY OT call) -> ",(unsigned long)ie); }
    ApListOtPorts("  [8-16] ★ OT's ports AS THEY STAND AT LAUNCH (before this app does anything):");
    Say("         ⇒ compare against enet0/gmac, the machine's own Ethernet: portFlags bit 0 is");
    Say("           ACTIVE. A port that exists but is not ACTIVE is one OT will not offer.");
    Say("");
    Say("         ⇒ Hooks 0 and 1 are k102/k103's negatives, left armed because they are free.");
    Say("           Hooks 2 and 3 are the BOOT VEHICLE: \"AirPort Extreme Startup\" must be in");
    Say("           the Extensions folder beside the driver, or both will read no.");
    Say("           Logs: 'AirPort INIT Log' (68K), 'AirPort Boot Log' (PPC), 'AirPort NM Log'.");

    /* ★★★★★ STAGE 8-5a: WHICH DRIVER ARCHITECTURE IS EVEN AVAILABLE?
     *
     * This runs FIRST, before any radio work, deliberately: it is the only question in this run
     * that decides what gets written next, and ~30% of runs die before reaching Stage 6. Put it
     * at the top and even a void run answers it.
     *
     * THE FORK. Stage 8-5 can be built two ways and they differ by an order of magnitude:
     *
     *   (A) A native OT PCI DLPI driver -- the five exports FEASIBILITY.md §2.1 pulled out of
     *       three shipping binaries (ValidateHardware, InitStreamModule, TerminateStreamModule,
     *       GetOTInstallInfo, TheDriverDescription), with our own streamtab, qinits and every
     *       DLPI primitive written by hand. This is what `pci1011,14` and `OTModl$radio` do.
     *
     *   (B) Apple's ETHERNET SHIM. usb-ddk/Examples/USBEnetSample implements ~11 selectors behind
     *       ONE exported entry point (EnetMac_ShimEntry) and lets `EnetShimLib` provide all of
     *       STREAMS and DLPI. The selector set -- Open, Close, Start, Stop, Read, Write,
     *       Get/SetMACAddress, SetMulticastFilters, Status -- is device-agnostic; the parameter
     *       block is called USBEnet but holds nothing but buffer, count, completion and result.
     *       ⭐ §2.1 already guessed this path existed: it noted Apple Enet's PCI drivers are
     *          "presumably bound by name through EnetShimLib". The sample confirms it, WITH
     *          source, and it was sitting in usb-ddk the whole time.
     *
     * (B) is a fraction of the work and is Apple's supported path with a working example. It is
     * only available if EnetShimLib is actually on this machine -- the shim ships with Mac OS USB
     * and Apple Enet, and nothing guarantees it is installed here.
     *
     * ⚠ THIS IS A READING QUESTION AND COSTS NOTHING TO ASK. GetSharedLibrary and FindSymbol are
     *   task-level, allocate nothing we keep, and touch no hardware. The connection is closed
     *   again immediately. Whatever it answers, no code has been written on a guess. */
    Say("");
    /* ★★★ [8-19b] CAN OPEN TRANSPORT EVEN OPEN THIS PORT?
     *
     * k107's port record reads `enet1  module=USBEnet`. That module name is not ours -- it is
     * EnetShimLib's, and OT resolves it by loading the CFM fragment "OTModl$USBEnet", which
     * lives in Apple's `Apple Enet DLPI Support` extension. If that file is not installed, the
     * port registers perfectly, appears in the pop-up, and can NEVER be opened -- which is
     * precisely the shape of what k107 observed: selected in TCP/IP, DHCP fell back to
     * link-local, and our driver received zero selector calls.
     *
     * ⚠ kReferenceCFrag, not kLoadCFrag: this asks whether CFM knows the name, and must not
     *   prepare a STREAMS module as a side effect of asking. */
    /* ★★★★★ [8-20] THE ONE MECHANISM THAT HAS EVER WORKED, POINTED AT THE BOOT-REGISTERED PORT.
     *
     * k108 settled that OT never called EnetHAL_Open at all -- the beacon written from inside
     * Open, outside every fragment copy, is absent. Registered, visible, selectable, Apple's
     * OTModl$USBEnet present, and still nothing opened it.
     *
     * Reading the banked logs, the ONLY way OT has ever driven this driver is an application
     * opening an endpoint on the port (k55 onward). Registration plus a link report has never
     * been sufficient, in any run. So ask that question directly, against the port the BOOT
     * VEHICLE registered rather than one the app made for itself:
     *
     *   opens, and the driver's selectors fire  -> the port is fine; the gap is that nothing
     *                                              opens it automatically, and TCP/IP is the
     *                                              thing to look at (portFlags ACTIVE is 0).
     *   refuses                                 -> the boot-registered port is defective, and
     *                                              the error code says how.
     *
     * ⚠ SAFE ONLY BECAUSE NOTHING HOLDS IT. The app has refused to open this port since 8-10,
     *   on the grounds that opening a port another client already holds is destructive -- and
     *   that is still right. k108's beacon is what makes it safe here: no Open has happened, so
     *   there is no other client. The check is made again below rather than assumed. */
    Say("");
    Say("=== [8-20] ★★★ WILL THE BOOT-REGISTERED PORT OPEN AT ALL? ===");
    /* ⛔⛔ InitOpenTransport FIRST, AND k109 CRASHED FOR WANT OF IT.
     *
     * This block was placed ahead of [8-5b], which is where the app had always called
     * InitOpenTransport -- so OTCreateConfiguration ran with Open Transport uninitialised in
     * this context and took the machine to MacsBug:
     *
     *     PowerPC access exception at 8528FFC0   ("not in RAM or ROM")
     *       5DE832A8  PPC  00EB2EF4  OTCfigNewConfiguration+00084
     *       5DE83268  PPC  00EB0DD0  OTAllocSharedClientMem+00020
     *
     * ⚠ AND THE REASON IT WAS NOT OBVIOUS IS WORTH KEEPING: OTGetIndexedPort works fine without
     *   initialisation -- [8-16] listed all six ports two lines earlier. Port ENUMERATION reads
     *   a registry; CONFIGURATION allocates OT client memory. "An OT call worked" is not
     *   evidence that OT is initialised.
     *
     * Nesting is supported: [8-5b] calls InitOpenTransport again below, and OT reference-counts
     * init/close pairs. */
    { OTPortRecord prq; OTItemCount idxq = 0; int held = 0;
      /* ★ k158: gate on CURRENT ownership (enet1/USBEnet portFlags ACTIVE bit), NOT the
       * airport-hal-open beacon. The beacon only means EnetHAL_Open ran at SOME point this boot
       * (the driver self-associates at boot, then the port settles free); k157 skipped opening on
       * the beacon while enet1 showed portFlags=0x0 (NOT active = free), so the 8-58 receive test
       * never ran. The real destructive case (8-10) is opening a port ANOTHER CLIENT holds NOW --
       * portFlags bit 0. Gate on that; a free port is safe (and, for 8-58, necessary) to open. */
      while(OTGetIndexedPort(&prq,idxq)){
        const char *aq=(const char*)prq.fModuleName, *bq="USBEnet"; int kq=0;
        while(aq[kq] && bq[kq] && aq[kq]==bq[kq]) kq++;
        if(aq[kq]==0 && bq[kq]==0){ if(prq.fPortFlags & 0x00000001UL) held = 1; break; }
        idxq++; }
      if(held){
        Say("  [--] SKIPPED: enet1/USBEnet is ACTIVE -- a client holds it NOW; opening would be");
        Say("       the destructive case 8-10 guards against (set OT off AirPort and retry).");
      } else {
        OTPortRecord pr; OTItemCount idx = 0; int found = -1;
        while(OTGetIndexedPort(&pr,idx)){
          /* fModuleName is a C string (OTPortRecord, char[32]). Compare it plainly rather
           * than reaching for a Pascal helper that does not exist in this file. */
          { const char *a = (const char *)pr.fModuleName, *b = "USBEnet"; int k = 0;
            while(a[k] && b[k] && a[k] == b[k]) k++;
            if(a[k] == 0 && b[k] == 0){ found = (int)idx; break; } }
          idx++; }
        if(found < 0){
          Say("  [!!] no port with module \"USBEnet\" -- the boot registration did not survive.");
        } else if(OTGetIndexedPort(&pr,(OTItemCount)found)){
          OSStatus oerr = kOTNoError;
          OTConfigurationRef cfg;
          { Str255 L; L[0]=0; PCat(L,"  opening port \""); PCat(L,(char*)pr.fPortName);
            PCat(L,"\" (module \""); PCat(L,(char*)pr.fModuleName); PCat(L,"\")"); Out(L); }
          cfg = OTCreateConfiguration((char*)pr.fPortName);
          if(cfg == kOTInvalidConfigurationPtr || cfg == kOTNoMemoryConfigurationPtr){
            Say("  [!!] OTCreateConfiguration REFUSED the port name. OT does not consider this");
            Say("       a configurable endpoint -- that is the finding, and it is upstream of");
            Say("       anything our driver does.");
          } else {
            EndpointRef ep = OTOpenEndpoint(cfg,0,NULL,&oerr);
            Say1("  OTOpenEndpoint -> err = ",(unsigned long)oerr);
            if(oerr == kOTNoError && ep != kOTInvalidEndpointRef){
              Say("  [ok] ★★★ THE STREAM OPENED. OT plumbed the boot-registered port.");
              ApListOtPorts("  AFTER opening it:");
              /* ★ AND ASK THE BEACON, NOT A COUNTER. airport-hal-open is written from INSIDE
               * EnetHAL_Open and lives in the Name Registry, so it answers "did this open
               * reach our driver" without needing a connection to any particular copy of the
               * fragment -- which is the exact ambiguity k107 could not resolve. */
              { RegEntryIter i2; RegEntryID f2; Boolean d2 = TRUE; int ran = 0;
                if(RegistryEntryIterateCreate(&i2) == noErr){
                  if(RegistryEntrySearch(&i2,kRegIterContinue,&f2,&d2,
                                         "airport-hal-open",NULL,0) == noErr && !d2){
                    RegPropertyValueSize vs = (RegPropertyValueSize)sizeof(UInt32);
                    UInt32 v = 0; ran = 1;
                    if(RegistryPropertyGet(&f2,"airport-hal-open",&v,&vs) == noErr)
                      Say1("    ★★★★★★ EnetHAL_Open RAN. times = ",(unsigned long)(v & 0xFFFFUL));
                    (void)RegistryEntryIDDispose(&f2); }
                  (void)RegistryEntryIterateDispose(&i2); }
                if(!ran){
                  Say("    ⚠ the stream opened but EnetHAL_Open still did NOT run. OT plumbed");
                  Say("      the port without ever reaching our HAL -- the break is between");
                  Say("      OTModl$USBEnet / EnetShimLib and us, not in OT's port handling.");
                } else {
                  /* ⚠⚠ AND THIS APP CANNOT SEE THAT DRIVER. k110 printed
                   *      EnetHAL_Open RAN. times = 1          (the Name Registry beacon)
                   *      selector calls our driver received = 0   (this app's own connection)
                   * Two instances of one fragment, separate globals -- the boot vehicle's
                   * GetSharedLibrary and ours returned different copies, and EnetShimLib calls
                   * the one the boot vehicle registered. So do not read the accessors below as
                   * the driver's state; they describe a copy that owns nothing. */
                  Say("    ⚠⚠ NOTE WHICH COPY. The accessors this app reads belong to a DIFFERENT");
                  Say("       instance of AirPortShim than the one OT just drove -- k110 showed");
                  Say("       the beacon at 1 while this app's selector count stayed 0.");
                  Say("       ⇒ THE DRIVER'S OWN ACCOUNT IS IN \"AirPort Driver Log\", written");
                  Say("         from inside EnetHAL_Open. Read that, not the numbers below."); } }
              { /* ★★★★★★ 8-58 (k157): SAP-0x0800 RECEIVE ISOLATION -- run HERE, in WITNESS mode,
                 * because the extension owns the card so the app-drives bind ladder never runs. This
                 * app is a pure OT client: bind 0x0800 on the endpoint OT just plumbed and dwell,
                 * logging what the BOUND stream receives. We do NOT read this app's driver accessors
                 * (k110: wrong fragment copy). The driver delivering unicast to the shim is already
                 * proven (g4boot: 21 ICMP); with the Pi's own capture confirming the pings went out,
                 * rcvUniIcmp alone splits "shim delivers unicast" from "shim drops unicast". */
                OSStatus se58, be58; T8022Address ba58; TBind rq58;
                UInt8 ab58[32], db58[1600], sb58[4][40];
                UInt32 rcvTot=0, rcvUni=0, rcvOth=0, sN=0, sL[4];
                long d58; int i58;
                for(i58=0;i58<4;i58++) sL[i58]=0;
                se58 = OTSetSynchronous(ep); SayOtErr("  8-58 OTSetSynchronous -> ",se58);
                for(i58=0;i58<6;i58++) ba58.fHWAddr[i58]=0;
                for(i58=0;i58<5;i58++) ba58.fSNAP[i58]=0;
                ba58.fAddrFamily=AF_8022; ba58.fSAP=0x0800;
                rq58.addr.buf=(UInt8*)&ba58; rq58.addr.len=k8022BasicAddressLength;
                rq58.addr.maxlen=sizeof(ba58); rq58.qlen=0;
                be58 = OTBind(ep,&rq58,(TBind*)0); SayOtErr("  8-58 OTBind(SAP 0x0800) -> ",be58);
                if(be58==kOTNoError){
                  Say("  ========== 8-58 (k157): SAP-0x0800 RECEIVE (witness) ==========");
                  Say("  ★ BOUND to 0x0800. On the Pi:  sudo arp -s 192.168.1.225 02:00:00:00:00:01");
                  Say("    then:  ping -c 100 -i 0.2 192.168.1.225");
                  Say("  ★★★ PING NOW -- dwelling ~45 s, receiving on the bound stream.");
                  d58 = TickCount();
                  while((TickCount()-d58) < 2700L){
                    TUnitData ud; OTFlags fl; OSStatus re; EventRecord e3;
                    ud.addr.buf=ab58; ud.addr.maxlen=sizeof(ab58); ud.addr.len=0;
                    ud.opt.buf=0; ud.opt.maxlen=0; ud.opt.len=0;
                    ud.udata.buf=db58; ud.udata.maxlen=sizeof(db58); ud.udata.len=0;
                    re = OTRcvUData(ep,&ud,&fl);
                    if(re==kOTNoError && ud.udata.len>0){
                      UInt32 n=(UInt32)ud.udata.len; rcvTot++;
                      if(n>=20u && (db58[0]&0xF0u)==0x40u){
                        UInt32 dst=((UInt32)db58[16]<<24)|((UInt32)db58[17]<<16)
                                  |((UInt32)db58[18]<<8)|db58[19];
                        if(db58[9]==1u && dst==0xC0A801E1UL) rcvUni++; else rcvOth++; }
                      if(sN<4u){ UInt32 c,lim=(n<40u)?n:40u;
                        for(c=0;c<lim;c++) sb58[sN][c]=db58[c]; sL[sN]=lim; sN++; }
                    } else if(re==kOTLookErr){ OTResult lk=OTLook(ep);
                        if(lk==T_UDERR)(void)OTRcvUDErr(ep,(TUDErr*)0); }
                    if(WaitNextEvent(everyEvent,&e3,1,NULL)){
                      if(e3.what==updateEvt && win && (WindowPtr)e3.message==win){
                        BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } }
                    ApDrawLog(win); }
                  Say("");
                  Say("  --- 8-58 RESULT (send me this + the Pi capture) ---");
                  Say1("    frames this bound 0x0800 stream RECEIVED (total) = ",(unsigned long)rcvTot);
                  Say1("      ...of which UNICAST ICMP to .225              = ",(unsigned long)rcvUni);
                  Say1("      ...other IP                                   = ",(unsigned long)rcvOth);
                  { UInt32 si; for(si=0;si<sN;si++){
                      Say1("    received SDU sample # (<=40 bytes) ",(unsigned long)(si+1));
                      HexDump(sb58[si],(int)sL[si],0); } }
                  Say("");
                  if(rcvUni>0)
                    Say("  ⇒ ★ BOUND STREAM GOT THE UNICAST PING -> shim delivers unicast IP; wall is OT's own TCP/IP stream.");
                  else
                    Say("  ⇒ bound stream got NO unicast ping. With the Pi capture showing the pings went out, and the driver already proven to deliver unicast to the shim, this means the SHIM drops unicast IP before the bound stream.");
                  Say("  ===============================================================");
                  (void)OTUnbind(ep); }
              }
              (void)OTCloseProvider(ep);
              Say("  (endpoint closed again -- this probe leaves nothing holding the port)");
            } else {
              Say("  [!!] OT REFUSED to open the boot-registered port. The error above is the");
              Say("       most specific thing we have; it is upstream of our driver entirely.");
            } } } } }

    Say("");
    Say("=== [8-19b] ★ IS THE MODULE OT MUST LOAD INSTALLED? ===");
    { static const char *kFrags[] = { "\pOTModl$USBEnet",
                                      "\pOTPortCfg$USBEnetCfgHelper",
                                      "\pOTKrnl$USBOTPortScanner", 0 };
      int i;
      for(i = 0; kFrags[i]; i++){
        CFragConnectionID c = 0; Ptr a = 0; Str255 em; OSErr e;
        e = GetSharedLibrary((ConstStr255Param)kFrags[i], kPowerPCCFragArch,
                             kReferenceCFrag, &c, &a, em);
        { Str255 L; L[0]=0; PCat(L, e == noErr ? "  [ok] " : "  [!!] ");
          PCat(L, (char*)kFrags[i] + 1); PCat(L, e == noErr ? " present" : " MISSING"); Out(L); }
        if(e == noErr) (void)CloseConnection(&c); } }
    Say("    ⇒ all three live in Apple's \"Apple Enet DLPI Support\" extension. If OTModl$USBEnet");
    Say("      is MISSING, that alone explains a port that registers, shows in TCP/IP, and can");
    Say("      never be opened -- and it is a one-file install, not a code change.");

    Say("=== [8-5a] ★ IS APPLE'S ETHERNET SHIM PRESENT? (decides the Stage 8 architecture) ===");
    { CFragConnectionID cid = 0; Ptr fragAddr = 0; Str255 errMsg; OSErr e;
      Ptr sym; CFragSymbolClass cls;
      e = GetSharedLibrary("\pEnetShimLib",kPowerPCCFragArch,kLoadCFrag,&cid,&fragAddr,errMsg);
      Say1("  GetSharedLibrary(\"EnetShimLib\") -> ",(unsigned long)e);
      if(e != noErr){
        Say("  [--] NOT PRESENT on this machine.");
        Say("    ⇒ Stage 8-5 takes path (A): a native OT PCI DLPI driver, five exports, our own");
        Say("      streamtab. FEASIBILITY.md §2.1 has the contract and the shipping exemplars.");
        Say("    ⇒ Before accepting that, check whether the shim merely is not INSTALLED rather");
        Say("      than not available: it ships with Mac OS USB and with Apple Enet, so an");
        Say("      Extensions folder that has 'Apple Enet' may be one drag away from path (B).");
      } else {
        int okI,okR,okA;
        Say("  [ok] ★ EnetShimLib IS PRESENT.");
        sym=0; okI = (FindSymbol(cid,"\pEnetShimInstallDriver",&sym,&cls)==noErr)
                     && (cls==kTVectorCFragSymbol);
        sym=0; okR = (FindSymbol(cid,"\pEnetShimRemoveDriver", &sym,&cls)==noErr)
                     && (cls==kTVectorCFragSymbol);
        sym=0; okA = (FindSymbol(cid,"\pEnetShimAsyncStatus",  &sym,&cls)==noErr)
                     && (cls==kTVectorCFragSymbol);
        SayOk("  EnetShimInstallDriver is exported as a TVector",okI);
        SayOk("  EnetShimRemoveDriver  is exported as a TVector",okR);
        SayOk("  EnetShimAsyncStatus   is exported as a TVector",okA);
        if(okI && okR && okA){
          Say("    ⇒ ★★★ STAGE 8-5 TAKES PATH (B). Implement EnetMac_ShimEntry with the eleven");
          Say("      selectors and let Apple's shim be STREAMS and DLPI. That is a fraction of");
          Say("      the work of a hand-written DLPI driver, on Apple's supported path, with a");
          Say("      complete worked example already in usb-ddk.");
        } else {
          Say("    ⇒ ⚠ The library loaded but its entry points are not all there, which is worse");
          Say("      than absent because it means the name matched something unexpected. Do NOT");
          Say("      build against it; treat this as path (A) and record what was found."); }
        /* ⚠ Close it again. This probe must not leave a CFM connection open on a library it is
         * not using -- and FindSymbol takes no reference, so nothing here may be cached. */
        (void)CloseConnection(&cid);
        Say("  (connection closed; nothing cached -- FindSymbol takes no reference)");
        shimLibOk = 1; } }
    Say("");

    /* ★★★★★ STAGE 8-5b: REGISTER OUR DRIVER WITH THE SHIM AND SEE IF OT DRIVES IT.
     *
     * The whole question is: will EnetShimLib accept a registration from a fragment that is
     * neither a USB class driver nor a PCI-matched ndrv, and will OT then call our selectors?
     *
     * The driver is AirPortShim.shlb -- a CFM shared library, built from ap_shim.c, exporting
     * EnetHAL_Entry. It contains NO radio code: it counts the selectors it is asked for and
     * refuses the ones it cannot honestly serve. This run reads those counters back out.
     *
     * ⚠ THIS MAKES A SYSTEM-VISIBLE CHANGE, briefly. A successful registration puts a new network
     *   port in front of Open Transport -- it may appear in the TCP/IP control panel. We remove it
     *   again before the run ends, on every path, and a port that was never installed cannot be
     *   left behind. If the machine is later odd about networking, reboot: nothing here writes
     *   anything persistent. */
    Say("=== [8-5b] ★★★ REGISTER THE SHIM DRIVER -- DOES OT CALL US? ===");
    if(!shimLibOk){
      Say("  [--] skipped: EnetShimLib is not available, so there is nothing to register with.");
    } else {
      CFragConnectionID drvCid = 0, shimCid = 0;
      Ptr fragAddr = 0; Str255 errMsg; OSErr e; CFragSymbolClass cls;
      int otUp = 0;

      /* ⚠ OT MUST BE INITIALISED BEFORE THE CLIENT PORT API, and if it is not, OTGetIndexedPort
       * returns false immediately -- which reads identically to "there are no ports". That is the
       * failure mode this whole increment exists to avoid, so the result is checked and reported
       * rather than assumed. */
      { OSStatus oe = InitOpenTransport();
        otUp = (oe == noErr);
        Say1("  InitOpenTransport -> ",(unsigned long)oe);
        if(!otUp) Say("  ⚠ OT did not initialise; the port listings below would be empty for that"
                      " reason alone and must not be read as 'no ports exist'."); }
      ShimInstallProc  installProc = 0;
      ShimRemoveProc   removeProc  = 0;
      ShimAsyncProc    asyncProc   = 0;
      ShimStatsProc    statsProc   = 0;
      ShimGetMacProc   getMacProc  = 0;
      ShimGetCoresProc getCoresProc= 0;
      ShimArmProc      armProc     = 0;
      ShimCoreStateProc coreStProc = 0;
      ShimGetFwProc    getFwProc   = 0;
      ShimGetLogProc   getLogProc  = 0;          /* 8-2d-1 */
      ShimGetBuildProc getBuildProc = 0;         /* 8-2d-1b */
      ShimGetIvsProc   getIvsProc  = 0;          /* 8-2d-2b */
      ShimGetPhyProc   getPhyProc  = 0;          /* 8-2d-3 */
      ShimGetTailProc  getTailProc = 0;          /* 8-2e */
      ShimGetDmaProc   getDmaProc  = 0;          /* 8-2f */
      ShimRxPollProc   rxPollProc  = 0;          /* 8-2g */
      ShimGetRxBufProc getRxBufProc= 0;
      ShimQuiesceProc  quiesceProc = 0;
      ShimGetRxProc    getRxProc   = 0;
      ShimGetTxProc    getTxProc   = 0;          /* 8-3a */
      ShimGetBssidProc getBssidProc= 0;
      ShimTxProbeProc  txProbeProc = 0;
      ShimAuthProc     authProc    = 0;          /* 8-3b */
      ShimGetAuthProc  getAuthProc = 0;
      ShimAssocProc    assocProc   = 0;          /* 8-3c */
      ShimGetAssocProc getAssocProc= 0;
      ShimPrepPmkProc  prepPmkProc = 0;          /* 8-4 */
      ShimHandshakeProc hsProc     = 0;
      ShimGetHsProc    getHsProc   = 0;
      ShimPumpProc     pumpProc    = 0;          /* 8-6a */
      ShimGetDlpiProc  getDlpiProc = 0;
      ShimSetFilterProc setFilterProc = 0;          /* 8-6d */
      ShimArmIrqProc   armIrqProc    = 0;           /* 8-7 */
      ShimArmIrqProc   disarmIrqProc = 0;
      ShimGetIrqProc   getIrqProc    = 0;
      ShimGetJoinProc  getJoinProc   = 0;           /* 8-9 */
      ShimTxEthProc    txEthProc     = 0;           /* 8-8 */
      ShimGetTxEthProc getTxEthProc  = 0;
      ShimRefNum       shimRef = 0;

      /* 1. Can CFM find OUR fragment by name? This is the first thing that can go wrong and it is
       *    entirely about packaging: the 'cfrg' names the fragment "AirPortShim", and the file
       *    must be in the Extensions folder for CFM to resolve it. */
      e = GetSharedLibrary("\pAirPortShim",kPowerPCCFragArch,kLoadCFrag,&drvCid,&fragAddr,errMsg);
      Say1("  GetSharedLibrary(\"AirPortShim\") -> ",(unsigned long)e);
      if(e != noErr){
        Say("  [--] OUR OWN LIBRARY WAS NOT FOUND. That is a packaging result, not a driver one:");
        Say("    AirPortShim.shlb must be in the Extensions folder (CFM resolves import libraries");
        Say("    by the fragment name in the 'cfrg', which is \"AirPortShim\"). Nothing about the");
        Say("    shim or about OT has been tested by this run.");
      } else {
        int haveEntry, haveStats;
        Ptr sym = 0;
        Say("  [ok] CFM found our fragment.");
        haveEntry = (FindSymbol(drvCid,"\pEnetHAL_Entry",&sym,&cls)==noErr)
                    && (cls==kTVectorCFragSymbol);
        sym = 0;
        haveStats = (FindSymbol(drvCid,"\pAirPortShimStats",&sym,&cls)==noErr)
                    && (cls==kTVectorCFragSymbol);
        if(haveStats) statsProc = (ShimStatsProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetMac",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getMacProc = (ShimGetMacProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetCores",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getCoresProc = (ShimGetCoresProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimArm",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) armProc = (ShimArmProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetCoreState",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) coreStProc = (ShimCoreStateProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetFw",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getFwProc = (ShimGetFwProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetLog",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getLogProc = (ShimGetLogProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetBuild",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getBuildProc = (ShimGetBuildProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetIvs",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getIvsProc = (ShimGetIvsProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetPhy",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getPhyProc = (ShimGetPhyProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetTail",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getTailProc = (ShimGetTailProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetDma",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getDmaProc = (ShimGetDmaProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimRxPoll",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) rxPollProc = (ShimRxPollProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetRxBuf",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getRxBufProc = (ShimGetRxBufProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimQuiesce",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) quiesceProc = (ShimQuiesceProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetRx",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getRxProc = (ShimGetRxProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetTx",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getTxProc = (ShimGetTxProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetBssid",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getBssidProc = (ShimGetBssidProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimTxProbe",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) txProbeProc = (ShimTxProbeProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimAuth",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) authProc = (ShimAuthProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetAuth",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getAuthProc = (ShimGetAuthProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimAssoc",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) assocProc = (ShimAssocProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetAssoc",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getAssocProc = (ShimGetAssocProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimPrepPmk",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) prepPmkProc = (ShimPrepPmkProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimHandshake",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) hsProc = (ShimHandshakeProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetHs",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getHsProc = (ShimGetHsProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimPump",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) pumpProc = (ShimPumpProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetDlpi",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getDlpiProc = (ShimGetDlpiProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimSetBssidFilter",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) setFilterProc = (ShimSetFilterProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimArmIrq",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) armIrqProc = (ShimArmIrqProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimDisarmIrq",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) disarmIrqProc = (ShimArmIrqProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetIrq",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getIrqProc = (ShimGetIrqProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimTxEthernet",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) txEthProc = (ShimTxEthProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetJoin",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getJoinProc = (ShimGetJoinProc)sym;
        sym = 0;
        if(FindSymbol(drvCid,"\pAirPortShimGetTxEth",&sym,&cls)==noErr
           && cls==kTVectorCFragSymbol) getTxEthProc = (ShimGetTxEthProc)sym;
        SayOk("  EnetHAL_Entry is exported (this is what the shim will call)",haveEntry);
        SayOk("  AirPortShimStats is exported (this is how we read the result)",haveStats);

        /* ★★★ 8-2d-1b: IS THIS THE SHIM WE JUST BUILT, OR THE ONE FROM LAST TIME?
         *
         * ⚠⚠ k65 WAS LOST TO THIS QUESTION GOING UNASKED. Every old export resolved, the new one
         *   did not, the driver ran perfectly, and the log said "the driver produced no lines" --
         *   which reads as a code defect. It was a stale AirPortShim.shlb: CFM builds its
         *   fragment registry AT BOOT by scanning Extensions, so a shim copied in without a
         *   restart is simply not the shim that runs.
         *
         * The build stamp answers it POSITIVELY when both sides have it. Until then, the export
         * table answers it by SHAPE: a fragment that resolves every old export and no new one is
         * the old fragment, and that inference does not need the driver to cooperate at all. */
        Say("");
        Say("  --- ★ which build of the shim is actually loaded? ---");
        if(getFwProc && !getLogProc) shimStaleSig = 1;
        if(getBuildProc){
          UInt32 b = (*getBuildProc)();
          Str255 L; L[0]=0;
          shimBuild = b; shimHaveBuild = 1;
          PCat(L,"    shim reports build "); PCatDec(L,(unsigned long)b);
          PCat(L,", this app expects ");     PCatDec(L,(unsigned long)AP_SHIM_BUILD_EXPECTED);
          Out(L);
          SayOk("  ★ the loaded shim MATCHES this app",
                b == (UInt32)AP_SHIM_BUILD_EXPECTED);
          if(b != (UInt32)AP_SHIM_BUILD_EXPECTED){
            Say("    ⚠⚠ MISMATCHED PAIR. The AirPortShim.shlb in the Extensions folder is not the");
            Say("       one built alongside this app. Nothing below that depends on the driver is");
            Say("       interpretable. Replace the extension and RESTART, then re-run."); }
        } else {
          Say("    [--] AirPortShimGetBuild did not resolve.");
          Say("      ⚠⚠ That export exists in every shim from 8-2d-1b onward, so its absence");
          Say("        means the loaded fragment is OLDER THAN THIS APP. Almost certainly a");
          Say("        stale AirPortShim.shlb: CFM registers Extensions at BOOT, so copying a");
          Say("        new one in without restarting leaves the previous fragment in place.");
          Say("      ⇒ Any 8-2d result below is UNRUN, not failed."); }
        Say("");
        Say("  --- which driver exports resolved (the export table dates the fragment) ---");
        SayOk("    AirPortShimGetMac        [since 8-5g]", getMacProc   != 0);
        SayOk("    AirPortShimGetCores      [since 8-2a]", getCoresProc != 0);
        SayOk("    AirPortShimArm           [since 8-2b]", armProc      != 0);
        SayOk("    AirPortShimGetCoreState  [since 8-2b]", coreStProc   != 0);
        SayOk("    AirPortShimGetFw         [since 8-2c]", getFwProc    != 0);
        SayOk("    AirPortShimGetLog        [since 8-2d-1]",  getLogProc   != 0);
        SayOk("    AirPortShimGetBuild      [since 8-2d-1b]", getBuildProc != 0);
        SayOk("    AirPortShimGetIvs        [since 8-2d-2b]", getIvsProc   != 0);
        SayOk("    AirPortShimGetPhy        [since 8-2d-3]",  getPhyProc   != 0);
        SayOk("    AirPortShimGetTail       [since 8-2e]",    getTailProc  != 0);
        SayOk("    AirPortShimGetDma        [since 8-2f]",    getDmaProc   != 0);
        SayOk("    AirPortShimRxPoll        [since 8-2g]",    rxPollProc   != 0);
        SayOk("    AirPortShimQuiesce       [since 8-2g]",    quiesceProc  != 0);
        SayOk("    AirPortShimGetTx         [since 8-3a]",    getTxProc    != 0);
        SayOk("    AirPortShimAuth          [since 8-3b]",    authProc     != 0);
        SayOk("    AirPortShimAssoc         [since 8-3c]",    assocProc    != 0);
        SayOk("    AirPortShimHandshake     [since 8-4]",     hsProc       != 0);
        if(getFwProc && !getLogProc)
          Say("    ⇒ ⚠⚠ OLD EXPORTS YES, NEW EXPORTS NO. That is the signature of a STALE shim.");

        /* ★★★★★★ 1b / STAGE 8-10: WITNESS MODE — IS SOMEBODY ELSE ALREADY USING THIS DRIVER?
         *
         * ⭐ THE POINT OF THIS BUILD. Once AirPort is selected in the TCP/IP control panel, OT
         *   opens the provider at boot and OWNS it. This application must then NOT register and
         *   must NOT call OTOpenEndpoint -- opening a port another client already holds is, at
         *   best, a conflict, and at worst the sort of thing that hangs a machine we have only
         *   just stopped hanging.
         *
         *   But every Get* accessor and AirPortShimGetLog are ORDINARY CFM FUNCTION CALLS. They
         *   need the fragment, not the endpoint. So the app can stand entirely outside Open
         *   Transport and still report everything the driver knows -- which is what keeps the
         *   standing rule intact: a diagnostic writes a log, it does not ask someone to
         *   photograph a control panel.
         *
         * ⚠ DETECTED, NOT CONFIGURED. A flag or a modifier key would be one more thing to get
         *   wrong on a run whose whole point is that nobody is driving. AirPortShimStats reports
         *   how many times EnetHAL_Open has been called; if that is non-zero before this app has
         *   registered anything, somebody else opened it and we are a witness. The driver
         *   answers the question; nobody has to remember to.
         *
         * ⚠ And it is stated in the log either way, because "the app quietly took a different
         *   path" is exactly how a run gets misread. */
        if(statsProc){
          /* ⚠⚠ THIS MUST MATCH AP_SHIM_NSEL IN ap_shim.c. AirPortShimStats fills the array
           * with that many entries, and it was widened from 11 to 16 at 8-27b to catch a
           * possible selector 11. An 11-element array here would have taken a 20-byte stack
           * overflow, silently, in exactly the way the RegPropertyName one-byte buffer did --
           * the callee decides the length and the type system says nothing. */
          UInt32 magic, tot = 0, cnts[AP_SHIM_NSEL_APP]; UInt32 q;
          for(q=0;q<AP_SHIM_NSEL_APP;q++) cnts[q]=0;
          magic = (*statsProc)(&tot,cnts,0,0,0,0);
          if(magic == 0x41505348UL && cnts[1] > 0) witnessMode = 1;   /* [1] == EnetHAL_Open */
          Say("");
          Say("  --- ★★★★★★ 8-10: WHO IS DRIVING? ---");
          Say1("    EnetHAL_Open calls before this app did anything = ",(unsigned long)cnts[1]);
          if(witnessMode){
            Say("    ★★★ WITNESS MODE. Something else -- almost certainly TCP/IP -- already");
            Say("      opened this driver. This app will NOT register and will NOT open the");
            Say("      endpoint. It reports what the driver has been doing and nothing else.");
          } else {
            Say("    nobody else has opened the driver, so this app drives it as usual."); } }

        /* ★★★★★★ THE WITNESS REPORT. Sixty one-second samples, and the DELTAS are the result.
         *
         * ⚠⚠ "STEADY" IS A CLAIM ABOUT DURATION, so a single snapshot cannot support it. Every
         *   run in this project so far has been a ten-second window and has reported totals;
         *   totals cannot tell a link that carried traffic for one second and then died from one
         *   that carried it for a minute. The per-second deltas can, and they are the only thing
         *   here that answers the question the run is being spent on.
         *
         * ⚠ It also watches for the two ways this can go quietly wrong: the receive queue
         *   overflowing (drops climbing) and the storm brake firing. Both are counters the
         *   driver keeps and neither is visible from a control panel. */
        if(witnessMode){
          UInt32 s;
          UInt32 pHard=0,pSec=0,pConv=0,pCons=0,pTx=0,pDrop=0;
          Say("");
          Say("  --- ★★★★★★ 8-10: SIXTY SECONDS OF SOMEBODY ELSE'S TRAFFIC ---");
          Say("    second |  IRQ/s  frames/s  collected/s   TX/s  | queue drops");
          for(s=0; s<60; s++){
            long t0 = TickCount();
            UInt32 hard=0,no=0,qd=0,qf=0,sec=0,secF=0,wb=0,rx=0,lr=0,ld=0;
            UInt32 pumped=0,isrc=0,cons=0,rc=0,rb=0,re=0,walked=0,why[AP_ENET_NREASON],ss=0;
            UInt32 tc=0,ts=0,ta=0,a=0,b=0,c=0,d=0,ee=0,f=0;
            int k; for(k=0;k<AP_ENET_NREASON;k++) why[k]=0;
            while(TickCount() - t0 < 60L){
              EventRecord ev;
              if(WaitNextEvent(everyEvent,&ev,6,NULL)){
                if(ev.what==updateEvt && win && (WindowPtr)ev.message==win){
                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } } }
            if(getIrqProc)   (void)(*getIrqProc)(&hard,&no,&qd,&qf,&sec,&secF,&wb,&rx,&lr,&ld);
            if(getDlpiProc)  (void)(*getDlpiProc)(&pumped,&isrc,&cons,&rc,&rb,&re,&walked,why,&ss);
            if(getTxEthProc) (void)(*getTxEthProc)(&tc,&ts,&ta,&a,&b,&c,&d,&ee,&f);
            { Str255 L; L[0]=0;
              PCat(L,"      "); PCatDec(L,(unsigned long)(s+1));
              PCat(L,"    |  "); PCatDec(L,(unsigned long)(hard   - pHard));
              PCat(L,"     ");   PCatDec(L,(unsigned long)(pumped - pConv));
              PCat(L,"        ");PCatDec(L,(unsigned long)(cons   - pCons));
              PCat(L,"        ");PCatDec(L,(unsigned long)(ts     - pTx));
              PCat(L,"   | ");   PCatDec(L,(unsigned long)qf);
              Out(L); }
            pHard=hard; pSec=sec; pConv=pumped; pCons=cons; pTx=ts; pDrop=qf;
            if((s % 10) == 9) ApDrawLog(win); }
          Say("");
          Say("    ⇒ A column of zeros after the first few seconds means the link came up and");
          Say("      then stopped. Steady numbers mean it did not. That distinction is the whole");
          Say("      reason this run costs a reboot.");
          (void)pSec; (void)pDrop; }

        /* 2. Resolve the shim's install/remove entry points. */
        e = GetSharedLibrary("\pEnetShimLib",kPowerPCCFragArch,kLoadCFrag,&shimCid,&fragAddr,errMsg);
        if(e == noErr){
          sym = 0;
          if(FindSymbol(shimCid,"\pEnetShimInstallDriver",&sym,&cls)==noErr
             && cls==kTVectorCFragSymbol) installProc = (ShimInstallProc)sym;
          sym = 0;
          if(FindSymbol(shimCid,"\pEnetShimRemoveDriver",&sym,&cls)==noErr
             && cls==kTVectorCFragSymbol) removeProc = (ShimRemoveProc)sym;
          sym = 0;
          if(FindSymbol(shimCid,"\pEnetShimAsyncStatus",&sym,&cls)==noErr
             && cls==kTVectorCFragSymbol) asyncProc = (ShimAsyncProc)sym; }
        SayOk("  the shim's install and remove entry points resolved",
              (installProc != 0) && (removeProc != 0));

        if(haveEntry && installProc && removeProc){
          EnetShimInterface ib;
          /* A Pascal string built by hand: Str31 is unsigned char[32] and gcc will not initialise
           * it from a "\p" literal. The length byte is explicit rather than counted by the
           * compiler, so it is stated once here and cannot drift from the text. */
          /* ⚠ Kept identical to ap_boot.c's. The app no longer registers (the boot
           * vehicle owns the port), but a divergence here would surface as two
           * differently-named ports the day anything makes the app register again. */
          static unsigned char drvName[17] =
                  { 15,'A','i','r','P','o','r','t',' ','E','x','t','r','e','m','e',0 };
          static UInt32 theIdStorage = 0;
          OSErr ie;
          /* ⛔ 8-10: NOT IN WITNESS MODE. Registering a second driver for a port TCP/IP is
           * already using, and then opening it, is the one thing this run must not do. The
           * skip is announced rather than silent -- an app that quietly took another path is
           * how a log gets misread. */
          if(witnessMode){
            Say("");
            Say("  [8-10] NOT REGISTERING and NOT OPENING -- another client owns this port.");
            Say("    Everything above came from CFM calls into the driver, which need the");
            Say("    fragment and not the endpoint. Nothing below this line runs.");
          } else {
          /* 3. Register. The struct goes BY VALUE and carries a connection to OUR fragment --
           *    Apple's sample comments this argument "CFrag Connection ID (mine)". */
          ib.DRVName = (StringPtr)drvName;
          ib.ConnID  = drvCid;
          ib.RefCon  = 0;
          ib.theID   = (UInt32)&theIdStorage;
          /* ★★★ 8-5d: ENUMERATE OT'S PORT LIST, BEFORE AND AFTER.
           *
           * k55 registered cleanly, held the port for two full minutes, and the tester reported
           * that NOTHING new appeared in the TCP/IP "Connect via" pop-up. So `noErr` from
           * EnetShimInstallDriver does not mean a user-visible port exists, and waiting longer
           * cannot distinguish the possibilities. The client side can be asked directly.
           *
           * OTGetIndexedPort walks every port OT knows about. Listing them BEFORE registering
           * gives the control this experiment has been missing: the machine's real Ethernet port
           * is right there, and its flags are what a working, visible, selectable port looks
           * like. Ours can then be diffed against it instead of guessed at.
           *
           * ⚠ THE FLAGS ARE THE POINT, not the presence. kOTPortIsPrivate (0x8000) and
           *   kOTPortIsUnavailable (0x0004) would each produce exactly what was observed: a port
           *   that exists, registered without error, and never offered to the user. */
          /* ★★★ 8-2b: ARM THE DRIVER, BECAUSE THE NEXT THING IT DOES IS A CORE RESET.
                     *
                     * This is the only moment in the whole run when the application can honestly say
                     * the card is idle: ApBringUp has not run yet and nothing has been mapped by us.
                     * Arming here and disarming the moment the shim work is done keeps the window as
                     * narrow as it can be, and makes the safety a STATEMENT BY THE OWNER rather than
                     * an inference from line numbers.
                     *
                     * ⚠ If this call is ever moved after ApBringUp, the core reset lands in the
                     *   middle of a live radio. The arm is what makes that a deliberate act rather
                     *   than an accident. */
                    if(armProc){
                      (void)(*armProc)(1);
                      Say("  [8-2b] driver ARMED -- core reset permitted (card is idle right now)"); }
          ApListOtPorts("  BEFORE registering:");

          ie = (*installProc)(ib,&shimRef);
          shim85bInstalled = (ie == noErr);
          Say1("  EnetShimInstallDriver -> ",(unsigned long)ie);
          SayOk("★★★ THE SHIM ACCEPTED OUR REGISTRATION",shim85bInstalled);
          if(shim85bInstalled){
            ApListOtPorts("  AFTER registering:");

            /* ★★★ 8-5e: REPORT LINK STATE, AND SEE IF THAT IS WHAT ACTIVATES THE PORT.
             *
             * k56 answered the visibility question precisely. Our port record IS created -- five
             * ports became six -- and it is NOT hidden: kOTPortIsPrivate is clear. What it is, is
             * INACTIVE. Set against the machine's own Ethernet port in the same listing:
             *
             *     enet0  gmac     portFlags=00000001  ACTIVE
             *     enet1  USBEnet  portFlags=00000000  -- ours
             *
             * A port that has never reported a link is not a port OT will drive. And the shim has
             * an entry point for exactly that, which this project resolved in k54 and then never
             * called: EnetShimAsyncStatus(ref, EnetShim_Link, state, 0), used that way in
             * USBEnetDriver.c:629.
             *
             * ⚠ TWO CHANGES, BUT EACH IS BRACKETED BY A LISTING, so attribution survives. Link is
             *   reported and the ports are re-read; only then is Speed reported and the ports are
             *   read again. If the ACTIVE bit appears after one and not the other, the listing
             *   says which -- doing both blind and re-reading once would not. */
            if(asyncProc){
              OSErr ae;
              Say("");
              Say("  --- reporting LINK UP via EnetShimAsyncStatus ---");
              ae = (*asyncProc)(shimRef,EnetShim_Link,1,0);
              Say1("  EnetShimAsyncStatus(Link,1) -> ",(unsigned long)ae);
              ApListOtPorts("  AFTER reporting link up:");

              Say("");
              Say("  --- reporting SPEED (11 Mbit, in bits/sec) ---");
              ae = (*asyncProc)(shimRef,EnetShim_Speed,11000000UL,11000000UL);
              Say1("  EnetShimAsyncStatus(Speed) -> ",(unsigned long)ae);
              ApListOtPorts("  AFTER reporting speed:");

              /* ★★★★★ 8-5f: STOP WAITING FOR SOMETHING TO OPEN THE PORT. OPEN IT OURSELVES.
               *
               * k57 reported link and speed, both returned noErr, and the flags did not move by a
               * single bit across three listings. So ACTIVE is not something a driver asserts.
               *
               * ⚠ AND k57 HAD A CONFOUND THAT WAS MY DOING. Selector calls were zero again, but
               *   the tester had -- on my own tidy-up advice -- switched TCP/IP back to the real
               *   Ethernet configuration. So nothing in the system had any reason to open our
               *   port, and "zero calls" measured my instructions rather than the driver. That is
               *   the second time this increment has produced a number that says more about the
               *   test than the subject.
               *
               * The fix is to remove the human from the loop entirely. OTOpenEndpoint on a
               * configuration naming the port makes OT plumb the stream itself -- which is
               * precisely the event that drives EnetHAL_Entry. No control panel, no ambiguity
               * about which configuration was active, and it runs the same way every time.
               *
               * ⚠ The port is named by the SHIM, not by us: k56 showed the record is `enet1`,
               *   module `USBEnet`. Our DRVName is only the display string. So the configuration
               *   is built from the name in the port record, read back at run time rather than
               *   hardcoded -- if the shim numbers it enet2 on a machine with another adapter,
               *   a literal "enet1" would quietly open the wrong thing. */
              Say("");
              Say("  --- ★ OPENING THE PORT OURSELVES (no control panel needed) ---");
              { OTPortRecord mine;
                OTItemCount  ix = 0;
                int found = 0;
                while(OTGetIndexedPort(&mine,ix)){
                  /* Match on the module the shim registers under, taking the LAST such port:
                   * ours is the one that appeared during this run. Matching on the module rather
                   * than on a literal "enet1" matters -- k56 showed the shim, not us, chooses the
                   * name, so a second adapter would shift it. */
                  if(mine.fModuleName[0] &&
                     mine.fModuleName[0]=='U' && mine.fModuleName[1]=='S' &&
                     mine.fModuleName[2]=='B'){ found = 1; shimPortIdx = ix; }
                  ix++; }
                if(!found){
                  Say("  ⚠ could not find our port in the registry to open it.");
                } else {
                  OTPortRecord pr;
                  if(OTGetIndexedPort(&pr,shimPortIdx)){
                    OSStatus oerr = kOTNoError;
                    OTConfigurationRef cfg;
                    Str255 L; L[0]=0;
                    PCat(L,"  opening port \""); PCat(L,(char*)pr.fPortName); PCat(L,"\"");
                    Out(L);
                    cfg = OTCreateConfiguration((char*)pr.fPortName);
                    if(cfg == kOTInvalidConfigurationPtr || cfg == kOTNoMemoryConfigurationPtr){
                      Say("  ⚠ OTCreateConfiguration refused the port name. That is itself a");
                      Say("    finding: OT does not consider this a configurable endpoint.");
                    } else {
                      EndpointRef ep = OTOpenEndpoint(cfg,0,NULL,&oerr);
                      Say1("  OTOpenEndpoint -> err = ",(unsigned long)oerr);
                      shim85fOpened = (oerr == kOTNoError) && (ep != kOTInvalidEndpointRef);
                      SayOk("★★★ THE STREAM OPENED -- OT plumbed our driver",shim85fOpened);
                      if(shim85fOpened){
                        ApListOtPorts("  AFTER opening the endpoint:");

                        /* ★★★ 8-5h: BIND THE ENDPOINT. OPENING ONE IS NOT USING ONE.
                         *
                         * k59 answered GetMACAddress from the card and the selector sequence did
                         * not change by a single call: same sixteen, same order, Stop x11, and
                         * Start still absent. So the Stop storm was never a reaction to our
                         * refusal -- it is just what this open/close cycle looks like.
                         *
                         * The readme says Start arrives "when network communications are to be
                         * established". Opening an endpoint and closing it immediately never
                         * establishes any: OT plumbs the stream, asks the driver to describe
                         * itself, and tears it down again. A BIND is the thing that puts a DLPI
                         * provider into service, and it is what should make the shim call Start
                         * -- the selector that hands us the receive ISR.
                         *
                         * ⚠ OTBind(ref, NULL, NULL) is legal: both address arguments are
                         *   documented nullable (OpenTransport.h:2920). Binding with no address
                         *   is the smallest possible "put this into service", which is exactly
                         *   the variable being tested -- a SAP-specific bind can come later if
                         *   this proves the mechanism. */
                        /* ★★★★★★ 8-6c: BIND WITH A REAL 802.2 ADDRESS -- THE LADDER.
                         *
                         * ⚠⚠ THE COMMENT ABOVE PLANNED THIS AT 8-5h AND IT WAS NEVER DONE. The
                         *   null bind returned 4294964146 in k80, k81, k82, k83 and k84, nobody
                         *   read the number, and the follow-up it names -- "a SAP-specific bind
                         *   with a real OTAddress" -- sat unwritten for five runs. It decodes to
                         *   -3150, kOTBadAddressErr: the provider is telling us the ADDRESS is
                         *   wrong, which is not a thing a NULL argument can be unless NULL is not
                         *   actually acceptable here.
                         *
                         *   And it is not. OTBind's own documentation says both address arguments
                         *   are nullable; that is the GENERIC contract. This endpoint is an 802.2
                         *   provider, and OpenTransportProviders.h:1649 defines what it wants:
                         *
                         *       struct T8022Address { OTAddressType fAddrFamily;   // AF_8022
                         *                             UInt8  fHWAddr[6];
                         *                             UInt16 fSAP;
                         *                             UInt8  fSNAP[5]; };
                         *
                         *   Reading the prose about the flag instead of the flag itself is a
                         *   named failure mode on this project. The header is the flag.
                         *
                         * ★ A LADDER, NOT A GUESS. Four attempts in one run, each reported with
                         *   its decoded error, because a reboot costs more than three extra
                         *   OTBind calls. The FIRST rung is the null bind that has always failed:
                         *   a control that reproduces the known failure proves the ladder is
                         *   really running and that nothing else about this build moved. If rung
                         *   1 suddenly succeeds, every conclusion below is suspect. */
                        { OSStatus be = -1;
                          int rung;
                          UInt8 myMac[6];
                          T8022Address bindAddr, retAddr;
                          TBind req, ret;
                          for(rung=0;rung<6;rung++) myMac[rung] = 0;
                          if(getMacProc){ SInt32 me = 0; (void)(*getMacProc)(myMac,&me); }
                          Say("");
                          Say("  --- ★★★★★★ 8-6c: BINDING THE ENDPOINT (a ladder of 4) ---");
                          /* ⚠ MAKE THE MODE EXPLICIT RATHER THAN INHERIT AN ASSUMPTION.
                           * OpenTransport.h:332 says to use OTSetSynchronous to set a provider's
                           * mode and never states the default. In ASYNCHRONOUS mode a successful
                           * OTBind does not return noErr -- it completes later through a notifier
                           * we have not installed -- so every rung below would read as a failure
                           * and the ladder would report the opposite of the truth. One call
                           * removes the unknown, and its own result is reported rather than
                           * discarded. */
                          { OSStatus se = OTSetSynchronous(ep);
                            SayOtErr("    OTSetSynchronous -> ",se); }
                          for(rung=0; rung<4 && be != kOTNoError; rung++){
                            const char *what;
                            int i;
                            for(i=0;i<6;i++)  bindAddr.fHWAddr[i] = 0;
                            for(i=0;i<5;i++)  bindAddr.fSNAP[i]   = 0;
                            bindAddr.fAddrFamily = AF_8022;
                            bindAddr.fSAP        = 0x0800;
                            req.addr.buf = (UInt8*)&bindAddr;
                            req.addr.len = k8022BasicAddressLength;
                            req.addr.maxlen = sizeof(bindAddr);
                            req.qlen = 0;
                            ret.addr.buf = (UInt8*)&retAddr;
                            ret.addr.len = 0;
                            ret.addr.maxlen = sizeof(retAddr);
                            ret.qlen = 0;
                            switch(rung){
                              case 0:
                                what = "rung 1: OTBind(ep,NULL,NULL) -- THE CONTROL, expected to fail";
                                Say(""); Say(what);
                                be = OTBind(ep,NULL,NULL);
                                break;
                              case 1:
                                what = "rung 2: T8022Address, AF_8022, SAP 0x0800 (DIX/IP), hw zeros";
                                Say(""); Say(what);
                                be = OTBind(ep,&req,&ret);
                                break;
                              case 2:
                                what = "rung 3: same, but hw = this card's own MAC";
                                Say(""); Say(what);
                                for(i=0;i<6;i++) bindAddr.fHWAddr[i] = myMac[i];
                                be = OTBind(ep,&req,&ret);
                                break;
                              default:
                                what = "rung 4: SAP 0x00AA (kSNAPSAP) with a SNAP address";
                                Say(""); Say(what);
                                bindAddr.fSAP = kSNAPSAP;
                                bindAddr.fSNAP[0]=0x00; bindAddr.fSNAP[1]=0x00;
                                bindAddr.fSNAP[2]=0x00; bindAddr.fSNAP[3]=0x08;
                                bindAddr.fSNAP[4]=0x00;
                                req.addr.len = k8022SNAPAddressLength;
                                be = OTBind(ep,&req,&ret);
                                break; }
                            SayOtErr("    -> ",be); }
                          shim85hBound = (be == kOTNoError);
                          SayOk("the endpoint BOUND -- the provider is in service",shim85hBound);
                          if(shim85hBound){
                            Say1("    ★ bound on rung ",(unsigned long)rung);
                            Say ("      ⇒ If Start now arrives BEFORE the close rather than during");
                            Say ("        it, the bind was the gate and 8-5h's guess was right.");
                          } else {
                            Say("    ⚠ ALL FOUR RUNGS FAILED. The decoded errors above are the");
                            Say("      result: they say whether the address FAMILY, the SAP or the");
                            Say("      LENGTH is being rejected, which are three different fixes.");
                            Say("      ⇒ If rung 1 failed the same way it has since k80, the ladder");
                            Say("        ran and this is a real answer, not a broken harness."); }
                          if(shim85hBound){
                            /* ★★★★★★ 8-58 (k155): SAP-0x0800 RECEIVE ISOLATION -- the decisive split.
                             * We are bound to SAP 0x0800 (IP) -- the same SAP OT's TCP/IP uses. If a
                             * UNICAST IP frame the driver hands to the shim reaches THIS bound stream,
                             * EnetShimLib DOES deliver unicast IP and the wall is OT's own TCP/IP
                             * stream; if it does not WHILE the driver's delivered count climbs, the
                             * shim itself drops unicast IP before any bound stream. why[OK] is the
                             * driver->shim half and sits OUTSIDE this shim->us test -- the validity
                             * witness. The Pi sends via a STATIC ARP so no G4 ARP is needed. */
                            UInt8  addrbuf[32], databuf[1600], sampleBuf[4][40];
                            UInt32 whyA[AP_ENET_NREASON], whyB[AP_ENET_NREASON];
                            UInt32 rbA=0, rbB=0, consA=0, consB=0, dmy=0;
                            UInt32 rcvTotal=0, rcvUniIcmp=0, rcvOtherIp=0, sampleN=0, sampleLen[4];
                            long   dt0; int di;
                            for(di=0;di<AP_ENET_NREASON;di++){ whyA[di]=0; whyB[di]=0; }
                            for(di=0;di<4;di++) sampleLen[di]=0;
                            if(getDlpiProc)(void)(*getDlpiProc)(&dmy,&dmy,&consA,&dmy,&rbA,&dmy,&dmy,whyA,&dmy);
                            Say("");
                            Say("  ============ 8-58 (k155): SAP-0x0800 RECEIVE ISOLATION ============");
                            Say("  ★ Endpoint BOUND to SAP 0x0800 (IP). With OT on BUILT-IN ETHERNET,");
                            Say("    on the Pi:  sudo arp -s 192.168.1.225 02:00:00:00:00:01");
                            Say("    then:       ping -c 100 -i 0.2 192.168.1.225");
                            Say("  ★★★ PING NOW -- dwelling ~45 s, receiving on the bound stream.");
                            dt0 = TickCount();
                            while((TickCount()-dt0) < 2700L){
                              TUnitData ud; OTFlags fl; OSStatus re; EventRecord e2;
                              ud.addr.buf=addrbuf; ud.addr.maxlen=sizeof(addrbuf); ud.addr.len=0;
                              ud.opt.buf=0; ud.opt.maxlen=0; ud.opt.len=0;
                              ud.udata.buf=databuf; ud.udata.maxlen=sizeof(databuf); ud.udata.len=0;
                              re = OTRcvUData(ep,&ud,&fl);
                              if(re==kOTNoError && ud.udata.len>0){
                                UInt32 n = (UInt32)ud.udata.len;
                                rcvTotal++;
                                /* On a DIX 0x0800 bind the SDU is the IP datagram: [0]=0x4x, [9]=proto,
                                 * [16..19]=dst. Unicast ICMP to .225 == proto 1, dst 0xC0A801E1. */
                                if(n>=20u && (databuf[0]&0xF0u)==0x40u){
                                  UInt32 dst=((UInt32)databuf[16]<<24)|((UInt32)databuf[17]<<16)
                                            |((UInt32)databuf[18]<<8)|databuf[19];
                                  if(databuf[9]==1u && dst==0xC0A801E1UL) rcvUniIcmp++;
                                  else rcvOtherIp++; }
                                if(sampleN<4u){ UInt32 c,lim=(n<40u)?n:40u;
                                  for(c=0;c<lim;c++) sampleBuf[sampleN][c]=databuf[c];
                                  sampleLen[sampleN]=lim; sampleN++; }
                              } else if(re==kOTLookErr){
                                OTResult lk = OTLook(ep);
                                if(lk==T_UDERR) (void)OTRcvUDErr(ep,(TUDErr*)0); }
                              if(WaitNextEvent(everyEvent,&e2,1,NULL)){
                                if(e2.what==updateEvt && win && (WindowPtr)e2.message==win){
                                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } }
                              ApDrawLog(win); }
                            if(getDlpiProc)(void)(*getDlpiProc)(&dmy,&dmy,&consB,&dmy,&rbB,&dmy,&dmy,whyB,&dmy);
                            Say("");
                            Say("  --- 8-58 RESULT (send me these numbers) ---");
                            Say1("    driver->shim delivered during dwell (why[OK] delta) = ",
                                 (unsigned long)(whyB[AP_ENET_OK]-whyA[AP_ENET_OK]));
                            Say1("    driver->shim readBytes delta                        = ",
                                 (unsigned long)(rbB-rbA));
                            Say1("    frames this bound 0x0800 stream RECEIVED (total)    = ",(unsigned long)rcvTotal);
                            Say1("      ...of which UNICAST ICMP to .225                  = ",(unsigned long)rcvUniIcmp);
                            Say1("      ...other IP                                       = ",(unsigned long)rcvOtherIp);
                            { UInt32 si; for(si=0;si<sampleN;si++){
                                Say1("    received SDU sample # (<=40 bytes) ",(unsigned long)(si+1));
                                HexDump(sampleBuf[si],(int)sampleLen[si],0); } }
                            Say("");
                            if(rcvUniIcmp>0)
                              Say("  ⇒ ★ BOUND STREAM GOT THE UNICAST PING -> shim delivers unicast IP; wall is OT's TCP/IP stream.");
                            else if((whyB[AP_ENET_OK]-whyA[AP_ENET_OK])>=40u)
                              Say("  ⇒ ⛔ driver delivered many frames but bound stream got NO unicast ping -> shim drops unicast IP.");
                            else
                              Say("  ⇒ ⚠ INVALID: delivered count barely moved -- ping never reached us (check OT-on-Ethernet / arp -s / ping running).");
                            Say("  ==================================================================");
                            ApListOtPorts("  AFTER binding:");
                            (void)OTUnbind(ep);
                            Say("  unbound."); } }

                        /* ⚠⚠ POLL BEFORE THE ENDPOINT CLOSES, AND THAT ORDERING IS THE
                         * WHOLE LESSON OF k73. OTCloseProvider drives EnetHAL_Close, Close
                         * calls AirPortShimQuiesce, and quiesce is what stops the receiver.
                         * k73 polled AFTER this line and found MAC enabled = 0 with a
                         * receiver state of "never attempted" -- while the driver's own
                         * narration, replayed later in the same log, showed it had enabled
                         * the MAC perfectly. The receiver exists only between Open and
                         * Close; asking about it outside that window asks about a card
                         * that has already been handed back. */

                      /* ★★★★★ 8-2g: DID THE DRIVER HEAR ANYTHING? */
                      if(rxPollProc && getRxProc){
                        UInt32 t0,waited=0; UInt32 prev=0;
                        shimHaveRx = 1;
                        /* ★★★★★★ 8-9: THE DRIVER JOINED BY ITSELF, OR IT DID NOT.
                         * Reported FIRST, because every block below it now describes a link
                         * this application did not ask for. If this says 0, read nothing
                         * below as a driver failure -- the join is the precondition. */
                        if(getJoinProc){
                          UInt32 jt=0,jb=0,jj=0,ja=0;
                          (void)(*getJoinProc)(&jt,&jb,&jj,&ja);
                          irqArmedEarly = (ja == 2);
                          Say("");
                          Say("  --- ★★★★★★ 8-9: DID THE DRIVER JOIN ON ITS OWN? ---");
                          Say1("    join attempts made by the DRIVER = ",(unsigned long)jt);
                          Say1("    ms spent waiting for a beacon    = ",(unsigned long)jb);
                          Say1("    interrupt state at this point    = ",(unsigned long)ja);
                          Say ("      (0 none / 1 installed / 2 ARMED -- 2 means Start armed it");
                          Say ("       itself, with no request from this application)");
                          SayOk("  ★★★★★★ THE DRIVER JOINED THE NETWORK WITHOUT AN APP",jj!=0);
                          if(jj && jt>1)
                            Say1("    ⚠ it took more than one attempt: ",(unsigned long)jt);
                          if(!jj)
                            Say ("    ⇒ Read the driver's [8-9] lines in the narration below;"
                                 " they say which step failed on each attempt."); }

                        Say("");
                        Say("  --- ★★★ 8-2g: the driver's RECEIVER ---");
                        (void)(*getRxProc)(&shimRxChan,&shimRxFilter,&shimRxMacOn,
                                           &shimRxEverOn,&shimRxErr);
                        Say1("    channel        = ",(unsigned long)shimRxChan);
                        SayH("    MACCTL filter  = ",(unsigned long)shimRxFilter,8);
                        Say1("    MAC enabled NOW  = ",(unsigned long)shimRxMacOn);
                        Say1("    MAC EVER enabled = ",(unsigned long)shimRxEverOn);
                        Say1("    receiver state = ",(unsigned long)shimRxErr);
                        /* ⚠ A DWELL, NOT A SINGLE LOOK. Beacons arrive about every 100 ms, so
                         * one poll immediately after Open would usually see nothing and would
                         * say nothing about whether the receiver works. Three seconds is ~30
                         * beacon intervals per AP in range. */
                        Say("    dwelling 3 s and polling the driver...");
                        t0 = TickCount();
                        while((TickCount()-t0) < 180UL){
                          shimRxFrames = (*rxPollProc)(&shimRxFirst,&shimRxStatus,&shimRxIndex);
                          if(shimRxFrames != prev){
                            Say1("      slots filled so far = ",(unsigned long)shimRxFrames);
                            prev = shimRxFrames; }
                          ApDrawLog(win); }
                        (void)waited;
                        Say1("    slots whose poison is GONE = ",(unsigned long)shimRxFrames);
                        Say1("    first such slot            = ",(unsigned long)shimRxFirst);
                        SayH("    RXSTATUS                   = ",(unsigned long)shimRxStatus,8);
                        SayH("    RXINDEX                    = ",(unsigned long)shimRxIndex,8);
                        /* ⚠⚠ THIS ORACLE CANNOT SEE THE RING ANY MORE, AND THAT IS THE DRIVER WORKING.
                         * It counts slots whose poison is GONE -- frames sitting unread in the ring. Since 8-9d
                         * the driver arms its own interrupt in Open and the secondary handler DRAINS the ring
                         * continuously, so by the time the app looks there is nothing left to find and this
                         * reports 0 on a perfectly healthy receiver. k74 produced three false alarms of exactly
                         * this shape and the rule from it was: when an increment changes what the app can see,
                         * TELL THE ORACLES. Scored on the driver's own counters instead when the IRQ is armed. */
                        if(irqArmedEarly)
                          Say("  [--] NOT COMPARABLE: the interrupt drains the ring before the\n                              app can look. Read 'frames delivered BY INTERRUPT' below.");
                        else
                          SayOk("  ★★★ THE DRIVER RECEIVED AT LEAST ONE FRAME",shimRxFrames > 0);

                        /* ⚠ COUNTING IS NOT DECODING. A slot whose poison is gone proves the
                         * card WROTE there; it does not prove what it wrote is a frame. So the
                         * first one is copied out and parsed, and the oracle wants a plausible
                         * PLCP and an 802.11 header -- content, not a counter. */
                        if(shimRxFrames > 0 && shimRxFirst >= 0 && getRxBufProc){
                          if((*getRxBufProc)((UInt32)shimRxFirst,shimRxBuf,K3_PAGE)){
                            UInt8 ch=0,jssi=0; UInt16 fc=0,flen=0;
                            shimRxDecoded = ParseRxBuffer(shimRxBuf,B43_DMA0_RX_FW351_FO,
                                                          shimRxBssid,shimRxSsid,&shimRxSsidLen,
                                                          &ch,&fc,&flen,&jssi);
                            Say1("    frame_len (incl FCS) = ",(unsigned long)flen);
                            SayH("    frame control        = ",(unsigned long)fc,4);
                            Say("");
                            Say("    --- the first frame the DRIVER received, decoded ---");
                            HexDump(shimRxBuf,64,B43_DMA0_RX_FW351_FO);
                            SayOk("  it parses as an 802.11 frame",shimRxDecoded); } } }

                        /* ★★★ 8-3a: and did the driver TRANSMIT? */
                        if(getTxProc){
                          shimHaveTx = 1;
                          Say("");
                          Say("  --- ★★★ 8-3a: the driver's TRANSMITTER ---");
                          shimTxAcked = 0;
                          /* ⚠ ASK THE DRIVER TO TRANSMIT NOW, AFTER THE DWELL. k76 had this
                           * inside EnetHAL_Open, microseconds after the MAC came up, when every
                           * RX buffer was still poisoned and there was no BSSID to aim at. The
                           * driver declined correctly; the question had simply been asked before
                           * the answer could exist. The app owns the timing, the driver owns the
                           * work -- the same split as AirPortShimRxPoll. */
                          if(txProbeProc){
                            Say1("    asking the driver to transmit -> ",
                                 (unsigned long)(*txProbeProc)()); }
                          (void)(*getTxProc)(&shimTxRing,&shimTxReadback,&shimTxAcked,
                                             &shimTxStats,&shimTxErr);
                          if(getBssidProc && (*getBssidProc)(shimTxBssid)){
                            Str255 L; L[0]=0; PCat(L,"    aimed at BSSID = ");
                            PCatMac(L,shimTxBssid); Out(L); }
                          SayH("    TX ring phys      = ",(unsigned long)shimTxRing,8);
                          SayH("    TXRING reads back = ",(unsigned long)shimTxReadback,8);
                          Say1("    TX status reports = ",(unsigned long)shimTxStats);
                          Say1("    ACKED             = ",(unsigned long)shimTxAcked);
                          SayOk("  ★★★ AN ACCESS POINT ACKNOWLEDGED THE DRIVER'S FRAME",
                                shimTxAcked != 0); }

                        /* ★★★★★ 8-3b: and will it AUTHENTICATE us? */
                        if(authProc && getAuthProc){
                          shimHaveAuth = 1;
                          Say("");
                          Say("  --- ★★★★★ 8-3b: AUTHENTICATION ---");
                          /* ⚠⚠ THE PMK BEFORE AUTHENTICATING, NOT BETWEEN AUTH AND ASSOC.
                           *
                           * k82 put it between them and the association FAILED: the request was
                           * ACKed, exactly as in k81, and the AP never answered. The only
                           * difference between the two runs was 333 ms of PBKDF2 sitting in that
                           * window. Whether the AP aged out the authentication or the 32-slot
                           * receive ring filled while unattended, the cure is the same -- do not
                           * put a third of a second between two halves of one exchange.
                           *
                           * It is also simply more correct: a real supplicant knows the PSK at
                           * configuration time, long before it ever authenticates. Message 1
                           * still has its PMK ready, which was the original requirement. */
                          if(prepPmkProc)
                            Say1("    preparing the PMK FIRST (before auth) -> ",
                                 (unsigned long)(*prepPmkProc)());
                          Say1("    asking the driver to authenticate -> ",
                               (unsigned long)(*authProc)());
                          (void)(*getAuthProc)(&shimAuthAlg,&shimAuthSeq,&shimAuthStatus,
                                               &shimAuthMs,&shimAuthAcked,&shimAuthGot,
                                               &shimAuthErr);
                          Say1("    auth request ACKed = ",(unsigned long)shimAuthAcked);
                          Say1("    a frame came back  = ",(unsigned long)shimAuthGot);
                          Say1("    response after ms  = ",(unsigned long)shimAuthMs);
                          SayH("    algorithm = ",(unsigned long)shimAuthAlg,4);
                          SayH("    sequence  = ",(unsigned long)shimAuthSeq,4);
                          SayH("    status    = ",(unsigned long)shimAuthStatus,4);
                          SayOk("  ★★★★★ THE ACCESS POINT AUTHENTICATED THIS DRIVER",
                                shimAuthErr == 0); }

                        /* ★★★★★★ 8-3c: and will it ASSOCIATE us? */
                        if(assocProc && getAssocProc){
                          shimHaveAssoc = 1;
                          Say("");
                          Say("  --- ★★★★★★ 8-3c: ASSOCIATION ---");
                          Say1("    asking the driver to associate -> ",
                               (unsigned long)(*assocProc)());
                          (void)(*getAssocProc)(&shimAssocCap,&shimAssocStatus,&shimAssocAid,
                                                &shimAssocMs,&shimAssocAcked,&shimAssocGot,
                                                &shimAssocErr);
                          Say1("    assoc request ACKed = ",(unsigned long)shimAssocAcked);
                          Say1("    a frame came back   = ",(unsigned long)shimAssocGot);
                          Say1("    response after ms   = ",(unsigned long)shimAssocMs);
                          SayH("    capability = ",(unsigned long)shimAssocCap,4);
                          SayH("    status     = ",(unsigned long)shimAssocStatus,4);
                          Say1("    AID        = ",(unsigned long)shimAssocAid);
                          SayOk("  ★★★★★★ THE ACCESS POINT ASSOCIATED THIS DRIVER",
                                shimAssocErr == 0); }

                        /* ★★★★★★ 8-4: and the four-way handshake. */
                        if(hsProc && getHsProc){
                          shimHaveHs = 1;
                          Say("");
                          Say("  --- ★★★★★★ 8-4: THE WPA2 FOUR-WAY HANDSHAKE ---");
                          Say1("    asking the driver to handshake -> ",
                               (unsigned long)(*hsProc)());
                          (void)(*getHsProc)(&shimPmkMs,&shimPmkNz,&shimPtkNz,&shimHsM1,
                                             &shimHsM3,&shimHsM1Ms,&shimHsM3Ms,&shimHsErr);
                          Say1("    PBKDF2 took ms      = ",(unsigned long)shimPmkMs);
                          Say1("    PMK non-zero bytes  = ",(unsigned long)shimPmkNz);
                          Say1("    PTK non-zero bytes  = ",(unsigned long)shimPtkNz);
                          Say1("    message 1 arrived   = ",(unsigned long)shimHsM1);
                          Say1("      after ms          = ",(unsigned long)shimHsM1Ms);
                          Say1("    message 3 arrived   = ",(unsigned long)shimHsM3);
                          Say1("      after ms          = ",(unsigned long)shimHsM3Ms);
                          Say ("    ⚠ No key material is reported. The PMK is password-equivalent.");
                          SayOk("  ★★★★★★ THE FOUR-WAY HANDSHAKE COMPLETED",shimHsErr == 0); }

                        /* ★★★★★★ 8-6a: ACCEPT Start, AND DELIVER UP THE DLPI STACK.
                         *
                         * ⚠⚠ THE POSITION OF THIS BLOCK IS LOAD-BEARING. It must run BEFORE
                         *   OTCloseProvider below, because Close calls AirPortShimQuiesce() and
                         *   takes the MAC down. It must also run after the handshake, because
                         *   until then there is no PTK and every protected frame converts to
                         *   NO_KEY. This window -- endpoint open, associated, keys installed --
                         *   is the only place the contract can be exercised at all.
                         *
                         * ⚠⚠ THE PUMP MAKES THE FIRST CALL THIS PROJECT HAS EVER MADE INTO A
                         *   PROC THE SHIM HANDED US. Apple's sample calls it directly
                         *   (USBEnetDriver.c:1167) and that sample is PowerPC CFM, so a direct
                         *   call matches the prior art -- but if that ProcPtr is not what we
                         *   believe, this is where the machine lands in MacsBug. The warning
                         *   line below is written and FLUSHED before the call for exactly that
                         *   reason: if the log ends there, the isr call is the culprit and
                         *   nothing else needs ruling out. `log MacsBugCrash.log` first. */
                        if(getDlpiProc){
                          UInt32 pumped=0,isrCalls=0,consumed=0,readCalls=0,readBytes=0;
                          UInt32 readEmpty=0,walked=0,why[AP_ENET_NREASON],startSeen=0,haveIsr;
                          int w;
                          for(w=0;w<AP_ENET_NREASON;w++) why[w]=0;
                          haveIsr = (*getDlpiProc)(&pumped,&isrCalls,&consumed,&readCalls,
                                                   &readBytes,&readEmpty,&walked,why,&startSeen);
                          Say("");
                          Say("  --- ★★★★★★ 8-6a: Start, AND DELIVERY UP THE DLPI STACK ---");
                          Say(startSeen == 0 ? "    EnetHAL_Start: NEVER ARRIVED" :
                              startSeen == 1 ? "    EnetHAL_Start: arrived, then OT STOPPED us"
                                             : "    EnetHAL_Start: arrived, still STARTED");
                          Say1("    the shim gave us a notify proc = ",(unsigned long)haveIsr);
                          if(startSeen == 1)
                            Say("      (we pump anyway -- the contract is what is being tested)");
                          /* ★★★ 8-6b: WAIT FOR Start, AND YIELD WHILE WAITING.
                           *
                           * ⚠⚠ THIS IS WHY k85 REPORTED "NEVER ARRIVED" ON A RUN WHERE Start WAS
                           *   ACCEPTED. k85's selector trace ends `... SetMulticastFilters Start
                           *   Stop Stop Stop Close` -- and it has only ONE Start where k84 had
                           *   two, because k84's was refused and OT retried. So acceptance
                           *   worked; the app simply asked three seconds too early.
                           *
                           *   Mac OS 9 is cooperatively scheduled. Between OTOpenProvider and
                           *   this point the app runs straight-line code -- a 3 s dwell that
                           *   yields, then a handshake that spins on SsbSpinUs and does not.
                           *   OT advances its own sequence when it gets the processor, and it
                           *   had reached only as far as SetMulticastFilters. The app's own hold
                           *   loop carries this lesson in a comment already: a loop that does not
                           *   yield does not merely block this app, it stops everything else
                           *   from making progress -- including the thing being waited for.
                           *
                           * ⚠ Bounded, and the timeout is a RESULT rather than a hang: if Start
                           *   has still not arrived after this, that is a real finding about
                           *   OT's sequence and not an app that gave up too soon. */
                          if(!startSeen && pumpProc){
                            long w0 = TickCount();
                            Say("    waiting for Start (yielding, so OT can make progress)...");
                            ApDrawLog(win);
                            while(TickCount() - w0 < 30L*60L){
                              EventRecord wev;
                              if(WaitNextEvent(everyEvent,&wev,6,NULL)){
                                if(wev.what==updateEvt && win && (WindowPtr)wev.message==win){
                                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } }
                              haveIsr = (*getDlpiProc)(&pumped,&isrCalls,&consumed,&readCalls,
                                                       &readBytes,&readEmpty,&walked,why,
                                                       &startSeen);
                              if(startSeen) break; }
                            Say1("    waited for Start, seconds = ",
                                 (unsigned long)((TickCount()-w0)/60));
                            Say(startSeen == 0 ? "    [!!] Start STILL never arrived."
                                : startSeen == 1 ? "    ★ Start arrived (OT has since Stopped us)"
                                                 : "    ★ Start arrived and we are STARTED"); }

                          if(!startSeen){
                            Say("    [--] OT never called Start, so there was nothing to deliver");
                            Say("      INTO. ⇒ Read the selector sequence later in this log: this");
                            Say("      is a DIFFERENT failure from 'Start was refused', and it");
                            Say("      means the contract was never reached at all.");
                          } else if(!haveIsr){
                            Say("    ⚠ Start arrived but carried a NULL ioCompletion. The shim is");
                            Say("      not asking to be notified, so the push half of the contract");
                            Say("      does not apply and delivery must be driven another way.");
                            Say("      That is a real finding, not a failure of this build.");
                          } else if(!pumpProc){
                            Say("    [--] AirPortShimPump did not resolve -- stale shim.");
                          } else {
                            UInt32 got = 0; long p0 = TickCount(); int rounds = 0;
                            /* ★★★★★★ 8-6d: THE CONTROL GOES INSIDE THE RUN.
                             *
                             * ⚠⚠ k87 walked 144 frames, 105 of them beacons, and ZERO data
                             *   frames -- while unicast worked perfectly end to end. That
                             *   combination is an ADDRESS FILTER, and this project has a memo
                             *   about it that cost 43 probe increments the first time.
                             *
                             *   The driver now writes MACFILTER_BSSID after association. But
                             *   proving that is what changed things must NOT be done by
                             *   comparing k87 against k88 across a reboot: the receiver has a
                             *   ~30% intermittent, and a cross-run control is exactly what
                             *   failed when this was first diagnosed. So both windows run here,
                             *   in one run, on one association, minutes apart at most:
                             *
                             *     WINDOW A   filter forced to 00:00:00:00:00:00
                             *     WINDOW B   filter set to the real BSSID
                             *
                             *   Data in B and none in A is the filter, and nothing else can
                             *   look like that. None in either is "the air was quiet", which is
                             *   a real answer too -- it is just not a pass, and the log says so
                             *   rather than letting a silent network read as a fixed bug. */
                            if(setFilterProc){
                              int wnd;
                              for(wnd=0; wnd<2; wnd++){
                                UInt32 w0d=0,w1d=0,w2d=0,w3d=0,w4d=0,w5d=0;
                                UInt32 walkA=0, whyA[AP_ENET_NREASON], ss=0;
                                UInt32 walkB=0, whyB[AP_ENET_NREASON];
                                long   wt; int k;
                                for(k=0;k<AP_ENET_NREASON;k++){ whyA[k]=0; whyB[k]=0; }
                                Say("");
                                Say(wnd==0
                                    ? "    --- WINDOW A: MACFILTER_BSSID forced to 00:00:00:00:00:00 ---"
                                    : "    --- WINDOW B: MACFILTER_BSSID = the real BSSID ---");
                                Say1("      set filter -> ",
                                     (unsigned long)(*setFilterProc)(wnd ? 1UL : 0UL));
                                (void)(*getDlpiProc)(&w0d,&w1d,&w2d,&w3d,&w4d,&w5d,
                                                     &walkA,whyA,&ss);
                                wt = TickCount();
                                while(TickCount() - wt < 5L*60L){
                                  EventRecord wv;
                                  (void)(*pumpProc)(8);
                                  if(WaitNextEvent(everyEvent,&wv,6,NULL)){
                                    if(wv.what==updateEvt && win &&
                                       (WindowPtr)wv.message==win){
                                      BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } } }
                                (void)(*getDlpiProc)(&w0d,&w1d,&w2d,&w3d,&w4d,&w5d,
                                                     &walkB,whyB,&ss);
                                Say1("      frames walked in this window = ",
                                     (unsigned long)(walkB - walkA));
                                { Str255 L; int any=0;
                                  for(k=0;k<AP_ENET_NREASON;k++) if(whyB[k]>whyA[k]){
                                    L[0]=0; PCat(L,"        ");
                                    PCatDec(L,(unsigned long)(whyB[k]-whyA[k]));
                                    PCat(L," x "); PCat(L,(char*)ApEnetWhy(k)); Out(L); any=1; }
                                  if(!any) Say("        (nothing examined)"); }
                                Say1("      ★ DATA frames converted in this window = ",
                                     (unsigned long)(whyB[AP_ENET_OK]-whyA[AP_ENET_OK])); }
                              Say("");
                              Say("    ⇒ Compare the two windows above. Data in B and none in A");
                              Say("      is the BSSID filter and nothing else. None in either");
                              Say("      means the air was quiet, which is NOT a pass."); }
                            Say("    ⚠ about to call the shim's notify proc for the FIRST time.");
                            Say("      If this log ends on this line, THAT CALL is the crash.");
                            Say("      ⇒ log MacsBugCrash.log before anything else.");
                            ApDrawLog(win);
                            /* ★ PUMP FOR A WHILE, YIELDING BETWEEN ROUNDS. One call would sample
                             * whatever happened to be in the ring at one instant; at the ~6-11
                             * frames/sec this channel actually carries (measured two ways in
                             * k85: backlog 2 after a 316 ms gap, and 32 slots in a 3 s dwell)
                             * that is a coin flip. Yielding also lets OT keep running, which is
                             * the whole lesson of the wait above. */
                            while(TickCount() - p0 < 10L*60L){
                              EventRecord pev;
                              got += (*pumpProc)(8);
                              rounds++;
                              if(WaitNextEvent(everyEvent,&pev,6,NULL)){
                                if(pev.what==updateEvt && win && (WindowPtr)pev.message==win){
                                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } }
                              if(got >= 4) break; }
                            Say1("    pump rounds                  = ",(unsigned long)rounds);
                            Say1("    frames delivered             = ",(unsigned long)got);
                            (void)(*getDlpiProc)(&pumped,&isrCalls,&consumed,&readCalls,
                                                 &readBytes,&readEmpty,&walked,why,&startSeen);
                            Say1("    frames converted to Ethernet = ",(unsigned long)pumped);
                            Say1("    isr calls made               = ",(unsigned long)isrCalls);
                            Say1("    ★ frames the shim COLLECTED  = ",(unsigned long)consumed);
                            Say1("    EnetHAL_Read calls           = ",(unsigned long)readCalls);
                            Say1("      bytes handed over          = ",(unsigned long)readBytes);
                            Say1("      empty Reads                = ",(unsigned long)readEmpty);
                            Say1("    ring frames walked           = ",(unsigned long)walked);
                            Say("    why frames were NOT delivered:");
                            { Str255 L; int any=0;
                              for(w=1;w<AP_ENET_NREASON;w++) if(why[w]){
                                L[0]=0; PCat(L,"      "); PCatDec(L,(unsigned long)why[w]);
                                PCat(L," x "); PCat(L,(char*)ApEnetWhy(w)); Out(L); any=1; }
                              if(!any) Say("      (none)"); }
                            /* ★ THE ORACLE, AND IT IS NOT SELF-CONSISTENT. `consumed` counts the
                             * times the shim came back DOWN through EnetHAL_Read while our isr
                             * call was still on the stack. We do not control the shim: it either
                             * believed us and collected the frame or it did not. Same class of
                             * evidence as the AP's own status codes. */
                            SayOk("  ★★★★★★ THE SHIM COLLECTED A FRAME FROM THIS DRIVER",
                                  consumed > 0);
                            if(pumped && !consumed){
                              Say("    ⚠ We converted a frame and called the isr, and the shim did");
                              Say("      NOT come back for it. Push works, pull does not: suspect");
                              Say("      the notify proc's calling convention, or that the shim");
                              Say("      wants something done before it will Read."); }
                            if(!pumped && walked){
                              Say("    ⚠ Frames WERE in the ring and none converted. The histogram");
                              Say("      above names which rule rejected them, which is its job."); }
                            if(!walked){
                              Say("    ⚠ The ring was EMPTY. Nothing arrived between the handshake");
                              Say("      and here, so this says nothing about the DLPI contract."); } } }

                        /* ★★★★★★ 8-7: ARM THE INTERRUPT SOURCE, AND LET THE CARD DRIVE.
                         *
                         * ⛔⛔ THE MOST DANGEROUS THING THIS PROJECT HAS ASKED THE HARDWARE TO
                         *   DO. From the moment ArmIrq returns, the card can interrupt this
                         *   machine, and a fault at hardware interrupt level on OS 9 hangs it
                         *   with no NMI and no debugger.
                         *
                         *   The reason this is survivable is that the driver arms ONLY when
                         *   asked. It is not armed from EnetHAL_Open, so an extension sitting in
                         *   the System Folder never arms itself at boot: if this run hangs the
                         *   machine, the NEXT BOOT IS CLEAN and nothing has to be removed from
                         *   another volume. That is the same gate 8-2b used for the driver's
                         *   first write to the card, and for the same reason.
                         *
                         * ⚠ The line below is written and FLUSHED before arming. If the log ends
                         *   there, the ISR install or the first interrupt is the hang, and there
                         *   is nothing else to rule out first. */
                        if(armIrqProc && getIrqProc){
                          UInt32 hard=0,notOurs=0,queued=0,qFail=0,sec=0,secF=0,winBad=0;
                          UInt32 rxDone=0,lastR=0,lastD=0,state;
                          long   it; int armed;
                          Say("");
                          Say("  --- ★★★★★★ 8-7: THE INTERRUPT SOURCE ---");
                          Say("    ⚠⚠ ARMING NOW. From the next line the card can interrupt this");
                          Say("       machine. If the log STOPS here, the ISR install or the first");
                          Say("       interrupt is the hang -- log MacsBugCrash.log, and note that");
                          Say("       the next boot is clean because Open never arms by itself.");
                          ApDrawLog(win);
                          armed = (int)(*armIrqProc)();
                          Say1("    ArmIrq returned = ",(unsigned long)armed);
                          if(!armed){
                            Say("    [--] not armed. The driver's own lines above say why; this is");
                            Say("      a refusal, not a crash, and the app pump still works.");
                          } else {
                            Say("    ★ ARMED AND SURVIVED THE CALL. Letting the card drive for 10 s.");
                            ApDrawLog(win);

                            /* ★★★★★★ 8-8: TRANSMIT THROUGH THE DRIVER, INSIDE THE ARMED WINDOW.
                             *
                             * Sent here, before the 10 s of interrupt-driven receive, so that if
                             * the AP answers the reply arrives through the path 8-7 just proved.
                             *
                             * ⚠ THIS IS A CONTROL FOR EnetHAL_Write, NOT A REPLACEMENT FOR IT.
                             *   OT may or may not choose to send anything of its own; if it does
                             *   not, "our transmitter is broken" and "nothing asked us to
                             *   transmit" would be indistinguishable. Feeding an identical frame
                             *   through the identical code path separates them. The Write
                             *   counters below count BOTH, so the selector trace is what says
                             *   whether OT used it. */
                            if(txEthProc){
                              UInt8 arp[42]; int q;
                              static const UInt8 bc[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                              UInt8 body[64];
                              UInt16 bl = BuildArpRequestBody(body,shimMac);
                              Say("");
                              Say("    --- 8-8: transmitting an ARP request THROUGH the driver ---");
                              for(q=0;q<6;q++) arp[q]    = bc[q];
                              for(q=0;q<6;q++) arp[6+q]  = shimMac[q];
                              arp[12]=0x08; arp[13]=0x06;             /* ethertype ARP */
                              /* BuildArpRequestBody emits SNAP + ARP; the Ethernet frame carries
                               * the ARP alone, so skip the 8-byte SNAP the driver will re-add. */
                              for(q=0;q<28 && (8+q)<(int)bl;q++) arp[14+q] = body[8+q];
                              Say1("      Ethernet frame bytes = ",(unsigned long)42);
                              ApDrawLog(win);
                              Say1("      AirPortShimTxEthernet -> ",
                                   (unsigned long)(*txEthProc)(arp,42UL)); }

                            it = TickCount();
                            while(TickCount() - it < 10L*60L){
                              EventRecord iv;
                              if(WaitNextEvent(everyEvent,&iv,6,NULL)){
                                if(iv.what==updateEvt && win &&
                                   (WindowPtr)iv.message==win){
                                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } } }
                            state = (*getIrqProc)(&hard,&notOurs,&queued,&qFail,&sec,&secF,
                                                  &winBad,&rxDone,&lastR,&lastD);
                            Say("");
                            Say1("    interrupt state (0 none / 1 installed / 2 armed) = ",
                                 (unsigned long)state);
                            Say1("    ★ hardware interrupts TAKEN      = ",(unsigned long)hard);
                            Say1("      not ours (shared line, passed on) = ",(unsigned long)notOurs);
                            Say1("      DMA0 RX_DONE seen              = ",(unsigned long)rxDone);
                            Say1("      secondary handlers queued      = ",(unsigned long)queued);
                            Say1("      ⚠ queue FAILURES               = ",(unsigned long)qFail);
                            Say1("      secondary handlers RUN         = ",(unsigned long)sec);
                            Say1("    ★★ frames delivered BY INTERRUPT = ",(unsigned long)secF);
                            Say1("      ⚠ BAR0 window wrong on entry   = ",(unsigned long)winBad);
                            SayH("      last GEN_IRQ_REASON            = ",(unsigned long)lastR,8);
                            SayH("      last DMA0_REASON               = ",(unsigned long)lastD,8);
                            /* ★ The oracle, and it is layered so a partial result still names
                             * which layer stopped. Each line below can only be true if every
                             * line above it is. */
                            SayOk("  ★ the card INTERRUPTED this machine",        hard > 0);
                            SayOk("  ★★ the secondary handler RAN",               sec > 0);
                            SayOk("  ★★★★★★ FRAMES DELIVERED BY INTERRUPT",       secF > 0);
                            if(hard && !sec)
                              Say("    ⚠ Interrupts taken but no secondary ran. QueueSecondary"
                                  " failed or never fired -- RX is stalled with the mask at 0.");
                            if(sec && !secF)
                              Say("    ⚠ The secondary ran and delivered nothing. Read the DLPI"
                                  " histogram above: same conversion rules, same reasons.");
                            if(winBad)
                              Say("    ⚠⚠ The BAR0 window had MOVED. Something else re-pointed it;"
                                  " the ISR refused rather than reading a stranger's registers.");
                            if(!hard && !notOurs)
                              Say("    ⚠ NOTHING arrived at all -- not even a shared-line call."
                                  " Suspect driver-ist / the enabler, not the b43 side.");

                            /* ★★★★★★ 8-8: what the transmitter did. */
                            if(getTxEthProc){
                              UInt32 tc=0,ts=0,ta=0,tnk=0,tna=0,tbf=0,twb=0,tpw=0,tll=0;
                              (void)(*getTxEthProc)(&tc,&ts,&ta,&tnk,&tna,&tbf,&twb,&tpw,&tll);
                              Say("");
                              Say("  --- ★★★★★★ 8-8: EnetHAL_Write / the TRANSMIT path ---");
                              Say1("    write calls (ours + OT's)  = ",(unsigned long)tc);
                              Say1("    frames PUT ON THE AIR      = ",(unsigned long)ts);
                              Say1("    ★ frames the AP ACKed      = ",(unsigned long)ta);
                              Say1("      ⚠ posted but NO status for our cookie = ",
                                   (unsigned long)tll);
                              Say ("    refusals, by reason (a Write that did not transmit"
                                   " NEVER returns noErr):");
                              Say1("      not associated           = ",(unsigned long)tna);
                              Say1("      no pairwise key          = ",(unsigned long)tnk);
                              Say1("      malformed Ethernet frame = ",(unsigned long)tbf);
                              Say1("      BAR0 window had moved    = ",(unsigned long)twb);
                              Say1("      ⚠⚠ PN WRAPPED            = ",(unsigned long)tpw);
                              /* ★ THE ORACLE IS THE ACK AND THE AP OWNS IT. A frame we merely
                               * handed to the DMA engine proves the engine accepted a descriptor.
                               * An ACK proves a real access point received, CRC-checked and
                               * acknowledged a frame we encrypted with a key it also derived. */
                              /* ⚠ k90 PRINTED THIS AS A PASS ON A FRAME WHOSE STATUS NEVER
                               * ARRIVED. DrainTxStatus returned a COUNT of any status it
                               * drained -- two stale EAPOL entries -- and the oracle read it
                               * as our ACK. It now returns OUR frame's ACK bit, and tll above
                               * counts the case where no status with our cookie came back at
                               * all, which is a different failure and must not look the same. */
                              SayOk("  ★★★★★★ THE AP ACKNOWLEDGED AN ENCRYPTED FRAME FROM OT'S"
                                    " PATH", ta > 0);
                              if(tll)
                                Say("    ⚠⚠ A frame was POSTED and no TX status with our cookie\n"
                                    "      ever came back. That is not 'the AP ignored us' --\n"
                                    "      it is 'we cannot tell whether it transmitted'.");
                              if(ts && !ta){
                                Say("    ⚠ Transmitted and not ACKed. The frame reached the DMA");
                                Say("      engine; the AP did not answer it. Suspect the CCMP");
                                Say("      encap or the PN before suspecting the radio -- 8-3a");
                                Say("      already proved this TX path ACKs unencrypted frames."); }
                              if(tpw)
                                Say("    ⛔ THE PN WRAPPED. Reusing a PN under one key destroys"
                                    " CCMP; the transmitter stopped rather than continue."); }

                            (void)(*disarmIrqProc)(); } }

                        OTCloseProvider(ep);
                        Say("  endpoint closed again."); }

                      /* ★ 8-5g: what did the DRIVER read off the card?
                       * EnetHAL_Open has run by now, so the shim has done its own Name Registry
                       * walk, taken its own BAR0 and read the SPROM itself. Reporting it here --
                       * and comparing it in Oracle U against the address the APP reads later
                       * through a completely separate mapping -- is a cross-check worth having:
                       * two independent paths to the same six bytes. If they disagree, one of the
                       * two mappings is wrong, and that is far better learned now than three
                       * stages downstream when frames quietly go to the wrong place. */
                      if(getMacProc){
                        SInt32 mapErr = 0;
                        shimMacOk = (int)(*getMacProc)(shimMac,&mapErr);
                        Say("");
                        Say("  --- what the DRIVER read from the card, on its own ---");
                        Say1("    SsbFindAndMap inside the shim -> ",(unsigned long)mapErr);
                        if(shimMacOk){
                          Str255 L; L[0]=0; PCat(L,"    driver's MAC = ");
                          PCatMac(L,shimMac); Out(L);
                        } else {
                          Say("    [--] the driver could NOT read a sane address.");
                          Say("      ⚠ It refuses rather than inventing one: a wrong MAC would let");
                          Say("        OT proceed to Start on an address the card does not have,");
                          Say("        which is the failure this design exists to avoid."); } }

                      /* ★★★ 8-2a: the backplane inventory, read by the DRIVER from inside its own
                       * fragment. Compared against the app's in Oracle U. */
                      if(getCoresProc){
                        Say("");
                        Say("  --- ★ 8-2a: the backplane, enumerated by the DRIVER itself ---");
                        shimCoresOk = (int)(*getCoresProc)(&shimChipId,&shimChipRev,&shimNCores,
                                                           &shimIdx80211,&shimIdxCC,&shimScanErr);
                        Say1("    SsbScanCores inside the shim -> ",(unsigned long)shimScanErr);
                        if(shimCoresOk){
                          SayH("    chip id      = ",(unsigned long)shimChipId,4);
                          Say1("    chip rev     = ",(unsigned long)shimChipRev);
                          Say1("    cores        = ",(unsigned long)shimNCores);
                          Say1("    ChipCommon @ ",(unsigned long)shimIdxCC);
                          Say1("    802.11 core @ ",(unsigned long)shimIdx80211);
                          Say("    (BAR0 window saved and restored around the scan, so the app's");
                          Say("     view of the backplane is untouched by this.)");
                        } else {
                          Say("    [--] the driver could not enumerate the backplane."); } }

                      /* ★★★ 8-2b: what happened when the driver enabled the 802.11 core. */
                      if(coreStProc){
                        Say("");
                        Say("  --- ★ 8-2b: the driver enabling the 802.11 core (the first WRITE) ---");
                        shimCoreUp = (int)(*coreStProc)(&shimTmsLow,&shimCoreErr,&shimArmed);
                        Say1("    armed at Open time = ",(unsigned long)shimArmed);
                        Say1("    core enable result = ",(unsigned long)shimCoreErr);
                        SayH("    TMSLOW read back   = ",(unsigned long)shimTmsLow,8);
                        SayOk("  the core came up (CLOCK set, RESET clear -- read back, not assumed)",
                              shimCoreUp); }

                      /* ★★★ 8-2c: the driver's OWN firmware upload. */
                      if(getFwProc){
                        Say("");
                        Say("  --- ★ 8-2c: the driver uploading its OWN microcode ---");
                        shimFwOk = (int)(*getFwProc)(&shimFwRev,&shimFwIrq,&shimFwErr);
                        Say1("    upload result      = ",(unsigned long)shimFwErr);
                        SayH("    GEN_IRQ_REASON     = ",(unsigned long)shimFwIrq,8);
                        Say1("    ucode revision     = ",(unsigned long)shimFwRev);
                        SayOk("  the driver's firmware came up",shimFwOk); }

                      /* ★★★ 8-2d-2b: the driver's OWN initvals. */
                      if(getIvsProc){
                        Say("");
                        Say("  --- ★ 8-2d-2: the driver applying the INITVALS itself ---");
                        shimHaveIvs = 1;
                        shimIvOk = (int)(*getIvsProc)(&shimIvApp,&shimIvDecl,
                                                      &shimBsApp,&shimBsDecl,&shimIvErr);
                        Say1("    status                  = ",(unsigned long)shimIvErr);
                        { Str255 L; L[0]=0;
                          PCat(L,"    b0g0initvals5   applied "); PCatDec(L,shimIvApp);
                          PCat(L," of ");   PCatDec(L,shimIvDecl); PCat(L," declared"); Out(L);
                          L[0]=0;
                          PCat(L,"    b0g0bsinitvals5 applied "); PCatDec(L,shimBsApp);
                          PCat(L," of ");   PCatDec(L,shimBsDecl); PCat(L," declared"); Out(L); }
                        SayOk("  both initvals tables applied IN FULL",shimIvOk); }

                      /* ★★★★★ 8-2d-3: the driver's OWN G-PHY initialisation. */
                      if(getPhyProc){
                        Say("");
                        Say("  --- ★★★ 8-2d-3: the driver running b43_phy_init ITSELF ---");
                        shimHavePhy = 1;
                        shimPhyOk = (int)(*getPhyProc)(&shimGpioOk,&shimBoardFlags,&shimRadioOn,
                                                       &shimPhyErr,&shimCoreWasUp);
                        Say1("    core was already up at Open = ",(unsigned long)shimCoreWasUp);
                        Say1("    GpioInit ChipCommon check   = ",(unsigned long)shimGpioOk);
                        SayH("    board flags (SPROM +0x72)   = ",(unsigned long)shimBoardFlags,4);
                        Say1("    radio powered on            = ",(unsigned long)shimRadioOn);
                        Say1("    phy status                  = ",(unsigned long)shimPhyErr);
                        SayOk("  ApPhyInitG() returned 1 -- all fifteen Stage 4 oracles",shimPhyOk); }

                      /* ★★★ 8-2e: the driver running chip_init's TAIL. */
                      if(getTailProc){
                        Say("");
                        Say("  --- ★ 8-2e: the driver running chip_init statements 8-13 ---");
                        shimHaveTail = 1;
                        shimTailOk = (int)(*getTailProc)(&shimTailMacctl,&shimTailTms,
                                                         &shimTailPretbtt,&shimTailPrmax,
                                                         &shimTailErr);
                        SayH("    MACCTL after opmode = ",(unsigned long)shimTailMacctl,8);
                        SayH("    TMSLOW after tail   = ",(unsigned long)shimTailTms,8);
                        Say1("    PRETBTT             = ",(unsigned long)shimTailPretbtt);
                        Say1("    PRMAXTIME read back = ",(unsigned long)shimTailPrmax);
                        SayOk("  all three tail read-backs held",shimTailOk); }

                      /* ★★★ 8-2f: the driver's OWN RX ring. */
                      if(getDmaProc){
                        Say("");
                        Say("  --- ★ 8-2f: the driver building and arming an RX ring ---");
                        shimHaveDma = 1;
                        shimDmaOk = (int)(*getDmaProc)(&shimDmaRingPhys,&shimDmaReadback,
                                                       &shimDmaNBuf,&shimDmaRxStat,&shimDmaErr);
                        SayH("    RX ring physical    = ",(unsigned long)shimDmaRingPhys,8);
                        SayH("    RXRING read back    = ",(unsigned long)shimDmaReadback,8);
                        Say1("    usable buffer pages = ",(unsigned long)shimDmaNBuf);
                        SayH("    RXSTATUS when armed = ",(unsigned long)shimDmaRxStat,8);
                        SayOk("  the driver armed its own RX ring",shimDmaOk); }

                      /* ★★★ 8-2d-1: REPLAY THE DRIVER'S OWN NARRATION INTO THIS LOG.
                       *
                       * Everything above this point is the driver reporting RESULTS through
                       * bespoke accessors -- one export per fact, each one hand-written. That
                       * does not scale: ap_phy_initg.h has 736 logging call sites and there will
                       * not be 736 accessors. This is the general mechanism that replaces them.
                       *
                       * The driver wrote these lines with the SAME Say/SayH/Say1/PCat as the
                       * application, over a sink that only memcpy's into static storage. What
                       * appears below is therefore byte-for-byte what the app would have written
                       * for the same events -- which is exactly what makes 8-2d-2 and 8-2d-3
                       * checkable: their output can be diffed against the app's banked logs.
                       *
                       * ⚠ THE INDENT MARKS THE SOURCE. Every replayed line is prefixed with "D|"
                       *   so that no one reading this log later can mistake the driver's voice
                       *   for the application's. The two now produce identical text on purpose,
                       *   and that is precisely why they have to be told apart. */
                      if(getLogProc){
                        Say("");
                        Say("  --- ★★★ 8-2d-1: the DRIVER'S OWN NARRATION, replayed verbatim ---");
                        shimLogN = (*getLogProc)(0,(UInt8*)0,&shimLogDropped,&shimLogBytes);
                        Say1("    lines the driver narrated = ",(unsigned long)shimLogN);
                        Say1("    lines it had to DROP      = ",(unsigned long)shimLogDropped);
                        Say1("    ring bytes used           = ",(unsigned long)shimLogBytes);
                        if(shimLogDropped){
                          Say("    ⚠⚠ THE RING FILLED. The lines below are the START of the");
                          Say("       narration and its TAIL IS MISSING -- ap_ring.h fills and");
                          Say("       stops rather than wrapping. Do NOT read the last line below");
                          Say("       as the point the driver stopped: raise AP_RING_BYTES."); }
                        if(shimLogN == 0){
                          Say("    [--] the driver narrated NOTHING.");
                          Say("      ⚠ That is a LOGGING failure, not a bring-up failure, and the");
                          Say("        two must not be confused. The 8-5g/8-2a/8-2b/8-2c results");
                          Say("        above were read through separate accessors and stand on");
                          Say("        their own. Suspect the sink: Out() in ap_shim.c, ap_ring.h,");
                          Say("        or EnetHAL_Open never reaching ApShimNarrateOpen().");
                        } else {
                          UInt32 li;
                          for(li = 0; li < shimLogN; li++){
                            Str255 D, R;
                            R[0] = 0;
                            (void)(*getLogProc)(li,(UInt8*)R,(UInt32*)0,(UInt32*)0);
                            D[0] = 0; PCat(D,"    D| ");
                            { short k; for(k = 1; k <= R[0] && D[0] < 255; k++) D[++D[0]] = R[k];
                              /* ⚠ A Str255 cannot hold 255 driver bytes plus our 7-byte prefix,
                               * so a very long line loses its tail here. Say so IN the line --
                               * a silently shortened log line is how a register dump comes to
                               * read as if the driver stopped mid-word.
                               * ⚠ Cut BACK before appending: PCat stops at 255, so a marker
                               * appended to a full Str255 would itself be truncated to " [LIN"
                               * and the warning would become the thing it warns about. */
                              if(k <= R[0]){
                                if(D[0] > 228) D[0] = 228;
                                PCat(D," [LINE TRUNCATED BY REPLAY]"); } }
                            Out(D); }
                          shimLogReplayed = 1;
                          Say("    --- end of the driver's narration ---"); } }

                      /* ⚠⚠ 8-2g: QUIESCE THE CARD BEFORE THIS APP QUITS.
                       * The driver's ring is still armed and the card holds physical addresses
                       * for the driver's system-heap pages. If this app exits and CFM releases
                       * the fragment while the engine is live, the card DMAs into freed memory
                       * -- silent corruption, no message. OT's Close does it too, so the
                       * ordinary teardown is covered; this covers the rest. */
                      if(quiesceProc){
                        Say("");
                        Say1("  [8-2g] quiescing the card through the driver -> ",
                             (unsigned long)(*quiesceProc)());
                        Say("    MAC suspended, both DMA engines reset, ring registers zeroed."); }

                      /* ⚠ DISARM. The card is about to belong to the application again. */
                      if(armProc){
                        (void)(*armProc)(0);
                        Say("  [8-2b] driver DISARMED -- the app owns the card from here."); } } } } }
            } else {
              Say("  ⚠ EnetShimAsyncStatus did not resolve, so link state could not be reported."); } }

          if(shim85bInstalled){
            /* ★★★ 8-5c: k54 REGISTERED CLEANLY AND WAS NEVER CALLED, so waiting longer alone is
             * not the experiment -- five seconds and ninety seconds of an idle OT look the same.
             * Oracle U's own "next" said it: OT does not drive a port it has merely been TOLD
             * about. Something has to select it.
             *
             * So the run now holds the port open and asks for one action. The window says what to
             * do; the log records what happened, so nothing has to be read off the screen and
             * nothing has to be photographed.
             *
             * ⚠ IT BREAKS OUT THE MOMENT A SELECTOR ARRIVES, so a run where OT drives us
             *   immediately costs nothing, and a run where the tester does nothing costs the full
             *   wait and still produces a clean "not driven" -- which stays a real answer rather
             *   than becoming a timeout. */
            long t0, last = -1; int waited = 0;
            /* ⚠ THE MANUAL STEP IS GONE, AND SO IS THE TWO-MINUTE WAIT.
             *
             * k55 asked the tester to select the port in TCP/IP. They could not: no such entry
             * appeared -- and, because of the update-event bug above, they could not read the
             * instructions either. Asking again would be asking someone to pick something that
             * demonstrably is not in the list.
             *
             * The port listing either side of the registration answers the same question without
             * anyone leaving their chair, and answers it better: it says whether a port record
             * exists at all and, if it does, which flag is keeping it out of the pop-up. So the
             * wait shrinks to a token window for an eager OT and the enumeration does the work. */
            Say("");
            Say1("  holding the port briefly, seconds = ",
                 (unsigned long)(AP85C_WAIT_TICKS/60));
            Say("  (no action needed -- the port listing above and below is the instrument now)");
            /* ⚠⚠ THIS LOOP MUST YIELD, AND THE FIRST VERSION OF IT DID NOT.
             *
             * Mac OS 9 is cooperatively scheduled. A `while(TickCount() - t0 < n);` busy-wait does
             * not merely make this app unresponsive -- it stops the FINDER and every control panel
             * with it. The one thing this experiment asks the tester to do is open TCP/IP and pick
             * a port, and a busy-wait makes that impossible for exactly as long as we are waiting
             * for it to happen. The run would have looked like a hung machine and produced "never
             * called" every time, for a reason that had nothing to do with OT.
             *
             * WaitNextEvent with a sleep of 30 ticks hands the processor to other processes and
             * still wakes us twice a second, which is far finer than needed to notice a selector. */
            ApDrawLog(win);           /* ★ paint the instructions BEFORE the wait, not after it */
            t0 = TickCount();
            while(TickCount() - t0 < AP85C_WAIT_TICKS){
              long secs;
              EventRecord waitEvt;
              if(WaitNextEvent(everyEvent,&waitEvt,30,NULL)){
                /* ★ HANDLE THE UPDATE. Discarding it leaves the region invalid, so the Window
                 * Manager re-posts it forever and the window never paints -- which is exactly
                 * what k55 did to the tester. */
                if(waitEvt.what==updateEvt && win && (WindowPtr)waitEvt.message==win){
                  BeginUpdate(win); ApDrawLog(win); EndUpdate(win); } }
              secs = (TickCount() - t0) / 60;
              if(statsProc){
                UInt32 tot = 0;
                (void)(*statsProc)(&tot,0,0,0,0,0);
                if(tot > 0){
                  Say1("  ★★★ A SELECTOR ARRIVED after seconds = ",(unsigned long)secs);
                  break; } }
              if(secs != last && (secs % 15) == 0){
                Say1("    ...waiting, seconds elapsed = ",(unsigned long)secs);
                ApDrawLog(win);       /* so the countdown is visible, not just logged */
                last = secs; }
              waited = 1; }
            (void)waited;
          }

          /* 5. Read the counters out of the driver fragment, registered or not. */
          if(statsProc){
            UInt32 magic, total = 0, counts[AP_SHIM_NSEL_APP], orderN = 0, lastRef = 0;
            UInt8 order[24]; SInt32 firstBad = -1;
            int z;
            for(z=0;z<AP_SHIM_NSEL_APP;z++) counts[z]=0;
            for(z=0;z<24;z++) order[z]=0;
            magic = (*statsProc)(&total,counts,order,&orderN,&firstBad,&lastRef);
            if(magic != 0x41505348UL){
              Say("  ⚠ the stats accessor did not return its magic. The counters below are NOT");
              Say("    trustworthy -- this is 'I am reading the wrong memory', not 'nothing ran'.");
            } else {
              shim85bCalls = (int)total;
              shimOpenCalls = (int)counts[1];       /* 8-2d-1: EnetHAL_Open == selector 1 */
              Say1("  [ok] stats magic OK. total selector calls = ",(unsigned long)total);
              if(total == 0){
                Say("    ⇒ Registered but never called. That is a REAL result and not a failure:");
                Say("      OT may not drive a port until it is selected in the TCP/IP or AppleTalk");
                Say("      control panel. NEXT: register, then pick the port by hand and re-read.");
              } else {
                static const char *kSelName[11] = {
                  "RegisterPorts","Open","Close","Start","Stop","Read","Write",
                  "GetMACAddress","SetMACAddress","SetMulticastFilters","Status" };
                Str255 L; UInt32 q;
                Say("    ⇒ ★★★ OT DROVE OUR DRIVER. The order below is the contract 8-5c must");
                Say("      implement against -- it is what OT actually asks for, and in which");
                Say("      sequence, rather than what the header's enum happens to list first.");
                L[0]=0; PCat(L,"      sequence:");
                for(q=0; q<orderN && q<24; q++){
                  PCat(L," "); PCat(L,(char*)kSelName[order[q] < 11 ? order[q] : 0]); }
                Out(L);
                for(q=0;q<AP_SHIM_NSEL_APP;q++) if(counts[q]){
                  L[0]=0; PCat(L,"      "); PCat(L,(char*)kSelName[q]);
                  PCat(L," x"); PCatDec(L,(unsigned long)counts[q]); Out(L); }
                if(firstBad >= 0 && firstBad < 11){
                  L[0]=0; PCat(L,"      ⚠ first selector we REFUSED: ");
                  PCat(L,(char*)kSelName[firstBad]); Out(L);
                  Say("        If the sequence stops there, that is the one 8-5c must implement");
                  Say("        first -- and that is exactly why this build refuses honestly");
                  Say("        instead of returning noErr to keep the conversation going."); } } } }

          /* ⚠ 8-6a DOES NOT LIVE HERE, and the reason is an ordering trap this project has
           * already paid for once. It runs BEFORE OTCloseProvider -- see the block after the
           * four-way handshake. Close calls AirPortShimQuiesce(): by this point the MAC is
           * down and the ring is torn down, so a pump here would walk a dead receiver and
           * report 'no frames' for a reason with nothing to do with the DLPI contract.
           * k73 lost a run to exactly this mistake with the receiver poll. */
          } /* end of the non-witness driving path */

          /* 6. Remove, on every path. A port left registered against a driver that refuses to
           *    start is the kind of thing that makes the next run's networking inexplicable. */
          if(shim85bInstalled){
            OSErr re = (*removeProc)(shimRef,false);
            Say1("  EnetShimRemoveDriver -> ",(unsigned long)re);
            if(re > 0) Say("    (positive = pending; the shim completes the removal itself)");
            SayOk("  the port was removed again",(re == noErr) || (re > 0)); }
        } else {
          Say("  [--] not registering: a required entry point is missing. Nothing was installed."); }

        if(shimCid) (void)CloseConnection(&shimCid);
        if(drvCid)  (void)CloseConnection(&drvCid); }
      if(otUp) CloseOpenTransport(); }
#if AP_APP_DRIVES_RADIO
    /* ⚠⚠ EVERYTHING IN HERE IS OFF AS OF 8-2g, AND THE CODE IS KEPT RATHER THAN DELETED.
     *
     * The driver owns the radio now. It brings the card up, arms an RX ring and enables the
     * MAC, and it cannot hand any of that back -- a receiver has to stay armed. Two owners of
     * one DMA engine is not a thing that can be made to work by ordering.
     *
     * ⚠ WHAT IS LOST WHILE THIS IS OFF, said plainly rather than discovered later: the app's
     *   authentication, association, four-way handshake and CCMP oracles. Fifteen of its
     *   thirty-one oracles survive, because ApPhyInitG re-runs them inside the driver and the
     *   narration carries them into this log. The rest come back when that work migrates.
     *
     * ⚠ KEPT, NOT DELETED, because it is the reference the migration is checked against. Every
     *   extraction so far has been proven by reconstruction against this file; deleting the
     *   original would retire the only thing those proofs compare to. It is also one #define
     *   away from running again if an increment needs the app's instruments back. */
    Say("");
    Say("  Every stage so far verified that the card accepted something we wrote. This is the");
    Say("  first where the card must act on its own and we read the result: receive a real");
    Say("  802.11 frame off the air and DMA it into our memory.");
    Say("");
    Say("  One beacon proves four things at once --");
    Say("    the card READS our descriptor ring (it had to, to learn where to put the frame)");
    Say("    the card WRITES our buffers");
    Say("    PowerMac PCI DMA is cache-coherent, because we can see what it wrote");
    Say("    Stage 4's PHY and radio work actually receives, end to end");
    Say("");
    Say("  ⚠ THE SWEEP EXISTS BECAUSE A TUNING MISS LOOKS EXACTLY LIKE A BROKEN RECEIVER.");
    Say("    ApBringUp tunes to channel 1. Channels 1, 6 and 11 are each given a CLEAN 32-slot");
    Say("    budget -- ring reset, buffers re-poisoned, descriptors rewritten -- so a busy");
    Say("    channel cannot starve the next one of descriptors and make it look empty.");
    Say("");
    Say("  ORACLE A: the rings still program cleanly (k2 regression).");
    Say("  ORACLE B: ★ b43_phy_init COMPLETE -- all fifteen Stage 4 oracles, AND the trailing");
    Say("            b43_switch_channel that writes the firmware's channel cookie. k3 ran");
    Say("            neither; k4 ran the oracles but not the cookie.");
    Say("  ORACLE C: chip_init statements 8-13 land -- antennae, opmode, MAC/PHY clock.");
    Say("  ORACLE D: core_init's five remaining statements land their values.");
    Say("  ORACLE E: MACCTL gains ENABLED and AWAKE, via the refcount b43 uses.");
    Say("  ORACLE F: ★ A FRAME ARRIVED -- poison gone, frame_len non-zero, RXDPTR moved.");
    Say("  ORACLE G: the frame decodes as plausible 802.11 -- five fields that must AGREE,");
    Say("            not one field that can pass on garbage as k5's did.");
    Say("  ORACLE H: ★ 5-4, the TX engine consumed our descriptors with no error.");
    Say("  ORACLE I: ★ 5-4, AN ACCESS POINT ANSWERED OUR PROBE REQUEST -- and this is the");
    Say("            FIRST build in which a failure of it means anything. k10 through k20");
    Say("            scored Oracle I against a receiver that was starved from the first");
    Say("            second of the transmit section onward, so eleven runs' worth of \"no");
    Say("            probe response\" was measured deaf. See RxRecycle's header.");
    Say("");

    for(i=0;i<K6_NCHAN;i++){ chanFrames[i]=0; chanFirst[i]=-1; chanRxStat[i]=0; }

    if(!ApBringUp()){Say("!! bring-up failed");goto cleanup;}

    /* ★ SELECT THE TXHDR LAYOUT FROM WHAT THE CARD ACTUALLY LOADED. b43, main.c:2699. */
    gTxhShift = (gFwRev >= 410) ? TXH_FMT_410_EXTRA : 0;
    Say("");
    SayH("  ucode revision reported by the card = ",(unsigned long)gFwRev,4);
    Say1("  -> b43_txhdr_size() = ",(unsigned long)TXH_SIZE_351);
    Say1("     cookie at +",(unsigned long)TXH_351_COOKIE);
    Say1("     main PLCP at +",(unsigned long)TXH_351_PLCP);
    Say (gTxhShift ? "     FW_HDR_410: mimo_antenna and preload_size sit ahead of the cookie"
                   : "     FW_HDR_351: no mimo fields; the cookie follows the 2-byte pad");

    base = B43_MMIO_DMA32_BASE0;
    if(base+0x20UL > gBus.bar0Size){Say("!! 0x200 outside BAR0");goto cleanup;}

    Say("");
    Say("=== [5-4c] ★ THE STAGE-BOUNDARY BISECT ===");
    Say("  b43_dummy_transmission at each boundary, with ShmClearTssi and a GEN_IRQ_REASON");
    Say("  clear in front of every one, so PHY_TXERR is attributable to that transmission.");
    Say("  ⚠ k19's VERSION WAS VACUOUS. Boundaries 1-4 printed \"(did not radiate)\" -- four");
    Say("    clean readings from four transmissions that never happened, because MACCTL_ENABLED");
    Say("    is not set until boundary 5. k20 FORCES the MAC on for each probe and restores it");
    Say("    after, and scores on RADIATION: a boundary is clean only if TSSI MOVED and no");
    Say("    PHY_TXERR. SILENT is its own outcome and is never counted as success.");
    Say("");
    Say("  ⚠ Forcing the MAC on before dma_init is OUT OF b43'S ORDER and is a deliberate probe");
    Say("    liberty, taken because a bisect in which only one point can transmit is not a");
    Say("    bisect. Each probe restores MACCTL to exactly what it found.");
    Say("");
    Say("  Each line also dumps RFOVER/RFOVERVAL. The T/R switch lives in RFOVERVAL's TRSWRX");
    Say("  field, and TSSI measures power at the PA -- so a switch stuck in receive would look");
    Say("  exactly like what we have: real measured power that never reaches the air.");
    Say("");
    (void)TxProbeHere("1. after ApBringUp -- chip_init 1-6, NO PHY init yet   ");

    coreRev = IDHIGH_REV(ssb_r32(gBus.bar0,SSB_IDHIGH));
    phyRev  = (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_PHY_VER) & 0x000F);

    /* ---------------- chip_init statement 7: THE PHY ---------------- */
    Say("");
    Say("=== [5-3a0] b43_phy_init -- chip_init STATEMENT 7 ===");
    Say("  ⚠ THIS IS WHAT k3 WAS MISSING. ap_bringup.h runs chip_init 1-6 and stops, and says");
    Say("    so in its own header. k3 read past that and enabled a MAC whose PHY had never been");
    Say("    initialised. Everything below the next line is Stage 4's own log, replayed from");
    Say("    ap_phy_initg.h -- all fifteen oracles, as a regression, on every run.");
    Say("");
    phyOk = ApPhyInitG();
    Say("");
    SayOk("b43_phy_initg completed with all fifteen Stage 4 oracles passing",phyOk);
    if(!phyOk){
      Say("  !! THE PHY DID NOT INITIALISE. Read the oracle report above; nothing below can");
      Say("     mean anything, and this is a Stage 4 REGRESSION, not a Stage 5 problem.");
      goto cleanup; }
    /* ⚠ b43_phy_init's LAST statement, and the one k3 and k4 both missed. ops->init is only
     * its middle; phy_common.c ends with b43_switch_channel(dev, phy->channel), which writes the
     * channel cookie the MICROCODE reads and then waits 8 ms for the radio to settle.
     * b43_gphy_op_get_default_chan() returns 1 for the G-PHY, so that is phy->channel here. */
    (void)TxProbeHere("2. after ApPhyInitG -- b43_phy_initg complete          ");

    Say("");
    Say("  b43_phy_init's trailing b43_switch_channel(dev, phy->channel = 1):");
    SayH("    SHM_SH_CHAN (0x00A0) before = ",(unsigned long)ShmRead16Shared(B43_SHM_SH_CHAN),4);
    Say ("      ⚠ NO PROBE IN THIS PROJECT HAS EVER WRITTEN THAT WORD. Whatever it reads above");
    Say ("        is what the initvals left, and the microcode has been believing it.");
    SwitchChannel(1);
    SayH("    SHM_SH_CHAN after           = ",(unsigned long)ShmRead16Shared(B43_SHM_SH_CHAN),4);
    chanCookieOk = (ShmRead16Shared(B43_SHM_SH_CHAN)==1);
    (void)TxProbeHere("3. after b43_switch_channel -- cookie + settle         ");
    SayOk("the firmware's channel cookie now reads 1",chanCookieOk);
    oracleB = phyOk && chanCookieOk;

    /* ---------------- chip_init statements 8-13 ---------------- */
    Say("");
    Say("=== [5-3a1] b43_chip_init STATEMENTS 8-13 -- never run by any probe before k4 ===");
    Say1("  core revision (IDHIGH) = ",(unsigned long)coreRev);
    Say1("  PHY revision           = ",(unsigned long)phyRev);
    SayH("  chip id / rev          = ",(unsigned long)gBus.chipId,4);
    Say1("                     rev = ",(unsigned long)gBus.chipRev);
    Say ("  [8]  interference mitigation -- PROVABLY inert (interfmode already NONE == 0)");
    Say ("  [9]  set_rx_antenna(DEFAULT=AUTO0) + mgmtframe_txantenna");
    Say ("  [10] PRMAXTIME = 0");
    Say ("  [11] b43_adjust_opmode");
    Say ("  [12] mac_phy_clock_set(true)");
    Say ("  [13] POWERUP_DELAY -- deliberately NOT written, see the source note");
    SayH("  POWERUP_DELAY currently reads = ",
         (unsigned long)ssb_r16(gBus.bar0,B43_MMIO_POWERUP_DELAY),4);
    macctlPreTail = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    tmslowPreTail = ssb_r32(gBus.bar0,SSB_TMSLOW);
    ChipInitTail(coreRev,phyRev,gBus.chipId,gBus.chipRev,&macctlTail,&pretbtt);
    tmslowPostTail = ssb_r32(gBus.bar0,SSB_TMSLOW);
    Say("");
    SayH("  MACCTL before tail = ",macctlPreTail,8);
    SayH("  MACCTL after  tail = ",macctlTail,8);
    SayH("    INFRA            = ",(unsigned long)(macctlTail&B43_MACCTL_INFRA),8);
    SayH("    DISCPMQ          = ",(unsigned long)(macctlTail&B43_MACCTL_DISCPMQ),8);
    SayH("    PROMISC          = ",(unsigned long)(macctlTail&B43_MACCTL_PROMISC),8);
    Say ((coreRev<=4) ? "      (core rev <= 4, so b43 FORCES PROMISC -- the HW address filter is"
                        " known broken there)"
                      : "      (core rev > 4, so PROMISC stays clear and the HW filter is used)");
    Say1("  cfp_pretbtt written = ",(unsigned long)pretbtt);
    Say ((gBus.chipId==0x4306UL && gBus.chipRev==3UL)
         ? "      (chip 0x4306 rev 3 -> 100, the special case)"
         : "      (50, the general infrastructure-STA value)");
    SayH("  TMSLOW before tail = ",tmslowPreTail,8);
    SayH("  TMSLOW after  tail = ",tmslowPostTail,8);
    SayH("    MACPHYCLKEN      = ",(unsigned long)(tmslowPostTail&B43_TMSLOW_MACPHYCLKEN),8);
    oracleC = ((macctlTail & B43_MACCTL_INFRA)!=0) &&
              ((macctlTail & B43_MACCTL_DISCPMQ)!=0) &&
              ((macctlTail & B43_MACCTL_AP)==0) &&
              ((tmslowPostTail & B43_TMSLOW_MACPHYCLKEN)!=0) &&
              (pretbtt==50 || pretbtt==100);
    SayOk("chip_init's tail landed: INFRA, DISCPMQ, MAC/PHY clock on, pretbtt set",oracleC);
    (void)TxProbeHere("4. after ChipInitTail -- chip_init 8-13 complete       ");

    /* ================================================================================
     * ★★★★★ [5-3a0b] THE BLOCK OF b43_wireless_core_init THAT NO PROBE HAS EVER RUN.
     *
     * THE MEASUREMENT THAT SENT US HERE. A monitor-mode capture on channel 1, 110,392
     * packets over the twenty minutes spanning the k20 run -- 5,151 beacons, 1,637 ACKs and
     * 275 probe responses in the ninety seconds around it, so the capture was healthy --
     * contains ZERO frames from this card. The OUI 00:11:24 appears nowhere in the file, in
     * any address field, under any byte-order variant of the MAC. Meanwhile Mac OS X 10.4.11
     * associates with the same card on the same antenna to the same AP on the same channel.
     *
     * So: the MAC accepts our frame, the DMA engine consumes it, the microcode reports TX_OK
     * with supp_reason NONE, the PA measurably produces energy -- and no demodulable 802.11
     * signal comes out. We are radiating something that is not a valid waveform.
     *
     * ⚠ AND THE REASON IS A SCOPING ERROR, FOR THE THIRD TIME IN THIS PROJECT.
     *   k3 stopped at chip_init statement 6 when statement 7 was b43_phy_init.
     *   k4 ported ops->init when the enclosing function was b43_phy_init.
     *   Now: [5-3b] is titled "b43_wireless_core_init's REMAINING FIVE STATEMENTS" and its
     *   own text says "k2 stopped after b43_dma_init". It ports what comes AFTER dma_init.
     *   Nobody ever ported what comes BEFORE it, because Stage 2 jumped from chip_init
     *   straight to the rings. main.c:4872..4925 is that gap, and it is where the microcode
     *   is told what hardware it is driving.
     *
     * Ported verbatim and in b43's order. Every word is reported before and after, because
     * a value that was already correct and a value we fixed are different findings.
     * ============================================================================== */
    Say("");
    Say("=== [5-3a0b] ★★★ b43_wireless_core_init, chip_init..dma_init -- NEVER RUN BEFORE ===");
    {
      UInt16 lo,mi,hi, was;
      Say("  main.c:4872-4925, in order. Before/after for every word.");
      Say("");

      /* stmt: b43_shm_write16(SHARED, B43_SHM_SH_WLCOREREV, dev->dev->core_rev) */
      was = ShmRead16Shared(0x0016);
      ShmWrite16Shared(0x0016,(UInt16)coreRev);
      SayH("  WLCOREREV   0x0016  was ",(unsigned long)was,4);
      SayH("                      now ",(unsigned long)ShmRead16Shared(0x0016),4);
      Say ("    the core revision, told to the microcode.");

      /* stmt: hf = b43_hf_read(); ... ; b43_hf_write(hf).
       * PHY is G -> HF_SYMW. phy->rev is 2, not 1, so no GDCW. boardflags_lo = 0x000A, and
       * B43_BFL_PACTRL is 0x0002 -> SET, so OFDMPABOOST applies. radio_ver 0x2050 and
       * radio_rev 2: rev != 6 so no 4318TSSI, rev < 6 so HF_VCORECALC. XTAL_NOSLOW is not in
       * 0x000A. The PCISCW arm is inside #if CONFIG_SSB_DRIVER_PCICORE and needs pcicore
       * revision <= 10, which we cannot read here, so it is NOT applied and that is recorded
       * rather than guessed. Finally hf &= ~SKCFPUP. */
      HfRead(&lo,&mi,&hi);
      Say("");
      { Str255 L;L[0]=0;PCat(L,"  host flags  was  hi ");PCatHex(L,hi,4);
        PCat(L," mi ");PCatHex(L,mi,4);PCat(L," lo ");PCatHex(L,lo,4);Out(L); }
      lo = (UInt16)(lo | 0x0002);                      /* B43_HF_SYMW,      bit 1  */
      if((UInt32)ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SPROM_BFLLO) & 0x0002)
        lo = (UInt16)(lo | 0x0040);                    /* B43_HF_OFDMPABOOST, bit 6 */
      mi = (UInt16)(mi | 0x0004);                      /* B43_HF_VCORECALC, bit 18 */
      hi = (UInt16)(hi & ~0x0400);                     /* ~B43_HF_SKCFPUP,  bit 26 */
      HfWrite(lo,mi,hi);
      HfRead(&lo,&mi,&hi);
      { Str255 L;L[0]=0;PCat(L,"  host flags  now  hi ");PCatHex(L,hi,4);
        PCat(L," mi ");PCatHex(L,mi,4);PCat(L," lo ");PCatHex(L,lo,4);Out(L); }
      Say ("    SYMW (G-PHY SYM workaround) and VCORECALC (radio_rev 2 < 6, force VCO");
      Say ("    recalculation when powering up synthpu) are the two that matter here, and");
      Say ("    NEITHER HAS EVER BEEN SET. VCORECALC in particular is a transmit-path bit.");
      Say ("    ⚠ PCISCW is NOT applied: it needs the ssb pcicore revision, which this probe");
      Say ("      does not read. Recorded as unported rather than guessed at.");

      /* core_rev is 5, so the >= 13 MAC_HW_CAP arm is unreachable. Not ported, by inspection. */
      Say("");
      Say1("  core_rev = ",(unsigned long)coreRev);
      Say ("    < 13, so the MACHW_L/H capability words are not written. Unreachable arm.");

      /* stmt: b43_set_retry_limits(7, 4) -- SCRATCH, not SHARED */
      Say("");
      ShmControl(2UL,0x0006); was = ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA);
      ShmControl(2UL,0x0006); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,7);
      ShmControl(2UL,0x0007); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,4);
      SayH("  SC_SRLIMIT  0x0006  was ",(unsigned long)was,4);
      Say ("    k23 measured 7 and 4 already present, so this one changes nothing. Ported");
      Say ("    anyway: a value that happened to be right is not the same as a value we set.");

      /* stmts: SFFBLIM = 3, LFFBLIM = 2 */
      was = ShmRead16Shared(0x0044); ShmWrite16Shared(0x0044,3);
      SayH("  SFFBLIM     0x0044  was ",(unsigned long)was,4);
      SayH("                      now ",(unsigned long)ShmRead16Shared(0x0044),4);
      was = ShmRead16Shared(0x0046); ShmWrite16Shared(0x0046,2);
      SayH("  LFFBLIM     0x0046  was ",(unsigned long)was,4);
      SayH("                      now ",(unsigned long)ShmRead16Shared(0x0046),4);

      /* stmt: PRMAXTIME = 1. ⚠ ChipInitTail wrote 0 earlier. b43's core_init writes 1 AFTER
       * chip_init, so 1 is the value the hardware should end up with, and 0 -- which means
       * "infinite" -- has been left in place by every probe. */
      was = ShmRead16Shared(B43_SHM_SH_PRMAXTIME);
      ShmWrite16Shared(B43_SHM_SH_PRMAXTIME,1);
      SayH("  PRMAXTIME   0x0074  was ",(unsigned long)was,4);
      SayH("                      now ",(unsigned long)ShmRead16Shared(B43_SHM_SH_PRMAXTIME),4);
      Say ("    ⚠ ChipInitTail writes 0 here and b43's core_init overwrites it with 1. Zero is");
      Say ("      INFINITE, i.e. the firmware may answer probe requests itself; 1 us always");
      Say ("      times out, which is how b43 suppresses that. Every probe has left it 0.");

      /* ★ stmt: b43_rate_memory_init -- main.c:3177. For each rate, copy SHARED[off] to
       * SHARED[off + 0x20]. CCK base 0x4C0, OFDM base 0x480, index = ratecode & 0xF, x2.
       * This is the microcode's rate table. If it was never built, the firmware has no
       * mapping from our phy_rate 0x0A to a modulation, and "transmits an invalid waveform"
       * is exactly what you would expect. THIS IS THE PRIME SUSPECT. */
      Say("");
      Say("  ★★★ b43_rate_memory_init -- THE PRIME SUSPECT. main.c:3162:");
      Say("        shm[off + 0x20] = shm[off]   with off = 0x4C0 + (cck_ratecode & 0xF)*2");
      Say("                                       or off = 0x480 + (ofdm_ratecode & 0xF)*2");
      { static const UInt16 kOfdmCode[8] = {0x000B,0x000F,0x000A,0x000E,
                                            0x0009,0x000D,0x0008,0x000C};
        static const UInt16 kCckCode[4]  = {0x000A,0x0014,0x0037,0x006E};
        int i;
        for(i=0;i<8;i++){
          UInt32 off = 0x0480UL + (UInt32)(kOfdmCode[i] & 0x000F)*2UL;
          UInt16 v   = ShmRead16Shared(off);
          ShmWrite16Shared(off+0x20,v);
          { Str255 L;L[0]=0;PCat(L,"      OFDM off 0x");PCatHex(L,off,4);
            PCat(L," = 0x");PCatHex(L,v,4);PCat(L,"  ->  0x");PCatHex(L,off+0x20,4);
            PCat(L," now 0x");PCatHex(L,ShmRead16Shared(off+0x20),4);Out(L); } }
        for(i=0;i<4;i++){
          UInt32 off = 0x04C0UL + (UInt32)(kCckCode[i] & 0x000F)*2UL;
          UInt16 v   = ShmRead16Shared(off);
          ShmWrite16Shared(off+0x20,v);
          { Str255 L;L[0]=0;PCat(L,"      CCK  off 0x");PCatHex(L,off,4);
            PCat(L," = 0x");PCatHex(L,v,4);PCat(L,"  ->  0x");PCatHex(L,off+0x20,4);
            PCat(L," now 0x");PCatHex(L,ShmRead16Shared(off+0x20),4);
            if(kCckCode[i]==0x000A) PCat(L,"   <- 1 Mbps, the rate we transmit at");
            Out(L); } } }

      /* stmt: b43_set_phytxctl_defaults -- main.c:3206.
       * ctl = ENC_CCK | ANT01AUTO | TXH_PHY_TXPWR, and TXPWR is the WHOLE 0xFC00 field, i.e.
       * maximum. Written to BEACPHYCTL, ACKCTSPHYCTL and PRPHYCTL. The probe has been
       * masking the antenna into ACKCTS/PR via mgmtframe_txantenna without ever giving them
       * a base value, and BEACPHYCTL has never been touched at all. */
      Say("");
      { UInt16 ctl = (UInt16)(0x0000 | 0x00C0 | 0xFC00);   /* ENC_CCK | ANT01AUTO | TXPWR */
        UInt16 b0 = ShmRead16Shared(0x0054);
        UInt16 a0 = ShmRead16Shared(B43_SHM_SH_ACKCTSPHYCTL);
        UInt16 p0 = ShmRead16Shared(B43_SHM_SH_PRPHYCTL);
        ShmWrite16Shared(0x0054,ctl);
        ShmWrite16Shared(B43_SHM_SH_ACKCTSPHYCTL,ctl);
        ShmWrite16Shared(B43_SHM_SH_PRPHYCTL,ctl);
        SayH("  b43_set_phytxctl_defaults, ctl = ",(unsigned long)ctl,4);
        Say ("    ENC_CCK 0x0000 | ANT01AUTO 0x00C0 | TXH_PHY_TXPWR 0xFC00 (the whole field)");
        SayH("    BEACPHYCTL   0x0054  was ",(unsigned long)b0,4);
        SayH("                         now ",(unsigned long)ShmRead16Shared(0x0054),4);
        SayH("    ACKCTSPHYCTL 0x0022  was ",(unsigned long)a0,4);
        SayH("                         now ",(unsigned long)ShmRead16Shared(B43_SHM_SH_ACKCTSPHYCTL),4);
        SayH("    PRPHYCTL     0x0188  was ",(unsigned long)p0,4);
        SayH("                         now ",(unsigned long)ShmRead16Shared(B43_SHM_SH_PRPHYCTL),4); }

      /* stmts: MINCONT and MAXCONT, in SCRATCH. PHY is G, so MINCONT is 0xF, not the 0x1F a
       * B PHY gets. k23's SCRATCH dump measured word 3 = 0x001F -- the B-PHY value -- so the
       * initvals leave this wrong for a G PHY and no probe has corrected it. */
      Say("");
      ShmControl(2UL,0x0003); was = ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA);
      ShmControl(2UL,0x0003); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,0x000F);
      ShmControl(2UL,0x0003);
      SayH("  SC_MINCONT  word 3  was ",(unsigned long)was,4);
      SayH("                      now ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA),4);
      Say ("    ⚠ k23 measured 0x001F, which is the value b43 writes for a B PHY. A G PHY");
      Say ("      gets 0x000F. The initvals left the wrong one and nothing corrected it.");
      ShmControl(2UL,0x0004); was = ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA);
      ShmControl(2UL,0x0004); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,0x03FF);
      SayH("  SC_MAXCONT  word 4  was ",(unsigned long)was,4);

      /* stmts: PHYTYPE and PHYVER. The microcode is told what PHY it is driving. */
      Say("");
      { UInt16 t0 = ShmRead16Shared(0x0052), v0 = ShmRead16Shared(0x0050);
        ShmWrite16Shared(0x0052,2);                    /* B43_PHYTYPE_G = 2 */
        ShmWrite16Shared(0x0050,phyRev);
        SayH("  SH_PHYTYPE  0x0052  was ",(unsigned long)t0,4);
        SayH("                      now ",(unsigned long)ShmRead16Shared(0x0052),4);
        SayH("  SH_PHYVER   0x0050  was ",(unsigned long)v0,4);
        SayH("                      now ",(unsigned long)ShmRead16Shared(0x0050),4);
        Say ("    ★ These tell the FIRMWARE what hardware it is driving. Neither has ever");
        Say ("      been written by any probe in this project."); }
      Say("");
      (void)TxProbeHere("4b. after core_init's pre-dma_init block -- the missing statements");
    }

    /* ---------------- memory + rings (k2 regression) ---------------- */
    Say("");
    Say("=== [5-3a] RINGS (k2 regression) ===");
    if(!AllocOrReport("TX ring",&txRing,B43_DMA32_RINGMEMSIZE)) goto cleanup;
    if(!AllocOrReport("RX ring",&rxRing,B43_DMA32_RINGMEMSIZE)) goto cleanup;
    if(!AllocOrReport("RX buffer pool",&rxPool,(UInt32)K3_RX_SLOTS*K3_PAGE)) goto cleanup;
    /* two pages: the 106-byte txhdr on one, the frame on the other. Separate pages so each has
     * its own guaranteed-contiguous physical address, the same reason the RX buffers do. */
    /* page 0 txhdr, page 1 frame, pages 2..5 one TX ring each. A ring is exactly 4096 bytes,
     * which is one page, so every ring is page-aligned and contiguous by construction. */
    if(!AllocOrReport("TX buffers + 4 rings",&txPool,(2UL+(UInt32)K9_NTXC)*K3_PAGE)) goto cleanup;
    for(i=0;i<K3_RX_SLOTS;i++){
      rxBufLog[i]=rxPool.base+(UInt32)i*K3_PAGE; rxBufPhys[i]=0;
      if(PhysOfPage(rxBufLog[i],&rxBufPhys[i]) && (rxBufPhys[i]&0xFFFUL)==0 &&
         InWindow(rxBufPhys[i],rxBufSize)) nBufOk++; }
    SayH("  TX ring phys = ",txRing.phys,8);
    SayH("  RX ring phys = ",rxRing.phys,8);
    Say1("  RX buffer pages usable = ",(unsigned long)nBufOk);

    (void)DmaControllerTxReset(base);
    { UInt32 ae = B43DmaAddressExt(txRing.phys);
      UInt32 v  = B43_DMA32_TXENABLE | ((ae<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
      ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL,v);
      ssb_w32(gBus.bar0,base+B43_DMA32_TXRING,B43DmaAddressLow(txRing.phys)); }
    (void)RxRingArm(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize);
    { UInt32 txr = ssb_r32(gBus.bar0,base+B43_DMA32_TXRING);
      UInt32 rxr = ssb_r32(gBus.bar0,base+B43_DMA32_RXRING);
      UInt32 rxs = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
      SayH("  TXRING reads back = ",txr,8);
      SayH("  RXRING reads back = ",rxr,8);
      SayH("  RXSTATUS          = ",rxs,8);
      oracleA = (nBufOk==K3_RX_SLOTS) &&
                (txr==B43DmaAddressLow(txRing.phys)) &&
                (rxr==B43DmaAddressLow(rxRing.phys)) &&
                ((rxs & B43_DMA32_RXERROR)==0);
      SayOk("both rings programmed, no RX error",oracleA); }

    /* ---------------- core_init's tail ---------------- */
    Say("");
    Say("=== [5-3b] b43_wireless_core_init's REMAINING FIVE STATEMENTS ===");
    Say("  k2 stopped after b43_dma_init. These are what stands between there and a working");
    Say("  receiver, ported in order rather than guessed at.");
    Say("");

    Say("  [1] b43_qos_init -- the DISABLE arm (we do not use QoS; it is a driver setting)");
    { UInt16 lo,mi,hi,ifs;
      HfRead(&lo,&mi,&hi); SayH("      hostflags lo before = ",(unsigned long)lo,4);
      ifs=ssb_r16(gBus.bar0,B43_MMIO_IFSCTL); SayH("      IFSCTL before       = ",(unsigned long)ifs,4);
      QosInitDisabled();
      HfRead(&lo,&mi,&hi); SayH("      hostflags lo after  = ",(unsigned long)lo,4);
      ifs=ssb_r16(gBus.bar0,B43_MMIO_IFSCTL); SayH("      IFSCTL after        = ",(unsigned long)ifs,4);
      SayOk("HF_EDCF clear and IFSCTL USE_EDCF clear",
            ((lo&B43_HF_EDCF_LO)==0) && ((ifs&B43_MMIO_IFSCTL_USE_EDCF)==0)); }

    Say("  [2] b43_set_synth_pu_delay(dev, idle=1)");
    puDelay = SetSynthPuDelay();
    Say1("      SPUWKUP written  = ",(unsigned long)puDelay);
    Say1("      reads back       = ",(unsigned long)ShmRead16Shared(B43_SHM_SH_SPUWKUP));
    Say ("      (idle -> 500. The radio_rev == 8 arm raises it to 2400; this card is rev 2,");
    Say ("       so that arm is unreachable and was not ported.)");

    Say("  [3] b43_bluetooth_coext_enable");
    boardflags = (UInt32)ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SPROM_BFLLO);  /* as GpioInit reads it */
    SayH("      board flags      = ",boardflags,4);
    btDid = BluetoothCoextEnable(boardflags);
    Say (btDid ? "      BTCOEXIST set -> host flags updated"
               : "      BTCOEXIST clear -> b43 returns early, nothing written");

    Say("  [4] b43_upload_card_macaddress");
    /* The card's own MAC comes from SPROM. b43 gets it via the ssb SPROM parse; we read the
     * same words directly. il0mac lives at SPROM offset 0x4A on this generation. */
    { UInt16 w0,w1,w2;
      w0=ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC+0);
      w1=ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC+2);
      w2=ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SSB_SPROM1_IL0MAC+4);
      mac[0]=(UInt8)(w0>>8); mac[1]=(UInt8)(w0&0xFF);
      mac[2]=(UInt8)(w1>>8); mac[3]=(UInt8)(w1&0xFF);
      mac[4]=(UInt8)(w2>>8); mac[5]=(UInt8)(w2&0xFF);
      { Str255 L;L[0]=0;PCat(L,"      card MAC (SPROM) = ");PCatMac(L,mac);Out(L); }
      Say ("      ⚠ REPORTED, NOT SCORED. ssb writes each SPROM word big-endian into il0mac, so");
      Say ("        mac[0] is the high byte -- and a wrong MAC does not");
      Say ("        stop BROADCAST reception -- beacons arrive either way. If it looks wrong,");
      Say ("        that is a finding for the association increment, not for this one.");
      UploadCardMacAddress(mac); }

    Say("  [5] b43_security_init");
    SecurityInit(&ktp,&nKeys);
    SayH("      KTP (byte addr)  = ",ktp,4);
    Say1("      keys cleared     = ",(unsigned long)nKeys);
    Say ("      (58, not 54: fw.rev 295 < 351 so b43_new_kidx_api is FALSE and");
    Say ("       pairwise_keys_start is B43_NR_GROUP_KEYS*2 = 8.)");
    Say1("      RCMTA_COUNT      = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_RCMTA_COUNT));
    oracleD = (ShmRead16Shared(B43_SHM_SH_SPUWKUP)==puDelay) &&
              (ssb_r16(gBus.bar0,B43_MMIO_RCMTA_COUNT)==B43_NR_PAIRWISE_KEYS) &&
              (nKeys==58);
    SayOk("core_init's tail landed its values",oracleD);

    /* ================= [6] SHM SCRATCH -- REPORTED, NOT SCORED, NOT WRITTEN ==============
     * ★ THE ONE PIECE OF k20's TX EVIDENCE THAT SURVIVES THE RX DEFECT.
     * The directed probe request came back NOT acked with frame_count = 1. An 802.11 ACK is
     * detected by the MICROCODE, not by our DMA ring, so that result is unaffected by the
     * starved receiver -- it is real. But frame_count = 1 means the MAC transmitted once and
     * did not retry, and a frame that goes unacknowledged should be retried. b43 sets the
     * retry limits with b43_set_retry_limits() into SHM_SCRATCH, and no probe in this project
     * has ever written them: grep finds SHM_SCRATCH only in the stage-3b/4a zeroing loops.
     *
     * ⚠ I DO NOT HAVE b43's SOURCE ON DISK to confirm which two scratch words they are, and
     *   CLAUDE.md says to check rather than recall. So this DUMPS the first sixteen and
     *   asserts nothing. If the window is all zeros, zero retries is a real finding and the
     *   next increment ports b43_set_retry_limits against the actual header. If the firmware
     *   already seeded them, this costs one log block and closes the lead. */
    Say("");
    Say("  [6] SHM_SCRATCH words 0..15 -- b43 keeps the short/long retry limits here");
    { int w; Str255 L;
      for(w=0;w<16;w+=8){
        int q; L[0]=0; PCat(L,"      +"); PCatHex(L,(unsigned long)w,2); PCat(L,"  ");
        for(q=w;q<w+8;q++){
          ShmControl(2UL,(UInt32)q);                 /* SCRATCH: a word index, not a byte offset */
          PCatHex(L,(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA),4); PCat(L," "); }
        Out(L); } }

    /* ---------------- enable the MAC ---------------- */
    Say("");
    Say("=== [5-3c] b43_mac_enable ===");
    Say("  mac_suspended starts at 1 (setup_struct_wldev_for_init), so THIS call takes it to 0");
    Say("  and is the one that actually sets MACCTL_ENABLED.");
    macctlBefore = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    SayH("  MACCTL before = ",macctlBefore,8);
    Say1("  mac_suspended before = ",(unsigned long)gMacSuspended);
    MacEnable();
    macctlAfter = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    Say1("  mac_suspended after  = ",(unsigned long)gMacSuspended);
    SayH("  MACCTL after  = ",macctlAfter,8);
    SayH("    ENABLED (0x1)        = ",(unsigned long)(macctlAfter&B43_MACCTL_ENABLED),8);
    SayH("    AWAKE   (0x4000000)  = ",(unsigned long)(macctlAfter&B43_MACCTL_AWAKE),8);
    SayH("    HWPS    (0x2000000)  = ",(unsigned long)(macctlAfter&B43_MACCTL_HWPS),8);
    /* ⚠ b43_wireless_core_start writes this immediately after b43_mac_enable, and NO PROBE IN
     * THIS PROJECT HAS EVER WRITTEN IT. One register, in the reference's start path, skipped --
     * which is exactly the pattern that has cost this stage five runs. Whether it matters is
     * not the point; not having run it is. */
    ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_MASK,B43_IRQ_MASKTEMPLATE);
    (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_MASK);
    SayH("  GEN_IRQ_MASK written = ",B43_IRQ_MASKTEMPLATE,8);
    SayH("    reads back         = ",(unsigned long)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_MASK),8);
    Say ("    (B43_IRQ_MASKTEMPLATE with PHY_TXERR kept -- b43 clears it below debug verbosity,");
    Say ("     and we are debugging. b43_wireless_core_start does this after mac_enable.)");

    oracleE = ((macctlAfter&B43_MACCTL_ENABLED)!=0) &&
              ((macctlAfter&B43_MACCTL_AWAKE)!=0) &&
              ((macctlAfter&B43_MACCTL_HWPS)==0) &&
              ((macctlBefore&B43_MACCTL_ENABLED)==0);
    SayOk("MACCTL went from not-ENABLED to ENABLED|AWAKE, HWPS off",oracleE);
    (void)TxProbeHere("5. after core_init's tail and b43_mac_enable           ");

    /* ---------------- the sweep ---------------- */
    Say("");
    /* ★★★★★ THE EXPERIMENT: DOES THE RECEIVER NEED A TRANSMISSION FIRST?
     *
     * Four runs now -- k35, k37, k39 and k42, the last on a VERIFIED COLD BOOT -- have the same
     * shape: the scan hears nothing across six 2-second dwells, and the transmit section later
     * in the SAME RUN hears 32 frames in one second and catches a reply in 18 ms.
     *
     * k42's dead-dwell census eliminated everything configurable. On a dead dwell:
     *     RXCTL    0x0000003D   RXENABLE set, frameoffset 30, both correct
     *     RXRING   0x42026000   the ring, with its DMA translation
     *     RXINDEX  0x00000100   256, READS BACK EXACTLY AS WRITTEN
     *     RXSTATUS 0x00801000   ACTIVE
     *     MACCTL   0xC4120503   ENABLED | AWAKE | INFRA | DISCPMQ | BEACPROMISC
     *     SHM[0xA0] 0x0001      the firmware is on channel 1, which is the dwell's channel
     * and all fifteen Stage 4 oracles passed in that same run. Nothing we can read is wrong.
     *
     * Two hypotheses are already dead: k38's "the RXINDEX write does not stick" (it reads back)
     * and k42's "a previous run left state behind" (cold boot, TMSLOW 0x00000001 on entry).
     *
     * What is left is the one difference between the scan and the transmit section: the
     * transmit section has TRANSMITTED. dummy_transmission drives TXE0 and writes radio 0x51
     * (0x17 then 0x37), and on this radio family the transmit and receive paths share the
     * synthesiser. A receiver that only works after the TX path has been keyed would produce
     * exactly this and nothing else we have measured contradicts it.
     *
     * ⚠ THIS IS A PROBE, NOT A FIX. If the scan starts working, we have the cause and the real
     *   fix is to find which register dummy_transmission touches that the receive path needs --
     *   not to keep a spurious transmission in the bring-up path forever. If the scan still
     *   hears nothing, the hypothesis is dead too and the census on every dwell below gives us
     *   a healthy-versus-dead diff for the first time. */
    Say("");
    /* ★★★★★ k45: THE DUMMY TRANSMISSION IS GONE, DELIBERATELY AND BEFORE THE DIAGNOSIS.
     *
     * k43 and k44 both scanned cleanly with a dummy transmission in front of the scan, and it
     * was tempting to call that the fix. It is not a fix, it is a workaround, and it was
     * blocking the diagnosis: while it is in place every run looks healthy and the fault cannot
     * be measured. Two clean runs against a 23% failure rate is also just not evidence --
     * 0.77^2 = 0.59, so a broken build clears two runs more often than not.
     *
     * So it comes out, and the A/B below measures the real suspect instead. */
    Say("=== [5-3c2] ★★★ k45: THE A/B THAT SHOULD KILL THE INTERMITTENT ===");
    Say("");
    Say("  WHAT 22 BANKED LOGS ACTUALLY SAY, read before touching the card:");
    Say("    - The scan outcome is BINARY: 32/32/32 buffers or 0/0/0. Never partial, never in");
    Say("      between, and 32 is exactly the ring size, so the counter saturates.");
    Say("    - ⚠ THE RATE DEPENDS ON HOW THE RUN WAS STARTED, and pooling the two hid that:");
    Say("        WARM (k33a-k40, relaunched from the Finder): 3 dead scans in 9, plus k40 void");
    Say("          in Stage 4 and k33a wedged -- call it 40-55%, and it fails in EXTRA ways.");
    Say("        COLD (k42-k45, full power-down, detector-confirmed): 1 dead in 4 = 25%.");
    Say("      An earlier version of this text said a pooled '23%'. That averaged two different");
    Say("      populations. Both samples are small and neither rate is well determined.");
    Say("    - ⚠ COLD BOOTING IS NOT A WORKAROUND. k42 was a cold boot and its scan still died.");
    Say("      It removes a SECOND failure source, it does not remove this one.");
    Say("    - ⚠ IN EVERY DEAD RUN THE RECEIVER WORKED LATER IN THE SAME RUN. The transmit");
    Say("      section's control heard 32 frames/second every single time, with no reboot in");
    Say("      between. So the receiver is NOT intermittent. The SCAN is.");
    Say("    - Refuted for free from the logs: RFOVER/TRSW state (splits evenly across both");
    Say("      outcomes) and IFSSTAT bit 15 (flickers within every run). Also already dead:");
    Say("      the RXINDEX write (k39) and stale teardown state (k42, cold boot).");
    Say("");
    Say("  WHAT DIFFERS BETWEEN THE SCAN AND THE SECTION THAT ALWAYS WORKS -- a code fact,");
    Say("  not a theory: the scan calls RxRingArm once per channel, which RESETS the DMA");
    Say("  controller. The transmit section calls RxRecycle, which does not.");
    Say("");
    Say("  AND b43 NEVER DOES WHAT THE SCAN DOES. b43_op_config suspends the MAC, switches");
    Say("  channel and re-enables, touching no ring code at all; b43_dmacontroller_rx_reset is");
    Say("  reachable only from dma_init and teardown. b43 resets the RX engine TWICE in a");
    Say("  session. We have been doing it up to NINE times per run.");
    Say("");
    Say("  ⚠⚠ AND k45's OWN DESIGN WAS WRONG, WHICH IS WHY IT ANSWERED NOTHING. It ran five");
    Say("  A/B rounds and called that five trials. It is not: across 22 logs NO run is ever");
    Say("  mixed -- every dwell in a run agrees, including runs with nine of them. The fault is");
    Say("  per-RUN, so five rounds are about ONE trial, and 0.77^5 = 27% made 'neither failed'");
    Say("  the single most likely outcome. It duly happened. Proving a fix that way would cost");
    Say("  twelve consecutive clean reboots.");
    Say("");
    Say("  ★★★ SO STOP SAMPLING THE FAULT AND PROVOKE IT.");
    Say1("  Phase 1 hammers the ARM path ",(unsigned long)K46_HAMMER);
    Say1("  times; phase 2 hammers b43's RECYCLE path ",(unsigned long)K46_HAMMER);
    Say("  times. One channel, the busiest, so the only variable is the arm.");
    Say("");
    Say("  DISCRIMINATOR, fixed before the run -- and note that BOTH outcomes are answers:");
    Say("    any ARM dwell reads 0  -> ★ REPRODUCED ON DEMAND. The arm is a per-event dice");
    Say("                              roll, we can now make the fault happen whenever we like,");
    Say("                              and phase 2 says whether recycling is immune.");
    Say1("    zero of ",(unsigned long)K46_HAMMER);
    Say("      arms fail            -> ★ STRONG REFUTATION. If the wedge were a per-event dice");
    Say("                              roll at the 25% we measure per COLD run, P(0 in 40) =");
    Say("                              0.75^40 = 0.00001. The argument survives a much lower");
    Say("                              rate too: at 10% it is 0.0148, still under 2%. It only");
    Say("                              gets weak below about 5% (0.13), and nothing suggests");
    Say("                              a rate that low. So zero failures exonerates the arm and");
    Say("                              says the state is set earlier -- go look at bring-up.");
    Say("");
    Say("  ⚠ k13 SAW THE VIOLENT VERSION OF THIS AND IT TOOK TX DOWN TOO, so if the hammer");
    Say("    wedges the card the rest of this run may be void. That is expected and acceptable:");
    Say("    this experiment is the point of the run, not a passenger in it.");
    Say("");
    { int hit, n, firstAB, dead1=0, dead2=0; UInt32 rxsAB; Str255 L;
      L[0]=0; PCat(L,"    probe dwells:");
      for(hit=0; hit<K47_PROBE; hit++){
        n = DwellAndCount(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,
                          kChans[0],kFilters[1].bits,K46_DWELL_MS,&rxsAB,&firstAB,1);
        if(n==0) dead1++;
        PCat(L," "); PCatDec(L,(unsigned long)n); }
      Out(L);
      abDeadA = dead1; abDeadB = dead2;

      /* ★★★★★ THE RECOVERY BISECT. It runs ONLY when the fault is present, so a healthy run
       * pays nothing for it. k46 proved the state latches hard -- 86 consecutive dead dwells --
       * which means that once we are in it there is unlimited time to experiment INSIDE the
       * run. That turns a 25%-per-run lottery into a single decisive measurement.
       *
       * Each rung applies ONE action and then re-dwells. The first rung whose dwell reads
       * non-zero names the cause.
       *
       * ⚠ RUNG 0 IS A NO-OP THAT ONLY BURNS TIME, and it is there because without it every
       *   later rung is confounded: if the engine recovers on its own after N seconds, a rung
       *   that happens to run at N seconds would take the credit. */
      if(dead1 == K47_PROBE){
        int rung; UInt32 ae, v;
        Say("");
        Say("  ★★★ THE FAULT IS PRESENT. Running the recovery bisect.");
        Say("");
        Say("  k46 exonerated the ring arm: 40 arms and 40 recycles were equally dead, so this");
        Say("  is not a DMA-reset question. What k46 DID bracket is where recovery happens --");
        Say("  the transmit section, whose first five steps are identical to the arm path that");
        Say("  had just failed 40 times. The one thing it does that the hammer never did is");
        Say("  program a SECOND TX DMA controller.");
        Say("");
        Say("  ⚠ AND THAT IS A REAL PORTING OMISSION, found by reading b43 rather than guessing:");
        Say("    b43_dma_init sets up FIVE tx rings -- AC_BK, AC_BE, AC_VI, AC_VO and mcast, on");
        Say("    controllers 0..4 -- plus the rx ring, all before anything else runs. We program");
        Say("    controller 0 at [5-3a] and controller 1 only when the transmit section starts.");
        Say("    Controllers 2, 3 and 4 are NEVER programmed, so their ring pointers hold");
        Say("    whatever was left there. A microcode that touches an unprogrammed TX FIFO");
        Say("    would stall -- and a stalled microcode looks exactly like these dwells: every");
        Say("    register we check perfect, RXSTATE ACTIVE, RXDPTR frozen at 0.");
        Say("");
        Say("  ⚠ RUNGS 2-5 ARE A PROBE, NOT THE FIX. They point every controller at the SAME");
        Say("    ring, which is enough to answer 'does programming this controller revive the");
        Say("    receiver' but is NOT what b43 does -- b43 gives each ring its own memory. If a");
        Say("    TX rung wins, the fix is five properly allocated rings at [5-3a], not this.");
        Say("");
        for(rung=0; rung<K47_RUNGS; rung++){
          switch(rung){
            case 0:  SsbSpinUs(500000); break;                       /* no-op: time only */
            case 1:  gRxSlot = 0; break;                             /* software only */
            case 2: case 3: case 4: case 5: {                        /* TX ctrl 1,2,3,4 */
              UInt32 cb = B43_MMIO_DMA32_BASE0 + (UInt32)(rung-1)*0x20UL;
              (void)DmaControllerTxReset(cb);
              ae = B43DmaAddressExt(txRing.phys);
              v  = B43_DMA32_TXENABLE |
                   ((ae<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
              ssb_w32(gBus.bar0,cb+B43_DMA32_TXCTL,v);
              ssb_w32(gBus.bar0,cb+B43_DMA32_TXRING,B43DmaAddressLow(txRing.phys));
              } break;
            case 6:  (void)MacSuspend(); MacEnable(); break;
            case 7:  DummyTransmission(0); break;
            case 8:  (void)RxRingArm(base,&rxRing,rxBufLog,rxBufPhys,
                                     frameoffset,rxBufSize); break;
          }
          n = DwellAndCount(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,
                            kChans[0],kFilters[1].bits,K46_DWELL_MS,&rxsAB,&firstAB,0);
          L[0]=0; PCat(L,"    rung "); PCatDec(L,(unsigned long)rung);
          PCat(L,"  "); PCat(L,(char*)kRungName[rung]);
          PCat(L,"  -> heard "); PCatDec(L,(unsigned long)n);
          if(n>0 && bisectWinner<0){ bisectWinner = rung; PCat(L,"   ★★★ RECOVERED HERE"); }
          Out(L);
        }
      } else {
        Say("");
        Say("  The fault is NOT present in this run, so the bisect is skipped and this run");
        Say("  says nothing about the cause. ⚠ That is the 75% case on a cold boot and it is");
        Say("  not a failure -- but do not read it as a pass either.");
        Say("  ⭐ CHEAPEST WAY TO GET A BISECT: quit and relaunch from the Finder WITHOUT");
        Say("     rebooting. Warm runs fail far more often (k33a-k40: roughly half, against");
        Say("     25% cold) and a relaunch costs seconds instead of a reboot. Repeat until");
        Say("     this block says THE FAULT IS PRESENT.");
        Say("     ⚠ Caveat worth stating: it is NOT established that the warm fault and the");
        Say("       cold fault are the same thing. If the bisect fires on a warm run, treat");
        Say("       the answer as provisional until it repeats on a cold one.");
      } }
    Say("");

    Say("=== [5-3d] ★ THE SCAN -- channels 1, 6, 11 at BEACPROMISC ===");
    Say ("  k5's ladder settled the filter question: level 0 heard 0 buffers, level 1 heard 32.");
    Say ("  So BEACPROMISC is used here and the ladder is not repeated -- re-running a settled");
    Say ("  experiment costs a hardware cycle and answers nothing.");
    Say ("  ⚠ Level 1 and NOT level 2: PROMISC|KEEP_BAD|KEEP_BADPLCP would also admit frames");
    Say ("    with bad FCS and bad PLCP, so a decode failure could not be told from noise.");
    Say ("    Every frame below passed the hardware's own FCS check.");
    Say1("  dwell per channel, ms = ",(unsigned long)K6_DWELL_MS);
    Say("");
    for(i=0;i<K6_NCHAN;i++){
      UInt8 ch = kChans[i];
      int n,first,k; UInt32 rxs; Str255 L;

      n = DwellAndCount(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,
                        ch,kFilters[1].bits,K6_DWELL_MS,&rxs,&first,0);
      chanFrames[i]=n; chanFirst[i]=first; chanRxStat[i]=rxs;
      totalFrames += n;
      if(n>0 && bestCh<0){ bestCh=i; bestSlot=first; }

      /* Walk every filled buffer on this channel, not just the first. */
      { int parsed=0,beacons=0;
        for(k=0;k<K6_RX_SLOTS_USED;k++){
          UInt8 bssid[6],ssid[33],ssidLen,dsChan,jssi; UInt16 fc,flen; int r;
          if(RxBufferIsPoisoned(rxBufLog[k],frameoffset)) continue;
          r = ParseRxBuffer(rxBufLog[k],frameoffset,bssid,ssid,&ssidLen,&dsChan,&fc,&flen,&jssi);
          if(r==0) continue;
          parsed++;
          if(r==2){ beacons++; NetAdd(bssid,ssid,ssidLen,dsChan?dsChan:ch,jssi); } }
        L[0]=0;PCat(L,"    ch ");if(ch<10)PCat(L," ");PCatDec(L,(unsigned long)ch);
        PCat(L,": ");PCatDec(L,(unsigned long)n);PCat(L," buffers, ");
        PCatDec(L,(unsigned long)parsed);PCat(L," parsed, ");
        PCatDec(L,(unsigned long)beacons);PCat(L," beacons/probe-resp");Out(L); }
    }

    /* ★★★★★ THE SCAN RETRY, AND THE MEASUREMENT IT MAKES.
     *
     * Two of the last five runs were void on the same intermittent, and k35 pinned down its
     * shape. The receiver comes and goes, and it is NOT tied to a phase:
     *
     *     k33a   scan heard 32 buffers   |  transmit section dead
     *     k35    scan heard 0 buffers    |  transmit section ALIVE, RXDPTR moved
     *
     * So in k35 the receiver was dead for three 2-second dwells and working minutes later in
     * the same run, on the same channel, with nothing reprogrammed in between that had not
     * already been programmed the same way. That is not a configuration error -- a wrong filter
     * or a wrong channel would not fix itself.
     *
     * The guards handled it correctly: zero networks meant no BSSID, so authentication refused
     * to run rather than guess an address. But a void run still costs a reboot, and "re-run it"
     * is not a fix when it is happening one time in three.
     *
     * ⚠ THIS IS A MEASUREMENT AS MUCH AS A MITIGATION, and that is why it is worth the code.
     *   If the retry SUCCEEDS the receiver merely needed more time after bring-up, and the real
     *   fix is a settle delay in the right place. If the retry ALSO finds nothing, time is not
     *   the variable and the fault is in the bring-up itself -- a much more interesting answer.
     *   Either way the next run tells us which, instead of costing a reboot to learn nothing. */
    if(totalFrames == 0){
      Say("");
      Say("  ⚠⚠ THE SCAN HEARD NOTHING ON ANY CHANNEL. Retrying once after a 2 s settle.");
      Say("     k35 hit exactly this and then received normally in the transmit section, so the");
      Say("     receiver was dead for three dwells and alive minutes later in the same run.");
      Say("     What the retry answers: succeed -> the receiver needed more time after bring-up");
      Say("     and a settle delay is the real fix; fail -> time is not the variable and the");
      Say("     fault is in the bring-up, which is the more interesting outcome.");
      SsbSpinUs(2000000);
      scanRetried = 1;
      for(i=0;i<K6_NCHAN;i++){
        UInt8 ch = kChans[i];
        int n,first,k; UInt32 rxs; Str255 L;
        n = DwellAndCount(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,
                          ch,kFilters[1].bits,K6_DWELL_MS,&rxs,&first,0);
        chanFrames[i]=n; chanFirst[i]=first; chanRxStat[i]=rxs;
        totalFrames += n;
        if(n>0 && bestCh<0){ bestCh=i; bestSlot=first; }
        { int parsed=0,beacons=0;
          for(k=0;k<K6_RX_SLOTS_USED;k++){
            UInt8 bssid[6],ssid[33],ssidLen,dsChan,jssi; UInt16 fc,flen; int r;
            if(RxBufferIsPoisoned(rxBufLog[k],frameoffset)) continue;
            r = ParseRxBuffer(rxBufLog[k],frameoffset,bssid,ssid,&ssidLen,&dsChan,&fc,&flen,&jssi);
            if(r==0) continue;
            parsed++;
            if(r==2){ beacons++; NetAdd(bssid,ssid,ssidLen,dsChan?dsChan:ch,jssi); } }
          L[0]=0;PCat(L,"    RETRY ch ");if(ch<10)PCat(L," ");PCatDec(L,(unsigned long)ch);
          PCat(L,": ");PCatDec(L,(unsigned long)n);PCat(L," buffers, ");
          PCatDec(L,(unsigned long)parsed);PCat(L," parsed, ");
          PCatDec(L,(unsigned long)beacons);PCat(L," beacons/probe-resp");Out(L); } }
      scanRetryWorked = (totalFrames > 0);
      Say("");
      if(scanRetryWorked){
        Say("  ★ THE RETRY WORKED. The receiver needed more time after bring-up than the scan");
        Say("    gave it. The fix is a settle delay before the first dwell, not a retry loop --");
        Say("    a retry is a workaround and this now has the evidence to replace it properly.");
      } else {
        Say("  ⇒ THE RETRY ALSO HEARD NOTHING, so time is not the variable. Six 2-second dwells");
        Say("    across three channels with a settle between them, and silence. Whatever is");
        Say("    wrong is in the bring-up, and the transmit section reviving later in the same");
        Say("    run says it is recoverable -- find what the transmit section does that the scan");
        Say("    does not."); } }

    /* ---------------- the hex dump: settle the layout by measurement ---------------- */
    Say("");
    Say("=== [5-3d2] ★ RAW BYTES OF ONE RECEIVED BUFFER ===");
    Say("  k5 decoded a frame and got addr2 = FF:FF:FF:FF:FF:FF, which is impossible, and its");
    Say("  Oracle G passed it anyway. So this run does not ask to be believed: here are the");
    Say("  bytes. Everything the decoder claims below can be checked against them by hand.");
    Say("");
    if(bestCh>=0 && bestSlot>=0){
      Say1("  channel ",(unsigned long)kChans[bestCh]);
      Say1("  slot    ",(unsigned long)bestSlot);
      Say ("");
      HexDump(rxBufLog[bestSlot],80,frameoffset);
      Say ("");
      Say ("  How to read it: bytes 0..29 are b43's rx header (frame_len at +0, JSSI at +6).");
      Say ("  At +30 the PLCP header starts -- its first byte should be 0x0A, 0x14, 0x37 or");
      Say ("  0x6E. At +36 the 802.11 header starts: frame control, duration, then addr1.");
      Say ("  A beacon shows 0x80 0x00 at +36 and FF FF FF FF FF FF at +40.");
    } else Say("  (no frame captured, nothing to dump)");

    /* ---------------- decode, with cross-validation ---------------- */
    Say("");
    Say("=== [5-3e] THE FIRST FRAME, DECODED ===");
    if(bestCh>=0 && bestSlot>=0){
      const UInt8 *buf = rxBufLog[bestSlot];
      const UInt8 *plcp = buf + frameoffset;
      const UInt8 *f = plcp + K6_HDR_PLCP6;
      UInt8 bssid[6],ssid[33],ssidLen,dsChan,jssi; UInt16 fc,flen;
      int r = ParseRxBuffer(buf,frameoffset,bssid,ssid,&ssidLen,&dsChan,&fc,&flen,&jssi);
      UInt32 type=((UInt32)fc>>2)&3, sub=((UInt32)fc>>4)&0xF;
      int rateOk,typeOk,srcOk,beaconShape,ssidOk;

      Say1("  frame_len (incl. PLCP) = ",(unsigned long)flen);
      Say1("  802.11 body length     = ",(unsigned long)(flen-K6_HDR_PLCP6));
      Say1("  JSSI                   = ",(unsigned long)jssi);
      SayH("  PLCP SIGNAL            = ",(unsigned long)plcp[0],2);
      { Str255 L;L[0]=0;PCat(L,"    -> ");PCat(L,PlcpRateName(plcp[0]));Out(L); }
      SayH("  frame control          = ",(unsigned long)fc,4);
      Say1("    type                 = ",type);
      Say1("    subtype              = ",sub);
      if(type==0&&sub==8)      Say("    ⇒ BEACON");
      else if(type==0&&sub==5) Say("    ⇒ PROBE RESPONSE");
      else if(type==0)         Say("    ⇒ management frame");
      else if(type==1)         Say("    ⇒ control frame");
      else if(type==2)         Say("    ⇒ data frame");
      Say1("  duration               = ",(unsigned long)le16at(f+2));
      { Str255 L;
        L[0]=0;PCat(L,"  addr1 (dest)  = ");PCatMac(L,f+4);Out(L);
        L[0]=0;PCat(L,"  addr2 (src)   = ");PCatMac(L,f+10);Out(L);
        L[0]=0;PCat(L,"  addr3 (bssid) = ");PCatMac(L,f+16);Out(L); }
      if(ssidLen){ Str255 L;int k;L[0]=0;PCat(L,"  SSID          = \"");
        for(k=0;k<ssidLen;k++){char c[2];c[0]=(char)((ssid[k]>=32&&ssid[k]<127)?ssid[k]:'.');c[1]=0;PCat(L,c);}
        PCat(L,"\"");Out(L); }
      if(dsChan) Say1("  DS param channel = ",(unsigned long)dsChan);

      /* ⚠ ORACLE G IS NOW A CONJUNCTION OF INDEPENDENT FIELDS, and that is the whole point.
       * k5's Oracle G tested FCSERR and a length bound, and passed on a decode whose source
       * address was the broadcast address. Garbage can satisfy one field; it cannot satisfy
       * five that have to agree with each other. */
      rateOk      = PlcpRateIsLegal(plcp[0]);
      typeOk      = (type != 3);                               /* 3 is reserved */
      srcOk       = !(f[10]==0xFF&&f[11]==0xFF&&f[12]==0xFF&&
                      f[13]==0xFF&&f[14]==0xFF&&f[15]==0xFF);  /* a source is never broadcast */
      beaconShape = !(type==0&&sub==8) ||
                    ((f[4]==0xFF&&f[5]==0xFF&&f[6]==0xFF&&f[7]==0xFF&&f[8]==0xFF&&f[9]==0xFF)
                     && le16at(f+2)==0);                       /* beacon: addr1 bcast, duration 0 */
      /* ⚠ k21 FAILED THIS ON A PERFECTLY VALID FRAME. Slot 0 happened to hold a beacon from
       * a HIDDEN network, and a hidden network's beacon carries an SSID
       * element of LENGTH ZERO -- that is how hiding works. The frame was well formed and
       * the oracle was wrong; k20 only passed because slot 0 happened to hold the guest network.
       * An oracle that depends on which beacon lands in slot 0 is not an oracle.
       *
       * The replacement is a field that IS mandatory and cannot be zero-length: the
       * capability word in a beacon's fixed parameters must have exactly one of ESS and IBSS
       * set. It is independent of the other four and garbage satisfies it only by chance. */
      ssidOk      = !(type==0&&sub==8) ||
                    ((le16at(f+34)&0x0003)==0x0001 || (le16at(f+34)&0x0003)==0x0002);
      Say("");
      SayOk("PLCP SIGNAL is a legal 802.11b rate",rateOk);
      SayOk("frame type is not the reserved value 3",typeOk);
      SayOk("source address is not broadcast (k5 failed exactly this)",srcOk);
      SayOk("if a beacon: addr1 is broadcast AND duration is 0",beaconShape);
      SayOk("if a beacon: capability has exactly one of ESS/IBSS (a hidden SSID is legal)",ssidOk);
      Say1("      ParseRxBuffer returned = ",(unsigned long)r);
      Say ("        0 = rejected outright, 1 = decoded but not a beacon/probe-response,");
      Say ("        2 = beacon or probe response, IEs walked");
      if(r==2 && ssidLen==0)
        Say("      (this frame's SSID element is zero-length -- a HIDDEN network, and legal)");
      oracleG = rateOk && typeOk && srcOk && beaconShape && ssidOk;
      oracleF = (flen != 0) && (totalFrames > 0);
    } else {
      Say("  ✗ NO FRAME CAPTURED on any of channels 1, 6, 11.");
      Say("    k5 caught 32 buffers on every one of thirteen channels, so this would be a");
      Say("    REGRESSION rather than a new unknown. Suspect the filter level or the dwell.");
    }

    Say("");
    Say("  per channel:");
    { int k2; for(k2=0;k2<K6_NCHAN;k2++){
        Str255 L;L[0]=0;PCat(L,"    ch ");
        if(kChans[k2]<10) PCat(L," ");
        PCatDec(L,(unsigned long)kChans[k2]);
        PCat(L,": ");PCatDec(L,(unsigned long)chanFrames[k2]);PCat(L," buffers");
        PCat(L,", first slot ");
        if(chanFirst[k2]<0) PCat(L,"none"); else PCatDec(L,(unsigned long)chanFirst[k2]);
        PCat(L,", RXSTATUS 0x");PCatHex(L,chanRxStat[k2],8);
        if((chanRxStat[k2]&B43_DMA32_RXERROR)!=0) PCat(L,"  ⚠ RXERROR");
        Out(L); } }

    /* ---------------- the networks we heard ---------------- */
    Say("");
    Say("=== [5-3e2] ★★★ NETWORKS HEARD FROM MAC OS 9 ===");
    Say1("  distinct BSSIDs seen = ",(unsigned long)gNNets);
    Say("");
    if(gNNets==0) Say("  (none decoded)");
    else { int k2;
      Say("    BSSID              ch  JSSI  SSID");
      Say("    -----------------  --  ----  --------------------------------");
      for(k2=0;k2<gNNets;k2++){
        Str255 L;int q;
        L[0]=0;PCat(L,"    ");PCatMac(L,gNets[k2].bssid);PCat(L,"  ");
        if(gNets[k2].chan<10) PCat(L," ");
        PCatDec(L,(unsigned long)gNets[k2].chan);PCat(L,"  ");
        if(gNets[k2].jssi<100) PCat(L," ");
        if(gNets[k2].jssi<10)  PCat(L," ");
        PCatDec(L,(unsigned long)gNets[k2].jssi);PCat(L,"  ");
        if(gNets[k2].ssidLen==0) PCat(L,"<hidden or not captured>");
        else for(q=0;q<gNets[k2].ssidLen;q++){
          char c[2];c[0]=(char)((gNets[k2].ssid[q]>=32&&gNets[k2].ssid[q]<127)?gNets[k2].ssid[q]:'.');c[1]=0;
          PCat(L,c); }
        Out(L); } }


    /* ---------------- find the target by SSID ---------------- */
    Say("");
    Say("=== [5-3e3] LOOKING UP \"" K10_TARGET_SSID "\" IN WHAT WE HEARD ===");
    Say("  ⚠ k10 RAN THIS BLOCK 130 LINES TOO LATE -- after the transmit section had already");
    Say("    read targetFound and fallen back. The lookup itself worked and printed");
    Say("    \"★ FOUND: 02:00:5E:00:53:0A on channel 1\"; it just printed it afterwards.");
    Say("  The network was created minutes ago and its BSSID and channel were unknown when");
    Say("  this probe was written. Rather than hardcode a guess, the scan above is searched");
    Say("  by SSID -- the same table it already builds.");
    { int k2;
      for(k2=0;k2<gNNets;k2++){
        if(gNets[k2].ssidLen==K10_TARGET_LEN){
          int q,same=1;
          for(q=0;q<K10_TARGET_LEN;q++)
            if(gNets[k2].ssid[q]!=(UInt8)K10_TARGET_SSID[q]){ same=0; break; }
          if(same){
            for(q=0;q<6;q++) targetBssid[q]=gNets[k2].bssid[q];
            targetChan = gNets[k2].chan ? gNets[k2].chan : 1;
            targetFound = 1;
            break; } } } }
    if(targetFound){
      Str255 L;L[0]=0;PCat(L,"  ★ FOUND: ");PCatMac(L,targetBssid);
      PCat(L,"  on channel ");PCatDec(L,(unsigned long)targetChan);Out(L);
    } else {
      Say("  ✗ not among the networks heard. See the table above for what was.");
    }

    /* ================= [5-4] ★ TRANSMIT ================= */
    Say("");
    Say("=== [5-4] ★ TRANSMIT -- A PROBE REQUEST, AND THE REPLY ===");
    Say("  Stage 4 already drove a frame out of the TX engine with b43_dummy_transmission, but");
    Say("  that used the template RAM path. This is the first frame to leave through the DMA");
    Say("  RING, with a real b43_txhdr in front of it.");
    Say("");
    Say("  ⚠ THE ORACLE IS THE REPLY, NOT THE SEND. A TX status register saying `sent` only");
    Say("    proves the MAC consumed our descriptors. A PROBE RESPONSE addressed to OUR MAC");
    Say("    proves an access point received the frame, parsed it, and answered it -- which is");
    Say("    the only evidence that what we put on the air was actually 802.11.");
    Say("");
    Say("  ★ k9 SETTLED WHICH CONTROLLER. Four were tried with the same frame:");
    Say("       controller 0  AC_BK   TXDPTR 0x0000  did not move");
    Say("       controller 1  AC_BE   TXDPTR 0x0010  ★ ADVANCED   <- b43_dma.c:1331");
    Say("       controller 2  AC_VI   TXDPTR 0x0000  did not move");
    Say("       controller 3  AC_VO   TXDPTR 0x0010  ★ ADVANCED");
    Say("    TXDPTR 0x0010 is 16 = both descriptors consumed. Controller 1 it is, and every");
    Say("    probe from k2 to k8 had been posting to controller 0, where b43 never sends.");
    Say("");
    Say("  ⚠⚠ BUT k9's RX WENT SILENT DURING THE TX PASSES -- 0 buffers on all four, where k8");
    Say("     saw 10 and 30 with the same filter, channel and dwell. THAT MAKES \"no probe");
    Say("     response\" WORTHLESS AS EVIDENCE: a dead receiver cannot hear a reply either.");
    Say("     Oracle I had no positive control, which is the defect. k10 adds one: every TX");
    Say("     pass counts the frames it hears, and the reply is only scored if the control");
    Say("     passed. RXSTATUS is sampled before and after, because k9 logged neither and I");
    Say("     could not diagnose it from the log.");
    Say("");
    if(targetFound){
      Str255 L;
      L[0]=0;PCat(L,"  ★ TARGET FOUND BY THE SCAN: \"");PCat(L,K10_TARGET_SSID);PCat(L,"\"");Out(L);
      L[0]=0;PCat(L,"      BSSID   ");PCatMac(L,targetBssid);Out(L);
      Say1("      channel ",(unsigned long)targetChan);
      Say ("    ⚠ NOT HARDCODED. The new network's BSSID and channel were unknown when this was");
      Say ("      written, so the scan discovers them by SSID and the transmit uses whatever it");
      Say ("      found. If the AP is retuned, this probe follows it.");
    } else {
      Say("  ✗ \"" K10_TARGET_SSID "\" WAS NOT FOUND BY THE SCAN.");
      Say("    The networks that WERE heard are listed above -- if the target is not among them");
      Say("    it is either on a channel outside 1/6/11 or not broadcasting. Falling back to");
      Say("    the k6 target so the TX path is still exercised.");
    }
    Say("");
    { UInt8 *txh  = txPool.base;
      UInt8 *frm  = txPool.base + K3_PAGE;
      UInt8 *ringLog = txPool.base + 2UL*K3_PAGE;
      UInt32 txhPhys=0, frmPhys=0, ringPhys=0;
      UInt16 frameLen;
      static const UInt8 kFallbackBssid[6] = {0x9A,0x41,0xB2,0xCC,0x58,0xAD};
      static const UInt8 kBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      /* ⚠ THE ANTENNA IS ELIMINATED -- k14's ladder was valid (live RX control on all three
       * passes, seq 1/2/3) and ANT0, ANT1 and ANT01AUTO behaved identically. So every pass
       * here uses b43's own default and the LADDER VARIES THE FRAME INSTEAD. */
      static const char *kWhich[K17_NPASS] = {
        "BROADCAST wildcard probe request, ACK bit CLEAR",
        "DIRECTED probe request to the target, ACK bit set" };
      const UInt8 *dst = targetFound ? targetBssid : kFallbackBssid;
      UInt8 ch = targetFound ? targetChan : 1;
      UInt32 cbase = B43_MMIO_DMA32_BASE0 + (UInt32)K10_TXC*0x20UL;
      int pass;
      /* ★★★★★ k22's LADDER: THE PA GAIN THE CODE ASKED FOR NEVER REACHED THE RADIO.
       *
       * k21 was the first run whose RX control was valid, and with a receiver PROVED live
       * the directed frame still drew no reply and no ACK. So the frame really is not being
       * understood on air. This is why, and it was measured in Stage 4 all along:
       *
       *   default_tx_control() returns = 0x0020      <- B43_TXCTL_PA2DB, the PA 2 dB stage
       *   gphy->tx_control (u8) becomes = 0x00       <- our `<< 4` overflowed a u8
       *   radio 0x52 = 0x0000                        <- MEASURED. bits 4-5 are zero.
       *
       * The `<< 4` is spurious, and the register layout proves it without needing b43's
       * source. lo_txctl_register_table gives reg 0x52 the mask 0x30 on a G-PHY with
       * radio_rev != 8, and the three TXCTL constants are TXMIX 0x10, PA2DB 0x20, PA3DB
       * 0x40 -- bits 4, 5 and 6, already in position. 0x20 belongs in bit 5 UNSHIFTED;
       * 0x20 << 4 = 0x200 is outside the mask entirely, so no shift can be correct.
       *
       * Net effect: every frame this project has ever transmitted went out with the PA at
       * minimum drive. That fits every surviving fact -- TSSI moves but only to 0x0C, the
       * MAC reports TX_OK, the microcode never suppresses, all three antennas behave
       * identically, and nothing on air answers either a broadcast or a directed frame.
       *
       * ⚠ THE FIX GOES HERE, NOT IN ap_phy_initg.h. That file is a mechanically extracted,
       *   diff-verified copy of Stage 4 and its value is that it still reproduces Stage 4
       *   exactly. Overriding radio 0x52 here is surgical and reversible, and if the ladder
       *   proves the point, the correction moves into Stage 4 as its own increment.
       *
       * THE DISCRIMINATOR, BEFORE THE RUN:
       *   TSSI climbs with the rung        -> the PA gain bits are real and we are now
       *                                       measuring output power rather than a sentinel.
       *   a reply or an ACK at any rung    -> ★ TRANSMIT IS SOLVED and the root cause is the
       *     above 0x00                        spurious shift. Stage 5 completes.
       *   nothing changes at any rung      -> the PA is not the variable after all; rung 0
       *                                       reproduces k21 exactly, so the ladder is its
       *                                       own control and that null is trustworthy. */
      /* k22's ladder was FLAT: radio 0x52 took 0x00/0x10/0x20/0x30, verified by readback, and
       * TSSI, the ACK and the replies were identical at every rung. PA gain is eliminated.
       *
       * ★★★★★ BUT THE TSSI VALUE ITSELF IS THE FINDING, AND IT RETRACTS k15's ORACLE.
       * Every pass in k22 read TSSI CCK = 0x7F00 / 0x7F7F. ShmClearTssi writes 0x7F to all
       * four bytes as "the microcode has not written this slot", so that decodes as: ONE
       * sample taken, with the value ZERO, and three slots never touched. k15 onward scored
       * "the value moved off the sentinel" and printed "the PHY produced and measured real
       * transmit power". A sentinel replaced by ZERO is the ABSENCE of measured power. That
       * oracle has been propping up "the analog chain works" for eight probes and it is
       * withdrawn: tested that a number changed, not that the value was sane.
       *
       * So the story is simpler than anything we have had. PHY_TXERR fires on 100% of
       * transmissions -- ours and dummy_transmission's, 8 of 8 in k22 -- the power detector
       * reads zero, and nothing on air hears us at any gain. No usable RF is leaving.
       *
       * k23 STOPS POKING AND TAKES A CENSUS, with one experiment attached.
       *
       * The experiment is an ATTENUATION ladder, and rung 2 is the point of it:
       * b43_phy_init_pctl measured cur_idle_tssi by setting bbatt 11 / rfatt 9 and firing
       * dummy_transmission, and B43_PHY_ITSSI read 0x28. That is a KNOWN-GOOD READING from
       * this very card, this run, that register. Rung 2 reproduces those exact attenuations.
       *
       *   ITSSI ~0x28 at rung 2, ~0 at rungs 0/1 -> the calibration is reproducible and the
       *                                             fault is in the attenuation state we
       *                                             transmit with. Actionable immediately.
       *   ITSSI ~0 at ALL THREE                   -> the PA is off regardless of attenuation,
       *                                             init_pctl's 0x28 came from a machine in a
       *                                             different state, and the next probe goes
       *                                             at the TR switch and the radio TX path.
       *
       * ⚠ ITSSI IS READ DIRECTLY, NOT THROUGH SHM. init_pctl's 0x28 came from
       *   PhyRead(B43_PHY_ITSSI); the per-frame SHM bytes are a different register written by
       *   the microcode. Comparing those two would not have been a comparison at all. */
      /* ⚠⚠ k23 WEDGED THE CARD AND THEN DREW A CONCLUSION FROM THE WRECKAGE.
       *
       * k23 wrote these same attenuation registers with the MAC LIVE. The card stopped dead
       * before rung 0's first frame: TXDPTR 0x0000 and never moved, XMITSTAT_0 = 0, no TX
       * status in 600 ms, RXDPTR frozen at slot 1, all four TSSI slots still sentinel. NOTHING
       * TRANSMITTED IN THE ENTIRE RUN -- and the verdict still printed "THE LADDER DID NOTHING
       * ... Transmit POWER is eliminated", because it was never gated on whether a frame went
       * out. That verdict is retracted. It is the same defect as k19's vacuous bisect and
       * k20's stale control: a conclusion drawn from a pass that never ran.
       *
       * I flagged the risk in k23's own comment and then did not act on it. b43 is explicit,
       * phy_g.c:2825 in b43_gphy_op_adjust_txpower:
       *     b43_mac_suspend(dev);
       *     ... compute ...
       *     b43_phy_lock(dev); b43_radio_lock(dev);
       *     b43_set_txpower_g(dev, &gphy->bbatt, &gphy->rfatt, gphy->tx_control);
       *     b43_radio_unlock(dev); b43_phy_unlock(dev);
       *     b43_mac_enable(dev);
       * b43_radio_lock is MACCTL |= 0x00080000, a flush read, then udelay(10) -- it waits for
       * the FIRMWARE to finish any radio access. Writing radio 0x43 underneath a live
       * microcode is what wedged us. k24 does the full sequence.
       *
       * ★ AND RUNG 1 IS NOW A REAL b43 STATE, NOT ONE I INVENTED. phy_g.c:2835 has a fixup
       * for radio_ver 0x2050 / radio_rev 2 -- exactly this card -- which fires when rfatt <= 1:
       *     if (tx_control == 0) { tx_control = B43_TXCTL_PA2DB | B43_TXCTL_TXMIX;
       *                            rfatt += 2; bbatt += 2; }
       * so b43 never transmits at rfatt 0. It lands on bbatt 2 / rfatt 2 / tx_control 0x30.
       * k23's "maximum power" rung was a state the driver cannot produce. */
      /* The ladder's tables and bookkeeping are gone with it. If attenuation is ever worth
       * testing again it comes back as its own probe, where a wedge costs only itself. */

      if(!PhysOfPage(txh,&txhPhys) || !PhysOfPage(frm,&frmPhys) || !PhysOfPage(ringLog,&ringPhys)){
        Say("  !! could not resolve the TX buffers"); goto tx_done; }

      frameLen = BuildProbeRequest(frm,mac,dst,K10_TARGET_SSID,
                                   targetFound?K10_TARGET_LEN:13);
      if(!targetFound) frameLen = BuildProbeRequest(frm,mac,dst,"HomeNet-Guest",13);
      /* the header is rebuilt per pass because the antenna lives in it */
      SayH("  controller  = 0x",cbase,3);
      Say1("  channel     = ",(unsigned long)ch);
      Say1("  frame bytes = ",(unsigned long)frameLen);

      /* ★ ARMED ONCE. Everything after this recycles. */
      (void)MacSuspend();
      (void)RxRingArm(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize);
      SwitchChannel(ch);
      (void)ApplyFilter(kFilters[1].bits);
      MacEnable();
      gRxSlot = 0;
      Say("  RX ring armed ONCE for the whole ladder; passes recycle rather than reset.");
      Say("");

      /* ================= ★ THE KNOWN-GOOD POSITIVE CONTROL ================= */
      Say("  === [5-4b] ★ POSITIVE CONTROL -- b43_dummy_transmission ===");
      Say("    CLAUDE.md, Hardware-test discipline, first line: \"Run the known-good control");
      Say("    first when a regression appears.\" I did not, for six probes. Stage 4 PROVED this");
      Say("    card transmits -- dummy_transmission drove the TX engine and the state machine");
      Say("    went 0x0001 -> 0x00C1 -> 0x0401 -- and it is a DIFFERENT path: template RAM, not");
      Say("    the DMA ring, no txhdr of ours anywhere in it.");
      Say("");
      Say("    ⚠ AND STAGE 4 NEVER CHECKED GEN_IRQ_REASON. Its oracle was the TXSTAT bits;");
      Say("      PHY_TXERR was not decoded anywhere until k12. So \"the card transmits\" meant");
      Say("      the state machine moved, NOT that valid RF left the antenna.");
      Say("");
      Say("      dummy clean, ours errors -> the fault is OUR frame or txhdr, and the two paths");
      Say("                                  are directly comparable in this one run.");
      Say("      dummy errors too         -> the PHY TX path is broken generally. That makes");
      Say("                                  this a STAGE 4 regression surfacing in Stage 5, and");
      Say("                                  six probes of Stage 5 debugging were in the wrong");
      Say("                                  place.");
      Say("");
      { UInt32 irD; UInt16 d0,d1;
        ShmClearTssi();
        ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
        (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);

        DummyTransmission(0);                    /* radio rev 2 -> the rev >= 2 variant */

        SsbSpinUs(200000);
        irD = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
        d0  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK);
        d1  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK+2);
        dummyIrq = irD;
        Say("");
        SayH("    GEN_IRQ_REASON after dummy_transmission = ",irD,8);
        if(irD & 0x20000000UL) Say("      0x20000000 B43_IRQ_TX_OK");
        if(irD & 0x00008000UL) Say("      0x00008000 B43_IRQ_DMA");
        if(irD & 0x00000800UL) Say("      0x00000800 B43_IRQ_PHY_TXERR ** PHY TRANSMIT ERROR **");
        if(irD & 0x00000200UL) Say("      0x00000200 B43_IRQ_MAC_TXERR");
        SayH("    TSSI CCK after dummy = ",(unsigned long)d0,4);
        SayH("                        + ",(unsigned long)d1,4);
        if(irD & 0x00000800UL){
          dummyPhyTxErr = 1;
          Say("");
          Say("    ⚠⚠ THE KNOWN-GOOD PATH ERRORS TOO. dummy_transmission uses template RAM and");
          Say("       none of our DMA work, so the fault is NOT in the ring, the descriptors,");
          Say("       the txhdr or the frame -- all of which are verified correct anyway. The");
          Say("       PHY TX path itself is faulty, and Stage 4 did not catch it because it");
          Say("       never looked at this register.");
        } else {
          Say("");
          Say("    ★ THE KNOWN-GOOD PATH IS CLEAN. dummy_transmission transmits without a PHY");
          Say("      error, so the PHY is fine and something in OUR frame or txhdr provokes it.");
          Say("      The two paths differ in exactly one way that matters: ours goes through the");
          Say("      DMA ring with a b43_txhdr in front of it."); }
        if(d0!=0x7F7F || d1!=0x7F7F) Say("    (dummy also moved TSSI, so it radiated too.)");
        else Say("    ⚠ dummy did NOT move TSSI -- it did not radiate, unlike our frames."); }
      Say("");

      /* ================= ★ THE NEGATIVE CONTROL ================= */
      Say("  === [5-4a] ★ NEGATIVE CONTROL -- ONE SECOND, NO TRANSMIT ===");
      Say("    k12 through k15 read B43_IRQ_PHY_TXERR after every transmit and treated it as a");
      Say("    finding. It was never checked for ATTRIBUTION. Clearing the reason register and");
      Say("    reading it back after a transmit proves the bit was raised DURING the transmit");
      Say("    window -- not BY the transmission. A background PHY condition, a periodic");
      Say("    calibration, or something provoked by receiving 32 beacons would look identical.");
      Say("");
      Say("    So: exactly the same dwell, exactly the same clears, and NO FRAME POSTED.");
      Say("");
      { UInt32 irQ; UInt16 q0,q1,qg;
        ShmClearTssi();
        ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
        (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
        (void)RxRecycle(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,0,0);

        SsbSpinUs(1000000);                       /* the same second, transmitting nothing */

        irQ = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
        q0  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK);
        q1  = ShmRead16Shared(B43_SHM_SH_TSSI_CCK+2);
        qg  = ShmRead16Shared(B43_SHM_SH_TSSI_OFDM_G);
        quietIrq = irQ;
        SayH("    GEN_IRQ_REASON after a QUIET second = ",irQ,8);
        if(irQ & 0x20000000UL) Say("      0x20000000 B43_IRQ_TX_OK");
        if(irQ & 0x00008000UL) Say("      0x00008000 B43_IRQ_DMA");
        if(irQ & 0x00000800UL) Say("      0x00000800 B43_IRQ_PHY_TXERR");
        if(irQ & 0x00000200UL) Say("      0x00000200 B43_IRQ_MAC_TXERR");
        if(irQ & 0x00000100UL) Say("      0x00000100 B43_IRQ_PIO_WORKAROUND");
        SayH("    TSSI CCK after a QUIET second = ",(unsigned long)q0,4);
        SayH("                                 + ",(unsigned long)q1,4);
        SayH("    TSSI OFDM-G                   = ",(unsigned long)qg,4);
        if(irQ & 0x00000800UL){
          quietPhyTxErr = 1;
          Say("");
          Say("    ⚠⚠ PHY_TXERR WITH NO TRANSMISSION. The bit is NOT attributable to our frame,");
          Say("       and k12 through k15 read it as though it were. Everything those runs");
          Say("       concluded from it is withdrawn -- it is a background condition.");
        } else {
          Say("");
          Say("    ★ NO PHY_TXERR in the quiet second. If the transmit passes below DO show it,");
          Say("      the bit is genuinely ours and porting recalc_txpower is justified."); }
        if(q0!=0x7F7F || q1!=0x7F7F){
          quietTssiMoved = 1;
          Say("    ⚠ TSSI ALSO MOVED WITHOUT TRANSMITTING, so k15's \"the PHY produced real");
          Say("      transmit power\" is withdrawn too -- the sentinel is measuring something");
          Say("      other than our frame.");
        } else Say("    ★ TSSI stayed at the sentinel, so k15's reading WAS caused by our frame."); }
      Say("");

      /* ⛔⛔ THE ATTENUATION LADDER IS DELETED. IT COST THREE RUNS AND ANSWERED NOTHING.
       *
       * k23, k24 and k26 all wedged the card inside it and all three produced a void
       * transmit section. k24 localised the cause to MacSuspend stopping the DMA engines;
       * k25 added the full suspend -> arm -> retune -> enable prologue that DwellAndCount
       * uses, and k26 wedged anyway. Four attempts, no data.
       *
       * Meanwhile the rest of the probe works perfectly and k26 produced the most important
       * result in the project -- PHY_TXERR disappeared from the known-good path -- in the
       * part of the run that happens BEFORE the ladder. The ladder is now the only thing
       * stopping us from testing the actual hypothesis.
       *
       * So it goes. k27 is k22's transmit section, which transmitted 8 passes out of 8,
       * plus the missing core_init block. One variable, and the variable is the thing we
       * came here to test. If attenuation ever matters again it can come back as its own
       * probe with its own reboot, not bolted onto the one experiment that counts. */
      { Say("");
        Say("  === THE TRANSMIT ANALOG CENSUS, ONCE, AT THE STATE STAGE 4 LEAVES ===");
      SayH("    radio 0x43 (rfatt)      = ",(unsigned long)RadioRead(0x0043),4);
      SayH("    radio 0x52 (bias/txctl) = ",(unsigned long)RadioRead(0x0052),4);
      SayH("    SHM[0x64] rfatt         = ",(unsigned long)ShmRead16Shared(0x0064),4);
      SayH("    PHY DACCTL (bbatt<<2)   = ",(unsigned long)PhyRead((UInt16)B43_PHY_DACCTL),4);
      SayH("    PHY RFOVER              = ",(unsigned long)PhyRead((UInt16)B43_PHY_RFOVER),4);
      SayH("    PHY RFOVERVAL           = ",(unsigned long)PhyRead((UInt16)B43_PHY_RFOVERVAL),4);
      SayH("      TRSWRX field          = ",
           (unsigned long)(PhyRead((UInt16)B43_PHY_RFOVERVAL)&B43_PHY_RFOVERVAL_TRSWRX),4);
      SayH("    PHY ITSSI, idle         = ",(unsigned long)PhyRead((UInt16)B43_PHY_ITSSI),4);
      Say("    (reported only -- nothing here is written, so this is the state Stage 4");
      Say("     and the new core_init block leave behind.)"); }
      /* ⚠ WRITTEN WITH THE MAC LIVE, ON PURPOSE. b43 suspends the MAC around adjust_txpower,
       * but k13 showed that per-pass state churn in this loop wedges the card, and these are
       * masked writes to gain registers rather than state machines. If the PHY needs a
       * suspend to APPLY them, the readbacks above still look right and ITSSI below will not
       * move -- which the verdict reads as "the setting is not reaching the analog chain",
       * and the next increment adds the suspend. Both outcomes are legible. */

      for(pass=0; pass<K17_NPASS; pass++){
        UInt32 t0,t1,rx0,rx1,rx2,promptMs=0; int ms,n,first,k,replies=0,beacons=0,fromTarget=0,rxLive=0,promptReply=0;
        Str255 L;

        L[0]=0;PCat(L,"  --- ");PCat(L,kWhich[pass]);PCat(L," ---");Out(L);

        /* pass 0 is the whole point: a broadcast frame needs NO ACK, so "not acked" stops
         * being the oracle. Every AP in range must still answer a wildcard probe request with
         * a probe response addressed to US. That separates "the frame is not understood on
         * air" from "the unicast ACK path is broken" -- two questions every run so far has
         * conflated, because every frame so far has been unicast. */
        if(pass==0) frameLen = BuildProbeRequest(frm,mac,kBroadcast,"",0);
        else        frameLen = BuildProbeRequest(frm,mac,dst,K10_TARGET_SSID,
                                                 targetFound?K10_TARGET_LEN:13);
        GenerateTxHdr351(txh,frm,frameLen,ch,(UInt16)(0xC000+pass),
                         B43_TXH_PHY_ANT01AUTO,(pass!=0));
        Say1("      frame bytes = ",(unsigned long)frameLen);
        SayH("      mac_ctl     = ",(unsigned long)le32ld(txh+TXH_MAC_CTL),8);
        Say ((le32ld(txh+TXH_MAC_CTL)&B43_TXH_MAC_ACK)
             ? "        ACK bit SET -- the MAC will expect an acknowledgement"
             : "        ACK bit CLEAR -- correct for broadcast; nothing acknowledges it");
        { Str255 M;M[0]=0;PCat(M,"      addr1 = ");PCatMac(M,frm+4);
          PCat(M,(pass==0)?"  (broadcast)":"  (the target)");Out(M); }
        SayH("      phy_ctl = ",(unsigned long)le16at(txh+TXH_PHY_CTL),4);
        if(pass==0){
          /* ⚠ k8 DUMPED 48 BYTES OF A 106-BYTE HEADER, so +72 (cookie) and +100 (the main
           * PLCP) were never seen -- and k12 came back with cookie 0x0000 where we stamped
           * 0xC000. The whole header goes in the log this time rather than the first half. */
          Say("");
          Say1("      the FULL txhdr, as the card reads it. Bytes = ",
               (unsigned long)TXH_SIZE_351);
          HexDump(txh,112,0xFFFFFFFFUL);
          Say1("      cookie reads = ",(unsigned long)le16at(txh+TXH_351_COOKIE));
          Say ("        (k12's status returned cookie 0x0000. If the bytes at +72 are 0x00 0xC0");
          Say ("         then we wrote it correctly and this firmware does not echo it; if they");
          Say ("         are zero, TXH_351_COOKIE is the wrong offset.)");
          SayH("      PLCP SIGNAL = ",(unsigned long)txh[TXH_351_PLCP],2);
          Say1("      PLCP length  at +102= ",
               (unsigned long)(txh[TXH_351_PLCP+2]|((UInt32)txh[TXH_351_PLCP+3]<<8)));
          Say(""); }

        /* ⚠ NO RE-ARM HERE. k13 proved the reset wedges a live engine and takes TX with it.
         * The ring was armed once before this loop; from here on it is recycled, which is what
         * b43 does at runtime. Channel and filter are already set, for the same reason --
         * touching them per pass meant suspending the MAC per pass. */
        (void)RxRecycle(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,0,0);

        (void)DmaControllerTxReset(cbase);
        { UInt32 ax = B43DmaAddressExt(ringPhys);
          UInt32 v  = B43_DMA32_TXENABLE |
                      ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,B43DmaAddressLow(ringPhys)); }

        /* ★ THE POSITIVE CONTROL. Listen BEFORE transmitting. If nothing arrives in a second
         * on a channel the scan just proved is busy, the receiver is not working in this
         * section and nothing the transmit does afterwards can be interpreted. */
        rx0 = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
        SsbSpinUs(1000000);
        n = RxScan(rxBufLog,frameoffset,&first);
        rx1 = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
        Say1("      CONTROL: frames heard in 1 s BEFORE transmitting = ",(unsigned long)n);
        SayRxStatus("        RXSTATUS before listening = ",rx0);
        SayRxStatus("        RXSTATUS after            = ",rx1);
        /* ★ THE CONTROL IS THE POINTER, NOT THE BUFFERS. k20 scored this on n > 0, and n
         * counts buffer CONTENTS -- which a starved engine leaves lying in the ring from an
         * earlier second. It read 32 twice while the engine sat in IDLEWAIT with RXDPTR
         * frozen, and so it certified a dead receiver as a passing control. RXDPTR is in the
         * same register and cannot be faked: if it moved, frames are arriving NOW. */
        rxLive = ((rx1 & B43_DMA32_RXDPTR) != (rx0 & B43_DMA32_RXDPTR));
        if(rxLive){
          rxControlOk = 1;
          Say("      ★ RXDPTR MOVED -- the receiver is live in this section."); }
        else {
          Say("      ⚠⚠ RXDPTR DID NOT MOVE -- THE RECEIVER IS DEAF RIGHT NOW.");
          Say("         Any 'no probe response' below is uninterpretable. This is k9's");
          Say("         defect, which k20's control could not detect because it counted");
          Say("         stale buffer contents rather than watching the pointer."); }

        /* hand back everything heard during the control, so a reply is distinguishable */
        (void)RxRecycle(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,0,0);

        /* ★ CLEAR THE TSSI SENTINELS. The microcode overwrites these with MEASURED transmit
         * power when a frame actually radiates. Stage 4 recorded all four still reading 0x7F7F
         * because nothing had transmitted yet. If they are still 0x7F7F after a transmission
         * that the MAC reported as TX_OK, the PHY produced no measurable RF -- which separates
         * "the analog chain is dead" from "it radiated and nobody answered", and those need
         * completely different fixes. */
        ShmClearTssi();
        tssiC0 = ShmRead16Shared(B43_SHM_SH_TSSI_CCK);
        tssiC1 = ShmRead16Shared(B43_SHM_SH_TSSI_CCK+2);

        /* ★ CLEAR THE IRQ REASON so PHY_TXERR below belongs to THIS frame rather than to
         * everything since power-on. k13 read 0x20008980 accumulated across the whole run and
         * could not say which transmit produced it. */
        ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
        (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);

        t0 = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
        PostTxFrameAt(cbase,ringLog,0,txhPhys,(UInt16)TXH_SIZE_351,frmPhys,frameLen);
        /* ⚠ 1 ms STEPS, NOT 20. This loop used to sleep 20 ms before its first look, which put
         * the whole reply window behind it -- an auth response arrives in single-digit
         * milliseconds. TXDPTR moves in microseconds; poll like it. */
        for(ms=0; ms<400; ms++){
          SsbSpinUs(1000);
          t1 = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
          if((t1 & B43_DMA32_TXDPTR) != (t0 & B43_DMA32_TXDPTR)) break; }

        /* ★★★ THE PROMPT LISTEN, BEFORE ANYTHING ELSE TOUCHES THE CARD.
         * Everything below -- draining TX status, settling TSSI, reading the IRQ reason -- costs
         * more than a second, and the ring wraps in about that. So the reply is caught HERE, at
         * the earliest moment there is, and the measurements happen afterwards. */
        { UInt32 rms=0; int rseen=0;
          promptReply = RxWaitFor(base,rxBufLog,frameoffset,
                                  0,5,                    /* mgmt, probe response */
                                  targetFound?dst:0,      /* from the target, if we know it */
                                  mac,                    /* addressed to us */
                                  300,gPromptBuf,&rms,&rseen);
          Say("");
          if(promptReply){
            Say1("      ★★★ PROMPT REPLY CAUGHT after ms = ",(unsigned long)rms);
            Say1("          frames walked while waiting = ",(unsigned long)rseen);
            promptMs = rms; promptHits++;
          } else {
            Say1("      no prompt reply within 300 ms; frames walked = ",(unsigned long)rseen);
            Say ("      (the ACK below still settles whether the AP heard us -- a missing probe");
            Say ("       RESPONSE and a missing ACK are different failures.)"); } }
        t1 = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
        SayH("      TXSTATUS after post = ",t1,8);
        SayH("        TXDPTR = ",(unsigned long)(t1&B43_DMA32_TXDPTR),4);
        SayH("        TXERROR= ",(unsigned long)(t1&B43_DMA32_TXERROR),8);
        if((t1 & B43_DMA32_TXDPTR) != (t0 & B43_DMA32_TXDPTR)){
          txMoved = 1; Say("      ★ the engine consumed the descriptors."); }
        else Say("      the engine did not move -- a k9 REGRESSION.");
        if((t1 & B43_DMA32_TXERROR)!=0) txError = 1;

        /* ★ did the MAC actually transmit? TXDPTR cannot answer that; this can. */
        { int ackBefore = gTxStatAcked;
          int st = DrainTxStatus(0xC000,600);
          if(gTxStatAcked > ackBefore && ackedOn<0) ackedOn = pass;
          if(!st){
            Say("      ✗ NO TX STATUS POSTED in 600 ms.");
            Say("        XMITSTAT_0 bit 0 never set, so the microcode never reported a");
            Say("        transmission at all. The DMA engine took the descriptors and the MAC");
            Say("        did nothing with them -- which means the frame CONTENTS are not the");
            Say("        suspect and the txhdr or the MAC's TX gating is.");
            SayH("        XMITSTAT_0 = ",(unsigned long)ssb_r32(gBus.bar0,B43_MMIO_XMITSTAT_0),8);
            SayH("        GEN_IRQ_REASON = ",
                 (unsigned long)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON),8); } }

        SsbSpinUs(500000);
        { UInt16 a0 = ShmRead16Shared(B43_SHM_SH_TSSI_CCK);
          UInt16 a1 = ShmRead16Shared(B43_SHM_SH_TSSI_CCK+2);
          UInt16 g0 = ShmRead16Shared(B43_SHM_SH_TSSI_OFDM_G);
          Say("");
          SayH("      TSSI CCK before = ",(unsigned long)tssiC0,4);
          SayH("                      + ",(unsigned long)tssiC1,4);
          SayH("      TSSI CCK after  = ",(unsigned long)a0,4);
          SayH("                      + ",(unsigned long)a1,4);
          SayH("      TSSI OFDM-G after = ",(unsigned long)g0,4);
          /* ★★ b43's OWN RULE, WHICH k15 THROUGH k22 DID NOT APPLY. ShmClearTssi writes 0x7F
           * into all four bytes as "the microcode has not written this slot". A byte still
           * reading 0x7F is therefore NOT a measurement of anything, and a set of four in
           * which any byte is still 0x7F is an INCOMPLETE measurement. Decode all four. */
          { int nValid=0, nZero=0, b;
            UInt8 tb[4];
            tb[0]=(UInt8)(a0&0xFF); tb[1]=(UInt8)(a0>>8);
            tb[2]=(UInt8)(a1&0xFF); tb[3]=(UInt8)(a1>>8);
            Say("      the four CCK TSSI slots, byte by byte:");
            for(b=0;b<4;b++){
              Str255 L;L[0]=0;PCat(L,"        slot ");PCatDec(L,(unsigned long)b);
              PCat(L," = 0x");PCatHex(L,(unsigned long)tb[b],2);
              if(tb[b]==0x7F) PCat(L,"   still the sentinel -- the microcode never wrote it");
              else { nValid++; if(tb[b]==0x00){ nZero++; PCat(L,"   A MEASUREMENT OF ZERO"); }
                     else PCat(L,"   a real reading"); }
              Out(L); }
            Say1("      slots the microcode actually wrote = ",(unsigned long)nValid);
            if(nValid==0)
              Say("      ⇒ NO MEASUREMENT AT ALL. The microcode did not sample this transmit.");
            else if(nZero==nValid)
              Say("      ⇒ EVERY WRITTEN SLOT READS ZERO -- the detector saw no output power.");
            else
              Say("      ⇒ ★ A NON-ZERO POWER READING. This is the first one."); }
          /* ★ AND THE REGISTER init_pctl USED. gCurIdleTssi came from PhyRead(B43_PHY_ITSSI)
           * and read 0x28 during calibration. Same register, same read, after a real frame --
           * so for once these two numbers are directly comparable. */
          SayH("      PHY ITSSI after this frame = ",
               (unsigned long)PhyRead((UInt16)B43_PHY_ITSSI),4);
          SayH("        init_pctl's calibration read = ",(unsigned long)gCurIdleTssi,4);
          if(a0==0x7F7F && a1==0x7F7F){
            Say("      ✗ STILL 0x7F7F -- THE PHY MEASURED NO TRANSMIT POWER.");
            Say("        The MAC reported TX_OK and the microcode did not suppress, but no RF");
            Say("        was produced. That points at the TX analog chain, and the first");
            Say("        suspect is tx_control = 0x00: phy_g.c:2526 does");
            Say("            gphy->tx_control = (default_tx_control(dev) << 4);");
            Say("        into a u8, so PA2DB (0x20) shifts to 0x200 and the u8 keeps 0x00.");
            Say("        Stage 4 ported that deliberately and said a later increment should");
            Say("        revisit it WITH EVIDENCE. This would be the evidence.");
          } else if(((a0&0xFF)!=0x7F && (a0&0xFF)!=0x00) || ((a0>>8)!=0x7F && (a0>>8)!=0x00)){
            Say("      ★ A NON-ZERO TSSI READING -- the PHY produced measurable output power.");
            tssiMoved = 1;
          } else {
            Say("      ⚠⚠ THE SENTINEL WAS REPLACED BY ZERO, WHICH IS NOT POWER.");
            Say("         k15 through k22 printed \"the PHY produced and measured real transmit");
            Say("         power\" on exactly this, because the oracle tested that the value");
            Say("         CHANGED rather than that it was sane. It is withdrawn. A detector");
            Say("         reading of zero after a transmission the MAC reported as TX_OK means");
            Say("         no usable RF left the analog chain."); } }
        { UInt32 ir = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
          SayH("      GEN_IRQ_REASON for THIS frame = ",ir,8);
          if(ir & 0x20000000UL) Say("        0x20000000 B43_IRQ_TX_OK      -- a transmission completed");
          if(ir & 0x00008000UL) Say("        0x00008000 B43_IRQ_DMA");
          if(ir & 0x00001000UL) Say("        0x00001000 B43_IRQ_PMEVENT");
          if(ir & 0x00000800UL){ Say("        0x00000800 B43_IRQ_PHY_TXERR ** PHY TRANSMIT ERROR **");
            phyTxErr++; }
          if(ir & 0x00000200UL){ Say("        0x00000200 B43_IRQ_MAC_TXERR ** MAC TRANSMIT ERROR **");
            macTxErr++; }
          if(ir & 0x00000100UL) Say("        0x00000100 B43_IRQ_PIO_WORKAROUND");
          if(!(ir & 0x00000800UL) && (ir & 0x20000000UL))
            Say("        ⇒ TX_OK with NO PHY error: this frame left the PHY cleanly."); }
        rx2 = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
        n = RxScan(rxBufLog,frameoffset,&first);
        for(k=0;k<K6_RX_SLOTS_USED;k++){
          UInt8 bs[6],sb[33],sl2,dc,js; UInt16 fc,fl; int r; const UInt8 *f2;
          if(RxBufferIsPoisoned(rxBufLog[k],frameoffset)) continue;
          r = ParseRxBuffer(rxBufLog[k],frameoffset,bs,sb,&sl2,&dc,&fc,&fl,&js);
          if(r==0) continue;
          if(r==2){ beacons++; if(MacEq(bs,dst)) fromTarget++; }
          f2 = rxBufLog[k] + frameoffset + K6_HDR_PLCP6;
          if(((fc>>2)&3)==0 && ((fc>>4)&0xF)==5 && MacEq(f2+4,mac)){
            replies++;
            if(repliedOn<0) repliedOn = pass;
            if(pass==0) bcastReplies++;
            slowReplies++;
            if(!gotReply){ gotReply = 1;
              Say("");
              Say("      ★★★ A PROBE RESPONSE ADDRESSED TO US:");
              { Str255 M; M[0]=0;PCat(M,"          from ");PCatMac(M,f2+10);Out(M);
                M[0]=0;PCat(M,"          to   ");PCatMac(M,f2+4);PCat(M,"  = our own MAC");Out(M); }
              if(sl2){ Str255 M;int q;M[0]=0;PCat(M,"          SSID \"");
                for(q=0;q<sl2;q++){char cc[2];cc[0]=(char)((sb[q]>=32&&sb[q]<127)?sb[q]:'.');cc[1]=0;PCat(M,cc);}
                PCat(M,"\"");Out(M); } } } }
        Say1("      frames heard AFTER transmitting = ",(unsigned long)n);
        Say1("        of which beacons/probe-resp   = ",(unsigned long)beacons);
        Say1("        of which from the target      = ",(unsigned long)fromTarget);
        Say1("        probe responses to US         = ",(unsigned long)replies);
        if(promptReply){
          Say1("        ★ and the PROMPT path had it after ms = ",(unsigned long)promptMs);
          Say ("          The slow scan above may or may not still find it -- the ring wraps in");
          Say ("          about a second. That gap is the whole reason RxWaitFor exists.");
        } else if(replies>0){
          Say("        ⚠⚠ THE SLOW SCAN FOUND A REPLY THE PROMPT PATH MISSED. That inverts the");
          Say("           premise of k29 and RxWaitFor's predicate is wrong -- most likely the");
          Say("           addr1/addr2 test, since the slow scan only checks addr1."); }
        SayRxStatus("        RXSTATUS after the transmit = ",rx2);
        /* ★ AND DID THE TRANSMIT ITSELF KILL THE RECEIVER? k13 found that resetting a live
         * RX engine takes TX down with it; the converse has never been checked. If the
         * pointer moved during the control and is parked now, the reply window was deaf even
         * though the control passed, and the count above still means nothing. */
        if(rxLive && ((rx2 & B43_DMA32_RXDPTR) == (rx1 & B43_DMA32_RXDPTR))){
          Say("        ⚠⚠ RXDPTR MOVED DURING THE CONTROL AND IS PARKED NOW -- the receiver");
          Say("           died across the transmit, so the reply count is still not evidence."); }
        Say("");
      }

      /* ================= ★★★ STAGE 6-1: OPEN SYSTEM AUTHENTICATION ================= */
      Say("");
      Say("  ============ [6-1] ★★★ OPEN SYSTEM AUTHENTICATION ============");
      Say("  The first frame that asks the AP for something rather than just being heard by it.");
      Say("  Two frames, no cryptography: we send algorithm 0 / sequence 1, the AP answers");
      Say("  sequence 2 with a status code. 802.11-2020 §9.3.3.12.");
      Say("");
      Say("  ⚠ THE PSK PLAYS NO PART HERE. Open System auth is a formality on a WPA2 network;");
      Say("    admission is decided at association and in the 4-way handshake. A non-zero status");
      Say("    means something structural -- MAC filtering, a band or rate mismatch, a malformed");
      Say("    frame -- and NOT a wrong passphrase.");
      Say("");
      if(!promptHits){
        Say("  ⛔ SKIPPED. k29's prompt receive path caught nothing on the probe passes above, and");
        Say("     an auth response arrives in single-digit milliseconds. Sending one now would");
        Say("     produce an unreadable result: we could not tell a refusal from our own deafness.");
        Say("     Fix the receive path first -- that is what Stage 6-0 is for.");
      } else if(!targetFound){
        Say("  ⛔ SKIPPED. The scan did not find the target SSID, so we have no BSSID to");
        Say("     authenticate against and would be guessing at the address.");
      } else {
        UInt32 t0a,t1a,ams; int ms2,aseen,got;
        UInt16 alen = BuildAuthRequest(frm,mac,dst);

        { Str255 M;M[0]=0;PCat(M,"  authenticating to ");PCatMac(M,dst);
          PCat(M," on channel ");PCatDec(M,(unsigned long)ch);Out(M); }
        Say1("  auth request bytes = ",(unsigned long)alen);
        Say ("  frame body: alg 0x0000, seq 0x0001, status 0x0000 -- little-endian, 6 bytes");

        /* mac_ctl: ACK requested (a unicast management frame is acknowledged), hardware sequence
         * numbering, start-of-MSDU. Exactly what the directed probe request used, which we know
         * draws an ACK from this AP. */
        GenerateTxHdr351(txh,frm,alen,ch,(UInt16)0xC010,B43_TXH_PHY_ANT01AUTO,1);

        (void)RxRecycle(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,0,0);
        (void)DmaControllerTxReset(cbase);
        { UInt32 ax = B43DmaAddressExt(ringPhys);
          UInt32 v  = B43_DMA32_TXENABLE |
                      ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,B43DmaAddressLow(ringPhys)); }

        t0a = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
        PostTxFrameAt(cbase,ringLog,0,txhPhys,(UInt16)TXH_SIZE_351,frmPhys,alen);
        for(ms2=0; ms2<400; ms2++){
          SsbSpinUs(1000);
          t1a = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
          if((t1a & B43_DMA32_TXDPTR) != (t0a & B43_DMA32_TXDPTR)) break; }
        if((t1a & B43_DMA32_TXDPTR) != (t0a & B43_DMA32_TXDPTR))
          Say("  ★ the engine consumed the auth descriptors.");
        else
          Say("  ✗ the TX engine did not move -- nothing was sent, so the silence below is ours.");

        /* subtype 11 = authentication, from the target, addressed to us */
        got = RxWaitFor(base,rxBufLog,frameoffset,0,11,dst,mac,500,gPromptBuf,&ams,&aseen);

        if(!got){
          Say1("  ✗ NO AUTH RESPONSE within 500 ms. frames walked = ",(unsigned long)aseen);
          Say ("    k29 measured the probe response at 5-14 ms, so 500 ms is not a timing");
          Say ("    problem. Either the AP declined to answer -- MAC filtering is the first");
          Say ("    thing to check, and we deliberately left it OFF on the test network -- or the");
          Say ("    frame was malformed. The ACK line above separates those: an ACKed auth");
          Say ("    request that draws no response means the AP received it and chose silence.");
        } else {
          const UInt8 *bf = gPromptBuf + frameoffset + K6_HDR_PLCP6;
          UInt16 aalg = (UInt16)(bf[24] | ((UInt16)bf[25]<<8));
          UInt16 aseq = (UInt16)(bf[26] | ((UInt16)bf[27]<<8));
          UInt16 asta = (UInt16)(bf[28] | ((UInt16)bf[29]<<8));
          Say("");
          Say1("  ★★★ AUTH RESPONSE RECEIVED after ms = ",(unsigned long)ams);
          Say1("      frames walked while waiting     = ",(unsigned long)aseen);
          { Str255 M;M[0]=0;PCat(M,"      from ");PCatMac(M,bf+10);
            PCat(M,"  to ");PCatMac(M,bf+4);Out(M); }
          SayH("      algorithm = ",(unsigned long)aalg,4);
          SayH("      sequence  = ",(unsigned long)aseq,4);
          SayH("      status    = ",(unsigned long)asta,4);
          Say("");
          Say("  the first 32 bytes of the 802.11 frame, so the decode can be checked by hand:");
          HexDump((UInt8*)bf,32,0xFFFFFFFFUL);
          authAlg = aalg; authSeq = aseq; authStatus = asta; authGot = 1;
          if(aseq != AUTH_SEQ_RESPONSE){
            Say("  ⚠ SEQUENCE IS NOT 2. This is an auth frame from the target addressed to us but");
            Say("    not the second frame of our exchange -- do not read the status as our answer."); }

          /* ============ ★★★ STAGE 6-2: ASSOCIATION ============
           * Only if authentication actually succeeded. An association request from a station the
           * AP does not hold as authenticated draws status 9 ("not authenticated"), and that
           * would be a self-inflicted result telling us nothing about our frame. */
          if(aalg==AUTH_ALG_OPEN && aseq==AUTH_SEQ_RESPONSE && asta==AUTH_STATUS_SUCCESS){
            UInt32 t0b,t1b,bms; int ms3,bseen,gotb;
            UInt16 blen = BuildAssocRequest(frm,mac,dst,K10_TARGET_SSID,K10_TARGET_LEN,1);

            /* ★ THE PMK, BEFORE ASSOCIATING. Message 1 arrives unprompted about 100 ms after the
             * association response and will not wait while we run 4096 iterations of HMAC-SHA1.
             * Timed here because the 0.3-1 s estimate for this G4 was extrapolated from the
             * development Mac and has never been measured on the actual hardware. */
            Say("");
            Say("  ============ [7-0] THE PMK ============");
            Say("  PBKDF2-HMAC-SHA1(password, \"" AP_PSK_SSID "\", 4096, 32).");
            Say("  The password comes from ap_psk_config.h, which is gitignored and is not read");
            Say("  by anything that leaves this machine. NOTHING derived from it is printed:");
            Say("  the PMK is password-equivalent for this SSID, so a log on a file share is the");
            Say("  last place it should appear. Only the timing and a presence check go below.");
            { UInt32 ms0 = TickCount();
              ApPbkdf2Sha1((const ApU8*)AP_PSK_PASSPHRASE,(ApU32)AP_PSK_PASSPHRASE_LEN,
                           (const ApU8*)AP_PSK_SSID,(ApU32)AP_PSK_SSID_LEN,
                           4096UL,gPmk,32UL);
              { UInt32 ms1 = TickCount(); int z,nz=0;
                for(z=0;z<32;z++) if(gPmk[z]) nz++;
                gPmkReady = (nz > 0);
                Say1("  elapsed, 60ths of a second = ",(unsigned long)(ms1-ms0));
                Say1("  elapsed, milliseconds      = ",(unsigned long)((ms1-ms0)*1000UL/60UL));
                Say1("  non-zero bytes in the PMK  = ",(unsigned long)nz);
                Say ("    (32 zero bytes would mean the derivation silently did nothing; any");
                Say ("     other count says it produced something without saying what.)"); } }

            Say("");
            Say("  ============ [6-2] ★★★ ASSOCIATION ============");
            Say1("  assoc request bytes = ",(unsigned long)blen);
            Say ("  capability = ESS | PRIVACY | SHORT PREAMBLE,  listen interval 1");
            Say ("  RSNE: group CCMP-128, pairwise CCMP-128, AKM PSK, RSN caps 0x0000 (no PMF)");
            Say ("  ⚠ The RSNE is stated, not discovered -- we never parse the AP's beacon. The");
            Say ("    downgrade defence is the RSNE comparison in EAPOL message 3, which is");
            Say ("    Stage 7 and is NOT skipped. See MINIMAL-STA-SCOPE.md §2.");
            Say("");
            Say("  the request, as the AP will read it:");
            HexDump(frm,blen>64?64:blen,0xFFFFFFFFUL);

            GenerateTxHdr351(txh,frm,blen,ch,(UInt16)0xC020,B43_TXH_PHY_ANT01AUTO,1);
            (void)RxRecycle(base,&rxRing,rxBufLog,rxBufPhys,frameoffset,rxBufSize,0,0);
            (void)DmaControllerTxReset(cbase);
            { UInt32 ax = B43DmaAddressExt(ringPhys);
              UInt32 v  = B43_DMA32_TXENABLE |
                          ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
              ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
              ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,B43DmaAddressLow(ringPhys)); }

            t0b = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
            PostTxFrameAt(cbase,ringLog,0,txhPhys,(UInt16)TXH_SIZE_351,frmPhys,blen);
            for(ms3=0; ms3<400; ms3++){
              SsbSpinUs(1000);
              t1b = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
              if((t1b & B43_DMA32_TXDPTR) != (t0b & B43_DMA32_TXDPTR)) break; }
            if((t1b & B43_DMA32_TXDPTR) != (t0b & B43_DMA32_TXDPTR))
              Say("  ★ the engine consumed the assoc descriptors.");
            else
              Say("  ✗ the TX engine did not move -- nothing was sent.");

            /* subtype 1 = association response */
            gotb = RxWaitFor(base,rxBufLog,frameoffset,0,1,dst,mac,800,gPromptBuf,&bms,&bseen);
            if(!gotb){
              Say1("  ✗ NO ASSOC RESPONSE within 800 ms. frames walked = ",(unsigned long)bseen);
              Say ("    Auth succeeded moments ago, so the AP knows this MAC. Silence here points");
              Say ("    at the request's CONTENTS rather than at addressing -- the RSNE first.");
            } else {
              const UInt8 *cf = gPromptBuf + frameoffset + K6_HDR_PLCP6;
              UInt16 ccap = (UInt16)(cf[24] | ((UInt16)cf[25]<<8));
              UInt16 csta = (UInt16)(cf[26] | ((UInt16)cf[27]<<8));
              UInt16 caid = (UInt16)(cf[28] | ((UInt16)cf[29]<<8));
              Say("");
              Say1("  ★★★ ASSOCIATION RESPONSE RECEIVED after ms = ",(unsigned long)bms);
              SayH("      capability = ",(unsigned long)ccap,4);
              SayH("      status     = ",(unsigned long)csta,4);
              SayH("      AID raw    = ",(unsigned long)caid,4);
              Say1("      AID        = ",(unsigned long)(caid & 0x3FFF));
              Say ("      (the top two bits are always set by the AP and are not part of the AID)");
              Say("");
              Say("  the first 32 bytes, checkable by hand:");
              HexDump((UInt8*)cf,32,0xFFFFFFFFUL);
              assocGot = 1; assocStatus = csta; assocAid = (UInt16)(caid & 0x3FFF);

              /* ============ ★★★ STAGE 7-1: EAPOL-KEY MESSAGE 1 ============ */
              if(csta==ASSOC_STATUS_SUCCESS && gPmkReady){
                UInt32 ems; int eseen, gote;
                Say("");
                Say("  ============ [7-1] ★★★ EAPOL-KEY MESSAGE 1 ============");
                Say("  We send nothing here. Having associated, the AP starts the 4-way handshake");
                Say("  on its own -- message 1 arrives unprompted with the ANonce. Waiting 2 s,");
                Say("  which is generous: it is normally under 100 ms.");

                /* Type 2 = data, any subtype. The SNAP header identifies EAPOL, not the subtype:
                 * a WMM station gets QoS data (subtype 8) and a plain one subtype 0, and which
                 * we are depends on a decision the AP made, not on anything we control. */
                gote = RxWaitFor(base,rxBufLog,frameoffset,2,RXW_ANY_SUB,dst,mac,
                                 2000,gPromptBuf,&ems,&eseen);
                if(!gote){
                  Say1("  ✗ NO DATA FRAME from the AP within 2 s. frames walked = ",
                       (unsigned long)eseen);
                  Say ("    We are associated, so the AP should be starting the handshake. If it");
                  Say ("    is not, the likely causes are that our association was accepted but");
                  Say ("    the AP does not consider us an RSN station -- check the RSNE went out");
                  Say ("    intact -- or that it already timed us out before we looked.");
                } else {
                  const UInt8 *ef = gPromptBuf + frameoffset + K6_HDR_PLCP6;
                  UInt16 eflen = le16at(gPromptBuf + RXH_FRAME_LEN);
                  const UInt8 *eap;
                  eflen = (UInt16)(eflen > K6_HDR_PLCP6 ? eflen - K6_HDR_PLCP6 : 0);
                  eap = EapolBody(ef,eflen);
                  Say1("  a data frame arrived after ms = ",(unsigned long)ems);
                  if(!eap){
                    Say("  ⚠ ...but it is not EAPOL. The LLC/SNAP header is not AA AA 03 00 00 00");
                    Say("    88 8E, so this is ordinary traffic and the handshake has not started.");
                    Say("    The first 32 bytes, to see what it actually is:");
                    HexDump((UInt8*)ef,32,0xFFFFFFFFUL);
                  } else {
                    UInt16 kinfo = be16at(eap+EAPOL_O_KEYINFO);
                    int isMsg1 = (eap[EAPOL_O_TYPE]==EAPOL_TYPE_KEY)
                              && ((kinfo & KEYINFO_PAIRWISE)!=0)
                              && ((kinfo & KEYINFO_ACK)!=0)
                              && ((kinfo & KEYINFO_MIC)==0);
                    Say("");
                    Say ("  ★★★ EAPOL-KEY RECEIVED.");
                    SayH("      EAPOL version    = ",(unsigned long)eap[EAPOL_O_VERSION],2);
                    SayH("      EAPOL type       = ",(unsigned long)eap[EAPOL_O_TYPE],2);
                    Say ("        (3 = EAPOL-Key)");
                    SayH("      descriptor type  = ",(unsigned long)eap[EAPOL_O_DESCTYPE],2);
                    Say ("        (2 = RSN, which is what WPA2 uses)");
                    SayH("      key information  = ",(unsigned long)kinfo,4);
                    Say1("        descriptor version = ",(unsigned long)(kinfo & KEYINFO_VERSION));
                    if(kinfo & KEYINFO_PAIRWISE)  Say("        PAIRWISE");
                    if(kinfo & KEYINFO_INSTALL)   Say("        INSTALL");
                    if(kinfo & KEYINFO_ACK)       Say("        ACK");
                    if(kinfo & KEYINFO_MIC)       Say("        MIC");
                    if(kinfo & KEYINFO_SECURE)    Say("        SECURE");
                    if(kinfo & KEYINFO_ENCRYPTED) Say("        ENCRYPTED KEY DATA");
                    SayH("      key length       = ",(unsigned long)be16at(eap+EAPOL_O_KEYLEN),4);
                    Say ("        (16 = a CCMP pairwise key, which is what we asked for)");
                    Say ("      the ANonce, which is public and safe to print:");
                    HexDump((UInt8*)(eap+EAPOL_O_NONCE),32,0xFFFFFFFFUL);

                    if(!isMsg1){
                      Say("  ⚠ THIS IS NOT MESSAGE 1. Message 1 has PAIRWISE and ACK set and MIC");
                      Say("    clear. Do not derive a PTK from another message's nonce.");
                    } else {
                      int z,nz=0;
                      eapolMsg1 = 1;
                      for(z=0;z<32;z++) gANonce[z] = eap[EAPOL_O_NONCE+z];
                      for(z=0;z<8;z++)  gReplay[z] = eap[EAPOL_O_REPLAY+z];

                      /* ⚠ SNonce must be unpredictable. There is no RNG here yet, so this is
                       * built from the tick counter and the ANonce -- adequate to prove the
                       * derivation runs, NOT adequate for a driver anyone relies on. Stage 7-2
                       * replaces it before message 2 is ever sent for real. */
                      { UInt32 seed = TickCount();
                        for(z=0;z<32;z++)
                          gSNonce[z] = (UInt8)((seed >> ((z&3)*8)) ^ gANonce[31-z] ^ (UInt8)(z*37+11)); }

                      ApPmkToPtk(gPmk,32UL,dst,mac,gANonce,gSNonce,gPtk);
                      for(z=0;z<AP_PTK_LEN;z++) if(gPtk[z]) nz++;
                      gPtkReady = (nz > 0);
                      Say("");
                      Say ("  ★★★ PTK DERIVED from this ANonce.");
                      Say1("      non-zero bytes in the 48-byte PTK = ",(unsigned long)nz);
                      Say ("      KCK, KEK and TK are NOT printed -- the TK is the session key and");
                      Say ("      the KCK forges MICs. A log on a file share is the wrong home for");
                      Say ("      any of them. The MIC on message 2 is what proves they are right,");
                      Say ("      and that is Stage 7-2.");
                      Say ("      ⚠ The SNonce here is derived from the tick counter, which is NOT");
                      Say ("        an acceptable source for a real supplicant. It is enough to");
                      Say ("        exercise the derivation; a real entropy source is required");
                      Say ("        before this is a driver rather than a probe.");

                      /* ====== ★★★ STAGE 7-2: MESSAGE 2, AND THE MIC ====== */
                      if(gPtkReady){
                        UInt32 t0c,t1c,m3ms; int ms4,m3seen,gotm3;
                        UInt16 m2len = BuildEapolKeyFrame(frm,mac,dst,gSNonce,gReplay,
                                          gPtk+AP_KCK_OFF,
                                          (UInt16)((kinfo & KEYINFO_VERSION)
                                                   | KEYINFO_PAIRWISE | KEYINFO_MIC),
                                          1);
                        Say("");
                        Say("  ============ [7-2] ★★★ EAPOL-KEY MESSAGE 2 ============");
                        Say1("  message 2 bytes = ",(unsigned long)m2len);
                        Say ("  key info = descriptor version | PAIRWISE | MIC. Key length 0 for");
                        Say ("  RSN. Replay counter echoed from message 1, not invented.");
                        Say ("  Key Data is the RSNE, byte-identical to the association request's");
                        Say ("  -- the AP compares them and aborts if they differ at all.");
                        Say ("  ⚠ The MIC covers from the EAPOL header with the MIC field zeroed,");
                        Say ("    NOT from the EAPOL-Key header. The published standard is wrong");
                        Say ("    about this; hostap wpa_common.c:186 documents the error.");
                        Say("");
                        Say("  the first 64 bytes as the AP will read them:");
                        HexDump(frm,64,0xFFFFFFFFUL);

                        GenerateTxHdr351(txh,frm,m2len,ch,(UInt16)0xC030,
                                         B43_TXH_PHY_ANT01AUTO,1);
                        (void)DmaControllerTxReset(cbase);
                        { UInt32 ax = B43DmaAddressExt(ringPhys);
                          UInt32 v  = B43_DMA32_TXENABLE |
                                      ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
                          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
                          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,B43DmaAddressLow(ringPhys)); }

                        t0c = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                        PostTxFrameAt(cbase,ringLog,0,txhPhys,(UInt16)TXH_SIZE_351,frmPhys,m2len);
                        for(ms4=0; ms4<400; ms4++){
                          SsbSpinUs(1000);
                          t1c = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                          if((t1c & B43_DMA32_TXDPTR) != (t0c & B43_DMA32_TXDPTR)) break; }
                        if((t1c & B43_DMA32_TXDPTR) != (t0c & B43_DMA32_TXDPTR)){
                          eapolMsg2Sent = 1;
                          Say("  ★ the engine consumed message 2's descriptors."); }
                        else
                          Say("  ✗ the TX engine did not move -- message 2 was never sent.");

                        /* ★ THE ORACLE IS MESSAGE 3, NOT ANYTHING IN MESSAGE 2's SEND PATH.
                         * A bad MIC is discarded silently: the AP does not answer, does not
                         * complain, and simply retries message 1. So "we sent it and the engine
                         * took it" proves nothing about the key. Message 3 arriving is the only
                         * evidence that our PTK is the AP's PTK. */
                        Say("");
                        Say("  waiting for message 3, which is the ONLY thing that validates the");
                        Say("  PTK. A wrong MIC is discarded in silence, so a timeout here means");
                        Say("  the key is wrong -- not that the frame was malformed.");
                        gotm3 = RxWaitFor(base,rxBufLog,frameoffset,2,RXW_ANY_SUB,dst,mac,
                                          2000,gPromptBuf,&m3ms,&m3seen);
                        if(!gotm3){
                          Say1("  ✗ NOTHING within 2 s. frames walked = ",(unsigned long)m3seen);
                        } else {
                          const UInt8 *mf = gPromptBuf + frameoffset + K6_HDR_PLCP6;
                          UInt16 mflen = le16at(gPromptBuf + RXH_FRAME_LEN);
                          const UInt8 *m3;
                          mflen = (UInt16)(mflen > K6_HDR_PLCP6 ? mflen - K6_HDR_PLCP6 : 0);
                          m3 = EapolBody(mf,mflen);
                          Say1("  a data frame arrived after ms = ",(unsigned long)m3ms);
                          if(!m3){
                            Say("  ⚠ not EAPOL. The handshake did not advance.");
                          } else {
                            UInt16 k3 = be16at(m3+EAPOL_O_KEYINFO);
                            SayH("  key information = ",(unsigned long)k3,4);
                            if(k3 & KEYINFO_MIC)       Say("      MIC");
                            if(k3 & KEYINFO_SECURE)    Say("      SECURE");
                            if(k3 & KEYINFO_INSTALL)   Say("      INSTALL");
                            if(k3 & KEYINFO_ENCRYPTED) Say("      ENCRYPTED KEY DATA");
                            if(k3 & KEYINFO_ACK)       Say("      ACK");
                            /* Message 3 is the one with INSTALL and SECURE set alongside ACK
                             * and MIC. Message 1 retried would have ACK only and no MIC. */
                            if((k3 & KEYINFO_MIC) && (k3 & KEYINFO_ACK) &&
                               (k3 & KEYINFO_INSTALL)){
                              eapolMsg3 = 1;
                              Say("");
                              Say("  ★★★★★★ MESSAGE 3. OUR MIC VERIFIED.");
                              Say("  The AP recomputed the MIC on message 2 with ITS PTK and got");
                              Say("  our value. Both sides derived the same key independently, so");
                              Say("  the PMK, the PRF, the min||max ordering, the label's NUL and");
                              Say("  the KCK are all correct at once.");

                              /* ====== ★★★ STAGE 7-3: VERIFY, UNWRAP, COMPARE ====== */
                              Say("");
                              Say("  ============ [7-3] ★★★ MESSAGE 3: MIC, RSNE, GTK ============");
                              {
                                UInt16 m3BodyLen = be16at(m3+EAPOL_O_BODYLEN);
                                UInt16 m3DataLen = be16at(m3+EAPOL_O_DATALEN);
                                UInt8  rxMic[16];
                                ApU8   calc[AP_SHA1_LEN];
                                UInt8  scratch[AP_PROMPT_BUF];
                                UInt8  plain[256];
                                int z, micOk = 0;

                                Say1("      EAPOL body length = ",(unsigned long)m3BodyLen);
                                Say1("      key data length   = ",(unsigned long)m3DataLen);

                                /* --- 1. VERIFY THEIR MIC, the same way they verified ours.
                                 * ⚠ THIS IS NOT CEREMONY. Without it we would unwrap and install
                                 * a group key from a frame we never authenticated, which is the
                                 * whole attack the handshake exists to prevent. Same input range
                                 * as message 2: from the EAPOL header, MIC field zeroed. */
                                if((UInt32)(4 + m3BodyLen) <= (UInt32)AP_PROMPT_BUF){
                                  for(z=0;z<4+m3BodyLen;z++) scratch[z] = m3[z];
                                  for(z=0;z<16;z++){ rxMic[z] = m3[EAPOL_O_MIC+z];
                                                     scratch[EAPOL_O_MIC+z] = 0; }
                                  ApHmacSha1((const ApU8*)(gPtk+AP_KCK_OFF),16UL,
                                             (const ApU8*)scratch,(ApU32)(4+m3BodyLen),calc);
                                  micOk = 1;
                                  for(z=0;z<16;z++) if(calc[z]!=rxMic[z]) micOk = 0; }
                                SayOk("message 3's OWN MIC verifies against our KCK",micOk);
                                if(!micOk){
                                  Say("      ⛔ STOPPING. An unauthenticated message 3 must not be");
                                  Say("         acted on: unwrapping it would install a group key");
                                  Say("         chosen by whoever sent the frame.");
                                } else {
                                  /* --- 2. UNWRAP the Key Data with the KEK. RFC 3394. */
                                  int unwrapped = 0;
                                  UInt16 plainLen = 0;
                                  if((k3 & KEYINFO_ENCRYPTED) && m3DataLen >= 24 &&
                                     m3DataLen <= sizeof(plain)+8){
                                    unwrapped = ApAesUnwrap((const ApU8*)(gPtk+AP_KEK_OFF),
                                                            (const ApU8*)(m3+EAPOL_O_DATA),
                                                            (ApU32)m3DataLen,(ApU8*)plain);
                                    plainLen = (UInt16)(m3DataLen - 8); }
                                  SayOk("the Key Data unwrapped and its RFC 3394 IV checked out",
                                        unwrapped);
                                  if(!unwrapped){
                                    Say("      ⛔ The integrity check failed, so the KEK is wrong or");
                                    Say("         the data was altered. Either way there is nothing");
                                    Say("         to salvage -- ApAesUnwrap leaves the output alone");
                                    Say("         rather than handing back a partial result.");
                                  } else {
                                    UInt16 q = 0; int rsneOk = 0, gtkFound = 0;
                                    gtkUnwrapped = 1;
                                    Say1("      plaintext key data bytes = ",(unsigned long)plainLen);
                                    Say ("      the decrypted Key Data, walked as KDEs:");

                                    /* --- 3. WALK THE KDEs. An RSN element (48) and a vendor
                                     * specific KDE (221) carrying 00-0F-AC type 1, the GTK. */
                                    while(q + 2 <= plainLen){
                                      UInt8 id = plain[q], ln = plain[q+1];
                                      if(q + 2 + ln > plainLen) break;
                                      if(id == 0x30){
                                        UInt16 caps = 0;
                                        rsneOk = RsneOffersCcmpPsk(plain+q,(UInt16)(ln+2),&caps);
                                        Say1("        RSN element, total bytes = ",
                                             (unsigned long)(ln+2));
                                        HexDump(plain+q,ln+2,0xFFFFFFFFUL);
                                        SayH("          the AP's RSN capabilities = ",
                                             (unsigned long)caps,4);
                                        Say ("          reported, NOT compared. Ours is 0x0000 and");
                                        Say ("          a difference here is normal -- replay-counter");
                                        Say ("          advertisements, not cipher policy.");
                                      } else if(id == 0xDD && ln >= 6 &&
                                                plain[q+2]==0x00 && plain[q+3]==0x0F &&
                                                plain[q+4]==0xAC && plain[q+5]==0x01){
                                        /* GTK KDE: OUI 00-0F-AC, data type 1, then a key-id byte,
                                         * a reserved byte, then the GTK itself. */
                                        /* ⚠ k34 REPORTED 14 AND SHOULD HAVE SAID 16. The KDE
                                         * length covers OUI(3) + type(1) + keyid(1) +
                                         * reserved(1) + GTK, so the GTK is ln - 6 -- and k34
                                         * then subtracted the keyid and reserved bytes a SECOND
                                         * time. It copied 14 of the 16 bytes, which would have
                                         * produced a group key that silently decrypts nothing.
                                         * The GTK starts at q+8, which was always right; only
                                         * the length was wrong. */
                                        UInt16 glen = (UInt16)(ln - 6);
                                        gtkFound = 1;
                                        gGtkId = (UInt8)(plain[q+6] & 0x03);
                                        if(glen >= 1 && glen <= 32){
                                          gGtkLen = (UInt8)glen;
                                          for(z=0;z<gGtkLen;z++) gGtk[z] = plain[q+8+z]; }
                                        Say1("        ★ GTK KDE found, key id = ",
                                             (unsigned long)gGtkId);
                                        Say1("          GTK length = ",(unsigned long)gGtkLen);
                                        Say ("          the GTK itself is NOT printed -- it decrypts");
                                        Say ("          this network's broadcast traffic.");
                                      } else {
                                        Say1("        KDE id ",(unsigned long)id); }
                                      q = (UInt16)(q + 2 + ln); }

                                    /* --- 4. THE DOWNGRADE DEFENCE. */
                                    Say("");
                                    SayOk("★ the AP's own RSNE offers CCMP and PSK, which is what"
                                          " we negotiated",rsneOk);
                                    Say ("      ⚠ k34 COMPARED THIS BYTE FOR BYTE AGAINST OURS AND");
                                    Say ("        WAS WRONG. Message 3's RSNE is the AP's own");
                                    Say ("        advertisement, not an echo of our request, and it");
                                    Say ("        differed only in RSN capabilities -- 0x000C, a");
                                    Say ("        replay-counter advertisement, against our 0x0000.");
                                    Say ("        No downgrade, no finding, just a wrong test.");
                                    Say ("      What this check is really for: wpa_supplicant");
                                    Say ("      compares message 3's RSNE against the BEACON's, to");
                                    Say ("      catch a rewritten beacon. We never read the beacon");
                                    Say ("      (MINIMAL-STA-SCOPE §2), so that defence is not");
                                    Say ("      available -- and not needed, because a client that");
                                    Say ("      never reads a beacon cannot be downgraded by one. We");
                                    Say ("      demand CCMP and PSK unconditionally. What IS worth");
                                    Say ("      checking is that the AP genuinely offers them.");
                                    if(!rsneOk){
                                      Say("      ⛔ THE AP DOES NOT OFFER WHAT WE NEGOTIATED. Abort:");
                                      Say("         do not install these keys, do not send msg 4."); }
                                    gtkOk = (gtkFound && rsneOk && gGtkLen > 0);

                                    /* ====== ★★★ STAGE 7-4: MESSAGE 4 ====== */
                                    if(gtkOk){
                                      UInt32 t0d,t1d,qms; int ms5,qseen,more;
                                      UInt16 m4info, m4len;
                                      /* hostap wpa.c:1634 -- keep ONLY the SECURE bit from
                                       * message 3, then add version, PAIRWISE and MIC. */
                                      m4info = (UInt16)((k3 & KEYINFO_SECURE)
                                                        | (k3 & KEYINFO_VERSION)
                                                        | KEYINFO_PAIRWISE | KEYINFO_MIC);
                                      /* ⚠ the replay counter comes from MESSAGE 3 now, not
                                       * message 1. Echoing message 1's would be rejected. */
                                      for(z=0;z<8;z++) gReplay[z] = m3[EAPOL_O_REPLAY+z];
                                      m4len = BuildEapolKeyFrame(frm,mac,dst,0,gReplay,
                                                                 gPtk+AP_KCK_OFF,m4info,0);
                                      Say("");
                                      Say("  ============ [7-4] ★★★ EAPOL-KEY MESSAGE 4 ============");
                                      SayH("      key info = ",(unsigned long)m4info,4);
                                      Say ("        SECURE carried over from message 3, plus");
                                      Say ("        version, PAIRWISE and MIC. No ACK, no INSTALL.");
                                      Say1("      bytes = ",(unsigned long)m4len);
                                      Say ("      zero nonce and no Key Data, both by the standard.");

                                      GenerateTxHdr351(txh,frm,m4len,ch,(UInt16)0xC040,
                                                       B43_TXH_PHY_ANT01AUTO,1);
                                      (void)DmaControllerTxReset(cbase);
                                      { UInt32 ax = B43DmaAddressExt(ringPhys);
                                        UInt32 v = B43_DMA32_TXENABLE |
                                          ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
                                        ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
                                        ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,
                                                B43DmaAddressLow(ringPhys)); }
                                      t0d = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                                      PostTxFrameAt(cbase,ringLog,0,txhPhys,
                                                    (UInt16)TXH_SIZE_351,frmPhys,m4len);
                                      for(ms5=0; ms5<400; ms5++){
                                        SsbSpinUs(1000);
                                        t1d = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                                        if((t1d & B43_DMA32_TXDPTR)!=(t0d & B43_DMA32_TXDPTR)) break; }
                                      if((t1d & B43_DMA32_TXDPTR)!=(t0d & B43_DMA32_TXDPTR)){
                                        eapolMsg4Sent = 1;
                                        Say("      ★ the engine consumed message 4's descriptors."); }
                                      else Say("      ✗ the TX engine did not move.");

                                      /* ★ THE ORACLE IS SILENCE, WHICH IS AN AWKWARD THING TO
                                       * MEASURE. There is no message 5. If the AP accepted our
                                       * message 4 it simply stops, opens the controlled port and
                                       * starts passing data; if it rejected it, it RETRIES
                                       * message 3. So: listen for 3 s and score on what does NOT
                                       * arrive -- another EAPOL-Key means rejection.
                                       *
                                       * The positive half is the Protected Frame bit. Once the
                                       * port is open the AP encrypts, so a data frame with
                                       * FC bit 14 set is traffic we could not have seen before
                                       * the handshake completed. We cannot decrypt it yet --
                                       * that is 7-5 -- but its existence is the evidence. */
                                      Say("");
                                      Say("      ★★★ k44: THE SAME THREE-SECOND LISTEN, TWICE, WITH THE");
                                      Say("      BSSID FILTER AS THE ONLY DIFFERENCE BETWEEN THEM.");
                                      Say("");
                                      Say("      k43 ran with a broadcast ping on the LAN for the whole");
                                      Say("      run and still saw zero protected frames, so 'the network");
                                      Say("      was quiet' is refuted -- the traffic was there and the");
                                      Say("      card did not report it. The suspect is the hardware BSSID");
                                      Say("      filter, still 00:00:00:00:00:00 from bring-up because b43");
                                      Say("      writes the real one only from bss_info_changed, which this");
                                      Say("      probe has no equivalent of. That predicts exactly what we");
                                      Say("      see: unicast-to-us arrives (MACFILTER_SELF matches addr1)");
                                      Say("      and beacons arrive (BEACPROMISC bypasses the match), while");
                                      Say("      group-addressed data -- which needs the match -- does not.");
                                      Say("");
                                      Say("      ⚠ A CROSS-RUN COMPARISON WOULD NOT SETTLE THIS, because the");
                                      Say("      receiver is intermittent run to run. So the control is IN");
                                      Say("      the run: window A with the zero filter, then the write,");
                                      Say("      then window B. Same channel, same association, same");
                                      Say("      traffic, one variable.");
                                      Say("");
                                      Say("      DISCRIMINATOR, stated before the run:");
                                      Say("        A group = 0 and B group > 0  -> the filter was the block.");
                                      Say("        A and B both 0               -> it is not (only) the");
                                      Say("                                        filter; the AP may not be");
                                      Say("                                        flooding to us at all.");
                                      Say("        A group > 0                  -> the theory is wrong and");
                                      Say("                                        k43's zero was something");
                                      Say("                                        else entirely.");
                                      { int tries,win; UInt32 t;
                                        for(win=0; win<2; win++){
                                          Say("");
                                          if(win==0){
                                            Say("      --- WINDOW A: BSSID filter still 00:00:00:00:00:00 ---");
                                          } else {
                                            Say("      --- programming the real BSSID, then WINDOW B ---");
                                            WriteMacBssidTemplates(mac,dst);
                                            { Str255 L;L[0]=0;PCat(L,"      MACFILTER_BSSID <- ");
                                              PCatMac(L,dst);Out(L); }
                                            Say("      (b43 main.c:4140 does exactly this on BSS_CHANGED_BSSID,");
                                            Say("       with the MAC running. Window B is otherwise identical.)");
                                          }
                                        for(tries=0; tries<6; tries++){
                                          more = RxWaitFor(base,rxBufLog,frameoffset,2,RXW_ANY_SUB,
                                                           dst,0,500,gPromptBuf,&qms,&qseen);
                                          if(!more) continue;
                                          { const UInt8 *df = gPromptBuf+frameoffset+K6_HDR_PLCP6;
                                            UInt16 dfc = le16at(df);
                                            UInt16 dlen = le16at(gPromptBuf+RXH_FRAME_LEN);
                                            const UInt8 *de;
                                            dlen = (UInt16)(dlen>K6_HDR_PLCP6 ? dlen-K6_HDR_PLCP6 : 0);
                                            winData[win]++;
                                            if(df[4] & 0x01) winGroup[win]++;
                                            if(dfc & 0x4000){
                                              protectedSeen++; winProt[win]++;
                                              /* ====== ★★★ STAGE 7-5: CCMP DECAP ====== */
                                              if(!ccmpTried){
                                                static UInt8 plain[512];
                                                ApU8 pn[6]; int kid = -1, okA, okB;
                                                ApU32 plen = 0;
                                                const ApU8 *k;
                                                int group = (df[4] & 0x01);   /* addr1 multicast? */
                                                ccmpTried = 1;
                                                Say("");
                                                Say("  ============ [7-5] ★★★ CCMP DECAPSULATION ============");
                                                Say1("      protected frame bytes (incl. PLCP) = ",
                                                     (unsigned long)le16at(gPromptBuf+RXH_FRAME_LEN));
                                                Say (group ? "      addr1 is a GROUP address -> decrypt with the GTK"
                                                           : "      addr1 is unicast -> decrypt with the TK");
                                                k = group ? gGtk : (gPtk + AP_TK_OFF);

                                                /* ⚠ AN OPEN QUESTION TURNED INTO A MEASUREMENT.
                                                 * We do not know whether the card's frame_len
                                                 * includes the 4-byte FCS. Guessing costs a
                                                 * reboot; the MIC settles it in one shot,
                                                 * because it can only verify for the correct
                                                 * length. So try both and report which. */
                                                okA = ApCcmpDecap((const ApU8*)df,(ApU32)dlen,k,
                                                                  (ApU8*)plain,&plen,pn,&kid);
                                                if(okA){
                                                  Say("      ★ MIC VERIFIED with frame_len AS GIVEN");
                                                  Say("        -> the card does NOT include the FCS.");
                                                } else if(dlen > 4){
                                                  okB = ApCcmpDecap((const ApU8*)df,(ApU32)(dlen-4),k,
                                                                    (ApU8*)plain,&plen,pn,&kid);
                                                  if(okB){
                                                    Say("      ★ MIC VERIFIED with frame_len MINUS 4");
                                                    Say("        -> the card DOES include the FCS, and");
                                                    Say("           every length in this probe that");
                                                    Say("           assumed otherwise is 4 bytes long.");
                                                    okA = 1; } }
                                                if(okA){
                                                  ccmpOk = 1;
                                                  Say1("      key id = ",(unsigned long)kid);
                                                  { Str255 L;int z;L[0]=0;PCat(L,"      PN = ");
                                                    for(z=0;z<6;z++){PCatHex(L,(unsigned long)pn[z],2);
                                                                     PCat(L," ");}Out(L); }
                                                  Say1("      plaintext bytes = ",(unsigned long)plen);
                                                  Say ("      the decrypted payload -- LLC/SNAP should");
                                                  Say ("      start AA AA 03 00 00 00 and then an");
                                                  Say ("      ethertype, 0800 for IPv4, 0806 for ARP:");
                                                  HexDump((UInt8*)plain,(int)(plen>48?48:plen),
                                                          0xFFFFFFFFUL);
                                                  if(plen>=8 && plain[0]==0xAA && plain[1]==0xAA &&
                                                     plain[2]==0x03){
                                                    ccmpSane = 1;
                                                    Say("      ★★★ AND IT IS A VALID LLC/SNAP HEADER.");
                                                    SayH("          ethertype = ",
                                                         (unsigned long)((plain[6]<<8)|plain[7]),4); }
                                                } else {
                                                  Say("      ✗ THE MIC DID NOT VERIFY, at either length.");
                                                  Say("        The CCM primitive passes RFC 3610 on the");
                                                  Say("        Mac, so suspect the framing rather than");
                                                  Say("        the cipher: the nonce (priority | A2 |");
                                                  Say("        PN), the AAD masks, or the wrong key --");
                                                  Say("        GTK for group, TK for unicast.");
                                                  Say("      the frame as received, for hand-checking:");
                                                  HexDump((UInt8*)df,48,0xFFFFFFFFUL); } }
                                              continue; }
                                            de = EapolBody(df,dlen);
                                            if(de && de[EAPOL_O_TYPE]==EAPOL_TYPE_KEY){
                                              eapolRetried++;
                                              SayH("      ⚠ another EAPOL-Key arrived, key info = ",
                                                   (unsigned long)be16at(de+EAPOL_O_KEYINFO),4); } } }
                                          Say1(win ? "      [B] data frames from the AP = "
                                                   : "      [A] data frames from the AP = ",
                                               (unsigned long)winData[win]);
                                          Say1(win ? "      [B]   of which GROUP-addressed = "
                                                   : "      [A]   of which GROUP-addressed = ",
                                               (unsigned long)winGroup[win]);
                                          Say1(win ? "      [B]   of which PROTECTED       = "
                                                   : "      [A]   of which PROTECTED       = ",
                                               (unsigned long)winProt[win]);
                                        }
                                        t = 0; (void)t; }
                                      Say1("      encrypted data frames seen = ",
                                           (unsigned long)protectedSeen);
                                      Say1("      further EAPOL-Key frames    = ",
                                           (unsigned long)eapolRetried);

                                      /* ============ [8-1] TRANSMIT AN ENCRYPTED FRAME ============ */
                                      Say("");
                                      arp81Ran = 1;   /* ★ we got here, so Oracle T is scoreable */
                                      Say("  ============ [8-1] ★★★ AN ENCRYPTED FRAME, AND AN ANSWER ============");
                                      Say("  Everything up to here proves we can READ protected traffic. Writing it");
                                      Say("  is a different claim and has never been tested. One ARP request,");
                                      Say("  CCMP-encrypted under the pairwise TK, and we look for the reply.");
                                      Say("");
                                      Say("  ★ THE ANSWER IS KNOWN BEFORE THE RUN. 192.168.1.1's hardware address");
                                      Say("    was read off the developer Mac's ARP table beforehand and compiled in");
                                      Say("    for the compare. So this does not score 'a reply arrived' -- it");
                                      Say("    compares against a value recorded elsewhere, which cannot match by");
                                      Say("    accident. Same class of evidence as Stage 7's ICMP sequence 3740.");
                                      { static UInt8 plainBody[64];
                                        static UInt8 hdr81[32];
                                        UInt16 bodyLen, hdrLen;
                                        ApU32 encLen;
                                        UInt32 t0e,t1e; int ms8;
                                        static const UInt8 bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

                                        hdrLen  = BuildDataHeaderToDs(hdr81,mac,dst,bcast);
                                        bodyLen = BuildArpRequestBody(plainBody,mac);
                                        /* PN starts at 1 for a fresh PTK: the handshake reset it. */
                                        gTxPn[0]=0;gTxPn[1]=0;gTxPn[2]=0;gTxPn[3]=0;gTxPn[4]=0;gTxPn[5]=1;

                                        /* ⚠ THREE ATTEMPTS, NOT ONE, AND THE PN ADVANCES EACH TIME.
                                         * Wireless drops frames for entirely ordinary reasons, and a
                                         * single lost ARP would read here as "the transmit path does
                                         * not work" -- a false negative costing a reboot to discover.
                                         * Advancing the PN is not optional: re-sending the same PN
                                         * under the same key is a replay, which the AP discards in
                                         * silence, so a retry that did NOT advance it would be
                                         * guaranteed to fail and would look like the first failure
                                         * repeating. */
                                        for(arp81Try=0; arp81Try<3 && !arp81Replied; arp81Try++){
                                        if(arp81Try > 0){
                                          if(!ApCcmpPnIncrement(gTxPn)){
                                            Say("  ⚠⚠ THE PN WRAPPED. Stopping: reusing a PN under one");
                                            Say("     key destroys CCMP outright. This cannot happen at");
                                            Say("     attempt 3 of a fresh key, so if you are reading");
                                            Say("     this the PN was not initialised.");
                                            break; }
                                          Say("");
                                          Say1("  --- attempt ",(unsigned long)(arp81Try+1));
                                          { Str255 L;int z;L[0]=0;PCat(L,"      PN now = ");
                                            for(z=0;z<6;z++){PCatHex(L,(unsigned long)gTxPn[z],2);
                                                             PCat(L," ");}Out(L); } }
                                        encLen = ApCcmpEncap(hdr81,(ApU32)hdrLen,
                                                             (const ApU8*)plainBody,(ApU32)bodyLen,
                                                             (const ApU8*)(gPtk+AP_TK_OFF),
                                                             gTxPn,0,0,0,0,0,(ApU8*)frm);
                                        if(arp81Try == 0){
                                          Say1("  plaintext body bytes (LLC/SNAP + ARP) = ",(unsigned long)bodyLen);
                                          Say1("  encrypted frame bytes                 = ",(unsigned long)encLen);
                                          arp81Encrypted = (encLen == (ApU32)(hdrLen + 8 + bodyLen + 8));
                                          SayOk("the encapsulated length is hdr + 8 + body + 8",arp81Encrypted);
                                          SayOk("the Protected bit is set on the outgoing frame",
                                                (frm[1] & 0x40) != 0);
                                          Say("  the frame as it will go on the air, first 48 bytes:");
                                          HexDump(frm,48,0xFFFFFFFFUL); }

                                        /* ⚠⚠⚠ k49 OMITTED THIS LINE AND THAT IS WHY IT FAILED.
                                         *
                                         * GenerateTxHdr351 writes the PLCP header, and the PLCP
                                         * carries the LENGTH the PHY will transmit: frameLen +
                                         * FCS_LEN. Every other transmit in this probe builds a
                                         * fresh txhdr first -- probe request, auth, assoc, EAPOL
                                         * msg 2, msg 4. The 8-1 block did not: it reused the
                                         * header left over from MESSAGE 4.
                                         *
                                         * So the PHY was told to send 131+4 = 135 bytes while the
                                         * DMA descriptor held 76. On the air that is our frame
                                         * followed by ~59 bytes of whatever sat after the buffer,
                                         * under an FCS computed for the wrong length. The AP drops
                                         * it at the PHY/MAC layer -- BELOW the MIC -- so there is
                                         * no ACK, no reply, and no complaint anywhere.
                                         *
                                         * ⇒ It looked exactly like a crypto failure and it was not
                                         *   a crypto failure. k49's Oracle T duly pointed at the
                                         *   AAD masks and the nonce, which were innocent. */
                                        GenerateTxHdr351(txh,frm,(UInt16)encLen,ch,
                                                         (UInt16)0xC050,B43_TXH_PHY_ANT01AUTO,1);
                                        (void)DmaControllerTxReset(cbase);
                                        { UInt32 ax = B43DmaAddressExt(ringPhys);
                                          UInt32 v = B43_DMA32_TXENABLE |
                                            ((ax<<B43_DMA32_TXADDREXT_SHIFT)&B43_DMA32_TXADDREXT_MASK);
                                          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXCTL,v);
                                          ssb_w32(gBus.bar0,cbase+B43_DMA32_TXRING,
                                                  B43DmaAddressLow(ringPhys)); }
                                        t0e = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                                        PostTxFrameAt(cbase,ringLog,0,txhPhys,
                                                      (UInt16)TXH_SIZE_351,frmPhys,(UInt16)encLen);
                                        for(ms8=0; ms8<400; ms8++){
                                          SsbSpinUs(1000);
                                          t1e = ssb_r32(gBus.bar0,cbase+B43_DMA32_TXSTATUS);
                                          if((t1e & B43_DMA32_TXDPTR)!=(t0e & B43_DMA32_TXDPTR)) break; }
                                        if((t1e & B43_DMA32_TXDPTR)!=(t0e & B43_DMA32_TXDPTR)) arp81Sent = 1;
                                        if(arp81Try == 0)
                                          SayOk("the TX engine consumed the encrypted frame",arp81Sent);

                                        /* ★ THE ACK IS THE DISCRIMINATOR k49 SHOULD HAVE HAD.
                                         * TXDPTR moving proves only that DMA read a descriptor.
                                         * An 802.11 ACK proves the AP RECEIVED the frame with a
                                         * good FCS -- which separates "it never radiated properly"
                                         * from "it radiated and the AP rejected it later". The AP
                                         * ACKs before it checks the MIC, so:
                                         *    ACK + no reply  -> the frame arrived, the MIC or the
                                         *                       forwarding is at fault
                                         *    no ACK          -> it never got there intact; crypto
                                         *                       is not the suspect at all
                                         * DrainTxStatus has decoded all of this since k12 and 8-1
                                         * simply never called it. */
                                        { int st50 = DrainTxStatus((UInt16)0xC050,600);
                                          if(!st50) Say("      (no TX status posted for this frame)");
                                          if(gTxStatAcked > arp81Acked){ arp81Acked = gTxStatAcked; } }

                                        /* Listen for the reply: a protected data frame from the AP,
                                         * addressed to us, decrypting to an ARP reply. */
                                        { int tries8;
                                          for(tries8=0; tries8<4 && !arp81Replied; tries8++){
                                            more = RxWaitFor(base,rxBufLog,frameoffset,2,RXW_ANY_SUB,
                                                             dst,0,500,gPromptBuf,&qms,&qseen);
                                            if(!more) continue;
                                            { const UInt8 *rf = gPromptBuf+frameoffset+K6_HDR_PLCP6;
                                              UInt16 rfc = le16at(rf);
                                              UInt16 rlen = le16at(gPromptBuf+RXH_FRAME_LEN);
                                              static UInt8 rplain[512];
                                              ApU32 rplen = 0; ApU8 rpn[6]; int rkid = -1;
                                              int rgroup;
                                              const ApU8 *rkey;
                                              rlen = (UInt16)(rlen>K6_HDR_PLCP6 ? rlen-K6_HDR_PLCP6 : 0);
                                              if(!(rfc & 0x4000)) continue;          /* not protected */
                                              arp81Protected++;
                                              /* ★★★ k49 TRIED THE TK ON EVERY FRAME AND LOGGED NO
                                               * ADDRESSES, which made its "0 decrypted" unreadable.
                                               * Group traffic needs the GTK -- Oracle P two hundred
                                               * lines above has chosen the key this way since k44,
                                               * and 8-1 did not carry the lesson across. Worse, with
                                               * no addr1 logged there was no way to tell "no reply
                                               * arrived" from "a reply arrived and I used the wrong
                                               * key". Both are fixed here. */
                                              rgroup = (rf[4] & 0x01);
                                              if(rgroup) arp81Group++; else arp81Unicast++;
                                              rkey = rgroup ? (const ApU8*)gGtk
                                                            : (const ApU8*)(gPtk+AP_TK_OFF);
                                              { Str255 L;L[0]=0;
                                                PCat(L,rgroup ? "      [rx group  ] a1 " : "      [rx UNICAST] a1 ");
                                                PCatMac(L,rf+4); PCat(L,"  a2 "); PCatMac(L,rf+10);
                                                Out(L); }
                                              /* ⚠ frame_len INCLUDES the FCS -- settled by the MIC in
                                               * k44, and every length here honours that. */
                                              if(rlen <= 4) continue;
                                              if(!ApCcmpDecap((const ApU8*)rf,(ApU32)(rlen-4),
                                                              rkey,
                                                              (ApU8*)rplain,&rplen,rpn,&rkid)){
                                                Say("        ...did not decrypt with that key");
                                                continue; }
                                              arp81Decrypted++;
                                              if(ParseArpReply(rplain,rplen,arp81Mac,arp81Ip)){
                                                arp81Replied = 1;
                                                Say("");
                                                Say("  ★★★ AN ARP REPLY CAME BACK, AND IT DECRYPTED.");
                                                { Str255 L;int z;L[0]=0;
                                                  PCat(L,"      sender IP  = ");
                                                  for(z=0;z<4;z++){PCatDec(L,(unsigned long)arp81Ip[z]);
                                                                   if(z<3)PCat(L,".");}Out(L);
                                                  L[0]=0;PCat(L,"      sender MAC = ");
                                                  PCatMac(L,arp81Mac);Out(L); }
                                                arp81Matched = MacEq(arp81Mac,kArpExpectMac);
                                              } } } }
                                        }  /* end of the three attempts */
                                        Say("");
                                        Say1("  ARP requests transmitted            = ",
                                             (unsigned long)(arp81Try));
                                        Say1("  protected frames seen while waiting = ",
                                             (unsigned long)arp81Protected);
                                        Say1("  of those, decrypted with our TK     = ",
                                             (unsigned long)arp81Decrypted); } }
                                  } } }
                            } else if(!(k3 & KEYINFO_MIC)){
                              Say("");
                              Say("  ⚠ THIS IS MESSAGE 1 AGAIN. No MIC, so the AP is retrying:");
                              Say("    it discarded our message 2. The MIC did not verify, which");
                              Say("    means our PTK is not the AP's PTK. Suspect, in order: the");
                              Say("    MIC input range (EAPOL header, not EAPOL-Key header), the");
                              Say("    zeroed MIC field, then the KCK.");
                            } else {
                              Say("  ⚠ An EAPOL-Key with a MIC but not message 3's flags."); } } } }
                    } } } } }
          } else {
            Say("");
            Say("  [6-2] ASSOCIATION SKIPPED -- authentication did not succeed, and an assoc");
            Say("        request from an unauthenticated station just draws status 9."); }
        }
      }
      Say("");
    }
tx_done:
    Say("");
    SayOk("the descriptors we wrote are readable back from memory",!descBad);
    SayOk("the TX engine consumed the descriptors (TXDPTR advanced)",txMoved);
    SayOk("no TX error reported",!txError);
    Say1("  RX ring arms that FAILED to reach DISABLED = ",(unsigned long)gArmFails);
    Say1("  arms that needed the LONG wait             = ",(unsigned long)gArmSlow);
    Say ("    ⚠ k10 discarded this. If failures are non-zero, the receiver working exactly once");
    Say ("      per run is explained and the fix follows from which column is non-zero:");
    Say ("        SLOW non-zero, FAILED zero -> b43's 10 ms rx_reset timeout is too short here.");
    Say ("        FAILED non-zero            -> the engine will not stop while armed, so stop");
    Say ("                                      re-arming: b43 recycles descriptors at runtime");
    Say ("                                      and never resets a live RX ring.");
    SayOk("every RX ring arm reset the engine cleanly",gArmFails==0);
    SayOk("the RX CONTROL passed -- we could hear traffic during the TX section",rxControlOk);
    Say("");
    Say1("  TX statuses posted by the microcode = ",(unsigned long)gTxStatCount);
    Say1("    of which carried OUR cookie       = ",(unsigned long)gTxStatOurs);
    Say1("    of which were ACKED               = ",(unsigned long)gTxStatAcked);
    SayOk("the MAC reported a transmission at all (XMITSTAT bit 0 ever set)",gTxStatCount>0);
    Say1("  passes reporting B43_IRQ_PHY_TXERR = ",(unsigned long)phyTxErr);
    Say1("  passes reporting B43_IRQ_MAC_TXERR = ",(unsigned long)macTxErr);
    if(phyTxErr>0){
      Say("    ⇒ ★ THE PHY IS FAILING TO TRANSMIT. b43 logs \"PHY transmission error\" on this");
      Say("      bit and restarts the controller after too many. The txhdr is verified correct");
      Say("      at every offset and the microcode does not suppress, so the fault is in the");
      Say("      PHY TX path -- TX power, the PA, or power control -- not in the frame."); }
    Say("");
    Say("");
    Say1("  BISECT: boundaries probed = ",(unsigned long)gTxProbeN);
    Say1("  boundaries that did not radiate at all = ",(unsigned long)gTxProbeSilent);
    if(gTxProbeSilent == gTxProbeN){
      Say("    ⚠⚠ NOTHING RADIATED ANYWHERE, even with the MAC forced on. That is a different");
      Say("      and bigger finding than a bisect result: dummy_transmission is not producing");
      Say("      RF under any condition this probe can create, so it is not a usable control");
      Say("      and k18's conclusion -- drawn from one dummy transmission that DID move TSSI");
      Say("      after the full init -- needs re-examining.");
    } else if(gTxProbeFirstBad < 0){
      Say("    ★★ NO BOUNDARY THAT RADIATED ERRORED. Every dummy_transmission through the whole init");
      Say("      sequence was clean, so the PHY only goes bad LATER -- during the scan, the");
      Say("      recycling, or the DMA transmits themselves. That contradicts k18 and is worth");
      Say("      more than it looks: it would mean something Stage 5 DOES, rather than");
      Say("      something Stage 4 left, breaks the PHY.");
    } else {
      Str255 L;L[0]=0;PCat(L,"    ⇒ FIRST BAD BOUNDARY = ");PCatDec(L,(unsigned long)(gTxProbeFirstBad+1));
      Out(L);
      if(gTxProbeFirstBad==0){
        Say("    ⇒ ★ BOUNDARY 1: the PHY errors BEFORE ANY PHY INIT RUNS. The fault predates");
        Say("      b43_phy_initg entirely, so Stage 4's 4,889 ported lines are NOT the place to");
        Say("      look. Suspect ApBringUp: the initvals, software_rfkill, or the radio simply");
        Say("      not being ready to transmit that early -- in which case a PHY_TXERR here may");
        Say("      be EXPECTED and the bisect needs a boundary further in to mean anything.");
      } else {
        Say("    ⇒ the stage named above introduced it, and the statements inside that stage");
        Say("      are the next bisect -- roughly a hundred lines rather than four hundred."); } }
    Say("");
    SayH("  POSITIVE CONTROL: GEN_IRQ_REASON after dummy_transmission = ",dummyIrq,8);
    SayOk("the KNOWN-GOOD TX path is free of PHY_TXERR",!dummyPhyTxErr);
    if(dummyPhyTxErr){
      Say("    ⇒ ★ THE PHY TX PATH IS FAULTY FOR EVERY TRANSMISSION, not just ours.");
      Say("      dummy_transmission shares none of the Stage 5 machinery -- no ring, no");
      Say("      descriptors, no txhdr -- so six probes of Stage 5 debugging were looking in");
      Say("      the wrong place. This belongs to Stage 4, which never decoded this register.");
      Say("      Next: bisect b43_phy_initg. It passed fifteen oracles, but every one of them");
      Say("      checked register VALUES; none checked that a frame leaves the antenna intact.");
    } else if(phyTxErr>0){
      Say("    ⇒ The known-good path is clean and ours is not, so the fault is in what WE add:");
      Say("      the DMA ring or the b43_txhdr. Every field of both is verified, so the next");
      Say("      step is to diff the two paths' PHY state rather than re-read the frame."); }
    Say("");
    SayH("  NEGATIVE CONTROL: GEN_IRQ_REASON after a quiet second = ",quietIrq,8);
    SayOk("the quiet second produced NO PHY_TXERR -- so the bit is attributable",!quietPhyTxErr);
    SayOk("the quiet second left TSSI at the sentinel -- so k15's reading was ours",!quietTssiMoved);
    if(quietPhyTxErr){
      Say("    ⇒ ★ PHY_TXERR IS A BYSTANDER. It appears without any transmission, so k12");
      Say("      through k15 were reading a background condition as a finding. Those");
      Say("      conclusions are WITHDRAWN. The transmit path has no error signal pointing at");
      Say("      it, and the open question returns to why a correct frame, radiated at");
      Say("      measurable power, is not acknowledged."); }
    else if(phyTxErr>0){
      Say("    ⇒ PHY_TXERR appears ONLY when we transmit, so it is genuinely ours and the");
      Say("      next step -- porting recalc_txpower and adjust_txpower -- is justified rather");
      Say("      than speculative."); }
    SayOk("the PHY measured real transmit power (TSSI moved off 0x7F7F)",tssiMoved);
    SayOk("no PHY transmit error was reported",phyTxErr==0);
    SayOk("★★ an access point ACKNOWLEDGED our frame",gTxStatAcked>0);
    if(ackedOn>=0){
      Str255 L;L[0]=0;PCat(L,"  ⇒ THE ANTENNA THAT WORKS IS ");PCat(L,kAntNameV[ackedOn]);Out(L);
      Say(ackedOn==2
          ? "    which is b43's own ANT01AUTO, so the antenna was never the problem."
          : "    ⇒ AUTO SELECTION WAS THE BUG. b43 asks for ANT01AUTO because it assumes two");
      if(ackedOn!=2)
        Say("      connected antennas; this machine has one, and TX was radiating into the other."); }
    else if(gTxStatCount>0){
      Say("  ⇒ NO ANTENNA GOT AN ACK. All three radiated without acknowledgement, so the");
      Say("    antenna hypothesis is spent and the frame itself is back to being the suspect --");
      Say("    read the full txhdr dump above, particularly the PLCP at +100."); }
    if(gTxStatCount==0){
      Say("    ⇒ THIS IS THE FINDING. The DMA engine consumed the descriptors and the MAC");
      Say("      never reported a transmission, so the frame never reached the air. The");
      Say("      contents of the probe request are therefore NOT the suspect -- the txhdr");
      Say("      or the MAC's TX gating is. Every run since k7 has been chasing the wrong");
      Say("      half because TXDPTR was the only thing being watched."); }
    else if(gTxStatOurs>0 && gTxStatAcked==0){
      Say("    ⇒ The MAC DID transmit our frame and nothing acknowledged it. Now the frame");
      Say("      contents ARE the suspect, and supp_reason above says whether the microcode");
      Say("      suppressed it or it simply went unanswered."); }
    Say1("  probe responses to the BROADCAST pass = ",(unsigned long)bcastReplies);
    if(bcastReplies>0){
      Say("    ⇒ ★★★ THE FRAME IS UNDERSTOOD ON AIR. A broadcast probe request needs no ACK,");
      Say("      and access points answered it. So transmission WORKS, and what has been");
      Say("      failing all along is the unicast path -- the ACK, or the address matching.");
    } else if(gTxStatCount>0 && rxControlOk){
      Say("    ⇒ Broadcast drew nothing either, so it is not the ACK mechanism: the frame is");
      Say("      not being understood on air at all, despite radiating measurable power.");
      Say("      PHY_TXERR is then pointing at a real modulation or timing fault -- though b43");
      Say("      tolerates 1000 of those per 15 s, so it treats them as soft.");
      Say("      ★ AND THIS TIME THE RECEIVER WAS PROVED LIVE, so the statement stands.");
    } else if(gTxStatCount>0){
      Say("    ⇒ Broadcast drew nothing, BUT THE RECEIVER WAS DEAF, so this says NOTHING.");
      Say("      ⚠⚠ k20 PRINTED THE OPPOSITE OF THIS AND IT WAS WRONG. It concluded \"the frame");
      Say("         is not being understood on air at all\" from a zero counted on a ring the");
      Say("         engine had stopped filling. Do not conclude anything about the air from a");
      Say("         pass whose RXDPTR never moved."); }
    Say("");
    Say("  === k29 / STAGE 6-0 PREREQUISITE: THE PROMPT RECEIVE PATH ===");
    Say1("  passes where RxWaitFor caught the reply = ",(unsigned long)promptHits);
    Say1("  passes run                              = ",(unsigned long)K17_NPASS);
    SayOk("★ PROMPT RX: a reply was caught on EVERY pass",promptHits==K17_NPASS);
    if(promptHits==K17_NPASS){
      Say("    ⇒ Stage 6 can proceed. An auth response arrives in single-digit milliseconds and");
      Say("      an assoc response not much later; both are now catchable. k28 caught 5 replies");
      Say("      in run 1 and 0 in run 2 with the same code, which is what a wrapped ring looks");
      Say("      like -- that ambiguity would have made every Stage 6 result unreadable.");
    } else if(promptHits>0){
      Say("    ⇒ INTERMITTENT. Better than the 1.1 s scan but not yet a foundation. Before");
      Say("      building auth on it, find out whether the misses are timeouts (the AP did not");
      Say("      answer) or predicate failures (it did and we rejected it) -- the per-pass");
      Say("      'frames walked' count separates those.");
    } else {
      Say("    ⇒ NOTHING CAUGHT PROMPTLY. Do NOT start Stage 6 on this. Either the predicate is");
      Say("      wrong or the reply genuinely is not arriving, and the ACK line below says which:");
      Say("      ACKed with no reply means the AP heard us and we are still deaf to its answer.");
    }
    Say("");
    Say("  === THE INTERMITTENT RECEIVER, TRACKED ACROSS RUNS ===");
    Say1("  the scan needed a retry this run = ",(unsigned long)scanRetried);
    if(scanRetried){
      SayOk("and the retry recovered it (so the receiver just needed time)",scanRetryWorked);
      Say ("    Bank this run's answer next to k33a and k35. Two voids in five runs is a rate,");
      Say ("    not a coincidence, and the retry is here to turn each occurrence into evidence");
      Say ("    rather than a lost reboot.");
    } else {
      Say("  (the first scan heard traffic, so this run says nothing about the intermittent)"); }

    Say("");
    Say("  === ORACLE J / STAGE 6-1: OPEN SYSTEM AUTHENTICATION ===");
    oracleJ = authGot && (authAlg==AUTH_ALG_OPEN)
                      && (authSeq==AUTH_SEQ_RESPONSE)
                      && (authStatus==AUTH_STATUS_SUCCESS);
    SayOk("an authentication response came back at all",authGot);
    if(authGot){
      SayH("    algorithm = ",(unsigned long)authAlg,4);
      SayH("    sequence  = ",(unsigned long)authSeq,4);
      SayH("    status    = ",(unsigned long)authStatus,4); }
    SayOk("★★★ AUTHENTICATED -- algorithm 0, sequence 2, status 0",oracleJ);
    if(oracleJ){
      Say("    ⇒ The AP has accepted us as an authenticated station. That is a state held on the");
      Say("      AP, not just a frame we heard: it will now entertain an association request from");
      Say("      this MAC and refuse one from a station that has not authenticated.");
      Say("    ⇒ NEXT (6-2): the association request, and an AID in the response.");
    } else if(authGot){
      Say("    ⇒ The AP answered and REFUSED. The status code is the finding -- 802.11-2020");
      Say("      Table 9-50 names them; 1 is unspecified failure, 13 is unsupported algorithm,");
      Say("      17 is 'too many associated stations'. Read it before changing anything: an");
      Say("      explicit refusal is far more informative than silence and says the frame was");
      Say("      well formed enough to be parsed and judged.");
    } else {
      Say("    ⇒ Nothing came back. Check the ACK on the auth frame above before suspecting the");
      Say("      frame contents: ACKed-but-silent is the AP declining, unACKed is us."); }

    Say("");
    Say("  === ORACLE K / STAGE 6-2: ASSOCIATION ===");
    oracleK = assocGot && (assocStatus==ASSOC_STATUS_SUCCESS) && (assocAid!=0);
    SayOk("an association response came back at all",assocGot);
    if(assocGot){
      SayH("    status = ",(unsigned long)assocStatus,4);
      Say1("    AID    = ",(unsigned long)assocAid); }
    SayOk("★★★★ ASSOCIATED -- status 0, and the AP allocated us an AID",oracleK);
    if(oracleK){
      Say("    ⇒ ★★★★★★ STAGE 6 IS COMPLETE. The AID is the proof: it is a number the AP");
      Say("      ALLOCATED and now HOLDS for this MAC, so this is a state change on the AP and");
      Say("      not merely a frame that parsed. We are an associated station on the target network.");
      Say("    ⇒ The link will NOT pass data yet and the AP will deauthenticate us shortly --");
      Say("      that is correct and expected. WPA2 requires the 4-way handshake before the");
      Say("      controlled port opens, and that is Stage 7.");
    } else if(assocGot){
      Say("    ⇒ REFUSED, and the status code is the finding. The ones that matter here:");
      Say("        9  not authenticated        -- the AP lost our auth state; re-auth first");
      Say("        12 association denied       -- outside the AP's policy, e.g. a MAC filter");
      Say("        31 robust management policy -- PMF Required, and our RSN caps say no PMF");
      Say("        40 invalid element          -- an IE is malformed; read the hexdump above");
      Say("        43 invalid pairwise cipher  -- the RSNE's cipher is not one the AP offers");
      Say("      31 and 43 are AP-configuration answers; 40 is ours. Check UniFi before code.");
    } else if(authGot && oracleJ){
      Say("    ⇒ Authenticated but no association response. Since the AP demonstrably knows this");
      Say("      MAC, silence points at the request's CONTENTS -- the RSNE is the first suspect,");
      Say("      because a malformed one is the difference between this frame and the auth frame");
      Say("      that did get answered."); }

    Say("");
    Say("  === ORACLE L / STAGE 7-1: EAPOL-KEY MESSAGE 1 AND THE PTK ===");
    oracleL = eapolMsg1 && gPmkReady && gPtkReady;
    SayOk("the PMK derived on the G4 (PBKDF2, 4096 iterations)",gPmkReady);
    SayOk("EAPOL-Key message 1 arrived and parsed as message 1",eapolMsg1);
    SayOk("★★★ a PTK was derived from the AP's ANonce",oracleL);
    if(oracleL){
      Say("    ⇒ The AP started the 4-way handshake, which means it accepted our RSNE and");
      Say("      considers us an RSN station. We hold a PTK computed from its ANonce, our");
      Say("      SNonce and the PMK. Whether it is the SAME PTK the AP computed is NOT yet");
      Say("      known and cannot be known from this side -- the MIC on message 2 is the only");
      Say("      thing that answers it, and that is Stage 7-2.");
      Say("    ⇒ Expect a deauthentication shortly. We never answer message 1, so the AP will");
      Say("      retry it and then give up. That is correct for this increment.");
    } else if(eapolMsg1 && !gPtkReady){
      Say("    ⇒ Message 1 parsed but the PTK came out all zeros, which means the derivation");
      Say("      did not run. Since kdf_test.c passes fourteen known-answer tests on the Mac,");
      Say("      suspect the Retro68 build of it rather than the algorithm -- long arithmetic");
      Say("      and struct padding are the differences between the two compilers.");
    } else if(gPmkReady && !eapolMsg1){
      Say("    ⇒ The PMK is fine and no message 1 came. Read the [7-1] block: a data frame that");
      Say("      was not EAPOL, or no data frame at all, are different problems.");
    } else {
      Say("    ⇒ The PMK itself did not derive. That is arithmetic, not radio: check that");
      Say("      ap_psk_config.h has a password in it and that the length matches."); }

    Say("");
    Say("  === ORACLE M / STAGE 7-2: MESSAGE 2 AND THE MIC ===");
    oracleM = eapolMsg2Sent && eapolMsg3;
    SayOk("message 2 was transmitted",eapolMsg2Sent);
    SayOk("★★★★★ MESSAGE 3 CAME BACK -- our MIC verified, the PTK is right",oracleM);
    if(oracleM){
      Say("    ⇒ This is the first proof that our key AGREES with the AP's. Both sides derived");
      Say("      the PTK independently and never exchanged it, so the AP recomputing our MIC and");
      Say("      accepting it validates the PMK, the PRF, the min||max ordering, the NUL in the");
      Say("      label and the KCK simultaneously. kdf_test.c's fourteen known-answer tests said");
      Say("      the arithmetic was right; this says it is the right arithmetic.");
      Say("    ⇒ NEXT (7-3): verify message 3's own MIC, check the AP's RSNE against the one we");
      Say("      sent -- that comparison is the downgrade defence and is the reason skipping the");
      Say("      beacon parser cost us no security -- and unwrap the GTK, which needs AES.");
    } else if(eapolMsg2Sent && eapolMsg1){
      Say("    ⇒ Message 2 went out and message 3 did not come back. A bad MIC is discarded in");
      Say("      SILENCE -- the AP neither answers nor complains, it just retries message 1. So");
      Say("      this is most likely a wrong PTK rather than a malformed frame. In order:");
      Say("        - the MIC input range: it starts at the EAPOL header, NOT the EAPOL-Key");
      Say("          header. The published 802.11i text is wrong here and says otherwise.");
      Say("        - the MIC field must be zeroed over that range before hashing");
      Say("        - the KCK is the FIRST 16 bytes of the PTK, not the last");
      Say("        - the replay counter must be echoed from message 1 exactly");
      Say("      The [7-2] block above says whether what came back was message 1 retried, which");
      Say("      would confirm the AP rejected us rather than lost us.");
    } else if(!eapolMsg2Sent){
      Say("    ⇒ Message 2 never left. That is a transmit problem, not a crypto one, and Stage");
      Say("      5's oracles above will say which."); }

    Say("");
    Say("  === ORACLE N / STAGE 7-3: MESSAGE 3 VERIFIED, UNWRAPPED, COMPARED ===");
    oracleN = gtkUnwrapped && gtkOk;
    SayOk("the Key Data unwrapped under RFC 3394 with our KEK",gtkUnwrapped);
    SayOk("★★★★★ the GTK is in hand and the AP's RSNE matched ours",oracleN);
    if(oracleN){
      Say("    ⇒ Three things at once. The KEK is right, or the unwrap's integrity check would");
      Say("      have rejected it -- and that check is tested against a tampered wrap and a wrong");
      Say("      KEK in kdf_test.c, so it is known to reject rather than merely known to pass.");
      Say("      The AP's own RSNE offers the cipher and AKM we negotiated, and we hold");
      Say("      the group key. ⚠ k34 claimed this comparison was the downgrade defence;");
      Say("      it is not, and that text is corrected in the [7-3] block above.");
      Say("    ⇒ NEXT (7-4): message 4, after which the AP opens the controlled port. Then 7-5");
      Say("      installs the TK and GTK in the card's key table and adds CCMP framing.");
    } else if(gtkUnwrapped){
      Say("    ⇒ Unwrapped but the check failed. If the RSNE mismatched, that is a REAL security");
      Say("      finding and not a bug to work around: something is advertising one cipher and");
      Say("      negotiating another. Read the RSNE hexdump above against BuildRsne().");
    } else if(eapolMsg3){
      Say("    ⇒ Message 3 arrived but nothing came out of it. The MIC check and the unwrap are");
      Say("      reported separately above, and they fail for different reasons: a bad MIC means");
      Say("      the frame is not from who we think, a bad unwrap means the KEK is wrong. The KEK");
      Say("      is PTK bytes 16..31 -- a wrong offset there would still let message 2's MIC pass,");
      Say("      because that uses the KCK at bytes 0..15."); }

    Say("");
    Say("  === ORACLE O / STAGE 7-4: MESSAGE 4 AND THE CONTROLLED PORT ===");
    /* ⚠ SCORED ON WHAT DID NOT HAPPEN, which needs stating plainly because it is unusual.
     * There is no message 5. An AP that accepts message 4 goes quiet and opens the port; one
     * that rejects it RETRIES message 3. So the oracle is: we sent it, and no further EAPOL-Key
     * came back. Encrypted traffic appearing is corroboration, not the test -- a quiet network
     * might legitimately send us nothing in three seconds. */
    oracleO = eapolMsg4Sent && (eapolRetried == 0);
    SayOk("message 4 was transmitted",eapolMsg4Sent);
    Say1("  further EAPOL-Key frames from the AP = ",(unsigned long)eapolRetried);
    Say1("  encrypted data frames seen           = ",(unsigned long)protectedSeen);
    SayOk("★★★★★★ THE HANDSHAKE COMPLETED -- no retry, the AP accepted message 4",oracleO);
    if(oracleO && protectedSeen > 0){
      Say("    ⇒ ★★★★★★★ AND THE AP IS SENDING US ENCRYPTED TRAFFIC. The Protected Frame bit is");
      Say("      set on data frames we could not have seen before the handshake completed. The");
      Say("      controlled port is open. STAGE 7's handshake is done.");
      Say("    ⇒ NEXT (7-5): install the TK and GTK in the card's key table and add CCMP framing,");
      Say("      so those frames can be decrypted rather than merely counted.");
    } else if(oracleO){
      Say("    ⇒ Accepted, but nothing encrypted arrived in three seconds. That is consistent");
      Say("      with a quiet network and is NOT a failure -- the absence of a retry is the real");
      Say("      evidence. Worth re-running to see whether protected frames show up at all,");
      Say("      because 7-5 will need some to decrypt.");

    } else if(eapolMsg4Sent){
      Say("    ⇒ Message 4 went out and the AP RETRIED anyway, so it rejected our MIC. Message 2's");
      Say("      MIC passed, so the KCK is right and the input range is right -- which leaves the");
      Say("      three things that differ in message 4: the SECURE bit carried over from message");
      Say("      3, the replay counter (it must come from message 3, NOT message 1), and the zero");
      Say("      nonce."); }

      Say("");
      Say("  === ORACLE P / STAGE 7-5: CCMP DECAPSULATION ===");
      oracleP = ccmpOk;
      Say1("  protected frames seen        = ",(unsigned long)protectedSeen);
      SayOk("a protected frame was decrypted and its MIC VERIFIED",oracleP);
      SayOk("  ...and the plaintext is a valid LLC/SNAP header",ccmpSane);
      if(oracleP && ccmpSane){
        Say("    ⇒ ★★★★★★★ STAGE 7 IS COMPLETE. The MIC verifying is a cryptographic oracle --");
        Say("      it cannot pass by accident. It proves the TK or GTK is right, the nonce is");
        Say("      right, every AAD mask is right, and the PN was read out of the CCMP header");
        Say("      correctly. And the plaintext being LLC/SNAP proves we decrypted a real packet");
        Say("      rather than 1500 bytes of plausible-looking noise.");
        Say("    ⇒ The link now carries traffic this machine can read. NEXT is Stage 8: the Open");
        Say("      Transport DLPI driver, which is glue rather than 802.11.");
      } else if(oracleP){
        Say("    ⇒ Decrypted and authenticated, but the plaintext does not begin AA AA 03. The");
        Say("      MIC cannot pass on wrong plaintext, so the frame IS genuine -- most likely it");
        Say("      is simply not LLC/SNAP encapsulated. Read the hexdump rather than assuming.");
      } else if(protectedSeen > 0){
        Say("    ⇒ Encrypted traffic arrived and did not decrypt. The CCM primitive passes RFC");
        Say("      3610 on the Mac including a tampered-AAD rejection, so the cipher is not the");
        Say("      suspect -- the framing is. In order: the right key (GTK for group, TK for");
        Say("      unicast), the nonce, the AAD masks, the FCS length question above.");
      } else {
        Say("    ⇒ NO PROTECTED FRAMES ARRIVED, so there was nothing to decrypt and this is");
        Say("      untested rather than failing. ⚠ 'THE NETWORK IS QUIET' IS NO LONGER AN");
        Say("      AVAILABLE EXCUSE -- k43 ran with a broadcast ping on the LAN across the whole");
        Say("      run and still saw zero. Read Oracle Q: it says whether the BSSID filter was");
        Say("      what stopped them."); }

    Say("");
    Say("  === ORACLE Q / k44: WAS THE BSSID FILTER BLOCKING GROUP TRAFFIC? ===");
    /* ⚠ SCORED AS A DIFFERENCE, NOT A LEVEL. Window B alone proving traffic exists would not
     * tell us the write caused it, because the receiver on this card is intermittent run to
     * run -- three hypotheses have already died on that. A > 0 is therefore as informative a
     * result as B > 0, and it REFUTES rather than confirms. */
    oracleQ = (winGroup[0] == 0) && (winGroup[1] > 0);
    Say1("  [A] zero filter -- data frames from the AP = ",(unsigned long)winData[0]);
    Say1("  [A]                  of which group-addressed = ",(unsigned long)winGroup[0]);
    Say1("  [B] real filter -- data frames from the AP = ",(unsigned long)winData[1]);
    Say1("  [B]                  of which group-addressed = ",(unsigned long)winGroup[1]);
    SayOk("★★★★★ THE BSSID FILTER WAS THE BLOCK -- A saw no group frames, B did",oracleQ);
    if(oracleQ){
      Say("    ⇒ One write, inside one run, with the association and the channel and the");
      Say("      traffic all held constant. This is the missing half of b43's");
      Say("      b43_write_mac_bssid_templates: bring-up writes zeros because nothing is");
      Say("      associated yet, and bss_info_changed writes the real BSSID afterwards. We had");
      Say("      only ever done the first. Fold the second into the association path.");
    } else if(winGroup[0] > 0){
      Say("    ⇒ ★ THE THEORY IS WRONG, and this is worth more than a confirmation would have");
      Say("      been. Group-addressed frames came through with the filter STILL ZEROED, so the");
      Say("      hardware was never dropping them and k43's zero has a different cause. Do not");
      Say("      carry the BSSID write forward as a fix -- it is now an unjustified change.");
    } else if(winData[1] > 0 || winData[0] > 0){
      Say("    ⇒ Data frames from the AP arrived but none were group-addressed, in either");
      Say("      window. So the receiver is alive and the filter is not the discriminator: the");
      Say("      AP is not flooding broadcast to us. UniFi filters multicast and broadcast per");
      Say("      WLAN, and a station that has never sent an uplink data frame or ARPed is a");
      Say("      candidate for proxy-ARP suppression. Check the target WLAN's multicast");
      Say("      settings before writing any more card code.");
    } else {
      Say("    ⇒ NOTHING arrived from the AP in either window, group or unicast. That is the");
      Say("      intermittent receiver rather than a filter question, and Oracle Q is UNRUN --");
      Say("      not failed. Re-run before drawing anything from it."); }

    Say("");
    Say("  === ORACLE R / k47: WHICH ACTION REVIVES A DEAD RECEIVER? ===");
    Say1("  probe dwells that read ZERO (of ",(unsigned long)K47_PROBE);
    Say1("    ) = ",(unsigned long)abDeadA);
    if(abDeadA < K47_PROBE){
      Say("  [--] the fault was not present, so the bisect did not run. NOT a pass: this run");
      Say("       is silent on the cause. Relaunch warm from the Finder to provoke it -- warm");
      Say("       fails ~2x more often and costs no reboot.");
    } else if(bisectWinner < 0){
      Say("  [!!] ⚠⚠ THE FAULT WAS PRESENT AND NOTHING IN THE LADDER REVIVED IT.");
      Say("    ⇒ That is a strong and unwelcome result: nine rungs including a full RxRingArm,");
      Say("      a MacSuspend/MacEnable cycle, a dummy transmission and every TX DMA controller");
      Say("      all failed. Whatever the transmit section does to recover is therefore NOT in");
      Say("      this list, and the next step is to bisect the transmit section itself rather");
      Say("      than to keep proposing single registers.");
    } else if(bisectWinner == 0){
      Say("  [!!] ⚠⚠ IT RECOVERED ON THE NO-OP RUNG -- 500 ms of nothing at all.");
      Say("    ⇒ So the receiver heals with TIME, not with any action, and EVERY previous");
      Say("      theory that credited an action is confounded, including the TX-controller one");
      Say("      this run was built to test. The question becomes what has a ~seconds time");
      Say("      constant: a PLL, an AGC settle, or the radio coming out of a low-power state.");
    } else {
      Say1("  [ok] ★★★★★★ RECOVERED AT RUNG ",(unsigned long)bisectWinner);
      Say("    ⇒ Read the rung name in the ladder above: that single action is what revives a");
      Say("      receiver which had just been dead for four consecutive dwells, and the no-op");
      Say("      rung before it did NOT revive it, so this is the action and not the elapsed");
      Say("      time.");
      if(bisectWinner >= 2 && bisectWinner <= 5){
        Say("    ⇒ ★ IT IS A TX DMA CONTROLLER, which makes this a porting omission rather than");
        Say("      a hardware quirk: b43_dma_init programs all five and we program one. The fix");
        Say("      is to set up every TX ring at [5-3a], exactly as b43 does, and then confirm");
        Say("      the fault rate goes to zero -- including on WARM relaunches, which is the");
        Say("      case Stage 8 actually needs."); } }
    Say("");
#endif /* AP_APP_DRIVES_RADIO */

    Say("  === ORACLE U / STAGE 8-5b: WILL OPEN TRANSPORT LOAD AND DRIVE OUR DRIVER? ===");
    /* ⚠ THREE OUTCOMES, NOT TWO, and the middle one is the most likely. "Registered but never
     * called" is not a failure -- OT may not drive a port until a user selects it in a control
     * panel -- and scoring it as one would send the next increment chasing a driver bug that does
     * not exist. It is called out explicitly for the same reason Oracle R's inconclusive branch
     * is: the default must not be a cheerful pass. */
    SayOk("EnetShimLib is present and usable",shimLibOk);
    SayDrv("★★★ the shim ACCEPTED our registration",shim85bInstalled);
    Say1("  selector calls our driver received = ",(unsigned long)shim85bCalls);
    /* ★★★ 8-5g: TWO INDEPENDENT PATHS TO THE SAME SIX BYTES.
     * The driver walked the Name Registry, mapped its own BAR0 and read the SPROM inside its own
     * fragment. The application did the same thing separately during bring-up. They share no
     * state, so agreement is real evidence that both mappings are right -- and disagreement would
     * be worth far more than either number alone. */
    /* ★★★ 8-2a ORACLE: TWO FRAGMENTS, ONE CARD, SAME BACKPLANE.
     * The driver enumerated the cores from its own mapping before the app had touched the card;
     * the app enumerated them again afterwards through a separate one. Agreement on all five
     * numbers is the evidence Stage 8-2 rests on -- it says two fragments can own this card in
     * sequence without fighting, which is what every remaining step assumes. */
    /* ★★★ 8-2b ORACLE: the first write the driver made, scored on a READ-BACK.
     * SsbCoreEnable returns void, so "we wrote the registers" is all the call itself can tell
     * us. TMSLOW is read afterwards and checked for CLOCK set and RESET clear -- a core that did
     * not come up would otherwise be discovered much later, somewhere unrelated. */
    /* ★★★ 8-2c ORACLE: the driver's own firmware, checked against the app's.
     * Both fragments uploaded microcode to the same chip from their own copies of the same blobs.
     * The revision is the thing to compare: 478 is the v4 generation this port requires, and
     * anything <= 0x128 is the v3 firmware that cost Stage 5 most of its schedule. */
    SayDrv("★★★ the driver uploaded its OWN firmware and it came up",shimFwOk);
    if(shimFwOk){
      Str255 L; L[0]=0;
      PCat(L,"    driver's ucode rev = "); PCatDec(L,(unsigned long)shimFwRev);
#if AP_APP_DRIVES_RADIO
      PCat(L,"    app's = ");              PCatDec(L,(unsigned long)gFwRev);
#endif
      Out(L);
#if !AP_APP_DRIVES_RADIO
      /* ⚠⚠ NOT COMPARABLE, AND SAYING SO IS THE POINT. The app no longer uploads firmware, so
       * its side of this cross-check was never filled in. k74 compared the driver's real 478
       * against an uninitialised 0, printed "The two uploads produced DIFFERENT revisions", and
       * sent the reader to check embed-fw.sh -- in a run where everything actually passed.
       * A cross-check whose second input has gone away must announce that it is unavailable,
       * not score its absence as a disagreement. */
      Say("  [--] NOT COMPARABLE: the app's own upload no longer runs.");
      Say("      The driver's revision above stands on its own -- 478 is the v4 generation this");
      Say("      port requires, and anything <= 0x128 is refused before a word is uploaded.");
#else
      if(shimFwRev == gFwRev){
        Say("  [ok] ★★★★★★ SAME REVISION. The driver carries its own copy of the microcode,");
        Say("       compiled in rather than read from the app's resources, and the chip came up");
        Say("       on it. ⇒ NEXT (8-2d): PHY and radio init.");
      } else {
        Say("  [!!] ⚠ The two uploads produced DIFFERENT revisions. Both read the same blobs, so");
        Say("       suspect the driver's copy first: embed-fw.sh writes the Rez and the C header");
        Say("       from the same files, but only if it was re-run after the firmware changed."); }
#endif
    } else if(shimFwErr == 1){
      Say("    ⇒ Not attempted -- the core was not up, so there was nothing to upload onto.");
    } else if(shimFwRev != 0 && shimFwRev <= 0x128){
      Say1("    ⇒ ⚠ v3 FIRMWARE (rev <= 296). rev = ",(unsigned long)shimFwRev);
      Say("      This port is b43-derived and b43 refuses that generation outright. The driver's");
      Say("      compiled-in copy was cut from the wrong directory -- re-run embed-fw.sh against");
      Say("      ../firmware/v4 and rebuild the shim.");
    } else {
      Say("    ⇒ The microcode did not come up. GEN_IRQ_REASON should read 1 and the revision");
      Say("      should be non-zero; check which of the two failed above."); }

    /* ★★★ 8-2d-1 ORACLE: CAN THE DRIVER TALK?
     *
     * This is a LOGGING result and it is scored separately from every bring-up result above,
     * deliberately. The 8-5g/8-2a/8-2b/8-2c facts came back through bespoke accessors and do not
     * depend on this at all -- so a failure here says the narration pipe is broken, and says
     * nothing whatever about the radio. Conflating the two would be a new way to misread a log,
     * which is the opposite of what this increment is for.
     *
     * The discriminator was stated before the run: the driver emits lines through all four
     * formatter paths, and the composite Out()+PCat+PCatHex path is the one that decides the
     * design. If Say/SayH/Say1 arrive and the composite lines do not, the sink is wrong and
     * ap_ilog.h's pointer ring was the right instinct after all. If all four arrive, then the 34
     * direct Out() call sites waiting in ap_phy_initg.h will arrive too, and 8-2d-2 can move the
     * real narration across without a second hardware cycle to find that out. */
    Say("");
    /* ⚠ THREE OUTCOMES, NOT TWO. A pass/fail headline over an UNRUN test is how k65 came to be
     * reported as a logging defect when the logging code had never been loaded. "[??]" is not
     * decoration: it is the difference between a closed question and an open one. */
    if(shimStaleSig || (shimHaveBuild && shimBuild != (UInt32)AP_SHIM_BUILD_EXPECTED))
      Say("  [??] ★★★ 8-2d-1: UNRUN -- the shim that was loaded is not the one built with this app");
    else
      SayDrv("★★★ 8-2d-1: the DRIVER narrated its own bring-up into this log",shimLogReplayed);
    if(shimLogReplayed){
      Str255 L; L[0]=0;
      /* ⚠ The ring's CAPACITY is deliberately not printed here. The app would have to hardcode
       * ap_ring.h's constant, and a constant copied across a fragment boundary is one that goes
       * stale silently -- this project has lost three recommendations to exactly that. The
       * dropped count is the authoritative answer to "was it big enough", and it comes from the
       * driver that actually enforced the limit. */
      PCat(L,"    "); PCatDec(L,(unsigned long)shimLogN);
      PCat(L," lines, "); PCatDec(L,(unsigned long)shimLogBytes);
      PCat(L," bytes, "); PCatDec(L,(unsigned long)shimLogDropped); PCat(L," dropped"); Out(L);
      if(shimLogDropped == 0){
        Say("  [ok] ★★★★★ THE DRIVER HAS A VOICE. Look for the D| lines above and check that a");
        Say("       COMPOSITE one is among them -- \"D|   [8-5g] MAC xx:xx:...\" and the chip/rev/");
        Say("       cores line. Those are built with PCat+PCatHex and handed to Out() directly.");
        Say("       ⚠ THEY ARE THE POINT OF THIS INCREMENT. A pointer-based ring would have");
        Say("       carried the Say() lines perfectly and dropped exactly those -- and the log");
        Say("       would have looked complete. 34 such call sites wait in ap_phy_initg.h.");
        Say("       ⇒ NEXT (8-2d-2): the driver applies the initvals, narrating through this.");
      } else {
        Say("  [!!] ⚠ Lines were DROPPED, so the narration above is truncated at the TAIL.");
        Say("       The mechanism works; the ring is too small. Raise AP_RING_BYTES in ap_ring.h");
        Say("       (and AP_RING_LINES if the count hit that instead -- the two limits are");
        Say("       enforced independently). Re-run before drawing any conclusion from the last");
        Say("       line above: it is where the ring filled, NOT where the driver stopped."); }
    } else if(shimStaleSig || (shimHaveBuild && shimBuild != (UInt32)AP_SHIM_BUILD_EXPECTED)){
      /* ⚠⚠ THIS BRANCH EXISTS BECAUSE k65 WAS SCORED WRONG WITHOUT IT. The driver ran perfectly
       * -- core up, firmware rev 478 -- and the only thing missing was an export added in the
       * very build that was not loaded. The log said "the driver produced no lines", which reads
       * as a code defect, and the code was never in the machine.
       *
       * A result obtained against the wrong binary is not a result. Scoring it either way would
       * retire a question that was never asked, and on this project an untested question that
       * looks closed is the most expensive thing there is. */
      Say("    ⇒ ⚠⚠ UNRUN -- THE WRONG SHIM WAS LOADED. Not a failure, not a pass.");
      if(shimHaveBuild){
        Str255 L; L[0]=0;
        PCat(L,"      loaded build "); PCatDec(L,(unsigned long)shimBuild);
        PCat(L,", this app was built against "); PCatDec(L,(unsigned long)AP_SHIM_BUILD_EXPECTED);
        Out(L);
      } else {
        Say("      AirPortShimGetBuild is absent, so the fragment predates 8-2d-1b entirely.");
      }
      Say("      The export table above dates it: every OLD export resolved and the NEW ones");
      Say("      did not. CFM registers Extensions AT BOOT -- replace AirPortShim in the");
      Say("      Extensions folder, RESTART, and re-run. Nothing about the narration path has");
      Say("      been tested by this run.");
    } else if(shimLogN == 0 && shimOpenCalls == 0){
      /* ⚠⚠ NOT A FAILURE. THE TEST DID NOT RUN. ApShimNarrateOpen is called from exactly one
       * place -- EnetHAL_Open -- and OT never got there, so there was never anything to drain.
       * Scoring this as "the narration is broken" would retire a question that was never asked,
       * and a retired question that was never tested is the most expensive object on this
       * project. Whatever stopped OT short of Open is the thing to fix; 8-2d-1 is UNRUN. */
      Say("    ⇒ ⚠⚠ VACUOUS, NOT FAILED. EnetHAL_Open was called ZERO times, and that is the");
      Say("      only place the driver narrates from. Nothing about the logging path was");
      Say("      exercised by this run -- do NOT record 8-2d-1 as failing.");
      Say("      Read the selector order above: whatever stopped OT before Open is the question.");
    } else if(shimLogN == 0){
      Say("    ⇒ The driver produced no lines at all. The candidates, in order of likelihood:");
      if(shimOpenCalls > 0){
        Say1("      1. RULED OUT -- EnetHAL_Open DID run, times = ",
             (unsigned long)shimOpenCalls);
        Say("         So ApShimNarrateOpen was reached and this IS a real logging failure.");
      } else {
        Say("      1. NOT RULED OUT -- the selector counters could not be read, so whether");
        Say("         EnetHAL_Open ran is UNKNOWN. If it did not, this result is VACUOUS and");
        Say("         8-2d-1 has not been tested. Settle that before touching the sink.");
      }
      Say("      2. AirPortShimGetLog did not resolve -- new in 8-2d-1, so it must be listed in");
      Say("         ap_shim.exp, and FindSymbol must return it as a TVector rather than data.");
      Say("         ⚠⚠ MOST LIKELY CAUSE OF 2: A STALE AirPortShim.shlb. CFM builds its fragment");
      Say("         registry AT BOOT by scanning Extensions. Copy a new .shlb in and run without");
      Say("         RESTARTING and you get the PREVIOUS fragment, which has no such export --");
      Say("         and the log then reads as a code defect. Check the D| banner's Open number:");
      Say("         if it is not #1, the old fragment was still prepared from an earlier run.");
      Say("      3. The sink itself: Out() in ap_shim.c, or ApRingPut refusing every line.");
      Say("      ⚠ ap_ring.h passed 48 host tests, so 3 is the least likely of the three.");
    } else {
      Say1("    ⇒ The driver reported lines but none were replayed. Count = ",
           (unsigned long)shimLogN);
      Say("      That is an APP-side failure in the replay loop, not a driver one."); }

    /* ★★★ 8-2d-2b ORACLE: DID THE DRIVER APPLY THE INITVALS, IN FULL?
     *
     * ★ THE EXPECTED ANSWER CAME OUT OF THE FIRMWARE, NOT OUT OF THIS CODE. An 'i' blob's header
     *   declares a RECORD COUNT, so "applied == declared" compares what the driver did against a
     *   number Broadcom wrote into the blob years ago. That is a content oracle: nothing in the
     *   driver, the app, or this oracle can make a short application look complete.
     *
     * ⚠ WHY NOT SCORE "ApplyIvs RETURNED NON-ZERO". Because ApplyIvs stops early and SILENTLY --
     *   it range-checks each offset before writing, which is correct, and it means a truncated or
     *   mis-parsed table simply applies fewer records and returns. A boolean test would pass on
     *   one record out of three hundred, and the PHY would then fail two stages later for
     *   reasons that looked like anything but this. */
    Say("");
    if(!shimHaveIvs){
      Say("  [??] ★★★ 8-2d-2: UNRUN -- AirPortShimGetIvs did not resolve.");
      Say("    ⇒ The accessor is new in 8-2d-2b. If the export table above shows it missing while");
      Say("      the older exports resolved, the loaded shim predates this app -- check the build");
      Say("      stamp. Nothing about the initvals was tested by this run.");
    } else {
      SayOk("★★★ 8-2d-2: the DRIVER applied BOTH initvals tables in full",shimIvOk);
      if(shimIvOk){
        Say("  [ok] ★★★★★ Every record in both tables landed, checked against the count declared");
        Say("       in each blob's own header. The driver has now done chip_init steps 4 and 5");
        Say("       on its own mapping, with its own compiled-in firmware.");
        Say("       ⇒ NEXT (8-2d-3): ApPhyInitG() from inside the driver. It must return 1.");
      } else if(shimIvErr == 1){
        Say("    ⇒ Not attempted. Read the 8-2c result above -- the initvals are DECLINED when the");
        Say("      microcode is not up, because writing them into a dead core would produce a");
        Say("      failure three stages downstream that looks like a bad blob.");
      } else if(shimIvDecl && shimIvApp != shimIvDecl){
        Str255 L; L[0]=0;
        PCat(L,"    ⇒ SHORT: b0g0initvals5 applied "); PCatDec(L,shimIvApp);
        PCat(L," of ");                                PCatDec(L,shimIvDecl); Out(L);
        Say("      ApplyIvs stops at the first record whose offset is >= 0x1000 or whose length");
        Say("      runs past the blob, so the count is where it gave up. A count of 0 means the");
        Say("      very first record was rejected -- suspect the payload pointer, not the table.");
      } else if(shimBsDecl && shimBsApp != shimBsDecl){
        Say1("    ⇒ SHORT on the BAND-SWITCH table only; the main table was complete. Applied = ",
             (unsigned long)shimBsApp);
      } else {
        Say("    ⇒ b0g0bsinitvals5 is ABSENT from the driver's compiled-in blobs.");
        Say("      Re-run ./embed-fw.sh ../firmware/v4 and rebuild the shim."); } }

    /* ★★★★★ 8-2d-3 ORACLE: DID THE DRIVER INITIALISE THE G-PHY?
     *
     * ★ ApPhyInitG() returns 1 only if all FIFTEEN Stage 4 oracles passed. Those were written
     *   months ago against measured hardware identity, and none of them knows it is being run
     *   from a driver rather than from an application. That makes this the strongest single
     *   oracle in the project: one call re-runs the whole of Stage 4 as a regression.
     *
     * ⚠ A FAILURE HERE IS MOST LIKELY A PRECONDITION, NOT THE PHY. ApPhyInitG's own header says
     *   it does not run chip_init's prefix and the caller must. 8-2d-3 found the driver had been
     *   running only part of that prefix -- core flags of 0, no PhyTakeOutOfReset, no GpioInit,
     *   no RadioOn -- for three increments, while every oracle upstream passed. So the prefix
     *   results are printed FIRST and checked before the PHY verdict is believed. */
    Say("");
    if(!shimHavePhy){
      Say("  [??] ★★★ 8-2d-3: UNRUN -- AirPortShimGetPhy did not resolve.");
      Say("    ⇒ New in 8-2d-3. If the older exports resolved and this did not, the loaded shim");
      Say("      predates this app -- check the build stamp at the top of this log.");
    } else if(!shimRadioOn){
      Say("  [??] ★★★ 8-2d-3: the PHY init was DECLINED, not failed.");
      Say("    ⇒ The chip_init prefix did not complete, so ApPhyInitG was never called. Read the");
      Say("      prefix values above -- GpioInit, radio powered on -- and the 8-2c/8-2d-2 results.");
      Say("      ⚠ This is the failure mode 8-2d-3 exists to make visible: a PHY init on an");
      Say("        incomplete prefix looks like a broken PHY and is not one.");
    } else {
      SayOk("★★★★★ 8-2d-3: the DRIVER initialised the G-PHY -- ApPhyInitG() returned 1",shimPhyOk);
      if(shimPhyOk){
        Say("  [ok] ★★★★★★ ALL FIFTEEN STAGE 4 ORACLES PASSED FROM INSIDE THE DRIVER.");
        Say("       The shim now does everything the probe application does to bring this card");
        Say("       up: finds it, enumerates the backplane, resets the core, uploads its own");
        Say("       microcode, applies both initvals tables, powers the radio and initialises");
        Say("       the G-PHY -- on its own mapping, with its own compiled-in firmware.");
        Say("       ⇒ NEXT (8-2e): chip_init's statements AFTER phy_init, then the RX ring.");
      } else {
        Say("    ⇒ ApPhyInitG ran and an oracle failed. The D| lines above carry the whole of");
        Say("      Stage 4's narration, ~850 lines, and the failing oracle names itself there.");
        Say("      ⚠ CHECK THE DROP COUNT FIRST. If the ring overflowed, the tail is missing and");
        Say("        the last D| line is where the ring filled, NOT where the PHY stopped."); } }

    /* ★★★ 8-2e ORACLE: DID THE DRIVER COMPLETE chip_init?
     *
     * ⚠ SCORED ON THREE READ-BACKS, each of a register the function demonstrably wrote:
     *     TMSLOW MACPHYCLKEN   MacPhyClockSet(1) sets bit 20
     *     SHM_SH_PRMAXTIME     statement [10] writes 0
     *     MACCTL INFRA         cleared, then set again -- must end SET
     *   This is written this way because 8-2b's oracle was not. It scored "the core came up" on
     *   CLOCK and RESET, bits its own operation never touched, and passed for three increments
     *   while six prefix steps were missing. An oracle must be able to fail on the thing it is
     *   named after.
     *
     * ★ AND THE MACCTL IS COMPARABLE. Both fragments now run the SAME extracted ChipInitTail on
     *   the same card, so the driver's macctl and the app's can be diffed -- two independent
     *   mappings, one function, one expected answer. */
    Say("");
    if(!shimHaveTail){
      Say("  [??] ★★★ 8-2e: UNRUN -- AirPortShimGetTail did not resolve.");
      Say("    ⇒ New in 8-2e. If the older exports resolved and this did not, the loaded shim");
      Say("      predates this app -- check the build stamp at the top of this log.");
    } else if(shimTailErr == 1){
      Say("  [??] ★★★ 8-2e: DECLINED -- the tail was never attempted.");
      Say("    ⇒ It is gated on the G-PHY being initialised, because b43 runs statements 8-13");
      Say("      AFTER phy_init. Read the 8-2d-3 result above; this is downstream of it.");
    } else {
      SayOk("★★★ 8-2e: the DRIVER completed chip_init (statements 8-13)",shimTailOk);
      if(shimTailOk){
        Say("  [ok] ★★★★★ MACPHYCLKEN set, PRMAXTIME 0, INFRA set -- all three read back from");
        Say("       the registers the tail actually wrote.");
        Say("       ⇒ b43_chip_init is now COMPLETE inside the driver, prefix through tail.");
        Say("       ⇒ NEXT (8-2f): b43_dma_init -- the RX ring.");
      } else if(!(shimTailTms & 0x00100000UL)){
        Say("    ⇒ MACPHYCLKEN (TMSLOW bit 20) is CLEAR. MacPhyClockSet(1) did not take, so the");
        Say("      MAC and PHY are not sharing a clock and nothing downstream will work.");
      } else if(shimTailPrmax != 0){
        Say1("    ⇒ PRMAXTIME read back as ",(unsigned long)shimTailPrmax);
        Say("      Statement [10] writes 0. A non-zero read-back means the SHM write did not");
        Say("      land -- suspect the SHM routing, not the tail.");
      } else {
        Say("    ⇒ INFRA is clear in MACCTL. The tail clears it and sets it again as two");
        Say("      masksets; ending clear means the second did not take."); } }

    /* ★★★ 8-2f ORACLE: DID THE DRIVER BUILD A REAL RX RING?
     *
     * ★ THE READ-BACK IS COMPARED AGAINST WHAT WAS WRITTEN, not against a constant. RXRING must
     *   equal B43DmaAddressLow(ringPhys) -- the ring's own physical address with the SSB
     *   translation applied. A test against a literal would pass on the wrong card, and a test
     *   that merely checked "non-zero" would pass on a stale value from the app's previous run.
     *
     * ⚠ READ THE BUFFER COUNT FIRST. The driver REFUSES to arm a partial ring: every one of the
     *   32 pages must be 4K-aligned physically and inside the 1 GB PCI DMA window, because a
     *   descriptor pointing outside it is silently un-DMA-able and presents as a dead receiver
     *   three increments later. So a count below 32 is a memory-placement result, not a DMA one.
     *
     * ⚠ THE ENGINE IS STOPPED AGAIN BEFORE Open RETURNS, deliberately. RXSTATUS above is sampled
     *   while ARMED, which is the evidence; the driver then resets it so the application does not
     *   inherit a live engine pointing at driver memory. Receiving is 8-2g. */
    Say("");
    if(!shimHaveDma){
      Say("  [??] ★★★ 8-2f: UNRUN -- AirPortShimGetDma did not resolve.");
      Say("    ⇒ New in 8-2f. Check the build stamp at the top of this log.");
    } else if(shimDmaErr == 1){
      Say("  [??] ★★★ 8-2f: DECLINED -- the ring was never attempted.");
      Say("    ⇒ Gated on chip_init completing. Read the 8-2e result above.");
    } else {
      SayOk("★★★ 8-2f: the DRIVER built and armed its own RX ring",shimDmaOk);
      if(shimDmaOk){
        Say("  [ok] ★★★★★ The driver allocated system-heap memory, locked it, resolved 32");
        Say("       physical pages inside the DMA window, filled 32 descriptors and armed the");
        Say("       receive engine on an address of its own -- then stopped it again cleanly.");
        Say("       ⇒ b43_dma_init's RX half now runs inside the driver.");
        Say("       ⇒ NEXT (8-2g): keep it armed, enable the MAC, and receive a frame.");
      } else if(shimDmaErr == memFullErr){
        Say("    ⇒ ALLOCATION failed. NewPtrSysClear draws on the SYSTEM heap; 160 KB is not a");
        Say("      large ask, so suspect a leak from an earlier Open before suspecting the heap.");
      } else if(shimDmaNBuf != 32){
        Say1("    ⇒ Only this many buffer pages were usable: ",(unsigned long)shimDmaNBuf);
        Say("      Each must be physically 4K-aligned AND inside the 1 GB PCI DMA window. This");
        Say("      is a memory-placement failure, not a DMA-engine one -- the ring was refused");
        Say("      rather than armed with a descriptor the card cannot reach.");
      } else {
        Say("    ⇒ The ring was built but the engine did not take it. Compare RXRING read back");
        Say("      against the physical address above: if they differ, the write did not land."); } }

    /* ★★★★★★ 8-2g ORACLE: IS THE DRIVER A RECEIVER?
     *
     * ★ TWO CHECKS, AND THE SECOND IS THE REAL ONE. "Slots whose poison is gone" proves the card
     *   WROTE into our buffers. It does not prove what it wrote is a frame -- a DMA engine
     *   scribbling garbage would satisfy it just as well. So the first filled slot is copied out
     *   and parsed, and the oracle wants a plausible PLCP and an 802.11 header. Counting is not
     *   decoding; this project has the rule written down and it applies to its own receiver.
     *
     * ⚠ THE FILTER IS THE WEAKEST ONE THAT CAN SUCCEED. BEACPROMISC only -- what mac80211 sets
     *   to scan. Level 0 hears only frames addressed to a card that has never associated, which
     *   is nothing; level 2 adds PROMISC and KEEP_BAD and would make "we received something"
     *   nearly unfalsifiable. A first receive test should be able to fail.
     *
     * ⚠ AND THE APP CONTRIBUTED NOTHING TO THIS. It did not bring the card up, set a channel or
     *   arm a ring -- AP_APP_DRIVES_RADIO is 0 and its whole radio path is compiled out. Whatever
     *   is reported below was done by the driver, at Open, on its own mapping. */
    Say("");
    if(!shimHaveRx){
      Say("  [??] ★★★ 8-2g: UNRUN -- the receiver accessors did not resolve.");
      Say("    ⇒ New in 8-2g. Check the build stamp at the top of this log.");
    } else if(shimRxErr != 0 && shimRxEverOn){
      /* ⚠⚠ THE DISTINCTION k73 COULD NOT MAKE. The receiver is off NOW but it was on: the MAC
       * was enabled and something quiesced it -- almost certainly OT's Close, which fires when
       * the endpoint is closed. That is a SEQUENCING fault in this app, not a driver fault, and
       * the two used to look identical. */
      Say("  [??] ★★★ 8-2g: the receiver was ENABLED and then TORN DOWN before we polled.");
      Say("    ⇒ AirPortShimQuiesce ran between Open and this read. EnetHAL_Close calls it, and");
      Say("      OT drives Close when the endpoint closes. The poll must happen while the port");
      Say("      is still open -- the receiver exists only between Open and Close.");
      Say("    ⚠ This is an APP sequencing fault. The driver did its job; read its D| lines.");
    } else if(shimRxErr != 0){
      Say("  [??] ★★★ 8-2g: DECLINED -- the driver never enabled the MAC.");
      Say("    ⇒ Gated on the RX ring. Read the 8-2f result above; this is downstream of it.");
    } else {
      /* ⚠ Same as above: since 8-9d the interrupt consumes the ring, so an empty ring is
       * evidence of a WORKING receiver, not a broken one. */
      if(irqArmedEarly)
        Say("[--] NOT COMPARABLE: the driver's own interrupt drains the ring continuously.");
      else
        SayOk("★★★★★★ 8-2g: the DRIVER received frames on its own ring",shimRxFrames > 0);
      if(shimRxFrames > 0){
        SayOk("  ★ and the first one PARSES as an 802.11 frame",shimRxDecoded);
        if(shimRxDecoded){
          Say("  [ok] ★★★★★★ THE DRIVER IS A RECEIVER.");
          Say("       It found the card, brought it up, loaded its own firmware, initialised the");
          Say("       G-PHY, built a DMA ring in its own memory, enabled the MAC and received");
          Say("       802.11 frames into its own buffers -- with the application contributing");
          Say("       nothing but a log file.");
          Say("       ⇒ NEXT: authentication and association from inside the driver, which is");
          Say("         what brings the app's remaining oracles back.");
        } else {
          Say("    ⇒ ⚠ Buffers were written but the first one does not parse. That is a DMA");
          Say("      result without a receive result: the engine is moving bytes into our pages,");
          Say("      but they are not frames. Check frameoffset (30 for FW_HDR_351) and the PLCP");
          Say("      at that offset in the hex dump above before suspecting the radio."); }
      } else {
        Say("    ⇒ The receiver is live and heard NOTHING in three seconds.");
        Say("      ⚠ Three seconds is ~30 beacon intervals per AP in range, so silence is a");
        Say("        result, not a timeout. Check in this order:");
        Say("        1. RXSTATUS above -- if RXSTATE is 0 the engine stopped after arming.");
        Say("        2. RXINDEX -- if it never moved, the card is not walking the ring at all.");
        Say("        3. The channel. The driver sits on channel 1; if the room is quiet there,");
        Say("           that is a coverage question and not a driver one."); } }

    /* ★★★★★★ 8-3a ORACLE: CAN THE DRIVER TRANSMIT?
     *
     * ★ THE ORACLE IS AN ACK FROM A REAL ACCESS POINT. `acked` in the microcode's own TX status
     *   means some AP on the air received this frame, checked its FCS and answered within a
     *   SIFS. That is a third party confirming our transmission -- the strongest kind of
     *   evidence this project can get, and not something any amount of register read-back can
     *   substitute for.
     *
     * ⚠ TXSTATUS ALONE WOULD NOT DO. k18 found the PHY raising TXERR on EVERY transmission while
     *   the TXSTAT bits read textbook -- "the MAC believes it transmitted" is compatible with
     *   nothing leaving the antenna. Stage 5 lost most of its schedule to that gap.
     *
     * ⚠ AND THE DESTINATION WAS NOT COMPILED IN. The driver aimed at a BSSID it pulled out of a
     *   beacon it received itself in 8-2g. A broadcast frame is never acked, so the oracle needs
     *   a unicast target, and the only honest one is an AP we have actually heard. */
    Say("");
    if(!shimHaveTx){
      Say("  [??] ★★★ 8-3a: UNRUN -- AirPortShimGetTx did not resolve.");
      Say("    ⇒ New in 8-3a. Check the build stamp at the top of this log.");
    } else if(shimTxErr == 2){
      Say("  [??] ★★★ 8-3a: ARMED BUT NEVER FIRED -- AirPortShimTxProbe did not resolve or run.");
      Say("    ⇒ The ring is programmed; nothing asked the driver to transmit. App-side fault.");
    } else if(shimTxErr == 1){
      Say("  [??] ★★★ 8-3a: DECLINED -- the transmitter was never attempted.");
      Say("    ⇒ Gated on the receiver, because the destination comes from a beacon. Read the");
      Say("      8-2g frame count above; if it is 0 this is a RECEIVE result, not a TX one.");
    } else {
      SayOk("★★★★★★ 8-3a: an ACCESS POINT ACKNOWLEDGED the driver's frame",shimTxAcked != 0);
      if(shimTxAcked){
        Say("  [ok] ★★★★★★ THE DRIVER TRANSMITS. A real AP received a frame this driver built,");
        Say("       DMA'd out of its own memory, and acknowledged it within a SIFS.");
        Say("       ⇒ b43_dma_init is now COMPLETE in the driver, both halves.");
        Say("       ⇒ NEXT (8-3b): authentication -- the same path, one exchange further.");
      } else if(shimTxStats == 0){
        Say("    ⇒ NO TX STATUS AT ALL came back. The microcode did not report on this frame,");
        Say("      which means it never reached the TX engine rather than failing on the air.");
        Say("      Check TXRING read-back above first, then whether the txhdr layout matches the");
        Say("      loaded firmware -- gTxhShift is set from the ucode revision for that reason.");
      } else {
        Say1("    ⇒ The frame WAS transmitted -- status reports seen = ",
             (unsigned long)shimTxStats);
        Say("      but nothing acknowledged it. In order: is the BSSID above one that is");
        Say("      actually in range (it came from a beacon, so it should be); is ackReq set in");
        Say("      the txhdr; and is the PHY TX path raising TXERR, which k18 found it doing for");
        Say("      every transmission on this card. That last one is a known, open defect."); } }

    /* ★★★★★★ 8-3b ORACLE: WILL AN ACCESS POINT AUTHENTICATE THIS DRIVER?
     *
     * ★ EVERY FIELD IN THE ANSWER WAS CHOSEN BY THE AP. Sequence 2 and status 0 come out of a
     *   frame the access point built and transmitted; nothing in the driver can manufacture
     *   them. That is the difference between this and every oracle before 8-3a, which checked
     *   values the driver itself had written.
     *
     * ⚠ SEQUENCE IS CHECKED BEFORE STATUS, and separately. An auth frame from the target
     *   addressed to us that is NOT sequence 2 belongs to some other exchange, and reading its
     *   status as our answer would be reading someone else's mail.
     *
     * ⚠ AND AN ACK IS NOT AN ANSWER. 8-3a proved the AP acknowledges our frames at the MAC
     *   layer. Agreeing to authenticate is a different decision, and an ACKed auth request that
     *   draws silence means the AP received it and declined -- MAC filtering first. */
    Say("");
    if(!shimHaveAuth){
      Say("  [??] ★★★ 8-3b: UNRUN -- the authentication accessors did not resolve.");
      Say("    ⇒ New in 8-3b. Check the build stamp at the top of this log.");
    } else if(shimAuthErr == 1){
      Say("  [??] ★★★ 8-3b: DECLINED -- authentication was never attempted.");
      Say("    ⇒ Gated on the transmitter, and on finding the TARGET SSID in a beacon. If the");
      Say("      driver never matched it, the log above says so -- it refuses to authenticate");
      Say("      against a BSSID it merely happened to hear, which is what k79 did.");
    } else {
      SayOk("★★★★★★ 8-3b: an ACCESS POINT AUTHENTICATED this driver",shimAuthErr == 0);
      if(shimAuthErr == 0){
        Say("  [ok] ★★★★★★ SEQUENCE 2, STATUS 0. The driver sent an open-system authentication");
        Say("       request and the access point accepted it. Every field in that answer was");
        Say("       chosen by the AP.");
        Say("       ⇒ NEXT (8-3c): association -- the same shape, one exchange further.");
      } else if(!shimAuthGot && shimAuthAcked){
        /* ⚠ ms == 0 NOW MEANS "no response". In k79 it did not: RxWaitFor reports the elapsed
         * time even on timeout, so a silent AP came back as ms = 501 and this oracle read it as
         * "a response arrived with sequence 0". The driver folds gShimAuthGot into ms so the
         * two states cannot be confused again. */
        Say("    ⇒ ACKed but NO RESPONSE. The AP received the request and chose silence. MAC");
        Say("      filtering is the first thing to check; a malformed frame is the second, and");
        Say("      the ACK largely rules that out -- a malformed frame is usually not ACKed.");
      } else if(!shimAuthGot){
        Say("    ⇒ Neither ACKed nor answered. That points back at the transmitter rather than");
        Say("      at the AP; compare against the 8-3a result, which used the same path.");
      } else if(shimAuthSeq != 2){
        Say1("    ⇒ A response arrived but its sequence is ",(unsigned long)shimAuthSeq);
        Say("      Not the second frame of our exchange -- its status says nothing about us.");
      } else {
        Say1("    ⇒ The AP REFUSED us. status = ",(unsigned long)shimAuthStatus);
        Say("      Status 1 is unspecified failure; 13 is 'unsupported authentication algorithm'");
        Say("      and would mean the open-system arm is wrong for this AP."); } }

    /* ★★★★★★ 8-3c ORACLE: WILL AN ACCESS POINT ASSOCIATE THIS DRIVER?
     *
     * ★ THE AID IS THE STRONG HALF. status 0 says the AP accepted; the AID is a number it
     *   ALLOCATED from its own association table, and a driver cannot invent a plausible one.
     *   status 0 with a zero AID is therefore scored as a FAILURE, not a pass -- an associated
     *   station always has a non-zero AID, so that combination means the field offsets are wrong.
     *
     * ⚠ SILENCE HERE IS NOT SILENCE AT 8-3b. Authentication succeeded moments earlier, so the AP
     *   knows this MAC and is willing to talk to it. No association response points at the
     *   request's CONTENTS -- the RSNE first -- rather than at addressing or the transmitter. */
    Say("");
    if(!shimHaveAssoc){
      Say("  [??] ★★★ 8-3c: UNRUN -- the association accessors did not resolve.");
      Say("    ⇒ New in 8-3c. Check the build stamp at the top of this log.");
    } else if(shimAssocErr == 1){
      Say("  [??] ★★★ 8-3c: DECLINED -- association was never attempted.");
      Say("    ⇒ Gated on authentication. Read the 8-3b result above.");
    } else {
      SayOk("★★★★★★ 8-3c: an ACCESS POINT ASSOCIATED this driver",shimAssocErr == 0);
      if(shimAssocErr == 0){
        Say1("  [ok] ★★★★★★ STATUS 0, AID = ",(unsigned long)shimAssocAid);
        Say("       The access point accepted this driver as a station and allocated it an");
        Say("       association ID from its own table. The driver is now a member of the BSS.");
        Say("       ⇒ NEXT (8-4): the WPA2 four-way handshake, which is what makes the port");
        Say("         controlled and brings the app's remaining oracles back.");
      } else if(!shimAssocGot && shimAssocAcked){
        Say("    ⇒ ACKed but NO RESPONSE. The AP received the request and did not answer.");
        Say("      ⚠ Auth succeeded moments ago, so it knows this MAC -- this points at the");
        Say("        request's CONTENTS. The RSNE is the first thing to check: it is stated");
        Say("        rather than discovered, so a mismatch with the AP's actual cipher suite");
        Say("        would produce exactly this silence.");
      } else if(!shimAssocGot){
        Say("    ⇒ Neither ACKed nor answered, which points back at the transmitter. Compare");
        Say("      against 8-3b, which used the same path moments earlier.");
      } else if(shimAssocStatus != 0){
        Say1("    ⇒ The AP REFUSED the association. status = ",(unsigned long)shimAssocStatus);
        Say("      Status 12 is 'association denied, unsupported capability'; 18 is 'basic rate");
        Say("      set not supported'. Both point at the capability field or the RSNE.");
      } else {
        Say("    ⇒ status 0 but the AID is ZERO, which is not a completed association. An");
        Say("      associated station always has a non-zero AID, so suspect the field offsets");
        Say("      in the response rather than the AP."); } }

    /* ★★★★★★ 8-4 ORACLE: DID BOTH SIDES DERIVE THE SAME KEY?
     *
     * ★ MESSAGE 3 IS THE ANSWER, AND IT IS THE STRONGEST ORACLE THIS PROJECT HAS. For the AP to
     *   send it, the AP must have recomputed the MIC on OUR message 2 using ITS OWN PTK and got
     *   our value. Both sides derived the same 48 bytes independently from the same PMK -- so the
     *   PMK, the PRF, the min||max address ordering, the label's terminating NUL and the KCK
     *   offset are ALL correct simultaneously. A driver cannot fake a cryptographic agreement
     *   with a third party.
     *
     * ⚠ MESSAGE 1 ARRIVING IS NOT THE SAME THING. It is unprompted and proves only that the AP
     *   started the handshake. Everything about our key derivation is tested by message 3.
     *
     * ⚠ AND NO KEY MATERIAL IS REPORTED ANYWHERE. Counts and timings only. */
    Say("");
    if(!shimHaveHs){
      Say("  [??] ★★★ 8-4: UNRUN -- the handshake accessors did not resolve.");
      Say("    ⇒ New in 8-4. Check the build stamp at the top of this log.");
    } else if(shimHsErr == 1 || shimHsErr == 2){
      /* ⚠⚠ 2 = DECLINED, and it is a separate value from paramErr on purpose. k82's decline path
       * set paramErr, which this oracle could not tell from a real failure -- so when the
       * association failed and the handshake correctly declined, the log reported "MESSAGE 1
       * NEVER ARRIVED" and sent the reader to check PBKDF2 timing. The driver had said
       * "HANDSHAKE DECLINED -- not associated" in the same log. Declined and failed are
       * different states and must not share a value. */
      Say("  [??] ★★★ 8-4: DECLINED -- the handshake was never attempted.");
      Say("    ⇒ Gated on association AND on the PMK being ready. Read 8-3c above: if the");
      Say("      association did not complete, nothing here is a handshake result.");
    } else {
      SayOk("★★★★★★ 8-4: the WPA2 four-way handshake COMPLETED",shimHsErr == 0);
      if(shimHsErr == 0){
        Say("  [ok] ★★★★★★ THE ACCESS POINT VERIFIED OUR MIC.");
        Say("       It recomputed the MIC on our message 2 with its own PTK and got our value.");
        Say("       Both sides derived the same key independently: the PMK, the PRF, the");
        Say("       min||max ordering, the label's NUL and the KCK offset are all correct.");
        Say("       ⇒ The driver is a fully authenticated station on a WPA2 network.");
        Say("       ⇒ NEXT (8-6): accept Start and deliver frames up the DLPI stack.");
      } else if(!shimHsM1){
        Say1("    ⇒ MESSAGE 1 NEVER ARRIVED. PBKDF2 took ms = ",(unsigned long)shimPmkMs);
        Say("      ⚠ If that figure is large, the PMK was still being computed when message 1");
        Say("        was sent. It is derived BEFORE associating for exactly that reason -- check");
        Say("        the ordering in the log above, not the radio.");
      } else if(!shimHsM3){
        Say("    ⇒ Message 1 arrived and message 3 did not. The AP verifies our MIC before");
        Say("      answering, so silence means it did NOT verify.");
        Say("      ⚠ That is a KEY DERIVATION result, not a radio one. Suspects in order: the");
        Say("        PTK (PRF label and min||max address ordering), the KCK offset, and the MIC");
        Say("        span -- it covers from the EAPOL header with the MIC field zeroed, NOT from");
        Say("        the EAPOL-Key header. hostap wpa_common.c:186 documents that the published");
        Say("        standard is wrong about it.");
      } else {
        Say("    ⇒ Message 3 arrived but the handshake did not complete -- message 4 failed to");
        Say("      send. That is a transmit result at the last step; compare against 8-3a."); } }

    Say("");
    SayDrv("★★★ the driver ENABLED the 802.11 core (TMSLOW read back)",shimCoreUp);
    if(shimCoreUp){
      Say("    ⇒ The driver has now done its first destructive operation on this card, behind an");
      Say("      explicit arm rather than an ordering argument, and the application went on to");
      Say("      use the card afterwards. ⇒ NEXT (8-2c): the firmware upload.");
    } else if(!shimArmed){
      Say("    ⇒ DECLINED, not failed: the driver was not armed when Open ran. That is the");
      Say("      interlock working -- it refuses to reset a core it has not been told is idle.");
    } else {
      Say1("    ⇒ Armed and it still did not come up. TMSLOW = ",(unsigned long)shimTmsLow);
      Say("      Expect CLOCK set and RESET clear. If RESET is still set the disable half of");
      Say("      SsbCoreEnable did not complete; if CLOCK is clear the core has no clock and");
      Say("      nothing else will work either."); }

    Say("");
    SayDrv("the DRIVER enumerated the backplane by itself",shimCoresOk);
    if(shimCoresOk){
      Str255 L;
      L[0]=0; PCat(L,"    driver: chip "); PCatHex(L,(unsigned long)shimChipId,4);
      PCat(L," rev "); PCatDec(L,(unsigned long)shimChipRev);
      PCat(L,", "); PCatDec(L,(unsigned long)shimNCores);
      PCat(L," cores, 802.11 @ "); PCatDec(L,(unsigned long)shimIdx80211); Out(L);
#if AP_APP_DRIVES_RADIO
      L[0]=0; PCat(L,"    app:    chip "); PCatHex(L,(unsigned long)gBus.chipId,4);
      PCat(L," rev "); PCatDec(L,(unsigned long)gBus.chipRev);
      PCat(L,", "); PCatDec(L,(unsigned long)gBus.nCores);
      PCat(L," cores, 802.11 @ "); PCatDec(L,(unsigned long)gBus.idx80211); Out(L);
#endif
      if(shimChipId   == gBus.chipId  && shimChipRev == gBus.chipRev &&
         shimNCores   == gBus.nCores  &&
         shimIdx80211 == (SInt32)gBus.idx80211 && shimIdxCC == (SInt32)gBus.idxChipCommon){
        Say("  [ok] ★★★★★★ IDENTICAL. Two fragments, two mappings, one backplane.");
        Say("    ⇒ STAGE 8-2a PASSES. The driver can find, map and enumerate this card on its");
        Say("      own, and the BAR0 window it moved to do so was restored correctly -- if it");
        Say("      had not been, the app's reading would be the corrupted one.");
        /* ⚠ AND THAT IS ALL THIS ORACLE MAY CLAIM. An earlier version of this text went on to
         * say "the app went on to complete its whole run afterwards" -- unconditionally, on the
         * strength of the inventories matching. k61 matched and the app's authentication then
         * failed anyway, on the pre-existing TX-dead intermittent. Asserting an outcome this
         * oracle has not checked is the same defect this probe has caught three times elsewhere,
         * so the app's actual fate is REPORTED rather than assumed. */
        SayOk("  ...and the app's own run also completed (reported, not assumed)",oracleJ);
        if(!oracleJ){
          Say("    ⇒ ⚠ The app did NOT get through authentication this run. That does not");
          Say("      retract 8-2a: the inventories still agree, which is the whole claim. Check");
          Say("      the TX-section control and the microcode's TX status count above -- if they");
          Say("      read 0 and 0 with a healthy scan, this is the parked DMA-resume");
          Say("      intermittent (k50, k51) and NOT something the driver's scan caused."); }
        Say("    ⇒ NEXT (8-2b): the firmware upload, which is the first step that WRITES.");
      } else {
#if AP_APP_DRIVES_RADIO
        Say("  [!!] ⚠⚠ THEY DISAGREE, and this is a stop-and-fix rather than a curiosity.");
        Say("    Two readings of one immutable backplane cannot legitimately differ. Suspect, in");
        Say("    order: the BAR0 window restore (the driver moves it to scan and puts it back --");
        Say("    if that failed, the APP's numbers are the corrupted ones, not the driver's);");
        Say("    then whether the two mappings actually resolve to the same aperture.");
#else
        /* ⚠ The app's inventory is all zeros because it never scanned. Not a disagreement. */
        Say("  [--] NOT COMPARABLE: the app does not enumerate the backplane any more.");
        Say("      The driver's inventory above stands on its own -- chip 4306 rev 3 with five");
        Say("      cores is the identity every Stage 2 run has measured on this card.");
#endif
      } }

    Say("");
    SayDrv("the DRIVER read a sane MAC off the card by itself",shimMacOk);
    if(shimMacOk){
      Str255 L;
      L[0]=0; PCat(L,"    driver's MAC = "); PCatMac(L,shimMac); Out(L);
#if AP_APP_DRIVES_RADIO
      L[0]=0; PCat(L,"    app's MAC    = "); PCatMac(L,mac);     Out(L);
#endif
#if !AP_APP_DRIVES_RADIO
      /* ⚠ The app's copy is the 00:EE:EE:EE:00:EE sentinel -- it never read the SPROM. k74
       * scored that as "One of the two SPROM reads is wrong" and pointed at byte order. */
      Say("  [--] NOT COMPARABLE: the app no longer reads the SPROM; its copy is a sentinel.");
      Say("      The driver's six bytes above are the card's own address, from BAR0+0x1000.");
#else
      if(MacEq(shimMac,mac)){
        Say("  [ok] ★★★ THEY AGREE -- two separate mappings, same six bytes.");
      } else {
        Say("  [!!] ⚠⚠ THEY DISAGREE. One of the two SPROM reads is wrong, and this is the");
        Say("       cheapest place this project will ever find that out. Suspect the byte order");
        Say("       first (ssb writes each SPROM word big-endian, so the HIGH byte leads), then");
        Say("       whether the two BAR0 mappings actually point at the same aperture."); }
#endif
    }

    if(shim85bInstalled && shim85bCalls > 0){
      Say("  [ok] ★★★★★★ OPEN TRANSPORT DROVE A DRIVER WE WROTE.");
      Say("    ⇒ The container is right, the registration path is right, and the selector");
      Say("      sequence printed in the [8-5b] block above is the contract 8-5c implements");
      Say("      against. No STREAMS or DLPI code was written to get here.");
    } else if(shim85bInstalled){
      Say("  [--] Registered, not yet driven. NOT a failure -- see the [8-5b] block. NEXT: leave");
      Say("       the port registered and select it in the TCP/IP control panel by hand, which");
      Say("       is what makes OT open a port it has merely been told about.");
    } else if(shimLibOk){
      if(witnessMode)
        Say("  [--] The app did not register -- the BOOT VEHICLE already owns this port. That is");
      else
        Say("  [!!] The shim is there and refused us. Read the two GetSharedLibrary results above");
      Say("       first: if CFM could not find \"AirPortShim\", this is a PACKAGING result (the");
      Say("       .shlb must be in Extensions) and says nothing about the driver.");
    } else {
      Say("  [--] Not run: EnetShimLib unavailable."); }

    Say("");
#if AP_APP_DRIVES_RADIO
    /* ⚠ Oracle T writes to the network and the quiesce tears the card down -- both are radio
     * work, and both now belong to the driver. AirPortShimQuiesce() does the teardown. */
    Say("  === ORACLE T / STAGE 8-1: CAN WE WRITE TO THE NETWORK? ===");
    oracleT = arp81Replied && arp81Matched;
    /* ⚠⚠ "DID NOT RUN" AND "RAN AND FAILED" ARE DIFFERENT ANSWERS AND k50 COULD NOT TELL THEM
     * APART. Stage 8-1 sits inside the EAPOL message-4 branch, so a run whose handshake never
     * completed prints this whole block as a wall of [!!] -- including "the ARP request
     * encapsulated to the right length", which is pure arithmetic that cannot fail. That is
     * exactly the defect this probe criticised in k49's oracle one increment earlier: a report
     * that names suspects before establishing whether the code even executed. */
    if(!arp81Ran){
      Say("  [--] ⚠ THIS RUN NEVER REACHED STAGE 8-1, so nothing below was measured.");
      Say("       Stage 8-1 runs only after the 4-way handshake completes. Read Oracles J..P");
      Say("       above for where the run actually stopped; the transmit fix is UNTESTED rather");
      Say("       than failed, and the correct response is to re-run, not to change code.");
      Say("");
    } else {
    SayOk("the ARP request encapsulated to the right length",arp81Encrypted);
    SayOk("the TX engine consumed the encrypted frame",arp81Sent);
    SayOk("★ THE AP ACKNOWLEDGED IT -- the frame arrived with a good FCS",arp81Acked > 0);
    Say1("  protected frames seen while waiting = ",(unsigned long)arp81Protected);
    Say1("    of those GROUP-addressed (GTK)    = ",(unsigned long)arp81Group);
    Say1("    of those UNICAST to us  (TK)      = ",(unsigned long)arp81Unicast);
    Say1("  frames that decrypted               = ",(unsigned long)arp81Decrypted);
    SayOk("an ARP REPLY came back and decrypted",arp81Replied);
    SayOk("★★★★★★★ and its sender MAC is the one recorded BEFORE the run",oracleT);
    if(oracleT){
      Say("    ⇒ ★★★★★★★ THE LINK IS BIDIRECTIONAL. This is the first frame this project has");
      Say("      ever encrypted, and a machine on the far side of the AP decrypted it, acted on");
      Say("      it, and answered. Everything had to be right at once: the CCM order, the nonce,");
      Say("      every AAD mask, the PN layout, the Protected bit, and the TK -- a wrong bit in");
      Say("      any of them and the AP discards the frame in silence.");
      Say("    ⇒ The sender MAC was compiled in from the developer Mac's ARP table before the");
      Say("      run, so this is a comparison against an independently recorded value, not a");
      Say("      plausibility check.");
      Say("    ⇒ NEXT (8-2): interrupt-driven receive. ⚠ That crosses the execution-level");
      Say("      boundary that caused the three worst bugs in the USB 2.0 work -- no File");
      Say("      Manager below task level, and re-run the static audit afterwards.");
    } else if(arp81Replied){
      Say("    ⇒ ⚠ A REPLY CAME BACK BUT FROM THE WRONG MAC. Read the sender address above. If");
      Say("      it is another host on the LAN, something else answered for .1 -- proxy ARP on");
      Say("      the UniFi, most likely -- and the transmit path still WORKS. Update");
      Say("      kArpExpectMac rather than debugging the radio.");
    } else if(arp81Decrypted > 0){
      Say("    ⇒ Encrypted traffic arrived and decrypted, but none of it was an ARP reply. So");
      Say("      the receive path is fine and the question is whether our frame was understood.");
      Say("      Most likely: the AP dropped it. Check the hexdump above against a capture.");
    } else if(arp81Sent && arp81Acked > 0){
      Say("    ⇒ ★ THE AP ACKNOWLEDGED THE FRAME AND STILL DID NOT ANSWER, which is a much");
      Say("      sharper result than k49's silence. An ACK is sent before the MIC is checked, so");
      Say("      the frame reached the AP intact at the MAC layer and the fault is AFTER that:");
      Say("        - the MIC failed, and the AP discarded it without complaint. Suspect the AAD");
      Say("          masks and the nonce -- but note both are shared with the receive path, which");
      Say("          verifies against this same AP every run;");
      Say("        - or the PN was rejected as a replay;");
      Say("        - or the AP declined to forward it: check the unicast/group counts above, and");
      Say("          whether UniFi is filtering broadcast from this client.");
    } else if(arp81Sent){
      Say("    ⇒ Sent, NOT acknowledged, nothing back. The TX engine moving proves only that DMA");
      Say("      consumed descriptors; without an ACK there is no evidence the frame ever reached");
      Say("      the AP intact, and CRYPTO IS NOT THE SUSPECT -- an AP ACKs before it decrypts.");
      Say("      Look at the frame on the air instead: the PLCP length in the txhdr, the rate, and");
      Say("      whether ackReq was set. ⚠ k49 failed exactly here and the cause was a stale");
      Say("      txhdr carrying message 4's length, not the cipher.");
    } else {
      Say("    ⇒ The TX engine never moved, so this says nothing about the crypto. That is a");
      Say("      Stage 5 problem and the oracles above will say which."); }
    }  /* end of the arp81Ran guard */

    Say("");
    Say("  === ORACLE S / k48: IS THE MICROCODE STALLED? ===");
    /* ⚠ SCORED AS A 2x2, NOT A LEVEL. "The counter was frozen on dead dwells" is worthless on
     * its own -- it only means something if it KEEPS MOVING on the dwells that hear traffic.
     * The live dwells in this same run are the control. */
    Say ("  SHM_SCRATCH[1]:[0] sampled either side of every dwell.");
    Say1("    dwells where it FROZE   = ",(unsigned long)gScrFrozen);
    Say1("      of those, heard 0     = ",(unsigned long)gScrFrozenDead);
    Say1("    dwells where it MOVED   = ",(unsigned long)gScrMoved);
    Say1("      of those, heard 0     = ",(unsigned long)gScrMovedDead);
    if(gScrFrozen > 0 && gScrFrozenDead == gScrFrozen && gScrMovedDead == 0 && gScrMoved > 0){
      Say("  [ok] ★★★★★★ PERFECT SPLIT: the counter froze on exactly the dwells that heard");
      Say("       nothing and moved on exactly the dwells that heard traffic.");
      Say("    ⇒ That is the microcode stalling, observed directly rather than inferred. Every");
      Say("      register we have been checking is a host-side register and they all read");
      Say("      correct because the HOST side is correct -- the part that stopped is the one");
      Say("      we never looked at. It also explains why the fault is per-run and absolute,");
      Say("      why no host register predicts it, and why the ring arm was irrelevant.");
      Say("    ⇒ NEXT: find what stalls it. The leading candidate is still the unprogrammed TX");
      Say("      DMA controllers -- b43_dma_init programs five, we program one -- because a");
      Say("      microcode touching an unconfigured TX FIFO is exactly the kind of thing that");
      Say("      would hang its main loop. The rung ladder below tests that directly.");
    } else if(gScrMoved > 0 && gScrMovedDead > 0){
      Say("  [!!] THE COUNTER KEPT MOVING THROUGH DEAD DWELLS.");
      Say("    ⇒ ★ The microcode is alive and this lead is dead. That is worth knowing: the");
      Say("      bring-up correlation was real but it is a coincidence of initial value, not a");
      Say("      stall. The fault is downstream of the microcode, in the path between it and");
      Say("      our ring.");
    } else if(gScrFrozen == 0){
      Say("  [--] the counter moved across every dwell and nothing was dead, so this run does");
      Say("       not exercise the question. Not a pass.");
    } else {
      Say("  [--] MIXED, so no clean reading. Print the per-dwell lines above and look at which");
      Say("       dwells froze -- a counter that freezes on SOME live dwells is a sampling");
      Say("       artefact (the dwell is short and the counter may tick slowly), and the fix is");
      Say("       a longer sample rather than a new theory."); }

    Say("");
    Say("  --- k46's original hammer verdict, retained for the record ---");
    /* ⚠ THERE IS NO INCONCLUSIVE BRANCH THIS TIME, AND THAT IS THE POINT OF THE REDESIGN.
     * k45 could only fail to reproduce; k46's zero-failure case is itself a strong result,
     * because 40 trials at any rate near 23% cannot plausibly all pass. */
    oracleR = (abDeadA > 0) && (abDeadB == 0);
    Say1("  arms hammered                      = ",(unsigned long)K46_HAMMER);
    Say1("  ARM     dwells that read ZERO      = ",(unsigned long)abDeadA);
    Say1("  RECYCLE dwells that read ZERO      = ",(unsigned long)abDeadB);
    SayOk("★★★★★★ REPRODUCED ON DEMAND, AND ONLY ON THE ARM PATH",oracleR);
    if(oracleR){
      Say("    ⇒ ★ THE FAULT IS NOW A TOOL. It can be triggered whenever we like, which turns");
      Say("      every future question about it from a 23% lottery into a measurement. And it");
      Say("      fired only where the DMA controller is reset -- b43 does that twice a session,");
      Say("      we were doing it up to nine times per run.");
      Say("    ⇒ NEXT: with a reproducer in hand, bisect RxRingArm itself. It does four things");
      Say("      (reset, re-poison + refill descriptors, write RXCTL/RXRING, write RXINDEX);");
      Say("      one of them is the one that wedges, and that no longer costs a reboot to find.");
    } else if(abDeadA > 0 && abDeadB > 0){
      Say("    ⇒ REPRODUCED, but BOTH paths fail, so the arm is not what distinguishes them.");
      Say("      The trigger is something the two share: MacSuspend (k24 showed it stops the");
      Say("      DMA engines), SwitchChannel, or the settle time. Still a win -- we have a");
      Say("      reproducer, which is the expensive half.");
    } else if(abDeadB > 0){
      Say("    ⇒ ★ INVERTED: only the RECYCLE path failed. The theory is dead and, worse, k45");
      Say("      shipped the scan onto that path. Revert the scan to the arm and re-think.");
    } else {
      Say1("    ⇒ ★★ STRONG REFUTATION, and this is a RESULT, not a failed run. Zero failures in ",
           (unsigned long)(K46_HAMMER*2));
      Say("      dwells. At the 25% measured per cold run, P(0 in 40) = 0.75^40 = 0.00001; even");
      Say("      at a tenth that rate it stays under 2%. So:");
      Say("        - the ring arm is NOT the trigger, and RxRingArm is exonerated;");
      Say("        - the fault is decided EARLIER, once per run, and then applies to every");
      Say("          dwell -- which is exactly the all-or-nothing shape the 22 logs show.");
      Say("      ⇒ Stop looking at the DMA path. The next place to look is bring-up, and the");
      Say("        useful fact is that 89 key/value pairs reported there ALREADY fail to");
      Say("        separate the dead runs from the live ones -- so the deciding state is");
      Say("        something bring-up does not currently print. Instrument it, do not guess.");
      Say("      ⚠ Keeping the scan on the recycle path is still right on b43-faithfulness");
      Say("        grounds, but it is NOT a fix for this and must not be reported as one."); }
    Say("");
    /* ⚠⚠ k30's LOG CONTRADICTED ITSELF AND THIS IS THE FIX.
     * `gotReply` is set only by the SLOW scan, which since k29 is the inferior instrument: k30's
     * prompt path caught replies at 18 ms and 5 ms while the slow scan, looking a second later at
     * a ring that wraps in about a second, found zero. So the run printed "[!!] an access point
     * answered our probe request" and "nothing on the air understood the frame" in the same log
     * that carried an ACK and a successful authentication. A future reader -- me -- would have
     * chased that. A reply caught by EITHER instrument is a reply. */
    if(promptHits) gotReply = 1;
    SayOk("★ an access point answered our probe request",gotReply);
    if(promptHits && !slowReplies)
      Say("    (caught by the PROMPT path; the slow scan found none, which is now the expected"
          " shape -- the ring wraps before it looks.)");
    if(!rxControlOk){
      Say("    ⚠⚠ THE CONTROL FAILED, SO THE REPLY RESULT MEANS NOTHING EITHER WAY. The");
      Say("       receiver was not working during the TX passes -- exactly k9's condition,");
      Say("       now measured instead of inferred. Read the RXSTATUS pairs above: that is");
      Say("       the bug to chase, and the probe request is not the suspect."); }
    if(txMoved && !gotReply && !promptHits && rxControlOk){
      Say("    ⚠ TX MOVED BUT NOTHING ANSWERED, AND THE RECEIVER WAS LIVE THIS TIME. That is");
      Say("      the informative split, and it is the FIRST run in which it can be believed:");
      Say("      the DMA engine took our descriptors, the receiver was demonstrably hearing");
      Say("      traffic in the same second, and still nothing on the air understood the");
      Say("      frame. Leads, in order: the PLCP length, the txhdr field offsets, TX power,");
      Say("      or the antenna."); }
    if(!txMoved){
      Say("    ⚠ NO CONTROLLER MOVED. Four were tried, so this is not a controller-mapping");
      Say("      problem and that hypothesis is spent. The remaining leads are the txhdr");
      Say("      contents -- the PLCP fields at +54 and +100 were never hex-dumped -- or the");
      Say("      MAC declining to transmit for a reason we have not measured.");
      Say("");
      Say("    (k8's wording, kept because it still applies:)");
      Say("      ordering hypothesis, and the readbacks above say which way to go next:");
      Say("        - descriptors bad      -> our own stores are wrong, look at le32st and the");
      Say("                                  descriptor format, not at the card.");
      Say("        - descriptors good,    -> the card is not fetching what is demonstrably");
      Say("          TXCTL/TXRING good       there. Suspect the txhdr contents, the MAC's");
      Say("                                  willingness to transmit, or a cache line the");
      Say("                                  card reads stale despite the barrier.");
      Say("        - TXCTL lost TXENABLE  -> something between 5-3a and here clears it."); }
    oracleH = txMoved && !txError && !descBad;
    oracleI = gotReply && rxControlOk;

    /* ---------------- quiesce ---------------- */
    Say("");
    Say("=== [5-3f] CLEANUP ===");
    (void)MacSuspend();
    (void)DmaControllerTxReset(base); ssb_w32(gBus.bar0,base+B43_DMA32_TXRING,0);
    (void)DmaControllerRxReset(base); ssb_w32(gBus.bar0,base+B43_DMA32_RXRING,0);
    SayH("  MACCTL final = ",(unsigned long)ssb_r32(gBus.bar0,B43_MMIO_MACCTL),8);
    Say ("  MAC suspended, both engines reset and ring registers zeroed BEFORE the memory goes");
    Say ("  away -- the card must not be left pointing at pages we are about to free.");

#endif /* AP_APP_DRIVES_RADIO */

cleanup:
    /* ⚠⚠ REACHED ON EVERY EXIT PATH. NewPtrSysClear draws from the SYSTEM heap, which OS 9 does
     * NOT reclaim when an application quits. DmaBlockFree nulls what it frees, so arriving here
     * after the normal path already ran is a no-op. */
    DmaBlockFree(&txRing); DmaBlockFree(&rxRing); DmaBlockFree(&rxPool); DmaBlockFree(&txPool);
    Say("  rings and pool unlocked and disposed.");

    Say("");
    Say("=== VERDICT ===");
#if !AP_APP_DRIVES_RADIO
    /* ⛔⛔ THE LEGACY VERDICT BELOW IS NOT APPLICABLE IN WITNESS MODE, AND IT LIED FOR FOUR
     * BUILDS BEFORE ANYONE NOTICED.
     *
     * Oracles A..I are set by the APP driving the radio directly. Since AP_APP_DRIVES_RADIO went
     * to 0 the driver drives itself and the app only watches, so every one of them stays false
     * and the block prints nine confident failures -- "the rings did not program cleanly", "NO
     * FRAME ARRIVED" -- in runs that associated, completed a WPA2 handshake and moved 3994 bytes
     * up the DLPI stack. k99 is BANKED AS A PASS and its verdict text is byte-identical to
     * k102's. A summary that always says NOT COMPLETE is worse than no summary: the one time it
     * is right, nobody will believe it. [[feedback_guards_must_not_latch]] is the same shape.
     *
     * The real result in this mode is the driver's own verdict on the D| channel, plus the
     * per-stage [ok] lines above, which ARE driven by what happened. */
    Say("  (the app is a WITNESS in this build -- AP_APP_DRIVES_RADIO is 0.)");
    Say("   The A..I oracles below are set only when the APP drives the radio, so they are");
    Say("   not evaluated here and would read as failures whatever happened. Read instead:");
    Say("     - the ★★★★★★ [ok] lines above, which are driven by this run, and");
    Say("     - the driver's own \"=== VERDICT ===\" on the D| channel.");
#else
    if(oracleA&&oracleB&&oracleC&&oracleD&&oracleE&&oracleF&&oracleG&&oracleH&&oracleI){
      Say("  ★★★★★★ THE CARD RECEIVED A FRAME OFF THE AIR AND DMA'd IT INTO OUR MEMORY.");
      Say("        A  both rings still program cleanly");
      Say("        B  core_init's five remaining statements landed");
      Say("        C  MACCTL went to ENABLED | AWAKE through b43's own refcount");
      Say("        D  a buffer's poison was overwritten and frame_len is non-zero");
      Say("        E  the frame decodes as 802.11 with a good FCS");
      Say("     ⇒ THIS SETTLES FOUR THINGS AT ONCE: the card reads our descriptors, writes our");
      Say("       buffers, PCI DMA is cache-coherent, and Stage 4's PHY genuinely receives.");
      Say("     ⇒ It also answers k2's open question -- the RXACTIVE field WAS descriptor");
      Say("       activity, because the ring is demonstrably being consumed.");
      Say("     ⇒ NEXT (5-4): transmit. Post a frame on the TX ring and look for the ACK, or");
      Say("       send a probe request and match the probe response against this decoder.");
    } else {
      Say("  ✗ NOT COMPLETE.");
      if(!oracleA) Say("    - the rings did not program cleanly. This is a k2 REGRESSION.");
      if(!oracleB) Say("    - b43_phy_init did not complete: either a Stage 4 oracle failed (read the");
      if(!oracleB) Say("      fifteen-oracle report above -- that would be a STAGE 4 regression), or the");
      if(!oracleB) Say("      firmware channel cookie did not stick.");
      if(!oracleC) Say("    - a chip_init 8-13 statement did not land.");
      if(!oracleD) Say("    - a core_init tail statement did not land its value.");
      if(!oracleE) Say("    - MACCTL did not reach ENABLED|AWAKE. The receiver is off.");
      if(!oracleF) Say("    - NO FRAME ARRIVED. See the diagnosis list above.");
      if(!oracleG) Say("    - a frame arrived but does not decode as valid 802.11.");
      if(!oracleH) Say("    - the TX engine did not consume the descriptors, or reported an error.");
      if(!oracleI) Say("    - no probe response came back. See the split above: TX moving without");
      if(!oracleI) Say("      a reply is a frame-contents problem, not a ring problem.");
      if(oracleA&&oracleB&&oracleC&&oracleD&&oracleE&&!oracleF){
        Say("    ⚠ A THROUGH E PASSING WITH F FAILING IS THE INFORMATIVE CASE, and it is a");
        Say("      stronger statement than it was in k3: the PHY is now initialised and");
        Say("      chip_init has run to completion, so every precondition we control is met.");
        Say("      The remaining leads, in order: POWERUP_DELAY (the one statement k4 does");
        Say("      not write), the radio front end, or genuinely no AP on 1/6/11."); }
    }
    Say("");
    /* ⚠ Say WHICH folder, rather than asserting one. The Desktop is the intended destination but
     * LogOpen falls back to the System Folder if FindFolder cannot resolve it, and a tester told
     * the wrong place will conclude the run produced no log. */
#endif /* !AP_APP_DRIVES_RADIO */
    Say(gLogOnDesktop ? "=== done. Log: 'AirPort RX Log' on the DESKTOP. ==="
                      : "=== done. Log: 'AirPort RX Log' in the System Folder (Desktop lookup failed). ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
