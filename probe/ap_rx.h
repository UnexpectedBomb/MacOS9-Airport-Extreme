/* ap_rx.h -- the RECEIVE layer: core_init's tail, MAC control, channel, filters, RX scan.
 *
 * ★ THE FIFTH EXTRACTION, and the one that changes who owns the card.
 * Everything before this could be run by the driver and then handed back: configure, verify,
 * stop. A RECEIVER cannot. It has to stay armed with the MAC enabled, which means the driver and
 * the probe application can no longer both drive the radio in one run. The app becomes a pure
 * reporter from 8-2g onward, and this header is what makes that possible -- the code the app
 * stops running does not disappear, it moves.
 *
 * ⚠⚠ MacSuspend/MacEnable ARE A REFCOUNT, NOT A TOGGLE. gMacSuspended starts at 1 because
 *   b43's setup_struct_wldev_for_init sets it so, which is why the FIRST mac_enable is what
 *   actually turns the receiver on. That global came across with the DMA layer in 8-2f and was
 *   unused there; it is used here. Calling MacEnable twice without a MacSuspend between does not
 *   double-enable -- it decrements a count that is already zero.
 *
 * ⚠ TxProbeHere DID NOT COME, AGAIN. It sits immediately above MacFilterSet in the original and
 *   is a probe diagnostic that TRANSMITS. It stayed in airport_rx.c for the same reason it stayed
 *   out of ap_chipinit_tail.h, and check-exec-level.py lists it as a forbidden primitive so its
 *   absence from the driver is asserted rather than assumed.
 *
 * ★ HOW IT WAS MADE. Three spans lifted VERBATIM by line range, never retyped, proven by
 * reconstruction against the original file. Same as the four extractions before it.
 */
#ifndef AP_RX_H
#define AP_RX_H

#include "ap_dma.h"   /* the ring, the descriptors, the barrier, and gMacSuspended */

/* ⚠ AND THE CONSTANTS CAME TOO, for the second time in two extractions. The compiler found
 * them the same way it found the DMA ones: implicit declarations that became a link error.
 * B43_MMIO_IFSCTL, the hostflag bits, the MAC filter registers and the SPROM offsets all
 * lived in airport_rx.c. Leaving them would have meant a second set of definitions for the
 * same registers -- the duplication 8-2d-2a removed from this project once already, and
 * which cost 8-2d-3 six missing prefix steps when it was allowed to persist elsewhere. */

/* ---- core_init tail + mac enable ---- */
#define B43_MMIO_MACFILTER_CONTROL  0x420UL
#define B43_MMIO_MACFILTER_DATA     0x422UL
#define B43_MMIO_RCMTA_COUNT        0x43CUL
#define B43_MMIO_IFSCTL             0x688UL
#define   B43_MMIO_IFSCTL_USE_EDCF  0x0004
#define B43_MACFILTER_SELF          0x0000UL
#define B43_MACFILTER_BSSID         0x0003UL
#define B43_SHM_SH_SPUWKUP          0x0094UL
#define B43_SHM_SH_KTP              0x0056UL
#define B43_SHM_SH_KEYIDXBLOCK      0x05D4UL
#define B43_SHM_SH_UCODESTAT        0x0040UL
#define   B43_SHM_SH_UCODESTAT_SLEEP 2
#define B43_SHM_RCMTA               4UL          /* enum: UCODE 0, SHARED 1, SCRATCH 2, HW 3 */
#define B43_NR_GROUP_KEYS           4
#define B43_NR_PAIRWISE_KEYS        50
#define B43_SEC_KEYSIZE             16
#define B43_SEC_ALGO_NONE           0
#define B43_HF_EDCF_LO              0x0100      /* B43_HF_EDCF is bit 8 of the LOW hostflags word */
#define B43_HF_BTCOEX_LO            0x0010
#define B43_BFL_BTCOEXIST           0x0001
#define B43_BFL_BTCMOD              0x4000
#define B43_IRQ_MAC_SUSPENDED       0x00000001UL
#define B43_MACCTL_ENABLED          0x00000001UL
#define B43_MACCTL_HWPS             0x02000000UL
#define B43_MACCTL_AWAKE            0x04000000UL
/* b43.h:462. b43_radio_lock sets this, flush-reads MACCTL and udelay(10)s, so the firmware
 * cannot be part-way through a radio register access while we write one. k23 skipped it and
 * the card stopped transmitting for the rest of the run. */
