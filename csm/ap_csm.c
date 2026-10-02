/*
 *  ap_csm.c -- the AirPort Extreme Control Strip module (1.3: the user's glyph, Apple's own five dots,
 *  and Turn AirPort Off / On).
 *
 *  Shows the signal of the network the AirPort Extreme driver is on, in the AirPort app's five levels
 *  ("Strong" ... "Out of Range", STR# 8007 of Apple's AirPort app), and offers a menu shaped like
 *  Apple's own AirPort Control Strip module (UI-PRIOR-ART.md, and the user's screenshot "AP CSM
 *  clicked"): a status header, Open AirPort Extreme, the network, and -- in later phases -- the scan
 *  list and Turn AirPort Off.
 *
 *  ★ WHERE THE DATA COMES FROM: the driver's status block (probe/ap_status.h), published as the
 *  Gestalt value 'APXe'. Gestalt, validate, copy with ApStatRead -- no files, no CFM connection into
 *  the driver. The Bluetooth module read files the driver wrote and had to test their freshness
 *  against the boot time; this block is live, and a stopped heartbeat says the driver is gone.
 *
 *  ★ BUILT ON THE BLUETOOTH MODULE'S PROVEN SKELETON (bluetooth/csm/bt_csm.c) and its hard lessons,
 *  each of which cost a hardware cycle there or in the CPU-temp module:
 *    - all state lives in a system-heap refCon; a mutable global made the whole strip fail to load;
 *    - the cell repaints ITSELF in sdevPeriodicTickle (the strip only redraws on a width change), and
 *      only through a cached port that PortLooksAlive has just vetted -- drawing through a dead one
 *      scribbled on the system heap;
 *    - the menu is tracked on mouse-DOWN (sdevDontAutoTrack), item numbers are computed, never literal;
 *    - an item that cannot work is left out rather than shown disabled-but-lying.
 *
 *  ★ THE CELL (1.1) IS APPLE'S, WITH THE USER'S GLYPH. Apple's AirPort module draws one of its PICTs
 *  130-136 ('off', 'on - 0' ... 'on - 5'), decoded and measured for this: 96 x 16, the AirPort glyph at
 *  x 5-20, an etched separator at x 26-27, five dots every 11 px from x 33 (green when lit, grey when not,
 *  hollow rings when AirPort is off), the arrow at x 90-93. Ours draws the same layout, with the user's
 *  own glyph ("airport-16x16-16bit.png", so this module is told apart from Apple's at a glance) and, at
 *  the user's request (1.2), APPLE'S OWN DOT BITMAPS, lifted from those PICTs. art/make-art.py rebuilds
 *  all seven of Apple's pictures from this layout (Apple's glyph in the glyph slot) with zero differing
 *  pixels, so the geometry below is proven against Apple's bitmaps. The pixel tables (ap_csm_art.h) are
 *  blended against the strip's face colour sampled at draw time and set with SetCPixel: exact colours at
 *  thousands or millions, the nearest at 256. An icon suite would have been 8-bit even on a millions
 *  screen, and the user's glyph is a 16-bit image.
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Menus.h>
#include <Files.h>
#include <Folders.h>
#include <TextUtils.h>
#include <Events.h>
#include <Processes.h>
#include <Gestalt.h>
#include <ControlStrip.h>
#include <string.h>
#include "ap_status.h"
#include "ap_scanlist.h"                /* k209: the network list the driver publishes ('APXs') */
#include "ap_known.h"                   /* k216: saved networks -- the CSM joins one directly, like Apple's CSM */
#include "ap_csm_art.h"

/* k216: the known-networks file -- the SAME file the panel and driver use (ap_panel.c kNetFileName). The CSM
 * reads it to tell which listed networks are saved, and writes it (mark most-recently-used) when you pick one,
 * exactly as the panel does, so the existing join command (kApCmdJoin -> most-recently-used) joins THAT one. */
#define kNetFileName   "\pAirPort Extreme Known Networks"   /* 30 chars -- fits HFS's 31 limit */
#define kNetFileTmp    "\pAirPort Extreme Nets (tmp)"       /* 26 chars; > 31 fails FSpCreate (the k211e bug) */

#define kPanelCreator  'APXp'        /* the AirPort Extreme control panel (an application) */
#define kRefreshTicks  30            /* read the block twice a second: it is live memory, not a file */
#define kStallTicks    240           /* heartbeat unchanged this long (4 s; it ticks 4/s) = not running */

