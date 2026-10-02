/*
 *  ap_panel.c -- the AirPort Extreme control panel, 1.2 (status; 1.1 the user's icon; 1.2 AirPort off/on).
 *
 *  ★ MODELLED ON APPLE'S AirPort APPLICATION (2.0.4, 2002), at the user's direction: the same window,
 *  the same three groups, the same words wherever the words still apply (ap_view.h holds all of them).
 *  The layout is Apple's 'PPob' 1000 ("Select"), checked pixel by pixel against the user's screenshot
 *  of Apple's window:
 *      - every rectangle below is Apple's, in content coordinates;
 *      - text is Geneva 10 (the theme's small system font), its baseline 10 px below each Apple 'stxt'
 *        top; group titles are the small EMPHASIZED font, and a group frame's top edge runs along its
 *        title's baseline row, broken for the title 3 px either side;
 *      - Apple's "Software Base Station..." row is out of scope (UI-PRIOR-ART.md), so the window is
 *        40 px shorter than Apple's; its Connect button (a base station's modem) is out of scope too.
 *
 *  ★ PHASE 1 IS STATUS. The controls for the later phases are present and DIMMED rather than absent, so
 *  the window can be judged whole now -- the Bluetooth panel's v0.2 approach:
 *      Turn AirPort Off / On                    phase 2 (the driver needs an off switch first)
 *      Choose network, Allow ... closed nets    phase 3 (runtime join; keys in a preferences file)
 *  A dimmed control claims nothing. (The strip module LEAVES such items out of its menu instead,
 *  because a menu has no dimmed-but-visible layout to judge.)
 *
 *  ★ WHERE THE DATA COMES FROM: the driver's status block (probe/ap_status.h), Gestalt 'APXe', copied
 *  with ApStatRead -- the strip module's route exactly. Plus two things the block cannot know:
 *    - whether the card is in the machine at all (the Name Registry), so "the driver is not running"
 *      and "no card is installed" are told apart, as Apple's app tells them apart;
 *    - the TCP/IP configuration's name (Network Setup), for Apple's second line, "TCP/IP is using
 *      "Default"". Loaded at RUN time: a Mac without Network Setup loses that one line, not the panel.
 *
 *  ★ BUILT ON bluetooth/cpanel's proven shape, and its lessons, each of which cost that panel a build:
 *    - an APPL in the Control Panels folder: the 'cdev' protocol is absent from these interfaces, and
 *      Apple made control panels applications from Mac OS 8.5 on;
 *    - NewCWindow, never NewWindow: a B&W GrafPort has nowhere to put platinum, and came up WHITE three
 *      times;
 *    - the ground painted with plain QuickDraw every time, never trusting a theme call's noErr;
 *    - no Dialog Manager items at all, so none of the UserItem mouse-routing traps can apply.
 *  And unlike it, a real SIZE: this panel keeps updating in the background (canBackground) and answers
 *  the Finder's quit Apple event, so leaving it open does not stall Shut Down.
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <MacWindows.h>
#include <Quickdraw.h>
#include <QuickdrawText.h>
#include <Fonts.h>
#include <Menus.h>
#include <Events.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <Icons.h>        /* 1.4.2: GetCIcon/PlotCIcon -- the About box draws the blue Wi-Fi cicn (ap_panel_wifi.r) */
#include <Devices.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Appearance.h>
#include <AppleEvents.h>
#include <Gestalt.h>
#include <CodeFragments.h>
#include <NameRegistry.h>
#include <NetworkSetup.h>
#include <Script.h>
#include <Files.h>
#include <Folders.h>
#include <string.h>
#include "ap_view.h"
#include "ap_known.h"     /* k210: the known-networks store + Preferences form (brings ap_scanlist.h) */
#include "ap_wpa_kdf.h"   /* k210: PBKDF2 -- the panel derives the PMK so the driver never sees the password */

#define kPanelVersion  "1.4.8"

#define kPollTicks     30            /* read the block twice a second: live memory, not a file */
#define kStallTicks    240           /* heartbeat unchanged 4 s (it ticks 4/s) = not responding */
#define kTcpTicks      600           /* re-read the TCP/IP configuration name every 10 s, and on resume */

/* ---- Apple's layout (PPob 1000), content coordinates: { top, left, bottom, right } ---------------- */
#define kWinW          400
#define kWinHFull      348           /* Apple's 360 less the Software Base Station row; k213 adds the Forget row */
#define kWinHShort     136           /* Settings hidden */
#define kWinHStatus    118           /* the Status group alone: Apple's no-card window */

static const Rect kGrpStatus  = {  12,  12, 106, 388 };
static const Rect kGrpAirPort = { 136,  12, 230, 388 };
static const Rect kGrpNetwork = { 238,  12, 336, 388 };   /* k213: taller, to hold the Forget button row */

static const Rect kLine1      = {  32,  24,  46, 354 };   /* Apple: 208 wide beside its Connect button */
static const Rect kLine2      = {  46,  24,  60, 354 };
static const Rect kArrowsR    = {  32, 360,  48, 376 };   /* chasing arrows, beside the line they explain */
static const Rect kSigLbl     = {  82,  49,  96, 166 };   /* "Signal level:", right-aligned */
static const Rect kMeterR     = {  82, 173,  96, 376 };

static const Rect kTriR       = { 116,  10, 128,  22 };
static const Rect kTriLbl     = { 115,  25, 129,  90 };   /* "Settings" -- a click here toggles too */

static const Rect kPowerTxt   = { 156,  36, 170, 250 };
static const Rect kPowerBtn   = { 156, 260, 176, 376 };
static const Rect kIdLbl      = { 184,  49, 198, 166 };
static const Rect kIdVal      = { 184, 173, 198, 376 };

static const Rect kNetLbl     = { 261,  49, 275, 166 };
static const Rect kNetPop     = { 258, 173, 278, 376 };
static const Rect kBssLbl     = { 286,  49, 300, 166 };
static const Rect kBssVal     = { 286, 173, 300, 376 };
static const Rect kForgetBtn  = { 308, 236, 328, 376 };   /* k213: "Forget This Network", right-aligned under the popup */

/* ---- menus ------------------------------------------------------------------------------------------ */
#define kMApple        128
#define kMFile         129
#define kMEdit         130
#define kNetMenuID     200           /* the network popup's MENU resource (ap_panel.r) */
#define kAboutItem     1
/* ⚠ Positions in the File menu, in the order SetUpMenus appends them. */
#define kCloseItem     1
#define kQuitItem      3             /* 1 Close, 2 separator, 3 Quit */

/* ---- state -------------------------------------------------------------------------------------- */
static WindowPtr     gWin;
static ControlHandle gTri, gPowerBtn, gNetPop, gMeter, gArrows;
static ControlHandle gForgetBtn;         /* k213: forgets the current network's saved password */
static Boolean       gCurSaved;          /* k213: the status block's network is in the known-networks file */
static MenuHandle    gNetMenu;
static Boolean       gDone, gActive = true, gExpanded = true, gThemeOK = true;
static short         gMode = -1;

static ApStatBlock   gSnap;              /* last consistent copy of the driver's block */
static Boolean       gHaveSnap;
static ApStU32       gLastBeat;
static unsigned long gBeatSeen, gLastPoll, gLastTcp;
static short         gCard = -1;         /* 1 present, 0 absent, -1 unknown -- see FindCard */
static Str255        gTcpName;
static Boolean       gTcpKnown;

static ApView        gShown;             /* what is on screen */
static Boolean       gShownValid;

/* k207: the power command this panel posted and is waiting on (the block's mailbox, ap_status.h). */
#define kCmdTimeoutTicks 600             /* 10 s: then stop waiting -- the stall detector tells the rest */
static short         gCmdPending = kPendNone;
static ApStU32       gCmdSeq;
static unsigned long gCmdTick;
static short         gCmdFailed = kPendNone; /* the driver answered "failed": say so, once, from the loop */

/* ---- the driver's block -------------------------------------------------------------------------- */

#ifdef AP_PANEL_DEMO
/* ★ THE DEMO BUILD (CMake target AirPortExtremePanelDemo, never staged): a fake block that walks every
 * state, 8 s each, so the window can be looked at in QEMU with no card and no driver. 8 s because the
 * stall phase needs the 4 s kStallTicks window to pass inside it. The IDs are made up.
 * ★ PUBLISHED THE DRIVER'S WAY -- a system-heap block under Gestalt 'APXe' -- so the Control Strip module
 * in the same QEMU session reads it and shows every state too, and this panel reads it back through the
 * real Gestalt path. The block is never freed (the driver's rule: a reader must never touch freed
 * memory); DemoRetire marks it not alive when the demo quits. */
#include "ap_scanlist.h"                 /* k209: the demo also publishes a fake network list for the strip */
#define kDemoPhaseTicks 480
static ApStatBlock   gDemo;
static ApStatBlock  *gDemoPub;
static ApScanBlock  *gDemoScanPub;       /* k209: a fixed fake 'APXs' so the strip module's list can be seen in QEMU */
static unsigned long gDemoT0;
static Boolean       gDemoOff;           /* k207: the demo plays the driver's side of the mailbox too */
static ApStU32       gDemoCmdSeen;
static unsigned long gDemoCmdTick;
#define kDemoPhases     11
static short DemoPhase(void) { return (short)(((TickCount() - gDemoT0) / kDemoPhaseTicks) % kDemoPhases); }
static const ApStatBlock *DemoBlock(void);

/* k209: publish a fixed fake network list once, so the real Control Strip module in the same QEMU session
 * shows its list. Names are invented; the security mix exercises the "(not supported)" path and a long name. */
static void DemoSetItem(ApScanItem *e, const char *name, int sec, int qual, int dbm)
{
    short n = (short)strlen(name), k;
    memset(e, 0, sizeof(*e));
    e->bssid[0] = 0x02; e->bssid[5] = (ApStU8)(dbm & 0xFF);
    e->channel = 6; e->security = (ApStU8)sec; e->quality = (ApStU8)qual; e->signalDbm = dbm;
    e->ssidLen = (ApStU8)(n > 32 ? 32 : n);
    for (k = 0; k < e->ssidLen; k++) e->ssid[k] = (ApStU8)name[k];
    e->flags = (ApStU8)(ApScanSecurityJoinable((unsigned)sec) ? kApScanFJoinable : 0);
}
static void DemoPublishScan(void)
{
    static ApScanItem items[5];
    if (gDemoScanPub == NULL) {
        long v = 0;
        if (Gestalt((OSType)kApScanSelector, &v) == noErr && v != 0) gDemoScanPub = (ApScanBlock *)v;
        else {
            gDemoScanPub = (ApScanBlock *)NewPtrSysClear((Size)sizeof(ApScanBlock));
            if (gDemoScanPub != NULL) {
                gDemoScanPub->magic = kApScanMagic; gDemoScanPub->version = kApScanVersion;
                gDemoScanPub->size = (ApStU32)sizeof(ApScanBlock);
                if (NewGestaltValue((OSType)kApScanSelector, (long)gDemoScanPub) != noErr) gDemoScanPub = NULL;
            }
        }
        if (gDemoScanPub == NULL) return;
        DemoSetItem(&items[0], "CoffeeShop",             kApSecOpen,       kApQualGood,    -58);
        DemoSetItem(&items[1], "Library",                kApSecWpa2Psk,    kApQualStrong,  -49);
        DemoSetItem(&items[2], "CorpNet",                kApSecEnterprise, kApQualAverage, -71);
        DemoSetItem(&items[3], "TheOldLinksysInTheAttic", kApSecWep,       kApQualWeak,    -82);
        DemoSetItem(&items[4], "Neighbour5",             kApSecWpa2Wpa3,   kApQualAverage, -73);
        ApScanPublish(gDemoScanPub, items, 5, 0, 1);
    }
}

