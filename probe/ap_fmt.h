/* ap_fmt.h -- the SINK-AGNOSTIC half of the logging, split out of ap_log.h by Stage 8-2d-1.
 *
 * ★ WHY A SECOND SPLIT. 8-2d-0 separated logging from HARDWARE, so the driver could take
 * ap_bringup.h without dragging the File Manager in with it. This separates FORMATTING from the
 * SINK, because the driver needs the formatters verbatim and must supply its own sink: it has no
 * log file, and by the time the RX path arrives it will run below task level, where FSWrite is a
 * silent hard hang with no NMI. See "Never call the File Manager below task level" in CLAUDE.md.
 *
 * ⚠ THE INTERCEPTION POINT IS Out(), AND THAT WAS NOT THE OBVIOUS CHOICE.
 *   ap_ilog.h stores POINTERS to string literals, which maps perfectly onto Say("literal") and
 *   Say1("literal", v) -- 642 of the 736 call sites. It was the natural sink to reach for.
 *   But ap_phy_initg.h and ap_bringup.h also build COMPOSITE lines with PCat/PCatHex and call
 *   Out() directly, 34 times: the register dumps and the multi-value lines. Those have no literal
 *   to point at -- by the time Out() sees them they are bytes on the caller's stack, and a
 *   pointer ring would have stored an address that is garbage before anything drains it.
 *
 *   Intercepting at Say() would therefore have dropped exactly the 34 densest lines in the PHY
 *   narration, and left a log that LOOKED complete. That is a failure mode this project has paid
 *   for before: a confident reading of a log that was missing the deciding line. So the sink is
 *   Out(), the driver's Out() COPIES bytes, and every path -- Say, SayH, Say1 and the 34 direct
 *   Out() calls -- goes through one mechanism.
 *
 * ⚠ EVERYTHING BELOW IS A VERBATIM MOVE, proven by reconstruction: these blocks plus the sink
 *   left behind in ap_log.h reproduce the previous ap_log.h byte for byte. The same check 8-2d-0
 *   used, for the same reason -- a formatter that quietly changed would corrupt every comparison
 *   this project makes against its banked logs, and it would do it invisibly.
 *
 * ⚠ THE INCLUDER MUST DEFINE Out(Str255) BEFORE INCLUDING THIS. Say/SayH/Say1 call it. A caller
 *   that forgets gets an implicit-declaration error at compile time, which is the right failure:
 *   loud, immediate, and on the host rather than on the hardware.
 *
 * The <MacTypes.h> include is the one thing here that is NOT a move. It is what makes the header
 * standalone for the driver -- the formatters need Str255 and nothing else.
 */
#ifndef AP_FMT_H
#define AP_FMT_H

#include <MacTypes.h>

static void PCat(Str255 d,const char*s){short l=d[0];while(*s&&l<255)d[++l]=(unsigned char)*s++;d[0]=(unsigned char)l;}
/* No PCatDec here: this probe reports register values only, and every one of them is hex.
 * Carrying an unused formatter would be the same liability as the unused PhyWrite that 4b-i
 * removed -- code that never runs is code that was never tested. */
static void PCatHex(Str255 d,unsigned long v,int digits){static const char h[]="0123456789ABCDEF";
  int i;for(i=digits-1;i>=0;i--)if(d[0]<255)d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF];}

static void Say(const char*s){Str255 L;L[0]=0;PCat(L,s);Out(L);}
static void SayH(const char*s,unsigned long v,int d){Str255 L;L[0]=0;PCat(L,s);PCat(L,"0x");PCatHex(L,v,d);Out(L);}
/* Record counts are compared against decimal figures in the blob headers (317, 31), so print
 * them in decimal -- 0x13D against "317 declared" is a needless translation at read time. */
static void PCatDec(Str255 d,unsigned long v){char b[12];int n=0;char s[2];s[1]=0;
  if(!v){PCat(d,"0");return;}
  while(v&&n<11){b[n++]=(char)('0'+(int)(v%10));v/=10;}
  while(n){s[0]=b[--n];PCat(d,s);} }
static void Say1(const char*s,unsigned long v){Str255 L;L[0]=0;PCat(L,s);PCatDec(L,v);Out(L);}

#endif /* AP_FMT_H */
