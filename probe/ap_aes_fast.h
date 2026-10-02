/* ap_aes_fast.h -- a table-driven AES-128 ENCRYPT for the CCMP data path. k199.
 *
 * ★★★ WHY THIS EXISTS. k198's clean run (Bluetooth unplugged) moved 356 KB/s up and 329 KB/s down
 * against Tiger's 2085/1685 on the same card and AP, and the capture showed the Mac, not the air, as
 * the limit: 94% of frames went out at 54 Mbps on the FIRST try, yet uploads left one 1460-byte
 * segment every ~3.4 ms and never two back to back (the air could carry one every ~0.4 ms), and the
 * Mac ACKed downloads ~16 ms after the data. Two facts account for that:
 *   - the WHOLE driver is compiled at -O0: CMakeLists.txt sets no -O and CMAKE_BUILD_TYPE is empty,
 *     so build/CMakeFiles/AirPortOTModl.dir/flags.make carries none;
 *   - ap_aes.h is a byte-at-a-time FIPS-197 transcription -- 16 S-box lookups, 32 xtime() CALLS and
 *     16 XORs per round, ten rounds a block -- and CCMP runs ~190 blocks per 1500-byte frame (CTR for
 *     the payload plus CBC-MAC over it).
 * k199's self-benchmark times both on the G4 itself, so "that was the cost" is a measurement in the
 * log, not this paragraph's claim.
 *
 * ★ WHAT THIS IS: the standard 32-bit T-table formulation -- rijndael-alg-fst.c (the reference
 * optimised code), Linux crypto/aes_generic.c, mbedTLS aes.c FT0..FT3 -- with ONE 256-entry table
 * rotated for the other three byte positions, so a round is 16 lookups + 16 XORs and no field
 * arithmetic. Compiled at O2 by the pragma below while the rest of the driver stays at -O0. That is
 * safe HERE and only here: this is pure arithmetic on caller buffers -- no MMIO, no volatile, no
 * state shared with interrupt level -- so optimisation has nothing to reorder that matters. Moving
 * the rest of the driver off -O0 is a separate decision with its own audit; it has only ever run
 * unoptimised.
 *
 * ⚠ ENCRYPT ONLY. CCM uses the FORWARD cipher in both directions (the CTR keystream and the CBC-MAC),
 *   so the data path never needs the inverse. RFC 3394 unwrap (the GTK, once per handshake) keeps the
 *   byte-wise ApAesDecryptBlock in ap_aes.h.
 *
 * ⚠ ap_aes.h's ApAesEncryptBlock STAYS, unchanged and unoptimised. It is the oracle this file is
 *   tested against (kdf_test.c: every table entry, FIPS-197, 200 000 random blocks, CCM at every
 *   length), the on-target self-test's reference (ApAesFastSelfTest in ap_ccmp.h runs the same
 *   differential ON THE G4, where a miscompile would actually show), the fallback if that test fails,
 *   and the "before" of k199's benchmark.
 *
 * ⚠ MACROS, NOT HELPERS, INSIDE THE ROUND. Retro68 gcc 12.2 does not inline a static helper into a
 *   pragma-O2 function when the file itself is compiled at -O0 -- measured: a pragma-O2 MixColumns
 *   halved (131 -> 65 instructions) but still did `bl .xt2` for every xtime.
 *
 * ⚠ ApW32, NOT ApU32. ApU32 is `unsigned long`: 32 bits on the G4, 64 on the Mac that runs
 *   kdf_test.c. Rotations and `>> 24` need exactly 32 bits, so this file uses `unsigned int`, checked
 *   at compile time, and both builds execute the same arithmetic.
 *
 * ⚠ NOT CONSTANT TIME -- see ap_aes.h; the same trade, recorded rather than hidden.
 * C89; builds under Retro68 and clang. */
#ifndef AP_AES_FAST_H
#define AP_AES_FAST_H

