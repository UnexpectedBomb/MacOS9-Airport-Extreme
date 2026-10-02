/* ap_view.h -- WHAT THE CONTROL PANEL SAYS, as a pure function of the driver's status block.
 *
 * Every string the panel shows and every show/hide decision is made here, from a copy of the block
 * (probe/ap_status.h) and two facts the panel gathers for itself -- is the card in the machine, and
 * what is the TCP/IP configuration called -- so view_test.c can walk the whole state table on the
 * host. ap_panel.c only draws what this returns; it decides nothing.
 *
 * ★ THE WORDS ARE APPLE'S wherever Apple had words for the case, from the AirPort app 2.0.4's own
 * string lists (UI-PRIOR-ART.md):
 *     STR# 263 #1   "“^0” AirPort network"            the joined line
 *     STR# 263 #14  "Status not available"
 *     STR# 263 #21  "No AirPort card is installed"     (ours says AirPort Extreme: it is not Apple's card
 *                                                      software, and an original AirPort card is not ours)
 *     STR# 8003 #11 "No AirPort Network is selected."
 *     STR# 8003 #12 "TCP/IP is using “^0”"
 * The rest are ours, for states Apple's driver never had: a driver that is loaded but not in use, a
 * driver that stopped answering, and the looking/joining that k204 does by itself.
 *
 * MacRoman, not UTF-8, in every string literal below -- these bytes are drawn by QuickDraw: \322 \323
 * are curly quotes, \311 an ellipsis. (The source file itself is UTF-8, like the rest of the project;
 * only these escapes are MacRoman.)
 *
 * Pure: no Toolbox. view_test.c includes it on the host.
 */
#ifndef AP_VIEW_H
#define AP_VIEW_H

#include <string.h>
#include "ap_status.h"

/* The panel's states, one place. ⚠ Ordered: everything from kUiIdle up has a status block to describe
 * the card with, which is what ApViewMode keys on. */
enum { kUiNoCard = 0, kUiNoDriver, kUiStalled, kUiIdle, kUiUnknown, kUiOff, kUiNoNet, kUiSearching, kUiJoining, kUiUp,
       kUiBadKey /* k213: the password was rejected by the base station */ };

/* The window's three heights: the Status group alone (Apple's no-card window), Settings hidden, and
 * Settings shown. */
enum { kModeStatus = 0, kModeShort, kModeFull };

typedef unsigned char ApVStr[256];      /* a Pascal string */

typedef struct {
    int    ui;
    ApVStr line1, line2;        /* the Status group's two lines */
    ApVStr power;               /* "AirPort: On" */
    ApVStr apId;                /* Apple's "AirPort ID", space-separated hex; empty = not known */
    ApVStr bssid;               /* Apple's "Base Station ID", shown only while joined */
    ApVStr net;                 /* the network popup's one item; empty = the placeholder */
    int    on;                  /* the power button reads "Turn AirPort Off" rather than "...On" */
    int    powerEnabled;        /* k207: the power button works -- a driver running its heartbeat, nothing pending */
    int    live;                /* joined: the "Signal level:" row is shown, as Apple shows it */
    int    meter;               /* 0..100 */
    int    arrows;              /* chasing arrows: looking, joining, or waiting on a command */
} ApView;

/* k207: a power command the panel has posted and the driver has not yet acknowledged. */
enum { kPendNone = 0, kPendOff = 1, kPendOn = 2 };

#define AP_VIEW_FN static __attribute__((unused))

AP_VIEW_FN void ApVSet(unsigned char *s, const char *c)
{
    s[0] = 0;
    while (*c && s[0] < 255) s[++s[0]] = (unsigned char)*c++;
}

AP_VIEW_FN void ApVCat(unsigned char *s, const char *c)
{
    while (*c && s[0] < 255) s[++s[0]] = (unsigned char)*c++;
}

/* An SSID is 0-32 arbitrary bytes. A control character drawn by QuickDraw is a box or nothing at all,
 * and a name that silently loses a character reads as a different network, so those are shown as '?'. */