/* What the cell and the menu show -- derived from the block, one place. */
enum { kUiNoDriver = 0, kUiOff, kUiNoNet, kUiSearching, kUiJoining, kUiUp, kUiBadKey /* k213: wrong password */ };

typedef struct {
    GrafPtr       cachePort;            /* from the last real sdevDrawStatus */
    Rect          cacheRect;
    Boolean       haveCache;
    unsigned long lastRefresh;
    ApStatBlock   snap;                 /* last consistent copy of the driver's block */
    Boolean       haveSnap;
    ApScanBlock   scan;                 /* k209: last consistent copy of the network list */
    Boolean       haveScan;
    short         ui;                   /* kUi* */
    short         shownUi, shownQual;   /* what the cell last showed */
    ApStU32       lastBeat;             /* heartbeat stall detection */
    unsigned long beatSeen;
    ApKnownDb     known;                /* k216: saved networks (loaded when the menu is built) */
    Boolean       knownLoaded;
} ApCsmState;

/* k216: menu item number -> scan index, for the selectable network items (as the panel's gNetItemScan does).
 * Single-threaded: DoClick fills it when building the menu and reads it after the selection. Sized to hold
 * every possible item number: the ~7 fixed items (header, separators, Open, current, power) plus up to
 * kApScanMax network lines -- separators count as menu items, so the index can run past kApScanMax. */
#define kCsmMaxItem (kApScanMax + 10)
static short gNetItemScan[kCsmMaxItem];

/* ---- the driver's block ---------------------------------------------------------- */

static const ApStatBlock *FindBlock(void)
{
    long v = 0;
    if (Gestalt((OSType)kApStatSelector, &v) != noErr || v == 0) return NULL;
    return ApStatValid((const ApStatBlock *)v) ? (const ApStatBlock *)v : NULL;
}

/* k209: the network-list block, found the same way. */
static const ApScanBlock *FindScanBlock(void)
{
    long v = 0;
    if (Gestalt((OSType)kApScanSelector, &v) != noErr || v == 0) return NULL;
    return ApScanValid((const ApScanBlock *)v) ? (const ApScanBlock *)v : NULL;
}

static void Refresh(ApCsmState *st)
{
    const ApStatBlock *b = FindBlock();
    unsigned long now = TickCount();
    st->ui = kUiNoDriver;
    if (b == NULL) { st->haveSnap = false; return; }
    (void)ApStatRead(b, &st->snap);            /* a torn read is at worst one stale frame */
    st->haveSnap = true;
    if (!st->snap.alive) return;
    /* ⚠ A block that says "alive" but whose heartbeat has stopped is a driver that has stopped
     * running (the module can be torn down or wedged without the flag ever being cleared). */
    if (st->snap.heartbeat != st->lastBeat) { st->lastBeat = st->snap.heartbeat; st->beatSeen = now; }
    else if (now - st->beatSeen > kStallTicks) return;
    switch (st->snap.linkState) {
    case kApLinkOff:       st->ui = kUiOff;       break;
    case kApLinkIdle:      st->ui = kUiNoNet;     break;   /* k211: on, running, no network chosen yet */
    case kApLinkSearching: st->ui = kUiSearching; break;
    case kApLinkJoining:   st->ui = kUiJoining;   break;
    case kApLinkUp:        st->ui = kUiUp;        break;
    case kApLinkBadKey:    st->ui = kUiBadKey;    break;   /* k213: the base station rejected our password */
    default:               st->ui = kUiNoDriver;  break;
    }
}

static short CurrentQual(const ApCsmState *st)
{
    if (st == NULL || st->ui != kUiUp) return kApQualOutOfRange;
    return (st->snap.quality <= kApQualStrong) ? (short)st->snap.quality : kApQualOutOfRange;
}

/* Apple's own words for the five levels (STR# 8007 of the AirPort app). */
static const char *QualWord(short q)
{
    switch (q) {
    case kApQualStrong:  return "Strong";
    case kApQualGood:    return "Good";
    case kApQualAverage: return "Average";
    case kApQualWeak:    return "Weak";
    default:             return "Out of Range";
    }
}

static void PStrAppendC(Str255 s, const char *p)
{
    while (*p && s[0] < 250) s[++s[0]] = (unsigned char)*p++;
}

