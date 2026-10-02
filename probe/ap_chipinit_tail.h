/* ap_chipinit_tail.h -- b43_chip_init statements 8 through 13, as ONE CALLABLE DEFINITION.
 *
 * ★ THE THIRD TIME THIS PROJECT HAS APPLIED THIS REMEDY, for the third identical reason.
 * ap_bringup.h exists because chip_init's PREFIX was trapped inside a probe's main();
 * ap_phy_initg.h exists because b43_phy_init was trapped inside another one. This code was
 * trapped inside airport_rx.c, so the driver could not run chip_init's TAIL without hand-copying
 * it -- and "a hand-copied sequence drifts, a called function cannot" is the sentence this
 * project keeps paying to relearn.
 *
 * ⚠ 8-2d-3 WAS THE MOST RECENT INVOICE. The driver had hand-assembled chip_init's PREFIX and
 *   was missing six of its steps -- core flags of 0, no PhyTakeOutOfReset, no GpioInit, no
 *   RadioOn -- for three increments, while three consecutive oracles passed on top of the gap.
 *   Extracting rather than copying is what stops that recurring here.
 *
 * ★ HOW IT WAS MADE. Lifted VERBATIM from airport_rx.c by line range, never retyped, and
 * proven by reconstruction: these blocks plus what stayed behind rebuild the original file byte
 * for byte. Same technique as the 8-2d-0 logging split, for the same reason -- a statement that
 * quietly changed during a move would be indistinguishable from a hardware fault later.
 *
 * ⚠ TxProbeHere DELIBERATELY DID NOT COME. It sat between MacPhyClockSet and ChipInitTail in
 *   the original and is a PROBE diagnostic -- it keys the MAC, transmits a dummy frame and
 *   reports PHY_TXERR. A driver has no business transmitting during bring-up, and nothing in
 *   ChipInitTail calls it (checked, not assumed). It stayed in airport_rx.c.
 *
 * ⚠ WHAT IT STILL DOES NOT DO, unchanged from the original and stated so it is not mistaken
 *   for drift: statement [13], POWERUP_DELAY, is not written. b43 takes that value from
 *   ssb_chipco_get_fast_pwrup_delay(), a ChipCommon slow-clock subsystem this project has never
 *   touched, and the register only matters when the chip sleeps -- which it never does here,
 *   because PowerSavingCtlBits forces hwps false and awake true. A reasoned deferral with a
 *   falsifiable condition, carried across intact.
 *
 * Caller supplies coreRev and phyRev:
 *     coreRev = IDHIGH_REV(ssb_r32(gBus.bar0,SSB_IDHIGH));
 *     phyRev  = (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_PHY_VER) & 0x000F);
 */
#ifndef AP_CHIPINIT_TAIL_H
#define AP_CHIPINIT_TAIL_H

#include "ap_phy_initg.h"   /* brings ap_bringup.h, and with it gBus, PhyWrite, Shm*, ssb_* */
/* ============================================================================
 * b43_chip_init statements 8 through 13 -- the part no probe has ever run.
 * ApBringUp() covers 1-6, ApPhyInitG() is 7, and everything below runs between
 * phy_init and b43_dma_init. k3 skipped all of it.
 * ==========================================================================*/

