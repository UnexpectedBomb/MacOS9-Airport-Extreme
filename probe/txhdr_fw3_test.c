/* txhdr_fw3_test.c -- host suite for the fw3 (b43legacy) TX header builder in ap_tx.h.
 *
 * The fw3 txhdr is a DIFFERENT 82-byte layout from the fw4 106-byte one, and a wrong offset is
 * silent on the card (null PLCP / null destination / wrong antenna -> nothing decodable off the
 * air, exactly the failure Stage 5 lost weeks to). So the layout is pinned here, on the host,
 * against offsets re-derived BY HAND from struct b43legacy_txhdr_fw3 (reference/b43legacy/xmit.h:25-43)
 * -- independently of ap_tx.h's TXH_FW3_* macros, so a transcription slip fails here, not on a boot.
 *
 * ap_tx.h is not self-contained (it includes the ap_rx.h/ap_dma.h chain), so, like enet_test.c stubs
 * ap_ccmp.h, we CLAIM those headers' include guards and provide the handful of symbols ap_tx.h uses
 * from them. The CCK path exercises the REAL GeneratePlcpHdrCck and the REAL GenerateTxHdrFw3; only
 * the OFDM PLCP generator (not under test here) is stubbed.
 *
 * Build and run (from probe/, so it finds ap_tx.h; -Wno-unused-function because including the whole
 * ap_tx.h brings functions this focused test does not call):
 *   cc -Wall -Wno-unused-function -o txhdr_fw3_test txhdr_fw3_test.c && ./txhdr_fw3_test   (exit 0 = passed)
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;

/* Claim the guards so ap_tx.h's #include "ap_rx.h" / "ap_rate.h" become no-ops. */
#define AP_RX_H
#define AP_RATE_H

/* --- symbols those (now-skipped) headers would have provided --- */
/* ap_dma.h little-endian helpers (the real one-liners) + constants */
static UInt16 le16at(const UInt8*p){ return (UInt16)((UInt32)p[0] | ((UInt32)p[1]<<8)); }
static UInt32 le32at(const UInt8*p){ return (UInt32)p[0] | ((UInt32)p[1]<<8) | ((UInt32)p[2]<<16) | ((UInt32)p[3]<<24); }
static void   le16st(void *p,UInt16 v){ UInt8*b=(UInt8*)p; b[0]=(UInt8)(v&0xFF); b[1]=(UInt8)((v>>8)&0xFF); }
static void   le32st(void *p,UInt32 v){ UInt8*b=(UInt8*)p; b[0]=(UInt8)(v&0xFF); b[1]=(UInt8)((v>>8)&0xFF); b[2]=(UInt8)((v>>16)&0xFF); b[3]=(UInt8)((v>>24)&0xFF); }
#define IEEE80211_MAX_FRAME_LEN      2352UL
#define B43_DMA0_RX_FW351_FO         30UL
#define B43_DMA0_RX_FW351_BUFSIZE    (B43_DMA0_RX_FW351_FO + IEEE80211_MAX_FRAME_LEN)
/* ap_rate.h: stub the OFDM PLCP generator (OFDM content is not what this test covers) */
static void GeneratePlcpHdrOfdm(UInt8 *plcp,UInt16 octets,UInt8 ratecode){ int i; for(i=0;i<6;i++) plcp[i]=0; plcp[0]=ratecode; (void)octets; }
/* hardware + logging: referenced by DrainTxStatus etc. (compiled, never called here) */
typedef unsigned char Str255[256];
static struct { UInt32 chipId; UInt32 bar0; } gBus = { 0x4306UL, 0 };
static UInt32 ssb_r32(UInt32 base,UInt32 off){ (void)base;(void)off; return 0; }
static void   ssb_w32(UInt32 base,UInt32 off,UInt32 v){ (void)base;(void)off;(void)v; }
static void   SsbSpinUs(UInt32 us){ (void)us; }
static void   Say(const char*s){ (void)s; }
static void   Say1(const char*s,unsigned long v){ (void)s;(void)v; }
static void   SayH(const char*s,unsigned long v,int w){ (void)s;(void)v;(void)w; }
static void   Out(Str255 s){ (void)s; }
static void   PCat(Str255 d,const char*s){ (void)d;(void)s; }
static void   PCatHex(Str255 d,unsigned long v,int w){ (void)d;(void)v;(void)w; }
static void   PCatDec(Str255 d,unsigned long v){ (void)d;(void)v; }
/* TX-ring posting layer (PostTxFrameAt is compiled but never called by this test).
 * K8_TX_SLOTS is defined by ap_tx.h itself, so it is not stubbed here. */
