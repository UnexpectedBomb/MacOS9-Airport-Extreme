/* ap_radiorev.h -- the B2050 radio REVISION as the 4306-vs-4318 discriminator, and the two
 * bring-up decisions b43 keys on it. PURE: two predicates on (radio_ver, radio_rev), no hardware,
 * no toolbox, no libc -- so radiorev_test.c compiles and runs them on the host.
 *
 * ★ WHY THIS EXISTS. The endgame grid (BCM4306/BCM4318 x A1026/PCI/CardBus) has ONE radio family:
 * every card carries the Broadcom B2050 radio, radio_ver 0x2050, so the chip is told apart by the
 * radio REVISION, not the version -- the 4306 reports rev 2, the 4318 reports rev 6. b43 branches on
 * that revision in exactly two places during bring-up, and both are expressed here so the logic can
 * be proven away from the hardware (each hardware run costs a reboot):
 *
 *   1. b43_synth_pu_workaround (phy_g.c:170) runs ONLY for (radio_ver == 0x2050 && radio_rev < 6).
 *      The 4318 (rev 6) must SKIP it. Expressed as SynthPuShouldSkip() -- see the ⚠ for why this is
 *      phrased as a skip-condition and not as the inverse of b43's run-condition.
 *
 *   2. b43_wireless_core_init (main.c:4882-4886) sets a host flag off the same revision:
 *          radio_rev == 6 -> B43_HF_4318TSSI   (the 4318 wants TSSI-based tx power control)
 *          radio_rev <  6 -> B43_HF_VCORECALC  (the 4306's arm)
 *      Wants4318Tssi() is the rev-6 arm -- the only one Phase 0 acts on. The 4306's VCORECALC arm is
 *      deliberately NOT forced on here: this driver's 4306 bring-up is HW-validated (k219) with no
 *      host flags at all, so perturbing it would demand a fresh 4306 hardware run. That is tracked
 *      separately in BCM4318-SUPPORT-PLAN.md, not folded in silently.
 *
 * ⚠ WHY SynthPuShouldSkip IS NOT THE INVERSE OF b43's GUARD. In this driver's bring-up order
 * (ap_shim.c:6033-6036) ApShimRadioOn() -- which reaches SynthPuWorkaround -- runs BEFORE
 * ApShimPhyInit(), and the radio identity is not read until inside ApPhyInitG(). So at the moment
 * SynthPuWorkaround is called the revision globals are still 0. b43's literal guard
 * `radio_ver != 0x2050 || radio_rev >= 6` would then be TRUE (0 != 0x2050) and SKIP the workaround
 * -- a silent regression on the HW-validated 4306, which has run that workaround on every shipping
 * build. Phrasing the guard as "skip ONLY when positively rev >= 6 on a 0x2050 radio" makes an
 * unread identity fall through to RUNNING the workaround, i.e. today's exact 4306 behavior, on both
 * chips. The guard therefore becomes EFFECTIVE on a 4318 only once the identity is read before
 * RadioOn (a Piece-2 sequencing task); until then the master gate in ap_phy_initg.h refuses rev!=2
 * and a 4318 never completes PHY init anyway. The one divergence from b43 -- a (hypothetical)
 * non-0x2050 radio would be RUN, not skipped -- is unreachable: no card in the grid carries any
 * radio but the 0x2050.
 */
#ifndef AP_RADIOREV_H
#define AP_RADIOREV_H

/* The one radio family across the whole grid. */
#define AP_RADIO_VER_B2050   0x2050U

/* Radio revisions on the 0x2050 family. The 4306 is rev 2. A 4318 is rev 6 OR rev 8 --
 * HARDWARE-CONFIRMED rev 8 on the FW400 WMP54GS (2026-10-10, radio raw 0x8205017F). b43's
 * B43_HF_4318TSSI arm (main.c:4883) is rev-6 ONLY; a rev-8 4318 gets NEITHER that flag nor
 * VCORECALC, and is handled by separate rev-8 arms (pu_delay 2400, radio_init2050's rev-8 path,
 * hardware power control). So Wants4318Tssi keying on rev 6 is b43-exact, not a bug. */
#define AP_RADIO_REV_4306       2U   /* BCM4306 */
#define AP_RADIO_REV_4318_R6    6U   /* the 4318 variant b43 gives HF_4318TSSI */
#define AP_RADIO_REV_4318_R8    8U   /* the 4318 variant on the FW400 WMP54GS (confirmed) */
#define AP_RADIO_REV_4318       AP_RADIO_REV_4318_R6  /* back-compat: the rev-6 TSSI arm */

/* b43_synth_pu_workaround runs for (ver == 0x2050 && rev < 6).
 * Skip ONLY a positively-identified rev>=6 radio on the 0x2050 family -- see the ⚠ above. */
static int SynthPuShouldSkip(unsigned short radioVer, unsigned short radioRev)
{ return (radioVer == AP_RADIO_VER_B2050) && (radioRev >= 6U); }

/* b43 main.c:4883-4884: B43_HF_4318TSSI is set for the rev-6 arm, and only that arm. */
static int Wants4318Tssi(unsigned short radioVer, unsigned short radioRev)
{ return (radioVer == AP_RADIO_VER_B2050) && (radioRev == AP_RADIO_REV_4318); }

#endif /* AP_RADIOREV_H */
