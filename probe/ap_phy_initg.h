/* ap_phy_initg.h -- Stage 4's G-PHY initialisation, as ONE CALLABLE DEFINITION.
 *
 * ★★★★★ WHY THIS FILE EXISTS. Stage 4 finished with b43_phy_initg fully ported and all fifteen
 * oracles passing -- and then k3 failed to receive a single frame because that work was not
 * reachable. Every line of it lived inside airport_initb6.c's main(), interleaved with narration
 * and oracle bookkeeping, so the only ways to reuse it were to hand-copy it or to build on
 * ApBringUp(), which stops one statement short of b43_phy_init and says so in its own header.
 * k3 did the latter and enabled a MAC whose PHY had never been initialised.
 *
 * This is the same remedy ap_bringup.h applied to b43_chip_init's prefix, for the same reason:
 * a hand-copied sequence drifts, a called function cannot.
 *
 * ★ HOW IT WAS MADE, AND HOW TO CHECK THAT. Extracted MECHANICALLY from airport_initb6.c at the
 * j7 commit -- the one whose log passed oracles A through O -- by a script that copied whole line
 * ranges and never retyped a statement:
 *     helpers   airport_initb6.c:117-1533    the 48 static functions and all file-scope state
 *     sequence  airport_initb6.c:1575-2792   main()'s body, from initb6's first statement
 *     verdict   airport_initb6.c:2794-2840   the oracle report
 * Excluded, deliberately: main()'s Toolbox prologue, its window, its ApBringUp() call, its event
 * loop and its FSClose -- everything that belongs to a standalone application rather than to the
 * sequence. A probe supplies those.
 *
 * ⚠ EXACTLY ONE LINE OF THE EXTRACTED BODY WAS CHANGED, and it is recorded here so it is not
 * mistaken for drift: the refusal path at airport_initb6.c:1721, which reads
 *     if(rmanuf!=0x017FU||rver!=0x2050U||rrev!=2U){ ... goto verdict; }
 * had its `goto verdict` retargeted to `goto initg_done`, because the label it jumped to was
 * main()'s and is now this function's. Nothing else differs; the extraction is diff-verified
 * against the ranges above.
 *
 * ★ THE NARRATION IS KEPT ON PURPOSE. Stripping the ~680 Say() lines would have meant editing
 * the body rather than copying it, which is precisely the risk this file exists to remove. The
 * side effect is worth more than the log space it costs: every probe that calls ApPhyInitG()
 * re-runs all fifteen Stage 4 oracles as a regression, so a later change that breaks the PHY
 * init announces itself instead of surfacing as a mysterious failure downstream.
 *
 * ★ WHAT IT DOES NOT DO. ApPhyInitG() is b43_phy_init's body only. It does not call ApBringUp()
 * -- the caller does that first -- and it does not run b43_chip_init's statements AFTER
 * phy_init: interference mitigation, antenna selection, PRMAXTIME, b43_adjust_opmode,
 * mac_phy_clock_set and the rest. Those are the caller's, exactly as phy_init was ApBringUp's
 * caller's. Stopping at a function boundary is the habit; guessing at what is required is not.
 *
 * Returns 1 if all fifteen oracles passed, 0 otherwise.
 */
#ifndef AP_PHY_INITG_H
#define AP_PHY_INITG_H

#include "ap_bringup.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#define AP_B6_VER "j7"

/* ---- The b43 read-modify-write helpers, semantics taken from phy_common.c. ---- */
static void PhySet(UInt16 reg,UInt16 set){ PhyWrite(reg,(UInt16)(PhyRead(reg)|set)); }
/* i1 deliberately did NOT define PhyMaskSet, because its only caller then was in the untaken
 * tail. i2 needs it: b43_gphy_set_baseband_attenuation uses b43_phy_maskset on DACCTL. It arrives
 * with the increment that needs it, exactly as i1 said it would. */
static void PhyMaskSet(UInt16 reg,UInt16 mask,UInt16 set)
{ PhyWrite(reg,(UInt16)((PhyRead(reg)&mask)|set)); }
static void RadioSet(UInt16 reg,UInt16 set){ RadioWrite(reg,(UInt16)(RadioRead(reg)|set)); }
static void RadioMaskSet(UInt16 reg,UInt16 mask,UInt16 set)
{ RadioWrite(reg,(UInt16)((RadioRead(reg)&mask)|set)); }

/* Recompute what each PHY table loop should have written, so the oracle compares against the
 * formula rather than against a transcribed table. */
static UInt16 ExpectLoop1(UInt16 off){ return (UInt16)(0x1E1FU - 0x0202U*(UInt16)(off-0x0088U)); }
static UInt16 ExpectLoop2(UInt16 off){ return (UInt16)(0x3E3FU - 0x0202U*(UInt16)(off-0x0098U)); }
static UInt16 ExpectLoop3(UInt16 off){ return (UInt16)((0x2120U + 0x0202U*(UInt16)(off-0x00A8U))
                                                       & 0x3F3FU); }

/* ---- Registers and constants that arrive with lo_measure_setup / lo_measure_restore.
 * Bank macros confirmed from phy_common.h: BASE/CCK 0x0000, OFDM 0x0400, ExtG 0x0800. ---- */
#define B43_PHY_OFDM(r)       ((r) | 0x0400UL)
#define B43_PHY_SYNCCTL       B43_PHY_CCK(0x35)
#define B43_PHY_CLASSCTL      B43_PHY_EXTG(0x02)
#define B43_PHY_LO_MASK       B43_PHY_EXTG(0x0F)
#define B43_PHY_RFOVER        B43_PHY_EXTG(0x11)
#define B43_PHY_RFOVERVAL     B43_PHY_EXTG(0x12)
#define B43_PHY_ANALOGOVER    B43_PHY_EXTG(0x14)
#define B43_PHY_ANALOGOVERVAL B43_PHY_EXTG(0x15)
#define B43_PHY_CRS0          B43_PHY_OFDM(0x29)
#define B43_MMIO_3E2          0x3E2UL

/* Mirror of b43's `struct lo_g_saved_values`, carrying ONLY the fields the branches this card
 * takes actually use. The hwpctl fields are absent because b43_gphy_op_supports_hwpctl returns
 * (phy->rev >= 6) and this PHY is rev 2, so b43_has_hardware_pctl is false here no matter what
 * the module parameter says -- that whole block is unreachable in both setup and restore. The
 * B-PHY fields (cck_30, cck_06) are absent for the same reason: this is a G-PHY. */
typedef struct {
  UInt8  old_channel;
  UInt16 reg_3F4, reg_3E2;
  UInt16 radio_43, radio_7A, radio_52;
  UInt16 phy_pgactl, phy_cck_2A, phy_syncctl, phy_dacctl;
  UInt16 phy_analogover, phy_analogoverval, phy_rfover, phy_rfoverval;
  UInt16 phy_classctl, phy_cck_3E, phy_crs0;
} LoSaved;

/* ---- Registers and constants that arrive with set_txpower_g. ---- */
#define B43_PHY_DACCTL        B43_PHY_CCK(0x60)   /* DAC control */
#define B43_SHM_SH_RFATT      0x0064UL            /* "Current radio attenuation value" */
#define B43_TXCTL_PA3DB       0x40
#define B43_TXCTL_PA2DB       0x20
#define B43_TXCTL_TXMIX       0x10
#define SSB_BOARDVENDOR_BCM   0x14E4
#define SSB_BOARD_BCM4309G    0x0421
#define SSB_BOARD_BU4306      0x0416

/* ---- b43's default-selection functions, ported as FUNCTIONS rather than as transcribed
 * constants. The probe measures radio ver/rev, chip id and the PCI subsystem IDs at runtime and
 * derives the same values b43 would, so a card that is not the one these were reasoned about
 * produces a refusal instead of a silently wrong attenuation. ---- */
static UInt16 DefaultBasebandAtt(UInt16 radioVer,UInt16 radioRev)
{ if(radioVer==0x2050U && radioRev<6U) return 0; return 2; }

/* Only the 0x2050 / rev 2 / G-PHY arm is ported -- the arm this card actually takes. Other radio
 * revisions are refused by the caller rather than implemented blind, on the same grounds i1 gave
 * for not porting the rev 4/5/8 register blocks: the hardware that would enter them is not the
 * hardware in the machine, so the port could never be tested. */
static UInt16 DefaultRadioAtt_2050rev2_G(UInt32 chipId,UInt16 boardVendor,UInt16 boardType,
                                         UInt16 boardRev)
{ if(boardVendor==SSB_BOARDVENDOR_BCM && boardType==SSB_BOARD_BCM4309G && boardRev>=30) return 3;
  if(boardVendor==SSB_BOARDVENDOR_BCM && boardType==SSB_BOARD_BU4306)                   return 5;
  if(chipId==0x4320UL)                                                                  return 4;
  return 3; }

static UInt16 DefaultTxControlRaw(UInt16 radioVer,UInt16 radioRev)
{ if(radioVer!=0x2050U)  return 0;
  if(radioRev==1U)       return B43_TXCTL_PA2DB|B43_TXCTL_TXMIX;
  if(radioRev<6U)        return B43_TXCTL_PA2DB;
  if(radioRev==8U)       return B43_TXCTL_TXMIX;
  return 0; }

/* ---- The PHY TABLE accessors, arriving with 4b-ii-5a. Shifts and masks read from phy_a.h /
 * phy_g.h, not assumed -- both table-number fields are 6 bits at shift 10 with a 10-bit offset
 * field (0x03FF), and getting that wrong would silently place every table write in the wrong
 * table with no fault whatsoever. ---- */
#define B43_PHY_OTABLECTL     B43_PHY_OFDM(0x72)
#define B43_PHY_OTABLEI       B43_PHY_OFDM(0x73)
#define B43_PHY_GTABCTL       B43_PHY_EXTG(0x03)
#define B43_PHY_GTABDATA      B43_PHY_EXTG(0x04)
#define B43_OFDMTAB(n,off)    ((UInt16)(((n)<<10)|(off)))   /* OTABLENR_SHIFT = 10 */
#define B43_GTAB(n,off)       ((UInt16)(((n)<<10)|(off)))   /* GTABNR_SHIFT   = 10 */
#define B43_OFDMTAB_RSSI      B43_OFDMTAB(0x10,0)           /* = 0x4000 */
#define B43_GTAB_ORIGTR       B43_GTAB(0x2E,0x298)          /* = 0xBA98 */
#define B43_PHY_CRSTHRES1     B43_PHY_OFDM(0xC0)
#define B43_PHY_CRSTHRES2     B43_PHY_OFDM(0xC1)
#define B43_PHY_ANTDWELL      B43_PHY_OFDM(0x2B)

/* gphy's OFDM table address cache. ⚠ THIS IS THE RISK IN 4b-ii-5a. The hardware auto-increments
 * its internal table pointer on each data access, so b43 SKIPS re-writing the address whenever
 * the next access is sequential AND in the same direction. That is a stateful optimisation: if
 * our copy of the state diverges from the hardware's, writes land at the wrong offsets and
 * nothing faults. It is the accessor-layer problem of 4b-ii-1 all over again. */
#define AP_OTAB_DIR_UNKNOWN 0
#define AP_OTAB_DIR_READ    1
#define AP_OTAB_DIR_WRITE   2
static UInt16 gOtabAddr=0; static int gOtabDir=AP_OTAB_DIR_UNKNOWN;
static UInt16 gOfdmRev=0;

/* ---- calc_nrssi_slope and its helpers, arriving with 4b-ii-9a. ---- */
#define B43_PHY_NRSSILT_CTRL  0x0803UL
#define B43_PHY_NRSSILT_DATA  0x0804UL
/* ⚠ TWO REGISTERS WITH TWO NAMES EACH, and this is the sort of thing that produces a silent
 * double-write. b43.h defines B43_PHY_G_CRS as 0x0429, which IS B43_PHY_CRS0 (OFDM 0x29); and
 * B43_PHY_G_LO_CONTROL as 0x0810, which IS B43_PHY_LO_CTL (ExtG 0x10). Same silicon, different
 * spelling depending on which header the caller came from. */
#define B43_PHY_G_CRS         B43_PHY_CRS0     /* 0x0429 */
#define B43_PHY_G_LO_CONTROL  B43_PHY_LO_CTL   /* 0x0810 */
static SInt16 gNrssi0=0,gNrssi1=0;
static SInt32 gNrssiSlope=0;
static SInt16 gNrssiLt[64];
static int gNrssiRestoreBad=0;
static UInt16 gThr048A=0;
static SInt32 gThrA=0,gThrB=0;

/* ---- b43_radio_init2050, arriving with 4b-ii-7. ---- */
#define B43_MMIO_PHY_RADIO  0x3E2UL            /* same register the setup ORs 0x8000 into */
#define LPD(L,P,D)          (((L)<<2)|((P)<<1)|((D)<<0))
static UInt16 gRcc=0,gRadio78=0,gInitval=0,gRadio60=0;
static UInt32 gTmp1=0,gTmp2=0;
static int gInitvalFromRadio78=0,gI2050RestoreBad=0,gLoop2Iters=0;
static UInt16 gRfoverA=0,gRfoverValA=0,gRfoverValB=0,gRfoverValC=0;

/* ---- b43_calc_loopback_gain, arriving with 4b-ii-6. ---- */
#define B43_PHY_CCKBBANDCFG   B43_PHY_CCK(0x01)
static UInt16 gLlMin=0xFFFF,gLlMax=0; static UInt32 gLlCount=0;
static int gLbOuter=0,gLbInner=0,gLbRestoreBad=0,gLbTrswUnderflow=0;
static UInt16 gLbTrsw=0;
static SInt16 gLbMaxGain=0;
/* gphy->bbatt.att as STATE rather than a local, because calc_loopback_gain saves it, overwrites
 * it with 11, and restores it -- so it has to live somewhere both that function and the
 * set_txpower_g path can see. */
static UInt16 bbAttG=0,rfAttG=0;

/* ---- wa_all's remaining nine, arriving with 4b-ii-5b. ---- */
#define B43_PHY_VERSION_OFDM   B43_PHY_OFDM(0x00)
#define B43_PHY_PWRDOWN        B43_PHY_OFDM(0x03)
#define B43_PHY_LPFGAINCTL     B43_PHY_OFDM(0x20)
#define B43_PHY_ANTWRSETT      B43_PHY_OFDM(0x8C)
#define B43_PHY_N1P1GAIN       B43_PHY_OFDM(0xA0)
#define B43_PHY_P1P2GAIN       B43_PHY_OFDM(0xA1)
#define B43_PHY_N1N2GAIN       B43_PHY_OFDM(0xA2)
#define B43_PHY_CCKSHIFTBITS_WA B43_PHY_OFDM(0xA5)
#define B43_PHY_DIVSRCHIDX     B43_PHY_OFDM(0xA8)
#define B43_PHYVER_VERSION     0x00FF
#define B43_OFDMTAB_AGC1       B43_OFDMTAB(0x00,0)   /* 0x0000 */
#define B43_OFDMTAB_GAINX      B43_OFDMTAB(0x01,0)   /* 0x0400 */
#define B43_OFDMTAB_AGC3       B43_OFDMTAB(0x02,0)   /* 0x0800 */
#define B43_OFDMTAB_NOISESCALE B43_OFDMTAB(0x05,0)   /* 0x1400 */
#define B43_OFDMTAB_AGC2       B43_OFDMTAB(0x06,0)   /* 0x1800 */
#define B43_OFDMTAB_DAC        B43_OFDMTAB(0x0C,0)   /* 0x3000 */
#define B43_OFDMTAB_UNKNOWN_0F B43_OFDMTAB(0x0F,0)   /* 0x3C00 */
#define B43_OFDMTAB_UNKNOWN_11 B43_OFDMTAB(0x11,4)   /* 0x4404 */
#define B43_OFDMTAB_MINSIGSQ   B43_OFDMTAB(0x14,0)   /* 0x5000 */
#define B43_OFDMTAB_WRSSI      B43_OFDMTAB(0x04,0)   /* 0x1000 */
#define B43_BFL_FEM            0x0800UL
#define B43_BFL_EXTLNA         0x1000UL

/* Static tables, transcribed from tables.c. ⚠ The loop bounds are taken with
 * sizeof(t)/sizeof(t[0]) rather than from B43_TAB_*_SIZE, which lives in a tables.h this port
 * does not have. That is not a shortcut: the array length is what bounds correctness, so deriving
 * the count from the data makes it IMPOSSIBLE for the bound and the data to disagree -- which a
 * transcribed constant could. */
static const UInt16 kTabNoiseG2[]={
  0x5484,0x3C40,0x0000,0x0000, 0x0000,0x0000,0x0000,0x0000 };
static const UInt16 kTabNoiseScaleG1[]={
  0x6C77,0x5162,0x3B40,0x3335, 0x2F2D,0x2A2A,0x2527,0x1F21,
  0x1A1D,0x1719,0x1616,0x1414, 0x1414,0x1400,0x1414,0x1614,
  0x1716,0x1A19,0x1F1D,0x2521, 0x2A27,0x2F2A,0x332D,0x3B35,
  0x5140,0x6C62,0x0077 };
static const UInt16 kTabSigmaSqr2[]={
  0x00DE,0x00DC,0x00DA,0x00D8, 0x00D6,0x00D4,0x00D2,0x00CF,
  0x00CD,0x00CA,0x00C7,0x00C4, 0x00C1,0x00BE,0x00BE,0x00BE,
  0x00BE,0x00BE,0x00BE,0x00BE, 0x00BE,0x00BE,0x00BE,0x00BE,
  0x00BE,0x00BE,0x0000,0x00BE, 0x00BE,0x00BE,0x00BE,0x00BE,
  0x00BE,0x00BE,0x00BE,0x00BE, 0x00BE,0x00BE,0x00BE,0x00BE,
  0x00C1,0x00C4,0x00C7,0x00CA, 0x00CD,0x00CF,0x00D2,0x00D4,
  0x00D6,0x00D8,0x00DA,0x00DC, 0x00DE };
#define kNoiseG2N      (sizeof(kTabNoiseG2)/sizeof(kTabNoiseG2[0]))
#define kNoiseScaleG1N (sizeof(kTabNoiseScaleG1)/sizeof(kTabNoiseScaleG1[0]))
#define kSigmaSqr2N    (sizeof(kTabSigmaSqr2)/sizeof(kTabSigmaSqr2[0]))

/* ---- The LO search, arriving with 4b-ii-4c. ---- */
#define B43_PHY_LO_LEAKAGE            B43_PHY_CCK(0x2D)  /* Measured LO leakage */
#define B43_PHY_LO_CTL                B43_PHY_EXTG(0x10) /* Local Oscillator control */
/* B43_PHY_RFOVERVAL is already defined with the lo_measure_setup constants above. */
#define B43_PHY_RFOVERVAL_EXTLNA      0x8000
#define B43_PHY_RFOVERVAL_LNA         0x7000
#define B43_PHY_RFOVERVAL_LNA_SHIFT   12
#define B43_PHY_RFOVERVAL_PGA         0x0F00
#define B43_PHY_RFOVERVAL_PGA_SHIFT   8
#define B43_PHY_RFOVERVAL_UNK         0x0010   /* "Unknown, always set." */
#define B43_PHY_RFOVERVAL_TRSWRX      0x00E0
#define B43_PHY_RFOVERVAL_BW_LPF      0x0001
#define B43_PHY_RFOVERVAL_BW_LBW      0x0002

typedef struct { int i,q; } LoCtl;

/* gphy fields the search reads and writes. All zero at this point: prepare_structs memsets gphy,
 * and b43_calc_loopback_gain -- the only other writer of max_lb_gain and trsw_rx_gain -- runs at
 * b43_phy_initg line 45, AFTER initb6 at line 10. */
/* ⚠ ALL FIVE ARE s16 IN b43 (phy_g.h:159-163), and that is not cosmetic. max_lb_gain spans
 * -47..+85 across reachable sweep exits, and the two arithmetic consumers are SIGNED:
 *     max_rx_gain += gphy->max_lb_gain;      (int += s16)
 *     trsw_rx_gain = gphy->trsw_rx_gain / 2; (int = s16 / 2)
 * Stored as UInt16, a max_lb_gain of -11 would enter the LO search as +65525 and a negative
 * trsw_rx_gain would take an unsigned division. This run measured +19 and +48 so nothing would
 * have broken today -- which is exactly why it is worth fixing now rather than when it bites. */
static SInt16 gLnaGain=0,gPgaGain=0,gLnaLodGain=0,gTrswRxGain=0,gMaxLbGain=0;

/* Feedthrough statistics. ⚠ THESE ARE THE POINT. The search's output is MEASURED, not computed,
 * so "it terminated" proves nothing on its own: if B43_PHY_LO_LEAKAGE returned a constant, the
 * hill-climb would find no improvement anywhere, return its starting (0,0), and look like a
 * success. A sensor that never moves is indistinguishable from a perfect starting point unless
 * the spread is recorded. */
static UInt16 gFtFirst=0,gFtMin=0xFFFF,gFtMax=0;
static UInt32 gFtCount=0;

/* ---- TX-engine and packet-RAM registers, arriving with b43_dummy_transmission. ---- */
#define B43_MMIO_RAM_CONTROL      0x130UL
#define B43_MMIO_RAM_DATA         0x134UL
#define B43_MMIO_TXE0_CTL         0x500UL
#define B43_MMIO_TXE0_AUX         0x502UL
#define B43_MMIO_TXE0_WM_0        0x508UL
#define B43_MMIO_TXE0_WM_1        0x50AUL
#define B43_MMIO_TXE0_PHYCTL      0x50CUL
#define B43_MMIO_TXE0_STATUS      0x50EUL
#define B43_MMIO_XMTTPLATETXPTR   0x54CUL
#define B43_MMIO_XMTSEL           0x568UL
#define B43_MMIO_XMTTXCNT         0x56AUL
#define B43_MMIO_IFSSTAT          0x690UL
#define B43_MMIO_WEPCTL           0x7C0UL
#define B43_MACCTL_BE             0x00010000UL   /* Big Endian mode */

/* Loop-exit counts from b43_dummy_transmission's three handshake waits. These ARE the oracle:
 * each loop polls a hardware status bit, so a count equal to the limit means the bit never
 * changed and the transmission did not happen. */
static int gTxLoop1=-1,gTxLoop2=-1,gTxLoop3=-1;
static UInt16 gRadioPctlReg=0;
static int gTxA1=-1,gTxA2=-1,gTxA3=-1,gTxB1=-1,gTxB2=-1,gTxB3=-1;
static UInt16 gTxAStat=0,gTxBStat=0;
static UInt16 gTxBase=0,gTxTrig=0,gTxIfsTrig=0,gTxFinal=0,gTxIfsFinal=0;
static UInt16 gLowestFeedth=0,gFt1First=0,gFt1Min=0,gFt1Max=0,gLow1=0,gLow2=0;
static UInt32 gFt1Count=0,gFt2Count=0;
static UInt16 gPassSeed[8],gPassLow[8],gPassMul[8];
static int gPassN=0;
static int gLoI=0,gLoQ=0,gLoI2=0,gLoQ2=0,gOuter1=0,gSteps1=0,gOuter2=0,gSteps2=0;
static int gLoI3=0,gLoQ3=0; static UInt32 gFt3Count=0; static UInt16 gLow3=0;
static UInt16 gTxStat1=0,gTxStat2=0,gTxIfs=0;

/* ---- b43_ram_write. Writes the card's packet RAM through RAM_CONTROL/RAM_DATA.
 * ⚠ The swab32 is conditional on B43_MACCTL_BE, a DEVICE configuration bit, not a host property.
 * Our ssb_w32 already byte-reverses at the bus (stwbrx), exactly as Linux's iowrite32 does on
 * every host, so the two are not the same mechanism and must not be conflated. We never set BE,
 * so the branch should not be taken -- but it is ported and the bit is logged rather than
 * assumed, because getting this backwards would corrupt the frame silently. ---- */
static void RamWrite(UInt16 offset,UInt32 val)
{
  UInt32 macctl=ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
  if(macctl&B43_MACCTL_BE)
    val=((val>>24)&0x000000FFUL)|((val>>8)&0x0000FF00UL)|
        ((val<<8)&0x00FF0000UL)|((val<<24)&0xFF000000UL);
  ssb_w32(gBus.bar0,B43_MMIO_RAM_CONTROL,(UInt32)offset);
  ssb_w32(gBus.bar0,B43_MMIO_RAM_DATA,val);
}