/* phy_a.h / phy_common.h constants, via the B43_PHY_OFDM() macro ap_phy_initg.h already has. */
#define B43_PHY_BBANDCFG            B43_PHY_OFDM(0x01)
#define   B43_PHY_BBANDCFG_RXANT    0x180
#define   B43_PHY_BBANDCFG_RXANT_SHIFT 7
#define B43_PHY_ADIVRELATED         B43_PHY_OFDM(0x27)
#define B43_PHY_OFDM61              B43_PHY_OFDM(0x61)
#define   B43_PHY_OFDM61_10         0x0010
#define B43_PHY_OFDM9B              B43_PHY_OFDM(0x9B)
#define B43_PHY_DIVSRCHGAINBACK     B43_PHY_OFDM(0xAD)
#define   B43_PHY_ANTDWELL_AUTODIV1 0x0100
#define   B43_PHY_ANTWRSETT_ARXDIV  0x2000
#define B43_ANTENNA_AUTO0           2
#define B43_ANTENNA_AUTO1           3
#define B43_ANTENNA_DEFAULT         B43_ANTENNA_AUTO0   /* = B43_ANTENNA_AUTO, phy_common.h:64 */
#define B43_HF_ANTDIVHELP_LO        0x0001              /* bit 0 -> the LOW hostflags word */
/* ★★★ 8-46 EXPERIMENT (k141), CONCLUDED -- AND UNDONE AT k196. It forced a FIXED RX antenna (0)
 * to test whether the ~half-of-boots deaf radio was auto-diversity settling on a dead antenna. The
 * tally came back 4 heard / 1 deaf on fixed 0, and deaf boots kept happening on it afterwards (k189
 * run 1, k193 run 1): the antenna is NOT the cause of the deaf boots. The experiment was never
 * reverted, so k141-k195 all ran a receive setup b43 never uses -- and it was not merely "one
 * antenna". GphySetRxAntenna's fixed arm ports b43's `b43_phy_mask(ANTWRSETT, ARXDIV)`, which KEEPS
 * only the ARXDIV bit the line before it just cleared: in fixed mode it writes 0 to the whole
 * antenna-write-settings register (the upstream bug noted at the function; nobody runs b43 fixed).
 * k195 then measured weak OFDM reception -- 70% of the AP's data frames reached us at CCK rates, and
 * a 14 s receive outage -- while Tiger on the same card and spot moves 1.7 MB/s down.
 * So: B43_ANTENNA_DEFAULT (AUTO0, auto-diversity), exactly b43's b43_wireless_core_init default.
 * The k196 snapshot's RX OFDM/CCK split and time series are the witnesses.
 *
 * ★★★ k198 EXPERIMENT: FIXED ANTENNA 1. The k197 timeline showed the receiver going mostly DEAF for
 * 77 s and again for 15 s -- our AP's beacons (1 Mbps broadcast, ~10/s) fell to 0-2/s -- while the
 * capture shows every frame the Mac TRANSMITTED in that window reaching the Pi: a RECEIVE-only
 * failure, with zero FCS-failed frames (nothing arrived corrupted; nothing was detected at all). With
 * auto-diversity 89% of good frames arrived on antenna 1 (3445 vs 423 on 0). A diversity circuit that
 * switches on what it hears can sit on a near-deaf antenna for a long time, hearing nothing that would
 * make it switch back; outages were short on fixed 0 (k195, 14-20 s) and long on auto (k196 4.8 min,
 * k197 77 s). Fixing the antenna that carried the traffic tests that directly.
 * DISCRIMINATOR (the timeline's per-second antenna columns): every frame on antenna 1 and NO deaf
 * seconds -> diversity parking on the weak antenna was the outage; then design the real fix (fixed 1
 * is wrong for Macs wired differently: a beacon-starvation watchdog that re-kicks diversity). Deaf
 * seconds WITH every frame on antenna 1 -> not the antenna: revert to B43_ANTENNA_DEFAULT in the next
 * build ([[feedback_sweep_concluded_experiments]]) and look at gain / carrier sense instead.
 *
 * ★★ k199: CONCLUDED -- AND KEPT, NOW AS THIS MAC'S SETTING RATHER THAN AN EXPERIMENT. k198 run 1 met the
 * revert condition above (deaf seconds, every frame on antenna 1), but the deaf seconds turned out to be
 * our own Bluetooth driver paging: the SAME k198 build with the A1044 unplugged had 0 deaf seconds in
 * 74. The premise the revert rested on is gone. What stands is separate evidence that antenna 1 is the
 * only CONNECTED port on this MDD -- k47 (transmit) and k197 (89% of received frames on 1 under
 * auto-diversity) -- and auto-diversity also drags transmit onto the open port, because the TX header's
 * ANT01AUTO follows the receive antenna. k198's clean run: 54 Mbps, 94% of frames acked first try.
 * ⚠ BEFORE ANY PUBLIC BUILD: other Macs may wire the card differently. This needs a per-model rule or a
 *   choice measured at join time -- never this constant shipped as a default for every Mac.
 * ★ k218 RESOLVED: the public build ships b43's auto-diversity (B43_ANTENNA_DEFAULT) at the call site in
 *   ApBringUp below -- "a choice measured at join time" (the hardware picks). AP_RX_ANT_FORCE is kept only
 *   as the historical MDD pin and is NO LONGER REFERENCED. */
