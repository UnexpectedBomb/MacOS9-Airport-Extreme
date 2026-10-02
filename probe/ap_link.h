/* ap_link.h -- the LINK layer (k204): notice a lost association, and decide when to join again.
 *
 * Pure computation, no MMIO, no Toolbox: included by ap_shim.c for the driver AND by link_test.c on the
 * host, so what the host suite proves is the code the card runs (the ap_rate.h / rate_test.c pattern).
 *
 * ★ WHY IT EXISTS. Through k203 the join ran ONCE, in InitStreamModule at boot (three attempts since
 * k203), and nothing ever ran it again. A boot whose join failed stayed offline until a reboot (k202
 * boot 1), and a link lost mid-session -- the AP restarting, the AP deauthenticating us, the radio
 * going quiet -- was never even noticed: the receive pump dropped every management frame as "not data".
 * Tiger rejoins on its own: its banked system.log shows an auto-join 5 s after the driver attached at
 * boot, and a rejoin within 0-7 s of every wake ([[reference_tiger_airport_rejoin_evidence]]).
 *
 * ★ HOW. Three pieces, split by execution level exactly as the EHCI driver's app-less pump is:
 *   the receive pump   (secondary interrupt level) records FACTS: the tick of the latest beacon from
 *                      our BSSID, and every deauthentication / disassociation addressed to us.
 *   the heartbeat      (SetInterruptTimer, secondary level, every AP_LINK_TICK_MS) advances the link
 *                      clock and posts a Notification Manager request when there is work.
 *   the NM response    (task level, in whatever process runs its event loop) owns ALL decisions --
 *                      ApLinkStep below -- and runs the join, which spins and logs and so cannot run
 *                      anywhere else.
 * Deciding only at task level means no link state is ever written from two levels.
 *
 * ★ ONE CLOCK. Every time here is in LINK TICKS: runs of the heartbeat. The pump stamps a beacon with
 * the tick count, and the task compares against the same count -- the EHCI h83 lesson ("stamp the arm in
 * the same clock the check reads"; a watchdog reading ms against SIH passes never fired in 100 builds).
 * It also sidesteps the 32-bit timebase, which wraps every ~103 s on this G4, and lowmem Ticks, which is
 * driven by VBL (EHCI h64).
 *
 * ★ WHEN TO TRY AGAIN. An attempt freezes the foreground process for its length (the join borrows its
 * event loop): ~0.1-0.3 s with the AP present, up to ~0.7 s without (the 600 ms beacon scan). So:
 *   - an attempt is made only when the AP is AUDIBLE (a beacon from our BSSID within
 *     AP_LINK_AUDIBLE_TICKS) -- an AP that is restarting costs nothing until it is back;
 *   - failures while audible back off 1, 2, 5, 10, then every 30 s;
 *   - a SILENT AP is still tried on a slow schedule, 20 s doubling to 5 min, because our beacon tracking
 *     is blind to a BSSID that changed (a replaced AP) and to a boot that never learned one.
 *   (wpa_supplicant also escalates its reconnect delay per failure; its source sits behind a bot wall
 *   and was not read for this, so these steps are chosen for the freeze cost above, not copied.)
 *
 * ⚠ ApLinkClassifyMgmt and ApLinkClassifyEapol run at SECONDARY INTERRUPT level (the receive pump):
 *   bounded reads of the frame they are handed, no statics, no Toolbox. ApLinkStep/ApLinkAttempted are
 *   task-level only by design (the only writer of ApLink), though nothing in them would care.
 */
#ifndef AP_LINK_H
#define AP_LINK_H

typedef unsigned int ApLinkU32;               /* 32 bits on the G4 AND the 64-bit host: wrap tests agree */
typedef char ApLinkU32IsFourBytes[(sizeof(ApLinkU32) == 4) ? 1 : -1];

#define AP_LINK_TICK_MS          250u    /* the heartbeat period, and the unit of every time below */
#define AP_LINK_LOSS_TICKS        12u    /* 3 s without a beacon from our AP, while up and receiving -> lost.
                                          * mac80211 declares beacon loss after 7 missed beacons (~0.7 s) but
                                          * then PROBES the AP before disconnecting; we skip the probe and
                                          * wait longer instead. */
#define AP_LINK_AUDIBLE_TICKS      6u    /* a beacon from our BSSID within 1.5 s -> the AP is there */
#define AP_LINK_KICK_WAIT_TICKS    2u    /* 0.5 s after a deauth/disassoc before the first rejoin */
#define AP_LINK_BOOT_WAIT_TICKS    8u    /* 2 s after the boot join's attempts all failed */

/* --- what the receive pump sees ---------------------------------------------------------------- */
enum { AP_LINK_MGMT_OTHER = 0, AP_LINK_MGMT_BEACON = 1, AP_LINK_MGMT_DEAUTH = 2, AP_LINK_MGMT_DISASSOC = 3 };