AP_VIEW_FN void ApVCatSsid(unsigned char *s, const ApStU8 *p, int n)
{
    int i;
    for (i = 0; i < n && s[0] < 255; i++)
        s[++s[0]] = (p[i] < 0x20 || p[i] == 0x7F) ? (unsigned char)'?' : p[i];
}

AP_VIEW_FN int ApVMacZero(const ApStU8 *m)
{
    int i;
    for (i = 0; i < 6; i++) if (m[i]) return 0;
    return 1;
}

/* "02 00 00 12 34 D5" -- Apple's form for an AirPort ID: uppercase pairs separated by spaces, never
 * colons (the user's screenshot of Apple's window). */
AP_VIEW_FN void ApVHexMac(unsigned char *s, const ApStU8 *m)
{
    static const char kHex[] = "0123456789ABCDEF";
    int i;
    s[0] = 0;
    for (i = 0; i < 6; i++) {
        if (i) s[++s[0]] = ' ';
        s[++s[0]] = (unsigned char)kHex[m[i] >> 4];
        s[++s[0]] = (unsigned char)kHex[m[i] & 15];
    }
}

/* Which state the panel is in.
 *   haveBlock  Gestalt 'APXe' answered with a block ApStatValid accepts
 *   card       1 = a BCM4306 is in the Name Registry, 0 = the walk worked and found none,
 *              -1 = unknown: the walk saw no PCI node at all, which proves nothing about the card
 *              ([[reference_os9_nameregistry_iterate]]: a broken walk reads exactly like absent hardware)
 *   stalled    alive, but the heartbeat has not moved for the panel's stall window
 * ⚠ alive == 0 is checked BEFORE stalled: a module Open Transport has closed stops its heartbeat on
 * purpose (ApLinkStop), and "not responding" would accuse a driver that is simply not in use. */
AP_VIEW_FN int ApViewUi(int haveBlock, const ApStatBlock *b, int card, int stalled)
{
    if (!haveBlock || b == 0) return (card == 0) ? kUiNoCard : kUiNoDriver;
    if (!b->alive) return kUiIdle;
    if (stalled) return kUiStalled;
    switch (b->linkState) {
    case kApLinkOff:       return kUiOff;
    case kApLinkIdle:      return kUiNoNet;     /* k211: running, radio on, no network chosen yet */
    case kApLinkSearching: return kUiSearching;
    case kApLinkJoining:   return kUiJoining;
    case kApLinkUp:        return kUiUp;
    case kApLinkBadKey:    return kUiBadKey;     /* k213: the base station rejected our password */
    default:               return kUiUnknown;   /* a state this panel predates: say so, guess nothing */
    }
}

/* Apple's no-card window is the Status group alone. Ours is too whenever there is no block to describe
 * the card with; from kUiIdle up the block carries the AirPort ID, so Settings has something to show. */
AP_VIEW_FN int ApViewMode(int ui, int expanded)
{
    if (ui < kUiStalled) return kModeStatus;
    return expanded ? kModeFull : kModeShort;
}

/* Fill *v for state ui. b may be NULL (no block); tcpName is a Pascal string or NULL (Network Setup
 * missing or unreadable -- the line is then left empty rather than guessed). */
