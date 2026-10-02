/* ap_eapol.h -- the WPA2 four-way handshake layer: state, EAPOL-Key framing, RSNE checking.
 *
 * ★ THE NINTH AND LAST EXTRACTION of the Stage 8 migration. What it carries is the reason the
 * driver binary changes character at 8-4.
 *
 * ⚠⚠ THE SECRETS LIVE HERE, AND THEY ARE NEVER PRINTED. gPmk, gPtk, gANonce, gSNonce and
 *   gGtk are key material. The PMK is PASSWORD-EQUIVALENT for this SSID: anyone holding it can
 *   join the network as effectively as with the passphrase itself. The app's own comment says a
 *   log on a file share is the last place it should appear, and that rule comes across unchanged
 *   -- only PRESENCE and TIMING are ever reported.
 *
 * ⚠⚠ AND THIS IS WHAT MAKES AirPortExtremeDriver.bin UNSHAREABLE. From 8-4 the fragment
 *   contains PSK-derived material compiled in from ap_psk_config.h. It must never be attached to
 *   a release, copied to the public repo, or handed to anyone. AirPortRx.bin has been in that
 *   position since Stage 7; the extension is worse, because an extension is the thing people
 *   install.
 *
 * ⚠ THE PMK MUST BE COMPUTED BEFORE ASSOCIATING, and this is not a preference. Message 1
 *   arrives UNPROMPTED about 100 ms after the association response, and PBKDF2 at 4096 iterations
 *   is 0.3-1 s on this G4 (8.13 ms on the development Mac -- the estimate was extrapolated and
 *   has never been measured on the real hardware). Deriving it after associating loses message 1
 *   every time. A real supplicant knows the PSK at configuration time for exactly this reason.
 *
 * ⚠ THE RSNE IN OUR ASSOCIATION REQUEST IS STATED, NOT DISCOVERED -- we never parse the AP's
 *   beacon. RsneOffersCcmpPsk below is the downgrade defence, and it runs against MESSAGE 3,
 *   where the AP restates its RSNE under a verified MIC. That is the check that cannot be
 *   spoofed, and it is not skipped.
 *
 * ★ HOW IT WAS MADE. One span lifted VERBATIM by line range, never retyped, proven by
 * reconstruction against the original file.
 */
#ifndef AP_EAPOL_H
#define AP_EAPOL_H

#include "ap_wait.h"   /* the receive predicate, and the whole stack below it */

/* ★★★ k210: THE DRIVER NO LONGER CARRIES A PASSPHRASE OR AN SSID. The PMK comes at runtime from the
 *   Preferences file the control panel writes (ap_known.h), so ap_psk_config.h is no longer included here
 *   and is not compiled into the driver. The gPmk/gPtk/gGtk globals below are filled from that runtime PMK
 *   and the handshake, exactly as before -- only their ORIGIN changed, from a compiled constant to a file.
 *   ⚠ The built binary still contains key material WHILE ASSOCIATED (in RAM), but nothing secret is
 *   compiled in, so the extension itself is no longer secret-bearing. (airport_rx.c, the dark app-era
 *   driver, still includes ap_psk_config.h directly for its own Stage 7 self-test; it is not this driver.) */
#include "ap_wpa_kdf.h"    /* ApU8, AP_PTK_LEN, ApPbkdf2Sha1, the PRF */
#include "ap_aes.h"
#include "ap_ccmp.h"
/* Stage 7 state. File scope so the handshake can span the association block without passing a
 * context struct through every call. ⚠ gPmk and gPtk are SECRETS: never printed, never logged,
 * never written to the share. Only presence and timing are reported. */
static ApU8 gPmk[32], gPtk[AP_PTK_LEN], gANonce[32], gSNonce[32], gReplay[8];
/* Stage 8-1: the transmit packet number. Starts at 1 on a fresh PTK and must never repeat under
 * one key -- ApCcmpPnIncrement reports a wrap for exactly that reason. */
static ApU8 gTxPn[6];
static int  gPmkReady = 0, gPtkReady = 0;
/* ⚠ gGtk is the GROUP key: it decrypts every broadcast and multicast frame on this network,
 * for every station, not just ours. Never printed, never logged. */
