/* ssb_core.h -- shared SiliconBackplane primitives for the AirPort Extreme probes.
 *
 * ★★★★★ WHY THIS EXISTS, AND WHY IT EXISTS NOW.
 * Stages 1 through 4b-i each carried their own copy of the backplane code, and the copies drifted:
 *   - Stage 2 bounded its core walk by ChipCommon's count. Every probe after it silently dropped
 *     that bound to "idx < 8", which was harmless only because they all broke out at the first
 *     802.11 core. 4b-i needed both cores, removed the break, and took a hard access exception at
 *     core index 5. The bug had been sitting in three probes, correct by accident.
 *   - IDHIGH_CC was written as ((v) >> 4) & 0xFFF in every probe. The real mask is
 *     SSB_IDHIGH_CC = 0x00008FF0, i.e. bits 4-11 plus bit 15. Bits 12-14 are RCHI, the revision
 *     HIGH part. Every core seen so far has revision < 16 so RCHI was zero and the two agreed --
 *     a core with revision >= 16 would have had its revision OR'd into the core code.
 * Copy-paste is how both of those happened. One header, one definition.
 *
 * ★ THE HEADLINE ADDITION: SsbCoreDisable(), a real port of ssb_device_disable().
 * Every probe since 3a has required a COLD BOOT, because they all skip the teardown path and are
 * only safe when RESET is already asserted. That cost five boots on 4b-i alone, and 4b-ii -- which
 * ports a ~3000-line PHY init sequence -- will iterate harder. This implements the path properly:
 * the reject handshake, the backplane-revision-dependent reject bitmask, and the initiator dance.
 *
 * Header-only, everything static, so a probe just #includes it -- no CMake or link changes.
 * Every constant and both reset sequences are taken VERBATIM from Linux mainline
 * (drivers/ssb/main.c, include/linux/ssb/ssb_regs.h), not from memory. That distinction is not
 * pedantry here: three of 4b-i's five builds failed on details asserted rather than read.
 */
#ifndef SSB_CORE_H
#define SSB_CORE_H

#include <Timer.h>
#include <NameRegistry.h>
#include <PCI.h>

/* --- backplane addressing -------------------------------------------------- */
#define SSB_ENUM_BASE     0x18000000UL
#define SSB_CORE_SIZE     0x1000UL
#define SSB_BAR0_WIN      0x80UL        /* PCI CONFIG register, not MMIO */
#define SSB_SPROM_BASE1   0x1000UL

/* --- per-core registers, at the TOP of each 4 KB window -------------------- */
#define SSB_IMSTATE       0x0F90UL
#define SSB_TMSLOW        0x0F98UL
#define SSB_TMSHIGH       0x0F9CUL
#define SSB_IDLOW         0x0FF8UL
#define SSB_IDHIGH        0x0FFCUL

#define SSB_TMSLOW_RESET     0x00000001UL
#define SSB_TMSLOW_REJECT    0x00000002UL   /* standard backplane */
#define SSB_TMSLOW_REJECT_23 0x00000004UL   /* backplane rev 2.3 uses a different bit */
#define SSB_TMSLOW_CLOCK     0x00010000UL
#define SSB_TMSLOW_FGC       0x00020000UL

#define SSB_TMSHIGH_SERR     0x00000001UL
#define SSB_TMSHIGH_BUSY     0x00000004UL
#define SSB_TMSHIGH_DMA64    0x10000000UL
#define B43_TMSHIGH_HAVE_2GHZ_PHY 0x00010000UL
#define B43_TMSHIGH_HAVE_5GHZ_PHY 0x00020000UL

#define SSB_IMSTATE_IBE      0x00020000UL
#define SSB_IMSTATE_TO       0x00040000UL
#define SSB_IMSTATE_BUSY     0x01800000UL   /* backplane rev >= 2.3 only */
#define SSB_IMSTATE_REJECT   0x02000000UL   /* backplane rev >= 2.3 only */

#define SSB_IDLOW_INITIATOR  0x00000080UL
#define SSB_IDLOW_SSBREV     0xF0000000UL
#define SSB_IDLOW_SSBREV_22  0x00000000UL
#define SSB_IDLOW_SSBREV_23  0x10000000UL
#define SSB_IDLOW_SSBREV_24  0x40000000UL
#define SSB_IDLOW_SSBREV_25  0x50000000UL
#define SSB_IDLOW_SSBREV_26  0x60000000UL
#define SSB_IDLOW_SSBREV_27  0x70000000UL

