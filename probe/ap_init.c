/* ap_init.c -- the 68K half of the BOOT VEHICLE: an 'INIT' code resource that loads the PowerPC
 * fragment embedded beside it and Mixed-Mode-calls InstallMe, once, at startup.
 *
 * ★ THIS IS usb2-ehci/resident/ehci_initdrv.c's SHAPE, deliberately. That vehicle is not a
 *   design being tried here for the first time -- it runs on this machine every boot, and the
 *   "EHCIInit INIT Log" it writes is dated the same boot as k103. Copying a proven vehicle is the
 *   cheapest correct move available. [[reference_os9_init_resident_driver]].
 *
 * ⚠⚠ NewRoutineDescriptorTrap, NOT NewRoutineDescriptor. In a 68K code resource the plain form is
 *   a no-op passthrough that hands back the address you gave it, so the "descriptor" is the raw
 *   PowerPC entry and calling it executes PowerPC bytes as 68K. The EHCI work crashed on exactly
 *   this (the r2b1 lesson) and the check below -- descriptor != symAddr -- is the cheap guard it
 *   left behind.
 *
 * ⚠ This file is 68K and Retro68 builds it as a flat code resource, so RETRO68_RELOCATE() must
 *   run before any global is touched and Retro68FreeGlobals() must run on the way out.
 *   Retro68 cannot build a PowerPC INIT at all -- [[reference_retro68_no_ppc_init]] -- which is
 *   precisely why the real work lives in the PPC fragment this loads.
 *
 * ⛔ RECOVERY, because this is the first code this project has ever run at boot: if a bad INIT
 *   stops the machine booting, hold SHIFT at startup to disable extensions, then remove the file.
 */
#include <OSUtils.h>
#include <MacMemory.h>   /* NewPtrSys, BlockMoveData */
#include <Resources.h>
#include <Gestalt.h>
#include <CodeFragments.h>
#include <MixedMode.h>
#include <Files.h>
#include "Retro68Runtime.h"

enum { uppInstallMeProcInfo = kCStackBased };   /* void InstallMe(void) */

