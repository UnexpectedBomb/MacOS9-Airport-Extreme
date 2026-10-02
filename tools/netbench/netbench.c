/*
 * NetBench -- a TCP throughput benchmark for Mac OS 9: QuickBench for the network.
 *
 * WHY: QuickBench cannot test a network volume (selecting the Pi share dropped it, over AirPort AND
 * Ethernet), and an AFP Finder copy blends AppleShare, the Pi's disk and the network into one number.
 * NetBench talks plain TCP to netbench-server.py on the Pi (start it with scripts/pi-netbench.sh):
 *
 *   PING  20 one-byte round trips          -> the per-exchange latency (driver + Open Transport + air)
 *   UP    128 KB warm-up, then 3 x 1 MB     -> Mac -> Pi, timed HERE and on the Pi
 *   DOWN  128 KB warm-up, then 3 x 1 MB     -> Pi -> Mac, timed here
 *
 * Every byte is VERIFIED against a known pattern at the receiving end -- a data-path oracle, not just
 * a stopwatch. It runs over whichever interface TCP/IP is set to, so AirPort / Ethernet is one ratio.
 * Results go to "NetBench Log" in the System Folder AND to the Pi's "NetBench Server Log".
 *
 * ⚠ NO printf. A Retro68 app that writes to the stdio console quits instantly on real OS 9 hardware
 *   (memory: reference_retro68_no_stdio_console). And no snprintf either: the first build used it and
 *   the link pulled in Retro68's stdio init and console backend (__sinit, _consolewrite) -- the very
 *   machinery behind those instant quits. So: a pure Toolbox window plus the tiny B* string builder
 *   below, the gpu-fix/nk-dump pattern. Every line is also written to the log, with PBFlushFile so a copy of the
 *   still-open file over the network is never a stale, truncated snapshot.
 * ⚠ Open Transport exactly as client/src/net.c does it (cy384's working Retro68 recipe): once-per-app
 *   InitOpenTransport, synchronous + blocking endpoint, OTInitDNSAddress("host:port"), Unbind + Close.
 *
 * PROTOCOL v1 (see netbench-server.py): 16-byte header 'NBv1' cmd n arg, big-endian.
 * PATTERN: byte at stream offset i = (i*7 + (i >> 9)) & 0xFF -- identical on both ends.
 */
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <TextUtils.h>
#include <Folders.h>
#include <Files.h>
#include <Timer.h>
#include <DateTimeUtils.h>
#include <MacMemory.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <string.h>         /* memcpy / memmove / memset / strlen only -- NO stdio (see header) */

#define kVersion        "NetBench 1.0"
#define kDefaultServer  "192.168.1.207:5201"
#define kPingCount      20
#define kWarmBytes      (128L * 1024L)
#define kTestBytes      (1024L * 1024L)
#define kRepeats        3
#define kMaxLines       40
#define kLineH          13

static Str255    gLines[kMaxLines];
static short     gNLines = 0;
static WindowPtr gWin = NULL;
static Rect      gWinRect;
static short     gLogRef = 0, gLogVRef = 0;
static char      gNote[3000];
static long      gNoteLen = 0;
static char      gServer[128] = kDefaultServer;

/* ---------------------------------------------------------------- output: window + log + note */
static void C2P(const char *c, Str255 p)
{
    short n = 0;
    while (c[n] && n < 255) { p[n + 1] = (unsigned char)c[n]; n++; }
    p[0] = (unsigned char)n;
}

