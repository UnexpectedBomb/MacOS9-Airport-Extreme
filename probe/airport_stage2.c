/* airport_stage2.c -- STAGE 2: TALK TO THE SILICON. CHIPCOMMON, CORE CENSUS, SPROM.
 *
 * ★★★★★ WHAT STAGE 1 LEFT US, AND WHY THIS RUN IS SHAPED THE WAY IT IS.
 * Stage 1 (probe a1, 2026-09-16) proved Mac OS 9 enumerates the card: node `pci80211`,
 * 14e4:4320, revision-id 0x03, BAR0 = 8 KB at 0x80086000, AAPL,interrupts 34. It also found
 * two things that dictate this run's first two instructions:
 *
 *   1. ⚠ MEMORY SPACE IS DISABLED. Stage 1 read command = 0x0004: bus master set, memory decode
 *      CLEAR. Open Firmware assigned the BAR and never enabled the card to answer to it. So the
 *      FIRST thing this app does is set command bit 1. Skip that and every MMIO read returns
 *      bus-float garbage that is indistinguishable from dead silicon -- we would "discover" a
 *      broken chip and burn cycles chasing it.
 *   2. ⚠ local-mac-address is F1:F2:F3:F4:F5:F6, a placeholder OF invented. The real address lives
 *      in the SPROM. That dummy is a gift: it gives this run a DISTINCTIVE WRONG ANSWER, so a
 *      failed SPROM read cannot masquerade as a successful one.
 *
 * ⚠⚠ THIS BINARY WRITES. Stage 1 was provably read-only; this one is not, and pretending otherwise
 * would be worse than the writes. Exactly two config-space registers are written, and NO MMIO
 * register is ever written:
 *      config 0x04 (command)       <- set bit 1, memory space enable.  Required; see above.
 *      config 0x80 (SSB_BAR0_WIN)  <- the SiliconBackplane core-select window.  This is how you
 *                                     reach any core at all; it is not optional and it is not a
 *                                     device-state change -- it just points the 4 KB window.
 * There is no MMIO write path in this file. Not gated, not disabled -- absent.
 *
 * ★★★★★ WHAT IS DELIBERATELY *NOT* HERE: THE SHM SELF-TEST.
 * FEASIBILITY.md §9 originally put b43's SHM pattern test (0x55663344 / 0xAABBCCDD to SHM words
 * 0-7) in Stage 2. It is moved to Stage 3, on purpose:
 *   SHM is reachable only once the 802.11 core is ENABLED AND OUT OF RESET. In b43,
 *   b43_validate_chipaccess() runs AFTER b43_wireless_core_reset() -- it is part of core bring-up,
 *   not part of enumeration. Pulling it into this run would mean writing TMSLOW and cycling the
 *   core's reset line, i.e. doing Stage 3's job inside a run whose question is "what is this chip".
 * Splitting them keeps each run's question answerable by itself. This run still carries TWO
 * independent discriminators, which is enough.
 *
 * ⚠ THE SPROM IS READABLE WITHOUT ANY OF THAT, and that is why it is here rather than later.
 * BAR0's lower 4 KB is the SELECTED CORE's register window; the upper 4 KB (+0x1000) is the SPROM
 * shadow, which is NOT part of any core window and does not move when the core selection changes.
 * So the SPROM can be read with no core enabled, no clock started, no reset touched.
 *
 * ORDERING IS THE SAFETY MECHANISM. Operations run safest-first, and every line is flushed to disk
 * before the next one executes. If the machine hangs, the log localises it exactly:
 *      [1] config writes        -- cannot hang; config space always responds
 *      [2] first MMIO read      -- THE risky step. If the app dies, it dies here, and the log's
 *                                  last line names it. That tells us an OF logical address is not
 *                                  reachable from an application context and the probe must move
 *                                  into a driver -- a completely different finding from "bad chip".
 *      [3] core census          -- reads only
 *      [4] SPROM                -- reads only, and the primary discriminator
 *
 * ★★★★★ THE ORACLES. Two, independent, both predicted before the run.
 *   A: ChipCommon must report CHIP ID 0x4306. We reached this card by matching PCI 14e4:4320 in the
 *      Name Registry -- a completely different register file, read through a different mechanism
 *      (config space vs the backplane window). Two unrelated paths agreeing on "this is a 4306" is
 *      not something a broken MMIO read produces. [[feedback_test_content_not_return_codes]]
 *   B: the SPROM MAC at byte offset 0x0048 must be a sane unicast address, must NOT be the
 *      F1:F2:F3:F4:F5:F6 placeholder, and must NOT be all-FF (an unprogrammed or unreachable SPROM
 *      reads as FF). Ideally it carries an Apple OUI. The user can then compare it against what
 *      Tiger reports for this card -- a third, human-checked path.
 * Either oracle passing means the MMIO path works. A fails and B passes => suspect the core-select
 * window. B fails and A passes => suspect the SPROM offset or the 16-bit byte order, not the chip.
 *
 * ⚠ WHAT THIS RUN DOES *NOT* SETTLE, and must not be read as settling: the 802.11 core revision is
 * read here from the backplane, which IS authoritative -- but chip revision and core revision are
 * different numbers and the log prints both separately. §3 expects chip rev 3 and 802.11 core rev 5.
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
#include <Timer.h>
#include <NameRegistry.h>
#include <PCI.h>

#define kFontIDMonaco 4
#define AP_S2_VER "b1"

#define kVendorBroadcom   0x14E4UL
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

/* --- SiliconBackplane, from include/linux/ssb/ssb_regs.h (§3a) -------------- */
#define SSB_ENUM_BASE     0x18000000UL  /* backplane enumeration space base            */
#define SSB_CORE_SIZE     0x1000UL      /* each core gets a 4 KB register window       */
#define SSB_BAR0_WIN      0x80UL        /* PCI CONFIG register: which core the window shows */
#define SSB_SPROM_BASE1   0x1000UL      /* SPROM shadow, BAR0 + this. NOT in a core window. */