static void DemoPublish(void)
{
    ApStU32 seq;
    (void)DemoBlock();                         /* fills gDemo for the current phase */
    DemoPublishScan();
    if (gDemoPub == NULL) {
        long v = 0;
        if (Gestalt((OSType)kApStatSelector, &v) == noErr && v != 0) gDemoPub = (ApStatBlock *)v;
        else {
            gDemoPub = (ApStatBlock *)NewPtrSysClear((Size)sizeof(ApStatBlock));
            if (gDemoPub != NULL && NewGestaltValue((OSType)kApStatSelector, (long)gDemoPub) != noErr)
                gDemoPub = NULL;               /* leaked, deliberately: freeing it is the unsafe option */
        }
        if (gDemoPub == NULL) return;
    }
    /* The mailbox belongs to the UIs and to whoever plays the driver: carry it across the rewrite. */
    gDemo.cmdSeq = gDemoPub->cmdSeq; gDemo.cmd = gDemoPub->cmd; gDemo.cmdArg = gDemoPub->cmdArg;
    gDemo.ackSeq = gDemoPub->ackSeq; gDemo.ackResult = gDemoPub->ackResult;
    seq = gDemoPub->seq;
    gDemoPub->seq = seq | 1u;                  /* odd while rewriting */
    memcpy(gDemoPub, &gDemo, sizeof(gDemo));
    gDemoPub->seq = (seq | 1u) + 1u;           /* even: consistent */
    /* k207: answer a command the way the driver does, a second later so "Waiting for..." shows. */
    if (ApStatBusy(gDemoPub)) {
        if (gDemoCmdSeen != gDemoPub->cmdSeq) { gDemoCmdSeen = gDemoPub->cmdSeq; gDemoCmdTick = TickCount(); }
        else if (TickCount() - gDemoCmdTick > 60) {
            ApStU32 s = 0, c = 0, a = 0;
            if (ApStatTake(gDemoPub, &s, &c, &a)) {
                if (c == kApCmdPower) gDemoOff = (Boolean)(a == 0);
                ApStatAck(gDemoPub, s, (c == kApCmdPower) ? kApAckDone : kApAckUnknown);
            }
        }
    }
}
static void DemoRetire(void)
{
    if (gDemoPub != NULL) gDemoPub->alive = 0;
}
static const ApStatBlock *DemoBlock(void)
{
    static const ApStU8 kId[6]  = { 0x02, 0x11, 0x24, 0x9A, 0x3C, 0x5E };   /* locally administered: no vendor */
    static const ApStU8 kBss[6] = { 0x02, 0x00, 0x5E, 0x10, 0x20, 0x30 };
    short p = DemoPhase();
    memset(&gDemo, 0, sizeof(gDemo));
    gDemo.magic = kApStatMagic; gDemo.version = kApStatVersion; gDemo.size = sizeof(gDemo);
    gDemo.driverBuild = 206; gDemo.alive = 1; gDemo.channel = 1;
    gDemo.heartbeat = (p == 8) ? 12345 : (ApStU32)(TickCount() / 15);   /* phase 8: frozen = stalled */
    memcpy(gDemo.airportId, kId, 6);
    memcpy(gDemo.bssid, kBss, 6);
    gDemo.ssidLen = 11;
    memcpy(gDemo.ssid, "DemoNetwork", 11);
    switch (p) {
    case 0: case 7: gDemo.linkState = kApLinkUp; gDemo.signalDbm = -50; break;   /* Strong: 5 dots */
    case 1:         gDemo.linkState = kApLinkUp; gDemo.signalDbm = -80; break;   /* Weak: 2 */
    case 2:         gDemo.linkState = kApLinkSearching; break;
    case 3:         gDemo.linkState = kApLinkJoining; break;
    case 4:         gDemo.linkState = kApLinkOff; break;
    case 5:         gDemo.linkState = kApLinkNoDriver; gDemo.alive = 0; break;
    case 6:         gDemo.magic = 0; break;         /* an invalid block reads as none: driver not running */
    case 9:         gDemo.linkState = kApLinkUp; gDemo.signalDbm = -70; break;   /* Average: 3 */
    case 10:        gDemo.linkState = kApLinkUp; gDemo.signalDbm = -90; break;   /* Out of Range: 1 */
    default:        gDemo.linkState = kApLinkUp; gDemo.signalDbm = -62; break;   /* Good: 4 (phase 8) */
    }
    /* The strip module draws its dots from `quality`, which the driver derives from the dBm at interrupt
     * level; the demo derives it with the driver's own function (no hysteresis history: prev = none). */
    if (gDemo.linkState == kApLinkUp) gDemo.quality = ApStatQuality(gDemo.signalDbm, 99u);
    if (gDemoOff) {                            /* AirPort off, from the panel or the strip: every phase is Off */
        gDemo.magic = kApStatMagic; gDemo.alive = 1; gDemo.linkState = kApLinkOff;
        gDemo.heartbeat = (ApStU32)(TickCount() / 15); gDemo.signalDbm = 0; gDemo.quality = 0;
    }
    return &gDemo;
}
#endif

static const ApStatBlock *FindBlock(void)
{
    long v = 0;
#ifdef AP_PANEL_DEMO
    DemoPublish();                             /* then read it back like any client */
#endif
    if (Gestalt((OSType)kApStatSelector, &v) != noErr || v == 0) return NULL;
    return ApStatValid((const ApStatBlock *)v) ? (const ApStatBlock *)v : NULL;
}

/* ================= k210: CHOOSE A NETWORK, DERIVE ITS PMK, REMEMBER IT ==================================
 * The panel is where the passphrase is turned into a PMK (PBKDF2, ~0.3 s) and written to Preferences; the
 * driver reads the PMK and never sees the passphrase. All of this is task level (a control panel is always
 * task level), so the File Manager is unconditionally safe here. */
#define kNetPassDLOG   300          /* the passphrase dialog (ap_panel.r); MENU 200 is the popup */
#define kNetFileName   "\pAirPort Extreme Known Networks"   /* 30 chars -- fits HFS's 31 limit */
/* ⚠ k211e: the temp name MUST also be <= 31 chars. "AirPort Extreme Known Networks tmp" was 34, so FSpCreate
 * of the temp failed (bdNamErr) and every save returned "could not write to the Preferences folder" -- the
 * first save ever reached on hardware, because the empty-dropdown bug had blocked the join flow until now. */
#define kNetFileTmp    "\pAirPort Extreme Nets (tmp)"       /* 26 chars */

static void Poll(Boolean force);    /* defined below; the join flow refreshes the view at once */
static void PCatC(StringPtr s, const char *c);   /* defined below; the alerts above use it */
static ApScanBlock  gScanSnap;      /* last consistent copy of the driver's network list */
static Boolean      gHaveScan;
static ApKnownDb    gKnownDb;       /* the panel's working copy of the known networks */
static Boolean      gKnownLoaded;
static ApStU32      gJoinSeq;       /* the kApCmdJoin we posted and are waiting on (0 = none) */
/* k213: verify-on-connect. When the user chooses a network we post a join and remember the SSID here; if the
 * driver then reports kApLinkBadKey for THAT network, the password was wrong -- we remove it (a wrong one is
 * never kept) and ask again. gVerifyLen == 0 means nothing is being verified. */
static ApStU8       gVerifySsid[32];
static short        gVerifyLen;

static const ApScanBlock *FindScanBlock(void)
{
    long v = 0;
#ifdef AP_PANEL_DEMO
    DemoPublishScan();
#endif
    if (Gestalt((OSType)kApScanSelector, &v) != noErr || v == 0) return NULL;
    return ApScanValid((const ApScanBlock *)v) ? (const ApScanBlock *)v : NULL;
}

/* System Folder:Preferences:AirPort Extreme Known Networks -- the same file the driver reads. */
static Boolean ApPanelKnownSpec(FSSpec *spec)
{
    short vRefNum; long dirID;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr)
        return false;
    { OSErr e = FSMakeFSSpec(vRefNum, dirID, kNetFileName, spec);
      if (e != noErr && e != fnfErr) return false;
      return (spec->name[0] != 0); }
}

static void ApPanelLoadKnown(void)
{
    static UInt8 buf[kApKnownFileSize];
    FSSpec spec; short ref; OSErr e; long n = (long)kApKnownFileSize;
    ApKnownClear(&gKnownDb);
    gKnownLoaded = true;
    if (!ApPanelKnownSpec(&spec)) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;      /* fnfErr on first run: an empty db is correct */
    e = FSRead(ref, &n, buf);
    (void)FSClose(ref);
    if (e != noErr && e != eofErr) return;
    { long i; for (i = n; i < (long)kApKnownFileSize; i++) buf[i] = 0; }
    (void)ApKnownLoad(&gKnownDb, buf);                          /* a bad file just leaves the db empty */
}

/* Write gKnownDb to the file, atomically: a temp file, then FSpExchangeFiles (bt_keyfile.c's pattern).
 * Creator 'APXp' so the Finder draws the panel's icon on it. Returns true on success. */
static Boolean ApPanelSaveKnown(void)
{
    static UInt8 buf[kApKnownFileSize];
    FSSpec spec, tmp; short vRefNum, ref; long dirID, n = (long)kApKnownFileSize; OSErr err;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr) return false;
    if (!ApPanelKnownSpec(&spec)) return false;
    ApKnownSave(&gKnownDb, buf);
    if (FSMakeFSSpec(vRefNum, dirID, kNetFileTmp, &tmp) != noErr
        && FSMakeFSSpec(vRefNum, dirID, kNetFileTmp, &tmp) != fnfErr) return false;
    (void)FSpDelete(&tmp);
    err = FSpCreate(&tmp, 'APXp', 'APXk', smSystemScript);
    if (err != noErr && err != dupFNErr) return false;
    if (FSpOpenDF(&tmp, fsWrPerm, &ref) != noErr) return false;
    (void)SetEOF(ref, 0);
    err = FSWrite(ref, &n, buf);
    (void)FSClose(ref);
    if (err != noErr) { (void)FSpDelete(&tmp); return false; }
    if (FSpExchangeFiles(&tmp, &spec) == noErr) {
        (void)FSpDelete(&tmp);
    } else {
        (void)FSpDelete(&spec);
        if (FSpRename(&tmp, kNetFileName) != noErr) return false;
    }
    (void)FlushVol(NULL, spec.vRefNum);
    return true;
}

/* Run the passphrase dialog for the chosen SSID. Returns true with the WPA2 PMK in pmk[32] (PBKDF2 of the
 * passphrase and the SSID), or false if the user cancelled or typed nothing. The passphrase is a local that
 * is cleared before returning; only the PMK survives, and only into the caller's buffer. */
/* k211e: a password field. The Dialog Manager has no masked EditText, so a filter keeps the real characters in
 * gPassReal and shows a bullet (MacRoman 0xA5) for each -- exactly how Apple's own password dialogs do it. The
 * field is append/backspace only (arrows and the mouse cannot move the caret), so the bullets and gPassReal
 * never drift apart. gPassReal is cleared the moment the dialog closes. */
static Str255 gPassReal;