static void PStrAppendBytes(Str255 s, const ApStU8 *p, short n)
{
    short i;
    for (i = 0; i < n && s[0] < 250; i++) s[++s[0]] = p[i];
}

/* ---- drawing -------------------------------------------------------------------- */

/* Apple's cell geometry, from its PICTs 130-136 (see the header note). ⚠ art/make-art.py carries the same
 * numbers (CELL_W ... ARROW_Y) and proves them by rebuilding Apple's pictures: change the two together. */
#define kCellW     96
#define kCellH     16
#define kGlyphX    5
#define kSepX      26              /* #555555, then #FFFFFF at kSepX + 1 */
#define kDot0X     33
#define kDotStep   11
#define kDots      5
#define kDotY      3               /* Apple's 8x8 dot bitmaps start on row 3 (the lit dot's halo) */
#define kArrowX    90
#define kArrowY    4
#define kArrowH    8               /* 1,2,3,4,4,3,2,1 wide -- Apple's marker, and Battery Monitor's */

/* ★★★★ bt_csm.c / cpu-temp v1.8, verbatim in intent: NEVER draw through the cached port without
 * proving it is still a port. A dangling one made QuickDraw write pixels through a freed PixMap chain
 * -- a silent system-heap scribbler. Structural checks; any failure clears the cache until the next
 * real sdevDrawStatus re-arms it. */
static Boolean PortLooksAlive(GrafPtr p, const Rect *cell)
{
    UInt32 a = (UInt32)p, h, m, base;
    SInt16 ver, rb;
    volatile SInt16 *pr;
    if (a < 0x1000UL || a >= 0x60000000UL || (a & 1)) return false;
    ver = *(volatile SInt16 *)((char *)p + 6);          /* portVersion: CGrafPort sets both top bits */
    if ((ver & (SInt16)0xC000) != (SInt16)0xC000) return false;
    h = *(volatile UInt32 *)((char *)p + 2);            /* portPixMap (a Handle) */
    if (h < 0x1000UL || h >= 0x60000000UL || (h & 3)) return false;
    m = *(volatile UInt32 *)h;                          /* master pointer -> PixMap */
    if (m < 0x1000UL || m >= 0x60000000UL || (m & 1)) return false;
    rb = *(volatile SInt16 *)((char *)m + 4);           /* PixMap rowBytes: flag bit must be set */
    if (!(rb & (SInt16)0x8000)) return false;
    base = *(volatile UInt32 *)m;                       /* baseAddr */
    if (base < 0x1000UL) return false;
    pr = (volatile SInt16 *)((char *)p + 16);           /* portRect {top,left,bottom,right} */
    if (cell->top < pr[0] || cell->left < pr[1] || cell->bottom > pr[2] || cell->right > pr[3])
        return false;
    return true;
}

static void DrawArrow(short x, short y)
{
    RGBColor blk;
    short    i, w;
    blk.red = blk.green = blk.blue = 0;
    RGBForeColor(&blk);
    for (i = 0; i < kArrowH; i++) {
        w = (i < kArrowH / 2) ? (short)(i + 1) : (short)(kArrowH - i);
        MoveTo(x, (short)(y + i));
        Line((short)(w - 1), 0);
    }
}

static void VLine(short x, short top, unsigned short grey)
{
    RGBColor c;
    c.red = c.green = c.blue = grey;
    RGBForeColor(&c);
    MoveTo(x, top);
    LineTo(x, (short)(top + kCellH - 1));
}

/* One sprite from ap_csm_art.h, each pixel alpha-blended against `face` and set with SetCPixel, so the
 * colours are exact at thousands or millions and the nearest at 256. dim: the glyph for "no driver" --
 * grey, then halfway to the face, the way Apple's module greys its glyph for an error (its PICT 137).
 * ⚠ art/make-art.py's preview composes the same way; change the two together. */
static void Blit(const ApCsmPx *px, short w, short h, short x0, short y0, const RGBColor *face, Boolean dim)
{
    unsigned fr = face->red >> 8, fg = face->green >> 8, fb = face->blue >> 8;
    short    x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const ApCsmPx *p = &px[y * w + x];
            unsigned r = p->r, g = p->g, b = p->b, a = p->a;
            RGBColor c;
            if (a == 0) continue;
            if (dim) {
                unsigned lum = (r * 30u + g * 59u + b * 11u) / 100u;
                r = (lum + fr) / 2u; g = (lum + fg) / 2u; b = (lum + fb) / 2u;
            }
            r = (r * a + fr * (255u - a)) / 255u;
            g = (g * a + fg * (255u - a)) / 255u;
            b = (b * a + fb * (255u - a)) / 255u;
            c.red = (unsigned short)(r * 257u); c.green = (unsigned short)(g * 257u); c.blue = (unsigned short)(b * 257u);
            SetCPixel((short)(x0 + x), (short)(y0 + y), &c);
        }
    }
}

