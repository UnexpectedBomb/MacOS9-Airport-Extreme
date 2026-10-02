/* airport_probe.c -- STAGE 1: DOES MAC OS 9 SEE THE AIRPORT EXTREME CARD AT ALL?
 *
 * ★★★★★ WHY THIS EXISTS, AND WHY IT IS THE FIRST THING BUILT.
 * FEASIBILITY.md §9 makes this the gating experiment for the whole project: every later stage
 * (BAR mapping, SPROM, firmware upload, PHY bring-up, the 802.11 stack) assumes Open Firmware
 * enumerated the card and handed OS 9 a Name Registry node. If it did not, no amount of driver
 * code helps and the first problem to solve is a firmware/OF one instead.
 *
 * This is UNREPORTED TERRITORY. The prior-art sweep found no report, on any machine, in any source,
 * of whether OS 9 enumerates an AirPort Extreme card. MacOS9Lives topic=7243 has a user with this
 * exact card saying only "it only works in OS X. I couldn't make it work in OS 9" -- nobody checked
 * whether the machine could see it. One boot answers it.
 *
 * ⚠⚠ THIS BINARY PERFORMS NO MMIO. Not a gated path, not a disabled one -- there is no code here
 * that dereferences a BAR. It reads the Name Registry and PCI CONFIG space only, both of which are
 * safe against a card that has not been initialised, has no firmware loaded, and may not even be
 * powered. Touching BAR0 on an uninitialised BCM4306 is Stage 2's job, deliberately, because that is
 * the first operation that can hang the machine. [[feedback_measure_every_direction_before_enabling]]
 *
 * ⚠ IT LINKS NO DRIVER SOURCES. There are none yet -- but the rule stands for its own sake, as with
 * SiI3512FlashProbe: an app has no business holding driver-loader imports.
 *
 * ★★★★★ THE ORACLE, AND WHY A BARE "NO 14e4 NODE" WOULD BE WORTHLESS WITHOUT IT.
 * The result we are most likely to get is a NEGATIVE, and a negative is exactly the kind of result
 * that reads identically to a broken test. [[feedback_test_content_not_return_codes]] -- and the
 * SiI3512 f1 run cost a cycle to learn that an oracle must not be able to fail for a reason
 * unrelated to what it tests.
 *
 *   ORACLE: the census must find the ATI RV250 in the AGP slot, vendor 0x1002.
 *
 * We know it is there by a COMPLETELY INDEPENDENT PATH: Apple System Profiler on this machine, run
 * 2026-09-15, reported "SLOT-1 (AGP) / Display card / Card model ATY,RV250 / Card vendor ID 1002".
 * ASP is Apple's code, not ours. If our walk finds the Radeon, the walk works, and "no 14e4" is then
 * a fact about the MACHINE. If our walk finds nothing at all, the walk is broken and the 14e4 result
 * is meaningless -- which is a different bug, in a different place, and the log says so.
 *
 * This is the same shape as SiI3512 oracle B: two unrelated paths agreeing on the same value.
 *
 * ⚠ AND THE FALSE-POSITIVE TRAP, WHICH IS WHY WE DO NOT MATCH ON VENDOR ALONE.
 * 0x14E4 is Broadcom, and Broadcom made Ethernet controllers as well as radios. If this machine's
 * built-in gigabit Ethernet were a Broadcom part, a match on vendor-id alone would find IT and we
 * would cheerfully report success against the wrong device. So the probe prints EVERY 14e4 node it
 * finds, with device-id and OF node name, and only calls something an AirPort Extreme card when the
 * device-id says so. Expected for BCM4306: 0x4320 or 0x4325.
 * (Note the ASP report lists built-in Ethernet as present and up at 1 Gbps, but does not name its
 * vendor -- so this is a real ambiguity to resolve from the log, not a hypothetical one.)
 *
 * ⚠ ALSO NOT AN ANSWER BY ITSELF: Apple System Profiler's PCI section lists only SLOT-1 (AGP) on
 * this machine, with no AirPort card. That is SUGGESTIVE, NOT CONCLUSIVE -- ASP keys its PCI list
 * off the "AAPL,slot-name" property, so a device in an internal, non-user-slot position can be fully
 * enumerated by OF and still never appear there. This probe walks the registry directly and does not
 * care about slot-name, which is precisely why it can answer what ASP could not.
 *
 * WHAT IT REPORTS
 *   1. A full PCI census -- every Name Registry node carrying a vendor-id, with device-id, class,
 *      revision, OF node name and slot-name. This is the control, and it is also a reusable record
 *      of what this machine's OS 9 actually enumerates.
 *   2. Whether any 0x14E4 node exists, and for each: the complete property list (iterated, not a
 *      guessed fixed list -- we do not yet know what OF puts there for this card).
 *   3. For a confirmed BCM4306: the BAR layout decoded from assigned-addresses + AAPL,address, and
 *      the interrupt assignment. §3a expects BAR0 to be 8 KB of memory space.
 *   4. PCI config space, read-only: command register (is memory space even ENABLED?), status, class,
 *      subsystem IDs, raw BARs, interrupt line/pin.
 *   5. A verdict block that states which of the three outcomes occurred, so the run cannot be
 *      ambiguous when it is read back.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <NameRegistry.h>
#include <PCI.h>

#define kFontIDMonaco 4
#define AP_PROBE_VER "a1"

/* --- what we are hunting -------------------------------------------------- */
#define kVendorBroadcom   0x14E4UL
#define kVendorATI        0x1002UL     /* the ORACLE: RV250 in SLOT-1, per ASP 2026-09-15 */
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

