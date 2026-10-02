/* ring_test.c -- known-answer tests for ap_ring.h, run on the Mac, not the G4.
 *
 * Same reasoning as ilog_test.c: every failure mode of this ring is SILENT on the target. A
 * mis-sized offset, an off-by-one at the exact fill boundary, or a line accepted half-written all
 * produce a log that READS as a complete narrative. On this project a log that reads complete and
 * is not is the single most expensive object there is -- it is what the stale-log rule exists for.
 *
 * ⚠ The central claim is group 5: when the ring overflows, the EARLY lines survive intact. That
 *   is the whole justification for fill-and-stop over wrapping, so it gets pinned here rather
 *   than assumed.
 *
 * ⚠ The filler is 97 characters (98 bytes a line) so that AP_RING_BYTES does NOT divide evenly by
 *   it. A filler that divided evenly would always leave room == 0 and would silently skip the
 *   partial-room boundary, which is the case most likely to be wrong.
 *
 * Build and run:  cc -Wall -o ring_test ring_test.c && ./ring_test
 */
#include <stdio.h>
#include <string.h>

#include "ap_ring.h"

static int gFail = 0;

static void checkb(const char *what, int ok)
{
    if(ok){ printf("  [ok]   %s\n", what); return; }
    printf("  [FAIL] %s\n", what);
    gFail++;
}

/* Build a Pascal string from a C string. */
static void ps(unsigned char *d, const char *s)
{
    unsigned long n = (unsigned long)strlen(s);
    if(n > 255) n = 255;
    d[0] = (unsigned char)n;
    memcpy(d + 1, s, n);
}

static int pseq(const unsigned char *p, const char *s)
{
    unsigned long n = (unsigned long)strlen(s);
    return p[0] == (unsigned char)n && memcmp(p + 1, s, n) == 0;
}

static const char kFill97[] =
    "0123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789"
    "01234567890123456";                         /* 40 + 40 + 17 = 97 */

