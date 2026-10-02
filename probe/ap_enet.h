/* ap_enet.h -- 802.11 IN, ETHERNET OUT. The conversion Stage 8-6 hands to Open Transport.
 *
 * ★ THE TENTH SHARED HEADER, and the first written rather than extracted. There was nothing to
 * extract: the app decrypted a frame and printed a hexdump, which is not the same as producing a
 * buffer another subsystem will parse. Everything it needs, though, already exists and is proven
 * -- ApCcmpDecap against a live AP's MIC, the address rules from the frames Stage 7 read off the
 * air -- so this is assembly of validated parts plus the framing rules, not new cryptography.
 *
 * ⚠⚠ THE FCS. THIS IS THE STANDING STAGE 8 LANDMINE AND IT IS DISARMED IN EXACTLY ONE PLACE.
 *
 *   The card's frame_len INCLUDES the 4-byte FCS. That is not an assumption: Stage 7-5 tried the
 *   decap at `dlen` and at `dlen-4` and the MIC -- which can only verify for the correct length --
 *   verified only at -4. The project's own open-items list carries it as "audit every frame_len
 *   consumer BEFORE Stage 8 hands buffers to Open Transport", and this is that audit.
 *
 *   So the arithmetic happens HERE, once, and this function takes the DMA slot exactly as
 *   RxWaitFor copied it rather than a frame and a length. A caller cannot pass the wrong length
 *   because a caller does not compute one. An off-by-four would not fail loudly: it would hand OT
 *   four bytes of FCS as payload on every single packet.
 *
 * ⚠ WHY A REASON CODE AND NOT A BOOLEAN. A pump that reports "delivered 0" is a question. The
 *   reason histogram says whether we are dropping frames because they are not data, because they
 *   are encrypted with a key we do not have, because the SNAP header is missing, or because the
 *   MIC failed -- four completely different bugs that a boolean renders identical. Same rule as
 *   RXSTATUS: decode at the point of reading.
 */
#ifndef AP_ENET_H
#define AP_ENET_H

#include "ap_ccmp.h"    /* ApCcmpDecap and the AES-CCM it is built on */

/* k199: timing hooks around the receive decrypt. ap_shim.c defines them (ap_tbclock.h) before it
 * includes this file; enet_test.c and every other includer get these no-ops. END takes whether the
 * MIC verified, so a frame that fails is not averaged in with the ones that were delivered. */
#ifndef AP_ENET_DECRYPT_BEGIN
#define AP_ENET_DECRYPT_BEGIN()    ((void)0)
#define AP_ENET_DECRYPT_END(ok)    ((void)(ok))
#endif

#define AP_ENET_HDR       14UL       /* dst 6 + src 6 + ethertype 2 */
#define AP_ENET_MAXFRAME  1514UL     /* the classic Ethernet II maximum, header included */
#define AP_ENET_PLAINBUF  2048UL

/* Why a frame was not delivered. 0 is success; everything else is counted and reported. */
enum {
    AP_ENET_OK = 0,
    AP_ENET_TOO_SHORT,        /* shorter than an 802.11 header */
    AP_ENET_NOT_DATA,         /* management or control -- beacons live here, and they are normal */
    AP_ENET_NULL_FRAME,       /* data-null / QoS-null: a keepalive with no body, also normal */
    AP_ENET_NO_KEY,           /* protected, and the key it needs has not been installed */
    AP_ENET_DECRYPT_FAILED,   /* pairwise (PTK): the MIC did not verify -- a REAL error */
    /* ★ SPLIT OUT AT k103, BECAUSE k102 SHOWED SIX OF THESE AND ONE COUNTER CANNOT SAY WHY.
     * k96/k97/k99 had the same reporting and never logged a single MIC failure; k102 logged 6
     * out of 23 encrypted frames. Group and pairwise fail for completely different reasons -- a
     * group failure points at the GTK (a re-key we do not follow, or the wrong key index), a
     * pairwise one at the PTK or the PN. Counting them together makes both invisible, and the
     * `group` flag is already computed two lines above the failure. */
    AP_ENET_DECRYPT_FAILED_GROUP,
    AP_ENET_NOT_SNAP,         /* decrypted, but the body is not AA AA 03 00 00 00 */
    AP_ENET_TOO_BIG,          /* would not fit an Ethernet frame */
    AP_ENET_NOT_FOR_US,       /* unicast to some other station */
    AP_ENET_OUR_OWN_TX,       /* ★ k120: the AP relayed our own broadcast back to us */
    AP_ENET_NREASON
};

static const char *ApEnetWhy(int w)
{
    switch(w){
      case AP_ENET_OK:             return "delivered";
      case AP_ENET_TOO_SHORT:      return "too short";
      case AP_ENET_NOT_DATA:       return "not a data frame (beacons are normal here)";
      case AP_ENET_NULL_FRAME:     return "data-null keepalive, no body (normal)";
      case AP_ENET_NO_KEY:         return "protected, key not installed";
      case AP_ENET_DECRYPT_FAILED: return "⚠ MIC FAILED, PAIRWISE/PTK -- not routine";
      case AP_ENET_DECRYPT_FAILED_GROUP:
                                   return "⚠ MIC FAILED, GROUP/GTK -- not routine";
      case AP_ENET_NOT_SNAP:       return "⚠ body is not LLC/SNAP";
      case AP_ENET_TOO_BIG:        return "⚠ larger than an Ethernet frame";
      case AP_ENET_NOT_FOR_US:     return "unicast to another station";
      case AP_ENET_OUR_OWN_TX:     return "★ OUR OWN transmission, relayed back by the AP";
      default:                     return "?"; }
}

/* A multicast/broadcast destination has bit 0 of the first byte set. Group-addressed traffic is
 * protected with the GTK, not the pairwise TK -- picking the wrong one fails the MIC, which is
 * why AP_ENET_DECRYPT_FAILED and AP_ENET_NO_KEY are separate counters. */
