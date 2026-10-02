/* ap_copy.h -- word copies to and from DMA memory, and how much of a received frame to copy. k200.
 *
 * ★★★ WHY. k199's per-frame timing (the first build that could see it) put the receive ring walk at
 * ~565 us for EVERY frame -- 9474 of 9481 samples between 0.5 and 1 ms, beacons included -- while the
 * AES that k199 made 43x faster now costs 37 us to decrypt. RxWaitFor copied the WHOLE 2400-byte
 * prompt buffer out of the DMA page, one byte per load, whatever the frame's length, and rescanned all
 * 32 slots' poison twice per call. ~2950 byte loads from DMA memory at ~190 ns each is the 565 us, and
 * an empty call (the two scans alone, ~520 loads) is the ~95 us every TX-status interrupt paid.
 * The same shape shows on transmit: the live encrypt, which writes the ring buffer, took 150 us
 * against the benchmark's 73 us into a static buffer.
 * So DMA memory behaves as if every access is a bus transaction. WHY is not settled -- the pools are
 * ordinary NewPtrSysClear + LockMemory, which should be cacheable -- and k200's bring-up probe
 * (ApShimDmaMemProbe) measures ns per access in each direction to settle it. Either way the cure is
 * the same: touch DMA memory as few times as possible.
 *   - copy only the frame, not the buffer: frameoffset + frame_len (+8), capped at the buffer;
 *   - copy in 32-bit words: a quarter of the transactions;
 *   - transmit: encrypt into cached memory, then copy the result into the ring in words.
 *
 * Pure and self-contained (no Mac headers), so rxcopy_test.c checks it on the host.
 * ⚠ Both pointers must be 4-byte aligned: DMA buffers are page-aligned (+ a 128-byte header area on
 *   transmit), and the cached side is declared __attribute__((aligned(16))). The copy rounds UP to a
 *   whole word, so the destination must have room for it -- every buffer here is a multiple of 4. */
#ifndef AP_COPY_H
#define AP_COPY_H

typedef unsigned int ApCopyW;
typedef char ApCopyWMustBe32Bits[(sizeof(ApCopyW) == 4) ? 1 : -1];

/* How many bytes of a received buffer are worth copying: the RX header up to frameoffset, then the
 * frame. frame_len 0 (the header not written yet, or garbage) or a length past the buffer means "we
 * do not know": copy all of it, exactly as through k199, so every downstream check sees what it saw. */
static unsigned long ApRxCopyLen(unsigned long frameoffset, unsigned long frameLen, unsigned long cap)
{
    unsigned long n;
    if(frameLen == 0UL) return cap;
    n = frameoffset + frameLen + 8UL;                /* +8: slack for anything that peeks one word past */
    return (n > cap) ? cap : n;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif
/* nBytes rounded UP to whole words. A plain copy: the caller has already decided the source is
 * complete (the poison check, or our own encrypt), and a call boundary is a compiler barrier. */
static void ApCopyWords(void *dst, const void *src, unsigned long nBytes)
{
    ApCopyW *d = (ApCopyW *)dst;
    const ApCopyW *s = (const ApCopyW *)src;
    unsigned long w = (nBytes + 3UL) >> 2;
    while(w--) *d++ = *s++;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

#endif /* AP_COPY_H */