/* OF "assigned-addresses" entry = 5 x UInt32: phys.hi, phys.mid, phys.lo, size.hi, size.lo.
 * phys.hi low byte = the BAR's config-register offset; bits 24-25 = address space.
 * Identical decode to esata-sil3512/src/sil3512_os.c map_bar5 -- proven on this machine. */
#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

/* --- Pascal-string helpers + the log, same shape as SiI3512FlashProbe ------ */
static void PCat(Str255 d, const char *s){ short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PCatDec(Str255 d, long v){ char t[12]; short n=0; unsigned long u;
  if(v<0){ if(d[0]<255) d[++d[0]]='-'; u=(unsigned long)(-v); } else u=(unsigned long)v;
  if(!u){ PCat(d,"0"); return; } while(u){ t[n++]=(char)('0'+(u%10)); u/=10; }
  while(n>0&&d[0]<255) d[++d[0]]=t[--n]; }
static void PCatHex(Str255 d, unsigned long v, int digits){ static const char h[]="0123456789ABCDEF";
  int i; for(i=digits-1;i>=0;i--) if(d[0]<255) d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF]; }

static short gLogRef=0, gLogVol=0; static long gLogDir=0;
static Str255 gLines[300]; static short gN=0;

static void LogOpen(void){ FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr) return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Probe Log",&sp)==noErr) FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr) return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr) gLogRef=0; }
/* Every line is flushed. A late crash must never cost the lines already produced -- this project
 * has lost a run to exactly that. */
static void Out(Str255 s){
  if(gLogRef){ long len=s[0]; char cr='\r'; FSWrite(gLogRef,&len,&s[1]); len=1; FSWrite(gLogRef,&len,&cr); FlushVol(NULL,gLogVol); }
  if(gN<300){ BlockMoveData(s,gLines[gN],(long)s[0]+1); gN++; } }
static void Say(const char *s){ Str255 L; L[0]=0; PCat(L,s); Out(L); }
static void Say1(const char *s, unsigned long v){ Str255 L; L[0]=0; PCat(L,s); PCatDec(L,(long)v); Out(L); }
static void SayH(const char *s, unsigned long v, int digits){ Str255 L; L[0]=0; PCat(L,s); PCat(L,"0x"); PCatHex(L,v,digits); Out(L); }

/* --- registry helpers ------------------------------------------------------ */

/* App-side property read: NewPtr, not PoolAllocateResident (that is DriverServices, not ours). */
static OSStatus GetProp(RegEntryID *node, const char *name, void **outBuf, RegPropertyValueSize *outSize)
{
    RegPropertyValueSize size = 0; void *buf; OSStatus err;
    err = RegistryPropertyGetSize(node, name, &size);
    if (err != noErr) return err;
    if (size == 0) return paramErr;
    buf = NewPtr((Size)size);
    if (buf == NULL) return memFullErr;
    err = RegistryPropertyGet(node, name, buf, &size);
    if (err != noErr) { DisposePtr((Ptr)buf); return err; }
    *outBuf = buf; *outSize = size;
    return noErr;
}