static int ApEnetIsGroup(const UInt8 *a){ return (a[0] & 0x01) != 0; }

static UInt8 gApEnetPlain[AP_ENET_PLAINBUF];

/* ★ THE CONVERSION.
 *
 * `rxbuf` is a DMA slot exactly as RxWaitFor copied it: the card's RX header, then the 6-byte
 * PLCP, then the 802.11 frame. `frameoffset` is the same value handed to the ring at arm time.
 *
 * Returns 1 and fills out/outLen on success. Otherwise returns 0 and sets *whyOut. */
static int ApRxToEnet(const UInt8 *rxbuf, UInt32 frameoffset,
                      const UInt8 *tk, int tkReady,
                      const UInt8 *gtk, int gtkReady,
                      const UInt8 *ourMac,
                      UInt8 *out, UInt32 *outLen, int *whyOut)
{
    const UInt8 *f;
    UInt32 flen, hdrLen, bodyLen;
    const UInt8 *body;
    const UInt8 *da, *sa;
    UInt16 fc;
    int ty, sb, tods, fromds, prot, isQos, a4;

    if(outLen) *outLen = 0;
    if(whyOut) *whyOut = AP_ENET_TOO_SHORT;

    /* ⚠⚠ THE ONE PLACE THE FCS IS SUBTRACTED. See this header's opening block. */
    {
        UInt32 raw = (UInt32)le16at(rxbuf + RXH_FRAME_LEN);
        if(raw <= K6_HDR_PLCP6 + 4UL) return 0;
        flen = raw - K6_HDR_PLCP6 - 4UL;          /* strip the PLCP and the trailing FCS */
    }
    f = rxbuf + frameoffset + K6_HDR_PLCP6;
    if(flen < 24UL) return 0;

    fc     = le16at(f);
    ty     = (int)((fc >> 2) & 3);
    sb     = (int)((fc >> 4) & 0x0F);
    tods   = (fc & 0x0100) != 0;
    fromds = (fc & 0x0200) != 0;
    prot   = (fc & 0x4000) != 0;
    a4     = (tods && fromds);
    isQos  = (ty == 2 && sb == 8);

    if(ty != 2){ if(whyOut) *whyOut = AP_ENET_NOT_DATA; return 0; }
    /* subtype 4 = data-null, 12 = QoS-null. Both are legitimate keepalives with no body. */
    if(sb == 4 || sb == 12){ if(whyOut) *whyOut = AP_ENET_NULL_FRAME; return 0; }

    hdrLen = 24UL + (a4 ? 6UL : 0UL) + (isQos ? 2UL : 0UL);
    if(flen < hdrLen){ if(whyOut) *whyOut = AP_ENET_TOO_SHORT; return 0; }

    /* ★ WHICH ADDRESS IS WHICH DEPENDS ON ToDS/FromDS, and getting it wrong does not crash --
     * it silently delivers frames with the BSSID as the source, which every ARP table upstream
     * would then learn. The infrastructure downlink case (FromDS only) is ours. */
    if(!tods && !fromds)      { da = f + 4;  sa = f + 10; }   /* IBSS: a1=DA a2=SA a3=BSSID */
    else if(!tods && fromds)  { da = f + 4;  sa = f + 16; }   /* AP->STA: a1=DA a2=BSSID a3=SA */
    else if(tods && !fromds)  { da = f + 16; sa = f + 10; }   /* STA->AP: a1=BSSID a2=SA a3=DA */
    else                      { da = f + 16; sa = f + 24; }   /* 4-address WDS */

    /* Deliver broadcast, multicast, and unicast addressed to us. Anything else belongs to
     * another station and is not ours to hand upstream. */
    if(!ApEnetIsGroup(da) && !MacEq(da,ourMac)){
        if(whyOut) *whyOut = AP_ENET_NOT_FOR_US; return 0; }

    /* ⛔⛔ AND NEVER HAND BACK OUR OWN TRANSMISSION. k120 caught one verbatim:
     *
     *     ff ff ff ff ff ff  02 00 00 00 00 01  08 00  45 00 ...
     *     DA = broadcast     SA = OUR OWN MAC   IP     ports 68->67, op=1 BOOTREQUEST
     *
     * That is this machine's own DHCP DISCOVER, delivered up the stack as though it had been
     * received. An access point relays a station's broadcast to the BSS and this one relays it
     * to the originator too; the DA check above cannot catch it, because a broadcast genuinely
     * IS addressed to us.
     *
     * ⚠ It is not a cosmetic duplicate. Open Transport sees a frame carrying its own source
     *   address arrive on the interface, which is the signature of a bridging loop or a
     *   duplicate MAC, and k120 shows it calling Stop TEN times against two Starts while DHCP
     *   falls back to link-local. It also silently inflated "DHCP for us", since our own
     *   DISCOVER naturally carries our own chaddr.
     *
     * A station does not receive what it sent. Dropping on SA is what b43/mac80211 do, and it
     * is one comparison against a value we already hold. */
    if(MacEq(sa,ourMac)){
        if(whyOut) *whyOut = AP_ENET_OUR_OWN_TX; return 0; }

    if(prot){
        const UInt8 *key;
        ApU32 plen = 0;
        ApU8 pn[6]; int kid = 0, okDec;
        int group = ApEnetIsGroup(da);
        if(group ? !gtkReady : !tkReady){ if(whyOut) *whyOut = AP_ENET_NO_KEY; return 0; }
        key = group ? gtk : tk;
        AP_ENET_DECRYPT_BEGIN();
        okDec = ApCcmpDecap((const ApU8*)f,(ApU32)flen,(const ApU8*)key,gApEnetPlain,&plen,pn,&kid);
        AP_ENET_DECRYPT_END(okDec);
        if(!okDec){
            if(whyOut) *whyOut = group ? AP_ENET_DECRYPT_FAILED_GROUP
                                       : AP_ENET_DECRYPT_FAILED;
            return 0; }
        body    = gApEnetPlain;
        bodyLen = (UInt32)plen;
    } else {
        body    = f + hdrLen;
        bodyLen = flen - hdrLen;
    }

    /* LLC/SNAP: AA AA 03 00 00 00 then the 2-byte ethertype. Anything else is not something an
     * Ethernet stack can be handed -- and after a successful decrypt it is the strongest sign
     * that the plaintext is real, which is why Stage 7 scored on it too. */
    if(bodyLen < 8UL){ if(whyOut) *whyOut = AP_ENET_NOT_SNAP; return 0; }
    if(!(body[0]==0xAA && body[1]==0xAA && body[2]==0x03 &&
         body[3]==0x00 && body[4]==0x00 && body[5]==0x00)){
        if(whyOut) *whyOut = AP_ENET_NOT_SNAP; return 0; }

    if(AP_ENET_HDR + (bodyLen - 8UL) > AP_ENET_MAXFRAME){
        if(whyOut) *whyOut = AP_ENET_TOO_BIG; return 0; }

    {
        UInt32 i, n = bodyLen - 8UL;
        for(i=0;i<6;i++) out[i]      = da[i];
        for(i=0;i<6;i++) out[6+i]    = sa[i];
        out[12] = body[6];                     /* the ethertype comes from the SNAP header, */
        out[13] = body[7];                     /* NOT from the 802.11 frame */
        for(i=0;i<n;i++) out[AP_ENET_HDR+i] = body[8+i];
        if(outLen) *outLen = AP_ENET_HDR + n;
    }
    if(whyOut) *whyOut = AP_ENET_OK;
    return 1;
}

