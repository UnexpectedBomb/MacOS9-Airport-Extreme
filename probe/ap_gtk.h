/* ap_gtk.h -- k208: THE GROUP KEY HANDSHAKE (IEEE 802.11-2016 12.7.7), and the table of group keys.
 *
 * ★ WHY. Every WPA2 access point renews its group key -- the key broadcast and multicast frames are sent
 * under, ARP included -- on a timer: hostapd every wpa_group_rekey seconds, UniFi hourly. It sends each
 * associated station a group message 1 carrying the new key, wrapped with that station's KEK, and waits for
 * a message 2. Through k207 the driver counted that frame and dropped it (ApLinkNoteEapol): hostapd resends
 * it until wpa_group_update_count = 4 tries are spent (500 ms, then 1 s apart) and then DISCONNECTS the
 * station with reason 16 ("group key handshake timeout"), and k204 rejoined -- a 1-2 s outage at every
 * renewal, on every network whose owner never turned the renewal off. The MDD's own network has it off,
 * which is why no run saw it.
 *
 * ★ WHAT. Parse message 1 (Key Info, the MIC under the KCK FIRST -- nothing is unwrapped from an
 * unauthenticated frame -- a replay counter newer than the last one accepted, then the Key Data unwrapped
 * with the KEK and the GTK KDE read out), and keep the group keys by KEY INDEX: the AP installs the new key
 * at the other index (1 <-> 2) and switches its own transmissions over once every station has it, so frames
 * under the old index keep arriving for a while and must still decrypt.
 *
 * Message 2 is NOT built here. It is message 4 without PAIRWISE (version | MIC | SECURE, message 1's replay
 * counter, zero nonce, no Key Data), so ap_eapol.h's BuildEapolKeyFrame builds it -- the one EAPOL-Key
 * builder, whose header explains why there must be only one (two copies of the MIC range drift apart).
 *
 * PRIOR ART, read for this: the driver's own 4-way handshake (AirPortShimHandshake: the MIC over the EAPOL
 * header with the MIC field zeroed -- the published standard is wrong about the range, hostap's
 * wpa_common.c documents it -- the RFC 3394 unwrap, the KDE walk); wpa_supplicant's
 * wpa_supplicant_process_1_of_2 / wpa_supplicant_send_2_of_2 (message 2's fields, and "not reinstalling
 * already in-use GTK" -- ApGtkSame below); wpas_glue.c's wpa_ether_send (EAPOL goes out ENCRYPTED once the
 * pairwise key is installed); hostapd's WPA_PTK_GROUP state machine (the retries and the reason-16
 * disconnect above).
 *
 * Pure: HMAC-SHA1 and AES key unwrap (ap_wpa_kdf.h, ap_aes.h) and the CCMP header helpers (ap_ccmp.h), no
 * libc -- the driver links none. gtk_test.c includes it on the host. The EAPOL offsets are ap_eapol.h's
 * (which cannot be included on the host); ap_shim.c pins the two sets equal with _Static_asserts.
 */
#ifndef AP_GTK_H
#define AP_GTK_H

#include "ap_ccmp.h"            /* ApCcmpKeyId/ApCcmpHasExtIv; via ap_aes.h: ApAesUnwrap, ApHmacSha1, ApU8/ApU32 */

#define AP_GK_O_TYPE       1
#define AP_GK_O_BODYLEN    2
#define AP_GK_O_DESCTYPE   4
#define AP_GK_O_KEYINFO    5
#define AP_GK_O_KEYLEN     7
#define AP_GK_O_REPLAY     9
#define AP_GK_O_MIC       81
#define AP_GK_O_DATALEN   97
#define AP_GK_O_DATA      99
#define AP_GK_KI_VERSION   0x0007u
#define AP_GK_KI_PAIRWISE  0x0008u
#define AP_GK_KI_ACK       0x0080u
#define AP_GK_KI_MIC       0x0100u
#define AP_GK_KI_SECURE    0x0200u
#define AP_GK_KI_ENCRYPTED 0x1000u
#define AP_GK_MAX_EAPOL    512u        /* an EAPOL frame we will look at; message 1 is ~160 bytes */

#if defined(__powerpc__) || defined(__POWERPC__) || defined(__ppc__)
#define AP_GK_BARRIER() __asm__ __volatile__("sync" ::: "memory")
#else
#define AP_GK_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

enum { AP_GK_OK = 0, AP_GK_SHORT, AP_GK_NOT_KEY, AP_GK_NOT_GROUP_M1, AP_GK_BAD_MIC, AP_GK_REPLAYED,
       AP_GK_NOT_ENCRYPTED, AP_GK_UNWRAP_FAILED, AP_GK_NO_GTK, AP_GK_NO_PTK, AP_GK_NRESULT };

typedef struct {
    ApU8     replay[8];
    unsigned keyInfo;
    int      keyId;              /* 0..3, or -1 */
    ApU32    gtkLen;
    ApU8     gtk[32];
} ApGkM1;

