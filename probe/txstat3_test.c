/* txstat3_test.c -- k245 host suite for the v3 (b43legacy) TX-status path: ap_txstat3.h's record ->
 * XMITSTAT mapping, and ap_tx.h's ring-3 consumer (ApTs3Pop).
 *
 * ★ WHAT IT PROVES, AND AGAINST WHAT. The driver feeds a ring-3 record to consumers written for the fw4
 * XMITSTAT pair. That is only right if decoding the synthesised pair the XMITSTAT way gives the same fields
 * as decoding the record the b43legacy way. Both decoders are re-typed HERE from the reference source --
 * independently of ap_txstat3.h -- and compared over EVERY flags byte x EVERY count byte (65,536 records):
 *   record decode   = b43legacy_handle_hwtxstatus   (reference/b43legacy/xmit.c:599-619)
 *   XMITSTAT decode = handle_irq_transmit_status    (reference/b43legacy/main.c:704-730)
 * Then the consumer: order, wrap, a drifted cursor, consumption, and "not armed" -- the cases the card
 * would otherwise be the first to find.
 *
 * ap_tx.h is not self-contained (it includes the ap_rx.h/ap_dma.h chain), so, like txhdr_fw3_test.c, we
 * CLAIM those headers' include guards and provide the handful of symbols ap_tx.h uses from them.
 *
 * Build and run (from probe/):
 *   cc -Wall -Wno-unused-function -o txstat3_test txstat3_test.c && ./txstat3_test   (exit 0 = passed)
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;

/* Claim the guards so ap_tx.h's #include "ap_rx.h" / "ap_rate.h" become no-ops. */
#define AP_RX_H
#define AP_RATE_H

/* --- symbols those (now-skipped) headers would have provided (as in txhdr_fw3_test.c) --- */
static UInt16 le16at(const UInt8*p){ return (UInt16)((UInt32)p[0] | ((UInt32)p[1]<<8)); }
static void   le16st(void *p,UInt16 v){ UInt8*b=(UInt8*)p; b[0]=(UInt8)(v&0xFF); b[1]=(UInt8)((v>>8)&0xFF); }
static void   le32st(void *p,UInt32 v){ UInt8*b=(UInt8*)p; b[0]=(UInt8)(v&0xFF); b[1]=(UInt8)((v>>8)&0xFF); b[2]=(UInt8)((v>>16)&0xFF); b[3]=(UInt8)((v>>24)&0xFF); }
#define IEEE80211_MAX_FRAME_LEN      2352UL
#define B43_DMA0_RX_FW351_FO         30UL
#define B43_DMA0_RX_FW351_BUFSIZE    (B43_DMA0_RX_FW351_FO + IEEE80211_MAX_FRAME_LEN)
static void GeneratePlcpHdrOfdm(UInt8 *plcp,UInt16 octets,UInt8 ratecode){ int i; for(i=0;i<6;i++) plcp[i]=0; plcp[0]=ratecode; (void)octets; }
typedef unsigned char Str255[256];
static struct { UInt32 chipId; UInt32 bar0; } gBus = { 0x4306UL, 0 };
static UInt32 ssb_r32(UInt32 base,UInt32 off){ (void)base;(void)off; return 0; }   /* fw4 FIFO: always empty */
static void   ssb_w32(UInt32 base,UInt32 off,UInt32 v){ (void)base;(void)off;(void)v; }
static void   SsbSpinUs(UInt32 us){ (void)us; }
static void   Say(const char*s){ (void)s; }
static void   Say1(const char*s,unsigned long v){ (void)s;(void)v; }
static void   SayH(const char*s,unsigned long v,int w){ (void)s;(void)v;(void)w; }
static void   Out(Str255 s){ (void)s; }
static void   PCat(Str255 d,const char*s){ (void)d;(void)s; }
static void   PCatHex(Str255 d,unsigned long v,int w){ (void)d;(void)v;(void)w; }
static void   PCatDec(Str255 d,unsigned long v){ (void)d;(void)v; }
#define B43_DMA32_TXINDEX    0x200UL
#define B43_DMADESC32_BYTES  8UL
static void Op32FillDescriptorFull(UInt8 *ringBase,int slot,int nslots,UInt32 phys,UInt16 len,int a,int b,int c){ (void)ringBase;(void)slot;(void)nslots;(void)phys;(void)len;(void)a;(void)b;(void)c; }
static void DmaPublish(void){}
static int gShimIsV3 = 0;

