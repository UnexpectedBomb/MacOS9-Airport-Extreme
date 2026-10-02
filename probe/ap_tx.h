/* ap_tx.h -- the TRANSMIT layer, and the frame parser the driver needs to aim it.
 *
 * ★ THE SIXTH EXTRACTION. Stage 8-3 is authentication and association, and both are exchanges:
 * send a frame, wait for a reply. The driver has been a receiver since 8-2g and has no transmit
 * path at all, so this is the foundation both of them stand on.
 *
 * ⚠ ParseRxBuffer CAME TOO, AND NOT FOR SYMMETRY. The driver has to aim at something. A
 *   unicast frame needs a BSSID, and the only BSSID the driver has access to is the one inside a
 *   beacon it already received. Re-deriving "addr3 of a management frame" in the driver would be
 *   a hand-copy of logic that exists and is validated -- the exact thing five extractions have
 *   been avoiding.
 *
 * ⚠⚠ gTxhShift IS LOAD-BEARING AND IS NOT A CONSTANT. TXH_351_COOKIE and friends are
 *   defined in terms of it, so the txhdr layout is chosen at RUNTIME from the firmware revision
 *   the card actually loaded: 0 for FW_HDR_351, 4 for FW_HDR_410. Stage 5 lost most of its
 *   schedule to a v3/v4 mismatch where the microcode read a 106-byte header as its own 82-byte
 *   layout -- null PLCP, null destination, nothing decodable off the antenna. Whoever calls this
 *   must set gTxhShift from the loaded revision before building a header.
 *
 * ⚠ EVERY TRANSMIT MUST REBUILD THE TXHDR. GenerateTxHdr351 writes the PLCP, which carries
 *   frameLen + FCS_LEN -- the length the PHY actually transmits. Reusing a header from a previous
 *   frame sends the previous frame's length, and k49 lost a run to exactly that.
 *
 * ★ HOW IT WAS MADE. Three spans lifted VERBATIM by line range, never retyped, proven by
 * reconstruction against the original file. Same as the five extractions before it.
 */
#ifndef AP_TX_H
#define AP_TX_H

#include "ap_rx.h"   /* the receive layer, the ring, the descriptors, the barrier */
#include "ap_rate.h" /* k193: OFDM PLCP, the data-rate table, the ARF -- pure, host-tested (rate_test.c) */

/* ⚠ ParseRxBuffer needs the RX header layout and the 802.11 frame offsets, and both lived
 * in airport_rx.c. Sixth extraction, sixth constants block to follow the code. */

/* ---- RX header, format_351 (our ucode is 295) ---- */
#define RXH_FRAME_LEN               0
#define RXH_PHY_STATUS0             4
#define RXH_JSSI                    6
#define RXH_SIG_QUAL                7
#define RXH_MAC_STATUS              12          /* format_351 puts mac_status here */
#define RXH_MAC_TIME                16
#define RXH_CHANNEL                 18
#define B43_RX_MAC_FCSERR           0x00000001UL
#define B43_RX_MAC_RESP             0x00000002UL   /* k197: "response frame transmitted" -- we ACKed it */
#define B43_RX_PHYST0_ANT           0x0020         /* k197: phy_status0 -- the antenna it arrived on */
#define B43_RX_MAC_PADDING          0x00000004UL
#define B43_RX_MAC_DEC              0x00000008UL
#define B43_RX_CHAN_ID              0x07F8
#define B43_RX_CHAN_ID_SHIFT        3

#define K6_HDR_PLCP6        6UL
#define K6_WLHDR_BYTES      24UL       /* fc 2, dur 2, addr1/2/3 18 */
#define K6_FIXED_PARAMS     12UL       /* timestamp 8, beacon interval 2, capability 2 */

/* ⚠ The TX slot/ring counts came with the code -- sixth extraction, sixth time a constants
 * block had to follow it. The compiler finds these every time, which is why the header is
 * compiled standalone in a driver context before the driver is built. */

#define K8_TX_SLOTS 256
#define K9_NTXC     4        /* controllers 0..3; b43 maps them to AC_BK, BE, VI, VO */
#define K10_TXC     1        /* ★ SETTLED BY k9: AC_BE is controller 1, and only 1 and 3 moved */
#define K10_TARGET_SSID "ExampleNet"
#define K10_TARGET_LEN  10
#define K13_NANT        3        /* k14: ANT0/ANT1/AUTO -- eliminated, all identical */
#define K17_NPASS       2        /* broadcast (no ACK expected), then directed */      /* B43_TXRING_SLOTS; the 4096-byte ring holds 512 */
/* Parse one received buffer. Returns:
 *    0  nothing usable
 *    1  a frame, but not a beacon or probe response
 *    2  a beacon or probe response, fields extracted
 * Every bound is checked against the real body length, because a malformed or truncated frame
 * off the air is the normal case, not the exceptional one. */
