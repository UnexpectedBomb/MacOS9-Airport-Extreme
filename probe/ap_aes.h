/* ap_aes.h -- AES-128 block cipher and RFC 3394 key unwrap. Stage 7-3.
 *
 * ★ WHY THIS IS WRITTEN RATHER THAN LINKED, AND WHERE THAT STOPS.
 * MINIMAL-STA-SCOPE.md §1 says the crypto is linked, and points at ssheven building mbedTLS under
 * Retro68. That is good evidence the build works and mbedTLS stays the fallback. But Stage 7-3
 * needs exactly one primitive -- an AES-128 block -- to run RFC 3394 unwrap on the GTK, and
 * integrating a TLS library for one function is a larger and less certain job than 200 lines of
 * fixed arithmetic with canonical test vectors. The same argument that justified writing SHA-1
 * in ap_wpa_kdf.h, and it is justified the same way: FIPS-197 and RFC 3394 known-answer tests
 * that run on the Mac before any of it reaches the card.
 *
 * ⚠ WHERE IT STOPS: Stage 7-5 needs AES-CCM for the data path, which is CTR plus CBC-MAC with
 *   the nonce and AAD construction that CCMP layers on top. That is materially more to get wrong
 *   than a block cipher, it is where a subtle error becomes a security bug rather than a
 *   handshake failure, and it is the point to reconsider mbedTLS rather than press on. Both
 *   directions of the block cipher are provided here because CCM needs encrypt even though
 *   unwrap needs decrypt.
 *
 * ⚠ THIS IS NOT CONSTANT TIME. Table-driven SubBytes is cache-timing observable. On a machine
 *   with one process, one user, and an attacker who would need local code execution to exploit
 *   it, that is an acceptable trade and it is recorded rather than hidden. It would not be
 *   acceptable in a server.
 *
 * C89, no dependencies, so the same text builds under Retro68 and under clang for the harness.
 */
#ifndef AP_AES_H
#define AP_AES_H

#include "ap_wpa_kdf.h"        /* ApU8, ApU32 */

#define AP_AES_BLOCK 16
#define AP_AES_ROUNDS 10       /* AES-128 */
#define AP_AES_EXPKEY 176      /* (rounds + 1) * 16 */

static const ApU8 apAesSbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const ApU8 apAesInvSbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d };

static const ApU8 apAesRcon[11] =
    { 0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

/* Multiply by x in GF(2^8) with the AES polynomial 0x11B. The conditional reduction is the
 * whole of the field arithmetic; everything else is built from it. */
static ApU8 ApXtime(ApU8 a)
{ return (ApU8)((a & 0x80) ? (((a << 1) & 0xFF) ^ 0x1B) : (ApU8)(a << 1)); }

static ApU8 ApGmul(ApU8 a,ApU8 b)
{
    ApU8 r = 0;
    int i;
    for(i=0;i<8;i++){
        if(b & 1) r ^= a;
        a = ApXtime(a);
        b = (ApU8)(b >> 1); }
    return r;
}

/* 176-byte expanded key: 11 round keys of 16 bytes. FIPS-197 §5.2. */
static void ApAesExpandKey(const ApU8 *key,ApU8 *rk)
{
    int i;
    ApU8 t[4];
    for(i=0;i<16;i++) rk[i] = key[i];
    for(i=16;i<AP_AES_EXPKEY;i+=4){
        t[0]=rk[i-4]; t[1]=rk[i-3]; t[2]=rk[i-2]; t[3]=rk[i-1];
        if((i % 16) == 0){
            ApU8 tmp = t[0];                       /* RotWord */
            t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;
            t[0]=apAesSbox[t[0]]; t[1]=apAesSbox[t[1]];      /* SubWord */
            t[2]=apAesSbox[t[2]]; t[3]=apAesSbox[t[3]];
            t[0] = (ApU8)(t[0] ^ apAesRcon[i/16]); }
        rk[i+0]=(ApU8)(rk[i-16]^t[0]); rk[i+1]=(ApU8)(rk[i-15]^t[1]);
        rk[i+2]=(ApU8)(rk[i-14]^t[2]); rk[i+3]=(ApU8)(rk[i-13]^t[3]); }
}

static void ApAddRoundKey(ApU8 *s,const ApU8 *rk)
{ int i; for(i=0;i<16;i++) s[i] ^= rk[i]; }

/* State is column-major: s[r + 4c]. ShiftRows rotates row r left by r. */
static void ApShiftRows(ApU8 *s)
{
    ApU8 t;
    t=s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13];  s[13]=t;            /* row 1 << 1 */
    t=s[2];  s[2]=s[10]; s[10]=t;  t=s[6];  s[6]=s[14]; s[14]=t;      /* row 2 << 2 */
    t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3];  s[3]=t;             /* row 3 << 3 */
}

