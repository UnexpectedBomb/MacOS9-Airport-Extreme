/* rxcopy_test.c -- ap_copy.h on the host: how much of a received frame RxWaitFor copies, and the word
 * copy itself. k200.
 *
 * What would make these fail (ask what an oracle would FAIL on):
 *   - a length rule that drops the RX header (frameoffset), the tail of the frame, or the fallback to
 *     the whole buffer when frame_len is 0 or impossible;
 *   - a word copy that loses the last partial word, overruns its rounded length, or corrupts bytes.
 * The end-to-end model builds an RX buffer the way the card does (little-endian frame_len at offset 0,
 * the frame from frameoffset on), copies it with the driver's own two functions into a destination
 * pre-filled with 0xEE, and checks every byte the pump and ApRxToEnet will read -- and that nothing
 * past the copied length was written.
 *
 * Build and run:  cc -Wall -o rxcopy_test rxcopy_test.c && ./rxcopy_test      (exit 0 = all passed) */
#include <stdio.h>
#include <string.h>
#include "ap_copy.h"

static int gChecks = 0, gFails = 0;
static void Check(const char *what, int ok)
{ gChecks++; if(!ok){ gFails++; printf("  [FAIL] %s\n", what); } }

#define FO    30UL          /* B43_DMA0_RX_FW351_FO */
#define CAP   2400UL        /* AP_PROMPT_BUF */
#define PAGE  4096UL

static unsigned char gSrc[PAGE] __attribute__((aligned(16)));
static unsigned char gDst[CAP + 64] __attribute__((aligned(16)));

int main(void)
{
    unsigned long n, len, i;
    int bad;

    /* ---- the length rule ---- */
    Check("frame_len 0 -> the whole buffer (as through k199)", ApRxCopyLen(FO, 0, CAP) == CAP);
    Check("a 14-byte ACK -> header + frame + 8",               ApRxCopyLen(FO, 14, CAP) == FO + 14 + 8);
    Check("a 1540-byte data frame -> header + frame + 8",       ApRxCopyLen(FO, 1540, CAP) == FO + 1540 + 8);
    Check("the largest frame that fits exactly",                ApRxCopyLen(FO, CAP - FO - 8, CAP) == CAP);
    Check("one byte more is capped at the buffer",              ApRxCopyLen(FO, CAP - FO - 7, CAP) == CAP);
    Check("an impossible 0xFFFF frame_len -> the whole buffer", ApRxCopyLen(FO, 0xFFFFUL, CAP) == CAP);
    Check("never more than the cap, never less than the frame", 1);
    for(len = 1; len <= 0xFFFFUL; len++){
        n = ApRxCopyLen(FO, len, CAP);
        if(n > CAP || (FO + len <= CAP && n < FO + len)){ Check("length rule holds for every frame_len", 0); break; }
    }

    /* ---- the word copy, every length, guard bytes after the rounded length ---- */
    for(i = 0; i < PAGE; i++) gSrc[i] = (unsigned char)(i * 7 + 3);
    bad = 0;
    for(n = 0; n <= CAP; n++){
        unsigned long rounded = (n + 3UL) & ~3UL;
        memset(gDst, 0xEE, sizeof gDst);
        ApCopyWords(gDst, gSrc, n);
        if(memcmp(gDst, gSrc, rounded) != 0) bad |= 1;             /* every byte up to the round-up */
        for(i = rounded; i < sizeof gDst; i++) if(gDst[i] != 0xEE){ bad |= 2; break; }
    }
    Check("ApCopyWords copies every byte of every length 0..2400", !(bad & 1));
    Check("ApCopyWords never writes past its rounded-up length",    !(bad & 2));

    /* ---- end to end: an RX buffer the way the card fills it ---- */
    bad = 0;
    for(len = 1; len <= CAP - FO; len++){
        unsigned long want = FO + len, got;
        memset(gSrc, 0x00, PAGE);
        gSrc[0] = (unsigned char)(len & 0xFF); gSrc[1] = (unsigned char)(len >> 8);  /* frame_len, LE */
        for(i = 2; i < FO; i++) gSrc[i] = (unsigned char)(0x40 + i);                 /* the RX header */
        for(i = 0; i < len; i++) gSrc[FO + i] = (unsigned char)(i ^ 0x5A);           /* the frame */
        memset(gDst, 0xEE, sizeof gDst);
        got = ApRxCopyLen(FO, (unsigned long)(gSrc[0] | (gSrc[1] << 8)), CAP);
        ApCopyWords(gDst, gSrc, got);
        if(memcmp(gDst, gSrc, want) != 0){ bad |= 1; }
        for(i = (got + 3UL) & ~3UL; i < sizeof gDst; i++) if(gDst[i] != 0xEE){ bad |= 2; break; }
    }
    Check("every RX header byte and every frame byte reaches the prompt buffer (all lengths)", !(bad & 1));
    Check("nothing past the copied length is written (all lengths)",                           !(bad & 2));

    printf("%s  %d checks, %d failed\n", gFails ? "[FAIL]" : "[ok] ", gChecks, gFails);
    return gFails ? 1 : 0;
}