/* Apple's six pictures, as states: no network yet = five grey dots ('on - 0'); joined = one green dot per
 * quality level, Out of Range 1 ... Strong 5 ('on - 1' ... 'on - 5'); AirPort off = hollow rings ('off');
 * no driver = hollow rings and the glyph greyed. */
static short LitDots(const ApCsmState *st)
{
    return (st != NULL && st->ui == kUiUp) ? (short)(CurrentQual(st) + 1) : 0;
}

/* Draw into rect r of the CURRENT port. erase: we are repainting ourselves in the tickle, so the strip
 * has not cleared the cell for us. */
static void DrawCell(ApCsmState *st, const Rect *r, Boolean erase)
{
    short    cellW = (short)(r->right - r->left), cellH = (short)(r->bottom - r->top);
    short    x0 = r->left, top = r->top, lit = LitDots(st), i;
    Boolean  dead = (Boolean)(st == NULL || st->ui == kUiNoDriver);
    Boolean  rings = (Boolean)(dead || st->ui == kUiOff);
    RGBColor face, save;

    if (cellW > kCellW) x0 = (short)(x0 + (cellW - kCellW) / 2);
    if (cellH > kCellH) top = (short)(top + (cellH - kCellH) / 2);
    GetForeColor(&save);
    if (erase) EraseRect(r);
    /* The strip's own face, from a pixel of the left margin nothing of ours is drawn in: the glyph's
     * antialiased edge and the dots' halo blend into whatever face the theme gives the strip. */
    GetCPixel((short)(x0 + 1), (short)(top + kCellH / 2), &face);

    Blit(kCellGlyph, kGlyphW, kGlyphH, (short)(x0 + kGlyphX), top, &face, dead);
    VLine((short)(x0 + kSepX), top, 0x5555);
    VLine((short)(x0 + kSepX + 1), top, 0xFFFF);
    for (i = 0; i < kDots; i++)
        Blit(rings ? kDotRing : (i < lit ? kDotLit : kDotDim), kDotW, kDotH,
             (short)(x0 + kDot0X + i * kDotStep), (short)(top + kDotY), &face, false);
    DrawArrow((short)(x0 + kArrowX), (short)(top + kArrowY));
    RGBForeColor(&save);
}

/* ⭐ FIXED WIDTH (bt_csm.c design note): a width that tracked the content would re-lay-out every
 * module to our right on each change, and the self-draw path is needed anyway. Apple's cell is 96. */
static short CellWidth(void)
{
    return kCellW;
}

/* ---- opening the control panel --------------------------------------------------- */

/* Scanned for in the Control Panels folder by creator, as bt_csm.c does -- not the Desktop database,
 * whose stale entries hand back a ghost FSSpec. Bounded: a hang here hangs the whole strip. */
static Boolean FindPanel(FSSpec *out)
{
    CInfoPBRec pb;
    Str255     nm;
    short      vRefNum, i;
    long       dirID;
    if (FindFolder(kOnSystemDisk, kControlPanelFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr)
        return false;
    for (i = 1; i <= 512; i++) {
        memset(&pb, 0, sizeof(pb));
        nm[0] = 0;
        pb.hFileInfo.ioNamePtr   = nm;
        pb.hFileInfo.ioVRefNum   = vRefNum;
        pb.hFileInfo.ioDirID     = dirID;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;
        if (pb.hFileInfo.ioFlFndrInfo.fdCreator == kPanelCreator && pb.hFileInfo.ioFlFndrInfo.fdType == 'APPL')
            return (Boolean)(FSMakeFSSpec(vRefNum, dirID, nm, out) == noErr);
    }
    return false;
}

static Boolean PanelToFront(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    Str255              nm;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof(info));
        info.processInfoLength = (UInt32)sizeof(ProcessInfoRec);
        info.processName       = nm;
        info.processAppSpec    = NULL;
        if (GetProcessInformation(&psn, &info) == noErr && info.processSignature == kPanelCreator)
            return (Boolean)(SetFrontProcess(&psn) == noErr);
    }
    return false;
}