/* A tiny string builder -- everything NetBench prints, with no stdio linked (see header). */
typedef struct { char *s; long cap, n; } Buf;
static void BInit(Buf *b, char *s, long cap) { b->s = s; b->cap = cap; b->n = 0; s[0] = 0; }
static void BChr(Buf *b, char c)             { if (b->n + 1 < b->cap) { b->s[b->n++] = c; b->s[b->n] = 0; } }
static void BStr(Buf *b, const char *t)      { while (*t) BChr(b, *t++); }
/* unsigned decimal: zpad = minimum digits (zero-filled), width = minimum field (space-padded, right) */
static void BNum(Buf *b, unsigned long v, int zpad, int width)
{
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 20);
    while (k < zpad && k < 20) t[k++] = '0';
    while (width-- > k) BChr(b, ' ');
    while (k) BChr(b, t[--k]);
}
static void BSigned(Buf *b, long v) { if (v < 0) { BChr(b, '-'); BNum(b, (unsigned long)(-v), 0, 0); } else BNum(b, (unsigned long)v, 0, 0); }
/* x10 fixed point -> "123.4", right-aligned in `width` */
static void BTenths(Buf *b, unsigned long v10, int width)
{
    char t[32]; Buf tb; long pad;
    BInit(&tb, t, sizeof t); BNum(&tb, v10 / 10, 0, 0); BChr(&tb, '.'); BNum(&tb, v10 % 10, 0, 0);
    for (pad = width - tb.n; pad > 0; pad--) BChr(b, ' ');
    BStr(b, t);
}

static void DrawAll(void)
{
    short i, y = 16;
    Rect r;
    if (!gWin) return;
    SetPort((GrafPtr)gWin);
    SetRect(&r, 0, 0, gWinRect.right - gWinRect.left, gWinRect.bottom - gWinRect.top);
    EraseRect(&r);
    TextFont(kFontIDMonaco); TextSize(9);
    for (i = 0; i < gNLines; i++) { MoveTo(10, y); DrawString(gLines[i]); y += kLineH; }
}

static void LogOpen(void)
{
    long dirID, eof;
    FSSpec spec;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &gLogVRef, &dirID) != noErr) return;
    if (FSMakeFSSpec(gLogVRef, dirID, "\pNetBench Log", &spec) != noErr)
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    if (FSpOpenDF(&spec, fsRdWrPerm, &gLogRef) != noErr) { gLogRef = 0; return; }
    if (GetEOF(gLogRef, &eof) == noErr) SetFPos(gLogRef, fsFromStart, eof);
}

/* One line: window, log (flushed + catalog EOF committed), and the note sent to the Pi at the end. */
static void Say(const char *s)
{
    long len = (long)strlen(s), one = 1;
    char cr = '\r';
    if (gNLines == kMaxLines) {                       /* full: scroll up by one */
        memmove(&gLines[0], &gLines[1], sizeof(Str255) * (kMaxLines - 1));
        gNLines--;
        C2P(s, gLines[gNLines++]);
        DrawAll();
    } else {
        C2P(s, gLines[gNLines++]);
        if (gWin) {
            SetPort((GrafPtr)gWin); TextFont(kFontIDMonaco); TextSize(9);
            MoveTo(10, (short)(16 + (gNLines - 1) * kLineH)); DrawString(gLines[gNLines - 1]);
        }
    }
    if (gLogRef) {
        ParamBlockRec pb;
        FSWrite(gLogRef, &len, s);
        FSWrite(gLogRef, &one, &cr);
        memset(&pb, 0, sizeof pb); pb.ioParam.ioRefNum = gLogRef;
        (void)PBFlushFileSync(&pb);                   /* commits the catalog EOF: copy-safe while open */
        (void)FlushVol(NULL, gLogVRef);               /* the LOG's volume, not the default one */
    }
    if (gNoteLen + len + 1 < (long)sizeof gNote) {
        memcpy(gNote + gNoteLen, s, len); gNoteLen += len; gNote[gNoteLen++] = '\r';
    }
}

/* Optional override: a text file "NetBench Server" in the System Folder holding "host:port". */
static void LoadServerOverride(void)
{
    short vRef, ref; long dirID, n = sizeof gServer - 1, i;
    FSSpec spec;
    char buf[128];
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRef, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRef, dirID, "\pNetBench Server", &spec) != noErr) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;
    if (FSRead(ref, &n, buf) == noErr || n > 0) {
        for (i = 0; i < n && buf[i] != '\r' && buf[i] != '\n' && buf[i] != ' '; i++) ;
        if (i > 0) { memcpy(gServer, buf, i); gServer[i] = 0; }
    }
    FSClose(ref);
}

