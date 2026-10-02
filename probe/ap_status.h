/* ap_status.h -- the AirPort Extreme STATUS BLOCK (k205): how the driver tells its control panel and its
 * Control Strip module what it is doing. ONE header, included by all three binaries, so they cannot
 * disagree about a field; the layout is pinned by _Static_asserts on every build (module_info, k189,
 * is this project's reminder that a layout mismatch is silent until it is a crash).
 *
 * ★ HOW THE UI FINDS IT. The driver allocates the block once, in the SYSTEM heap, and publishes its
 * address as a Gestalt value (kApStatSelector). The panel and the strip module call Gestalt, check
 * magic + version + size, and read. Nothing else is shared: no CFM connection into the driver, no
 * FindSymbol. The Bluetooth panel reached its driver through FindSymbol and spent several builds on
 * "found the wrong copy" and a pointer that dangled once the connection closed; Open Transport loads
 * this module on its own, apart from any application, so the same traps would apply here.
 *
 * ★ WHO WRITES WHAT.
 *   the driver, task level     identity (linkState, SSID, BSSID, channel) under the seq word: odd
 *                              while it rewrites them, even when they are consistent; and the
 *                              command acknowledgement (ackResult, then ackSeq)
 *   the driver, interrupt      single words only -- signalDbm, quality, heartbeat, txRateKbps. One
 *                              aligned 32-bit store is atomic on the G4; no seq needed or taken
 *   the UI                     reads everything; writes ONLY the command words (cmd, cmdArg, then cmdSeq)
 * ⇒ A reader copies the identity with ApStatRead (retry while seq is odd or moved) and takes the
 *   single words as they are.
 *
 * ★ COMMANDS (version 2, k207): the UI posts with ApStatPost -- command and argument, then a new sequence
 * number -- and waits for ackSeq to reach it. The heartbeat notices cmdSeq != ackSeq and wakes the task
 * pump, which takes the NEWEST command (one posted before the last was taken supersedes it: off-then-on
 * in quick succession simply ends on), acts, writes the result and then the sequence number. Every UI
 * and the driver's task body run at task level under cooperative scheduling, so two posts can never
 * interleave; the barriers keep each writer's own order.
 *
 * ⚠ The block OUTLIVES the driver: it is never freed, so a reload of the module finds and reuses it,
 *   and a reader never touches freed memory. alive = 0 and a heartbeat that stops advancing are how a
 *   reader knows the driver is gone or stalled.
 * ⚠ NOTHING SECRET GOES HERE. No PSK, no PMK, no key of any kind -- any process can read the block.
 *
 * Pure: no Toolbox, no MMIO. status_test.c includes it on the host.
 */
#ifndef AP_STATUS_H
#define AP_STATUS_H

typedef unsigned int  ApStU32;               /* 32 bits on the G4 and the 64-bit host alike */
typedef int           ApStS32;
typedef unsigned char ApStU8;
typedef char ApStU32IsFourBytes[(sizeof(ApStU32) == 4) ? 1 : -1];
/* Each binary uses a different subset of the helpers below (the driver converts RSSI, the UIs only
 * read); without this every build warns about the ones it does not call. */
#define AP_STAT_FN static __attribute__((unused))

#define kApStatSelector  0x41505865UL        /* Gestalt 'APXe' -> the ApStatBlock's address */
#define kApStatMagic     0x41505842UL        /* 'APXB' */
#define kApStatVersion   2u                  /* bump on ANY layout change; readers refuse others.
                                              * 2 (k207): the command mailbox */