static void OpenPanel(const FSSpec *spec)
{
    LaunchParamBlockRec lpb;
    if (PanelToFront()) return;
    memset(&lpb, 0, sizeof(lpb));
    lpb.launchBlockID       = extendedBlock;
    lpb.launchEPBLength     = extendedBlockLen;
    lpb.launchControlFlags  = launchContinue | launchNoFileFlags;
    lpb.launchAppSpec       = (FSSpecPtr)spec;
    (void)LaunchApplication(&lpb);    /* only offered when FindPanel found it -- see DoClick */
}

/* ---- k216: the known-networks file (join a saved network from the strip) ---------------------------------
 * These mirror the panel's ApPanelKnownSpec/LoadKnown/SaveKnown. All task level (the menu handler), so the
 * File Manager is safe. The CSM only ever marks an EXISTING saved network most-recently-used; it never adds a
 * new one (that needs a passphrase, which an sdev cannot ask for -- those go to the panel). */
static Boolean ApCsmKnownSpec(FSSpec *spec)
{
    short vRefNum; long dirID;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr) return false;
    { OSErr e = FSMakeFSSpec(vRefNum, dirID, kNetFileName, spec);
      if (e != noErr && e != fnfErr) return false;
      return (spec->name[0] != 0); }
}

static void ApCsmLoadKnown(ApCsmState *st)
{
    static UInt8 buf[kApKnownFileSize];
    FSSpec spec; short ref; OSErr e; long n = (long)kApKnownFileSize;
    ApKnownClear(&st->known);
    st->knownLoaded = true;
    if (!ApCsmKnownSpec(&spec)) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;      /* fnfErr = no saved networks yet; db stays empty */
    e = FSRead(ref, &n, buf);
    (void)FSClose(ref);
    if (e != noErr && e != eofErr) return;
    { long i; for (i = n; i < (long)kApKnownFileSize; i++) buf[i] = 0; }
    (void)ApKnownLoad(&st->known, buf);
}

/* Write st->known back atomically (temp + FSpExchangeFiles, FSpRename fallback), exactly as the panel does so
 * the two cannot disagree about the file. Creator 'APXp' matches the panel's. Returns true on success. */
static Boolean ApCsmSaveKnown(ApCsmState *st)
{
    static UInt8 buf[kApKnownFileSize];
    FSSpec spec, tmp; short vRefNum, ref; long dirID, n = (long)kApKnownFileSize; OSErr err;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID) != noErr) return false;
    if (!ApCsmKnownSpec(&spec)) return false;
    ApKnownSave(&st->known, buf);
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

/* ---- menu ------------------------------------------------------------------------ */

#define kMenuID  1025

/* Apple's shape (the "AP CSM clicked" screenshot): a status header, Open AirPort, the networks, then
 * the radio switch. There is no scan yet, so the network section is the one network the driver is on;
 * the radio switch arrived with k207 (1.3). An item that cannot work is left out rather than shown
 * disabled-but-lying (the Bluetooth module's rule). Item numbers are computed. */
/* k209: one line per network the driver last listed. Our own is shown as the header/checked item above, so
 * it is skipped here; a network whose security this driver cannot join is listed but disabled, so the user
 * sees it and (k210) learns why. Read-only in k209: selecting a joinable one does nothing yet. Returns the
 * first and last menu item numbers covering the list (0 if none), so the click handler can tell them apart
 * from the fixed items -- though in k209 a network item is disabled and cannot be chosen. */
