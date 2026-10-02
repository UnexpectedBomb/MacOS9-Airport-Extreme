/* ap_bringup.h -- the b43_chip_init prefix and the accessor layer, as ONE definition.
 *
 * ★★★★★ WHY THIS FILE EXISTS, AND WHY IT IS A HEADER RATHER THAN A COPY.
 * Stage 4b-ii-1 cost nine aborted runs. The single largest cause -- five of them -- was a probe
 * running a subset of the reference sequence because the sequence had been hand-copied and a
 * statement was dropped. g2 is the clearest case: it hand-copied chip_init's prefix and silently
 * omitted gpio_init, upload_initvals_band and software_rfkill, then reported a result as though
 * it had run the whole thing.
 *
 * A hand-copied sequence drifts. A called function cannot. Everything below is extracted VERBATIM
 * from airport_accessors.c at the commit where h3 passed all three oracles -- not retyped -- and
 * ApBringUp() is the lifted form of that probe's own [1] through [1e].
 *
 * ⚠ airport_accessors.c IS FROZEN and keeps its own inline copy. That is deliberate: it is the
 * artifact whose passing log is banked, so it is not edited again. One live copy here, one frozen
 * copy there, and no third copy is ever to be made -- new probes include this header.
 *
 * ★ WHAT ApBringUp() PERFORMS, in b43_chip_init's order:
 *     1 MACCTL            2 upload_microcode      3 b43_gpio_init
 *     4 upload_initvals   5 upload_initvals_band  6 switch_analog(1)
 *     7 software_rfkill(dev,false)  [the PGACTL strobe + both channel switches]
 * It stops there. b43_phy_init would next call ops->init -> b43_phy_initg -> b43_phy_initb6, and
 * that is the caller's business, not this header's.
 *
 * ★ THE ACCESS RULE THIS HEADER ENFORCES. Every radio access here maps to a real b43 call site:
 *     RadioRead      -> b43_gphy_op_radio_read   (selector ORed with 0x80, then read DATA_LOW)
 *     RadioWrite     -> b43_gphy_op_radio_write  (selector WITHOUT 0x80, then WRITE DATA_LOW)
 *     RadioIdControl -> b43_phy_versioning       (selector 0x0001, the ID register)
 * There is deliberately NO generic "write any selector, then read" primitive. h1 and h2 both died
 * on exactly that access -- select register 0x7A with no read bit, then read the data port -- which
 * b43 performs nowhere. The absence of the primitive is the enforcement; a comment asking future
 * edits to be careful is not.
 *
 * ⚠ REQUIRES the firmware resources 'BCMu' 128, 'BCMp' 128, 'BCMi' 128 and 'BCMi' 129. A probe
 * that includes this header must link firmware_blobs.r.
 *
 * ⚠⚠ RETRACTED 2026-09-20 -- "NO FRESH BOOT REQUIRED" WAS WRONG.
 *
 * This header used to claim: "NO FRESH BOOT REQUIRED. SsbCoreEnable() does a real teardown first
 * -- proven by r1 from a clean state and by h3 from a CRASHED one, which is the case that
 * actually matters." Every probe since has been written on that assurance.
 *
 * It does not hold. k33a through k40 were run back to back with no reboot at all, the app simply
 * relaunched from the Finder, and roughly HALF were void -- in Stage 4's NRSSI restore as well as
 * Stage 5's scan and transmit sections. What r1 and h3 actually established is that ONE repeat
 * run can succeed. That is not the same as repeats being reliable, and nine runs say they are
 * not. The teardown clears enough to usually work and not enough to always work.
 *
 * ⇒ RUN FROM A FULL POWER-DOWN. Not a restart. ApBringUp now reads TMSLOW on entry and says in
 *   the log whether the core was already running, so this is measured rather than remembered.
 */
#ifndef AP_BRINGUP_H
#define AP_BRINGUP_H

/* Resources.h stays: LoadFw() below uses GetResource. The driver does not call LoadFw -- it
 * carries its firmware compiled in (8-2c) -- so this costs it the Resource Manager and nothing
 * more. Trimming it further belongs to the increment that removes LoadFw from the driver's
 * reachable set, not to this one. */
#include <Resources.h>
#include "ssb_core.h"