static int ApLinkMacEq(const unsigned char *a, const unsigned char *b)
{
    int k;
    for (k = 0; k < 6; k++) if (a[k] != b[k]) return 0;
    return 1;
}

static int ApLinkIsBroadcast(const unsigned char *a)
{
    int k;
    for (k = 0; k < 6; k++) if (a[k] != 0xFFu) return 0;
    return 1;
}

/* One 802.11 frame, header first (after the PLCP), flen bytes WITHOUT the FCS. A beacon counts only if
 * our BSSID sent it; a deauth/disassoc counts only if our BSSID sent it to US or to everyone, and only if
 * it is not protected (802.11w -- which our association never negotiates, so a protected one is not
 * ours to read). The reason code is the 2 bytes after the 24-byte header. */
static int ApLinkClassifyMgmt(const unsigned char *f, ApLinkU32 flen, const unsigned char *bssid,
                              const unsigned char *me, unsigned short *reason)
{
    unsigned fc, type, sub;
    if (reason) *reason = 0;
    if (!f || !bssid || !me || flen < 24u) return AP_LINK_MGMT_OTHER;
    fc   = (unsigned)f[0] | ((unsigned)f[1] << 8);
    type = (fc >> 2) & 3u;
    sub  = (fc >> 4) & 0xFu;
    if (type != 0u) return AP_LINK_MGMT_OTHER;                     /* management frames only */
    if (!ApLinkMacEq(f + 16, bssid) || !ApLinkMacEq(f + 10, bssid)) return AP_LINK_MGMT_OTHER;
    if (sub == 8u) return AP_LINK_MGMT_BEACON;
    if (sub != 10u && sub != 12u) return AP_LINK_MGMT_OTHER;
    if (!ApLinkMacEq(f + 4, me) && !ApLinkIsBroadcast(f + 4)) return AP_LINK_MGMT_OTHER;
    if (fc & 0x4000u) return AP_LINK_MGMT_OTHER;                   /* Protected Frame bit */
    if (flen < 26u) return AP_LINK_MGMT_OTHER;
    if (reason) *reason = (unsigned short)((unsigned)f[24] | ((unsigned)f[25] << 8));
    return (sub == 12u) ? AP_LINK_MGMT_DEAUTH : AP_LINK_MGMT_DISASSOC;
}

/* An EAPOL frame after the join means the AP is re-keying. The data path decrypts it like any frame and,
 * through k203, handed it to Open Transport, which has no stream bound to 0x888E and dropped it: the AP
 * got no answer. Classify it so the log can say what the AP asked for. Key Information (802.11-2016
 * 12.7.2): 0x0008 pairwise, 0x0080 ACK, 0x0100 MIC. A group-key message 1 is not pairwise, with ACK and
 * MIC; hostapd retries it wpa_group_update_count = 4 times (first after 500 ms) and then DISCONNECTS the
 * station with reason 16 (wpa_auth.c, SM_STATE(WPA_PTK_GROUP, KEYERROR)). Its default rekey interval is
 * wpa_group_rekey = 600 s (ap_config.c). */
enum { AP_LINK_EAPOL_NONE = 0, AP_LINK_EAPOL_OTHER = 1, AP_LINK_EAPOL_PAIR_M1 = 2,
       AP_LINK_EAPOL_PAIR_M3 = 3, AP_LINK_EAPOL_GROUP_M1 = 4 };

/* eth: the converted Ethernet frame (dst, src, type, payload), len bytes. */
static int ApLinkClassifyEapol(const unsigned char *eth, ApLinkU32 len, unsigned *keyInfo)
{
    unsigned ki;
    if (keyInfo) *keyInfo = 0;
    if (!eth || len < 14u || eth[12] != 0x88u || eth[13] != 0x8Eu) return AP_LINK_EAPOL_NONE;
    /* 802.1X: version, packet type (3 = EAPOL-Key), body length (2); then the descriptor type and the
     * 2-byte Key Information, big-endian. */
    if (len < 21u || eth[15] != 3u) return AP_LINK_EAPOL_OTHER;
    ki = ((unsigned)eth[19] << 8) | (unsigned)eth[20];
    if (keyInfo) *keyInfo = ki;
    if (!(ki & 0x0080u)) return AP_LINK_EAPOL_OTHER;               /* not from the authenticator */
    if (ki & 0x0008u) return (ki & 0x0100u) ? AP_LINK_EAPOL_PAIR_M3 : AP_LINK_EAPOL_PAIR_M1;
    return (ki & 0x0100u) ? AP_LINK_EAPOL_GROUP_M1 : AP_LINK_EAPOL_OTHER;
}