static void AppendNetworks(ApCsmState *st, MenuHandle m, short *firstOut, short *countOut)
{
    short i, added = 0, first = 0;
    *firstOut = 0; *countOut = 0;
    if (!st->haveScan || st->scan.count == 0) return;
    for (i = 0; i < (short)st->scan.count && i < (short)kApScanMax; i++) {
        const ApScanItem *e = &st->scan.items[i];
        Str255 s;
        short n = (short)((e->ssidLen > 32) ? 32 : e->ssidLen);
        short it;
        Boolean joinable;
        if (n == 0) continue;                                  /* a hidden network has no name to show */
        if (ApScanDupEarlier(st->scan.items, i)) continue;     /* k212: one line per SSID, as Apple does */
        /* skip the current/target network by NAME (it is the checked item above). k216: NOT by kApScanFOurs --
         * that flag is stale on the network you just left between sweeps (the k215 panel bug); the name check
         * is live, from the status block. */
        if (st->haveSnap && st->snap.ssidLen == (ApStU8)n) {
            short q, cur = 1;
            for (q = 0; q < n; q++) if (st->snap.ssid[q] != e->ssid[q]) { cur = 0; break; }
            if (cur) continue;
        }
        /* k217: a network we can actually join -- open or WPA2-Personal. ApScanSecurityJoinable is the single
         * source of truth (true for open and WPA2-Personal); everything else is listed disabled. */
        joinable = ApScanSecurityJoinable(e->security);
        if (added == 0) AppendMenu(m, "\p(-");                 /* the separator, only if a network follows */
        s[0] = 0;
        PStrAppendBytes(s, e->ssid, n);
        if (!joinable) PStrAppendC(s, "  (not supported)");
        AppendMenu(m, "\px");
        it = (short)CountMenuItems(m);
        SetMenuItemText(m, it, s);
        if (first == 0) first = it;
        if (joinable && it < (short)kCsmMaxItem) gNetItemScan[it] = i;   /* k216: selectable -> scan index */
        else DisableItem(m, it);                               /* not joinable by us: listed, dimmed */
        added++;
    }
    *firstOut = first; *countOut = added;
}

/* k216: the user chose a network from the strip menu. Saved already -> mark it most-recently-used and tell the
 * driver to join it (kApCmdJoin joins the most-recently-used saved network: the panel's exact path). Not saved
 * -> it needs a passphrase, which an sdev cannot ask for, so open the panel for it, exactly as Apple's CSM hands
 * a password-needing network off to the AirPort application. */
static void ApCsmChoose(ApCsmState *st, const ApScanItem *e, const FSSpec *panelSpec, Boolean havePanel)
{
    short n = (short)((e->ssidLen > 32) ? 32 : e->ssidLen);
    int idx;
    if (n == 0) return;
    ApCsmLoadKnown(st);
    idx = ApKnownFind(&st->known, e->ssid, (ApStU8)n);
    if (idx >= 0) {
        ApStatBlock *b = (ApStatBlock *)FindBlock();
        ApKnownTouch(&st->known, idx);                 /* make it the one kApCmdJoin will pick */
        (void)ApCsmSaveKnown(st);
        if (b != NULL && !ApStatBusy(b)) (void)ApStatPost(b, kApCmdJoin, 0u);
    } else if (havePanel) {
        OpenPanel(panelSpec);                          /* new network: the panel asks for the password */
    }
}

