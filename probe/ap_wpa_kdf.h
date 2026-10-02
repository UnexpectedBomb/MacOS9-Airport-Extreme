/* ap_wpa_kdf.h -- WPA2-PSK key derivation, Stage 7-0.
 *
 * PMK <- PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32)
 * PTK <- PRF-384(PMK, "Pairwise key expansion", MACs and nonces in canonical order)
 *
 * ★ WHY SHA-1 IS WRITTEN HERE RATHER THAN LINKED.
 * MINIMAL-STA-SCOPE.md §1 says the crypto is linked, not written, and that remains right for
 * AES-CCM and AES key unwrap -- those are where a hand-rolled implementation would be both
 * hard and dangerous, and mbedTLS already builds under Retro68 in ssheven. But the whole key
 * derivation path needs only SHA-1 and HMAC, SHA-1 is ~80 lines of fixed arithmetic, and it has
 * published known-answer vectors. Writing it here means this entire increment can be compiled
 * and verified on the Mac today, with no mbedTLS in the Retro68 build yet and no hardware.
 * When mbedTLS arrives for CCMP it can replace this, and the vectors below will prove the swap.
 *
 * ★ C89 AND NO DEPENDENCIES ON PURPOSE. No stdint, no stdlib, no OS 9 headers. The same text
 * compiles under Retro68 for the G4 and under clang on the Mac for the test harness, so what
 * the known-answer tests verify is literally the code that will run on the card.
 *
 * ⚠ THE THREE PLACES THIS IS EASY TO GET SILENTLY WRONG, all of which the tests cover:
 *   1. sha1_prf hashes the label INCLUDING its NUL terminator (hostap: strlen(label) + 1).
 *      Drop the NUL and every byte of the PTK is wrong, with no other symptom.
 *   2. The PTK input is min||max of the two MACs and min||max of the two nonces, by memcmp,
 *      NOT "AP then station". Get the order wrong and only one side of the handshake agrees.
 *   3. The PRF counter is a single byte appended AFTER the data, starting at 0 and incrementing
 *      per 20-byte block -- not prepended, not 1-based.
 */
#ifndef AP_WPA_KDF_H
#define AP_WPA_KDF_H

typedef unsigned char  ApU8;
typedef unsigned long  ApU32;      /* >= 32 bits everywhere we build */

/* ---------------------------------------------------------------- SHA-1 */
typedef struct {
    ApU32 h[5];
    ApU32 lenLo, lenHi;            /* message length in BITS */
    ApU8  blk[64];
    int   n;                       /* bytes currently buffered */
} ApSha1;

#define AP_SHA1_LEN 20

static void ApSha1Block(ApSha1 *c,const ApU8 *p)
{
    ApU32 w[80], a,b,d,e,f,k,t;
    int i;
    for(i=0;i<16;i++)
        w[i] = ((ApU32)p[i*4]<<24)|((ApU32)p[i*4+1]<<16)|
               ((ApU32)p[i*4+2]<<8)|((ApU32)p[i*4+3]);
    for(i=16;i<80;i++){
        t = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = ((t<<1)|(t>>31)) & 0xFFFFFFFFUL; }
    a=c->h[0]; b=c->h[1]; d=c->h[2]; e=c->h[3]; f=c->h[4];
    for(i=0;i<80;i++){
        ApU32 g;
        if(i<20)      { k=0x5A827999UL; g=(b&d)|((~b)&e); }
        else if(i<40) { k=0x6ED9EBA1UL; g=b^d^e; }
        else if(i<60) { k=0x8F1BBCDCUL; g=(b&d)|(b&e)|(d&e); }
        else          { k=0xCA62C1D6UL; g=b^d^e; }
        t = (((a<<5)|(a>>27)) + g + f + k + w[i]) & 0xFFFFFFFFUL;
        f=e; e=d; d=((b<<30)|(b>>2)) & 0xFFFFFFFFUL; b=a; a=t; }
    c->h[0]=(c->h[0]+a)&0xFFFFFFFFUL; c->h[1]=(c->h[1]+b)&0xFFFFFFFFUL;
    c->h[2]=(c->h[2]+d)&0xFFFFFFFFUL; c->h[3]=(c->h[3]+e)&0xFFFFFFFFUL;
    c->h[4]=(c->h[4]+f)&0xFFFFFFFFUL;
}

static void ApSha1Init(ApSha1 *c)
{
    c->h[0]=0x67452301UL; c->h[1]=0xEFCDAB89UL; c->h[2]=0x98BADCFEUL;
    c->h[3]=0x10325476UL; c->h[4]=0xC3D2E1F0UL;
    c->lenLo=0; c->lenHi=0; c->n=0;
}

