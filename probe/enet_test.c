/* enet_test.c -- host tests for ap_enet.h's 802.11 -> Ethernet conversion.
 *
 * ★ WHY THIS RUNS ON THE HOST. Every case below is decidable without a radio: the address
 * selection rules, the FCS arithmetic, the SNAP check and the reason codes are pure framing.
 * A hardware run must NEED hardware, and none of this does. What the G4 cannot tell me is
 * whether OT calls Read after the isr -- that is what the reboot is for.
 *
 * ⚠ THE CASE THAT MATTERS MOST is fcs_is_stripped. An off-by-four here does not fail loudly;
 *   it hands Open Transport four bytes of FCS as payload on every packet, forever.
 *
 * Build:  cc -o enet_test enet_test.c && ./enet_test
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned long  UInt32;
typedef unsigned char  ApU8;
typedef unsigned long  ApU32;

#define RXH_FRAME_LEN  0
#define K6_HDR_PLCP6   6UL
#define FRAMEOFFSET    30UL          /* a stand-in for B43_DMA0_RX_FW351_FO */

static UInt16 le16at(const UInt8 *p){ return (UInt16)(p[0] | ((UInt16)p[1] << 8)); }
static int MacEq(const UInt8 *a,const UInt8 *b)
{ int i; for(i=0;i<6;i++) if(a[i]!=b[i]) return 0; return 1; }

/* Stub the CCMP layer: these tests cover FRAMING, and the crypto is already covered by
 * kdf_test.c against RFC 3610 vectors and a live AP's MIC. gCcmpWorks lets a test choose
 * whether the decrypt succeeds, which is how DECRYPT_FAILED is exercised. */
static int gCcmpWorks = 1;
static UInt8 gCcmpPlain[256];
static ApU32 gCcmpPlainLen = 0;
static int ApCcmpDecap(const ApU8 *f,ApU32 flen,const ApU8 *key,
                       ApU8 *out,ApU32 *outLen,ApU8 *pn,int *kid)
{ (void)f;(void)flen;(void)key;(void)pn;(void)kid;
  if(!gCcmpWorks) return 0;
  memcpy(out,gCcmpPlain,gCcmpPlainLen); if(outLen)*outLen=gCcmpPlainLen; return 1; }

/* Claim ap_ccmp.h's include guard so ap_enet.h picks up the stub above instead of the real
 * AES. The conversion under test is framing; the crypto has its own suite. */
#define AP_CCMP_H
#include "ap_enet.h"

/* ============================================================================
 * 8-36b: the rig for testing ApIpInspect against overreads.
 * ==========================================================================*/
#include <sys/mman.h>
#include <unistd.h>

static void chk(const char *what, int ok, int *fails)
{
    printf("  %-64s %s\n", what, ok ? "[ok]" : "[FAIL]");
    if(!ok) (*fails)++;
}

/* Place `len` bytes at the very END of a writable page, with an INACCESSIBLE page directly
 * after. Any read one byte past the frame is then a segfault rather than a silent success --
 * which is the only way a bounds bug in a parser shows up on a host at all. */
static UInt8 *GuardedFrame(const UInt8 *src, UInt32 len)
{
    long  pg   = sysconf(_SC_PAGESIZE);
    char *base = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if(base == MAP_FAILED){ printf("  [FAIL] mmap for the guard page\n"); return NULL; }
    if(mprotect(base + pg, (size_t)pg, PROT_NONE) != 0)
        printf("  [warn] mprotect failed; overreads will NOT be caught\n");
    memcpy(base + pg - len, src, (size_t)len);
    return (UInt8 *)(base + pg - len);
}

static void FreeGuarded(UInt8 *p, UInt32 len)
{
    long pg = sysconf(_SC_PAGESIZE);
    if(p) munmap((char *)p + len - pg, (size_t)pg * 2);
}

/* A DHCP reply modelled on the Tiger reference captured 2026-09-23 from this card and MAC on
 * this AP: src 192.168.1.1, siaddr 192.168.1.1, broadcast, 300-byte BOOTP, options 53 and 54.
 * Both checksums are computed, so a test can corrupt a byte and expect the UDP sum to notice. */
static UInt32 BuildDhcpReply(UInt8 *f, UInt32 yi, int mt, const UInt8 *cha)
{
    UInt32 bootp = 300, ulen = 8 + bootp, iplen = 20 + ulen, tot = 14 + iplen, i, s;
    UInt8 *ip = f + 14, *ud = ip + 20, *bp = ud + 8;

    for(i = 0; i < tot; i++) f[i] = 0;
    for(i = 0; i < 6; i++)   f[i] = 0xFF;                      /* DA = broadcast */
    f[6]=0x70; f[7]=0xA7; f[8]=0x41; f[9]=0xAD; f[10]=0x7F; f[11]=0x25;   /* SA = gateway */
    f[12]=0x08; f[13]=0x00;

    ip[0]=0x45; ip[1]=0xC0;
    ip[2]=(UInt8)(iplen >> 8); ip[3]=(UInt8)iplen;
    ip[8]=64; ip[9]=17;                                        /* TTL, UDP */
    ip[12]=192; ip[13]=168; ip[14]=1; ip[15]=1;
    ip[16]=ip[17]=ip[18]=ip[19]=0xFF;
    s = ApOnesSum(ip, 20, 0);
    ip[10]=(UInt8)((~s) >> 8); ip[11]=(UInt8)(~s);

    ud[0]=0; ud[1]=67; ud[2]=0; ud[3]=68;
    ud[4]=(UInt8)(ulen >> 8); ud[5]=(UInt8)ulen;

    bp[0]=2; bp[1]=1; bp[2]=6; bp[3]=0;
    bp[4]=0xE6; bp[5]=0xD8; bp[6]=0xBD; bp[7]=0x00;            /* xid */
    bp[16]=(UInt8)(yi>>24); bp[17]=(UInt8)(yi>>16);
    bp[18]=(UInt8)(yi>>8);  bp[19]=(UInt8)yi;                  /* yiaddr */
    bp[20]=192; bp[21]=168; bp[22]=1; bp[23]=1;                /* siaddr, as Tiger's reply has */
    for(i = 0; i < 6; i++) bp[28+i] = cha[i];                  /* chaddr */
    bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63;    /* magic cookie */
    bp[240]=53; bp[241]=1; bp[242]=(UInt8)mt;                  /* message type */
    bp[243]=54; bp[244]=4; bp[245]=192; bp[246]=168; bp[247]=1; bp[248]=1;  /* server id */
    bp[249]=0xFF;                                              /* end */

    s = ApOnesSum(ip + 12, 8, 0);                              /* pseudo-header addresses */
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    { UInt32 c = (~s) & 0xFFFFu; if(!c) c = 0xFFFFu;
      ud[6]=(UInt8)(c >> 8); ud[7]=(UInt8)c; }
    return tot;
}

/* ---------------------------------------------------------------------------------------- */