/* ---- b43_dummy_transmission(dev, ofdm=false, pa_on=true).
 * ⚠⚠ THIS MAKES THE MAC TRANSMIT. It is the first time this card emits anything under OS 9.
 * b43 runs it during every G-PHY init on every card it supports, on channel 6 at the attenuation
 * initb6 just programmed, for a handful of microseconds -- it is a calibration aid, not traffic.
 *
 * Branches for this card (core rev 5, PHY G, radio 0x2050 rev 2):
 *   ofdm = false      -> max_loop 0xFA, buffer[0] = 0x000B846E
 *   core_rev < 11     -> WEPCTL = 0x0000
 *   PHY type N/LP/LCN -> NO, so TXE0_PHYCTL1 is not written
 *   pa_on = true      -> the N-only PA override is not reached
 *   default PHY arm   -> TXE0_AUX = 0x0030
 *   radio_rev <= 5    -> radio 0x51 = 0x0017 before the waits, 0x0037 after
 * ---- */
/* Read-only probe of the two status ports, both proven readable by i4. Free to call anywhere. */
static void TxStat(const char*label)
{ Str255 L;L[0]=0;PCat(L,"        ");PCat(L,label);
  PCat(L,"  TXE0_STATUS=0x");PCatHex(L,ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS),4);
  PCat(L,"  IFSSTAT=0x");PCatHex(L,ssb_r16(gBus.bar0,B43_MMIO_IFSSTAT),4);Out(L); }

/* variant 0 = b43's sequence (what i4 ran). variant 1 = b43legacy's, which differs in exactly
 * two places for this chip family and is a SHIPPED driver for this same BCM4306:
 *   - a dummy read of MACCTL right after the RAM writes, which b43 does not do
 *   - TXE0_PHYCTL (0x50C) written 0x0000, where b43 writes 0x40 for the CCK case
 * Both are driver-sanctioned sequences, so running both is an A/B between two references, not an
 * experiment on undocumented silicon. */
static void DummyTransmission(int legacyVariant)
{
  static const UInt32 kBuf[5]={0x000B846EUL,0x00D40000UL,0x00000000UL,
                               0x01000000UL,0x00000000UL};
  int i; UInt16 v;
  UInt32 macctl=ssb_r32(gBus.bar0,B43_MMIO_MACCTL);

  gTxLoop1=-1;gTxLoop2=-1;gTxLoop3=-1;
  SayH("      MACCTL = ",macctl,8);
  Say((macctl&B43_MACCTL_BE)
      ? "        ⚠ B43_MACCTL_BE IS SET -- b43_ram_write byte-swaps the frame words."
      : "        BE clear, so b43_ram_write writes the words unswapped, as expected.");

  gTxBase=ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS);
  TxStat("BASELINE, before any TXE0 write ");

  for(i=0;i<5;i++) RamWrite((UInt16)(i*4),kBuf[i]);
  if(legacyVariant) (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);   /* b43legacy's dummy read */
  TxStat("after RAM writes                ");

  ssb_w16(gBus.bar0,B43_MMIO_XMTSEL,0x0000);
  ssb_w16(gBus.bar0,B43_MMIO_WEPCTL,0x0000);                    /* core_rev 5 < 11 */
  ssb_w16(gBus.bar0,B43_MMIO_TXE0_PHYCTL,(UInt16)(legacyVariant?0x0000:0x0040));
  /* PHY type is G, so TXE0_PHYCTL1 is not written. */
  ssb_w16(gBus.bar0,B43_MMIO_TXE0_WM_0,0x0000);
  ssb_w16(gBus.bar0,B43_MMIO_TXE0_WM_1,0x0000);
  ssb_w16(gBus.bar0,B43_MMIO_XMTTPLATETXPTR,0x0000);
  ssb_w16(gBus.bar0,B43_MMIO_XMTTXCNT,0x0014);
  TxStat("after XMTTXCNT=0x14             ");
  ssb_w16(gBus.bar0,B43_MMIO_XMTSEL,0x0826);
  TxStat("after XMTSEL=0x0826             ");
  ssb_w16(gBus.bar0,B43_MMIO_TXE0_CTL,0x0000);
  TxStat("after TXE0_CTL=0                ");
  /* pa_on is true, so the N-only override is not reached. */
  ssb_w16(gBus.bar0,B43_MMIO_TXE0_AUX,0x0030);                  /* default PHY arm */
  (void)ssb_r16(gBus.bar0,B43_MMIO_TXE0_AUX);                   /* b43's flush read */
  /* ⚠ THIS IS THE TRIGGER. i5's trace showed TXE0_STATUS move 0x0001 -> 0x00C1 here and
   * IFSSTAT 0x809F -> 0x8D80, i.e. bit 7 (transmitting) and IFSSTAT bit 8 (MAC busy) both set
   * on this write. Sampled immediately, because bit 7 is transient. */
  gTxTrig=ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS);
  gTxIfsTrig=ssb_r16(gBus.bar0,B43_MMIO_IFSSTAT);
  TxStat("after TXE0_AUX=0x30 + flush read");

  RadioWrite(0x0051,0x0017);                                    /* radio_rev <= 5 */
  TxStat("after radio 0x51=0x17           ");

  for(i=0;i<0xFA;i++){ v=ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS);
    if(v&0x0080){gTxLoop1=i;gTxStat1=v;break;} SsbSpinUs(10); }
  if(gTxLoop1<0){gTxLoop1=0xFA;gTxStat1=v;}

  for(i=0;i<0x0A;i++){ v=ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS);
    if(v&0x0400){gTxLoop2=i;gTxStat2=v;break;} SsbSpinUs(10); }
  if(gTxLoop2<0){gTxLoop2=0x0A;gTxStat2=v;}

  for(i=0;i<0x19;i++){ v=ssb_r16(gBus.bar0,B43_MMIO_IFSSTAT);
    if(!(v&0x0100)){gTxLoop3=i;gTxIfs=v;break;} SsbSpinUs(10); }
  if(gTxLoop3<0){gTxLoop3=0x19;gTxIfs=v;}

  RadioWrite(0x0051,0x0037);
  gTxFinal=ssb_r16(gBus.bar0,B43_MMIO_TXE0_STATUS);
  gTxIfsFinal=ssb_r16(gBus.bar0,B43_MMIO_IFSSTAT);
  TxStat("after the three waits           ");
}

/* ---- lo_measure_txctl_values. Much smaller on this card than the line count suggests, and the
 * reason is worth stating: the entire tx_magn/tx_bias search -- including every call to
 * lo_measure_feedthrough -- lives inside `if (has_tx_magnification(phy))`, which requires
 * radio_rev == 8. This radio is rev 2, so that whole loop is unreachable and the else arm is
 * three statements. lo_measure_feedthrough is therefore NOT part of this increment at all.
 *
 * radio_pctl_reg comes out of an integer chain worth checking rather than trusting:
 *   has_loopback_gain = (rev > 1) || gmode -> TRUE (rev 2)
 *   lb_gain = gphy->max_lb_gain / 2 = 0, because b43_calc_loopback_gain runs LATER in initg
 *   lb_gain > 10 ? no -> cmp_val = 0x24 (rev>=2 && radio_rev==8 would make it 0x3C; not us)
 *   tmp = lb_gain = 0; (10 - 0) < 36 so tmp = 10; tmp >= 0 so tmp += 3 -> 13
 *   cmp_val /= 4 -> 9 ; tmp /= 4 -> 3 ; 3 >= 9 ? no -> radio_pctl_reg = 3
 * ---- */
static UInt16 LoMeasureTxctlValues(void)
{
  UInt16 radio_pctl_reg,lbGain,tmp,cmpVal;
  UInt16 maxLbGain=0;          /* gphy->max_lb_gain, still 0 during initb6 */

  lbGain=(UInt16)(maxLbGain/2);
  if(lbGain>10){ /* unreachable while max_lb_gain is 0; not ported blind */
    radio_pctl_reg=0; }
  else{
    cmpVal=0x24;               /* rev>=2 && radio_rev==8 -> 0x3C; this radio is rev 2 */
    tmp=lbGain;
    if((UInt16)(10-lbGain)<cmpVal) tmp=(UInt16)(10-lbGain);
    tmp=(UInt16)(tmp+3);       /* tmp is never negative here, so the +6 arm is unreachable */
    cmpVal=(UInt16)(cmpVal/4);
    tmp=(UInt16)(tmp/4);
    radio_pctl_reg=(tmp>=cmpVal)?cmpVal:tmp; }

  RadioMaskSet(0x0043,0xFFF0,radio_pctl_reg);
  PhyMaskSet((UInt16)B43_PHY_DACCTL,0xFFC3,(UInt16)(2<<2));   /* set_baseband_attenuation(2) */

  /* lo_txctl_register_table: G-PHY and radio_rev != 8 -> reg 0x52, value 0x30 */
  RadioMaskSet(0x0052,(UInt16)~0x0030,0);                     /* b43_radio_mask(reg, ~mask) */

  /* has_tx_magnification FALSE -> the three-statement else arm */
  RadioMaskSet(0x0052,0xFFF0,0);                              /* tx_bias = 0 */
  return radio_pctl_reg;
}

/* ---- b43_ofdmtab_write16 / _read16 (tables.c), cache and all. The condition is verbatim:
 *     if (direction != WANTED) || (addr - 1 != cached_addr)   -> rewrite the address
 * Note `addr - 1 != cached`, NOT `addr != cached + 1`: on a UInt16 those differ at the wrap, and
 * copying the reference's form rather than an algebraically-equal one costs nothing. ---- */
static void OfdmTabWrite16(UInt16 table,UInt16 offset,UInt16 value)
{
  UInt16 addr=(UInt16)(table+offset);
  if((gOtabDir!=AP_OTAB_DIR_WRITE) || ((UInt16)(addr-1)!=gOtabAddr)){
    PhyWrite((UInt16)B43_PHY_OTABLECTL,addr);
    gOtabDir=AP_OTAB_DIR_WRITE; }
  gOtabAddr=addr;
  PhyWrite((UInt16)B43_PHY_OTABLEI,value);
}

static UInt16 OfdmTabRead16(UInt16 table,UInt16 offset)
{
  UInt16 addr=(UInt16)(table+offset);
  if((gOtabDir!=AP_OTAB_DIR_READ) || ((UInt16)(addr-1)!=gOtabAddr)){
    PhyWrite((UInt16)B43_PHY_OTABLECTL,addr);
    gOtabDir=AP_OTAB_DIR_READ; }
  gOtabAddr=addr;
  return PhyRead((UInt16)B43_PHY_OTABLEI);
}

/* b43_gphy_set_baseband_attenuation. analog is 2 (> 1), so only that arm is ported; the analog==0
 * arm drives B43_MMIO_PHY0 instead and the analog==1 arm shifts by 3 rather than 2. */
static void SetBasebandAtt(UInt16 att)
{ PhyWrite((UInt16)B43_PHY_DACCTL,
           (UInt16)((PhyRead((UInt16)B43_PHY_DACCTL)&0xFFC3)|(UInt16)(att<<2))); }

/* A self-verifying phy_maskset. b43_phy_maskset READS the register by definition, so reading it
 * again to check the result is sanctioned -- and `want` is computed from the value measured
 * immediately before, not from a guess about what the register held. altagc is twenty-odd of
 * these with hand-transcribed masks, which is exactly the kind of list where one wrong nibble is
 * silent; verifying each one turns a transcription error into a named line in the log. */
static int gMsBad=0; static int gMsN=0;
static void MaskSetV(const char*name,UInt16 reg,UInt16 mask,UInt16 set)
{
  UInt16 before=PhyRead(reg),want,got;
  want=(UInt16)((before&mask)|set);
  PhyWrite(reg,want);
  got=PhyRead(reg);
  gMsN++;
  if(got!=want){ gMsBad++;
    { Str255 L;L[0]=0;PCat(L,"      ✗ ");PCat(L,name);
      PCat(L," 0x");PCatHex(L,reg,4);
      PCat(L,"  want 0x");PCatHex(L,want,4);
      PCat(L,"  got 0x");PCatHex(L,got,4);Out(L); } }
}

/* b43_gtab_write / _read. No cache -- the address is written every time. */
static void GTabWrite(UInt16 table,UInt16 offset,UInt16 value)
{ PhyWrite((UInt16)B43_PHY_GTABCTL,(UInt16)(table+offset));
  PhyWrite((UInt16)B43_PHY_GTABDATA,value); }
static UInt16 GTabRead(UInt16 table,UInt16 offset)
{ PhyWrite((UInt16)B43_PHY_GTABCTL,(UInt16)(table+offset));
  return PhyRead((UInt16)B43_PHY_GTABDATA); }

/* ---- b43_lo_write. b43 asserts abs(i) <= 16 && abs(q) <= 16 here, which is where the legal
 * range for the search result comes from -- it is the reference's own bound, not one we chose. */
static void LoWrite(const LoCtl *c)
{ UInt16 v=(UInt16)((UInt8)(c->q)); v|=(UInt16)(((UInt16)(UInt8)(c->i))<<8);
  PhyWrite((UInt16)B43_PHY_LO_CTL,v); }

/* ---- lo_measure_feedthrough. gmode is TRUE for this card, so only that arm is ported; the
 * !gmode arm drives PGACTL instead and cannot be reached here.
 * (boardflags & EXTLNA) && phy->rev > 6 is FALSE at rev 2, so EXTLNA is never ORed in. ---- */
static UInt16 LoMeasureFeedthrough(UInt16 lna,UInt16 pga,UInt16 trsw)
{
  UInt16 rfover,ft;
  lna=(UInt16)(lna<<B43_PHY_RFOVERVAL_LNA_SHIFT);
  pga=(UInt16)(pga<<B43_PHY_RFOVERVAL_PGA_SHIFT);
  trsw&=(UInt16)(B43_PHY_RFOVERVAL_TRSWRX|0x0003);
  rfover=(UInt16)(B43_PHY_RFOVERVAL_UNK|pga|lna|trsw);
  PhyWrite((UInt16)B43_PHY_PGACTL,0xE300);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,rfover);            SsbSpinUs(10);
  rfover|=B43_PHY_RFOVERVAL_BW_LBW;
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,rfover);            SsbSpinUs(10);
  rfover|=B43_PHY_RFOVERVAL_BW_LPF;
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,rfover);            SsbSpinUs(10);
  PhyWrite((UInt16)B43_PHY_PGACTL,0xF300);
  SsbSpinUs(21);
  ft=PhyRead((UInt16)B43_PHY_LO_LEAKAGE);
  if(gFtCount==0) gFtFirst=ft;
  if(ft<gFtMin) gFtMin=ft;
  if(ft>gFtMax) gFtMax=ft;
  gFtCount++;
  return ft;
}

/* ---- lo_measure_gain_values. has_loopback_gain is TRUE (rev 2 > 1), so the first arm runs and
 * the else arm -- which is the one that would set gphy->trsw_rx_gain = 0x20 -- does not. ---- */
static void LoMeasureGainValues(int maxRxGain,int useTrswRx)
{
  UInt16 tmp;
  int trswRxGain;
  if(maxRxGain<0) maxRxGain=0;
  if(useTrswRx){
    trswRxGain=(int)(gTrswRxGain/2);
    if(maxRxGain>=trswRxGain) trswRxGain=maxRxGain-trswRxGain;
  } else trswRxGain=maxRxGain;
  if(trswRxGain<9) gLnaLodGain=0;
  else { gLnaLodGain=1; trswRxGain-=8; }
  if(trswRxGain<0)    trswRxGain=0;                      /* clamp_val(x, 0, 0x2D) */
  if(trswRxGain>0x2D) trswRxGain=0x2D;
  gPgaGain=(SInt16)(trswRxGain/3);
  if(gPgaGain>=5){ gPgaGain=(SInt16)(gPgaGain-5); gLnaGain=2; }
  else gLnaGain=0;
  tmp=RadioRead(0x007A);
  if(gLnaLodGain==0) tmp&=(UInt16)~0x0008; else tmp|=0x0008;
  RadioWrite(0x007A,tmp);
}

/* ---- lo_probe_possible_loctls. The eight compass directions around the current pair, stepped by
 * state_val_multiplier. The `(nr_measured < 2) && !has_loopback_gain` early break is unreachable
 * here because has_loopback_gain is TRUE, so it is not ported. ---- */
typedef struct { int current_state,nr_measured,state_val_multiplier; UInt16 lowest_feedth;
                 LoCtl min_loctl; } LoSm;

static int LoProbePossibleLoctls(LoCtl *probe,LoSm *d)
{
  static const LoCtl kMod[8]={{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1}};
  LoCtl test,orig,prev; int i,begin,end,foundLower=0; UInt16 ft;
  prev.i=-100; prev.q=-100;
  if(d->current_state==0){ begin=1; end=8; }
  else if(d->current_state%2==0){ begin=d->current_state-1; end=d->current_state+1; }
  else { begin=d->current_state-2; end=d->current_state+2; }
  if(begin<1) begin+=8;
  if(end>8)   end-=8;
  orig=*probe;
  i=begin; d->current_state=i;
  for(;;){
    test=orig;
    test.i+=kMod[i-1].i*d->state_val_multiplier;
    test.q+=kMod[i-1].q*d->state_val_multiplier;
    if((test.i!=prev.i||test.q!=prev.q) &&
       (test.i<=16&&test.i>=-16) && (test.q<=16&&test.q>=-16)){
      LoWrite(&test);
      ft=LoMeasureFeedthrough((UInt16)gLnaGain,(UInt16)gPgaGain,(UInt16)gTrswRxGain);
      if(ft<d->lowest_feedth){ *probe=test; foundLower=1; d->lowest_feedth=ft; }
    }
    prev=test;
    if(i==end) break;
    i=(i==8)?1:(i+1);
    d->current_state=i;
  }
  return foundLower;
}

/* ---- lo_probe_loctls_statemachine. max_repeat is 4 and state_val_multiplier starts at 3,
 * both because has_loopback_gain is TRUE. Bounded by construction: 4 outer passes, each with at
 * most 24 accepted steps, each step probing at most 8 directions. ---- */
static void LoProbeLoctlsStatemachine(LoCtl *loctl,int *maxRxGain,int *outerOut,int *stepsOut)
{
  LoSm d; UInt16 ft; int foundLower; LoCtl probe;
  int maxRepeat=4,repeatCnt=0,totalSteps=0;
  d.nr_measured=0; d.state_val_multiplier=3;
  d.min_loctl=*loctl;
  do{
    LoWrite(&d.min_loctl);
    ft=LoMeasureFeedthrough((UInt16)gLnaGain,(UInt16)gPgaGain,(UInt16)gTrswRxGain);
    if(ft<0x258){
      if(ft>=0x12C) *maxRxGain+=6; else *maxRxGain+=3;
      ft=LoMeasureFeedthrough((UInt16)gLnaGain,(UInt16)gPgaGain,(UInt16)gTrswRxGain);
    }
    d.lowest_feedth=ft;
    /* Per-pass seed, captured AFTER the possible gain bump. Gains are constant for the rest of
     * this pass, so this is the only figure lowest_feedth is commensurable with. */
    if(gPassN<8){ gPassSeed[gPassN]=ft; }
    d.current_state=0;
    do{
      probe=d.min_loctl;
      foundLower=LoProbePossibleLoctls(&probe,&d);
      if(!foundLower) break;
      if(probe.i==d.min_loctl.i && probe.q==d.min_loctl.q) break;
      d.min_loctl=probe;
      d.nr_measured++; totalSteps++;
    } while(d.nr_measured<24);
    *loctl=d.min_loctl;
    /* has_loopback_gain TRUE */
    if(d.lowest_feedth>0x1194)      *maxRxGain-=6;
    else if(d.lowest_feedth<0x5DC)  *maxRxGain+=3;
    if(repeatCnt==0){
      if(d.lowest_feedth<=0x5DC){ d.state_val_multiplier=1; repeatCnt++; }
      else d.state_val_multiplier=2;
    } else if(repeatCnt==2) d.state_val_multiplier=1;
    if(gPassN<8){ gPassLow[gPassN]=d.lowest_feedth; gPassMul[gPassN]=(UInt16)d.state_val_multiplier;
                  gPassN++; }
    LoMeasureGainValues(*maxRxGain,1);   /* ⚠ CHANGES THE GAINS -> new measurement scale */
  } while(++repeatCnt<maxRepeat);
  gLowestFeedth=d.lowest_feedth;
  if(outerOut) *outerOut=repeatCnt;
  if(stepsOut) *stepsOut=totalSteps;
}

/* ---- b43_calc_loopback_gain (phy_g.c). Measures the loopback gain by sweeping radio 0x43 and
 * the RFOVERVAL PGA field and watching LO_LEAKAGE, then derives two values the LO code consumes:
 *     gphy->max_lb_gain  = ((inner * 6) - (outer * 4)) - 11
 *     gphy->trsw_rx_gain = trsw_rx * 2
 *
 * ⚠⚠ THREE OF ITS BRANCHES CARRY THE COMMENT "Not in specs, but needed to prevent PPC machine
 * check", all guarded by `phy->rev != 1`. This PHY is rev 2, so all three RUN -- and we are on
 * PowerPC, which is the architecture that comment is about. They are the ANALOGOVER/ANALOGOVERVAL
 * save, the four-step override, and the restore. Dropping any of them as "not in the spec" would
 * reproduce the exact fault someone else already paid for.
 *
 * ⚠ backup_phy[15] is LO_LEAKAGE. b43 SAVES it and never RESTORES it -- it is a measurement
 * register, not state. An oracle that expected all sixteen saved registers to come back would
 * fail a correct port, so the restore check deliberately covers fifteen. ---- */
