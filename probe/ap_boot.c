/* ap_boot.c -- the PowerPC half of the BOOT VEHICLE. Exports InstallMe, called once by the 68K
 * INIT via Mixed Mode.
 *
 * ★★★ WHY A VEHICLE AT ALL. Our OT port exists only while the test application has called
 * EnetShimInstallDriver, so TCP/IP has nothing to offer at boot. Four builds (k100-k103) looked
 * for a system mechanism that would call US at boot -- Device Manager discovery, a node-named
 * fragment, an OTKrnl$ port scanner -- and all four were negative. The prior-art sweep the user
 * asked for settled why, from Apple's own Readme in usb-ddk/Examples/USBEnetSample:
 *
 *     "the driver will register itself with the EnetShimLib ... a call is made to the
 *      EnetShimInstallDriver entry point, WHERE THE PORT IS REGISTERED WITH OPEN TRANSPORT."
 *
 * There is no discovery to join. The driver registers itself; for USB, what runs it at the right
 * moment is the USB expert matching a class driver. We have no such expert, so we must supply the
 * thing that runs at boot -- and this project already ships one, on this machine.
 *
 * ★ THE VEHICLE IS usb2-ehci/resident/, VERBATIM IN SHAPE. A 68K 'INIT' code resource loads a
 *   CFM fragment with GetMemFragment and Mixed-Mode-calls one exported symbol.
 *   [[reference_os9_init_resident_driver]]. The EHCIInit INIT and NM logs on the Pi share are
 *   dated the same boot as k103, so both halves of this are demonstrably working on the target.
 *
 * ★★★★★★ k105 PROVED THE VEHICLE, AND k106 USES IT. k105's NM log:
 *
 *     === ApBootNMResponse: FIRED at TASK level, post-boot ===
 *       nmRefCon (expect 0x41506274 'APbt') 0x41506274
 *       Gestalt('otan') err (0 = OT present) 0x00000000
 *       OT attributes 0x0000003f
 *
 *   Task level, after boot, with Open Transport up -- the first context in which
 *   EnetShimInstallDriver is legal with no application running. k104 and k105 deliberately
 *   registered NOTHING so that the vehicle could be proven on its own; that is done, so the
 *   registration moves in here now, and nowhere earlier.
 *
 * ⚠ The registration sequence is the APPLICATION'S, verbatim -- airport_rx.c has been making it
 *   successfully since k55, and k56 established the second half: registering creates the port
 *   INACTIVE, and EnetShimAsyncStatus(ref, EnetShim_Link, 1, 0) is what makes OT offer it.
 *   Nothing about the calls is new. Only where they run is new.
 *
 * ⚠ GetMemFragment, NOT GetSharedLibrary("AirPortShim"). Two reasons, both from the prior art:
 *   the INIT runs before we can assume CFM has finished registering Extensions-folder libraries,
 *   and an INIT-loaded fragment is a SEPARATE COPY WITH SEPARATE GLOBALS anyway
 *   ([[reference_os9_two_fragment_copies_own_globals]]) -- so sharing the shim's PEF would buy
 *   nothing and would put two copies on the same card. This is a tiny fragment of its own that
 *   touches no hardware at all.
 */
#include <MacTypes.h>
#include <Gestalt.h>
#include <MacMemory.h>
#include <Notification.h>
#include <Events.h>        /* TickCount, for the snapshot schedule */
#include <NameRegistry.h>
#include <Files.h>
#include <CodeFragments.h>

#define AP_BOOT_VER "k165-native"

/* ---- Apple's EnetShimLib interface, vendored exactly as airport_rx.c:156 vendors it (usb-ddk is
 * not on the include path). The struct is from EthernetShim.pdf, "Installing a driver":
 * DRVName is the name Open Transport shows in the TCP/IP control panel's pop-up, ConnID is the
 * CFM connection to the DRIVER so the shim can call back into it. ---- */
typedef UInt32 ShimRefNum;
typedef struct EnetShimInterface {
    StringPtr         DRVName;
    CFragConnectionID ConnID;
    UInt32            RefCon;
    UInt32            theID;
} EnetShimInterface;
typedef OSErr (*ShimInstallProc)(EnetShimInterface, ShimRefNum *);
typedef OSErr (*ShimAsyncProc)(ShimRefNum, UInt16, UInt32, UInt32);
#define EnetShim_Link   0

/* ---- logging. Task level only: INIT time and the NM response are both task level, and the File
 * Manager below task level hangs OS 9 with no NMI -- the most-bitten rule in this project. ---- */
