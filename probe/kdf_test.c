/* kdf_test.c -- known-answer tests for ap_wpa_kdf.h. Runs on the Mac, not the G4.
 *
 * This is the point of Stage 7-0. Everything in ap_wpa_kdf.h is arithmetic with no hardware in
 * it, and a key derivation that is wrong produces no diagnostic at all -- the 4-way handshake
 * simply fails its MIC check and the AP deauthenticates, which looks identical to a dozen other
 * faults. Twenty-eight reboots went into Stage 5 partly because the instrument was lying; here
 * the instrument can be checked on this machine, for free, before the G4 ever sees it.
 *
 * Vectors are taken from hostapd-2.10 src/crypto/crypto_module_tests.c, which carries them from
 * the standards rather than from anyone's implementation:
 *   - RFC 6070          PBKDF2-HMAC-SHA1 at 1, 2 and 4096 iterations
 *   - IEEE 802.11i-2004 Annex H.4.2  passphrase -> PSK
 *   - RFC 2202          HMAC-SHA1
 *
 * ⚠ These prove the primitives AND the two orderings that are easy to get silently wrong: the
 *   NUL hashed with the PRF label, and min||max of MACs and nonces. The PTK case below is a
 *   self-consistency check, not a published vector -- it is labelled as such rather than dressed
 *   up as one, because I do not have a published PTK vector for WPA2-PSK/CCMP in hand.
 *
 * Build and run:  cc -std=c89 -Wall -Wextra -o kdf_test kdf_test.c && ./kdf_test
 */
#include <stdio.h>
#include <string.h>
#include "ap_wpa_kdf.h"
#include "ap_aes.h"
#define AP_AES_COUNT_BLOCKS  /* k199: ap_ccmp.h counts which block cipher each CCM block went through */
#include "ap_ccmp.h"
#include "ap_ccm_vec336.h"   /* 8-36: a LibreSSL-generated vector at a realistic frame length */

static int gFail = 0;

static void hex(const char *tag,const ApU8 *p,ApU32 n)
{
    ApU32 i; printf("    %-12s",tag);
    for(i=0;i<n;i++){ printf("%02x",p[i]); if((i%4)==3) printf(" "); }
    printf("\n");
}

static void check(const char *what,const ApU8 *got,const ApU8 *want,ApU32 n)
{
    if(memcmp(got,want,n)==0){ printf("  [ok]   %s\n",what); return; }
    printf("  [FAIL] %s\n",what);
    hex("got",got,n); hex("want",want,n);
    gFail++;
}

/* Boolean form, for the Stage 8-0 checks whose answer is a property rather than a byte string
 * -- "the Protected bit is set", "a tampered frame is rejected". Same reporting shape. */
static void checkb(const char *what,int ok)
{
    if(ok){ printf("  [ok]   %s\n",what); return; }
    printf("  [FAIL] %s\n",what);
    gFail++;
}