static void CalcLoopbackGain(void)
{
  UInt16 bphy[16],bradio[3],bband;
  UInt16 i,j,loopIMax,trswRx,ll;
  int outerDone=0,innerDone=0,broke=0;

  gLlMin=0xFFFF;gLlMax=0;gLlCount=0;gLbTrswUnderflow=0;

  bphy[0]=PhyRead((UInt16)B43_PHY_CRS0);
  bphy[1]=PhyRead((UInt16)B43_PHY_CCKBBANDCFG);
  bphy[2]=PhyRead((UInt16)B43_PHY_RFOVER);
  bphy[3]=PhyRead((UInt16)B43_PHY_RFOVERVAL);
  bphy[4]=PhyRead((UInt16)B43_PHY_ANALOGOVER);        /* rev != 1 */
  bphy[5]=PhyRead((UInt16)B43_PHY_ANALOGOVERVAL);
  bphy[6]=PhyRead((UInt16)B43_PHY_CCK(0x5A));
  bphy[7]=PhyRead((UInt16)B43_PHY_CCK(0x59));
  bphy[8]=PhyRead((UInt16)B43_PHY_CCK(0x58));
  bphy[9]=PhyRead((UInt16)B43_PHY_CCK(0x0A));
  bphy[10]=PhyRead((UInt16)B43_PHY_CCK(0x03));
  bphy[11]=PhyRead((UInt16)B43_PHY_LO_MASK);
  bphy[12]=PhyRead((UInt16)B43_PHY_LO_CTL);
  bphy[13]=PhyRead((UInt16)B43_PHY_CCK(0x2B));
  bphy[14]=PhyRead((UInt16)B43_PHY_PGACTL);
  bphy[15]=PhyRead((UInt16)B43_PHY_LO_LEAKAGE);       /* saved, never restored */
  bband=bbAttG;
  bradio[0]=RadioRead(0x0052);
  bradio[1]=RadioRead(0x0043);
  bradio[2]=RadioRead(0x007A);

  PhyMaskSet((UInt16)B43_PHY_CRS0,0x3FFF,0);
  PhySet((UInt16)B43_PHY_CCKBBANDCFG,0x8000);
  PhySet((UInt16)B43_PHY_RFOVER,0x0002);
  PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xFFFD,0);
  PhySet((UInt16)B43_PHY_RFOVER,0x0001);
  PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xFFFE,0);
  /* the first PPC machine-check guard */
  PhySet((UInt16)B43_PHY_ANALOGOVER,0x0001);
  PhyMaskSet((UInt16)B43_PHY_ANALOGOVERVAL,0xFFFE,0);
  PhySet((UInt16)B43_PHY_ANALOGOVER,0x0002);
  PhyMaskSet((UInt16)B43_PHY_ANALOGOVERVAL,0xFFFD,0);
  PhySet((UInt16)B43_PHY_RFOVER,0x000C);
  PhySet((UInt16)B43_PHY_RFOVERVAL,0x000C);
  PhySet((UInt16)B43_PHY_RFOVER,0x0030);
  PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xFFCF,0x0010);

  PhyWrite((UInt16)B43_PHY_CCK(0x5A),0x0780);
  PhyWrite((UInt16)B43_PHY_CCK(0x59),0xC810);
  PhyWrite((UInt16)B43_PHY_CCK(0x58),0x000D);

  PhySet((UInt16)B43_PHY_CCK(0x0A),0x2000);
  /* the second PPC machine-check guard */
  PhySet((UInt16)B43_PHY_ANALOGOVER,0x0004);
  PhyMaskSet((UInt16)B43_PHY_ANALOGOVERVAL,0xFFFB,0);
  PhyMaskSet((UInt16)B43_PHY_CCK(0x03),0xFF9F,0x0040);

  /* radio_rev != 8 */
  RadioWrite(0x0052,0);
  RadioMaskSet(0x0043,0xFFF0,0x0009);
  SetBasebandAtt(11);

  PhyWrite((UInt16)B43_PHY_LO_MASK,0x8020);            /* rev < 3 */
  PhyWrite((UInt16)B43_PHY_LO_CTL,0);
  PhyMaskSet((UInt16)B43_PHY_CCK(0x2B),0xFFC0,0x0001);
  PhyMaskSet((UInt16)B43_PHY_CCK(0x2B),0xC0FF,0x0800);
  PhySet((UInt16)B43_PHY_RFOVER,0x0100);
  PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xCFFF,0);
  /* boardflags EXTLNA is clear on this card, so the rev >= 7 block cannot be reached */
  RadioMaskSet(0x007A,0x00F7,0);

  /* ---- sweep 1: radio 0x43 = i, RFOVERVAL PGA = j ---- */
  /* ⚠ b43 leaves this loop with `goto exit_loop1`, which keeps i at its BREAK value. Writing the
   * outer loop as `for(i=0; i<max && !broke; i++)` is NOT equivalent: the i++ runs before the
   * condition is re-tested, so i ends one too high and max_lb_gain comes out 4 low -- silently.
   * `break` from the outer body reproduces the goto exactly, because break skips the increment. */
  j=0; loopIMax=9;                                     /* radio_rev != 8 */
  for(i=0;i<loopIMax;i++){
    for(j=0;j<16;j++){
      RadioWrite(0x0043,i);
      PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xF0FF,(UInt16)(j<<8));
      PhyMaskSet((UInt16)B43_PHY_PGACTL,0x0FFF,0xA000);
      PhySet((UInt16)B43_PHY_PGACTL,0xF000);
      SsbSpinUs(20);
      ll=PhyRead((UInt16)B43_PHY_LO_LEAKAGE);
      if(ll<gLlMin){ gLlMin=ll; }
      if(ll>gLlMax){ gLlMax=ll; }
      gLlCount++;
      if(ll>=0x0DFC){ broke=1; break; } }
    if(broke) break; }                                 /* break, NOT a for-header condition */
  outerDone=(int)i;
  innerDone=(int)j;

  /* ---- sweep 2 ---- */
  if(j>=8){
    PhySet((UInt16)B43_PHY_RFOVERVAL,0x0030);
    trswRx=0x1B;
    for(j=(UInt16)(j-8);j<16;j++){
      PhyMaskSet((UInt16)B43_PHY_RFOVERVAL,0xF0FF,(UInt16)(j<<8));
      PhyMaskSet((UInt16)B43_PHY_PGACTL,0x0FFF,0xA000);
      PhySet((UInt16)B43_PHY_PGACTL,0xF000);
      SsbSpinUs(20);
      if(trswRx<3) gLbTrswUnderflow=1;                 /* u16 wrap, reported not "fixed" */
      trswRx=(UInt16)(trswRx-3);
      ll=PhyRead((UInt16)B43_PHY_LO_LEAKAGE);
      if(ll<gLlMin){ gLlMin=ll; }
      if(ll>gLlMax){ gLlMax=ll; }
      gLlCount++;
      if(ll>=0x0DFC) break; } }
  else trswRx=0x18;

  /* ---- restore ---- */
  PhyWrite((UInt16)B43_PHY_ANALOGOVER,bphy[4]);        /* third PPC machine-check guard */
  PhyWrite((UInt16)B43_PHY_ANALOGOVERVAL,bphy[5]);
  PhyWrite((UInt16)B43_PHY_CCK(0x5A),bphy[6]);
  PhyWrite((UInt16)B43_PHY_CCK(0x59),bphy[7]);
  PhyWrite((UInt16)B43_PHY_CCK(0x58),bphy[8]);
  PhyWrite((UInt16)B43_PHY_CCK(0x0A),bphy[9]);
  PhyWrite((UInt16)B43_PHY_CCK(0x03),bphy[10]);
  PhyWrite((UInt16)B43_PHY_LO_MASK,bphy[11]);
  PhyWrite((UInt16)B43_PHY_LO_CTL,bphy[12]);
  PhyWrite((UInt16)B43_PHY_CCK(0x2B),bphy[13]);
  PhyWrite((UInt16)B43_PHY_PGACTL,bphy[14]);
  SetBasebandAtt(bband);
  RadioWrite(0x0052,bradio[0]);
  RadioWrite(0x0043,bradio[1]);
  RadioWrite(0x007A,bradio[2]);
  PhyWrite((UInt16)B43_PHY_RFOVER,(UInt16)(bphy[2]|0x0003));
  SsbSpinUs(10);
  PhyWrite((UInt16)B43_PHY_RFOVER,bphy[2]);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,bphy[3]);
  PhyWrite((UInt16)B43_PHY_CRS0,bphy[0]);
  PhyWrite((UInt16)B43_PHY_CCKBBANDCFG,bphy[1]);

  /* ---- verify the restore. Fifteen PHY registers, not sixteen: LO_LEAKAGE is saved but
   * deliberately never written back, so checking it would fail a correct port. ---- */
  gLbRestoreBad=0;
  { UInt16 regs[15],want[15]; int n=0,z;
    regs[n]=(UInt16)B43_PHY_CRS0;              want[n++]=bphy[0];
    regs[n]=(UInt16)B43_PHY_CCKBBANDCFG;       want[n++]=bphy[1];
    regs[n]=(UInt16)B43_PHY_RFOVER;            want[n++]=bphy[2];
    regs[n]=(UInt16)B43_PHY_RFOVERVAL;         want[n++]=bphy[3];
    regs[n]=(UInt16)B43_PHY_ANALOGOVER;        want[n++]=bphy[4];
    regs[n]=(UInt16)B43_PHY_ANALOGOVERVAL;     want[n++]=bphy[5];
    regs[n]=(UInt16)B43_PHY_CCK(0x5A);         want[n++]=bphy[6];
    regs[n]=(UInt16)B43_PHY_CCK(0x59);         want[n++]=bphy[7];
    regs[n]=(UInt16)B43_PHY_CCK(0x58);         want[n++]=bphy[8];
    regs[n]=(UInt16)B43_PHY_CCK(0x0A);         want[n++]=bphy[9];
    regs[n]=(UInt16)B43_PHY_CCK(0x03);         want[n++]=bphy[10];
    regs[n]=(UInt16)B43_PHY_LO_MASK;           want[n++]=bphy[11];
    regs[n]=(UInt16)B43_PHY_LO_CTL;            want[n++]=bphy[12];
    regs[n]=(UInt16)B43_PHY_CCK(0x2B);         want[n++]=bphy[13];
    regs[n]=(UInt16)B43_PHY_PGACTL;            want[n++]=bphy[14];
    for(z=0;z<n;z++){ UInt16 got=PhyRead(regs[z]);
      if(got!=want[z]){ gLbRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ PHY 0x");PCatHex(L,regs[z],4);
          PCat(L,"  want 0x");PCatHex(L,want[z],4);
          PCat(L,"  got 0x");PCatHex(L,got,4);Out(L); } } }
    if(RadioRead(0x0052)!=bradio[0]) gLbRestoreBad++;
    if(RadioRead(0x0043)!=bradio[1]) gLbRestoreBad++;
    if(RadioRead(0x007A)!=bradio[2]) gLbRestoreBad++; }

  gLbOuter=outerDone; gLbInner=innerDone; gLbTrsw=trswRx;
  gLbMaxGain=(SInt16)(((innerDone*6)-(outerDone*4))-11);
  gMaxLbGain=gLbMaxGain;
  gTrswRxGain=(SInt16)((SInt16)trswRx*2);
}

/* ---- radio2050_rfover_val. THIS IS WHERE max_lb_gain FEEDS BACK IN -- the value 4b-ii-6
 * measured is consumed here to pick an extlna code and a gain index, and those become the
 * RFOVERVAL words written hundreds of times by the sweeps below. gmode is TRUE and
 * has_loopback_gain is TRUE, so only that arm is ported; rev < 7 selects the non-EXTLNA half.
 * The `for (i...) { max_lb_gain -= (i * 6); ... }` loop subtracts a GROWING amount each pass, so
 * it is not a division -- transcribing it as one would land on a different gain index. ---- */
static UInt16 Radio2050RfoverVal(UInt16 phyRegister,unsigned int lpd)
{
  SInt32 mlb=(SInt32)gLbMaxGain;
  UInt16 extlna; int i;
  mlb+=0x26;                                   /* radio_rev != 8 */
  if(mlb>=0x46)      { extlna=0x3000; mlb-=0x46; }
  else if(mlb>=0x3A) { extlna=0x1000; mlb-=0x3A; }
  else if(mlb>=0x2E) { extlna=0x2000; mlb-=0x2E; }
  else               { extlna=0;      mlb-=0x10; }
  for(i=0;i<16;i++){ mlb-=(SInt32)(i*6); if(mlb<6) break; }
  /* rev 2 < 7, so the first arm regardless of EXTLNA */
  if(phyRegister==(UInt16)B43_PHY_RFOVER) return 0x01B3;
  if(phyRegister==(UInt16)B43_PHY_RFOVERVAL){
    extlna|=(UInt16)(i<<8);
    switch(lpd){
      case LPD(0,1,1): return 0x0F92;
      case LPD(0,0,1):
      case LPD(1,0,1): return (UInt16)(0x0092|extlna);
      case LPD(1,0,0): return (UInt16)(0x0093|extlna); } }
  return 0;
}

/* b43_radio_core_calibration_value. Reads radio 0x60, which is FIRST TOUCH on this card. */
static UInt16 RadioCoreCalibrationValue(void)
{
  static const UInt8 kRcc[16]={0x02,0x03,0x01,0x0F, 0x06,0x07,0x05,0x0F,
                               0x0A,0x0B,0x09,0x0F, 0x0E,0x0F,0x0D,0x0F};
  UInt16 reg,index,ret;
  reg=RadioRead(0x0060);
  gRadio60=reg;
  index=(UInt16)((reg&0x001E)>>1);
  ret=(UInt16)(kRcc[index]<<1);
  ret|=(UInt16)(reg&0x0001);
  ret|=0x0020;
  return ret;
}

static UInt16 BitRev4(UInt16 v)
{ UInt16 r=0,k; for(k=0;k<4;k++){ r=(UInt16)((r<<1)|(v&1)); v>>=1; } return r; }

/* ---- b43_radio_init2050. Returns gphy->initval. ---- */
static UInt16 RadioInit2050(void)
{
  UInt16 sRadio43,sRadio51,sRadio52,sPgactl,sCck5A,sCck59,sCck58;
  UInt16 sRfover,sRfoverval,sAnalogover,sAnalogoverval,sCrs0,sClassctl;
  UInt16 sLoMask,sLoCtl,sSyncctl,sReg3E6,sReg3F4;
  UInt16 rcc,radio78=0,ret; UInt16 i,j;
  UInt32 tmp1=0,tmp2=0;

  sRadio43=RadioRead(0x0043); sRadio51=RadioRead(0x0051); sRadio52=RadioRead(0x0052);
  sPgactl=PhyRead((UInt16)B43_PHY_PGACTL);
  sCck5A=PhyRead((UInt16)B43_PHY_CCK(0x5A));
  sCck59=PhyRead((UInt16)B43_PHY_CCK(0x59));
  sCck58=PhyRead((UInt16)B43_PHY_CCK(0x58));

  /* not B-PHY; gmode || rev >= 2 -> this arm */
  sRfover=PhyRead((UInt16)B43_PHY_RFOVER);
  sRfoverval=PhyRead((UInt16)B43_PHY_RFOVERVAL);
  sAnalogover=PhyRead((UInt16)B43_PHY_ANALOGOVER);
  sAnalogoverval=PhyRead((UInt16)B43_PHY_ANALOGOVERVAL);
  sCrs0=PhyRead((UInt16)B43_PHY_CRS0);
  sClassctl=PhyRead((UInt16)B43_PHY_CLASSCTL);
  PhySet((UInt16)B43_PHY_ANALOGOVER,0x0003);
  PhyMaskSet((UInt16)B43_PHY_ANALOGOVERVAL,0xFFFC,0);
  PhyMaskSet((UInt16)B43_PHY_CRS0,0x7FFF,0);
  PhyMaskSet((UInt16)B43_PHY_CLASSCTL,0xFFFC,0);
  /* has_loopback_gain TRUE */
  sLoMask=PhyRead((UInt16)B43_PHY_LO_MASK);
  sLoCtl=PhyRead((UInt16)B43_PHY_LO_CTL);
  PhyWrite((UInt16)B43_PHY_LO_MASK,0x8020);    /* rev < 3 */
  PhyWrite((UInt16)B43_PHY_LO_CTL,0);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(0,1,1)));
  PhyWrite((UInt16)B43_PHY_RFOVER,   Radio2050RfoverVal((UInt16)B43_PHY_RFOVER,0));

  ssb_w16(gBus.bar0,B43_MMIO_PHY_RADIO,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_PHY_RADIO)|0x8000));

  sSyncctl=PhyRead((UInt16)B43_PHY_SYNCCTL);
  PhyMaskSet((UInt16)B43_PHY_SYNCCTL,0xFF7F,0);
  sReg3E6=ssb_r16(gBus.bar0,B43_MMIO_PHY0);
  sReg3F4=ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT);

  /* analog is 2, so the else arm, and analog >= 2 adds the CCK 0x03 maskset */
  PhyMaskSet((UInt16)B43_PHY_CCK(0x03),0xFFBF,0x0040);
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT)|0x2000));

  rcc=RadioCoreCalibrationValue();
  /* type is G, so the radio 0x78 = 0x26 write is not run */
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(0,1,1)));
  PhyWrite((UInt16)B43_PHY_PGACTL,0xBFAF);
  PhyWrite((UInt16)B43_PHY_CCK(0x2B),0x1403);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(0,0,1)));
  PhyWrite((UInt16)B43_PHY_PGACTL,0xBFA0);
  RadioSet(0x0051,0x0004);
  /* radio_rev != 8 */
  RadioWrite(0x0052,0);
  RadioMaskSet(0x0043,0xFFF0,0x0009);
  PhyWrite((UInt16)B43_PHY_CCK(0x58),0);

  for(i=0;i<16;i++){
    PhyWrite((UInt16)B43_PHY_CCK(0x5A),0x0480);
    PhyWrite((UInt16)B43_PHY_CCK(0x59),0xC810);
    PhyWrite((UInt16)B43_PHY_CCK(0x58),0x000D);
    PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
    PhyWrite((UInt16)B43_PHY_PGACTL,0xAFB0); SsbSpinUs(10);
    PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
    PhyWrite((UInt16)B43_PHY_PGACTL,0xEFB0); SsbSpinUs(10);
    PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,0)));
    PhyWrite((UInt16)B43_PHY_PGACTL,0xFFF0); SsbSpinUs(20);
    tmp1+=(UInt32)PhyRead((UInt16)B43_PHY_LO_LEAKAGE);
    PhyWrite((UInt16)B43_PHY_CCK(0x58),0);
    PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
    PhyWrite((UInt16)B43_PHY_PGACTL,0xAFB0); }
  SsbSpinUs(10);
  PhyWrite((UInt16)B43_PHY_CCK(0x58),0);
  tmp1++; tmp1>>=9;

  for(i=0;i<16;i++){
    radio78=(UInt16)((BitRev4(i)<<1)|0x0020);
    RadioWrite(0x0078,radio78);
    SsbSpinUs(10);
    tmp2=0;
    for(j=0;j<16;j++){
      PhyWrite((UInt16)B43_PHY_CCK(0x5A),0x0D80);
      PhyWrite((UInt16)B43_PHY_CCK(0x59),0xC810);
      PhyWrite((UInt16)B43_PHY_CCK(0x58),0x000D);
      PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
      PhyWrite((UInt16)B43_PHY_PGACTL,0xAFB0); SsbSpinUs(10);
      PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
      PhyWrite((UInt16)B43_PHY_PGACTL,0xEFB0); SsbSpinUs(10);
      PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,0)));
      PhyWrite((UInt16)B43_PHY_PGACTL,0xFFF0); SsbSpinUs(10);
      tmp2+=(UInt32)PhyRead((UInt16)B43_PHY_LO_LEAKAGE);
      PhyWrite((UInt16)B43_PHY_CCK(0x58),0);
      PhyWrite((UInt16)B43_PHY_RFOVERVAL,Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,1)));
      PhyWrite((UInt16)B43_PHY_PGACTL,0xAFB0); }
    tmp2++; tmp2>>=8;
    if(tmp1<tmp2) break; }
  gLoop2Iters=(int)i;

  /* ---- restore ---- */
  PhyWrite((UInt16)B43_PHY_PGACTL,sPgactl);
  RadioWrite(0x0051,sRadio51);
  RadioWrite(0x0052,sRadio52);
  RadioWrite(0x0043,sRadio43);
  PhyWrite((UInt16)B43_PHY_CCK(0x5A),sCck5A);
  PhyWrite((UInt16)B43_PHY_CCK(0x59),sCck59);
  PhyWrite((UInt16)B43_PHY_CCK(0x58),sCck58);
  ssb_w16(gBus.bar0,B43_MMIO_PHY0,sReg3E6);
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,sReg3F4);   /* analog != 0 */
  PhyWrite((UInt16)B43_PHY_SYNCCTL,sSyncctl);
  SynthPuWorkaround(1);                              /* phy->channel */
  /* gmode arm */
  ssb_w16(gBus.bar0,B43_MMIO_PHY_RADIO,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_PHY_RADIO)&0x7FFF));
  PhyWrite((UInt16)B43_PHY_RFOVER,sRfover);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,sRfoverval);
  PhyWrite((UInt16)B43_PHY_ANALOGOVER,sAnalogover);
  PhyWrite((UInt16)B43_PHY_ANALOGOVERVAL,sAnalogoverval);
  PhyWrite((UInt16)B43_PHY_CRS0,sCrs0);
  PhyWrite((UInt16)B43_PHY_CLASSCTL,sClassctl);
  PhyWrite((UInt16)B43_PHY_LO_MASK,sLoMask);
  PhyWrite((UInt16)B43_PHY_LO_CTL,sLoCtl);

  gI2050RestoreBad=0;
  { UInt16 r[12],w[12]; int n=0,z;
    r[n]=(UInt16)B43_PHY_PGACTL;        w[n++]=sPgactl;
    r[n]=(UInt16)B43_PHY_CCK(0x5A);     w[n++]=sCck5A;
    r[n]=(UInt16)B43_PHY_CCK(0x59);     w[n++]=sCck59;
    r[n]=(UInt16)B43_PHY_CCK(0x58);     w[n++]=sCck58;
    r[n]=(UInt16)B43_PHY_SYNCCTL;       w[n++]=sSyncctl;
    r[n]=(UInt16)B43_PHY_RFOVER;        w[n++]=sRfover;
    r[n]=(UInt16)B43_PHY_RFOVERVAL;     w[n++]=sRfoverval;
    r[n]=(UInt16)B43_PHY_ANALOGOVER;    w[n++]=sAnalogover;
    r[n]=(UInt16)B43_PHY_ANALOGOVERVAL; w[n++]=sAnalogoverval;
    r[n]=(UInt16)B43_PHY_CRS0;          w[n++]=sCrs0;
    r[n]=(UInt16)B43_PHY_CLASSCTL;      w[n++]=sClassctl;
    r[n]=(UInt16)B43_PHY_LO_MASK;       w[n++]=sLoMask;
    for(z=0;z<n;z++){ UInt16 got=PhyRead(r[z]);
      if(got!=w[z]){ gI2050RestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ PHY 0x");PCatHex(L,r[z],4);
          PCat(L,"  want 0x");PCatHex(L,w[z],4);
          PCat(L,"  got 0x");PCatHex(L,got,4);Out(L); } } }
    if(RadioRead(0x0051)!=sRadio51) gI2050RestoreBad++;
    if(RadioRead(0x0052)!=sRadio52) gI2050RestoreBad++;
    if(RadioRead(0x0043)!=sRadio43) gI2050RestoreBad++;
    /* ⚠ 0x3F4 is NOT checked: SynthPuWorkaround runs AFTER it is written back and rewrites
     * B43_MMIO_CHANNEL, and channel_switch's else-arm elsewhere clears bits in CHANNEL_EXT.
     * Same trap as Oracle E's 0x3F4 -- a naive check would fail a correct port. */ }

  gTmp1=tmp1; gTmp2=tmp2; gRcc=rcc; gRadio78=radio78;
  if(i>15){ ret=radio78; gInitvalFromRadio78=1; }
  else    { ret=rcc;     gInitvalFromRadio78=0; }
  return ret;
}

/* ---- lo_measure_setup, statements 1..12 (lo.c). A PREFIX: it stops immediately before
 * `if (phy->rev >= 2) b43_dummy_transmission(dev, false, true);`, which is an 85-line callee that
 * makes the MAC actually TRANSMIT -- the first transmission this card would ever perform under
 * OS 9 -- and before lo_measure_txctl_values (96 lines, itself calling lo_measure_feedthrough).
 * Both are 4b-ii-4b.
 *
 * Branches, resolved against measured identity (PHY G rev 2, analog 2, radio 0x2050 rev 2):
 *   b43_has_hardware_pctl   FALSE  -- supports_hwpctl is (rev >= 6); block not run
 *   phy->type == B          FALSE  -- the CCK 0x16/0x17 writes are not run
 *   phy->rev >= 2           TRUE   -- the 7-register save + override block IS run
 *   rev >= 7 && EXTLNA      FALSE  -- RFOVER takes 0x133, not 0x933
 *   has_tx_magnification    FALSE  -- radio 0x52 IS saved (masked to 0x00F0)
 * ---- */