static int ParseRxBuffer(const UInt8 *buf,UInt32 frameoffset,
                         UInt8 *bssidOut,UInt8 *ssidOut,UInt8 *ssidLenOut,
                         UInt8 *chanOut,UInt16 *fcOut,UInt16 *flenOut,UInt8 *jssiOut)
{
    UInt16 flen = le16at(buf+RXH_FRAME_LEN);
    const UInt8 *plcp = buf + frameoffset;
    const UInt8 *f    = plcp + K6_HDR_PLCP6;
    UInt32 bodyLen, fc, type, sub;
    const UInt8 *ie, *end;

    *ssidLenOut = 0; *chanOut = 0;
    *flenOut = flen;
    *jssiOut = buf[RXH_JSSI];
    if(flen < K6_HDR_PLCP6 + K6_WLHDR_BYTES) return 0;
    if(flen > B43_DMA0_RX_FW351_BUFSIZE)     return 0;
    bodyLen = (UInt32)flen - K6_HDR_PLCP6;

    fc = (UInt32)le16at(f);
    *fcOut = (UInt16)fc;
    type = (fc >> 2) & 3;
    sub  = (fc >> 4) & 0xF;
    { int i; for(i=0;i<6;i++) bssidOut[i] = f[10+i]; }     /* addr2 = the transmitter */
    if(type != 0) return 1;
    if(sub != 8 && sub != 5) return 1;

    /* tagged elements start after the 24-byte header and the 12 fixed parameters */
    ie  = f + K6_WLHDR_BYTES + K6_FIXED_PARAMS;
    end = f + bodyLen;
    while(ie + 2 <= end){
      UInt8 id = ie[0], len = ie[1];
      if(ie + 2 + len > end) break;                        /* truncated element: stop, do not guess */
      if(id == 0x00 && *ssidLenOut == 0){                  /* SSID */
        UInt8 n = len; int k;
        if(n > 32) n = 32;
        for(k=0;k<n;k++) ssidOut[k] = ie[2+k];
        *ssidLenOut = n; }
      else if(id == 0x03 && len >= 1) *chanOut = ie[2];    /* DS Parameter Set */
      ie += 2 + len; }
    return 2;
}

/* ============================================================================
 * STAGE 5-4: TRANSMIT.
 *
 * ★ b43_txhdr, FORMAT_351, is 106 BYTES -- and that is not a guess.
 * b43_txhdr_size() returns `100 + sizeof(struct b43_plcp_hdr6)` for FW_HDR_351,
 * and adding up the struct members by hand gives the same 106 with format_351
 * starting at +70. The two agree, so the offsets below are checked against a
 * number b43 computes itself rather than against my own arithmetic alone.
 *
 * ★ MOST OF b43_generate_txhdr IS UNREACHABLE FOR US, and each arm is named so
 * the omissions are inspectable rather than silent:
 *    encryption      use_encryption is false: a probe request is never protected
 *    RTS/CTS         not requested, and a 45-byte frame is under any threshold
 *    OFDM            we transmit CCK 1 Mbps, where beacons and probes live
 *    phy_ctl1*       fill_phy_ctl1 is (LP || N || HT); this is a G-PHY, so false
 *    5 GHz           gmode is true
 *    fallback rate   rate_fb == rate, so dur_fb is just the frame's duration_id
 * What remains is the list of assignments below, all of which b43 performs.
 * ==========================================================================*/
#define TXH_MAC_CTL          0UL      /* le32 */
#define TXH_MAC_FRAME_CTL    4UL      /* le16 */
#define TXH_PHY_CTL          8UL      /* le16 */
#define TXH_PHY_RATE        18UL      /* u8   */
#define TXH_EXTRA_FT        20UL      /* u8   */
#define TXH_CHAN_RADIO_CODE 21UL      /* u8   */
#define TXH_TX_RECEIVER     38UL      /* 6 bytes */
#define TXH_PLCP_FB         54UL      /* plcp6 */
#define TXH_DUR_FB          60UL      /* le16 */
/* ★★★★★ THE TXHDR TAIL IS FIRMWARE-GENERATION-DEPENDENT, AND UNTIL k28 WE GUESSED IT.
 *
 * b43 picks the layout from the ucode revision the CARD reports (main.c:2699):
 *     rev >= 598 -> FW_HDR_598,  rev >= 410 -> FW_HDR_410,  else FW_HDR_351
 * and b43_txhdr_size() (xmit.h:192) returns 112/104/100 plus sizeof(plcp6) = 6.
 *
 * The two formats we can actually meet differ by exactly four bytes -- format_410 inserts
 * mimo_antenna and preload_size ahead of the cookie -- so everything from the cookie on
 * shifts by 4 and the total goes 106 -> 110.
 *
 * ⚠ THIS IS SELECTED AT RUNTIME, NOT HARDCODED. Hardcoding 351 is what k1..k27 did, and it
 *   was right for the v3 blobs and wrong the moment the firmware changed. gFwRev is read from
 *   SHM by the bring-up, so the probe now follows whatever microcode is actually loaded. */
#define TXH_FMT_410_EXTRA    4UL
static UInt32 gTxhShift = 0;           /* 0 for FW_HDR_351, 4 for FW_HDR_410 */
#define TXH_351_COOKIE      (72UL  + gTxhShift)   /* le16 */
#define TXH_351_TX_STATUS   (74UL  + gTxhShift)   /* le16 */
#define TXH_351_PLCP       (100UL  + gTxhShift)   /* plcp6 */
#define TXH_SIZE_351       (106UL  + gTxhShift)   /* == b43_txhdr_size() */