/* ⚠ CORE CODE MASK IS 0x00008FF0, NOT 0x0000FFF0. Bits 12-14 are RCHI (revision high part).
 * Using >>4 & 0xFFF ORs a core's high revision bits into its core code. Harmless on every core
 * this project has met (all rev < 16) and wrong the moment one is not. */
#define SSB_IDHIGH_CC        0x00008FF0UL
#define SSB_IDHIGH_RCLO      0x0000000FUL
#define SSB_IDHIGH_RCHI      0x00007000UL
#define IDHIGH_CC(v)      (((v) & SSB_IDHIGH_CC) >> 4)
#define IDHIGH_REV(v)     ((((v) & SSB_IDHIGH_RCHI) >> 8) | ((v) & SSB_IDHIGH_RCLO))
#define IDHIGH_VENDOR(v)  (((v) >> 16) & 0xFFFFUL)

#define SSB_DEV_CHIPCOMMON 0x800UL
#define SSB_DEV_PCI        0x804UL
#define SSB_DEV_MIPS       0x805UL
#define SSB_DEV_ETHERNET   0x806UL
#define SSB_DEV_V90        0x807UL
#define SSB_DEV_PCMCIA     0x80DUL
#define SSB_DEV_80211      0x812UL

#define CC_CHIPID         0x0000UL
#define CHIPID_ID(v)      ((v) & 0xFFFFUL)
#define CHIPID_REV(v)     (((v) >> 16) & 0xFUL)
#define CHIPID_NCORES(v)  (((v) >> 24) & 0xFUL)

#define kVendorBroadcom   0x14E4UL
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

/* --- byte-reversing MMIO. The chip is little-endian; the G4 is not. -------- */
static inline UInt32 ssb_r32(volatile void*b,UInt32 o){volatile UInt8*p=(volatile UInt8*)b+o;UInt32 v;
  __asm__ __volatile__("lwbrx %0,0,%1":"=r"(v):"r"(p):"memory");return v;}
static inline void ssb_w32(volatile void*b,UInt32 o,UInt32 v){volatile UInt8*p=(volatile UInt8*)b+o;
  __asm__ __volatile__("stwbrx %0,0,%1"::"r"(v),"r"(p):"memory");}
static inline UInt16 ssb_r16(volatile void*b,UInt32 o){volatile UInt8*p=(volatile UInt8*)b+o;UInt32 v;
  __asm__ __volatile__("lhbrx %0,0,%1":"=r"(v):"r"(p):"memory");return (UInt16)v;}
static inline void ssb_w16(volatile void*b,UInt32 o,UInt16 v){volatile UInt8*p=(volatile UInt8*)b+o;
  __asm__ __volatile__("sthbrx %0,0,%1"::"r"((UInt32)v),"r"(p):"memory");}

/* ⚠ No Delay() near any of this. Delay(1) is 16.7 ms on OS 9; these want microseconds. */
static void SsbSpinUs(UInt32 us){UnsignedWide a,b;Microseconds(&a);do{Microseconds(&b);}while((b.lo-a.lo)<us);}

/* --- the bus handle -------------------------------------------------------- */
typedef struct {
    RegEntryID      node;
    volatile void  *bar0;
    UInt32          bar0Size;
    UInt32          nCores;
    UInt32          idx80211;
    UInt32          idxChipCommon;
    UInt32          chipId;
    UInt32          chipRev;
    UInt32          selectRetries;
    UInt32          lastError;
    /* 8-78 k184: backplane crystal/PLL power-up witness (SsbPowerOn) */
    UInt32          gpioIn;        /* PCI-config GPIO_IN read at power-on entry */
    UInt32          pwrDanceRan;   /* 1 = we ran the xtal+PLL dance; 0 = crystal already on */
    UInt32          stabortWas;    /* PCI status (0x06) before we scrubbed Signaled Target Abort */
} SsbBus;

#define kSsbNoCore 0xFFFFFFFFUL