/* Per-core registers live at the TOP of the core's 4 KB window. ⚠ IDLOW is 0x0FF8 and IDHIGH is
 * 0x0FFC -- an earlier note in this project had IDHIGH at 0x0FF8, which is wrong and would have
 * decoded the low ID word as if it were the high one. */
#define SSB_IDLOW         0x0FF8UL
#define SSB_IDHIGH        0x0FFCUL
#define SSB_TMSLOW        0x0F98UL
#define SSB_TMSHIGH       0x0F9CUL

/* SSB_IDHIGH field decode. Revision is split across two fields -- miss the high part and every
 * core above rev 15 reads wrong. */
#define IDHIGH_CC(v)      (((v) >> 4) & 0xFFFUL)          /* core code                  */
#define IDHIGH_REV(v)     (((((v) & 0x7000UL) >> 8)) | ((v) & 0xFUL))
#define IDHIGH_VENDOR(v)  (((v) >> 16) & 0xFFFFUL)

/* Core codes we care about naming in the census.
 * ⚠ CORRECTED after the b1 run: 0x805 is MIPS, not PCMCIA. PCMCIA is 0x80D and V90 is 0x807 --
 * both are present on this die and the b1 build printed them as "(unnamed)". Cosmetic only; no
 * result depended on it, since ChipCommon, 802.11 and PCI host were all named correctly. */
#define SSB_DEV_CHIPCOMMON 0x800UL
#define SSB_DEV_PCI        0x804UL
#define SSB_DEV_MIPS       0x805UL
#define SSB_DEV_ETHERNET   0x806UL
#define SSB_DEV_V90        0x807UL
#define SSB_DEV_PCMCIA     0x80DUL
#define SSB_DEV_80211      0x812UL

/* ChipCommon core, register 0x0000 = the chip identification word. */
#define CC_CHIPID         0x0000UL
#define CHIPID_ID(v)      ((v) & 0xFFFFUL)
#define CHIPID_REV(v)     (((v) >> 16) & 0xFUL)
#define CHIPID_PKG(v)     (((v) >> 20) & 0xFUL)
#define CHIPID_NCORES(v)  (((v) >> 24) & 0xFUL)

/* SPROM rev-1 layout (§3a). Byte offsets; the shadow is read as 16-bit words, so word = byte/2. */
#define SPROM_WORDS_R1    64            /* 128 bytes */
#define SPROM_IL0MAC      0x0048UL      /* 6-byte 802.11b/g MAC -- the discriminator */
#define SPROM_BFLLO       0x0072UL      /* board flags, low word (rev 1 has no high word) */
#define SPROM_AGAIN       0x0074UL      /* antenna gain -- Apple programs this badly, see below */
#define SPROM_REVISION    0x007EUL      /* rev in 0x00FF, CRC8 in 0xFF00 */

/* OF assigned-addresses decode, identical to Stage 1 and to sil3512_os.c. */
#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