#define B43_DMA32_TXINDEX    0x200UL
#define B43_DMADESC32_BYTES  8UL
static void Op32FillDescriptorFull(UInt8 *ringBase,int slot,int nslots,UInt32 phys,UInt16 len,int a,int b,int c){ (void)ringBase;(void)slot;(void)nslots;(void)phys;(void)len;(void)a;(void)b;(void)c; }
static void DmaPublish(void){}
/* gShimIsV3 lives in ap_phy_initg.h (stubbed out by the AP_RX_H guard above), so provide it here. */
static int gShimIsV3 = 0;

#include "ap_tx.h"

static int gFails = 0, gChecks = 0;
static void Check(const char *what, int ok){ gChecks++; if(!ok) gFails++;
    printf("  %-66s %s\n", what, ok ? "[ok]" : "[FAIL]"); }

int main(void)
{
    UInt8 txh[128], txh351[128];
    UInt8 frame[64];
    UInt16 fc = 0x00B0;      /* a management frame's frame-control (auth) */
    UInt16 durId = 0x013A;
    UInt8  addr1[6] = {0x9E,0x41,0xB2,0xCC,0x58,0xAD};
    int i;

    /* --- independent offsets, summed by hand from struct b43legacy_txhdr_fw3 (xmit.h:25-43) --- */
    printf("== fw3 txhdr offsets match the hand-derived struct layout ==\n");
    Check("TXH_FW3_MAC_CTL       == 0",  TXH_FW3_MAC_CTL       == 0);
    Check("TXH_FW3_MAC_FRAME_CTL == 4",  TXH_FW3_MAC_FRAME_CTL == 4);
    Check("TXH_FW3_PHY_CTL       == 8",  TXH_FW3_PHY_CTL       == 8);
    Check("TXH_FW3_TX_RECEIVER   == 26", TXH_FW3_TX_RECEIVER   == 26);  /* 10 + iv[16] */
    Check("TXH_FW3_PLCP_FB       == 40", TXH_FW3_PLCP_FB       == 40);  /* 26+6 +2 fes_fb +4 rts_plcp_fb +2 rts_dur_fb */
    Check("TXH_FW3_DUR_FB        == 44", TXH_FW3_DUR_FB        == 44);  /* 40 + plcp_fb(4) */
    Check("TXH_FW3_COOKIE        == 48", TXH_FW3_COOKIE        == 48);  /* 44+2 dur_fb +2 pad */
    Check("TXH_FW3_PLCP          == 76", TXH_FW3_PLCP          == 76);  /* 48+2 +2 scb +6 rts_plcp +18 rts_frame */
    Check("TXH_FW3_SIZE          == 82", TXH_FW3_SIZE          == 82);  /* 76 + plcp6(6) */
    Check("fw3 (82) is smaller than fw4 format_351 (106)", TXH_FW3_SIZE < 106);

    /* --- build a header and check the fields land where they belong --- */
    printf("== GenerateTxHdrFw3 field placement ==\n");
    memset(frame,0,sizeof(frame));
    le16st(frame+0,fc); le16st(frame+2,durId);
    for(i=0;i<6;i++) frame[4+i]=addr1[i];               /* addr1 is the receiver */
    memset(txh,0xEE,sizeof(txh));                       /* poison: prove every field is written */
    GenerateTxHdrFw3(txh,frame,20,1,0xC001,0,1);        /* ackReq=1 */

    Check("mac_ctl = ACK|HWSEQ|STMSDU (0x19)",
          le32at(txh+TXH_FW3_MAC_CTL) == (0x01UL|0x10UL|0x08UL));
    Check("mac_frame_ctl = frame's fc", le16at(txh+TXH_FW3_MAC_FRAME_CTL) == fc);
    Check("phy_ctl = ENC_CCK | ANTLAST (0x0300)",
          le16at(txh+TXH_FW3_PHY_CTL) == (0x0000|0x0300));
    Check("tx_receiver = addr1", memcmp(txh+TXH_FW3_TX_RECEIVER,addr1,6)==0);
    Check("dur_fb = frame's duration_id", le16at(txh+TXH_FW3_DUR_FB) == durId);
    Check("cookie = 0xC001", le16at(txh+TXH_FW3_COOKIE) == 0xC001);
    Check("main plcp rate code = CCK 1Mb (0x0A)", txh[TXH_FW3_PLCP]    == 0x0A);
    Check("fallback plcp rate code = CCK 1Mb (0x0A)", txh[TXH_FW3_PLCP_FB] == 0x0A);

    /* --- the subtle one: plcp_fb is a plcp4 field; writing it must NOT clobber dur_fb at +44 --- */
    printf("== plcp_fb (plcp4) does not overrun dur_fb ==\n");
    Check("dur_fb survived the plcp_fb write (still == duration_id)",
          le16at(txh+TXH_FW3_DUR_FB) == durId);
    Check("plcp_fb occupies exactly 40..43 (byte 44 is dur_fb low, not plcp)",
          txh[40]==0x0A && txh[44]==(UInt8)(durId&0xFF));

    /* --- nothing written past the 82-byte header --- */
    Check("byte 82 untouched (poison intact) -> builder wrote within 82",
          txh[82]==0xEE && txh[83]==0xEE);

    /* --- ackReq=0 clears the ACK bit (broadcast/multicast path) --- */
    memset(txh,0,sizeof(txh));
    GenerateTxHdrFw3(txh,frame,20,1,0xC000,0,0);        /* ackReq=0 */
    Check("ackReq=0 -> mac_ctl has HWSEQ|STMSDU, no ACK (0x18)",
          le32at(txh+TXH_FW3_MAC_CTL) == (0x10UL|0x08UL));

    /* --- the dispatch: with gShimIsV3, GenerateTxHdr351 must produce the fw3 header --- */
    printf("== GenerateTxHdr351 dispatches to fw3 when gShimIsV3 ==\n");
    memset(txh,0,sizeof(txh)); memset(txh351,0,sizeof(txh351));
    gShimIsV3 = 1;
    GenerateTxHdr351  (txh351,frame,20,1,0xC001,0,1);
    GenerateTxHdrFw3  (txh,   frame,20,1,0xC001,0,1);
    Check("GenerateTxHdr351 == GenerateTxHdrFw3 output (first 82 bytes) when v3",
          memcmp(txh351,txh,82)==0);
    gShimIsV3 = 0;

    /* --- k245: the length every post hands the DMA must be the header the builder wrote. k244 built the
     * 82-byte fw3 header and posted 106, so the microcode took 24 stray bytes as the start of every frame. --- */
    printf("== k245: TXH_SIZE_LIVE (the header descriptor's length) follows the generation ==\n");
    gShimIsV3 = 1; gTxhShift = 0;
    Check("v3: TXH_SIZE_LIVE == 82 (k244 posted TXH_SIZE_351 = 106)", TXH_SIZE_LIVE == 82);
    memset(txh,0xEE,sizeof(txh));
    GenerateTxHdr351(txh,frame,20,1,0xC001,0,1);
    Check("v3: the frame starts right after the header (byte TXH_SIZE_LIVE untouched)",
          txh[TXH_SIZE_LIVE] == 0xEE && txh[TXH_SIZE_LIVE - 6] == 0x0A);   /* main plcp rate code @76 */
    gShimIsV3 = 0; gTxhShift = 0;
    Check("fw4, ucode < 410: TXH_SIZE_LIVE == 106 (unchanged)", TXH_SIZE_LIVE == 106);
    gTxhShift = TXH_FMT_410_EXTRA;
    Check("fw4, ucode >= 410: TXH_SIZE_LIVE == 110 (unchanged)", TXH_SIZE_LIVE == 110);
    gTxhShift = 0;

    printf("\n%d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
