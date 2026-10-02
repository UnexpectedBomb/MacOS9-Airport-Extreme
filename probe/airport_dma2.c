/* airport_dma2.c -- STAGE 5-2 (k2): the DMA rings, programmed.
 *
 * ★★★★★ WHAT THIS DOES. k1 settled addressing: the engine is 30-bit, the translation is
 * SSB_PCI_DMA = 0x40000000, and OS 9 hands out contiguous page-aligned memory at ~32 MB on a
 * 2 GB machine. k2 uses that to do the thing addressing was for -- build both descriptor rings,
 * hand them to the hardware, and confirm the engines take them. Still polled, still no
 * interrupts, still no frame posted.
 *
 * ★ WHY TX AND RX ARE IN ONE INCREMENT. They are one function. dmacontroller_setup (b43_dma.c:672)
 * has a TX arm and an RX arm, and alloc_initial_descbuffers is called from INSIDE the RX arm.
 * Splitting them would put one function's statements in two probes, which is the drift that
 * ap_bringup.h exists to prevent. "Program RX but skip its descriptors" is not a subset of the
 * reference, it is a deviation from it.
 *
 * ⚠⚠ THE DESCRIPTORS ARE LITTLE-ENDIAN AND THIS MACHINE IS NOT. op32_fill_descriptor ends with
 *     desc->dma32.control = cpu_to_le32(ctl);
 *     desc->dma32.address = cpu_to_le32(addr);
 * The MMIO path has always handled this -- ssb_core.h's accessors are lwbrx/stwbrx. But
 * DESCRIPTORS LIVE IN ORDINARY MEMORY and a normal PowerPC store writes big-endian. Every
 * descriptor word here goes through le32st(), which is the same stwbrx instruction pointed at
 * RAM instead of at the BAR. Oracle B reads the raw bytes back and prints them individually,
 * because "it compiled" is not evidence of byte order.
 *
 * ★ nr_slots IS A PARAMETER, NOT A TRANSCRIBED CONSTANT. b43 uses B43_RXRING_SLOTS = 256, and
 * 256 RX buffers at B43_DMA0_RX_FW351_BUFSIZE (2382) is 596 KB of OS 9 SYSTEM HEAP for a probe.
 * This uses 32. That is not a liberty: the hardware wraps where DCTL_DTABLEEND says it does, and
 * b43 itself already runs a PARTIALLY USED ring -- 256 slots inside a 4096-byte (512-slot) ring,
 * leaving the tail uninitialised. Using 32 is the reference's own pattern with a smaller number,
 * and RXINDEX is written as nr_slots * 8 exactly as b43 writes it.
 *
 * ★ THE FIRMWARE IS THE OLD FORMAT, AND THAT CHANGES TWO CONSTANTS. b43_main.c:2699 sets
 * hdr_format from the ucode revision: >= 598 is FW_HDR_598, >= 410 is FW_HDR_410, else FW_HDR_351.
 * k1 measured ucode revision 0x127 = 295, so this card is FW_HDR_351 and b43 would print its
 * "you are using an old firmware image" warning. Therefore frameoffset = B43_DMA0_RX_FW351_FO
 * = 30 and rx_buffersize = 30 + IEEE80211_MAX_FRAME_LEN (2352) = 2382. NOT 38 / 2390.
 *
 * ⚠ k1 SAID "b43_dma.h ENUMERATES NO RX STATE VALUES". THAT WAS WRONG, and it is corrected here.
 * b43_dmacontroller_rx_reset uses B43_DMA32_RXSTAT_DISABLED, so they plainly exist; the earlier
 * extract had simply not captured them. Re-fetched: RXSTAT_{DISABLED,ACTIVE,IDLEWAIT,STOPPED} at
 * 0x0000/0x1000/0x2000/0x3000, plus an RXERR_* enumeration the extract had also missed. k1's
 * measured RXSTATE of 0x0000 was DISABLED and nothing was mis-reported -- but the reason given
 * for not scoring it was false, so RXSTATE and RXERROR are both scored here.
 *
 * ⚠ NO FRAME WILL BE RECEIVED, AND THAT IS EXPECTED, NOT A FAILURE. k1 measured MACCTL =
 * 0x80020504: GMODE | INFRA | IHR_ENABLED | SHM_ENABLED | PSM_JMP0. B43_MACCTL_ENABLED is bit 0
 * and it is CLEAR -- ApBringUp never sets it, and b43 sets it in b43_mac_enable, which runs after
 * dma_init. So the MAC is not listening. The rings are programmed and enabled exactly as b43
 * programs them at this point in the sequence, and RXDPTR is reported rather than asserted on.
 * Turning the MAC on is the next increment's business.
 *
 * ★ CACHE COHERENCY IS NOT TESTED HERE AND DOES NOT NEED TO BE. With the MAC off the card never
 * reads our descriptors, so nothing in this probe depends on coherency. When it does matter, the
 * prior art is our own: usb2-ehci runs EHCI at full speed over plain NewPtrSysClear memory with
 * no explicit flushing, because PowerMac PCI DMA is hardware cache-coherent by snooping.
 *
 * ⚠ NO FRESH BOOT REQUIRED -- ApBringUp()'s SsbCoreEnable() does a real teardown first.
 */

