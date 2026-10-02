/* ap_rate.h -- the DATA-RATE layer (k193): the OFDM PLCP encoder, the rate table, and a minimal ARF.
 *
 * Pure computation, no MMIO, no Toolbox: included by ap_tx.h for the driver AND by rate_test.c on the
 * host, so what the host suite proves is the code the card runs (the ap_enet.h / enet_test.c pattern).
 *
 * ★ WHY IT EXISTS. Through k192 every data frame went out at 1 Mbps CCK -- GenerateTxHdr351 hard-codes
 * it (the right first-join choice) -- and the k192 capture measured the cost: the Pi ACKed each segment
 * in 0.6 ms, yet the Mac emitted one 1460-byte segment per ~23 ms (~40 KB/s on an upload); ~13 ms of
 * that is the frame's own airtime at 1 Mbps. MINIMAL-STA-SCOPE.md always planned "fix one OFDM rate".
 *
 * ★ HOW THE RATE IS CHOSEN. Data frames carry a main rate from kTxRates (CCK 1 fallback inside the
 * header, always). The rate INDEX rides in the frame's status cookie (AP_TX_COOKIE_DATA + index), so
 * every XMITSTAT entry names the exact rate its frame used -- no queue of "what did I send", nothing
 * to drift. The ISR's status drain calls ApTxRateFeedback per entry:
 *     GOOD: acked within AP_ARF_OK_TRIES (2) attempts  x AP_ARF_UP_AFTER in a row    -> one rate up
 *     BAD:  3+ attempts, or never acked                x AP_ARF_DOWN_AFTER in a row  -> one rate down
 * and only statuses at the CURRENT rate vote, so frames still in flight from before a step cannot
 * cascade it. Start at OFDM 24.
 *
 * ★★ k196: A SINGLE RETRY NO LONGER VOTES "TOO FAST". Through k195 only a first-try ACK was good,
 * and the k195 snapshot measured what that did. Retries were MORE common at LOWER rates -- 5.6% of
 * frames at 54 Mbps needed one, 14% at 48, 17-19% at 24/36, 24-27% at 6/12, 30% at 1 -- so retries
 * here track a frame's AIRTIME (contention with the AP's own traffic, interference), not its rate.
 * Stepping down for them made frames longer and retries likelier: a spiral to the 1 Mbps floor. 614
 * frames went at 1 Mbps (~12.5 ms of air each, ~7.7 s in all) against 2962 at 54 (~0.9 s), and
 * NetBench's first 1 MB upload crawled at 72 KB/s. Tiger, on the same card and spot, sits at 54
 * ("lastTxRate 54") and moves 2 MB/s. A frame that needed a THIRD attempt is at the in-frame
 * fallback's doorstep (SFFBLIM = 3: the 4th attempt goes at CCK 1), and two of those in a row is a
 * rate that is genuinely failing -- that still steps down at once.
 *
 * ⚠ ApTxRateFeedback runs at SECONDARY INTERRUPT level (ApShimDrainTxStatQuiet): computation on
 *   statics only -- no MMIO, no allocation, no Toolbox. Keep it that way; check-exec-level.py audits it.
 */
#ifndef AP_RATE_H
#define AP_RATE_H

/* b43_generate_plcp_hdr, the OFDM arm -- "not ported: we transmit CCK" until k193.
 *     d = b43_plcp_get_ratecode_ofdm(bitrate); d |= (octets << 5); plcp->data = cpu_to_le32(d);
 * The 4-bit codes are b43's: 6:0xB 9:0xF 12:0xA 18:0xE 24:0x9 36:0xD 48:0x8 54:0xC. The PHY adds the
 * SIGNAL parity and tail. b43 WARNs if octets needs more than 12 bits; a 2346-byte MPDU is 0x92A. */
