/* view_test.c -- host test for ap_view.h, the control panel's whole state table.
 *
 *   cc -std=c99 -Wall -I../probe -o view_test view_test.c && ./view_test
 *
 * Every string the panel can show, every show/hide decision, and the two traps the header names:
 * a closed module must read "not in use", never "not responding"; and a registry walk that saw
 * nothing must not become "no card".
 */
#include <stdio.h>
#include <string.h>
#include "ap_view.h"

static int gFail, gPass;

static void Check(int ok, const char *what)
{
    if (ok) gPass++;
    else { gFail++; printf("FAIL: %s\n", what); }
}

/* Pascal string equals C string (the C string may carry MacRoman escapes). */
static int PEq(const unsigned char *p, const char *c)
{
    size_t n = strlen(c);
    return p[0] == n && memcmp(p + 1, c, n) == 0;
}

static void CheckStr(const unsigned char *p, const char *want, const char *what)
{
    if (!PEq(p, want)) printf("  got \"%.*s\" want \"%s\"\n", p[0], (const char *)(p + 1), want);
    Check(PEq(p, want), what);
}

static ApStatBlock Block(ApStU32 state, const char *ssid)
{
    static const ApStU8 kId[6]  = { 0x02, 0x00, 0x00, 0x12, 0x34, 0xD5 };   /* made up (locally administered) */
    static const ApStU8 kBss[6] = { 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x0F };
    ApStatBlock b;
    memset(&b, 0, sizeof(b));
    b.magic = kApStatMagic; b.version = kApStatVersion; b.size = sizeof(b);
    b.alive = 1; b.heartbeat = 100; b.linkState = state; b.channel = 1;
    memcpy(b.airportId, kId, 6);
    memcpy(b.bssid, kBss, 6);
    b.ssidLen = (ApStU8)strlen(ssid);
    memcpy(b.ssid, ssid, b.ssidLen);
    b.signalDbm = -55;
    b.quality = kApQualStrong;
    return b;
}

static const unsigned char kTcp[] = "\007Default";