#include "ap_tx.h"

static int gFails = 0, gChecks = 0;
static void Check(const char *what, int ok){ gChecks++; if(!ok) gFails++;
    printf("  %-70s %s\n", what, ok ? "[ok]" : "[FAIL]"); }

/* ---- the two reference decoders, re-typed from the reference source (NOT from ap_txstat3.h) ---- */
typedef struct { UInt32 cookie, seq, phy_stat, frame_count, rts_count, supp_reason,
                 pm_indicated, intermediate, for_ampdu, acked; } Stat;

/* b43legacy_handle_hwtxstatus (xmit.c:599-619), on struct b43legacy_hwtxstatus (xmit.h:118-127):
 * PAD(4) cookie(le16 @4) flags(u8 @6) count(u8 @7) PAD(2) seq(le16 @10) phy_stat(u8 @12) PAD(1) */
static void RefDecodeRecord(const UInt8 *hw, Stat *s)
{
    UInt8 tmp;
    s->cookie   = (UInt32)hw[4] | ((UInt32)hw[5] << 8);
    s->seq      = (UInt32)hw[10] | ((UInt32)hw[11] << 8);
    s->phy_stat = hw[12];
    tmp = hw[7];
    s->frame_count = (tmp >> 4);
    s->rts_count   = (tmp & 0x0F);
    tmp = (UInt8)(hw[6] << 1);
    s->supp_reason  = ((tmp & 0x1C) >> 2);
    s->pm_indicated = !!(tmp & 0x80);
    s->intermediate = !!(tmp & 0x40);
    s->for_ampdu    = !!(tmp & 0x20);
    s->acked        = !!(tmp & 0x02);
}

/* handle_irq_transmit_status (main.c:704-730): the XMITSTAT_0 / XMITSTAT_1 pair. */
static int RefDecodeXmitstat(UInt32 v0, UInt32 v1, Stat *s)
{
    UInt16 tmp;
    if(!(v0 & 0x00000001)) return 0;                 /* "if (!(v0 & 0x00000001)) break;" */
    s->cookie   = (v0 >> 16);
    s->seq      = (v1 & 0x0000FFFF);
    s->phy_stat = ((v1 & 0x00FF0000) >> 16);
    tmp = (UInt16)(v0 & 0x0000FFFF);
    s->frame_count  = ((tmp & 0xF000) >> 12);
    s->rts_count    = ((tmp & 0x0F00) >> 8);
    s->supp_reason  = ((tmp & 0x001C) >> 2);
    s->pm_indicated = !!(tmp & 0x0080);
    s->intermediate = !!(tmp & 0x0040);
    s->for_ampdu    = !!(tmp & 0x0020);
    s->acked        = !!(tmp & 0x0002);
    return 1;
}

static void MakeRecord(UInt8 *r, UInt16 cookie, UInt8 flags, UInt8 count, UInt16 seq, UInt8 phy)
{
    memset(r, 0xA5, AP_TS3_BUFSZ);                  /* the PAD bytes hold junk: the mapping must ignore them */
    r[4] = (UInt8)(cookie & 0xFF); r[5] = (UInt8)(cookie >> 8);
    r[6] = flags; r[7] = count;
    r[10] = (UInt8)(seq & 0xFF); r[11] = (UInt8)(seq >> 8);
    r[12] = phy;
}

static UInt8 gRing[AP_TS3_SLOTS * AP_TS3_BUFSZ];