static OSStatus SsbGetProp(RegEntryID*n,const char*nm,void**ob,RegPropertyValueSize*os){
  RegPropertyValueSize sz=0;void*b;OSStatus e;
  e=RegistryPropertyGetSize(n,nm,&sz);if(e!=noErr||sz==0)return (e!=noErr)?e:paramErr;
  b=NewPtr((Size)sz);if(!b)return memFullErr;
  e=RegistryPropertyGet(n,nm,b,&sz);if(e!=noErr){DisposePtr((Ptr)b);return e;}
  *ob=b;*os=sz;return noErr;}

/* Find the BCM4306 node, map BAR0, and make sure memory space is enabled.
 * Open Firmware leaves memory decode CLEAR on this card (Stage 1: command = 0x0004), so the
 * enable is not optional -- without it every MMIO read returns bus-float garbage. */
/* 8-78 k184: PCI-config GPIO for the backplane crystal+PLL, offsets/bits verbatim from FreeBSD bwi
 * (reference/bwi/if_bwireg.h:351-360, SAME BCM4306 silicon) and drivers/ssb/pci.c ssb_pci_xtal. */
#define SSB_GPIO_IN        0xB0UL   /* PCI CONFIG, 32-bit */
#define SSB_GPIO_OUT       0xB4UL
#define SSB_GPIO_OUTEN     0xB8UL
#define SSB_GPIO_XTAL      0x40UL   /* bit6: crystal/backplane power ON (bwi PWR_ON) */
#define SSB_GPIO_PLL       0x80UL   /* bit7: PLL held OFF while this bit is set (bwi PLL_PWR_OFF) */
#define SSB_PCI_STABORT    0x0800UL /* bit11 of PCI status @0x06: Signaled Target Abort -- the card ended one
                                     * of OUR reads with target-abort (FreeBSD PCIM_STATUS_STABORT = 0x0800,
                                     * the constant bwi uses). ⚠ k184-k203 had 0x1000 here: bit12, RECEIVED
                                     * Target Abort, set when a DMA cycle the CARD started is aborted. */

/* ★★★ 8-78 k184: THE ~30% BRING-UP TARGET-ABORT FIX. b43/ssb and bwi power the backplane crystal +
 * PLL through PCI-config GPIO and settle BEFORE reading ANY core register; we went straight from PCI
 * D0 (k173) into SsbScanCores. D0 is the PCI FUNCTION power state -- the backplane clock is a SEPARATE
 * domain via GPIO 0xB0/0xB4/0xB8, which nothing here ever touched. Reading a core-window register
 * before the PLL locks target-aborts (R3=bar0), intermittently. Port of bwi_power_on(sc,1) /
 * ssb_pci_xtal(XTAL|PLL,1): config-space only, task level, idempotent (skips the dance if the crystal
 * is already on, so it cannot regress a boot that already works), and it ends by scrubbing the latched
 * target-abort the power-up window produces -- bwi does exactly this, knowing this fault. */
static void SsbPowerOn(SsbBus *bus)
{
    UInt32 gin = 0, gout = 0, gen = 0; UInt16 status = 0;
    bus->gpioIn = 0; bus->pwrDanceRan = 0; bus->stabortWas = 0;
    if (ExpMgrConfigReadLong(&bus->node,(LogicalAddress)SSB_GPIO_IN,&gin) == noErr) {
        bus->gpioIn = gin;
        if (!(gin & SSB_GPIO_XTAL)) {                 /* crystal not already on -- don't glitch it */
            bus->pwrDanceRan = 1;
            (void)ExpMgrConfigReadLong(&bus->node,(LogicalAddress)SSB_GPIO_OUT,&gout);
            (void)ExpMgrConfigReadLong(&bus->node,(LogicalAddress)SSB_GPIO_OUTEN,&gen);
            gout |= (SSB_GPIO_XTAL | SSB_GPIO_PLL);   /* crystal ON, PLL held OFF first */
            gen  |= (SSB_GPIO_XTAL | SSB_GPIO_PLL);
            (void)ExpMgrConfigWriteLong(&bus->node,(LogicalAddress)SSB_GPIO_OUT,gout);
            (void)ExpMgrConfigWriteLong(&bus->node,(LogicalAddress)SSB_GPIO_OUTEN,gen);
            SsbSpinUs(1000);                           /* bwi DELAY(1000): crystal settle */
            gout &= ~SSB_GPIO_PLL;                     /* release PLL -> PLL ON */
            (void)ExpMgrConfigWriteLong(&bus->node,(LogicalAddress)SSB_GPIO_OUT,gout);
            SsbSpinUs(5000);                           /* bwi DELAY(5000): PLL-lock settle */
        }
    }
    if (ExpMgrConfigReadWord(&bus->node,(LogicalAddress)0x06,&status) == noErr) {
        bus->stabortWas = (UInt32)status;
        /* Scrub the power-up-window target-abort. The status error bits are write-1-to-clear (PCI 2.2
         * 6.2.3): a 1 clears the bit, a 0 leaves it. So write ONLY this bit. bwi's read, mask off, write
         * back does the reverse on a compliant device -- clears every OTHER latched error bit and leaves
         * this one set. k184-k203 masked 0x1000 off, which by the same rule wrote 0x0800 back and cleared
         * it anyway: the right effect for the wrong reason. Other error bits stay latched for the log. */
        if (status & SSB_PCI_STABORT)
            (void)ExpMgrConfigWriteWord(&bus->node,(LogicalAddress)0x06,(UInt16)SSB_PCI_STABORT);
    }
}