/* A UInt32-valued property, or the supplied default when absent. OF stores these big-endian and the
 * CPU is big-endian, so there is no swap here -- unlike the MMIO path Stage 2 will need. */
static UInt32 GetU32(RegEntryID *node, const char *name, UInt32 dflt)
{
    UInt32 v = 0; RegPropertyValueSize sz = sizeof(v);
    if (RegistryPropertyGet(node, name, &v, &sz) != noErr) return dflt;
    return v;
}

/* The OF node name, e.g. "pci14e4,4320". This is not decoration: FEASIBILITY.md §2.1 establishes
 * that an OS 9 network driver's CFM fragment is named after exactly this string, so whatever this
 * prints is the name the driver will have to register under. */
static void SayNodeName(const char *lead, RegEntryID *node)
{
    char *nm = NULL; RegPropertyValueSize sz = 0; Str255 L; long i;
    L[0]=0; PCat(L, lead);
    if (GetProp(node, "name", (void **)&nm, &sz) == noErr) {
        for (i = 0; i < (long)sz && nm[i]; i++) if (L[0] < 255) L[++L[0]] = (unsigned char)nm[i];
        DisposePtr((Ptr)nm);
    } else {
        PCat(L, "(no 'name' property)");
    }
    Out(L);
}

/* Dump one property as hex bytes plus a printable-ASCII gloss. Used for the full property sweep on
 * the Broadcom node: we do NOT yet know what OF publishes for this card, so guessing a fixed list
 * would be how we miss the one property that matters. */
static void SayPropRaw(RegEntryID *node, const char *pname)
{
    unsigned char *b = NULL; RegPropertyValueSize sz = 0; Str255 L; long i, shown;
    if (GetProp(node, pname, (void **)&b, &sz) != noErr) {
        L[0]=0; PCat(L,"      "); PCat(L,pname); PCat(L," = <unreadable>"); Out(L); return;
    }
    shown = (sz > 32) ? 32 : (long)sz;
    L[0]=0; PCat(L,"      "); PCat(L,pname); PCat(L," ["); PCatDec(L,(long)sz); PCat(L,"] =");
    for (i = 0; i < shown; i++) { PCat(L," "); PCatHex(L, b[i], 2); }
    if (sz > shown) PCat(L," ...");
    Out(L);
    /* An ASCII gloss, because "name"/"compatible"/"device_type"/"AAPL,slot-name" are strings and
     * reading them as hex is how you overlook the answer. */
    { int printable = 1;
      for (i = 0; i < shown; i++) if (b[i] != 0 && (b[i] < 32 || b[i] > 126)) { printable = 0; break; }
      if (printable && shown > 1) {
          L[0]=0; PCat(L,"          as text: \"");
          for (i = 0; i < shown; i++) { if (b[i] == 0) { if (i+1 < shown) PCat(L,"\" \""); }
                                        else if (L[0] < 250) L[++L[0]] = b[i]; }
          PCat(L,"\""); Out(L);
      } }
    DisposePtr((Ptr)b);
}

/* --- BAR decode from assigned-addresses + AAPL,address --------------------- */