/* ⚠ Append only: these are a wire contract across the driver, the strip module and the panel. k211 adds
 * kApLinkIdle -- the radio is on and the driver is running, but no network has been chosen yet (an empty
 * known-networks file). It is NOT kApLinkSearching: with no target there is nothing to look for, so the UI
 * must say "no network selected", not spin forever (the k210 "scanned continually" report). No layout
 * change (linkState is already a word), so kApStatVersion stays 2; an un-updated reader falls to its
 * default, which is why strip 1.5 and the panel below both learn this value in the same build.
 *
 * k213 adds kApLinkBadKey -- the password is wrong. A wrong WPA2 passphrase cannot be told from a right one
 * until the 4-way handshake: authentication and association succeed either way (they do not use the key), and
 * only message 3 -- the AP's proof that its key matches ours -- is withheld when the key is wrong. So the
 * driver latches this ONLY after it has reached message 2 and seen no message 3 on two attempts in a row
 * (ap_shim.c gShimKeyBad); the UI then says "incorrect password" and stops the spinner, instead of "Looking
 * for ..." forever. Still no layout change; an un-updated reader falls to kUiUnknown ("a state this predates"). */
enum { kApLinkNoDriver = 0, kApLinkOff = 1, kApLinkSearching = 2, kApLinkJoining = 3, kApLinkUp = 4,
       kApLinkIdle = 5, kApLinkBadKey = 6 };

/* Commands (cmd) and their argument (cmdArg). */
enum { kApCmdNone = 0, kApCmdPower = 1, /* arg: 0 = turn AirPort off, 1 = on */
       kApCmdScan = 2, /* k209: rebuild the network list (ap_scanlist.h); arg unused */
       kApCmdJoin = 3 /* k210: join the network the panel just wrote to the known-networks file; arg unused */ };
/* Results (ackResult). */
enum { kApAckNone = 0, kApAckDone = 1, kApAckUnknown = 2, kApAckFailed = 3 };

/* Apple's five levels, in the order of the AirPort app's own STR# 8007 ("Strong" ... "Out of Range")
 * and the strip module's STR# 257, but numbered so that a bigger number is a better signal. */
enum { kApQualOutOfRange = 0, kApQualWeak = 1, kApQualAverage = 2, kApQualGood = 3, kApQualStrong = 4 };

typedef struct {
    ApStU32 magic;              /*   0  kApStatMagic */
    ApStU32 version;            /*   4  kApStatVersion */
    ApStU32 size;               /*   8  sizeof(ApStatBlock) */
    ApStU32 driverBuild;        /*  12  AP_OTMODL_BUILD */
    ApStU32 alive;              /*  16  1 while the module is loaded */
    ApStU32 heartbeat;          /*  20  the link clock: +4 a second while the driver runs */
    ApStU32 seq;                /*  24  even = the identity below is consistent */
    ApStU32 linkState;          /*  28  kApLink* */
    ApStU32 linkWhy;            /*  32  AP_LINK_WHY_* of the latest outage (ap_link.h), 0 = none */
    ApStU32 channel;            /*  36 */
    ApStU8  airportId[6];       /*  40  this card's MAC -- Apple's "AirPort ID" */
    ApStU8  bssid[6];           /*  46  the base station's -- Apple's "Base Station ID" */
    ApStU8  ssidLen;            /*  52 */
    ApStU8  ssid[32];           /*  53 */
    ApStU8  pad0[3];            /*  85 */
    ApStS32 signalDbm;          /*  88  smoothed RSSI of our AP's beacons; 0 = no reading */
    ApStU32 quality;            /*  92  kApQual* -- what the strip draws */
    ApStU32 txRateKbps;         /*  96  the data rate in use */
    ApStU32 upSince;            /* 100  heartbeat when the current association began */
    ApStU32 outages;            /* 104  link-layer totals (ap_link.h) */
    ApStU32 rejoins;            /* 108 */
    ApStU32 cmdSeq;             /* 112  the UI: bumped (never to 0) AFTER cmd and cmdArg are written */
    ApStU32 cmd;                /* 116  kApCmd* */
    ApStU32 cmdArg;             /* 120 */
    ApStU32 ackSeq;             /* 124  the driver: the cmdSeq it has finished, written AFTER ackResult */
    ApStU32 ackResult;          /* 128  kApAck*, for that command */
    ApStU32 reserved[15];       /* 132  the scan list's companion fields, in later versions */
} ApStatBlock;                  /* 192 */