/* This is a header-only library: every probe includes the whole accessor layer but uses only the
 * part it needs, so -Wunused-function fires on the rest. Suppressed for the header ONLY -- the
 * pragma is popped at the bottom, so a probe's own dead code still warns, which is how the unused
 * PhyMaskSet in 4b-ii-2 was caught. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#define B43_MMIO_MACCTL             0x120UL
#define B43_MMIO_SHM_CONTROL        0x160UL
#define B43_MMIO_SHM_DATA           0x164UL
#define B43_MMIO_SHM_DATA_UNALIGNED 0x166UL
#define B43_MMIO_PHY_VER            0x3E0UL
#define B43_MMIO_PHY0               0x3E6UL
#define B43_MMIO_RADIO_CONTROL      0x3F6UL
#define B43_MMIO_RADIO_DATA_HIGH    0x3F8UL
#define B43_MMIO_RADIO_DATA_LOW     0x3FAUL
#define B43_MMIO_PHY_CONTROL        0x3FCUL
#define B43_MMIO_PHY_DATA           0x3FEUL
#define B43_MMIO_CHANNEL            0x3F0UL   /* written by channel_switch; b43 never reads it */
#define B43_MMIO_CHANNEL_EXT        0x3F4UL   /* read-modify-written, so a readback IS evidence */

/* GPIO init -- constants as used by 4b-i/f5, which proved this path on this card. */
#define B43_MMIO_GPIO_MASK          0x49EUL
#define B43_GPIO_CONTROL            0x6CUL    /* ChipCommon core offset, NOT the 802.11 core */
#define B43_MACCTL_GPOUTSMSK        0x0000C000UL
#define SPROM_BFLLO                 0x0072UL
#define B43_BFL_PACTRL              0x0002UL  /* GPIO 9 controls the PA; let the ucode drive it */

#define B43_MACCTL_SHM_ENABLED 0x00000100UL
#define B43_MACCTL_IHR_ENABLED 0x00000400UL
#define B43_MACCTL_GMODE       0x80000000UL
#define B43_MACCTL_INFRA       0x00020000UL   /* Infrastructure mode -- set by b43_chip_init */
#define B43_MACCTL_PSM_RUN     0x00000002UL
#define B43_MACCTL_PSM_JMP0    0x00000004UL
#define B43_MMIO_GEN_IRQ_REASON 0x128UL
#define B43_TMSLOW_GMODE       0x20000000UL
#define B43_TMSLOW_PHYRESET    0x00080000UL
#define B43_TMSLOW_PHYCLKEN    0x00040000UL

#define B43_SHM_SHARED         1UL
#define B43_SHM_SH_HOSTF1      0x005EUL   /* 2-mod-4 -> UNALIGNED path */
#define B43_SHM_SH_HOSTF2      0x0060UL   /* aligned */
#define B43_SHM_SH_HOSTF3      0x0062UL   /* 2-mod-4 -> UNALIGNED path */

/* PHY banking, from phy_common.h */
#define B43_PHY_CCK(r)   (r)
#define B43_PHY_EXTG(r)  ((r) | 0x0800UL)
#define B43_PHY_PGACTL          B43_PHY_CCK(0x15)
#define  B43_PHY_PGACTL_LOWBANDW 0x0040UL   /* ⚠ 0x0040 is INSIDE the 0x00C0 radio-on value */
#define  B43_PHY_PGACTL_LPF      0x1000UL   /* which is why Oracle A tests THIS bit instead */
#define B43_PHY_VERSION_CCK     B43_PHY_CCK(0x00)

#define kRadioTestReg  0x7AUL   /* first register b43_phy_initb6 touches */
#define kRadioReadBit  0x80UL   /* G-PHY reads need it; writes must not have it */

/* ★ THE LOGGING PRIMITIVES USED TO LIVE HERE. Stage 8-2d-0 moved them, verbatim, to ap_log.h.
 *
 * PCat, PCatHex, PCatDec, gLogRef/gLines, LogOpen, Out, Say, SayH and Say1 are now supplied by
 * whoever includes this file, and this file does NOT include them itself. That is the whole point:
 * the hardware below calls Say(), and so do ap_phy_initg.h's 736 log statements, but the SINK is
 * the includer's choice --
 *
 *     an application  #include "ap_log.h"   first  -> Out() writes a file on the Desktop
 *     the driver      defines its own Out() first  -> the ring in ap_ilog.h, no File Manager
 *
 * ⚠ An #include of ap_log.h here would be the easy thing and would defeat it entirely: the
 *   driver would get the File Manager back and this project would be one careless call away from
 *   a silent hard hang. The dependency runs upward on purpose.
 *
 * ⚠ A caller that forgets to provide them gets "implicit declaration of function Say" at compile
 *   time, which is a loud, immediate, host-side failure -- the right way for this to go wrong. */