/* --- the decision, task level ------------------------------------------------------------------ */
enum { AP_LINK_WHY_NONE = 0, AP_LINK_WHY_BOOT = 1, AP_LINK_WHY_BEACONS = 2,
       AP_LINK_WHY_DEAUTH = 3, AP_LINK_WHY_DISASSOC = 4, AP_LINK_WHY_OFF = 5 };
enum { AP_LINK_ACT_NONE = 0, AP_LINK_ACT_WENT_DOWN = 1, AP_LINK_ACT_TRY = 2 };

typedef struct {
    int            off;             /* k207: the user turned AirPort off -- no attempts until on again */
    int            up;              /* 1 = associated and keyed */
    int            why;             /* AP_LINK_WHY_*: why the latest outage began */
    unsigned short reason;          /* its 802.11 reason code, for a deauth/disassoc */
    int            tried;           /* an attempt has been made in this outage (lastTry is valid) */
    ApLinkU32      downTick;        /* when this outage began */
    ApLinkU32      nextTick;        /* the earliest next attempt */
    ApLinkU32      lastTryTick;
    unsigned       fails;           /* consecutive failed attempts in this outage */
    ApLinkU32      kickSeen;        /* the kick sequence number already acted on */
    ApLinkU32      downs, tries, rejoins;       /* totals, for the snapshot */
} ApLink;

typedef struct {
    ApLinkU32      now;             /* link ticks */
    ApLinkU32      lastBeacon;      /* tick of the latest beacon from our BSSID (valid iff beaconSeen) */
    int            beaconSeen;
    ApLinkU32      kickSeq;         /* deauths + disassocs recorded so far */
    int            kickKind;        /* AP_LINK_MGMT_DEAUTH / _DISASSOC, of the latest */
    unsigned short kickReason;
    int            monitoring;      /* the receive pump is running, so silence means something */
} ApLinkFacts;

/* Age in ticks, never negative: the pump can stamp a beacon with a tick newer than the `now` the task
 * read a moment earlier, and an unsigned now - last would then read as ~4 billion ticks of silence. */
static ApLinkU32 ApLinkAge(ApLinkU32 now, ApLinkU32 then)
{
    int d = (int)(now - then);
    return (d > 0) ? (ApLinkU32)d : 0u;
}

/* 3 s of silence from our AP. Shared by the task's decision and the heartbeat's hint, so the two can
 * never disagree about what "lost" means. */
static int ApLinkSilent(ApLinkU32 now, ApLinkU32 lastBeacon, int beaconSeen)
{
    return beaconSeen && ApLinkAge(now, lastBeacon) >= AP_LINK_LOSS_TICKS;
}

static ApLinkU32 ApLinkRetryTicks(unsigned fails)      /* after a failed attempt, AP audible */
{
    static const ApLinkU32 k[5] = { 4u, 8u, 20u, 40u, 120u };            /* 1, 2, 5, 10, 30 s */
    if (fails == 0u) return 0u;
    return k[(fails > 5u) ? 4u : fails - 1u];
}

static ApLinkU32 ApLinkFallbackTicks(unsigned fails)   /* between attempts at a SILENT AP */
{
    static const ApLinkU32 k[5] = { 80u, 160u, 320u, 640u, 1200u };      /* 20 s .. 5 min */
    return k[(fails > 4u) ? 4u : fails];
}

static void ApLinkGoDown(ApLink *L, ApLinkU32 now, int why, unsigned short reason, ApLinkU32 wait)
{
    L->up = 0; L->why = why; L->reason = reason; L->tried = 0; L->fails = 0;
    L->downTick = now; L->nextTick = now + wait;
    L->downs++;
}

static void ApLinkInit(ApLink *L, int up, ApLinkU32 now, ApLinkU32 kickSeq)
{
    ApLink z = { 0 };
    *L = z;
    L->kickSeen = kickSeq;
    if (up) L->up = 1;
    else    ApLinkGoDown(L, now, AP_LINK_WHY_BOOT, 0, AP_LINK_BOOT_WAIT_TICKS);
}

/* ★ k207: AIRPORT OFF AND ON, the user's switch. Off is not an outage -- the user asked for it -- so it
 * does not count in `downs`, and nothing is attempted until on. The caller has already said goodbye to the
 * AP and turned the radio off (in that order: a radio that is off cannot send the deauth). */
static void ApLinkTurnOff(ApLink *L, ApLinkU32 now)
{
    L->off = 1; L->up = 0; L->why = AP_LINK_WHY_OFF; L->reason = 0; L->tried = 0; L->fails = 0;
    L->downTick = now; L->nextTick = now;
}

/* On again: down, "because it was off", and due at once -- the first attempt is made the moment our AP is
 * audible again, which with the radio just back on is the next beacon (~100 ms). An AP that stays silent
 * gets the ordinary fallback schedule (20 s, doubling). The caller has already turned the radio on. */