#define AP_LOG_NAME "\pAirPort DMA2 Log"
#define AP_DMA_VER  "k2"

#include <Gestalt.h>
#include <MacMemory.h>
/* ★ 8-2d-0: ap_log.h supplies Say/Out/PCat* and the Toolbox surface they need. It must come
 * BEFORE ap_bringup.h, which no longer includes them -- see the note at that file's old
 * logging block for why the dependency deliberately runs upward. */
#include "ap_log.h"
#include "ap_bringup.h"

/* ---- b43_dma.h, 32-bit engine ---- */
#define B43_MMIO_DMA32_BASE0        0x200UL

#define B43_DMA32_TXCTL             0x00UL
#define   B43_DMA32_TXENABLE        0x00000001UL
#define   B43_DMA32_TXSUSPEND       0x00000002UL
#define   B43_DMA32_TXPARITYDISABLE 0x00000800UL
#define   B43_DMA32_TXADDREXT_MASK  0x00030000UL
#define   B43_DMA32_TXADDREXT_SHIFT 16
#define B43_DMA32_TXRING            0x04UL
#define B43_DMA32_TXINDEX           0x08UL
#define B43_DMA32_TXSTATUS          0x0CUL
#define   B43_DMA32_TXDPTR          0x00000FFFUL
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
#define   B43_DMA32_RXPARITYDISABLE 0x00000800UL
#define   B43_DMA32_RXADDREXT_MASK  0x00030000UL
#define   B43_DMA32_RXADDREXT_SHIFT 16
#define B43_DMA32_RXRING            0x14UL
#define B43_DMA32_RXINDEX           0x18UL
#define B43_DMA32_RXSTATUS          0x1CUL
#define   B43_DMA32_RXDPTR          0x00000FFFUL
#define   B43_DMA32_RXSTATE         0x0000F000UL
#define     B43_DMA32_RXSTAT_DISABLED 0x00000000UL
#define     B43_DMA32_RXSTAT_ACTIVE   0x00001000UL
#define     B43_DMA32_RXSTAT_IDLEWAIT 0x00002000UL
#define     B43_DMA32_RXSTAT_STOPPED  0x00003000UL
#define   B43_DMA32_RXERROR         0x000F0000UL
#define     B43_DMA32_RXERR_NOERR     0x00000000UL
#define     B43_DMA32_RXERR_PROT      0x00010000UL
#define     B43_DMA32_RXERR_OVERFLOW  0x00020000UL
#define     B43_DMA32_RXERR_BUFWRITE  0x00030000UL
#define     B43_DMA32_RXERR_DESCREAD  0x00040000UL

/* descriptor control word */
#define B43_DMA32_DCTL_BYTECNT      0x00001FFFUL
#define B43_DMA32_DCTL_ADDREXT_MASK 0x00030000UL
#define B43_DMA32_DCTL_ADDREXT_SHIFT 16
#define B43_DMA32_DCTL_DTABLEEND    0x10000000UL
#define B43_DMA32_DCTL_IRQ          0x20000000UL
#define B43_DMA32_DCTL_FRAMEEND     0x40000000UL
#define B43_DMA32_DCTL_FRAMESTART   0x80000000UL

#define B43_DMA32_RINGMEMSIZE       4096UL
#define B43_DMADESC32_BYTES         8UL

/* FW_HDR_351, because the measured ucode revision is 295. */
#define IEEE80211_MAX_FRAME_LEN     2352UL
#define B43_DMA0_RX_FW351_FO        30UL
#define B43_DMA0_RX_FW351_BUFSIZE   (B43_DMA0_RX_FW351_FO + IEEE80211_MAX_FRAME_LEN)  /* 2382 */
#define B43_PLCP_HDR6_BYTES         6UL

/* ---- ssb.h / ssb_regs.h ---- */
#define SSB_DMA_TRANSLATION_MASK    0xC0000000UL
#define SSB_DMA_TRANSLATION_SHIFT   30
#define SSB_PCI_DMA                 0x40000000UL
#define SSB_PCI_DMA_SZ              0x40000000UL

/* ---- this probe's ring parameters ---- */
#define K2_RX_SLOTS                 32
#define K2_PAGE                     4096UL

static void SayOk(const char*s,int ok){Str255 L;L[0]=0;PCat(L,ok?"  [ok] ":"  [!!] ");PCat(L,s);Out(L);}
static void SayS(const char*s,long v){Str255 L;L[0]=0;PCat(L,s);
  if(v<0){PCat(L,"-");v=-v;} PCatDec(L,(unsigned long)v);Out(L);}

/* ============================================================================
 * LITTLE-ENDIAN STORES INTO ORDINARY MEMORY.
 * Same instruction ssb_core.h uses for the BAR, pointed at RAM. Nothing else in
 * this file may write a descriptor word.
 * ==========================================================================*/
static inline void le32st(void *p,UInt32 v){
  __asm__ __volatile__("stwbrx %0,0,%1"::"r"(v),"r"(p):"memory"); }
static inline UInt32 le32ld(const void *p){ UInt32 v;
  __asm__ __volatile__("lwbrx %0,0,%1":"=r"(v):"r"(p):"memory"); return v; }