#include "ap_aes.h"            /* ApU8, apAesSbox, apAesRcon, AP_AES_ROUNDS */

typedef unsigned int ApW32;
typedef char ApW32MustBe32Bits[(sizeof(ApW32) == 4) ? 1 : -1];

#define AP_AESF_WORDS 44       /* (rounds + 1) * 4 round-key words */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif

/* Te0[x] = S(x) * {02,01,01,03}, packed row 0 in the most significant byte. Generated from
 * apAesSbox above; kdf_test.c recomputes all 256 entries from the S-box and compares, so a typo
 * here cannot pass. Te0[0] = 0xc66363a5 and Te0[255] = 0x2c16163a are the published values. */
static const ApW32 apAesTe0[256] = {
0xc66363a5U,0xf87c7c84U,0xee777799U,0xf67b7b8dU,0xfff2f20dU,0xd66b6bbdU,
0xde6f6fb1U,0x91c5c554U,0x60303050U,0x02010103U,0xce6767a9U,0x562b2b7dU,
0xe7fefe19U,0xb5d7d762U,0x4dababe6U,0xec76769aU,0x8fcaca45U,0x1f82829dU,
0x89c9c940U,0xfa7d7d87U,0xeffafa15U,0xb25959ebU,0x8e4747c9U,0xfbf0f00bU,
0x41adadecU,0xb3d4d467U,0x5fa2a2fdU,0x45afafeaU,0x239c9cbfU,0x53a4a4f7U,
0xe4727296U,0x9bc0c05bU,0x75b7b7c2U,0xe1fdfd1cU,0x3d9393aeU,0x4c26266aU,
0x6c36365aU,0x7e3f3f41U,0xf5f7f702U,0x83cccc4fU,0x6834345cU,0x51a5a5f4U,
0xd1e5e534U,0xf9f1f108U,0xe2717193U,0xabd8d873U,0x62313153U,0x2a15153fU,
0x0804040cU,0x95c7c752U,0x46232365U,0x9dc3c35eU,0x30181828U,0x379696a1U,
0x0a05050fU,0x2f9a9ab5U,0x0e070709U,0x24121236U,0x1b80809bU,0xdfe2e23dU,
0xcdebeb26U,0x4e272769U,0x7fb2b2cdU,0xea75759fU,0x1209091bU,0x1d83839eU,
0x582c2c74U,0x341a1a2eU,0x361b1b2dU,0xdc6e6eb2U,0xb45a5aeeU,0x5ba0a0fbU,
0xa45252f6U,0x763b3b4dU,0xb7d6d661U,0x7db3b3ceU,0x5229297bU,0xdde3e33eU,
0x5e2f2f71U,0x13848497U,0xa65353f5U,0xb9d1d168U,0x00000000U,0xc1eded2cU,
0x40202060U,0xe3fcfc1fU,0x79b1b1c8U,0xb65b5bedU,0xd46a6abeU,0x8dcbcb46U,
0x67bebed9U,0x7239394bU,0x944a4adeU,0x984c4cd4U,0xb05858e8U,0x85cfcf4aU,
0xbbd0d06bU,0xc5efef2aU,0x4faaaae5U,0xedfbfb16U,0x864343c5U,0x9a4d4dd7U,
0x66333355U,0x11858594U,0x8a4545cfU,0xe9f9f910U,0x04020206U,0xfe7f7f81U,
0xa05050f0U,0x783c3c44U,0x259f9fbaU,0x4ba8a8e3U,0xa25151f3U,0x5da3a3feU,
0x804040c0U,0x058f8f8aU,0x3f9292adU,0x219d9dbcU,0x70383848U,0xf1f5f504U,
0x63bcbcdfU,0x77b6b6c1U,0xafdada75U,0x42212163U,0x20101030U,0xe5ffff1aU,
0xfdf3f30eU,0xbfd2d26dU,0x81cdcd4cU,0x180c0c14U,0x26131335U,0xc3ecec2fU,
0xbe5f5fe1U,0x359797a2U,0x884444ccU,0x2e171739U,0x93c4c457U,0x55a7a7f2U,
0xfc7e7e82U,0x7a3d3d47U,0xc86464acU,0xba5d5de7U,0x3219192bU,0xe6737395U,
0xc06060a0U,0x19818198U,0x9e4f4fd1U,0xa3dcdc7fU,0x44222266U,0x542a2a7eU,
0x3b9090abU,0x0b888883U,0x8c4646caU,0xc7eeee29U,0x6bb8b8d3U,0x2814143cU,
0xa7dede79U,0xbc5e5ee2U,0x160b0b1dU,0xaddbdb76U,0xdbe0e03bU,0x64323256U,
0x743a3a4eU,0x140a0a1eU,0x924949dbU,0x0c06060aU,0x4824246cU,0xb85c5ce4U,
0x9fc2c25dU,0xbdd3d36eU,0x43acacefU,0xc46262a6U,0x399191a8U,0x319595a4U,
0xd3e4e437U,0xf279798bU,0xd5e7e732U,0x8bc8c843U,0x6e373759U,0xda6d6db7U,
0x018d8d8cU,0xb1d5d564U,0x9c4e4ed2U,0x49a9a9e0U,0xd86c6cb4U,0xac5656faU,
0xf3f4f407U,0xcfeaea25U,0xca6565afU,0xf47a7a8eU,0x47aeaee9U,0x10080818U,
0x6fbabad5U,0xf0787888U,0x4a25256fU,0x5c2e2e72U,0x381c1c24U,0x57a6a6f1U,
0x73b4b4c7U,0x97c6c651U,0xcbe8e823U,0xa1dddd7cU,0xe874749cU,0x3e1f1f21U,
0x964b4bddU,0x61bdbddcU,0x0d8b8b86U,0x0f8a8a85U,0xe0707090U,0x7c3e3e42U,
0x71b5b5c4U,0xcc6666aaU,0x904848d8U,0x06030305U,0xf7f6f601U,0x1c0e0e12U,
0xc26161a3U,0x6a35355fU,0xae5757f9U,0x69b9b9d0U,0x17868691U,0x99c1c158U,
0x3a1d1d27U,0x279e9eb9U,0xd9e1e138U,0xebf8f813U,0x2b9898b3U,0x22111133U,
0xd26969bbU,0xa9d9d970U,0x078e8e89U,0x339494a7U,0x2d9b9bb6U,0x3c1e1e22U,
0x15878792U,0xc9e9e920U,0x87cece49U,0xaa5555ffU,0x50282878U,0xa5dfdf7aU,
0x038c8c8fU,0x59a1a1f8U,0x09898980U,0x1a0d0d17U,0x65bfbfdaU,0xd7e6e631U,
0x844242c6U,0xd06868b8U,0x824141c3U,0x299999b0U,0x5a2d2d77U,0x1e0f0f11U,
0x7bb0b0cbU,0xa85454fcU,0x6dbbbbd6U,0x2c16163aU };

