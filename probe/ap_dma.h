/* ap_dma.h -- the DMA layer: coherent memory, 32-bit descriptors, and the RX ring.
 *
 * ★ THE FOURTH EXTRACTION, and by now the pattern is the point. ap_bringup.h freed chip_init's
 * prefix, ap_phy_initg.h freed b43_phy_init, ap_chipinit_tail.h freed statements 8-13. This frees
 * b43_dma_init's RX half so the driver can build a ring by CALLING the proven code rather than
 * re-deriving it. "A hand-copied sequence drifts, a called function cannot."
 *
 * ⚠ THE STORAGE BARRIER IS THE LOAD-BEARING PART. DmaPublish() is an eieio, and the long note
 *   carried across below explains why it exists: a device-memory store may overtake earlier
 *   stores to ordinary memory, so the card can be told "there is work at index 16" before the
 *   descriptors are visible to it. RX appeared to work without it purely by timing -- the dwell
 *   drains the stores long before the card needs them -- which is correctness by accident.
 *   Moving this code without the barrier would have reproduced that accident in a driver whose
 *   timing is nothing like the probe's.
 *
 * ⚠ MEMORY IS SYSTEM-HEAP AND LOCKED. DmaBlockAlloc does NewPtrSysClear, LockMemory and
 *   GetPhysical. All three are InterfaceLib, which the driver already links -- so this costs no
 *   new library dependency and DriverServicesLib stays absent, which remains the guarantee worth
 *   keeping until there is an interrupt handler that needs it.
 *
 * ⚠⚠ AND ALLOCATION IS TASK-LEVEL ONLY. Apple documents EnetHAL_Open as task time and says
 *   memory allocation is safe there; NO other selector carries that promise. A driver that
 *   allocated from Read, Write or Start would be allocating below task level, which is the same
 *   class of error as calling the File Manager from an ISR. The static audit in the build now
 *   enforces this by SELECTOR rather than globally: NewPtrSysClear reachable from Open is fine,
 *   reachable from anything else is a defect.
 *
 * ★ HOW IT WAS MADE. Lifted VERBATIM from airport_rx.c by line range, never retyped, and proven
 * by reconstruction against the original file. Same technique as the three extractions before it.
 */
#ifndef AP_DMA_H
#define AP_DMA_H

#include "ap_chipinit_tail.h"   /* brings ap_phy_initg.h and ap_bringup.h with it */

/* ⚠ THESE CONSTANTS CAME WITH THE CODE, and had to. They lived in airport_rx.c, so a
 * driver including this header compiled until it did not -- SSB_PCI_DMA, the translation
 * mask, K3_PAGE and the whole B43_DMA32_* block were all app-local. Leaving them behind
 * would have meant a second set of definitions for the same registers, which is the
 * duplication 8-2d-2a removed from this project once already. Moved, not copied. */

/* ---- b43_dma.h, 32-bit engine (as k2) ---- */
#define B43_MMIO_DMA32_BASE0        0x200UL
#define B43_DMA32_TXCTL             0x00UL
#define   B43_DMA32_TXENABLE        0x00000001UL
#define   B43_DMA32_TXADDREXT_MASK  0x00030000UL
#define   B43_DMA32_TXADDREXT_SHIFT 16
#define B43_DMA32_TXRING            0x04UL
#define B43_DMA32_TXINDEX           0x08UL
#define B43_DMA32_TXSTATUS          0x0CUL
#define   B43_DMA32_TXDPTR          0x00000FFFUL
#define   B43_DMA32_TXERROR         0x000F0000UL
#define   B43_DMA32_TXSTATE         0x0000F000UL
#define     B43_DMA32_TXSTAT_DISABLED 0x00000000UL
#define     B43_DMA32_TXSTAT_IDLEWAIT 0x00002000UL
#define     B43_DMA32_TXSTAT_STOPPED  0x00003000UL
#define B43_DMA32_RXCTL             0x10UL
#define   B43_DMA32_RXENABLE        0x00000001UL
#define   B43_DMA32_RXFROFF_SHIFT   1
#define   B43_DMA32_RXADDREXT_MASK  0x00030000UL
#define   B43_DMA32_RXADDREXT_SHIFT 16
#define B43_DMA32_RXRING            0x14UL
#define B43_DMA32_RXINDEX           0x18UL
#define B43_DMA32_RXSTATUS          0x1CUL
#define   B43_DMA32_RXDPTR          0x00000FFFUL
#define   B43_DMA32_RXSTATE         0x0000F000UL
#define     B43_DMA32_RXSTAT_DISABLED 0x00000000UL
/* ⚠ k20 PRINTED RXSTATUS AS A BARE NUMBER because only DISABLED was ever defined here, and
 * the one value that mattered -- 0x2000, IDLEWAIT -- went past unread in four separate log
 * lines. CLAUDE.md, "Read what you already have": never log a status register as a bare
 * number, decode every field you have a definition for. These are the missing definitions. */