/* ============================================================================================
 * ★★★★★★ 8-8: THE OTHER DIRECTION. Ethernet in, the 802.11 plaintext body out.
 *
 * This is the mirror of ApRxToEnet and it is deliberately the SMALLER half: it produces only the
 * body that goes under the 802.11 header, because the header itself is BuildDataHeaderToDs in
 * ap_eapol.h -- already written, already used to put an encrypted ARP on the air at Stage 8-1,
 * and already correct about the thing that is easy to get wrong (it does NOT set the Protected
 * bit, because ApCcmpEncap sets it on its output and the AAD is computed from the unmodified
 * header; setting it here would leave the AAD disagreeing with the wire).
 *
 * Ethernet II in:   dst(6) src(6) ethertype(2) payload
 * 802.11 body out:  AA AA 03 00 00 00 ethertype(2) payload
 *
 * ⚠ THE SOURCE ADDRESS IN THE ETHERNET FRAME IS IGNORED, ON PURPOSE. addr2 of the 802.11 frame
 *   is us and can be nothing else -- we are the station that is associated, and a frame claiming
 *   another source would be discarded by the AP or, worse, accepted and attributed to us. OT
 *   should be filling it with our own address anyway; if it ever does not, silently transmitting
 *   whatever it asked for is not the helpful choice. The caller supplies addr2 itself.
 */
static int ApEnetToBody(const UInt8 *eth, UInt32 ethLen,
                        UInt8 *bodyOut, UInt32 *bodyLenOut,
                        const UInt8 **daOut, int *whyOut)
{
    UInt32 payload, i;

    if(bodyLenOut) *bodyLenOut = 0;
    if(whyOut)     *whyOut     = AP_ENET_TOO_SHORT;
    if(ethLen < AP_ENET_HDR) return 0;
    if(ethLen > AP_ENET_MAXFRAME){ if(whyOut) *whyOut = AP_ENET_TOO_BIG; return 0; }

    payload = ethLen - AP_ENET_HDR;
    bodyOut[0]=0xAA; bodyOut[1]=0xAA; bodyOut[2]=0x03;
    bodyOut[3]=0x00; bodyOut[4]=0x00; bodyOut[5]=0x00;
    bodyOut[6]=eth[12]; bodyOut[7]=eth[13];        /* the ethertype moves into the SNAP header */
    for(i=0;i<payload;i++) bodyOut[8+i] = eth[AP_ENET_HDR+i];

    if(bodyLenOut) *bodyLenOut = 8UL + payload;
    if(daOut)      *daOut      = eth;              /* addr3: the final destination */
    if(whyOut)     *whyOut     = AP_ENET_OK;
    return 1;
}

/* ============================================================================
 * ★★★ 8-36b: THE IP / UDP / DHCP INSPECTION, AS A FUNCTION, SO IT CAN BE TESTED.
 *
 * ⛔⛔ THIS LIVED INLINE IN EnetHAL_Read AND IT CRASHED THE MACHINE ON ITS FIRST BOOT.
 *
 * The BOOTP length was taken from the UDP header and never bounded against the buffer, so a
 * frame declaring ulen = 0xFFFF walked the option list 65 KB past a 1500-byte buffer, at
 * interrupt level. One of the three reads that used that length WAS guarded; the other two
 * were not. It took a reboot and a MacsBug session to find a bug a host test finds instantly.
 *
 * ⚠⚠ THE REASON IT WAS UNTESTABLE IS THAT IT WAS INLINE IN A SELECTOR. Nothing in enet_test.c
 *   could reach it. That is the actual defect; the missing clamp was only its symptom, and
 *   k127 was the same story one build earlier -- a new read on a hot path, inline, unguarded,
 *   found by crashing rather than by testing. Parsing belongs in a function with a bound.
 *
 * ★ EVERY read below is bounded by `n`, and `avail` is computed ONCE so the bound cannot be
 *   applied to some reads and forgotten on others -- which is exactly what happened.
 * ==========================================================================*/