/* ---------------------------------------------------------------- time + formatting */
static unsigned long long NowUs(void)
{
    UnsignedWide w;
    Microseconds(&w);
    return ((unsigned long long)w.hi << 32) | (unsigned long long)w.lo;
}

/* KB/s x10 as integer (one decimal place), from bytes and microseconds. */
static unsigned long KBps10(unsigned long bytes, unsigned long long us)
{
    if (us == 0) return 0;
    return (unsigned long)(((unsigned long long)bytes * 10000000ULL / 1024ULL) / us);
}

static void BMs(Buf *b, unsigned long long us)                     /* microseconds -> "12.34" ms */
{
    BNum(b, (unsigned long)(us / 1000ULL), 0, 0); BChr(b, '.'); BNum(b, (unsigned long)((us % 1000ULL) / 10ULL), 2, 0);
}

/* ---------------------------------------------------------------- the pattern (same as the server) */
static void FillPattern(UInt8 *b, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) b[i] = (UInt8)(i * 7UL + (i >> 9));
}

static unsigned long Mismatches(const UInt8 *got, const UInt8 *expect, unsigned long n)
{
    unsigned long i, bad = 0;
    for (i = 0; i < n; i++) if (got[i] != expect[i]) bad++;
    return bad;
}

/* ---------------------------------------------------------------- OT helpers (blocking) */
static OSStatus SendAll(EndpointRef ep, const void *buf, long n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        OTResult r = OTSnd(ep, (void *)p, (OTByteCount)n, 0);
        if (r > 0) { p += r; n -= r; continue; }
        if (r == kOTFlowErr || r == kOTNoDataErr) continue;
        if (r == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_DISCONNECT) { OTRcvDisconnect(ep, NULL); return kOTLookErr; }
            if (look == T_ORDREL)     { OTRcvOrderlyDisconnect(ep); return kOTLookErr; }
            continue;
        }
        return (OSStatus)r;
    }
    return noErr;
}

static OSStatus RecvAll(EndpointRef ep, void *buf, long n)
{
    char *p = (char *)buf;
    OTFlags fl;
    while (n > 0) {
        OTResult r = OTRcv(ep, p, (OTByteCount)n, &fl);
        if (r > 0) { p += r; n -= r; continue; }
        if (r == kOTNoDataErr) continue;
        if (r == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_DISCONNECT) { OTRcvDisconnect(ep, NULL); return kOTLookErr; }
            if (look == T_ORDREL)     { OTRcvOrderlyDisconnect(ep); return kOTLookErr; }
            continue;
        }
        if (r == 0) return kOTLookErr;                 /* peer closed */
        return (OSStatus)r;
    }
    return noErr;
}

static OSStatus SendHdr(EndpointRef ep, UInt32 cmd, UInt32 n)
{
    UInt8 h[16];
    UInt32 v[3]; int k;
    v[0] = cmd; v[1] = n; v[2] = 0;
    h[0] = 'N'; h[1] = 'B'; h[2] = 'v'; h[3] = '1';
    for (k = 0; k < 3; k++) {                          /* big-endian, explicitly */
        h[4 + k * 4 + 0] = (UInt8)(v[k] >> 24); h[4 + k * 4 + 1] = (UInt8)(v[k] >> 16);
        h[4 + k * 4 + 2] = (UInt8)(v[k] >> 8);  h[4 + k * 4 + 3] = (UInt8)(v[k]);
    }
    return SendAll(ep, h, 16);
}

static UInt32 BE32(const UInt8 *p) { return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) | ((UInt32)p[2] << 8) | p[3]; }