_Static_assert(__builtin_offsetof(ApStatBlock, heartbeat) == 20,  "ApStatBlock.heartbeat moved");
_Static_assert(__builtin_offsetof(ApStatBlock, airportId) == 40,  "ApStatBlock.airportId moved");
_Static_assert(__builtin_offsetof(ApStatBlock, ssid)      == 53,  "ApStatBlock.ssid moved");
_Static_assert(__builtin_offsetof(ApStatBlock, signalDbm) == 88,  "ApStatBlock.signalDbm moved");
_Static_assert(__builtin_offsetof(ApStatBlock, cmdSeq)    == 112, "ApStatBlock.cmdSeq moved");
_Static_assert(__builtin_offsetof(ApStatBlock, ackResult) == 128, "ApStatBlock.ackResult moved");
_Static_assert(__builtin_offsetof(ApStatBlock, reserved)  == 132, "ApStatBlock.reserved moved");
_Static_assert(sizeof(ApStatBlock) == 192,                        "ApStatBlock size changed");

/* Is this the block this build understands? The first thing every reader does. */
AP_STAT_FN int ApStatValid(const ApStatBlock *b)
{
    return b && b->magic == kApStatMagic && b->version == kApStatVersion && b->size == sizeof(ApStatBlock);
}

/* The identity fields, consistently: retry while the writer is inside (seq odd) or finished a rewrite
 * while we copied (seq moved). Bounded, so a writer that died mid-rewrite cannot hang the reader; on
 * giving up the copy is whatever was last read. Returns 1 if the copy is known-consistent.
 * AP_STAT_READ_HOOK is a TEST SEAM between the copy and the second seq read: empty in every real build,
 * defined by status_test.c to play a writer that completes a rewrite mid-copy -- the one case a
 * single-threaded host test otherwise never reaches. */
#ifndef AP_STAT_READ_HOOK
#define AP_STAT_READ_HOOK(b) ((void)0)
#endif
AP_STAT_FN int ApStatRead(const volatile ApStatBlock *b, ApStatBlock *out)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        ApStU32 s0 = b->seq, s1;
        const volatile ApStU8 *src = (const volatile ApStU8 *)b;
        ApStU8 *dst = (ApStU8 *)out;
        unsigned k;
        if (s0 & 1u) continue;
        for (k = 0; k < sizeof(ApStatBlock); k++) dst[k] = src[k];
        AP_STAT_READ_HOOK(b);
        s1 = b->seq;
        if (s1 == s0) return 1;
    }
    return 0;
}

/* ---- the command mailbox (version 2) ------------------------------------------------------------------
 * Each writer's order is kept by a barrier: the G4 is weakly ordered, and gcc may move plain stores. */
#if defined(__powerpc__) || defined(__POWERPC__) || defined(__ppc__)
#define AP_STAT_BARRIER() __asm__ __volatile__("sync" ::: "memory")
#else
#define AP_STAT_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

/* The UI: post a command, return its sequence number (never 0) to wait on with ApStatAcked. */
AP_STAT_FN ApStU32 ApStatPost(volatile ApStatBlock *b, ApStU32 cmd, ApStU32 arg)
{
    ApStU32 seq = b->cmdSeq + 1u;
    if (seq == 0u) seq = 1u;                             /* 0 is "no command yet" in a fresh block */
    b->cmd = cmd;
    b->cmdArg = arg;
    AP_STAT_BARRIER();                                   /* the command, THEN the number that posts it */
    b->cmdSeq = seq;
    return seq;
}

/* A command is waiting (or being carried out). */
AP_STAT_FN int ApStatBusy(const volatile ApStatBlock *b) { return b->cmdSeq != b->ackSeq; }

/* The command numbered seq has been carried out (or superseded by a newer one that has). */
AP_STAT_FN int ApStatAcked(const volatile ApStatBlock *b, ApStU32 seq)
{
    return b->ackSeq == seq || (b->cmdSeq != seq && b->ackSeq == b->cmdSeq);
}