static SsbBus gBus;

/* The ucode revision the CARD reports, read from SHM 0x0000 after the microcode handshake.
 * Everything generation-dependent selects on this rather than on a hardcoded assumption --
 * see the txhdr layout in airport_rx.c. */
static UInt16 gFwRev = 0;

/* Set by ApBringUp: non-zero if the 802.11 core was ALREADY enabled when we arrived, i.e. a
 * previous run left it up and this is not a fresh boot. The best single predictor of a void
 * run that this project has found. */
static int gCoreWasAlreadyUp = 0;

/* --- the four primitives, each exactly as b43 defines it ------------------- */

static UInt16 PhyRead(UInt16 reg)
{ ssb_w16(gBus.bar0,B43_MMIO_PHY_CONTROL,reg); return ssb_r16(gBus.bar0,B43_MMIO_PHY_DATA); }

static void PhyWrite(UInt16 reg, UInt16 value)
{ ssb_w16(gBus.bar0,B43_MMIO_PHY_CONTROL,reg); ssb_w16(gBus.bar0,B43_MMIO_PHY_DATA,value); }

/* ⚠ READ sets bit 7 in the selector; WRITE must not. b43_gphy_op_radio_read does reg |= 0x80 with
 * the comment "G-PHY needs 0x80 for read access"; b43_gphy_op_radio_write has no such line. */
static UInt16 RadioRead(UInt16 reg)
{ ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,(UInt16)(reg|kRadioReadBit));
  return ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_LOW); }

static void RadioWrite(UInt16 reg, UInt16 value)
{ ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,reg);
  ssb_w16(gBus.bar0,B43_MMIO_RADIO_DATA_LOW,value); }

/* SHM_SHARED 16-bit access, WITH the unaligned path. A byte offset that is 2-mod-4 must go through
 * SHM_DATA_UNALIGNED; Stage 3b omitted this because nothing needed it, and host flags need it. */
static void ShmControl(UInt32 routing,UInt32 off)
{ ssb_w32(gBus.bar0,B43_MMIO_SHM_CONTROL,(routing<<16)|off); }

static UInt16 ShmRead16Shared(UInt32 byteOffset)
{ ShmControl(B43_SHM_SHARED,byteOffset>>2);
  if(byteOffset&3) return ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA_UNALIGNED);
  return ssb_r16(gBus.bar0,B43_MMIO_SHM_DATA); }

static void ShmWrite16Shared(UInt32 byteOffset,UInt16 v)
{ ShmControl(B43_SHM_SHARED,byteOffset>>2);
  if(byteOffset&3) ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA_UNALIGNED,v);
  else             ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,v); }

/* b43_hf_read / b43_hf_write: a 48-bit value spread across three SHM words, low word first. */
static void HfRead(UInt16 *lo,UInt16 *mi,UInt16 *hi)
{ *lo=ShmRead16Shared(B43_SHM_SH_HOSTF1);
  *mi=ShmRead16Shared(B43_SHM_SH_HOSTF2);
  *hi=ShmRead16Shared(B43_SHM_SH_HOSTF3); }
static void HfWrite(UInt16 lo,UInt16 mi,UInt16 hi)
{ ShmWrite16Shared(B43_SHM_SH_HOSTF1,lo);
  ShmWrite16Shared(B43_SHM_SH_HOSTF2,mi);
  ShmWrite16Shared(B43_SHM_SH_HOSTF3,hi); }

/* --- firmware + initvals: the precondition g1 wrongly stripped ------------- */
typedef struct { Handle h; UInt8 *payload; UInt32 bytes; } FwBlob;
static UInt32 be32at(const UInt8*p){return ((UInt32)p[0]<<24)|((UInt32)p[1]<<16)|((UInt32)p[2]<<8)|(UInt32)p[3];}
static UInt16 be16at(const UInt8*p){return (UInt16)(((UInt32)p[0]<<8)|(UInt32)p[1]);}