#define B43_MACCTL_RADIOLOCK        0x00080000UL
/* B43_MACCTL_BE, B43_MMIO_RAM_CONTROL, B43_MMIO_RAM_DATA and RamWrite() all come from
 * ap_phy_initg.h now -- they were extracted verbatim from the j7 probe that proved them, so
 * redefining them here would be a second copy of exactly the kind this project keeps deleting. */
/* ⚠ SPROM IS AN MMIO WINDOW AT BAR0 + 0x1000, NOT SHARED MEMORY. The first draft of this probe
 * read board flags and the MAC with ShmRead16Shared and would have fed garbage to the MAC
 * filter. ap_bringup.h's GpioInit reads SPROM the right way and is the witness:
 *     boardflags = ssb_r16(gBus.bar0, SSB_SPROM_BASE1 + SPROM_BFLLO); */
#define SSB_SPROM1_IL0MAC           0x0048UL   /* 6 bytes, the 802.11b/g MAC */

/* b43_switch_channel's cookie. The 5 GHz and 40 MHz arms are not taken: this is a 2.4 GHz-only
 * radio and b43 guards the 40 MHz bit with a literal `if (0)`. So the cookie is the channel. */
#define B43_SHM_SH_CHAN             0x00A0UL

/* ============================================================================
 * b43_wireless_core_init's tail -- the five statements after b43_dma_init.
 * ==========================================================================*/

/* [1] b43_qos_init. qos_enabled is a DRIVER setting and we do not use QoS, so the disable arm
 *     is the one that runs. Two operations, both read-modify-write. */
static void QosInitDisabled(void)
{
    UInt16 lo,mi,hi,ifs;
    HfRead(&lo,&mi,&hi);
    lo = (UInt16)(lo & ~B43_HF_EDCF_LO);
    HfWrite(lo,mi,hi);
    ifs = ssb_r16(gBus.bar0,B43_MMIO_IFSCTL);
    ifs = (UInt16)(ifs & ~B43_MMIO_IFSCTL_USE_EDCF);
    ssb_w16(gBus.bar0,B43_MMIO_IFSCTL,ifs);
}

/* [2] b43_set_synth_pu_delay(dev, idle=1). idle -> 500. The radio_rev == 8 arm raises it to
 *     2400, and this card is rev 2, so that arm is unreachable and is not ported. */
static UInt16 SetSynthPuDelay(void)
{
    UInt16 pu_delay = 500;
    ShmWrite16Shared(B43_SHM_SH_SPUWKUP,pu_delay);
    return pu_delay;
}

/* [3] b43_bluetooth_coext_enable. Gated on boardflags & B43_BFL_BTCOEXIST. Returns whether it
 *     did anything, so the log can say which arm ran instead of implying one. */
static int BluetoothCoextEnable(UInt32 boardflags)
{
    UInt16 lo,mi,hi;
    if(!(boardflags & B43_BFL_BTCOEXIST)) return 0;
    HfRead(&lo,&mi,&hi);
    /* ⚠ B43_HF_BTCOEXALT is 0x000001000000ULL = BIT 24. HOSTF1 holds bits 0-15 and HOSTF2 holds
     * bits 16-31, so bit 24 is bit 8 of the MIDDLE word -- not the high one. The first draft put
     * it in `hi`. Unreachable on this card (boardflags 0x000A has no BTCOEXIST) but wrong. */
    if(boardflags & B43_BFL_BTCMOD) mi = (UInt16)(mi | 0x0100);   /* HF_BTCOEXALT, bit 24 */
    else                            lo = (UInt16)(lo | B43_HF_BTCOEX_LO);
    HfWrite(lo,mi,hi);
    return 1;
}

/* ★ 8-2e: b43_chip_init statements 8-13 moved out so the DRIVER can call them too.
 * Verbatim move, proven by reconstruction. TxProbeHere stayed -- see the header. */
#include "ap_chipinit_tail.h"