static OSStatus SsbFindAndMap(SsbBus *bus)
{
    RegEntryIter c; RegEntryID e; Boolean d=false,f=true; OSStatus er;
    UInt32 *aa=NULL,*la=NULL; RegPropertyValueSize as=0,ls=0; UInt32 i,ne;
    UInt16 cmd=0, back=0;
    int found=0;

    bus->bar0=0; bus->bar0Size=0; bus->nCores=0;
    bus->idx80211=kSsbNoCore; bus->idxChipCommon=kSsbNoCore;
    bus->selectRetries=0; bus->lastError=0;

    er=RegistryEntryIterateCreate(&c); if(er!=noErr) return er;
    for(;;){ UInt32 vid=0,did=0; RegPropertyValueSize sz;
      er=RegistryEntryIterate(&c,f?kRegIterDescendants:kRegIterContinue,&e,&d); f=false;
      if(er!=noErr||d) break;
      sz=sizeof(vid); if(RegistryPropertyGet(&e,"vendor-id",&vid,&sz)!=noErr) continue;
      if((vid&0xFFFFUL)!=kVendorBroadcom) continue;
      sz=sizeof(did); if(RegistryPropertyGet(&e,"device-id",&did,&sz)!=noErr) continue;
      did&=0xFFFFUL; if(did!=kDevBCM4306_4320&&did!=kDevBCM4306_4325) continue;
      bus->node=e; found=1; break; }
    RegistryEntryIterateDispose(&c);
    if(!found) return -1;

    if(SsbGetProp(&bus->node,"assigned-addresses",(void**)&aa,&as)!=noErr) return paramErr;
    if(SsbGetProp(&bus->node,"AAPL,address",(void**)&la,&ls)!=noErr){DisposePtr((Ptr)aa);return paramErr;}
    ne=(UInt32)as/(kAddrEntryWords*sizeof(UInt32));
    for(i=0;i<ne&&(i*sizeof(UInt32))<(UInt32)ls;i++){
      UInt32 ph=aa[i*kAddrEntryWords];
      if(AA_REG(ph)==0x10&&AA_SPACE(ph)>=2){
        bus->bar0=(volatile void*)la[i];
        bus->bar0Size=aa[i*kAddrEntryWords+4];
        break;} }
    DisposePtr((Ptr)aa); DisposePtr((Ptr)la);
    if(bus->bar0==0) return paramErr;
    if(((unsigned long)bus->bar0 & 3UL)!=0UL) return paramErr;   /* refuse misaligned MMIO */

    if(ExpMgrConfigReadWord(&bus->node,(LogicalAddress)0x04,&cmd)!=noErr) return paramErr;
    if(!(cmd&0x0002)){
      if(ExpMgrConfigWriteWord(&bus->node,(LogicalAddress)0x04,(UInt16)(cmd|0x0002))!=noErr) return paramErr;
      (void)ExpMgrConfigReadWord(&bus->node,(LogicalAddress)0x04,&back);
      if(!(back&0x0002)) return paramErr; }
    /* 8-78 k184: crystal+PLL up and settled BEFORE anyone reads a core register (the ~30% target-abort fix). */
    SsbPowerOn(bus);
    return noErr;
}