#define B43_TXH_MAC_ACK        0x00000001UL
#define B43_TXH_MAC_HWSEQ      0x00000010UL
#define B43_TXH_MAC_STMSDU     0x00000008UL
#define B43_TXH_PHY_ENC_CCK    0x0000
#define B43_TXH_PHY_ENC_OFDM   0x0001      /* k193: b43 xmit.h -- the main rate is OFDM */
/* b43_xmit.h:154. The card has two antenna connectors; ANT2/ANT3 are for parts that do not
 * apply here and are not offered in the ladder. */
#define B43_TXH_PHY_ANT_MASK   0x03C0
#define B43_TXH_PHY_ANT0       0x0000
#define B43_TXH_PHY_ANT1       0x0040
#define B43_TXH_EFT_FB_CCK     0x00
#define B43_CCK_RATE_1MB       0x02        /* units of 500 kbps; the PLCP formula confirms it */
#define B43_CCK_RATECODE_1MB   0x0A
#define FCS_LEN                4UL
#define B43_MMIO_XMITSTAT_0    0x170UL
#define B43_MMIO_XMITSTAT_1    0x174UL
#define B43_MMIO_GEN_IRQ_MASK  0x12CUL
/* B43_IRQ_MASKTEMPLATE, expanded. b43 clears PHY_TXERR from it unless the build is at
 * B43_VERBOSITY_DEBUG; we keep it, because that IS the debug value and we are debugging. */
#define B43_IRQ_MASKTEMPLATE   0x38058A64UL



/* ============================================================================
 * THE MEASUREMENT EVERY PROBE SINCE k7 HAS BEEN MISSING.
 *
 * TXDPTR advancing proves only that the DMA ENGINE read the descriptor. It says
 * nothing about whether the MAC put anything on the air. b43 gets that from
 * XMITSTAT, in handle_irq_transmit_status (b43_main.c:1334):
 *
 *     v0 = b43_read32(dev, B43_MMIO_XMITSTAT_0);
 *     if (!(v0 & 0x00000001))
 *             break;                      <- BIT 0 = a status is PENDING
 *     v1 = b43_read32(dev, B43_MMIO_XMITSTAT_1);
 *
 * k7 and k8 read XMITSTAT_0, got 0x00000000, logged it as a bare number and moved
 * on. Bit 0 clear means the microcode had posted NO STATUS AT ALL -- a different
 * fact from "the AP ignored us", and it was sitting in the log the whole time.
 *
 * The status carries the cookie we stamped into the txhdr, so a status can be tied
 * to OUR frame rather than to traffic in general; `acked`, the AP's acknowledgement;
 * a retry count; and supp_reason, a NAMED suppression code whose values include
 * CHAN -- channel mismatch -- which is live, because the microcode compares the
 * txhdr's chan_radio_code against SHM_SH_CHAN and this probe writes both.
 * ==========================================================================*/
#define B43_TXST_SUPP_NONE    0
#define B43_TXST_SUPP_PMQ     1
#define B43_TXST_SUPP_FLUSH   2
#define B43_TXST_SUPP_PREV    3
#define B43_TXST_SUPP_CHAN    4
#define B43_TXST_SUPP_LIFE    5
#define B43_TXST_SUPP_UNDER   6
#define B43_TXST_SUPP_ABNACK  7

static int gTxStatCount = 0, gTxStatAcked = 0, gTxStatOurs = 0, gTxStatSupp = -1;
static const char *kAntNameV[3] = {"ANTENNA 0 (forced)","ANTENNA 1 (forced)",
                                   "ANT01AUTO (b43's default)"};

/* Drain the queue the way b43 does -- while(1) until bit 0 is clear -- with a bounded wait in
 * front, because b43 is driven by an interrupt and we poll. */
/* ⛔⛔ THIS RETURNS "WAS **OUR** FRAME ACKED", AND UNTIL k90 IT DID NOT.
 *
 * It used to return `found` -- the number of status entries drained, of ANY cookie, ACKed or
 * not -- and then `if(found) break` returned the moment ANY status appeared. Every caller in
 * the driver assigns it to a variable called something Acked:
 *
 *     gShimTxAcked   = DrainTxStatus(0xC000,600);      gShimAuthAcked  = ...(0xC001,600);
 *     gShimAssocAcked= DrainTxStatus(0xC020,600);      gTxAcked       += ...(0xC0E0,600);
 *
 * so the contract never matched a single caller's intent. It was right only while no stale
 * entry was pending, which was true for every run until 8-8 put a transmit AFTER the four-way
 * handshake: two leftover EAPOL statuses (0xC030, 0xC040) were sitting in the FIFO, `found`
 * became 2 on the first pass, the loop broke, and k90 reported "frames the AP ACKed = 2" for a
 * frame whose status never arrived at all. The log had no 0xC0E0 block in it anywhere.
 *
 * ⚠ A COUNT IS NOT A VERDICT. The per-entry printing below was always truthful -- it says which
 *   cookie and whether THAT entry was acked -- and the defect was entirely in what got handed
 *   back to the caller. That is this project's own "test content, not return codes" rule, and
 *   the return value was the one thing nobody tested.
 *
 * Now: keep draining and waiting until OUR cookie appears or the timeout expires, and return
 * that frame's ACK bit. Strictly more correct for all four callers. */
