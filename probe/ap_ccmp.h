/* ap_ccmp.h -- AES-CCM and CCMP decapsulation. Stage 7-5.
 *
 * ★ WHY THIS IS WRITTEN AND NOT LINKED, HAVING SAID 7-5 WAS THE POINT TO RECONSIDER.
 * It was, and here is the reconsideration. mbedTLS would supply AES-CCM, which has published
 * vectors and is the part least likely to be got wrong. It would NOT supply the CCMP nonce, the
 * AAD masking, or the packet-number handling -- and those are where the real hazard lives,
 * because a wrong nonce or a reused PN is a security failure that still decrypts correctly on a
 * cooperative network. Linking a library would move the safe part out of our hands and leave the
 * dangerous part exactly where it is. MINIMAL-STA-SCOPE.md §1 says the same: only the framing is
 * ours. So the framing gets the care, and the primitive gets RFC 3610's known-answer tests.
 *
 * ⚠ WHAT IS DELIBERATELY NOT HERE YET: replay protection. A real receiver keeps the highest PN
 *   seen per key and per TID and discards anything not greater, which is what stops an attacker
 *   replaying a captured frame. This probe DECRYPTS and reports; it does not defend. Recorded as
 *   a gap rather than left to be discovered, because "the MIC verified" reads like safety and is
 *   not the same thing.
 *
 * ⚠ AND THIS IS STILL NOT CONSTANT TIME -- see ap_aes.h. Same trade, same reasoning.
 *
 * Structure follows hostap src/crypto/aes-ccm.c; the CCMP nonce and AAD follow the Linux kernel
 * net/mac80211/wpa.c `ccmp_special_blocks` and `ccmp_gcmp_aad`, which were read rather than
 * recalled because every one of those masks fails silently.
 */
#ifndef AP_CCMP_H
#define AP_CCMP_H

#include "ap_aes.h"
#include "ap_aes_fast.h"

#define CCMP_HDR_LEN   8      /* the CCMP header between the 802.11 header and the payload */
#define CCMP_MIC_LEN   8
#define CCMP_PN_LEN    6
#define CCMP_M         8      /* MIC length for CCMP */
#define CCMP_L         2      /* length field size, so the nonce is 15 - 2 = 13 bytes */

/* ★★★ k199: WHICH AES THE CCM LAYER RUNS -- AND WHY THAT IS DECIDED ON THE G4, AT RUN TIME.
 * The CCM code below is unchanged in structure; what changed is the block cipher under it. Every
 * block now goes through AP_CCM_BLOCK, which runs ap_aes_fast.h's table-driven cipher (compiled at
 * O2) or ap_aes.h's byte-wise reference (unoptimised, as through k198).
 *
 * The fast path is not trusted because the Mac's tests passed. kdf_test.c proves the C, on clang,
 * on a little-endian 64-bit host; it cannot prove what gcc's PowerPC O2 code generator made of it.
 * So ApAesFastSelfTest (bottom of this file) re-runs the differential ON THE G4 at bring-up --
 * FIPS-197, the key schedule word for byte, 512 random blocks and a full-size CCM frame, each fast
 * against reference -- and only a pass sets gApAesFast. Until then, and forever after a failure,
 * the reference path runs: slower, but it is the code that carried every frame through k198. A
 * crypto path that failed its own test on this machine never touches a frame. */
typedef struct {
    union { ApW32 w[AP_AESF_WORDS]; ApU8 rk[AP_AES_EXPKEY]; } s;   /* ONE schedule, not both: the
                                                               * receive path runs on the secondary
                                                               * interrupt's stack */
    int fast;
} ApAesCtx;

/* 0 until ApAesFastSelfTest passes on THIS machine. One copy in the driver: only ap_shim.c includes
 * this header (ap_otmodl.c includes no ap_*.h). -1 = the self-test has not run. */
static int gApAesFast     = 0;
static int gApAesSelfTest = -1;

/* ⚠ A dispatch that quietly always took the reference arm would pass every output check there is --
 *   both arms produce the same bytes -- and would only show as "no faster". So a TEST build (kdf_test.c
 *   defines AP_AES_COUNT_BLOCKS) counts which arm each block took; the driver defines nothing and pays
 *   nothing. */