static short gLog = 0;
static void L(const char *s)
{
    long n = 0, z = 1;
    if (!gLog) {
        FSSpec sp;
        (void)FSMakeFSSpec(0, 0, "\pAirPort INIT Log", &sp);
        (void)FSpDelete(&sp);
        if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gLog);
    }
    if (gLog) { while (s[n]) n++; (void)FSWrite(gLog, &n, (Ptr)s);
                (void)FSWrite(gLog, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void Lx(const char *label, unsigned long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; L(b);
}

void _start(void)
{
    long              cfmAttr = 0;
    Handle            h       = NULL;
    CFragConnectionID connID  = (CFragConnectionID)0;
    Ptr               mainAddr, symAddr;
    Ptr               img = NULL;
    Size              sz  = 0;
    Str255            errName;
    CFragSymbolClass  symClass;
    UniversalProcPtr  upp;
    OSErr             err;
    Boolean           haveConn = false;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    L("=== AirPort boot vehicle k149: 68K INIT _start ran ===");

    if (Gestalt(gestaltCFMAttr, &cfmAttr) != noErr) { L("Gestalt(CFM) FAILED"); goto done; }
    if (!(cfmAttr & (1L << gestaltCFMPresent)))     { L("CFM NOT present");     goto done; }

    h = Get1Resource('PPC ', 128);
    if (h == NULL) { L("Get1Resource('PPC ',128) == NULL -- the fragment is not in this file");
                     goto done; }

    /* ⛔⛔ THE PEF MUST BE COPIED INTO THE SYSTEM HEAP FIRST. THIS IS WHAT k104 CRASHED ON.
     *
     * k104 passed `*h` -- the resource handle's own memory -- straight to GetMemFragment. The
     * INIT ran, InstallMe ran, NMInstall succeeded, and then the System closed this extension's
     * resource file (which it does once the INIT returns), the handle's memory went back to the
     * heap, and something else took it. At Finder time the Notification Manager called our
     * response proc and the PowerPC executed whatever 68K code now lived there:
     *
     *     PowerPC illegal instruction at 0134DFA0      CurApName Finder,  Int 0
     *       0134DFA0  dc.l 0x000C2F0C                  LR FFCECE20 (ROM = the NM calling us)
     *       0134DFA4  dc.l 0x4EBA01B0                  <- 68K JSR *+$01B0(PC)
     *       0134DFA8  andi. r7,r0,0x244C               <- 68K MOVEQ #7,D0 / MOVEA.L A4,A2
     *     No procedure name
     *
     * ⚠ kPrivateCFragCopy does NOT copy the container. It means "give me my own instance if this
     *   fragment is already prepared" -- the memory we hand in must stay valid for the life of
     *   the connection, and a resource handle does not.
     *
     * usb2-ehci/resident/bootmain.c:1840 does exactly the four lines below, and its own comment
     * says so in words ("NewPtrSys-copies our own PEF"). I had read that comment and did not
     * apply it. [[feedback_consult_prior_art_constantly]], [[reference_os9_preload_defeats_setzone]]
     * -- the same family: memory that looks permanent and is not.
     *
     * ⚠ `img` is deliberately NEVER disposed. It IS the fragment for the rest of the boot. */
    sz  = GetHandleSize(h);
    img = NewPtrSys(sz);
    Lx("PEF size", (unsigned long)sz);
    Lx("NewPtrSys image (0 = FAILED)", (unsigned long)img);
    if (img == NULL) { L("  NewPtrSys FAILED -- cannot make the fragment resident"); goto done; }
    HLock(h); BlockMoveData(*h, img, sz); HUnlock(h);

    /* ★ The range, logged so a future crash PC can be checked against it in one glance rather
     * than guessed at. k104's crash address meant nothing until it was disassembled by hand. */
    Lx("fragment lives in [img, img+size) -- end", (unsigned long)((unsigned long)img + sz));

    /* ⚠ SYSTEM ZONE ACROSS THE PREPARE, as cheap insurance on the OTHER half of the same bug.
     * k104's register dump showed TOC = 0x003B8848 while the PC was at 0x0134DFA0 -- the
     * fragment's DATA section had survived in one region while its CODE was gone from another.
     * So CFM used the container in place for code and allocated data separately, and only the
     * code died. The data landing somewhere durable was luck, not design: whatever zone is
     * current during the extension parade is not guaranteed to outlive boot. Forcing the system
     * zone here costs two calls and removes the variable. (h90 idiom, same as InstallMe's
     * descriptor and NMRec.) */
    { THz oldZone = GetZone();
      SetZone(SystemZone());
      err = GetMemFragment(img, sz, "\pAirPortBootVehicle", kPrivateCFragCopy,
                           &connID, &mainAddr, errName);
      SetZone(oldZone); }
    Lx("GetMemFragment err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto done;
    haveConn = true;

    err = FindSymbol(connID, "\pInstallMe", &symAddr, &symClass);
    Lx("FindSymbol(InstallMe) err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto done;

    upp = NewRoutineDescriptorTrap((ProcPtr)symAddr, uppInstallMeProcInfo, kPowerPCISA);
    Lx("InstallMe descriptor (0 = FAILED; must differ from symAddr)", (unsigned long)upp);
    Lx("  symAddr, for that comparison", (unsigned long)symAddr);
    if (upp == NULL || (Ptr)upp == symAddr) {
        L("  ⛔ descriptor == symAddr: NewRoutineDescriptorTrap did not build one.");
        L("     Calling it would execute PowerPC bytes as 68K. NOT calling. (the r2b1 lesson)");
        goto done; }

    L(">>> calling InstallMe: writes beacons and arms the NM. See 'AirPort Boot Log'. <<<");
    (*(void (*)(void))upp)();
    L("<<< InstallMe returned; boot continues >>>");
    DisposeRoutineDescriptorTrap(upp);

done:
    /* ⚠ The connection is deliberately NOT closed. This fragment armed a Notification whose
     * response proc lives inside it; dropping the connection could unload the code the
     * Notification Manager is going to call after boot. EHCI can close its equivalent only
     * because InstallDriverFromMemory has already handed a separate resident copy to the Device
     * Manager -- we have no such owner, so the INIT's connection IS the residency.
     * [[reference_os9_uim_lifetime_bound_to_loader]] is the same lesson from the other side. */
    if (haveConn) Lx("holding the CFM connection open for residency", (unsigned long)connID);
    Retro68FreeGlobals();
}