/* ---------------------------------------------------------------- the tests */
static OSStatus TestPing(EndpointRef ep, unsigned long long *avgUs)
{
    char s[160]; Buf b;
    unsigned long long mn = ~0ULL, mx = 0, sum = 0, t, d;
    UInt8 out, in;
    int k;
    OSStatus err = SendHdr(ep, 1, kPingCount);
    if (err) return err;
    for (k = 0; k < kPingCount; k++) {
        out = (UInt8)k;
        t = NowUs();
        if ((err = SendAll(ep, &out, 1)) != noErr) return err;
        if ((err = RecvAll(ep, &in, 1)) != noErr) return err;
        d = NowUs() - t;
        if (in != out) { Say("  [!!] PING: the echoed byte differs from the one sent"); return -1; }
        sum += d; if (d < mn) mn = d; if (d > mx) mx = d;
    }
    *avgUs = sum / kPingCount;
    BInit(&b, s, sizeof s);
    BStr(&b, "PING   "); BNum(&b, kPingCount, 0, 0); BStr(&b, " round trips   min "); BMs(&b, mn);
    BStr(&b, " ms   avg "); BMs(&b, *avgUs); BStr(&b, " ms   max "); BMs(&b, mx); BStr(&b, " ms");
    Say(s);
    return noErr;
}

static OSStatus TestUp(EndpointRef ep, const UInt8 *pat, long n, const char *label,
                       unsigned long *kb10Out, unsigned long *badOut)
{
    char s[200]; Buf b;
    UInt8 rep[16];
    unsigned long long t0, us;
    unsigned long got, srvUs, bad, mac10, pi10;
    OSStatus err;
    t0 = NowUs();
    if ((err = SendHdr(ep, 2, (UInt32)n)) != noErr) return err;
    if ((err = SendAll(ep, pat, n)) != noErr) return err;
    if ((err = RecvAll(ep, rep, 16)) != noErr) return err;   /* the Pi's verdict */
    us = NowUs() - t0;
    if (rep[0] != 'N' || rep[1] != 'B' || rep[2] != 'o' || rep[3] != 'k') { Say("  [!!] UP: bad reply from the Pi"); return -1; }
    got = BE32(rep + 4); srvUs = BE32(rep + 8); bad = BE32(rep + 12);
    mac10 = KBps10((unsigned long)n, us); pi10 = KBps10(got, (unsigned long long)srvUs);
    BInit(&b, s, sizeof s);
    BStr(&b, "UP     "); BStr(&b, label); BChr(&b, ' '); BNum(&b, (unsigned long)n, 0, 7); BStr(&b, " bytes  ");
    BTenths(&b, mac10, 8); BStr(&b, " KB/s (Mac)  "); BTenths(&b, pi10, 8); BStr(&b, " KB/s (Pi)  bad bytes ");
    BNum(&b, bad, 0, 0); if (got != (unsigned long)n) BStr(&b, "  [!!] short");
    Say(s);
    if (kb10Out) *kb10Out = mac10;
    if (badOut)  *badOut  = bad + ((got != (unsigned long)n) ? 1 : 0);
    return noErr;
}

static OSStatus TestDown(EndpointRef ep, UInt8 *buf, const UInt8 *pat, long n, const char *label,
                         unsigned long *kb10Out, unsigned long *badOut)
{
    char s[200]; Buf b;
    unsigned long long t0, us;
    unsigned long bad, mac10;
    OSStatus err;
    t0 = NowUs();
    if ((err = SendHdr(ep, 3, (UInt32)n)) != noErr) return err;
    if ((err = RecvAll(ep, buf, n)) != noErr) return err;
    us = NowUs() - t0;
    bad = Mismatches(buf, pat, (unsigned long)n);      /* verified OUTSIDE the timed window */
    mac10 = KBps10((unsigned long)n, us);
    BInit(&b, s, sizeof s);
    BStr(&b, "DOWN   "); BStr(&b, label); BChr(&b, ' '); BNum(&b, (unsigned long)n, 0, 7); BStr(&b, " bytes  ");
    BTenths(&b, mac10, 8); BStr(&b, " KB/s (Mac)  bad bytes "); BNum(&b, bad, 0, 0);
    Say(s);
    if (kb10Out) *kb10Out = mac10;
    if (badOut)  *badOut  = bad;
    return noErr;
}