static int DrainTxStatus(UInt16 wantCookie,UInt32 waitMs)
{
    UInt32 v0,v1,ms; int found=0; int sawWanted=0, wantedAcked=0;
    for(ms=0; ms<waitMs; ms+=10){
      while(1){
        v0 = ssb_r32(gBus.bar0,B43_MMIO_XMITSTAT_0);
        if(!(v0 & 0x00000001UL)) break;
        v1 = ssb_r32(gBus.bar0,B43_MMIO_XMITSTAT_1);
        { UInt16 cookie = (UInt16)(v0 >> 16);
          UInt16 seq    = (UInt16)(v1 & 0xFFFFUL);
          UInt32 phySt  = (v1 & 0x00FF0000UL) >> 16;
          UInt16 tmp    = (UInt16)(v0 & 0xFFFFUL);
          UInt32 fcount = (tmp & 0xF000U) >> 12;
          UInt32 rcount = (tmp & 0x0F00U) >> 8;
          UInt32 supp   = (tmp & 0x001CU) >> 2;
          int acked     = (tmp & 0x0002U) ? 1 : 0;
          int interm    = (tmp & 0x0040U) ? 1 : 0;
          Str255 L;
          found++; gTxStatCount++;
          L[0]=0;PCat(L,"      ★ TX STATUS: cookie 0x");PCatHex(L,(unsigned long)cookie,4);
          if(cookie==wantCookie){ PCat(L,"  = OURS"); gTxStatOurs++;
                                  sawWanted = 1;
                                  /* ⚠ an INTERMEDIATE status is not the final word on this
                                   * frame -- only a final one settles whether it was acked. */
                                  if(!interm) wantedAcked = acked; }
          else PCat(L,"  (not the cookie we stamped)");
          Out(L);
          SayH("          XMITSTAT_0 = ",v0,8);
          SayH("          XMITSTAT_1 = ",v1,8);
          Say1("          seq         = ",(unsigned long)seq);
          Say1("          frame_count = ",fcount);
          Say1("          rts_count   = ",rcount);
          SayH("          phy_stat    = ",phySt,2);
          Say (acked ? "          ★★ ACKED -- an access point acknowledged this frame"
                     : "          NOT acked");
          if(interm) Say("          (intermediate status, not final)");
          if(acked) gTxStatAcked++;
          gTxStatSupp = (int)supp;
          { const char *r =
              (supp==B43_TXST_SUPP_NONE)  ? "NONE -- not suppressed" :
              (supp==B43_TXST_SUPP_PMQ)   ? "PMQ -- power-management queue entry" :
              (supp==B43_TXST_SUPP_FLUSH) ? "FLUSH -- a flush was requested" :
              (supp==B43_TXST_SUPP_PREV)  ? "PREV -- a previous fragment failed" :
              (supp==B43_TXST_SUPP_CHAN)  ? "CHAN -- CHANNEL MISMATCH" :
              (supp==B43_TXST_SUPP_LIFE)  ? "LIFE -- lifetime expired" :
              (supp==B43_TXST_SUPP_UNDER) ? "UNDER -- buffer underflow" :
              (supp==B43_TXST_SUPP_ABNACK)? "ABNACK -- afterburner NACK" : "unknown";
            Str255 M;M[0]=0;PCat(M,"          supp_reason = ");PCat(M,r);Out(M); }
          if(supp==B43_TXST_SUPP_CHAN){
            Say("          ⚠ CHANNEL MISMATCH. The microcode compares the txhdr's");
            Say("            chan_radio_code against SHM_SH_CHAN. Both are written by this");
            Say("            probe; if they disagree, that is the bug and it is one line."); } } }
      /* ⚠ WAIT FOR OURS, NOT FOR ANY. `if(found) break` is what made k90 report an ACK for a
       * frame that never reported a status: two stale EAPOL entries satisfied it instantly. */
      if(sawWanted) break;
      SsbSpinUs(10000); }
    (void)found;
    return wantedAcked;
}

/* b43_generate_plcp_hdr, CCK arm. The OFDM arm is not ported: we transmit CCK.
 * ⚠ plcp is SIX bytes in the header but the generator writes a 4-byte union --
 * raw[0] is the rate code, raw[1] the service byte, and data bits 16.. the length. */
static void GeneratePlcpHdrCck(UInt8 *plcp,UInt16 octets,UInt8 bitrate)
{
    UInt32 plen;
    int i;
    for(i=0;i<6;i++) plcp[i]=0;
    plen = ((UInt32)octets * 16UL) / (UInt32)bitrate;
    if((((UInt32)octets * 16UL) % (UInt32)bitrate) > 0){
      plen++;
      /* the 11 Mbps length-extension arm needs bitrate == B43_CCK_RATE_11MB; we send at 1 Mbps,
       * so it cannot be taken and raw[1] is 0x04 either way. */
      plcp[1] = 0x04;
    } else plcp[1] = 0x04;
    /* plcp->data |= cpu_to_le32(plen << 16) -- bytes 2 and 3 of the little-endian word */
    plcp[2] = (UInt8)(plen & 0xFF);
    plcp[3] = (UInt8)((plen >> 8) & 0xFF);
    plcp[0] = B43_CCK_RATECODE_1MB;
}

/* k193: the OFDM arm of b43_generate_plcp_hdr (GeneratePlcpHdrOfdm) lives in ap_rate.h, so the host
 * suite (rate_test.c) exercises the same code the card runs. */