/* b43_macfilter_set */
static void MacFilterSet(UInt16 offset,const UInt8 *mac)
{
    static const UInt8 zero[6]={0,0,0,0,0,0};
    UInt16 data;
    if(!mac) mac = zero;
    offset = (UInt16)(offset | 0x0020);
    ssb_w16(gBus.bar0,B43_MMIO_MACFILTER_CONTROL,offset);
    data=(UInt16)(mac[0]|((UInt32)mac[1]<<8)); ssb_w16(gBus.bar0,B43_MMIO_MACFILTER_DATA,data);
    data=(UInt16)(mac[2]|((UInt32)mac[3]<<8)); ssb_w16(gBus.bar0,B43_MMIO_MACFILTER_DATA,data);
    data=(UInt16)(mac[4]|((UInt32)mac[5]<<8)); ssb_w16(gBus.bar0,B43_MMIO_MACFILTER_DATA,data);
}

/* b43_write_mac_bssid_templates (main.c:679).
 *
 * ★★★ b43 CALLS THIS TWICE, AND THE SECOND CALL IS THE ONE THIS PROBE WENT FORTY-THREE
 *     INCREMENTS WITHOUT. b43_op_bss_info_changed (main.c:4140) re-runs it on
 *     BSS_CHANGED_BSSID -- that is, the moment mac80211 learns the BSSID from association --
 *     and it does so with the MAC still RUNNING, before its own b43_mac_suspend three lines
 *     later. So no suspend is needed here either.
 *
 * ⚠ THE CONSEQUENCE OF MISSING IT IS SELECTIVE, WHICH IS WHY IT SURVIVED SO LONG. With the
 *   filter left at zeros the card still delivers:
 *      - anything addressed to us, because MACFILTER_SELF matches addr1;
 *      - beacons, because MACCTL's BEACPROMISC bit deliberately bypasses the BSSID match.
 *   Auth, assoc, probe responses and all four EAPOL-Key frames are unicast to us, so the
 *   whole of Stages 5, 6 and 7-1..7-4 passes with a zero filter. GROUP-ADDRESSED DATA is the
 *   one class that needs the match, and it is exactly the class 7-5 needs to decrypt.
 *   This probe's own header, lines 44-48, worked the same mechanism out for beacons in k1. */
static void WriteMacBssidTemplates(const UInt8 *mac,const UInt8 *bssid)
{
    UInt8 mac_bssid[12];
    int i;
    MacFilterSet((UInt16)B43_MACFILTER_BSSID,bssid);
    for(i=0;i<6;i++) mac_bssid[i]=mac[i];
    for(i=0;i<6;i++) mac_bssid[6+i]=bssid[i];
    for(i=0;i<12;i+=4){
      UInt32 tmp = (UInt32)mac_bssid[i+0];
      tmp |= ((UInt32)mac_bssid[i+1])<<8;
      tmp |= ((UInt32)mac_bssid[i+2])<<16;
      tmp |= ((UInt32)mac_bssid[i+3])<<24;
      RamWrite((UInt32)(0x20+i),tmp); }
}

/* [4] b43_upload_card_macaddress = b43_write_mac_bssid_templates + macfilter_set(SELF).
 *     At this point in b43, wl->bssid is all zeros -- nothing is associated yet -- so the
 *     BSSID written here is zeros, deliberately, not a placeholder we forgot to fill.
 *     The real one goes in after association; see WriteMacBssidTemplates above. */
static void UploadCardMacAddress(const UInt8 *mac)
{
    static const UInt8 zeroBssid[6]={0,0,0,0,0,0};
    WriteMacBssidTemplates(mac,zeroBssid);
    MacFilterSet((UInt16)B43_MACFILTER_SELF,mac);
}

/* b43_kidx_to_fw, old API arm -- fw.rev 295 < 351 */
static UInt8 KidxToFw(UInt8 raw){ return (UInt8)((raw>=4)?(raw-4):raw); }

/* key_write(index, ALGO_NONE, zeros) */
static void KeyWriteCleared(UInt8 index)
{
    UInt8 kidx = KidxToFw(index);
    UInt16 value = (UInt16)(((UInt32)kidx<<4) | B43_SEC_ALGO_NONE);
    UInt32 offset; int i;
    ShmWrite16Shared((UInt32)(B43_SHM_SH_KEYIDXBLOCK + (UInt32)kidx*2),value);
    offset = gKtp + (UInt32)index*B43_SEC_KEYSIZE;
    for(i=0;i<B43_SEC_KEYSIZE;i+=2) ShmWrite16Shared(offset+(UInt32)i,0);
}