enum { AP_UDPCK_NOTCHECKED = -1, AP_UDPCK_BAD = 0, AP_UDPCK_OK = 1, AP_UDPCK_ABSENT = 2 };

typedef struct {
    int    isIp, isUdp, isDhcp, isReply, chaddrIsOurs, isBcast;
    int    ipCkOk;        /* -1 not checked, 0 bad, 1 ok */
    int    udpCk;         /* AP_UDPCK_* */
    int    msgType;       /* DHCP option 53, or -1 if absent */
    UInt32 yiaddr;
    UInt32 etherType;
    /* ★ 8-54 (k151): decoded DHCP options, for diagnosing why OT won't REQUEST off an OFFER.
     * A SELECTING client MUST find opt 54 (server-id) in the offer to build its REQUEST; if it is
     * absent or the option list is truncated/malformed, OT cannot proceed and re-DISCOVERs. Every
     * value is 0 when the option was absent. optCount/sawEnd witness that the whole list parsed. */
    UInt32 serverId;      /* opt 54 */
    UInt32 subnetMask;    /* opt 1  */
    UInt32 router;        /* opt 3, first address */
    UInt32 dns1;          /* opt 6, first address */
    UInt32 leaseSecs;     /* opt 51 */
    int    optCount;      /* options seen (pad/end excluded) */
    int    sawEnd;        /* the 0xFF end marker was reached */
} ApIpInfo;

/* The one's-complement sum, end-around-carry. A leaf: legal at any execution level. */
static UInt32 ApOnesSum(const UInt8 *p, UInt32 len, UInt32 seed)
{
    UInt32 s = seed;
    while(len > 1u){ s += ((UInt32)p[0] << 8) | p[1]; p += 2; len -= 2u; }
    if(len) s += (UInt32)p[0] << 8;               /* odd tail is zero-padded on the right */
    while(s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return s;
}

static void ApIpInspect(const UInt8 *eh, UInt32 n, const UInt8 *ourMac, ApIpInfo *o)
{
    const UInt8 *ip, *ud, *bp;
    UInt32 ihl, ulen, blen, avail;

    if(!o) return;
    o->isIp = o->isUdp = o->isDhcp = o->isReply = o->chaddrIsOurs = o->isBcast = 0;
    o->ipCkOk = -1; o->udpCk = AP_UDPCK_NOTCHECKED; o->msgType = -1;
    o->yiaddr = 0;  o->etherType = 0;
    o->serverId = o->subnetMask = o->router = o->dns1 = o->leaseSecs = 0;
    o->optCount = 0; o->sawEnd = 0;
    if(n < AP_ENET_HDR) return;

    o->isBcast    = (eh[0] & 0x01) != 0;
    o->etherType  = ((UInt32)eh[12] << 8) | eh[13];
    if(o->etherType != 0x0800u) return;
    o->isIp = 1;

    ip  = eh + 14;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || n < 14u + ihl) return;
    o->ipCkOk = (ApOnesSum(ip, ihl, 0) == 0xFFFFu);

    if(ip[9] != 17u || n < 14u + ihl + 8u) return;   /* not UDP, or no room for its header */
    o->isUdp = 1;
    ud = ip + ihl;
    { UInt32 sp = ((UInt32)ud[0] << 8) | ud[1];
      UInt32 dp = ((UInt32)ud[2] << 8) | ud[3];
      if(sp != 67u && dp != 67u && sp != 68u && dp != 68u) return; }
    o->isDhcp = 1;

    ulen = ((UInt32)ud[4] << 8) | ud[5];

    /* ⚠ THE CHECKSUM USES THE DECLARED LENGTH -- a short Ethernet frame is padded to 60 and
     * summing the padding would manufacture a failure. It is therefore guarded separately. */
    if(ud[6] == 0 && ud[7] == 0) o->udpCk = AP_UDPCK_ABSENT;
    else if(ulen >= 8u && (14u + ihl + ulen) <= n){
      UInt32 s = ApOnesSum(ip + 12, 8u, 0);          /* the two IP addresses */
      s = ApOnesSum(ud, ulen, s + 17u + ulen);       /* + protocol + length, then the datagram */
      o->udpCk = (s == 0xFFFFu) ? AP_UDPCK_OK : AP_UDPCK_BAD; }

    /* ⛔ AND EVERY STRUCTURAL READ USES `avail`, WHICH IS BOUNDED BY THE BUFFER. */
    bp    = ud + 8;
    avail = (n > (14u + ihl + 8u)) ? (n - 14u - ihl - 8u) : 0u;
    blen  = (ulen >= 8u) ? (ulen - 8u) : 0u;
    if(blen > avail) blen = avail;

    if(blen >= 20u){
      o->isReply = (bp[0] == 2);
      o->yiaddr  = ((UInt32)bp[16] << 24) | ((UInt32)bp[17] << 16) |
                   ((UInt32)bp[18] <<  8) |  (UInt32)bp[19]; }
    if(blen >= 34u && ourMac){
      int k, same = 1;
      for(k = 0; k < 6; k++) if(bp[28 + k] != ourMac[k]){ same = 0; break; }
      o->chaddrIsOurs = same; }

    if(blen >= 240u && bp[236]==0x63 && bp[237]==0x82 && bp[238]==0x53 && bp[239]==0x63){
      UInt32 q = 240u;
      while(q < blen){
        UInt8 code = bp[q];
        if(code == 0xFF){ o->sawEnd = 1; break; }    /* end of options */
        if(code == 0x00){ q++; continue; }           /* pad */
        if(q + 2u > blen) break;
        { UInt32 olen = bp[q+1];
          if(q + 2u + olen > blen) break;            /* the option body must fit the buffer */
          o->optCount++;
          /* ★ 8-54 (k151): capture the options a SELECTING client needs to build its REQUEST. Do
           * NOT break on opt 53 any more -- walk the whole list so opt 54 (server-id, which normally
           * follows) is seen. 4-byte big-endian address options; every read is inside the bound
           * (q+2+olen <= blen) proven just above, and q advances by the declared length so a lying
           * length cannot spin. This is the whole point of k151: to see if OT is handed a complete,
           * server-id-bearing offer or a truncated one. */
          if     (code == 53u && olen >= 1u) o->msgType = bp[q+2];
          else if(code == 54u && olen >= 4u)
            o->serverId   = ((UInt32)bp[q+2]<<24)|((UInt32)bp[q+3]<<16)|((UInt32)bp[q+4]<<8)|bp[q+5];
          else if(code ==  1u && olen >= 4u)
            o->subnetMask = ((UInt32)bp[q+2]<<24)|((UInt32)bp[q+3]<<16)|((UInt32)bp[q+4]<<8)|bp[q+5];
          else if(code ==  3u && olen >= 4u)
            o->router     = ((UInt32)bp[q+2]<<24)|((UInt32)bp[q+3]<<16)|((UInt32)bp[q+4]<<8)|bp[q+5];
          else if(code ==  6u && olen >= 4u)
            o->dns1       = ((UInt32)bp[q+2]<<24)|((UInt32)bp[q+3]<<16)|((UInt32)bp[q+4]<<8)|bp[q+5];
          else if(code == 51u && olen >= 4u)
            o->leaseSecs  = ((UInt32)bp[q+2]<<24)|((UInt32)bp[q+3]<<16)|((UInt32)bp[q+4]<<8)|bp[q+5];
          q += 2u + olen; } } }
}