/* b43_generate_txhdr, our live path only. */
static void GenerateTxHdr351(UInt8 *txh,const UInt8 *frame,UInt16 frameLen,
                             UInt8 channel,UInt16 cookie,UInt16 antSel,int ackReq)
{
    UInt32 mac_ctl = 0;
    UInt16 phy_ctl = 0;
    UInt16 fctl = le16at(frame);
    UInt16 durId = le16at(frame+2);
    UInt16 plcpLen = (UInt16)(frameLen + FCS_LEN);
    int i;

    for(i=0;i<(int)TXH_SIZE_351;i++) txh[i]=0;      /* memset(txhdr, 0, sizeof(*txhdr)) */

    txh[TXH_PHY_RATE] = B43_CCK_RATECODE_1MB;
    le16st(txh+TXH_MAC_FRAME_CTL,fctl);
    for(i=0;i<6;i++) txh[TXH_TX_RECEIVER+i] = frame[4+i];   /* wlhdr->addr1 */

    /* rate_fb == rate, so b43 takes the first arm and copies duration_id verbatim. */
    le16st(txh+TXH_DUR_FB,durId);

    GeneratePlcpHdrCck(txh+TXH_351_PLCP,plcpLen,B43_CCK_RATE_1MB);
    GeneratePlcpHdrCck(txh+TXH_PLCP_FB,  plcpLen,B43_CCK_RATE_1MB);

    txh[TXH_EXTRA_FT]        = B43_TXH_EFT_FB_CCK;
    txh[TXH_CHAN_RADIO_CODE] = channel;

    phy_ctl |= B43_TXH_PHY_ENC_CCK;
    /* short preamble is only set when mac80211 asks; it does not for a probe request at 1 Mbps */
    /* ⚠ k12 PROVED THE MAC TRANSMITS AND THE MICROCODE DOES NOT SUPPRESS -- seq incremented
     * 1 -> 2 across exactly our two frames and supp_reason was NONE -- yet nothing ACKed. An AP
     * must ACK any unicast frame addressed to it with a good FCS, so the frame is either not
     * radiating or radiating corrupted. b43 asks for ANT01AUTO here because it assumes two
     * connected antennas; an MDD typically has one wire. If TX auto-selects the unconnected
     * one, every observation fits: RX is fine because receive diversity settles on the
     * connected antenna, and TX radiates into an open circuit. Hence the ladder. */
    phy_ctl |= (antSel & B43_TXH_PHY_ANT_MASK);

    /* ⚠ b43: `if (!(info->flags & IEEE80211_TX_CTL_NO_ACK)) mac_ctl |= B43_TXH_MAC_ACK;`
     * and mac80211 sets NO_ACK for every broadcast and multicast frame, because nothing
     * acknowledges them. Asking for an ACK on a broadcast frame would make the MAC wait for
     * one that cannot come -- so this is a parameter, not a constant. */
    if(ackReq) mac_ctl |= B43_TXH_MAC_ACK;
    mac_ctl |= B43_TXH_MAC_HWSEQ;                   /* let the hardware assign the sequence number */
    mac_ctl |= B43_TXH_MAC_STMSDU;                  /* first (and only) fragment */
    /* !phy->gmode -> 5GHZ: gmode is true. LONGFRAME: only above the RTS threshold. */

    le16st(txh+TXH_351_COOKIE,cookie);
    le32st(txh+TXH_MAC_CTL,mac_ctl);
    le16st(txh+TXH_PHY_CTL,phy_ctl);
}

/* ★ k193: b43_generate_txhdr for DATA frames, with a real main rate. GenerateTxHdr351 above stays the
 * join's (1 Mbps main AND fallback -- every management/EAPOL frame keeps its proven header).
 *   main     = `ofdm ? ratecode : CCK 1`   ->  phy_rate, plcp, phy_ctl ENC_OFDM/ENC_CCK
 *   fallback = CCK 1 Mbps, always          ->  plcp_fb, extra_ft FB_CCK
 * The fallback is the rate every frame of this project has ever been proven at, so a main-rate miss
 * still has a known-good retry inside the same frame. dur_fb copies the frame's own duration_id (b43
 * recomputes it for a different fallback rate; copying can only over-reserve the medium -- safe).
 * Everything else is GenerateTxHdr351 verbatim, including the ACK / HWSEQ / STMSDU mac_ctl bits. */
/* ★★ k203: THE FALLBACK IS THE NEXT RATE DOWN, NOT CCK 1. The ucode sends a data frame's retries at
 * the header's FALLBACK rate. Through k202 that was always CCK 1 Mbps (k193's "proven floor"): one
 * failed 1500-byte frame at 54 Mbps came back as ~12.5 ms of airtime at 1 Mbps. k202's capture shows
 * them as a cluster of 14-16.5 ms upload gaps -- 22 of them, ~0.32 s of a 2.1 s upload (15%); k198
 * and k199 had the same peak at 12-12.5 ms. b43 falls back ONE STEP (b43_calc_fallback_rate:
 * 54->48->36->24->18->12->9->6->CCK 5.5; mac80211-era b43 takes rate control's second rate). Within
 * our table that is simply the previous index, floor CCK 1: a retry now costs ~0.3 ms, not 12.5.
 * fbOfdm/fbCode describe it; the CCK arm is unchanged. */