static ApU8 gGtk[32]; static UInt8 gGtkLen = 0, gGtkId = 0;




/* ============================================================================
 * ★★★★★ STAGE 6-2: THE ASSOCIATION REQUEST.
 *
 * 802.11-2020 §9.3.3.6. Unlike authentication this one carries information elements, and one of
 * them is the RSNE that tells the AP which cipher suite we intend to use. Get that wrong and the
 * AP refuses with status 43 (invalid pairwise cipher) or 40 (invalid IE) rather than silence.
 *
 *     capability (2) | listen interval (2) | SSID IE | supported rates IE | RSNE
 *
 * ★ THE RSNE IS HARDCODED, AND THAT IS A DELIBERATE SCOPE DECISION, NOT LAZINESS.
 *   MINIMAL-STA-SCOPE.md §2: we do not parse the AP's beacon to discover its RSNE, because a
 *   Wi-Fi 7 AP's beacon carries EHT Capabilities, EHT Operation, Multi-Link and possibly RSN
 *   Override elements -- precisely the stuffing that has broken real legacy parsers. We state
 *   what we intend to use instead. The security property is NOT lost: EAPOL message 3 in the
 *   4-way handshake carries the AP's own RSNE and must match this, and that comparison is the
 *   actual defence against a downgrade attack. We skip DISCOVERING the RSNE, not VERIFYING it.
 *
 * The 20 bytes below are the standard WPA2-PSK/CCMP element, and every field is named so the
 * hexdump in the log can be checked against the standard by hand:
 *
 *   30 14                    element 48 (RSN), length 20
 *   01 00                    version 1
 *   00 0F AC 04              group cipher   = 00-0F-AC:4  CCMP-128
 *   01 00  00 0F AC 04       1 pairwise     = 00-0F-AC:4  CCMP-128
 *   01 00  00 0F AC 02       1 AKM          = 00-0F-AC:2  PSK
 *   00 00                    RSN capabilities -- no PMF, no preauth
 *
 * ⚠ RSN CAPABILITIES IS 0x0000, i.e. MFPC and MFPR both clear. The test network was created with PMF
 *   Disabled precisely so this is legal; on a network with PMF Required an AP must refuse this
 *   with status 31. That is the one AP-side setting that would break us silently, and §4 of the
 *   scope document flagged it for exactly this reason.
 *
 * ⚠ LISTEN INTERVAL 1, not 0. It is expressed in beacon intervals and tells the AP how long it
 *   may buffer for us. A mains-powered desktop never sleeps, so 1 is honest and small.
 *
 * THE ORACLE: an association response, status 0, with an AID. The AID is the thing to want --
 * it is a number the AP allocates to us and holds, so receiving one proves a state change on the
 * AP rather than a frame that merely parsed. */
/* ★ ONE PLACE, TWO USERS, AND THAT IS THE WHOLE POINT.
 * The RSNE goes out twice: in the association request, and again as the Key Data of EAPOL-Key
 * message 2. The AP compares the second against the first and aborts the handshake if they
 * differ by a single byte -- that comparison is a downgrade defence, so it is supposed to be
 * unforgiving. Two hand-written copies would drift eventually and the symptom would be a
 * handshake that dies at message 3 for no visible reason. */
#define AP_RSNE_LEN 22                 /* 2 element header + 20 body */