static int LoadFw(ResType t,short id,UInt8 want,FwBlob*out){
  Handle h;long sz;UInt8*p;UInt32 dec;
  h=GetResource(t,id); if(!h) return 0;
  HLock(h); sz=GetHandleSize(h); p=(UInt8*)*h;
  if(sz<9){HUnlock(h);return 0;}
  dec=((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|(UInt32)p[7];
  if(p[0]!=want||p[1]!=1){HUnlock(h);return 0;}
  if(want!='i'&&dec!=(UInt32)(sz-8)){HUnlock(h);return 0;}   /* 'i' declares an IV COUNT */
  out->h=h; out->payload=p+8; out->bytes=(UInt32)(sz-8);
  return 1;}

static UInt32 ApplyIvs(const UInt8*p,UInt32 bytes){
  UInt32 c=0,consumed=0;
  while(consumed+2<=bytes){UInt16 osz;UInt32 off,v;int is32;
    osz=be16at(p+consumed);consumed+=2;
    off=(UInt32)osz&0x7FFFUL; is32=((UInt32)osz&0x8000UL)!=0;
    if(off>=0x1000UL)break;                       /* range-check BEFORE the write */
    if(is32){if(consumed+4>bytes)break;v=be32at(p+consumed);consumed+=4;ssb_w32(gBus.bar0,off,v);}
    else    {if(consumed+2>bytes)break;v=be16at(p+consumed);consumed+=2;ssb_w16(gBus.bar0,off,(UInt16)v);}
    c++;}
  return c;}

static void SwitchAnalog(int on){ ssb_w16(gBus.bar0,B43_MMIO_PHY0,(UInt16)(on?0x0000:0x00F4)); }

/* ---- b43_gpio_init(), chip_init step 3. Body is 4b-i/f5's, which PASSED on this card; the only
 * change is that it is now in the position chip_init gives it -- after the microcode handshake and
 * before the initvals. Switches the backplane window to ChipCommon and ⚠ ALWAYS SWITCHES BACK:
 * every register after this point is an 802.11-core offset, and leaving the window on ChipCommon
 * would silently redirect all of them. ---- */
static void GpioInit(UInt32 *boardflagsOut,UInt32 *maskOut,UInt32 *setOut,int *ccOk)
{
  UInt32 boardflags,gpioMask,gmask,gset,macctl,before,want,after;
  *ccOk=0;
  boardflags=(UInt32)ssb_r16(gBus.bar0,SSB_SPROM_BASE1+SPROM_BFLLO);
  macctl=ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
  ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl&~B43_MACCTL_GPOUTSMSK);
  gpioMask=(UInt32)ssb_r16(gBus.bar0,B43_MMIO_GPIO_MASK);
  ssb_w16(gBus.bar0,B43_MMIO_GPIO_MASK,(UInt16)(gpioMask|0x000F));
  gmask=0x0000001FUL; gset=0x0000000FUL;
  if(boardflags&B43_BFL_PACTRL){
    gpioMask=(UInt32)ssb_r16(gBus.bar0,B43_MMIO_GPIO_MASK);
    ssb_w16(gBus.bar0,B43_MMIO_GPIO_MASK,(UInt16)(gpioMask|0x0200));
    gmask|=0x0200UL; gset|=0x0200UL; }
  if(gBus.idxChipCommon!=kSsbNoCore){
    if(SsbSelectCore(&gBus,gBus.idxChipCommon)==noErr){
      before=ssb_r32(gBus.bar0,B43_GPIO_CONTROL);
      want=(before&~gmask)|gset;
      ssb_w32(gBus.bar0,B43_GPIO_CONTROL,want);
      (void)ssb_r32(gBus.bar0,B43_GPIO_CONTROL);
      after=ssb_r32(gBus.bar0,B43_GPIO_CONTROL);
      /* Masked compare only: GPIO_CONTROL carries lines this driver does not own. */
      if((after&gmask)==(want&gmask)) *ccOk=1; }
    (void)SsbSelectCore(&gBus,gBus.idx80211); }
  *boardflagsOut=boardflags; *maskOut=gmask; *setOut=gset;
}

/* ---- The radio power-on path, chip_init step 7's first half.
 * b43_radio_channel_codes_bg[] verbatim from phy_g.c:46. channel2freq_bg(ch) is codes[ch-1], so
 * ch 1 -> 12, ch 6 -> 37, ch 10 -> 57. ---- */
static const UInt8 kChanCodeBg[14]={12,17,22,27,32,37,42,47,52,57,62,67,72,84};
static UInt16 Channel2FreqBg(UInt8 ch){return (UInt16)kChanCodeBg[ch-1];}

/* b43_synth_pu_workaround: guarded by (radio_ver == 0x2050 && radio_rev < 6). Stage 4a read this
 * card as radio 0x2050 rev 2, so THE BRANCH IS TAKEN -- the guard is not reproduced as a runtime
 * test because the hardware that would fail it is not the hardware in the machine. Writes
 * B43_MMIO_CHANNEL only; it reads and writes no radio register. */
static void SynthPuWorkaround(UInt8 ch)
{ if(ch<=10) ssb_w16(gBus.bar0,B43_MMIO_CHANNEL,Channel2FreqBg((UInt8)(ch+4)));
  else       ssb_w16(gBus.bar0,B43_MMIO_CHANNEL,Channel2FreqBg(1));
  SsbSpinUs(1000);                                  /* msleep(1) */
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL,Channel2FreqBg(ch)); }