int main(void)
{
    unsigned char in[260], pad[260], out[260];
    unsigned long i, filled, used, room;

    printf("=== 1. a fresh ring is empty and answers nothing ===\n");
    ApRingReset();
    checkb("count is 0",          ApRingCount()   == 0);
    checkb("dropped is 0",        ApRingDropped() == 0);
    checkb("bytes used is 0",     ApRingBytes()   == 0);
    checkb("Get(0) refuses",      ApRingGet(0, out)  == 0);
    checkb("Get(99) refuses",     ApRingGet(99, out) == 0);

    printf("\n=== 2. a line goes in and comes back byte-identical ===\n");
    ApRingReset();
    ps(in, "PHY: attn 0x0045");
    ApRingPut(in);
    checkb("count is 1",          ApRingCount() == 1);
    checkb("bytes = len+1",       ApRingBytes() == (unsigned long)in[0] + 1u);
    memset(out, 0xAA, sizeof out);
    checkb("Get(0) copies",       ApRingGet(0, out) == 1);
    checkb("the bytes match",     pseq(out, "PHY: attn 0x0045"));
    checkb("Get(1) refuses",      ApRingGet(1, out) == 0);
    checkb("Get(NULL) refuses",   ApRingGet(0, (unsigned char *)0) == 0);
    ApRingPut((const unsigned char *)0);
    checkb("Put(NULL) is counted, not crashed",
           ApRingDropped() == 1 && ApRingCount() == 1);

    printf("\n=== 3. order is preserved, which is the whole point of a narration ===\n");
    ApRingReset();
    { static const char *lines[5] = { "one", "two", "three", "four", "five" };
      for(i = 0; i < 5; i++){ ps(in, lines[i]); ApRingPut(in); }
      checkb("count is 5", ApRingCount() == 5);
      for(i = 0; i < 5; i++){
          char what[64];
          ApRingGet(i, out);
          sprintf(what, "line %lu reads back as \"%s\"", i, lines[i]);
          checkb(what, pseq(out, lines[i]));
      } }

    printf("\n=== 4. the degenerate lengths, 0 and 255 ===\n");
    ApRingReset();
    in[0] = 0;                                   /* an empty line still costs its length byte */
    ApRingPut(in);
    checkb("empty line accepted",     ApRingCount() == 1);
    checkb("it costs exactly 1 byte", ApRingBytes() == 1);
    checkb("it reads back empty",     ApRingGet(0, out) == 1 && out[0] == 0);
    in[0] = 255;
    for(i = 1; i <= 255; i++) in[i] = (unsigned char)('A' + (i % 26));
    ApRingPut(in);
    checkb("255-byte line accepted",  ApRingCount() == 2);
    memset(out, 0, sizeof out);
    checkb("it reads back whole",     ApRingGet(1, out) == 1 &&
                                      out[0] == 255 && memcmp(out + 1, in + 1, 255) == 0);
    checkb("nothing dropped",         ApRingDropped() == 0);

    printf("\n=== 5. ★ overflow drops the TAIL and leaves the HEAD intact ===\n");
    ApRingReset();
    ps(in, kFill97);
    checkb("the filler is 97 bytes", in[0] == 97);
    while(ApRingCount() < AP_RING_LINES && ApRingBytes() + 98u <= AP_RING_BYTES) ApRingPut(in);
    filled = ApRingCount();
    checkb("filled without a single drop", ApRingDropped() == 0);
    checkb("something actually went in",   filled > 0);
    checkb("it was the byte limit that stopped it, not the line limit",
           filled < AP_RING_LINES);
    for(i = 0; i < 50; i++) ApRingPut(in);
    checkb("count did not grow past the fill", ApRingCount()   == filled);
    checkb("every refusal was counted",        ApRingDropped() == 50);
    checkb("bytes used never exceeded the buffer", ApRingBytes() <= AP_RING_BYTES);
    checkb("★ line 0 survived the overflow",   ApRingGet(0, out) == 1 && pseq(out, kFill97));
    checkb("★ the last accepted line survived",
           ApRingGet(filled - 1, out) == 1 && pseq(out, kFill97));
    checkb("one past it is still refused",     ApRingGet(filled, out) == 0);

    printf("\n=== 6. the fit boundary: refused WHOLE, never truncated ===\n");
    ApRingReset();
    ps(in, kFill97);
    while(ApRingCount() < AP_RING_LINES && ApRingBytes() + 98u <= AP_RING_BYTES) ApRingPut(in);
    used = ApRingBytes();
    room = AP_RING_BYTES - used;                 /* 0..97 -- 78 with today's constants */
    filled = ApRingCount();
    checkb("less than one filler line of room is left", room < 98u);
    checkb("and the gap is not zero, so the boundary is really tested", room > 0);
    pad[0] = (unsigned char)room;                /* needs room+1 bytes: ONE too many */
    for(i = 1; i <= room; i++) pad[i] = 'Z';
    ApRingPut(pad);
    checkb("a line one byte too long is refused", ApRingCount()   == filled);
    checkb("and counted as dropped",              ApRingDropped() == 1);
    checkb("and consumed no bytes at all",        ApRingBytes()   == used);
    checkb("the previous line is unharmed",
           ApRingGet(filled - 1, out) == 1 && pseq(out, kFill97));
    pad[0] = (unsigned char)(room - 1);          /* needs exactly room bytes */
    ApRingPut(pad);
    checkb("a line that exactly fills is accepted", ApRingCount() == filled + 1);
    checkb("the ring is now exactly full",          ApRingBytes() == AP_RING_BYTES);
    checkb("it reads back whole",
           ApRingGet(filled, out) == 1 && out[0] == (unsigned char)(room - 1));

    printf("\n=== 7. the line limit is enforced independently of the byte limit ===\n");
    ApRingReset();
    ps(in, "x");                                 /* 2 bytes -- bytes will never run out */
    for(i = 0; i < AP_RING_LINES; i++) ApRingPut(in);
    checkb("exactly AP_RING_LINES accepted", ApRingCount()   == AP_RING_LINES);
    checkb("no drops up to the limit",       ApRingDropped() == 0);
    checkb("bytes are nowhere near full",    ApRingBytes()   <  AP_RING_BYTES / 2);
    ApRingPut(in);
    checkb("the next one is refused",        ApRingCount()   == AP_RING_LINES);
    checkb("and counted",                    ApRingDropped() == 1);
    checkb("the last valid index still reads",
           ApRingGet(AP_RING_LINES - 1, out) == 1 && pseq(out, "x"));

    printf("\n=== 8. reset really resets ===\n");
    ApRingReset();
    checkb("count cleared",      ApRingCount()   == 0);
    checkb("dropped cleared",    ApRingDropped() == 0);
    checkb("bytes cleared",      ApRingBytes()   == 0);
    checkb("old lines are gone", ApRingGet(0, out) == 0);
    ps(in, "after reset");
    ApRingPut(in);
    checkb("it accepts again",
           ApRingCount() == 1 && ApRingGet(0, out) && pseq(out, "after reset"));

    printf("\n");
    if(gFail == 0) printf("ALL RING TESTS PASSED.\n");
    else           printf("%d TEST(S) FAILED -- do not build this into a driver.\n", gFail);
    return gFail ? 1 : 0;
}