/* ---- b43_dma_address, both arms we can reach ---- */
static UInt32 gTranslation = SSB_PCI_DMA;

static UInt32 B43DmaAddressLow(UInt32 dmaaddr)
{ UInt32 a=dmaaddr; a&=~SSB_DMA_TRANSLATION_MASK; a|=gTranslation; return a; }

/* B43_DMA_ADDR_EXT. translation_in_low is true for every non-64-bit type, so this takes the
 * LOW word -- and it takes it from the RAW physical address, before the translation is ORed in.
 * For anything inside the 1 GB window the top two bits are zero, so this returns 0. */
static UInt32 B43DmaAddressExt(UInt32 dmaaddr)
{ UInt32 a=dmaaddr; a&=SSB_DMA_TRANSLATION_MASK; a>>=SSB_DMA_TRANSLATION_SHIFT; return a; }

/* ============================================================================
 * OS 9 memory, exactly as k1 proved it.
 * ==========================================================================*/
typedef struct {
    Ptr     raw;
    UInt8  *base;
    UInt32  phys;
    UInt32  physRun;
    UInt32  size;
    UInt32  entries;
    OSErr   lockErr, physErr;
    int     ok;
} DmaBlock;

static int DmaBlockAlloc(DmaBlock *b, UInt32 size)
{
    LogicalToPhysicalTable tbl;
    unsigned long count = 8;
    int i;
    b->raw=0;b->base=0;b->phys=0;b->physRun=0;b->size=size;
    b->entries=0;b->lockErr=noErr;b->physErr=noErr;b->ok=0;
    b->raw = NewPtrSysClear((Size)(size + K2_PAGE));
    if(b->raw==0) return 0;
    b->base = (UInt8*)((((UInt32)b->raw)+(K2_PAGE-1))&~(K2_PAGE-1));
    b->lockErr = LockMemory((void*)b->base,(unsigned long)size);
    if(b->lockErr!=noErr) return 0;
    for(i=0;i<8;i++){tbl.physical[i].address=0;tbl.physical[i].count=0;}
    tbl.logical.address=(void*)b->base; tbl.logical.count=(unsigned long)size;
    b->physErr = GetPhysical(&tbl,&count);
    if(b->physErr!=noErr) return 0;
    b->entries=(UInt32)count; b->phys=(UInt32)tbl.physical[0].address;
    b->physRun=(UInt32)tbl.physical[0].count; b->ok=1;
    return 1;
}
static void DmaBlockFree(DmaBlock *b)
{
    if(b->lockErr==noErr && b->base) UnlockMemory((void*)b->base,(unsigned long)b->size);
    if(b->raw) DisposePtr(b->raw);
    /* ⚠ These two are NOT part of the if above. They were written on the same line as it and
     * -Wmisleading-indentation caught the shape, which is the whole value of that warning:
     * the behaviour was right but unreadable, and unreadable is how a real one hides. */
    b->raw  = 0;
    b->base = 0;
}

/* Allocate and say WHY if it fails. An allocation that dies silently in a probe costs a whole
 * hardware cycle to diagnose, and the three causes need different fixes. */
static int AllocOrReport(const char *what, DmaBlock *b, UInt32 size)
{
    if(DmaBlockAlloc(b,size)) return 1;
    { Str255 L;L[0]=0;PCat(L,"!! ");PCat(L,what);PCat(L," allocation FAILED");Out(L); }
    if(b->raw==0)              Say ("   NewPtrSysClear returned NULL -- the system heap is exhausted.");
    else if(b->lockErr!=noErr) SayS("   LockMemory err = ",(long)b->lockErr);
    else                       SayS("   GetPhysical err = ",(long)b->physErr);
    return 0;
}

/* One page's physical address. A 4096-byte region starting on a page boundary maps to exactly
 * one physical page, so contiguity is definitional and the entry count must come back 1. */
static int PhysOfPage(void *pageAligned, UInt32 *physOut)
{
    LogicalToPhysicalTable tbl; unsigned long count=8; int i;
    for(i=0;i<8;i++){tbl.physical[i].address=0;tbl.physical[i].count=0;}
    tbl.logical.address=pageAligned; tbl.logical.count=K2_PAGE;
    if(GetPhysical(&tbl,&count)!=noErr) return 0;
    if(count!=1) return 0;
    *physOut=(UInt32)tbl.physical[0].address;
    return 1;
}

static int InWindow(UInt32 phys,UInt32 size)
{ return (phys < SSB_PCI_DMA_SZ) && ((phys+size) <= SSB_PCI_DMA_SZ); }

/* ============================================================================
 * b43_dmacontroller_tx_reset / _rx_reset, ported.
 * msleep(1) becomes SsbSpinUs(1000). Delay() is 16.7 ms on OS 9 and must never appear here.
 * ==========================================================================*/