static pascal Boolean ApPassFilter(DialogPtr d, EventRecord *ev, short *item)
{
    unsigned char c;
    if (ev->what != keyDown && ev->what != autoKey) return false;
    c = (unsigned char)(ev->message & charCodeMask);
    if (c == '\r' || c == 3) {                                 /* ⚠ k211f: Return/Enter ALWAYS fires Join --
                                 * the field is multi-line, so letting it through inserted a newline instead. */
        short t; Handle h; Rect bx; unsigned long t2;
        GetDialogItem(d, 1, &t, &h, &bx);
        if (h != NULL) { HiliteControl((ControlHandle)h, kControlButtonPart); Delay(8, &t2);
                         HiliteControl((ControlHandle)h, 0); }
        *item = 1; return true;
    }
    if (c == 0x1B) { *item = 2; return true; }                 /* Esc -> Cancel */
    if (ev->modifiers & cmdKey) return false;                  /* command keys -> ModalDialog (e.g. Cmd-.) */
    if (c == 8 || c == 0x7F) { if (gPassReal[0] > 0) gPassReal[0]--; return false; }   /* delete a bullet too */
    if (c >= 0x1C && c <= 0x1F) return true;                   /* arrow keys: keep the caret at the end */
    if (c >= 0x20 && gPassReal[0] < 63) {                      /* a real character: remember it, show a bullet */
        gPassReal[++gPassReal[0]] = c;
        ev->message = (ev->message & ~(unsigned long)charCodeMask) | 0xA5u;
        return false;
    }
    return true;                                               /* field full, or an unwanted control key */
}

/* k211f: draw Apple's Note icon (ap_panel_note.r cicn 300) in the dialog's icon user item. A DITL Icon item
 * cannot show a color icon, so the item is a UserItem and this proc plots the cicn into its rect. */
static pascal void ApNoteIconItem(DialogPtr d, short item)
{
    short t; Handle h; Rect box; CIconHandle ci;
    GetDialogItem(d, item, &t, &h, &box);
    ci = GetCIcon(300);
    if (ci != NULL) { PlotCIcon(&box, ci); DisposeCIcon(ci); }
}

static Boolean ApPanelAskPassphrase(const ApStU8 *ssid, short ssidLen, ApStU8 *pmk)
{
    DialogPtr d; short hit = 0; Boolean ok = false; ModalFilterUPP filt;
    UserItemUPP iconUPP; short it; Handle ih; Rect ib;
    gPassReal[0] = 0;
    d = GetNewDialog(kNetPassDLOG, NULL, (WindowPtr)-1L);
    if (d == NULL) return false;
    SetDialogDefaultItem(d, 1);                    /* OK/Join */
    SetDialogCancelItem(d, 2);                     /* Cancel */
    iconUPP = NewUserItemUPP(ApNoteIconItem);      /* item 3: Apple's Note icon, drawn by ApNoteIconItem */
    GetDialogItem(d, 3, &it, &ih, &ib);
    SetDialogItem(d, 3, it, (Handle)iconUPP, &ib);
    ShowWindow((WindowPtr)d);
    filt = NewModalFilterUPP(ApPassFilter);
    do { ModalDialog(filt, &hit); } while (hit != 1 && hit != 2);
    if (filt != NULL) DisposeModalFilterUPP(filt);
    if (iconUPP != NULL) DisposeUserItemUPP(iconUPP);
    if (hit == 1 && gPassReal[0] >= 8) {           /* WPA2 passphrases are 8..63 characters */
        ApPbkdf2Sha1((const ApU8 *)&gPassReal[1], (ApU32)gPassReal[0], ssid, (ApU32)ssidLen, 4096UL, pmk, 32UL);
        ok = true;
    }
    { short i; for (i = 0; i <= 255; i++) gPassReal[i] = 0; }   /* the passphrase does not outlive this call */
    DisposeDialog(d);
    return ok;
}

/* Post a join for the network the user chose, and WATCH it (k213 verify-on-connect): remember the SSID so that
 * if the driver then reports kApLinkBadKey for it, we know the password was wrong and can remove it. Shared by
 * the "already known" and the "newly entered" paths. If AirPort is not the active TCP/IP port the driver is not
 * running, so nothing visibly happens -- the choice IS saved, so say how to connect rather than leave the user
 * staring at an unchanged window (alive == 0 = the OT module is not loaded, ap_shim.c ApLinkStop). */
static void ApPanelPostJoin(const ApStU8 *ssid, short n)
{
    ApStatBlock *b = (ApStatBlock *)FindBlock();
    gVerifyLen = 0;                                /* a fresh post replaces any previous watch */
    if (b != NULL && !ApStatBusy(b)) {
        short i;
        for (i = 0; i < n && i < 32; i++) gVerifySsid[i] = ssid[i];
        gVerifyLen = n;                            /* watch THIS network for a wrong-password verdict */
        gJoinSeq = ApStatPost(b, kApCmdJoin, 0u);
        Poll(true);                                /* the view updates at once */
    }
    if (!(gHaveSnap && gSnap.alive)) {
        Str255 m, x; SInt16 h = 0;
        m[0] = 0; PCatC(m, "The network was saved, but the Mac is not connected to it yet.");
        x[0] = 0; PCatC(x, "AirPort is not your active network connection. To connect now, open the TCP/IP "
                           "control panel, choose AirPort for \322Connect via,\323 and close it to save. The "
                           "Mac will then join this network on its own.");
        (void)StandardAlert(kAlertNoteAlert, m, x, NULL, &h);
    }
}

/* A network that is not yet saved (new, or just removed after a wrong password): ask for the passphrase, derive
 * and store its PMK, save, and post a watched join. Returns true if a join was posted. The passphrase lives only
 * inside ApPanelAskPassphrase; only the PMK reaches the file, and the local copy is cleared here. */
static Boolean ApPanelJoinNew(const ApStU8 *ssid, short n)
{
    ApStU8 pmk[32];
    int idx;
    if (n <= 0) return false;
    if (!ApPanelAskPassphrase(ssid, (short)n, pmk)) return false;    /* cancelled or too short */
    if (!gKnownLoaded) ApPanelLoadKnown();
    idx = ApKnownAdd(&gKnownDb, ssid, (ApStU8)n, kApSecWpa2Psk, pmk);
    { short i; for (i = 0; i < 32; i++) pmk[i] = 0; }                /* clear the local PMK copy */
    if (idx < 0) return false;                                       /* table full */
    ApKnownTouch(&gKnownDb, idx);                                    /* make it the auto-join preference */
    if (!ApPanelSaveKnown()) {
        Str255 m, x; SInt16 h = 0;
        m[0] = 0; PCatC(m, "The network could not be saved.");
        x[0] = 0; PCatC(x, "AirPort Extreme could not write to the Preferences folder.");
        (void)StandardAlert(kAlertCautionAlert, m, x, NULL, &h);
        return false;
    }
    ApPanelPostJoin(ssid, (short)n);
    return true;
}

/* k217: an OPEN network that is not yet saved. No passphrase -- store it keyless (ApKnownAdd zeroes the PMK
 * for kApSecOpen), make it the auto-join preference, save, and post a watched join. Mirrors ApPanelJoinNew
 * without the passphrase dialog. The wrong-password watch in ApPanelPostJoin is inert for an open network:
 * the driver never latches kApLinkBadKey without a handshake, so it can never remove a keyless entry. */
static Boolean ApPanelJoinNewOpen(const ApStU8 *ssid, short n)
{
    int idx;
    if (n <= 0) return false;
    if (!gKnownLoaded) ApPanelLoadKnown();
    idx = ApKnownAdd(&gKnownDb, ssid, (ApStU8)n, kApSecOpen, NULL);   /* keyless -- no PMK */
    if (idx < 0) return false;                                        /* table full */
    ApKnownTouch(&gKnownDb, idx);                                     /* make it the auto-join preference */
    if (!ApPanelSaveKnown()) {
        Str255 m, x; SInt16 h = 0;
        m[0] = 0; PCatC(m, "The network could not be saved.");
        x[0] = 0; PCatC(x, "AirPort Extreme could not write to the Preferences folder.");
        (void)StandardAlert(kAlertCautionAlert, m, x, NULL, &h);
        return false;
    }
    ApPanelPostJoin(ssid, n);
    return true;
}

/* The user chose a scan entry. Known already -> mark it used and join (watched: the stored password may be
 * stale). Unknown WPA2 -> ask the password, derive and store the PMK, then join. Unknown open (k217) -> store
 * it keyless and join, no password asked. Writes Preferences and posts kApCmdJoin. */
static void ApPanelChoose(const ApScanItem *e)
{
    int idx;
    short n = (short)((e->ssidLen > 32) ? 32 : e->ssidLen);
    if (n == 0) return;                            /* hidden network: k211's "Other..." */
    if (!ApScanSecurityJoinable(e->security)) {    /* k217: open is joinable, so it passes here */
        Str255 m, x; SInt16 h = 0;
        m[0] = 0; PCatC(m, "This network cannot be joined.");
        x[0] = 0; PCatC(x, "AirPort Extreme joins WPA2 Personal and open networks only.");
        (void)StandardAlert(kAlertNoteAlert, m, x, NULL, &h);
        return;
    }
    if (!gKnownLoaded) ApPanelLoadKnown();
    idx = ApKnownFind(&gKnownDb, e->ssid, (ApStU8)n);
    if (idx < 0) {                                  /* new network: store it, then join (watched) */
        if (e->security == kApSecOpen) (void)ApPanelJoinNewOpen(e->ssid, n);  /* k217: no password asked */
        else                           (void)ApPanelJoinNew(e->ssid, n);      /* WPA2: ask the password */
        return;
    }
    ApKnownTouch(&gKnownDb, idx);                  /* known already: make it the auto-join preference */
    if (!ApPanelSaveKnown()) {
        Str255 m, x; SInt16 h = 0;
        m[0] = 0; PCatC(m, "The network could not be saved.");
        x[0] = 0; PCatC(x, "AirPort Extreme could not write to the Preferences folder.");
        (void)StandardAlert(kAlertCautionAlert, m, x, NULL, &h);
        return;
    }
    ApPanelPostJoin(e->ssid, n);                   /* join and watch */
}

/* k213: does the driver's current network match the one we are verifying? The driver writes the target SSID
 * into the status block, so a kApLinkBadKey with this SSID is the verdict on the password we just saved. */
static Boolean ApVerifyMatch(void)
{
    short n = (short)((gSnap.ssidLen > 32) ? 32 : gSnap.ssidLen);
    short i;
    if (n != gVerifyLen) return false;
    for (i = 0; i < n; i++) if (gSnap.ssid[i] != gVerifySsid[i]) return false;
    return true;
}

/* k213: the base station rejected the password for the network we were verifying. Per the chosen behaviour
 * ("remove it, ask again"): remove the saved network so a wrong password is never kept, tell the user, and ask
 * for the password again. gVerifyLen is cleared FIRST so the modal dialogs below (which pump their own event
 * loops, re-entering Poll) cannot fire this twice for the same verdict. */
static void ApPanelBadKey(void)
{
    ApStU8 ssid[32];
    short  n, i;
    Str255 m, x; SInt16 h = 0;
    n = (short)((gSnap.ssidLen > 32) ? 32 : gSnap.ssidLen);
    for (i = 0; i < n; i++) ssid[i] = gSnap.ssid[i];
    gVerifyLen = 0; gJoinSeq = 0;                   /* the verdict is in: stop watching before any dialog */
    if (!gKnownLoaded) ApPanelLoadKnown();
    ApKnownRemove(&gKnownDb, ssid, (ApStU8)n);      /* a wrong password is never kept */
    (void)ApPanelSaveKnown();
    m[0] = 0; PCatC(m, "The password is incorrect.");
    x[0] = 0; PCatC(x, "The base station did not accept the password for \322");
    ApVCatSsid(x, ssid, n);
    PCatC(x, "\323. It has been removed. Enter the password again to try once more.");
    (void)StandardAlert(kAlertNoteAlert, m, x, NULL, &h);
    (void)ApPanelJoinNew(ssid, n);                  /* "ask again": re-enter and re-join (watched) */
}

/* Rebuild the "Choose network" popup from the last scan: item 1 is the current network (or a dash), then one
 * item per joinable, named network the driver heard. gNetItemScan maps a menu item to its scan index. Rebuilt
 * only when the scan or the current network changed, so the popup does not churn twice a second. */
