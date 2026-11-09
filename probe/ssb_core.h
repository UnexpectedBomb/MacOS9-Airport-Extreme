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
#define kDevBCM4318       0x4318UL   /* BCM4318 single-chip 802.11g (PCI 14e4:4318) */

/* b235: the BCM4318 is now a FULLY SUPPORTED chip (14e4:4318, single-chip 802.11g), not a diagnostic
 * probe -- its rev-8 radio init, OFDM/ExtG init and full 802.11g throughput are HW-proven on the FW400.
 * SsbFindAndMap binds, and ap_shim accepts, BOTH the BCM4306 (4320/4325) and the BCM4318 (4318); the old
 * AP_DIAG_ANYCHIP flag (bind a 4318 only to read its revs, then abort) is retired. */

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
    /* k240: SsbAcquire's record -- the card's PCI config as found, BEFORE anything was written */
    UInt32          acqKind;       /* kSsbKind*: where the card sits, which decides what may be written */
    UInt32          acqVidDid;     /* config 0x00 as read: device << 16 | vendor */
    UInt32          acqCmdSts;     /* config 0x04 as read: status << 16 | command */
    UInt32          acqLatCls;     /* config 0x0C as read: BIST | header type | latency timer | cache line */
    UInt32          acqBar0;       /* config 0x10 as read */
    UInt32          acqBar0Want;   /* the bus address the Name Registry's assigned-addresses gives BAR0 */
    UInt32          acqPm;         /* PMCSR's config offset << 16 | PMCSR as read; 0 = no PM capability */
    UInt32          acqFixed;      /* kSsbFix*: what this acquisition wrote back */
    UInt32          acqRefused;    /* kSsbAcq*: 0 = acquired, else why the card was left alone */
    UInt32          acqBridgeBus;  /* CardBus: the bridge's config 0x18 (latency|subordinate|CardBus bus|PCI bus) */
    UInt32          acqBridgeCmd;  /* CardBus: the bridge's config 0x04 (status << 16 | command) */
    UInt32          restoreLatCls; /* IN: config 0x0C as the firmware left it at this boot's first acquisition */
    UInt32          restoreLatClsOk; /* IN: 1 = restoreLatCls is known */
    /* k241: the card's ancestors as SsbKindOf walked them, nearest first (name, device_type; "" = absent) */
    UInt32          acqChainN;            /* levels recorded, 0..kSsbChainMax */
    UInt32          acqBridgeLevel;       /* CardBus: the level the "cardbus" bridge was found at */
    char            acqChainName[4][24];
    char            acqChainType[4][16];
} SsbBus;
#define kSsbChainMax 4

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

/* ★★★ k240: ACQUIRE THE CARD -- READ ITS PCI CONFIG BEFORE THE FIRST ACCESS THROUGH THE BAR.
 *
 * WHY. 1.0b239d took down the Finder on the FW800 MDD the first time Open Transport loaded the driver after
 * a deep sleep (Run 20 Part B, 2026-10-26), before the load wrote a single log line. What is known: TCP/IP
 * unloads itself at every sleep (OTInetControllerLib's kOTSystemSleep handler, unless wake-on-network is set
 * -- read from the binary) and took this module with it (the panel's "not in use" after the wake is
 * alive == 0, which only ApLinkStop writes); the next load's first card access is the SPROM MAC read, before
 * any log line; and SsbFindAndMap took BAR0's address from the Name Registry without ever asking the card.
 * What is INFERRED, not measured: that the sleep cut the slot's power and left the card's config at power-on
 * defaults, so that read went to an address the card no longer decodes. The FW800 companion met exactly that
 * on the NEC ("wake1b went dark reading MMIO with a reset BAR0") and restores its config before any MMIO,
 * which is what OS X's IOPCIFamily does for every device at wake (pci_restore_state, in Linux's terms). The
 * record kept below is the measurement.
 *
 * WHAT. Read what config space holds, keep every value for the log, then act by where the card sits:
 *   PCI (the A1026 AirPort slot, or a PCI card): the registry's assigned-addresses is the truth. Put the
 *     card in D0, BAR0 back (memory decode off while it moves), and the firmware's latency timer and cache
 *     line size if this boot saw them -- in that order, as a PCI driver must.
 *   CardBus: Card Services owns the card's config and re-enumerates the socket at wake. Never write it. A
 *     bridge with no CardBus bus, a card not in D0, or a BAR0 that is not the registry's means Card Services
 *     has not finished -- refuse, touch nothing, and let the caller ask again.
 *   A card whose ancestors we cannot place is treated as CardBus: never written.
 * Config space only (non-faulting); task level (Name Registry iteration). */