/* b43_gphy_channel_switch. The channel-14 arm (Japan country-code / ACPR host flag) is not ported:
 * this probe passes 6 and 1 only, so that arm is unreachable, and porting an unreachable branch
 * would mean writing a country-code SPROM read that nothing here can exercise or verify. */
static void GphyChannelSwitch(UInt8 ch,int synthPu)
{ if(synthPu) SynthPuWorkaround(ch);
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL,Channel2FreqBg(ch));
  ssb_w16(gBus.bar0,B43_MMIO_CHANNEL_EXT,
          (UInt16)(ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT)&0xF7BF)); }

/* b43_gphy_op_software_rfkill(dev, blocked=false) -- the radio-ON arm.
 * The `if (phy->radio_on) return;` early-out and the gphy->radio_off_context.valid RFover restore
 * are both omitted: the first is a software latch we do not keep, and the second is false on a
 * first init by construction (nothing has run the radio-OFF arm that sets it). gmode is true for
 * this card, so PGACTL's third write is 0x00C0, not 0x0000. */
static void RadioOn(UInt8 defaultChan)
{ PhyWrite((UInt16)B43_PHY_PGACTL,0x8000);
  PhyWrite((UInt16)B43_PHY_PGACTL,0xCC00);
  PhyWrite((UInt16)B43_PHY_PGACTL,0x00C0);
  GphyChannelSwitch(6,1);
  GphyChannelSwitch(defaultChan,0); }

/* ⚠ LadderStep() WAS HERE AND IS DELETED ON PURPOSE. It took a raw selector value, which is what
 * let h1 issue "select register 0x7A with no read bit, then read the data port" -- an access b43
 * performs nowhere. Deleting the generic primitive means the only way to reach the radio in this
 * file is RadioRead/RadioWrite/RadioIdControl, each of which mirrors a specific b43 call site.
 * The rule is enforced by what exists, not by a comment asking future edits to be careful. */

/* ---- THE CONTROL: the radio ID read. Driver-sanctioned -- b43_phy_versioning performs exactly
 * this, BEFORE any of chip_init's prefix, which is why Stage 4a could do it in minimal state.
 * h1 proved the flush is NOT load-bearing (selector 0x0001 read 0x017F with and without it), so
 * the flush is kept only because 4a had it and this is the control. ---- */
static int RadioIdControl(void)
{
  UInt16 lo,hi;
  Say("    CONTROL -- radio ID via selector 0x0001, as b43_phy_versioning does.");
  ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,0x0001);
  (void)ssb_r16(gBus.bar0,B43_MMIO_RADIO_CONTROL);
  lo=ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_LOW);
  ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,0x0001);
  (void)ssb_r16(gBus.bar0,B43_MMIO_RADIO_CONTROL);
  hi=ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_HIGH);
  SayH("      DATA_LOW  = ",lo,4);
  SayH("      DATA_HIGH = ",hi,4);
  if(lo==0x017FU&&hi==0x2205U){
    Say("      ==> matches Stage 4a's radio raw 0x2205017F exactly."); return 1; }
  Say("      ⚠ does NOT match Stage 4a's 0x2205017F -- treat what follows as suspect.");
  return 0;
}