#define AP_RX_ANT_FORCE             1
#define B43_SHM_SH_ACKCTSPHYCTL     0x0022UL
#define B43_SHM_SH_PRPHYCTL         0x0188UL
#define B43_SHM_SH_PRMAXTIME        0x0074UL
/* b43.h:263 -- "TSSI for last 4 CCK frames (32bit)". We transmit CCK 1 Mbps, so THIS is the word
 * the microcode updates, not the OFDM one at 0x0070. ShmClearTssi (ap_phy_initg.h) writes 0x7F7F
 * to all four halves as the "no measurement" sentinel.
 * ⚠ Declared HERE rather than with the TX constants because TxProbeHere, the boundary bisect,
 * runs long before the transmit section and needs them. */
#define B43_SHM_SH_TSSI_CCK         0x0058UL
#define B43_SHM_SH_TSSI_OFDM_G      0x0070UL
#define B43_TXH_PHY_ANT             0x03C0
#define B43_TXH_PHY_ANT01AUTO       0x00C0
#define B43_TMSLOW_MACPHYCLKEN      0x00100000UL
#define B43_MMIO_TSF_CFP_PRETBTT    0x612UL
#define B43_MMIO_POWERUP_DELAY      0x6A8UL
#define B43_MMIO_DMA0_IRQ_MASK      0x24UL
#define B43_MMIO_DMA1_IRQ_MASK      0x2CUL
#define B43_MMIO_DMA2_IRQ_MASK      0x34UL
#define B43_MMIO_DMA3_IRQ_MASK      0x3CUL
#define B43_MMIO_DMA4_IRQ_MASK      0x44UL
#define B43_MMIO_DMA5_IRQ_MASK      0x4CUL
#define B43_MACCTL_AP               0x00040000UL
#define B43_MACCTL_KEEP_BADPLCP     0x00200000UL
#define B43_MACCTL_KEEP_CTL         0x00400000UL
#define B43_MACCTL_KEEP_BAD         0x00800000UL
#define B43_MACCTL_PROMISC          0x01000000UL
#define B43_MACCTL_BEACPROMISC      0x00100000UL
#define B43_MACCTL_DISCPMQ          0x40000000UL

/* b43_gphy_op_set_rx_antenna(dev, B43_ANTENNA_DEFAULT).
 * DEFAULT is AUTO0, so autodiv is TRUE and every autodiv arm is the live one. phy->rev is 2 on
 * this card -- measured in Stage 4a -- so the `rev >= 2` block runs with its `rev == 2` arm, and
 * the `rev >= 6` write does not. */
