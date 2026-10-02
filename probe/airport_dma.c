/* airport_dma.c -- STAGE 5-1 (k1): the addressing probe.
 *
 * ★★★★★ WHAT THIS ANSWERS, AND WHY IT IS THE FIRST STAGE 5 INCREMENT.
 * Stage 4 ended with a fully initialised G-PHY and a MAC that transmits. Stage 5 is a different
 * kind of work: not register translation, but OS 9 integration -- DMA rings, physical addresses,
 * cache coherency, interrupts. Those are four independent failure classes, and running them
 * together would make any failure ambiguous. This increment isolates ONE of them: addressing.
 *
 * It allocates no ring, programs no descriptor, enables no engine and installs no interrupt
 * handler. It reads registers, performs b43_engine_type's single probing write, asks OS 9 for
 * four blocks of DMA-able memory, and reports. Everything stays polled, so the flushed-per-line
 * log that has carried all of Stage 4 keeps working for one more run.
 *
 * ★ THE QUESTION THE ROADMAP ASKED WAS "32-bit or 30-bit?". READING THE SOURCE CHANGED IT.
 * b43_engine_type (b43_dma.c:808) decides between B43_DMA_32BIT and B43_DMA_30BIT by writing
 * TXADDREXT_MASK to the DMA32 controller's TXCTL and seeing whether the bits stick. That was
 * expected to be the whole of this increment. But b43_dma_address (b43_dma.c:37) is what actually
 * composes the address that goes into a descriptor, and for THIS card it does:
 *
 *     addr  = lower_32_bits(dmaaddr);
 *     addr &= ~SSB_DMA_TRANSLATION_MASK;     /_ 0xC0000000 -- clears bits 30 and 31 _/
 *     addr |= dma->translation;              /_ SSB_PCI_DMA = 0x40000000 _/
 *
 * That masking is gated on `dma->translation_in_low`, and b43_dma_translation_in_low_word
 * (b43_dma.c:1033) returns true immediately for any type that is not B43_DMA_64BIT. So it applies
 * to the 32-bit ring EXACTLY as it applies to the 30-bit one.
 *
 * ⇒ A HOST PHYSICAL ADDRESS AT OR ABOVE 0x40000000 IS SILENTLY TRUNCATED, whichever answer the
 *   width probe gives. The width probe is therefore the SMALLER half of this increment; the half
 *   that can actually stop the project is whether OS 9 hands us memory below 1 GB. Oracle D is
 *   the one that matters, and it is measured, not assumed.
 *
 * ★ THE TRANSLATION VALUE IS DERIVED FROM SOURCE, NOT RECALLED. ssb_dma_translation
 * (ssb/main.c) returns SSB_PCIE_DMA_H32 only when the host bridge is PCIe, or when
 * ssb_dma_translation_special_bit() matches -- and that function tests chip_id against exactly
 * 0x4322, 43221, 43231 and 43222. This is a PCI (not PCIe) card and chip 0x4306, so neither arm
 * is taken and the translation is SSB_PCI_DMA = 0x40000000. The probe re-checks the chip id
 * against that list at runtime rather than trusting the comment.
 *
 * ★ NO NEW LIBRARY. GetPhysical and LockMemory are declared in MacMemory.h and exported by
 * InterfaceLib (7.1 and later) -- verified with nm against libInterfaceLib.a, not assumed from
 * the header. DriverServicesLib and DriverLoaderLib stay ABSENT, so the structural guarantee the
 * CMakeLists has carried since Stage 1 -- this binary cannot install anything -- survives Stage 5
 * intact. The allocation idiom itself is lifted from the user's own working ehci_os.c
 * (ehci_dma_pool_init, usb2-ehci/src/ehci_os.c:455): NewPtrSysClear(size + 0x1000), align up,
 * LockMemory, GetPhysical.
 *
 * ⚠ ONE DELIBERATE DEVIATION FROM b43, recorded so it is not mistaken for drift. b43_engine_type
 * leaves TXADDREXT_MASK sitting in TXCTL, because b43_dma_init immediately follows it and resets
 * every controller. This probe stops at the measurement, so it restores TXCTL to the value it
 * read before the probe. Leaving a stray non-zero value in a control register of an engine nobody
 * is about to reset is not faithfulness, it is litter.
 *
 * ⚠ NO FRESH BOOT REQUIRED -- ApBringUp()'s SsbCoreEnable() does a real teardown first.
 */