/* ★★ WHAT MESSAGE 3's RSNE IS FOR, AND WHAT k34 GOT WRONG.
 *
 * k34 compared the AP's RSNE against BuildRsne() byte for byte and called the result a downgrade
 * defence. It failed on a perfectly healthy network, and the comparison was wrong in principle:
 *
 *     AP:   30 14 01 00 00 0F AC 04 01 00 00 0F AC 04 01 00 00 0F AC 02  0C 00
 *     ours: 30 14 01 00 00 0F AC 04 01 00 00 0F AC 04 01 00 00 0F AC 02  00 00
 *
 * One field differs -- RSN capabilities, where 0x000C is the AP advertising 16 PTKSA replay
 * counters. Ciphers and AKM are identical. The RSNE in message 3 is the AP's OWN advertisement,
 * not an echo of ours, and wpa_supplicant compares it against what the AP put in its BEACON, to
 * catch an attacker who rewrote that beacon to advertise something weaker.
 *
 * We deliberately never parse the beacon (MINIMAL-STA-SCOPE §2), so we have nothing to compare
 * against and that particular defence is not available to us. It is also not needed: a client
 * that never reads a beacon cannot be downgraded by rewriting one. We demand CCMP and PSK
 * unconditionally, and if the real AP did not offer them the association would have failed.
 *
 * So the check that IS meaningful here is the one below -- does the AP's own RSNE actually offer
 * the pairwise cipher and AKM we chose? -- and RSN capabilities are reported, not compared. */
static int RsneOffersCcmpPsk(const UInt8 *ie,UInt16 len,UInt16 *capsOut)
{
    UInt16 p = 2, n, i;
    int group = 0, pair = 0, akm = 0;
    if(capsOut) *capsOut = 0;
    if(len < 8 || ie[0] != 0x30) return 0;
    p += 2;                                              /* version */
    if(p+4 > len) return 0;
    group = (ie[p]==0x00 && ie[p+1]==0x0F && ie[p+2]==0xAC && ie[p+3]==0x04);
    p = (UInt16)(p + 4);
    if(p+2 > len) return 0;
    n = (UInt16)(ie[p] | ((UInt16)ie[p+1] << 8)); p = (UInt16)(p + 2);
    for(i=0;i<n;i++){ if(p+4 > len) return 0;
      if(ie[p]==0x00 && ie[p+1]==0x0F && ie[p+2]==0xAC && ie[p+3]==0x04) pair = 1;
      p = (UInt16)(p + 4); }
    if(p+2 > len) return 0;
    n = (UInt16)(ie[p] | ((UInt16)ie[p+1] << 8)); p = (UInt16)(p + 2);
    for(i=0;i<n;i++){ if(p+4 > len) return 0;
      if(ie[p]==0x00 && ie[p+1]==0x0F && ie[p+2]==0xAC && ie[p+3]==0x02) akm = 1;
      p = (UInt16)(p + 4); }
    if(capsOut && p+2 <= len) *capsOut = (UInt16)(ie[p] | ((UInt16)ie[p+1] << 8));
    return group && pair && akm;
}

/* ============================================================================
 * ★★★★★ STAGE 7-1: EAPOL-KEY MESSAGE 1, AND THE PTK.
 *
 * Once associated, the AP starts the 4-way handshake on its own -- message 1 arrives unprompted,
 * typically within ~100 ms, and carries the ANonce. We derive the PTK from it and stop there.
 * Message 2 is the next increment, deliberately: if message 1 is parsed wrongly then message 2
 * is garbage, and sending both at once would leave us unable to say which failed.
 *
 * ⚠ THIS IS A DATA FRAME, NOT MANAGEMENT. Everything Stage 6 sent and received was type 0.
 *   EAPOL rides in an 802.11 DATA frame (type 2) from the AP, with FromDS set, LLC/SNAP
 *   encapsulated:
 *
 *     802.11 hdr 24 | AA AA 03 00 00 00 88 8E | EAPOL hdr 4 | EAPOL-Key body
 *
 *   so the SNAP header and its 0x888E ethertype are what identify it, not the subtype. We match
 *   on type 2 from the AP to us and then check the SNAP, rather than guessing a subtype -- a
 *   QoS data frame would be subtype 8 and a plain one subtype 0, and which we get depends on
 *   whether the AP decided we are a WMM station.
 *
 * ★ THE PMK IS COMPUTED BEFORE ASSOCIATING, not here. PBKDF2 at 4096 iterations is 0.3-1 s on
 *   this G4 (measured 8.13 ms on the development Mac), and message 1 will not wait politely
 *   while we do it. A real supplicant knows the PSK at configuration time for the same reason.
 *
 * Offsets below are from the start of the EAPOL header. 802.11-2020 §12.7.2. */