/* ============================================================================================
 * ★★★★★★ 8-42: FORCE THE DHCP BROADCAST FLAG ON OUTGOING CLIENT REQUESTS.
 *
 * k136 proved, from both the wire and the driver's own snapshot in one healthy boot, exactly why
 * OT never gets an address even though everything works:
 *
 *   - We DISCOVER with the BOOTP broadcast flag CLEAR (OT's choice: "unicast the reply to me").
 *   - The server OFFERs 192.168.1.225 -- UNICAST, dst MAC = us, dst IP = the offered .225.
 *   - The driver RECEIVES, DECRYPTS and DELIVERS all nine offers to OT with valid IP+UDP
 *     checksums (snapshot: DHCP OFFER = 9, cksum ok, yiaddr = c0a801e1). Reception is not the bug.
 *   - OT DROPS every one, because a unicast datagram to 192.168.1.225 is not addressed to any IP
 *     the interface holds yet, and OT's IP input discards it before the bootp client sees it.
 *   - The NAKs, by contrast, are BROADCAST -- and OT acted on those (it abandoned the stale .90).
 *
 * So OT accepts BROADCAST DHCP and drops UNICAST DHCP, and the only lever we have over which one
 * the server sends is the broadcast flag in our own request. Setting it makes the server broadcast
 * the OFFER and the ACK, onto the exact path k136 proved OT accepts. This is a driver-side edit of
 * OT's frame, deliberately the narrowest possible: only a BOOTP client request (udp 68->67, op=1),
 * only the one flag bit, and the UDP checksum recomputed so the server still accepts the frame.
 *
 * ⚠ Operates on the SNAP+ethertype+IP body ApEnetToBody produced into gTxEthBody -- OUR buffer,
 *   never OT's. Every read is bounded by bodyLen (the k129 lesson: a frame-walker on the hot path
 *   parses only inside the buffer it was handed). If anything is out of range the frame is left
 *   completely untouched -- a wrong checksum here would make the server drop our DISCOVER, which
 *   is worse than the status quo. Returns 1 iff it set the bit and fixed the checksum.
 */
static int ApTxForceDhcpBroadcast(UInt8 *body, UInt32 bodyLen)
{
    UInt8 *ip, *ud, *bp;
    UInt32 ihl, ulen, off;

    if(bodyLen < 8u) return 0;
    if(!(body[6] == 0x08 && body[7] == 0x00)) return 0;      /* SNAP ethertype IPv4 */
    ip = body + 8;
    if(bodyLen < 8u + 20u) return 0;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || bodyLen < 8u + ihl + 8u) return 0;
    if(ip[9] != 17u) return 0;                                /* UDP */
    ud = ip + ihl;
    if(!(ud[0] == 0 && ud[1] == 68 && ud[2] == 0 && ud[3] == 67)) return 0;  /* client 68->67 */
    bp  = ud + 8;
    off = (UInt32)(bp - body);
    if(bodyLen < off + 12u) return 0;                         /* need op..flags (12 BOOTP bytes) */
    if(bp[0] != 1) return 0;                                  /* BOOTREQUEST */
    if(bp[10] & 0x80) return 0;                               /* already broadcast; idempotent */

    /* Bound the UDP length BEFORE touching anything: if the checksum cannot be recomputed safely,
     * leave the frame exactly as it was rather than ship it with a stale checksum. */
    ulen = ((UInt32)ud[4] << 8) | ud[5];
    if(ulen < 8u || (UInt32)(ud - body) + ulen > bodyLen) return 0;

    bp[10] |= 0x80;                                           /* set the broadcast flag (0x8000) */

    ud[6] = 0; ud[7] = 0;                                     /* zero, then recompute the UDP cksum */
    {
        UInt32 s = ApOnesSum(ip + 12, 8u, 0);                /* src + dst IP (the pseudo-header) */
        UInt32 nc;
        s  = ApOnesSum(ud, ulen, s + 17u + ulen);            /* + proto + len, then the datagram */
        nc = (~s) & 0xFFFFu;
        if(nc == 0u) nc = 0xFFFFu;                           /* a computed 0 goes on the wire as FFFF */
        ud[6] = (UInt8)(nc >> 8); ud[7] = (UInt8)(nc & 0xFFu);
    }
    return 1;
}