/* --- byte-reversing MMIO ---------------------------------------------------
 * ⚠ THE BCM43xx REGISTER FILE IS LITTLE-ENDIAN AND THE G4 IS NOT. Linux hides this behind
 * ioread16/ioread32, which are defined as little-endian accessors on every architecture. On PowerPC
 * that means a byte swap, so we use lwbrx/lhbrx -- exactly the pattern sil_flash_probe.c uses and
 * which is proven on this machine. Get this wrong and the chip ID reads as 0x0643 instead of 0x4306,
 * which looks like the wrong chip rather than the wrong byte order. */
static inline UInt32 ap_r32(volatile void *base, UInt32 off)
{
    volatile UInt8 *p = (volatile UInt8 *)base + off;
    UInt32 v;
    __asm__ __volatile__("lwbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline UInt16 ap_r16(volatile void *base, UInt32 off)
{
    volatile UInt8 *p = (volatile UInt8 *)base + off;
    UInt32 v;
    __asm__ __volatile__("lhbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    return (UInt16)v;
}

/* --- log, same shape as Stage 1 -------------------------------------------- */
static void PCat(Str255 d, const char *s){ short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PCatDec(Str255 d, long v){ char t[12]; short n=0; unsigned long u;
  if(v<0){ if(d[0]<255) d[++d[0]]='-'; u=(unsigned long)(-v); } else u=(unsigned long)v;
  if(!u){ PCat(d,"0"); return; } while(u){ t[n++]=(char)('0'+(u%10)); u/=10; }
  while(n>0&&d[0]<255) d[++d[0]]=t[--n]; }
static void PCatHex(Str255 d, unsigned long v, int digits){ static const char h[]="0123456789ABCDEF";
  int i; for(i=digits-1;i>=0;i--) if(d[0]<255) d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF]; }

static short gLogRef=0, gLogVol=0; static long gLogDir=0;
static Str255 gLines[260]; static short gN=0;

static void LogOpen(void){ FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr) return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Stage2 Log",&sp)==noErr) FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr) return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr) gLogRef=0; }
/* ⚠ EVERY LINE IS FLUSHED BEFORE THE NEXT EXECUTES. On this run that is not tidiness, it is the
 * whole containment strategy: if the first MMIO read hangs the machine, the log on disk still ends
 * with the line that says we were about to do it. */
static void Out(Str255 s){
  if(gLogRef){ long len=s[0]; char cr='\r'; FSWrite(gLogRef,&len,&s[1]); len=1; FSWrite(gLogRef,&len,&cr); FlushVol(NULL,gLogVol); }
  if(gN<260){ BlockMoveData(s,gLines[gN],(long)s[0]+1); gN++; } }
static void Say(const char *s){ Str255 L; L[0]=0; PCat(L,s); Out(L); }
static void Say1(const char *s, unsigned long v){ Str255 L; L[0]=0; PCat(L,s); PCatDec(L,(long)v); Out(L); }
static void SayH(const char *s, unsigned long v, int digits){ Str255 L; L[0]=0; PCat(L,s); PCat(L,"0x"); PCatHex(L,v,digits); Out(L); }

/* --- registry helpers (same as Stage 1) ------------------------------------ */
static OSStatus GetProp(RegEntryID *node, const char *name, void **outBuf, RegPropertyValueSize *outSize)
{
    RegPropertyValueSize size = 0; void *buf; OSStatus err;
    err = RegistryPropertyGetSize(node, name, &size);
    if (err != noErr || size == 0) return (err != noErr) ? err : paramErr;
    buf = NewPtr((Size)size);
    if (buf == NULL) return memFullErr;
    err = RegistryPropertyGet(node, name, buf, &size);
    if (err != noErr) { DisposePtr((Ptr)buf); return err; }
    *outBuf = buf; *outSize = size;
    return noErr;
}

static OSStatus FindBroadcomNode(RegEntryID *outNode, UInt32 *outDid)
{
    RegEntryIter cookie; RegEntryID entry; Boolean done=false, first=true; OSStatus err;
    err = RegistryEntryIterateCreate(&cookie);
    if (err != noErr) return err;
    for (;;) {
        UInt32 vid=0, did=0; RegPropertyValueSize sz;
        /* ⚠ [[reference_os9_nameregistry_iterate]] -- first call kRegIterDescendants, all others
         * kRegIterContinue. Stage 1 depended on this and so does this run. */
        err = RegistryEntryIterate(&cookie, first ? kRegIterDescendants : kRegIterContinue, &entry, &done);
        first = false;
        if (err != noErr || done) break;
        sz = sizeof(vid);
        if (RegistryPropertyGet(&entry, "vendor-id", &vid, &sz) != noErr) continue;
        if ((vid & 0xFFFFUL) != kVendorBroadcom) continue;
        sz = sizeof(did);
        if (RegistryPropertyGet(&entry, "device-id", &did, &sz) != noErr) continue;
        did &= 0xFFFFUL;
        if (did != kDevBCM4306_4320 && did != kDevBCM4306_4325) continue;
        *outNode = entry; *outDid = did;
        RegistryEntryIterateDispose(&cookie);
        return noErr;
    }
    RegistryEntryIterateDispose(&cookie);
    return -1;
}

/* BAR0's OS-visible logical address, from assigned-addresses paired with AAPL,address.
 * Stage 1 read these as cfg reg 0x10 / mem32 / logical 0x80086000. */
static OSStatus MapBar0(RegEntryID *node, volatile void **outBase, UInt32 *outSize)
{
    UInt32 *aa=NULL, *la=NULL; RegPropertyValueSize aaSize=0, laSize=0;
    UInt32 i, nEntries; OSStatus result = paramErr;
    if (GetProp(node, "assigned-addresses", (void **)&aa, &aaSize) != noErr) return paramErr;
    if (GetProp(node, "AAPL,address", (void **)&la, &laSize) != noErr) { DisposePtr((Ptr)aa); return paramErr; }
    nEntries = (UInt32)aaSize / (kAddrEntryWords * sizeof(UInt32));
    for (i = 0; i < nEntries && (i * sizeof(UInt32)) < (UInt32)laSize; i++) {
        UInt32 physHi = aa[i * kAddrEntryWords];
        if (AA_REG(physHi) == 0x10 && AA_SPACE(physHi) >= 2) {
            *outBase = (volatile void *)la[i];
            *outSize = aa[i * kAddrEntryWords + 4];
            result = noErr;
            break;
        }
    }
    DisposePtr((Ptr)aa); DisposePtr((Ptr)la);
    return result;
}

/* --- the backplane ---------------------------------------------------------- */

static RegEntryID gNode;
static volatile void *gBar0 = 0;
static UInt32 gSelectRetries = 0, gSelectFailures = 0;

/* Point BAR0's lower 4 KB at core `idx`.
 * ⚠ THE READ-BACK LOOP IS NOT OPTIONAL. ssb_pci_switch_coreidx writes the window register, reads it
 * back, compares, and retries with a 10 us delay -- the write is not reliably immediate on this
 * bridge. Skipping the read-back is how you end up reading core N's registers believing they are
 * core M's, which produces plausible-looking nonsense rather than an obvious failure. */
static OSStatus SelectCore(UInt32 idx)
{
    UInt32 want = (idx * SSB_CORE_SIZE) + SSB_ENUM_BASE;
    int attempts = 0;
    for (;;) {
        UInt32 got = 0;
        if (ExpMgrConfigWriteLong(&gNode, (LogicalAddress)SSB_BAR0_WIN, want) != noErr) return paramErr;
        if (ExpMgrConfigReadLong(&gNode, (LogicalAddress)SSB_BAR0_WIN, &got) != noErr) return paramErr;
        if (got == want) return noErr;
        gSelectRetries++;
        if (++attempts > 10) { gSelectFailures++; return paramErr; }
        { UnsignedWide w0, w1; Microseconds(&w0);
          do { Microseconds(&w1); } while ((w1.lo - w0.lo) < 20UL); }  /* ~20 us, no Delay() */
    }
}

static const char *CoreName(UInt32 cc)
{
    switch (cc) {
        case SSB_DEV_CHIPCOMMON: return "ChipCommon";
        case SSB_DEV_PCI:        return "PCI host";
        case SSB_DEV_MIPS:       return "MIPS";
        case SSB_DEV_ETHERNET:   return "Ethernet";
        case SSB_DEV_V90:        return "V.90 modem (unused on this card)";
        case SSB_DEV_PCMCIA:     return "PCMCIA (unused on this card)";
        case SSB_DEV_80211:      return "802.11 MAC  <<< THE ONE WE WANT";
        default:                 return "(unnamed)";
    }
}

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i, y;
    UInt32 did = 0, bar0Size = 0;
    UInt16 cmd = 0;
    UInt32 chipid = 0, chipId = 0, chipRev = 0, nCores = 0;
    UInt32 dot11Rev = 0xFFFFFFFFUL;
    int haveChipCommon = 0, have80211 = 0;
    int oracleA = 0, oracleB = 0;
    UInt16 sprom[SPROM_WORDS_R1];
    UInt8  mac[6];
    int macAllFF = 1, macIsDummy = 0, macMulticast = 0;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    LogOpen();

    bounds.left=8; bounds.top=40; bounds.right=8+700; bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Stage 2 " AP_S2_VER " - ChipCommon / core census / SPROM",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9); }

    Say("=== AIRPORT EXTREME STAGE 2 " AP_S2_VER " -- CHIPCOMMON, CORE CENSUS, SPROM ===");
    Say("  Oracle A: ChipCommon must report chip id 0x4306 (we found this card as PCI 14e4:4320,");
    Say("            through a different register file -- two unrelated paths must agree).");
    Say("  Oracle B: the SPROM MAC at byte 0x0048 must be sane, NOT all-FF, and NOT the");
    Say("            F1:F2:F3:F4:F5:F6 placeholder that Open Firmware invented.");
    Say("  Writes:   config 0x04 (memory space enable) and config 0x80 (core-select window) ONLY.");
    Say("            There is no MMIO write path in this binary.");
    Say("  Not here: the SHM self-test. It needs the 802.11 core enabled and out of reset, which is");
    Say("            Stage 3's job -- b43 runs it after b43_wireless_core_reset(), not before.");
    Say("");

    /* ---- [0] find the card again ---- */
    if (FindBroadcomNode(&gNode, &did) != noErr) {
        Say("!! No BCM4306 node. Stage 1 found one on 2026-09-16 -- if it is gone now, the card has");
        Say("   moved/failed or this is a different machine state. Re-run Stage 1 before anything.");
        goto verdict;
    }
    SayH("[0] BCM4306 node found, device-id = ", did, 4);
    if (MapBar0(&gNode, &gBar0, &bar0Size) != noErr || gBar0 == 0) {
        Say("!! Could not pair BAR0 from assigned-addresses + AAPL,address.");
        goto verdict;
    }
    SayH("    BAR0 logical = ", (unsigned long)gBar0, 8);
    SayH("    BAR0 size    = ", bar0Size, 8);
    if (((unsigned long)gBar0 & 3UL) != 0UL) {
        Say("!! BAR0 logical address is not 4-byte aligned. Refusing to do MMIO -- that is a broken");
        Say("   property pairing, not a broken card.");
        goto verdict;
    }
    if (bar0Size < 0x2000UL) {
        Say("   ⚠ BAR0 is smaller than the 8 KB §3a expects. The SPROM shadow at +0x1000 may not be");
        Say("     inside it. Continuing, but treat the SPROM section with suspicion.");
    }

    /* ---- [1] the two config writes ---- */
    Say("");
    Say("[1] ENABLING MEMORY SPACE (the one thing Stage 1 told us we must do first)");
    if (ExpMgrConfigReadWord(&gNode, (LogicalAddress)0x04, &cmd) != noErr) {
        Say("!! could not read the command register");
        goto verdict;
    }
    SayH("    command before = ", (unsigned long)cmd, 4);
    if (!(cmd & 0x0002)) {
        UInt16 back = 0;
        if (ExpMgrConfigWriteWord(&gNode, (LogicalAddress)0x04, (UInt16)(cmd | 0x0002)) != noErr) {
            Say("!! config write to the command register FAILED. Without memory decode there is");
            Say("   nothing further to do -- every MMIO read below would be meaningless.");
            goto verdict;
        }
        (void)ExpMgrConfigReadWord(&gNode, (LogicalAddress)0x04, &back);
        SayH("    command after  = ", (unsigned long)back, 4);
        if (!(back & 0x0002)) {
            Say("!! bit 1 did NOT stick. The card is refusing memory-space enable. Stop here; this is");
            Say("   a bridge/power problem, not a driver problem.");
            goto verdict;
        }
        Say("    ==> memory space ENABLED. MMIO is now meaningful.");
    } else {
        Say("    (already enabled -- unexpected given Stage 1, but harmless)");
    }

    /* ---- [2] THE FIRST MMIO READ ---- */
    Say("");
    Say("[2] FIRST MMIO READ -- selecting ChipCommon (core index 0) and reading its ID");
    Say("    ⚠ IF THIS APP DIES HERE, the finding is NOT 'the chip is bad'. It is that an Open");
    Say("      Firmware logical address is not reachable from an application context, and the probe");
    Say("      must be rebuilt as a driver. The log ends at this line either way.");
    if (SelectCore(0) != noErr) {
        Say("!! core-select write/read-back never agreed. The backplane window is not responding.");
        goto verdict;
    }
    Say("    core-select window set to 0x18000000 (core 0) and read back OK");
    {
        UInt32 idhigh = ap_r32(gBar0, SSB_IDHIGH);
        UInt32 cc = IDHIGH_CC(idhigh);
        SayH("    core0 SSB_IDHIGH = ", idhigh, 8);
        SayH("      core code   = ", cc, 3);
        Say1("      core rev    = ", IDHIGH_REV(idhigh));
        SayH("      vendor      = ", IDHIGH_VENDOR(idhigh), 4);
        if (idhigh == 0xFFFFFFFFUL || idhigh == 0UL) {
            Say("    !! SSB_IDHIGH reads as all-ones or all-zero -- that is a bus float, not a core.");
            Say("       Memory decode may be enabled but the window is not landing on the chip.");
            goto do_sprom;   /* SPROM is independent; still worth trying */
        }
        if (cc == SSB_DEV_CHIPCOMMON) {
            haveChipCommon = 1;
            chipid  = ap_r32(gBar0, CC_CHIPID);
            chipId  = CHIPID_ID(chipid);
            chipRev = CHIPID_REV(chipid);
            nCores  = CHIPID_NCORES(chipid);
            Say("");
            SayH("    ChipCommon CHIPID raw = ", chipid, 8);
            SayH("      >>> CHIP ID   = ", chipId, 4);
            Say1("      >>> CHIP REV  = ", chipRev);
            Say1("          package   = ", CHIPID_PKG(chipid));
            Say1("          num cores = ", nCores);
            if (chipId == 0x4306UL) {
                oracleA = 1;
                Say("      ==> ORACLE A PASSED. The backplane says 0x4306 and the Name Registry said");
                Say("          PCI 14e4:4320. Two unrelated register files agree, so the MMIO path,");
                Say("          the byte order and the core-select window are all PROVEN.");
                if (chipRev == 3UL) {
                    Say("      ==> CHIP REV 3 CONFIRMED FROM SILICON. §3's inference (and Stage 1's PCI");
                    Say("          revision-id 0x03) held. b43 + v4 firmware is the right reference set.");
                } else {
                    Say1("      ⚠ CHIP REV IS NOT 3, it is ", chipRev);
                    Say("        §3 assumed rev 3 / core rev 5 on strong but indirect evidence. If this");
                    Say("        says rev 2, the reference set changes to b43legacy + core rev 4, and");
                    Say("        openfwwf becomes IMPOSSIBLE (there is no ucode4.asm). Re-read §3b.");
                }
            } else {
                SayH("      ⚠ ORACLE A FAILED. Expected chip id 0x4306, got ", chipId, 4);
                Say("        If this looks byte-swapped (0x0643), the lwbrx accessors are wrong.");
            }
        } else {
            Say("    ⚠ core 0 is not ChipCommon. On a real BCM4306 it should be. Core census follows.");
        }
    }

    /* ---- [3] core census ---- */
    Say("");
    Say("[3] BACKPLANE CORE CENSUS");
    {
        UInt32 idx, limit = nCores;
        if (limit == 0 || limit > 16) {
            limit = 6;   /* chipid_to_nrcores(0x4306) == 6; used when ChipCommon is absent/odd */
            Say1("    (ChipCommon gave no usable core count; walking a default of ", limit);
        }
        for (idx = 0; idx < limit; idx++) {
            UInt32 idhigh, cc, rev; Str255 L;
            if (SelectCore(idx) != noErr) { Say1("    core ", idx); Say("      !! select failed"); continue; }
            idhigh = ap_r32(gBar0, SSB_IDHIGH);
            cc  = IDHIGH_CC(idhigh);
            rev = IDHIGH_REV(idhigh);
            L[0]=0; PCat(L,"    core "); PCatDec(L,(long)idx);
            PCat(L,": code=0x"); PCatHex(L,cc,3);
            PCat(L,"  rev="); PCatDec(L,(long)rev);
            PCat(L,"  vendor=0x"); PCatHex(L,IDHIGH_VENDOR(idhigh),4);
            PCat(L,"  "); PCat(L,CoreName(cc));
            Out(L);
            if (cc == SSB_DEV_80211) {
                have80211 = 1; dot11Rev = rev;
                { UInt32 tl = ap_r32(gBar0, SSB_TMSLOW), th = ap_r32(gBar0, SSB_TMSHIGH);
                  SayH("        TMSLOW  = ", tl, 8);
                  SayH("        TMSHIGH = ", th, 8);
                  Say1("        reset asserted (TMSLOW bit0) = ", (tl & 0x1UL) ? 1 : 0);
                  Say1("        clock enabled (TMSLOW bit16) = ", (tl & 0x10000UL) ? 1 : 0);
                  Say("        ^ Stage 3 drives these. Recorded here as the BEFORE picture, so the");
                  Say("          core-reset sequence has something to be compared against."); }
            }
        }
    }
    if (have80211) {
        Say("");
        Say1("    >>> 802.11 CORE REVISION = ", dot11Rev);
        if (dot11Rev == 5UL) {
            Say("    ==> CORE REV 5 CONFIRMED. This is the number §3 and §3a are built on:");
            Say("        b43 + ucode5/pcm5/b0g0initvals5, the 54-slot key table with RCMTA, the");
            Say("        8 KB BAR0 layout, and the rev-5-only 'needs pcm5 for hwcrypto' rule.");
        } else {
            Say1("    ⚠ CORE REV IS NOT 5, it is ", dot11Rev);
            Say("      This invalidates specific claims in §3/§3a -- the firmware blob names, the key");
            Say("      table size and the hwcrypto gate all key off core rev. Re-derive before coding.");
        }
    } else {
        Say("    !! NO 802.11 CORE FOUND IN THE CENSUS. That would be extraordinary on this card;");
        Say("       suspect the core-select window before believing it.");
    }

do_sprom:
    /* ---- [4] SPROM ---- */
    Say("");
    Say("[4] SPROM (BAR0 + 0x1000, 16-bit reads)");
    Say("    ⚠ The SPROM shadow is NOT part of any core's window -- it does not move when the core");
    Say("      selection changes, so this section is valid regardless of what happened above.");
    if (gBar0 == 0) { Say("    (no BAR0; skipping)"); goto verdict; }
    {
        int w; int allFF = 1, allZero = 1;
        for (w = 0; w < SPROM_WORDS_R1; w++) {
            sprom[w] = ap_r16(gBar0, SSB_SPROM_BASE1 + (UInt32)(w * 2));
            if (sprom[w] != 0xFFFF) allFF = 0;
            if (sprom[w] != 0x0000) allZero = 0;
        }
        /* Raw dump, 8 words a line -- cheap, and the one thing we cannot re-read without another boot. */
        for (w = 0; w < SPROM_WORDS_R1; w += 8) {
            Str255 L; int k;
            L[0]=0; PCat(L,"      +"); PCatHex(L,(unsigned long)(w*2),3); PCat(L,":");
            for (k = 0; k < 8; k++) { PCat(L," "); PCatHex(L, sprom[w+k], 4); }
            Out(L);
        }
        if (allFF) {
            Say("    !! SPROM reads as ALL 0xFFFF -- unprogrammed, absent, or not reachable at this");
            Say("       address. Oracle B cannot pass. Do not conclude the chip is bad from this.");
        } else if (allZero) {
            Say("    !! SPROM reads as ALL ZERO -- that is a dead bus, not an SPROM.");
        } else {
            UInt16 revword = sprom[SPROM_REVISION / 2];
            SayH("    revision word (+0x7E) = ", revword, 4);
            Say1("      SPROM revision = ", revword & 0xFFUL);
            SayH("      stored CRC8    = ", (revword >> 8) & 0xFFUL, 2);
            Say("      (CRC is recorded, not verified -- an unverified CRC implementation could fail");
            Say("       for reasons unrelated to the read, which is exactly the oracle trap this");
            Say("       project has already paid for once. The MAC below is the real discriminator.)");
            SayH("    board flags (+0x72)   = ", sprom[SPROM_BFLLO / 2], 4);
            SayH("    antenna gain (+0x74)  = ", sprom[SPROM_AGAIN / 2], 4);
            Say("      ⚠ Apple programs antenna gain in a way that fails BSD's sanity check (an iBook");
            Say("        G4 dmesg shows 'invalid antenna gain in sprom'). A garbage value here is");
            Say("        EXPECTED and must be clamped, not treated as a failed read.");

            /* THE DISCRIMINATOR. ssb's sprom_get_mac: each 16-bit word yields two bytes, high first. */
            { int k; for (k = 0; k < 3; k++) {
                UInt16 v = sprom[(SPROM_IL0MAC / 2) + k];
                mac[k*2]   = (UInt8)(v >> 8);
                mac[k*2+1] = (UInt8)(v & 0xFF); } }
            { Str255 L; int k;
              L[0]=0; PCat(L,"    >>> SPROM MAC (+0x48) = ");
              for (k = 0; k < 6; k++) { if (k) PCat(L,":"); PCatHex(L, mac[k], 2); } Out(L); }
            for (i = 0; i < 6; i++) if (mac[i] != 0xFF) macAllFF = 0;
            macIsDummy = (mac[0]==0xF1 && mac[1]==0xF2 && mac[2]==0xF3 &&
                          mac[3]==0xF4 && mac[4]==0xF5 && mac[5]==0xF6);
            macMulticast = (mac[0] & 0x01) ? 1 : 0;
            if (macAllFF) {
                Say("      !! all-FF. Unprogrammed or unread. ORACLE B FAILS.");
            } else if (macIsDummy) {
                Say("      !! THIS IS THE F1:F2:F3:F4:F5:F6 PLACEHOLDER Open Firmware invented.");
                Say("         Reading it back out of the SPROM means we are NOT reading the SPROM --");
                Say("         we are reading something OF wrote. ORACLE B FAILS, and this is exactly");
                Say("         the distinctive wrong answer Stage 1 handed us. Suspect the offset.");
            } else if (macMulticast) {
                Say("      !! the multicast bit is set in byte 0 -- not a valid station address.");
                Say("         Suspect the 16-bit byte order (lhbrx) or the word offset.");
            } else {
                oracleB = 1;
                Say("      ==> ORACLE B PASSED: a sane unicast address, not all-FF, not the OF dummy.");
                if (mac[0]==0x00 && (mac[1]==0x0A || mac[1]==0x03 || mac[1]==0x30 ||
                                     mac[1]==0x0D || mac[1]==0x05 || mac[1]==0x50 || mac[1]==0x16))
                    Say("      ==> and the OUI looks like an Apple range, which we did not write there.");
                Say("      ★ CHECK THIS AGAINST TIGER. Boot OS X and compare the AirPort card's MAC.");
                Say("        That is a third, fully independent path to the same six bytes.");
            }
        }
    }

verdict:
    Say("");
    Say1("    ChipCommon core present = ", (unsigned long)haveChipCommon);
    Say1("    802.11 core present     = ", (unsigned long)have80211);
    if (!haveChipCommon)
        Say("    ⚠ no ChipCommon core. On a real BCM4306 there IS one, and its absence means the");
    if (!haveChipCommon)
        Say("      core-select window is not landing where we think -- not that the chip lacks it.");
    Say1("    core-select retries = ", gSelectRetries);
    Say1("    core-select failures = ", gSelectFailures);
    if (gSelectRetries) Say("    (retries are normal -- the window write is not immediate. Failures are not.)");
    Say("");
    Say("=== VERDICT ===");
    if (oracleA && oracleB) {
        Say("  ✓✓✓ BOTH ORACLES PASSED. The MMIO path, the little-endian accessors, the");
        Say("      SiliconBackplane core-select window and the SPROM addressing are ALL PROVEN on");
        Say("      real hardware. STAGE 2 PASSES.");
        Say("      Stage 3 can proceed: core reset, the SHM pattern self-test, then firmware upload.");
    } else if (oracleA) {
        Say("  ~ ORACLE A PASSED, B FAILED. The chip is talking and the backplane window works, so");
        Say("    MMIO is sound -- the problem is confined to the SPROM read: the +0x1000 base, the");
        Say("    +0x48 offset, or the 16-bit byte order. A much smaller search than it looks.");
    } else if (oracleB) {
        Say("  ~ ORACLE B PASSED, A FAILED. The SPROM read works, which means BAR0 and the endian");
        Say("    handling are right. So the fault is in the CORE-SELECT window specifically, since");
        Say("    the SPROM shadow does not depend on it. Check the config 0x80 write.");
    } else {
        Say("  ✗ BOTH ORACLES FAILED. Before blaming the card, check in this order: did memory space");
        Say("    actually stick (section 1)? did the first MMIO read return 0xFFFFFFFF (bus float)?");
        Say("    is BAR0's logical address reachable from an app at all (section 2's warning)?");
        Say("    Stage 1 proved the card is present and assigned -- so this is a reachability or an");
        Say("    addressing fault, not an absent device.");
    }
    Say("");
    Say("  ⚠ Memory space was left ENABLED. That is harmless and Open Firmware re-disables it at the");
    Say("    next boot, but note it if anything else touches this card before then.");
    Say("");
    Say("=== done. Log: 'AirPort Stage2 Log' in the System Folder. ===");

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