static const char *ApGkResultName(int r)
{
    switch (r) {
    case AP_GK_OK:            return "ok";
    case AP_GK_SHORT:         return "too short / lengths inconsistent";
    case AP_GK_NOT_KEY:       return "not an RSN EAPOL-Key frame";
    case AP_GK_NOT_GROUP_M1:  return "not group message 1 (Key Info)";
    case AP_GK_BAD_MIC:       return "MIC does NOT verify -- not trusted, not unwrapped";
    case AP_GK_REPLAYED:      return "replay counter not newer -- ignored";
    case AP_GK_NOT_ENCRYPTED: return "key data not encrypted";
    case AP_GK_UNWRAP_FAILED: return "RFC 3394 unwrap FAILED (wrong KEK or altered data)";
    case AP_GK_NO_GTK:        return "no GTK KDE in the key data";
    case AP_GK_NO_PTK:        return "no pairwise key yet (the 4-way handshake has not completed)";
    default:                  return "?";
    }
}

/* The EAPOL-Key replay counter is a big-endian 64-bit number: is a newer than b? */
static int ApGkReplayNewer(const ApU8 *a, const ApU8 *b)
{
    int i;
    for (i = 0; i < 8; i++) if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

/* Group message 1. eap: the frame from the 802.1X header onward, len bytes as received (it may run past
 * the EAPOL body: an Ethernet minimum pad). lastReplay: the newest replay counter accepted from this AP in
 * this association -- message 3's, then each renewal's. Fills *m and returns AP_GK_OK, or says why not.
 * ⚠ One caller, the driver's secondary interrupt handler (ApGkService, after the receive pump; OS 9 never
 *   re-enters it): the scratch buffers are static, so this is not reentrant. They are static to keep the
 *   unwrap's working space off that level's stack (ApAesUnwrapScratch has the measurement). */
static int ApGkParseM1(const ApU8 *eap, ApU32 len, const ApU8 *kck, const ApU8 *kek,
                       const ApU8 *lastReplay, ApGkM1 *m)
{
    static ApU8 scratch[AP_GK_MAX_EAPOL], plain[AP_GK_MAX_EAPOL], rk[AP_AES_EXPKEY];
    ApU8 calc[20];
    ApU32 bodyLen, dataLen, q, plainLen, i;
    unsigned ki;
    int unwrapped;

    for (i = 0; i < 8u; i++) m->replay[i] = 0;
    for (i = 0; i < 32u; i++) m->gtk[i] = 0;
    m->keyInfo = 0; m->gtkLen = 0; m->keyId = -1;
    if (!eap || len < AP_GK_O_DATA) return AP_GK_SHORT;
    if (eap[AP_GK_O_TYPE] != 3u || eap[AP_GK_O_DESCTYPE] != 2u) return AP_GK_NOT_KEY;
    bodyLen = ((ApU32)eap[AP_GK_O_BODYLEN] << 8) | eap[AP_GK_O_BODYLEN + 1];
    if (bodyLen < AP_GK_O_DATA - 4u || 4u + bodyLen > len || 4u + bodyLen > AP_GK_MAX_EAPOL) return AP_GK_SHORT;
    ki = ((unsigned)eap[AP_GK_O_KEYINFO] << 8) | eap[AP_GK_O_KEYINFO + 1];
    m->keyInfo = ki;
    /* A group key (no PAIRWISE), from the AP (ACK), authenticated (MIC), after the 4-way handshake (SECURE). */
    if ((ki & AP_GK_KI_PAIRWISE) || !(ki & AP_GK_KI_ACK) || !(ki & AP_GK_KI_MIC) || !(ki & AP_GK_KI_SECURE))
        return AP_GK_NOT_GROUP_M1;
    for (i = 0; i < 8u; i++) m->replay[i] = eap[AP_GK_O_REPLAY + i];
    dataLen = ((ApU32)eap[AP_GK_O_DATALEN] << 8) | eap[AP_GK_O_DATALEN + 1];
    if (AP_GK_O_DATA + dataLen > 4u + bodyLen) return AP_GK_SHORT;

    /* ⛔ THE MIC FIRST: HMAC-SHA1 under the KCK, over the 802.1X header onward with the MIC zeroed. */
    for (i = 0; i < 4u + bodyLen; i++) scratch[i] = eap[i];
    for (i = 0; i < 16u; i++) scratch[AP_GK_O_MIC + i] = 0;
    ApHmacSha1(kck, 16u, scratch, 4u + bodyLen, calc);
    for (i = 0; i < 16u; i++) if (calc[i] != eap[AP_GK_O_MIC + i]) return AP_GK_BAD_MIC;

    if (!ApGkReplayNewer(m->replay, lastReplay)) return AP_GK_REPLAYED;
    if (!(ki & AP_GK_KI_ENCRYPTED)) return AP_GK_NOT_ENCRYPTED;
    if (dataLen < 24u || (dataLen % 8u) != 0u || dataLen - 8u > sizeof(plain)) return AP_GK_UNWRAP_FAILED;
    /* The MIC's copy of the frame is finished with, so its buffer is the unwrap's working space. It holds
     * the key data in the clear by the end, and rk the KEK's schedule: both are wiped whatever the outcome. */
    unwrapped = ApAesUnwrapScratch(kek, eap + AP_GK_O_DATA, dataLen, plain, rk, scratch, (ApU32)sizeof(scratch));
    for (i = 0; i < sizeof(rk); i++) rk[i] = 0;
    for (i = 0; i < sizeof(scratch); i++) scratch[i] = 0;
    if (!unwrapped) return AP_GK_UNWRAP_FAILED;
    plainLen = dataLen - 8u;

    /* The KDEs: 0xDD, length, OUI 00-0F-AC, type 1 = GTK: key ID (bits 0-1) and Tx, a reserved byte, the key.
     * (0xDD with length 0 then zeros is RFC 3394 padding, and walks off harmlessly.) */
    for (q = 0; q + 2u <= plainLen; ) {
        ApU8 id = plain[q], ln = plain[q + 1];
        if (q + 2u + ln > plainLen) break;
        if (id == 0xDDu && ln >= 7u && plain[q + 2] == 0x00u && plain[q + 3] == 0x0Fu
            && plain[q + 4] == 0xACu && plain[q + 5] == 0x01u) {
            ApU32 glen = (ApU32)ln - 6u;
            if (glen <= 32u) {
                m->keyId = plain[q + 6] & 0x03;
                m->gtkLen = glen;
                for (i = 0; i < glen; i++) m->gtk[i] = plain[q + 8 + i];
            }
        }
        q += 2u + ln;
    }
    for (i = 0; i < sizeof(plain); i++) plain[i] = 0;     /* the key's clear copy does not outlive the call */
    return (m->keyId < 0) ? AP_GK_NO_GTK : AP_GK_OK;
}

/* ---- the group keys, by key index ---------------------------------------------------------------------
 * Written by the 4-way handshake (task level, with the receive interrupt quiesced) and by a renewal (the
 * receive pump itself); read by the receive pump. So today no reader can race a writer. The order is kept
 * anyway -- a slot is marked empty BEFORE its bytes change and filled only AFTER, each with a barrier -- so
 * that stays true if a writer ever moves: a frame arriving mid-install finds "no key" and is dropped, never
 * decrypted under half a key. */
typedef struct {
    ApU8           key[4][32];
    volatile ApU8  len[4];                     /* 0 = empty; a usable CCMP key is 16 */
} ApGtkTable;

static void ApGtkClear(ApGtkTable *t)
{
    int i, j;
    for (i = 0; i < 4; i++) t->len[i] = 0;
    AP_GK_BARRIER();
    for (i = 0; i < 4; i++) for (j = 0; j < 32; j++) t->key[i][j] = 0;
}

static void ApGtkInstall(ApGtkTable *t, int id, const ApU8 *key, ApU32 len)
{
    ApU32 i;
    if (id < 0 || id > 3 || len == 0u || len > 32u) return;
    t->len[id] = 0;
    AP_GK_BARRIER();
    for (i = 0; i < 32u; i++) t->key[id][i] = (i < len) ? key[i] : 0;
    AP_GK_BARRIER();
    t->len[id] = (ApU8)len;
}

/* Does slot id already hold exactly this key? A renewal message 1 the AP RETRIED (our message 2 was lost)
 * carries the key we already installed; wpa_supplicant answers it without reinstalling ("not reinstalling
 * already in-use GTK"), and so does the driver. */
static int ApGtkSame(const ApGtkTable *t, int id, const ApU8 *key, ApU32 len)
{
    ApU32 i;
    if (id < 0 || id > 3 || len == 0u || len > 32u || t->len[id] != len) return 0;
    for (i = 0; i < len; i++) if (t->key[id][i] != key[i]) return 0;
    return 1;
}

/* The key for a frame under key index id, or NULL if that slot holds no CCMP-sized key. */
static const ApU8 *ApGtkFor(const ApGtkTable *t, int id)
{
    if (id < 0 || id > 3 || t->len[id] < 16u) return 0;
    return t->key[id];
}

/* The key index a protected data frame names: bits 6-7 of the 4th byte of its CCMP header, which follows
 * the MAC header -- located with ApCcmpDecap's own rule (QoS data = subtype 8; 4-address = ToDS and FromDS).
 * f: the 802.11 frame, flen bytes without the FCS. -1 if it is not a protected data frame, is too short to
 * carry a CCMP header, or has no ExtIV (so is not CCMP). */
static int ApGtkFrameKeyId(const ApU8 *f, ApU32 flen)
{
    unsigned fc;
    ApU32 hdr;
    if (!f || flen < 24u) return -1;
    fc = (unsigned)f[0] | ((unsigned)f[1] << 8);
    if (((fc >> 2) & 3u) != 2u || !(fc & 0x4000u)) return -1;
    hdr = 24u + (((fc & 0x0300u) == 0x0300u) ? 6u : 0u) + ((((fc >> 4) & 0x0Fu) == 8u) ? 2u : 0u);
    if (flen < hdr + 8u) return -1;
    if (!ApCcmpHasExtIv(f + hdr)) return -1;
    return ApCcmpKeyId(f + hdr);
}

#endif /* AP_GTK_H */