/* ============================================================================================
 * ★★★★★★ 8-50 (k145): CLEAR THE DHCP BROADCAST FLAG ON A RECEIVED SERVER REPLY.
 *
 * We force the broadcast flag on OT's outgoing DISCOVER/REQUEST (ApTxForceDhcpBroadcast, k137) so
 * the server BROADCASTS its OFFER/ACK -- k136 proved OT drops the unicast reply to an unowned IP.
 * But RFC 2131 says the server echoes the flags field as received, so the reply comes back with
 * flags = 0x8000, while OT's own DISCOVER carried flags = 0x0000 (we set the bit on the wire; OT
 * never did). k143 boot 7, k144 boot: OT receives clean, xid-matching, valid-checksum .225 OFFERs
 * and never REQUESTs -- and a client that checks the echoed flag against what it sent would reject
 * exactly these. This clears the bit back to 0 on the received reply, so OT sees a flags field
 * matching what it believes it sent, and recomputes the UDP checksum so OT still accepts it.
 *
 * ⚠ Operates on the ETHERNET frame we are about to hand up (eh[12..13] = ethertype, eh+14 = IP),
 *   in OUR queue buffer, never on the wire. Every read is bounded by n; out of range leaves the
 *   frame untouched. Only a BOOTP server reply (udp 67->68, op=2) with the bit set is changed.
 *   Returns 1 iff it cleared the bit and fixed the checksum. Host-tested in enet_test.c. */
static int ApRxClearDhcpBroadcast(UInt8 *eh, UInt32 n)
{
    UInt8 *ip, *ud, *bp;
    UInt32 ihl, ulen, off;

    if(!eh || n < AP_ENET_HDR) return 0;
    if(!(eh[12] == 0x08 && eh[13] == 0x00)) return 0;         /* ethertype IPv4 */
    ip = eh + 14;
    if(n < 14u + 20u) return 0;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || n < 14u + ihl + 8u) return 0;
    if(ip[9] != 17u) return 0;                                /* UDP */
    ud = ip + ihl;
    if(!(ud[0] == 0 && ud[1] == 67 && ud[2] == 0 && ud[3] == 68)) return 0;  /* server 67->68 */
    bp  = ud + 8;
    off = (UInt32)(bp - eh);
    if(n < off + 11u) return 0;                               /* need op..flags (bp[0..10]) */
    if(bp[0] != 2) return 0;                                  /* BOOTREPLY */
    if(!(bp[10] & 0x80)) return 0;                            /* already clear; nothing to do */

    ulen = ((UInt32)ud[4] << 8) | ud[5];
    if(ulen < 8u || (UInt32)(ud - eh) + ulen > n) return 0;   /* checksum must recompute safely */

    bp[10] &= (UInt8)~0x80;                                   /* clear the broadcast flag (0x8000) */

    ud[6] = 0; ud[7] = 0;                                     /* zero, then recompute the UDP cksum */
    {
        UInt32 s = ApOnesSum(ip + 12, 8u, 0);                /* src + dst IP (the pseudo-header) */
        UInt32 nc;
        s  = ApOnesSum(ud, ulen, s + 17u + ulen);            /* + proto + len, then the datagram */
        nc = (~s) & 0xFFFFu;
        if(nc == 0u) nc = 0xFFFFu;                           /* a computed 0 goes on the wire as FFFF */
        ud[6] = (UInt8)(nc >> 8); ud[7] = (UInt8)(nc & 0xFFu);
    }
    return 1;
}

/* ============================================================================================
 * ★★★★★★ 8-55 (k152): FLIP A BROADCAST DHCP SERVER REPLY'S ETHERNET DST TO OUR OWN MAC.
 *
 * The single finding that reframes this whole project: OT's Ethernet shim delivers L2-UNICAST
 * frames up to IP/ARP on this port but DROPS L2-BROADCAST ones. Proof, from two independent
 * angles across every boot: (1) in the static-.225 boot OT owned .225 and sent gratuitous ARP
 * from it -- its ARP was live -- yet never answered the BROADCAST who-has-.225, while it DID
 * process UNICAST ICMP to .225 and reply; (2) OT physically pulls the broadcast offers (24 bcast
 * frames/boot in EnetHAL_Read) and its upper layers act on none. Apple's EnetShimLib spec
 * (EthernetShim.pdf) makes the driver responsible only for RETURNING PACKET BYTES on receive --
 * broadcast delivery is the shim/DLPI-stub's job -- so the gate is in a binary we cannot touch.
 *
 * We adapt the frame instead. k150 still forces the server to BROADCAST, so the IP dst is
 * 255.255.255.255 (which OT's IP accepts even on an unconfigured interface); HERE we rewrite only
 * the ETHERNET dst from broadcast to our own MAC, so the shim sees a unicast frame and hands it
 * up. IP then reads the still-broadcast IP dst and accepts it. The IP/UDP checksums do NOT cover
 * the Ethernet header, so nothing is recomputed. Bounded; only a BOOTP server reply (udp 67->68,
 * op=2) whose L2 dst is group-addressed is touched. Returns 1 iff it rewrote the dst. Host-tested. */
static int ApRxDhcpDstToUnicast(UInt8 *eh, UInt32 n, const UInt8 *ourMac)
{
    UInt8 *ip, *ud, *bp;
    UInt32 ihl;

    if(!eh || !ourMac || n < AP_ENET_HDR) return 0;
    if(!(eh[0] & 0x01)) return 0;                             /* already L2-unicast; nothing to do */
    if(!(eh[12] == 0x08 && eh[13] == 0x00)) return 0;         /* ethertype IPv4 */
    ip = eh + 14;
    if(n < 14u + 20u) return 0;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || n < 14u + ihl + 8u) return 0;
    if(ip[9] != 17u) return 0;                                /* UDP */
    ud = ip + ihl;
    if(!(ud[0] == 0 && ud[1] == 67 && ud[2] == 0 && ud[3] == 68)) return 0;  /* server 67->68 */
    bp = ud + 8;
    if(n < (UInt32)(bp - eh) + 1u) return 0;                  /* need bp[0] (BOOTP op) */
    if(bp[0] != 2) return 0;                                  /* BOOTREPLY */

    { int k; for(k = 0; k < 6; k++) eh[k] = ourMac[k]; }      /* L2 dst := our unicast MAC */
    return 1;
}