static void SayBars(RegEntryID *node)
{
    UInt32 *aa=NULL, *la=NULL; RegPropertyValueSize aaSize=0, laSize=0;
    UInt32 i, nEntries, nLog;
    Say("    BAR LAYOUT (decoded from assigned-addresses, paired with AAPL,address)");
    if (GetProp(node, "assigned-addresses", (void **)&aa, &aaSize) != noErr) {
        Say("      !! no 'assigned-addresses' property.");
        Say("         Open Firmware enumerated the node but assigned it NO resources. That is a");
        Say("         different failure from 'card absent' and it is fixable from OF -- record it.");
        return;
    }
    if (GetProp(node, "AAPL,address", (void **)&la, &laSize) != noErr) {
        Say("      (no 'AAPL,address' -- physical addresses only, no OS-visible logical mapping)");
        la = NULL; laSize = 0;
    }
    nEntries = (UInt32)aaSize / (kAddrEntryWords * sizeof(UInt32));
    nLog     = (UInt32)laSize / sizeof(UInt32);
    Say1("      entries = ", nEntries);
    for (i = 0; i < nEntries; i++) {
        UInt32 physHi = aa[i * kAddrEntryWords];
        UInt32 physLo = aa[i * kAddrEntryWords + 2];
        UInt32 sizeLo = aa[i * kAddrEntryWords + 4];
        UInt32 space  = AA_SPACE(physHi);
        UInt32 reg    = AA_REG(physHi);
        Str255 L;
        L[0]=0; PCat(L,"      [");  PCatDec(L,(long)i);  PCat(L,"] cfg reg 0x"); PCatHex(L,reg,2);
        PCat(L,"  space="); PCatDec(L,(long)space);
        PCat(L, (space==2)?" (mem32)" : (space==3)?" (mem64)" : (space==1)?" (I/O)" : " (config)");
        PCat(L,"  phys=0x"); PCatHex(L,physLo,8);
        PCat(L,"  size=0x"); PCatHex(L,sizeLo,8);
        if (la && i < nLog) { PCat(L,"  logical=0x"); PCatHex(L,la[i],8); }
        Out(L);
        /* §3a: BAR0 on a BCM4306 is 8 KB -- 4 KB of the selected backplane core's registers plus a
         * 4 KB SPROM shadow at +0x1000. Flag a match, and flag a mismatch just as loudly. */
        if (reg == 0x10) {
            if (sizeLo == 0x2000UL) {
                Say("           ==> BAR0 is 0x2000 (8 KB), EXACTLY what FEASIBILITY.md §3a predicts for");
                Say("               a BCM4306: 4 KB core window + 4 KB SPROM shadow at +0x1000.");
            } else {
                Say("           ==> ⚠ BAR0 size is NOT 8 KB. §3a's register model is built on an 8 KB");
                Say("               BAR0 (verified from an iBook G4 dmesg). Re-check the assumption");
                Say("               before writing any register code against it.");
            }
        }
    }
    DisposePtr((Ptr)aa);
    if (la) DisposePtr((Ptr)la);
}

static void SayInterrupts(RegEntryID *node)
{
    UInt32 *ip=NULL; RegPropertyValueSize sz=0; UInt32 i, n;
    if (GetProp(node, "AAPL,interrupts", (void **)&ip, &sz) == noErr) {
        n = (UInt32)sz / sizeof(UInt32);
        for (i = 0; i < n; i++) Say1("      AAPL,interrupts = ", ip[i]);
        DisposePtr((Ptr)ip);
        return;
    }
    if (GetProp(node, "interrupts", (void **)&ip, &sz) == noErr) {
        n = (UInt32)sz / sizeof(UInt32);
        for (i = 0; i < n; i++) Say1("      interrupts (OF, not AAPL) = ", ip[i]);
        DisposePtr((Ptr)ip);
        Say("      ⚠ only the raw OF 'interrupts' property is present, not 'AAPL,interrupts'.");
        Say("        The driver installs its handler against the AAPL, form -- note the absence.");
        return;
    }
    Say("      !! no interrupt property at all. A driver cannot install a handler without one.");
}

/* --- PCI config space, read-only ------------------------------------------ */