/* ⚠ Initb6Opening() WAS HERE AND IS REMOVED. It was h2/h3's one-shot experiment -- write PHY
 * 0x003E, then read radio 0x7A -- and it answered its question: PHY 0x003E = 0x817A is what makes
 * register 0x7A reachable. Its content now belongs to b43_phy_initb6's real port (4b-ii-2), which
 * performs those two statements as statements rather than as a test. Keeping a probe-specific
 * experiment in a shared header would invite a later probe to call it and run initb6's opening
 * TWICE -- once here and once in the port -- which for a read-modify-write on 0x7A would silently
 * produce a different value than the reference computes. Removed, not kept "just in case".
 * airport_accessors.c keeps its own frozen copy. */
static void PhyTakeOutOfReset(void)
{ UInt32 t;
  t=ssb_r32(gBus.bar0,SSB_TMSLOW);t&=~B43_TMSLOW_PHYRESET;t&=~B43_TMSLOW_PHYCLKEN;t|=SSB_TMSLOW_FGC;
  ssb_w32(gBus.bar0,SSB_TMSLOW,t);(void)ssb_r32(gBus.bar0,SSB_TMSLOW);SsbSpinUs(1500);
  t=ssb_r32(gBus.bar0,SSB_TMSLOW);t&=~SSB_TMSLOW_FGC;t|=B43_TMSLOW_PHYCLKEN;
  ssb_w32(gBus.bar0,SSB_TMSLOW,t);(void)ssb_r32(gBus.bar0,SSB_TMSLOW);SsbSpinUs(1500); }


/* ---- ApBringUp(): b43_chip_init steps 1-7, in order. Lifted from airport_accessors.c's main()
 * at the h3 commit; the only edit is that its `goto verdict` exits became `return 0`.
 * Returns 1 when the core is up, firmware is running and the radio is powered. ---- */