static int DmaControllerTxReset(UInt32 base,UInt32 *finalStatus)
{
    int i; UInt32 value;
    for(i=0;i<10;i++){
      value = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS) & B43_DMA32_TXSTATE;
      if(value==B43_DMA32_TXSTAT_DISABLED||value==B43_DMA32_TXSTAT_IDLEWAIT||
         value==B43_DMA32_TXSTAT_STOPPED) break;
      SsbSpinUs(1000); }
    ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL,0);
    for(i=0;i<10;i++){
      value = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS) & B43_DMA32_TXSTATE;
      if(value==B43_DMA32_TXSTAT_DISABLED){ i=-1; break; }
      SsbSpinUs(1000); }
    *finalStatus = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS);
    if(i!=-1) return 0;
    SsbSpinUs(1000);                       /* "ensure the reset is completed" */
    return 1;
}

static int DmaControllerRxReset(UInt32 base,UInt32 *finalStatus)
{
    int i; UInt32 value;
    ssb_w32(gBus.bar0,base+B43_DMA32_RXCTL,0);
    for(i=0;i<10;i++){
      value = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS) & B43_DMA32_RXSTATE;
      if(value==B43_DMA32_RXSTAT_DISABLED){ i=-1; break; }
      SsbSpinUs(1000); }
    *finalStatus = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
    return (i==-1);
}

/* ============================================================================
 * op32_fill_descriptor, ported. The two stores at the end are the whole point.
 * ==========================================================================*/
static void Op32FillDescriptor(UInt8 *descbase,int slot,int nrSlots,
                               UInt32 dmaaddr,UInt16 bufsize,
                               int start,int end,int irq)
{
    UInt8 *d = descbase + (UInt32)slot*B43_DMADESC32_BYTES;
    UInt32 ctl,addr,addrext;

    addr    = B43DmaAddressLow(dmaaddr);
    addrext = B43DmaAddressExt(dmaaddr);

    ctl = (UInt32)bufsize & B43_DMA32_DCTL_BYTECNT;
    if(slot == nrSlots-1) ctl |= B43_DMA32_DCTL_DTABLEEND;
    if(start)             ctl |= B43_DMA32_DCTL_FRAMESTART;
    if(end)               ctl |= B43_DMA32_DCTL_FRAMEEND;
    if(irq)               ctl |= B43_DMA32_DCTL_IRQ;
    ctl |= (addrext << B43_DMA32_DCTL_ADDREXT_SHIFT) & B43_DMA32_DCTL_ADDREXT_MASK;

    le32st(d+0,ctl);
    le32st(d+4,addr);
}

/* b43_poison_rx_buffer: frame_len = 0, then 0xFF over plcp_hdr6 + 2 at the frame offset.
 * This is how b43 tells "the card has not written here yet" from a real short frame, and it
 * becomes a live oracle the moment the MAC is enabled. */
static void PoisonRxBuffer(UInt8 *buf,UInt32 frameoffset)
{
    UInt32 i;
    buf[0]=0; buf[1]=0;                                  /* rxhdr->frame_len = 0 */
    for(i=0;i<B43_PLCP_HDR6_BYTES+2;i++) buf[frameoffset+i]=0xFF;
}