/* keymac_write(index, NULL) -- zero the RCMTA slot */
static void KeyMacWriteZero(UInt8 index,UInt8 pairwiseStart)
{
    UInt32 slot;
    if(index < pairwiseStart) return;
    slot = (UInt32)(index - pairwiseStart);
    if(slot >= B43_NR_PAIRWISE_KEYS) return;
    ShmControl(B43_SHM_RCMTA,(slot*2)+0); ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,0);
    ShmControl(B43_SHM_RCMTA,(slot*2)+1); ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,0);
}

/* [5] b43_security_init. rx_tkip_phase1_write is NOT called: it opens with
 *     `if (!modparam_hwtkip) return;` and modparam_hwtkip defaults to 0. */
static void SecurityInit(UInt32 *ktpOut,int *nKeysOut)
{
    UInt8 pairwiseStart = (UInt8)(B43_NR_GROUP_KEYS*2);   /* old kidx API: 8 */
    int count = B43_NR_GROUP_KEYS*2 + B43_NR_PAIRWISE_KEYS;  /* 58 */
    int i;
    gKtp = (UInt32)ShmRead16Shared(B43_SHM_SH_KTP);
    gKtp *= 2;                                            /* KTP is a WORD address */
    *ktpOut = gKtp;
    ssb_w16(gBus.bar0,B43_MMIO_RCMTA_COUNT,(UInt16)B43_NR_PAIRWISE_KEYS);
    for(i=0;i<count;i++){
      UInt8 idx=(UInt8)i;
      if(idx>=pairwiseStart) KeyMacWriteZero(idx,pairwiseStart);
      KeyWriteCleared(idx);
      if(idx>=pairwiseStart) KeyMacWriteZero(idx,pairwiseStart);
      /* b43_key_clear also clears index+4 for index<=3 when the kidx API is OLD */
      if(idx<=3) KeyWriteCleared((UInt8)(idx+4)); }
    *nKeysOut = count;
}

/* ============================================================================
 * b43_power_saving_ctl_bits / b43_mac_enable / b43_mac_suspend
 * ==========================================================================*/
static void PowerSavingCtlBits(void)
{
    /* b43 has `/_ FIXME: For now we force awake-on and hwps-off _/` immediately before using
     * these, which overrides every branch above it. So the whole computation collapses to two
     * constants, and porting the dead branches would be porting code b43 itself cannot reach. */
    UInt32 macctl; int i;
    macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    macctl &= ~B43_MACCTL_HWPS;        /* hwps = false */
    macctl |=  B43_MACCTL_AWAKE;       /* awake = true */
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);
    (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    for(i=0;i<100;i++){
      UInt16 uc = ShmRead16Shared(B43_SHM_SH_UCODESTAT);
      if(uc != B43_SHM_SH_UCODESTAT_SLEEP) break;
      SsbSpinUs(10); }
}

static void MacEnable(void)
{
    gMacSuspended--;
    if(gMacSuspended == 0){
      UInt32 macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL) | B43_MACCTL_ENABLED;
      ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);
      ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,B43_IRQ_MAC_SUSPENDED);
      (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
      (void)ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
      PowerSavingCtlBits(); }
}

static int MacSuspend(void)
{
    int i,ok=0; UInt32 tmp,macctl;
    if(gMacSuspended == 0){
      PowerSavingCtlBits();
      macctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL) & ~B43_MACCTL_ENABLED;
      ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);
      (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
      for(i=35;i;i--){
        tmp = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
        if(tmp & B43_IRQ_MAC_SUSPENDED){ ok=1; goto out; }
        SsbSpinUs(10); }
      for(i=40;i;i--){
        tmp = ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
        if(tmp & B43_IRQ_MAC_SUSPENDED){ ok=1; goto out; }
        SsbSpinUs(1000); }
    } else ok=1;
out:
    gMacSuspended++;
    return ok;
}

/* ============================================================================
 * b43_switch_channel (phy_common.c) -- the statement b43_phy_init ENDS with, and
 * that no probe in this project has ever run. GphyChannelSwitch alone is only
 * ops->switch_channel, the middle third of it.
 * ==========================================================================*/
