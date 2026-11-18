/* ap_phy_reset.h -- k248: put every file-scope variable of ap_phy_initg.h back to its declared value, so that
 * ApPhyInitG() can run a SECOND time in one load exactly as it runs the first time.
 *
 * ★ WHY. 2.0.1's bring-up runs ApPhyInitG once per load and treats any failed Stage 4 oracle as fatal: chip_init's
 * tail, DMA, the MAC and the join are all declined and the card stays down until Open Transport loads the driver
 * again. On the FW800 (2026-11-17, claude-os9 firewire-fw800/logs/uwake21A-idle-usb2-hw-2026-11-17) the reload
 * after a wake failed Oracle N on ONE restore readback, `RADIO 0x007A want 0x007F got 0x007C`; the identical
 * readback failed k98 (2026-09-22), and aspnodes2's Oracle K failure (2026-11-11) was one of the three radio
 * registers that oracle compares silently. b43 itself never checks these restores. Switching TCP/IP to Ethernet and
 * back (a fresh load: fresh data, a core reset, the whole bring-up again) recovered every time.
 *
 * So k248 does what that reload does, inside the load: on a failed oracle, reset the core and run the bring-up from
 * the core reset through ApPhyInitG once more (ap_shim.c ApShimRadioBringUp). b43's own recovery has the same shape
 * (main.c b43_chip_reset: wireless_core_exit, then wireless_core_init = core reset, chip_init, phy init).
 *
 * ⚠⚠ A RETRY IS ONLY A RETRY IF THE SECOND RUN STARTS WHERE THE FIRST DID. ApPhyInitG keeps ~110 variables at file
 *   scope: oracle results, measured values, sentinels (gLlMin = 0xFFFF, gTxLoop1 = -1, gLoTxBias = 0xFF) and the
 *   verdict's own running count (gBad). A fresh load gets them from the data section; a second call in the same load
 *   would inherit the first call's values -- gBad and gNrssiRestoreBad among them, which alone would fail the retry
 *   for the reason the first run failed. This function writes every one of them back.
 * ⚠ IT MUST NOT DRIFT. A variable added to ap_phy_initg.h and not to this list is a retry that silently inherits
 *   it. check-phy-reset.py parses both files and fails unless every file-scope variable of ap_phy_initg.h is reset
 *   here to exactly its declared initializer (an array without one: memset to 0). Run it with the other gates.
 * ⚠ This file is NOT part of the mechanically extracted Stage 4 copy; ap_phy_initg.h is unchanged by it. Include it
 *   AFTER ap_phy_initg.h. Task level only (the bring-up's level). */
#ifndef AP_PHY_RESET_H
#define AP_PHY_RESET_H

#include <string.h>

static void ApPhyInitGResetState(void)
{
    /* ap_phy_initg.h line 125 */
    gShimIsV3 = 0;
    /* 172-173 */
    gOtabAddr = 0; gOtabDir = AP_OTAB_DIR_UNKNOWN;
    gOfdmRev = 0;
    /* 184-189: Oracle N, the NRSSI slope */
    gNrssi0 = 0; gNrssi1 = 0;
    gNrssiSlope = 0;
    memset(gNrssiLt, 0, sizeof gNrssiLt);
    gNrssiRestoreBad = 0;
    gThr048A = 0;
    gThrA = 0; gThrB = 0;
    /* 194-197: radio_init2050 */
    gRcc = 0; gRadio78 = 0; gInitval = 0; gRadio60 = 0;
    gTmp1 = 0; gTmp2 = 0;
    gInitvalFromRadio78 = 0; gI2050RestoreBad = 0; gLoop2Iters = 0;
    gRfoverA = 0; gRfoverValA = 0; gRfoverValB = 0; gRfoverValC = 0;
    /* 201-208: Oracle K, the loopback gain */
    gLlMin = 0xFFFF; gLlMax = 0;
    gLlCount = 0;
    gLbOuter = 0; gLbInner = 0; gLbRestoreBad = 0; gLbTrswUnderflow = 0;
    gLbTrsw = 0;
    gLbMaxGain = 0;
    bbAttG = 0; rfAttG = 0;
    /* 285-293 */
    gLnaGain = 0; gPgaGain = 0; gLnaLodGain = 0; gTrswRxGain = 0; gMaxLbGain = 0;
    gFtFirst = 0; gFtMin = 0xFFFF; gFtMax = 0;
    gFtCount = 0;
    /* 314-330: the LO calibration */
    gTxLoop1 = -1; gTxLoop2 = -1; gTxLoop3 = -1;
    gRadioPctlReg = 0;
    gLoTxMagn = 0;
    gLoTxBias = 0xFFU;
    gTxA1 = -1; gTxA2 = -1; gTxA3 = -1; gTxB1 = -1; gTxB2 = -1; gTxB3 = -1;
    gTxAStat = 0; gTxBStat = 0;
    gTxBase = 0; gTxTrig = 0; gTxIfsTrig = 0; gTxFinal = 0; gTxIfsFinal = 0;
    gLowestFeedth = 0; gFt1First = 0; gFt1Min = 0; gFt1Max = 0; gLow1 = 0; gLow2 = 0;
    gFt1Count = 0; gFt2Count = 0;
    memset(gPassSeed, 0, sizeof gPassSeed); memset(gPassLow, 0, sizeof gPassLow); memset(gPassMul, 0, sizeof gPassMul);
    gPassN = 0;
    gLoI = 0; gLoQ = 0; gLoI2 = 0; gLoQ2 = 0; gOuter1 = 0; gSteps1 = 0; gOuter2 = 0; gSteps2 = 0;
    gLoI3 = 0; gLoQ3 = 0;
    gFt3Count = 0;
    gLow3 = 0;
    gTxStat1 = 0; gTxStat2 = 0; gTxIfs = 0;
    /* 539: the maskset oracle */
    gMsBad = 0;
    gMsN = 0;
    /* 1653-1654: init_pctl and the chip package */
    gCurIdleTssi = 0; gOldBb = 0; gOldRf = 0;
    gChipPkg = 0;
    gChipIdRaw = 0;
    gChipPkgBranch = 0;
    /* 1758: the verdict's running count */
    gBad = 0;
}

#endif /* AP_PHY_RESET_H */