int main(void)
{
    ApU8 out[64];

    printf("=== SHA-1, FIPS 180-1 ===\n");
    { /* "abc" */
        static const ApU8 want[20] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
        ApSha1 c; ApSha1Init(&c); ApSha1Update(&c,(const ApU8*)"abc",3);
        ApSha1Final(&c,out);
        check("SHA-1(\"abc\")",out,want,20); }
    { /* the 448-bit two-block message */
        static const ApU8 want[20] = {
            0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,0xba,0xae,
            0x4a,0xa1,0xf9,0x51,0x29,0xe5,0xe5,0x46,0x70,0xf1 };
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        ApSha1 c; ApSha1Init(&c); ApSha1Update(&c,(const ApU8*)m,(ApU32)strlen(m));
        ApSha1Final(&c,out);
        check("SHA-1(56-byte message, spans two blocks)",out,want,20); }
    { /* one million 'a' -- exercises the 32-bit bit-length carry and the block loop */
        static const ApU8 want[20] = {
            0x34,0xaa,0x97,0x3c,0xd4,0xc4,0xda,0xa4,0xf6,0x1e,
            0xeb,0x2b,0xdb,0xad,0x27,0x31,0x65,0x34,0x01,0x6f };
        ApSha1 c; int i; ApU8 a[1000];
        for(i=0;i<1000;i++) a[i]='a';
        ApSha1Init(&c);
        for(i=0;i<1000;i++) ApSha1Update(&c,a,1000);
        ApSha1Final(&c,out);
        check("SHA-1(1,000,000 x 'a')",out,want,20); }

    printf("=== HMAC-SHA1, RFC 2202 ===\n");
    { static const ApU8 key[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
      static const ApU8 want[20] = {
        0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
        0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00 };
      ApHmacSha1(key,20,(const ApU8*)"Hi There",8,out);
      check("HMAC-SHA1 case 1",out,want,20); }
    { /* a key longer than the 64-byte block, so the hash-the-key path runs */
      ApU8 key[80]; int i;
      static const ApU8 want[20] = {
        0xaa,0x4a,0xe5,0xe1,0x52,0x72,0xd0,0x0e,0x95,0x70,
        0x56,0x37,0xce,0x8a,0x3b,0x55,0xed,0x40,0x21,0x12 };
      for(i=0;i<80;i++) key[i]=0xaa;
      ApHmacSha1(key,80,(const ApU8*)"Test Using Larger Than Block-Size Key - Hash Key First",54,out);
      check("HMAC-SHA1 case 6, 80-byte key",out,want,20); }

    printf("=== PBKDF2-HMAC-SHA1, RFC 6070 ===\n");
    { static const ApU8 want[20] = {
        0x0c,0x60,0xc8,0x0f,0x96,0x1f,0x0e,0x71,0xf3,0xa9,
        0xb5,0x24,0xaf,0x60,0x12,0x06,0x2f,0xe0,0x37,0xa6 };
      ApPbkdf2Sha1((const ApU8*)"password",8,(const ApU8*)"salt",4,1,out,20);
      check("c=1, dkLen=20",out,want,20); }
    { static const ApU8 want[20] = {
        0xea,0x6c,0x01,0x4d,0xc7,0x2d,0x6f,0x8c,0xcd,0x1e,
        0xd9,0x2a,0xce,0x1d,0x41,0xf0,0xd8,0xde,0x89,0x57 };
      ApPbkdf2Sha1((const ApU8*)"password",8,(const ApU8*)"salt",4,2,out,20);
      check("c=2, dkLen=20",out,want,20); }
    { static const ApU8 want[20] = {
        0x4b,0x00,0x79,0x01,0xb7,0x65,0x48,0x9a,0xbe,0xad,
        0x49,0xd9,0x26,0xf7,0x21,0xd0,0x65,0xa4,0x29,0xc1 };
      ApPbkdf2Sha1((const ApU8*)"password",8,(const ApU8*)"salt",4,4096,out,20);
      check("c=4096, dkLen=20",out,want,20); }
    { /* 25 bytes, so the final partial block is exercised */
      static const ApU8 want[25] = {
        0x3d,0x2e,0xec,0x4f,0xe4,0x1c,0x84,0x9b,0x80,0xc8,
        0xd8,0x36,0x62,0xc0,0xe4,0x4a,0x8b,0x29,0x1a,0x96,
        0x4c,0xf2,0xf0,0x70,0x38 };
      ApPbkdf2Sha1((const ApU8*)"passwordPASSWORDpassword",24,
                   (const ApU8*)"saltSALTsaltSALTsaltSALTsaltSALTsalt",36,4096,out,25);
      check("c=4096, dkLen=25, two blocks + partial",out,want,25); }

    printf("=== passphrase -> PSK, IEEE 802.11i-2004 Annex H.4.2 ===\n");
    { static const ApU8 want[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
      ApPbkdf2Sha1((const ApU8*)"password",8,(const ApU8*)"IEEE",4,4096,out,32);
      check("\"password\" / \"IEEE\"",out,want,32); }
    { static const ApU8 want[32] = {
        0x0d,0xc0,0xd6,0xeb,0x90,0x55,0x5e,0xd6,0x41,0x97,0x56,0xb9,0xa1,0x5e,0xc3,0xe3,
        0x20,0x9b,0x63,0xdf,0x70,0x7d,0xd5,0x08,0xd1,0x45,0x81,0xf8,0x98,0x27,0x21,0xaf };
      ApPbkdf2Sha1((const ApU8*)"ThisIsAPassword",15,(const ApU8*)"ThisIsASSID",11,4096,out,32);
      check("\"ThisIsAPassword\" / \"ThisIsASSID\"",out,want,32); }
    { static const ApU8 want[32] = {
        0xbe,0xcb,0x93,0x86,0x6b,0xb8,0xc3,0x83,0x2c,0xb7,0x77,0xc2,0xf5,0x59,0x80,0x7c,
        0x8c,0x59,0xaf,0xcb,0x6e,0xae,0x73,0x48,0x85,0x00,0x13,0x00,0xa9,0x81,0xcc,0x62 };
      ApPbkdf2Sha1((const ApU8*)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",32,
                   (const ApU8*)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",32,4096,out,32);
      check("32-char passphrase / 32-char SSID",out,want,32); }

    printf("=== the two orderings that fail silently ===\n");
    { /* The PRF must hash the label's NUL. If it did not, this would differ -- so the check is
       * that hashing "prefix" (6 bytes) and "prefix\\0" (7) give different answers, and that our
       * PRF agrees with the 7-byte form computed directly through HMAC. */
      ApU8 viaPrf[20], viaHmac[20], buf[64];
      ApU8 key[20]; int i; ApU32 n=0;
      for(i=0;i<20;i++) key[i]=(ApU8)(0x0b+i);
      ApSha1Prf(key,20,"prefix",(const ApU8*)"data",4,viaPrf,20);
      memcpy(buf+n,"prefix",7); n+=7;          /* label AND its NUL */
      memcpy(buf+n,"data",4);   n+=4;
      buf[n++]=0;                              /* counter, first block */
      ApHmacSha1(key,20,buf,n,viaHmac);
      check("PRF hashes the label's NUL terminator",viaPrf,viaHmac,20); }

    { /* min||max by memcmp. Swapping which MAC is "ours" must not change the PTK, because both
       * sides of a real handshake compute it from the same pair without negotiating order. */
      static const ApU8 pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
      static const ApU8 aa[6]  = {0xa2,0x41,0xb2,0xcc,0x58,0xad};
      static const ApU8 spa[6] = {0x00,0x11,0x24,0xaa,0xc4,0x4c};
      ApU8 anonce[32], snonce[32], ptk1[AP_PTK_LEN], ptk2[AP_PTK_LEN];
      int i;
      for(i=0;i<32;i++){ anonce[i]=(ApU8)(0x10+i); snonce[i]=(ApU8)(0xf0-i); }
      ApPmkToPtk(pmk,32,aa,spa,anonce,snonce,ptk1);
      ApPmkToPtk(pmk,32,spa,aa,snonce,anonce,ptk2);   /* the AP's point of view */
      check("PTK is identical from either side (min||max ordering)",ptk1,ptk2,AP_PTK_LEN);
      printf("    ⚠ SELF-CONSISTENCY, NOT A PUBLISHED VECTOR. It proves the ordering is\n");
      printf("      symmetric, which is the property that matters and the thing that breaks\n");
      printf("      silently. It does NOT prove our PTK matches what a real AP computes --\n");
      printf("      only a live 4-way handshake with a passing MIC will show that.\n");
      hex("KCK",ptk1+AP_KCK_OFF,16);
      hex("KEK",ptk1+AP_KEK_OFF,16);
      hex("TK", ptk1+AP_TK_OFF, 16); }

    printf("=== AES-128 block cipher, FIPS-197 Appendix C.1 ===\n");
    { static const ApU8 key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
      static const ApU8 pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
      static const ApU8 ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
      ApU8 rk[AP_AES_EXPKEY], got[16];
      ApAesExpandKey(key,rk);
      ApAesEncryptBlock(rk,pt,got);
      check("AES-128 encrypt",got,ct,16);
      ApAesDecryptBlock(rk,ct,got);
      check("AES-128 decrypt",got,pt,16); }

    { /* encrypt/decrypt round trip over every byte value, so a table typo cannot hide in a
       * corner the single FIPS vector happens not to touch */
      ApU8 key[16], pt[16], ct[16], back[16], rk[AP_AES_EXPKEY];
      int i,v,bad=0;
      for(v=0;v<256;v++){
          for(i=0;i<16;i++){ key[i]=(ApU8)(v^i); pt[i]=(ApU8)(v+i*7); }
          ApAesExpandKey(key,rk);
          ApAesEncryptBlock(rk,pt,ct);
          ApAesDecryptBlock(rk,ct,back);
          if(memcmp(pt,back,16)!=0){ bad++; break; } }
      if(bad){ printf("  [FAIL] encrypt/decrypt round trip over 256 key/plaintext pairs\n"); gFail++; }
      else     printf("  [ok]   encrypt/decrypt round trip over 256 key/plaintext pairs\n"); }

    /* ★★★ k199: THE FAST CIPHER (ap_aes_fast.h) AGAINST THE BYTE-WISE REFERENCE. The reference is the
     * oracle: it passed everything in this file through k198, and every check below is "fast ==
     * reference" or a published vector. ApAesFastSelfTest at the end is the same function the G4 runs
     * at bring-up; it must pass here too, and from that point on every CCM/CCMP test in this file runs
     * through the FAST cipher, exactly as the driver will. */
    printf("=== k199: fast (T-table) AES-128 against the byte-wise reference ===\n");
    { /* Te0: all 256 entries recomputed from the S-box with the reference's own field arithmetic */
      int v, bad = 0;
      for(v = 0; v < 256; v++){
          ApU8 s = apAesSbox[v];
          ApW32 want = ((ApW32)ApXtime(s) << 24) | ((ApW32)s << 16) | ((ApW32)s << 8) |
                       (ApW32)(ApU8)(ApXtime(s) ^ s);
          if(apAesTe0[v] != want){
              bad++;
              if(bad < 4) printf("    Te0[%d] = %08x, want %08x\n",v,(unsigned)apAesTe0[v],(unsigned)want); } }
      checkb("Te0: all 256 entries equal S(x)*{02,01,01,03} from the S-box",bad == 0); }
    { static const ApU8 key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
      static const ApU8 pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
      static const ApU8 ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
      ApW32 w[AP_AESF_WORDS];
      ApU8 got[16], inplace[16];
      ApAesFastExpandKey(key,w);
      ApAesFastEncryptBlock(w,pt,got);
      check("fast AES-128 encrypt, FIPS-197 C.1",got,ct,16);
      memcpy(inplace,pt,16);
      ApAesFastEncryptBlock(w,inplace,inplace);          /* CCM's CBC-MAC calls it with in == out */
      check("fast AES-128 encrypt in place (in == out)",inplace,ct,16); }
    { ApW32 w[AP_AESF_WORDS], x = 0x9E3779B9U;
      ApU8 rk[AP_AES_EXPKEY], key[16], blk[16], a[16], b[16];
      long n, badKs = 0, badBlk = 0;
      int i;
      for(n = 0; n < 200000L; n++){
          for(i = 0; i < 16; i++) key[i] = (ApU8)ApAesStNext(&x);
          for(i = 0; i < 16; i++) blk[i] = (ApU8)(ApAesStNext(&x) >> 11);
          ApAesFastExpandKey(key,w);
          ApAesExpandKey(key,rk);
          if(n < 2000L)
              for(i = 0; i < AP_AESF_WORDS; i++) if(w[i] != AP_AESF_GET(rk + 4*i)){ badKs++; break; }
          ApAesFastEncryptBlock(w,blk,a);
          ApAesEncryptBlock(rk,blk,b);
          if(memcmp(a,b,16)) badBlk++; }
      checkb("fast key schedule == reference, word for byte (2000 random keys)",badKs == 0);
      checkb("fast AES == reference on 200000 random key/block pairs",badBlk == 0); }
    { /* CCM at EVERY length 0..2400 -- every partial final block, one- and two-block AAD, no AAD --
       * fast against reference; then the fast decrypt must verify and restore, and must REJECT a
       * flipped MIC bit. 2400 covers the largest frame the receive buffer takes. */
      static ApU8 pt[2400], ca[2400], cb[2400], back[2400];
      ApU8 key[16], nonce[13], aad[30], ma[8], mb[8];
      ApW32 x = 0x01234567U;
      ApAesCtx kf, kr;
      int len, i, bad = 0, badDec = 0, badRej = 0;
      for(i = 0; i < 2400; i++) pt[i] = (ApU8)ApAesStNext(&x);
      for(len = 0; len <= 2400; len++){
          ApU32 aadLen = (ApU32)(len % 31);            /* 0 (none), 1..14 (one block), 15..30 (two) */
          for(i = 0; i < 16; i++) key[i]   = (ApU8)ApAesStNext(&x);
          for(i = 0; i < 13; i++) nonce[i] = (ApU8)ApAesStNext(&x);
          for(i = 0; i < 30; i++) aad[i]   = (ApU8)ApAesStNext(&x);
          ApAesCtxInitMode(&kf,key,1);
          ApAesCtxInitMode(&kr,key,0);
          ApAesCcmEncryptCtx(&kf,nonce,aad,aadLen,pt,(ApU32)len,ca,ma);
          ApAesCcmEncryptCtx(&kr,nonce,aad,aadLen,pt,(ApU32)len,cb,mb);
          if(memcmp(ca,cb,(size_t)len) || memcmp(ma,mb,8)) bad++;
          if(!ApAesCcmDecryptCtx(&kf,nonce,aad,aadLen,ca,(ApU32)len,ma,back) ||
             memcmp(back,pt,(size_t)len)) badDec++;
          ma[len % 8] ^= (ApU8)(1 << (len % 8));
          if(ApAesCcmDecryptCtx(&kf,nonce,aad,aadLen,ca,(ApU32)len,ma,back)) badRej++; }
      checkb("CCM encrypt: fast == reference at every length 0..2400 (ciphertext and MIC)",bad == 0);
      checkb("CCM decrypt: the fast path verifies and restores at every length",badDec == 0);
      checkb("CCM decrypt: the fast path REJECTS a flipped MIC bit at every length",badRej == 0); }
    { /* THE GATE: before the self-test the data-path entry points must run the REFERENCE cipher, and
       * after it the FAST one -- counted per block, because the bytes cannot tell the two apart. A
       * 1500-byte CCMP payload is 1 (B_0) + 2 (22-byte AAD) + 94 (CBC-MAC) + 1 (tag) + 94 (CTR) = 192. */
      static ApU8 p[1500], c[1500];
      ApU8 key[16], nonce[13], aad[22], mic[8];
      unsigned long f0, r0;
      int r;
      memset(p,0x5A,sizeof p); memset(key,0x11,16); memset(nonce,0x22,13); memset(aad,0x33,22);
      checkb("before the self-test the data path is on the REFERENCE cipher (gApAesFast = 0)",
             gApAesFast == 0 && gApAesSelfTest == -1);
      f0 = gApAesFastBlocks; r0 = gApAesRefBlocks;
      ApAesCcmEncrypt(key,nonce,aad,22,p,1500,c,mic);
      checkb("  ...and a 1500-byte encrypt takes all 192 blocks through the reference",
             gApAesRefBlocks - r0 == 192UL && gApAesFastBlocks == f0);
      r = ApAesFastSelfTest();
      if(r) printf("    ApAesFastSelfTest failed at check %d\n",r);
      checkb("ApAesFastSelfTest (the G4's bring-up test) passes and sets gApAesFast",
             r == 0 && gApAesFast == 1 && gApAesSelfTest == 0);
      f0 = gApAesFastBlocks; r0 = gApAesRefBlocks;
      ApAesCcmEncrypt(key,nonce,aad,22,p,1500,c,mic);
      checkb("  ...and afterwards the same encrypt takes all 192 blocks through the FAST cipher",
             gApAesFastBlocks - f0 == 192UL && gApAesRefBlocks == r0);
      f0 = gApAesFastBlocks; r0 = gApAesRefBlocks;
      checkb("  ...and so does the decrypt, which verifies it",
             ApAesCcmDecrypt(key,nonce,aad,22,c,1500,mic,p) == 1 &&
             gApAesFastBlocks - f0 == 192UL && gApAesRefBlocks == r0);
      printf("    (every CCM/CCMP test below this line runs through the FAST cipher, as the driver will)\n"); }

    printf("=== AES Key Unwrap, RFC 3394 §4.1 ===\n");
    { /* 128-bit KEK, 128-bit key data -- the case the GTK actually uses */
      static const ApU8 kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F };
      static const ApU8 wrapped[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5 };
      static const ApU8 want[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF };
      ApU8 got[16];
      if(ApAesUnwrap(kek,wrapped,24,got)) check("unwrap 128-bit key data",got,want,16);
      else { printf("  [FAIL] unwrap 128-bit key data -- integrity check rejected it\n"); gFail++; } }

    { /* ★ THE INTEGRITY CHECK MUST REJECT A TAMPERED WRAP. This is the test that matters:
       * without it the unwrap would happily hand back attacker-chosen bytes as a group key. */
      static const ApU8 kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F };
      ApU8 wrapped[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5 };
      ApU8 got[16];
      wrapped[20] ^= 0x01;                       /* one bit, in the middle */
      if(ApAesUnwrap(kek,wrapped,24,got)){
        printf("  [FAIL] a tampered wrap was ACCEPTED -- the IV check is not working\n"); gFail++; }
      else printf("  [ok]   a tampered wrap is rejected (RFC 3394 IV check)\n"); }

    { /* and a wrong KEK must also be rejected */
      ApU8 kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F };
      static const ApU8 wrapped[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5 };
      ApU8 got[16];
      kek[0] ^= 0x80;
      if(ApAesUnwrap(kek,wrapped,24,got)){
        printf("  [FAIL] a wrong KEK was ACCEPTED\n"); gFail++; }
      else printf("  [ok]   a wrong KEK is rejected\n"); }

    { /* k208: the same unwrap with the CALLER's scratch -- the form the receive pump uses, so its 816-byte
       * frame stays off the secondary interrupt's stack. Same vector, same answer; a scratch too small for
       * the data is refused; and a failed check still leaves the output alone. */
      static const ApU8 kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F };
      static const ApU8 want[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF };
      ApU8 wrapped[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5 };
      static ApU8 rk[AP_AES_EXPKEY], r[16];
      ApU8 got[16], untouched[16];
      int k;
      if(ApAesUnwrapScratch(kek,wrapped,24,got,rk,r,16)) check("unwrap with caller scratch (RFC 3394 4.1)",got,want,16);
      else { printf("  [FAIL] unwrap with caller scratch -- rejected\n"); gFail++; }
      for(k=0;k<16;k++) got[k] = untouched[k] = (ApU8)(0x5A ^ k);
      checkb("  a scratch smaller than the key data is refused, output untouched",
             !ApAesUnwrapScratch(kek,wrapped,24,got,rk,r,15) && memcmp(got,untouched,16) == 0);
      wrapped[20] ^= 0x01;
      checkb("  a tampered wrap is rejected through the scratch form too, output untouched",
             !ApAesUnwrapScratch(kek,wrapped,24,got,rk,r,16) && memcmp(got,untouched,16) == 0); }

    printf("=== AES-CCM, RFC 3610 packet vector #1 ===\n");
    { /* M=8, L=2 -- exactly CCMP's parameters, which is why this vector was chosen over the
       * others in RFC 3610. Encrypt direction, then decrypt-and-verify back. */
      static const ApU8 key[16] = {
        0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF };
      static const ApU8 nonce[13] = {
        0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 };
      static const ApU8 aad[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
      static const ApU8 pt[23] = {
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,
        0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E };
      /* RFC 3610 gives ciphertext || MIC as one 31-byte string */
      static const ApU8 want[31] = {
        0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
        0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84,
        0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0 };
      ApU8 x[16], a[16], ct[32], tag[16], back[32];
      ApAesCtx k;
      int mode;
      /* encrypt, at the primitive level, through BOTH block ciphers (k199): 0 = ap_aes.h's byte-wise
       * reference, 1 = ap_aes_fast.h's T-table cipher */
      for(mode = 0; mode <= 1; mode++){
        ApAesCtxInitMode(&k,key,mode);
        ApCcmAuthStart(&k,8,2,nonce,aad,8,23,x);
        ApCcmAuth(&k,pt,23,x);
        ApCcmCtrStart(2,nonce,a);
        ApCcmCtr(&k,pt,23,ct,a);
        ApCcmCtrStart(2,nonce,a);
        ApCcmTag(&k,8,x,a,tag);
        check(mode ? "CCM encrypt: ciphertext (fast AES)" : "CCM encrypt: ciphertext (reference AES)",
              ct,want,23);
        check(mode ? "CCM encrypt: MIC (fast AES)" : "CCM encrypt: MIC (reference AES)",
              tag,want+23,8); }
      /* and the decrypt path that CCMP actually uses */
      if(ApAesCcmDecrypt(key,nonce,aad,8,want,23,want+23,back))
        check("CCM decrypt-and-verify",back,pt,23);
      else { printf("  [FAIL] CCM decrypt-and-verify -- MIC rejected a valid frame\n"); gFail++; } }

    { /* ★ AND IT MUST REJECT A TAMPERED FRAME. Same reasoning as the key-unwrap check: an
       * authenticator is only useful if it says no. */
      static const ApU8 key[16] = {
        0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF };
      static const ApU8 nonce[13] = {
        0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 };
      static const ApU8 aad[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
      ApU8 ct[31] = {
        0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
        0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84,
        0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0 };
      ApU8 back[32]; ApU8 aadBad[8]; int i;
      ct[5] ^= 0x01;
      if(ApAesCcmDecrypt(key,nonce,aad,8,ct,23,ct+23,back)){
        printf("  [FAIL] a tampered CIPHERTEXT was accepted\n"); gFail++; }
      else printf("  [ok]   a tampered ciphertext is rejected\n");
      ct[5] ^= 0x01;
      for(i=0;i<8;i++) aadBad[i] = aad[i];
      aadBad[3] ^= 0x01;                 /* AAD is authenticated but not encrypted */
      if(ApAesCcmDecrypt(key,nonce,aadBad,8,ct,23,ct+23,back)){
        printf("  [FAIL] a tampered AAD was accepted -- the header is not being authenticated\n");
        gFail++; }
      else printf("  [ok]   a tampered AAD is rejected (the 802.11 header IS authenticated)\n"); }

    /* ================= STAGE 8-0: CCMP ENCAPSULATION =================
     * ⚠ A ROUND TRIP IS NOT SUFFICIENT ON ITS OWN. encap->decap agreeing proves the two are
     * consistent with EACH OTHER, which a pair of matching bugs also achieves -- most obviously
     * getting CCM's order backwards in both directions. So the first test here is against an
     * INDEPENDENT authority (RFC 3610), and only then the round trip. */
    printf("\n-- Stage 8-0: CCMP encapsulation (transmit) --\n");
    { /* RFC 3610 packet vector #1, driven FORWARDS through the encrypt path. The expected
       * ciphertext and MIC are the same bytes the decrypt test above consumes, so if encrypt
       * and decrypt ever disagree with the RFC, only one of them can still be blamed. */
      static const ApU8 key[16] = {
        0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF };
      static const ApU8 nonce[13] = {
        0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 };
      static const ApU8 aad[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
      static const ApU8 pt[23] = {
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E };
      static const ApU8 wantCt[23] = {
        0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
        0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84 };
      static const ApU8 wantMic[8] = { 0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0 };
      ApU8 ct[32], mic[8];
      ApAesCcmEncrypt(key,nonce,aad,8,pt,23,ct,mic);
      checkb("CCM encrypt: ciphertext matches RFC 3610 vector #1", !memcmp(ct,wantCt,23));
      checkb("CCM encrypt: MIC matches RFC 3610 vector #1",        !memcmp(mic,wantMic,8)); }

    { /* PN <-> CCMP header must be exact inverses. The byte order is deliberately scrambled,
       * so this is the single easiest place in the whole path to make a silent mistake. */
      static const ApU8 pn[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };
      ApU8 h[8], back[6];
      ApCcmpPnToHdr(pn,2,h);
      ApCcmpHdrToPn(h,back);
      checkb("CCMP header: PN survives a round trip through the scrambled layout",
            !memcmp(pn,back,6));
      checkb("CCMP header: ExtIV is set",   (h[3] & 0x20) != 0);
      checkb("CCMP header: key id round-trips", ApCcmpKeyId(h) == 2);
      checkb("CCMP header: byte 2 is reserved-zero", h[2] == 0); }

    { /* The full frame round trip, through the real encap and the real decap. */
      static const ApU8 tk[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF };
      static const ApU8 pn[6] = { 0x00,0x00,0x00,0x00,0x12,0x34 };
      /* A plain data frame, ToDS, from us to the AP. Protected NOT set: encap sets it. */
      ApU8 hdr[24] = {
        0x08,0x01, 0x00,0x00,
        0x00,0x11,0x22,0x33,0x44,0x55,        /* A1 = BSSID */
        0x66,0x77,0x88,0x99,0xAA,0xBB,        /* A2 = us    */
        0xCC,0xDD,0xEE,0xFF,0x00,0x11,        /* A3 = dest  */
        0x40,0x00 };                          /* seq 4, frag 0 */
      /* LLC/SNAP + a short IPv4-looking payload, the shape Stage 7-5 recovered off the air. */
      static const ApU8 body[16] = {
        0xAA,0xAA,0x03,0x00,0x00,0x00,0x08,0x00, 0x45,0x00,0x00,0x08,0xDE,0xAD,0xBE,0xEF };
      ApU8 frame[128], back[128], gotPn[6];
      ApU32 flen, blen; int kid = -1;
      flen = ApCcmpEncap(hdr,24,body,16,tk,pn,1,0,0,0,0,frame);
      checkb("CCMP encap: length is hdr + 8 + payload + 8",
            flen == 24u + 8u + 16u + 8u);
      checkb("CCMP encap: the Protected bit is SET on the outgoing frame",
            (frame[1] & 0x40) != 0);
      checkb("CCMP encap: the ciphertext is not the plaintext",
            memcmp(frame + 24 + 8, body, 16) != 0);
      checkb("CCMP encap -> decap: the MIC verifies",
            ApCcmpDecap(frame,flen,tk,back,&blen,gotPn,&kid) == 1);
      checkb("CCMP encap -> decap: the plaintext comes back byte for byte",
            blen == 16 && !memcmp(back,body,16));
      checkb("CCMP encap -> decap: the PN comes back",   !memcmp(gotPn,pn,6));
      checkb("CCMP encap -> decap: the key id comes back", kid == 1);

      { /* ★ AND THE RECEIVER MUST REJECT OUR OWN FRAME IF IT IS TOUCHED. Without this the
         * round trip only shows the two halves agree, not that either one authenticates. */
        ApU8 t[128]; ApU32 tl;
        memcpy(t,frame,(size_t)flen); tl = flen;
        t[24+8+3] ^= 0x01;                                  /* flip a ciphertext bit */
        checkb("CCMP: a tampered payload in OUR OWN frame is rejected",
              ApCcmpDecap(t,tl,tk,back,&blen,gotPn,&kid) == 0);
        memcpy(t,frame,(size_t)flen);
        t[16] ^= 0x01;                                      /* flip a bit in A3, inside the AAD */
        checkb("CCMP: a tampered ADDRESS in OUR OWN frame is rejected",
              ApCcmpDecap(t,tl,tk,back,&blen,gotPn,&kid) == 0); } }

    /* ★★★★★ 8-36: A REALISTIC FRAME LENGTH, AGAINST AN INDEPENDENT IMPLEMENTATION.
     *
     * ⚠⚠ EVERY CCMP CASE ABOVE THIS LINE IS 23 BYTES OR SHORTER -- the RFC 3610 vector is 23,
     *   the encap round trip is 16. A DHCP reply on the target network is 336 bytes, which is
     *   twenty-one AES blocks, so blocks 3 through 20 of both the CTR keystream and the
     *   CBC-MAC had never been executed by any test on any machine. The suite printed
     *   "ALL KNOWN-ANSWER TESTS PASSED" throughout. Ask what an oracle would FAIL on.
     *
     * ⚠ The encap -> decap round trip above cannot close this gap, however long the payload:
     *   it agrees with itself even when the counter construction is wrong, because both
     *   directions use the same wrong counter. The vector below was produced by LibreSSL and
     *   shares no line of code with this project. See ap_ccm_vec336.h. */
    printf("\n-- 8-36: AES-CCM at a realistic frame length (336 bytes, 21 blocks) --\n");
    { ApU8 x[16], a[16];
      ApU8 ct[336], tag[16], back[336];
      ApAesCtx k;
      int mode;
      for(mode = 0; mode <= 1; mode++){            /* k199: both block ciphers, as above */
        ApAesCtxInitMode(&k,kV_key,mode);
        ApCcmAuthStart(&k,8,2,kV_nonce,kV_aad,22,336,x);
        ApCcmAuth(&k,kV_pt,336,x);
        ApCcmCtrStart(2,kV_nonce,a);
        ApCcmCtr(&k,kV_pt,336,ct,a);
        ApCcmCtrStart(2,kV_nonce,a);
        ApCcmTag(&k,8,x,a,tag);
        check(mode ? "CCM 336: ciphertext matches LibreSSL (fast AES)"
                   : "CCM 336: ciphertext matches LibreSSL (reference AES)",ct,kV_ct,336);
        check(mode ? "CCM 336: MIC matches LibreSSL (fast AES)"
                   : "CCM 336: MIC matches LibreSSL (reference AES)",tag,kV_mic,8); }
      if(ApAesCcmDecrypt(kV_key,kV_nonce,kV_aad,22,kV_ct,336,kV_mic,back))
        check("CCM 336: decrypt-and-verify returns the plaintext",back,kV_pt,336);
      else { printf("  [FAIL] CCM 336: the MIC rejected a frame LibreSSL says is good\n");
             gFail++; }
      { /* ★ and a single flipped bit at byte 52 -- inside AES block 3, which is exactly where
         * a DHCP reply keeps yiaddr -- must be caught. */
        ApU8 t[336]; memcpy(t,kV_ct,336); t[52] ^= 0x01;
        checkb("CCM 336: a flipped bit in block 3 is REJECTED",
              ApAesCcmDecrypt(kV_key,kV_nonce,kV_aad,22,t,336,kV_mic,back) == 0); } }

    { /* The PN must increment, and a wrap must be reported rather than silently reused --
       * a repeated PN under one key breaks CCMP outright. */
      ApU8 pn[6];
      memset(pn,0,6); pn[5] = 0xFE;
      checkb("PN increment: ordinary case reports no wrap", ApCcmpPnIncrement(pn) == 1);
      checkb("PN increment: low byte advanced", pn[5] == 0xFF);
      checkb("PN increment: carries into the next byte",
            ApCcmpPnIncrement(pn) == 1 && pn[5] == 0x00 && pn[4] == 0x01);
      memset(pn,0xFF,6);
      checkb("PN increment: a full wrap is REPORTED, not hidden",
            ApCcmpPnIncrement(pn) == 0); }

    printf("\n");
    if(gFail==0) printf("ALL KNOWN-ANSWER TESTS PASSED.\n");
    else         printf("%d TEST(S) FAILED -- do not build this into a probe.\n",gFail);
    return gFail ? 1 : 0;
}