#ifdef AP_AES_COUNT_BLOCKS
static unsigned long gApAesFastBlocks = 0, gApAesRefBlocks = 0;
#define AP_AES_COUNT_FAST() gApAesFastBlocks++
#define AP_AES_COUNT_REF()  gApAesRefBlocks++
#else
#define AP_AES_COUNT_FAST() ((void)0)
#define AP_AES_COUNT_REF()  ((void)0)
#endif
/* each arm counts ITSELF, so the count reports the cipher that actually ran, not the flag's intent */
#define AP_CCM_BLOCK(k,in,out) do{ if((k)->fast){ AP_AES_COUNT_FAST(); ApAesFastEncryptBlock((k)->s.w,(in),(out)); } \
                                   else         { AP_AES_COUNT_REF();  ApAesEncryptBlock((k)->s.rk,(in),(out)); } }while(0)

/* ⚠ EVERYTHING FROM HERE TO THE MATCHING pop_options IS COMPILED AT O2 -- the CCM loops as well as
 *   the cipher. Pure arithmetic over caller buffers, like ap_aes_fast.h; see its header for why that
 *   is safe here and nowhere else yet. The reference ApAesEncryptBlock is DEFINED in ap_aes.h, above
 *   this region, so it stays unoptimised: the benchmark's "before" is still k198's cipher. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif

static void ApAesCtxInitMode(ApAesCtx *k,const ApU8 *key,int fast)
{
    k->fast = fast;
    if(fast) ApAesFastExpandKey(key,k->s.w);
    else     ApAesExpandKey(key,k->s.rk);
}

/* ---- CCM primitive. RFC 3610, and hostap aes-ccm.c for the block layout. ---- */
static void ApCcmXorBlock(ApU8 *d,const ApU8 *s)
{ int i; for(i=0;i<16;i++) d[i] ^= s[i]; }

static void ApCcmAuthStart(const ApAesCtx *k,ApU32 M,ApU32 L,const ApU8 *nonce,
                           const ApU8 *aad,ApU32 aadLen,ApU32 plainLen,ApU8 *x)
{
    ApU8 b[16], aadBuf[32];
    ApU32 i;
    /* B_0: Flags | Nonce | l(m) */
    b[0] = (ApU8)((aadLen ? 0x40 : 0) | (((M-2)/2) << 3) | (L-1));
    for(i=0;i<15-L;i++) b[1+i] = nonce[i];
    b[16-2] = (ApU8)((plainLen >> 8) & 0xFF);
    b[16-1] = (ApU8)(plainLen & 0xFF);
    AP_CCM_BLOCK(k,b,x);                               /* X_1 = E(K, B_0) */
    if(!aadLen) return;
    aadBuf[0] = (ApU8)((aadLen >> 8) & 0xFF);
    aadBuf[1] = (ApU8)(aadLen & 0xFF);
    for(i=0;i<aadLen && i<30UL;i++) aadBuf[2+i] = aad[i];
    for(i=2+aadLen;i<32UL;i++)      aadBuf[i] = 0;
    ApCcmXorBlock(aadBuf,x);
    AP_CCM_BLOCK(k,aadBuf,x);                          /* X_2 */
    if(aadLen > 16-2){
      ApCcmXorBlock(aadBuf+16,x);
      AP_CCM_BLOCK(k,aadBuf+16,x); }                   /* X_3 */
}

static void ApCcmAuth(const ApAesCtx *k,const ApU8 *data,ApU32 len,ApU8 *x)
{
    ApU32 last = len % 16, i, j;
    for(i=0;i<len/16;i++){
      ApCcmXorBlock(x,data); data += 16;
      AP_CCM_BLOCK(k,x,x); }
    if(last){
      for(j=0;j<last;j++) x[j] ^= *data++;             /* zero-padded final block */
      AP_CCM_BLOCK(k,x,x); }
}

static void ApCcmCtrStart(ApU32 L,const ApU8 *nonce,ApU8 *a)
{
    ApU32 i;
    a[0] = (ApU8)(L - 1);                              /* A_i flags = L' */
    for(i=0;i<15-L;i++) a[1+i] = nonce[i];
}