static short   gNetItemScan[kApScanMax + 2];
static ApStU32 gNetMenuScanSeq = 0xFFFFFFFFUL;
static Boolean gNetMenuCurValid = false;
static Str255  gNetMenuCur;

/* ⚠ k211b: a classic popupMenuProc (CDEF 1008) draws its dropdown from its OWN menu -- the mHandle inside
 * the PopupPrivateData that hangs off the control's contrlData -- NOT from GetMenuHandle(id) (tried k210)
 * and NOT from the Appearance tag kControlPopupButtonMenuHandleTag (tried k211). Both of those left the
 * popup showing "-" while the menu we had populated sat unused. So edit the control's own menu. The
 * PopupPrivateData layout is not in the Multiversal headers; it is Inside Macintosh's documented struct. */
typedef struct { MenuHandle mHandle; short mID; } ApPopupPriv;

static MenuHandle ApNetMenu(void)
{
    if (gNetPop != NULL) {
        Handle d = (**gNetPop).contrlData;           /* popupMenuProc: the PopupPrivateData handle */
        if (d != NULL && *d != NULL) {
            MenuHandle m = (*(ApPopupPriv **)d)->mHandle;
            if (m != NULL) return m;
        }
    }
    return GetMenuHandle(kNetMenuID);                 /* fallback: the menu-list copy */
}

static void RebuildNetMenu(void)
{
    short i;
    gNetMenu = ApNetMenu();                           /* k211b: the control's OWN menu, re-fetched each time */
    if (gNetMenu == NULL) return;
    while (CountMenuItems(gNetMenu) > 0) DeleteMenuItem(gNetMenu, 1);
    AppendMenu(gNetMenu, "\px");                    /* item 1: the current network, set by name below */
    if (gShown.net[0]) { short k; gNetMenuCur[0]=0; for (k=1;k<=gShown.net[0];k++) gNetMenuCur[++gNetMenuCur[0]]=gShown.net[k];
                         SetMenuItemText(gNetMenu, 1, gNetMenuCur); }
    else SetMenuItemText(gNetMenu, 1, "\p\320");    /* en dash, Apple's placeholder */
    gNetItemScan[1] = -1;
    if (gHaveScan) {
        for (i = 0; i < (short)gScanSnap.count && i < (short)kApScanMax; i++) {
            const ApScanItem *e = &gScanSnap.items[i];
            Str255 s; short it, k, n = (short)((e->ssidLen > 32) ? 32 : e->ssidLen);
            if (n == 0) continue;                             /* hidden: k211's "Other..." */
            if (ApScanDupEarlier(gScanSnap.items, i)) continue;   /* k212: one line per SSID, as Apple does */
            /* k215: skip ONLY the current network, and BY NAME (gShown.net, which the status block updates the
             * instant we associate). We no longer skip kApScanFOurs: that flag is stamped at SCAN time on the
             * BSSID that was the target then, and the scan block is refreshed only every ~90 s -- so after you
             * switch networks it still marks the one you LEFT as "ours" and hid it from the list, even though
             * you had just been on it (the "current network vanished from the dropdown" report). The name check below
             * is the live, non-stale equivalent and already keeps the current network from appearing twice. */
            if (gShown.net[0] == (unsigned char)n) {
                short q; Boolean cur = true;
                for (q = 0; q < n; q++) if ((unsigned char)gShown.net[1 + q] != e->ssid[q]) { cur = false; break; }
                if (cur) continue;
            }
            if (!ApScanSecurityJoinable(e->security)) continue;   /* only choosable networks in the popup */
            s[0] = 0; for (k = 0; k < n; k++) s[++s[0]] = e->ssid[k];
            AppendMenu(gNetMenu, "\px");
            it = (short)CountMenuItems(gNetMenu);
            SetMenuItemText(gNetMenu, it, s);
            if (it < (short)(kApScanMax + 2)) gNetItemScan[it] = i;
        }
    }
    CalcMenuSize(gNetMenu);                /* k211c: recompute width/height after changing items, so the drop is sized */
    if (gNetPop != NULL) {
        SetControlMaximum(gNetPop, CountMenuItems(gNetMenu));
        SetControlValue(gNetPop, 1);
        if (IsControlVisible(gNetPop)) Draw1Control(gNetPop);
    }
}

/* Read the scan block and rebuild the popup when it changed. A stale or empty list also asks the driver for
 * a fresh scan (kApCmdScan), at most that often, so opening the panel shows current networks. */
static void RefreshNetworks(void)
{
    const ApScanBlock *sb = FindScanBlock();
    Boolean curChanged;
    gHaveScan = (sb != NULL && ApScanRead(sb, &gScanSnap));
    curChanged = (!gNetMenuCurValid) || (Byte)gNetMenuCur[0] != (Byte)gShown.net[0]
                 || memcmp(&gNetMenuCur[1], &gShown.net[1], gShown.net[0]) != 0;
    if (!gHaveScan) { if (curChanged) { RebuildNetMenu(); gNetMenuCurValid = true; } return; }
    if (gScanSnap.scanSeq != gNetMenuScanSeq || curChanged) {
        RebuildNetMenu();
        gNetMenuScanSeq = gScanSnap.scanSeq;
        gNetMenuCurValid = true;
    }
    { ApStatBlock *b = (ApStatBlock *)FindBlock();
      Boolean stale = (gScanSnap.scanSeq == 0
                       || (gHaveSnap && (ApStU32)(gSnap.heartbeat - gScanSnap.scanTick) > 240u));
      if (b != NULL && stale && gHaveSnap && gSnap.linkState != kApLinkNoDriver
          && gSnap.linkState != kApLinkOff && !ApStatBusy(b))
          (void)ApStatPost(b, kApCmdScan, 0u); }
}

/* Copy the block and decide the state. The stall clock lives here, not in ap_view.h, because it is
 * TickCount history rather than a fact in the block. */
static short ReadDriver(void)
{
    const ApStatBlock *b = FindBlock();
    unsigned long now = TickCount();
    int stalled = 0;
    if (b == NULL) { gHaveSnap = false; return (short)ApViewUi(0, NULL, gCard, 0); }
    (void)ApStatRead(b, &gSnap);          /* a torn read is at worst one stale frame, retried in 0.5 s */
    gHaveSnap = true;
    if (gSnap.heartbeat != gLastBeat) { gLastBeat = gSnap.heartbeat; gBeatSeen = now; }
    else if (now - gBeatSeen > kStallTicks) stalled = 1;
    return (short)ApViewUi(1, &gSnap, gCard, stalled);
}

/* ---- is the card in the machine? ------------------------------------------------------------------ */

/* A BCM4306 (14e4:4320 or 4325) anywhere in the Name Registry. The walk is airport_probe.c's, proven on
 * this machine against this card: the FIRST iterate takes kRegIterDescendants, every later one
 * kRegIterContinue ([[reference_os9_nameregistry_iterate]] -- get that wrong and the walk returns zero
 * nodes with no error). ⚠ And the verdict is GATED: a walk that saw no PCI node at all is broken, and
 * reports -1 (unknown) rather than "no card", which would send the user looking for hardware that is
 * there. Run once: a PCI card does not come and go. */
static short FindCard(void)
{
    RegEntryIter cookie;
    RegEntryID   entry;
    Boolean      done = false, first = true;
    long         nodes = 0, guard;
    short        found = 0;

    if (RegistryEntryIterateCreate(&cookie) != noErr) return -1;
    for (guard = 0; guard < 8192 && !found; guard++) {
        UInt32 vid = 0, did = 0;
        RegPropertyValueSize sz;
        OSStatus err;
        RegistryEntryIDInit(&entry);
        err = RegistryEntryIterate(&cookie, first ? kRegIterDescendants : kRegIterContinue, &entry, &done);
        first = false;
        if (err != noErr || done) { RegistryEntryIDDispose(&entry); break; }
        sz = sizeof(vid);
        if (RegistryPropertyGet(&entry, "vendor-id", &vid, &sz) == noErr && sz == sizeof(vid)) {
            nodes++;
            sz = sizeof(did);
            if ((vid & 0xFFFFUL) == 0x14E4UL
                && RegistryPropertyGet(&entry, "device-id", &did, &sz) == noErr && sz == sizeof(did)
                && ((did & 0xFFFFUL) == 0x4320UL || (did & 0xFFFFUL) == 0x4325UL))
                found = 1;
        }
        RegistryEntryIDDispose(&entry);
    }
    RegistryEntryIterateDispose(&cookie);
    if (found) return 1;
    return (short)(nodes > 0 ? 0 : -1);
}

/* ---- the TCP/IP configuration's name (Network Setup) ---------------------------------------------- */

/* ⚠ RESOLVED AT RUN TIME from the "CfgOpenTpt" fragment, not linked: a strong import of a library the
 * machine lacks stops an application launching at all, with a dialog naming the library. Our connection
 * is a real one (GetSharedLibrary takes a reference, held for the panel's life and closed by CFM when it
 * quits) -- not the FindSymbol-on-someone-else's-connection trap of the Bluetooth panel
 * ([[reference_os9_findsymbol_no_reference]]), so the pointers stay valid. */
static struct {
    OSStatus (*OpenDatabase)(CfgDatabaseRef *);
    OSStatus (*CloseDatabase)(CfgDatabaseRef *);
    OSStatus (*GetCurrentArea)(CfgDatabaseRef, CfgAreaID *);
    OSStatus (*OpenArea)(CfgDatabaseRef, CfgAreaID);
    OSStatus (*CloseArea)(CfgDatabaseRef, CfgAreaID);
    OSStatus (*GetEntitiesCount)(CfgDatabaseRef, CfgAreaID, CfgEntityClass, CfgEntityType, ItemCount *);
    OSStatus (*GetEntitiesList)(CfgDatabaseRef, CfgAreaID, CfgEntityClass, CfgEntityType, ItemCount *,
                                CfgEntityRef *, CfgEntityInfo *);
    OSStatus (*OpenPrefs)(CfgDatabaseRef, const CfgEntityRef *, Boolean, CfgEntityAccessID *);
    OSStatus (*ClosePrefs)(CfgEntityAccessID);
    OSStatus (*GetPrefs)(CfgEntityAccessID, OSType, void *, ByteCount);
    OSStatus (*GetPrefsSize)(CfgEntityAccessID, OSType, ByteCount *);
    Boolean  (*IsSameEntityRef)(const CfgEntityRef *, const CfgEntityRef *, Boolean);
#ifdef AP_PANEL_DEMO
    void     (*GetEntityName)(const CfgEntityRef *, Str255);      /* the demo logs what it returns (an ID) */
#endif
} gNs;
static Boolean gNsTried, gNsOK;

/* NetworkSetup.h is mac68k-packed, which is also Retro68's default packing
 * ([[reference_retro68_ignores_pragma_align_power]]); these pin the layouts the vector walk indexes by. */
_Static_assert(sizeof(CfgEntityRef) == 264,                         "CfgEntityRef layout");
_Static_assert(sizeof(CfgEntityInfo) == 336,                        "CfgEntityInfo layout");
_Static_assert(sizeof(CfgSetsElement) == 600,                       "CfgSetsElement layout");
_Static_assert(__builtin_offsetof(CfgSetsVector, fElements) == 4,   "CfgSetsVector layout");
_Static_assert(sizeof(CfgSetsStruct) == 12,                         "CfgSetsStruct layout");