int main(void)
{
    ApStatBlock b;
    ApView v, w;

    /* ---- ApViewUi: which state ---------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    Check(ApViewUi(0, NULL, 0, 0)  == kUiNoCard,   "no block + walk found no card = no card");
    Check(ApViewUi(0, NULL, 1, 0)  == kUiNoDriver, "no block + card present = driver not running");
    Check(ApViewUi(0, NULL, -1, 0) == kUiNoDriver, "no block + walk saw nothing = NOT 'no card'");
    Check(ApViewUi(0, &b, 0, 0)    == kUiNoCard,   "haveBlock=0 wins over a stale pointer");
    b.alive = 0;
    Check(ApViewUi(1, &b, 1, 0) == kUiIdle,        "alive=0 = not in use");
    Check(ApViewUi(1, &b, 1, 1) == kUiIdle,        "alive=0 + frozen heartbeat = still 'not in use', not 'not responding'");
    b.alive = 1;
    Check(ApViewUi(1, &b, 1, 1) == kUiStalled,     "alive + frozen heartbeat = not responding");
    Check(ApViewUi(1, &b, 1, 0) == kUiUp,          "linkState Up");
    b.linkState = kApLinkOff;       Check(ApViewUi(1, &b, 1, 0) == kUiOff,       "linkState Off");
    b.linkState = kApLinkSearching; Check(ApViewUi(1, &b, 1, 0) == kUiSearching, "linkState Searching");
    b.linkState = kApLinkJoining;   Check(ApViewUi(1, &b, 1, 0) == kUiJoining,   "linkState Joining");
    b.linkState = kApLinkIdle;      Check(ApViewUi(1, &b, 1, 0) == kUiNoNet,     "k211: linkState Idle = no network selected");
    b.linkState = kApLinkBadKey;    Check(ApViewUi(1, &b, 1, 0) == kUiBadKey,    "k213: linkState BadKey = wrong password");
    b.linkState = kApLinkNoDriver;  Check(ApViewUi(1, &b, 1, 0) == kUiUnknown,   "linkState NoDriver while alive = unknown");
    b.linkState = 99;               Check(ApViewUi(1, &b, 1, 0) == kUiUnknown,   "a future state = unknown");
    b.linkState = kApLinkUp;
    Check(ApViewUi(1, &b, 0, 0) == kUiUp, "a block wins over a registry walk that missed the card");

    /* ---- ApViewMode: which height ------------------------------------------------------------- */
    Check(ApViewMode(kUiNoCard, 1)    == kModeStatus, "no card: Status group only, even expanded");
    Check(ApViewMode(kUiNoDriver, 1)  == kModeStatus, "no driver: Status group only");
    Check(ApViewMode(kUiStalled, 1)   == kModeFull,   "stalled: Settings available (the block has the ID)");
    Check(ApViewMode(kUiIdle, 1)      == kModeFull,   "idle expanded = full");
    Check(ApViewMode(kUiIdle, 0)      == kModeShort,  "idle collapsed = short");
    Check(ApViewMode(kUiUp, 1)        == kModeFull,   "up expanded = full");
    Check(ApViewMode(kUiUp, 0)        == kModeShort,  "up collapsed = short");
    Check(ApViewMode(kUiNoNet, 1)     == kModeFull,   "k211: no-network expanded = full (the block has the ID)");
    Check(ApViewMode(kUiNoNet, 0)     == kModeShort,  "k211: no-network collapsed = short");
    Check(ApViewMode(kUiBadKey, 1)    == kModeFull,   "k213: bad-key expanded = full");
    Check(ApViewMode(kUiBadKey, 0)    == kModeShort,  "k213: bad-key collapsed = short");
    Check(ApViewMode(kUiUnknown, 0)   == kModeShort,  "unknown collapsed = short");

    /* ---- joined ------------------------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    ApViewBuild(&v, kUiUp, &b, kTcp);
    CheckStr(v.line1, "\322HomeNet\323 AirPort network", "up: Apple's STR# 263 #1");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "up: Apple's STR# 8003 #12");
    CheckStr(v.power, "AirPort: On", "up: power");
    CheckStr(v.apId,  "02 00 00 12 34 D5", "up: AirPort ID in Apple's form");
    CheckStr(v.bssid, "AA BB CC 01 02 0F", "up: base station ID, uppercase");
    CheckStr(v.net,   "HomeNet", "up: popup item");
    Check(v.on == 1 && v.live == 1 && v.arrows == 0, "up: on, live, no arrows");
    Check(v.meter == (int)ApStatPercent(-55) && v.meter == 66, "up: meter from signalDbm (-55 dBm = 66)");

    b.ssidLen = 0;
    ApViewBuild(&v, kUiUp, &b, kTcp);
    CheckStr(v.line1, "No AirPort Network is selected.", "up with no SSID: Apple's STR# 8003 #11");
    Check(v.net[0] == 0, "up with no SSID: popup placeholder");

    /* ---- looking and joining ------------------------------------------------------------------ */
    b = Block(kApLinkSearching, "HomeNet");
    ApViewBuild(&v, kUiSearching, &b, kTcp);
    CheckStr(v.line1, "Looking for \322HomeNet\323\311", "searching: line 1");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "searching: TCP line kept");
    Check(v.arrows == 1 && v.live == 0 && v.meter == 0, "searching: arrows, no signal row");
    Check(v.bssid[0] == 0, "searching: no base station ID (Apple shows it only when joined)");
    CheckStr(v.net, "HomeNet", "searching: popup shows the target network");
    CheckStr(v.power, "AirPort: On", "searching: power on");
    b.ssidLen = 0;
    ApViewBuild(&v, kUiSearching, &b, kTcp);
    CheckStr(v.line1, "Looking for AirPort networks\311", "searching with no SSID");

    b = Block(kApLinkJoining, "HomeNet");
    ApViewBuild(&v, kUiJoining, &b, kTcp);
    CheckStr(v.line1, "Joining \322HomeNet\323\311", "joining: line 1");
    Check(v.arrows == 1 && v.live == 0, "joining: arrows, no signal row");
    b.ssidLen = 0;
    ApViewBuild(&v, kUiJoining, &b, kTcp);
    CheckStr(v.line1, "Joining an AirPort network\311", "joining with no SSID");

    /* ---- off, idle, stalled, unknown ---------------------------------------------------------- */
    b = Block(kApLinkOff, "HomeNet");
    ApViewBuild(&v, kUiOff, &b, kTcp);
    CheckStr(v.line1, "AirPort is turned off", "off: line 1");
    CheckStr(v.power, "AirPort: Off", "off: power");
    Check(v.on == 0 && v.net[0] == 0 && v.arrows == 0 && v.live == 0, "off: button offers On, no network, no arrows");
    CheckStr(v.apId, "02 00 00 12 34 D5", "off: the card's ID is still known");

    b = Block(kApLinkNoDriver, "HomeNet"); b.alive = 0;
    ApViewBuild(&v, kUiIdle, &b, kTcp);
    CheckStr(v.line1, "AirPort Extreme is not in use", "idle: line 1");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "idle: TCP line says which configuration IS in use");
    CheckStr(v.power, "AirPort: Not in use", "idle: power");
    Check(v.on == 1, "idle: the button does not offer to turn on a radio nobody turned off");
    CheckStr(v.apId, "02 00 00 12 34 D5", "idle: ID shown");
    Check(v.net[0] == 0 && v.bssid[0] == 0 && v.live == 0, "idle: no network, no base station");

    /* k211: the radio is on and the driver running, but no network has been chosen (empty known-networks
     * file). The whole point: NO chasing arrows -- say "no network selected", do not spin forever. */
    b = Block(kApLinkIdle, "HomeNet");            /* Block() sets an SSID; the no-net state must ignore it */
    ApViewBuild(&v, kUiNoNet, &b, kTcp);
    CheckStr(v.line1, "No AirPort Network is selected.", "no-net: Apple's STR# 8003 #11");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "no-net: TCP line says the configuration in use");
    CheckStr(v.power, "AirPort: On", "no-net: the radio is on");
    Check(v.on == 1, "no-net: the button offers to turn AirPort off");
    Check(v.powerEnabled == 1, "no-net: the power button works (a driver is there to carry it out)");
    Check(v.arrows == 0, "no-net: NO chasing arrows -- the k210 'scanned continually' fix");
    Check(v.live == 0 && v.meter == 0 && v.bssid[0] == 0, "no-net: no signal row, no base station");
    Check(v.net[0] == 0, "no-net: popup shows its placeholder, not a stale target name");
    CheckStr(v.apId, "02 00 00 12 34 D5", "no-net: the card's ID is known");

    /* k213: the base station rejected the password. Like no-net, the point is NO chasing arrows -- say the
     * password is wrong and stop, rather than spin on "Looking for ..." forever. */
    b = Block(kApLinkBadKey, "HomeNet");
    ApViewBuild(&v, kUiBadKey, &b, kTcp);
    CheckStr(v.line1, "The password for \322HomeNet\323 is incorrect", "k213 bad-key: line 1 names the network");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "bad-key: TCP line kept");
    CheckStr(v.power, "AirPort: On", "bad-key: the radio is on");
    Check(v.on == 1, "bad-key: the button offers to turn AirPort off");
    Check(v.powerEnabled == 1, "bad-key: the power button works");
    Check(v.arrows == 0, "bad-key: NO chasing arrows -- a verdict, not a search");
    Check(v.live == 0 && v.meter == 0 && v.bssid[0] == 0, "bad-key: no signal row, no base station");
    b.ssidLen = 0;
    ApViewBuild(&v, kUiBadKey, &b, kTcp);
    CheckStr(v.line1, "The AirPort password is incorrect", "bad-key with no SSID");

    b = Block(kApLinkUp, "HomeNet");
    ApViewBuild(&v, kUiStalled, &b, kTcp);
    CheckStr(v.line1, "The AirPort Extreme driver is not responding", "stalled: line 1");
    CheckStr(v.line2, "Status not available", "stalled: line 2 is Apple's, not a TCP line");
    CheckStr(v.power, "AirPort: Not responding", "stalled: power");
    CheckStr(v.apId, "02 00 00 12 34 D5", "stalled: ID shown");
    Check(v.live == 0 && v.meter == 0 && v.bssid[0] == 0 && v.net[0] == 0,
          "stalled: a frozen block's signal and base station are NOT shown as current");

    ApViewBuild(&v, kUiUnknown, &b, kTcp);
    CheckStr(v.line1, "Status not available", "unknown: says so");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "unknown: TCP line kept");

    /* ---- no block --------------------------------------------------------------------------- */
    ApViewBuild(&v, kUiNoCard, NULL, kTcp);
    CheckStr(v.line1, "No AirPort Extreme card is installed", "no card: line 1");
    CheckStr(v.line2, "Status not available", "no card: Apple's STR# 263 #14");
    Check(v.apId[0] == 0 && v.net[0] == 0 && v.live == 0 && v.arrows == 0, "no card: nothing else");
    ApViewBuild(&v, kUiNoCard, &b, kTcp);
    Check(v.apId[0] == 0, "no card: a stray block's ID is not shown");
    ApViewBuild(&v, kUiNoDriver, NULL, kTcp);
    CheckStr(v.line1, "The AirPort Extreme driver is not running", "no driver: line 1");
    CheckStr(v.line2, "Status not available", "no driver: line 2");

    /* ---- the TCP/IP line ---------------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    ApViewBuild(&v, kUiUp, &b, NULL);
    Check(v.line2[0] == 0, "no Network Setup: line 2 empty, not guessed");
    ApViewBuild(&v, kUiUp, &b, (const unsigned char *)"\000");
    Check(v.line2[0] == 0, "empty configuration name: line 2 empty");

    /* ---- SSID edge cases ---------------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    b.ssidLen = 40;                                  /* corrupt length: clamp to the field */
    memset(b.ssid, 'x', 32);
    ApViewBuild(&v, kUiUp, &b, kTcp);
    Check(v.net[0] == 32, "SSID length clamped to 32");
    b = Block(kApLinkUp, "a\tb\177c");
    ApViewBuild(&v, kUiUp, &b, kTcp);
    CheckStr(v.net, "a?b?c", "control characters in an SSID shown as '?'");

    /* ---- IDs ---------------------------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    memset(b.airportId, 0, 6);
    memset(b.bssid, 0, 6);
    ApViewBuild(&v, kUiUp, &b, kTcp);
    Check(v.apId[0] == 0 && v.bssid[0] == 0, "all-zero IDs = not known, shown empty");
    { static const ApStU8 m[6] = { 0xFF, 0x0A, 0xB0, 0x00, 0x9C, 0xE1 };
      unsigned char s[256];
      ApVHexMac(s, m);
      CheckStr(s, "FF 0A B0 00 9C E1", "hex: every nibble, uppercase, zero-padded"); }

    /* ---- the meter ---------------------------------------------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    b.signalDbm = -20;  ApViewBuild(&v, kUiUp, &b, kTcp); Check(v.meter == 100, "meter clamps high");
    b.signalDbm = -100; ApViewBuild(&v, kUiUp, &b, kTcp); Check(v.meter == 0,   "meter clamps low");
    b.signalDbm = 0;    ApViewBuild(&v, kUiUp, &b, kTcp); Check(v.meter == 0,   "no reading yet = 0");

    /* ---- k207: the power button -------------------------------------------------------------- */
    {   static const struct { int ui; int enabled; const char *name; } kB[] = {
            { kUiOff, 1, "off" }, { kUiSearching, 1, "searching" }, { kUiJoining, 1, "joining" },
            { kUiUp, 1, "up" }, { kUiBadKey, 1, "bad key (radio on, power works)" },
            { kUiUnknown, 1, "unknown (a driver newer than the panel)" },
            { kUiIdle, 0, "idle (module closed: no heartbeat to carry it out)" },
            { kUiStalled, 0, "stalled (not responding)" }, { kUiNoDriver, 0, "no driver" }, { kUiNoCard, 0, "no card" } };
        unsigned k;
        char what[120];
        b = Block(kApLinkUp, "HomeNet");
        for (k = 0; k < sizeof kB / sizeof kB[0]; k++) {
            ApViewBuild(&v, kB[k].ui, kB[k].ui < kUiStalled ? NULL : &b, kTcp);
            snprintf(what, sizeof what, "power button %s: %s", kB[k].enabled ? "works" : "dimmed", kB[k].name);
            Check(v.powerEnabled == kB[k].enabled, what);
        } }
    b = Block(kApLinkUp, "HomeNet");
    ApViewBuild(&v, kUiUp, &b, kTcp);
    ApViewPending(&v, kPendOff);
    CheckStr(v.line1, "Waiting for AirPort to turn off\311", "pending off: Apple's STR# 1000 #10");
    Check(v.arrows == 1 && v.powerEnabled == 0, "pending off: arrows turn, the button cannot be pressed twice");
    CheckStr(v.line2, "TCP/IP is using \322Default\323", "pending off: line 2 unchanged");
    Check(v.on == 1, "pending off: the button still names the state the radio is in (on)");
    b = Block(kApLinkOff, "HomeNet");
    ApViewBuild(&v, kUiOff, &b, kTcp);
    ApViewPending(&v, kPendOn);
    CheckStr(v.line1, "Waiting for AirPort to turn on\311", "pending on: Apple's STR# 1000 #9");
    Check(v.arrows == 1 && v.powerEnabled == 0 && v.on == 0, "pending on: arrows, dimmed button, still reads On");
    ApViewBuild(&w, kUiOff, &b, kTcp);
    memcpy(&v, &w, sizeof v);
    ApViewPending(&v, kPendNone);
    Check(memcmp(&v, &w, sizeof v) == 0, "no command pending: the view is untouched");

    /* ---- determinism: the panel finds changes with memcmp ------------------------------------- */
    b = Block(kApLinkUp, "HomeNet");
    memset(&v, 0xA5, sizeof(v));
    memset(&w, 0x5A, sizeof(w));
    ApViewBuild(&v, kUiUp, &b, kTcp);
    ApViewBuild(&w, kUiUp, &b, kTcp);
    Check(memcmp(&v, &w, sizeof(v)) == 0, "same input = byte-identical view whatever the buffer held");
    b.signalDbm = -70;
    ApViewBuild(&w, kUiUp, &b, kTcp);
    Check(memcmp(&v, &w, sizeof(v)) != 0, "a signal change is a view change");

    printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