#define B43_TXH_EFT_FB_OFDM    0x01        /* b43 xmit.h: extra_ft fallback encoding OFDM */
static void GenerateTxHdr351Rate(UInt8 *txh,const UInt8 *frame,UInt16 frameLen,
                                 UInt8 channel,UInt16 cookie,UInt16 antSel,int ackReq,
                                 int ofdm,UInt8 ratecode,int fbOfdm,UInt8 fbCode)
{
    UInt32 mac_ctl = 0;
    UInt16 phy_ctl = 0;
    UInt16 fctl = le16at(frame);
    UInt16 durId = le16at(frame+2);
    UInt16 plcpLen = (UInt16)(frameLen + FCS_LEN);
    int i;

    for(i=0;i<(int)TXH_SIZE_351;i++) txh[i]=0;

    txh[TXH_PHY_RATE] = ofdm ? ratecode : B43_CCK_RATECODE_1MB;
    le16st(txh+TXH_MAC_FRAME_CTL,fctl);
    for(i=0;i<6;i++) txh[TXH_TX_RECEIVER+i] = frame[4+i];   /* wlhdr->addr1 (the AP, for ToDS data) */
    le16st(txh+TXH_DUR_FB,durId);

    if(ofdm) GeneratePlcpHdrOfdm(txh+TXH_351_PLCP,plcpLen,ratecode);
    else     GeneratePlcpHdrCck (txh+TXH_351_PLCP,plcpLen,B43_CCK_RATE_1MB);
    if(fbOfdm) GeneratePlcpHdrOfdm(txh+TXH_PLCP_FB,plcpLen,fbCode);      /* k203: one step down */
    else       GeneratePlcpHdrCck (txh+TXH_PLCP_FB,plcpLen,B43_CCK_RATE_1MB);

    txh[TXH_EXTRA_FT]        = fbOfdm ? B43_TXH_EFT_FB_OFDM : B43_TXH_EFT_FB_CCK;   /* the FALLBACK's encoding */
    txh[TXH_CHAN_RADIO_CODE] = channel;

    phy_ctl |= ofdm ? B43_TXH_PHY_ENC_OFDM : B43_TXH_PHY_ENC_CCK;   /* the MAIN rate's encoding */
    phy_ctl |= (antSel & B43_TXH_PHY_ANT_MASK);

    if(ackReq) mac_ctl |= B43_TXH_MAC_ACK;
    mac_ctl |= B43_TXH_MAC_HWSEQ;
    mac_ctl |= B43_TXH_MAC_STMSDU;

    le16st(txh+TXH_351_COOKIE,cookie);
    le32st(txh+TXH_MAC_CTL,mac_ctl);
    le16st(txh+TXH_PHY_CTL,phy_ctl);
}

/* A probe request. 24-byte header, then the two elements every AP expects. */
static UInt16 BuildProbeRequest(UInt8 *f,const UInt8 *myMac,const UInt8 *dstBssid,
                                const char *ssid,int ssidLen)
{
    UInt16 n = 0;
    int i;
    f[0]=0x40; f[1]=0x00;                           /* type 0 mgmt, subtype 4 probe request */
    f[2]=0x00; f[3]=0x00;                           /* duration */
    for(i=0;i<6;i++) f[4+i]  = dstBssid[i];         /* addr1 */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2 */
    for(i=0;i<6;i++) f[16+i] = dstBssid[i];         /* addr3 */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    n = 24;
    f[n++]=0x00; f[n++]=(UInt8)ssidLen;             /* SSID element (len 0 = wildcard) */
    for(i=0;i<ssidLen;i++) f[n++]=(UInt8)ssid[i];
    f[n++]=0x01; f[n++]=0x04;                       /* supported rates */
    f[n++]=0x82; f[n++]=0x84; f[n++]=0x0B; f[n++]=0x16;   /* 1,2 basic; 5.5,11 */
    return n;
}

/* ============================================================================
 * ★★★★★ STAGE 6-1: OPEN SYSTEM AUTHENTICATION.
 *
 * The first frame in this project that asks the AP for something rather than merely being heard
 * by it. 802.11-2020 §9.3.3.12. An Open System exchange is two frames and no cryptography:
 *
 *     us -> AP   auth, algorithm 0 (Open System), sequence 1, status 0
 *     AP -> us   auth, algorithm 0,               sequence 2, status <- THE ANSWER
 *
 * The body is three little-endian 16-bit words and nothing else. There are no information
 * elements in an Open System request, so there is no IE builder here and no IE parser in the
 * response path -- which is exactly what MINIMAL-STA-SCOPE.md §2 buys us: the Wi-Fi 7 element
 * stuffing that breaks legacy parsers never has to be looked at.
 *
 * ⚠ WPA2 DOES NOT CHANGE THIS FRAME. The PSK plays no part in authentication; 802.11 Open System
 *   auth is a formality that always succeeds against an AP that will talk to us at all, and the
 *   real admission decision happens at association and then in the 4-way handshake. So a status
 *   other than 0 here means something structural -- MAC filtering, a band or rate mismatch, or a
 *   malformed frame -- and NOT a wrong passphrase. Worth stating because "auth failed" on a WPA2
 *   network reads like a credentials problem and is not one.
 *
 * ⚠ ADDR3 IS THE BSSID, and for authentication addr1 and addr3 are both the AP. The probe request
 *   builder above happens to set all three the same way, but it does so for a different reason
 *   (a probe can be broadcast), so this does not share its code.
 *
 * THE ORACLE: an auth frame back from the target, sequence 2, status 0. Sequence 2 is what makes
 * it a reply to us rather than someone else's auth we overheard, and RxWaitFor already filters on
 * addr2 == the target and addr1 == our MAC. */