static Boolean NsResolve(void)
{
    static const struct { const char *name; void **slot; } kSyms[] = {
        { "OTCfgOpenDatabase",     (void **)&gNs.OpenDatabase },
        { "OTCfgCloseDatabase",    (void **)&gNs.CloseDatabase },
        { "OTCfgGetCurrentArea",   (void **)&gNs.GetCurrentArea },
        { "OTCfgOpenArea",         (void **)&gNs.OpenArea },
        { "OTCfgCloseArea",        (void **)&gNs.CloseArea },
        { "OTCfgGetEntitiesCount", (void **)&gNs.GetEntitiesCount },
        { "OTCfgGetEntitiesList",  (void **)&gNs.GetEntitiesList },
        { "OTCfgOpenPrefs",        (void **)&gNs.OpenPrefs },
        { "OTCfgClosePrefs",       (void **)&gNs.ClosePrefs },
        { "OTCfgGetPrefs",         (void **)&gNs.GetPrefs },
        { "OTCfgGetPrefsSize",     (void **)&gNs.GetPrefsSize },
        { "OTCfgIsSameEntityRef",  (void **)&gNs.IsSameEntityRef },
#ifdef AP_PANEL_DEMO
        { "OTCfgGetEntityName",    (void **)&gNs.GetEntityName },
#endif
    };
    CFragConnectionID conn = 0;
    Ptr               mainAddr = NULL;
    Str255            errName;
    unsigned          i;

    if (gNsTried) return gNsOK;
    gNsTried = true;
    if (GetSharedLibrary("\pCfgOpenTpt", kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName) != noErr)
        return false;
    for (i = 0; i < sizeof(kSyms) / sizeof(kSyms[0]); i++) {
        Str255           nm;
        Ptr              addr = NULL;
        CFragSymbolClass cls;
        const char      *c = kSyms[i].name;
        nm[0] = 0;
        while (*c && nm[0] < 255) nm[++nm[0]] = (unsigned char)*c++;
        if (FindSymbol(conn, nm, &addr, &cls) != noErr || addr == NULL) return false;
        /* On PowerPC a C function pointer IS the TVector's address, which is what FindSymbol returns.
         * memcpy, not a cast-and-store, so no aliasing rule is in play. */
        memcpy(kSyms[i].slot, &addr, sizeof(addr));
    }
    gNsOK = true;
    return true;
}

static Boolean NsReadFixed(CfgDatabaseRef db, const CfgEntityRef *e, OSType type, void *buf, ByteCount len)
{
    CfgEntityAccessID acc = NULL;
    OSStatus          err;
    if (gNs.OpenPrefs(db, e, false, &acc) != noErr) return false;
    err = gNs.GetPrefs(acc, type, buf, len);
    (void)gNs.ClosePrefs(acc);       /* read-only access: a failed close loses nothing */
    return (Boolean)(err == noErr);
}

#ifdef AP_PANEL_DEMO
/* The demo build writes what each Network Setup step returned to "AirPort Extreme Panel Demo Log" in the
 * System Folder, once, so the lookup can be checked against a real OS 9 database under QEMU. */
static char  gDiag[3000];
static long  gDiagLen;
static void DiagC(const char *c) { while (*c && gDiagLen < (long)sizeof(gDiag) - 1) gDiag[gDiagLen++] = *c++; }
static void DiagP(ConstStr255Param p)
{
    short i;
    DiagC("\"");
    for (i = 1; i <= p[0] && gDiagLen < (long)sizeof(gDiag) - 2; i++) gDiag[gDiagLen++] = (char)p[i];
    DiagC("\"");
}
static void DiagN(long v) { Str255 s; NumToString(v, s); DiagP(s); }
#else
#define DiagC(c) ((void)0)
#define DiagP(p) ((void)0)
#define DiagN(v) ((void)0)
#endif

/* Network Setup's documented route to "the TCP/IP configuration in use", in three steps.
 *
 * 1. The ACTIVE set entity in the current area: the one whose 'stru' has kOTCfgSetsFlagActiveMask. */
#define kNsMaxList 16
static Boolean NsActiveSet(CfgDatabaseRef db, CfgAreaID area, CfgEntityRef *active)
{
    ItemCount     n = 0, i;
    CfgEntityRef *refs;
    Boolean       found = false;

    if (gNs.GetEntitiesCount(db, area, kOTCfgClassSetOfSettings, kOTCfgTypeSetOfSettings, &n) != noErr || n == 0)
        return false;
    if (n > kNsMaxList) n = kNsMaxList;
    refs = (CfgEntityRef *)NewPtrClear((Size)(n * sizeof(CfgEntityRef)));
    if (refs == NULL) return false;
    if (gNs.GetEntitiesList(db, area, kOTCfgClassSetOfSettings, kOTCfgTypeSetOfSettings, &n, refs, NULL) == noErr) {
        DiagC("sets "); DiagN((long)n);
        for (i = 0; i < n && i < kNsMaxList && !found; i++) {
            CfgSetsStruct ss;
            memset(&ss, 0, sizeof(ss));
            if (NsReadFixed(db, &refs[i], kOTCfgSetsStructPref, &ss, sizeof(ss))
                && (ss.fFlags & kOTCfgSetsFlagActiveMask)) {
                *active = refs[i];
                found = true;
                DiagC(" active #"); DiagN((long)i);
            }
        }
    }
    DisposePtr((Ptr)refs);
    return found;
}

/* 2. The TCP/IP entity that set's 'vect' names: its ref, and the name the set recorded for it.
 * ⚠⚠ NOT OTCfgGetEntityName. Measured under QEMU's OS 9.2.2 (the demo build's log): on this ref it returns
 * the ref's fID -- "-26472", an internal ID -- and the first build showed that as the configuration's name.
 * The same log: vect 3004 bytes = 5 elements of 600 + the 4-byte count (the _Static_asserts above hold),
 * stored fName "Default", correct. */
static Boolean NsSetTcpRef(CfgDatabaseRef db, const CfgEntityRef *set, CfgEntityRef *tcp, StringPtr stored)
{
    CfgEntityAccessID acc = NULL;
    ByteCount         size = 0;
    Ptr               buf;
    Boolean           found = false;

    if (gNs.OpenPrefs(db, set, false, &acc) != noErr) return false;
    if (gNs.GetPrefsSize(acc, kOTCfgSetsVectorPref, &size) == noErr && size >= 4 && size <= 60000) {
        buf = NewPtrClear((Size)size);
        if (buf != NULL) {
            if (gNs.GetPrefs(acc, kOTCfgSetsVectorPref, buf, size) == noErr) {
                const CfgSetsVector *vec = (const CfgSetsVector *)buf;
                ItemCount room = (ItemCount)((size - 4) / sizeof(CfgSetsElement)), k, cnt = vec->fCount;
                DiagC(" | vect bytes "); DiagN((long)size); DiagC(" count "); DiagN((long)cnt);
                if (cnt > room) cnt = room;                  /* never index past what was read */
                for (k = 0; k < cnt && !found; k++) {
                    const CfgSetsElement *el = &vec->fElements[k];
                    if (el->fEntityInfo.fClass == kOTCfgClassNetworkConnection
                        && el->fEntityInfo.fType == kOTCfgTypeTCPv4) {
                        *tcp = el->fEntityRef;
                        BlockMoveData(el->fEntityInfo.fName, stored, (Size)el->fEntityInfo.fName[0] + 1);
                        found = true;
#ifdef AP_PANEL_DEMO
                        {   Str255 nm; CfgEntityRef r = el->fEntityRef;
                            DiagC(" tcp #"); DiagN((long)k); DiagC(" stored fName "); DiagP(el->fEntityInfo.fName);
                            DiagC(" fID "); DiagP(el->fEntityRef.fID); DiagC(" fLoc "); DiagN((long)el->fEntityRef.fLoc);
                            nm[0] = 0; gNs.GetEntityName(&r, nm); DiagC(" GetEntityName "); DiagP(nm); }
#endif
                    }
                }
            }
            DisposePtr(buf);
        }
    }
    (void)gNs.ClosePrefs(acc);              /* read-only access: a failed close loses nothing */
    return found;
}

/* 3. Its NAME, from the area's own list of TCP/IP entities -- the list the TCP/IP control panel's
 * Configurations dialog shows -- matched to the set's ref with OTCfgIsSameEntityRef, ignoring the area
 * (a set's stored refs carry the area they were saved in, which need not be the current one: the TCP/IP
 * control panel edits in a copy and commits). QEMU: 1 entity, "Default", MATCH. Without a match: the only
 * configuration if there is just one (the active set must be using it), else the name the set itself
 * recorded -- the database's own words either way, never a guess. */
static Boolean NsTcpName(CfgDatabaseRef db, CfgAreaID area, const CfgEntityRef *tcp, ConstStr255Param stored,
                         StringPtr out)
{
    ItemCount      n = 0, i;
    CfgEntityRef  *refs;
    CfgEntityInfo *infos;
    Boolean        ok = false;

    if (gNs.GetEntitiesCount(db, area, kOTCfgClassNetworkConnection, kOTCfgTypeTCPv4, &n) != noErr || n == 0)
        return false;
    if (n > kNsMaxList) n = kNsMaxList;
    refs  = (CfgEntityRef *)NewPtrClear((Size)(n * sizeof(CfgEntityRef)));
    infos = (CfgEntityInfo *)NewPtrClear((Size)(n * sizeof(CfgEntityInfo)));
    if (refs != NULL && infos != NULL
        && gNs.GetEntitiesList(db, area, kOTCfgClassNetworkConnection, kOTCfgTypeTCPv4, &n, refs, infos) == noErr) {
        if (n > kNsMaxList) n = kNsMaxList;
        DiagC(" | tcp entities "); DiagN((long)n);
        for (i = 0; i < n && !ok; i++) {
            DiagC(" ["); DiagN((long)i); DiagC(" "); DiagP(infos[i].fName); DiagC(" fID "); DiagP(refs[i].fID); DiagC("]");
            if (gNs.IsSameEntityRef(&refs[i], tcp, kCfgIgnoreArea) && infos[i].fName[0] != 0) {
                BlockMoveData(infos[i].fName, out, (Size)infos[i].fName[0] + 1);
                ok = true;
                DiagC(" MATCH");
            }
        }
        if (!ok && n == 1 && infos[0].fName[0] != 0) {
            BlockMoveData(infos[0].fName, out, (Size)infos[0].fName[0] + 1);
            ok = true;
            DiagC(" (only one)");
        }
    }
    if (!ok && stored[0] != 0) {
        BlockMoveData(stored, out, (Size)stored[0] + 1);
        ok = true;
        DiagC(" (stored)");
    }
    if (refs != NULL)  DisposePtr((Ptr)refs);
    if (infos != NULL) DisposePtr((Ptr)infos);
    return ok;
}

static Boolean NsActiveTcpName(CfgDatabaseRef db, CfgAreaID area, StringPtr out)
{
    CfgEntityRef set, tcp;
    Str255       stored;
    memset(&set, 0, sizeof(set));
    memset(&tcp, 0, sizeof(tcp));
    stored[0] = 0;
    return (Boolean)(NsActiveSet(db, area, &set) && NsSetTcpRef(db, &set, &tcp, stored)
                     && NsTcpName(db, area, &tcp, stored, out));
}

#ifdef AP_PANEL_DEMO
static void DiagWrite(void)
{
    static Boolean done;
    short vRef, ref;
    long  dirID, n;
    FSSpec sp;
    if (done) return;
    done = true;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRef, &dirID) != noErr) return;
    (void)FSMakeFSSpec(vRef, dirID, "\pAirPort Extreme Panel Demo Log", &sp);   /* fnfErr: not there yet */
    (void)FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript);                       /* dupFNErr: reuse it */
    if (FSpOpenDF(&sp, fsWrPerm, &ref) != noErr) return;
    (void)SetEOF(ref, 0);
    DiagC("\r");
    n = gDiagLen;
    (void)FSWrite(ref, &n, gDiag);
    (void)FSClose(ref);
    (void)FlushVol(NULL, vRef);
}
#endif