#define EAPOL_SNAP_LEN       8
#define EAPOL_TYPE_KEY       3
#define EAPOL_DESC_RSN       2

#define EAPOL_O_VERSION      0
#define EAPOL_O_TYPE         1
#define EAPOL_O_BODYLEN      2     /* 2, big-endian */
#define EAPOL_O_DESCTYPE     4
#define EAPOL_O_KEYINFO      5     /* 2, big-endian */
#define EAPOL_O_KEYLEN       7     /* 2 */
#define EAPOL_O_REPLAY       9     /* 8 */
#define EAPOL_O_NONCE       17     /* 32  <- the ANonce in message 1 */
#define EAPOL_O_IV          49     /* 16 */
#define EAPOL_O_RSC         65     /* 8 */
#define EAPOL_O_KEYID       73     /* 8 */
#define EAPOL_O_MIC         81     /* 16 */
#define EAPOL_O_DATALEN     97     /* 2 */
#define EAPOL_O_DATA        99

/* Key Information bits, 802.11-2020 Table 12-8. Big-endian on the wire. */
#define KEYINFO_VERSION     0x0007
#define KEYINFO_PAIRWISE    0x0008
#define KEYINFO_INSTALL     0x0040
#define KEYINFO_ACK         0x0080
#define KEYINFO_MIC         0x0100
#define KEYINFO_SECURE      0x0200
#define KEYINFO_ERROR       0x0400
#define KEYINFO_REQUEST     0x0800
#define KEYINFO_ENCRYPTED   0x1000

static const UInt8 kEapolSnap[EAPOL_SNAP_LEN] =
    { 0xAA,0xAA,0x03,0x00,0x00,0x00,0x88,0x8E };

/* ============================================================================
 * ★★★ STAGE 8-1: TRANSMIT AN ENCRYPTED DATA FRAME, AND GET AN ANSWER.
 *
 * ⚠ THIS PROJECT HAS NEVER SENT AN ENCRYPTED FRAME. Stage 7 ended with a CCMP frame decrypted
 *   off the air, which proves the RECEIVE direction and says nothing whatever about transmit.
 *   Every increment from here that assumes we can write to the network is resting on this one.
 *
 * WHY ARP AND NOT PING. ARP is link-layer, so it needs no IP stack, no DHCP and no routing --
 * exactly the parts we have not built. One request, one reply, and the reply is unicast back to
 * our MAC so it arrives encrypted under the pairwise key we already hold.
 *
 * ★ AND THE ANSWER IS KNOWN IN ADVANCE, WHICH IS WHAT MAKES THIS AN ORACLE RATHER THAN A
 *   MEASUREMENT. The gateway's hardware address was read off the developer Mac's own ARP table
 *   before the run and compiled in. So the probe does not merely report "a
 *   reply arrived" -- it compares the sender hardware address against a value recorded
 *   elsewhere, beforehand. That cannot pass by accident, and it is the same class of evidence as
 *   the ICMP sequence 3740 match that closed Stage 7. */
#define AP81_SNAP_ARP_LEN    8
static const UInt8 kArpSnap[AP81_SNAP_ARP_LEN] =
    { 0xAA,0xAA,0x03,0x00,0x00,0x00,0x08,0x06 };   /* LLC/SNAP, ethertype 0x0806 = ARP */

/* The target, and the answer we expect from it. Both are configuration, stated here so that a
 * reader can see exactly what is being claimed. */
static const UInt8 kArpTargetIp[4] = { 192,168,1,1 };
static const UInt8 kArpExpectMac[6] = { 0x70,0xA7,0x41,0xAD,0x7F,0x25 };
/* ⚠⚠ THE SENDER IP MUST BE GENUINELY UNUSED, AND THE FIRST CHOICE WAS NOT.
 *
 * This started as 192.168.1.222 purely because it looked like a spare number. The pre-flight
 * check found it answering ping at 0c:dc:91:dc:e5:52 -- a real device. Claiming it would have
 * caused an IP conflict on somebody's home network, and worse for this experiment, the gateway's
 * ARP reply would have been directed at THAT host's MAC rather than ours, producing a silent
 * "no reply" that looked exactly like a broken transmit path. A reboot to discover, and the
 * wrong conclusion at the end of it.
 *
 * 192.168.1.240 was verified free before this build: three pings unanswered and the ARP cache
 * entry incomplete. ⇒ RE-VERIFY IF THIS IS RUN ON ANOTHER NETWORK. */
