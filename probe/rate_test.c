/* rate_test.c -- host suite for ap_rate.h (k193): the OFDM PLCP encoder and the ARF state machine.
 *
 * Build:  cc -o rate_test rate_test.c && ./rate_test          (exit 0 = every check passed)
 *
 * The expected values are written out INDEPENDENTLY of ap_rate.h -- b43's rate codes from
 * b43_plcp_get_ratecode_ofdm, and the PLCP word from b43_generate_plcp_hdr's formula done by hand --
 * so a transcription slip in the header fails here instead of on the card.
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;      /* 32-bit, as on the G4 (enet_test.c's `long` is 64-bit on a Mac host) */

#include "ap_rate.h"

static int gFails = 0, gChecks = 0;
static void Check(const char *what, int ok)
{
    gChecks++;
    if(!ok) gFails++;
    printf("  %-66s %s\n", what, ok ? "[ok]" : "[FAIL]");
}

/* One XMITSTAT_0 word as the hardware reports it: cookie<<16 | frame_count<<12 | acked(bit1) | valid(bit0). */
static UInt32 St(UInt32 cookie, UInt32 fc, int acked)
{
    return (cookie << 16) | ((fc & 0xF) << 12) | (acked ? 2u : 0u) | 1u;
}
static UInt32 Data(UInt32 r) { return AP_TX_COOKIE_DATA + r; }

static void ArfReset(void)
{
    UInt32 i;
    gTxRateIdx = AP_TX_RATE_START;
    gTxArfOkRun = gTxArfBadRun = gTxRateUp = gTxRateDown = 0;
    for(i = 0; i < AP_TX_NRATES; i++) gTxRateSent[i] = gTxRateOk1[i] = gTxRateRetryOk[i] = gTxRateFail[i] = 0;
    for(i = 0; i < 5; i++) gTxFcHist[i] = 0;
}
static void Ok(int n)   { while(n--) ApTxRateFeedback(St(Data(gTxRateIdx), 1, 1)); }
static void Bad(int n)  { while(n--) ApTxRateFeedback(St(Data(gTxRateIdx), 4, 0)); }