/* CTR over the payload. Used for both directions -- XOR is its own inverse. */
static void ApCcmCtr(const ApAesCtx *k,const ApU8 *in,ApU32 len,ApU8 *out,ApU8 *a)
{
    ApU8 s[16];
    ApU32 last = len % 16, i, j;
    for(i=1;i<=len/16;i++){
      a[14] = (ApU8)((i >> 8) & 0xFF); a[15] = (ApU8)(i & 0xFF);
      AP_CCM_BLOCK(k,a,s);
      for(j=0;j<16;j++) out[(i-1)*16+j] = (ApU8)(in[(i-1)*16+j] ^ s[j]); }
    if(last){
      ApU32 base = (len/16)*16;
      i = len/16 + 1;
      a[14] = (ApU8)((i >> 8) & 0xFF); a[15] = (ApU8)(i & 0xFF);
      AP_CCM_BLOCK(k,a,s);
      for(j=0;j<last;j++) out[base+j] = (ApU8)(in[base+j] ^ s[j]); }
}

static void ApCcmTag(const ApAesCtx *k,ApU32 M,const ApU8 *x,ApU8 *a,ApU8 *tag)
{
    ApU8 s0[16];
    ApU32 i;
    a[14] = 0; a[15] = 0;                              /* A_0 */
    AP_CCM_BLOCK(k,a,s0);
    for(i=0;i<M;i++) tag[i] = (ApU8)(x[i] ^ s0[i]);    /* U = T XOR S_0 */
}

/* AES-CCM decrypt-and-verify under an expanded key. Returns 1 if the MIC matches, 0 otherwise.
 * ⚠ On failure `out` holds decrypted-but-unauthenticated bytes. The caller must not use them;
 *   that is why the return value exists and why callers here treat 0 as "discard the frame". */
static int ApAesCcmDecryptCtx(const ApAesCtx *k,const ApU8 *nonce,
                              const ApU8 *aad,ApU32 aadLen,
                              const ApU8 *cipher,ApU32 cipherLen,
                              const ApU8 *mic,ApU8 *out)
{
    ApU8 x[16], a[16], tag[16];
    ApU32 i;
    ApCcmCtrStart(CCMP_L,nonce,a);
    ApCcmCtr(k,cipher,cipherLen,out,a);                /* decrypt first: CBC-MAC is over plaintext */
    ApCcmAuthStart(k,CCMP_M,CCMP_L,nonce,aad,aadLen,cipherLen,x);
    ApCcmAuth(k,out,cipherLen,x);
    ApCcmCtrStart(CCMP_L,nonce,a);
    ApCcmTag(k,CCMP_M,x,a,tag);
    for(i=0;i<CCMP_M;i++) if(tag[i] != mic[i]) return 0;
    return 1;
}

/* AES-CCM encrypt-and-tag under an expanded key. `out` receives plainLen bytes, `mic` CCMP_M.
 * ⚠ ORDER IS THE OPPOSITE OF DECRYPT: CCM authenticates the PLAINTEXT, so MAC first, then CTR.
 *   See the Stage 8-0 note further down; kdf_test.c checks this against RFC 3610 independently. */
static void ApAesCcmEncryptCtx(const ApAesCtx *k,const ApU8 *nonce,
                               const ApU8 *aad,ApU32 aadLen,
                               const ApU8 *plain,ApU32 plainLen,
                               ApU8 *out,ApU8 *mic)
{
    ApU8 x[16], a[16], tag[16];
    ApU32 i;
    ApCcmAuthStart(k,CCMP_M,CCMP_L,nonce,aad,aadLen,plainLen,x);
    ApCcmAuth(k,plain,plainLen,x);                     /* MAC the PLAINTEXT, before encrypting */
    ApCcmCtrStart(CCMP_L,nonce,a);
    ApCcmTag(k,CCMP_M,x,a,tag);                        /* tag uses counter block 0 */
    for(i=0;i<CCMP_M;i++) mic[i] = tag[i];
    ApCcmCtrStart(CCMP_L,nonce,a);
    ApCcmCtr(k,plain,plainLen,out,a);                  /* payload uses counter blocks 1.. */
}

/* The data path's entry points: same signatures as through k198, so ApCcmpDecap/ApCcmpEncap and
 * every caller are untouched. The key is expanded per frame -- a few hundred instructions at O2,
 * against ~190 blocks -- rather than cached, so a rekey can never leave a stale schedule behind. */