static UInt8 buf[4096];
static UInt8 out[2048];
static const UInt8 OURS[6] = {0x00,0x11,0x24,0xAA,0xC4,0x4C};
static const UInt8 AP_[6]  = {0xA2,0x41,0xB2,0xCC,0x58,0xAD};
static const UInt8 PEER[6] = {0x70,0xA7,0x41,0xAD,0x7F,0x25};
static const UInt8 BCAST[6]= {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const UInt8 OTHER[6]= {0xDE,0xAD,0xBE,0xEF,0x00,0x01};

static int fails = 0;

/* Lay a frame into a DMA slot the way the card would: frame_len counts the PLCP AND the FCS. */
static void mkframe(UInt16 fc,const UInt8 *a1,const UInt8 *a2,const UInt8 *a3,
                    const UInt8 *bodyBytes,UInt32 bodyLen)
{
    UInt8 *f = buf + FRAMEOFFSET + K6_HDR_PLCP6;
    UInt32 flen = 24 + bodyLen;
    memset(buf,0,sizeof buf);
    f[0]=(UInt8)(fc & 0xFF); f[1]=(UInt8)(fc >> 8);
    f[2]=0; f[3]=0;
    memcpy(f+4,a1,6); memcpy(f+10,a2,6); memcpy(f+16,a3,6);
    f[22]=0; f[23]=0;
    if(bodyLen) memcpy(f+24,bodyBytes,bodyLen);
    /* + PLCP + FCS, exactly as the hardware reports it */
    { UInt32 raw = flen + K6_HDR_PLCP6 + 4;
      buf[RXH_FRAME_LEN]   = (UInt8)(raw & 0xFF);
      buf[RXH_FRAME_LEN+1] = (UInt8)(raw >> 8); }
}

static void check(const char *name,int got,int wantOk,int wantWhy,int why)
{
    int ok = (got == wantOk) && (wantOk ? 1 : (why == wantWhy));
    printf("  %-34s %s", name, ok ? "[ok]" : "[FAIL]");
    if(!ok){ printf("   got ret=%d why=%d (%s), wanted ret=%d why=%d",
                    got,why,ApEnetWhy(why),wantOk,wantWhy); fails++; }
    printf("\n");
}

/* 8-42: build a DHCP DISCOVER in the SNAP+ethertype+IP BODY form ApEnetToBody produces and
 * ApTxForceDhcpBroadcast operates on (NOT a full Ethernet frame). op=1, udp 68->67, flags per
 * arg, both checksums valid. Returns the body length. */
static UInt32 BuildDhcpDiscoverBody(UInt8 *body, int bcastFlag)
{
    UInt32 bootp = 300, ulen = 8 + bootp, iplen = 20 + ulen, tot = 8 + iplen, i, s;
    UInt8 *ip = body + 8, *ud = ip + 20, *bp = ud + 8;

    for(i = 0; i < tot; i++) body[i] = 0;
    body[0]=0xAA; body[1]=0xAA; body[2]=0x03; body[3]=0x00; body[4]=0x00; body[5]=0x00;
    body[6]=0x08; body[7]=0x00;                                /* SNAP + ethertype IPv4 */

    ip[0]=0x45; ip[1]=0x00;
    ip[2]=(UInt8)(iplen >> 8); ip[3]=(UInt8)iplen;
    ip[8]=64; ip[9]=17;                                        /* TTL, UDP */
    /* src 0.0.0.0, dst 255.255.255.255 */
    ip[16]=ip[17]=ip[18]=ip[19]=0xFF;
    s = ApOnesSum(ip, 20, 0);
    ip[10]=(UInt8)((~s) >> 8); ip[11]=(UInt8)(~s);

    ud[0]=0; ud[1]=68; ud[2]=0; ud[3]=67;                      /* client 68 -> server 67 */
    ud[4]=(UInt8)(ulen >> 8); ud[5]=(UInt8)ulen;

    bp[0]=1; bp[1]=1; bp[2]=6; bp[3]=0;                        /* op=BOOTREQUEST */
    bp[4]=0xE6; bp[5]=0xD9; bp[6]=0xBB; bp[7]=0x66;            /* xid */
    if(bcastFlag){ bp[10]=0x80; bp[11]=0x00; }                 /* flags */
    bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63;    /* magic cookie */
    bp[240]=53; bp[241]=1; bp[242]=1;                          /* DHCP DISCOVER */
    bp[243]=0xFF;

    s = ApOnesSum(ip + 12, 8, 0);
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    { UInt32 c = (~s) & 0xFFFFu; if(!c) c = 0xFFFFu; ud[6]=(UInt8)(c >> 8); ud[7]=(UInt8)c; }
    return tot;
}

/* 8-47: build a DHCP REQUEST (or DISCOVER) in the same SNAP+ethertype+IP BODY form, with a chosen
 * option 53 (msgType), an optional requested-IP (opt 50, omitted when reqIp==0), an optional
 * server-id (opt 54, omitted when serverId==0), and a chosen ciaddr. Both checksums valid. Lets a
 * test reproduce SELECTING (opt50+opt54), INIT-REBOOT (opt50, no opt54) and RENEW (ciaddr, no
 * opt50) and confirm ApTxDhcpInspect reads each field back. */
static UInt32 BuildDhcpRequestBody(UInt8 *body, int msgType, UInt32 reqIp, UInt32 serverId, UInt32 ciaddr)
{
    UInt32 bootp = 300, ulen = 8 + bootp, iplen = 20 + ulen, tot = 8 + iplen, i, s, q;
    UInt8 *ip = body + 8, *ud = ip + 20, *bp = ud + 8;

    for(i = 0; i < tot; i++) body[i] = 0;
    body[0]=0xAA; body[1]=0xAA; body[2]=0x03; body[3]=0x00; body[4]=0x00; body[5]=0x00;
    body[6]=0x08; body[7]=0x00;                                /* SNAP + ethertype IPv4 */

    ip[0]=0x45; ip[1]=0x00;
    ip[2]=(UInt8)(iplen >> 8); ip[3]=(UInt8)iplen;
    ip[8]=64; ip[9]=17;
    ip[16]=ip[17]=ip[18]=ip[19]=0xFF;
    s = ApOnesSum(ip, 20, 0);
    ip[10]=(UInt8)((~s) >> 8); ip[11]=(UInt8)(~s);

    ud[0]=0; ud[1]=68; ud[2]=0; ud[3]=67;                      /* client 68 -> server 67 */
    ud[4]=(UInt8)(ulen >> 8); ud[5]=(UInt8)ulen;

    bp[0]=1; bp[1]=1; bp[2]=6; bp[3]=0;                        /* op=BOOTREQUEST */
    bp[4]=0xE6; bp[5]=0xD9; bp[6]=0xFF; bp[7]=0x73;            /* xid */
    bp[12]=(UInt8)(ciaddr>>24); bp[13]=(UInt8)(ciaddr>>16);
    bp[14]=(UInt8)(ciaddr>>8);  bp[15]=(UInt8)ciaddr;         /* ciaddr */
    bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63;    /* magic cookie */
    q = 240;
    bp[q++]=53; bp[q++]=1; bp[q++]=(UInt8)msgType;             /* option 53 (message type) */
    if(reqIp){    bp[q++]=50; bp[q++]=4;
                  bp[q++]=(UInt8)(reqIp>>24);    bp[q++]=(UInt8)(reqIp>>16);
                  bp[q++]=(UInt8)(reqIp>>8);     bp[q++]=(UInt8)reqIp; }       /* opt 50 */
    if(serverId){ bp[q++]=54; bp[q++]=4;
                  bp[q++]=(UInt8)(serverId>>24); bp[q++]=(UInt8)(serverId>>16);
                  bp[q++]=(UInt8)(serverId>>8);  bp[q++]=(UInt8)serverId; }    /* opt 54 */
    bp[q++]=0xFF;                                              /* end of options */

    s = ApOnesSum(ip + 12, 8, 0);
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    { UInt32 c = (~s) & 0xFFFFu; if(!c) c = 0xFFFFu; ud[6]=(UInt8)(c >> 8); ud[7]=(UInt8)c; }
    return tot;
}

/* Independent UDP-checksum verdict on such a body: the ones-sum over pseudo-header + datagram is
 * 0xFFFF exactly when the checksum is valid. Deliberately NOT sharing code with the helper under
 * test, so a bug in one cannot hide a bug in the other. */
static int BodyUdpCkValid(const UInt8 *body)
{
    const UInt8 *ip = body + 8;
    UInt32 ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    const UInt8 *ud = ip + ihl;
    UInt32 ulen = ((UInt32)ud[4] << 8) | ud[5];
    UInt32 s = ApOnesSum(ip + 12, 8u, 0);
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    return (s == 0xFFFFu);
}

/* 8-50: the same verdict for an ETHERNET-framed frame (eh+14 = IP), for ApRxClearDhcpBroadcast. */
static int EthUdpCkValid(const UInt8 *eh)
{
    const UInt8 *ip = eh + 14;
    UInt32 ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    const UInt8 *ud = ip + ihl;
    UInt32 ulen = ((UInt32)ud[4] << 8) | ud[5];
    UInt32 s = ApOnesSum(ip + 12, 8u, 0);
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    return (s == 0xFFFFu);
}
/* Recompute an Ethernet-framed frame's UDP checksum in place (to make a hand-edited frame valid). */
static void EthUdpCkFix(UInt8 *eh)
{
    UInt8 *ip = eh + 14;
    UInt32 ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    UInt8 *ud = ip + ihl;
    UInt32 ulen = ((UInt32)ud[4] << 8) | ud[5];
    UInt32 s;
    ud[6] = 0; ud[7] = 0;
    s = ApOnesSum(ip + 12, 8u, 0);
    s = ApOnesSum(ud, ulen, s + 17u + ulen);
    { UInt32 c = (~s) & 0xFFFFu; if(!c) c = 0xFFFFu; ud[6] = (UInt8)(c >> 8); ud[7] = (UInt8)c; }
}

int main(void)
{
    UInt32 olen; int why; int r;
    static const UInt8 SNAP_ARP[] = {0xAA,0xAA,0x03,0x00,0x00,0x00,0x08,0x06,
                                     0x00,0x01,0x08,0x00,0x06,0x04,0x00,0x01};
    UInt8 tk[16], gtk[16];
    memset(tk,0x11,16); memset(gtk,0x22,16);

    printf("ap_enet.h -- 802.11 to Ethernet\n\n");

    /* ★★★ THE ONE THAT MATTERS: is the FCS stripped, and exactly once? */
    mkframe(0x0208,OURS,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);   /* data, FromDS */
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("fcs_is_stripped (len)",r,1,0,why);
    if(r){
        UInt32 want = AP_ENET_HDR + (sizeof SNAP_ARP - 8);
        printf("  %-34s %s  (%lu, wanted %lu)\n","fcs_is_stripped (exact bytes)",
               olen==want?"[ok]":"[FAIL]",(unsigned long)olen,(unsigned long)want);
        if(olen!=want) fails++;
        printf("  %-34s %s\n","ethertype from SNAP not 802.11",
               (out[12]==0x08 && out[13]==0x06) ? "[ok]" : "[FAIL]");
        if(!(out[12]==0x08 && out[13]==0x06)) fails++;
    }

    /* Address selection: FromDS means a1=DA, a3=SA. The BSSID must NOT become the source. */
    if(r){
        int dsOk = MacEq(out,OURS) && MacEq(out+6,PEER) && !MacEq(out+6,AP_);
        printf("  %-34s %s\n","FromDS: src is peer, not BSSID",dsOk?"[ok]":"[FAIL]");
        if(!dsOk) fails++;
    }

    /* ToDS (uplink): a1=BSSID, a2=SA, a3=DA. */
    mkframe(0x0108,PEER,OURS,OTHER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OTHER,out,&olen,&why);
    { int ok = r && MacEq(out,OTHER) && MacEq(out+6,OURS);
      printf("  %-34s %s\n","ToDS: a3=DA a2=SA",ok?"[ok]":"[FAIL]"); if(!ok) fails++; }

    /* IBSS: a1=DA a2=SA. */
    mkframe(0x0008,OURS,PEER,AP_,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    { int ok = r && MacEq(out,OURS) && MacEq(out+6,PEER);
      printf("  %-34s %s\n","IBSS: a1=DA a2=SA",ok?"[ok]":"[FAIL]"); if(!ok) fails++; }

    /* Broadcast must be delivered even though it is not addressed to us. */
    mkframe(0x0208,BCAST,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("broadcast is delivered",r,1,0,why);

    /* A unicast for somebody else is not ours. */
    mkframe(0x0208,OTHER,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("unicast to another station",r,0,AP_ENET_NOT_FOR_US,why);

    /* ★★★ k120: THE AP RELAYS OUR OWN BROADCAST BACK TO US, AND WE MUST NOT DELIVER IT.
     *
     * Captured verbatim on hardware: DA=broadcast, SA=our own MAC, an IP frame on UDP 68->67
     * with op=1 -- this machine's own DHCP DISCOVER, arriving as though received. The DA check
     * above cannot catch it, because a broadcast genuinely IS addressed to us. Open Transport
     * saw its own source address arrive on the interface and called Stop ten times.
     *
     * FromDS with a3 (the source, for a FromDS frame) set to OURS is exactly that shape. */
    mkframe(0x0208,BCAST,AP_,OURS,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("our own broadcast, relayed back -> refused",r,0,AP_ENET_OUR_OWN_TX,why);

    /* ...and the same test in reverse: a broadcast from ANOTHER station still gets through, so
     * the new filter cannot be passing by rejecting everything. */
    mkframe(0x0208,BCAST,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("another station's broadcast still delivered",r,1,AP_ENET_OK,why);

    /* Beacons are management: normal, and must be counted as such rather than as an error. */
    mkframe(0x0080,BCAST,AP_,AP_,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("beacon -> NOT_DATA",r,0,AP_ENET_NOT_DATA,why);

    /* Data-null keepalive: also normal. */
    mkframe(0x0248,OURS,AP_,PEER,NULL,0);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("data-null -> NULL_FRAME",r,0,AP_ENET_NULL_FRAME,why);

    /* Not SNAP. */
    { static const UInt8 junk[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
      mkframe(0x0208,OURS,AP_,PEER,junk,sizeof junk);
      r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
      check("non-SNAP body",r,0,AP_ENET_NOT_SNAP,why); }

    /* Protected, no key installed -- must be NO_KEY, NOT decrypt-failed. */
    mkframe(0x4208,OURS,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,0,gtk,1,OURS,out,&olen,&why);
    check("protected, no TK -> NO_KEY",r,0,AP_ENET_NO_KEY,why);

    /* Protected, key present, MIC fails -- a REAL error and must not be confused with NO_KEY. */
    gCcmpWorks = 0;
    mkframe(0x4208,OURS,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("protected, MIC fails",r,0,AP_ENET_DECRYPT_FAILED,why);

    /* Protected, decrypts fine -> delivered. */
    gCcmpWorks = 1;
    memcpy(gCcmpPlain,SNAP_ARP,sizeof SNAP_ARP); gCcmpPlainLen = sizeof SNAP_ARP;
    mkframe(0x4208,OURS,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("protected, decrypts -> delivered",r,1,0,why);

    /* A group-addressed protected frame must reach for the GTK, so a missing GTK is NO_KEY
     * even when the TK is present. Picking the wrong key would show up as a MIC failure on
     * hardware, which is a much more confusing symptom. */
    mkframe(0x4208,BCAST,AP_,PEER,SNAP_ARP,sizeof SNAP_ARP);
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,0,OURS,out,&olen,&why);
    check("group frame wants the GTK",r,0,AP_ENET_NO_KEY,why);

    /* Runt. */
    memset(buf,0,sizeof buf);
    buf[0] = 8; buf[1] = 0;
    r = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,OURS,out,&olen,&why);
    check("runt frame",r,0,AP_ENET_TOO_SHORT,why);

    /* ══ 8-8: the TRANSMIT direction, and a ROUND TRIP ══════════════════════════════════════
     *
     * ★ THE ROUND TRIP IS THE ORACLE HERE and it is worth more than the field checks above it.
     *   Ethernet -> 802.11 body -> (wrapped in a real header) -> back through ApRxToEnet must
     *   return the ORIGINAL BYTES. The two converters were written a day apart against the same
     *   spec, so agreeing with each other is not the circularity of a function agreeing with
     *   itself -- and a disagreement about SNAP, the ethertype's position or the address roles
     *   shows up as a byte difference rather than as a plausible-looking frame. */
    {
        static const UInt8 ETH[] = {
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,            /* dst: broadcast */
            0x00,0x11,0x24,0xAA,0xC4,0x4C,            /* src: us */
            0x08,0x06,                                /* ethertype: ARP */
            0x00,0x01,0x08,0x00,0x06,0x04,0x00,0x01,
            0x00,0x11,0x24,0xAA,0xC4,0x4C, 0xC0,0xA8,0x01,0x40,
            0x00,0x00,0x00,0x00,0x00,0x00, 0xC0,0xA8,0x01,0x01 };
        UInt8 body[256]; UInt32 blen = 0; const UInt8 *da = NULL; int w = -1;
        int ok = ApEnetToBody(ETH,(UInt32)sizeof ETH,body,&blen,&da,&w);
        printf("\n  -- 8-8: Ethernet -> 802.11 body --\n");
        printf("  %-34s %s\n","converts", ok ? "[ok]" : "[FAIL]"); if(!ok) fails++;
        { int snap = ok && body[0]==0xAA && body[1]==0xAA && body[2]==0x03 &&
                     body[3]==0 && body[4]==0 && body[5]==0;
          printf("  %-34s %s\n","LLC/SNAP prefix", snap?"[ok]":"[FAIL]"); if(!snap) fails++; }
        { int et = ok && body[6]==0x08 && body[7]==0x06;
          printf("  %-34s %s\n","ethertype moved into SNAP", et?"[ok]":"[FAIL]"); if(!et) fails++; }
        { int len = ok && blen == 8 + (sizeof ETH - 14);
          printf("  %-34s %s (%lu)\n","body length", len?"[ok]":"[FAIL]",(unsigned long)blen);
          if(!len) fails++; }
        { int d = ok && da && MacEq(da,ETH);
          printf("  %-34s %s\n","addr3 is the Ethernet dst", d?"[ok]":"[FAIL]"); if(!d) fails++; }

        if(ok){
            UInt8 out2[2048]; UInt32 olen2 = 0; int w2 = -1, r2, same = 1; UInt32 q;
            /* An AP relaying it back would use FromDS: a1 = DA, a2 = BSSID, a3 = SA. */
            mkframe(0x0208, ETH, AP_, ETH+6, body, blen);
            r2 = ApRxToEnet(buf,FRAMEOFFSET,tk,1,gtk,1,ETH,out2,&olen2,&w2);
            if(!r2 || olen2 != (UInt32)sizeof ETH) same = 0;
            else for(q=0;q<olen2;q++) if(out2[q] != ETH[q]) same = 0;
            printf("  %-34s %s","ROUND TRIP returns the original", same?"[ok]":"[FAIL]");
            if(!same){ printf("  (ret=%d len=%lu want=%lu why=%s)",
                              r2,(unsigned long)olen2,(unsigned long)sizeof ETH,ApEnetWhy(w2));
                       fails++; }
            printf("\n");
        }

        { UInt8 tiny[16]; UInt32 bl; int ww = -1;
          int rr = ApEnetToBody(tiny,13,body,&bl,&da,&ww);
          printf("  %-34s %s\n","runt refused",
                 (!rr && ww==AP_ENET_TOO_SHORT) ? "[ok]" : "[FAIL]");
          if(rr || ww!=AP_ENET_TOO_SHORT) fails++; }
        { static UInt8 big[4096]; UInt32 bl; int ww = -1;
          int rr = ApEnetToBody(big,(UInt32)sizeof big,body,&bl,&da,&ww);
          printf("  %-34s %s\n","oversized refused",
                 (!rr && ww==AP_ENET_TOO_BIG) ? "[ok]" : "[FAIL]");
          if(rr || ww!=AP_ENET_TOO_BIG) fails++; }
    }

    /* ★★★★★ 8-36b: ApIpInspect, THE PARSER THAT CRASHED THE MACHINE ON k129.
     *
     * ⛔ It was inline in EnetHAL_Read, so nothing here could reach it and its first execution
     *   anywhere was on the G4, where it walked 65 KB off the end of a buffer. The frames below
     *   are placed at the END of a writable page with an INACCESSIBLE page immediately after,
     *   so any read past the frame is a SIGSEGV and this test dies loudly instead of passing
     *   quietly. That is the oracle: a silent overread is not detectable any other way.
     *
     * ★ The good frame is modelled on the Tiger reference captured 2026-09-23 from this exact
     *   card and MAC on this exact AP: yiaddr 192.168.1.225, siaddr 192.168.1.1, type ACK. */
    printf("\n  -- 8-36b: ApIpInspect (IP / UDP / DHCP), on guarded pages --\n");
    {
        static const UInt8 OURMAC[6] = { 0x00,0x11,0x24,0xAA,0xC4,0x4C };
        UInt8 tmpl[512];
        ApIpInfo inf;
        UInt32 flen;

        flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 5, OURMAC);   /* 192.168.1.225, ACK */

        { UInt8 *g = GuardedFrame(tmpl, flen);
          ApIpInspect(g, flen, OURMAC, &inf);
          chk("well-formed ACK: IP header checksum verifies", inf.ipCkOk == 1, &fails);
          chk("well-formed ACK: UDP checksum verifies",  inf.udpCk == AP_UDPCK_OK, &fails);
          chk("well-formed ACK: recognised as DHCP",     inf.isDhcp == 1, &fails);
          chk("well-formed ACK: message type is 5",      inf.msgType == 5, &fails);
          chk("well-formed ACK: yiaddr is 192.168.1.225",
              inf.yiaddr == 0xC0A801E1UL, &fails);
          chk("well-formed ACK: chaddr is ours",         inf.chaddrIsOurs == 1, &fails);
          chk("well-formed ACK: opt54 server-id found AFTER opt53 (the k151 walk)",
              inf.serverId == 0xC0A80101UL, &fails);
          chk("well-formed ACK: option list fully walked (opt53 + opt54)", inf.optCount == 2, &fails);
          chk("well-formed ACK: 0xFF end marker reached",  inf.sawEnd == 1, &fails);
          FreeGuarded(g, flen); }

        /* ★ 8-54 (k151): a full OFFER -- opt53=OFFER, then opt54/1/3/51/6. Verifies the walk finds
         * EVERY option after opt53 (the whole change: the old walk broke on 53 and never saw 54).
         * Falsification: revert to break-on-53 and inf.serverId reads 0 -> these chks fail loudly. */
        {   UInt8 *g; UInt32 q, ulen, s;
            UInt8 *ip = tmpl + 14, *ud = ip + 20, *bp = ud + 8;
            flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURMAC);   /* an OFFER of .225 */
            q = 240;
            bp[q++]=53; bp[q++]=1; bp[q++]=2;                                     /* opt53 OFFER   */
            bp[q++]=1;  bp[q++]=4; bp[q++]=255; bp[q++]=255; bp[q++]=255; bp[q++]=0;   /* opt1  /24 */
            bp[q++]=3;  bp[q++]=4; bp[q++]=192; bp[q++]=168; bp[q++]=1; bp[q++]=1;     /* opt3  rtr */
            bp[q++]=54; bp[q++]=4; bp[q++]=192; bp[q++]=168; bp[q++]=1; bp[q++]=1;     /* opt54 srv */
            bp[q++]=51; bp[q++]=4; bp[q++]=0;   bp[q++]=1;   bp[q++]=0x51; bp[q++]=0x80; /* opt51 86400 */
            bp[q++]=6;  bp[q++]=4; bp[q++]=192; bp[q++]=168; bp[q++]=1; bp[q++]=1;     /* opt6  dns */
            bp[q++]=0xFF;                                                         /* end           */
            ud[6]=ud[7]=0;                                          /* re-checksum after the edit  */
            ulen = ((UInt32)ud[4]<<8)|ud[5];
            s = ApOnesSum(ip + 12, 8, 0);
            s = ApOnesSum(ud, ulen, s + 17u + ulen);
            { UInt32 c=(~s)&0xFFFFu; if(!c)c=0xFFFFu; ud[6]=(UInt8)(c>>8); ud[7]=(UInt8)c; }
            g = GuardedFrame(tmpl, flen);
            ApIpInspect(g, flen, OURMAC, &inf);
            chk("full OFFER: message type is OFFER (2)",   inf.msgType == 2, &fails);
            chk("full OFFER: opt54 server-id decoded",     inf.serverId   == 0xC0A80101UL, &fails);
            chk("full OFFER: opt1 subnet mask decoded",    inf.subnetMask == 0xFFFFFF00UL, &fails);
            chk("full OFFER: opt3 router decoded",         inf.router     == 0xC0A80101UL, &fails);
            chk("full OFFER: opt6 dns decoded",            inf.dns1       == 0xC0A80101UL, &fails);
            chk("full OFFER: opt51 lease decoded (86400)", inf.leaseSecs  == 86400UL, &fails);
            chk("full OFFER: all six options counted",     inf.optCount == 6, &fails);
            chk("full OFFER: 0xFF end marker reached",     inf.sawEnd == 1, &fails);
            chk("full OFFER: UDP checksum valid after the edit", inf.udpCk == AP_UDPCK_OK, &fails);
            FreeGuarded(g, flen); }

        { UInt8 *g;                                   /* a NAK: yiaddr 0 is CORRECT here */
          flen = BuildDhcpReply(tmpl, 0UL, 6, OURMAC);
          g = GuardedFrame(tmpl, flen);
          ApIpInspect(g, flen, OURMAC, &inf);
          chk("NAK: message type is 6, not confused with an OFFER", inf.msgType == 6, &fails);
          chk("NAK: yiaddr reads zero",                  inf.yiaddr == 0, &fails);
          chk("NAK: UDP checksum still verifies",        inf.udpCk == AP_UDPCK_OK, &fails);
          FreeGuarded(g, flen); }

        { UInt8 *g;                                   /* ★ a corrupted payload MUST show up */
          flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 5, OURMAC);
          tmpl[58] ^= 0x01;                           /* one bit inside yiaddr */
          g = GuardedFrame(tmpl, flen);
          ApIpInspect(g, flen, OURMAC, &inf);
          chk("a flipped payload bit FAILS the UDP checksum",
              inf.udpCk == AP_UDPCK_BAD, &fails);
          chk("...and the IP header checksum still passes (it does not cover the payload)",
              inf.ipCkOk == 1, &fails);
          FreeGuarded(g, flen); }

        /* ⛔⛔⛔ THE k129 CRASH, REPRODUCED TWO WAYS.
         *
         * ⚠⚠ THE FIRST VERSION OF THIS TEST PASSED WITH THE CLAMP DELETED. Its frame put
         *   option 53 first, so the walk matched on its very first iteration and broke out
         *   before it could run anywhere. A bounds test whose input never reaches the bound
         *   is decoration -- the same defect as the 23-byte CCMP suite, found the same way,
         *   by deleting the fix and checking the test actually dies. Both cases below were
         *   verified to SIGSEGV against a scratch copy with the clamp removed.
         *
         * Case 1: a SHORT frame that declares a long one. This is the realistic vector -- a
         * runt on port 67/68 -- and it faults before any walk, on the magic-cookie probe at
         * bp[236], which sits 218 bytes past a 60-byte frame. */
        { UInt8 *g;
          flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 5, OURMAC);   /* declares ulen = 308 */
          g = GuardedFrame(tmpl, 60);                             /* but only 60 bytes exist */
          ApIpInspect(g, 60, OURMAC, &inf);
          chk("⛔ short frame declaring a long one: no read past it (k129's crash)",
              1, &fails);                              /* reaching this line IS the result */
          chk("...the cookie probe at bp[236] is not attempted", inf.msgType == -1, &fails);
          chk("...and the checksum it cannot cover is refused",
              inf.udpCk == AP_UDPCK_NOTCHECKED, &fails);
          FreeGuarded(g, 60); }

        /* Case 2: a full frame with a LYING length and options that never terminate, so the
         * walk itself is what runs off the end -- 65287 iterations past the page boundary. */
        { UInt8 *g; UInt32 k;
          flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 5, OURMAC);
          for(k = 240; k < 300; k++) tmpl[42 + k] = 0x00;  /* all pad: no type, no end marker */
          tmpl[34 + 4] = 0xFF; tmpl[34 + 5] = 0xFF;        /* UDP length := 65535, a lie */
          g = GuardedFrame(tmpl, flen);
          ApIpInspect(g, flen, OURMAC, &inf);
          chk("⛔ unterminated options + lying length: the walk stays in the frame",
              1, &fails);
          chk("...and no message type is invented", inf.msgType == -1, &fails);
          FreeGuarded(g, flen); }

        { UInt8 *g = GuardedFrame(tmpl, 8);           /* shorter than an Ethernet header */
          ApIpInspect(g, 8, OURMAC, &inf);
          chk("a frame shorter than an Ethernet header is refused", inf.isIp == 0, &fails);
          FreeGuarded(g, 8); }
    }

    /* ★★★★★ 8-42: ApTxForceDhcpBroadcast -- the k136/k137 fix, on guarded pages.
     *
     * The risk that matters is the UDP checksum: set the flag and get the checksum wrong and the
     * SERVER drops our DISCOVER -- strictly worse than the unicast-offer status quo. So the decisive
     * assertion is an INDEPENDENT recompute (BodyUdpCkValid, which shares no code with the helper)
     * after the edit. The last case is the k129-style falsification: remove the length bound in the
     * helper and it SIGSEGVs here reading a 65535-byte datagram off a 60-byte guarded page. */
    printf("\n  -- 8-42: ApTxForceDhcpBroadcast (force the DHCP broadcast flag) --\n");
    {
        UInt8 tmpl[512];
        UInt32 blen = BuildDhcpDiscoverBody(tmpl, 0);
        UInt8  *ip  = tmpl + 8;
        UInt32 ihl  = (UInt32)(ip[0] & 0x0F) * 4u;
        UInt32 flagOff = 8u + ihl + 8u + 10u;         /* SNAP + IP + UDP + BOOTP flags */

        chk("built DISCOVER: broadcast flag starts CLEAR", (tmpl[flagOff] & 0x80) == 0, &fails);
        chk("built DISCOVER: its own UDP checksum is valid", BodyUdpCkValid(tmpl), &fails);

        { UInt8 *g = GuardedFrame(tmpl, blen);
          int r2 = ApTxForceDhcpBroadcast(g, blen);
          chk("DISCOVER: helper reports it set the flag",     r2 == 1, &fails);
          chk("DISCOVER: broadcast bit is now SET",           (g[flagOff] & 0x80) != 0, &fails);
          chk("★ DISCOVER: UDP checksum STILL valid after the edit", BodyUdpCkValid(g), &fails);
          chk("DISCOVER: second call is a no-op (idempotent)", ApTxForceDhcpBroadcast(g, blen) == 0, &fails);
          chk("...and the no-op leaves the checksum valid",    BodyUdpCkValid(g), &fails);
          FreeGuarded(g, blen); }

        /* a server REPLY (ports 67->68) is not a client request: left untouched */
        { UInt8 *g; UInt32 rl = BuildDhcpDiscoverBody(tmpl, 0);
          UInt8 *ud = tmpl + 8 + 20; ud[0]=0; ud[1]=67; ud[2]=0; ud[3]=68;
          g = GuardedFrame(tmpl, rl);
          chk("a server REPLY (67->68) is left untouched", ApTxForceDhcpBroadcast(g, rl) == 0, &fails);
          FreeGuarded(g, rl); }

        /* a non-IP body (ARP ethertype) is left untouched */
        { UInt8 arp[32]; UInt8 *g; UInt32 i;
          for(i=0;i<32;i++) arp[i]=0;
          arp[0]=0xAA;arp[1]=0xAA;arp[2]=0x03; arp[6]=0x08;arp[7]=0x06;   /* SNAP + ethertype ARP */
          g = GuardedFrame(arp, 32);
          chk("a non-IP body (ARP) is left untouched", ApTxForceDhcpBroadcast(g, 32) == 0, &fails);
          FreeGuarded(g, 32); }

        /* ⛔ a short body DECLARING a long UDP length: refused, no edit, no overread (k129 lesson) */
        { UInt8 *g; UInt32 bl = BuildDhcpDiscoverBody(tmpl, 0);
          UInt8 *ud = tmpl + 8 + 20; ud[4]=0xFF; ud[5]=0xFF;   /* lie: ulen := 65535 */
          (void)bl;
          g = GuardedFrame(tmpl, 60);                          /* only 60 bytes exist */
          chk("⛔ short body, lying UDP length: refused, no overread",
              ApTxForceDhcpBroadcast(g, 60) == 0, &fails);
          FreeGuarded(g, 60); }
    }

    /* ★★★★★ 8-47 (k142): ApTxDhcpInspect -- decode our OWN outgoing DHCP, on guarded pages.
     *
     * The diagnostic question is which client STATE OT transmits in: SELECTING (opt50 + opt54)
     * gets an ACK; INIT-REBOOT (opt50, no opt54) for a stale address gets a NAK, which is what
     * k141 saw. So the suite proves the inspector reads msgType / opt50 / opt54 / ciaddr back for
     * each state -- and, the k129 rule, that a mis-declared option length cannot walk it off the
     * buffer (the last case SIGSEGVs with the fit-check deleted). */
    printf("\n  -- 8-47: ApTxDhcpInspect (decode outgoing DHCP client request) --\n");
    {
        UInt8 tmpl[512];

        /* SELECTING: REQUEST + requested-IP .225 + server-id 192.168.1.1, ciaddr 0 (the ACK path) */
        { UInt32 bl = BuildDhcpRequestBody(tmpl, 3, 0xC0A801E1UL, 0xC0A80101UL, 0);
          UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; int rr = ApTxDhcpInspect(g, bl, &tx);
          chk("SELECTING: recognised as a request",          rr == 1 && tx.isReq, &fails);
          chk("SELECTING: msgType == REQUEST (3)",           tx.msgType == 3, &fails);
          chk("SELECTING: opt50 == 192.168.1.225",           tx.hasReqIp && tx.requestedIp == 0xC0A801E1UL, &fails);
          chk("SELECTING: opt54 == 192.168.1.1",             tx.hasServerId && tx.serverId == 0xC0A80101UL, &fails);
          chk("SELECTING: ciaddr == 0",                      tx.ciaddr == 0, &fails);
          FreeGuarded(g, bl); }

        /* INIT-REBOOT: REQUEST + requested-IP .90, NO server-id -- the NAK signature we suspect */
        { UInt32 bl = BuildDhcpRequestBody(tmpl, 3, 0xC0A8015AUL, 0, 0);
          UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; ApTxDhcpInspect(g, bl, &tx);
          chk("INIT-REBOOT: msgType == REQUEST (3)",         tx.msgType == 3, &fails);
          chk("INIT-REBOOT: opt50 == 192.168.1.90",          tx.hasReqIp && tx.requestedIp == 0xC0A8015AUL, &fails);
          chk("INIT-REBOOT: NO opt54 (the stale signature)", !tx.hasServerId, &fails);
          FreeGuarded(g, bl); }

        /* RENEWING: REQUEST, ciaddr = current IP, no opt50 / opt54 */
        { UInt32 bl = BuildDhcpRequestBody(tmpl, 3, 0, 0, 0xC0A801E1UL);
          UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; ApTxDhcpInspect(g, bl, &tx);
          chk("RENEW: msgType == REQUEST (3)",               tx.msgType == 3, &fails);
          chk("RENEW: ciaddr == 192.168.1.225",              tx.ciaddr == 0xC0A801E1UL, &fails);
          chk("RENEW: no opt50, no opt54",                   !tx.hasReqIp && !tx.hasServerId, &fails);
          FreeGuarded(g, bl); }

        /* DISCOVER (reuse the 8-42 builder): request, msgType 1, no opt50/54; flag reflects OT */
        { UInt32 bl = BuildDhcpDiscoverBody(tmpl, 1);
          UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; ApTxDhcpInspect(g, bl, &tx);
          chk("DISCOVER: msgType == DISCOVER (1)",           tx.msgType == 1, &fails);
          chk("DISCOVER: no opt50, no opt54",                !tx.hasReqIp && !tx.hasServerId, &fails);
          chk("DISCOVER: bcastFlag reflects OT's frame (set)", tx.bcastFlag == 1, &fails);
          FreeGuarded(g, bl); }
        { UInt32 bl = BuildDhcpDiscoverBody(tmpl, 0);
          UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; ApTxDhcpInspect(g, bl, &tx);
          chk("DISCOVER (unicast flag): bcastFlag == 0",     tx.bcastFlag == 0, &fails);
          FreeGuarded(g, bl); }

        /* a server REPLY (67->68) is not a client request */
        { UInt32 bl = BuildDhcpDiscoverBody(tmpl, 0);
          UInt8 *ud = tmpl + 8 + 20; ud[0]=0; ud[1]=67; ud[2]=0; ud[3]=68;
          { UInt8 *g = GuardedFrame(tmpl, bl); ApTxDhcpInfo tx; int rr = ApTxDhcpInspect(g, bl, &tx);
            chk("a server REPLY (67->68) is ignored",        rr == 0 && !tx.isReq, &fails);
            FreeGuarded(g, bl); } }

        /* a non-IP body (ARP) is ignored */
        { UInt8 arp[32]; UInt32 i; for(i=0;i<32;i++) arp[i]=0;
          arp[0]=0xAA;arp[1]=0xAA;arp[2]=0x03; arp[6]=0x08;arp[7]=0x06;
          { UInt8 *g = GuardedFrame(arp, 32); ApTxDhcpInfo tx; int rr = ApTxDhcpInspect(g, 32, &tx);
            chk("a non-IP body (ARP) is ignored",            rr == 0, &fails);
            FreeGuarded(g, 32); } }

        /* ⛔ k129 falsification: an opt50 header flush against the buffer edge. The fit-check
         *   (q + 2 + olen > blen) must BREAK the scan; delete that one line and the 4-byte opt50
         *   read crosses the guard page and SIGSEGVs here, exactly as the k129 overread did on the
         *   G4. With the bound present the scan bails and no requested-IP is recorded. */
        { UInt8 t2[512]; UInt32 i;
          UInt32 ihl=20, off=8u+ihl+8u, blen=247, ulen=8u+blen, iplen=20u+ulen, bodyLen=off+blen;
          UInt8 *ip=t2+8, *ud=ip+ihl, *bp=ud+8;
          for(i=0;i<bodyLen;i++) t2[i]=0;
          t2[6]=0x08; t2[7]=0x00;
          ip[0]=0x45; ip[2]=(UInt8)(iplen>>8); ip[3]=(UInt8)iplen; ip[9]=17;
          ud[1]=68; ud[3]=67; ud[4]=(UInt8)(ulen>>8); ud[5]=(UInt8)ulen;
          bp[0]=1;                                     /* BOOTREQUEST */
          bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63;
          bp[240]=53; bp[241]=1; bp[242]=3;            /* opt53 = REQUEST, q -> 243 */
          bp[243]=50; bp[244]=4;                       /* opt50 header; body bp[245..248] is OUT */
          { UInt8 *g = GuardedFrame(t2, bodyLen); ApTxDhcpInfo tx; int rr = ApTxDhcpInspect(g, bodyLen, &tx);
            chk("⛔ opt50 flush at buffer edge: breaks, no overread", rr == 1 && !tx.hasReqIp, &fails);
            FreeGuarded(g, bodyLen); } }
    }

    /* ★★★★★ 8-50 (k145): ApRxClearDhcpBroadcast -- reset the echoed broadcast flag on a reply.
     * The decisive assertion, as with 8-42, is an INDEPENDENT recompute (EthUdpCkValid) after the
     * edit: clear the bit and get the checksum wrong and OT drops the OFFER, which is worse than
     * the flag mismatch we are trying to remove. The last case is the k129 falsification. */
    printf("\n  -- 8-50: ApRxClearDhcpBroadcast (clear the echoed DHCP broadcast flag) --\n");
    {
        UInt8 tmpl[512];
        UInt32 flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS);   /* OFFER, yiaddr .225 */
        UInt8  *ip  = tmpl + 14;
        UInt32 ihl  = (UInt32)(ip[0] & 0x0F) * 4u;
        UInt32 flagOff = 14u + ihl + 8u + 10u;      /* Eth + IP + UDP + BOOTP flags high byte */

        chk("built OFFER: broadcast flag starts CLEAR", (tmpl[flagOff] & 0x80) == 0, &fails);
        chk("built OFFER: its own UDP checksum is valid", EthUdpCkValid(tmpl), &fails);

        /* set the bit as the server would echo it, and fix the checksum so the INPUT is well-formed */
        tmpl[flagOff] |= 0x80; EthUdpCkFix(tmpl);
        chk("flag-set OFFER: checksum valid (well-formed input)", EthUdpCkValid(tmpl), &fails);

        { UInt8 *g = GuardedFrame(tmpl, flen);
          int r = ApRxClearDhcpBroadcast(g, flen);
          chk("OFFER: helper reports it cleared the flag", r == 1, &fails);
          chk("OFFER: broadcast bit is now CLEAR", (g[flagOff] & 0x80) == 0, &fails);
          chk("★ OFFER: UDP checksum STILL valid after the edit", EthUdpCkValid(g), &fails);
          chk("OFFER: second call is a no-op (idempotent)", ApRxClearDhcpBroadcast(g, flen) == 0, &fails);
          FreeGuarded(g, flen); }

        /* a reply whose flag is already clear is left untouched */
        { UInt8 *g; UInt32 rl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 5, OURS);   /* ACK, flags 0 */
          g = GuardedFrame(tmpl, rl);
          chk("a reply with the flag already clear is untouched", ApRxClearDhcpBroadcast(g, rl) == 0, &fails);
          FreeGuarded(g, rl); }

        /* a client request (ports 68->67) is not a server reply, even with the bit set */
        { UInt8 *g; UInt32 rl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 3, OURS);
          UInt8 *ud = tmpl + 14 + 20; ud[0]=0; ud[1]=68; ud[2]=0; ud[3]=67;
          tmpl[flagOff] |= 0x80;
          g = GuardedFrame(tmpl, rl);
          chk("a client request (68->67) is left untouched", ApRxClearDhcpBroadcast(g, rl) == 0, &fails);
          FreeGuarded(g, rl); }

        /* a non-IP frame (ARP ethertype) is left untouched */
        { UInt8 arp[64]; UInt32 i; for(i=0;i<64;i++) arp[i]=0;
          for(i=0;i<6;i++) arp[i]=0xFF; arp[12]=0x08; arp[13]=0x06;   /* Eth dst + ethertype ARP */
          { UInt8 *g = GuardedFrame(arp, 64);
            chk("a non-IP frame (ARP) is left untouched", ApRxClearDhcpBroadcast(g, 64) == 0, &fails);
            FreeGuarded(g, 64); } }

        /* ⛔ a short frame DECLARING a long UDP length: refused, no edit, no overread (k129) */
        { UInt8 *g; UInt32 rl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS);
          UInt8 *ud = tmpl + 14 + 20; ud[4]=0xFF; ud[5]=0xFF;   /* lie: ulen := 65535 */
          tmpl[flagOff] |= 0x80; (void)rl;
          g = GuardedFrame(tmpl, 60);                            /* only 60 bytes exist */
          chk("⛔ short frame, lying UDP length: refused, no overread",
              ApRxClearDhcpBroadcast(g, 60) == 0, &fails);
          FreeGuarded(g, 60); }
    }

    /* ★★★★★ 8-55 (k152): ApRxDhcpDstToUnicast -- flip a broadcast DHCP reply's L2 dst to our MAC.
     * The Ethernet header is NOT covered by the IP/UDP checksums, so the decisive pair is: the dst
     * becomes ours AND the UDP checksum is STILL valid (we touched nothing the sum covers). */
    printf("\n  -- 8-55: ApRxDhcpDstToUnicast (broadcast DHCP reply -> unicast L2 dst) --\n");
    {
        UInt8 tmpl[512];
        UInt32 flen = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS);   /* OFFER, yiaddr .225 */

        chk("built OFFER: Eth dst starts BROADCAST", tmpl[0]==0xFF && tmpl[5]==0xFF, &fails);

        { UInt8 *g = GuardedFrame(tmpl, flen);
          chk("broadcast OFFER: helper reports it rewrote the dst",
              ApRxDhcpDstToUnicast(g, flen, OURS) == 1, &fails);
          chk("broadcast OFFER: Eth dst is now OUR unicast MAC",
              g[0]==OURS[0] && g[5]==OURS[5] && (g[0]&0x01)==0, &fails);
          chk("★ broadcast OFFER: UDP checksum STILL valid (Eth header not covered)",
              EthUdpCkValid(g), &fails);
          chk("...second call is a no-op (already unicast)",
              ApRxDhcpDstToUnicast(g, flen, OURS) == 0, &fails);
          FreeGuarded(g, flen); }

        /* a broadcast BOOTP CLIENT request (udp 68->67) is NOT a server reply -- untouched */
        { UInt8 *g; UInt32 fl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS);
          UInt8 *ud = tmpl + 14 + 20;
          ud[1]=68; ud[3]=67;                                       /* 67->68 becomes 68->67 */
          g = GuardedFrame(tmpl, fl);
          chk("a client request (udp 68->67) is left untouched",
              ApRxDhcpDstToUnicast(g, fl, OURS) == 0, &fails);
          FreeGuarded(g, fl); }

        /* an already-unicast reply (Eth dst = us) is a no-op */
        { UInt8 *g; UInt32 fl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS);
          int i; for(i=0;i<6;i++) tmpl[i]=OURS[i];
          g = GuardedFrame(tmpl, fl);
          chk("an already-unicast reply is left untouched",
              ApRxDhcpDstToUnicast(g, fl, OURS) == 0, &fails);
          FreeGuarded(g, fl); }

        /* a non-IP frame (broadcast ARP) is left untouched */
        { UInt8 arp[64]; UInt32 i; for(i=0;i<64;i++) arp[i]=0;
          for(i=0;i<6;i++) arp[i]=0xFF; arp[12]=0x08; arp[13]=0x06;
          { UInt8 *g = GuardedFrame(arp, 64);
            chk("a non-IP frame (ARP) is left untouched",
                ApRxDhcpDstToUnicast(g, 64, OURS) == 0, &fails);
            FreeGuarded(g, 64); } }

        /* ⛔ a short broadcast frame: refused before any read past it */
        { UInt8 *g; UInt32 fl = BuildDhcpReply(tmpl, 0xC0A801E1UL, 2, OURS); (void)fl;
          g = GuardedFrame(tmpl, 20);                               /* only 20 bytes exist */
          chk("⛔ short frame: refused, no overread",
              ApRxDhcpDstToUnicast(g, 20, OURS) == 0, &fails);
          FreeGuarded(g, 20); }
    }

    /* ★★★★★ 8-56 (k153): ApRxArpToUnicast -- flip a broadcast who-has-.225 ARP request to unicast.
     * ARP has no IP layer, so this is the CLEAN test of the broadcast-RX theory. */
    printf("\n  -- 8-56: ApRxArpToUnicast (broadcast who-has-.225 -> unicast L2 dst) --\n");
    {
        UInt8 arp[42]; UInt32 i;
        for(i=0;i<42;i++) arp[i]=0;
        for(i=0;i<6;i++) arp[i]=0xFF;                            /* Eth dst = broadcast */
        arp[6]=0x70;arp[7]=0xA7;arp[8]=0x41;arp[9]=0xAD;arp[10]=0x7F;arp[11]=0x25; /* Eth src */
        arp[12]=0x08; arp[13]=0x06;                              /* ethertype ARP */
        arp[14]=0x00; arp[15]=0x01; arp[16]=0x08; arp[17]=0x00;  /* htype ether, ptype IPv4 */
        arp[18]=6; arp[19]=4;                                    /* hlen, plen */
        arp[20]=0x00; arp[21]=0x01;                              /* op = request */
        arp[28]=192;arp[29]=168;arp[30]=1;arp[31]=1;             /* sender .1 */
        arp[38]=192;arp[39]=168;arp[40]=1;arp[41]=225;           /* target .225 (us) */

        { UInt8 *g = GuardedFrame(arp, 42);
          chk("who-has-.225 (broadcast): helper rewrote the dst",
              ApRxArpToUnicast(g, 42, OURS) == 1, &fails);
          chk("who-has-.225: Eth dst is now OUR unicast MAC",
              g[0]==OURS[0] && g[5]==OURS[5] && (g[0]&0x01)==0, &fails);
          chk("...second call is a no-op (already unicast)",
              ApRxArpToUnicast(g, 42, OURS) == 0, &fails);
          FreeGuarded(g, 42); }

        { UInt8 *g; for(i=0;i<6;i++) arp[i]=0xFF; arp[41]=1;     /* who-has .1 instead */
          g = GuardedFrame(arp, 42);
          chk("who-has-.1 (not us) is left untouched",
              ApRxArpToUnicast(g, 42, OURS) == 0, &fails);
          arp[41]=225; FreeGuarded(g, 42); }

        { UInt8 *g; for(i=0;i<6;i++) arp[i]=0xFF; arp[21]=0x02; /* op = reply */
          g = GuardedFrame(arp, 42);
          chk("an ARP reply (op=2) is left untouched",
              ApRxArpToUnicast(g, 42, OURS) == 0, &fails);
          arp[21]=0x01; FreeGuarded(g, 42); }

        { UInt8 *g; for(i=0;i<6;i++) arp[i]=OURS[i];             /* already unicast */
          g = GuardedFrame(arp, 42);
          chk("an already-unicast ARP request is left untouched",
              ApRxArpToUnicast(g, 42, OURS) == 0, &fails);
          FreeGuarded(g, 42); }

        { UInt8 *g; for(i=0;i<6;i++) arp[i]=0xFF;
          g = GuardedFrame(arp, 30);                             /* only 30 bytes exist */
          chk("⛔ short ARP frame: refused, no overread",
              ApRxArpToUnicast(g, 30, OURS) == 0, &fails);
          FreeGuarded(g, 30); }
    }

    /* ★★★★★ 8-57 (k154): ApTxIcmpInspect -- decode our outgoing ICMP from a SNAP+IP body. */
    printf("\n  -- 8-57: ApTxIcmpInspect (outgoing ICMP echo reply / request) --\n");
    {
        UInt8 body[64]; UInt32 i; ApTxIcmpInfo ic;
        for(i=0;i<64;i++) body[i]=0;
        body[0]=0xaa;body[1]=0xaa;body[2]=0x03;body[3]=0;body[4]=0;body[5]=0;  /* SNAP */
        body[6]=0x08;body[7]=0x00;                                            /* ethertype IPv4 */
        body[8]=0x45;                                                         /* IP v4, ihl=5 */
        body[8+9]=1;                                                          /* proto ICMP */
        body[8+16]=192;body[8+17]=168;body[8+18]=1;body[8+19]=207;            /* dst .207 (Pi) */
        body[8+20]=0;                                                         /* ICMP type 0 = reply */

        { UInt8 *g = GuardedFrame(body, 36);
          chk("echo reply: recognised as ICMP",
              ApTxIcmpInspect(g, 36, &ic) == 1 && ic.isIcmp == 1, &fails);
          chk("echo reply: type is 0",           ic.icmpType == 0, &fails);
          chk("echo reply: dst IP is 192.168.1.207", ic.dstIp == 0xC0A801CFUL, &fails);
          FreeGuarded(g, 36); }

        body[8+20]=8;                                                         /* type 8 = request */
        { UInt8 *g = GuardedFrame(body, 36);
          chk("echo request: type is 8",
              ApTxIcmpInspect(g, 36, &ic) == 1 && ic.icmpType == 8, &fails);
          FreeGuarded(g, 36); }

        body[8+9]=17;                                                         /* proto UDP, not ICMP */
        { UInt8 *g = GuardedFrame(body, 36);
          chk("UDP body: isIp set but not isIcmp",
              ApTxIcmpInspect(g, 36, &ic) == 0 && ic.isIp == 1 && ic.isIcmp == 0, &fails);
          FreeGuarded(g, 36); }

        body[8+9]=1; body[6]=0x08; body[7]=0x06;                             /* ethertype ARP */
        { UInt8 *g = GuardedFrame(body, 36);
          chk("ARP body: not IP at all",
              ApTxIcmpInspect(g, 36, &ic) == 0 && ic.isIp == 0, &fails);
          FreeGuarded(g, 36); }

        body[6]=0x08; body[7]=0x00; body[8]=0x4F;                            /* lying IHL = 15 (60B) */
        { UInt8 *g = GuardedFrame(body, 36);
          chk("⛔ lying IHL: refused, no overread",
              ApTxIcmpInspect(g, 36, &ic) == 0, &fails);
          FreeGuarded(g, 36); }
    }

    printf("\n%s\n", fails ? "FAILURES" : "[ok] all conversion cases pass");
    return fails != 0;
}