static void DoClick(ApCsmState *st, Rect *statusRect)
{
    MenuHandle m;
    short      chosen = 0, headItem, panelItem, netItem = 0, powerItem = 0;
    short      netFirst = 0, netCount = 0;
    Str255     s;
    Boolean    havePanel;
    FSSpec     panelSpec;

    if (st != NULL) Refresh(st);
    /* Read the network list now (not on every tickle -- it is 1.5 KB), and ask the driver for a fresh scan
     * so the next open is current; the scan runs after this menu closes, so it never freezes the menu. */
    if (st != NULL) {
        const ApScanBlock *sb = FindScanBlock();
        st->haveScan = (sb != NULL && ApScanRead(sb, &st->scan));
        /* Ask for a fresh scan only when the list is empty or stale (older than ~60 s of driver heartbeat,
         * 4/s), so opening the menu repeatedly does not trigger a scan -- and hence a ~1.2 s freeze -- each
         * time. The scan runs after this menu closes; the next open shows the result. */
        { ApStatBlock *b = (ApStatBlock *)FindBlock();
          Boolean stale = (!st->haveScan || st->scan.scanSeq == 0
                           || (st->haveSnap && (ApStU32)(st->snap.heartbeat - st->scan.scanTick) > 240u));
          if (b != NULL && stale && st->ui != kUiNoDriver && st->ui != kUiOff && !ApStatBusy(b))
              (void)ApStatPost(b, kApCmdScan, 0u); }
    }
    m = NewMenu(kMenuID, "\pAirPort Extreme");
    if (m == NULL) return;
    { short k; for (k = 0; k < (short)kCsmMaxItem; k++) gNetItemScan[k] = -1; }   /* k216: no network item yet */

    /* the status header: information, not a control -- disabled and italic, as Apple's is */
    headItem = 1;
    s[0] = 0;
    PStrAppendC(s, "AirPort Extreme: ");
    PStrAppendC(s, (st == NULL || st->ui == kUiNoDriver) ? "Not Available"
                   : (st->ui == kUiOff) ? "Off" : "On");
    AppendMenu(m, "\px");
    SetMenuItemText(m, headItem, s);
    SetItemStyle(m, headItem, italic);
    DisableItem(m, headItem);
    AppendMenu(m, "\p(-");

    havePanel = FindPanel(&panelSpec);
    panelItem = (short)(CountMenuItems(m) + 1);
    AppendMenu(m, "\pOpen AirPort Extreme");
    if (!havePanel) DisableItem(m, panelItem);

    /* the network the driver is on: checked when connected, "connecting" while it tries */
    if (st != NULL && st->haveSnap && st->ui != kUiNoDriver && st->ui != kUiOff && st->snap.ssidLen > 0) {
        short n = (short)((st->snap.ssidLen > 32) ? 32 : st->snap.ssidLen);
        AppendMenu(m, "\p(-");
        netItem = (short)(CountMenuItems(m) + 1);
        s[0] = 0;
        PStrAppendBytes(s, st->snap.ssid, n);
        if (st->ui == kUiBadKey)   PStrAppendC(s, "  (wrong password)");   /* k213 */
        else if (st->ui != kUiUp)  PStrAppendC(s, "  (connecting\311)");
        AppendMenu(m, "\px");
        SetMenuItemText(m, netItem, s);
        if (st->ui == kUiUp) SetItemMark(m, netItem, (short)sdevMenuItemMark);
        DisableItem(m, netItem);          /* phase 1: nothing to switch to yet */
    }

    /* k216: the other networks the driver heard -- now SELECTABLE (a joinable one joins; see ApCsmChoose). */
    if (st != NULL && st->ui != kUiNoDriver && st->ui != kUiOff)
        AppendNetworks(st, m, &netFirst, &netCount);
    (void)netFirst; (void)netCount;       /* the item->scan map (gNetItemScan) is how a choice is routed now */

    /* k207: Apple's last item -- "Turn AirPort Off", or On -- posted to the driver through the status block's
     * mailbox (ap_status.h). Offered only while a driver is running its heartbeat (kUiNoDriver covers absent,
     * closed and stalled alike), and dimmed while an earlier command is still being carried out. */
    if (st != NULL && st->haveSnap && st->ui != kUiNoDriver) {
        const ApStatBlock *b = FindBlock();
        AppendMenu(m, "\p(-");
        powerItem = (short)(CountMenuItems(m) + 1);
        AppendMenu(m, (st->ui == kUiOff) ? "\pTurn AirPort On" : "\pTurn AirPort Off");
        if (b == NULL || ApStatBusy(b)) DisableItem(m, powerItem);
    }

    InsertMenu(m, hierMenu);
    if (statusRect != NULL) {
        long r = SBTrackPopupMenu(statusRect, m);
        chosen = (short)(r & 0xFFFF);
    }
    DeleteMenu(kMenuID);
    DisposeMenu(m);

    if (chosen == panelItem && havePanel) OpenPanel(&panelSpec);
    else if (powerItem != 0 && chosen == powerItem) {
        ApStatBlock *b = (ApStatBlock *)FindBlock();
        if (b != NULL && !ApStatBusy(b)) (void)ApStatPost(b, kApCmdPower, (st->ui == kUiOff) ? 1u : 0u);
        /* The cell follows by itself: the next tickle reads Off (rings) or the rejoin's progress. */
    }
    /* k216: a network was chosen from the list -> join it (saved) or open the panel for its password (new). */
    else if (st != NULL && chosen > 0 && chosen < (short)kCsmMaxItem && gNetItemScan[chosen] >= 0
             && gNetItemScan[chosen] < (short)st->scan.count)
        ApCsmChoose(st, &st->scan.items[gNetItemScan[chosen]], &panelSpec, havePanel);
}