#define     B43_DMA32_RXSTAT_ACTIVE   0x00001000UL
#define     B43_DMA32_RXSTAT_IDLEWAIT 0x00002000UL
#define     B43_DMA32_RXSTAT_STOPPED  0x00003000UL
#define   B43_DMA32_RXERROR         0x000F0000UL
#define     B43_DMA32_RXERR_NOERR     0x00000000UL
#define     B43_DMA32_RXERR_PROT      0x00010000UL
#define     B43_DMA32_RXERR_UNDERRUN  0x00020000UL
#define     B43_DMA32_RXERR_DESCREAD  0x00040000UL
#define     B43_DMA32_RXERR_CORE      0x00050000UL
#define B43_DMA32_DCTL_BYTECNT      0x00001FFFUL
#define B43_DMA32_DCTL_ADDREXT_MASK 0x00030000UL
#define B43_DMA32_DCTL_ADDREXT_SHIFT 16
#define B43_DMA32_DCTL_DTABLEEND    0x10000000UL
#define B43_DMA32_DCTL_IRQ          0x20000000UL
#define B43_DMA32_DCTL_FRAMEEND     0x40000000UL
#define B43_DMA32_DCTL_FRAMESTART   0x80000000UL
#define B43_DMA32_RINGMEMSIZE       4096UL
#define B43_DMADESC32_BYTES         8UL

#define IEEE80211_MAX_FRAME_LEN     2352UL
#define B43_DMA0_RX_FW351_FO        30UL
#define B43_DMA0_RX_FW351_BUFSIZE   (B43_DMA0_RX_FW351_FO + IEEE80211_MAX_FRAME_LEN)
#define B43_PLCP_HDR6_BYTES         6UL

#define SSB_DMA_TRANSLATION_MASK    0xC0000000UL
#define SSB_DMA_TRANSLATION_SHIFT   30
#define SSB_PCI_DMA                 0x40000000UL
#define SSB_PCI_DMA_SZ              0x40000000UL

#define K3_RX_SLOTS                 32
#define K3_PAGE                     4096UL

/* ⚠ 8-2f: SayErr and the little-endian 32-bit accessors came across with the DMA code
 * because it CALLS them -- AllocOrReport reports a LockMemory error through SayErr, and
 * Op32FillDescriptor stores descriptors little-endian. The compiler found both: they were
 * implicit-declaration warnings that became a link error, which is the cheap way to learn
 * an extraction was incomplete. A header that compiles standalone in a driver context is
 * the check that catches this, and it now runs before the driver is built. */

/* ⚠ Named SayErr, not SayS. ap_phy_initg.h already exports SayS(const char*,int) from the j7
 * extraction, and that file is a verbatim copy of a probe whose log is banked -- renaming OURS
 * is correct, editing the extraction to make room for a newer probe is not. */
static void SayErr(const char*s,long v){Str255 L;L[0]=0;PCat(L,s);
  if(v<0){PCat(L,"-");v=-v;} PCatDec(L,(unsigned long)v);Out(L);}

static inline void le32st(void *p,UInt32 v){
  __asm__ __volatile__("stwbrx %0,0,%1"::"r"(v),"r"(p):"memory"); }
static inline UInt32 le32ld(const void *p){ UInt32 v;
  __asm__ __volatile__("lwbrx %0,0,%1":"=r"(v):"r"(p):"memory"); return v; }
/* ============================================================================
 * ⚠⚠ THE STORAGE BARRIER k7 DID NOT HAVE, AND WHY IT IS HERE.
 *
 * k7 wrote two TX descriptors to memory and then wrote TXINDEX to the BAR. The
 * engine took the index -- TXACTIVE went 0 -> 16, exactly the value written --
 * and never fetched descriptor 0. TXDPTR stayed at 0, TXERROR stayed clean.
 *
 * ssb_core.h's accessors carry a "memory" clobber, which stops the COMPILER
 * reordering and does nothing whatever about the hardware. On PowerPC an MMIO
 * store may overtake earlier stores to ordinary memory, so the card can be told
 * "there is work at index 16" before the descriptors are visible to it.
 *
 * The witness is our own driver. usb2-ehci uses eieio in twelve places and says
 * what for:
 *     ehci_os.c:211   eieio   /_ publish payload before the index _/
 *     ehci_uim.c:615  eieio   /_ publish payload before the index (dual-CPU safe) _/
 *     ehci_hw.c:197   eieio   /_ order before publish _/
 * That is exactly this operation, in a driver moving data at USB 2.0 rates, and
 * the AirPort code had no barrier anywhere.
 *
 * ★ IT ALSO EXPLAINS WHY RX WORKED. The RX descriptors are written and then we
 * dwell for a second before the card needs them, so the stores drain long
 * beforehand. RX has been correct by timing, not by construction -- so the
 * barrier goes in BOTH paths, not just the one that failed.
 * ========================================================================= */
