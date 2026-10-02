/* ap_scanner.c -- OTKrnl$AirPortScanner: the Open Transport PORT SCANNER fragment.
 *
 * ★★★ WHY THIS EXISTS (k188). Through k187 we registered our port by HAND, from the boot vehicle's
 * Notification Manager task (ap_boot.c ApBootRegisterNativePort). That made the port APPEAR in
 * TCP/IP -- but k187 proved on hardware it is second-class: OT opens our module ONCE, asks DL_INFO_REQ
 * + the 0x4f02 framing ioctl (both answered exactly as Apple's OTModl$radio answers, verified against
 * the binary), then closes and NEVER binds. The card joins WPA2 fine; the config is correct; every
 * DLPI reply matches Apple. The one thing we never did is let OT register the port through ITS OWN
 * boot scan. That is what installs a driver into the Open Transport module registry (the ValidateHardware
 * hook fires only then -- MEASURED uncalled for us at k66 precisely because "we register by hand").
 *
 * ★ THE MECHANISM, disassembled from Apple's OTKrnl$USBOTPortScanner (Extensions/Apple Enet DLPI
 * Support) on 2026-09-28. OT discovers a port scanner by a typed 'otan' cfrg-member extension whose
 * first string is the fixed tag "OTKrnl$pScnr"; it then loads that fragment and calls the exported
 * OTScanPorts(scanType) at boot. scanType==kOTInitialScan(0) is the one that registers; ==1 is a
 * post-sleep rescan. Inside OTScanPorts, Apple calls OTCreatePortRef + OTRegisterPort with the exact
 * record below (see scratchpad/reverse-scanner/THREE-FRAGMENT-SPEC.md). We do the same, so OT installs
 * OTModl$AirPortBCM into its module registry and the port becomes ACTIVATABLE, not merely listable.
 *
 * ⚠ OTScanPorts runs at TASK level -- Apple calls GetSharedLibrary from it (task-level-only), so the
 *   File Manager logging here is safe, and so is resolving OTKernelUtilLib the same way ap_boot does.
 * ⚠ The per-port ref handed to OTRegisterPort must OUTLIVE this fragment (CFM may release the scanner
 *   after the scan). Apple uses OTAllocPortMem; we resolve it too, falling back to NewPtrSys. The
 *   OTPortRecord itself may be a local -- OTRegisterPort COPIES it (allocates its own TPortRecord).
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <NameRegistry.h>
#include <CodeFragments.h>
#include <Files.h>
#include <Folders.h>          /* k190: FindFolder -- the log goes to the System Folder */

#define AP_SCANNER_BUILD 1     /* keep in lockstep with the k-number when the scanner changes */

/* ---- OTPortRecord (OpenTransport.h; 0x128 = 296 bytes) + the OTKernelUtilLib primitives we resolve
 * at runtime, exactly as ap_boot.c did. No OT headers needed -- these are the on-the-wire shapes. ---- */
typedef UInt32 OTPortRef_;
typedef struct AP_OTPortRecord {
    OTPortRef_ fRef;                                   /* 0x00 */
    UInt32     fPortFlags, fInfoFlags, fCapabilities, fNumChildPorts;  /* 0x04 0x08 0x0c 0x10 */
    void      *fChildPorts;                            /* 0x14 */
    char       fPortName[36];                          /* 0x18 -- OT assigns enet0/enet1 */
    char       fModuleName[32];                        /* 0x3c -- "AirPortBCM" -> OTModl$AirPortBCM */
    char       fSlotID[8];                             /* 0x5c */
    char       fResourceInfo[32];                      /* 0x64 -- "AirPortCfgHelper" -> OTPortCfg$... */
    char       fReserved[164];                         /* 0x84 .. 0x128 */
} AP_OTPortRecord;
typedef OTPortRef_ (*OTCreatePortRefProc)(UInt8 bus, UInt16 dev, UInt16 slot, UInt16 other);
typedef OSStatus   (*OTRegisterPortProc)(AP_OTPortRecord *info, void *ref);
typedef void *     (*OTAllocPortMemProc)(UInt32 size);

#define AP_kOTPCIBus          3          /* kOTPCIBus */
#define AP_kOTEthernetDevice  10         /* kOTEthernetDevice */
#define AP_kOTPortIsDLPI      0x00000001UL
#define AP_kOTInitialScan     0          /* kOTInitialScan */
#define AP_kOTScanAfterSleep  1          /* kOTScanAfterSleep */

/* ---- logging. OTScanPorts is task level (see header), so the File Manager is safe here, same idiom
 * as ap_boot.c. A Name Registry Beacon is written too as a level-independent backup witness. ---- */
static short gScanLog = 0;
static void Lw(short *ref, const char *name, const char *s)
{
    long n = 0, z = 1;
    if (!*ref) {
        FSSpec sp; Str63 p; int i = 0; short vRef; long dirID;
        while (name[i] && i < 62) { p[i+1] = name[i]; i++; }
        p[0] = (unsigned char)i;
        /* k190: the System Folder, not "the running program's default folder" -- (0,0) scattered the
         * driver's logs across System Folder / Control Panels / Extensions in k189 run 2. */
        if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRef, &dirID) == noErr)
            (void)FSMakeFSSpec(vRef, dirID, p, &sp);
        else
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
#define SLOG(s)     Lw(&gScanLog, "AirPort Scanner Log", (s))
#define SLOGX(s,v)  Lx(&gScanLog, "AirPort Scanner Log", (s), (unsigned long)(v))

/* A Name Registry property is outside every fragment's private globals, so a beacon here is readable
 * regardless of which copy of anything ran -- same rationale as ap_boot.c's Beacon. */
