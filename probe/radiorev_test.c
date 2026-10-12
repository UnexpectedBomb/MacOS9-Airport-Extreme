/* radiorev_test.c -- host test for ap_radiorev.h: the 4306-vs-4318 radio-revision predicates that
 * drive two Phase 0 bring-up decisions -- the synth_pu workaround skip, and the B43_HF_4318TSSI host
 * flag. The decisions touch hardware in the driver; the LOGIC is pure and proven here, including the
 * deliberate divergence from b43's literal synth_pu guard (an unread identity must RUN the
 * workaround, never skip it -- see ap_radiorev.h's ⚠).
 *
 *   cc -std=c99 -Wall -o radiorev_test radiorev_test.c && ./radiorev_test
 */
#include <stdio.h>
#include "ap_radiorev.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{ gChecks++; if(!ok){ gFails++; printf("  FAIL: %s\n", what); } }

/* Every card in the grid reports radio_ver 0x2050; only the revision differs. */
#define VER    AP_RADIO_VER_B2050      /* 0x2050 */
#define R4306  AP_RADIO_REV_4306       /* 2 */
#define R4318  AP_RADIO_REV_4318       /* 6 */

static void TestSynthPu(void)
{
    /* b43: run for (ver == 0x2050 && rev < 6); i.e. skip iff rev >= 6 on a 0x2050 radio. */
    Check("4306 (rev 2) RUNS the synth_pu workaround",   !SynthPuShouldSkip(VER, R4306));
    Check("4318 (rev 6) SKIPS the synth_pu workaround",   SynthPuShouldSkip(VER, AP_RADIO_REV_4318_R6));
    Check("4318 rev 8 (FW400 WMP54GS, HW-confirmed) skips", SynthPuShouldSkip(VER, AP_RADIO_REV_4318_R8));
    Check("rev 7 (a later 0x2050) also skips",            SynthPuShouldSkip(VER, 7));
    Check("rev 5 still runs (the <6 boundary)",          !SynthPuShouldSkip(VER, 5));

    /* ⚠ THE DELIBERATE DIVERGENCE. An UNREAD identity (0,0) is the state at the moment
     * SynthPuWorkaround is reached, because ApShimRadioOn runs before the radio-id read. It MUST
     * fall through to RUNNING the workaround, matching the 4306's shipped behavior; b43's literal
     * guard (ver != 0x2050 || rev >= 6) would SKIP here and silently regress the validated 4306. */
    Check("unread identity (0,0) RUNS the workaround -- no 4306 regression", !SynthPuShouldSkip(0, 0));
}

static void TestHostFlag(void)
{
    Check("4318 (rev 6) wants B43_HF_4318TSSI",              Wants4318Tssi(VER, AP_RADIO_REV_4318_R6));
    Check("4306 (rev 2) does NOT want the 4318 flag",       !Wants4318Tssi(VER, R4306));
    /* ★ HW-CONFIRMED: the FW400 4318 is radio rev 8, and b43 (main.c:4883) gives HF_4318TSSI to
     * rev 6 ONLY -- a rev-8 4318 correctly gets neither it nor VCORECALC. So this must be FALSE. */
    Check("4318 rev 8 (FW400 hardware) gets NO HF_4318TSSI -- b43 is rev-6-only",
                                                            !Wants4318Tssi(VER, AP_RADIO_REV_4318_R8));
    Check("rev 7 does NOT want the 4318 flag (exactly 6)",  !Wants4318Tssi(VER, 7));
    Check("unread identity (0,0) wants no 4318 flag",       !Wants4318Tssi(0, 0));
    Check("a non-0x2050 radio at rev 6 is not a 4318",      !Wants4318Tssi(0x2060, AP_RADIO_REV_4318_R6));
}

int main(void)
{
    printf("ap_radiorev.h host test\n");
    TestSynthPu();
    TestHostFlag();
    printf("%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