static int ApBringUp(void)
{
    UInt32 flags,tmslow,macctl,w,words,irq=0;
    UInt32 bf=0,gm=0,gs=0; int ccOk=0; short i;
    FwBlob ucode,pcm,iv,bsiv; int havePcm=0,haveBs=0;

    if(SsbFindAndMap(&gBus)!=noErr){Say("!! could not find/map the BCM4306");return 0;}
    if(SsbScanCores(&gBus)!=noErr){Say("!! core scan failed");return 0;}
    SayH("[1] BAR0 = ",(unsigned long)gBus.bar0,8);
    SayH("    chip = ",gBus.chipId,4);
    if(SsbSelectCore(&gBus,gBus.idx80211)!=noErr){Say("!! select failed");return 0;}

    /* ★★★★★ IS THIS THE FIRST RUN SINCE THE MACHINE BOOTED? THE LOG NOW SAYS SO.
     *
     * On 2026-09-20 it emerged that k33a onwards were run back to back WITHOUT rebooting -- the
     * app relaunched from the Finder each time. Roughly half of those runs were void, across two
     * unrelated stages: the Stage 5 scan receive, the Stage 5 transmit section, and finally
     * Stage 4's NRSSI restore. Three independent bugs in unrelated subsystems was always a poor
     * explanation; one incomplete teardown is a much better one.
     *
     * ⚠ AND IT FALSIFIES THIS FILE'S OWN HEADER, line 37: "NO FRESH BOOT REQUIRED.
     *   SsbCoreEnable() does a real teardown first -- proven by r1". r1 proved that ONE repeat
     *   run could work, which is not the same as proving repeats are reliable, and nine runs at
     *   a ~50% failure rate say they are not. The claim is retracted at its source.
     *
     * A core still holding its clock with reset clear means a previous run left it running. That
     * is not fatal -- SsbCoreEnable still tears it down and most such runs do succeed -- but it
     * is the single best predictor we have of a void run, and it must appear in the log rather
     * than depend on anyone remembering how the machine was started. */
    { UInt32 pre = ssb_r32(gBus.bar0,SSB_TMSLOW);
      int wasUp = ((pre & SSB_TMSLOW_CLOCK) != 0) && ((pre & SSB_TMSLOW_RESET) == 0);
      gCoreWasAlreadyUp = wasUp;
      SayH("    TMSLOW on entry = ",pre,8);
      if(wasUp){
        Say("    ⚠⚠ THE CORE WAS ALREADY RUNNING -- THIS IS NOT A FRESH BOOT.");
        Say("       A previous run left the 802.11 core enabled. About half of the runs taken");
        Say("       this way have been void, in Stage 4 as well as Stage 5, so if anything below");
        Say("       fails strangely this is the first thing to suspect and the cheapest to rule");
        Say("       out: shut the machine down fully -- not restart -- and run once.");
      } else {
        Say("    ★ the core was NOT running: this is a clean first run since boot."); } }

    flags=B43_TMSLOW_GMODE|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET;
    SsbCoreEnable(&gBus,flags);
    SsbSpinUs(2500);
    PhyTakeOutOfReset();
    tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
    SayH("    TMSLOW after enable = ",tmslow,8);
    if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){
      Say("!! core did not come up");return 0;}
    SayH("    PHY_VER sanity = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* The radio ID control, in minimal state -- exactly where Stage 4a read it successfully.
     * Cheap, driver-sanctioned, and it fails loudly if the teardown left the core wrong. */
    Say("");
    Say("[CTL-A] radio ID control in minimal state (b43_phy_versioning's path)");
    (void)RadioIdControl();

    SwitchAnalog(1);
    Say("");
    SayH("    switch_analog(1) done; PHY_VER = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ---- [1b] chip_init steps 1 and 2: MACCTL, then the microcode. ---- */
    Say("");
    Say("[1b] MICROCODE UPLOAD -- chip_init steps 1 and 2");
    if(!LoadFw('BCMu',128,'u',&ucode)){Say("!! ucode5 resource missing/bad");return 0;}
    if(!LoadFw('BCMi',128,'i',&iv))   {Say("!! initvals resource missing/bad");return 0;}
    havePcm = LoadFw('BCMp',128,'p',&pcm);
    haveBs  = LoadFw('BCMi',129,'i',&bsiv);
    /* ⚠⚠ B43_MACCTL_INFRA WAS MISSING HERE FROM STAGE 3b UNTIL i4, AND THIS IS THE FIX.
     * b43 does this in TWO steps, and the second is a read-modify-write:
     *     b43_chip_init:        MACCTL = IHR_ENABLED | SHM_ENABLED | [GMODE] | INFRA
     *     b43_upload_microcode: macctl = read(MACCTL); macctl |= PSM_JMP0; write(MACCTL, macctl)
     * The read-modify-write PRESERVES INFRA, and the later maskset(~PSM_JMP0, PSM_RUN) preserves
     * it too. This port had collapsed both into one hand-composed literal and dropped INFRA in
     * the process -- the same class of error as g2 hand-copying chip_init's prefix and losing
     * three steps. It went unnoticed because nothing before i4 exercised the MAC's TX engine:
     * microcode upload, SHM, PHY and radio register work are all indifferent to it.
     * Now written as two statements, matching the reference. */
    macctl=B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_INFRA;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);(void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    macctl=ssb_r32(gBus.bar0,B43_MMIO_MACCTL)|B43_MACCTL_PSM_JMP0;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);(void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    SayH("     MACCTL = ",macctl,8);
    Say ("       (INFRA 0x00020000 must be set -- it was missing from 3b through i3.)");
    for(w=0;w<64;w++){ShmControl(2UL,w);ssb_w16(gBus.bar0,B43_MMIO_SHM_DATA,0);}
    for(w=0;w<4096;w+=4){ShmControl(B43_SHM_SHARED,w>>2);ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,0);}
    words=ucode.bytes/4;
    ShmControl(0UL|0x0100UL,0x0000);                       /* SHM_UCODE | AUTOINC_W */
    for(w=0;w<words;w++){ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,be32at(ucode.payload+w*4));SsbSpinUs(10);}
    if(havePcm){ words=pcm.bytes/4;
      ShmControl(3UL,0x01EA); ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,0x00004000UL);
      ShmControl(3UL,0x01EB);
      for(w=0;w<words;w++){ssb_w32(gBus.bar0,B43_MMIO_SHM_DATA,be32at(pcm.payload+w*4));SsbSpinUs(10);} }
    ssb_w32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON,0xFFFFFFFFUL);
    macctl=ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    macctl&=~B43_MACCTL_PSM_JMP0; macctl|=B43_MACCTL_PSM_RUN;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);(void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
    for(i=0;i<100;i++){
      irq=ssb_r32(gBus.bar0,B43_MMIO_GEN_IRQ_REASON);
      if(irq==0x00000001UL) break;
      SsbSpinUs(10000); }
    SayH("     GEN_IRQ_REASON = ",irq,8);
    /* ★★★★★ THIS GUARD WAS bwi's, AND IT WAS BACKWARDS FOR THIS PORT.
     *
     * It read `rev > 0x128 -> reject`, which is bwi's BWI_FW_VERSION3_REVMAX: bwi accepts v3
     * firmware and refuses v4. But Stage 4 and Stage 5 are a b43 port -- 1,390 b43 citations,
     * zero bwi ones -- and b43's rule is the exact opposite (main.c:2689):
     *     if (fwrev <= 0x128) { "YOUR FIRMWARE IS TOO OLD ... older than version 4.x is
     *                            unsupported"; return -EOPNOTSUPP; }
     * 0x128 = 296 is the v3/v4 boundary and the two drivers sit on opposite sides of it. We
     * had v3 firmware under a v4 driver, so the microcode read our 106-byte b43_txhdr as its
     * own 82-byte layout: null PLCP at +76, null destination at +26. Nothing decodable ever
     * left the antenna, which is exactly what the channel-1 capture measured. */
    { UInt16 rev=ShmRead16Shared(0x0000UL);
      gFwRev = rev;
      SayH("     ucode revision = ",(unsigned long)rev,4);
      if(irq!=0x00000001UL||rev==0){
        Say("!! firmware did not come up -- everything below would be meaningless.");
        return 0; }
      if(rev<=0x128UL){
        Say("!! FIRMWARE IS v3 (rev <= 0x128). This port is b43-derived and b43 refuses this");
        Say("   generation outright. Re-run ./embed-fw.sh against ../firmware/v4 and rebuild.");
        return 0; }
      Say (rev>=410 ? "     -> FW_HDR_410 txhdr layout (110 bytes)"
                    : "     -> FW_HDR_351 txhdr layout (106 bytes)"); }
    Say("     firmware running");

    /* ---- [1c] chip_init step 3: GPIO INIT, between the microcode and the initvals. ---- */
    Say("");
    Say("[1c] GPIO INIT (b43_gpio_init) -- chip_init step 3");
    GpioInit(&bf,&gm,&gs,&ccOk);
    SayH("     board flags (SPROM +0x72) = ",bf,4);
    Say((bf&B43_BFL_PACTRL)
        ? "     PACTRL set -> GPIO 9 enabled so the ucode can drive the PA"
        : "     PACTRL clear -> GPIO 9 branch not taken");
    Say(ccOk ? "     ChipCommon GPIO_CONTROL holds the masked bits -- matches 4b-i/f5"
             : "     ⚠ ChipCommon GPIO_CONTROL did NOT hold the masked bits; f5 says it should");
    SayH("     window back on the 802.11 core, TMSLOW = ",ssb_r32(gBus.bar0,SSB_TMSLOW),8);

    /* ---- [1d] chip_init steps 4 and 5: BOTH initvals tables. ---- */
    Say("");
    Say("[1d] INITVALS -- BOTH tables (chip_init steps 4 and 5)");
    { UInt32 n=ApplyIvs(iv.payload,iv.bytes);
      Say1("     b0g0initvals5   records applied = ",n); }
    if(haveBs){ UInt32 n=ApplyIvs(bsiv.payload,bsiv.bytes);
      Say1("     b0g0bsinitvals5 records applied = ",n); }
    else Say("     ⚠ b0g0bsinitvals5 ('BCMi' 129) ABSENT -- re-run ./embed-fw.sh");
    SayH("     PHY_VER after = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ---- [1e] chip_init steps 6 and 7: switch_analog, then the radio power-on. ---- */
    Say("");
    Say("[1e] switch_analog(1), then b43_software_rfkill(dev,false)");
    SwitchAnalog(1);
    RadioOn(1);                                   /* b43_gphy_op_get_default_chan() returns 1 */
    SayH("     PGACTL after the 0x8000 / 0xCC00 / 0x00C0 strobe = ",
         (unsigned long)PhyRead((UInt16)B43_PHY_PGACTL),4);
    SayH("     CHANNEL_EXT (0x3F4) = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT),4);

    Say("");
    Say("[CTL-B] the same radio ID control, now AFTER the full prefix");
    (void)RadioIdControl();
    return 1;
}

#pragma GCC diagnostic pop

#endif /* AP_BRINGUP_H */