static inline void DmaPublish(void)
{ __asm__ __volatile__("eieio" ::: "memory"); }

static inline void le16st(void *p,UInt16 v){
  __asm__ __volatile__("sthbrx %0,0,%1"::"r"((UInt32)v),"r"(p):"memory"); }
static UInt16 le16at(const UInt8*p){ return (UInt16)((UInt32)p[0] | ((UInt32)p[1]<<8)); }

static UInt32 gTranslation = SSB_PCI_DMA;
static UInt32 B43DmaAddressLow(UInt32 a){ a&=~SSB_DMA_TRANSLATION_MASK; a|=gTranslation; return a; }
static UInt32 B43DmaAddressExt(UInt32 a){ a&=SSB_DMA_TRANSLATION_MASK; a>>=SSB_DMA_TRANSLATION_SHIFT; return a; }

/* b43's mac_suspended starts at 1 (setup_struct_wldev_for_init), so the first mac_enable is
 * what actually turns the receiver on. Modelled explicitly rather than implied. */
static int gMacSuspended = 1;
static UInt32 gKtp = 0;

/* ============================================================================
 * memory (k1/k2, proven)
 * ==========================================================================*/
typedef struct { Ptr raw; UInt8 *base; UInt32 phys,physRun,size,entries;
                 OSErr lockErr,physErr; int ok; } DmaBlock;

static int DmaBlockAlloc(DmaBlock *b, UInt32 size)
{
    LogicalToPhysicalTable tbl; unsigned long count=8; int i;
    b->raw=0;b->base=0;b->phys=0;b->physRun=0;b->size=size;
    b->entries=0;b->lockErr=noErr;b->physErr=noErr;b->ok=0;
    b->raw = NewPtrSysClear((Size)(size + K3_PAGE));
    if(b->raw==0) return 0;
    b->base = (UInt8*)((((UInt32)b->raw)+(K3_PAGE-1))&~(K3_PAGE-1));
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
    b->raw=0; b->base=0;
}
static int AllocOrReport(const char *what, DmaBlock *b, UInt32 size)
{
    if(DmaBlockAlloc(b,size)) return 1;
    { Str255 L;L[0]=0;PCat(L,"!! ");PCat(L,what);PCat(L," allocation FAILED");Out(L); }
    if(b->raw==0)              Say ("   NewPtrSysClear returned NULL -- system heap exhausted.");
    else if(b->lockErr!=noErr) SayErr("   LockMemory err = ",(long)b->lockErr);
    else                       SayErr("   GetPhysical err = ",(long)b->physErr);
    return 0;
}
static int PhysOfPage(void *pg, UInt32 *out)
{
    LogicalToPhysicalTable tbl; unsigned long count=8; int i;
    for(i=0;i<8;i++){tbl.physical[i].address=0;tbl.physical[i].count=0;}
    tbl.logical.address=pg; tbl.logical.count=K3_PAGE;
    if(GetPhysical(&tbl,&count)!=noErr) return 0;
    if(count!=1) return 0;
    *out=(UInt32)tbl.physical[0].address; return 1;
}
static int InWindow(UInt32 p,UInt32 s){ return (p<SSB_PCI_DMA_SZ)&&((p+s)<=SSB_PCI_DMA_SZ); }

/* ============================================================================
 * DMA controller (k2, proven)
 * ==========================================================================*/
static int DmaControllerRxReset(UInt32 base)
{
    int i; UInt32 v;
    ssb_w32(gBus.bar0,base+B43_DMA32_RXCTL,0);
    for(i=0;i<10;i++){
      v = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS) & B43_DMA32_RXSTATE;
      if(v==B43_DMA32_RXSTAT_DISABLED){ i=-1; break; }
      SsbSpinUs(1000); }
    return (i==-1);
}
static int DmaControllerTxReset(UInt32 base)
{
    int i; UInt32 v;
    for(i=0;i<10;i++){
      v = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS) & B43_DMA32_TXSTATE;
      if(v==B43_DMA32_TXSTAT_DISABLED||v==B43_DMA32_TXSTAT_IDLEWAIT||
         v==B43_DMA32_TXSTAT_STOPPED) break;
      SsbSpinUs(1000); }
    ssb_w32(gBus.bar0,base+B43_DMA32_TXCTL,0);
    for(i=0;i<10;i++){
      v = ssb_r32(gBus.bar0,base+B43_DMA32_TXSTATUS) & B43_DMA32_TXSTATE;
      if(v==B43_DMA32_TXSTAT_DISABLED){ i=-1; break; }
      SsbSpinUs(1000); }
    if(i!=-1) return 0;
    SsbSpinUs(1000); return 1;
}
/* The full op32_fill_descriptor. The RX path calls the wrapper below with all three flags
 * clear, which is what an RX descriptor carries; TX needs FRAMESTART/FRAMEEND/IRQ. */