static int ApAesCcmDecrypt(const ApU8 *key,const ApU8 *nonce,
                           const ApU8 *aad,ApU32 aadLen,
                           const ApU8 *cipher,ApU32 cipherLen,
                           const ApU8 *mic,ApU8 *out)
{
    ApAesCtx k;
    ApAesCtxInitMode(&k,key,gApAesFast);
    return ApAesCcmDecryptCtx(&k,nonce,aad,aadLen,cipher,cipherLen,mic,out);
}

static void ApAesCcmEncrypt(const ApU8 *key,const ApU8 *nonce,
                            const ApU8 *aad,ApU32 aadLen,
                            const ApU8 *plain,ApU32 plainLen,
                            ApU8 *out,ApU8 *mic)
{
    ApAesCtx k;
    ApAesCtxInitMode(&k,key,gApAesFast);
    ApAesCcmEncryptCtx(&k,nonce,aad,aadLen,plain,plainLen,out,mic);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

/* ---- CCMP framing: the part that is genuinely ours ----
 * The nonce and AAD below are transcribed from net/mac80211/wpa.c. Every mask in them fails
 * silently if it is wrong -- the frame simply does not authenticate, with no clue which byte
 * was at fault -- so none of it is from memory. */

/* CCMP header -> PN. mac80211 ccmp_hdr2pn. The PN is stored in a deliberately scrambled order
 * with a reserved byte and the ExtIV flag interleaved, so it cannot be read off as an integer. */
static void ApCcmpHdrToPn(const ApU8 *h,ApU8 *pn)
{ pn[0]=h[7]; pn[1]=h[6]; pn[2]=h[5]; pn[3]=h[4]; pn[4]=h[1]; pn[5]=h[0]; }

static int ApCcmpKeyId(const ApU8 *h){ return (h[3] >> 6) & 0x03; }
static int ApCcmpHasExtIv(const ApU8 *h){ return (h[3] & 0x20) != 0; }

/* Build the 13-byte CCMP nonce: Nonce Flags | A2 | PN.
 * Flags = priority (b0..b3) | management (b4). Non-QoS data gives priority 0. */
static void ApCcmpNonce(const ApU8 *hdr,const ApU8 *pn,int isMgmt,ApU8 qosTid,ApU8 *nonce)
{
    int i;
    nonce[0] = (ApU8)(qosTid | (isMgmt ? 0x10 : 0x00));
    for(i=0;i<6;i++) nonce[1+i] = hdr[10+i];           /* A2, the transmitter */
    for(i=0;i<6;i++) nonce[7+i] = pn[i];
}

/* Build the AAD. Returns its length. hdr points at the 802.11 header.
 *   FC (masked) | A1 | A2 | A3 | SC (masked) | [A4] | [QC]
 * ⚠ THE MASKS ARE THE WHOLE POINT and each is a silent failure if wrong:
 *     clear Retry (0x0800), PwrMgmt (0x1000), MoreData (0x2000)
 *     clear subtype bits 0x0070 for non-management frames
 *     SET Protected (0x4000) -- it is set on the wire and must be set here
 *     mask the sequence number, KEEP the fragment number: seq_ctrl low byte & 0x0F, next byte 0
 */
static ApU32 ApCcmpAad(const ApU8 *hdr,int isMgmt,int a4,ApU8 qosTid,int isQos,ApU8 *aad)
{
    ApU32 fc = (ApU32)hdr[0] | ((ApU32)hdr[1] << 8);
    ApU32 n;
    int i;
    fc &= ~0x0800UL;      /* Retry */
    fc &= ~0x1000UL;      /* PwrMgmt */
    fc &= ~0x2000UL;      /* MoreData */
    if(!isMgmt) fc &= ~0x0070UL;
    if(isQos)   fc &= ~0x8000UL;                       /* Order, for QoS data */
    fc |= 0x4000UL;       /* Protected */

    aad[0] = (ApU8)(fc & 0xFF); aad[1] = (ApU8)(fc >> 8);   /* little-endian, as on the wire */
    for(i=0;i<18;i++) aad[2+i] = hdr[4+i];             /* A1, A2, A3 */
    aad[20] = (ApU8)(hdr[22] & 0x0F);                  /* SC: keep the fragment number only */
    aad[21] = 0;
    n = 22;
    if(a4){ for(i=0;i<6;i++) aad[22+i] = hdr[24+i]; n = 28; }
    if(isQos){ aad[n] = qosTid; aad[n+1] = 0; n += 2; }
    return n;
}

/* Decapsulate one received CCMP frame in place-ish.
 *   frame    the 802.11 header onwards, as the card delivered it
 *   frameLen its total length, INCLUDING the header, CCMP header and MIC
 *   key      the 16-byte TK (unicast) or GTK (group)
 *   out      receives the plaintext payload; outLen its length
 * Returns 1 if the MIC verified. 0 means discard the frame and do not look at `out`. */
static int ApCcmpDecap(const ApU8 *frame,ApU32 frameLen,const ApU8 *key,
                       ApU8 *out,ApU32 *outLen,ApU8 *pnOut,int *keyIdOut)
{
    ApU8 nonce[13], aad[32], pn[6];
    ApU32 fc = (ApU32)frame[0] | ((ApU32)frame[1] << 8);
    int isMgmt = (((fc >> 2) & 3) == 0);
    int isData = (((fc >> 2) & 3) == 2);
    int isQos  = isData && (((fc >> 4) & 0x0F) == 8);
    int a4     = ((fc & 0x0300) == 0x0300);            /* ToDS and FromDS both set */
    ApU32 hdrLen = (ApU32)(24 + (a4 ? 6 : 0) + (isQos ? 2 : 0));
    ApU8  qosTid = 0;
    ApU32 aadLen, dataLen;

    if(outLen) *outLen = 0;
    if(frameLen < hdrLen + CCMP_HDR_LEN + CCMP_MIC_LEN) return 0;
    if(isQos) qosTid = (ApU8)(frame[hdrLen-2] & 0x0F);

    if(!ApCcmpHasExtIv(frame + hdrLen)) return 0;      /* no ExtIV means this is not CCMP */
    ApCcmpHdrToPn(frame + hdrLen,pn);
    if(keyIdOut) *keyIdOut = ApCcmpKeyId(frame + hdrLen);
    if(pnOut){ int i; for(i=0;i<6;i++) pnOut[i] = pn[i]; }

    ApCcmpNonce(frame,pn,isMgmt,qosTid,nonce);
    aadLen  = ApCcmpAad(frame,isMgmt,a4,qosTid,isQos,aad);
    dataLen = frameLen - hdrLen - CCMP_HDR_LEN - CCMP_MIC_LEN;

    if(!ApAesCcmDecrypt(key,nonce,aad,aadLen,
                        frame + hdrLen + CCMP_HDR_LEN, dataLen,
                        frame + frameLen - CCMP_MIC_LEN, out)) return 0;
    if(outLen) *outLen = dataLen;
    return 1;
}

/* ============================================================================
 * ★★★ STAGE 8-0: CCMP ENCAPSULATION -- THE TRANSMIT DIRECTION.
 *
 * ⚠ THIS PROJECT HAS NEVER ENCRYPTED A FRAME. Stage 7 proved we can READ protected traffic;
 *   it proved nothing about writing it, and read evidence is not write evidence. Everything
 *   below is new code even though it looks like a mirror of the decap above.
 *
 * The saving grace is that the hard parts are SHARED. The nonce, the AAD and their masks are
 * the same functions the receive path already uses, and those are validated by a cryptographic
 * MIC against a live access point -- so a bug here is confined to the new sequencing, not to
 * the framing rules.
 *
 * ORDER MATTERS AND IT IS THE OPPOSITE OF DECRYPT. CCM authenticates the PLAINTEXT, so encrypt
 * must CBC-MAC first and then CTR-encrypt; the decrypt path above must CTR-decrypt first and
 * then CBC-MAC. Getting this backwards produces a MIC that verifies against itself and fails
 * against every real implementation -- which is exactly the class of bug a round-trip test
 * cannot catch on its own. That is why kdf_test.c also checks encap output against the RFC 3610
 * vectors and against the decap path independently. */

/* AES-CCM encrypt-and-tag: ApAesCcmEncrypt, with ApAesCcmEncryptCtx under it, now lives with the CCM
 * primitives near the top of this file (k199), inside the O2 region, in exactly the order described
 * above -- CBC-MAC over the plaintext first, then CTR. */

/* PN -> CCMP header. The inverse of ApCcmpHdrToPn, and deliberately written as its mirror so
 * the two can be read against each other:
 *     h[0]=pn[5] h[1]=pn[4] h[2]=0(reserved) h[3]=ExtIV|keyid<<6 h[4]=pn[3] h[5]=pn[2]
 *     h[6]=pn[1] h[7]=pn[0] */
static void ApCcmpPnToHdr(const ApU8 *pn,int keyId,ApU8 *h)
{
    h[0] = pn[5]; h[1] = pn[4];
    h[2] = 0;
    h[3] = (ApU8)(0x20 | ((keyId & 0x03) << 6));       /* ExtIV must be set for CCMP */
    h[4] = pn[3]; h[5] = pn[2]; h[6] = pn[1]; h[7] = pn[0];
}

/* Increment a 48-bit PN, big-endian (pn[0] is the most significant byte).
 * Returns 0 if it wrapped, which must never be ignored: a repeated PN under the same key
 * destroys CCMP's security outright. The caller rekeys or stops. */
static int ApCcmpPnIncrement(ApU8 *pn)
{
    int i;
    for(i=5;i>=0;i--){ if(++pn[i] != 0) return 1; }
    return 0;
}

/* Encapsulate one frame for transmission.
 *   hdr/hdrLen  the 802.11 header, already built, WITHOUT the Protected bit set
 *   plain       the payload (LLC/SNAP + packet)
 *   pn          the 48-bit packet number to use; the caller increments it
 *   out         receives hdr | CCMP header | ciphertext | MIC
 * Returns the total length written, or 0 if the arguments are inconsistent.
 *
 * ⚠ THE PROTECTED BIT IS SET HERE, ON THE COPY IN `out`, AFTER the AAD is computed from the
 *   caller's header. ApCcmpAad sets it in its own working copy regardless, so the two agree --
 *   but the frame that goes on the air must carry it too, and forgetting that produces a frame
 *   the AP silently discards. */
static ApU32 ApCcmpEncap(const ApU8 *hdr,ApU32 hdrLen,
                         const ApU8 *plain,ApU32 plainLen,
                         const ApU8 *key,const ApU8 *pn,int keyId,
                         int isMgmt,int a4,ApU8 qosTid,int isQos,
                         ApU8 *out)
{
    ApU8 nonce[13], aad[32];
    ApU32 aadLen, i;
    if(hdrLen < 24) return 0;
    for(i=0;i<hdrLen;i++) out[i] = hdr[i];
    ApCcmpNonce(hdr,pn,isMgmt,qosTid,nonce);
    aadLen = ApCcmpAad(hdr,isMgmt,a4,qosTid,isQos,aad);
    ApCcmpPnToHdr(pn,keyId,out + hdrLen);
    ApAesCcmEncrypt(key,nonce,aad,aadLen,plain,plainLen,
                    out + hdrLen + CCMP_HDR_LEN,
                    out + hdrLen + CCMP_HDR_LEN + plainLen);
    out[1] = (ApU8)(out[1] | 0x40);                    /* Protected, bit 14 of the FC */
    return hdrLen + CCMP_HDR_LEN + plainLen + CCMP_MIC_LEN;
}

/* ============================================================================
 * ★★★ k199: THE ON-TARGET SELF-TEST. The only thing that turns the fast path on.
 *
 * kdf_test.c proves the fast cipher on the Mac. This proves it where it RUNS: the reference cipher and
 * the fast one, both compiled by Retro68 gcc for the G4, must agree on everything below before
 * gApAesFast becomes 1. Deterministic (a fixed xorshift seed), so a failure number seen in the G4's log
 * reproduces exactly on the host.
 *
 *   1  FIPS-197 C.1 through the fast cipher
 *   2  the fast key schedule, word for byte, against ApAesExpandKey
 *   3  AP_AES_ST_BLOCKS random keys and blocks: fast == reference
 *   4  a full-size (1500-byte) CCM encrypt, fast and reference: same ciphertext, same MIC
 *   5  the fast CCM decrypt verifies that MIC and restores the plaintext
 *   6  a MIC with one bit flipped is REJECTED by the fast path -- a decrypt that "passes" everything is
 *      the failure a round trip alone cannot see
 *
 * Returns 0 and sets gApAesFast = 1 on a pass; otherwise the number of the first failed check, leaving
 * the reference path in force. Task level only: ~20 ms, most of it the unoptimised reference cipher.
 * The buffers are static so the benchmark in ap_shim.c can reuse the same plaintext. */
#define AP_AES_ST_BLOCKS 512
#define AP_AES_ST_LEN    1500           /* a full-size CCMP payload: ~190 AES blocks of CTR + CBC-MAC */
static ApU8 gApAesStPlain[AP_AES_ST_LEN], gApAesStOutA[AP_AES_ST_LEN], gApAesStOutB[AP_AES_ST_LEN];
static ApU8 gApAesStKey[16], gApAesStNonce[13], gApAesStAad[22];

static ApW32 ApAesStNext(ApW32 *x)      /* xorshift32 */
{ *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

static int ApAesFastSelfTestRun(void)
{
    static const ApU8 fk[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                 0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    static const ApU8 fp[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                 0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const ApU8 fc[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                 0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    ApW32 w[AP_AESF_WORDS], x = 0x2545F491U;
    ApU8  rk[AP_AES_EXPKEY], a[16], b[16], key[16], blk[16], micA[8], micB[8];
    ApAesCtx kf, kr;
    int i, j;

    ApAesFastExpandKey(fk,w);                                                    /* 1 */
    ApAesFastEncryptBlock(w,fp,a);
    for(i = 0; i < 16; i++) if(a[i] != fc[i]) return 1;

    ApAesExpandKey(fk,rk);                                                       /* 2 */
    for(i = 0; i < AP_AESF_WORDS; i++) if(w[i] != AP_AESF_GET(rk + 4*i)) return 2;

    for(j = 0; j < AP_AES_ST_BLOCKS; j++){                                       /* 3 */
        for(i = 0; i < 16; i++) key[i] = (ApU8)ApAesStNext(&x);
        for(i = 0; i < 16; i++) blk[i] = (ApU8)(ApAesStNext(&x) >> 8);
        ApAesFastExpandKey(key,w);  ApAesFastEncryptBlock(w,blk,a);
        ApAesExpandKey(key,rk);     ApAesEncryptBlock(rk,blk,b);
        for(i = 0; i < 16; i++) if(a[i] != b[i]) return 3; }

    for(i = 0; i < AP_AES_ST_LEN; i++) gApAesStPlain[i] = (ApU8)ApAesStNext(&x);  /* 4 */
    for(i = 0; i < 16; i++) gApAesStKey[i]   = (ApU8)ApAesStNext(&x);
    for(i = 0; i < 13; i++) gApAesStNonce[i] = (ApU8)ApAesStNext(&x);
    for(i = 0; i < 22; i++) gApAesStAad[i]   = (ApU8)ApAesStNext(&x);
    ApAesCtxInitMode(&kf,gApAesStKey,1);
    ApAesCtxInitMode(&kr,gApAesStKey,0);
    ApAesCcmEncryptCtx(&kf,gApAesStNonce,gApAesStAad,22,gApAesStPlain,AP_AES_ST_LEN,gApAesStOutA,micA);
    ApAesCcmEncryptCtx(&kr,gApAesStNonce,gApAesStAad,22,gApAesStPlain,AP_AES_ST_LEN,gApAesStOutB,micB);
    for(i = 0; i < AP_AES_ST_LEN; i++) if(gApAesStOutA[i] != gApAesStOutB[i]) return 4;
    for(i = 0; i < CCMP_M; i++) if(micA[i] != micB[i]) return 4;

    if(!ApAesCcmDecryptCtx(&kf,gApAesStNonce,gApAesStAad,22,                     /* 5 */
                           gApAesStOutA,AP_AES_ST_LEN,micA,gApAesStOutB)) return 5;
    for(i = 0; i < AP_AES_ST_LEN; i++) if(gApAesStOutB[i] != gApAesStPlain[i]) return 5;

    micA[3] ^= 0x10;                                                             /* 6 */
    if(ApAesCcmDecryptCtx(&kf,gApAesStNonce,gApAesStAad,22,
                          gApAesStOutA,AP_AES_ST_LEN,micA,gApAesStOutB)) return 6;
    return 0;
}

static int ApAesFastSelfTest(void)
{
    gApAesSelfTest = ApAesFastSelfTestRun();
    gApAesFast = (gApAesSelfTest == 0);
    return gApAesSelfTest;
}

#endif /* AP_CCMP_H */