#define AP_LOG_NAME "\pAirPort DMA Log"
#define AP_DMA_VER  "k1"

#include <Gestalt.h>
#include <MacMemory.h>
/* ★ 8-2d-0: ap_log.h supplies Say/Out/PCat* and the Toolbox surface they need. It must come
 * BEFORE ap_bringup.h, which no longer includes them -- see the note at that file's old
 * logging block for why the dependency deliberately runs upward. */
#include "ap_log.h"
#include "ap_bringup.h"

/* ---- b43_dma.h, 32-bit engine. Fetched from mainline; see b43_dma_h_extract.txt. ---- */
#define B43_MMIO_DMA32_BASE0        0x200UL
#define B43_MMIO_DMA32_BASE1        0x220UL
#define B43_MMIO_DMA32_BASE2        0x240UL
#define B43_MMIO_DMA32_BASE3        0x260UL
#define B43_MMIO_DMA32_BASE4        0x280UL
#define B43_MMIO_DMA32_BASE5        0x2A0UL

#define B43_DMA32_TXCTL             0x00UL
#define   B43_DMA32_TXENABLE        0x00000001UL
#define   B43_DMA32_TXSUSPEND       0x00000002UL
#define   B43_DMA32_TXLOOPBACK      0x00000004UL
#define   B43_DMA32_TXFLUSH         0x00000010UL
#define   B43_DMA32_TXADDREXT_MASK  0x00030000UL
#define B43_DMA32_TXRING            0x04UL
#define B43_DMA32_TXINDEX           0x08UL
#define B43_DMA32_TXSTATUS          0x0CUL
#define   B43_DMA32_TXSTATE         0x0000F000UL
#define     B43_DMA32_TXSTAT_DISABLED 0x00000000UL
#define     B43_DMA32_TXSTAT_ACTIVE   0x00001000UL
#define     B43_DMA32_TXSTAT_IDLEWAIT 0x00002000UL
#define     B43_DMA32_TXSTAT_STOPPED  0x00003000UL
#define     B43_DMA32_TXSTAT_SUSP     0x00004000UL
#define   B43_DMA32_TXERROR         0x000F0000UL
#define B43_DMA32_RXCTL             0x10UL
#define   B43_DMA32_RXENABLE        0x00000001UL
#define   B43_DMA32_RXFROFF_MASK    0x000000FEUL
#define   B43_DMA32_RXFROFF_SHIFT   1
#define   B43_DMA32_RXADDREXT_MASK  0x00030000UL
#define B43_DMA32_RXRING            0x14UL
#define B43_DMA32_RXINDEX           0x18UL
#define B43_DMA32_RXSTATUS          0x1CUL
#define   B43_DMA32_RXSTATE         0x0000F000UL

#define B43_DMA32_RINGMEMSIZE       4096UL
#define B43_TXRING_SLOTS            256
#define B43_RXRING_SLOTS            256

/* ---- ssb.h / ssb_regs.h ---- */
#define SSB_DMA_TRANSLATION_MASK    0xC0000000UL
#define SSB_DMA_TRANSLATION_SHIFT   30
#define SSB_PCI_DMA                 0x40000000UL   /* Client Mode sb2pcitranslation2, 1 GB */
#define SSB_PCI_DMA_SZ              0x40000000UL
#define SSB_PCIE_DMA_H32            0x80000000UL

/* struct b43_dmadesc32 { __le32 control; __le32 address; } -- 8 bytes. */
#define B43_DMADESC32_BYTES         8UL

static void SayOk(const char*s,int ok){Str255 L;L[0]=0;PCat(L,ok?"  [ok] ":"  [!!] ");PCat(L,s);Out(L);}

/* Say1 takes unsigned long, so an OSErr of -108 would print as 4294967188. Mac error codes are
 * read as small negative decimals, so they get their own printer. */
static void SayS(const char*s,long v){Str255 L;L[0]=0;PCat(L,s);
  if(v<0){PCat(L,"-");v=-v;} PCatDec(L,(unsigned long)v);Out(L);}