static void LoMeasureSetupPrefix(LoSaved *sav)
{
  UInt16 t;
  sav->old_channel = 1;                        /* phy->channel; RadioOn(1) set it */

  /* rev >= 2 block */
  sav->phy_analogover    = PhyRead((UInt16)B43_PHY_ANALOGOVER);
  sav->phy_analogoverval = PhyRead((UInt16)B43_PHY_ANALOGOVERVAL);
  sav->phy_rfover        = PhyRead((UInt16)B43_PHY_RFOVER);
  sav->phy_rfoverval     = PhyRead((UInt16)B43_PHY_RFOVERVAL);
  sav->phy_classctl      = PhyRead((UInt16)B43_PHY_CLASSCTL);
  sav->phy_cck_3E        = PhyRead((UInt16)B43_PHY_CCK(0x3E));
  sav->phy_crs0          = PhyRead((UInt16)B43_PHY_CRS0);

  PhyMaskSet((UInt16)B43_PHY_CLASSCTL,0xFFFC,0);         /* b43_phy_mask */
  PhyMaskSet((UInt16)B43_PHY_CRS0,0x7FFF,0);
  PhySet((UInt16)B43_PHY_ANALOGOVER,0x0003);
  PhyMaskSet((UInt16)B43_PHY_ANALOGOVERVAL,0xFFFC,0);
  PhyWrite((UInt16)B43_PHY_RFOVER,0x0133);               /* G, rev < 7 */

  /* ⚠⚠⚠ THE RISK IN THIS INCREMENT, AND IT IS WORTH STATING BEFORE IT HAPPENS.
   * B43_PHY_CCK(0x3E) IS 0x003E -- the exact register initb6 wrote 0x817A to, and the write that
   * made the radio register file reachable after five master aborts. lo_measure_setup clears it
   * to ZERO here, and the next statements READ radio 0x43 and 0x7A. If 0x003E's VALUE is the
   * gate rather than a one-time strobe, that read re-locks and aborts.
   * b43 performs exactly this sequence on real hardware, so it evidently survives there -- but
   * nothing on THIS card has tested it, and "b43 does it" is why we run it, not why it is safe.
   * Logged either side so an abort names this transition instead of looking like a fresh mystery. */
  Say("    ⚠ clearing PHY 0x003E to 0 -- the register whose 0x817A unlocked the radio.");
  PhyWrite((UInt16)B43_PHY_CCK(0x3E),0x0000);
  SayH("      PHY 0x003E now = ",(unsigned long)PhyRead((UInt16)B43_PHY_CCK(0x3E)),4);

  /* the unconditional saves */
  sav->reg_3F4      = ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT);   /* 0x3F4 */
  Say("    -> MMIO 0x3E2 read (FIRST TOUCH on this card)");
  sav->reg_3E2      = ssb_r16(gBus.bar0,B43_MMIO_3E2);
  SayH("      0x3E2 = ",(unsigned long)sav->reg_3E2,4);
  Say("    -> RADIO READ with PHY 0x003E == 0. If the log stops here, 0x003E's VALUE is the");
  Say("       gate, not a strobe, and lo_measure_setup cannot be run as written on this card.");
  sav->radio_43     = RadioRead(0x0043);
  Say("      radio read survived with 0x003E cleared -- 0x003E is a STROBE, not a latch.");
  sav->radio_7A     = RadioRead(0x007A);
  sav->phy_pgactl   = PhyRead((UInt16)B43_PHY_PGACTL);
  sav->phy_cck_2A   = PhyRead((UInt16)B43_PHY_CCK(0x2A));
  sav->phy_syncctl  = PhyRead((UInt16)B43_PHY_SYNCCTL);
  sav->phy_dacctl   = PhyRead((UInt16)B43_PHY_DACCTL);

  /* !has_tx_magnification -> save radio 0x52, masked to 0x00F0 by b43 itself */
  sav->radio_52 = (UInt16)(RadioRead(0x0052) & 0x00F0);

  /* not B-PHY -> the else arm */
  ssb_w16(gBus.bar0,B43_MMIO_3E2,(UInt16)(ssb_r16(gBus.bar0,B43_MMIO_3E2)|0x8000));
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT)&0xF000));

  PhyWrite((UInt16)B43_PHY_LO_MASK,0x007F);              /* G -> LO_MASK, not CCK 0x2E */
  t=sav->phy_syncctl; PhyWrite((UInt16)B43_PHY_SYNCCTL,(UInt16)(t&0xFF7F));
  t=sav->radio_7A;    RadioWrite(0x007A,(UInt16)(t&0xFFF0));
  PhyWrite((UInt16)B43_PHY_CCK(0x2A),0x08A3);
  PhyWrite((UInt16)B43_PHY_CCK(0x2B),0x1003);            /* G */

  /* ---- statements 13..18, added in i4 / 4b-ii-4b. lo_measure_setup is now COMPLETE. ---- */
  Say("    -> stmt 13: b43_dummy_transmission(dev, false, true) -- VARIANT A, b43's sequence");
  DummyTransmission(0);                                  /* rev >= 2 */
  gTxA1=gTxLoop1;gTxA2=gTxLoop2;gTxA3=gTxLoop3;gTxAStat=gTxStat1;
  Say("");
  Say("    -> VARIANT B, b43legacy's sequence for this same BCM4306 family.");
  Say("       It differs in exactly two places: a dummy MACCTL read after the RAM writes, and");
  Say("       TXE0_PHYCTL written 0x0000 instead of 0x0040. Both drivers ship this chip, so");
  Say("       this is an A/B between two references, not an experiment on undocumented silicon.");
  Say("       ⚠ b43 itself calls dummy_transmission repeatedly (phy_g.c:325, 357, 1954), so");
  Say("         running it twice in a row is a sanctioned thing to do to this engine.");
  DummyTransmission(1);
  gTxB1=gTxLoop1;gTxB2=gTxLoop2;gTxB3=gTxLoop3;gTxBStat=gTxStat1;
  Say("    -> stmt 14: channel_switch(6, 0)");
  GphyChannelSwitch(6,0);
  (void)RadioRead(0x0051);                               /* stmt 15: b43's dummy read */
  PhyWrite((UInt16)B43_PHY_CCK(0x2F),0x0000);            /* stmt 16: G */
  /* stmt 17: b43 re-measures the txctl values when the cached ones have expired:
   *     if (time_before(lo->txctl_measured_time, jiffies - B43_LO_TXCTL_EXPIRE))
   * lo_control is kzalloc'd, so txctl_measured_time is 0 on a first init and the comparison is
   * true for any sane jiffies. It therefore ALWAYS runs here -- this is not an optimisation we
   * are skipping, it is the branch the reference takes on the path we are on. */
  Say("    -> stmt 17: lo_measure_txctl_values (txctl_measured_time is 0 on a first init)");
  gRadioPctlReg=LoMeasureTxctlValues();
  /* stmt 18: G && rev >= 3 -> 0xC078, else 0x8078. This PHY is rev 2. */
  PhyWrite((UInt16)B43_PHY_LO_MASK,0x8078);
}

/* ---- lo_measure_restore, in full. It consumes only `sav` and gphy->pga_gain.
 * ⚠ pga_gain is set by lo_measure_gain_values, which this increment does NOT run, so it is 0
 * here (prepare_structs memsets gphy). That affects ONLY the transient RFOVERVAL ramp
 * (0xA0 -> 0xA2 -> 0xA3); the rev>=2 block near the end overwrites RFOVERVAL from
 * sav->phy_rfoverval, so the FINAL state is fully determined by `sav`. That is what makes a
 * setup-then-restore round trip a legitimate test rather than a half-run sequence. ---- */
static void LoMeasureRestore(LoSaved *sav)
{
  UInt16 t;
  UInt16 pgaGain=0;                            /* gphy->pga_gain, 0 until gain_values runs */

  PhyWrite((UInt16)B43_PHY_PGACTL,0xE300);
  t=(UInt16)(pgaGain<<8);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,(UInt16)(t|0x00A0)); SsbSpinUs(5);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,(UInt16)(t|0x00A2)); SsbSpinUs(2);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,(UInt16)(t|0x00A3));

  PhyWrite((UInt16)B43_PHY_CCK(0x2E),0x8078);  /* G, rev < 3 */
  PhyWrite((UInt16)B43_PHY_CCK(0x2F),0x0202);  /* G, rev >= 2 */

  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,sav->reg_3F4);
  PhyWrite((UInt16)B43_PHY_PGACTL,sav->phy_pgactl);
  PhyWrite((UInt16)B43_PHY_CCK(0x2A),sav->phy_cck_2A);
  PhyWrite((UInt16)B43_PHY_SYNCCTL,sav->phy_syncctl);
  PhyWrite((UInt16)B43_PHY_DACCTL,sav->phy_dacctl);
  RadioWrite(0x0043,sav->radio_43);
  RadioWrite(0x007A,sav->radio_7A);
  RadioMaskSet(0x0052,0xFF0F,sav->radio_52);   /* !has_tx_magnification */
  ssb_w16(gBus.bar0,B43_MMIO_3E2,sav->reg_3E2);
  /* B-PHY block: not run */
  PhyWrite((UInt16)B43_PHY_ANALOGOVER,sav->phy_analogover);
  PhyWrite((UInt16)B43_PHY_ANALOGOVERVAL,sav->phy_analogoverval);
  PhyWrite((UInt16)B43_PHY_CLASSCTL,sav->phy_classctl);
  PhyWrite((UInt16)B43_PHY_RFOVER,sav->phy_rfover);
  PhyWrite((UInt16)B43_PHY_RFOVERVAL,sav->phy_rfoverval);
  PhyWrite((UInt16)B43_PHY_CCK(0x3E),sav->phy_cck_3E);
  PhyWrite((UInt16)B43_PHY_CRS0,sav->phy_crs0);
  /* hwpctl block: not run */
  GphyChannelSwitch(sav->old_channel,1);       /* ⚠ synth_pu = 1, and this touches 0x3F4 again */
}

/* Signed printer: i7 printed abs() plus a separate "(negative)" line, which read badly and
 * made q = -1 look like q = 1. */
static void SayS(const char*t,int v)
{
  Str255 L;L[0]=0;PCat(L,t);
  if(v<0){ PCat(L,"-"); PCatDec(L,(unsigned long)(-v)); }
  else   { PCatDec(L,(unsigned long)v); }
  Out(L);
}

static void SaySigned16(const char*t,SInt16 v)
{
  Str255 L;L[0]=0;PCat(L,t);
  if(v<0){ PCat(L,"-"); PCatDec(L,(unsigned long)(-(long)v)); }
  else   { PCatDec(L,(unsigned long)v); }
  Out(L);
}

/* ---- b43_calibrate_lo_setting's MIDDLE: the statements between lo_measure_setup and
 * lo_probe_loctls_statemachine. Factored out because 4b-ii-8's b43_lo_g_adjust needs exactly the
 * same sequence, and a second hand-written copy is how g2 lost three steps. ---- */
static int LoCalibrateMiddle(void)
{
  int maxRxGain;
  RadioMaskSet(0x0043,0xFFF0,(UInt16)(rfAttG&0x000F));
  RadioMaskSet(0x0052,(UInt16)~0x0030,0);        /* with_padmix FALSE -> set 0 */
  maxRxGain=(int)(rfAttG*2)+(int)(bbAttG/2);     /* padmix false, so no subtraction */
  maxRxGain+=(int)gMaxLbGain;                    /* has_loopback_gain TRUE */
  LoMeasureGainValues(maxRxGain,1);
  SetBasebandAtt(bbAttG);
  return maxRxGain;
}

/* ---- b43_lo_g_adjust. rfatt is copied, b43_lo_fixup_rfatt is a no-op here (with_padmix is
 * FALSE), then get_calib_lo_settings calibrates on a cache miss and lo_write commits the pair.
 * We keep no persistent calibration cache, so this always calibrates -- which is what b43 does on
 * the first call for a given (bbatt, rfatt) anyway. ---- */
static void LoGAdjust(LoCtl *outCtl,int *outerOut,int *stepsOut)
{
  LoSaved sav; LoCtl ctl; int maxRxGain;
  LoMeasureSetupPrefix(&sav);
  maxRxGain=LoCalibrateMiddle();
  ctl.i=0; ctl.q=0;
  LoProbeLoctlsStatemachine(&ctl,&maxRxGain,outerOut,stepsOut);
  LoMeasureRestore(&sav);
  LoWrite(&ctl);                                 /* b43_lo_write(dev, &cal->ctl) */
  if(outCtl) *outCtl=ctl;
}

/* ⚠ b43_nrssi_hw_read / b43_nrssi_hw_write ARE NOT PORTED, AND THAT IS THE RULE, NOT AN OMISSION.
 * They touch a third indirect table (control PHY 0x0803, data 0x0804, no auto-increment cache),
 * but on this card NEITHER has a caller: b43_nrssi_hw_update is only reached from initg's
 * `!(boardflags & RSSI)` arm, and boardflags 0x000A has RSSI set; and calc_nrssi_threshold's
 * nrssi_hw_read(0x20) is in its first arm, which needs !gmode or !RSSI. Defining an accessor whose
 * only callers are unreachable is how h1 made its forbidden access expressible. They arrive with
 * the increment that needs them, if one ever does. */

static SInt32 ClampS32(SInt32 v,SInt32 lo,SInt32 hi)
{ if(v<lo) return lo; if(v>hi) return hi; return v; }

/* b43_set_all_gains. rev 2 > 1, so start/end are 0x08/0x18 and the table is GAINX (not
 * GAINX_R1). Ends with a dummy transmission -- the same routine 4b-ii-4b proved. */
static void SetAllGains(SInt16 first,SInt16 second,SInt16 third)
{
  UInt16 i,tmp;
  for(i=0;i<4;i++) OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,i,(UInt16)first);
  for(i=0x08;i<0x18;i++) OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,i,(UInt16)second);
  if(third!=-1){
    tmp=(UInt16)(((UInt16)third<<14)|((UInt16)third<<6));
    PhyMaskSet(0x04A0,0xBFBF,tmp);
    PhyMaskSet(0x04A1,0xBFBF,tmp);
    PhyMaskSet(0x04A2,0xBFBF,tmp); }
  DummyTransmission(0);
}

/* b43_set_original_gains. ⚠ It does NOT restore what was there before -- it writes a canonical
 * pattern. The first four entries swap bits 0 and 1 of the index (0,2,1,3), which is easy to
 * mistake for the identity while reading. */
static void SetOriginalGains(void)
{
  UInt16 i,tmp;
  for(i=0;i<4;i++){
    tmp=(UInt16)(i&0xFFFC);
    tmp|=(UInt16)((i&0x0001)<<1);
    tmp|=(UInt16)((i&0x0002)>>1);
    OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,i,tmp); }
  for(i=0x08;i<0x18;i++) OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,i,(UInt16)(i-0x08));
  PhyMaskSet(0x04A0,0xBFBF,0x4040);
  PhyMaskSet(0x04A1,0xBFBF,0x4040);
  PhyMaskSet(0x04A2,0xBFBF,0x4000);            /* ⚠ 0x4000 here, not 0x4040 */
  DummyTransmission(0);
}

/* b43_nrssi_mem_update: fills the SOFTWARE table gphy->nrssi_lt[], not hardware. */
static void NrssiMemUpdate(void)
{
  SInt16 i,delta; SInt32 tmp;
  delta=(SInt16)(0x1F-gNrssi0);
  for(i=0;i<64;i++){
    tmp=(SInt32)(i-delta)*gNrssiSlope;
    tmp/=0x10000;
    tmp+=0x3A;
    tmp=ClampS32(tmp,0,0x3F);
    gNrssiLt[i]=(SInt16)tmp; }
}

/* b43_calc_nrssi_threshold. gmode is TRUE and boardflags has RSSI set, so the else arm runs.
 * interfmode is NONE and both aci flags are 0 from prepare_structs, so a = 0xE and b = 0x11. */
static void CalcNrssiThreshold(void)
{
  SInt32 a,b; UInt16 t;
  a=0xE; b=0x11;
  a=a*(SInt32)(gNrssi1-gNrssi0);
  a+=((SInt32)gNrssi0<<6);
  if(a<32) a+=31; else a+=32;
  a=a>>6;
  a=ClampS32(a,-31,31);
  b=b*(SInt32)(gNrssi1-gNrssi0);
  b+=((SInt32)gNrssi0<<6);
  if(b<32) b+=31; else b+=32;
  b=b>>6;
  b=ClampS32(b,-31,31);
  gThrA=a; gThrB=b;
  t=(UInt16)(PhyRead(0x048A)&0xF000);
  t|=(UInt16)((UInt32)b&0x0000003F);
  t|=(UInt16)(((UInt32)a&0x0000003F)<<6);
  PhyWrite(0x048A,t);
  gThr048A=t;
}

/* ---- b43_calc_nrssi_slope. rev 2 and radio_rev 2, so: the rev >= 9 early return and the
 * radio_rev == 8 offset call do not happen, the rev >= 3 backup block and its switch do not run,
 * and every `rev >= 2` arm does. ---- */
static void CalcNrssiSlope(void)
{
  UInt16 b0,b1,b2,b3,b4,b5,b6,b7,b8,b9;
  SInt16 n0,n1; UInt16 tmp;

  PhyMaskSet((UInt16)B43_PHY_G_CRS,0x7FFF,0);
  PhyMaskSet(0x0802,0xFFFC,0);
  b7=ssb_r16(gBus.bar0,B43_MMIO_PHY_RADIO);
  ssb_w16(gBus.bar0,B43_MMIO_PHY_RADIO,(UInt16)(b7|0x8000));
  b0=RadioRead(0x007A); b1=RadioRead(0x0052); b2=RadioRead(0x0043);
  b3=PhyRead(0x0015); b4=PhyRead(0x005A); b5=PhyRead(0x0059); b6=PhyRead(0x0058);
  b8=ssb_r16(gBus.bar0,B43_MMIO_PHY0);
  b9=ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT);
  /* rev >= 3 block: NOT run */

  RadioSet(0x007A,0x0070);
  SetAllGains(0,8,0);
  RadioMaskSet(0x007A,0x00F7,0);
  PhyMaskSet(0x0811,0xFFCF,0x0030);            /* rev >= 2 */
  PhyMaskSet(0x0812,0xFFCF,0x0010);
  RadioSet(0x007A,0x0080);
  SsbSpinUs(20);

  n0=(SInt16)((PhyRead(0x047F)>>8)&0x003F);
  if(n0>=0x0020) n0=(SInt16)(n0-0x0040);

  RadioMaskSet(0x007A,0x007F,0);
  PhyMaskSet(0x0003,0xFF9F,0x0040);            /* rev >= 2 */
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT)|0x2000));
  RadioSet(0x007A,0x000F);
  PhyWrite(0x0015,0xF330);
  PhyMaskSet(0x0812,0xFFCF,0x0020);            /* rev >= 2 */
  PhyMaskSet(0x0811,0xFFCF,0x0020);

  SetAllGains(3,0,1);
  /* radio_rev != 8 */
  tmp=(UInt16)(RadioRead(0x0052)&0xFF0F); RadioWrite(0x0052,(UInt16)(tmp|0x0060));
  tmp=(UInt16)(RadioRead(0x0043)&0xFFF0); RadioWrite(0x0043,(UInt16)(tmp|0x0009));
  PhyWrite(0x005A,0x0480);
  PhyWrite(0x0059,0x0810);
  PhyWrite(0x0058,0x000D);
  SsbSpinUs(20);
  n1=(SInt16)((PhyRead(0x047F)>>8)&0x003F);
  if(n1>=0x0020) n1=(SInt16)(n1-0x0040);

  gNrssi0=n0; gNrssi1=n1;
  if(n0==n1) gNrssiSlope=0x00010000L;
  else       gNrssiSlope=0x00400000L/(SInt32)(n0-n1);
  /* ⚠ the nrssi[] pair is only stored when n0 >= -4, and the ORDER is swapped:
   * nrssi[0] = nrssi1, nrssi[1] = nrssi0. Getting that backwards would invert every
   * threshold computed from them. */
  if(n0>=-4){ gNrssi0=n1; gNrssi1=n0; }

  /* rev >= 3 restore block: NOT run */
  PhyMaskSet(0x0812,0xFFCF,0);                 /* rev >= 2 */
  PhyMaskSet(0x0811,0xFFCF,0);

  RadioWrite(0x007A,b0); RadioWrite(0x0052,b1); RadioWrite(0x0043,b2);
  ssb_w16(gBus.bar0,B43_MMIO_PHY_RADIO,b7);
  ssb_w16(gBus.bar0,B43_MMIO_PHY0,b8);
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,b9);
  PhyWrite(0x0015,b3); PhyWrite(0x005A,b4); PhyWrite(0x0059,b5); PhyWrite(0x0058,b6);
  SynthPuWorkaround(1);
  PhySet(0x0802,0x0003);
  SetOriginalGains();
  PhySet((UInt16)B43_PHY_G_CRS,0x8000);
  /* rev >= 3 restore block: NOT run */

  gNrssiRestoreBad=0;
  { UInt16 r[7],w[7]; int n=0,z;
    r[n]=0x0015; w[n++]=b3;
    r[n]=0x005A; w[n++]=b4;
    r[n]=0x0059; w[n++]=b5;
    r[n]=0x0058; w[n++]=b6;
    for(z=0;z<n;z++){ UInt16 got=PhyRead(r[z]);
      if(got!=w[z]){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ PHY 0x");PCatHex(L,r[z],4);
          PCat(L,"  want 0x");PCatHex(L,w[z],4);
          PCat(L,"  got 0x");PCatHex(L,got,4);Out(L); } } }
    /* ⚠⚠ THESE FIVE USED TO FAIL SILENTLY, AND k40 IS WHY THAT MATTERED.
     * The four PHY registers above name themselves on a mismatch. These five only bumped a
     * counter, so k40's "restore mismatches = 1" with no PHY line told us a register did not
     * come back and refused to say which -- out of five candidates. That is a bare count
     * standing where a decoded answer belongs, which is the same rule CLAUDE.md's "never log a
     * status register as a bare number" exists for.
     *
     * ⚠ This adds OUTPUT ONLY. ap_phy_initg.h is the mechanically extracted, diff-verified copy
     *   of Stage 4 and its value is that it still reproduces Stage 4 exactly; no register access
     *   or control flow changes here, only what gets printed when a comparison already failed. */
    { UInt16 g;
      g = RadioRead(0x007A); if(g!=b0){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ RADIO 0x007A  want 0x");PCatHex(L,b0,4);
          PCat(L,"  got 0x");PCatHex(L,g,4);Out(L); } }
      g = RadioRead(0x0052); if(g!=b1){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ RADIO 0x0052  want 0x");PCatHex(L,b1,4);
          PCat(L,"  got 0x");PCatHex(L,g,4);Out(L); } }
      g = RadioRead(0x0043); if(g!=b2){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ RADIO 0x0043  want 0x");PCatHex(L,b2,4);
          PCat(L,"  got 0x");PCatHex(L,g,4);Out(L); } }
      g = ssb_r16(gBus.bar0,B43_MMIO_PHY_RADIO); if(g!=b7){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ MMIO PHY_RADIO  want 0x");PCatHex(L,b7,4);
          PCat(L,"  got 0x");PCatHex(L,g,4);Out(L); } }
      g = ssb_r16(gBus.bar0,B43_MMIO_PHY0); if(g!=b8){ gNrssiRestoreBad++;
        { Str255 L;L[0]=0;PCat(L,"      ✗ MMIO PHY0  want 0x");PCatHex(L,b8,4);
          PCat(L,"  got 0x");PCatHex(L,g,4);Out(L); } } }
    /* ⚠ CHANNEL_EXT is NOT checked: synth_pu_workaround runs after it is written back, the
     * same trap as Oracle E's and Oracle L's 0x3F4. */ }

  NrssiMemUpdate();
  CalcNrssiThreshold();
}

/* ---- b43_set_txpower_g, COMPLETE at last. i2 ported statements 1..4 and stopped at
 * b43_lo_g_adjust because lo.c was not yet ported. j5 supplied LoGAdjust, so the function can
 * finally be closed -- and init_pctl needs the whole thing, twice. ---- */
static void SetTxpowerG(UInt16 bb,UInt16 rf,UInt8 txctl)
{
  int outer=0,steps=0;
  bbAttG=bb; rfAttG=rf;                          /* b43 stores these into gphy */
  SetBasebandAtt(bb);                            /* stmt 1 */
  ShmWrite16Shared(B43_SHM_SH_RFATT,rf);         /* stmt 2 */
  RadioMaskSet(0x0043,0xFFF0,(UInt16)(rf&0x000F));            /* stmt 3, radio_rev != 8 */
  RadioMaskSet(0x0052,(UInt16)~0x0070,(UInt16)(txctl&0x0070));
  RadioMaskSet(0x0052,0xFFF0,0);                 /* stmt 4, !has_tx_magnification, tx_bias 0 */
  LoGAdjust(NULL,&outer,&steps);                 /* stmt 5 -- the piece i2 could not reach */
}

/* b43_hardware_pctl_early_init. has_hardware_pctl is FALSE (rev 2 < 6), so the early-return arm
 * runs and the whole hardware-power-control body does not. */
static void HardwarePctlEarlyInit(void){ PhyWrite(0x047A,0xC111); }

/* b43_hardware_pctl_init_gphy. Same guard, same outcome: clear HWPCTL in the host flags and
 * return. B43_HF_HWPCTL is 0x000000800000, i.e. bit 23 -- which lands in the MIDDLE host-flag
 * word (bits 16..31) as 0x0080, not the low one. */
static void HardwarePctlInitGphy(void)
{ UInt16 lo,mi,hi; HfRead(&lo,&mi,&hi); mi=(UInt16)(mi&(UInt16)~0x0080); HfWrite(lo,mi,hi); }

static void ShmClearTssi(void)
{ ShmWrite16Shared(0x0058,0x7F7F); ShmWrite16Shared(0x005A,0x7F7F);
  ShmWrite16Shared(0x0070,0x7F7F); ShmWrite16Shared(0x0072,0x7F7F); }

static UInt16 gCurIdleTssi=0,gOldBb=0,gOldRf=0;
static UInt16 gChipPkg=0; static UInt32 gChipIdRaw=0; static int gChipPkgBranch=0;
#define B43_PHY_ITSSI B43_PHY_CCK(0x29)