static void SayConfig(RegEntryID *node)
{
    UInt32 v; UInt16 w; UInt8 b; int i;
    Say("    PCI CONFIG SPACE (read-only; no writes anywhere in this binary)");
    if (ExpMgrConfigReadLong(node, (LogicalAddress)0x00, &v) == noErr)
        SayH("      0x00 vendor/device      = ", v, 8);
    if (ExpMgrConfigReadWord(node, (LogicalAddress)0x04, &w) == noErr) {
        SayH("      0x04 command            = ", (unsigned long)w, 4);
        Say1("           bit0 I/O space enable      = ", (w & 0x0001) ? 1 : 0);
        Say1("           bit1 MEMORY space enable   = ", (w & 0x0002) ? 1 : 0);
        Say1("           bit2 bus master enable     = ", (w & 0x0004) ? 1 : 0);
        if (!(w & 0x0002)) {
            Say("           ⚠ MEMORY SPACE IS DISABLED. The BARs are assigned but the card will not");
            Say("             respond to them until bit 1 is set. Stage 2 must enable it before any");
            Say("             MMIO -- reading BAR0 in this state returns garbage, not registers.");
        }
        if (!(w & 0x0004)) {
            Say("           ⚠ BUS MASTER IS DISABLED. DMA cannot work until bit 2 is set (Stage 4).");
        }
    }
    if (ExpMgrConfigReadWord(node, (LogicalAddress)0x06, &w) == noErr)
        SayH("      0x06 status             = ", (unsigned long)w, 4);
    if (ExpMgrConfigReadLong(node, (LogicalAddress)0x08, &v) == noErr) {
        SayH("      0x08 class/revision     = ", v, 8);
        SayH("           class code             = ", (v >> 8) & 0xFFFFFFUL, 6);
        SayH("           revision id            = ", v & 0xFFUL, 2);
        if (((v >> 16) & 0xFFFFUL) == 0x0280UL)
            Say("           ==> class 0x0280 = NETWORK CONTROLLER / OTHER. Correct for an 802.11 part.");
    }
    for (i = 0; i < 6; i++) {
        if (ExpMgrConfigReadLong(node, (LogicalAddress)(0x10 + i*4), &v) == noErr) {
            Str255 L; L[0]=0; PCat(L,"      0x"); PCatHex(L,(unsigned long)(0x10+i*4),2);
            PCat(L," BAR"); PCatDec(L,(long)i); PCat(L," raw          = 0x"); PCatHex(L,v,8); Out(L);
        }
    }
    if (ExpMgrConfigReadLong(node, (LogicalAddress)0x2C, &v) == noErr) {
        SayH("      0x2C subsystem          = ", v, 8);
        SayH("           subsystem vendor       = ", v & 0xFFFFUL, 4);
        SayH("           subsystem id           = ", (v >> 16) & 0xFFFFUL, 4);
        Say("           ^ record these. Apple's OS X driver matches on subsystem IDs, and they are");
        Say("             the cleanest way to tell an Apple A1026 from a generic BCM4306 card.");
    }
    if (ExpMgrConfigReadByte(node, (LogicalAddress)0x3C, &b) == noErr)
        Say1("      0x3C interrupt line     = ", (unsigned long)b);
    if (ExpMgrConfigReadByte(node, (LogicalAddress)0x3D, &b) == noErr)
        Say1("      0x3D interrupt pin      = ", (unsigned long)b);
}

/* --- the Broadcom node, in full ------------------------------------------- */