/* Apple's four balloon states (STR# 256 of its module), reworded for what this build can do. */
static void ShowHelp(ApCsmState *st, Rect *statusRect)
{
    Str255 s;
    s[0] = 0;
    if (st == NULL || st->ui == kUiNoDriver) {
        PStrAppendC(s, "The AirPort Extreme driver is not running, so no AirPort status is available.");
    } else if (st->ui == kUiOff) {
        PStrAppendC(s, "AirPort communication is currently turned off. "
                       "To open the AirPort Extreme control panel, click here.");
    } else if (st->ui == kUiUp) {
        /* k216: you can now pick a different network straight from this menu (Apple's wording). */
        PStrAppendC(s, "To select an AirPort network to join, turn AirPort off, or open its control panel, "
                       "click here. The quality of the current AirPort network is \322");
        PStrAppendC(s, QualWord(CurrentQual(st)));
        PStrAppendC(s, "\323.");
    } else if (st->ui == kUiNoNet) {
        /* k216: the strip now lets you choose a network from this menu (saved ones join straight away; a new
         * one opens the panel for its password), as Apple's AirPort strip did. */
        PStrAppendC(s, "No AirPort network is selected. To join one, click here and choose it from the menu.");
    } else if (st->ui == kUiBadKey) {
        /* k213: the password was rejected. Re-entering it needs the passphrase dialog, which lives in the
         * panel (an sdev cannot show one), so point there. */
        PStrAppendC(s, "The password for this AirPort network is incorrect. To enter it again, open the "
                       "AirPort Extreme control panel by clicking here.");
    } else {
        PStrAppendC(s, "AirPort Extreme is looking for its network. "
                       "To open the AirPort Extreme control panel, click here.");
    }
    SBShowHelpString(statusRect, s);
}

/* ---- entry point ------------------------------------------------------------------- */

/* ⚠ VERIFIED SIGNATURE (bt_csm.c; Apple patent US6493002), not in ControlStrip.h: four discrete args,
 * no EventRecord; `params` is the refCon returned from sdevInitModule. */
pascal long ControlStripModule(long message, long params, Rect *statusRect, GrafPtr statusPort)
{
    ApCsmState *st = (ApCsmState *)params;

    switch (message) {
    case sdevInitModule: {
        ApCsmState *ns = (ApCsmState *)NewPtrSys((Size)sizeof(ApCsmState));
        if (ns == NULL) return 0;                 /* run stateless: every path tolerates NULL */
        memset(ns, 0, sizeof(ApCsmState));
        ns->shownUi = -1; ns->shownQual = -1;
        ns->beatSeen = TickCount();
        Refresh(ns);
        ns->lastRefresh = TickCount();
        return (long)ns;
    }

    case sdevCloseModule:
        if (st != NULL) DisposePtr((Ptr)st);      /* the art is const tables: nothing else to release */
        return 0;

    case sdevFeatures:
        return (1L << sdevWantMouseClicks) | (1L << sdevDontAutoTrack) | (1L << sdevHasCustomHelp);

    case sdevGetDisplayWidth:
        return CellWidth();

    case sdevPeriodicTickle:
        if (st != NULL) {
            unsigned long now = TickCount();
            if (now - st->lastRefresh >= kRefreshTicks) {
                short nq, nui;
                st->lastRefresh = now;
                Refresh(st);
                nq = CurrentQual(st); nui = st->ui;
                if (st->haveCache && st->cachePort != NULL
                    && (nq != st->shownQual || nui != st->shownUi) && SBIsControlStripVisible()) {
                    if (!PortLooksAlive(st->cachePort, &st->cacheRect)) {
                        st->haveCache = false; st->cachePort = NULL;
                    } else {
                        GrafPtr savePort;
                        GetPort(&savePort);
                        SetPort(st->cachePort);
                        DrawCell(st, &st->cacheRect, true);
                        SetPort(savePort);
                    }
                }
                st->shownQual = nq; st->shownUi = nui;
            }
        }
        return 0;

    case sdevDrawStatus:
        if (statusPort != NULL) SetPort(statusPort);
        if (st != NULL && statusRect != NULL && statusPort != NULL) {
            st->cachePort = statusPort; st->cacheRect = *statusRect; st->haveCache = true;
            st->shownQual = CurrentQual(st); st->shownUi = st->ui;
        }
        if (statusRect != NULL) DrawCell(st, statusRect, false);
        return 0;

    case sdevMouseClick:
        DoClick(st, statusRect);
        return 0;

    case sdevShowBalloonHelp:
        if (statusRect != NULL) ShowHelp(st, statusRect);
        return 0;

    default:
        return 0;
    }
}
