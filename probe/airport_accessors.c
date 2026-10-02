/* airport_accessors.c -- STAGE 4b-ii-1: PROVE THE FOUR PRIMITIVES THE PHY PORT RESTS ON.
 *
 * ★★★★★ WHY THIS IS THE FIRST PIECE OF 4b-ii, AND WHY 4b-ii IS NOT ONE PROBE.
 * The roadmap said "port the G-PHY init sequence". Measured against real source, that sequence is:
 *     phy_g.c  3057    wa.c  376    lo.c  1003    tables.c  453    = 4889 lines
 * b43_phy_initg's own body is ~100 lines, but its callees are not small and are not independent:
 *     b43_phy_initb6      115   -- and it calls channel_switch, hf_read/write, set_txpower_g
 *     b43_phy_inita        18   -- and it calls b43_wa_all(), an entire workarounds file
 *     b43_calc_loopback_gain  153
 *     b43_radio_init2050   234
 *     b43_calc_nrssi_slope 141
 *     b43_phy_init_pctl     67
 *     b43_lo_g_init/adjust  --  not in phy_g.c at all; they are lo.c, 1003 lines
 * There is NO clean hundred-line slice of initg. Trying to take one would be Stage 4b-i's mistake
 * at ten times the scale.
 *
 * So the first increment is not a slice of initg -- it is the ACCESSOR LAYER UNDERNEATH ALL OF IT.
 * Stage 4b-i proved PHY register READS and nothing else. These four primitives are used hundreds of
 * times across those 4889 lines, and three of them have never been executed on this hardware:
 *     PHY write        never tested
 *     radio read       only via the RADIOCTL_ID identity path, never the general G-PHY path
 *     radio write      never tested
 *     host flags       never tested; they live in SHM at offsets that force the UNALIGNED path
 * ⚠ A WRONG ACCESSOR DOES NOT FAULT. It silently reads or writes the wrong register, and the error
 * surfaces thousands of lines later as a radio that will not tune. A read of an undecoded register
 * announces itself; a mis-routed write does not. That asymmetry is the whole argument for testing
 * these four before porting anything that uses them.
 *
 * ★ THE ASYMMETRY THAT MAKES THIS WORTH A RUN. From phy_g.c:
 *     b43_gphy_op_radio_read():   reg |= 0x80;   /-* G-PHY needs 0x80 for read access *-/
 *     b43_gphy_op_radio_write():  no such OR
 * Reads need bit 7 set in the register selector; writes must not have it. Getting that backwards
 * reads one register and writes another, with nothing to indicate it. Test [2] demonstrates the
 * difference directly rather than trusting the comment.
 *
 * ⚠ ALSO: "Register 1 is a 32-bit register" (B43_WARN_ON(reg == 1) in both radio ops). Radio
 * register 1 is never touched here.
 *
 * ★★★★★ THE ORACLES -- all three must pass; each covers a primitive the port cannot do without.
 *   A: PHY WRITE. PGACTL (CCK 0x15) is read-proven (4b-i read 0x00C0) and write-proven (initg
 *      writes 0xC0 to it). Round trip: rewrite its own value, then a changed value, then restore.
 *      Every step verified by reading back. PHY registers are storage, not ports -- b43 does
 *      read-modify-write on them via b43_phy_maskset -- so readback is a legitimate test here, by
 *      the Stage 4a storage-versus-port rule.
 *   B: RADIO READ AND WRITE. Radio 0x7A is the first register b43_phy_initb6 touches
 *      (read, OR 0x0058, write back), so it is exercised by the real sequence. Same round trip,
 *      plus a direct demonstration that reading with and without the 0x80 bit differ.
 *   C: HOST FLAGS. b43_hf_read/write assemble a 48-bit value from SHM_SHARED 0x5E, 0x60, 0x62.
 *      0x5E and 0x62 are 2-mod-4, so two of the three go through SHM_DATA_UNALIGNED (0x166) --
 *      a path Stage 3b deliberately did NOT implement because nothing needed it then. It is needed
 *      now, and this is where it gets proven rather than assumed.
 *
 * ⚠⚠ g1 RAN WITHOUT FIRMWARE OR INITVALS AND CRASHED. THAT CALL WAS WRONG, AND THE REASON IS
 * WORTH MORE THAN THE FIX.
 * g1 argued: none of the four primitives needs firmware, Stage 3a proved SHM works before it, and
 * loading it would make a failure ambiguous -- the "don't widen a narrow probe" rule that 4b-i
 * taught. Oracle A passed, proving the PHY port works bare. Then the FIRST general radio read
 * faulted: R4 = 0x3FA (RADIO_DATA_LOW), R5 = 0x7A.
 *
 * The ordering was already in the source I had read. b43_phy_initg runs from b43_phy_init, which
 * b43_chip_init calls AFTER b43_upload_microcode and b43_upload_initvals. So when b43_phy_initb6
 * reads radio 0x7A -- the exact operation g1 tried -- firmware is running and initvals are applied.
 *
 * ⇒ "DON'T WIDEN A NARROW PROBE" AND "DON'T STRIP ITS PRECONDITIONS" ARE DIFFERENT RULES, and g1
 * applied the first where the second governed. Firmware and initvals are not extra scope here; they
 * are the state the radio path is documented to run in. The test for which rule applies is not "is
 * this step needed by my code" but "does the reference sequence establish it before the operation I
 * am copying". g2 restores them, in b43's order.
 *
 * ★ g1 WAS NOT WASTED: it established that the banked PHY register path works with NO firmware and
 * NO initvals, which nothing before it had shown -- 4b-i only ever read through that port after
 * both. Oracle A is banked and does not need re-proving.
 *
 * ⚠⚠⚠ g2 ADDED FIRMWARE AND INITVALS AND DIED AT THE SAME INSTRUCTION: R4 = 0x3FA, R5 = 0x7A.
 * Identical fault, identical registers. g2 therefore did not merely fail -- it FALSIFIED the
 * hypothesis it was built to test. "Firmware + initvals" was not the missing precondition, or not
 * the only one, and a third guess at one more bolt-on would have been the same move a third time.
 *
 * ★★★★★ SO g3 STOPS GUESSING AND PORTS THE DOCUMENTED PREFIX IN FULL.
 * Read end to end rather than recalled, b43_chip_init() is:
 *     1  write MACCTL                      2  b43_upload_microcode
 *     3  b43_gpio_init                     4  b43_upload_initvals
 *     5  b43_upload_initvals_band          6  phy->ops->switch_analog(dev, 1)
 *     7  b43_phy_init  ->  switch_analog(1); b43_software_rfkill(dev,false); ops->init -> initb6
 * initb6's first act is the radio 0x7A read that g1 and g2 both died on. Against that list, g2 ran
 * steps 1, 2, 4 and 6. IT WAS MISSING THREE STEPS, NOT ONE:
 *     step 3  b43_gpio_init          -- g2's header asserted "nothing in the reference sequence
 *                                       puts it before the radio register reads in initb6". That
 *                                       sentence was never checked against chip_init, and
 *                                       chip_init contradicts it outright. 4b-i/f5 already proved
 *                                       this code works on this card; g2 simply left it out.
 *     step 5  upload_initvals_band   -- g2 applied 'BCMi' 128 and ignored 'BCMi' 129. Stage 4a
 *                                       applied both.
 *     step 7  b43_software_rfkill(dev,false)  -- the radio power-on itself.
 *
 * ★ WHY step 7 IS THE ONE THAT MATTERS. A PCI master abort on an MMIO read is exactly what an
 * unpowered radio looks like from the host side, and on Uni-N a master abort surfaces as the
 * machine check both crashes showed. b43_gphy_op_software_rfkill's radio-ON arm is:
 *     b43_phy_write(0x0015, 0x8000); b43_phy_write(0x0015, 0xCC00);
 *     b43_phy_write(0x0015, gmode ? 0x00C0 : 0x0000);      /-* 0x0015 IS PGACTL *-/
 *     channel = phy->channel; b43_gphy_channel_switch(dev, 6, 1);
 *     b43_gphy_channel_switch(dev, channel, 0);
 * Every one of those is a PHY write or a write to B43_MMIO_CHANNEL / CHANNEL_EXT. NOT ONE of them
 * is a radio-register access -- so the whole power-on path runs over ports g1 already proved work
 * bare. That is what makes this safe to add: it is not new risk, it is the strobe that makes 0x3FA
 * answer at all. b43_synth_pu_workaround applies to us (radio 0x2050 rev 2 < 6) and likewise
 * touches only B43_MMIO_CHANNEL.
 *
 * ★★ AND ONE THING DELIBERATELY *NOT* PORTED, WHICH FIDELITY ARGUES FOR LEAVING OUT.
 * b43_software_rfkill() wraps the op in b43_mac_suspend()/b43_mac_enable(). Those are refcounted,
 * and setup_struct_wldev_for_init() sets dev->mac_suspended = 1 (b43_main.c:4699) BEFORE
 * b43_chip_init runs -- so during PHY init both reduce to an increment and a decrement, and
 * neither touches a register. Porting them literally would have cleared MACCTL_ENABLED (a bit this
 * probe never sets) and then spun 35 + 40 iterations waiting for B43_IRQ_MAC_SUSPENDED, which the
 * firmware has no reason to post for a MAC that was never enabled. Copying them would have been a
 * self-inflicted stall dressed up as faithfulness. Fidelity means porting what the reference
 * ACTUALLY EXECUTES, which is not the same as what its source text contains.
 *
 * ★★★★★ h1 -- STOP PORTING. START MEASURING.
 *
 * g3 ran the COMPLETE documented prefix -- all seven chip_init steps, GPIO taking the PACTRL
 * branch, both initvals tables (317 + 31 records), switch_analog, software_rfkill with the full
 * channel programming -- and still aborted on the same read. Three probes, three identical faults.
 * The prefix is not the variable, and g3's own verdict said the next move is measurement.
 *
 * ★ THE FACT THAT REFRAMES THE WHOLE PROBLEM: STAGE 4a READ 0x3FA SUCCESSFULLY.
 * `airport_stage4a.c:500` reads B43_MMIO_RADIO_DATA_LOW and produced raw 0x2205017F -- the low
 * half, 0x017F (Broadcom's manufacturer ID), IS that read. So **0x3FA is decoded on this card**
 * and the offset was never the problem. Exactly three things differ between 4a's working read and
 * our failing one:
 *
 *   (a) THE SELECTOR VALUE. 4a wrote 0x0001 (RADIOCTL_ID). We write 0x7A | 0x80 = 0x00FA.
 *   (b) THE FLUSH. 4a read RADIO_CONTROL back between the selector write and the data read.
 *       b43_gphy_op_radio_read does not, and neither did g1-g3. Note b43_write16f's flush
 *       COMPILES OUT on PCI -- it is gated on CONFIG_BCM47XX_BCMA -- so the port was faithful,
 *       which is precisely why this must be tested rather than assumed either way.
 *   (c) switch_analog. ⚠ STAGE 4a NEVER CALLS IT. There is no B43_MMIO_PHY0 write anywhere in
 *       airport_stage4a.c. g1, g2 and g3 all write PHY0 = 0 before touching the radio. 4b-i/f2
 *       established that switch_analog(1) is REQUIRED for the PHY data port; nothing has ever
 *       established what it does to the RADIO port.
 *
 * ★ THE LADDER separates all three in ONE boot. It runs at three phases that differ by exactly
 * one variable each, and within a phase the steps run safest-first so the maximum number complete
 * before any abort. Every step logs its intent BEFORE the access, and Out() does FSWrite +
 * FlushVol per line, so THE LAST LINE IN THE LOG NAMES THE EXACT ACCESS THAT DIED.
 *
 *   phase A  core enabled, PHY out of reset, NO switch_analog   <- Stage 4a's state
 *   phase B  + switch_analog(1)                                 <- the only delta from A
 *   phase C  + the full chip_init prefix                        <- g3's exact state
 *
 * HOW TO READ THE RESULT:
 *   A step 1 aborts            -> our bring-up differs from 4a's; nothing below is meaningful
 *   A passes, B fails          -> (c) switch_analog kills the radio port
 *   step 2 aborts, 1 passes    -> (b) the flush is load-bearing on real hardware
 *   1 and 2 pass, 3/4/5/6 fail -> (a) the selector, and WHICH step names which bit
 *   every phase passes         -> the fault needs a fourth explanation, but we will have real
 *                                 radio values in hand and the oracles run anyway
 *
 * ★★★★★ h1's RESULT, AND THE ONE-LINE OMISSION BEHIND FOUR BOOTS.
 * The ladder ran in minimal state and produced real answers before it died:
 *     (1) selector 0x0001 + flush  -> 0x017F / 0x2205   CONTROL PASSES, identical to Stage 4a
 *     (2) selector 0x0001 NO flush -> 0x017F            ⇒ THE FLUSH IS NOT LOAD-BEARING
 *     (3) selector 0x0080 + flush  -> 0x0000            ⇒ the 0x80 read bit alone is fine
 *     (4) selector 0x007A + flush  -> ABORT
 * Combined with g1/g2/g3 (selector 0x00FA -> ABORT), radio register 0x7A is unreachable in BOTH
 * modes while registers 0x00 and 0x01 answer normally. Hypotheses (a) and (b) are both dead: it
 * is not the read bit and it is not the flush.
 *
 * ⇒ THE ACTUAL CAUSE, and it was in the source the whole time. b43_phy_initb6 opens:
 *       phy_g.c:1586   b43_phy_write(dev, 0x003E, 0x817A);
 *       phy_g.c:1587   b43_radio_write16(dev, 0x007A, b43_radio_read16(dev, 0x007A) | 0x0058);
 * Every probe from g1 onward copied line 1587 and skipped 1586 -- the statement DIRECTLY ABOVE it,
 * in the same function. PHY 0x003E appears EXACTLY ONCE in the whole driver, right there, and is
 * unnamed in every b43 header, which is exactly the profile of a register that arms the path the
 * next line uses.
 *
 * ★★ THE DESIGN RULE h1 ITSELF BROKE, WRITTEN DOWN SO IT STOPS RECURRING.
 * h1 died on ITS OWN step (4) -- "select register 0x7A with no read bit, then read the data port".
 * b43 NEVER performs that access: radio_read always ORs 0x80, radio_write always WRITES the data
 * port afterwards rather than reading it. Step (4) was an access with no counterpart anywhere in
 * the reference driver, and h1 ranked it as safer than steps (5) and (6), which replicate the real
 * failing path and therefore never ran.
 *   ⇒ NEVER ISSUE AN MMIO ACCESS THAT DOES NOT APPEAR IN THE REFERENCE DRIVER. b43's access set
 *     has been validated on millions of cards; anything outside it is unexplored silicon, and on
 *     this machine unexplored silicon costs a reboot. When a novel access is genuinely required,
 *     it goes LAST in the run, after everything driver-sanctioned has completed.
 * h2 obeys that rule: every access below appears in b43, and the invented ones are simply gone.
 *
 * ★★★★★ h2's RESULT: CONFIRMED. THE RADIO READ.
 *     -> writing PHY 0x003E = 0x817A
 *        PHY 0x003E reads back = 0x817A
 *     -> w 0x3F6 = 0x00FA, then r 0x3FA
 *        RADIO 0x7A = 0x0000          <- no abort, after four boots of aborts
 * The missing precondition was the PHY 0x003E write on the line directly above the radio access
 * in b43_phy_initb6. Both controls (minimal state and post-prefix) read 0x017F / 0x2205, so the
 * prefix never broke the radio path -- there was simply never a path until 0x003E was written.
 *
 * ⚠⚠ AND THIS FALSIFIES g3's STATED EXPLANATION, which must not be left standing.
 * g3 claimed the radio was unpowered and that b43_software_rfkill was the fix. h2 ran the entire
 * prefix INCLUDING software_rfkill and still aborted; the read only worked once 0x003E was
 * written. The power-on path was never the cause. It is still correct to perform -- b43 performs
 * it -- but it explained nothing, and g3 presented it as the answer.
 *
 * ⚠⚠⚠ h2 STILL CRASHED, AND ON THE EXACT ACCESS ITS OWN HEADER FORBADE.
 * h2 died AFTER the successful read, in Oracle B's "asymmetry demonstration": select register
 * 0x7A with NO read bit, then read the data port. That is h1 ladder step (4), verbatim. h2 wrote
 * the rule, deleted LadderStep() to enforce it, and never grepped the rest of the file for the
 * same pattern -- which had been sitting 200 lines below since g1, unchanged.
 *   ⇒ A RULE ENFORCED ONLY WHERE IT WAS DISCOVERED IS NOT ENFORCED. h3 audited every
 *     RADIO_CONTROL / RADIO_DATA access in this file against a real b43 call site; the one
 *     violation is deleted, not guarded, because the hardware master-aborts on it.
 *
 * ★ THE ASYMMETRY NEEDED NO PROBE. h1 step (3) read selector 0x0080 (register 0x00 WITH the bit)
 * and got 0x0000 cleanly; h1 step (4) and h2 both selected 0x007A WITHOUT it and took a master
 * abort. A bus abort is not a different value -- it is the data port declining to drive. That is
 * a stronger demonstration of "the 0x80 bit is real" than comparing two returned numbers, and it
 * was already in hand before h2 ran.
 *
 * ⚠ THIS PROBE DOES NOT REQUIRE A FRESH BOOT. It uses ssb_core.h's SsbCoreEnable(), which now
 * performs a real teardown first. Run it repeatedly.
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
#include <Resources.h>
#include "ssb_core.h"

#define kFontIDMonaco 4
#define AP_ACC_VER "h3"

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

static void PCat(Str255 d,const char*s){short l=d[0];while(*s&&l<255)d[++l]=(unsigned char)*s++;d[0]=(unsigned char)l;}
/* No PCatDec here: this probe reports register values only, and every one of them is hex.
 * Carrying an unused formatter would be the same liability as the unused PhyWrite that 4b-i
 * removed -- code that never runs is code that was never tested. */