int main(void)
{
    UInt8 p[6];
    char msg[128];
    UInt32 i;

    printf("== OFDM PLCP (b43_generate_plcp_hdr, OFDM arm) ==\n");
    /* b43_plcp_get_ratecode_ofdm, transcribed independently: Mbps -> code */
    { static const struct { UInt8 mbps, code; } b43[] =
        { {6,0xB},{9,0xF},{12,0xA},{18,0xE},{24,0x9},{36,0xD},{48,0x8},{54,0xC} };
      for(i = 0; i < AP_TX_NRATES; i++){
          UInt32 k; int found = !kTxRates[i].ofdm;
          for(k = 0; k < 8; k++) if(kTxRates[i].ofdm && b43[k].mbps == kTxRates[i].mbps)
              found = (b43[k].code == kTxRates[i].code);
          sprintf(msg, "kTxRates[%u] = %2u Mbps %s has b43's rate code", i, kTxRates[i].mbps,
                  kTxRates[i].ofdm ? "OFDM" : "CCK ");
          Check(msg, found);
      } }
    Check("kTxRates[0] is CCK 1 with b43's CCK code 0x0A", !kTxRates[0].ofdm && kTxRates[0].code == 0x0A && kTxRates[0].mbps == 1);
    Check("rates are strictly ascending (ARF steps must mean faster/slower)",
          kTxRates[1].mbps > kTxRates[0].mbps && kTxRates[2].mbps > kTxRates[1].mbps &&
          kTxRates[3].mbps > kTxRates[2].mbps && kTxRates[4].mbps > kTxRates[3].mbps &&
          kTxRates[5].mbps > kTxRates[4].mbps && kTxRates[6].mbps > kTxRates[5].mbps);
    Check("start rate is OFDM 24", kTxRates[AP_TX_RATE_START].ofdm && kTxRates[AP_TX_RATE_START].mbps == 24);

    /* 24 Mbps, a full frame: 1504 bytes + FCS = 1508 octets. d = 0x9 | (1508 << 5) = 0x9 | 0xBC80 = 0xBC89 */
    memset(p, 0xEE, 6); GeneratePlcpHdrOfdm(p, 1508, 0x9);
    Check("24 Mbps, 1508 octets -> 89 BC 00 00 00 00 (d = code | octets<<5, LE)",
          p[0]==0x89 && p[1]==0xBC && p[2]==0x00 && p[3]==0x00 && p[4]==0 && p[5]==0);
    /* 54 Mbps, the largest MPDU: 2346 = 0x92A. d = 0xC | (0x92A << 5) = 0xC | 0x12540 = 0x1254C */
    memset(p, 0xEE, 6); GeneratePlcpHdrOfdm(p, 2346, 0xC);
    Check("54 Mbps, 2346 octets -> 4C 25 01 00 00 00 (length reaches byte 2)",
          p[0]==0x4C && p[1]==0x25 && p[2]==0x01 && p[3]==0x00 && p[4]==0 && p[5]==0);
    /* 6 Mbps, a TCP ACK frame: 24+8+8+40+8 = 88 + FCS = 92. d = 0xB | (92 << 5) = 0xB | 0xB80 = 0xB8B */
    memset(p, 0xEE, 6); GeneratePlcpHdrOfdm(p, 92, 0xB);
    Check("6 Mbps, 92 octets -> 8B 0B 00 00 00 00", p[0]==0x8B && p[1]==0x0B && p[2]==0 && p[3]==0);
    memset(p, 0xEE, 6); GeneratePlcpHdrOfdm(p, 100, 0xF9);
    Check("rate code is masked to 4 bits (0xF9 -> 9)", (p[0] & 0x0F) == 0x9);

    printf("== ARF (ApTxRateFeedback) ==\n");
    ArfReset();
    Ok(9);  Check("9 clean sends at 24: no step yet", gTxRateIdx == 3 && gTxRateUp == 0);
    Ok(1);  Check("10th clean send: up to 36", gTxRateIdx == 4 && gTxRateUp == 1);
    Ok(10); Check("10 more: up to 48", gTxRateIdx == 5);
    Ok(10); Check("10 more: up to 54", gTxRateIdx == 6);
    Ok(30); Check("54 is the ceiling: stays at 54", gTxRateIdx == 6 && gTxRateUp == 3);

    Bad(1); Check("one miss at 54: no step", gTxRateIdx == 6);
    Bad(1); Check("second consecutive miss: down to 48", gTxRateIdx == 5 && gTxRateDown == 1);

    ApTxRateFeedback(St(Data(6), 1, 1));
    ApTxRateFeedback(St(Data(6), 4, 0)); ApTxRateFeedback(St(Data(6), 4, 0));
    Check("statuses for a frame sent at the OLD rate (54) do not vote", gTxRateIdx == 5 && gTxRateDown == 1);
    Check("...but they are still counted against 54", gTxRateOk1[6] > 0 && gTxRateFail[6] >= 3);

    ApTxRateFeedback(St(0xC000, 1, 1)); ApTxRateFeedback(St(0xC00D, 4, 0)); ApTxRateFeedback(St(0xC00D, 4, 0));
    Check("join/mgmt cookies (0xC000, 0xC00D) are ignored entirely", gTxRateIdx == 5 && gTxRateDown == 1);

    Bad(1); Ok(1); Bad(1);
    Check("miss, clean, miss: the clean send breaks the run -- no step", gTxRateIdx == 5);
    Ok(9); Bad(1); Ok(9);
    Check("9 clean, 1 miss, 9 clean: the miss resets the up-run -- no step", gTxRateIdx == 5);

    { UInt32 before = gTxRateRetryOk[5];
      gTxArfOkRun = gTxArfBadRun = 0;   /* the block above left a 9-send good run open: isolate this one */
      ApTxRateFeedback(St(Data(5), 2, 1)); ApTxRateFeedback(St(Data(5), 2, 1));
      Check("k196: acked on the 2nd try votes GOOD -- two in a row do not step down", gTxRateIdx == 5);
      Check("...and is still counted as 'retried ok', not first-try and not failed", gTxRateRetryOk[5] == before + 2);
      ApTxRateFeedback(St(Data(5), 3, 1)); ApTxRateFeedback(St(Data(5), 2, 1)); ApTxRateFeedback(St(Data(5), 3, 1));
      Check("k196: 3 tries, 2 tries, 3 tries -- the 2-try send breaks the bad run, no step", gTxRateIdx == 5);
      ApTxRateFeedback(St(Data(5), 3, 1));
      Check("k196: two consecutive 3-try sends (the fallback's doorstep) vote bad: down to 36", gTxRateIdx == 4); }

    ApTxRateFeedback(St(Data(4), 1, 0)); ApTxRateFeedback(St(Data(4), 2, 0));
    Check("k196: few attempts but NO ACK (1 and 2 tries, unacked) votes bad: down to 24", gTxRateIdx == 3);
    ApTxRateFeedback(St(Data(3), 0, 0)); ApTxRateFeedback(St(Data(3), 0, 1));
    Check("frame_count 0 votes bad -- even with the ACK bit set, a contradiction is not a success: down to 12",
          gTxRateIdx == 2);

    Bad(20);
    Check("a run of misses walks all the way to the CCK 1 floor", gTxRateIdx == 0);
    Bad(20);
    Check("the floor holds: CCK 1 never goes below index 0", gTxRateIdx == 0);
    Ok(10);
    Check("from the floor, 10 clean sends climb back to OFDM 6", gTxRateIdx == 1);
    { UInt32 k; for(k = 0; k < 10; k++) ApTxRateFeedback(St(Data(gTxRateIdx), 2, 1)); }
    Check("k196: ten sends that each needed one retry also climb (OFDM 6 -> 12)", gTxRateIdx == 2);

    ApTxRateFeedback(St(AP_TX_COOKIE_DATA + 7, 1, 1)); ApTxRateFeedback(St(AP_TX_COOKIE_DATA + 15, 1, 1));
    Check("out-of-table rate index in a cookie is ignored (no crash, no vote)", gTxRateIdx == 2);

    /* ★ k196: REPLAY THE k195 SNAPSHOT'S OWN NUMBERS. At 54 Mbps: 2795 first-try, 165 retried, 2
     * failed of 2962 -- 5.6% needed a retry, 0.07% never got through. A deterministic LCG draws each
     * frame's outcome from those rates. Written out here rather than derived from ap_rate.h. */
    {   UInt32 seed = 12345u, k, downs;
        ArfReset(); gTxRateIdx = 6;
        for(k = 0; k < 3000u; k++){
            UInt32 d; seed = seed * 1103515245u + 12345u; d = (seed >> 8) % 10000u;
            if(d < 7u)        ApTxRateFeedback(St(Data(gTxRateIdx), 4, 0));   /* 0.07%: failed */
            else if(d < 567u) ApTxRateFeedback(St(Data(gTxRateIdx), 2, 1));   /* 5.6%: one retry */
            else              ApTxRateFeedback(St(Data(gTxRateIdx), 1, 1)); }
        downs = gTxRateDown;
        sprintf(msg, "k196: 3000 frames at k195's measured 54 Mbps odds stay at 54 (down-steps %u)", downs);
        Check(msg, gTxRateIdx == 6 && downs == 0);
    }
    /* ...and a rate that is genuinely failing still drops at once: 40% of frames need 4 attempts. */
    {   UInt32 seed = 777u, k;
        ArfReset(); gTxRateIdx = 6;
        for(k = 0; k < 40u && gTxRateIdx == 6; k++){
            UInt32 d; seed = seed * 1103515245u + 12345u; d = (seed >> 8) % 100u;
            ApTxRateFeedback(St(Data(gTxRateIdx), d < 40u ? 4u : 1u, 1)); }
        sprintf(msg, "k196: a failing 54 (40%% of frames need 4 tries) steps down within %u frames", k);
        Check(msg, gTxRateIdx < 6 && k <= 30u);
    }

    ArfReset();
    ApTxRateFeedback(St(Data(3), 1, 1)); ApTxRateFeedback(St(Data(3), 2, 1));
    ApTxRateFeedback(St(Data(3), 3, 0)); ApTxRateFeedback(St(Data(3), 9, 0)); ApTxRateFeedback(St(Data(3), 0, 0));
    Check("attempt histogram buckets 0,1,2,3,4+ are counted exactly",
          gTxFcHist[0]==1 && gTxFcHist[1]==1 && gTxFcHist[2]==1 && gTxFcHist[3]==1 && gTxFcHist[4]==1);

    printf("\n%s  %d of %d checks passed\n", gFails ? "[FAIL]" : "[ok]", gChecks - gFails, gChecks);
    return gFails ? 1 : 0;
}