static void Beacon(const char *prop, UInt32 value)
{
    RegEntryID root;
    if (RegistryCStrEntryLookup(NULL, "Devices:device-tree", &root) != noErr) return;
    (void)RegistryPropertyCreate(&root, (char *)prop, (void *)&value, (RegPropertyValueSize)sizeof(value));
    (void)RegistryEntryIDDispose(&root);
}

/* ============================================================================================
 * OTScanPorts -- OT calls this at boot. Register the OTModl$AirPortBCM port THROUGH OT's scan.
 * ========================================================================================== */
void OTScanPorts(UInt32 scanType);
void OTScanPorts(UInt32 scanType)
{
    AP_OTPortRecord    rec;            /* local: OTRegisterPort copies it */
    CFragConnectionID  cid = 0; Ptr a = 0; Str255 en; OSErr e;
    OTCreatePortRefProc mkRef = 0; OTRegisterPortProc regPort = 0; OTAllocPortMemProc allocMem = 0;
    Ptr s; CFragSymbolClass cls; OTPortRef_ pr; OSStatus rr; int i;
    UInt32 *ref;
    const char *mod = "AirPortBCM";        /* OT loads OTModl$ + this */
    const char *cfg = "AirPortCfgHelper";  /* OT loads OTPortCfg$ + this (built as fragment 3) */

    SLOG("=== OTKrnl$AirPortScanner: OTScanPorts CALLED ===");
    SLOGX("  scanner build", (unsigned long)AP_SCANNER_BUILD);
    SLOGX("  scanType (0=initial,1=afterSleep)", (unsigned long)scanType);
    Beacon("airport-scanner-called", 0x53500000UL | (scanType & 0xFFFF));   /* 'SP'|scanType */
    if (scanType != AP_kOTInitialScan) { SLOG("  not the initial scan -- nothing to register"); return; }

    e = GetSharedLibrary("\pOTKernelUtilLib", kPowerPCCFragArch, kLoadCFrag, &cid, &a, en);
    SLOGX("  GetSharedLibrary(OTKernelUtilLib) err (0=ok)", (unsigned long)(long)e);
    if (e != noErr) { Beacon("airport-scanner-err", 0x53450001UL); return; }
    if (FindSymbol(cid, "\pOTCreatePortRef", &s, &cls) == noErr) mkRef    = (OTCreatePortRefProc)s;
    if (FindSymbol(cid, "\pOTRegisterPort",  &s, &cls) == noErr) regPort  = (OTRegisterPortProc)s;
    if (FindSymbol(cid, "\pOTAllocPortMem",  &s, &cls) == noErr) allocMem = (OTAllocPortMemProc)s;
    SLOGX("  OTCreatePortRef resolved", (unsigned long)mkRef);
    SLOGX("  OTRegisterPort  resolved", (unsigned long)regPort);
    SLOGX("  OTAllocPortMem  resolved", (unsigned long)allocMem);
    if (!mkRef || !regPort) { Beacon("airport-scanner-err", 0x53450002UL); return; }

    /* The per-port ref must survive this fragment. Apple uses OTAllocPortMem; fall back to the system
     * heap (NewPtrSys), which also outlives us. It carries our 'APbt' marker so OT hands back a ref we
     * can recognise (the OTModl reads port->0x2c, not this, but the marker aids debugging). */
    ref = allocMem ? (UInt32 *)(*allocMem)(16) : (UInt32 *)NewPtrSys(16);
    if (!ref) { Beacon("airport-scanner-err", 0x53450003UL); return; }
    ref[0] = 0x41504254UL;   /* 'APbt' */

    /* The exact recipe from OTKrnl$USBOTPortScanner (fPortFlags=0, fInfoFlags=1, fCapabilities=9;
     * fModuleName -> OTModl$AirPortBCM; fResourceInfo -> OTPortCfg$AirPortCfgHelper). */
    pr = (*mkRef)(AP_kOTPCIBus, AP_kOTEthernetDevice, 0x0E, 0);
    for (i = 0; i < (int)sizeof(rec); i++) ((char *)&rec)[i] = 0;
    rec.fRef           = pr;
    rec.fPortFlags     = 0;
    rec.fInfoFlags     = AP_kOTPortIsDLPI;   /* 1 */
    rec.fCapabilities  = 9;
    rec.fNumChildPorts = 0;
    rec.fPortName[0]   = 0;                   /* OT assigns */
    for (i = 0; mod[i] && i < 31; i++) rec.fModuleName[i]   = mod[i];
    rec.fSlotID[0]     = 0;
    for (i = 0; cfg[i] && i < 31; i++) rec.fResourceInfo[i] = cfg[i];

    rr = (*regPort)(&rec, (void *)ref);
    SLOGX("  ★ OTRegisterPort err (0=ok)", (unsigned long)(long)rr);
    SLOGX("  fRef", (unsigned long)pr);
    SLOGX("  OT-assigned fPortName[0..3]", (unsigned long)*(UInt32 *)rec.fPortName);
    Beacon("airport-scanner-regport", (UInt32)rr);
    Beacon("airport-scanner-portref", (UInt32)pr);
    SLOG("=== scan done; OTModl$AirPortBCM registered THROUGH the boot scan ===");
}

/* CFMInitialize -- CFM's fragment-prepare hook (exported for shape-parity with Apple's scanner). We
 * need no prepare-time work: OTScanPorts resolves everything it uses. Kept as a witness only. */
OSErr CFMInitialize(CFragInitBlockPtr initBlock);
OSErr CFMInitialize(CFragInitBlockPtr initBlock)
{
    (void)initBlock;
    Beacon("airport-scanner-cfminit", 0x53430000UL | (AP_SCANNER_BUILD & 0xFFFF));   /* 'SC'|build */
    return noErr;
}