static void ApSha1Update(ApSha1 *c,const ApU8 *p,ApU32 len)
{
    ApU32 i;
    for(i=0;i<len;i++){
        c->blk[c->n++] = p[i];
        /* length in bits, carried across 32 bits so >512 MB inputs stay correct */
        c->lenLo += 8;
        if(c->lenLo < 8) c->lenHi++;
        if(c->n==64){ ApSha1Block(c,c->blk); c->n=0; } }
}

static void ApSha1Final(ApSha1 *c,ApU8 *out)
{
    ApU32 lo=c->lenLo, hi=c->lenHi;
    int i;
    c->blk[c->n++] = 0x80;
    if(c->n > 56){ while(c->n<64) c->blk[c->n++]=0;
                   ApSha1Block(c,c->blk); c->n=0; }
    while(c->n<56) c->blk[c->n++]=0;
    c->blk[56]=(ApU8)((hi>>24)&0xFF); c->blk[57]=(ApU8)((hi>>16)&0xFF);
    c->blk[58]=(ApU8)((hi>>8)&0xFF);  c->blk[59]=(ApU8)(hi&0xFF);
    c->blk[60]=(ApU8)((lo>>24)&0xFF); c->blk[61]=(ApU8)((lo>>16)&0xFF);
    c->blk[62]=(ApU8)((lo>>8)&0xFF);  c->blk[63]=(ApU8)(lo&0xFF);
    ApSha1Block(c,c->blk);
    for(i=0;i<5;i++){
        out[i*4]  =(ApU8)((c->h[i]>>24)&0xFF); out[i*4+1]=(ApU8)((c->h[i]>>16)&0xFF);
        out[i*4+2]=(ApU8)((c->h[i]>>8)&0xFF);  out[i*4+3]=(ApU8)(c->h[i]&0xFF); }
}

/* ------------------------------------------------------- HMAC-SHA1, vector form
 * Vector form because sha1_prf hashes three non-contiguous pieces -- label, data, counter --
 * and concatenating them into a scratch buffer first would need a bound we do not want to
 * reason about. hostap's hmac_sha1_vector has the same shape for the same reason. */
#define AP_HMAC_MAX_VEC 4

static void ApHmacSha1Vec(const ApU8 *key,ApU32 keyLen,
                          int nvec,const ApU8 * const *vec,const ApU32 *vlen,
                          ApU8 *out)
{
    ApSha1 c;
    ApU8 k[64], ipad[64], opad[64], inner[AP_SHA1_LEN], shortened[AP_SHA1_LEN];
    int i;

    if(keyLen > 64){                       /* RFC 2104: long keys are hashed first */
        ApSha1Init(&c); ApSha1Update(&c,key,keyLen); ApSha1Final(&c,shortened);
        key = shortened; keyLen = AP_SHA1_LEN; }
    for(i=0;i<64;i++) k[i] = (i < (int)keyLen) ? key[i] : 0;
    for(i=0;i<64;i++){ ipad[i] = (ApU8)(k[i]^0x36); opad[i] = (ApU8)(k[i]^0x5C); }

    ApSha1Init(&c); ApSha1Update(&c,ipad,64);
    for(i=0;i<nvec;i++) ApSha1Update(&c,vec[i],vlen[i]);
    ApSha1Final(&c,inner);

    ApSha1Init(&c); ApSha1Update(&c,opad,64);
    ApSha1Update(&c,inner,AP_SHA1_LEN); ApSha1Final(&c,out);
}

static void ApHmacSha1(const ApU8 *key,ApU32 keyLen,const ApU8 *data,ApU32 dataLen,ApU8 *out)
{
    const ApU8 *v[1]; ApU32 l[1];
    v[0]=data; l[0]=dataLen;
    ApHmacSha1Vec(key,keyLen,1,v,l,out);
}

/* -------------------------------------------------------------- PBKDF2-HMAC-SHA1
 * RFC 2898 with the PRF fixed to HMAC-SHA1. For WPA2 the salt is the SSID, the iteration
 * count is 4096 and the output is 32 bytes, so exactly two blocks. Written generally anyway
 * because the RFC 6070 vectors exercise 1, 2 and 4096 iterations at 20 and 25 bytes. */