/* Point BAR0's lower 4 KB at core `idx`.
 * ⚠ The read-back loop is not optional: ssb_pci_switch_coreidx writes, reads back, compares and
 * retries, because the write is not reliably immediate on this bridge. Skipping it means reading
 * core N believing it is core M, which yields plausible nonsense rather than an obvious failure. */
static OSStatus SsbSelectCore(SsbBus *bus, UInt32 idx)
{
    UInt32 want=(idx*SSB_CORE_SIZE)+SSB_ENUM_BASE; int a=0;
    for(;;){ UInt32 got=0;
      if(ExpMgrConfigWriteLong(&bus->node,(LogicalAddress)SSB_BAR0_WIN,want)!=noErr) return paramErr;
      if(ExpMgrConfigReadLong(&bus->node,(LogicalAddress)SSB_BAR0_WIN,&got)!=noErr) return paramErr;
      if(got==want) return noErr;
      bus->selectRetries++;
      if(++a>10) return paramErr;
      SsbSpinUs(20); }
}

/* Enumerate the backplane.
 * ⚠⚠ BOUNDED BY ChipCommon's CORE COUNT, ALWAYS. Reading SSB_IDHIGH at a nonexistent core index
 * is not a soft failure -- the window points at unmapped space and the read takes a PowerPC access
 * exception. There is no sentinel to test for, because the load never completes. An "obviously
 * safe" upper bound like idx < 8 is only safe if overrunning it RETURNS AN ERROR rather than
 * faulting, and on this backplane it does not. */
static OSStatus SsbScanCores(SsbBus *bus)
{
    UInt32 idx, idhigh, chipid;
    if(SsbSelectCore(bus,0)!=noErr) return paramErr;
    idhigh=ssb_r32(bus->bar0,SSB_IDHIGH);
    if(IDHIGH_CC(idhigh)!=SSB_DEV_CHIPCOMMON) return paramErr;   /* no count => do not walk */
    bus->idxChipCommon=0;
    chipid=ssb_r32(bus->bar0,CC_CHIPID);
    bus->chipId=CHIPID_ID(chipid);
    bus->chipRev=CHIPID_REV(chipid);
    bus->nCores=CHIPID_NCORES(chipid);
    if(bus->nCores==0||bus->nCores>8) bus->nCores=5;   /* Stage 2 measured 5 on this card */
    for(idx=0;idx<bus->nCores;idx++){
      if(SsbSelectCore(bus,idx)!=noErr) continue;
      idhigh=ssb_r32(bus->bar0,SSB_IDHIGH);
      if(idhigh==0xFFFFFFFFUL||idhigh==0) continue;
      if(IDHIGH_CC(idhigh)==SSB_DEV_80211 && bus->idx80211==kSsbNoCore) bus->idx80211=idx; }
    return (bus->idx80211==kSsbNoCore) ? paramErr : noErr;
}

/* --- ssb_wait_bits ---------------------------------------------------------
 * Verbatim shape from drivers/ssb/main.c: poll up to `timeout` times, 10 us apart, for `bitmask`
 * to become set (set=1) or clear (set=0). Returns 0 on success, -1 on timeout. */
static int SsbWaitBits(SsbBus *bus, UInt32 reg, UInt32 bitmask, int timeout, int set)
{
    int i; UInt32 val;
    for(i=0;i<timeout;i++){
      val=ssb_r32(bus->bar0,reg);
      if(set){ if((val&bitmask)==bitmask) return 0; }
      else   { if(!(val&bitmask))         return 0; }
      SsbSpinUs(10); }
    return -1;
}

/* ssb_tmslow_reject_bitmask: which TMSLOW bit means "reject" depends on the BACKPLANE revision,
 * read from SSB_IDLOW. Getting this wrong means the reject handshake below never completes. */
static UInt32 SsbRejectBitmask(SsbBus *bus)
{
    UInt32 rev = ssb_r32(bus->bar0,SSB_IDLOW) & SSB_IDLOW_SSBREV;
    switch(rev){
      case SSB_IDLOW_SSBREV_22:
      case SSB_IDLOW_SSBREV_24:
      case SSB_IDLOW_SSBREV_26:
        return SSB_TMSLOW_REJECT;
      case SSB_IDLOW_SSBREV_23:
        return SSB_TMSLOW_REJECT_23;
      case SSB_IDLOW_SSBREV_25:   /* ssb: "TODO - find the proper REJECT bit" */
      case SSB_IDLOW_SSBREV_27:   /* ssb: "same here" */
        return SSB_TMSLOW_REJECT; /* ssb calls this a guess; we inherit the guess and say so */
      default:
        break; }
    return (SSB_TMSLOW_REJECT | SSB_TMSLOW_REJECT_23);
}