/* Te1..Te3 are Te0 rotated right by 8/16/24 -- one rlwinm each on PowerPC -- so one 1 KB table
 * serves all four byte positions of the MixColumns matrix. */
#define AP_AESF_R8(x)   (((x) >> 8)  | ((x) << 24))
#define AP_AESF_R16(x)  (((x) >> 16) | ((x) << 16))
#define AP_AESF_R24(x)  (((x) >> 24) | ((x) << 8))
#define AP_AESF_GET(p)  (((ApW32)(p)[0] << 24) | ((ApW32)(p)[1] << 16) | \
                         ((ApW32)(p)[2] << 8)  |  (ApW32)(p)[3])
#define AP_AESF_PUT(p,v) do{ ApW32 v_ = (v); (p)[0] = (ApU8)(v_ >> 24); (p)[1] = (ApU8)(v_ >> 16); \
                             (p)[2] = (ApU8)(v_ >> 8); (p)[3] = (ApU8)v_; }while(0)
/* one full round: output column c takes row r from input column (c + r) mod 4 (ShiftRows), each
 * byte through Te0 rotated for its row (SubBytes + MixColumns), then the round key */
#define AP_AESF_COL(a,b,c,d,k) (apAesTe0[(a) >> 24] ^ AP_AESF_R8(apAesTe0[((b) >> 16) & 0xFF]) ^ \
                                AP_AESF_R16(apAesTe0[((c) >> 8) & 0xFF]) ^ \
                                AP_AESF_R24(apAesTe0[(d) & 0xFF]) ^ (k))