static void GphySetRxAntenna(int antenna,UInt16 phyRev)
{
    UInt16 tmp;
    int autodiv = (antenna==B43_ANTENNA_AUTO0 || antenna==B43_ANTENNA_AUTO1);
    UInt16 lo,mi,hi;

    HfRead(&lo,&mi,&hi); lo=(UInt16)(lo & ~B43_HF_ANTDIVHELP_LO); HfWrite(lo,mi,hi);

    PhyMaskSet((UInt16)B43_PHY_BBANDCFG,(UInt16)~B43_PHY_BBANDCFG_RXANT,
               (UInt16)((autodiv?B43_ANTENNA_AUTO1:antenna) << B43_PHY_BBANDCFG_RXANT_SHIFT));

    if(autodiv){
      tmp = PhyRead((UInt16)B43_PHY_ANTDWELL);
      if(antenna==B43_ANTENNA_AUTO1) tmp=(UInt16)(tmp & ~B43_PHY_ANTDWELL_AUTODIV1);
      else                           tmp=(UInt16)(tmp |  B43_PHY_ANTDWELL_AUTODIV1);
      PhyWrite((UInt16)B43_PHY_ANTDWELL,tmp); }

    tmp = PhyRead((UInt16)B43_PHY_ANTWRSETT);
    if(autodiv) tmp=(UInt16)(tmp |  B43_PHY_ANTWRSETT_ARXDIV);
    else        tmp=(UInt16)(tmp & ~B43_PHY_ANTWRSETT_ARXDIV);
    PhyWrite((UInt16)B43_PHY_ANTWRSETT,tmp);

    /* ⚠ b43 writes ANTWRSETT TWICE here: b43_phy_set(ARXDIV) vs b43_phy_mask(ARXDIV). But
     * b43_phy_mask(reg,mask) ANDs with the mask AS GIVEN, so upstream's fixed-antenna arm KEEPS only
     * ARXDIV -- which the write above just cleared -- and so writes 0 to the whole register. An
     * upstream bug nobody meets (Linux always runs auto); k141-k195 met it on every boot. k198 runs a
     * fixed antenna ON PURPOSE, so this arm now does what the write above plainly intends: clear
     * ARXDIV and keep every other bit (b43's own idiom elsewhere is b43_phy_mask(dev, reg, ~BIT)). */
    if(autodiv) PhySet((UInt16)B43_PHY_ANTWRSETT,B43_PHY_ANTWRSETT_ARXDIV);
    else        PhyWrite((UInt16)B43_PHY_ANTWRSETT,
                         (UInt16)(PhyRead((UInt16)B43_PHY_ANTWRSETT) & (UInt16)~B43_PHY_ANTWRSETT_ARXDIV));

    if(phyRev >= 2){
      PhySet((UInt16)B43_PHY_OFDM61,B43_PHY_OFDM61_10);
      PhyMaskSet((UInt16)B43_PHY_DIVSRCHGAINBACK,0xFF00,0x15);
      if(phyRev == 2) PhyWrite((UInt16)B43_PHY_ADIVRELATED,8);
      else            PhyMaskSet((UInt16)B43_PHY_ADIVRELATED,0xFF00,8); }
    if(phyRev >= 6) PhyWrite((UInt16)B43_PHY_OFDM9B,0xDC);

    HfRead(&lo,&mi,&hi); lo=(UInt16)(lo | B43_HF_ANTDIVHELP_LO); HfWrite(lo,mi,hi);
}

/* b43_mgmtframe_txantenna(dev, B43_ANTENNA_DEFAULT). DEFAULT is AUTO0, and
 * b43_antenna_to_phyctl maps AUTO0/AUTO1 to B43_TXH_PHY_ANT01AUTO. */
static void MgmtFrameTxAntenna(void)
{
    UInt16 ant = B43_TXH_PHY_ANT01AUTO, tmp;
    tmp = ShmRead16Shared(B43_SHM_SH_ACKCTSPHYCTL);
    tmp = (UInt16)((tmp & ~B43_TXH_PHY_ANT) | ant);
    ShmWrite16Shared(B43_SHM_SH_ACKCTSPHYCTL,tmp);
    tmp = ShmRead16Shared(B43_SHM_SH_PRPHYCTL);
    tmp = (UInt16)((tmp & ~B43_TXH_PHY_ANT) | ant);
    ShmWrite16Shared(B43_SHM_SH_PRPHYCTL,tmp);
}