static void ApInvShiftRows(ApU8 *s)
{
    ApU8 t;
    t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t=s[2];  s[2]=s[10]; s[10]=t;  t=s[6];  s[6]=s[14]; s[14]=t;
    t=s[3];  s[3]=s[7];  s[7]=s[11]; s[11]=s[15]; s[15]=t;
}

static void ApMixColumns(ApU8 *s)
{
    int c;
    for(c=0;c<4;c++){
        ApU8 *p = s + 4*c;
        ApU8 a0=p[0],a1=p[1],a2=p[2],a3=p[3];
        p[0]=(ApU8)(ApXtime(a0) ^ (ApXtime(a1)^a1) ^ a2 ^ a3);
        p[1]=(ApU8)(a0 ^ ApXtime(a1) ^ (ApXtime(a2)^a2) ^ a3);
        p[2]=(ApU8)(a0 ^ a1 ^ ApXtime(a2) ^ (ApXtime(a3)^a3));
        p[3]=(ApU8)((ApXtime(a0)^a0) ^ a1 ^ a2 ^ ApXtime(a3)); }
}

static void ApInvMixColumns(ApU8 *s)
{
    int c;
    for(c=0;c<4;c++){
        ApU8 *p = s + 4*c;
        ApU8 a0=p[0],a1=p[1],a2=p[2],a3=p[3];
        p[0]=(ApU8)(ApGmul(a0,0x0e)^ApGmul(a1,0x0b)^ApGmul(a2,0x0d)^ApGmul(a3,0x09));
        p[1]=(ApU8)(ApGmul(a0,0x09)^ApGmul(a1,0x0e)^ApGmul(a2,0x0b)^ApGmul(a3,0x0d));
        p[2]=(ApU8)(ApGmul(a0,0x0d)^ApGmul(a1,0x09)^ApGmul(a2,0x0e)^ApGmul(a3,0x0b));
        p[3]=(ApU8)(ApGmul(a0,0x0b)^ApGmul(a1,0x0d)^ApGmul(a2,0x09)^ApGmul(a3,0x0e)); }
}

static void ApAesEncryptBlock(const ApU8 *rk,const ApU8 *in,ApU8 *out)
{
    ApU8 s[16];
    int i,r;
    for(i=0;i<16;i++) s[i]=in[i];
    ApAddRoundKey(s,rk);
    for(r=1;r<AP_AES_ROUNDS;r++){
        for(i=0;i<16;i++) s[i]=apAesSbox[s[i]];
        ApShiftRows(s); ApMixColumns(s);
        ApAddRoundKey(s,rk + 16*r); }
    for(i=0;i<16;i++) s[i]=apAesSbox[s[i]];        /* final round: no MixColumns */
    ApShiftRows(s);
    ApAddRoundKey(s,rk + 16*AP_AES_ROUNDS);
    for(i=0;i<16;i++) out[i]=s[i];
}

static void ApAesDecryptBlock(const ApU8 *rk,const ApU8 *in,ApU8 *out)
{
    ApU8 s[16];
    int i,r;
    for(i=0;i<16;i++) s[i]=in[i];
    ApAddRoundKey(s,rk + 16*AP_AES_ROUNDS);
    for(r=AP_AES_ROUNDS-1;r>=1;r--){
        ApInvShiftRows(s);
        for(i=0;i<16;i++) s[i]=apAesInvSbox[s[i]];
        ApAddRoundKey(s,rk + 16*r);
        ApInvMixColumns(s); }
    ApInvShiftRows(s);
    for(i=0;i<16;i++) s[i]=apAesInvSbox[s[i]];
    ApAddRoundKey(s,rk);
    for(i=0;i<16;i++) out[i]=s[i];
}