static void ReadTcpName(void)
{
    CfgDatabaseRef db = NULL;
    CfgAreaID      area = 0;
    Str255         nm;
    gTcpKnown = false;
    if (!NsResolve()) { DiagC("Network Setup not resolved"); goto out; }
    if (gNs.OpenDatabase(&db) != noErr || db == NULL) { DiagC("OpenDatabase failed"); goto out; }
    if (gNs.GetCurrentArea(db, &area) == noErr && gNs.OpenArea(db, area) == noErr) {
        nm[0] = 0;
        if (NsActiveTcpName(db, area, nm)) { BlockMoveData(nm, gTcpName, (Size)nm[0] + 1); gTcpKnown = true; }
        (void)gNs.CloseArea(db, area);        /* opened for reading only */
    }
    (void)gNs.CloseDatabase(&db);
out:
    DiagC(" | result "); DiagP(gTcpKnown ? gTcpName : (ConstStr255Param)"\p(none)");
#ifdef AP_PANEL_DEMO
    DiagWrite();
#endif
}

/* ---- drawing primitives ----------------------------------------------------------------------------- */

static short ScreenDepth(void)
{
    GDHandle gd = GetMainDevice();
    return (gd != NULL && (*gd)->gdPMap != NULL) ? (*(*gd)->gdPMap)->pixelSize : 8;
}

/* bluetooth/cpanel's lesson, verbatim in intent: ask the theme for the colour, but do the ERASE with
 * plain QuickDraw every time -- SetThemeBackground once returned noErr and painted nothing. */
static void ErasePlatinum(const Rect *r)
{
    RGBColor c;
    if (GetThemeBrushAsColor(kThemeBrushDialogBackgroundActive, ScreenDepth(), true, &c) != noErr) {
        gThemeOK = false;
        c.red = c.green = c.blue = 0xDDDD;
    }
    RGBBackColor(&c);
    EraseRect(r);
}

/* Geneva 10 (bold for titles): the theme's small system font, as Apple's window uses. */
static void UseSmall(Boolean emphasized)
{
    if (UseThemeFont(emphasized ? kThemeSmallEmphasizedSystemFont : kThemeSmallSystemFont, smSystemScript) != noErr) {
        gThemeOK = false;
        TextFont(kFontIDGeneva);
        TextSize(10);
        TextFace(emphasized ? bold : normal);
    }
}

/* Dialog text, greyed while the window is in the background, as a platinum dialog's is. */
static void InkText(void)
{
    if (SetThemeTextColor(gActive ? kThemeTextColorDialogActive : kThemeTextColorDialogInactive,
                          ScreenDepth(), true) != noErr) {
        RGBColor k;
        k.red = k.green = k.blue = gActive ? 0x0000 : 0x7777;
        RGBForeColor(&k);
    }
}

enum { kJustLeft = 0, kJustRight = 1 };

/* One line in its box: erase the box, then draw s truncated to fit, baseline 10 px below the top. */
static void DrawTextBox(const Rect *r, ConstStr255Param s, short just, Boolean emphasized)
{
    Str255 t;
    short  x;
    ErasePlatinum(r);
    if (s[0] == 0) return;
    UseSmall(emphasized);
    InkText();
    BlockMoveData(s, t, (Size)s[0] + 1);
    (void)TruncString((short)(r->right - r->left), t, truncEnd);
    x = (just == kJustRight) ? (short)(r->right - StringWidth(t)) : r->left;
    MoveTo(x, (short)(r->top + 10));
    DrawString(t);
}

/* A titled primary group, Apple's way (measured off the screenshot): the frame's top edge on the title's
 * baseline row, 9 px below the group's top, broken from 3 px before the title to 4 px after it. */
static void DrawGroup(const Rect *g, ConstStr255Param title)
{
    Rect  fr = *g, gap;
    short tw;
    fr.top = (short)(g->top + 9);
    if (DrawThemePrimaryGroup(&fr, gActive ? kThemeStateActive : kThemeStateInactive) != noErr) {
        gThemeOK = false;
        ForeColor(blackColor);
        FrameRect(&fr);
    }
    UseSmall(true);
    tw = StringWidth(title);
    SetRect(&gap, (short)(g->left + 9), (short)(g->top + 7), (short)(g->left + 12 + tw + 4), (short)(g->top + 12));
    ErasePlatinum(&gap);
    InkText();
    MoveTo((short)(g->left + 12), (short)(g->top + 9));
    DrawString(title);
}

/* ---- the window ------------------------------------------------------------------------------------ */

static void DrawStatic(void)
{
    DrawGroup(&kGrpStatus, "\pStatus");
    if (gMode != kModeStatus) DrawTextBox(&kTriLbl, "\pSettings", kJustLeft, true);
    if (gMode == kModeFull) {
        DrawGroup(&kGrpAirPort, "\pAirPort");
        DrawTextBox(&kIdLbl, "\pAirPort ID:", kJustRight, false);
        DrawGroup(&kGrpNetwork, "\pAirPort Network");
        DrawTextBox(&kNetLbl, "\pChoose network:", kJustRight, false);
        DrawTextBox(&kBssLbl, "\pBase Station ID:", kJustRight, false);
    }
}

static void DrawDynamic(const ApView *v)
{
    DrawTextBox(&kLine1, v->line1, kJustLeft, false);
    DrawTextBox(&kLine2, v->line2, kJustLeft, false);
    DrawTextBox(&kSigLbl, v->live ? "\pSignal level:" : "\p", kJustRight, false);   /* Apple hides the row */
    if (gMode == kModeFull) {
        DrawTextBox(&kPowerTxt, v->power, kJustLeft, false);
        DrawTextBox(&kIdVal, v->apId, kJustLeft, false);
        DrawTextBox(&kBssVal, v->bssid, kJustLeft, false);
    }
    ForeColor(blackColor);
}

static void ShowIf(ControlHandle c, Boolean show)
{
    if (c == NULL) return;
    if (show && !IsControlVisible(c)) ShowControl(c);
    else if (!show && IsControlVisible(c)) HideControl(c);
}

/* Dimmed state for every control. The power button (k207) and the network chooser (k210) work when the view
 * says a driver can carry the action out. The Forget button (k213) is live only when the current network is
 * one we have a saved password for. The rest follow the window, as a platinum dialog's controls do. */
static void ApplyHilites(void)
{
    if (gTri != NULL)       HiliteControl(gTri, gActive ? 0 : 255);
    if (gMeter != NULL)     HiliteControl(gMeter, (gActive && gShown.live) ? 0 : 255);
    if (gArrows != NULL)    HiliteControl(gArrows, gActive ? 0 : 255);
    if (gPowerBtn != NULL)  HiliteControl(gPowerBtn, (gActive && gShown.powerEnabled) ? 0 : 255);
    /* k210: the network chooser is live when the panel is active and a running driver could carry out a
     * join. Off / no driver -> dimmed, like the power button. */
    if (gNetPop != NULL)    HiliteControl(gNetPop,
        (gActive && gShown.ui != kUiNoDriver && gShown.ui != kUiOff && gShown.ui != kUiStalled) ? 0 : 255);
    if (gForgetBtn != NULL) HiliteControl(gForgetBtn, (gActive && gCurSaved) ? 0 : 255);   /* k213 */
}

/* The controls that show values: only what changed, so a poll that finds a new signal level does not make
 * the push button flash. old == NULL means everything. */
static void SyncControls(const ApView *v, const ApView *old)
{
    if (gMeter != NULL && (old == NULL || v->live != old->live || v->meter != old->meter)) {
        if (v->live) SetControlValue(gMeter, v->meter);
        ShowIf(gMeter, (Boolean)v->live);
        HiliteControl(gMeter, (gActive && v->live) ? 0 : 255);
    }
    if (gArrows != NULL && (old == NULL || v->arrows != old->arrows))
        ShowIf(gArrows, (Boolean)v->arrows);
    if (gPowerBtn != NULL && (old == NULL || v->on != old->on))
        SetControlTitle(gPowerBtn, v->on ? "\pTurn AirPort Off" : "\pTurn AirPort On");
    if (gPowerBtn != NULL && (old == NULL || v->powerEnabled != old->powerEnabled))
        HiliteControl(gPowerBtn, (gActive && v->powerEnabled) ? 0 : 255);
    /* k210: the "Choose network" popup is owned by RefreshNetworks/RebuildNetMenu now (it holds the whole
     * list, not just the current network), so SyncControls no longer touches it. */
}

/* The window's height follows the state (no card: Status only) and the Settings triangle. Controls outside
 * the new height are HIDDEN, not just clipped, so they can neither draw nor be found by FindControl. */
static void SetMode(short m)
{
    short h;
    if (m == gMode) return;
    gMode = m;
    h = (m == kModeFull) ? kWinHFull : (m == kModeShort) ? kWinHShort : kWinHStatus;
    ShowIf(gPowerBtn,  false);
    ShowIf(gNetPop,    false);
    ShowIf(gForgetBtn, false);
    if (m == kModeStatus) ShowIf(gTri, false);
    SizeWindow(gWin, kWinW, h, true);
    if (m != kModeStatus) ShowIf(gTri, true);
    if (m == kModeFull) {
        ShowIf(gPowerBtn,  true);
        ShowIf(gNetPop,    true);
        ShowIf(gForgetBtn, true);
    }
    InvalRect(&gWin->portRect);
}

/* Every pass of the event loop; rate-limited here. force: ignore the timer (startup, resume). */
static void Poll(Boolean force)
{
    unsigned long now = TickCount();
    ApView        v;
    short         ui;
    GrafPtr       save;

    if (!force && now - gLastPoll < kPollTicks) return;
    gLastPoll = now;
    if (force || now - gLastTcp >= kTcpTicks) { ReadTcpName(); gLastTcp = now; }

    ui = ReadDriver();
    /* k213: is the driver's current network one we have a saved password for? -- gates the Forget button. The
     * panel's in-memory db is authoritative (only the panel writes the file), so this needs no re-read. */
    if (!gKnownLoaded) ApPanelLoadKnown();
    gCurSaved = (Boolean)(gHaveSnap && gSnap.ssidLen > 0 &&
                ApKnownFind(&gKnownDb, gSnap.ssid, (ApStU8)((gSnap.ssidLen > 32) ? 32 : gSnap.ssidLen)) >= 0);
    /* k207: our command -- settled (acknowledged, or superseded by a newer one that is), or given up on */
    if (gCmdPending != kPendNone) {
        if (gHaveSnap && ApStatAcked(&gSnap, gCmdSeq)) {
            if (gSnap.ackSeq == gCmdSeq && gSnap.ackResult == kApAckFailed) gCmdFailed = gCmdPending;
            gCmdPending = kPendNone;
        } else if (!gHaveSnap || now - gCmdTick > kCmdTimeoutTicks)
            gCmdPending = kPendNone;      /* no driver, or none answering: the view already says which */
    }
    ApViewBuild(&v, ui, gHaveSnap ? &gSnap : NULL, gTcpKnown ? gTcpName : NULL);
    ApViewPending(&v, gCmdPending);

    GetPort(&save);
    SetPort(gWin);
#ifdef AP_PANEL_DEMO
    {   /* demo phase 7 shows the window with Settings collapsed */
        Boolean want = (Boolean)(DemoPhase() != 7);
        if (want != gExpanded) { gExpanded = want; if (gTri != NULL) SetControlValue(gTri, want ? 1 : 0); }
    }
#endif
    SetMode((short)ApViewMode(ui, gExpanded));
    if (!gShownValid || memcmp(&v, &gShown, sizeof(v)) != 0) {
        DrawDynamic(&v);
        SyncControls(&v, gShownValid ? &gShown : NULL);
        gShown = v;
        gShownValid = true;
        ApplyHilites();     /* k211d: re-apply enable states on EVERY view change, not just window activate --
                             * the popup was left dimmed from the boot "no driver" state and never re-enabled. */
    }
    if (gMode == kModeFull) RefreshNetworks();     /* k210: keep the "Choose network" list current */
    SetPort(save);

    /* k213: verify-on-connect. A kApLinkBadKey for the network we are watching is a verdict on the password we
     * just saved -- but only once the driver has TAKEN our join (ApStatAcked); before that, kApLinkBadKey is the
     * stale state from before the command. On kUiUp the password is proven good, so stop watching. ApPanelBadKey
     * runs modal dialogs, so it goes last, with the port already restored. */
    if (gVerifyLen > 0 && gHaveSnap) {
        if (ui == kUiUp && ApVerifyMatch()) { gVerifyLen = 0; gJoinSeq = 0; }
        else if (ui == kUiBadKey && gJoinSeq != 0 && ApStatAcked(&gSnap, gJoinSeq) && ApVerifyMatch())
            ApPanelBadKey();
    }
}