/* ============================================================================================
 * ★★★★★★ 8-56 (k153): THE CLEAN TEST -- flip a broadcast "who-has-192.168.1.225" ARP request's
 * Ethernet dst to our own MAC. This is k152's trick pointed at ARP, and it is the DECISIVE probe
 * for the broadcast-RX theory because ARP has NO IP layer to confound it: k152's DHCP offer carries
 * IP dst 255.255.255.255, which OT's IP may drop on an unconfigured interface, so we could not tell
 * whether the offer reached the client. An ARP request has no IP dst -- OT's ARP module handles it
 * directly off the DLPI stream. So, in a STATIC-.225 boot where OT owns .225:
 *   - if OT then ANSWERS (a TX ARP op=2 appears) -> the shim's L2-broadcast drop WAS the gate;
 *     broadcast-RX-broken is CONFIRMED, and the Mac becomes reachable at .225.
 *   - if OT still ignores it (no op=2) -> broadcast was never the issue; OT is not receiving our
 *     delivered frames on the path it listens on at all, and we go after the DLPI receive binding.
 * Only a broadcast ARP REQUEST (op=1) whose target IP is .225 is touched; the Ethernet header is
 * not covered by any checksum, so nothing is recomputed. Bounded to a 42-byte Eth+ARP. Host-tested. */
static int ApRxArpToUnicast(UInt8 *eh, UInt32 n, const UInt8 *ourMac)
{
    UInt16 op;
    UInt32 tpa;

    if(!eh || !ourMac || n < 42u) return 0;                   /* Eth(14) + ARP(28) must fit */
    if(!(eh[0] & 0x01)) return 0;                             /* already L2-unicast */
    if(!(eh[12] == 0x08 && eh[13] == 0x06)) return 0;         /* ethertype ARP */
    op  = ((UInt16)eh[20] << 8) | eh[21];                     /* ARP op, at eh+14+6 */
    if(op != 1) return 0;                                     /* REQUEST only (a reply is unicast) */
    tpa = ((UInt32)eh[38] << 24) | ((UInt32)eh[39] << 16) |
          ((UInt32)eh[40] <<  8) |  (UInt32)eh[41];           /* target IP, at eh+14+24 */
    if(tpa != 0xC0A801E1UL) return 0;                         /* only who-has 192.168.1.225 (us) */

    { int k; for(k = 0; k < 6; k++) eh[k] = ourMac[k]; }      /* L2 dst := our unicast MAC */
    return 1;
}

/* ============================================================================================
 * ★★★★★★ 8-47 (k142): DECODE OUR OWN OUTGOING DHCP CLIENT REQUEST.
 *
 * k141 boot 1 was a HEARD boot -- radio joined, 5 OFFERs of 192.168.1.225 delivered to OT, and
 * still 2 NAKs (chaddr = our MAC), never an ACK, then link-local fallback. A DHCPNAK is only sent
 * in reply to a DHCPREQUEST, so OT IS requesting and the server IS refusing -- yet that same
 * server ACKs 192.168.1.225 for this exact MAC under Tiger. So OT is asking for the WRONG thing,
 * and every snapshot to date decoded only RECEIVED DHCP (gDhcpType is RX-only). This decodes what
 * we TRANSMIT. The four fields that separate the client states are:
 *
 *   SELECTING     REQUEST, opt50 = offered IP, opt54 = server id, ciaddr = 0   (server ACKs)
 *   INIT-REBOOT   REQUEST, opt50 = remembered IP, NO opt54,       ciaddr = 0   (NAK if stale)
 *   RENEWING      REQUEST, NO opt50, NO opt54,    ciaddr = current IP          (unicast)
 *   INIT          DISCOVER (opt53 = 1), no opt50/54, ciaddr = 0
 *
 * The header of ApTxForceDhcpBroadcast already recorded OT once "abandoned the stale .90". The
 * leading hypothesis -- INIT-REBOOT for a stale address, NAK'd, never converging to a clean
 * SELECTING request for the .225 on offer -- is CONFIRMED iff our REQUESTs carry opt50 != .225
 * with no opt54, and REFUTED iff they carry opt50 == .225 + opt54 == the server (a different bug).
 *
 * ⚠ Read-only, a leaf: legal at any execution level (the TX path can run below task). Operates on
 *   the SAME SNAP+ethertype+IP body ApTxForceDhcpBroadcast validates (ethertype at [6..7], IP at
 *   [8]) -- OUR gTxEthBody, before encryption. Every read is bounded by bodyLen; the option scan
 *   is bounded by BOTH the declared UDP length and the buffer, and no option body is read until it
 *   is proven to fit. On anything malformed it clears the struct and returns 0, touching nothing.
 *   Host-tested in enet_test.c on guarded pages (the k129 rule: a frame-walker is a bounded
 *   function the host suite can reach, never inline in a selector). */
/* ============================================================================================
 * ★★★★★★ 8-57 (k154): DECODE OUR OWN OUTGOING ICMP, from gTxEthBody (SNAP + IP), before encryption.
 *
 * The clean static boot showed OT answering ARP (the reply reached the Pi) but the Pi's 10 ICMP
 * echoes got 0 replies on the wire. Two possibilities: OT never generates the echo reply, or it
 * does but the reply never reaches the wired Pi. This decodes what OT hands us to transmit: is it
 * an ICMP echo REPLY (type 0), and to what dst IP? The caller pairs it with `da` (the dst MAC) to
 * see whether OT addresses the reply to the Pi (correct -> bridging/uplink issue) or elsewhere
 * (-> OT misrouting). Same bounded, body[6..7]=ethertype / body[8]=IP shape as ApTxDhcpInspect. */