/* ---------------------------------------------------- RFC 3394 AES Key Unwrap
 * The GTK in EAPOL-Key message 3 is wrapped with this, using the KEK. Ciphertext is 8 bytes
 * longer than the plaintext: an integrity check value followed by n 64-bit blocks.
 *
 * ⚠ THE IV CHECK IS THE POINT, NOT A FORMALITY. After unwrapping, A must equal A6A6A6A6A6A6A6A6.
 *   It is the only thing standing between us and installing an attacker-chosen group key, so a
 *   failure here must abort the handshake rather than warn. Returning 0 and leaving `out`
 *   untouched is deliberate: there is no partial success to salvage.
 *
 * inLen is the wrapped length and must be a multiple of 8 and at least 24.
 * Writes inLen - 8 bytes to out. Returns 1 on success, 0 if the integrity check fails. */
static const ApU8 apKwIv[8] = { 0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6 };

/* ★ k208: THE SAME UNWRAP, WITH THE CALLER'S SCRATCH -- rk (AP_AES_EXPKEY bytes) and r (rCap bytes, at least
 * inLen - 8). The group key renewal unwraps at secondary interrupt level, and at -O0 ApAesUnwrap's frame is
 * 816 bytes, the deepest thing that level would ever carry (measured with -fstack-usage: it alone took the
 * secondary handler's worst case from 1248 to 1824 bytes). So that caller passes static scratch that only it
 * uses; ApAesUnwrap below keeps its locals for the task-level 4-way handshake. One algorithm either way. */
static int ApAesUnwrapScratch(const ApU8 *kek,const ApU8 *in,ApU32 inLen,ApU8 *out,
                              ApU8 *rk,ApU8 *r,ApU32 rCap)
{
    ApU8 a[8], b[16];
    ApU32 n, i;
    long j, k;

    if(inLen < 24 || (inLen % 8) != 0 || (inLen - 8) > rCap) return 0;
    n = (inLen - 8) / 8;

    ApAesExpandKey(kek,rk);
    for(i=0;i<8;i++)        a[i] = in[i];
    for(i=0;i<inLen-8;i++)  r[i] = in[8+i];

    /* RFC 3394 §2.2.2: for j = 5 downto 0, for i = n downto 1 */
    for(j=5;j>=0;j--){
        for(k=(long)n;k>=1;k--){
            ApU32 t = (ApU32)(n * (ApU32)j) + (ApU32)k;
            for(i=0;i<8;i++) b[i] = a[i];
            /* A ^ t, big-endian over the low bytes of the 64-bit counter */
            b[7] = (ApU8)(b[7] ^ (t & 0xFF));
            b[6] = (ApU8)(b[6] ^ ((t >> 8) & 0xFF));
            b[5] = (ApU8)(b[5] ^ ((t >> 16) & 0xFF));
            b[4] = (ApU8)(b[4] ^ ((t >> 24) & 0xFF));
            for(i=0;i<8;i++) b[8+i] = r[(k-1)*8 + i];
            ApAesDecryptBlock(rk,b,b);
            for(i=0;i<8;i++) a[i] = b[i];
            for(i=0;i<8;i++) r[(k-1)*8 + i] = b[8+i]; } }

    for(i=0;i<8;i++) if(a[i] != apKwIv[i]) return 0;   /* ★ abort, do not warn */
    for(i=0;i<inLen-8;i++) out[i] = r[i];
    return 1;
}

static int ApAesUnwrap(const ApU8 *kek,const ApU8 *in,ApU32 inLen,ApU8 *out)
{
    ApU8 rk[AP_AES_EXPKEY], r[512];
    return ApAesUnwrapScratch(kek,in,inLen,out,rk,r,(ApU32)sizeof(r));
}

#endif /* AP_AES_H */