static void DumpBroadcomNode(RegEntryID *node, UInt32 did)
{
    RegPropertyIter pcookie; RegPropertyNameBuf pname; Boolean pdone = false;
    OSStatus err; int nprops = 0;

    Say("");
    SayNodeName("    OF node name = ", node);
    SayH("    device-id = ", did, 4);
    if (did == kDevBCM4306_4320 || did == kDevBCM4306_4325) {
        Say("    ==> THIS IS A BCM4306. Both 0x4320 and 0x4325 are BCM4306 IDs (§3).");
        Say("        ⚠ The PCI ID does NOT encode the chip revision -- Linux's pcidev_to_chipid()");
        Say("          lumps 0x4320..0x4325 together, and the b43 table lists 4320 as BOTH rev 2 and");
        Say("          rev 3. Stage 2 must read ChipCommon to get the real revision. Do not branch");
        Say("          on this value.");
    } else {
        Say("    ⚠ Broadcom, but NOT a BCM4306 device id. This may be the built-in Ethernet rather");
        Say("      than the AirPort card -- check the OF node name above before concluding anything.");
    }

    /* Full property sweep. We do not know what OF publishes for this card on this machine, and a
     * guessed fixed list is how the one property that matters gets missed. */
    Say("    ALL PROPERTIES ON THIS NODE:");
    err = RegistryPropertyIterateCreate(node, &pcookie);
    if (err == noErr) {
        for (;;) {
            err = RegistryPropertyIterate(&pcookie, pname, &pdone);
            if (err != noErr || pdone) break;
            SayPropRaw(node, pname);
            if (++nprops > 60) { Say("      ... (property list truncated at 60)"); break; }
        }
        RegistryPropertyIterateDispose(&pcookie);
        Say1("      property count = ", (unsigned long)nprops);
    } else {
        Say("      !! RegistryPropertyIterateCreate failed; falling back to a fixed list.");
        SayPropRaw(node, "name");
        SayPropRaw(node, "compatible");
        SayPropRaw(node, "device_type");
        SayPropRaw(node, "AAPL,slot-name");
    }

    Say("");
    SayBars(node);
    Say("");
    Say("    INTERRUPTS");
    SayInterrupts(node);
    Say("");
    SayConfig(node);
}

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i, y;
    RegEntryIter cookie; RegEntryID entry; Boolean done = false, first = true;
    OSStatus err;
    int nPci = 0, nBroadcom = 0, nBcm4306 = 0, sawOracleATI = 0;
    RegEntryID bcmNode; UInt32 bcmDid = 0; int haveBcm = 0;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    LogOpen();

    bounds.left=8; bounds.top=40; bounds.right=8+700; bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Extreme Probe " AP_PROBE_VER " - enumeration only, no MMIO",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9); }

    Say("=== AIRPORT EXTREME (BCM4306) PROBE " AP_PROBE_VER " -- STAGE 1 ===");
    Say("  Question:      does Mac OS 9 enumerate the AirPort Extreme card?");
    Say("  Discriminator: a Name Registry node with vendor-id 0x14E4 and a BCM4306 device-id.");
    Say("  Oracle:        the census must also find the ATI RV250 (vendor 0x1002) that Apple System");
    Say("                 Profiler reported in SLOT-1 on 2026-09-15. If we cannot see a card we know");
    Say("                 is there, a negative result means nothing.");
    Say("  This binary performs NO MMIO and writes NO registers.");
    Say("");
    Say("[1] PCI CENSUS -- every Name Registry node carrying a vendor-id");
    Say("");

    err = RegistryEntryIterateCreate(&cookie);
    if (err != noErr) {
        Say1("!! RegistryEntryIterateCreate failed, err = ", (unsigned long)err);
        goto verdict;
    }
    for (;;) {
        UInt32 vid, did, classrev;
        Str255 L;
        /* ⚠ [[reference_os9_nameregistry_iterate]]: the FIRST call takes kRegIterDescendants and
         * every call after it takes kRegIterContinue. Passing kRegIterDescendants every time returns
         * only top-level roots -- zero nodes -- and reads EXACTLY like absent hardware, which on this
         * probe would be a catastrophic false negative. */
        err = RegistryEntryIterate(&cookie, first ? kRegIterDescendants : kRegIterContinue, &entry, &done);
        first = false;
        if (err != noErr || done) break;

        { UInt32 probe = 0; RegPropertyValueSize psz = sizeof(probe);
          if (RegistryPropertyGet(&entry, "vendor-id", &probe, &psz) != noErr) continue;
          vid = probe & 0xFFFFUL; }
        did      = GetU32(&entry, "device-id", 0xFFFFFFFFUL) & 0xFFFFUL;
        classrev = GetU32(&entry, "class-code", 0xFFFFFFFFUL);
        nPci++;

        L[0]=0; PCat(L,"  vendor=0x"); PCatHex(L,vid,4);
        PCat(L,"  device=0x"); PCatHex(L,did,4);
        if (classrev != 0xFFFFFFFFUL) { PCat(L,"  class=0x"); PCatHex(L,classrev & 0xFFFFFFUL,6); }
        Out(L);
        SayNodeName("      name = ", &entry);
        { char *sn=NULL; RegPropertyValueSize snsz=0;
          if (GetProp(&entry,"AAPL,slot-name",(void**)&sn,&snsz)==noErr) {
              Str255 S; long k; S[0]=0; PCat(S,"      AAPL,slot-name = ");
              for(k=0;k<(long)snsz && sn[k];k++) if(S[0]<255) S[++S[0]]=(unsigned char)sn[k];
              Out(S); DisposePtr((Ptr)sn); } }

        if (vid == kVendorATI) {
            sawOracleATI = 1;
            Say("      ==> ORACLE: this is the ATI card ASP reported in SLOT-1. The walk works.");
        }
        if (vid == kVendorBroadcom) {
            nBroadcom++;
            Say("      ==> ★★★ BROADCOM NODE FOUND.");
            if (did == kDevBCM4306_4320 || did == kDevBCM4306_4325) {
                nBcm4306++;
                if (!haveBcm) { bcmNode = entry; bcmDid = did; haveBcm = 1; }
            }
        }
    }
    RegistryEntryIterateDispose(&cookie);

    Say("");
    Say1("  PCI nodes with a vendor-id = ", (unsigned long)nPci);
    Say1("  Broadcom (0x14E4) nodes    = ", (unsigned long)nBroadcom);
    Say1("  of those, BCM4306 ids      = ", (unsigned long)nBcm4306);

    if (haveBcm) {
        Say("");
        Say("[2] THE BCM4306 NODE, IN FULL");
        DumpBroadcomNode(&bcmNode, bcmDid);
    }