static void GeneratePlcpHdrOfdm(UInt8 *plcp,UInt16 octets,UInt8 ratecode)
{
    UInt32 d = ((UInt32)ratecode & 0x0FUL) | (((UInt32)octets & 0x0FFFUL) << 5);
    int i;
    for(i=0;i<6;i++) plcp[i]=0;
    plcp[0] = (UInt8)(d & 0xFF);                    /* cpu_to_le32: little-endian */
    plcp[1] = (UInt8)((d >> 8) & 0xFF);
    plcp[2] = (UInt8)((d >> 16) & 0xFF);
    plcp[3] = (UInt8)((d >> 24) & 0xFF);
}

typedef struct { UInt8 ofdm, code, mbps; } ApTxRate;
static const ApTxRate kTxRates[] = {
    { 0, 0x0A,  1 },    /* 0: CCK 1 (b43 B43_CCK_RATECODE_1MB) -- the proven floor and in-frame fallback */
    { 1, 0x0B,  6 },    /* 1: OFDM  6 */
    { 1, 0x0A, 12 },    /* 2: OFDM 12 */
    { 1, 0x09, 24 },    /* 3: OFDM 24 -- the start */
    { 1, 0x0D, 36 },    /* 4: OFDM 36 */
    { 1, 0x08, 48 },    /* 5: OFDM 48 */
    { 1, 0x0C, 54 },    /* 6: OFDM 54 */
};
#define AP_TX_NRATES       7
#define AP_TX_RATE_START   3
#define AP_TX_COOKIE_DATA  0xC0E0u      /* data cookies are 0xC0E0 + rate index; join/mgmt use 0xC00x */
#define AP_ARF_UP_AFTER    10
#define AP_ARF_DOWN_AFTER  2
#define AP_ARF_OK_TRIES    2            /* k196: acked within this many attempts votes GOOD */

static volatile UInt32 gTxRateIdx = AP_TX_RATE_START;
static UInt16  gTxLastCookie = AP_TX_COOKIE_DATA;   /* what ApShimWriteFrame last used (DrainTxStatus waits) */
static UInt32  gTxRateSent[AP_TX_NRATES], gTxRateOk1[AP_TX_NRATES];
static UInt32  gTxRateRetryOk[AP_TX_NRATES], gTxRateFail[AP_TX_NRATES];
static UInt32  gTxArfOkRun = 0, gTxArfBadRun = 0, gTxRateUp = 0, gTxRateDown = 0;
static UInt32  gTxFcHist[5];                        /* data statuses by attempt count: 0,1,2,3,4+ */

/* One XMITSTAT_0 word -> per-rate stats + the ARF vote. b43's decode: cookie = v0 >> 16,
 * frame_count = (v0 & 0xF000) >> 12 (attempts; 0 = never sent), acked = v0 & 0x2. */
static void ApTxRateFeedback(UInt32 v0)
{
    UInt32 cookie = (v0 >> 16) & 0xFFFFUL, r, fc;
    int acked;
    if((cookie & 0xFFF0UL) != AP_TX_COOKIE_DATA) return;      /* join/mgmt frames: not ours to judge */
    r = cookie & 0x000FUL;
    if(r >= AP_TX_NRATES) return;
    fc = (v0 >> 12) & 0x0FUL;
    acked = (v0 & 0x00000002UL) != 0;
    gTxFcHist[fc >= 4 ? 4 : fc]++;
    if(acked && fc == 1) gTxRateOk1[r]++;
    else if(acked)       gTxRateRetryOk[r]++;
    else                 gTxRateFail[r]++;
    if(r != gTxRateIdx) return;                                /* sent before the last step: no vote */
    if(acked && fc >= 1 && fc <= AP_ARF_OK_TRIES){             /* k196: was fc == 1 -- see the header */
        gTxArfBadRun = 0;
        if(++gTxArfOkRun >= AP_ARF_UP_AFTER && gTxRateIdx + 1 < AP_TX_NRATES){
            gTxRateIdx++; gTxRateUp++; gTxArfOkRun = 0; }
    } else {
        gTxArfOkRun = 0;
        if(++gTxArfBadRun >= AP_ARF_DOWN_AFTER && gTxRateIdx > 0){
            gTxRateIdx--; gTxRateDown++; gTxArfBadRun = 0; }
    }
}

#endif /* AP_RATE_H */