static void DoUpdate(void)
{
    BeginUpdate(gWin);
    SetPort(gWin);
    ErasePlatinum(&gWin->portRect);
    DrawStatic();
    if (gShownValid) DrawDynamic(&gShown);
    UpdateControls(gWin, gWin->visRgn);
    EndUpdate(gWin);
}

static void DoActivate(Boolean on)
{
    if (on == gActive) return;
    gActive = on;
    SetPort(gWin);
    ApplyHilites();
    InvalRect(&gWin->portRect);            /* the text changes colour too */
}

static void ToggleSettings(void)
{
    gExpanded = !gExpanded;
    if (gTri != NULL) SetControlValue(gTri, gExpanded ? 1 : 0);
    SetMode((short)ApViewMode(gShown.ui, gExpanded));
}

static void MakeControls(void)
{
    Rect r;
    ControlFontStyleRec fs;

    r = kTriR;       gTri       = NewControl(gWin, &r, "\p", false, gExpanded ? 1 : 0, 0, 1, kControlTriangleProc, 0);
    r = kPowerBtn;   gPowerBtn  = NewControl(gWin, &r, "\pTurn AirPort Off", false, 0, 0, 1, kControlPushButtonProc, 0);
    /* popupMenuProc with min = the MENU resource's ID: client/src/prefs.c's route, proven on OS 9 in this
     * project, and no window between creating the control and attaching its menu. popupFixedWidth: Apple's
     * popup is 203 px whatever it holds -- without it the box shrank to fit "-" (QEMU, first build). */
    r = kNetPop;     gNetPop    = NewControl(gWin, &r, "\p", false, 0, kNetMenuID, 0,
                                             popupMenuProc + popupFixedWidth, 0);
    r = kMeterR;     gMeter     = NewControl(gWin, &r, "\p", false, 0, 0, 100, kControlProgressBarProc, 0);
    r = kArrowsR;    gArrows    = NewControl(gWin, &r, "\p", false, 0, 0, 1, kControlChasingArrowsProc, 0);
    r = kForgetBtn;  gForgetBtn = NewControl(gWin, &r, "\pForget This Network", false, 0, 0, 1, kControlPushButtonProc, 0);

    /* Apple's popup text is Geneva 10, like the labels; its push button is the system font. */
    memset(&fs, 0, sizeof(fs));
    fs.flags = kControlUseFontMask;
    fs.font  = kControlFontSmallSystemFont;
    if (gNetPop != NULL)    (void)SetControlFontStyle(gNetPop, &fs);

    /* ⚠ k211c: THE REAL BUG. NewControl(popupMenuProc) loaded the menu with GetMenu but never INSERTED it
     * into the menu list (the k211fix diagnostic proved it: the control's own menu held 8 items, yet
     * GetMenuHandle(200) was NULL). A classic popup drops its list with PopUpMenuSelect, which cannot show a
     * menu that is not inserted -- so the box stayed at "-". Insert the control's OWN menu (prefs.c does the
     * same with InsertMenu(m, -1), the step we had skipped). */
    if (gNetPop != NULL) {
        Handle d = (**gNetPop).contrlData;
        if (d != NULL && *d != NULL) {
            ApPopupPriv *pp = *(ApPopupPriv **)d;
            if (pp->mHandle != NULL && GetMenuHandle(pp->mID) != pp->mHandle)
                InsertMenu(pp->mHandle, -1);        /* -1: a popup/hierarchical menu, not in the menu bar */
        }
    }
    gNetMenu = ApNetMenu();
}

/* ---- menus, About ------------------------------------------------------------------------------------ */

static void SetUpMenus(void)
{
    MenuHandle m;
    if ((m = NewMenu(kMApple, "\p\024")) != NULL) {            /* 0x14: the apple, in the system font */
        AppendMenu(m, "\pAbout AirPort Extreme\311;(-");
        AppendResMenu(m, 'DRVR');
        InsertMenu(m, 0);
    }
    if ((m = NewMenu(kMFile, "\pFile")) != NULL) {
        AppendMenu(m, "\pClose/W;(-;Quit/Q");
        InsertMenu(m, 0);
    }
    /* Present and disabled, as bluetooth/cpanel's: the panel has no editable text. */
    if ((m = NewMenu(kMEdit, "\pEdit")) != NULL) {
        AppendMenu(m, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");
        DisableItem(m, 0);
        InsertMenu(m, 0);
    }
    DrawMenuBar();
}

static void PCatC(StringPtr s, const char *c)
{
    while (*c && s[0] < 255) s[++s[0]] = (unsigned char)*c++;
}

/* 1.4.2: the About box. An Apple-style About-window LAYOUT -- the icon OUTSIDE the text column on the left, the product
 * name over a Charcoal version line sharing the framed box's left edge, a framed white description well, a
 * copyright line, an OK button -- modelled on the Bluetooth panel's own About (bt_panel.c DrawAboutContent),
 * which was built to that layout. ⚠ The description is ORIGINAL text for THIS driver, written to describe what
 * it actually does; it is not Apple's About-AirPort wording. "AirPort Extreme" is Apple's product name, so the
 * box claims no trademark on it (the same reasoning the Bluetooth About records -- no "(TM)" lead line). */
#define kAboutTextL 66                 /* the product name and the box share this left edge; the icon is left of it */
#define kAboutBoxR  456                /* 1.4.3: 20px from the 476-wide window's right edge, matching Apple's width */

static void DrawAboutContent(WindowPtr w, ControlHandle okBtn)
{
    Rect        box, ir, tr;
    Str255      t;
    const char *desc =
        "Wireless 802.11g networking for the AirPort Extreme card on Mac OS 9. Connect to a WPA2-secured or "
        "open network from this control panel or the AirPort Extreme Control Strip. Network passwords are "
        "stored in \322AirPort Extreme Known Networks\323 in the Preferences folder. This control panel "
        "requires the AirPort Extreme system extension to be installed.\r\r"
        "Yes, this driver is awesome. No, there isn't a warranty.";

    SetPort(w);
    ErasePlatinum(&w->portRect);

    SetRect(&ir, 18, 16, 50, 48);                                  /* the AirPort Extreme icon, left of the column */
    { CIconHandle ci = GetCIcon(301);          /* 1.4.4: the blue Wi-Fi icon (ap_panel_wifi.r), NOT the styled */
      if (ci != NULL) { PlotCIcon(&ir, ci); DisposeCIcon(ci); }   /* control-panel Finder icon the family at 128 is */
      else FrameRect(&ir); }

    /* product name: Charcoal (system font) 24pt, plain -- the size carries the emphasis, as Apple's title does */
    TextFont(systemFont); TextSize(24); TextFace(0);
    MoveTo(kAboutTextL, 42); DrawString("\pAirPort Extreme");

    /* version line: Charcoal 12pt -- the control panel's version, then the loaded extension's build (k216...) */
    TextSize(12);
    t[0] = 0;
    PCatC(t, "Version " kPanelVersion);
    if (gHaveSnap) {
        Str255 num; short i;
        PCatC(t, " (Build k");
        NumToString((long)gSnap.driverBuild, num);
        for (i = 1; i <= num[0] && t[0] < 255; i++) t[++t[0]] = num[i];
        PCatC(t, ")");
    }
    MoveTo(kAboutTextL, 62); DrawString(t);

    /* the framed white description well. ⚠ keep the background white until the text is drawn, or DrawString /
     * TETextBox stamp the Platinum grey back in behind each glyph run (the Bluetooth About's own bug note). */
    SetRect(&box, kAboutTextL, 74, kAboutBoxR, 214);   /* 1.4.4: taller well for the added paragraph */
    { RGBColor saveBk; GetBackColor(&saveBk);
      BackColor(whiteColor);
      EraseRect(&box);
      FrameRect(&box);
      TextFont(applFont); TextSize(10); TextFace(0);
      tr = box; InsetRect(&tr, 10, 10);
      TETextBox((Ptr)desc, (long)strlen(desc), &tr, teFlushDefault);
      RGBBackColor(&saveBk);        /* restore the Platinum grey only now, with the well finished */
    }

    /* copyright line: applFont 10, the same face and size as the body, as Apple's copyright line is */
    TextFont(applFont); TextSize(10); TextFace(0);
    MoveTo(kAboutTextL, 232); DrawString("\pCopyright \251 2026 UnexpectedBomb");

    if (okBtn != NULL) Draw1Control(okBtn);   /* last, so the Appearance default ring is not overpainted */
}

/* A small movable-modal window with its own event loop (the Bluetooth About's shape): no DLOG resource, and a
 * window that loses and regains the foreground redraws itself and the panel behind it. */
static void DoAbout(void)
{
    Rect          r, ok;
    WindowPtr     w;
    EventRecord   e;
    ControlHandle okBtn = NULL;
    Boolean       go = true;

    SetRect(&r, 0, 0, 476, 286);       /* 1.4.3: Apple's About-AirPort window width; 1.4.4: its height too */
    OffsetRect(&r, (qd.screenBits.bounds.right - 476) / 2, (qd.screenBits.bounds.bottom - 286) / 3);
    w = NewCWindow(NULL, &r, "\pAbout AirPort Extreme", true, movableDBoxProc, (WindowPtr)-1L, false, 0);
    if (w == NULL) return;
    SetPort(w);
    (void)SetThemeWindowBackground(w, kThemeBrushDialogBackgroundActive, false);

    SetRect(&ok, 386, 238, 456, 258);  /* right edge aligns with the widened box (kAboutBoxR) */
    okBtn = NewControl(w, &ok, "\pOK", true, 0, 0, 1, kControlPushButtonProc, 0);
    if (okBtn != NULL) {
        Boolean isDef = true;
        (void)SetControlData(okBtn, kControlEntireControl, kControlPushButtonDefaultTag, sizeof(isDef), (Ptr)&isDef);
    }
    DrawAboutContent(w, okBtn);

    while (go) {
        if (WaitNextEvent(mDownMask | keyDownMask | updateMask | activMask, &e, 10, NULL)) {
            switch (e.what) {
            case updateEvt:
                if ((WindowPtr)e.message == w) { BeginUpdate(w); DrawAboutContent(w, okBtn); EndUpdate(w); }
                else if ((WindowPtr)e.message == gWin) DoUpdate();
                break;
            case activateEvt:
                if ((WindowPtr)e.message == w) { SetPort(w);
                    if (okBtn != NULL) HiliteControl(okBtn, (e.modifiers & activeFlag) ? 0 : 255); }
                break;
            case keyDown:
            case autoKey: {
                unsigned char c = (unsigned char)(e.message & charCodeMask);
                if (c == '\r' || c == 3 || c == 0x1B) go = false;   /* Return / Enter / Esc dismiss, like OK */
                break; }
            case mouseDown: {
                WindowPtr hw; short part = FindWindow(e.where, &hw);
                if (part == inDrag && hw == w) DragWindow(w, e.where, &qd.screenBits.bounds);
                else if (part == inContent && hw == w) {
                    ControlHandle cc; Point p = e.where; GlobalToLocal(&p);
                    if (FindControl(p, w, &cc) && cc == okBtn && TrackControl(cc, p, NULL) != 0) go = false;
                }
                break; }
            }
        }
    }
    DisposeWindow(w);                                   /* disposes its OK control too */
    if (gWin != NULL) { SetPort(gWin); InvalRect(&gWin->portRect); }   /* repaint the panel under where it was */
}

static void DoMenu(long sel)
{
    short menu = HiWord(sel), item = LoWord(sel);
    switch (menu) {
    case kMApple:
        if (item == kAboutItem) DoAbout();
        else if (item > 2) {
            Str255 nm;
            GetMenuItemText(GetMenuHandle(kMApple), item, nm);
            (void)OpenDeskAcc(nm);
        }
        break;
    case kMFile:
        if (item == kCloseItem || item == kQuitItem) gDone = true;   /* a control panel quits on close */
        break;
    default:
        break;
    }
    HiliteMenu(0);
}

/* ---- Apple events ------------------------------------------------------------------------------------- */

/* The SIZE resource says isHighLevelEventAware, so the Finder asks us to quit at Shut Down and Restart with
 * a 'quit' event rather than by choosing Quit from our menu -- and a panel that ignored it would hold up
 * the whole shutdown. */
static pascal OSErr AeQuit(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e; (void)reply; (void)refcon;
    gDone = true;
    return noErr;
}

static pascal OSErr AeOpenApp(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e; (void)reply; (void)refcon;
    return noErr;
}

/* The panel has no documents: say so rather than claim to have opened or printed them. */
static pascal OSErr AeNoDocs(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e; (void)reply; (void)refcon;
    return errAEEventNotHandled;
}

static void InstallAppleEvents(void)
{
    long resp = 0;
    if (Gestalt(gestaltAppleEventsAttr, &resp) != noErr) return;
    /* ⚠ A failed install is left alone deliberately: with no 'quit' handler the Finder falls back to
     * asking again and then reporting that the panel could not quit -- visible, and the user can quit it by
     * hand. There is nothing better this code could do about a system that cannot install a handler. */
    (void)AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(AeOpenApp), 0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,   NewAEEventHandlerUPP(AeNoDocs),  0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,  NewAEEventHandlerUPP(AeNoDocs),  0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(AeQuit),    0, false);
}