/* "[!!] <what> failed, err <n><hint>" */
static void SayErr(const char *what, long err, const char *hint)
{
    char s[200]; Buf b;
    BInit(&b, s, sizeof s);
    BStr(&b, "[!!] "); BStr(&b, what); BStr(&b, " failed, err "); BSigned(&b, err); BStr(&b, hint);
    Say(s);
}

/* ---------------------------------------------------------------- main */
int main(void)
{
    EventRecord evt;
    long deadline;
    char s[200]; Buf b;
    EndpointRef ep = kOTInvalidEndpointRef;
    OSStatus err;
    Boolean otUp = false, bound = false, ok = false;
    UInt8 *pat = NULL, *buf = NULL;
    unsigned long long pingUs = 0;
    unsigned long upSum = 0, downSum = 0, k10, badSum = 0, bad;
    int r;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();

    SetRect(&gWinRect, 20, 44, 20 + 600, 44 + 16 + kMaxLines * kLineH + 8);
    gWin = NewWindow(NULL, &gWinRect, "\pNetBench", true, documentProc, (WindowPtr)-1L, false, 0);
    LogOpen();
    LoadServerOverride();

    {   unsigned long secs; DateTimeRec dt;
        GetDateTime(&secs); SecondsToDate(secs, &dt);
        BInit(&b, s, sizeof s);
        BStr(&b, "==== "); BStr(&b, kVersion); BStr(&b, "  ");
        BNum(&b, (unsigned long)dt.year, 4, 0);  BChr(&b, '-'); BNum(&b, (unsigned long)dt.month, 2, 0);
        BChr(&b, '-'); BNum(&b, (unsigned long)dt.day, 2, 0); BChr(&b, ' ');
        BNum(&b, (unsigned long)dt.hour, 2, 0);  BChr(&b, ':'); BNum(&b, (unsigned long)dt.minute, 2, 0);
        BChr(&b, ':'); BNum(&b, (unsigned long)dt.second, 2, 0); BStr(&b, " ====");
        Say(s); }

    pat = (UInt8 *)NewPtr(kTestBytes); buf = (UInt8 *)NewPtr(kTestBytes);
    if (!pat || !buf) { Say("[!!] not enough memory for the 2 x 1 MB test buffers -- raise the app's memory in Get Info"); goto done; }
    FillPattern(pat, (unsigned long)kTestBytes);

    Say("opening Open Transport...");
    if ((err = InitOpenTransport()) != noErr) { SayErr("InitOpenTransport", (long)err, ""); goto done; }
    otUp = true;

    {   InetInterfaceInfo info;
        if (OTInetGetInterfaceInfo(&info, kDefaultInetInterface) == noErr) {
            UInt32 h = (UInt32)info.fAddress;
            BInit(&b, s, sizeof s);
            BStr(&b, "local IP "); BNum(&b, (unsigned long)(h >> 24), 0, 0); BChr(&b, '.');
            BNum(&b, (unsigned long)((h >> 16) & 0xFF), 0, 0); BChr(&b, '.');
            BNum(&b, (unsigned long)((h >> 8) & 0xFF), 0, 0);  BChr(&b, '.');
            BNum(&b, (unsigned long)(h & 0xFF), 0, 0);
            BStr(&b, "   (which TCP/IP configuration this run used)");
            Say(s);
        } else Say("local IP unknown (OTInetGetInterfaceInfo failed)"); }

    ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (err != noErr || ep == kOTInvalidEndpointRef) { SayErr("OTOpenEndpoint(tcp)", (long)err, ""); goto done; }
    OTSetSynchronous(ep); OTSetBlocking(ep);
    if ((err = OTBind(ep, NULL, NULL)) != noErr) { SayErr("OTBind", (long)err, ""); goto done; }
    bound = true;

    BInit(&b, s, sizeof s); BStr(&b, "connecting to "); BStr(&b, gServer); BStr(&b, " ..."); Say(s);
    {   DNSAddress dns; TCall call;
        OTMemzero(&call, sizeof call);
        call.addr.buf = (UInt8 *)&dns;
        call.addr.len = OTInitDNSAddress(&dns, gServer);
        if ((err = OTConnect(ep, &call, NULL)) != noErr) {
            SayErr("connect", (long)err, " -- is pi-netbench.sh running on the Pi?");
            goto done; } }
    Say("connected.");

    if (TestPing(ep, &pingUs) != noErr) { Say("[!!] PING failed"); goto done; }

    if (TestUp(ep, pat, kWarmBytes, "warm", NULL, &bad) != noErr) { Say("[!!] UP failed"); goto done; }
    badSum += bad;
    for (r = 1; r <= kRepeats; r++) {
        char lab[8] = { '#', (char)('0' + r), ' ', ' ', 0 };
        if (TestUp(ep, pat, kTestBytes, lab, &k10, &bad) != noErr) { Say("[!!] UP failed"); goto done; }
        upSum += k10; badSum += bad;
    }
    if (TestDown(ep, buf, pat, kWarmBytes, "warm", NULL, &bad) != noErr) { Say("[!!] DOWN failed"); goto done; }
    badSum += bad;
    for (r = 1; r <= kRepeats; r++) {
        char lab[8] = { '#', (char)('0' + r), ' ', ' ', 0 };
        if (TestDown(ep, buf, pat, kTestBytes, lab, &k10, &bad) != noErr) { Say("[!!] DOWN failed"); goto done; }
        downSum += k10; badSum += bad;
    }

    BInit(&b, s, sizeof s);
    BStr(&b, "SUMMARY  up "); BTenths(&b, upSum / kRepeats, 0); BStr(&b, " KB/s   down ");
    BTenths(&b, downSum / kRepeats, 0); BStr(&b, " KB/s   ping "); BMs(&b, pingUs);
    BStr(&b, " ms   bad bytes "); BNum(&b, badSum, 0, 0);
    Say(s);
    ok = true;

    /* hand the whole transcript to the Pi, so the result lands in its log without a copy */
    if (SendHdr(ep, 4, (UInt32)gNoteLen) == noErr && SendAll(ep, gNote, gNoteLen) == noErr) {
        UInt8 rep[16];
        (void)RecvAll(ep, rep, 16);
    }
    (void)SendHdr(ep, 5, 0);

done:
    if (ep != kOTInvalidEndpointRef) { if (bound) OTUnbind(ep); OTCloseProvider(ep); }
    if (otUp) CloseOpenTransport();
    if (pat) DisposePtr((Ptr)pat);
    if (buf) DisposePtr((Ptr)buf);
    Say(ok ? "done. Logged to 'NetBench Log' (System Folder) and the Pi. Click or press a key to quit."
            : "stopped early -- see the lines above. Click or press a key to quit.");
    if (gLogRef) { FSClose(gLogRef); gLogRef = 0; }

    deadline = TickCount() + 30L * 60L * 60L;            /* 30 minutes, then quit on its own */
    while (TickCount() < deadline) {
        if (!WaitNextEvent(mDownMask | keyDownMask | updateMask, &evt, 30, NULL)) continue;
        if (evt.what == updateEvt && (WindowPtr)evt.message == gWin) {
            BeginUpdate(gWin); DrawAll(); EndUpdate(gWin);
        } else if (evt.what == mouseDown || evt.what == keyDown) break;
    }
    if (gWin) DisposeWindow(gWin);
    return 0;
}