/* the last round has no MixColumns: SubBytes + ShiftRows + round key */
#define AP_AESF_LAST(a,b,c,d,k) (((ApW32)apAesSbox[(a) >> 24] << 24) ^ \
                                 ((ApW32)apAesSbox[((b) >> 16) & 0xFF] << 16) ^ \
                                 ((ApW32)apAesSbox[((c) >> 8) & 0xFF] << 8) ^ \
                                  (ApW32)apAesSbox[(d) & 0xFF] ^ (k))

/* FIPS-197 §5.2 in words: 44 round-key words from the 16-byte key. The same schedule as
 * ApAesExpandKey, which kdf_test.c checks word for byte. */
static void ApAesFastExpandKey(const ApU8 *key,ApW32 *w)
{
    int i;
    for(i = 0; i < 4; i++) w[i] = AP_AESF_GET(key + 4*i);
    for(i = 4; i < AP_AESF_WORDS; i++){
        ApW32 t = w[i-1];
        if((i & 3) == 0)          /* SubWord(RotWord(t)) ^ Rcon: RotWord moves byte 0 to the bottom */
            t = ((ApW32)apAesSbox[(t >> 16) & 0xFF] << 24) ^ ((ApW32)apAesSbox[(t >> 8) & 0xFF] << 16) ^
                ((ApW32)apAesSbox[t & 0xFF] << 8)          ^  (ApW32)apAesSbox[t >> 24] ^
                ((ApW32)apAesRcon[i >> 2] << 24);
        w[i] = w[i-4] ^ t; }
}

/* One AES-128 block, in -> out (they may alias: the whole block is read before anything is written). */
static void ApAesFastEncryptBlock(const ApW32 *w,const ApU8 *in,ApU8 *out)
{
    ApW32 s0, s1, s2, s3, t0, t1, t2, t3;
    const ApW32 *k;
    int r;
    s0 = AP_AESF_GET(in)      ^ w[0];
    s1 = AP_AESF_GET(in + 4)  ^ w[1];
    s2 = AP_AESF_GET(in + 8)  ^ w[2];
    s3 = AP_AESF_GET(in + 12) ^ w[3];
    for(r = 1; r < AP_AES_ROUNDS; r++){
        k  = w + 4*r;
        t0 = AP_AESF_COL(s0,s1,s2,s3,k[0]);
        t1 = AP_AESF_COL(s1,s2,s3,s0,k[1]);
        t2 = AP_AESF_COL(s2,s3,s0,s1,k[2]);
        t3 = AP_AESF_COL(s3,s0,s1,s2,k[3]);
        s0 = t0; s1 = t1; s2 = t2; s3 = t3; }
    k  = w + 4*AP_AES_ROUNDS;
    t0 = AP_AESF_LAST(s0,s1,s2,s3,k[0]);
    t1 = AP_AESF_LAST(s1,s2,s3,s0,k[1]);
    t2 = AP_AESF_LAST(s2,s3,s0,s1,k[2]);
    t3 = AP_AESF_LAST(s3,s0,s1,s2,k[3]);
    AP_AESF_PUT(out,t0); AP_AESF_PUT(out + 4,t1); AP_AESF_PUT(out + 8,t2); AP_AESF_PUT(out + 12,t3);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

#endif /* AP_AES_FAST_H */