#define kSsbKindPci        0UL   /* direct parent's device_type "pci": the A1026 AirPort slot, or a PCI card */
#define kSsbKindCardBus    1UL   /* a "cardbus" ancestor at any level (k241; measured: ALBOOK17-CARDBUS.md) */
#define kSsbKindUnknown    2UL   /* neither: never written */

#define kSsbAcqOk          0UL
#define kSsbAcqCfgFail     1UL   /* a config-space access itself failed */
#define kSsbAcqNoAnswer    2UL   /* config 0x00 holds no Broadcom ID: the card is unpowered, or not there yet */
#define kSsbAcqBridgeBad   3UL   /* CardBus: the bridge has no CardBus bus, or is not decoding and mastering */
#define kSsbAcqNotOurs     4UL   /* CardBus/unknown: not in D0, or BAR0 not the registry's -- not ours to fix */
#define kSsbAcqPmStuck     5UL   /* PCI: PMCSR would not go to D0 */
#define kSsbAcqBar0Stuck   6UL   /* PCI: BAR0 would not take the registry's address */

#define kSsbFixD0          0x01UL  /* PMCSR was D1-D3: wrote D0, waited 10 ms */
#define kSsbFixBar0        0x02UL  /* BAR0 was not the assigned address: wrote it back */
#define kSsbFixLatCls      0x04UL  /* latency timer / cache line size put back to the firmware's */

#define kSsbErrRefused     (-2)    /* SsbFindAndMap: SsbAcquire left the card alone; bus->acqRefused says why */