int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    DmaBlock txRing={0},rxRing={0},rxPool={0};
    UInt32 base,txStat0,rxStat0;
    UInt32 txCtlVal,txRingVal,rxCtlVal,rxRingVal,rxIndexVal;
    UInt32 txCtlRb,txRingRb,txStatRb,rxCtlRb,rxRingRb,rxIndexRb,rxStatRb;
    UInt32 rxBufPhys[K2_RX_SLOTS];
    UInt8 *rxBufLog[K2_RX_SLOTS];
    UInt32 frameoffset = B43_DMA0_RX_FW351_FO;
    UInt32 rxBufSize   = B43_DMA0_RX_FW351_BUFSIZE;
    int oracleA=0,oracleB=0,oracleC=0,oracleD=0,oracleE=0;
    int txResetOk=0,rxResetOk=0;
    int nBufOk=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+660;
    win=NewWindow(NULL,&bounds,"\pAirPort DMA2 " AP_DMA_VER " - Stage 5-2 ring programming",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT STAGE 5-2 " AP_DMA_VER " -- THE DMA RINGS, PROGRAMMED ===");
    Say("  k1 settled addressing: 30-bit engine, translation 0x40000000, OS 9 memory at ~32 MB");
    Say("  on a 2 GB machine. k2 builds both rings and hands them to the hardware.");
    Say("");
    Say("  ⚠ THE DESCRIPTORS ARE LITTLE-ENDIAN AND THIS MACHINE IS NOT. The MMIO path has always");
    Say("    byte-reversed (ssb_core.h uses lwbrx/stwbrx). Descriptors live in ORDINARY MEMORY,");
    Say("    where a normal PowerPC store writes big-endian. Every descriptor word goes through");
    Say("    le32st() -- the same stwbrx aimed at RAM. Oracle B reads the bytes back one at a");
    Say("    time, because `it compiled` is not evidence of byte order.");
    Say("");
    Say("  ⚠ NO FRAME WILL ARRIVE, AND THAT IS EXPECTED. k1 measured MACCTL = 0x80020504;");
    Say("    B43_MACCTL_ENABLED is bit 0 and it is CLEAR. b43 sets it in b43_mac_enable, which");
    Say("    runs AFTER dma_init. The rings are enabled exactly where b43 enables them. RXDPTR");
    Say("    is reported, not asserted on. Turning the MAC on is the next increment.");
    Say("");
    Say("  ⚠ k1 CLAIMED b43_dma.h ENUMERATES NO RX STATE VALUES. THAT WAS WRONG --");
    Say("    b43_dmacontroller_rx_reset uses B43_DMA32_RXSTAT_DISABLED. Re-fetched; RXSTATE and");
    Say("    RXERROR are both scored here. k1's measured 0x0000 was DISABLED, so nothing was");
    Say("    mis-reported -- but the reason given for not scoring it was false.");
    Say("");
    Say("  ORACLE A: both controller resets reach DISABLED.");
    Say("  ORACLE B: descriptor words are LITTLE-ENDIAN in memory, proven byte by byte.");
    Say("  ORACLE C: the TX ring is programmed and the engine reports no error.");
    Say("  ORACLE D: the RX ring is programmed, frame offset 30, and reports no error.");
    Say("  ORACLE E: every descriptor in the ring decodes back to its own buffer's address.");
    Say("");

    if(!ApBringUp()){Say("!! bring-up failed -- nothing below would mean anything");goto cleanup;}

    base = B43_MMIO_DMA32_BASE0;
    if(base + 0x20UL > gBus.bar0Size){Say("!! 0x200 outside BAR0");goto cleanup;}

    /* ================= allocate ================= */
    Say("");
    Say("=== [5-2a] MEMORY ===");
    Say("  Ring memory is B43_DMA32_RINGMEMSIZE = 4096 with 4 KB alignment. alloc_ringmemory's");
    Say("  comment is explicit that the ALIGNMENT is what matters, not the size.");
    Say("");
    if(!AllocOrReport("TX ring",&txRing,B43_DMA32_RINGMEMSIZE)) goto cleanup;
    if(!AllocOrReport("RX ring",&rxRing,B43_DMA32_RINGMEMSIZE)) goto cleanup;
    /* One page per RX buffer: 2382 bytes fits inside 4096, so each buffer is page-aligned and
     * wholly inside one physical page -- contiguous by definition, no matter how the pool as a
     * whole is mapped. That is why the pool is carved per page rather than trusted as one run. */
    if(!AllocOrReport("RX buffer pool",&rxPool,(UInt32)K2_RX_SLOTS*K2_PAGE)) goto cleanup;

    SayH("  TX ring  logical = ",(unsigned long)txRing.base,8);
    SayH("           physical= ",txRing.phys,8);
    SayH("  RX ring  logical = ",(unsigned long)rxRing.base,8);
    SayH("           physical= ",rxRing.phys,8);
    SayH("  RX pool  logical = ",(unsigned long)rxPool.base,8);
    Say1("           pages   = ",(unsigned long)K2_RX_SLOTS);
    Say1("  rx_buffersize    = ",(unsigned long)rxBufSize);
    Say1("  frameoffset      = ",(unsigned long)frameoffset);
    Say ("    (FW_HDR_351, because k1 measured ucode revision 295 which is below 410.)");

    SayOk("TX ring contiguous, aligned, in-window",
          txRing.ok&&txRing.entries==1&&(txRing.phys&0xFFFUL)==0&&InWindow(txRing.phys,txRing.size));
    SayOk("RX ring contiguous, aligned, in-window",
          rxRing.ok&&rxRing.entries==1&&(rxRing.phys&0xFFFUL)==0&&InWindow(rxRing.phys,rxRing.size));

    /* per-page physical addresses for the RX buffers */
    for(i=0;i<K2_RX_SLOTS;i++){
      rxBufLog[i] = rxPool.base + (UInt32)i*K2_PAGE;
      rxBufPhys[i]= 0;
      if(PhysOfPage(rxBufLog[i],&rxBufPhys[i]) &&
         (rxBufPhys[i]&0xFFFUL)==0 && InWindow(rxBufPhys[i],rxBufSize)) nBufOk++; }
    Say1("  RX buffer pages resolved, aligned and in-window: ",(unsigned long)nBufOk);
    Say1("                                          out of : ",(unsigned long)K2_RX_SLOTS);
    SayOk("every RX buffer page is usable",nBufOk==K2_RX_SLOTS);

    /* ================= ORACLE A: the resets ================= */
    Say("");
    Say("=== [5-2b] b43_dmacontroller_tx_reset / _rx_reset ===");
    Say("  Ported statement for statement; msleep(1) becomes SsbSpinUs(1000), never Delay().");
    txResetOk = DmaControllerTxReset(base,&txStat0);
    rxResetOk = DmaControllerRxReset(base,&rxStat0);
    SayH("  TXSTATUS after reset = ",txStat0,8);
    SayH("  RXSTATUS after reset = ",rxStat0,8);
    SayOk("TX reset reached DISABLED",txResetOk);
    SayOk("RX reset reached DISABLED",rxResetOk);
    Say ("  ⚠ WEAK BY CONSTRUCTION THIS RUN. k1 measured both engines already DISABLED, so both");
    Say ("    loops exit on their first read and nothing is actually torn down. Porting it now,");
    Say ("    where it is trivially satisfied, is the point -- its real test is the increment");
    Say ("    that resets a RUNNING engine. Scored so a regression there would show.");
    oracleA = txResetOk && rxResetOk;

    /* ================= ORACLE B: byte order ================= */
    Say("");
    Say("=== [5-2c] DESCRIPTOR BYTE ORDER ===");
    Say("  A known word is written through le32st and the four bytes are read back individually.");
    { UInt32 probe = 0x11223344UL; UInt8 *d = txRing.base; UInt32 back;
      int b0,b1,b2,b3,ok;
      le32st(d,probe);
      b0=d[0];b1=d[1];b2=d[2];b3=d[3];
      back = le32ld(d);
      SayH("  wrote (host value)  = ",probe,8);
      { Str255 L;L[0]=0;PCat(L,"  bytes in memory     = ");
        PCat(L,"0x");PCatHex(L,(unsigned long)b0,2);PCat(L," 0x");PCatHex(L,(unsigned long)b1,2);
        PCat(L," 0x");PCatHex(L,(unsigned long)b2,2);PCat(L," 0x");PCatHex(L,(unsigned long)b3,2);
        Out(L); }
      Say ("  little-endian means 0x44 0x33 0x22 0x11. Big-endian would be 0x11 0x22 0x33 0x44.");
      SayH("  le32ld reads back   = ",back,8);
      ok = (b0==0x44)&&(b1==0x33)&&(b2==0x22)&&(b3==0x11)&&(back==probe);
      SayOk("descriptor words are little-endian in memory, and round trip",ok);
      if(!ok) Say("     !! STOP. Every descriptor would be byte-swapped and nothing downstream works.");
      le32st(d,0);                                        /* put the slot back */
      oracleB = ok; }

    /* ================= build the RX descriptors ================= */
    Say("");
    Say("=== [5-2d] alloc_initial_descbuffers -- the RX descriptor ring ===");
    Say("  setup_rx_descbuffer per slot: poison the buffer, then fill the descriptor with");
    Say("  bufsize, DTABLEEND on the last slot, and no FRAMESTART/FRAMEEND/IRQ -- an RX");
    Say("  descriptor carries none of those.");
    for(i=0;i<K2_RX_SLOTS;i++){
      PoisonRxBuffer(rxBufLog[i],frameoffset);
      Op32FillDescriptor(rxRing.base,i,K2_RX_SLOTS,rxBufPhys[i],(UInt16)rxBufSize,0,0,0); }
    Say1("  descriptors filled: ",(unsigned long)K2_RX_SLOTS);

    /* ================= ORACLE E: read the ring back ================= */
    Say("");
    Say("  Reading the ring back and decoding it -- this tests the fill logic AND the stores.");
    { int allOk=1;
      for(i=0;i<K2_RX_SLOTS;i++){
        UInt8 *d = rxRing.base + (UInt32)i*B43_DMADESC32_BYTES;
        UInt32 ctl = le32ld(d+0), addr = le32ld(d+4);
        UInt32 wantAddr = B43DmaAddressLow(rxBufPhys[i]);
        UInt32 wantCtl  = (rxBufSize & B43_DMA32_DCTL_BYTECNT)
                        | ((i==K2_RX_SLOTS-1)?B43_DMA32_DCTL_DTABLEEND:0UL);
        int ok = (ctl==wantCtl)&&(addr==wantAddr);
        if(!ok) allOk=0;
        if(i<3 || i==K2_RX_SLOTS-1 || !ok){
          Str255 L;L[0]=0;PCat(L,"    slot ");PCatDec(L,(unsigned long)i);
          PCat(L,"  ctl=0x");PCatHex(L,ctl,8);
          PCat(L," addr=0x");PCatHex(L,addr,8);
          PCat(L,ok?"  ok":"  !! MISMATCH");Out(L);
          if(!ok){ SayH("        wanted ctl  = ",wantCtl,8);
                   SayH("        wanted addr = ",wantAddr,8); }
          } }
      Say ("    (slots 0-2 and the last are shown; any mismatch is always shown.)");
      SayH("    DTABLEEND on the last slot = ",B43_DMA32_DCTL_DTABLEEND,8);
      SayOk("every descriptor decodes back to its own buffer's composed address",allOk);
      oracleE = allOk; }

    /* ================= ORACLE C: program TX ================= */
    Say("");
    Say("=== [5-2e] dmacontroller_setup, TX arm ===");
    Say("  value  = B43_DMA32_TXENABLE;");
    Say("  value |= (addrext << TXADDREXT_SHIFT) & TXADDREXT_MASK;");
    Say("  if (!parity) value |= TXPARITYDISABLE;      <- dma->parity is TRUE for SSB");
    Say("  write TXCTL, then write TXRING = addrlo.");
    { UInt32 addrext = B43DmaAddressExt(txRing.phys);
      txRingVal = B43DmaAddressLow(txRing.phys);
      txCtlVal  = B43_DMA32_TXENABLE;
      txCtlVal |= (addrext << B43_DMA32_TXADDREXT_SHIFT) & B43_DMA32_TXADDREXT_MASK;
      /* parity is true (b43_dma.c:1076, set false only for BCMA), so TXPARITYDISABLE is NOT set */
      SayH("  ring physical      = ",txRing.phys,8);
      SayH("  addrext            = ",addrext,8);
      SayH("  TXRING (composed)  = ",txRingVal,8);
      SayH("  TXCTL              = ",txCtlVal,8);
      ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL, txCtlVal);
      ssb_w32(gBus.bar0,base+B43_DMA32_TXRING,txRingVal);
      txCtlRb  = ssb_r32(gBus.bar0,base+B43_DMA32_TXCTL);
      txRingRb = ssb_r32(gBus.bar0,base+B43_DMA32_TXRING);
      txStatRb = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS);
      Say("");
      SayH("  TXCTL   reads back = ",txCtlRb,8);
      SayH("  TXRING  reads back = ",txRingRb,8);
      SayH("  TXSTATUS           = ",txStatRb,8);
      SayH("    TXSTATE          = ",txStatRb&B43_DMA32_TXSTATE,4);
      SayH("    TXERROR          = ",txStatRb&B43_DMA32_TXERROR,8);
      SayH("    TXDPTR           = ",txStatRb&B43_DMA32_TXDPTR,4);
      { int ctlOk  = ((txCtlRb & B43_DMA32_TXENABLE)!=0);
        int ringOk = (txRingRb == txRingVal);
        int errOk  = ((txStatRb & B43_DMA32_TXERROR)==0);
        SayOk("TXCTL holds TXENABLE",ctlOk);
        SayOk("TXRING reads back exactly what was written",ringOk);
        SayOk("TXERROR is clear",errOk);
        if(!ringOk) Say("     !! the ring address did not stick -- check alignment and the window.");
        oracleC = ctlOk && ringOk && errOk; } }

    /* ================= ORACLE D: program RX ================= */
    Say("");
    Say("=== [5-2f] dmacontroller_setup, RX arm ===");
    Say("  value  = (frameoffset << RXFROFF_SHIFT);");
    Say("  value |= B43_DMA32_RXENABLE;");
    Say("  write RXCTL, RXRING = addrlo, then RXINDEX = nr_slots * 8.");
    { UInt32 addrext = B43DmaAddressExt(rxRing.phys);
      rxRingVal  = B43DmaAddressLow(rxRing.phys);
      rxCtlVal   = (frameoffset << B43_DMA32_RXFROFF_SHIFT);
      rxCtlVal  |= B43_DMA32_RXENABLE;
      rxCtlVal  |= (addrext << B43_DMA32_RXADDREXT_SHIFT) & B43_DMA32_RXADDREXT_MASK;
      rxIndexVal = (UInt32)K2_RX_SLOTS * B43_DMADESC32_BYTES;
      SayH("  ring physical      = ",rxRing.phys,8);
      SayH("  RXRING (composed)  = ",rxRingVal,8);
      SayH("  RXCTL              = ",rxCtlVal,8);
      Say ("    (frameoffset 30 << 1 = 0x3C, plus RXENABLE 0x1, so 0x3D is expected.)");
      Say ("    ⚠ k1 read RXCTL = 0x40 before anything was written -- offset 32, left by the");
      Say ("      initvals. b43 overwrites it here, which is why 0x3D replaces it.");
      SayH("  RXINDEX            = ",rxIndexVal,8);
      ssb_w32(gBus.bar0,base+B43_DMA32_RXCTL,  rxCtlVal);
      ssb_w32(gBus.bar0,base+B43_DMA32_RXRING, rxRingVal);
      ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,rxIndexVal);
      rxCtlRb   = ssb_r32(gBus.bar0,base+B43_DMA32_RXCTL);
      rxRingRb  = ssb_r32(gBus.bar0,base+B43_DMA32_RXRING);
      rxIndexRb = ssb_r32(gBus.bar0,base+B43_DMA32_RXINDEX);
      rxStatRb  = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
      Say("");
      SayH("  RXCTL   reads back = ",rxCtlRb,8);
      SayH("  RXRING  reads back = ",rxRingRb,8);
      SayH("  RXINDEX reads back = ",rxIndexRb,8);
      SayH("  RXSTATUS           = ",rxStatRb,8);
      SayH("    RXSTATE          = ",rxStatRb&B43_DMA32_RXSTATE,4);
      SayH("    RXERROR          = ",rxStatRb&B43_DMA32_RXERROR,8);
      SayH("    RXDPTR           = ",rxStatRb&B43_DMA32_RXDPTR,4);
      { UInt32 st = rxStatRb & B43_DMA32_RXSTATE;
        UInt32 er = rxStatRb & B43_DMA32_RXERROR;
        int ctlOk  = ((rxCtlRb & B43_DMA32_RXENABLE)!=0) &&
                     (((rxCtlRb & B43_DMA32_RXFROFF_MASK)>>B43_DMA32_RXFROFF_SHIFT)==frameoffset);
        int ringOk = (rxRingRb == rxRingVal);
        int errOk  = (er == B43_DMA32_RXERR_NOERR);
        int stOk   = (st==B43_DMA32_RXSTAT_DISABLED)||(st==B43_DMA32_RXSTAT_ACTIVE)||
                     (st==B43_DMA32_RXSTAT_IDLEWAIT)||(st==B43_DMA32_RXSTAT_STOPPED);
        SayOk("RXCTL holds RXENABLE and frame offset 30",ctlOk);
        SayOk("RXRING reads back exactly what was written",ringOk);
        SayOk("RXERROR is NOERR",errOk);
        SayOk("RXSTATE is one of DISABLED/ACTIVE/IDLEWAIT/STOPPED",stOk);
        if(!errOk){
          Say("     RXERROR decodes as:");
          if(er==B43_DMA32_RXERR_PROT)     Say("       PROT -- a protocol error on the backplane");
          if(er==B43_DMA32_RXERR_OVERFLOW) Say("       OVERFLOW");
          if(er==B43_DMA32_RXERR_BUFWRITE) Say("       BUFWRITE -- the card could not write a buffer");
          if(er==B43_DMA32_RXERR_DESCREAD) Say("       DESCREAD -- the card could not READ the ring."); }
        oracleD = ctlOk && ringOk && errOk && stOk; } }

    /* ---- did anything move? Reported, not scored. ---- */
    Say("");
    Say("=== [5-2g] DID EITHER ENGINE MOVE? (reported, NOT scored) ===");
    Say("  With MACCTL_ENABLED clear no frame can arrive, so RXDPTR staying at 0 is the");
    Say("  expected result. A DMA engine that prefetches its first descriptor would show");
    Say("  movement here, and that would be free evidence that the card reads our memory.");
    { UInt32 t0,r0; int k;
      for(k=0;k<5;k++){
        t0=ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS);
        r0=ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS);
        { Str255 L;L[0]=0;PCat(L,"    poll ");PCatDec(L,(unsigned long)k);
          PCat(L,": TXSTATUS=0x");PCatHex(L,t0,8);
          PCat(L," RXSTATUS=0x");PCatHex(L,r0,8);Out(L); }
        SsbSpinUs(20000); } }

    /* ---- leave the engines quiescent ---- */
    Say("");
    Say("=== [5-2h] CLEANUP ===");
    Say("  dmacontroller_cleanup: reset each controller, then zero its ring register. The rings");
    Say("  are about to be freed, so leaving the hardware pointed at them would be a loaded gun.");
    { UInt32 s1,s2;
      (void)DmaControllerTxReset(base,&s1);
      ssb_w32(gBus.bar0,base+B43_DMA32_TXRING,0);
      (void)DmaControllerRxReset(base,&s2);
      ssb_w32(gBus.bar0,base+B43_DMA32_RXRING,0);
      SayH("  TXSTATUS after cleanup = ",s1,8);
      SayH("  RXSTATUS after cleanup = ",s2,8);
      SayH("  TXRING now = ",(unsigned long)ssb_r32(gBus.bar0,base+B43_DMA32_TXRING),8);
      SayH("  RXRING now = ",(unsigned long)ssb_r32(gBus.bar0,base+B43_DMA32_RXRING),8); }