static void PCatHex(Str255 d,unsigned long v,int digits){static const char h[]="0123456789ABCDEF";
  int i;for(i=digits-1;i>=0;i--)if(d[0]<255)d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF];}

static short gLogRef=0,gLogVol=0; static long gLogDir=0;
static Str255 gLines[220]; static short gN=0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Accessors Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<220){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}
static void Say(const char*s){Str255 L;L[0]=0;PCat(L,s);Out(L);}
static void SayH(const char*s,unsigned long v,int d){Str255 L;L[0]=0;PCat(L,s);PCat(L,"0x");PCatHex(L,v,d);Out(L);}
/* Record counts are compared against decimal figures in the blob headers (317, 31), so print
 * them in decimal -- 0x13D against "317 declared" is a needless translation at read time. */
static void PCatDec(Str255 d,unsigned long v){char b[12];int n=0;char s[2];s[1]=0;
  if(!v){PCat(d,"0");return;}
  while(v&&n<11){b[n++]=(char)('0'+(int)(v%10));v/=10;}
  while(n){s[0]=b[--n];PCat(d,s);} }
static void Say1(const char*s,unsigned long v){Str255 L;L[0]=0;PCat(L,s);PCatDec(L,v);Out(L);}

static SsbBus gBus;

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

/* ---- ★★★★★ THE TEST: b43_phy_initb6's FIRST TWO STATEMENTS, IN ORDER. ---- */
static int Initb6Opening(void)
{
  UInt16 back,got;
  Say("");
  Say("  ⇒ b43_phy_initb6's opening, BOTH statements this time:");
  Say("      phy_g.c:1586  b43_phy_write(dev, 0x003E, 0x817A);");
  Say("      phy_g.c:1587  b43_radio_write16(dev, 0x007A, b43_radio_read16(dev,0x007A)|0x0058);");
  Say("    g1, g2, g3 and h1 ALL ran the second statement without the first. PHY 0x003E appears");
  Say("    EXACTLY ONCE in the entire driver -- here, on the line directly above the radio access");
  Say("    that has now aborted four times -- and it has no name in any b43 header.");
  Say("    -> writing PHY 0x003E = 0x817A");
  PhyWrite(0x003E,0x817A);
  back=PhyRead(0x003E);
  SayH("       PHY 0x003E reads back    = ",back,4);
  Say("    -> NOW the radio read: w 0x3F6 = 0x00FA (register 0x7A | read bit), then r 0x3FA");
  ssb_w16(gBus.bar0,B43_MMIO_RADIO_CONTROL,0x00FA);
  got=ssb_r16(gBus.bar0,B43_MMIO_RADIO_DATA_LOW);
  SayH("       RADIO 0x7A               = ",got,4);
  Say("       ==> ★★★ IT READ. The missing precondition was the PHY 0x003E write on the line");
  Say("           directly above, and four boots went to a one-line omission.");
  return 1;
}
static void PhyTakeOutOfReset(void)
{ UInt32 t;
  t=ssb_r32(gBus.bar0,SSB_TMSLOW);t&=~B43_TMSLOW_PHYRESET;t&=~B43_TMSLOW_PHYCLKEN;t|=SSB_TMSLOW_FGC;
  ssb_w32(gBus.bar0,SSB_TMSLOW,t);(void)ssb_r32(gBus.bar0,SSB_TMSLOW);SsbSpinUs(1500);
  t=ssb_r32(gBus.bar0,SSB_TMSLOW);t&=~SSB_TMSLOW_FGC;t|=B43_TMSLOW_PHYCLKEN;
  ssb_w32(gBus.bar0,SSB_TMSLOW,t);(void)ssb_r32(gBus.bar0,SSB_TMSLOW);SsbSpinUs(1500); }