/* ============================================================================
 * b43_dma_address, ported. This is the function every descriptor write will go
 * through, so it is written ONCE here and unit-tested by Oracle E rather than
 * open-coded at each call site later.
 *
 * Only the B43_DMA_ADDR_LOW arm exists. ADDR_HIGH and ADDR_EXT are reachable
 * only from the 64-bit descriptor path, and Oracle C establishes that this core
 * does not support 64-bit DMA -- so porting them would be porting branches the
 * hardware cannot enter, which is the habit that cost Stage 4 five runs.
 * ==========================================================================*/
static UInt32 gTranslation = SSB_PCI_DMA;

static UInt32 B43DmaAddressLow(UInt32 dmaaddr)
{
    UInt32 addr = dmaaddr;
    addr &= ~SSB_DMA_TRANSLATION_MASK;
    addr |= gTranslation;
    return addr;
}

/* ============================================================================
 * The OS 9 side: one wired, page-aligned, physically-resolved block.
 * Lifted from ehci_dma_pool_init (usb2-ehci/src/ehci_os.c:455), with one change
 * that is the whole point of the probe: ehci_os.c passes physicalEntryCount = 1,
 * meaning "I have room for one entry". That is correct for a driver that has
 * already decided a page is contiguous. A PROBE must not assume it, so this
 * passes 8 -- the full size of LogicalToPhysicalTable.physical[] -- and reports
 * how many entries actually came back. entries == 1 is then evidence rather
 * than a restatement of the input.
 * ==========================================================================*/
typedef struct {
    Ptr     raw;          /* NewPtrSysClear's return, kept for DisposePtr */
    UInt8  *base;         /* page-aligned logical address */
    UInt32  phys;         /* physical address of base */
    UInt32  physRun;      /* bytes contiguous at phys, from physical[0].count */
    UInt32  size;
    UInt32  entries;      /* physical entry count GetPhysical reported */
    OSErr   lockErr;
    OSErr   physErr;
    int     ok;
} DmaBlock;

static int DmaBlockAlloc(DmaBlock *b, UInt32 size)
{
    LogicalToPhysicalTable tbl;
    unsigned long count = 8;                 /* capacity of tbl.physical[], not an assertion */
    int i;

    b->raw=0; b->base=0; b->phys=0; b->physRun=0; b->size=size;
    b->entries=0; b->lockErr=noErr; b->physErr=noErr; b->ok=0;

    b->raw = NewPtrSysClear((Size)(size + 0x1000UL));
    if(b->raw==0) return 0;
    b->base = (UInt8*)((((UInt32)b->raw) + 0xFFFUL) & ~0xFFFUL);

    b->lockErr = LockMemory((void*)b->base, (unsigned long)size);
    if(b->lockErr!=noErr) return 0;

    for(i=0;i<8;i++){ tbl.physical[i].address=0; tbl.physical[i].count=0; }
    tbl.logical.address = (void*)b->base;
    tbl.logical.count   = (unsigned long)size;
    b->physErr = GetPhysical(&tbl,&count);
    if(b->physErr!=noErr) return 0;

    b->entries = (UInt32)count;
    b->phys    = (UInt32)tbl.physical[0].address;
    b->physRun = (UInt32)tbl.physical[0].count;
    b->ok      = 1;
    return 1;
}

static void DmaBlockFree(DmaBlock *b)
{
    if(b->lockErr==noErr && b->base) UnlockMemory((void*)b->base,(unsigned long)b->size);
    if(b->raw) DisposePtr(b->raw);
    b->raw=0; b->base=0;
}

/* Report one block and score it. Returns 1 if it satisfies every requirement a
 * descriptor ring or a DMA buffer has on this card. */