#define AUTH_ALG_OPEN        0
#define AUTH_SEQ_REQUEST     1
#define AUTH_SEQ_RESPONSE    2
#define AUTH_STATUS_SUCCESS  0

static UInt16 BuildAuthRequest(UInt8 *f,const UInt8 *myMac,const UInt8 *bssid)
{
    UInt16 n = 0;
    int i;
    f[0]=0xB0; f[1]=0x00;                           /* type 0 mgmt, subtype 11 authentication */
    f[2]=0x00; f[3]=0x00;                           /* duration -- the MAC fills it */
    for(i=0;i<6;i++) f[4+i]  = bssid[i];            /* addr1, the AP */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2, us */
    for(i=0;i<6;i++) f[16+i] = bssid[i];            /* addr3, the BSSID */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    n = 24;
    /* Written byte by byte on purpose. le16st() is the stwbrx-style store used for MMIO and
     * descriptors; these are frame CONTENTS, and spelling the two bytes out makes the wire
     * order explicit and independent of what the host's byte order happens to be. */
    f[n++]=(UInt8)(AUTH_ALG_OPEN       & 0xFF); f[n++]=(UInt8)(AUTH_ALG_OPEN       >> 8);
    f[n++]=(UInt8)(AUTH_SEQ_REQUEST    & 0xFF); f[n++]=(UInt8)(AUTH_SEQ_REQUEST    >> 8);
    f[n++]=(UInt8)(AUTH_STATUS_SUCCESS & 0xFF); f[n++]=(UInt8)(AUTH_STATUS_SUCCESS >> 8);
    return n;                                        /* 30 bytes */
}

/* ★★★★★ 8-31: DEAUTHENTICATE. THIS DRIVER HAS NEVER DONE IT, IN ANY BUILD.
 *
 * k122's join failed like this:
 *
 *     [8-3a] probe request  ★★ ACKED
 *     [8-3b] auth request   ★★ ACKED        <- the AP received it at layer 2
 *            [!!] NO AUTH RESPONSE within 500 ms
 *            by type: mgmt 1   subtypes seen: 5x1   (a probe response, no auth response)
 *
 * The AP acknowledges the frame and then says nothing. 802.11 requires a station to
 * deauthenticate when it leaves, and this driver never has -- not on EnetHAL_Stop, not on
 * EnetHAL_Close, not anywhere. So every session this project has ever run has left the access
 * point holding association state for our MAC, and an AP that already believes this station is
 * associated has every reason to ACK a fresh auth request and ignore it.
 *
 * ⚠ AND DEAUTHENTICATING ONLY ON CLOSE WOULD NOT FIX IT. A reboot, a crash or a power cut never
 *   reaches Close -- which is precisely how this machine has ended every session so far. The
 *   robust order is to deauthenticate BEFORE authenticating: it costs one frame, it is correct
 *   regardless of how the previous session ended, and it is what a station coming up with no
 *   memory of its past should do.
 *
 * Reason 3 is "Deauthenticated because sending STA is leaving (or has left) IBSS or ESS", which
 * is exactly the claim we are making. */
#define AP_DEAUTH_REASON_LEAVING 3

static UInt16 BuildDeauth(UInt8 *f,const UInt8 *myMac,const UInt8 *bssid)
{
    UInt16 n = 0;
    int i;
    f[0]=0xC0; f[1]=0x00;                           /* type 0 mgmt, subtype 12 deauthentication */
    f[2]=0x00; f[3]=0x00;                           /* duration -- the MAC fills it */
    for(i=0;i<6;i++) f[4+i]  = bssid[i];            /* addr1, the AP */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2, us */
    for(i=0;i<6;i++) f[16+i] = bssid[i];            /* addr3, the BSSID */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    n = 24;
    /* Reason code, little-endian on the wire, spelled out for the same reason BuildAuthRequest
     * spells its fields out. */
    f[n++]=(UInt8)(AP_DEAUTH_REASON_LEAVING & 0xFF);
    f[n++]=(UInt8)(AP_DEAUTH_REASON_LEAVING >> 8);
    return n;                                        /* 26 bytes */
}

static void PostTxFrameAt(UInt32 base,UInt8 *ringBase,int slot,
                          UInt32 hdrPhys,UInt16 hdrLen,UInt32 dataPhys,UInt16 dataLen)
{
    Op32FillDescriptorFull(ringBase,slot,  K8_TX_SLOTS,hdrPhys, hdrLen, 1,0,0);
    Op32FillDescriptorFull(ringBase,slot+1,K8_TX_SLOTS,dataPhys,dataLen,0,1,1);
    /* ★ THE FIX. Everything above is a store to ordinary memory; the line below is a store to
     * the BAR. Without this the second can reach the card first. */
    DmaPublish();
    ssb_w32(gBus.bar0,base+B43_DMA32_TXINDEX,
            (UInt32)(slot+2)*B43_DMADESC32_BYTES);
}