int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    UInt32 flags,tmslow,macctl;
    UInt16 orig,got,want;
    UInt16 rOrig,rGot,rWithBit;   /* rNoBit is gone with the access that produced it */
    UInt16 hLo,hMi,hHi,hLo2,hMi2,hHi2;
    FwBlob ucode,pcm,iv,bsiv; int havePcm=0,haveBs=0;
    UInt32 w,words,irq=0;
    UInt32 bf=0,gm=0,gs=0; int ccOk=0;
    int oracleA=0,oracleB=0,oracleC=0,asymmetry=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+660;
    win=NewWindow(NULL,&bounds,"\pAirPort Accessors " AP_ACC_VER " - PHY/radio/SHM write paths",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT ACCESSOR LAYER " AP_ACC_VER " -- STAGE 4b-ii-1 ===");
    Say("  4b-ii is NOT one probe. Measured against source, the G-PHY init sequence is");
    Say("  phy_g.c 3057 + wa.c 376 + lo.c 1003 + tables.c 453 = 4889 lines, with no clean");
    Say("  hundred-line slice: initb6 calls channel_switch and set_txpower_g, and the 18-line");
    Say("  inita calls b43_wa_all(), an entire file. So the first increment is the ACCESSOR");
    Say("  LAYER those 4889 lines rest on.");
    Say("");
    Say("  4b-i proved PHY READS. Never tested: PHY writes, general radio reads, radio writes,");
    Say("  host flags. ⚠ A WRONG ACCESSOR DOES NOT FAULT -- it silently reads or writes the wrong");
    Say("  register and surfaces thousands of lines later as a radio that will not tune.");
    Say("");
    Say("  Oracle A: PHY write round-trip on PGACTL (read-proven by 4b-i, written by initg).");
    Say("  Oracle B: radio read+write round-trip on 0x7A, and the 0x80 read-bit asymmetry shown.");
    Say("  Oracle C: host flags via SHM 0x5E/0x60/0x62 -- two of three need the UNALIGNED path.");
    Say("  ⚠⚠ g1 (no firmware) and g2 (firmware + initvals) BOTH died at the identical");
    Say("     instruction: R4 = 0x3FA (RADIO_DATA_LOW), R5 = 0x7A. Same fault, same registers.");
    Say("     So g2 did not just fail -- it FALSIFIED its own hypothesis. \"Firmware + initvals\"");
    Say("     was not the missing precondition. A third bolt-on guess was not worth a boot.");
    Say("");
    Say("  g3 ports b43_chip_init's prefix IN FULL instead. Read end to end, chip_init is:");
    Say("     1 MACCTL   2 upload_microcode   3 gpio_init   4 upload_initvals");
    Say("     5 upload_initvals_band   6 switch_analog(1)   7 phy_init -> switch_analog,");
    Say("       software_rfkill(false), then ops->init -> initb6, whose FIRST act is radio 0x7A.");
    Say("  Against that list g2 ran 1, 2, 4 and 6. IT WAS MISSING THREE STEPS, NOT ONE:");
    Say("     3  gpio_init          -- 4b-i/f5 proved it on this card; g2 left it out on a");
    Say("                             stated rationale that chip_init contradicts outright.");
    Say("     5  initvals_band      -- g2 applied 'BCMi' 128 only; Stage 4a applied both.");
    Say("     7  software_rfkill(false) -- the radio power-on itself.");
    Say("  ⚠ A PCI master abort on an MMIO read is what an UNPOWERED RADIO looks like from the");
    Say("    host. Every write in the power-on path is a PHY write or a CHANNEL/CHANNEL_EXT");
    Say("    write -- not one is a radio access -- so it runs entirely over ports g1 proved");
    Say("    work bare. It adds no new risk; it is the strobe that makes 0x3FA answer at all.");
    Say("  ⚠ NO FRESH BOOT REQUIRED -- SsbCoreEnable does a real teardown first.");
    Say("");

    if(SsbFindAndMap(&gBus)!=noErr){Say("!! could not find/map the BCM4306");goto verdict;}
    if(SsbScanCores(&gBus)!=noErr){Say("!! core scan failed");goto verdict;}
    SayH("[1] BAR0 = ",(unsigned long)gBus.bar0,8);
    SayH("    chip = ",gBus.chipId,4);
    if(SsbSelectCore(&gBus,gBus.idx80211)!=noErr){Say("!! select failed");goto verdict;}

    flags=B43_TMSLOW_GMODE|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET;
    SsbCoreEnable(&gBus,flags);
    SsbSpinUs(2500);
    PhyTakeOutOfReset();
    tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
    SayH("    TMSLOW after enable = ",tmslow,8);
    if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){
      Say("!! core did not come up");goto verdict;}
    SayH("    PHY_VER sanity = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ⚠ switch_analog is DELIBERATELY NOT CALLED YET. Stage 4a never calls it at all and read
     * 0x3FA successfully; ladder A reproduces that state exactly so the control is a real
     * control, and ladder B then changes this one variable and nothing else. */
    Say("");
    Say("[CTL-A] control in minimal state -- exactly where Stage 4a read the radio successfully");
    (void)RadioIdControl();

    SwitchAnalog(1);
    Say("");
    Say("    switch_analog(1) done -- PHY0 (0x3E6) = 0x0000.");
    SayH("    PHY_VER after switch_analog = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ---- [1b] THE PRECONDITION g1 STRIPPED: firmware, then initvals, in b43's order ---- */
    Say("");
    Say("[1b] MICROCODE UPLOAD -- chip_init steps 1 and 2. Initvals are NOT here; they moved to");
    Say("     [1d], because chip_init puts gpio_init between the two and g2 did not.");
    if(!LoadFw('BCMu',128,'u',&ucode)){Say("!! ucode5 resource missing/bad");goto verdict;}
    if(!LoadFw('BCMi',128,'i',&iv))   {Say("!! initvals resource missing/bad");goto verdict;}
    havePcm = LoadFw('BCMp',128,'p',&pcm);
    haveBs  = LoadFw('BCMi',129,'i',&bsiv);   /* b0g0bsinitvals5 -- chip_init step 5 */
    macctl=B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_PSM_JMP0;
    ssb_w32(gBus.bar0,B43_MMIO_MACCTL,macctl);(void)ssb_r32(gBus.bar0,B43_MMIO_MACCTL);
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
    { UInt16 rev=ShmRead16Shared(0x0000UL);
      SayH("     ucode revision = ",(unsigned long)rev,4);
      if(irq!=0x00000001UL||rev==0||rev>0x128UL){
        Say("!! firmware did not come up -- the radio test below would be meaningless.");
        goto verdict; } }
    Say("     firmware running");

    /* ---- [1c] chip_init step 3: GPIO INIT. Between the microcode and the initvals. ---- */
    Say("");
    Say("[1c] GPIO INIT (b43_gpio_init) -- chip_init step 3, the first step g2 skipped");
    GpioInit(&bf,&gm,&gs,&ccOk);
    SayH("     board flags (SPROM +0x72) = ",bf,4);
    Say((bf&B43_BFL_PACTRL)
        ? "     PACTRL set -> GPIO 9 enabled so the ucode can drive the PA (as 4b-i found)"
        : "     PACTRL clear -> GPIO 9 branch not taken");
    SayH("     mask = ",gm,8);
    SayH("     set  = ",gs,8);
    Say(ccOk ? "     ChipCommon GPIO_CONTROL holds the masked bits -- matches 4b-i/f5"
             : "     ⚠ ChipCommon GPIO_CONTROL did NOT hold the masked bits; f5 says it should");
    SayH("     window back on the 802.11 core, TMSLOW = ",ssb_r32(gBus.bar0,SSB_TMSLOW),8);
    Say("       (that TMSLOW read is the check that matters -- if the window were still on");
    Say("        ChipCommon, every register below would silently be the wrong one.)");

    /* ---- [1d] chip_init steps 4 and 5: BOTH initvals tables. ---- */
    Say("");
    Say("[1d] INITVALS -- BOTH tables (chip_init steps 4 and 5)");
    { UInt32 n=ApplyIvs(iv.payload,iv.bytes);
      Say1("     b0g0initvals5   records applied = ",n);
      Say ("       (the blob header declares 317; Stage 4a established that only 35 of those");
      Say ("        offsets are readable at all, so record COUNT is the measurement, not readback.)"); }
    if(haveBs){ UInt32 n=ApplyIvs(bsiv.payload,bsiv.bytes);
      Say1("     b0g0bsinitvals5 records applied = ",n);
      Say ("       (header declares 31.)"); }
    else Say("     ⚠ b0g0bsinitvals5 ('BCMi' 129) ABSENT -- re-run ./embed-fw.sh. g2 ran without it.");
    SayH("     PHY_VER after = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER),4);

    /* ---- [1e] chip_init steps 6 and 7: switch_analog, then the radio power-on. ---- */
    Say("");
    Say("[1e] switch_analog(1), then b43_software_rfkill(dev,false) -- the step g1 and g2 both");
    Say("     lacked, and the one that most directly explains a faulting read of 0x3FA.");
    SwitchAnalog(1);
    RadioOn(1);                                   /* b43_gphy_op_get_default_chan() returns 1 */
    SayH("     PGACTL after the 0x8000 / 0xCC00 / 0x00C0 strobe = ",
         (unsigned long)PhyRead((UInt16)B43_PHY_PGACTL),4);
    SayH("     CHANNEL_EXT (0x3F4) = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_CHANNEL_EXT),4);
    Say("       CHANNEL_EXT is read-modify-written by channel_switch itself, so b43 treats it as");
    Say("       readable and this value is evidence.");
    SayH("     CHANNEL (0x3F0) = ",(unsigned long)ssb_r16(gBus.bar0,B43_MMIO_CHANNEL),4);
    Say("       ⚠ INFORMATIONAL ONLY. b43 never reads CHANNEL back, so by the Stage 4a");
    Say("       storage-versus-port rule this number gates nothing -- it is logged because a");
    Say("       surprise here is worth seeing, not because 0x000C would prove anything.");
    Say("");
    Say("[CTL-B] the same control again, now AFTER the full prefix -- does the prefix break it?");
    (void)RadioIdControl();
    (void)Initb6Opening();

    /* ---- [2] RADIO -- FIRST, and deliberately so.
     * g2 ran the PHY write test before the radio test. That ordering is now unsafe: Oracle A
     * flips B43_PHY_PGACTL_LOWBANDW, which is 0x0040 -- one of the two bits in the 0x00C0 that
     * software_rfkill just wrote to power the radio ON. Toggling part of the power-on value
     * immediately before the test that depends on it would make a failure ambiguous between the
     * radio path and Oracle A's own poking. initb6 reads radio 0x7A directly after
     * software_rfkill with nothing in between, and so does this. ---- */
    Say("");
    Say("[2] RADIO READ/WRITE -- register 0x7A, the first one initb6 touches");
    Say("    ⚠ THIS RUNS FIRST NOW. Oracle A flips PGACTL 0x0040, which is one of the two bits");
    Say("      in the 0x00C0 the radio power-on just wrote. initb6 reads radio 0x7A immediately");
    Say("      after software_rfkill with nothing in between; so does this.");
    Say("    ⚠ h2 DIED HERE, and not on the radio read -- that had already SUCCEEDED above.");
    Say("      It died on the next two lines: an 'asymmetry demonstration' that selected");
    Say("      register 0x7A with NO read bit and then read the data port. That is the SAME");
    Say("      access h1's ladder step (4) died on, and the same one h2's own header forbids.");
    Say("      h2 deleted LadderStep() and never grepped the rest of the file for the pattern.");
    Say("      It is removed in h3, not guarded -- the hardware master-aborts on it.");
    Say("");
    Say("    ★ THE ASYMMETRY IS ALREADY PROVEN, and by stronger evidence than that test could");
    Say("      ever have produced. h1 step (3) read selector 0x0080 (register 0x00 WITH the bit)");
    Say("      and got 0x0000 cleanly. h1 step (4) and h2 both selected 0x007A WITHOUT the bit");
    Say("      and took a master abort. A bus abort is not a different VALUE -- it is the data");
    Say("      port declining to drive at all. Nothing further needs demonstrating.");
    rWithBit=RadioRead((UInt16)kRadioTestReg);
    SayH("    read WITH 0x80 (correct) = ",rWithBit,4);
    asymmetry=1;   /* established by h1 step (3) vs h1 step (4)/h2, not by a probe here */
    rOrig=rWithBit;
    RadioWrite((UInt16)kRadioTestReg,(UInt16)(rOrig|0x0058));  /* exactly what initb6 does */
    rGot=RadioRead((UInt16)kRadioTestReg);
    SayH("    wrote orig|0x0058        = ",(unsigned long)(UInt16)(rOrig|0x0058),4);
    SayH("    read back                = ",rGot,4);
    RadioWrite((UInt16)kRadioTestReg,rOrig);
    { UInt16 rBack=RadioRead((UInt16)kRadioTestReg);
      SayH("    restored                 = ",rBack,4);
      if(rGot==(UInt16)(rOrig|0x0058) && rBack==rOrig){ oracleB=1;
        Say("    ==> ORACLE B PASSED: radio reads and writes both work through the 0x7A path.");
        Say("    ⇒ THE g1/g2/g3 CRASHES ARE EXPLAINED, AND *NOT* BY THE EXPLANATION g3 GAVE.");
        Say("      g3 asserted the radio was unpowered and that software_rfkill was the fix.");
        Say("      h2 falsified that: the prefix ran in full, software_rfkill included, and the");
        Say("      read still aborted -- until PHY 0x003E = 0x817A was written. That one line,");
        Say("      directly above the radio access in b43_phy_initb6, was the whole cause.");
      } else Say("    ✗ ORACLE B FAILED."); }

    /* ---- [3] PHY WRITE -- moved after the radio test; see the note at [2]. ---- */
    Say("");
    Say("[3] PHY WRITE ROUND-TRIP -- PGACTL (CCK 0x15)");
    Say("    Tests bit 0x1000 (LPF), NOT 0x0040 (LOWBANDW). 0x0040 is inside the 0x00C0 radio-on");
    Say("    value; picking a bit outside it keeps this test from disturbing radio power at all.");
    orig=PhyRead((UInt16)B43_PHY_PGACTL);
    SayH("    original            = ",orig,4);
    Say("      ⚠ if this reads 0x00C0, that is NOT evidence the power-on strobe did anything:");
    Say("        4b-i read PGACTL as 0x00C0 before any strobe had ever run. The strobe is a");
    Say("        SEQUENCE (0x8000, 0xCC00, 0x00C0) plus the channel writes, not a final value.");
    PhyWrite((UInt16)B43_PHY_PGACTL,orig);
    got=PhyRead((UInt16)B43_PHY_PGACTL);
    SayH("    rewrote own value   = ",got,4);
    want=(UInt16)(orig ^ B43_PHY_PGACTL_LPF);
    PhyWrite((UInt16)B43_PHY_PGACTL,want);
    got=PhyRead((UInt16)B43_PHY_PGACTL);
    SayH("    wrote               = ",want,4);
    SayH("    read back           = ",got,4);
    if(got==want) Say("      ==> the changed value took.");
    else          Say("      ✗ the changed value did NOT take.");
    PhyWrite((UInt16)B43_PHY_PGACTL,orig);
    { UInt16 back=PhyRead((UInt16)B43_PHY_PGACTL);
      SayH("    restored            = ",back,4);
      if(got==want && back==orig){ oracleA=1;
        Say("    ==> ORACLE A PASSED: PHY writes reach the register, the value reads back, and");
        Say("        the original restores exactly. PHY registers are storage, so this readback");
        Say("        is legitimate by the Stage 4a storage-versus-port rule.");
      } else Say("    ✗ ORACLE A FAILED."); }

    /* ---- [4] HOST FLAGS ---- */
    Say("");
    Say("[4] HOST FLAGS -- SHM 0x5E / 0x60 / 0x62, two of which are 2-mod-4");
    Say("    ⚠ 0x5E and 0x62 force the SHM_DATA_UNALIGNED path (0x166), which Stage 3b");
    Say("      deliberately did not implement because nothing needed it. It is needed now.");
    HfRead(&hLo,&hMi,&hHi);
    { Str255 L;L[0]=0;PCat(L,"    original = ");PCat(L,"hi 0x");PCatHex(L,hHi,4);
      PCat(L,"  mid 0x");PCatHex(L,hMi,4);PCat(L,"  lo 0x");PCatHex(L,hLo,4);Out(L); }
    HfWrite((UInt16)(hLo^0x0001),(UInt16)(hMi^0x0002),(UInt16)(hHi^0x0004));
    HfRead(&hLo2,&hMi2,&hHi2);
    { Str255 L;L[0]=0;PCat(L,"    after xor = ");PCat(L,"hi 0x");PCatHex(L,hHi2,4);
      PCat(L,"  mid 0x");PCatHex(L,hMi2,4);PCat(L,"  lo 0x");PCatHex(L,hLo2,4);Out(L); }
    { int ok = (hLo2==(UInt16)(hLo^0x0001)) && (hMi2==(UInt16)(hMi^0x0002)) && (hHi2==(UInt16)(hHi^0x0004));
      HfWrite(hLo,hMi,hHi);
      HfRead(&hLo2,&hMi2,&hHi2);
      { Str255 L;L[0]=0;PCat(L,"    restored  = ");PCat(L,"hi 0x");PCatHex(L,hHi2,4);
        PCat(L,"  mid 0x");PCatHex(L,hMi2,4);PCat(L,"  lo 0x");PCatHex(L,hLo2,4);Out(L); }
      if(ok && hLo2==hLo && hMi2==hMi && hHi2==hHi){ oracleC=1;
        Say("    ==> ORACLE C PASSED: all three words round-trip, including the two that go");
        Say("        through the unaligned port. Note the LOW and HIGH words are the unaligned");
        Say("        ones -- if only the MIDDLE word had worked, the unaligned path would be the");
        Say("        fault, and the log makes that distinguishable.");
      } else Say("    ✗ ORACLE C FAILED -- compare which of the three words survived."); }

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB&&oracleC){
      Say("  ✓✓✓ THE ACCESSOR LAYER IS PROVEN. PHY writes, radio reads and writes, and host flags");
      Say("      through the unaligned SHM path all round-trip and restore exactly.");
      Say("      ⇒ The 4889-line G-PHY port can now be written against primitives that are known");
      Say("        good, instead of debugging a mis-routed accessor from three steps downstream.");
      if(!asymmetry) Say("      (The 0x80 read-bit demonstration was inconclusive -- see [3].)");
    } else {
      Say("  ✗ NOT ALL PRIMITIVES ARE PROVEN. Do not start porting the PHY sequence: every failure");
      Say("    downstream would be ambiguous between the port and the accessor under it.");
      if(!oracleA) Say("    - PHY write failed.");
      if(!oracleB) Say("    - radio read/write failed.");
      if(!oracleC) Say("    - host flags failed; suspect the SHM unaligned path first.");
      if(!oracleB){
        Say("");
        Say("  ⚠⚠ IF THIS CRASHED AT 0x3FA A THIRD TIME, STOP ADDING STEPS. g3 is the complete");
        Say("     documented prefix -- all seven chip_init steps in order -- so a third fault");
        Say("     would mean the prefix is not the problem and the next move is MEASUREMENT,");
        Say("     not another port. The specific thing to measure: whether 0x3FA is decoded AT");
        Say("     ALL, by reading RADIO_DATA_HIGH (0x3F8) and the already-proven RADIOCTL_ID");
        Say("     identity path in the same run. Stage 4a read the radio ID successfully, so");
        Say("     SOME radio read works on this card; the question would then be what differs");
        Say("     between that path and this one -- which is a narrower question than any");
        Say("     amount of further porting can answer.");
      }
    }
    Say("");
    Say("  ⚠ Core left running WITH firmware loaded, initvals applied and the radio powered on.");
    Say("    Re-runnable immediately -- SsbCoreEnable tears down first.");
    Say("");
    Say("=== done. Log: 'AirPort Accessors Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