static int DmaBlockReport(const char *what, DmaBlock *b)
{
    int contig, aligned, inWindow, good;
    Str255 L;

    L[0]=0; PCat(L,"  "); PCat(L,what); Out(L);
    if(!b->ok){
      if(b->raw==0)              Say("     !! NewPtrSysClear FAILED -- system heap exhausted");
      else if(b->lockErr!=noErr) SayS("     !! LockMemory failed, err = ",(long)b->lockErr);
      else                       SayS("     !! GetPhysical failed, err = ",(long)b->physErr);
      return 0; }

    SayH ("     logical   = ",(unsigned long)b->base,8);
    SayH ("     physical  = ",(unsigned long)b->phys,8);
    Say1 ("     size      = ",(unsigned long)b->size);
    Say1 ("     entries   = ",(unsigned long)b->entries);
    Say1 ("     run bytes = ",(unsigned long)b->physRun);

    contig   = (b->entries==1) && (b->physRun>=b->size);
    aligned  = ((b->phys & 0xFFFUL)==0);
    /* b43_dma_mapping_error uses `addr + buffersize > (1ULL << 30)` -> error, so a block
     * ENDING exactly at 0x40000000 is legal. Matched exactly.
     * ⚠ The `phys < SZ` term is not redundant. b43 does this arithmetic in u64; here both
     * operands are UInt32, so a physical address near 0xFFFFF000 plus 4096 WRAPS TO ZERO and
     * the sum test alone would report the worst possible address as in-window. */
    inWindow = (b->phys < SSB_PCI_DMA_SZ) && ((b->phys + b->size) <= SSB_PCI_DMA_SZ);

    SayOk("physically contiguous (entries == 1, run covers size)",contig);
    SayOk("page aligned",aligned);
    SayOk("fits under 0x40000000 -- survives the translation mask",inWindow);
    if(!inWindow){
      SayH("     ⚠ this block would be TRUNCATED to ",
           (unsigned long)B43DmaAddressLow(b->phys),8);
      Say ("       -- the card would DMA to the wrong page. This is the failure that");
      Say ("          stops Stage 5 and needs a bounce-buffer or a low-memory allocator."); }

    good = contig && aligned && inWindow;
    return good;
}