static const UInt8 kArpSenderIp[4] = { 192,168,1,240 };

/* Build the ARP request payload -- LLC/SNAP followed by a 28-byte ARP-over-Ethernet packet.
 * Returns the payload length. This is the PLAINTEXT; CCMP encapsulation happens after. */
static UInt16 BuildArpRequestBody(UInt8 *p,const UInt8 *myMac)
{
    UInt16 n = 0; int i;
    for(i=0;i<AP81_SNAP_ARP_LEN;i++) p[n++] = kArpSnap[i];
    p[n++]=0x00; p[n++]=0x01;                       /* hardware type 1, Ethernet */
    p[n++]=0x08; p[n++]=0x00;                       /* protocol type 0x0800, IPv4 */
    p[n++]=6;                                       /* hardware address length */
    p[n++]=4;                                       /* protocol address length */
    p[n++]=0x00; p[n++]=0x01;                       /* opcode 1, request */
    for(i=0;i<6;i++) p[n++] = myMac[i];             /* sender hardware address */
    for(i=0;i<4;i++) p[n++] = kArpSenderIp[i];      /* sender protocol address */
    for(i=0;i<6;i++) p[n++] = 0x00;                 /* target hardware address, unknown */
    for(i=0;i<4;i++) p[n++] = kArpTargetIp[i];      /* target protocol address */
    return n;
}

/* Build the 802.11 data header for an uplink frame. ToDS, so addr1 is the BSSID and addr3 is the
 * final destination -- broadcast for an ARP request.
 * ⚠ The Protected bit is deliberately NOT set here. ApCcmpEncap sets it on its output, and the
 *   AAD is computed from this unmodified header; setting it twice is harmless but setting it
 *   only here would leave the AAD disagreeing with the wire. */
static UInt16 BuildDataHeaderToDs(UInt8 *f,const UInt8 *myMac,const UInt8 *bssid,
                                  const UInt8 *finalDest)
{
    int i;
    f[0]=0x08; f[1]=0x01;                           /* type 2 data, subtype 0, ToDS */
    f[2]=0x00; f[3]=0x00;                           /* duration */
    for(i=0;i<6;i++) f[4+i]  = bssid[i];            /* addr1, the receiver: the AP */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2, the transmitter: us */
    for(i=0;i<6;i++) f[16+i] = finalDest[i];        /* addr3, the final destination */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    return 24;
}

/* Parse an ARP reply out of a decrypted payload. Returns 1 and fills senderMac/senderIp if the
 * payload is LLC/SNAP + ARP + opcode 2. */
static int ParseArpReply(const UInt8 *p,UInt32 len,UInt8 *senderMac,UInt8 *senderIp)
{
    int i;
    if(len < (UInt32)(AP81_SNAP_ARP_LEN + 28)) return 0;
    for(i=0;i<AP81_SNAP_ARP_LEN;i++) if(p[i] != kArpSnap[i]) return 0;
    p += AP81_SNAP_ARP_LEN;
    if(p[0]!=0x00 || p[1]!=0x01) return 0;          /* Ethernet */
    if(p[2]!=0x08 || p[3]!=0x00) return 0;          /* IPv4 */
    if(p[4]!=6 || p[5]!=4) return 0;
    if(p[6]!=0x00 || p[7]!=0x02) return 0;          /* opcode 2, reply */
    for(i=0;i<6;i++) senderMac[i] = p[8+i];
    for(i=0;i<4;i++) senderIp[i]  = p[14+i];
    return 1;
}

/* be16at() already exists in ap_bringup.h, where it reads the big-endian firmware headers.
 * EAPOL is big-endian on the wire too, so it is reused rather than duplicated. */