/* ---- events ------------------------------------------------------------------------------------------- */

/* k207: "Turn AirPort Off" / "Turn AirPort On" -- post the command through the block's mailbox, then wait
 * for the driver in Poll. The button is dimmed while we wait, and whenever no driver could carry it out. */
static void DoPower(void)
{
    ApStatBlock *b = (ApStatBlock *)FindBlock();
    if (b == NULL || gCmdPending != kPendNone || !gShown.powerEnabled) return;
    gCmdPending = (gShown.ui == kUiOff) ? kPendOn : kPendOff;
    gCmdSeq  = ApStatPost(b, kApCmdPower, (gCmdPending == kPendOn) ? 1u : 0u);
    gCmdTick = TickCount();
    Poll(true);                               /* "Waiting for AirPort to turn off..." at once */
}

/* The driver answered "failed" (it could not reach the card): say so, once, outside the poll. */
static void ReportCommandFailure(void)
{
    Str255 msg, expl;
    SInt16 hit = 0;
    msg[0] = 0;
    PCatC(msg, (gCmdFailed == kPendOn) ? "AirPort could not be turned on." : "AirPort could not be turned off.");
    expl[0] = 0;
    PCatC(expl, "The AirPort Extreme driver could not reach the card. The AirPort Driver Log in the System "
                "Folder has the details.");
    gCmdFailed = kPendNone;
    (void)StandardAlert(kAlertCautionAlert, msg, expl, NULL, &hit);
}

/* k213: forget the current network -- remove its saved password, after a confirm. The network is the one the
 * driver is on (gSnap's SSID); the button is live only when that network is saved (gCurSaved). To forget a
 * network you are not on, choose it from the popup first. */
static void DoForget(void)
{
    ApStU8 ssid[32]; short n, i;
    AlertStdAlertParamRec p; SInt16 hit = 0;
    Str255 m, x;
    if (!gHaveSnap || !gCurSaved) return;
    n = (short)((gSnap.ssidLen > 32) ? 32 : gSnap.ssidLen);
    if (n <= 0) return;
    for (i = 0; i < n; i++) ssid[i] = gSnap.ssid[i];
    m[0] = 0; PCatC(m, "Forget this network?");
    x[0] = 0; PCatC(x, "The saved password for \322"); ApVCatSsid(x, ssid, n);
    PCatC(x, "\323 will be removed. You will need to enter it again to rejoin this network.");
    memset(&p, 0, sizeof(p));
    p.movable       = true;
    p.defaultText   = (StringPtr)"\pForget";
    p.cancelText    = (StringPtr)"\pCancel";
    p.defaultButton = kAlertStdAlertOKButton;
    p.cancelButton  = kAlertStdAlertCancelButton;
    if (StandardAlert(kAlertCautionAlert, m, x, &p, &hit) != noErr) return;
    if (hit != kAlertStdAlertOKButton) return;          /* Cancel */
    if (!gKnownLoaded) ApPanelLoadKnown();
    ApKnownRemove(&gKnownDb, ssid, (ApStU8)n);
    (void)ApPanelSaveKnown();
    gVerifyLen = 0; gJoinSeq = 0;                        /* not watching a join any more */
    gCurSaved = false; ApplyHilites();                  /* dim the button at once */
}

static void DoContentClick(Point where)
{
    ControlHandle c = NULL;
    Point         pt = where;
    SetPort(gWin);
    GlobalToLocal(&pt);
    if (gMode != kModeStatus && PtInRect(pt, &kTriLbl)) { ToggleSettings(); return; }

    /* ⚠ k211d: the "Choose network" popup, driven like the Control Strip does -- PopUpMenuSelect on the menu
     * DIRECTLY, not through the popupMenuProc control. The control path (FindControl -> TrackControl -> CDEF)
     * never dropped the list on hardware across four builds, because FindControl ignores a dimmed control and
     * the popup's enable state goes stale; the CSM's direct-menu track always worked. PtInRect (not
     * FindControl) so the control's hilite cannot swallow the click. */
    if (gMode == kModeFull && gNetPop != NULL && IsControlVisible(gNetPop)
        && gShown.ui != kUiNoDriver && gShown.ui != kUiOff && gShown.ui != kUiStalled
        && PtInRect(pt, &(**gNetPop).contrlRect)) {
        Rect  r = (**gNetPop).contrlRect;
        Point tl;
        long  res;
        short item;
        gNetMenu = ApNetMenu();                           /* the control's own, inserted menu */
        tl.v = r.top; tl.h = (short)(r.left + 1);
        LocalToGlobal(&tl);
        res  = (gNetMenu != NULL) ? PopUpMenuSelect(gNetMenu, tl.v, tl.h, (short)GetControlValue(gNetPop)) : 0;
        item = (short)(res & 0xFFFF);
        if (item >= 2 && item < (short)(kApScanMax + 2) && gNetItemScan[item] >= 0
            && gNetItemScan[item] < (short)gScanSnap.count)
            ApPanelChoose(&gScanSnap.items[gNetItemScan[item]]);
        if (gNetPop != NULL) SetControlValue(gNetPop, 1); /* settle back to the current network */
        return;
    }

    if (FindControl(pt, gWin, &c) == 0 || c == NULL) return;
    if (TrackControl(c, pt, NULL) == 0) return;
    if (c == gTri) ToggleSettings();
    else if (c == gPowerBtn) DoPower();
    else if (c == gForgetBtn) DoForget();        /* k213 */
    /* Any other control (dimmed ones, labels) is never actioned here. */
}

static void DoMouseDown(const EventRecord *e)
{
    WindowPtr w;
    short     part = FindWindow(e->where, &w);
    switch (part) {
    case inMenuBar:   DoMenu(MenuSelect(e->where)); break;
    case inSysWindow: SystemClick(e, w); break;
    case inDrag:      if (w == gWin) DragWindow(gWin, e->where, &qd.screenBits.bounds); break;
    case inGoAway:    if (w == gWin && TrackGoAway(gWin, e->where)) gDone = true; break;
    case inContent:
        if (w != gWin) break;
        if (gWin != FrontWindow()) SelectWindow(gWin);
        else DoContentClick(e->where);
        break;
    default: break;
    }
}

int main(void)
{
    Rect        b;
    short       top, left;
    EventRecord evt;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    if (RegisterAppearanceClient() != noErr) gThemeOK = false;
    InstallAppleEvents();
    SetUpMenus();
    gCard = FindCard();
    gBeatSeen = TickCount();
#ifdef AP_PANEL_DEMO
    gDemoT0 = TickCount();
    gCard = 1;                  /* so the no-block phase reads "not running" rather than "no card" */
#endif

    left = (short)((qd.screenBits.bounds.right - kWinW) / 2);
    top  = (short)(GetMBarHeight() + 48);
#ifdef AP_PANEL_DEMO
    left = (short)(qd.screenBits.bounds.right - kWinW - 16);   /* beside a real panel, not on top of it */
#endif
    SetRect(&b, left, top, (short)(left + kWinW), (short)(top + kWinHFull));
    /* ⚠ NewCWindow, NOT NewWindow -- see the header. Created hidden, so the first thing the user sees is
     * the finished window at its right height, not a resize. */
    gWin = NewCWindow(NULL, &b, "\pAirPort Extreme", false, noGrowDocProc, (WindowPtr)-1L, true, 0);
    if (gWin == NULL) return 1;
    SetPort(gWin);
    (void)SetThemeWindowBackground(gWin, kThemeBrushDialogBackgroundActive, false);  /* ErasePlatinum backs it up */

    MakeControls();
    Poll(true);                 /* the state, the height and the view, before the window is shown */
    ApplyHilites();
    ShowWindow(gWin);

    while (!gDone) {
        Boolean arrows = (Boolean)(gArrows != NULL && IsControlVisible(gArrows));
        Boolean got = WaitNextEvent(everyEvent, &evt, arrows ? 2 : 15, NULL);
        Poll(false);
        if (gCmdFailed != kPendNone) ReportCommandFailure();
        if (arrows) IdleControls(gWin);           /* the chasing arrows turn only when idled */
        if (!got) continue;
        switch (evt.what) {
        case mouseDown:
            DoMouseDown(&evt);
            break;
        case keyDown:
        case autoKey:
            if (evt.modifiers & cmdKey) DoMenu(MenuKey((short)(evt.message & charCodeMask)));
            break;
        case updateEvt:
            if ((WindowPtr)evt.message == gWin) DoUpdate();
            break;
        case activateEvt:
            if ((WindowPtr)evt.message == gWin) DoActivate((Boolean)((evt.modifiers & activeFlag) != 0));
            break;
        case osEvt:
            /* Back in front: the user may have been in the TCP/IP control panel, so read its name now. */
            if (((evt.message >> 24) & 0xFF) == suspendResumeMessage && (evt.message & resumeFlag))
                Poll(true);
            break;
        case kHighLevelEvent:
            (void)AEProcessAppleEvent(&evt);      /* errors go back to the sender in the reply */
            break;
        default:
            break;
        }
    }

#ifdef AP_PANEL_DEMO
    DemoRetire();               /* the strip module then shows "no driver", as it would with none */
#endif
    DisposeWindow(gWin);
    return 0;
}