AP_VIEW_FN void ApViewBuild(ApView *v, int ui, const ApStatBlock *b, const unsigned char *tcpName)
{
    int n = 0;

    memset(v, 0, sizeof(*v));                   /* ⚠ whole struct: the panel memcmp()s views to find changes */
    v->ui = ui;
    if (b != 0) n = (b->ssidLen > 32) ? 32 : b->ssidLen;

    switch (ui) {
    case kUiNoCard:
        ApVSet(v->line1, "No AirPort Extreme card is installed");
        ApVSet(v->line2, "Status not available");
        break;
    case kUiNoDriver:
        ApVSet(v->line1, "The AirPort Extreme driver is not running");
        ApVSet(v->line2, "Status not available");
        break;
    case kUiStalled:
        ApVSet(v->line1, "The AirPort Extreme driver is not responding");
        ApVSet(v->line2, "Status not available");
        break;
    default:
        switch (ui) {
        case kUiIdle:
            ApVSet(v->line1, "AirPort Extreme is not in use");
            break;
        case kUiOff:
            ApVSet(v->line1, "AirPort is turned off");
            break;
        case kUiNoNet:
            ApVSet(v->line1, "No AirPort Network is selected.");   /* Apple's STR# 8003 #11, verbatim */
            break;
        case kUiSearching:
            if (n) { ApVSet(v->line1, "Looking for \322"); ApVCatSsid(v->line1, b->ssid, n); ApVCat(v->line1, "\323\311"); }
            else     ApVSet(v->line1, "Looking for AirPort networks\311");
            break;
        case kUiJoining:
            if (n) { ApVSet(v->line1, "Joining \322"); ApVCatSsid(v->line1, b->ssid, n); ApVCat(v->line1, "\323\311"); }
            else     ApVSet(v->line1, "Joining an AirPort network\311");
            break;
        case kUiUp:
            if (n) { ApVSet(v->line1, "\322"); ApVCatSsid(v->line1, b->ssid, n); ApVCat(v->line1, "\323 AirPort network"); }
            else     ApVSet(v->line1, "No AirPort Network is selected.");
            break;
        case kUiBadKey:         /* k213: a wrong password, told plainly -- and no chasing arrows (below) */
            if (n) { ApVSet(v->line1, "The password for \322"); ApVCatSsid(v->line1, b->ssid, n); ApVCat(v->line1, "\323 is incorrect"); }
            else     ApVSet(v->line1, "The AirPort password is incorrect");
            break;
        default:
            ApVSet(v->line1, "Status not available");
            break;
        }
        if (tcpName != 0 && tcpName[0] != 0) {
            ApVSet(v->line2, "TCP/IP is using \322");
            ApVCatSsid(v->line2, tcpName + 1, tcpName[0]);
            ApVCat(v->line2, "\323");
        }
        break;
    }

    /* The AirPort group. ⚠ "Off" only for a driver that SAYS it is off: every other state leaves the
     * radio as it was, and the button must not offer to turn on a radio that is on. It works only when a
     * driver is there to carry the command out: running its heartbeat, so not idle (its module closed),
     * not stalled, and of course not absent. */
    v->on = (ui != kUiOff);
    v->powerEnabled = (ui == kUiOff || ui == kUiNoNet || ui == kUiSearching || ui == kUiJoining || ui == kUiUp
                       || ui == kUiBadKey || ui == kUiUnknown);
    ApVSet(v->power, ui == kUiOff     ? "AirPort: Off"
                   : ui == kUiIdle    ? "AirPort: Not in use"
                   : ui == kUiStalled ? "AirPort: Not responding"
                   :                    "AirPort: On");
    if (b != 0 && ui >= kUiStalled && !ApVMacZero(b->airportId)) ApVHexMac(v->apId, b->airportId);

    /* The network group: the base station and the signal only while joined, as Apple shows them; the
     * network name whenever the driver is working toward it. */
    v->live = (ui == kUiUp);
    if (v->live && b != 0 && !ApVMacZero(b->bssid)) ApVHexMac(v->bssid, b->bssid);
    if ((ui == kUiSearching || ui == kUiJoining || ui == kUiUp) && n) ApVCatSsid(v->net, b->ssid, n);
    v->meter  = (v->live && b != 0) ? (int)ApStatPercent(b->signalDbm) : 0;
    v->arrows = (ui == kUiSearching || ui == kUiJoining);
}

/* k207: while a power command waits for the driver, say so in Apple's words (the AirPort app's STR# 1000:
 * #10 "Waiting for AirPort to turn off…", #9 "... on…"), turn the arrows, and keep the button from being
 * pressed twice. Applied after ApViewBuild; nothing when pending is kPendNone. */
AP_VIEW_FN void ApViewPending(ApView *v, int pending)
{
    if (pending == kPendNone) return;
    ApVSet(v->line1, (pending == kPendOff) ? "Waiting for AirPort to turn off\311" : "Waiting for AirPort to turn on\311");
    v->arrows = 1;
    v->powerEnabled = 0;
}

#endif /* AP_VIEW_H */