/* ---- b43_phy_init_pctl. board_vendor is 0x106B (Apple), not BCM, so the BU4306 early return
 * does not fire. gmode is TRUE. cur_idle_tssi is 0 from prepare_structs, so the measurement arm
 * runs; analog is 2 (not 0), so it takes the set_txpower_g path rather than the radio 0x76 one,
 * and radio_rev != 8 gives rfatt 9 / no padmix. ---- */
static void PhyInitPctl(void)
{
  UInt16 oldBb,oldRf; UInt8 oldTxctl;
  PhyWrite(0x0028,0x8018);
  ssb_w16(gBus.bar0,B43_MMIO_PHY0,(UInt16)(ssb_r16(gBus.bar0,B43_MMIO_PHY0)&0xFFDF));
  HardwarePctlEarlyInit();
  /* cur_idle_tssi == 0 */
  oldBb=bbAttG; oldRf=rfAttG; oldTxctl=0;        /* gphy->tx_control, the truncated 0x00 */
  gOldBb=oldBb; gOldRf=oldRf;
  SetTxpowerG(11,9,0);                           /* bbatt 11, rfatt 9, no padmix */
  DummyTransmission(0);
  gCurIdleTssi=PhyRead((UInt16)B43_PHY_ITSSI);
  /* ⚠ b43's idle-TSSI sanity check is inside `if (B43_DEBUG)`, which is off in a normal build,
   * so it is NOT ported. Porting a debug-only branch would change behaviour relative to the
   * driver everyone actually ships. The value is logged instead. */
  SetTxpowerG(oldBb,oldRf,oldTxctl);             /* restore */
  HardwarePctlInitGphy();
  ShmClearTssi();
}

static int gBad=0;
static void CheckPhy(const char*what,UInt16 reg,UInt16 want)
{ UInt16 got=PhyRead(reg); Str255 L;L[0]=0;
  PCat(L,"      ");PCat(L,what);PCat(L," 0x");PCatHex(L,reg,4);
  PCat(L,"  want 0x");PCatHex(L,want,4);PCat(L,"  got 0x");PCatHex(L,got,4);
  PCat(L,(got==want)?"  ok":"  ✗ MISMATCH");Out(L);
  if(got!=want) gBad++; }

/* ===========================================================================
 * b43_phy_init -> b43_phy_initg, lifted from airport_initb6.c's main().
 * The declarations below are main()'s, minus the four that belonged to its
 * window and event loop (win, bounds, evt, y) -- none of which the sequence
 * references. Checked, not assumed.
 * ========================================================================= */