static void ApPbkdf2Sha1(const ApU8 *pass,ApU32 passLen,
                         const ApU8 *salt,ApU32 saltLen,
                         ApU32 iter,ApU8 *out,ApU32 outLen)
{
    ApU8 block[AP_SHA1_LEN], u[AP_SHA1_LEN], saltBuf[256];
    ApU32 done = 0, count = 1, i;
    int j;

    while(done < outLen){
        ApU32 take = outLen - done;
        if(take > AP_SHA1_LEN) take = AP_SHA1_LEN;

        /* U1 = PRF(pass, salt || INT_BE32(count)) */
        for(i=0;i<saltLen && i<252UL;i++) saltBuf[i] = salt[i];
        saltBuf[i++]=(ApU8)((count>>24)&0xFF); saltBuf[i++]=(ApU8)((count>>16)&0xFF);
        saltBuf[i++]=(ApU8)((count>>8)&0xFF);  saltBuf[i++]=(ApU8)(count&0xFF);
        ApHmacSha1(pass,passLen,saltBuf,i,u);
        for(j=0;j<AP_SHA1_LEN;j++) block[j] = u[j];

        /* Ui = PRF(pass, Ui-1), XORed in.  T = U1 ^ U2 ^ ... ^ Uc */
        for(i=1;i<iter;i++){
            ApHmacSha1(pass,passLen,u,AP_SHA1_LEN,u);
            for(j=0;j<AP_SHA1_LEN;j++) block[j] = (ApU8)(block[j]^u[j]); }

        for(i=0;i<take;i++) out[done+i] = block[i];
        done += take; count++; }
}

/* ------------------------------------------------------------------- sha1_prf
 * hostap src/crypto/sha1-prf.c, ported. HMAC-SHA1 over (label including its NUL) || data ||
 * counter, counter a single byte starting at 0 and incrementing per 20-byte output block. */
static void ApSha1Prf(const ApU8 *key,ApU32 keyLen,
                      const char *label,const ApU8 *data,ApU32 dataLen,
                      ApU8 *out,ApU32 outLen)
{
    ApU8 counter = 0, hash[AP_SHA1_LEN];
    ApU32 pos = 0, labelLen = 0, i;
    const ApU8 *v[3]; ApU32 l[3];

    while(label[labelLen]) labelLen++;
    labelLen++;                                  /* ★ the NUL is hashed too */

    v[0]=(const ApU8*)label; l[0]=labelLen;
    v[1]=data;               l[1]=dataLen;
    v[2]=&counter;           l[2]=1;

    while(pos < outLen){
        ApU32 plen = outLen - pos;
        if(plen >= AP_SHA1_LEN){
            ApHmacSha1Vec(key,keyLen,3,v,l,&out[pos]);
            pos += AP_SHA1_LEN;
        } else {
            ApHmacSha1Vec(key,keyLen,3,v,l,hash);
            for(i=0;i<plen;i++) out[pos+i] = hash[i];
            break; }
        counter++; }
}

/* ------------------------------------------------------------------ PMK -> PTK
 * hostap wpa_pmk_to_ptk. For WPA2-PSK with CCMP the PTK is 48 bytes:
 *     KCK  16   confirms the MIC on EAPOL-Key frames
 *     KEK  16   encrypts the GTK in message 3
 *     TK   16   the CCMP pairwise key
 * ⚠ min||max BY memcmp, not "AP first". Both sides compute the same PTK only because they
 *   agree on this ordering without exchanging it. */
#define AP_PTK_LEN 48
#define AP_KCK_OFF  0
#define AP_KEK_OFF 16
#define AP_TK_OFF  32

static int ApMemLess(const ApU8 *a,const ApU8 *b,ApU32 n)
{
    ApU32 i;
    for(i=0;i<n;i++){ if(a[i]!=b[i]) return a[i] < b[i]; }
    return 0;
}

static void ApPmkToPtk(const ApU8 *pmk,ApU32 pmkLen,
                       const ApU8 *aa,const ApU8 *spa,          /* AP MAC, station MAC */
                       const ApU8 *anonce,const ApU8 *snonce,
                       ApU8 *ptk)                               /* AP_PTK_LEN bytes out */
{
    ApU8 data[2*6 + 2*32];
    ApU32 i;

    if(ApMemLess(aa,spa,6)){ for(i=0;i<6;i++){ data[i]=aa[i]; data[6+i]=spa[i]; } }
    else                   { for(i=0;i<6;i++){ data[i]=spa[i]; data[6+i]=aa[i]; } }

    if(ApMemLess(anonce,snonce,32)){
        for(i=0;i<32;i++){ data[12+i]=anonce[i]; data[44+i]=snonce[i]; } }
    else {
        for(i=0;i<32;i++){ data[12+i]=snonce[i]; data[44+i]=anonce[i]; } }

    ApSha1Prf(pmk,pmkLen,"Pairwise key expansion",data,sizeof(data),ptk,AP_PTK_LEN);
}

#endif /* AP_WPA_KDF_H */
