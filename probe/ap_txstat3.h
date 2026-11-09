/* ap_txstat3.h -- k245: the v3 (b43legacy) TRANSMIT STATUS, as the BCM4306/2's microcode delivers it.
 *
 * ★ WHY IT EXISTS. On an 802.11 core rev <= 4 the v3 microcode does not post TX status to the XMITSTAT
 *   FIFO that b43 reads (and this driver has read since k7). It writes each frame's status as a 16-byte
 *   record into DMA RX engine 3 -- b43legacy's rx_ring3, "only on core.rev < 5" (reference/b43legacy/
 *   b43legacy.h:531, dma.c:814-819) -- and b43legacy decodes the record in b43legacy_handle_hwtxstatus
 *   (xmit.c:599-619). k244 never armed that ring, so on the BCM4306/2 every "ACKED = 0" meant UNKNOWN,
 *   and the join's TX-prove gate refused to authenticate on it (the tester's k244 log, 2026-11-08).
 *
 * ★ ONE RECORD BECOMES THE SAME (XMITSTAT_0, XMITSTAT_1) PAIR THE fw4 MICROCODE WOULD HAVE POSTED, so every
 *   consumer this driver already has -- DrainTxStatus's per-frame decode and cookie match, the k193 ARF
 *   (ApTxRateFeedback) -- reads a v3 status unchanged. The mapping is exact, not approximate. b43legacy
 *   decodes the record as
 *       frame_count = count >> 4            rts_count = count & 0x0F
 *       tmp = flags << 1:  supp_reason = (tmp & 0x1C) >> 2,  pm = tmp & 0x80,  intermediate = tmp & 0x40,
 *                          for_ampdu = tmp & 0x20,           acked = tmp & 0x02
 *   and decodes XMITSTAT_0 (handle_irq_transmit_status, main.c:704-730) as
 *       frame_count = (v0 & 0xF000) >> 12   rts_count = (v0 & 0x0F00) >> 8
 *       supp_reason = (v0 & 0x001C) >> 2,   pm = v0 & 0x0080,  intermediate = v0 & 0x0040,
 *       for_ampdu = v0 & 0x0020,            acked = v0 & 0x0002,  cookie = v0 >> 16,  valid = v0 & 0x0001
 *   So v0 = cookie << 16 | count << 8 | ((flags << 1) & 0xFE) | 1 reads back every field identically. The
 *   & 0xFE drops flags bit 7, exactly as b43legacy's u8 tmp does. XMITSTAT_1 is seq | phy_stat << 16 in both.
 *
 * PURE: plain integer types, no Toolbox, no MMIO -- host-tested by txstat3_test.c.
 */
#ifndef AP_TXSTAT3_H
#define AP_TXSTAT3_H

#define AP_TS3_SLOTS      64u    /* B43legacy_RXRING_SLOTS (dma.h:94) */
#define AP_TS3_BUFSZ      16u    /* B43legacy_DMA3_RX_BUFFERSIZE (dma.h:96) */
#define AP_TS3_FRAMEOFF    0u    /* B43legacy_DMA3_RX_FRAMEOFFSET (dma.h:89) */
/* The ring lives in ONE page: 64 descriptors x 8 bytes at 0..511, the 64 records x 16 bytes at
 * 2048..3071. One page is physically contiguous by construction, so a record's physical address is
 * the page's plus its offset -- no per-buffer GetPhysical. */
#define AP_TS3_RECOFF   2048u

/* struct b43legacy_hwtxstatus (xmit.h:118-127):
 *   PAD_BYTES(4); __le16 cookie; u8 flags; u8 count; PAD_BYTES(2); __le16 seq; u8 phy_stat; PAD_BYTES(1) */
#define AP_TS3_COOKIE      4u
#define AP_TS3_FLAGS       6u
#define AP_TS3_COUNT       7u
#define AP_TS3_SEQ        10u
#define AP_TS3_PHYSTAT    12u

/* A record whose cookie is still 0 has not been written. b43legacy zeroes the cookie when it posts the
 * buffer (setup_rx_descbuffer, dma.c:460-461) and dma_rx waits for it to change (dma.c:1241-1247). Every
 * cookie this driver stamps is 0xC000 or above, so 0 is never a real one. */
static int ApTs3Pending(const UInt8 *rec)
{
    return (rec[AP_TS3_COOKIE] | rec[AP_TS3_COOKIE + 1u]) != 0;
}

/* The record as XMITSTAT_0. Bit 0 = "valid", as the fw4 FIFO sets it on every entry it hands out. */
static UInt32 ApTs3V0(const UInt8 *rec)
{
    UInt32 cookie = (UInt32)rec[AP_TS3_COOKIE] | ((UInt32)rec[AP_TS3_COOKIE + 1u] << 8);
    return (cookie << 16) | ((UInt32)rec[AP_TS3_COUNT] << 8)
         | (((UInt32)rec[AP_TS3_FLAGS] << 1) & 0xFEu) | 0x01u;
}

/* The record as XMITSTAT_1: seq in bits 0-15, phy_stat in bits 16-23. */
static UInt32 ApTs3V1(const UInt8 *rec)
{
    return ((UInt32)rec[AP_TS3_PHYSTAT] << 16)
         | (UInt32)rec[AP_TS3_SEQ] | ((UInt32)rec[AP_TS3_SEQ + 1u] << 8);
}

#endif /* AP_TXSTAT3_H */