/* Return a pointer to the EAPOL header inside a received data frame, or 0 if this is not one.
 * `f` points at the 802.11 header. */
static const UInt8 *EapolBody(const UInt8 *f,UInt16 frameLen)
{
    int i;
    if(frameLen < 24 + EAPOL_SNAP_LEN + 4) return 0;
    for(i=0;i<EAPOL_SNAP_LEN;i++)
        if(f[24+i] != kEapolSnap[i]) return 0;
    return f + 24 + EAPOL_SNAP_LEN;
}

/* ============================================================================
 * ★★★★★ STAGE 7-2: EAPOL-KEY MESSAGE 2, AND THE MIC THAT PROVES THE PTK.
 *
 * This is the first frame whose correctness the AP can actually judge. Everything up to here
 * could be well-formed and still built on a wrong key: the PTK is derived independently on both
 * sides and never exchanged, so "we computed 48 bytes" says nothing about whether they are the
 * AP's 48 bytes. The MIC on this frame is the answer. If it verifies, the PMK, the PRF, the
 * min||max ordering, the NUL in the label and the KCK are all correct simultaneously. If it does
 * not, the AP silently discards the frame and retries message 1 -- so the oracle is message 3
 * arriving, not anything in message 2's own send path.
 *
 * ⚠⚠ THE MIC COVERS FROM THE EAPOL HEADER, NOT THE EAPOL-KEY HEADER.
 *   hostap wpa_common.c:186 says the published standard is WRONG about this:
 *     "IEEE Std 802.11i-2004 - 8.5.2 EAPOL-Key frames has an error in the description of the
 *      Key MIC calculation. It includes packet data from the beginning of the EAPOL-Key header,
 *      not EAPOL header. This incorrect change happened during final editing of the standard
 *      and the correct behavior is defined in the last draft (IEEE 802.11i/D10)."
 *   So the input starts at the EAPOL version byte and runs 4 + body_length bytes, with the
 *   16-byte MIC field ZEROED. Implement it from the standard and it fails with no diagnostic.
 *
 * ⚠ DESCRIPTOR VERSION 2, measured in k32's message 1, fixes the algorithm: HMAC-SHA1 truncated
 *   to 16 bytes. Not AES-CMAC, which version 3 would have required along with mbedTLS.
 *
 * Frame shape, 153 bytes for our 22-byte RSNE:
 *   802.11 data hdr 24 (ToDS) | LLC/SNAP 8 | EAPOL hdr 4 | EAPOL-Key 95 | Key Data 22
 */
#define EAPOL_KEY_FIXED_LEN  95        /* desc type through key data length, inclusive */

/* ★ ONE BUILDER FOR MESSAGES 2 AND 4, because they differ in three fields and nothing else.
 * Message 4 is message 2 with the SECURE bit carried over from message 3, a zero nonce, and no
 * Key Data. Writing it twice would mean two copies of the MIC input range -- the one detail the
 * published standard gets wrong -- and the copies would drift. hostap wpa.c:1634 builds it the
 * same way, by masking message 3's key_info down to SECURE and OR-ing the rest back on.
 *
 *   keyInfo    caller supplies it; msg 2 is ver|PAIRWISE|MIC, msg 4 adds SECURE
 *   nonce      msg 2 sends the SNonce, msg 4 sends 32 zero bytes
 *   withRsne   msg 2 carries the RSNE as Key Data, msg 4 carries nothing
 *
 * ★ k208: AND THE GROUP KEY HANDSHAKE'S MESSAGE 2, which is message 4 without PAIRWISE: ver|MIC|SECURE,
 *   group message 1's replay counter, zero nonce, no Key Data (wpa_supplicant_send_2_of_2). ApGkRenew
 *   (ap_shim.c) calls this for it and sends the EAPOL part through the encrypted data path, because the
 *   pairwise key is installed by then. Still one builder. */