cleanup:
    /* ⚠⚠ THIS MUST BE REACHED ON EVERY EXIT PATH, AND IT IS WHY THE EARLY `goto`s TARGET IT.
     * NewPtrSysClear takes from the SYSTEM heap, and OS 9 does NOT reclaim that when an
     * application quits -- unlike the app heap. A probe that bailed out after allocating would
     * leak locked system memory until the next reboot, and this one is meant to be re-run back
     * to back. DmaBlockFree nulls what it frees, so the normal path reaching here after already
     * freeing is a no-op. */
    DmaBlockFree(&txRing); DmaBlockFree(&rxRing); DmaBlockFree(&rxPool);
    Say("  rings and pool unlocked and disposed.");

    /* No `verdict:` label: every early exit now targets cleanup, which falls through to here.
     * Leaving an unreachable label behind would be one more place a future goto could land
     * while skipping the frees, which is the bug this restructure just removed. */
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB&&oracleC&&oracleD&&oracleE){
      Say("  ★★★★★ BOTH DMA RINGS ARE PROGRAMMED AND THE ENGINES ACCEPTED THEM.");
      Say("        A  both controller resets reached DISABLED");
      Say("        B  descriptor words are little-endian in memory, proven byte by byte");
      Say("        C  the TX ring address stuck and TXERROR is clear");
      Say("        D  the RX ring address stuck, frame offset is 30, RXERROR is NOERR");
      Say("        E  all 32 descriptors decode back to their own buffers");
      Say("     ⇒ NEXT (5-3): enable the MAC and post a frame. That is the first increment");
      Say("       where the card must READ our descriptors, so it is also the first that");
      Say("       depends on cache coherency -- and the poison bytes become a live oracle.");
    } else {
      Say("  ✗ NOT VERIFIED. Do not build on this.");
      if(!oracleA) Say("    - a controller reset did not reach DISABLED.");
      if(!oracleB) Say("    - DESCRIPTORS ARE NOT LITTLE-ENDIAN. Fix this before anything else.");
      if(!oracleC) Say("    - the TX ring did not program cleanly.");
      if(!oracleD) Say("    - the RX ring did not program cleanly. Check RXERROR above.");
      if(!oracleE) Say("    - a descriptor does not match its buffer. The fill logic is wrong.");
    }
    Say("");
    Say("  ⚠ Engines reset and ring registers zeroed before the memory was freed.");
    Say("    Re-runnable immediately.");
    Say("");
    Say("=== done. Log: 'AirPort DMA2 Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