static void ApLinkTurnOn(ApLink *L, ApLinkU32 now)
{
    L->off = 0; L->up = 0; L->why = AP_LINK_WHY_OFF; L->reason = 0; L->tried = 0; L->fails = 0;
    L->downTick = now; L->nextTick = now;
}

/* Returns AP_LINK_ACT_WENT_DOWN when this call declared the link lost (the caller stops transmitting),
 * AP_LINK_ACT_TRY when the caller should make ONE join attempt now and report it with ApLinkAttempted. */
static int ApLinkStep(ApLink *L, const ApLinkFacts *f)
{
    int audible;
    if (L->off) {                    /* k207: consume kicks (the AP answering our deauth), decide nothing */
        L->kickSeen = f->kickSeq;
        return AP_LINK_ACT_NONE;
    }
    if (L->up) {
        if (f->kickSeq != L->kickSeen) {
            L->kickSeen = f->kickSeq;
            ApLinkGoDown(L, f->now, (f->kickKind == AP_LINK_MGMT_DISASSOC) ? AP_LINK_WHY_DISASSOC
                                                                           : AP_LINK_WHY_DEAUTH,
                         f->kickReason, AP_LINK_KICK_WAIT_TICKS);
            return AP_LINK_ACT_WENT_DOWN;
        }
        if (f->monitoring && ApLinkSilent(f->now, f->lastBeacon, f->beaconSeen)) {
            ApLinkGoDown(L, f->now, AP_LINK_WHY_BEACONS, 0, 0u);
            return AP_LINK_ACT_WENT_DOWN;
        }
        return AP_LINK_ACT_NONE;
    }
    /* Down. More deauths are expected now (the AP answers any frame of ours with one); they are
     * consumed without restarting the schedule, or a chatty AP could postpone the rejoin forever. */
    L->kickSeen = f->kickSeq;
    if ((int)(f->now - L->nextTick) < 0) return AP_LINK_ACT_NONE;
    audible = f->beaconSeen && ApLinkAge(f->now, f->lastBeacon) < AP_LINK_AUDIBLE_TICKS;
    if (!audible) {
        ApLinkU32 since = ApLinkAge(f->now, L->tried ? L->lastTryTick : L->downTick);
        if (since < ApLinkFallbackTicks(L->fails)) return AP_LINK_ACT_NONE;
    }
    L->tried = 1; L->lastTryTick = f->now; L->tries++;
    return AP_LINK_ACT_TRY;
}

static void ApLinkAttempted(ApLink *L, ApLinkU32 now, int ok)
{
    if (ok) { L->up = 1; L->fails = 0; L->rejoins++; return; }
    L->fails++;
    L->nextTick = now + ApLinkRetryTicks(L->fails);
}

/* --- words for the log --------------------------------------------------------------------------- */
static const char *ApLinkReasonName(unsigned r)             /* IEEE 802.11-2016 Table 9-45 */
{
    switch (r) {
    case 1:  return "unspecified";
    case 2:  return "previous authentication no longer valid";
    case 3:  return "the AP is leaving or restarting";
    case 4:  return "inactivity";
    case 5:  return "the AP has too many stations";
    case 6:  return "class 2 frame from a station that is not authenticated";
    case 7:  return "class 3 frame from a station that is not associated";
    case 8:  return "the station is leaving";
    case 9:  return "the station is not authenticated";
    case 14: return "MIC failure";
    case 15: return "4-way handshake timeout";
    case 16: return "group key handshake timeout -- the AP never got a message 2 it accepted";
    case 17: return "handshake element mismatch";
    case 23: return "802.1X authentication failed";
    default: return "(not in this table)";
    }
}

static const char *ApLinkWhyName(int why)
{
    switch (why) {
    case AP_LINK_WHY_BOOT:     return "the boot join did not complete";
    case AP_LINK_WHY_BEACONS:  return "no beacon from our AP for 3 s";
    case AP_LINK_WHY_DEAUTH:   return "the AP deauthenticated us";
    case AP_LINK_WHY_DISASSOC: return "the AP disassociated us";
    case AP_LINK_WHY_OFF:      return "AirPort was turned off";
    default:                   return "(none)";
    }
}

static const char *ApLinkEapolName(int kind)
{
    switch (kind) {
    case AP_LINK_EAPOL_PAIR_M1:  return "pairwise message 1 (the AP restarting the 4-way handshake)";
    case AP_LINK_EAPOL_PAIR_M3:  return "pairwise message 3";
    case AP_LINK_EAPOL_GROUP_M1: return "GROUP KEY message 1 (a group rekey; k208 answers it)";
    case AP_LINK_EAPOL_OTHER:    return "other EAPOL";
    default:                     return "(none)";
    }
}

#endif /* AP_LINK_H */