int main(void)
{
    UInt8 rec[AP_TS3_BUFSZ];
    UInt32 f, c, v0 = 0, v1 = 0, mism = 0, firstBad = 0xFFFFFFFFu;
    Stat a, b;

    printf("== the record -> XMITSTAT mapping, every flags x every count byte ==\n");
    for(f = 0; f < 256; f++) for(c = 0; c < 256; c++){
        MakeRecord(rec, 0xC0E3, (UInt8)f, (UInt8)c, 0x1234, 0x5A);
        RefDecodeRecord(rec, &a);
        if(!RefDecodeXmitstat(ApTs3V0(rec), ApTs3V1(rec), &b) || memcmp(&a, &b, sizeof(Stat)) != 0){
            if(firstBad == 0xFFFFFFFFu) firstBad = (f << 8) | c;
            mism++; } }
    Check("65,536 records: XMITSTAT decode of the pair == b43legacy decode of the record", mism == 0);
    if(mism) printf("    first mismatch at flags 0x%02X count 0x%02X (%u total)\n",
                    (unsigned)(firstBad >> 8), (unsigned)(firstBad & 0xFF), (unsigned)mism);

    MakeRecord(rec, 0xC000, 0x01, 0x10, 0x0001, 0x00);   /* acked, frame_count 1 */
    v0 = ApTs3V0(rec); v1 = ApTs3V1(rec);
    Check("v0 bit 0 (valid) is set: the FIFO-style consumers accept it", (v0 & 1u) == 1u);
    Check("cookie 0xC000 -> v0 >> 16 (little-endian record, byte order right)", (v0 >> 16) == 0xC000u);
    Check("flags bit 0 -> acked (v0 bit 1)", (v0 & 0x2u) != 0);
    Check("count 0x10 -> frame_count 1 (v0 bits 12-15)", ((v0 >> 12) & 0xFu) == 1u);
    MakeRecord(rec, 0xC001, 0x00, 0x70, 0xBEEF, 0x3C);   /* not acked, 7 attempts (the retry limit) */
    v0 = ApTs3V0(rec); v1 = ApTs3V1(rec);
    Check("not acked + count 0x70 -> acked 0, frame_count 7", (v0 & 0x2u) == 0 && ((v0 >> 12) & 0xFu) == 7u);
    Check("seq 0xBEEF / phy_stat 0x3C land in v1 like XMITSTAT_1", (v1 & 0xFFFFu) == 0xBEEFu && ((v1 >> 16) & 0xFFu) == 0x3Cu);
    MakeRecord(rec, 0xC020, 0x06, 0x10, 0, 0);           /* supp_reason 3 (PREV): flags bits 1-3 */
    Check("flags 0x06 -> supp_reason 3, acked 0", ((ApTs3V0(rec) >> 2) & 7u) == 3u && (ApTs3V0(rec) & 2u) == 0);
    MakeRecord(rec, 0xC020, 0x80, 0x10, 0, 0);           /* flags bit 7: b43legacy's u8 tmp drops it */
    Check("flags bit 7 is dropped (no leak into rts_count)", ((ApTs3V0(rec) >> 8) & 0xFu) == 0u);

    printf("== empty / pending ==\n");
    memset(rec, 0, sizeof rec);
    Check("cookie 0 -> not pending (b43legacy's own 'not yet written' test)", !ApTs3Pending(rec));
    rec[4] = 0x01; Check("cookie 0x0001 -> pending", ApTs3Pending(rec));
    rec[4] = 0; rec[5] = 0x01; Check("cookie 0x0100 -> pending (high byte alone)", ApTs3Pending(rec));

    printf("== the ring-3 consumer (ApTs3Pop) ==\n");
    memset(gRing, 0, sizeof gRing);
    gTs3Rec = gRing; gTs3Armed = 0; gTs3Cursor = 0;
    MakeRecord(gRing + 0 * AP_TS3_BUFSZ, 0xC000, 0x01, 0x10, 1, 0);
    Check("not armed -> pops nothing, even with a record pending", ApTs3Pop(&v0, &v1, 1) == 0);
    gTs3Armed = 1;
    MakeRecord(gRing + 1 * AP_TS3_BUFSZ, 0xC00D, 0x00, 0x70, 2, 0);
    MakeRecord(gRing + 2 * AP_TS3_BUFSZ, 0xC001, 0x01, 0x20, 3, 0);
    Check("pop 1 (cheap) = slot 0, cookie 0xC000", ApTs3Pop(&v0, &v1, 0) == 1 && (v0 >> 16) == 0xC000u);
    Check("pop 2 (cheap) = slot 1, cookie 0xC00D", ApTs3Pop(&v0, &v1, 0) == 1 && (v0 >> 16) == 0xC00Du);
    Check("pop 3 (cheap) = slot 2, cookie 0xC001", ApTs3Pop(&v0, &v1, 0) == 1 && (v0 >> 16) == 0xC001u);
    Check("pop 4 -> nothing left; cursor at 3", ApTs3Pop(&v0, &v1, 1) == 0 && gTs3Cursor == 3u);
    Check("popped records are empty again (cookie zeroed)", !ApTs3Pending(gRing) && !ApTs3Pending(gRing + 16) && !ApTs3Pending(gRing + 32));
    Check("acked counted only for acked records (2 of 3)", gTs3Acked == 2u && gTs3Popped == 3u);

    /* wrap: the engine writes 63 then 0 */
    gTs3Cursor = 63;
    MakeRecord(gRing + 63 * AP_TS3_BUFSZ, 0xC030, 0x01, 0x10, 4, 0);
    MakeRecord(gRing +  0 * AP_TS3_BUFSZ, 0xC040, 0x01, 0x10, 5, 0);
    Check("wrap: slot 63 first", ApTs3Pop(&v0, &v1, 0) == 1 && (v0 >> 16) == 0xC030u);
    Check("wrap: then slot 0, cursor at 1", ApTs3Pop(&v0, &v1, 0) == 1 && (v0 >> 16) == 0xC040u && gTs3Cursor == 1u);

    /* drift: a re-armed engine restarted at 0 while the cursor sat at 10 */
    gTs3Cursor = 10;
    MakeRecord(gRing + 0 * AP_TS3_BUFSZ, 0xC0E3, 0x01, 0x10, 6, 0);
    Check("drift: the cheap pop (interrupt pump) does not look past the cursor", ApTs3Pop(&v0, &v1, 0) == 0);
    Check("drift: the full pop (task level) finds slot 0 round the ring", ApTs3Pop(&v0, &v1, 1) == 1 && (v0 >> 16) == 0xC0E3u);
    Check("drift: counted as skipped; cursor resynchronised to 1", gTs3Skipped == 1u && gTs3Cursor == 1u);

    printf("== DrainTxStatus on the v3 path ==\n");
    gShimIsV3 = 1; gTs3Cursor = 5;
    MakeRecord(gRing + 5 * AP_TS3_BUFSZ, 0xC00D, 0x00, 0x30, 7, 0);   /* a stale deauth status, not ours */
    MakeRecord(gRing + 6 * AP_TS3_BUFSZ, 0xC001, 0x01, 0x10, 8, 0);   /* ours, acked */
    Check("returns OUR frame's ACK (1), past another cookie's status", DrainTxStatus(0xC001, 50) == 1);
    Check("and reports that our status was SEEN", gTxStatSawWanted == 1);
    MakeRecord(gRing + 7 * AP_TS3_BUFSZ, 0xC020, 0x00, 0x70, 9, 0);   /* ours, NOT acked, 7 tries */
    Check("a not-acked status for ours -> 0, and SEEN (a real answer)", DrainTxStatus(0xC020, 50) == 0 && gTxStatSawWanted == 1);
    Check("no status at all -> 0 and NOT seen (UNKNOWN, not a refusal)", DrainTxStatus(0xC030, 30) == 0 && gTxStatSawWanted == 0);
    gTs3Armed = 0;
    Check("ring never armed -> 0 at once, NOT seen, counted as skipped", DrainTxStatus(0xC030, 30) == 0 && gTxStatSawWanted == 0 && gTxStatV3Skipped == 1u);
    gShimIsV3 = 0;

    printf("\n%d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