int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    DmaBlock ringA,ringB,rxbuf,big;
    UInt32 tmshigh,txctlPost,txctlSaved;
    UInt32 base,pre[8],chip;
    UInt32 physRam=0,logRam=0,vmAttr=0; long g=0;
    int is32bit=0,special=0,vmOn=0;
    int oracleA=0,oracleB=0,oracleC=0,oracleD=0,oracleE=0;
    int dA,dB,dC,dD;
    int eOk=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+660;
    win=NewWindow(NULL,&bounds,"\pAirPort DMA " AP_DMA_VER " - Stage 5-1 addressing probe",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT STAGE 5-1 " AP_DMA_VER " -- THE ADDRESSING PROBE ===");
    Say("  Stage 4 is complete: the G-PHY is initialised and the MAC transmits. Stage 5 is");
    Say("  OS 9 integration, which has four independent failure classes -- addressing, cache");
    Say("  coherency, ring programming and interrupts. This probe isolates the FIRST.");
    Say("");
    Say("  It allocates no ring, programs no descriptor, enables no engine and installs no");
    Say("  interrupt handler. Polled throughout, so the flushed log still works.");
    Say("");
    Say("  ⚠ READING b43_dma.c CHANGED THIS INCREMENT'S QUESTION. b43_dma_address masks every");
    Say("    descriptor address with ~0xC0000000 and ORs in a 0x40000000 translation, and that");
    Say("    applies to the 32-bit ring exactly as to the 30-bit one. So the width probe is the");
    Say("    SMALL half. The half that can stop the project is whether OS 9 gives us memory");
    Say("    below 1 GB -- Oracle D.");
    Say("");
    Say("  ORACLE A: the DMA32 register file at 0x200 is decoded and both engines are idle.");
    Say("  ORACLE B: b43_engine_type's probing write returns a definite width.");
    Say("  ORACLE C: TMSHIGH says no 64-bit DMA, and the translation is 0x40000000.");
    Say("  ORACLE D: every block OS 9 gives us is contiguous, aligned, and under 0x40000000.");
    Say("  ORACLE E: the ported b43_dma_address is correct -- INCLUDING on an address that");
    Say("            must fail, so the check is shown to have teeth.");
    Say("");

    if(!ApBringUp()){Say("!! bring-up failed -- nothing below would mean anything");goto verdict;}

    chip = gBus.chipId;
    base = B43_MMIO_DMA32_BASE0;

    /* ================= ORACLE A: is 0x200 even decoded? ================= */
    Say("");
    Say("=== [5-1a] THE DMA32 REGISTER FILE AT 0x200 ===");
    Say("  Nothing in Stages 1-4 has ever touched the 0x2xx window. These are reads only,");
    Say("  taken BEFORE any write, so they describe the state ApBringUp leaves behind.");

    if(base + 0x20UL > gBus.bar0Size){
      Say1("  !! BAR0 is only this many bytes: ",(unsigned long)gBus.bar0Size);
      Say ("     0x200..0x21F is outside it. Refusing to read -- that would master-abort.");
      goto verdict; }

    pre[0]=ssb_r32(gBus.bar0,base+B43_DMA32_TXCTL);
    pre[1]=ssb_r32(gBus.bar0,base+B43_DMA32_TXRING);
    pre[2]=ssb_r32(gBus.bar0,base+B43_DMA32_TXINDEX);
    pre[3]=ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS);
    pre[4]=ssb_r32(gBus.bar0,base+B43_DMA32_RXCTL);
    pre[5]=ssb_r32(gBus.bar0,base+B43_DMA32_RXRING);
    pre[6]=ssb_r32(gBus.bar0,base+B43_DMA32_RXINDEX);
    pre[7]=ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);

    SayH("  TXCTL    (0x200) = ",pre[0],8);
    SayH("  TXRING   (0x204) = ",pre[1],8);
    SayH("  TXINDEX  (0x208) = ",pre[2],8);
    SayH("  TXSTATUS (0x20C) = ",pre[3],8);
    SayH("  RXCTL    (0x210) = ",pre[4],8);
    SayH("  RXRING   (0x214) = ",pre[5],8);
    SayH("  RXINDEX  (0x218) = ",pre[6],8);
    SayH("  RXSTATUS (0x21C) = ",pre[7],8);

    { int allFF=1,legalTx,txIdle,rxIdle; UInt32 ts,rs;
      for(i=0;i<8;i++) if(pre[i]!=0xFFFFFFFFUL) allFF=0;
      ts=pre[3]&B43_DMA32_TXSTATE; rs=pre[7]&B43_DMA32_RXSTATE;
      legalTx = (ts==B43_DMA32_TXSTAT_DISABLED)||(ts==B43_DMA32_TXSTAT_ACTIVE)||
                (ts==B43_DMA32_TXSTAT_IDLEWAIT)||(ts==B43_DMA32_TXSTAT_STOPPED)||
                (ts==B43_DMA32_TXSTAT_SUSP);
      txIdle = ((pre[0]&B43_DMA32_TXENABLE)==0);
      rxIdle = ((pre[4]&B43_DMA32_RXENABLE)==0);
      Say("");
      SayOk("not all-ones -- the window is decoded, not floating",!allFF);
      SayOk("TXSTATE is one of DISABLED/ACTIVE/IDLEWAIT/STOPPED/SUSP",legalTx);
      SayOk("TX engine not enabled",txIdle);
      SayOk("RX engine not enabled",rxIdle);
      SayH ("     TXSTATE field = ",ts,4);
      SayH ("     RXSTATE field = ",rs,4);
      /* ⚠ RXSTATE IS REPORTED BUT NOT ASSERTED ON, DELIBERATELY. b43_dma.h names the five TX
       * state values but gives the RX field a mask and NO enumeration. Scoring RXSTATE against
       * the TX names would be asserting a constraint the reference does not state -- the same
       * invention that master-aborted h1 and h2, just in a check instead of an access. The
       * value is printed so 5-2 can compare it against a measured baseline. */
      Say ("     (RXSTATE is printed, not scored: b43_dma.h enumerates no RX state values.)");
      oracleA = (!allFF)&&legalTx&&txIdle&&rxIdle; }

    /* The other five controllers, read-only. b43 uses 0-3 for TX (BK/BE/VI/VO) and a
     * fifth for multicast; controller 0's RX half is the receive ring. Free evidence
     * about how many engines this core actually implements. */
    Say("");
    Say("  The other five DMA32 controllers (read-only, for the ring layout later):");
    { UInt32 b2; const char *nm[6];
      nm[0]="0x200 ";nm[1]="0x220 ";nm[2]="0x240 ";nm[3]="0x260 ";nm[4]="0x280 ";nm[5]="0x2A0 ";
      for(i=1;i<6;i++){
        Str255 L2;
        b2 = B43_MMIO_DMA32_BASE0 + (UInt32)i*0x20UL;
        if(b2+0x20UL > gBus.bar0Size){ Say("     (past the end of BAR0)"); break; }
        L2[0]=0; PCat(L2,"     ctrl "); PCat(L2,nm[i]);
        PCat(L2,"TXCTL=0x"); PCatHex(L2,ssb_r32(gBus.bar0,b2+B43_DMA32_TXCTL),8);
        PCat(L2," TXSTATUS=0x"); PCatHex(L2,ssb_r32(gBus.bar0,b2+B43_DMA32_TXSTATUS),8);
        Out(L2); } }

    /* ================= ORACLE C: 64-bit? and the translation ================= */
    Say("");
    Say("=== [5-1b] b43_engine_type, first half: TMSHIGH ===");
    tmshigh = ssb_r32(gBus.bar0,SSB_TMSHIGH);
    SayH("  TMSHIGH = ",tmshigh,8);
    SayH("  DMA64 bit (0x10000000) = ",(unsigned long)(tmshigh&SSB_TMSHIGH_DMA64),8);
    Say((tmshigh&SSB_TMSHIGH_DMA64)
        ? "  ⇒ 64-BIT DMA SUPPORTED. Everything below assumes it is not -- STOP and re-plan."
        : "  ⇒ no 64-bit DMA, so b43_engine_type falls through to the 32-bit probe.");

    /* ssb_dma_translation: PCI (not PCIe) and not one of the four special chips. */
    special = (chip==0x4322UL)||(chip==43221UL)||(chip==43231UL)||(chip==43222UL);
    SayH("  chip id = ",chip,4);
    Say (special
         ? "  ⚠ chip IS in ssb_dma_translation_special_bit's list -> translation 0x80000000"
         : "  chip is NOT in ssb_dma_translation_special_bit's list {0x4322,43221,43231,43222}");
    Say ("  host bridge is PCI, not PCIe -- the pci_is_pcie arm is unreachable on an MDD.");
    gTranslation = special ? SSB_PCIE_DMA_H32 : SSB_PCI_DMA;
    SayH("  ⇒ translation = ",gTranslation,8);
    oracleC = ((tmshigh&SSB_TMSHIGH_DMA64)==0) && !special && (gTranslation==SSB_PCI_DMA);
    Say("");
    SayOk("no 64-bit DMA, and the translation resolves to SSB_PCI_DMA",oracleC);

    /* ================= ORACLE B: the width probe ================= */
    Say("");
    Say("=== [5-1c] b43_engine_type, second half: the TXADDREXT probe ===");
    Say("  b43_dma.c:822-828, verbatim: write TXADDREXT_MASK to TXCTL, read it back, and if");
    Say("  the bits stick the engine is 32-bit; otherwise it is 30-bit.");
    txctlSaved = pre[0];
    ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL,B43_DMA32_TXADDREXT_MASK);
    txctlPost  = ssb_r32(gBus.bar0,base+B43_DMA32_TXCTL);
    SayH("  wrote     0x",B43_DMA32_TXADDREXT_MASK,8);
    SayH("  read back ",txctlPost,8);
    is32bit = ((txctlPost & B43_DMA32_TXADDREXT_MASK)!=0);
    Say(is32bit ? "  ⇒ B43_DMA_32BIT -- the address-extension bits are implemented."
                : "  ⇒ B43_DMA_30BIT -- the address-extension bits read back as zero.");

    /* ⚠ DELIBERATE DEVIATION: b43 leaves the mask in TXCTL because b43_dma_init resets the
     * controller immediately afterwards. This probe stops here, so it puts the register back. */
    ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL,txctlSaved);
    SayH("  TXCTL restored to its pre-probe value, reads back ",
         (unsigned long)ssb_r32(gBus.bar0,base+B43_DMA32_TXCTL),8);

    Say("");
    Say("  ⚠ HONEST SCORING. `field is all-set or all-clear` CANNOT FAIL -- the mask has two");
    Say("    bits and both branches are accepted, so that half of the oracle carries no");
    Say("    information. What is actually checked is that no bit OUTSIDE the field came back");
    Say("    set, since the whole register was overwritten with a value that has none.");
    { int noStray = ((txctlPost & ~B43_DMA32_TXADDREXT_MASK)==0);
      int restored = (ssb_r32(gBus.bar0,base+B43_DMA32_TXCTL)==txctlSaved);
      SayOk("no bits outside TXADDREXT read back set",noStray);
      SayOk("TXCTL restored",restored);
      if(!noStray) SayH("     stray bits = ",(unsigned long)(txctlPost&~B43_DMA32_TXADDREXT_MASK),8);
      oracleB = noStray && restored; }

    Say("");
    Say("  ⚠ AND THE WIDTH DOES NOT RELAX THE BOUND. b43_dma_translation_in_low_word returns");
    Say("    true for every type except B43_DMA_64BIT, so the ~0xC0000000 mask is applied to a");
    Say("    32-bit ring too. Either answer above leaves the usable range at 0..0x3FFFFFFF.");

    /* ================= ORACLE D: what does OS 9 actually give us? ================= */
    Say("");
    Say("=== [5-1d] THE MEMORY OS 9 HANDS US -- the half that can stop Stage 5 ===");

    if(Gestalt(gestaltPhysicalRAMSize,&g)==noErr) physRam=(UInt32)g;
    if(Gestalt(gestaltLogicalRAMSize,&g)==noErr)  logRam =(UInt32)g;
    if(Gestalt(gestaltVMAttr,&g)==noErr){ vmAttr=(UInt32)g; vmOn=((vmAttr&(1UL<<gestaltVMPresent))!=0); }
    SayH("  physical RAM = ",physRam,8);
    Say1("               = MB ",(unsigned long)(physRam>>20));
    SayH("  logical  RAM = ",logRam,8);
    Say(vmOn ? "  Virtual Memory is ON -- LockMemory is REQUIRED and is what we use."
             : "  Virtual Memory is OFF -- LockMemory is a no-op but is issued anyway.");

    Say("");
    Say("  ⚠ HOW STRONG IS THIS EVIDENCE? If physical RAM is 1 GB or less, every physical");
    Say("    address on this machine is below 0x40000000 by construction and Oracle D CANNOT");
    Say("    FAIL. It would then confirm contiguity and alignment, which are real, but say");
    Say("    nothing about the 1 GB bound. The driver must enforce the bound in code either");
    Say("    way, because an MDD takes 2 GB.");
    Say(physRam<=SSB_PCI_DMA_SZ
        ? "    ⇒ THIS MACHINE HAS <= 1 GB. Treat the window result below as UNTESTED."
        : "    ⇒ this machine has MORE than 1 GB, so the window result is a real test.");

    Say("");
    Say("  Four allocations, via the ehci_os.c idiom: NewPtrSysClear(size+0x1000), align to a");
    Say("  page, LockMemory, GetPhysical. A descriptor ring is 256 slots x 8 bytes = 2048,");
    Say("  and b43 allocates B43_DMA32_RINGMEMSIZE = 4096 for it.");
    Say("");

    (void)DmaBlockAlloc(&ringA,B43_DMA32_RINGMEMSIZE);
    (void)DmaBlockAlloc(&ringB,B43_DMA32_RINGMEMSIZE);
    (void)DmaBlockAlloc(&rxbuf,B43_DMA32_RINGMEMSIZE);
    (void)DmaBlockAlloc(&big,  65536UL);

    dA=DmaBlockReport("[1] TX descriptor ring, 4096 bytes",&ringA);   Say("");
    dB=DmaBlockReport("[2] RX descriptor ring, 4096 bytes",&ringB);   Say("");
    dC=DmaBlockReport("[3] one RX buffer page, 4096 bytes",&rxbuf);   Say("");
    dD=DmaBlockReport("[4] a 64 KB block -- do big ones land high?",&big);

    oracleD = dA&&dB&&dC&&dD;

    /* ================= ORACLE E: the ported function, unit-tested ================= */
    Say("");
    Say("=== [5-1e] b43_dma_address, ported and unit-tested ===");
    Say("  Every descriptor this driver ever writes goes through this one function, so it is");
    Say("  written once and tested here rather than open-coded at each call site later.");
    Say("");
    { UInt32 in,out; int t1,t2,t3,t4;

      /* (1) a REAL measured address must survive the composition losslessly. */
      in  = ringA.ok ? ringA.phys : 0x00100000UL;
      out = B43DmaAddressLow(in);
      SayH("  (1) measured ring phys   = ",in,8);
      SayH("      descriptor address   = ",out,8);
      t1  = ((out & ~SSB_DMA_TRANSLATION_MASK) == in) && ((out & SSB_DMA_TRANSLATION_MASK)==gTranslation);
      SayOk("low 30 bits preserved, translation field set to 0x40000000",t1);

      /* (2) zero -- the degenerate case, must become exactly the translation. */
      out = B43DmaAddressLow(0x00000000UL);
      SayH("  (2) 0x00000000 becomes   = ",out,8);
      t2  = (out==gTranslation);
      SayOk("a zero address becomes the bare translation",t2);

      /* (3) the last address that still fits. */
      in  = 0x3FFFF000UL;
      out = B43DmaAddressLow(in);
      SayH("  (3) 0x3FFFF000 becomes   = ",out,8);
      t3  = (out==0x7FFFF000UL);
      SayOk("the top of the window maps to 0x7FFFF000",t3);

      /* (4) ⚠ THE CASE THAT MUST FAIL. Without it, (1)-(3) prove only that the function does
       *     something; they do not prove the bound has teeth.
       *
       *     ⚠⚠ AND THE OBVIOUS CHECK HERE IS WRONG. 0x50000000 is a FIXED POINT of this
       *     composition: 0x50000000 & ~0xC0000000 = 0x10000000, and 0x10000000 | 0x40000000
       *     = 0x50000000 again. So `did the value change?` reports NO CORRUPTION on an address
       *     that is thoroughly corrupted -- the card would be told 0x50000000, its
       *     sb2pcitranslation2 window maps that to host physical 0x10000000, and the DMA lands
       *     1 GB away from the buffer. The first draft of this probe used `out != in` and would
       *     have passed itself while describing the wrong memory.
       *
       *     The correct invariant is the SAME ONE test (1) uses: the low 30 bits must round
       *     trip. It holds exactly when phys < 0x40000000, so it is true for (1)-(3) and must
       *     be false here. One predicate, used in both directions. */
      in  = 0x50000000UL;
      out = B43DmaAddressLow(in);
      SayH("  (4) 0x50000000 becomes   = ",out,8);
      Say ("      -- note it is UNCHANGED: 0x50000000 is a fixed point of the composition.");
      Say ("         `did the value change?` would report success here. It is the wrong test.");
      SayH("      low 30 bits round trip to = ",(unsigned long)(out & ~SSB_DMA_TRANSLATION_MASK),8);
      t4  = ((out & ~SSB_DMA_TRANSLATION_MASK) != in);
      SayOk("an out-of-window address does NOT round trip -- the bound has teeth",t4);
      Say ("      (the card would be handed 0x50000000, translate it to host 0x10000000,");
      Say ("       and DMA a gigabyte away from the buffer.)");

      eOk = t1&&t2&&t3&&t4;
      oracleE = eOk; }

    DmaBlockFree(&ringA); DmaBlockFree(&ringB);
    DmaBlockFree(&rxbuf); DmaBlockFree(&big);
    Say("");
    Say("  all four blocks unlocked and disposed.");

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB&&oracleC&&oracleD&&oracleE){
      Say("  ★★★★★ THE ADDRESSING QUESTION IS SETTLED.");
      Say("        A  the DMA32 register file at 0x200 is decoded and both engines are idle");
      Say("        B  b43_engine_type's probing write returned a definite width, with no");
      Say("           stray bits, and TXCTL was put back");
      Say("        C  no 64-bit DMA; the translation resolves to SSB_PCI_DMA = 0x40000000");
      Say("        D  every block OS 9 gave us is contiguous, page aligned and in-window");
      Say("        E  b43_dma_address is correct, including on the address that must fail");
      Say(is32bit ? "     ⇒ engine width: 32-BIT" : "     ⇒ engine width: 30-BIT");
      Say("     ⇒ NEXT (5-2): allocate the real TX ring, program TXRING with the composed");
      Say("       address, and read TXSTATUS back. Still polled, still no interrupts.");
    } else {
      Say("  ✗ NOT VERIFIED. Do not build on this.");
      if(!oracleA) Say("    - the 0x200 register file is not decoded, or an engine is running.");
      if(!oracleB) Say("    - the width probe left stray bits, or TXCTL did not restore.");
      if(!oracleC) Say("    - 64-bit DMA is claimed, or the translation is not 0x40000000.");
      if(!oracleD) Say("    - OS 9 memory is not usable as-is. THIS IS THE ONE THAT MATTERS:");
      if(!oracleD) Say("      re-read the per-block lines above before writing any ring code.");
      if(!oracleE) Say("    - the ported b43_dma_address is wrong. Fix it before 5-2.");
    }
    Say("");
    Say("  ⚠ No ring was programmed and no engine was enabled. Re-runnable immediately.");
    Say("");
    Say("=== done. Log: 'AirPort DMA Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