static UInt16 BuildEapolKeyFrame(UInt8 *f,const UInt8 *myMac,const UInt8 *bssid,
                                 const UInt8 *nonce,const UInt8 *replay,
                                 const UInt8 *kck,UInt16 keyInfoIn,int withRsne)
{
    UInt8 *eap, *mic;
    UInt16 n = 0, keyInfo, dataLen, bodyLen;
    ApU8 hash[AP_SHA1_LEN];
    int i;

    /* ---- 802.11 data header, ToDS. addr1 is the AP because the frame is going THROUGH it. */
    f[0]=0x08; f[1]=0x01;                           /* type 2 data, subtype 0, ToDS */
    f[2]=0x00; f[3]=0x00;                           /* duration */
    for(i=0;i<6;i++) f[4+i]  = bssid[i];            /* addr1, the BSSID (receiver) */
    for(i=0;i<6;i++) f[10+i] = myMac[i];            /* addr2, us (transmitter) */
    for(i=0;i<6;i++) f[16+i] = bssid[i];            /* addr3, the AP (destination) */
    f[22]=0x00; f[23]=0x00;                         /* seq ctl -- HWSEQ fills it */
    n = 24;

    for(i=0;i<EAPOL_SNAP_LEN;i++) f[n++] = kEapolSnap[i];

    eap = f + n;
    dataLen = withRsne ? AP_RSNE_LEN : 0;
    bodyLen = (UInt16)(EAPOL_KEY_FIXED_LEN + dataLen);

    eap[EAPOL_O_VERSION]  = 0x02;
    eap[EAPOL_O_TYPE]     = EAPOL_TYPE_KEY;
    eap[EAPOL_O_BODYLEN]  = (UInt8)(bodyLen >> 8);      /* big-endian on the wire */
    eap[EAPOL_O_BODYLEN+1]= (UInt8)(bodyLen & 0xFF);
    eap[EAPOL_O_DESCTYPE] = EAPOL_DESC_RSN;

    keyInfo = keyInfoIn;
    eap[EAPOL_O_KEYINFO]   = (UInt8)(keyInfo >> 8);
    eap[EAPOL_O_KEYINFO+1] = (UInt8)(keyInfo & 0xFF);

    /* ⚠ KEY LENGTH IS ZERO IN MESSAGE 2 FOR RSN. hostap wpa.c:561 puts 0 for WPA_PROTO_RSN and
     * echoes message 1's value only for the older WPA1 protocol. */
    eap[EAPOL_O_KEYLEN]=0x00; eap[EAPOL_O_KEYLEN+1]=0x00;

    /* ⚠ THE REPLAY COUNTER IS ECHOED, NOT INVENTED. The AP matches it against the one it sent
     * in message 1 and drops anything else, which is how it rejects replayed handshakes. */
    for(i=0;i<8;i++)  eap[EAPOL_O_REPLAY+i] = replay[i];
    for(i=0;i<32;i++) eap[EAPOL_O_NONCE+i]  = nonce ? nonce[i] : 0;
    for(i=0;i<16;i++) eap[EAPOL_O_IV+i]     = 0;
    for(i=0;i<8;i++)  eap[EAPOL_O_RSC+i]    = 0;
    for(i=0;i<8;i++)  eap[EAPOL_O_KEYID+i]  = 0;

    mic = eap + EAPOL_O_MIC;
    for(i=0;i<16;i++) mic[i] = 0;                   /* ★ zeroed BEFORE the MIC is computed */

    eap[EAPOL_O_DATALEN]   = (UInt8)(dataLen >> 8);
    eap[EAPOL_O_DATALEN+1] = (UInt8)(dataLen & 0xFF);
    if(withRsne) (void)BuildRsne(eap + EAPOL_O_DATA);  /* byte-identical to the assoc request's */

    /* ★ HMAC-SHA1(KCK, EAPOL header .. end of key data), truncated to 16. The length is
     * 4 + bodyLen because it starts at the EAPOL version byte -- see the header comment. */
    ApHmacSha1((const ApU8*)kck,16UL,(const ApU8*)eap,(ApU32)(4 + bodyLen),hash);
    for(i=0;i<16;i++) mic[i] = hash[i];

    n = (UInt16)(n + 4 + bodyLen);
    return n;
}

#endif /* AP_EAPOL_H */