/* The driver: take the newest command, if one is waiting. 1 = took one. */
AP_STAT_FN int ApStatTake(const volatile ApStatBlock *b, ApStU32 *seq, ApStU32 *cmd, ApStU32 *arg)
{
    ApStU32 s = b->cmdSeq;
    if (s == b->ackSeq) return 0;
    AP_STAT_BARRIER();                                   /* the number, THEN the command it covers */
    *cmd = b->cmd;
    *arg = b->cmdArg;
    *seq = s;
    return 1;
}

/* The driver: publish the result of command seq. */
AP_STAT_FN void ApStatAck(volatile ApStatBlock *b, ApStU32 seq, ApStU32 result)
{
    b->ackResult = result;
    AP_STAT_BARRIER();                                   /* the result, THEN the number that publishes it */
    b->ackSeq = seq;
}

/* b43_rssi_postprocess (xmit.c), the radio 0x2050 arm, verbatim: this card is a BCM4306 with a 2050
 * radio and SPROM boardflags 0x000A, which has B43_BFL_RSSI (0x0008) set, so a CCK frame goes through
 * the NRSSI table the PHY bring-up computed (b43_nrssi_mem_update -> gNrssiLt). b43_rx passes
 * ofdm = phy_status0 & B43_RX_PHYST0_OFDM (0x0001) and adjust_2050 = phy_status3 & B43_RX_PHYST3_TRSTATE
 * (0x0400); adjust_2053 is unused for a 2050. Returns dBm, as b43's (s8). */
AP_STAT_FN ApStS32 ApRssiDbm(ApStU8 jssi, unsigned phyStatus0, unsigned phyStatus3, const short *nrssiLt)
{
    ApStS32 tmp;
    int adjust2050 = (phyStatus3 & 0x0400u) != 0;
    if (phyStatus0 & 0x0001u) {                          /* OFDM */
        tmp = jssi;
        if (tmp > 127) tmp -= 256;
        tmp *= 73;
        tmp /= 64;
        tmp += adjust2050 ? 25 : -3;
    } else {                                             /* CCK, boardflags RSSI set */
        unsigned in = (jssi > 63) ? 63u : jssi;
        tmp = nrssiLt ? nrssiLt[in] : (ApStS32)in;
        tmp = 31 - tmp;
        tmp *= -131;
        tmp /= 128;
        tmp -= 57;
        if (adjust2050) tmp += 25;                       /* phy->type == G on this card */
    }
    return (ApStS32)(signed char)tmp;
}

/* dBm -> Apple's five levels, with 2 dB of hysteresis so a beacon-to-beacon wobble at a boundary does
 * not make the strip redraw back and forth. The bounds are ours (Apple's are not in any resource we
 * have): Strong >= -60, Good >= -67, Average >= -75, Weak >= -85, else Out of Range. prev is the level
 * currently shown; a new level needs to clear its boundary by the margin in the direction of travel. */
#define AP_QUAL_HYST_DB 2
AP_STAT_FN ApStU32 ApStatQuality(ApStS32 dbm, ApStU32 prev)
{
    static const ApStS32 kFloor[5] = { -1000, -85, -75, -67, -60 };   /* the lowest dBm of each level */
    ApStU32 lvl = 0, k;
    for (k = 1; k < 5; k++) if (dbm >= kFloor[k]) lvl = k;
    if (prev > 4) return lvl;
    if (lvl > prev) {                                    /* going up: must clear each floor by the margin */
        while (lvl > prev && dbm < kFloor[lvl] + AP_QUAL_HYST_DB) lvl--;
    } else if (lvl < prev) {                             /* going down: must fall the margin below prev's floor */
        if (dbm >= kFloor[prev] - AP_QUAL_HYST_DB) lvl = prev;
    }
    return lvl;
}

/* For a meter (the app's "Signal level:" bar): dBm -> 0..100, linear from -95 (0) to -35 (100). */
AP_STAT_FN ApStU32 ApStatPercent(ApStS32 dbm)
{
    ApStS32 p;
    if (dbm == 0) return 0;
    p = (dbm + 95) * 100 / 60;
    return (ApStU32)(p < 0 ? 0 : p > 100 ? 100 : p);
}

#endif /* AP_STATUS_H */