static short gBootLog = 0, gNmLog = 0;
static void Lw(short *ref, const char *name, const char *s)
{
    long n = 0, z = 1;
    if (!*ref) {
        FSSpec sp; Str63 p; int i = 0;
        while (name[i] && i < 62) { p[i+1] = name[i]; i++; }
        p[0] = (unsigned char)i;
        (void)FSMakeFSSpec(0, 0, p, &sp);
        (void)FSpDelete(&sp);
        if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, ref);
    }
    if (*ref) { while (s[n]) n++; (void)FSWrite(*ref, &n, (Ptr)s);
                (void)FSWrite(*ref, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void Lx(short *ref, const char *name, const char *label, unsigned long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; Lw(ref, name, b);
}
#define BLOG(s)     Lw(&gBootLog, "AirPort Boot Log", (s))
#define BLOGX(s,v)  Lx(&gBootLog, "AirPort Boot Log", (s), (unsigned long)(v))
#define NLOG(s)     Lw(&gNmLog,   "AirPort NM Log",   (s))
#define NLOGX(s,v)  Lx(&gNmLog,   "AirPort NM Log",   (s), (unsigned long)(v))

/* ★ THE BEACON, and it is not optional bookkeeping.
 *
 * This fragment's globals are not the app's globals -- CFM gives an INIT-loaded copy its own.
 * A counter here is unreadable from the probe application, so "did the vehicle run" would be
 * unanswerable exactly as it was at k100, whose witness sat behind the connection its own change
 * broke and whose run therefore reported nothing either way. A Name Registry property is outside
 * every copy: whoever writes it, anyone can read it. */
static OSStatus Beacon(const char *prop, UInt32 value)
{
    RegEntryID root;
    OSStatus e = RegistryCStrEntryLookup(NULL, "Devices:device-tree", &root);
    if (e != noErr) return e;
    e = RegistryPropertyCreate(&root, (char *)prop, (void *)&value,
                               (RegPropertyValueSize)sizeof(value));
    (void)RegistryEntryIDDispose(&root);
    return e;
}

/* ============================================================================================
 * ★★★ 8-58: NATIVE OPEN TRANSPORT PORT REGISTRATION -- make OT load OUR module, OTModl$AirPortBCM.
 *
 * WHY. Apple's USBEnet shim delivers ARP to OT but never inbound IP (proven build 81: OT answers a
 * unicast ARP who-has in the same 6 s window it ignores 7 unicast ICMP echo requests). Apple's own
 * AirPort avoids this by being a NATIVE DLPI module, not a shim client. So we register a port whose
 * module is "AirPortBCM"; OT then loads OTModl$AirPortBCM (ap_otmodl.c) by that name and drives it.
 *
 * HOW, and it is not guesswork -- it is the exact recipe disassembled from Apple's own port scanner
 * OTKrnl$USBOTPortScanner (Extensions/Apple Enet DLPI Support), 2026-09-25:
 *   - OTRegisterPort + OTCreatePortRef are exported by OTKernelUtilLib (resolve at runtime).
 *   - the scanner builds an OTPortRecord: OTCreatePortRef(bus, kOTEthernetDevice=10, slot, other) ->
 *     fRef; OTMemzero; fInfoFlags = kOTPortIsDLPI(1); fCapabilities = 9; fPortFlags = 0; fPortName
 *     empty (OT assigns enet0/1); fModuleName = "USBEnet"; fResourceInfo = "USBEnetCfgHelper"; then
 *     OTRegisterPort(&rec, ref). We change only fModuleName -> "AirPortBCM".
 *   - NO port scanner is needed. The shim path already proves OTRegisterPort works from THIS
 *     task-level NM context (EnetShimInstallDriver reaches it the same way and enet1 appears in
 *     TCP/IP). k103's OTScanPorts dead end is bypassed. See [[reference_os9_ot_network_driver_contract]].
 * ==========================================================================================*/
#define AP_NATIVE_OTMODL 1     /* 1 = register the native OTModl$AirPortBCM port; 0 = the old shim */

typedef UInt32 OTPortRef_;
typedef struct AP_OTPortRecord {   /* == OpenTransport.h OTPortRecord (0x11c bytes) */
    OTPortRef_ fRef;
    UInt32     fPortFlags, fInfoFlags, fCapabilities, fNumChildPorts;
    void      *fChildPorts;
    char       fPortName[36];
    char       fModuleName[32];
    char       fSlotID[8];
    char       fResourceInfo[32];
    char       fReserved[164];
} AP_OTPortRecord;
typedef OTPortRef_ (*OTCreatePortRefProc)(UInt8 bus, UInt16 dev, UInt16 slot, UInt16 other);
typedef OSStatus   (*OTRegisterPortProc)(AP_OTPortRecord *info, void *ref);
#define AP_kOTPCIBus          3
#define AP_kOTEthernetDevice  10
#define AP_kOTPortIsDLPI      0x00000001UL
#define AP_kOTPortIsActive    0x00000001UL

#if AP_NATIVE_OTMODL
/* Registers the OTModl$AirPortBCM port. Task level (NM response). Returns OTRegisterPort's status,
 * or a resolve error. Beacons "airport-otmodl-port" so the result is readable across CFM copies. */
static OSStatus ApBootRegisterNativePort(void)
{
    static AP_OTPortRecord rec;          /* resident: this fragment stays loaded for the session */
    static UInt32          refBlock[8];  /* the per-port ref OT keeps; must outlive this call */
    CFragConnectionID cid = 0; Ptr a = 0; Str255 en; OSErr e;
    OTCreatePortRefProc mkRef = 0; OTRegisterPortProc regPort = 0;
    Ptr s; CFragSymbolClass cls; OTPortRef_ pr; OSStatus rr; int i;
    const char *mod = "AirPortBCM";

    e = GetSharedLibrary("\pOTKernelUtilLib", kPowerPCCFragArch, kLoadCFrag, &cid, &a, en);
    NLOGX("  [8-58] GetSharedLibrary(OTKernelUtilLib) err (0=ok)", (unsigned long)(long)e);
    if (e != noErr) return e;
    if (FindSymbol(cid, "\pOTCreatePortRef", &s, &cls) == noErr) mkRef  = (OTCreatePortRefProc)s;
    if (FindSymbol(cid, "\pOTRegisterPort",  &s, &cls) == noErr) regPort = (OTRegisterPortProc)s;
    NLOGX("  [8-58] OTCreatePortRef resolved", (unsigned long)mkRef);
    NLOGX("  [8-58] OTRegisterPort  resolved", (unsigned long)regPort);
    if (!mkRef || !regPort) return cfragNoSymbolErr;

    /* fRef: a PCI Ethernet port ref. slot/other are chosen distinctive so the ref cannot collide
     * with the built-in gmac port; our module finds its own card, so the exact slot is not load-
     * bearing here -- only uniqueness is. Logged so a conflict is visible. */
    pr = (*mkRef)(AP_kOTPCIBus, AP_kOTEthernetDevice, 0x0E, 0);
    NLOGX("  [8-58] OTCreatePortRef -> fRef", (unsigned long)pr);

    for (i = 0; i < (int)sizeof(rec); i++) ((char *)&rec)[i] = 0;
    rec.fRef          = pr;
    rec.fPortFlags    = 0;                     /* 8-81 k187: MATCH USBEnet's proven record (fPortFlags=0).
                                                * k159 set kOTPortIsActive as a "force visible" hack, and it has
                                                * ridden along ever since. But fPortFlags bits are the port's
                                                * live STATE; a fresh port claiming kOTPortIsActive reads to OT's
                                                * cold config-apply path as "already active -- nothing to open or
                                                * bind", which is EXACTLY our symptom (k186: link up, port selected
                                                * for DHCP, OT enumerates INFO+0x4f02 then quits, never DL_BIND).
                                                * USBEnet -- the working DLPI Ethernet record we copied -- uses 0,
                                                * and the port is selectable WITHOUT the hack (verified in the
                                                * TCP/IP popup). "If no bits are set, the port is inactive" is the
                                                * correct registered-but-not-yet-activated state. */
    rec.fInfoFlags    = AP_kOTPortIsDLPI;     /* 1 */
    rec.fCapabilities = 9;                     /* the value USBEnet's working record carries */
    rec.fNumChildPorts = 0;
    rec.fPortName[0]  = 0;                      /* OT assigns enet0/enet1 */
    for (i = 0; mod[i] && i < 31; i++) rec.fModuleName[i] = mod[i];
    rec.fSlotID[0]      = 0;
    rec.fResourceInfo[0] = 0;                   /* no OTPortCfg$ helper yet */

    refBlock[0] = 0x41504254UL;                /* 'APbt' -- our marker in the ref OT hands back */
    rr = (*regPort)(&rec, (void *)refBlock);
    NLOGX("  [8-58] ★ OTRegisterPort err (0=ok)", (unsigned long)(long)rr);
    NLOGX("  [8-58] OT-assigned fPortName[0..3]", (unsigned long)*(UInt32 *)rec.fPortName);
    (void)Beacon("airport-otmodl-port",
                 (UInt32)(rr == noErr ? (0x80000000UL | (pr & 0x7FFFFFFFUL)) : (UInt32)(long)rr));
    return rr;
}

/* ★★★ 8-60: NATIVE SNAPSHOT PUMP. Every ~2 s, resolve the native module (a shlb -> ONE shared data
 * instance, so we read the copy OT drives, per k131) and call its exported ApOtmSnapshot, which
 * dumps the live RX/DLPI counters to "AirPort Driver Log". This is the black-box recorder that says
 * where the RX path stops. Task level (NM response). Re-arms NM each fire. The module is not loaded
 * until the port is opened, so this logs an "not loaded yet" note until the user selects the port. */
static void ApBootNativeSnapshotPump(NMRecPtr nm)
{
    static UInt32 sLastTick = 0;
    static int    sLogged   = 0;
    UInt32 now = TickCount();
    if (now - sLastTick >= 120) {                       /* ~2 s */
        CFragConnectionID c = 0; Ptr a = 0; Str255 en; Ptr sym; CFragSymbolClass sc;
        OSErr e;
        sLastTick = now;
        e = GetSharedLibrary("\pOTModl$AirPortBCM", kPowerPCCFragArch, kFindCFrag, &c, &a, en);
        if (e == noErr && c && FindSymbol(c, "\pApOtmSnapshot", &sym, &sc) == noErr) {
            (*(void (*)(void))sym)();
            if (!sLogged) { sLogged = 1; NLOG("  [8-60] native snapshot pump live -> AirPort Driver Log"); }
        } else if (!sLogged) {
            sLogged = 1;
            NLOGX("  [8-60] OTModl$AirPortBCM not loaded yet (select the port in TCP/IP); err",
                  (unsigned long)(long)e);
        }
    }
    if (nm) { (void)NMRemove(nm); (void)NMInstall(nm); }   /* re-arm to fire again */
}
#endif /* AP_NATIVE_OTMODL */

/* ★★★ THE DEFERRED HALF. Delivered at TASK level during SystemTask/WaitNextEvent, i.e. after the
 * boot parade, when Open Transport is up and the File Manager is safe.
 *
 * ⚠⚠ NOT A TIME MANAGER TASK, and this is the correction the prior art already paid for. The USB
 *   2.0 scope document's first instinct was a Time Manager one-shot; bootmain.c:362 overrides it:
 *   "A Time Manager task fires at INTERRUPT level, where FSWrite/Gestalt/CFM are all unsafe --
 *   hence NM, not a raw timer." Every call below is one of exactly those three things. */
static UInt32 sTries = 0;
static CFragConnectionID gDrvConn = 0;   /* the copy OT drives, resolved during InstallMe */
static int sRegistered = 0;              /* the pump re-enters this proc; register once */
static int sPreambleDone = 0;            /* ...and log the preamble once */
static ShimAsyncProc gAsyncProc = 0;     /* 8-34: so the pump can report link-up later */
static ShimRefNum    gShimRef   = 0;
static int sPhaseErrLogged = 0;   /* the phase poll runs ~4x/sec; log its error once */
static int sSnapErrLogged  = 0;   /* the snapshot now retries on failure; log its error once too */

/* ★★★ 8-37: RE-RESOLVE THE DRIVER CONNECTION IN THE PUMP'S OWN CONTEXT, PER FIRE.
 *
 * ⛔ k130 proved gDrvConn cannot be cached across the boot -> task boundary. It is resolved
 *   inside InstallMe, during boot; the Notification Manager fires this pump POST-boot, at task
 *   level, in a DIFFERENT CFM context, and FindSymbol(gDrvConn, ...) then returns -2801,
 *   cfragConnectionIDErr -- the id is not valid in the firing context. It is intermittent
 *   because some NM fires land in a context where the boot id still resolves and some do not:
 *   k130's log shows GetPhase succeeding twice while all four AirPortShimSnapshot calls failed
 *   -2801, on the same gDrvConn. So the whole DHCP decode this build exists to emit never ran.
 *
 * The fix is the rule this project already carries for TVectors, applied to connections:
 * [[reference_os9_findsymbol_no_reference]] -- resolve in the context you will use it in, and
 * do not cache across a boundary. GetSharedLibrary returns a connection valid in the CURRENT
 * context, and for an import library (shlb) every connection shares ONE data instance, so the
 * counters this reaches are the very ones OT's copy is incrementing. kFindCFrag, never
 * kLoadCFrag: the fragment is already prepared and OT is driving it; we only want a handle to
 * it, not a second preparation.
 *
 * ⚠ Task level only, which the NM response proc is by construction -- GetSharedLibrary is the
 *   same call InstallMe makes, and this file's other CFM/File-Manager work is already audited
 *   as task time. */
static int sPumpConnLogged = 0;
static CFragConnectionID PumpResolveDrv(void)
{
    CFragConnectionID c = 0;
    Ptr               a = 0;
    Str255            en;
    OSErr             e = GetSharedLibrary("\pAirPortShim", kPowerPCCFragArch, kFindCFrag,
                                           &c, &a, en);
    if (e == noErr && c != 0) {
        if (!sPumpConnLogged) { sPumpConnLogged = 1;
            NLOGX("  [8-37] pump re-resolved AirPortShim; connection", (unsigned long)c);
            NLOGX("         (the boot-context id was)", (unsigned long)gDrvConn); }
        return c; }
    if (!sPumpConnLogged) { sPumpConnLogged = 1;
        NLOGX("  [8-37] ⛔ pump GetSharedLibrary(AirPortShim) err", (unsigned long)(long)e);
        NLOG("         this fire's CFM context is foreign; SKIP and retry next fire."); }
    /* ⛔ DO NOT fall back to gDrvConn. k131 proved that path: when GetSharedLibrary fails the
     * boot-context id is invalid in THIS context too, so FindSymbol on it returns -2801 every
     * time and floods the log with a connection error that is really a context error. Returning
     * 0 makes the caller's `conn ?` guard skip cleanly; GetSharedLibrary succeeds on a later
     * fire (k131: it did, at rejoin attempts 7..10) and the work runs then. The DRIVER now
     * dumps the snapshot itself (ap_shim.c 8-38), so a skipped fire here no longer loses data. */
    return 0;
}

static void ApBootNMResponse(NMRecPtr nm)
{
    long  otAttr = 0;
    OSErr ge;

    /* ⛔⛔ LOG THE PREAMBLE ONCE. This response is a PUMP now, and k114 measured what that
     *   means: 4112 deliveries in about a minute, roughly 68 a second. k114 re-logged these
     *   five lines and re-wrote the Name Registry beacon on EVERY ONE of them -- a 934 KB log
     *   from one minute of pumping. k115 waits up to TEN MINUTES for the bind, which at that
     *   rate is ~40,000 deliveries, ~9 MB of log and ~40,000 registry writes. The instrument
     *   would have become a bigger problem than the thing it was measuring.
     *
     *   From here the preamble is written once and later deliveries say nothing unless something
     *   actually happened. */
    ge = Gestalt(gestaltOpenTpt, &otAttr);
    if (!sPreambleDone) {
        sPreambleDone = 1;
        NLOG("=== ApBootNMResponse " AP_BOOT_VER ": FIRED at TASK level, post-boot ===");
        NLOGX("  nmRefCon (expect 0x41506274 'APbt')", nm ? nm->nmRefCon : 0);
        NLOGX("  Gestalt('otan') err (0 = OT present)", (unsigned long)(long)ge);
        NLOGX("  OT attributes", (unsigned long)otAttr);
        NLOGX("  beacon airport-nm-ran err",
              (unsigned long)Beacon("airport-nm-ran", (UInt32)(ge == noErr ? otAttr : 0)));
    }

    /* ⚠ IF OPEN TRANSPORT IS NOT UP, DO NOT REGISTER -- RE-ARM AND COME BACK. k105 saw
     * attributes 0x3f on the first delivery, so this is not the expected path; but "OT was not
     * ready yet" and "registration failed" are different findings and must not share an outcome.
     * Bounded, because a notification that re-arms forever on a permanent failure is a pump that
     * never stops -- [[feedback_guards_must_not_latch]] in the other direction. */
    if (ge != noErr || otAttr == 0) {
        if (nm && ++sTries < 20) {
            NLOGX("  OT not ready; re-arming. attempt", (unsigned long)sTries);
            (void)NMRemove(nm); (void)NMInstall(nm);
        } else {
            NLOG("  ⛔ OT never came up in 20 deliveries -- giving up for this boot.");
            if (nm) (void)NMRemove(nm);
        }
        return;
    }

    /* ★★★★★★ THE REGISTRATION. This is the call the whole vehicle exists to make, in the first
     * context where it is legal: task level, post-boot, Open Transport confirmed up.
     *
     * The sequence below is the application's, verbatim -- airport_rx.c:1457-1545 has been making
     * it successfully since k55. Nothing here is new except WHERE it runs. */
    /* ⛔⛔ REGISTER EXACTLY ONCE. The pump below re-arms this notification, so this response
     * runs REPEATEDLY -- and everything from here to the end of the block is the registration
     * sequence. Without this guard, every delivery would call EnetShimInstallDriver again,
     * registering the same driver over and over and handing OT a new ShimRefNum each time.
     * 8-21 and earlier were one-shot, so the question never arose; making the response a pump
     * is exactly what makes it arise. */
    if (!sRegistered) {
        sRegistered = 1;
#if AP_NATIVE_OTMODL
        /* ★★★ 8-58: NATIVE PATH. Register our own OT port naming module "AirPortBCM" so OT loads
         * OTModl$AirPortBCM. 8-62 (k165): RETIRE after registering -- the snapshot pump never
         * resolved an OT-loaded module (dead end) and its NM churn only muddied the crash. */
        (void)ApBootRegisterNativePort();
        if (nm) (void)NMRemove(nm);
        return;
#else
        /* ⚠ STATIC, because the snapshot pump below needs it on LATER deliveries. A connection
         * id in a local would be gone the moment this function returned, and FindSymbol on a
         * stale id is exactly the kind of call that returns garbage rather than an error. */
        CFragConnectionID shimCid = 0;
        CFragConnectionID drvCid;
        Ptr               addr   = 0;
        Str255            errName;
        ShimInstallProc   installProc = 0;
        ShimAsyncProc     asyncProc   = 0;   /* copied into gAsyncProc for the pump */
        Ptr               sym; CFragSymbolClass cls;
        OSErr             e;

        /* The driver. kLoadCFrag because nothing else has connected to it this boot -- the app is
         * not running. This is the connection the shim will call EnetHAL_Entry through, so it is
         * the DRIVER's fragment, never this one. */
        drvCid = 0;
        e = GetSharedLibrary("\pAirPortShim", kPowerPCCFragArch, kLoadCFrag,
                             &drvCid, &addr, errName);
        NLOGX("  GetSharedLibrary(AirPortShim) err (0=ok)", (unsigned long)(long)e);
        gDrvConn = drvCid;
        if (e != noErr) {
            NLOG("  ⛔ the driver is not installed, or CFM cannot resolve it by name.");
            NLOG("     \"AirPort Extreme Driver\" must be in Extensions beside this file.");
            if (nm) (void)NMRemove(nm); return; }

        /* ★★★★★ ARM THIS COPY. k111's driver log named the blocker in one line:
         *
         *     [8-2b] armed by the app: 0
         *     [8-2b] core-enable status 0xFFFFFFCA      core is up: 0
         *     [8-2c] firmware status 0xFFFFFFCE
         *
         * ApShimEnableCore is gated on AirPortShimArm(), an interlock added so the driver would
         * never reset the 802.11 core while the TEST APPLICATION was using the card. Only the
         * app has ever called it. So when Open Transport opened the port, the driver did all its
         * read-only work correctly -- found the card, read MAC 00:11:24:.. from SPROM, enumerated
         * chip 0x4306 rev 3 with 5 cores -- and then declined every destructive step, exactly as
         * designed. No core, no firmware, no PHY, no receiver, and a join that heard nothing
         * because the receiver was never switched on.
         *
         * ★ ap_shim.c:279 anticipated this moment in as many words: "This interlock is
         *   TRANSITIONAL. It exists because two owners share one card during the migration. When
         *   the app is gone the driver is the only owner and arms itself."
         *
         * ⚠⚠ AND IT IS ARMED HERE, NOT IN THE DRIVER, BECAUSE OF THE TWO COPIES. k110 proved the
         *   boot vehicle and the application hold SEPARATE instances with separate globals. This
         *   call arms the instance OPEN TRANSPORT will drive -- the one with no application
         *   sharing its card. The app's own copy stays unarmed and its interlock keeps working,
         *   which is the whole reason the interlock exists. Self-arming inside EnetHAL_Open would
         *   arm both and throw that away.
         *
         * ⚠ This does NOT touch the interrupt arm. AP_ARM_IRQ_IN_OPEN stays off -- that is the
         *   one that grey-screened the machine at k92, and it is a separate decision. */
        { Ptr as; CFragSymbolClass ac;
          if(FindSymbol(drvCid, "\pAirPortShimArm", &as, &ac) == noErr){
            UInt32 was = (*(UInt32 (*)(UInt32))as)(1);
            NLOGX("  ★ AirPortShimArm(1) on the copy OT drives; previous state", (unsigned long)was);
            NLOG("    ⇒ Open may now enable the core, upload microcode and bring the radio up.");
          } else {
            NLOG("  ⛔ AirPortShimArm did not resolve -- the driver will decline every");
            NLOG("     destructive step and the radio will stay down. Check ap_shim.exp."); } }

        e = GetSharedLibrary("\pEnetShimLib", kPowerPCCFragArch, kLoadCFrag,
                             &shimCid, &addr, errName);
        NLOGX("  GetSharedLibrary(EnetShimLib) err (0=ok)", (unsigned long)(long)e);
        if (e != noErr) { if (nm) (void)NMRemove(nm); return; }

        if (FindSymbol(shimCid, "\pEnetShimInstallDriver", &sym, &cls) == noErr)
            installProc = (ShimInstallProc)sym;
        if (FindSymbol(shimCid, "\pEnetShimAsyncStatus", &sym, &cls) == noErr)
            asyncProc = (ShimAsyncProc)sym;
            gAsyncProc = asyncProc;
        NLOGX("  EnetShimInstallDriver resolved", (unsigned long)installProc);
        NLOGX("  EnetShimAsyncStatus   resolved", (unsigned long)asyncProc);
        if (!installProc) { if (nm) (void)NMRemove(nm); return; }

        {
            /* ★★★★★ theID: A REAL NAME REGISTRY ENTRY, NOT A POINTER TO FOUR ZERO BYTES.
             *
             * EthernetShim.pdf says only "theID is the NameRegistry ID of the device driver",
             * and this project has been passing `(UInt32)&aStaticUInt32` since k55 -- a pointer
             * to FOUR zero bytes where a RegEntryID is SIXTEEN (NameRegistry.h:61,
             * `UInt32 contents[4]`). Anything dereferencing it reads twelve bytes of whatever
             * static data happens to follow.
             *
             * Apple's own stub, USBEnetStub.c:105-144, does the real thing:
             *
             *     err = RegistryEntryIDInit(&theID);
             *     err = RegistryEntrySearch(&cookie, kRegIterDescendants, &theID, &done,
             *                               "deviceRef", &USBRef, sizeof(USBRef));
             *     gGlobals->theID = theID;          // save our reg entry
             *
             * It FINDS the device's node by searching for a property it knows, and hands the
             * shim a pointer to that. So we do the same, searching on the one property we have
             * measured on this card: name == "pci80211" (k98; nine bytes, the NUL counts).
             *
             * ⚠ HONESTY ABOUT WHAT THIS IS. It is a REAL defect and a real difference from
             *   Apple's working driver, but it is NOT proven to be the blocker: the app passed
             *   the same bogus value from k55 onward and OT still drove the driver in those
             *   runs -- because the APP opened an endpoint itself. What has never worked is OT
             *   opening the port on its own. This is the only concrete difference from Apple's
             *   code found so far, and it is cheap, so it goes first.
             *
             * ⚠ STATIC, because the shim KEEPS this pointer. Apple's lives in their globals for
             *   the same reason; a stack RegEntryID would be a dangling pointer the moment this
             *   function returned. */
            static RegEntryID sCardNode;
            { RegEntryIter it; Boolean done = TRUE; OSStatus re;
              (void)RegistryEntryIDInit(&sCardNode);
              re = RegistryEntryIterateCreate(&it);
              if(re == noErr){
                re = RegistryEntrySearch(&it, kRegIterContinue, &sCardNode, &done,
                                         "name", (const void *)"pci80211",
                                         (RegPropertyValueSize)9);
                (void)RegistryEntryIterateDispose(&it); }
              NLOGX("  RegistryEntrySearch(name=pci80211) err (0=ok)", (unsigned long)re);
              NLOGX("    found (0 = yes, the search did not run out)", (unsigned long)done);
              if(re != noErr || done)
                NLOG("    ⚠ the card's node was NOT found -- theID will be an empty RegEntryID,");
              NLOG("      which is still better formed than the four zero bytes k55..k108 sent."); }

            /* ★★★ THIS STRING IS WHAT THE USER SEES. EthernetShim.pdf, "Installing a
             * driver": "The DRVName is the name that Open Transport uses in the TCP/IP Control
             * Panel Connect via pop up." k106 shipped "AirPortShim" and that is exactly what
             * appeared in the pop-up, which confirms the mapping by observation.
             *
             * ⚠ The length byte is written out rather than counted by the compiler, because
             *   Str31 is unsigned char[32] and gcc will not initialise it from a "\p" literal.
             *   It is stated once, here, so it cannot drift from the text -- a wrong count byte
             *   would either truncate the name or run past it into whatever follows.
             *
             * ⚠ THE PORT'S IDENTITY IS NOT THIS STRING. OT names the port record itself (ours
             *   came back as "enet1", module "USBEnet" -- EnetShimLib's, not ours), so this is a
             *   display name only. Changing it should not move the port, but TCP/IP stores the
             *   user's choice, so a re-select may be needed once after the rename. */
            static unsigned char drvName[17] =
                { 15,'A','i','r','P','o','r','t',' ','E','x','t','r','e','m','e',0 };
            static ShimRefNum shimRef = 0;
            EnetShimInterface ib;
            ib.DRVName = (StringPtr)drvName;
            ib.ConnID  = drvCid;
            ib.RefCon  = 0;
            ib.theID   = (UInt32)&sCardNode;    /* a real 16-byte RegEntryID */

            e = (*installProc)(ib, &shimRef);
            gShimRef = shimRef;
            NLOGX("  ★ EnetShimInstallDriver err (0=ok)", (unsigned long)(long)e);
            NLOGX("    ShimRefNum", (unsigned long)shimRef);
            NLOGX("  beacon airport-ot-port err",
                  (unsigned long)Beacon("airport-ot-port",
                                        (UInt32)(e == noErr ? (shimRef | 0x80000000UL)
                                                            : (UInt32)(long)e)));

            /* ★★★ AND THE LINK REPORT, WITHOUT WHICH THE PORT IS CREATED BUT NEVER OFFERED.
             * k56 measured this precisely: registration made five ports into six, the port was
             * NOT private, it was simply INACTIVE --
             *     enet0  gmac     portFlags=00000001  ACTIVE
             *     enet1  USBEnet  portFlags=00000000  ours
             * A port that has never reported a link is not a port OT will drive.
             * USBEnetDriver.c:629 uses exactly this call. */
            if (e == noErr && asyncProc) {
                OSErr ae = (*asyncProc)(shimRef, EnetShim_Link, 1, 0);
                NLOGX("  ★ EnetShimAsyncStatus(Link, up) err (0=ok)", (unsigned long)(long)ae);
                NLOG("  => if the port is now ACTIVE, \"AirPort\" is selectable in TCP/IP with");
                NLOG("     no application running. That is the whole objective of stage 8.");
            }
        }
#endif /* AP_NATIVE_OTMODL */
    }   /* end of if(!sRegistered) -- the pump below re-enters this proc */

#if AP_NATIVE_OTMODL
    /* 8-60: native mode uses its own snapshot pump (reads OTModl$AirPortBCM's live counters); the
     * shim snapshot pump below is compiled out. This re-arms NM itself, so return here. */
    ApBootNativeSnapshotPump(nm);
    return;
#endif

    /* ★★★★★ 8-23: DO NOT RETIRE -- BECOME A BOUNDED SNAPSHOT PUMP.
     *
     * Three builds of logging have landed too early to see what matters, because every dump was
     * triggered by a point in the DRIVER's lifecycle and the question is about the quiet
     * afterwards: TCP/IP binds, Start arrives, and then either DHCP works or it does not. There
     * is no selector for "thirty seconds later". OT never calls EnetHAL_Status (k113: zero
     * polls) and never Closes, because it keeps the port open.
     *
     * This response proc, though, runs at TASK level and holds a connection to the copy OT
     * drives. So it re-arms and asks on a clock -- the same NM-as-pump shape usb2-ehci/resident
     * proved ("the NM pump is alive and polling at task level").
     *
     * ⚠ BOUNDED, ON A WALL CLOCK, AND THEN IT STOPS. A notification that re-arms forever is a
     *   pump that never stops -- [[feedback_guards_must_not_latch]]. TickCount is 60/sec, so the
     *   schedule below is roughly 2s, 10s, 30s and 60s after registration, four snapshots, and
     *   then NMRemove for good. Firing rate does not matter: the schedule is in real time, not
     *   in deliveries. */
    if (nm) {
        /* ⚠⚠ ANCHORED ON THE BIND, NOT ON REGISTRATION -- k114 got this wrong and it would have
         *   cost a reboot to discover. Registration happens during BOOT, so a schedule measured
         *   from here fires its whole sequence inside the first minute at the Finder, long
         *   before anyone can open the TCP/IP control panel. Four snapshots, all reading zero,
         *   all perfectly correct, all measuring the wrong moment.
         *
         * So: poll the phase (three bits, no file, no ring write) until EnetHAL_Start arrives,
         * and only then start the clock. The wait is bounded too -- if nobody ever selects the
         * port, this must not poll for the rest of the uptime. */
        static UInt32 sT0 = 0, sWaitFrom = 0, sLastRejoin = 0, sRejoins = 0;
        static int    sNext = 0, sArmed = 0;
        /* ⚠ SIX samples reaching to two minutes, not four to one. The DHCP exchange begins a few
         *   seconds after the bind and can run ~25 s (k134: NAKs, then nine DISCOVERs out to
         *   secs 16). The old four-shot schedule's last sample at 60 s was fine in principle, but
         *   the block below advanced its slot even on a FAILED resolve, so an intermittent could
         *   spend all four inside the first seconds and never snapshot once DHCP had happened --
         *   which is exactly how k134's only surviving snapshot came to read TX = 0. */
        static const UInt32 kAfterBind[6] = { 120, 900, 1800, 3600, 5400, 7200 };
                                                    /* 2s, 15s, 30s, 60s, 90s, 120s */
        UInt32 now = (UInt32)TickCount();
        Ptr sym; CFragSymbolClass sc;

        if (sWaitFrom == 0) sWaitFrom = now;

        if (!sArmed) {
            UInt32 phase = 0;
            /* ⚠ THROTTLED TO ~4 Hz. At 68 deliveries a second a FindSymbol-and-call on every
             * one is pure waste, and caching the resolved TVector instead is the thing
             * [[reference_os9_findsymbol_no_reference]] explicitly forbids -- FindSymbol takes
             * no reference, so a cached pointer is only as valid as assumptions about the
             * fragment's lifetime. Throttling costs nothing and keeps the lookup honest. */
            static UInt32 sLastPoll = 0;
            CFragConnectionID conn;
            if (sLastPoll != 0 && (now - sLastPoll) < 15u) {
                (void)NMRemove(nm); (void)NMInstall(nm); return; }
            sLastPoll = now;
            conn = PumpResolveDrv();        /* 8-37: this fire's own valid connection */
            { OSErr fe = conn ? FindSymbol(conn, "\pAirPortShimGetPhase", &sym, &sc)
                              : (OSErr)cfragNoSymbolErr;
              if (fe == noErr) phase = (*(UInt32 (*)(void))sym)();
              else if (!sPhaseErrLogged) { sPhaseErrLogged = 1;
                NLOGX("    ⛔ FindSymbol(AirPortShimGetPhase) err", (unsigned long)(long)fe);
                NLOGX("       connection was", (unsigned long)conn); } }
            if (phase & 4u) {                       /* Start arrived: OT bound the port */
                sArmed = 1; sT0 = now;
                NLOGX("  [8-24] ★ BIND SEEN (phase bits)", (unsigned long)phase);
                NLOG("    ⇒ snapshot clock starts NOW: 2/15/30/60/90/120s, each retried till it lands.");
            } else if ((now - sWaitFrom) > 36000u) {   /* 10 minutes of nobody selecting it */
                NLOGX("  [8-24] no bind within 10 minutes; retiring. last phase",
                      (unsigned long)phase);
                (void)NMRemove(nm);
                return;
            }
        }

        if (sArmed && sNext < 6 && (now - sT0) >= kAfterBind[sNext]) {
            CFragConnectionID conn = PumpResolveDrv();   /* 8-37 */
            OSErr fe = conn ? FindSymbol(conn, "\pAirPortShimSnapshot", &sym, &sc)
                            : (OSErr)cfragNoSymbolErr;
            if (fe == noErr) {
                (*(void (*)(void))sym)();
                NLOGX("  [8-24] snapshot written; ticks since the bind", (unsigned long)(now - sT0));
                /* ★ ADVANCE ONLY ON SUCCESS. A failed resolve leaves the slot due, so the next NM
                 *   fire retries it at once instead of surrendering it to the -2802 intermittent.
                 *   The snapshot APPENDS, so the LAST block on disk is always the freshest state
                 *   and taking more than four can only help. */
                sNext++;
            } else if (!sSnapErrLogged) {
                /* ⚠⚠ THE ERROR CODE, BECAUSE k125 THREW IT AWAY AND LEFT ME GUESSING. Logged ONCE
                 *   now, because retry-on-failure would otherwise fill the log with it. */
                sSnapErrLogged = 1;
                NLOGX("    ⛔ FindSymbol(AirPortShimSnapshot) err", (unsigned long)(long)fe);
                NLOGX("       connection was", (unsigned long)conn);
            }
        }

        /* ★★★★★ 8-33: AND WHILE THE RADIO IS DOWN, KEEP RETRYING THE WHOLE BRING-UP.
         *
         * Three runs in a row were lost to intermittents rather than to the thing under test:
         * k121 and k124 came up deaf (zero frames of any type, with MACCTL reading
         * ENABLED|AWAKE|BEACPROMISC and the DMA engine ACTIVE), k122 got an ACK for its auth
         * request and no answer. Each cost a reboot and answered nothing.
         *
         * A TCP/IP re-select clears it by hand, because that makes OT Close and re-Open the port
         * and EnetHAL_Open runs the entire bring-up again. The tester noticed that before I did.
         * This is the same thing without the human: poll the phase, and while joined is clear,
         * ask the driver to redo the bring-up every ~15 seconds.
         *
         * ⚠ NOT a workaround that hides the intermittent. Every attempt narrates itself into the
         *   driver log with its own census, so one reboot now yields a DOZEN samples of a fault
         *   we have only ever seen one sample of per reboot. It measures more, not less --
         *   which is the distinction [[feedback_a_working_workaround_blocks_diagnosis]] turns on.
         *
         * ⚠ Bounded at 40 attempts (~10 minutes) so it cannot poll for the rest of the uptime,
         *   and it stops the moment joined goes true. */
        /* ⚠ TIME FALLBACK on the snapshot count. Advancing sNext only on a successful resolve is
         *   what makes the retry work, but it also means a boot whose connection NEVER resolves
         *   would sit in the snapshot phase forever, re-arming and never retiring. 150 s is well
         *   past the six scheduled samples, so on a healthy boot this is never the reason we leave
         *   -- it only rescues the dead-resolve boot, where rejoin's own FindSymbols would fail
         *   too, into the existing bounded retirement below. */
        if (sArmed && (sNext >= 6 || (now - sT0) >= 9000u)) {
            UInt32 phase = 0;
            CFragConnectionID conn = PumpResolveDrv();   /* 8-37 */
            if (conn && FindSymbol(conn, "\pAirPortShimGetPhase", &sym, &sc) == noErr)
                phase = (*(UInt32 (*)(void))sym)();
            if (phase & 2u) {                       /* joined -- nothing left to do */
                NLOG("  [8-33] joined; retiring the pump.");
                (void)NMRemove(nm);
            } else if (sRejoins >= 40) {
                NLOGX("  [8-33] gave up after attempts", (unsigned long)sRejoins);
                (void)NMRemove(nm);
            } else if ((now - sLastRejoin) >= 900u) {   /* 15 s between attempts */
                sLastRejoin = now;
                sRejoins++;
                NLOGX("  [8-33] radio still not joined; re-bring-up attempt",
                      (unsigned long)sRejoins);
                { OSErr fe = conn ? FindSymbol(conn, "\pAirPortShimRejoin", &sym, &sc)
                                  : (OSErr)cfragNoSymbolErr;
                  if (fe != noErr) {
                    NLOGX("    ⛔ FindSymbol(AirPortShimRejoin) err", (unsigned long)(long)fe);
                    NLOGX("       connection was", (unsigned long)conn); } }
                if (conn && FindSymbol(conn, "\pAirPortShimRejoin", &sym, &sc) == noErr) {
                    UInt32 ok = (*(UInt32 (*)(void))sym)();
                    NLOGX("    AirPortShimRejoin -> joined", (unsigned long)ok);
                    /* ★★★★★ 8-34: AND TELL OPEN TRANSPORT THE LINK CAME UP.
                     *
                     * ⚠ WITHOUT THIS THE WHOLE RETRY IS INVISIBLE. TCP/IP runs DHCP the instant
                     *   the user saves; if the radio is not joined at that moment it fails and
                     *   falls back to link-local. When attempt seven succeeds two minutes later,
                     *   NOTHING tells OT -- the only link report this driver has ever made is the
                     *   single one at registration, long before any join. The machine would sit
                     *   on 169.254.x.x with a working link underneath it, and the run would read
                     *   as another failure.
                     *
                     * ⚠ It is also the honest report. k56 established Link=1 at registration as
                     *   the thing that makes OT offer the port -- but at that moment it was a
                     *   claim about a radio that had not yet associated. THIS one is true when
                     *   it is made, which is the difference between reporting state and
                     *   asserting it. */
                    if (ok && gAsyncProc) {
                        OSErr ae = (*gAsyncProc)(gShimRef, EnetShim_Link, 1, 0);
                        NLOGX("    ★ EnetShimAsyncStatus(Link, up) after a successful retry",
                              (unsigned long)(long)ae);
                        NLOG("      ⇒ OT has been told the link is up; DHCP should re-run.");
                    }
                } else
                    NLOG("    ⛔ AirPortShimRejoin did not resolve -- check ap_shim.exp");
                (void)NMRemove(nm); (void)NMInstall(nm);
            } else {
                (void)NMRemove(nm); (void)NMInstall(nm);
            }
        } else {
            (void)NMRemove(nm); (void)NMInstall(nm);
        }
    }
}

/* Entry point the 68K INIT resolves and calls, once, at INIT time. Task level. */
void InstallMe(void)
{
    NMRecPtr nm;
    NMUPP    upp;
    THz      oldZone;

    BLOG("=== AirPort boot vehicle " AP_BOOT_VER ": InstallMe ran (PPC, via Mixed Mode) ===");
    BLOGX("  beacon airport-init-ran err", (unsigned long)Beacon("airport-init-ran", 0x41504254UL));

    /* ⚠ SYSTEM ZONE FOR BOTH, and the reason is that whatever heap is current at INIT time does
     * not survive the boot. The EHCI note calls this "the h90 idiom: the RD must outlive
     * whichever app hosts boot". A routine descriptor or NMRec left in a doomed heap is a freed
     * pointer that the Notification Manager will happily call later. */
    oldZone = GetZone();
    SetZone(SystemZone());
    upp = NewNMUPP((NMProcPtr)ApBootNMResponse);
    SetZone(oldZone);
    BLOGX("  NM response descriptor (system zone)", (unsigned long)upp);
    if (upp == NULL) { BLOG("  NewNMUPP FAILED -- no deferred half this boot"); return; }

    nm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    BLOGX("  NMRec (system heap)", (unsigned long)nm);
    if (nm == NULL) { BLOG("  NewPtrSysClear FAILED -- no deferred half this boot"); return; }

    nm->qType    = nmType;
    nm->nmResp   = upp;
    nm->nmRefCon = 0x41506274UL;   /* 'APbt' */
    BLOGX("  NMInstall err (0=ok; fires post-boot at task level)",
          (unsigned long)(long)NMInstall(nm));
    BLOG("=== InstallMe returned; boot continues ===");
}