static void SwitchChannel(UInt8 ch)
{
    UInt16 cookie = (UInt16)ch;
    /* The 5 GHz and 40 MHz arms are not taken: this is a 2.4 GHz-only radio and b43 guards the
     * 40 MHz bit with a literal `if (0)`. So the cookie is just the channel number.
     * b43 also reads the old cookie so it can restore it if the PHY switch fails. Ours cannot
     * fail -- b43_gphy_op_switch_channel's only error is a 1..14 range check that every caller
     * here satisfies -- so there is nothing to restore and that path is not ported. */
    ShmWrite16Shared(B43_SHM_SH_CHAN,cookie);
    GphyChannelSwitch(ch,0);
    SsbSpinUs(8000);            /* msleep(8): "wait for the radio to tune ... and stabilize" */
}

/* The RX filter, as a ladder. Every level clears the whole filter field first, so the levels are
 * independent rather than cumulative by accident.
 * ⚠ Level 0 is what b43_adjust_opmode leaves for a STA with filter_flags = 0. On a core_rev <= 4
 * part adjust_opmode would also force PROMISC; this card is rev 5, so level 0 really is the
 * restrictive case and the ladder has somewhere to climb from. */
typedef struct { const char *name; UInt32 bits; } FilterLevel;
static const FilterLevel kFilters[3] = {
  { "0: as b43 leaves it, filter_flags = 0",         0UL },
  { "1: + BEACPROMISC, what mac80211 sets to scan",  B43_MACCTL_BEACPROMISC },
  { "2: + PROMISC | KEEP_BAD | KEEP_BADPLCP",
      B43_MACCTL_BEACPROMISC|B43_MACCTL_PROMISC|B43_MACCTL_KEEP_BAD|B43_MACCTL_KEEP_BADPLCP }
};

static UInt32 ApplyFilter(UInt32 bits)
{
    UInt32 ctl = ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    ctl &= ~(B43_MACCTL_PROMISC|B43_MACCTL_BEACPROMISC|B43_MACCTL_KEEP_BAD|
             B43_MACCTL_KEEP_BADPLCP|B43_MACCTL_KEEP_CTL);
    ctl |= bits;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,ctl);
    (void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    return ctl;
}

/* ★ DECODE RXSTATUS, ALWAYS. k20 logged this register four times in the transmit section as
 * raw hex -- 0x05802058 every time -- and both of the things it was saying went unread: the
 * pointer never moved, and the state field was 0x2000, IDLEWAIT, not 0x1000, ACTIVE. */
static void SayRxStatus(const char *what,UInt32 rxs)
{
    UInt32 st = rxs & B43_DMA32_RXSTATE;
    UInt32 er = rxs & B43_DMA32_RXERROR;
    Str255 L; L[0]=0;
    PCat(L,what); PCatHex(L,rxs,8);
    PCat(L,"  slot "); PCatDec(L,(unsigned long)((rxs & B43_DMA32_RXDPTR)/B43_DMADESC32_BYTES));
    PCat(L,"  ");
    PCat(L, st==B43_DMA32_RXSTAT_DISABLED ? "DISABLED" :
            st==B43_DMA32_RXSTAT_ACTIVE   ? "ACTIVE" :
            st==B43_DMA32_RXSTAT_IDLEWAIT ? "IDLEWAIT  <- ring exhausted, engine parked" :
            st==B43_DMA32_RXSTAT_STOPPED  ? "STOPPED" : "state?");
    if(er){ PCat(L,"  ERROR ");
      PCat(L, er==B43_DMA32_RXERR_PROT     ? "PROT" :
              er==B43_DMA32_RXERR_UNDERRUN ? "UNDERRUN" :
              er==B43_DMA32_RXERR_DESCREAD ? "DESCREAD" :
              er==B43_DMA32_RXERR_CORE     ? "CORE" : "?"); }
    Out(L);
}

/* count buffers whose poison is gone; remember the first one */
static int RxScan(UInt8 **bufLog,UInt32 frameoffset,int *firstOut)
{
    int i,n=0; *firstOut=-1;
    for(i=0;i<K3_RX_SLOTS;i++){
      if(!RxBufferIsPoisoned(bufLog[i],frameoffset)){
        if(*firstOut<0) *firstOut=i;
        n++; } }
    return n;
}

#endif /* AP_RX_H */