/* b43_adjust_opmode. We are a STA in infrastructure mode with no filter flags set, so every
 * `if (wl->filter_flags & ...)` arm is false and the reset-to-STA block is the whole function.
 * ⚠ The core_rev <= 4 workaround FORCES PROMISC ("the HW-MAC-address-filter doesn't work
 * properly"), which matters here: on such a core the card accepts everything and software
 * filters. Decided by the measured core revision, not assumed either way. */
static UInt32 AdjustOpmode(UInt32 coreRev,UInt32 chipId,UInt32 chipRev,UInt16 *pretbttOut)
{
    UInt32 ctl;
    UInt16 cfp_pretbtt;

    ctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    ctl &= ~B43_MACCTL_AP;
    ctl &= ~B43_MACCTL_KEEP_CTL;
    ctl &= ~B43_MACCTL_KEEP_BADPLCP;
    ctl &= ~B43_MACCTL_KEEP_BAD;
    ctl &= ~B43_MACCTL_PROMISC;
    ctl &= ~B43_MACCTL_BEACPROMISC;
    ctl |=  B43_MACCTL_INFRA;
    /* AP/mesh/adhoc arms: not taken, we are a STA. filter_flags: none set. */
    if(coreRev <= 4) ctl |= B43_MACCTL_PROMISC;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,ctl);

    cfp_pretbtt = 2;
    if((ctl & B43_MACCTL_INFRA) && !(ctl & B43_MACCTL_AP)){
      if(chipId==0x4306UL && chipRev==3UL) cfp_pretbtt = 100;
      else                                 cfp_pretbtt = 50; }
    ssb_w16(gBus.bar0,B43_MMIO_TSF_CFP_PRETBTT,cfp_pretbtt);
    *pretbttOut = cfp_pretbtt;

    /* PMQ is not implemented, so b43 always takes the else arm and SETS DISCPMQ. */
    ctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL) | B43_MACCTL_DISCPMQ;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,ctl);
    return ctl;
}

/* b43_mac_phy_clock_set(dev, true) -- a TMSLOW bit, so it needs the 802.11 core selected. */
static void MacPhyClockSet(int on)
{
    UInt32 tmp = ssb_r32(gBus.bar0,SSB_TMSLOW);
    if(on) tmp |=  B43_TMSLOW_MACPHYCLKEN;
    else   tmp &= ~B43_TMSLOW_MACPHYCLKEN;
    ssb_w32(gBus.bar0,SSB_TMSLOW,tmp);
}