static int SsbStrIs(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void SsbCopyStr(char *d, const char *s, UInt32 cap)
{
    UInt32 i;
    for (i = 0; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* One string property into buf, always terminated: "" if absent, "(long)" if it does not fit. */
static void SsbPropStr(RegEntryID *n, const char *prop, char *buf, RegPropertyValueSize cap)
{
    RegPropertyValueSize sz = 0;
    buf[0] = 0;
    if (RegistryPropertyGetSize(n, prop, &sz) != noErr || sz == 0) return;
    if (sz > cap - 1) { SsbCopyStr(buf, "(long)", (UInt32)cap); return; }
    if (RegistryPropertyGet(n, prop, buf, &sz) == noErr && sz < cap) buf[sz] = 0;
    else buf[0] = 0;
}

/* ★ k241: WHERE THE CARD SITS, FROM ITS ANCESTORS -- NOT JUST ITS PARENT.
 * Run 21 (TiBook, Linksys WPC54G): k240 read only the card's parent and logged "parent not named pci or
 * cardbus", because Card Services publishes a CardBus card as
 *     cardbus (the bridge)  >  pccard-<vendor>,<device> (NO device_type)  >  the card's function
 * -- the layout CardBusCompat measured on the same TiBook (MacOS9-AlBook-CardBus logs/tibook 1.5: level 0
 * pccard-pci1033,35, level 1 cardbus, level 2 pci, level 3 device-tree, level 4 Devices). So walk up, bounded,
 * the way CardBusCompat's entry_parent does on that machine: set the iterator to the node, ask kRegIterParents
 * once, repeat from the parent ([[reference_os9_nameregistry_iterate]]: one call, one parent).
 *   a "cardbus" ancestor at ANY level = CardBus, and that ancestor is the bridge SsbAcquire asks;
 *   otherwise a direct parent of device_type "pci" = PCI (the A1026 slot, or a PCI card);
 *   otherwise unknown -- never written.
 * The nearest kSsbChainMax levels (name, device_type) are kept for the log. Task level. */
static UInt32 SsbKindOf(SsbBus *bus, RegEntryID *bridge, int *haveBridge)
{
    RegEntryID cur, par; RegEntryIter it; Boolean done;
    UInt32 level, kind = kSsbKindUnknown;
    int parentIsPci = 0, got;
    char dt[16];
    *haveBridge = 0; bus->acqChainN = 0; bus->acqBridgeLevel = 0;
    RegistryEntryIDInit(&cur);
    if (RegistryEntryIDCopy(&bus->node, &cur) != noErr) return kind;
    for (level = 0; level < 8; level++) {
        got = 0; done = false;
        RegistryEntryIDInit(&par);
        if (RegistryEntryIterateCreate(&it) == noErr) {
            if (RegistryEntryIterateSet(&it, &cur) == noErr &&
                RegistryEntryIterate(&it, kRegIterParents, &par, &done) == noErr && !done) got = 1;
            RegistryEntryIterateDispose(&it);
        }
        if (!got) { RegistryEntryIDDispose(&par); break; }
        SsbPropStr(&par, "device_type", dt, (RegPropertyValueSize)sizeof(dt));
        if (level < kSsbChainMax) {
            SsbPropStr(&par, "name", bus->acqChainName[level], (RegPropertyValueSize)sizeof(bus->acqChainName[level]));
            SsbCopyStr(bus->acqChainType[level], dt, (UInt32)sizeof(bus->acqChainType[level]));
            bus->acqChainN = level + 1;
        }
        if (level == 0 && SsbStrIs(dt, "pci")) parentIsPci = 1;
        if (SsbStrIs(dt, "cardbus")) {
            if (RegistryEntryIDCopy(&par, bridge) == noErr) *haveBridge = 1;
            kind = kSsbKindCardBus; bus->acqBridgeLevel = level;
            RegistryEntryIDDispose(&par);
            break;
        }
        RegistryEntryIDDispose(&cur);              /* step up: the parent becomes the node asked next */
        RegistryEntryIDInit(&cur);
        got = (RegistryEntryIDCopy(&par, &cur) == noErr);
        RegistryEntryIDDispose(&par);
        if (!got) break;
    }
    RegistryEntryIDDispose(&cur);
    if (kind == kSsbKindUnknown && parentIsPci) kind = kSsbKindPci;
    return kind;
}

static OSStatus SsbAcquire(SsbBus *bus, UInt32 want)
{
    RegEntryID bridge; int haveBridge = 0, guard = 0;
    UInt32 v = 0; UInt16 pm = 0, cmd; UInt8 cap = 0, id = 0;
    unsigned short pmOff = 0;

    bus->acqVidDid = bus->acqCmdSts = bus->acqLatCls = bus->acqBar0 = bus->acqPm = 0;
    bus->acqFixed = 0; bus->acqRefused = kSsbAcqOk; bus->acqBar0Want = want;
    bus->acqBridgeBus = bus->acqBridgeCmd = 0;
    bus->acqKind = SsbKindOf(bus, &bridge, &haveBridge);
    if (haveBridge) {
        /* The bridge first (only a CardBus walk returns one): it sits on the host bus, so asking it is always
         * safe, and a bridge with CardBus bus 0 -- or not decoding (which also stops it forwarding to the card's
         * memory window) and mastering -- has not been set up by Card Services. The TiBook's PCI1410 reads
         * command 0x0007, buses 0/1/4 (CardBusCompat 2.5-2.9 -- through RAW config cycles, not this call).
         * ⚠ So a bridge the Expansion Manager will not read is NOT a refusal: k240 never asked it and acquired
         *   the TiBook's card fine. Marked unreadable (all-ones) for the log; the card's own checks decide. Only
         *   a bridge positively read as not set up refuses. */
        if (bus->acqKind == kSsbKindCardBus) {
            if (ExpMgrConfigReadLong(&bridge, (LogicalAddress)0x18, &bus->acqBridgeBus) != noErr ||
                ExpMgrConfigReadLong(&bridge, (LogicalAddress)0x04, &bus->acqBridgeCmd) != noErr)
                bus->acqBridgeBus = bus->acqBridgeCmd = 0xFFFFFFFFUL;
            else if (bus->acqBridgeCmd != 0xFFFFFFFFUL &&
                     (((bus->acqBridgeBus >> 8) & 0xFFUL) == 0 || (bus->acqBridgeCmd & 0x0006UL) != 0x0006UL))
                bus->acqRefused = kSsbAcqBridgeBad;
        }
        RegistryEntryIDDispose(&bridge);
        if (bus->acqRefused != kSsbAcqOk) return kSsbErrRefused;
    }
    if (ExpMgrConfigReadLong(&bus->node, (LogicalAddress)0x00, &bus->acqVidDid) != noErr ||
        ExpMgrConfigReadLong(&bus->node, (LogicalAddress)0x04, &bus->acqCmdSts) != noErr ||
        ExpMgrConfigReadLong(&bus->node, (LogicalAddress)0x0C, &bus->acqLatCls) != noErr ||
        ExpMgrConfigReadLong(&bus->node, (LogicalAddress)0x10, &bus->acqBar0)   != noErr) {
        bus->acqRefused = kSsbAcqCfgFail; return kSsbErrRefused; }
    if ((bus->acqVidDid & 0xFFFFUL) != kVendorBroadcom) { bus->acqRefused = kSsbAcqNoAnswer; return kSsbErrRefused; }
    if (bus->acqCmdSts & 0x00100000UL) {                /* status bit 4: there is a capability list */
        if (ExpMgrConfigReadByte(&bus->node, (LogicalAddress)0x34, &cap) == noErr) {
            cap &= 0xFC;
            while (cap >= 0x40 && guard++ < 48) {
                UInt8 nxt = 0;
                if (ExpMgrConfigReadByte(&bus->node, (LogicalAddress)(unsigned long)cap, &id) != noErr) break;
                if (ExpMgrConfigReadByte(&bus->node, (LogicalAddress)(unsigned long)(cap + 1), &nxt) != noErr) break;
                if (id == 0x01) { pmOff = (unsigned short)(cap + 4); break; }   /* PCI Power Management */
                cap = (UInt8)(nxt & 0xFC);
            }
        }
        if (pmOff && ExpMgrConfigReadWord(&bus->node, (LogicalAddress)(unsigned long)pmOff, &pm) == noErr)
            bus->acqPm = ((UInt32)pmOff << 16) | pm;
        else pmOff = 0;
    }
    if (bus->acqKind != kSsbKindPci) {
        if ((pmOff && (pm & 0x0003)) || (bus->acqBar0 & 0xFFFFFFF0UL) != (want & 0xFFFFFFF0UL)) {
            bus->acqRefused = kSsbAcqNotOurs; return kSsbErrRefused; }
        return noErr;
    }
    if (pmOff && (pm & 0x0003)) {                       /* D0 first: a function in D3 need not keep BAR writes */
        (void)ExpMgrConfigWriteWord(&bus->node, (LogicalAddress)(unsigned long)pmOff, (UInt16)(pm & 0x7FFCU));
        SsbSpinUs(10000);                               /* PCI PM 1.1: 10 ms from D3hot to D0 */
        pm = 0xFFFF;
        if (ExpMgrConfigReadWord(&bus->node, (LogicalAddress)(unsigned long)pmOff, &pm) != noErr || (pm & 0x0003)) {
            bus->acqRefused = kSsbAcqPmStuck; return kSsbErrRefused; }
        bus->acqFixed |= kSsbFixD0;
    }
    if ((bus->acqBar0 & 0xFFFFFFF0UL) != (want & 0xFFFFFFF0UL)) {
        cmd = (UInt16)(bus->acqCmdSts & 0xFFFFUL);
        if (cmd & 0x0002)                               /* stop decoding while the BAR moves */
            (void)ExpMgrConfigWriteWord(&bus->node, (LogicalAddress)0x04, (UInt16)(cmd & ~0x0002U));
        if (ExpMgrConfigWriteLong(&bus->node, (LogicalAddress)0x10, want) != noErr ||
            ExpMgrConfigReadLong(&bus->node, (LogicalAddress)0x10, &v) != noErr ||
            (v & 0xFFFFFFF0UL) != (want & 0xFFFFFFF0UL)) {
            bus->acqRefused = kSsbAcqBar0Stuck; return kSsbErrRefused; }
        bus->acqFixed |= kSsbFixBar0;
        /* Only after a power cut: the firmware's latency timer + cache line size, as this boot first found
         * them (the reset value of both is 0). One 16-bit write covers both bytes at 0x0C. */
        if (bus->restoreLatClsOk && ((bus->acqLatCls ^ bus->restoreLatCls) & 0xFFFFUL)) {
            (void)ExpMgrConfigWriteWord(&bus->node, (LogicalAddress)0x0C, (UInt16)(bus->restoreLatCls & 0xFFFFUL));
            bus->acqFixed |= kSsbFixLatCls;
        }
    }
    return noErr;
}

static OSStatus SsbFindAndMap(SsbBus *bus)
{
    RegEntryIter c; RegEntryID e; Boolean d=false,f=true; OSStatus er;
    UInt32 *aa=NULL,*la=NULL; RegPropertyValueSize as=0,ls=0; UInt32 i,ne;
    UInt16 cmd=0, back=0;
    UInt32 bar0Bus=0;
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
      did&=0xFFFFUL;
      if(did!=kDevBCM4306_4320 && did!=kDevBCM4306_4325 && did!=kDevBCM4318) continue;
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
        bar0Bus=aa[i*kAddrEntryWords+2];          /* k240: phys.lo -- the bus address BAR0 must hold */
        break;} }
    DisposePtr((Ptr)aa); DisposePtr((Ptr)la);
    if(bus->bar0==0) return paramErr;
    if(((unsigned long)bus->bar0 & 3UL)!=0UL) return paramErr;   /* refuse misaligned MMIO */
    /* k240: before anything goes through the BAR -- see SsbAcquire. kSsbErrRefused: nothing was touched. */
    er=SsbAcquire(bus,bar0Bus); if(er!=noErr){ bus->bar0=0; return er; }   /* unmapped: every bar0 guard holds */

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