static void Op32FillDescriptorFull(UInt8 *descbase,int slot,int nrSlots,
                                   UInt32 dmaaddr,UInt16 bufsize,int start,int end,int irq)
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

static void Op32FillDescriptor(UInt8 *descbase,int slot,int nrSlots,
                               UInt32 dmaaddr,UInt16 bufsize)
{
    UInt8 *d = descbase + (UInt32)slot*B43_DMADESC32_BYTES;
    UInt32 ctl,addr,addrext;
    addr    = B43DmaAddressLow(dmaaddr);
    addrext = B43DmaAddressExt(dmaaddr);
    ctl = (UInt32)bufsize & B43_DMA32_DCTL_BYTECNT;
    if(slot == nrSlots-1) ctl |= B43_DMA32_DCTL_DTABLEEND;
    ctl |= (addrext << B43_DMA32_DCTL_ADDREXT_SHIFT) & B43_DMA32_DCTL_ADDREXT_MASK;
    le32st(d+0,ctl);
    le32st(d+4,addr);
}
static void PoisonRxBuffer(UInt8 *buf,UInt32 frameoffset)
{
    UInt32 i;
    buf[0]=0; buf[1]=0;
    for(i=0;i<B43_PLCP_HDR6_BYTES+2;i++) buf[frameoffset+i]=0xFF;
}
static int RxBufferIsPoisoned(const UInt8 *buf,UInt32 frameoffset)
{
    const UInt8 *f = buf + frameoffset;
    UInt8 a = (UInt8)(f[0]&f[1]&f[2]&f[3]&f[4]&f[5]&f[6]&f[7]);
    return (a == 0xFF);
}

static int gArmFails = 0;
static int gArmSlow  = 0;

static int RxRingArm(UInt32 base,DmaBlock *ring,UInt8 **bufLog,UInt32 *bufPhys,
                     UInt32 frameoffset,UInt32 bufSize)
{
    UInt32 value,addrext,stBefore,stAfterReset,stArmed; int i,ok,slow=0;
    Str255 L;

    stBefore = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS) & B43_DMA32_RXSTATE;
    ok = DmaControllerRxReset(base);
    if(!ok){
      /* b43's rx_reset polls 10 times at msleep(1). If that is simply not long enough on this
       * part, a longer wait reaches DISABLED and the fix is the timeout. If it never does, the
       * engine will not stop while armed and the fix is to stop re-arming -- b43 recycles
       * descriptors at runtime and never resets a live RX ring. */
      for(i=0;i<200;i++){
        if((ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS)&B43_DMA32_RXSTATE)
            ==B43_DMA32_RXSTAT_DISABLED){ ok=1; slow=1; break; }
        SsbSpinUs(1000); } }
    stAfterReset = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS) & B43_DMA32_RXSTATE;

    for(i=0;i<K3_RX_SLOTS;i++){
      PoisonRxBuffer(bufLog[i],frameoffset);
      Op32FillDescriptor(ring->base,i,K3_RX_SLOTS,bufPhys[i],(UInt16)bufSize); }
    addrext = B43DmaAddressExt(ring->phys);
    value  = (frameoffset << B43_DMA32_RXFROFF_SHIFT);
    value |= B43_DMA32_RXENABLE;
    value |= (addrext << B43_DMA32_RXADDREXT_SHIFT) & B43_DMA32_RXADDREXT_MASK;
    DmaPublish();          /* the descriptors and the poison must be visible before the index */
    ssb_w32(gBus.bar0,base+B43_DMA32_RXCTL, value);
    ssb_w32(gBus.bar0,base+B43_DMA32_RXRING,B43DmaAddressLow(ring->phys));
    ssb_w32(gBus.bar0,base+B43_DMA32_RXINDEX,(UInt32)K3_RX_SLOTS*B43_DMADESC32_BYTES);
    stArmed = ssb_r32(gBus.bar0,base+B43_DMA32_RXSTATUS) & B43_DMA32_RXSTATE;

    if(!ok)   gArmFails++;
    if(slow)  gArmSlow++;
    L[0]=0;PCat(L,"      [arm] RXSTATE ");PCatHex(L,stBefore,4);
    PCat(L," -> reset ");PCat(L, ok ? (slow?"OK (SLOW)":"ok") : "FAILED");
    PCat(L," -> ");PCatHex(L,stAfterReset,4);
    PCat(L," -> armed ");PCatHex(L,stArmed,4);
    if(!ok) PCat(L,"   ⚠ engine never reached DISABLED");
    Out(L);
    return ok;
}

#endif /* AP_DMA_H */