/* b43_chip_init, statements 8 through 13, in order. */
static void ChipInitTail(UInt32 coreRev,UInt16 phyRev,UInt32 chipId,UInt32 chipRev,
                         UInt32 *macctlOut,UInt16 *pretbttOut)
{
    UInt32 macctl;

    /* [8] Disable Interference Mitigation.
     * ⚠ PROVABLY INERT HERE, WHICH IS WHY NO BODY IS PORTED -- this is a proof, not a skip.
     * b43_gphy_op_interf_mitigation does:
     *     currentmode = gphy->interfmode;
     *     if (currentmode == mode) return 0;
     * gphy is memset to zero by prepare_structs, and B43_INTERFMODE_NONE is the FIRST value of
     * enum b43_interference_mitigation (phy_common.h:48), so it is 0. currentmode == mode, and
     * the function returns before writing a single register. Porting its body would mean
     * porting code that cannot execute in this state. */

    /* [9] Select the antennae. ★ k218: ship b43's standard auto-diversity (B43_ANTENNA_DEFAULT = AUTO0),
     * NOT a fixed antenna. The MDD has a single antenna on port 1, but other Macs wire the card
     * differently -- some on port 0, laptops use two for diversity -- so a fixed pin would make them
     * deaf. Auto-diversity lets the hardware pick the connected/better antenna per machine, which is
     * exactly the "choice measured at join time" the define's warning asked for. On the MDD this is
     * still fine: k197 received under auto-diversity (89% on antenna 1), and the deaf spells once blamed
     * on diversity were our own Bluetooth sweep (k199). The old MDD-only AP_RX_ANT_FORCE pin and its full
     * history are at the define above. TX antenna stays auto. The three antenna registers are read back
     * so the log shows what the PHY holds, not what we meant. */
    Say("  [9] RX antenna = AUTO (b43 auto-diversity, B43_ANTENNA_DEFAULT) -- general across Macs");
    GphySetRxAntenna(B43_ANTENNA_DEFAULT,phyRev);
    SayH("      BBANDCFG (RXANT field = bits 7-8) = ",(unsigned long)PhyRead((UInt16)B43_PHY_BBANDCFG),4);
    SayH("      ANTWRSETT (ARXDIV = 0x2000)       = ",(unsigned long)PhyRead((UInt16)B43_PHY_ANTWRSETT),4);
    SayH("      ANTDWELL                          = ",(unsigned long)PhyRead((UInt16)B43_PHY_ANTDWELL),4);
    MgmtFrameTxAntenna();

    /* phy->type == B43_PHYTYPE_B arm (read 0x005E, set 0x0004): NOT TAKEN, this is a G-PHY. */

    ssb_w32(gBus.bar0,0x0100UL,0x01000000UL);
    if(coreRev < 5) ssb_w32(gBus.bar0,0x010CUL,0x01000000UL);

    /* INFRA cleared and then set again. b43 does exactly this, as two masksets. */
    macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL) & ~B43_MACCTL_INFRA;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);
    macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL) | B43_MACCTL_INFRA;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);

    /* [10] Probe Response Timeout. b43's own comment is "FIXME: Default to 0". */
    ShmWrite16Shared(B43_SHM_SH_PRMAXTIME,0);

    /* [11] Initially set the wireless operation mode. */
    *macctlOut = AdjustOpmode(coreRev,chipId,chipRev,pretbttOut);

    if(coreRev < 3){
      ssb_w16(gBus.bar0,0x060EUL,0x0000);
      ssb_w16(gBus.bar0,0x0610UL,0x8000);
      ssb_w16(gBus.bar0,0x0604UL,0x0000);
      ssb_w16(gBus.bar0,0x0606UL,0x0200);
    } else {
      ssb_w32(gBus.bar0,0x0188UL,0x80000000UL);
      ssb_w32(gBus.bar0,0x018CUL,0x02000000UL); }

    ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0x00004000UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA0_IRQ_MASK,0x0001FC00UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA1_IRQ_MASK,0x0000DC00UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA2_IRQ_MASK,0x0000DC00UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA3_IRQ_MASK,0x0001DC00UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA4_IRQ_MASK,0x0000DC00UL);
    ssb_w32(gBus.bar0,B43_MMIO_DMA5_IRQ_MASK,0x0000DC00UL);
    /* ⚠ These arm DMA interrupt SOURCES. Nothing is installed to service them -- this probe
     * still polls and holds no interrupt handler. b43 writes them here, before it ever enables
     * the line, so writing them here is faithful and inert. */

    /* [12] MAC/PHY clock. */
    MacPhyClockSet(1);

    /* [13] POWERUP_DELAY.
     * ⚠ DELIBERATELY NOT WRITTEN, and this is the one statement of chip_init that k4 does not
     * run. b43 writes ssb's chipco.fast_pwrup_delay, which is computed by
     * ssb_chipco_get_fast_pwrup_delay() from the ChipCommon slow-clock configuration -- a
     * subsystem this project has never touched. The register sets how long the chip waits when
     * waking from sleep, and k4 runs with power saving OFF: PowerSavingCtlBits forces
     * hwps = false and awake = true, so the chip never sleeps and never consults it.
     * That is a reasoned deferral with a falsifiable condition attached, not a guess -- and if
     * k4 still hears nothing, this is the first thing to come back to. The register's current
     * value is logged so the next increment starts from a measurement. */
}

#endif /* AP_CHIPINIT_TAIL_H */