static void SsbFlushTmslow(SsbBus *bus){(void)ssb_r32(bus->bar0,SSB_TMSLOW);SsbSpinUs(2);}

/* ★★★ ssb_device_disable() -- THE POINT OF THIS HEADER.
 * A faithful port. This is what lets a probe re-run without a cold boot, and it is the code every
 * probe since 3a has refused to implement (correctly, while it was being written from memory).
 * Returns 0 on success; a negative value means a wait timed out and the caller should NOT proceed
 * to re-enable, because the core is in an unknown state. */
static int SsbCoreDisable(SsbBus *bus, UInt32 coreFlags)
{
    UInt32 reject, val;
    int rc = 0;

    if(ssb_r32(bus->bar0,SSB_TMSLOW) & SSB_TMSLOW_RESET)
        return 0;                                   /* already in reset: nothing to do */

    reject = SsbRejectBitmask(bus);

    if(ssb_r32(bus->bar0,SSB_TMSLOW) & SSB_TMSLOW_CLOCK){
        ssb_w32(bus->bar0,SSB_TMSLOW, reject | SSB_TMSLOW_CLOCK);
        if(SsbWaitBits(bus,SSB_TMSLOW,reject,1000,1)!=0)            rc = -1;
        if(SsbWaitBits(bus,SSB_TMSHIGH,SSB_TMSHIGH_BUSY,1000,0)!=0) rc = -2;

        /* The initiator dance. Only cores that are backplane INITIATORS need it, and only
         * rev >= 2.3 backplanes define the REJECT/BUSY bits in IMSTATE at all. */
        if(ssb_r32(bus->bar0,SSB_IDLOW) & SSB_IDLOW_INITIATOR){
            val = ssb_r32(bus->bar0,SSB_IMSTATE);
            val |= SSB_IMSTATE_REJECT;
            ssb_w32(bus->bar0,SSB_IMSTATE,val);
            if(SsbWaitBits(bus,SSB_IMSTATE,SSB_IMSTATE_BUSY,1000,0)!=0) rc = -3;
        }

        ssb_w32(bus->bar0,SSB_TMSLOW,
                SSB_TMSLOW_FGC | SSB_TMSLOW_CLOCK | reject | SSB_TMSLOW_RESET | coreFlags);
        SsbFlushTmslow(bus);

        if(ssb_r32(bus->bar0,SSB_IDLOW) & SSB_IDLOW_INITIATOR){
            val = ssb_r32(bus->bar0,SSB_IMSTATE);
            val &= ~SSB_IMSTATE_REJECT;
            ssb_w32(bus->bar0,SSB_IMSTATE,val);
        }
    }

    ssb_w32(bus->bar0,SSB_TMSLOW, reject | SSB_TMSLOW_RESET | coreFlags);
    SsbFlushTmslow(bus);
    return rc;
}

/* ssb_device_enable(). Calls disable first, exactly as ssb does -- which is now a real teardown
 * rather than a precondition we hoped for. */
static void SsbCoreEnable(SsbBus *bus, UInt32 coreFlags)
{
    UInt32 val;
    (void)SsbCoreDisable(bus, coreFlags);
    ssb_w32(bus->bar0,SSB_TMSLOW,
            SSB_TMSLOW_RESET|SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|coreFlags);
    SsbFlushTmslow(bus);
    if(ssb_r32(bus->bar0,SSB_TMSHIGH)&SSB_TMSHIGH_SERR) ssb_w32(bus->bar0,SSB_TMSHIGH,0);
    val=ssb_r32(bus->bar0,SSB_IMSTATE);
    if(val&(SSB_IMSTATE_IBE|SSB_IMSTATE_TO))
        ssb_w32(bus->bar0,SSB_IMSTATE,val&~(SSB_IMSTATE_IBE|SSB_IMSTATE_TO));
    ssb_w32(bus->bar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|coreFlags);
    SsbFlushTmslow(bus);
    ssb_w32(bus->bar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|coreFlags);
    SsbFlushTmslow(bus);
}

#endif /* SSB_CORE_H */