verdict:
    Say("");
    Say("=== VERDICT ===");
    if (!sawOracleATI) {
        Say("  ✗ ORACLE FAILED. The census did not find the ATI RV250 that Apple System Profiler");
        Say("    reports in SLOT-1 of this machine. THE WALK IS BROKEN, not the hardware.");
        Say("    Any conclusion about the AirPort card from this run is WORTHLESS. Suspect the");
        Say("    iterate (kRegIterDescendants/kRegIterContinue) before suspecting the card.");
    } else if (nBcm4306 > 0) {
        Say("  ✓✓✓ THE CARD IS ENUMERATED. Mac OS 9 sees a BCM4306 in the Name Registry, and the");
        Say("      oracle confirms the walk is sound. STAGE 1 PASSES -- the project rests on a real");
        Say("      foundation and Stage 2 (core select, SHM self-test, SPROM) can proceed.");
        Say("      Record the OF node name above: §2.1 says the driver's CFM fragment is named after");
        Say("      it, and that settles the open pci14e4,4320 vs OTModl$pci14e4,4320 question.");
    } else if (nBroadcom > 0) {
        Say("  ~ A Broadcom node exists but carries no BCM4306 device id. Read the node names above:");
        Say("    this is most likely the built-in Ethernet, not the AirPort card. Treat as a NEGATIVE");
        Say("    for the AirPort card unless a node name says otherwise.");
    } else {
        Say("  ✗ NO BROADCOM NODE. The oracle passed, so the walk works and this is a fact about the");
        Say("    MACHINE: Open Firmware is not enumerating the AirPort Extreme card into the OS 9");
        Say("    device tree.");
        Say("    THIS DOES NOT NECESSARILY MEAN THE PROJECT IS DEAD. Check, in order:");
        Say("      1. Is the card actually seated, and does Tiger still see it? (Known-good control.)");
        Say("      2. Has this machine had the Open Firmware DOWNGRADE applied? A 2024 report");
        Say("         (MacOS9Lives topic=7248) says that route 'caused loss of AirPort Extreme and");
        Say("         Bluetooth'. ASP reports Mac OS ROM 10.2.1 Generic, which is the patched-ROM");
        Say("         route rather than the downgrade -- but confirm the OF version directly.");
        Say("      3. Does the MacOS9Lives ROM's OF probe the AirPort slot at all? If OF skips it,");
        Say("         the fix is an OF/ROM problem, not a driver problem -- and that is a different");
        Say("         project with different prior art.");
    }
    Say("");
    Say("=== done. Log: 'AirPort Probe Log' in the System Folder. ===");

    if (win) {
        SetPort((GrafPtr)win);
        y = 12;
        for (i = 0; i < gN; i++) { MoveTo(6, y); DrawString(gLines[i]); y += 11; }
    }
    for (;;) {
        if (WaitNextEvent(everyEvent, &evt, 10, NULL)) {
            if (evt.what == keyDown || evt.what == mouseDown) break;
            if (evt.what == updateEvt && win) {
                BeginUpdate(win); SetPort((GrafPtr)win);
                y = 12;
                for (i = 0; i < gN; i++) { MoveTo(6, y); DrawString(gLines[i]); y += 11; }
                EndUpdate(win);
            }
        }
    }
    if (gLogRef) { FSClose(gLogRef); FlushVol(NULL, gLogVol); }
    return 0;
}