/* ★ 8-3c: the association request, moved from airport_rx.c with its capability constants.
 *
 * ⚠ THE RSNE IT BUILDS IS STATED, NOT DISCOVERED. The app's own comment at this call site
 *   says so: "we never parse the AP's beacon. The downgrade defence is the RSNE comparison
 *   in EAPOL message 3, which is Stage 7 and is NOT skipped." That is a deliberate scope
 *   decision with its defence named, carried across intact rather than re-argued.
 *
 * ⚠ The AID in the response has its top two bits set by the AP and they are NOT part of
 *   the AID -- mask with 0x3FFF. */

/* ⚠ BuildRsne came with BuildAssocRequest because the request embeds it -- eighth
 * extraction, eighth helper that had to follow its caller. The compiler finds these every
 * time, which is the cheap way to learn a move was incomplete. */
static UInt16 BuildRsne(UInt8 *p)
{
    UInt16 n = 0;
    p[n++]=0x30; p[n++]=0x14;                       /* element 48, length 20 */
    p[n++]=0x01; p[n++]=0x00;                       /* version 1 */
    p[n++]=0x00; p[n++]=0x0F; p[n++]=0xAC; p[n++]=0x04;   /* group    CCMP-128 */
    p[n++]=0x01; p[n++]=0x00;                             /* pairwise count 1 */
    p[n++]=0x00; p[n++]=0x0F; p[n++]=0xAC; p[n++]=0x04;   /* pairwise CCMP-128 */
    p[n++]=0x01; p[n++]=0x00;                             /* AKM count 1 */
    p[n++]=0x00; p[n++]=0x0F; p[n++]=0xAC; p[n++]=0x02;   /* AKM      PSK */
    p[n++]=0x00; p[n++]=0x00;                             /* RSN capabilities, no PMF */
    return n;
}

#define ASSOC_CAP_ESS        0x0001    /* infrastructure, not IBSS */
#define ASSOC_CAP_PRIVACY    0x0010    /* WEP/WPA bit -- required when the BSS uses encryption */
#define ASSOC_CAP_SHORTPRE   0x0020    /* short preamble, which 802.11g APs expect */
#define ASSOC_LISTEN_INTERVAL 1
#define ASSOC_STATUS_SUCCESS 0

static UInt16 BuildAssocRequest(UInt8 *f,const UInt8 *myMac,const UInt8 *bssid,
                                const char *ssid,int ssidLen,int wantRsn)
{
    UInt16 n = 0, cap;
    int i;
    f[0]=0x00; f[1]=0x00;                           /* type 0 mgmt, subtype 0 assoc request */
    f[2]=0x00; f[3]=0x00;                           /* duration */
    for(i=0;i<6;i++) f[4+i]  = bssid[i];            /* addr1, the AP */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2, us */
    for(i=0;i<6;i++) f[16+i] = bssid[i];            /* addr3, the BSSID */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    n = 24;

    /* k217: Privacy (the WEP/WPA bit) and the RSNE below belong to an encrypted BSS only. An open AP
     * can refuse a request that sets Privacy or carries an RSNE, so both are gated on wantRsn (0 for
     * an open network, 1 for WPA2-Personal). */
    cap = (UInt16)(ASSOC_CAP_ESS | ASSOC_CAP_SHORTPRE | (wantRsn ? ASSOC_CAP_PRIVACY : 0));
    f[n++]=(UInt8)(cap & 0xFF);                   f[n++]=(UInt8)(cap >> 8);
    f[n++]=(UInt8)(ASSOC_LISTEN_INTERVAL & 0xFF); f[n++]=(UInt8)(ASSOC_LISTEN_INTERVAL >> 8);

    f[n++]=0x00; f[n++]=(UInt8)ssidLen;             /* SSID, and it must be the real one here */
    for(i=0;i<ssidLen;i++) f[n++]=(UInt8)ssid[i];

    /* ★ k193: ADVERTISE 802.11g. Through k192 this listed only 1/2/5.5/11, so the AP treated us as an
     * 11b station: it sent to us at <= 11 Mbps, and a single 11b member can switch a whole g BSS into
     * protection (CTS-to-self before every OFDM frame, for every client). Supported Rates holds 8 --
     * the four CCK (1, 2 marked basic exactly as before) + OFDM 6/9/12/18 -- and Extended Supported
     * Rates (IE 50) the other four. IE 50 goes right after IE 1 and before the RSNE, per the
     * association-request order (SSID, Supported Rates, Extended Supported Rates, ..., RSN). */
    f[n++]=0x01; f[n++]=0x08;                       /* supported rates */
    f[n++]=0x82; f[n++]=0x84; f[n++]=0x0B; f[n++]=0x16;   /* 1, 2 basic; 5.5, 11 */
    f[n++]=0x0C; f[n++]=0x12; f[n++]=0x18; f[n++]=0x24;   /* 6, 9, 12, 18 */
    f[n++]=0x32; f[n++]=0x04;                       /* extended supported rates */
    f[n++]=0x30; f[n++]=0x48; f[n++]=0x60; f[n++]=0x6C;   /* 24, 36, 48, 54 */

    if(wantRsn) n = (UInt16)(n + BuildRsne(f+n));   /* k217: an open network sends no RSNE */
    return n;
}

#endif /* AP_TX_H */