typedef struct {
    int    isIp, isIcmp;
    int    icmpType;     /* -1 if not ICMP; 0 = echo reply, 8 = echo request */
    UInt32 dstIp;        /* IP destination */
} ApTxIcmpInfo;

static int ApTxIcmpInspect(const UInt8 *body, UInt32 bodyLen, ApTxIcmpInfo *o)
{
    const UInt8 *ip;
    UInt32 ihl;

    if(!o) return 0;
    o->isIp = o->isIcmp = 0; o->icmpType = -1; o->dstIp = 0;
    if(bodyLen < 8u + 20u) return 0;
    if(!(body[6] == 0x08 && body[7] == 0x00)) return 0;    /* SNAP ethertype IPv4 */
    ip  = body + 8;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || bodyLen < 8u + ihl) return 0;
    o->isIp  = 1;
    o->dstIp = ((UInt32)ip[16] << 24) | ((UInt32)ip[17] << 16) |
               ((UInt32)ip[18] <<  8) |  (UInt32)ip[19];
    if(ip[9] != 1u) return 0;                              /* protocol != ICMP */
    if(bodyLen < 8u + ihl + 1u) return 0;                  /* need the ICMP type byte */
    o->isIcmp   = 1;
    o->icmpType = ip[ihl];                                 /* ICMP type: 0=echo reply, 8=echo req */
    return 1;
}

typedef struct {
    int    isReq;        /* a BOOTP client request: udp 68->67, op = 1 */
    int    msgType;      /* option 53, or -1 if absent */
    int    hasReqIp;     /* option 50 present */
    int    hasServerId;  /* option 54 present */
    int    bcastFlag;    /* BOOTP flags bit 0x8000, as OT set it (decode BEFORE we force it) */
    UInt32 ciaddr;       /* BOOTP ciaddr (non-zero only when RENEWING/REBINDING) */
    UInt32 requestedIp;  /* option 50 */
    UInt32 serverId;     /* option 54 */
} ApTxDhcpInfo;

static int ApTxDhcpInspect(const UInt8 *body, UInt32 bodyLen, ApTxDhcpInfo *o)
{
    const UInt8 *ip, *ud, *bp;
    UInt32 ihl, ulen, off, blen, avail, q;

    if(!o) return 0;
    o->isReq = 0; o->msgType = -1; o->hasReqIp = 0; o->hasServerId = 0; o->bcastFlag = 0;
    o->ciaddr = 0; o->requestedIp = 0; o->serverId = 0;

    if(bodyLen < 8u) return 0;
    if(!(body[6] == 0x08 && body[7] == 0x00)) return 0;          /* SNAP ethertype IPv4 */
    ip = body + 8;
    if(bodyLen < 8u + 20u) return 0;
    ihl = (UInt32)(ip[0] & 0x0F) * 4u;
    if(ihl < 20u || bodyLen < 8u + ihl + 8u) return 0;
    if(ip[9] != 17u) return 0;                                   /* UDP */
    ud = ip + ihl;
    if(!(ud[0] == 0 && ud[1] == 68 && ud[2] == 0 && ud[3] == 67)) return 0;  /* client 68->67 */
    bp  = ud + 8;
    off = (UInt32)(bp - body);
    if(bodyLen < off + 20u) return 0;                            /* op..ciaddr (bp[0..15]) present */
    if(bp[0] != 1) return 0;                                     /* BOOTREQUEST */

    o->isReq     = 1;
    o->bcastFlag = (bp[10] & 0x80) != 0;
    o->ciaddr    = ((UInt32)bp[12] << 24) | ((UInt32)bp[13] << 16) |
                   ((UInt32)bp[14] <<  8) |  (UInt32)bp[15];

    /* Bound the option scan by BOTH the declared UDP length and the buffer -- exactly as
     * ApIpInspect does, and no option body is dereferenced until it is proven to fit. */
    ulen  = ((UInt32)ud[4] << 8) | ud[5];
    avail = (bodyLen > off) ? (bodyLen - off) : 0u;
    blen  = (ulen >= 8u) ? (ulen - 8u) : 0u;
    if(blen > avail) blen = avail;

    if(blen >= 240u && bp[236]==0x63 && bp[237]==0x82 && bp[238]==0x53 && bp[239]==0x63){
      q = 240u;
      while(q < blen){
        UInt8 code = bp[q];
        if(code == 0xFF) break;                                  /* end of options */
        if(code == 0x00){ q++; continue; }                      /* pad */
        if(q + 2u > blen) break;                                 /* room for the length byte */
        { UInt32 olen = bp[q+1];
          if(q + 2u + olen > blen) break;                        /* the option body must fit */
          if(code == 53u && olen >= 1u) o->msgType = bp[q+2];    /* keep scanning: 50/54 may follow */
          else if(code == 50u && olen >= 4u){
            o->hasReqIp = 1;
            o->requestedIp = ((UInt32)bp[q+2] << 24) | ((UInt32)bp[q+3] << 16) |
                             ((UInt32)bp[q+4] <<  8) |  (UInt32)bp[q+5]; }
          else if(code == 54u && olen >= 4u){
            o->hasServerId = 1;
            o->serverId = ((UInt32)bp[q+2] << 24) | ((UInt32)bp[q+3] << 16) |
                          ((UInt32)bp[q+4] <<  8) |  (UInt32)bp[q+5]; }
          q += 2u + olen; } } }
    return 1;
}

#endif /* AP_ENET_H */