static int ApPhyInitG(void)
{
    /* ⚠ main()'s `short i,y` is NOT here, and the compiler proved that correct: both were used
     * only by the window-drawing epilogue this extraction deliberately leaves behind. Every `i`
     * inside the sequence is a block-local of its own. Dropping them is the second and last
     * difference from main()'s declarations, after win/bounds/evt. */
    UInt16 off,val,r7a,before7a;
    UInt16 bbAtt=0,rfAtt=0,txRaw=0,txBias=0;
    UInt8  txCtl=0;
    int oracleA=0,oracleB=0,oracleC=0,oracleD=0,oracleE=0,oracleF=0,oracleG=0,oracleH=0,oracleI=0,oracleJ=0,oracleK=0,oracleL=0,oracleM=0,oracleN=0,oracleO=0;
    /* ================= b43_phy_initb6, from its first statement ================= */
    Say("");
    Say("=== b43_phy_initb6 (phy_g.c:1585) ===");

    /* ⚠ THERE IS NO "read 0x7A before we start" LINE HERE, AND THAT IS DELIBERATE.
     * The obvious diagnostic -- sample radio 0x7A first, so Oracle A can show its starting value
     * -- is the access that master-aborted in g1, g2, g3, h1 and h2. Register 0x7A is unreachable
     * until PHY 0x003E is written, so a read placed before that write is the identical bug with a
     * friendlier comment on it. The starting value is captured instead as the READ HALF of
     * statement 1587, which is where the reference reads it too. No extra access exists. */

    /* phy_g.c:1586 */
    PhyWrite(0x003E,0x817A);
    SayH("  PHY 0x003E = 0x817A, reads back ",(unsigned long)PhyRead(0x003E),4);

    /* phy_g.c:1587 -- b43_radio_write16(dev, 0x007A, b43_radio_read16(dev, 0x007A) | 0x0058) */
    before7a=RadioRead(0x007A);
    SayH("  radio 0x7A on entry      = ",before7a,4);
    Say("    (h3 measured 0x0000; Oracle A's expected value is derived from whatever this reads.)");
    RadioWrite(0x007A,(UInt16)(before7a|0x0058));
    SayH("  after stmt 1587          = ",(unsigned long)RadioRead(0x007A),4);

    /* radio_rev 4/5, 6/7 and 8 blocks: NOT TAKEN on this card (rev 2). Not ported as dead
     * branches -- the hardware that would enter them is not the hardware in the machine. */

    /* ---- the three PHY table loops ---- */
    Say("");
    Say("  PHY table loops 0x88..0xC7");
    val=0x1E1F;
    for(off=0x0088;off<0x0098;off++){ PhyWrite(off,val); val=(UInt16)(val-0x0202); }
    val=0x3E3F;
    for(off=0x0098;off<0x00A8;off++){ PhyWrite(off,val); val=(UInt16)(val-0x0202); }
    val=0x2120;
    for(off=0x00A8;off<0x00C8;off++){ PhyWrite(off,(UInt16)(val&0x3F3F)); val=(UInt16)(val+0x0202); }
    Say("    written; sampling first/middle/last of each against the recomputed formula");
    gBad=0;
    CheckPhy("loop1",0x0088,ExpectLoop1(0x0088));
    CheckPhy("loop1",0x0090,ExpectLoop1(0x0090));
    CheckPhy("loop1",0x0097,ExpectLoop1(0x0097));
    CheckPhy("loop2",0x0098,ExpectLoop2(0x0098));
    CheckPhy("loop2",0x00A0,ExpectLoop2(0x00A0));
    CheckPhy("loop2",0x00A7,ExpectLoop2(0x00A7));
    CheckPhy("loop3",0x00A8,ExpectLoop3(0x00A8));
    CheckPhy("loop3",0x00B8,ExpectLoop3(0x00B8));
    CheckPhy("loop3",0x00C7,ExpectLoop3(0x00C7));
    if(!gBad){ oracleB=1;
      Say("    ==> ORACLE B PASSED: stride, seed, and the loop-3 distinction between the MASKED");
      Say("        value written and the UNMASKED running counter are all correct.");
    } else Say("    ✗ ORACLE B FAILED -- see the mismatches above.");

    /* ---- phy->type == B43_PHYTYPE_G: taken on this card ---- */
    Say("");
    Say("  PHY type G block");
    RadioSet(0x007A,0x0020);
    RadioSet(0x0051,0x0004);
    PhySet((UInt16)B43_PHY_EXTG(0x02),0x0100);   /* b43_phy_set(dev, 0x0802, 0x0100) */
    PhySet(0x042B,0x2000);                       /* OFDM bank (0x0400) register 0x2B */
    PhyWrite(0x005B,0);
    PhyWrite(0x005C,0);
    SayH("    radio 0x7A after |0x0020 = ",(unsigned long)RadioRead(0x007A),4);

    /* ---- channel dance: phy->channel is 1 (< 8), so switch to 13 and back ---- */
    Say("");
    Say("  channel: old = 1, so switch to 13, program the radio, switch back");
    Say("    ⚠ FIRST TOUCH of radio 0x50, 0x5A, 0x5B, 0x5C and 0x7C. Every one appears in");
    Say("      b43_phy_initb6, so all are driver-sanctioned -- but none has ever been accessed");
    Say("      on this card, and only 0x7C is READ (0x50/0x5A/0x5B/0x5C are write-only here).");
    Say("      A read is what aborts, so 0x7C is the one to watch; each step logs before it acts.");
    GphyChannelSwitch(13,0);
    RadioWrite(0x0050,0x0020);
    RadioWrite(0x0050,0x0023);
    SsbSpinUs(40);                                /* udelay(40) */
    /* radio_rev < 6 -> taken */
    Say("    -> about to READ radio 0x7C (first ever). If the log stops on this line, 0x7C is");
    Say("       unreachable the way 0x7A was before PHY 0x003E, and that is the next question.");
    RadioWrite(0x007C,(UInt16)(RadioRead(0x007C)|0x0002));
    SayH("       radio 0x7C read ok, now = ",(unsigned long)RadioRead(0x007C),4);
    RadioWrite(0x0050,0x0020);
    /* radio_rev <= 2 -> taken */
    RadioWrite(0x0050,0x0020);
    RadioWrite(0x005A,0x0070);
    RadioWrite(0x005B,0x007B);
    RadioWrite(0x005C,0x00B0);
    RadioMaskSet(0x007A,0x00F8,0x0007);
    GphyChannelSwitch(1,0);
    SayH("    CHANNEL_EXT after the dance = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT),4);

    /* ---- ORACLE A ---- */
    Say("");
    Say("  ORACLE A -- radio 0x7A, end to end");
    r7a=RadioRead(0x007A);
    SayH("    radio 0x7A = ",r7a,4);
    Say("      0x0000 |0x0058 = 0x0058 ; |0x0020 = 0x0078 ; (&0x00F8)|0x0007 = 0x007F");
    if(before7a==0x0000U && r7a==0x007FU){ oracleA=1;
      Say("    ==> ORACLE A PASSED: three read-modify-writes through the radio accessor, and");
      Say("        the branch decisions above, all land on the computed value.");
    } else if(r7a==(UInt16)((((before7a|0x0058U)|0x0020U)&0x00F8U)|0x0007U)){ oracleA=1;
      Say("    ==> ORACLE A PASSED against the value actually measured before initb6 (which was");
      Say("        not 0x0000 this run). The arithmetic chain is still exactly right.");
    } else Say("    ✗ ORACLE A FAILED.");

    /* ---- the scalar tail, up to but NOT including set_txpower_g ---- */
    Say("");
    Say("  scalar writes (radio_rev < 6 -> PHY 0x2A = 0x8AC0, not 0x88C2)");
    PhyWrite(0x0014,0x0200);
    PhyWrite(0x002A,0x8AC0);
    PhyWrite(0x0038,0x0668);
    gBad=0;
    CheckPhy("scalar",0x0014,0x0200);
    CheckPhy("scalar",0x002A,0x8AC0);
    CheckPhy("scalar",0x0038,0x0668);
    if(!gBad){ oracleC=1; Say("    ==> ORACLE C PASSED."); }
    else Say("    ✗ ORACLE C FAILED.");

    /* ================= b43_set_txpower_g (phy_g.c:203) ================= */
    Say("");
    Say("=== b43_set_txpower_g (phy_g.c:203) -- 4b-ii-3 ===");
    Say("  Its arguments are gphy->bbatt, gphy->rfatt and gphy->tx_control, all set by");
    Say("  b43_gphy_op_prepare_hardware. Ordering read from b43_wireless_core_init:");
    Say("    prepare_structs (memset gphy, lo->tx_bias = 0xFF)  ->  prepare_hardware (the");
    Say("    defaults)  ->  chip_init -> phy_init -> initg -> initb6 -> here.");
    Say("  So the defaults ARE in place by now; prepare_structs runs first and cannot wipe them.");

    /* Identity, measured -- the derivations below are only valid for this exact part. */
    { UInt32 raw,subsys=0; UInt16 rmanuf,rver,rrev,bvend,btype;
      ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,0x0001);
      (void)ssb_r16(gBus.bar0,B43_MMIO_RADIO_CONTROL);
      raw=(UInt32)ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_LOW);
      ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,0x0001);
      (void)ssb_r16(gBus.bar0,B43_MMIO_RADIO_CONTROL);
      raw|=((UInt32)ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_HIGH))<<16;
      rmanuf=(UInt16)(raw&0x00000FFFUL);
      rver  =(UInt16)((raw&0x0FFFF000UL)>>12);   /* b43 calls this radio_ver */
      rrev  =(UInt16)((raw&0xF0000000UL)>>28);
      SayH("  radio raw = ",raw,8);
      SayH("    manuf = ",rmanuf,4); SayH("    ver = ",rver,4); SayH("    rev = ",rrev,4);

      if(ExpMgrConfigReadLong(&gBus.node,(LogicalAddress)0x2C,&subsys)!=noErr) subsys=0;
      bvend=(UInt16)(subsys&0xFFFFUL); btype=(UInt16)((subsys>>16)&0xFFFFUL);
      SayH("  PCI 0x2C subsystem = ",subsys,8);
      SayH("    board vendor = ",bvend,4); SayH("    board type   = ",btype,4);

      if(rmanuf!=0x017FU||rver!=0x2050U||rrev!=2U){
        Say("  !! NOT the part these derivations were reasoned about (expected Broadcom 0x17F,");
        Say("     radio 0x2050 rev 2). REFUSING rather than applying an attenuation derived for");
        Say("     different silicon -- a wrong attenuation does not fault, it mis-tunes.");
        goto initg_done; }

      /* b43_gphy_op_prepare_hardware's three defaults, derived not transcribed. */
      bbAtt = DefaultBasebandAtt(rver,rrev); bbAttG = bbAtt;
      /* boardRev is passed 0: it is read from SPROM in b43 and gates ONLY the BCM4309G arm,
       * which additionally requires boardVendor == 0x14E4. This card reports 0x106B (Apple),
       * so that arm is unreachable here and the value cannot change the result. */
      rfAtt = DefaultRadioAtt_2050rev2_G(gBus.chipId,bvend,btype,0); rfAttG = rfAtt;
      txRaw = DefaultTxControlRaw(rver,rrev);
      txCtl = (UInt8)(txRaw<<4);      /* ⚠⚠ see the note below -- this truncates */

      Say1("  derived bbatt.att   = ",bbAtt);
      Say1("  derived rfatt.att   = ",rfAtt);
      SayH("  default_tx_control() returns = ",txRaw,4);
      SayH("  gphy->tx_control (u8) becomes = ",txCtl,2);
      Say("    ⚠⚠ THIS TRUNCATION IS REAL AND IS b43's, NOT A PORTING SLIP.");
      Say("      phy_g.c:2526 is `gphy->tx_control = (default_tx_control(dev) << 4);` and");
      Say("      phy_g.h:150 declares `u8 tx_control`. default_tx_control returns PA2DB (0x20)");
      Say("      for radio rev 2, so 0x20 << 4 = 0x200 and the u8 keeps 0x00. Every non-zero");
      Say("      return it can make (0x30, 0x20, 0x10) shifts out of u8 range the same way, and");
      Say("      the constants are ALREADY positioned in the 0x70 field that set_txpower_g masks");
      Say("      with. The shift looks like upstream breakage. We port what b43 EXECUTES -- 0x00");
      Say("      -- because that is what has run on millions of cards; the alternative reading");
      Say("      (0x20) is logged above so a later TX-power increment can revisit it.");
    }

    /* lo->tx_bias is 0xFF from prepare_structs, and set_txpower_g folds 0xFF to 0.
     * lo->tx_magn is 0 because lo_control is kzalloc'd and nothing has set it yet. */
    txBias=0;
    Say("  lo->tx_bias = 0xFF from prepare_structs -> folded to 0 by set_txpower_g");
    Say("  lo->tx_magn = 0 (lo_control is kzalloc'd and nothing has written it yet), but it is");
    Say("    never read here: only the has_tx_magnification arm consumes it, and that needs");
    Say("    radio_rev == 8.");

    /* stmt 1 -- b43_gphy_set_baseband_attenuation(dev, bb). analog is 2, so the `> 1` arm. */
    Say("");
    Say("  stmt 1: set_baseband_attenuation(bb) -- analog 2 -> maskset(DACCTL, 0xFFC3, bb<<2)");
    PhyMaskSet((UInt16)B43_PHY_DACCTL,0xFFC3,(UInt16)(bbAtt<<2));
    SayH("    DACCTL (CCK 0x60) now = ",(unsigned long)PhyRead((UInt16)B43_PHY_DACCTL),4);

    /* stmt 2 -- b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_RFATT, rf) */
    Say("  stmt 2: SHM_SHARED[0x0064] = rfatt");
    ShmWrite16Shared(B43_SHM_SH_RFATT,rfAtt);

    /* stmt 3 -- radio_ver == 0x2050 && radio_rev == 8 ? no (rev 2) -> the else arm */
    Say("  stmt 3: radio_rev != 8, so the two-maskset arm");
    Say("    ⚠ FIRST TOUCH of radio 0x43 and 0x52, and both are read-modify-WRITES, so both");
    Say("      READ first. A read is what master-aborts, and 0x7A needed PHY 0x003E before it");
    Say("      would answer. Each is logged before it happens so an abort names the register.");
    Say("    -> about to READ radio 0x43 (first ever)");
    RadioMaskSet(0x0043,0xFFF0,(UInt16)(rfAtt&0x000F));
    SayH("       radio 0x43 ok, now = ",(unsigned long)RadioRead(0x0043),4);
    Say("    -> about to READ radio 0x52 (first ever)");
    RadioMaskSet(0x0052,(UInt16)~0x0070,(UInt16)(txCtl&0x0070));
    SayH("       radio 0x52 ok, now = ",(unsigned long)RadioRead(0x0052),4);

    /* stmt 4 -- has_tx_magnification = (rev>=2 && radio_ver==0x2050 && radio_rev==8): FALSE
     * for us, because radio_rev is 2, so the maskset arm rather than the direct write. */
    Say("  stmt 4: has_tx_magnification is FALSE (needs radio_rev == 8), so maskset(0x52,0xFFF0)");
    RadioMaskSet(0x0052,0xFFF0,(UInt16)(txBias&0x000F));

    /* ---- ORACLE D: the values set_txpower_g leaves behind ---- */
    Say("");
    Say("  ORACLE D -- set_txpower_g's visible results");
    gBad=0;
    { UInt16 shmRf=ShmRead16Shared(B43_SHM_SH_RFATT);
      UInt16 r43=RadioRead(0x0043), r52=RadioRead(0x0052);
      Str255 L;
      L[0]=0;PCat(L,"      SHM[0x64] rfatt  want 0x");PCatHex(L,rfAtt,4);
      PCat(L,"  got 0x");PCatHex(L,shmRf,4);PCat(L,(shmRf==rfAtt)?"  ok":"  ✗ MISMATCH");Out(L);
      if(shmRf!=rfAtt) gBad++;
      SayH("      radio 0x43 = ",r43,4);
      Say ("        low nibble must equal rfatt; the upper bits are whatever the part held.");
      if((UInt16)(r43&0x000F)!=(UInt16)(rfAtt&0x000F)) gBad++;
      SayH("      radio 0x52 = ",r52,4);
      Say ("        bits 0-3 and 4-6 were both masked to 0 this run (tx_bias 0, tx_control 0),");
      Say ("        so the low SEVEN bits must read 0.");
      if((UInt16)(r52&0x007F)!=0) gBad++; }
    if(!gBad){ oracleD=1;
      Say("    ==> ORACLE D PASSED: the SHM attenuation word, the radio 0x43 nibble and the");
      Say("        masked field of radio 0x52 all hold the derived values.");
    } else Say("    ✗ ORACLE D FAILED.");

    /* ================= b43_lo_g_adjust -> ... -> lo_measure_setup / restore ================= */
    Say("");
    Say("=== lo_measure_setup PREFIX + lo_measure_restore (lo.c) -- 4b-ii-4a ===");
    Say("  b43_lo_g_adjust -> b43_get_calib_lo_settings -> b43_calibrate_lo_setting, whose first");
    Say("  act after the (no-op) mac_suspend is lo_measure_setup. This runs its statements 1..12");
    Say("  and then lo_measure_restore, with the MEASUREMENT deliberately omitted between them.");
    Say("");
    Say("  ⚠ WHY THAT IS A ROUND TRIP AND NOT A HALF-RUN SEQUENCE. lo_measure_restore consumes");
    Say("    only `sav` and gphy->pga_gain. pga_gain is 0 until lo_measure_gain_values runs, and");
    Say("    it affects ONLY the transient RFOVERVAL ramp (0xA0/0xA2/0xA3) -- the rev>=2 block");
    Say("    later in restore overwrites RFOVERVAL from sav->phy_rfoverval. So restore's FINAL");
    Say("    state is fully determined by what setup saved, which is exactly what makes");
    Say("    'every saved register must come back' a real oracle.");
    Say("");
    Say("  ⏹ setup STOPS before statement 13, `b43_dummy_transmission(dev, false, true)` -- 85");
    Say("    lines that make the MAC actually TRANSMIT. That would be the first transmission this");
    Say("    card has ever performed under OS 9 and it deserves its own run, not a footnote in");
    Say("    someone else's. lo_measure_txctl_values (96 lines, calling lo_measure_feedthrough)");
    Say("    is deferred with it. Both are 4b-ii-4b.");
    Say("");
    Say("  ORACLE E: every register lo_measure_setup saved must read back its saved value after");
    Say("  lo_measure_restore. This is the pairing most likely to be silently wrong -- a register");
    Say("  saved but not restored corrupts PHY state with no fault at all.");
    { LoSaved sav;
      UInt16 got,want;
      Say("");
      Say("  -> lo_measure_setup (statements 1..18 -- COMPLETE in i4)");
      LoMeasureSetupPrefix(&sav);
      SayH("     saved PGACTL      = ",sav.phy_pgactl,4);
      SayH("     saved SYNCCTL     = ",sav.phy_syncctl,4);
      SayH("     saved DACCTL      = ",sav.phy_dacctl,4);
      SayH("     saved CCK 0x3E    = ",sav.phy_cck_3E,4);
      Say ("       ⚠ CCK 0x3E IS THE REGISTER initb6 WROTE 0x817A TO. b43_phy_write(dev, 0x003E,");
      Say ("         0x817A) at phy_g.c:1586 and B43_PHY_CCK(0x3E) are the same address, so this");
      Say ("         save has a COMPUTED expectation, not just a round-trip one.");
      SayH("     saved RFOVER      = ",sav.phy_rfover,4);
      SayH("     saved RFOVERVAL   = ",sav.phy_rfoverval,4);
      SayH("     saved CLASSCTL    = ",sav.phy_classctl,4);
      SayH("     saved CRS0        = ",sav.phy_crs0,4);
      SayH("     saved radio 0x43  = ",sav.radio_43,4);
      SayH("     saved radio 0x7A  = ",sav.radio_7A,4);
      SayH("     saved radio 0x52  = ",sav.radio_52,4);
      Say ("       (b43 masks this one to 0x00F0 as it saves it, so only bits 4-7 are carried.)");
      SayH("     saved 0x3F4       = ",sav.reg_3F4,4);
      SayH("     saved 0x3E2       = ",sav.reg_3E2,4);
      Say ("       ⚠ 0x3E2 is FIRST TOUCH -- never read on this card before this line.");

      gBad=0;
      if(sav.phy_cck_3E!=0x817AU){ gBad++;
        Say("     ✗ CCK 0x3E did not hold 0x817A. initb6 wrote it and i1/i2 verified the write,");
        Say("       so either the bank routing differs for this read or something cleared it."); }
      else Say("     ✓ CCK 0x3E held 0x817A, as initb6 left it.");

      /* ---- ORACLE F: the transmission handshake, and the txctl derivation ---- */
      Say("");
      Say("  ORACLE F -- did the card actually TRANSMIT?");
      Say("");
      Say("  ⚠⚠ i4's ORACLE F WAS MIS-SPECIFIED, AND i5 PROVED IT. It required loop 1 to exit");
      Say("     early, treating a timeout as 'no transmission'. But b43 NEVER CHECKS THOSE LOOPS:");
      Say("     `i` and `value` are discarded after each one and it proceeds regardless. They are");
      Say("     bounded best-effort waits, not assertions. i4 turned a BOUND into an assertion");
      Say("     the reference does not make, then reported 'No transmission' when the card had");
      Say("     in fact transmitted. The port was right and the oracle was wrong.");
      Say("");
      Say("  i5's per-step trace gave the real state machine, measured on this card:");
      Say("     TXE0_STATUS  0x0001 idle (bit 0) -> 0x00C1 during TX (bits 0,6,7) -> 0x0401");
      Say("                  done (bits 0,10). Bit 10 is STICKY: variant B's baseline still read");
      Say("                  0x0401 from variant A's transmission.");
      Say("     IFSSTAT      bit 8 SET while transmitting, CLEAR otherwise.");
      Say("     The trigger is the TXE0_AUX = 0x0030 write, not any of the earlier ones.");
      Say("");
      Say("  So ORACLE F now checks the LATCHED evidence rather than a transient the poll loop");
      Say("  is too slow to catch. Bit 7 is confirming, not required: missing it means the TX");
      Say("  finished faster, which is not a failure.");
      Say("    ⚠ These five are VARIANT B's values -- it runs second and overwrites the capture.");
      Say("      Variant A's baseline is in the per-step trace above, and it is the one that");
      Say("      proves a cold start: A reads 0x0001 (bit 10 CLEAR) after SsbCoreEnable's reset,");
      Say("      then 0x0401. B then starts at 0x0401 because A had just transmitted, which is");
      Say("      what shows bit 10 is sticky across the sequence but cleared by a core reset.");
      SayH("    baseline TXE0_STATUS          = ",(unsigned long)gTxBase,4);
      SayH("    at the trigger, TXE0_STATUS   = ",(unsigned long)gTxTrig,4);
      SayH("    at the trigger, IFSSTAT       = ",(unsigned long)gTxIfsTrig,4);
      SayH("    after the waits, TXE0_STATUS  = ",(unsigned long)gTxFinal,4);
      SayH("    after the waits, IFSSTAT      = ",(unsigned long)gTxIfsFinal,4);
      { int done   = ((gTxFinal&0x0400)!=0);            /* latched completion */
        int busy   = ((gTxIfsTrig&0x0100)!=0);          /* MAC was busy at the trigger */
        int idleOk = ((gTxIfsFinal&0x0100)==0);         /* and is not busy afterwards */
        int saw7   = ((gTxTrig&0x0080)!=0);             /* confirming only */
        Say(done  ? "    ✓ TXE0_STATUS bit 10 SET -- a transmission completed."
                  : "    ✗ TXE0_STATUS bit 10 clear -- no transmission completed.");
        Say(busy  ? "    ✓ IFSSTAT bit 8 was SET at the trigger -- the MAC was transmitting."
                  : "    ✗ IFSSTAT bit 8 was clear at the trigger -- the MAC never went busy.");
        Say(idleOk? "    ✓ IFSSTAT bit 8 clear afterwards -- the MAC returned to idle."
                  : "    ✗ IFSSTAT bit 8 still set -- the MAC stayed busy.");
        Say(saw7  ? "    ✓ bit 7 caught in the act (confirming)."
                  : "      (bit 7 not caught -- the TX finished faster than the sample. Not a");
        if(!saw7) Say("       failure: b43 does not check it either.");
        Say("");
        Say("    lo_measure_txctl_values:");
        Say1("      derived radio_pctl_reg = ",(unsigned long)gRadioPctlReg);
        Say ("        chain: max_lb_gain 0 (calc_loopback_gain runs later in initg) -> lb_gain 0");
        Say ("        -> (10-0) < 36 so tmp 10 -> +3 = 13 -> /4 = 3 ; cmp 36/4 = 9 ; 3 < 9 -> 3");
        { UInt16 d=PhyRead((UInt16)B43_PHY_DACCTL), r43=RadioRead(0x0043), r52=RadioRead(0x0052);
          SayH("      DACCTL now   = ",d,4);
          Say ("        set_baseband_attenuation(2) -> (x & 0xFFC3) | 0x08. i2 left DACCTL at");
          Say ("        0x0003, so this should now read 0x000B -- a CHANGE, not a no-op.");
          SayH("      radio 0x43   = ",r43,4);
          SayH("      radio 0x52   = ",r52,4);
          Say ("        0x52 had bits 4-5 cleared by radio_mask(0x52, ~0x30) and bits 0-3 by");
          Say ("        radio_mask(0x52, 0xFFF0), so its low SIX bits must read 0.");
          if(done && busy && idleOk && gRadioPctlReg==3
             && (UInt16)(d&0x003C)==0x0008
             && (UInt16)(r43&0x000F)==3
             && (UInt16)(r52&0x003F)==0){ oracleF=1;
            Say("    ==> ORACLE F PASSED: the MAC drove a frame out of the TX engine and came");
            Say("        back to idle, and the txctl derivation landed on its computed values.");
            Say("    ⇒ ★★★ THIS CARD HAS TRANSMITTED UNDER MAC OS 9.");
          } else Say("    ✗ ORACLE F FAILED."); } }

      /* ===== b43_calibrate_lo_setting's middle, between setup and restore ===== */
      Say("");
      Say("=== the LO I/Q search (lo.c) -- 4b-ii-4c ===");
      Say("  b43_calibrate_lo_setting, between lo_measure_setup and lo_measure_restore:");
      Say("    txctl_reg = lo_txctl_register_table()    -> 0x52, value 0x30 (G, radio_rev != 8)");
      Say("    radio_maskset(0x43, 0xFFF0, rfatt->att)");
      Say("    radio_maskset(txctl_reg, ~txctl_value, with_padmix ? txctl_value : 0)");
      Say("    max_rx_gain = rfatt*2 + bbatt/2 [- pad_mix if padmix] [+ max_lb_gain if lb]");
      Say("    lo_measure_gain_values(max_rx_gain, has_loopback_gain)");
      Say("    set_baseband_attenuation(bbatt->att)");
      Say("    lo_probe_loctls_statemachine(&loctl, &max_rx_gain)");
      { LoCtl ctl; int maxRxGain,outer=0,steps=0;
        /* ⚠ ONE DEFINITION. This used to be an inline copy of the same six statements that
         * b43_lo_g_adjust needs in 4b-ii-8. Two hand-written copies of a sequence is exactly how
         * g2 lost three steps out of chip_init's prefix, so it is now a shared function and both
         * callers use it. */
        maxRxGain=LoCalibrateMiddle();
        Say1("    max_rx_gain = rfatt*2 + bbatt/2 + max_lb_gain = ",(unsigned long)maxRxGain);
        SaySigned16("    lna_lod_gain = ",gLnaLodGain);
        SaySigned16("    pga_gain     = ",gPgaGain);
        SaySigned16("    lna_gain     = ",gLnaGain);
        Say ("      ⚠ pga_gain feeds lo_measure_restore's RFOVERVAL ramp, which i3/i4 ran with 0");
        Say ("        because the search had not executed yet.");

        gFtFirst=0;gFtMin=0xFFFF;gFtMax=0;gFtCount=0;
        ctl.i=0; ctl.q=0;
        Say("    -> running the search (bounded: 4 outer passes x 24 steps x 8 directions)");
        LoProbeLoctlsStatemachine(&ctl,&maxRxGain,&outer,&steps);
        gLoI=ctl.i; gLoQ=ctl.q;
        gFt1First=gFtFirst; gFt1Min=gFtMin; gFt1Max=gFtMax; gFt1Count=gFtCount;
        gOuter1=outer; gSteps1=steps; gLow1=gLowestFeedth;

        /* REPEATABILITY. The result is measured, so the strongest check available is whether an
         * identical search on unchanged hardware lands on the same pair. b43 calls
         * b43_calibrate_lo_setting repeatedly across different (bbatt, rfatt) pairs, so running
         * the search twice is inside the envelope of what the driver does to this engine -- and
         * every register it touches (LO_CTL, RFOVERVAL, PGACTL) it is already writing hundreds
         * of times during the first pass. */
        Say("");
        Say("    -> running it a SECOND time from the same starting pair, for repeatability");
        gFtFirst=0;gFtMin=0xFFFF;gFtMax=0;gFtCount=0;
        ctl.i=0; ctl.q=0;
        maxRxGain=(int)(rfAtt*2)+(int)(bbAtt/2)+(int)gMaxLbGain;
        LoMeasureGainValues(maxRxGain,1);
        LoProbeLoctlsStatemachine(&ctl,&maxRxGain,&outer,&steps);
        gLoI2=ctl.i; gLoQ2=ctl.q;
        gFt2Count=gFtCount; gOuter2=outer; gSteps2=steps; gLow2=gLowestFeedth; }

      Say("");
      Say("  -> lo_measure_restore (full)");
      LoMeasureRestore(&sav);

      Say("");
      Say("  ORACLE E -- every saved register must be back");
      CheckPhy("PGACTL       ",(UInt16)B43_PHY_PGACTL,sav.phy_pgactl);
      CheckPhy("CCK 0x2A     ",(UInt16)B43_PHY_CCK(0x2A),sav.phy_cck_2A);
      CheckPhy("SYNCCTL      ",(UInt16)B43_PHY_SYNCCTL,sav.phy_syncctl);
      CheckPhy("DACCTL       ",(UInt16)B43_PHY_DACCTL,sav.phy_dacctl);
      CheckPhy("ANALOGOVER   ",(UInt16)B43_PHY_ANALOGOVER,sav.phy_analogover);
      CheckPhy("ANALOGOVERVAL",(UInt16)B43_PHY_ANALOGOVERVAL,sav.phy_analogoverval);
      CheckPhy("CLASSCTL     ",(UInt16)B43_PHY_CLASSCTL,sav.phy_classctl);
      CheckPhy("RFOVER       ",(UInt16)B43_PHY_RFOVER,sav.phy_rfover);
      CheckPhy("RFOVERVAL    ",(UInt16)B43_PHY_RFOVERVAL,sav.phy_rfoverval);
      CheckPhy("CCK 0x3E     ",(UInt16)B43_PHY_CCK(0x3E),sav.phy_cck_3E);
      CheckPhy("CRS0         ",(UInt16)B43_PHY_CRS0,sav.phy_crs0);

      got=RadioRead(0x0043);
      { Str255 L;L[0]=0;PCat(L,"      radio 0x43     want 0x");PCatHex(L,sav.radio_43,4);
        PCat(L,"  got 0x");PCatHex(L,got,4);
        PCat(L,(got==sav.radio_43)?"  ok":"  ✗ MISMATCH");Out(L); }
      if(got!=sav.radio_43) gBad++;
      got=RadioRead(0x007A);
      { Str255 L;L[0]=0;PCat(L,"      radio 0x7A     want 0x");PCatHex(L,sav.radio_7A,4);
        PCat(L,"  got 0x");PCatHex(L,got,4);
        PCat(L,(got==sav.radio_7A)?"  ok":"  ✗ MISMATCH");Out(L); }
      if(got!=sav.radio_7A) gBad++;

      /* ⚠ radio 0x52 is restored with maskset(0x52, 0xFF0F, sav.radio_52), and sav.radio_52 was
       * itself masked to 0x00F0 on the way in. So ONLY bits 4-7 are carried, and only those may
       * be compared. Demanding the whole word would fail for a correct port. */
      got=(UInt16)(RadioRead(0x0052)&0x00F0);
      { Str255 L;L[0]=0;PCat(L,"      radio 0x52 &0xF0  want 0x");PCatHex(L,sav.radio_52,4);
        PCat(L,"  got 0x");PCatHex(L,got,4);
        PCat(L,(got==sav.radio_52)?"  ok":"  ✗ MISMATCH");Out(L); }
      if(got!=sav.radio_52) gBad++;

      got=ssb_r16(gBus.bar0,B43_MMIO_3E2);
      { Str255 L;L[0]=0;PCat(L,"      MMIO 0x3E2    want 0x");PCatHex(L,sav.reg_3E2,4);
        PCat(L,"  got 0x");PCatHex(L,got,4);
        PCat(L,(got==sav.reg_3E2)?"  ok":"  ✗ MISMATCH");Out(L); }
      if(got!=sav.reg_3E2) gBad++;

      /* ⚠ 0x3F4 is NOT expected to equal sav.reg_3F4 exactly. restore writes it early, then its
       * LAST statement is b43_gphy_channel_switch(old_channel, 1), whose else-arm does
       * CHANNEL_EXT &= 0xF7BF. So the correct expectation is the saved value with those two bits
       * cleared. Comparing against the raw saved value would fail a correct port -- this is the
       * kind of detail that looks like a hardware problem for a day. */
      want=(UInt16)(sav.reg_3F4 & 0xF7BF);
      got=ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT);
      { Str255 L;L[0]=0;PCat(L,"      0x3F4 &0xF7BF want 0x");PCatHex(L,want,4);
        PCat(L,"  got 0x");PCatHex(L,got,4);
        PCat(L,(got==want)?"  ok":"  ✗ MISMATCH");Out(L); }
      if(got!=want) gBad++;
      Say("        (channel_switch at the end of restore clears bits 6 and 11, so the raw saved");
      Say("         value is NOT the expectation here.)");

      if(!gBad){ oracleE=1;
        Say("    ==> ORACLE E PASSED: setup saved 15 registers across three banks, two MMIO");
        Say("        registers and three radio registers, and restore brought every one back.");
      } else Say("    ✗ ORACLE E FAILED -- a save/restore pair is wrong. Do NOT proceed to 4b-ii-4b."); }

    Say("");
    /* ---- ORACLE G: the LO search. Structural, because the answer is measured. ---- */
    Say("");
    Say("  ORACLE G -- the LO I/Q search");
    Say("    This is the first result in the whole port with NO computable expected value: the");
    Say("    pair is chosen by measuring RF feedthrough and hill-climbing to a minimum. So the");
    Say("    oracle is structural, and the checks are in order of how much they prove.");
    Say("");
    Say("    (1) ⚠⚠ THE SENSOR MUST ACTUALLY MOVE. If B43_PHY_LO_LEAKAGE returned a constant,");
    Say("        the hill-climb would find no improvement in any direction, return its starting");
    Say("        (0,0), terminate cleanly, and look exactly like success. A dead sensor and a");
    Say("        perfect starting point are indistinguishable unless the SPREAD is recorded.");
    SayH("        first reading = ",(unsigned long)gFt1First,4);
    SayH("        min           = ",(unsigned long)gFt1Min,4);
    SayH("        max           = ",(unsigned long)gFt1Max,4);
    Say1("        readings      = ",(unsigned long)gFt1Count);
    Say("    (2) PER-PASS, lowest_feedth must not exceed that pass's own seed.");
    Say("        ⚠⚠ i7's CHECK (2) WAS WRONG AND THE PORT WAS FINE. It compared the LAST pass's");
    Say("           lowest_feedth against the FIRST reading of the whole search. Those are not");
    Say("           commensurable, for two reasons both visible in lo.c:");
    Say("             - lo_measure_gain_values runs at the END of every pass and changes");
    Say("               pga_gain / lna_gain, which are the arguments to lo_measure_feedthrough.");
    Say("               Readings from different passes are on different measurement scales.");
    Say("             - `if (feedth < 0x258)` RAISES the gain and re-measures, so a tiny first");
    Say("               reading is the designed trigger for a gain bump, not a low-water mark.");
    Say("               i7 read 0x0008 first, which is exactly that path firing.");
    Say("           b43 never compares them either. Within ONE pass the gains are fixed and");
    Say("           lowest_feedth starts at the seed and only ever decreases, so THAT is the");
    Say("           invariant worth asserting -- and it fails only if the port is broken.");
    { int p; for(p=0;p<gPassN&&p<8;p++){ Str255 L;L[0]=0;
        PCat(L,"        pass ");PCatDec(L,(unsigned long)p);
        PCat(L,"  seed 0x");PCatHex(L,gPassSeed[p],4);
        PCat(L,"  lowest 0x");PCatHex(L,gPassLow[p],4);
        PCat(L,"  step x");PCatDec(L,(unsigned long)gPassMul[p]);
        PCat(L,(gPassLow[p]<=gPassSeed[p])?"  ok":"  ✗ ROSE");Out(L); } }
    Say("    (3) the pair must be inside b43's OWN legal range -- b43_lo_write asserts");
    Say("        abs(i) <= 16 && abs(q) <= 16, so this bound is the reference's, not ours.");
    SayS("        i = ",gLoI);
    SayS("        q = ",gLoQ);
    Say1("        final repeat_cnt = ",(unsigned long)gOuter1);
    Say("          ⚠ NOT the number of passes. b43 increments repeat_cnt BOTH inside the loop");
    Say("            (`if (lowest_feedth <= 0x5DC) { multiplier = 1; repeat_cnt++; }`) AND in");
    Say("            the `while (++repeat_cnt < max_repeat)`, so on this card it reaches 4 after");
    Say("            only THREE body executions. The per-pass table above is the real count.");
    Say1("        accepted steps   = ",(unsigned long)gSteps1);
    Say("    (4) REPEATABILITY -- an identical search on unchanged hardware should land on the");
    Say("        same pair. This is the strongest check available for a measured quantity.");
    SayS("        run 2: i = ",gLoI2);
    SayS("        run 2: q = ",gLoQ2);
    Say1("        run 2: readings = ",(unsigned long)gFt2Count);
    SayH("        run 2: lowest   = ",(unsigned long)gLow2,4);
    { int p,improved=1;
      int moved   = (gFt1Max>gFt1Min);
      int ranged  = (gLoI<=16&&gLoI>=-16&&gLoQ<=16&&gLoQ>=-16);
      int ranged2 = (gLoI2<=16&&gLoI2>=-16&&gLoQ2<=16&&gLoQ2>=-16);
      int same    = (gLoI==gLoI2 && gLoQ==gLoQ2);
      int ran     = (gFt1Count>0 && gFt2Count>0);
      for(p=0;p<gPassN&&p<8;p++) if(gPassLow[p]>gPassSeed[p]) improved=0;
      Say("");
      Say(moved   ? "    ✓ the feedthrough reading VARIES -- the sensor is live."
                  : "    ✗ the feedthrough reading NEVER CHANGED. The search measured nothing and");
      if(!moved) Say("      its result is meaningless, however tidy it looks.");
      Say(improved? "    ✓ no pass ended above its own seed."
                  : "    ✗ a pass ended ABOVE its own seed, which the inner loop cannot do.");
      Say(ranged&&ranged2 ? "    ✓ both pairs are inside b43's own abs() <= 16 bound."
                          : "    ✗ a pair is outside abs() <= 16 -- b43_lo_write would WARN_ON it.");
      Say(same    ? "    ✓ both runs converged on the SAME pair."
                  : "      ~ the two runs differ. Not automatically a failure -- this is a noisy");
      if(!same) Say("        analogue minimum -- but compare the lowest_feedth values above.");
      if(moved&&improved&&ranged&&ranged2&&ran){ oracleG=1;
        Say("    ==> ORACLE G PASSED: the LO search runs on a live sensor, improves, stays in");
        Say("        range, and terminates inside its bounds.");
        if(!same) Say("        (Convergence differed between runs; see above.)");
        Say("    ⇒ b43_phy_initb6 IS NOW COMPLETE THROUGH b43_set_txpower_g.");
      } else Say("    ✗ ORACLE G FAILED."); }

    /* ================= b43_phy_initb6's TAIL -- the last three statements ================= */
    Say("");
    Say("=== b43_phy_initb6's TAIL -- 4b-ii-4d. set_txpower_g has now fully returned. ===");
    Say("  i1 left these unrun because reaching them would have meant jumping over");
    Say("  set_txpower_g. That is no longer true: lo_measure_restore returning IS");
    Say("  lo_g_adjust returning, which IS set_txpower_g returning. They are now simply next.");
    Say("");
    Say("  Branches for this card (radio_rev 2, analog 2, PHY type G):");
    Say("    radio_rev == 4 || 5   NO  -> b43_phy_maskset(0x5D, 0xFF80, 3) not run");
    Say("      ⚠ note that is PHY 0x5D. The next statement is RADIO 0x5D. Different registers,");
    Say("        same number, adjacent lines -- worth saying out loud before porting either.");
    Say("    radio_rev <= 2        YES -> b43_radio_write16(0x005D, 0x000D)");
    Say("    analog == 4           NO  -> the else arm, b43_phy_maskset(0x0002, 0xFFC0, 0x0004)");
    Say("    PHY type == G         YES -> b43_write16(0x03E6, 0x0)");
    { UInt16 p2Before,p2After,p3e6;
      Say("");
      Say("  -> radio 0x5D = 0x000D");
      RadioWrite(0x005D,0x000D);
      Say("     ⚠ NOT VERIFIED BY READBACK, AND THAT IS DELIBERATE. b43 NEVER READS radio 0x5D:");
      Say("       five write sites across phy_g.c, no read, no mask/maskset/set anywhere. So a");
      Say("       readback here would be an access the reference never performs -- the exact");
      Say("       move that killed h1 and h2 on radio 0x7A. Unverifiable and unperformed beats");
      Say("       verified and invented.");

      p2Before=PhyRead((UInt16)B43_PHY_CCK(0x02));
      SayH("  -> PHY 0x0002 before = ",p2Before,4);
      PhyMaskSet((UInt16)B43_PHY_CCK(0x02),0xFFC0,0x0004);
      p2After=PhyRead((UInt16)B43_PHY_CCK(0x02));
      SayH("     after maskset(0xFFC0, 0x0004) = ",p2After,4);
      Say("     (maskset READS the register by definition, so this readback is sanctioned.)");

      Say("  -> MMIO 0x3E6 = 0x0000  (this is switch_analog(dev, true) by another name)");
      ssb_w16(gBus.bar0,B43_MMIO_PHY0,0x0000);
      p3e6=ssb_r16(gBus.bar0,B43_MMIO_PHY0);
      SayH("     reads back = ",p3e6,4);
      Say("     (b43 reads 0x3E6 in three places, including a save in radio_init2050, so this");
      Say("      readback is sanctioned too.)");

      Say("");
      Say("  ORACLE H -- the tail's two verifiable results");
      { UInt16 want=(UInt16)((p2Before&0xFFC0)|0x0004);
        Str255 L;L[0]=0;
        PCat(L,"      PHY 0x0002  want 0x");PCatHex(L,want,4);
        PCat(L,"  got 0x");PCatHex(L,p2After,4);
        PCat(L,(p2After==want)?"  ok":"  ✗ MISMATCH");Out(L);
        L[0]=0;
        PCat(L,"      MMIO 0x3E6  want 0x0000  got 0x");PCatHex(L,p3e6,4);
        PCat(L,(p3e6==0)?"  ok":"  ✗ MISMATCH");Out(L);
        if(p2After==want && p3e6==0){ oracleH=1;
          Say("    ==> ORACLE H PASSED.");
          Say("    ⇒ ★★★ b43_phy_initb6 IS COMPLETE. Every statement, in order, from");
          Say("      phy_g.c:1585 to its closing brace, with every branch resolved against");
          Say("      measured identity rather than assumed.");
        } else Say("    ✗ ORACLE H FAILED."); } }

    /* ============ b43_phy_initg line 12: b43_phy_inita -> b43_wa_all ============ */
    Say("");
    Say("=== b43_phy_inita -> b43_wa_all, PREFIX -- 4b-ii-5a ===");
    Say("  initg line 12 is `if (rev >= 2 || gmode) b43_phy_inita(dev)`, and initb6 at line 10");
    Say("  has just completed, so inita is simply next. inita is:");
    Say("    if (rev >= 6) ...ENCORE...     NO at rev 2, not run");
    Say("    b43_wa_all(dev)");
    Say("    if (boardflags & PACTRL) b43_phy_maskset(OFDM(0x6E), 0xE000, 0x3CF)");
    Say("  and b43_wa_all's rev-2 arm is TWELVE workarounds in a fixed order:");
    Say("    tr_ltov, crs_ed, rssi_lt, nft, nst, msst, wrssi_offset, altagc, analog,");
    Say("    txpuoff_rxpuon, then boards_g and cpll_nonpilot.");
    Say("");
    Say("  ⏹ THIS INCREMENT RUNS THE FIRST THREE AND STOPS. Ten of the twelve write PHY TABLES");
    Say("    through b43_ofdmtab_write16, which is an indirect path this port has never used and");
    Say("    which carries a STATEFUL ADDRESS CACHE. That is the 4b-ii-1 situation exactly: a");
    Say("    wrong accessor does not fault, it silently writes the wrong table offset and");
    Say("    surfaces much later as a PHY that will not receive. Prove the accessor first.");
    Say("");
    Say("  ⚠ THE THREE ARE NOT CHERRY-PICKED. tr_ltov, crs_ed and rssi_lt are wa_all's first");
    Say("    three IN ORDER. Taking tr_ltov and rssi_lt alone -- the two that touch the table");
    Say("    accessors -- would skip crs_ed from the middle, which is the forbidden move.");
    { UInt16 origRssi[64]; UInt16 origTr; int k,bad=0,firstBad=-1;
      Say("");
      Say("  -> saving the 64 RSSI table entries and GTAB ORIGTR before touching them");
      origTr=GTabRead((UInt16)B43_GTAB_ORIGTR,0);
      SayH("     GTAB ORIGTR (0xBA98) before = ",origTr,4);
      gOtabDir=AP_OTAB_DIR_UNKNOWN;
      for(k=0;k<64;k++) origRssi[k]=OfdmTabRead16((UInt16)B43_OFDMTAB_RSSI,(UInt16)k);
      SayH("     RSSI[0] before = ",origRssi[0],4);
      SayH("     RSSI[63] before = ",origRssi[63],4);

      Say("");
      Say("  -> wa_all #1: b43_wa_tr_ltov -- gtab_write(ORIGTR, 0, 0x7654)");
      GTabWrite((UInt16)B43_GTAB_ORIGTR,0,0x7654);

      Say("  -> wa_all #2: b43_wa_crs_ed -- the rev == 2 arm");
      PhyWrite((UInt16)B43_PHY_CRSTHRES1,0x1861);
      PhyWrite((UInt16)B43_PHY_CRSTHRES2,0x0271);
      PhySet((UInt16)B43_PHY_ANTDWELL,0x0800);

      Say("  -> wa_all #3: b43_wa_rssi_lt -- 64 writes, value == offset (an IDENTITY table)");
      Say("     ⚠ THE FIRST WRITE SETS THE ADDRESS; THE OTHER 63 RELY ON THE HARDWARE");
      Say("       AUTO-INCREMENTING. That is what makes this workaround the right accessor test:");
      Say("       it is not a test pattern at all, it is what b43 writes, and its content is the");
      Say("       identity function, so a cache that drifts by even one slot is visible.");
      for(k=0;k<64;k++) OfdmTabWrite16((UInt16)B43_OFDMTAB_RSSI,(UInt16)k,(UInt16)k);

      Say("");
      Say("  ORACLE I -- the PHY table accessors");
      Say("    Read back in REVERSE order, 63 down to 0. Reverse reads are never sequential, so");
      Say("    the read path takes the address-writing branch EVERY time. A write-cache bug");
      Say("    therefore cannot be masked by a symmetric read-cache bug -- which it would be if");
      Say("    both were read forwards.");
      gOtabDir=AP_OTAB_DIR_UNKNOWN;
      for(k=63;k>=0;k--){
        UInt16 got=OfdmTabRead16((UInt16)B43_OFDMTAB_RSSI,(UInt16)k);
        if(got!=(UInt16)k){ bad++; if(firstBad<0) firstBad=k; } }
      Say1("    mismatches out of 64 = ",(unsigned long)bad);
      if(bad) Say1("    first bad offset (scanning down) = ",(unsigned long)firstBad);
      { UInt16 tr=GTabRead((UInt16)B43_GTAB_ORIGTR,0);
        Str255 L;L[0]=0;
        PCat(L,"    GTAB ORIGTR  want 0x7654  got 0x");PCatHex(L,tr,4);
        PCat(L,(tr==0x7654)?"  ok":"  ✗ MISMATCH");Out(L);
        CheckPhy("CRSTHRES1    ",(UInt16)B43_PHY_CRSTHRES1,0x1861);
        CheckPhy("CRSTHRES2    ",(UInt16)B43_PHY_CRSTHRES2,0x0271);
        if(bad==0 && tr==0x7654){ oracleI=1;
          Say("    ==> ORACLE I PASSED: 64 sequential writes through the auto-incrementing OFDM");
          Say("        table path landed in the right slots, and the G table round-trips.");
          Say("        ⇒ The table accessor is proven, so the remaining nine workarounds can be");
          Say("          written against a primitive that is known good.");
        } else Say("    ✗ ORACLE I FAILED."); }
      /* ===================== 4b-ii-5b: wa_all #4 .. #12, then inita's tail ===================== */
      Say("");
      Say("=== wa_all #4..#12 + inita's tail -- 4b-ii-5b ===");
      Say("  The accessor is proven, so these are written against a known-good primitive.");
      gMsBad=0; gMsN=0;

      Say("  #4 b43_wa_nft   -- rev != 1 -> noiseg2 into AGC2");
      for(k=0;k<(int)kNoiseG2N;k++)
        OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC2,(UInt16)k,kTabNoiseG2[k]);

      Say("  #5 b43_wa_nst   -- rev < 6 -> noisescaleg1 into NOISESCALE");
      for(k=0;k<(int)kNoiseScaleG1N;k++)
        OfdmTabWrite16((UInt16)B43_OFDMTAB_NOISESCALE,(UInt16)k,kTabNoiseScaleG1[k]);

      Say("  #6 b43_wa_msst  -- type G -> sigmasqr2 into MINSIGSQ");
      for(k=0;k<(int)kSigmaSqr2N;k++)
        OfdmTabWrite16((UInt16)B43_OFDMTAB_MINSIGSQ,(UInt16)k,kTabSigmaSqr2[k]);

      Say("  #7 b43_wa_wrssi_offset -- rev != 1 -> 32 x 0x0820 into WRSSI");
      for(k=0;k<32;k++)
        OfdmTabWrite16((UInt16)B43_OFDMTAB_WRSSI,(UInt16)k,0x0820);

      Say("  #8 b43_wa_altagc -- the rev-2 arm, every maskset self-verified");
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC1,0,254);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC1,1,13);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC1,2,19);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC1,3,25);
      MaskSetV("CCKSHIFTBITS_WA",(UInt16)B43_PHY_CCKSHIFTBITS_WA,0x00FF,0x5700);
      MaskSetV("OFDM 0x1A a",(UInt16)B43_PHY_OFDM(0x1A),0xFF80,0x000F);  /* ~0x007F */
      MaskSetV("OFDM 0x1A b",(UInt16)B43_PHY_OFDM(0x1A),0xC07F,0x2B80);  /* ~0x3F80 */
      MaskSetV("ANTWRSETT",(UInt16)B43_PHY_ANTWRSETT,0xF0FF,0x0300);
      RadioSet(0x007A,0x0008);
      MaskSetV("N1P1GAIN a",(UInt16)B43_PHY_N1P1GAIN,0xFFF0,0x0008);     /* ~0x000F */
      MaskSetV("P1P2GAIN",  (UInt16)B43_PHY_P1P2GAIN,0xF0FF,0x0600);     /* ~0x0F00 */
      MaskSetV("N1N2GAIN",  (UInt16)B43_PHY_N1N2GAIN,0xF0FF,0x0700);     /* ~0x0F00 */
      MaskSetV("N1P1GAIN b",(UInt16)B43_PHY_N1P1GAIN,0xF0FF,0x0100);     /* ~0x0F00 */
      MaskSetV("OFDM 0x88 a",(UInt16)B43_PHY_OFDM(0x88),0xFF00,0x001C);
      MaskSetV("OFDM 0x88 b",(UInt16)B43_PHY_OFDM(0x88),0xC0FF,0x0200);
      MaskSetV("OFDM 0x96 a",(UInt16)B43_PHY_OFDM(0x96),0xFF00,0x001C);
      MaskSetV("OFDM 0x89 a",(UInt16)B43_PHY_OFDM(0x89),0xFF00,0x0020);
      MaskSetV("OFDM 0x89 b",(UInt16)B43_PHY_OFDM(0x89),0xC0FF,0x0200);
      MaskSetV("OFDM 0x82",  (UInt16)B43_PHY_OFDM(0x82),0xFF00,0x002E);
      MaskSetV("OFDM 0x96 b",(UInt16)B43_PHY_OFDM(0x96),0x00FF,0x1A00);
      MaskSetV("OFDM 0x81 a",(UInt16)B43_PHY_OFDM(0x81),0xFF00,0x0028);
      MaskSetV("OFDM 0x81 b",(UInt16)B43_PHY_OFDM(0x81),0x00FF,0x2C00);
      MaskSetV("OFDM 0x1B",  (UInt16)B43_PHY_OFDM(0x1B),0xFFE1,0x0000);  /* b43_phy_mask */
      PhyWrite((UInt16)B43_PHY_OFDM(0x1F),0x287A);
      MaskSetV("LPFGAINCTL",(UInt16)B43_PHY_LPFGAINCTL,0xFFF0,0x0004);
      /* rev >= 6 block: not run */
      MaskSetV("DIVSRCHIDX",(UInt16)B43_PHY_DIVSRCHIDX,0x8080,0x7874);
      PhyWrite((UInt16)B43_PHY_OFDM(0x8E),0x1C00);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC3,0,0);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC3,1,7);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC3,2,16);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_AGC3,3,28);
      (void)PhyRead((UInt16)B43_PHY_VERSION_OFDM);   /* b43's dummy read */

      Say("  #9 b43_wa_analog -- branches on a RUNTIME value, so it is read, not assumed");
      { UInt16 ofdmrev=(UInt16)(PhyRead((UInt16)B43_PHY_VERSION_OFDM)&B43_PHYVER_VERSION);
        SayH("     VERSION_OFDM & 0x00FF = ",ofdmrev,4);
        if(ofdmrev>2){ Say("     ofdmrev > 2 -> PWRDOWN = 0x1000");
          PhyWrite((UInt16)B43_PHY_PWRDOWN,0x1000); }
        else { Say("     ofdmrev <= 2 -> three DAC table writes");
          OfdmTabWrite16((UInt16)B43_OFDMTAB_DAC,3,0x1044);
          OfdmTabWrite16((UInt16)B43_OFDMTAB_DAC,4,0x7201);
          OfdmTabWrite16((UInt16)B43_OFDMTAB_DAC,6,0x0040); }
        gOfdmRev=ofdmrev; }

      Say("  #10 b43_wa_txpuoff_rxpuon -- UNKNOWN_0F[2]=15, [3]=20");
      OfdmTabWrite16((UInt16)B43_OFDMTAB_UNKNOWN_0F,2,15);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_UNKNOWN_0F,3,20);

      Say("  #11 b43_wa_boards_g -- board vendor 0x106B is not BCM, so the outer arm is taken;");
      Say("      rev 2 -> GAINX[1]=0x0002, GAINX[2]=0x0001. rev >= 7 EXTLNA block not run.");
      OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,1,0x0002);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_GAINX,2,0x0001);
      Say("      boardflags 0x000A & FEM 0x0800 = 0, so the FEM pair is NOT written.");

      Say("  #12 b43_wa_cpll_nonpilot -- UNKNOWN_11[0]=0, [1]=0");
      OfdmTabWrite16((UInt16)B43_OFDMTAB_UNKNOWN_11,0,0);
      OfdmTabWrite16((UInt16)B43_OFDMTAB_UNKNOWN_11,1,0);

      Say("  inita tail: boardflags & PACTRL is SET -> maskset(OFDM(0x6E), 0xE000, 0x3CF)");
      MaskSetV("OFDM 0x6E",(UInt16)B43_PHY_OFDM(0x6E),0xE000,0x03CF);

      /* ---- ORACLE J ---- */
      Say("");
      Say("  ORACLE J -- the nine workarounds");
      Say("    (a) every phy_maskset self-verified against a want computed from the value read");
      Say("        immediately before it. altagc alone is twenty-odd hand-transcribed masks,");
      Say("        which is precisely the list where one wrong nibble is silent.");
      Say1("        masksets applied  = ",(unsigned long)gMsN);
      Say1("        masksets WRONG    = ",(unsigned long)gMsBad);
      Say("    (b) table contents spot-checked against the source arrays, including two");
      Say("        deliberately awkward entries: sigmasqr2[26] is 0x0000 in an otherwise smooth");
      Say("        curve, and noisescaleg1[13] is 0x1400 among 0x1414s. A transcription slip or a");
      Say("        one-slot cache drift shows up at exactly those two.");
      { int tb=0;
        UInt16 v;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_MINSIGSQ,26);
        { Str255 L;L[0]=0;PCat(L,"        sigmasqr2[26]    want 0x0000  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x0000)?"  ok":"  ✗");Out(L); } if(v!=0x0000) tb++;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_MINSIGSQ,52);
        { Str255 L;L[0]=0;PCat(L,"        sigmasqr2[52]    want 0x00DE  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x00DE)?"  ok":"  ✗");Out(L); } if(v!=0x00DE) tb++;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_NOISESCALE,13);
        { Str255 L;L[0]=0;PCat(L,"        noisescaleg1[13] want 0x1400  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x1400)?"  ok":"  ✗");Out(L); } if(v!=0x1400) tb++;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_NOISESCALE,26);
        { Str255 L;L[0]=0;PCat(L,"        noisescaleg1[26] want 0x0077  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x0077)?"  ok":"  ✗");Out(L); } if(v!=0x0077) tb++;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_AGC2,0);
        { Str255 L;L[0]=0;PCat(L,"        noiseg2[0]       want 0x5484  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x5484)?"  ok":"  ✗");Out(L); } if(v!=0x5484) tb++;
        gOtabDir=AP_OTAB_DIR_UNKNOWN;
        v=OfdmTabRead16((UInt16)B43_OFDMTAB_WRSSI,31);
        { Str255 L;L[0]=0;PCat(L,"        wrssi[31]        want 0x0820  got 0x");PCatHex(L,v,4);
          PCat(L,(v==0x0820)?"  ok":"  ✗");Out(L); } if(v!=0x0820) tb++;
        if(gMsBad==0 && tb==0){ oracleJ=1;
          Say("    ==> ORACLE J PASSED: all nine workarounds applied, every maskset landed its");
          Say("        computed value, and the transcribed tables read back correctly.");
          Say("        ⇒ b43_wa_all IS COMPLETE, and so is b43_phy_inita.");
        } else Say("    ✗ ORACLE J FAILED."); }

      /* ============ initg lines 15..45: the run-up, then calc_loopback_gain ============ */
      Say("");
      Say("=== b43_phy_initg lines 15..45 -- 4b-ii-6 ===");
      Say("  inita has returned, so initg continues. These statements sit BETWEEN inita and");
      Say("  calc_loopback_gain and are run in order -- reaching line 45 without them would be");
      Say("  the forbidden move, and the roadmap's 'calc_loopback_gain is next' was shorthand,");
      Say("  not a licence to jump.");
      gMsBad=0; gMsN=0;
      Say("    rev >= 2 -> ANALOGOVER = 0, ANALOGOVERVAL = 0");
      PhyWrite((UInt16)B43_PHY_ANALOGOVER,0);
      PhyWrite((UInt16)B43_PHY_ANALOGOVERVAL,0);
      Say("    rev == 2 -> RFOVER = 0, PGACTL = 0x00C0");
      PhyWrite((UInt16)B43_PHY_RFOVER,0);
      PhyWrite((UInt16)B43_PHY_PGACTL,0x00C0);
      Say("    rev > 5  -> NOT run");
      { UInt16 t=(UInt16)(PhyRead((UInt16)B43_PHY_VERSION_OFDM)&B43_PHYVER_VERSION);
        SayH("    gmode || rev >= 2 -> VERSION_OFDM & 0x00FF = ",t,4);
        if(t==3||t==5){ Say("      == 3 or 5 -> OFDM(0xC2) = 0x1816, OFDM(0xC3) = 0x8006");
          PhyWrite((UInt16)B43_PHY_OFDM(0xC2),0x1816);
          PhyWrite((UInt16)B43_PHY_OFDM(0xC3),0x8006); }
        else Say("      neither 3 nor 5 -> the 0xC2/0xC3 pair is NOT written");
        if(t==5){ MaskSetV("OFDM 0xCC",(UInt16)B43_PHY_OFDM(0xCC),0x00FF,0x1F00); }
        else Say("      != 5 -> the OFDM(0xCC) maskset is NOT run"); }
      Say("    (rev <= 2 && gmode) || rev >= 2 -> OFDM(0x7E) = 0x0078");
      PhyWrite((UInt16)B43_PHY_OFDM(0x7E),0x0078);
      Say("    radio_rev == 8 -> NOT run (this radio is rev 2)");

      Say("");
      Say("  -> has_loopback_gain is TRUE -> b43_calc_loopback_gain(dev)");
      Say("     ⚠⚠ THREE OF ITS BRANCHES CARRY THE COMMENT \"Not in specs, but needed to prevent");
      Say("        PPC machine check\", all guarded by rev != 1. This PHY is rev 2, so all three");
      Say("        RUN -- and we are on the very architecture that comment is about. Dropping any");
      Say("        of them as 'not in the spec' would reproduce a fault someone already paid for.");
      Say("     ⚠ It sweeps radio 0x43 x the RFOVERVAL PGA field (up to 9 x 16 = 144 points),");
      Say("       reading LO_LEAKAGE at each, then restores 15 PHY registers, 3 radio registers");
      Say("       and the baseband attenuation.");
      CalcLoopbackGain();

      Say("");
      Say("  ORACLE K -- the loopback gain measurement");
      Say("    Measured, not computed -- so structural again, on the pattern Oracle G settled.");
      Say1("    (1) LO_LEAKAGE readings taken = ",(unsigned long)gLlCount);
      SayH("        min = ",(unsigned long)gLlMin,4);
      SayH("        max = ",(unsigned long)gLlMax,4);
      Say("        A sensor that never moves would let the sweep run to its bound and produce a");
      Say("        tidy, meaningless answer -- the same trap Oracle G guards against.");
      Say1("    (2) sweep 1 exit: outer i = ",(unsigned long)gLbOuter);
      Say1("                      inner j = ",(unsigned long)gLbInner);
      Say("        bounded by construction at 9 x 16; both indices below their bounds means it");
      Say("        broke on the 0x0DFC threshold rather than exhausting the sweep.");
      SayH("    (3) trsw_rx = ",(unsigned long)gLbTrsw,4);
      if(gLbTrswUnderflow){
        Say("        ⚠⚠ trsw_rx UNDERFLOWED. b43 declares it u16 and subtracts 3 per iteration");
        Say("           from 0x1B, so more than nine iterations of sweep 2 wraps it. This is");
        Say("           ported faithfully and REPORTED rather than silently corrected: if it");
        Say("           fires, b43 has the same behaviour on this hardware and the right move is");
        Say("           to understand why sweep 2 ran that long, not to patch the arithmetic."); }
      else Say("        no underflow (sweep 2 ran nine iterations or fewer, or did not run)");
      SaySigned16("    (4) max_lb_gain  = ",gLbMaxGain);
      Say("        = ((inner * 6) - (outer * 4)) - 11, so it CAN legitimately be negative.");
      SaySigned16("        trsw_rx_gain = ",gTrswRxGain);
      Say1("    (5) restore mismatches = ",(unsigned long)gLbRestoreBad);
      Say("        15 PHY registers, not 16: LO_LEAKAGE is saved by b43 and never written back,");
      Say("        because it is a measurement register. Checking it would fail a correct port.");
      Say("        Plus 3 radio registers and the baseband attenuation.");
      { int moved=(gLlMax>gLlMin);
        int bounded=(gLbOuter<=9 && gLbInner<=16);
        int restored=(gLbRestoreBad==0);
        int ran=(gLlCount>0);
        Say("");
        Say(moved   ? "    ✓ LO_LEAKAGE varies -- the sensor is live."
                    : "    ✗ LO_LEAKAGE never changed; the sweep measured nothing.");
        Say(bounded ? "    ✓ both sweep indices are inside their bounds."
                    : "    ✗ a sweep index exceeded its bound.");
        Say(restored? "    ✓ every saved register came back."
                    : "    ✗ a saved register did not come back -- PHY state is now corrupt.");
        if(moved&&bounded&&restored&&ran){ oracleK=1;
          Say("    ==> ORACLE K PASSED.");
          Say("    ⇒ max_lb_gain and trsw_rx_gain are REAL for the first time. Every increment");
          Say("      before this read them as 0, correctly, because initb6 is initg line 10 and");
          Say("      this is line 45. Any LO search re-run from here will produce a DIFFERENT");
          Say("      I/Q pair than 4b-ii-4c's -- that is the inputs changing, not a regression.");
        } else Say("    ✗ ORACLE K FAILED."); }

      /* ============ initg line 49: b43_radio_init2050 ============ */
      Say("");
      Say("=== b43_radio_init2050 -- 4b-ii-7, initg line 49 ===");
      Say("  Nothing sits between calc_loopback_gain and this, so it is directly next.");
      Say("  initg: if (radio_rev != 8) { if (initval == 0xFFFF) initval = radio_init2050();");
      Say("                               else radio_write16(0x0078, initval); }");
      Say("  initval is 0xFFFF from prepare_structs, so the FIRST arm runs.");
      Say("");
      Say("  ⚠ THIS IS WHERE max_lb_gain FEEDS BACK IN. radio2050_rfover_val consumes the value");
      Say("    4b-ii-6 measured to pick an extlna code and a gain index, and those become the");
      Say("    RFOVERVAL words the two sweeps write hundreds of times. The chain is computable,");
      Say("    so it is checked in software BEFORE any of it touches the radio.");
      { UInt16 e0,e1,e2,e3;
        e0=Radio2050RfoverVal((UInt16)B43_PHY_RFOVER,0);
        e1=Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(0,1,1));
        e2=Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(0,0,1));
        e3=Radio2050RfoverVal((UInt16)B43_PHY_RFOVERVAL,LPD(1,0,0));
        gRfoverA=e0; gRfoverValA=e1; gRfoverValB=e2; gRfoverValC=e3;
        SaySigned16("    max_lb_gain in  = ",gLbMaxGain);
        SayH("    RFOVER              = ",e0,4);
        SayH("    RFOVERVAL LPD(0,1,1) = ",e1,4);
        SayH("    RFOVERVAL LPD(0,0,1) = ",e2,4);
        SayH("    RFOVERVAL LPD(1,0,0) = ",e3,4);
        /* ⚠ THE DERIVATION IS RECOMPUTED AND PRINTED FROM THE VALUE ACTUALLY MEASURED, not
         * narrated from a previous run. j4 caught this the hard way: the text here used to
         * hardcode j3's max_lb_gain of 19 while the card had just measured 25, so the prose
         * contradicted the numbers directly beneath it. Explanatory text that disagrees with
         * the data is worse than no text -- it tells a future reader a confident wrong story. */
        { SInt32 m=(SInt32)gLbMaxGain; UInt16 e; int ii;
          m+=0x26;
          SaySigned16("    derivation: max_lb_gain = ",gLbMaxGain);
          Say1       ("      + 0x26            = ",(unsigned long)m);
          if(m>=0x46)      { e=0x3000; m-=0x46; }
          else if(m>=0x3A) { e=0x1000; m-=0x3A; }
          else if(m>=0x2E) { e=0x2000; m-=0x2E; }
          else             { e=0;      m-=0x10; }
          SayH       ("      -> extlna         = ",e,4);
          Say1       ("      -> remainder      = ",(unsigned long)m);
          for(ii=0;ii<16;ii++){ m-=(SInt32)(ii*6); if(m<6) break; }
          Say1       ("      -> gain index i   = ",(unsigned long)ii);
          SayH       ("      -> extlna|(i<<8)  = ",(unsigned long)(e|(UInt16)(ii<<8)),4); }
        Say ("    ⚠ that loop subtracts a GROWING i*6 each pass -- it is NOT a division.");
        Say ("      Transcribing it as one lands on a different gain index and every RFOVERVAL");
        Say ("      write below would be quietly wrong.");
        Say ("    ⚠ max_lb_gain IS NOT STABLE RUN TO RUN. j3 measured 19, j4 measured 25 -- one");
        Say ("      step of the calc_loopback_gain sweep, whose LO_LEAKAGE crossing is steep. It");
        Say ("      changes extlna and the gain index, so the RFOVERVAL words differ between");
        Say ("      runs. That is faithful: b43 measures once per init and uses what it gets."); }
      Say("");
      Say("  ⚠ Two sweeps: 16 iterations, then up to 16 x 16, each accumulating LO_LEAKAGE.");
      Say("    radio 0x60 is FIRST TOUCH (the core calibration value) and radio 0x78 is written");
      Say("    across a bit-reversed sequence.");
      gInitval=RadioInit2050();

      Say("");
      Say("  ORACLE L -- radio_init2050");
      SayH("    radio 0x60 (first touch) = ",gRadio60,4);
      SayH("    rcc (core calibration)   = ",gRcc,4);
      Say1("    tmp1 (sweep 1, +1 >>9)   = ",gTmp1);
      Say1("    tmp2 (sweep 2, +1 >>8)   = ",gTmp2);
      Say1("    sweep 2 exit i           = ",(unsigned long)gLoop2Iters);
      SayH("    last radio78 written     = ",gRadio78,4);
      SayH("    ==> initval              = ",gInitval,4);
      Say(gInitvalFromRadio78
          ? "    (i > 15, so initval is radio78 -- sweep 2 never found tmp1 < tmp2)"
          : "    (i <= 15, so initval is rcc -- sweep 2 broke early)");
      Say1("    restore mismatches       = ",(unsigned long)gI2050RestoreBad);
      Say("      12 PHY + 3 radio. 0x3F4 is deliberately NOT checked: synth_pu_workaround runs");
      Say("      AFTER it is written back, so a naive check would fail a correct port -- the same");
      Say("      trap as Oracle E's 0x3F4.");
      { int rfok=(gRfoverA==0x01B3 && gRfoverValA==0x0F92);
        int restored=(gI2050RestoreBad==0);
        int bounded=(gLoop2Iters<=16);
        Say("");
        Say(rfok     ? "    ✓ rfover_val returned its computed values for the fixed cases."
                     : "    ✗ rfover_val did NOT return 0x01B3 / 0x0F92 -- the chain is wrong.");
        Say(bounded  ? "    ✓ sweep 2 stayed inside its bound."
                     : "    ✗ sweep 2 exceeded its bound.");
        Say(restored ? "    ✓ every saved register came back."
                     : "    ✗ a saved register did not come back.");
        if(rfok&&bounded&&restored){ oracleL=1;
          Say("    ==> ORACLE L PASSED.");
          Say("    ⇒ initg is ported through line 49. Only lo_g_init and initg's tail remain.");
        } else Say("    ✗ ORACLE L FAILED."); }

      /* ============ initg lines 52..71: lo_g_init through the lo_g_adjust block ============ */
      Say("");
      Say("=== b43_phy_initg's tail -- 4b-ii-8, lines 52..71 ===");
      Say("  -> line 52: b43_lo_g_init(dev)");
      Say("     ⚠ A COMPLETE NO-OP ON THIS CARD, and that is worth stating rather than quietly");
      Say("       skipping. Its entire body is `if (b43_has_hardware_pctl(dev)) { ... }`, and");
      Say("       b43_gphy_op_supports_hwpctl returns (phy->rev >= 6). This PHY is rev 2, so the");
      Say("       guard is false no matter what the module parameter says -- exactly as it was");
      Say("       for the hwpctl blocks in lo_measure_setup and lo_measure_restore. Nothing runs.");
      Say("  -> has_tx_magnification is FALSE -> radio_maskset(0x52, 0xFFF0, lo->tx_bias)");
      Say1("     lo->tx_bias = ",(unsigned long)0);
      Say("     (set to 0 by lo_measure_txctl_values' else arm back in 4b-ii-5b)");
      RadioMaskSet(0x0052,0xFFF0,0);
      Say("  -> rev >= 6 -> NOT run (no CCK(0x36) maskset)");
      Say("  -> boardflags & PACTRL is SET -> CCK(0x2E) = 0x8075  (not 0x807F)");
      PhyWrite((UInt16)B43_PHY_CCK(0x2E),0x8075);
      Say("  -> rev >= 2 -> CCK(0x2F) = 0x0202  (not 0x0101)");
      PhyWrite((UInt16)B43_PHY_CCK(0x2F),0x0202);

      Say("");
      Say("  -> gmode || rev >= 2 -> b43_lo_g_adjust(dev), then LO_MASK = 0x8078");
      Say("     ★ THIS IS THE CROSS-CHECK FLAGGED IN 4b-ii-6. lo_g_adjust re-runs the whole LO");
      Say("       calibration, and this time max_lb_gain and trsw_rx_gain are REAL rather than 0.");
      Say("       4b-ii-4c ran the identical search with both at 0 and converged twice on (0,-1).");
      Say("       A DIFFERENT pair here is the expected result -- the inputs changed. An identical");
      Say("       pair would be the surprising outcome, and would suggest the gains are not");
      Say("       actually reaching the search.");
      SaySigned16("       max_lb_gain now  = ",gMaxLbGain);
      SaySigned16("       trsw_rx_gain now = ",gTrswRxGain);
      { LoCtl ctl; int outer=0,steps=0;
        gFtFirst=0;gFtMin=0xFFFF;gFtMax=0;gFtCount=0;gPassN=0;
        LoGAdjust(&ctl,&outer,&steps);
        gLoI3=ctl.i; gLoQ3=ctl.q; gFt3Count=gFtCount; gLow3=gLowestFeedth;
        PhyWrite((UInt16)B43_PHY_LO_MASK,0x8078);

        Say("");
        Say("  ORACLE M -- initg's tail and the re-calibration");
        SayS("    lo_g_adjust pair: i = ",gLoI3);
        SayS("                      q = ",gLoQ3);
        Say1("    feedthrough readings = ",(unsigned long)gFt3Count);
        SayH("    lowest_feedth        = ",(unsigned long)gLow3,4);
        Say("");
        Say("    4b-ii-4c, with max_lb_gain = 0 and trsw_rx_gain = 0:");
        SayS("      i = ",gLoI);
        SayS("      q = ",gLoQ);
        { int changed=(gLoI3!=gLoI || gLoQ3!=gLoQ);
          int ranged=(gLoI3<=16&&gLoI3>=-16&&gLoQ3<=16&&gLoQ3>=-16);
          int moved=(gFtMax>gFtMin);
          int p,seedOk=1;
          for(p=0;p<gPassN&&p<8;p++) if(gPassLow[p]>gPassSeed[p]) seedOk=0;
          Say(moved  ? "    ✓ the feedthrough sensor is live."
                     : "    ✗ the feedthrough reading never changed.");
          Say(seedOk ? "    ✓ no pass ended above its own seed."
                     : "    ✗ a pass ended above its own seed.");
          Say(ranged ? "    ✓ the pair is inside b43's abs() <= 16 bound."
                     : "    ✗ the pair is outside abs() <= 16.");
          Say(changed? "    ✓ the pair MOVED now that the gains are real -- the cross-check holds:"
                     : "    ~ the pair is UNCHANGED from 4b-ii-4c.");
          if(changed) Say("      max_lb_gain and trsw_rx_gain are demonstrably reaching the search.");
          else { Say("      That is not automatically wrong -- the minimum may genuinely sit in the");
                 Say("      same place -- but it is the outcome that deserves a second look, since");
                 Say("      it is also what a gain value that never arrives would produce."); }
          if(moved&&seedOk&&ranged){ oracleM=1;
            Say("    ==> ORACLE M PASSED.");
            Say("    ⇒ initg is ported through line 71. Only calc_nrssi_slope, init_pctl and the");
            Say("      chip_pkg tail remain -- that is 4b-ii-9.");
          } else Say("    ✗ ORACLE M FAILED."); } }

      /* ============ initg lines 76..89: the NRSSI arm ============ */
      Say("");
      Say("=== b43_calc_nrssi_slope -- 4b-ii-9a ===");
      Say("  initg: if (!(boardflags & RSSI)) { nrssi_hw_update(0xFFFF); calc_nrssi_threshold(); }");
      Say("         else if (gmode || rev >= 2) { if (nrssi[0] == -1000) calc_nrssi_slope();");
      Say("                                       else calc_nrssi_threshold(); }");
      Say("  boardflags 0x000A has RSSI (0x0008) SET, so the FIRST arm does not run.");
      Say("  nrssi[] is -1000 from prepare_structs, so calc_nrssi_slope runs, not threshold.");
      Say("");
      Say("  ⚠ A THIRD INDIRECT TABLE PATH. The NRSSI lookup table uses PHY 0x0803 (control) and");
      Say("    0x0804 (data), distinct from the OFDM and G tables and with NO auto-increment");
      Say("    cache, so every access writes its own address.");
      Say("  ⚠ TWO REGISTERS HAVE TWO NAMES EACH IN b43's HEADERS: B43_PHY_G_CRS is 0x0429,");
      Say("    which IS B43_PHY_CRS0; B43_PHY_G_LO_CONTROL is 0x0810, which IS B43_PHY_LO_CTL.");
      Say("    Treating either pair as distinct registers would produce a silent double-write.");
      CalcNrssiSlope();

      Say("");
      Say("  ORACLE N -- the NRSSI slope");
      SaySigned16("    nrssi[0] = ",gNrssi0);
      SaySigned16("    nrssi[1] = ",gNrssi1);
      Say("      ⚠ the pair is stored SWAPPED and only when the first reading is >= -4:");
      Say("        nrssi[0] = nrssi1, nrssi[1] = nrssi0. Backwards, every threshold computed");
      Say("        from them inverts.");
      { Str255 L;L[0]=0;PCat(L,"    nrssislope = ");
        if(gNrssiSlope<0){PCat(L,"-");PCatDec(L,(unsigned long)(-gNrssiSlope));}
        else PCatDec(L,(unsigned long)gNrssiSlope);
        PCat(L,"  (0x00400000 / (n0 - n1), or 0x00010000 if equal)");Out(L); }
      SaySigned16("    threshold a = ",(SInt16)gThrA);
      SaySigned16("    threshold b = ",(SInt16)gThrB);
      SayH       ("    PHY 0x048A  = ",(unsigned long)gThr048A,4);
      Say1("    restore mismatches = ",(unsigned long)gNrssiRestoreBad);
      Say("      4 PHY + 3 radio + 2 MMIO. CHANNEL_EXT is NOT checked -- synth_pu_workaround");
      Say("      runs after it is written back, the same trap as Oracle E's and L's 0x3F4.");
      Say("    software nrssi_lt[] samples (this table is memory, not hardware):");
      { Str255 L;L[0]=0;PCat(L,"      [0]=");PCatDec(L,(unsigned long)gNrssiLt[0]);
        PCat(L,"  [31]=");PCatDec(L,(unsigned long)gNrssiLt[31]);
        PCat(L,"  [63]=");PCatDec(L,(unsigned long)gNrssiLt[63]);Out(L); }
      { int restored=(gNrssiRestoreBad==0);
        int ltOk=1,z,up=0,down=0,mono;
        int slopeOk=(gNrssiSlope!=0);
        int thrOk=(gThrA>=-31&&gThrA<=31&&gThrB>=-31&&gThrB<=31);
        for(z=0;z<64;z++) if(gNrssiLt[z]<0||gNrssiLt[z]>0x3F) ltOk=0;
        /* MONOTONICITY -- the discriminating check. nrssi_lt[i] is LINEAR in i before the clamp
         * ((i - delta) * slope / 0x10000 + 0x3A), and clamping preserves monotonicity, so the
         * table must run one way only. A transcription slip in that formula -- a wrong shift, a
         * sign, delta computed from the wrong endpoint -- breaks it. */
        for(z=1;z<64;z++){ if(gNrssiLt[z]>gNrssiLt[z-1]) up++;
                           else if(gNrssiLt[z]<gNrssiLt[z-1]) down++; }
        mono=(up==0||down==0);
        Say("");
        Say(restored? "    ✓ every saved register came back."
                    : "    ✗ a saved register did not come back.");
        Say(mono    ? "    ✓ nrssi_lt is MONOTONIC -- the linear formula holds across all 64."
                    : "    ✗ nrssi_lt changes direction; the formula cannot do that.");
        Say1("        rises = ",(unsigned long)up);
        Say1("        falls = ",(unsigned long)down);
        Say(slopeOk ? "    ✓ nrssislope is non-zero."
                    : "    ✗ nrssislope is zero, which the formula cannot produce.");
        Say("    ⚠ THE NEXT TWO CHECKS CANNOT FAIL, and saying so is more useful than letting");
        Say("      them look like evidence. Both ranges are guaranteed by clamp_val itself, so");
        Say("      they confirm the clamps were ported, nothing more. Modelled offline across all");
        Say("      4096 (n0,n1) combinations: zero escapes either bound. The discriminating");
        Say("      checks here are the restore and the monotonicity.");
        Say(thrOk   ? "    ✓ both thresholds are inside b43's own clamp of -31..31."
                    : "    ✗ a threshold escaped the clamp -- impossible if clamp_val is right.");
        Say(ltOk    ? "    ✓ all 64 nrssi_lt entries are inside the 0..0x3F clamp."
                    : "    ✗ an nrssi_lt entry escaped the 0..0x3F clamp.");
        if(restored&&slopeOk&&thrOk&&ltOk&&mono){ oracleN=1;
          Say("    ==> ORACLE N PASSED.");
          Say("    ⇒ Only b43_phy_init_pctl and the chip_pkg tail remain. That is 4b-ii-9b, and");
          Say("      it finishes Stage 4.");
        } else Say("    ✗ ORACLE N FAILED."); }

      /* ============ initg's last statements: init_pctl and the chip_pkg tail ============ */
      Say("");
      Say("=== b43_phy_init_pctl and initg's final branch -- 4b-ii-9b ===");
      Say("  -> radio_rev == 8 -> EXTG(0x05) = 0x3230   NOT run (this radio is rev 2)");
      Say("  -> b43_phy_init_pctl(dev)");
      Say("     board vendor is 0x106B (Apple), not BCM, so the BU4306 early return does not");
      Say("     fire. gmode is TRUE. cur_idle_tssi is 0 from prepare_structs, so the measurement");
      Say("     arm runs; analog is 2 (not 0) so it takes the set_txpower_g path, and radio_rev");
      Say("     != 8 gives rfatt 9 with no padmix.");
      Say("");
      Say("     ★ b43_set_txpower_g IS FINALLY COMPLETE. i2 ported its first four statements and");
      Say("       stopped at b43_lo_g_adjust because lo.c was not yet ported -- that was 4b-ii-3.");
      Say("       j5 supplied LoGAdjust, so the fifth statement can now run, and init_pctl needs");
      Say("       the whole function TWICE: once at bbatt 11 / rfatt 9 to measure idle TSSI, and");
      Say("       once more to put the previous attenuations back.");
      SaySigned16("       attenuations to restore afterwards: bbatt = ",(SInt16)bbAttG);
      SaySigned16("                                            rfatt = ",(SInt16)rfAttG);
      PhyInitPctl();
      SayH("     cur_idle_tssi (PHY 0x0029) = ",(unsigned long)gCurIdleTssi,4);
      Say("       ⚠ b43's sanity check on this value lives inside `if (B43_DEBUG)`, which is off");
      Say("         in a normal build, so it is NOT ported -- porting a debug-only branch would");
      Say("         diverge from the driver everyone actually ships. Logged instead.");

      Say("");
      Say("  -> initg's final branch: if (chip_id == 0x4306 && chip_pkg == 2) { two masks }");
      { UInt32 raw=0,pkg,ncores;
        if(SsbSelectCore(&gBus,gBus.idxChipCommon)==noErr){
          raw=ssb_r32(gBus.bar0,CC_CHIPID);
          (void)SsbSelectCore(&gBus,gBus.idx80211); }
        pkg=(raw&0x00F00000UL)>>20;
        ncores=(raw&0x0F000000UL)>>24;
        SayH("     ChipCommon CHIPID raw = ",raw,8);
        SayH("       chip_id  (0x0000FFFF)      = ",(unsigned long)(raw&0xFFFFUL),4);
        Say1("       chip_rev (0x000F0000 >>16) = ",(unsigned long)((raw&0x000F0000UL)>>16));
        Say1("       chip_pkg (0x00F00000 >>20) = ",(unsigned long)pkg);
        Say1("       nr_cores (0x0F000000 >>24) = ",(unsigned long)ncores);
        Say("     ⚠ THE FIELD LAYOUT IS CONFIRMED FROM TWO DIRECTIONS, not assumed. nr_cores");
        Say("       decodes to 5, which is exactly the core count Stage 2 measured by walking the");
        Say("       backplane. A wrong mask would not land on that by chance.");
        gChipPkg=(UInt16)pkg; gChipIdRaw=raw;
        if((raw&0xFFFFUL)==0x4306UL && pkg==2){
          Say("     chip_id 0x4306 AND chip_pkg 2 -> the two masks RUN");
          PhyMaskSet((UInt16)B43_PHY_CRS0,0xBFFF,0);
          PhyMaskSet((UInt16)B43_PHY_OFDM(0xC3),0x7FFF,0);
          gChipPkgBranch=1; }
        else {
          Say("     chip_id is 0x4306 but chip_pkg is NOT 2 -> the two masks do NOT run.");
          Say("     ⚠ b43's comment says the 0 in `... || 0` should be 'if OFDM may not be used");
          Say("       in the current locale', and adds that OFDM is legal everywhere. So the");
          Say("       branch is chip-package-gated only, and this package is not the gated one.");
          gChipPkgBranch=0; } }

      Say("");
      Say("  ORACLE O -- init_pctl and the final branch");
      SayH("    cur_idle_tssi      = ",(unsigned long)gCurIdleTssi,4);
      SayH("    CHIPID raw         = ",(unsigned long)gChipIdRaw,8);
      Say1("    chip_pkg           = ",(unsigned long)gChipPkg);
      Say1("    chip_pkg branch    = ",(unsigned long)gChipPkgBranch);
      { UInt16 hlo,hmi,hhi; UInt16 t58,t5A,t70,t72; UInt16 p47A;
        HfRead(&hlo,&hmi,&hhi);
        t58=ShmRead16Shared(0x0058); t5A=ShmRead16Shared(0x005A);
        t70=ShmRead16Shared(0x0070); t72=ShmRead16Shared(0x0072);
        p47A=PhyRead(0x047A);
        SayH("    PHY 0x047A         = ",(unsigned long)p47A,4);
        Say ("      (hardware_pctl_early_init's !has_hardware_pctl arm writes 0xC111)");
        SayH("    host flags mid     = ",(unsigned long)hmi,4);
        Say ("      (HWPCTL is bit 23 of the 48-bit value, i.e. 0x0080 of the MIDDLE word, and");
        Say ("       init_gphy's !has_hardware_pctl arm clears it)");
        { Str255 L;L[0]=0;PCat(L,"    TSSI SHM 0x58/0x5A/0x70/0x72 = 0x");PCatHex(L,t58,4);
          PCat(L," 0x");PCatHex(L,t5A,4);PCat(L," 0x");PCatHex(L,t70,4);
          PCat(L," 0x");PCatHex(L,t72,4);Out(L); }
        { int pctlOk=(p47A==0xC111);
          int hwpctlOk=((hmi&0x0080)==0);
          int tssiOk=(t58==0x7F7F&&t5A==0x7F7F&&t70==0x7F7F&&t72==0x7F7F);
          int attOk=(bbAttG==gOldBb && rfAttG==gOldRf);
          Say("");
          Say(pctlOk  ? "    ✓ PHY 0x047A holds 0xC111 -- early_init took the expected arm."
                      : "    ✗ PHY 0x047A does not hold 0xC111.");
          Say(hwpctlOk? "    ✓ HWPCTL is clear in the host flags."
                      : "    ✗ HWPCTL is still set in the host flags.");
          Say(tssiOk  ? "    ✓ all four TSSI SHM words are 0x7F7F."
                      : "    ✗ a TSSI SHM word is not 0x7F7F.");
          Say(attOk   ? "    ✓ the attenuations were restored after the TSSI measurement."
                      : "    ✗ the attenuations were NOT restored -- set_txpower_g's second call");
          if(!attOk) Say("      did not put bbatt and rfatt back.");
          if(pctlOk&&hwpctlOk&&tssiOk&&attOk){ oracleO=1;
            Say("    ==> ORACLE O PASSED.");
            Say("");
            Say("    ★★★★★ b43_phy_initg IS COMPLETE. STAGE 4 IS DONE.");
            Say("      Every statement of the G-PHY initialisation, from b43_chip_init's prefix");
            Say("      through initg's closing brace, runs in order on this card with every");
            Say("      branch resolved against measured identity rather than assumed.");
          } else Say("    ✗ ORACLE O FAILED."); } }

      Say("");
      Say("    ⚠ THE TABLE IS LEFT AS wa_rssi_lt WROTE IT, NOT RESTORED -- deliberately. This is");
      Say("      a real workaround, not a probe scribble: b43 leaves the identity table in place");
      Say("      and the next workarounds run on top of it. The saved originals above are logged");
      Say("      for the record, not to be written back.");
      SayH("      (for the record) RSSI[0] was = ",origRssi[0],4);
      SayH("      (for the record) ORIGTR  was = ",origTr,4); }

    Say("");
    Say("");
    Say("");
    Say("");
    Say("  ⏹ NOTHING OF b43_phy_initg REMAINS. Stage 5 is a different kind of work:");
    Say("      DMA ring allocation under the 30-bit addressing limit");
    Say("      PrepareMemoryForIO / CheckpointIO cache management");
    Say("      physical-address translation for descriptors");
    Say("      PCI interrupt installation, chaining to the parent handler");
    Say("    ⚠ AND THE PROBE METHOD ITSELF HAS TO CHANGE. Interrupt-level code cannot call the");
    Say("      File Manager, so the flushed-line-per-Say pattern that carried every run of");
    Say("      Stages 1-4 does not work inside an ISR. That needs the interrupt-safe ring the");
    Say("      EHCI work already uses.");
    Say("");

initg_done:
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB&&oracleC&&oracleD&&oracleE&&oracleF&&oracleG&&oracleH&&oracleI&&oracleJ&&oracleK&&oracleL&&oracleM&&oracleN&&oracleO){
      Say("  ★★★★★ b43_phy_initg IS COMPLETE. THE G-PHY IS FULLY INITIALISED.");
      Say("      Every statement from b43_chip_init's prefix through initg's closing brace, in");
      Say("      order, with every branch resolved against MEASURED identity:");
      Say("        A  radio 0x7A reaches 0x007F through three read-modify-writes");
      Say("        B  the three PHY table loops land on their formula at every sampled offset");
      Say("        C  the scalar PHY writes hold");
      Say("        D  set_txpower_g's derived attenuations land where it puts them");
      Say("        E  the LO save/restore pair returns all 16 registers");
      Say("        F  the MAC drove a frame out of the TX engine and came back to idle");
      Say("        G  the LO I/Q search ran on a live sensor and converged, twice, to the same");
      Say("           pair inside b43's own abs() <= 16 bound");
      Say("        H  the tail's two verifiable results");
      Say("        I  the PHY table accessors: 64 auto-incremented writes landed in the right");
      Say("           slots, read back in reverse so the read path could not mask a write bug");
      Say("        J  wa_all's nine remaining workarounds: 21 masksets, 0 wrong, tables verified");
      Say("        K  calc_loopback_gain swept a live sensor and restored all 18 saved registers");
      Say("        L  radio_init2050: the rfover chain computed from max_lb_gain, and a clean");
      Say("           restore of 12 PHY + 3 radio registers");
      Say("        M  initg's tail, and lo_g_adjust re-converging to a DIFFERENT I/Q pair now");
      Say("           that max_lb_gain and trsw_rx_gain are real -- the two subsystems agree");
      Say("        N  the NRSSI slope, with a monotonic 64-entry table and a clean restore");
      Say("        O  init_pctl, and the chip_pkg branch decided by reading CHIPID rather than");
      Say("           assuming it -- nr_cores decoding to 5 confirms the field layout");
      Say("      ⇒ STAGE 4 IS COMPLETE. Next is Stage 5: DMA rings and the first packet, which");
      Say("        is the go/no-go gate and a different kind of work -- OS 9 driver integration");
      Say("        rather than register translation.");
    } else {
      Say("  ✗ NOT VERIFIED. Do not build on this.");
      if(!oracleA) Say("    - radio 0x7A did not reach the computed value.");
      if(!oracleB) Say("    - a PHY table loop entry did not match the formula.");
      if(!oracleC) Say("    - a scalar PHY write did not hold.");
      if(!oracleD) Say("    - set_txpower_g's attenuation results did not hold.");
      if(!oracleE) Say("    - a lo_measure_setup/restore save/restore pair is wrong.");
      if(!oracleF) Say("    - the transmission handshake did not complete, or txctl values are wrong.");
      if(!oracleG) Say("    - the LO search did not run on a live sensor, or left the legal range.");
      if(!oracleH) Say("    - initb6's tail did not land its two verifiable values.");
      if(!oracleI) Say("    - the PHY table accessor mis-placed writes. Do NOT port the other nine.");
      if(!oracleJ) Say("    - a workaround maskset or a transcribed table entry is wrong.");
      if(!oracleK) Say("    - the loopback gain sweep failed, or did not restore what it saved.");
      if(!oracleL) Say("    - radio_init2050's rfover chain or its restore is wrong.");
      if(!oracleM) Say("    - initg's tail or the lo_g_adjust re-calibration failed.");
      if(!oracleN) Say("    - the NRSSI slope measurement or its restore is wrong.");
      if(!oracleO) Say("    - init_pctl did not leave the expected state.");
    }
    return (oracleA&&oracleB&&oracleC&&oracleD&&oracleE&&oracleF&&oracleG&&oracleH&&
            oracleI&&oracleJ&&oracleK&&oracleL&&oracleM&&oracleN&&oracleO);
}

#pragma GCC diagnostic pop

#endif /* AP_PHY_INITG_H */
