/* airport_reset.c -- CAN THE 802.11 CORE BE CYCLED WITHOUT A COLD BOOT?
 *
 * ★★★★★ WHY THIS IS ITS OWN RUN.
 * Every probe since Stage 3a has required a FRESH BOOT, because they all skip
 * ssb_device_disable()'s teardown and are only correct when RESET is already asserted. That was
 * the right call while the teardown was going to be written from memory -- it is subtle code, and
 * a probe that half-tears-down a running core is worse than one that refuses. But it cost FIVE
 * BOOTS on Stage 4b-i alone, and 4b-ii ports a ~3000-line PHY init sequence that will iterate
 * harder than 4b-i did. The arithmetic has flipped.
 *
 * ssb_core.h now implements the real path -- the reject handshake, the backplane-revision-dependent
 * reject bitmask, the initiator dance -- ported verbatim from drivers/ssb/main.c. This run tests
 * ONLY that: nothing else, so a failure cannot be anything else.
 *
 * ⚠ THIS PROBE DOES NOT REQUIRE A FRESH BOOT. That is the entire point. Run it twice in a row
 * without rebooting; it should behave identically both times.
 *
 * ★★★★★ THE ORACLES.
 *   A: STATE. Across N cycles, every disable must leave TMSLOW with RESET asserted, and every
 *      enable must leave it with CLOCK set and RESET clear. Mechanical, and it catches a teardown
 *      that silently does nothing.
 *   B: FUNCTION -- and this is the one that matters. After the LAST cycle, reading PHY register
 *      0x0000 (B43_PHY_VERSION_CCK) through the banked PHY port must return the same version word
 *      that MMIO PHY_VER at 0x3E0 reports. Stage 4b-i established both read 0x2202.
 *      ⇒ Oracle A only proves the RESET BIT TOGGLED. Oracle B proves the core is genuinely alive
 *        after a software-only reset cycle -- that the PHY is clocked, the analog section is on,
 *        and the indirect register path still works. A core can report "reset released" and still
 *        be useless; this is the difference, and it is exactly the distinction Stage 4a's initvals
 *        readback failed to make and Stage 4b-i's oracle finally got right.
 *
 * ⚠ WHAT THIS DOES NOT DO: no firmware, no initvals, no GPIO. Cycling the core is the question.
 * Loading firmware on each cycle would make a failure ambiguous between "the teardown is wrong"
 * and "something in the reload is wrong", which is the mistake Stage 4b-i kept making by widening
 * a narrow probe.
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
#include "ssb_core.h"

#define kFontIDMonaco 4
#define AP_RST_VER "r1"
#define kCycles 4

/* b43-specific MMIO the backplane header does not own. */
#define B43_MMIO_MACCTL           0x120UL
#define B43_MMIO_PHY_VER          0x3E0UL
#define B43_MMIO_PHY0             0x3E6UL   /* switch_analog: 0 = on, 0xF4 = off */
#define B43_MMIO_PHY_CONTROL      0x3FCUL
#define B43_MMIO_PHY_DATA         0x3FEUL
#define B43_TMSLOW_GMODE          0x20000000UL
#define B43_TMSLOW_PHYRESET       0x00080000UL
#define B43_TMSLOW_PHYCLKEN       0x00040000UL
#define B43_PHY_VERSION_CCK       0x0000UL   /* CCK(0x00); base bank, no routing bits */

static void PCat(Str255 d,const char*s){short l=d[0];while(*s&&l<255)d[++l]=(unsigned char)*s++;d[0]=(unsigned char)l;}
static void PCatDec(Str255 d,long v){char t[12];short n=0;unsigned long u;
  if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;}while(u){t[n++]=(char)('0'+(u%10));u/=10;}
  while(n>0&&d[0]<255)d[++d[0]]=t[--n];}
static void PCatHex(Str255 d,unsigned long v,int digits){static const char h[]="0123456789ABCDEF";
  int i;for(i=digits-1;i>=0;i--)if(d[0]<255)d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF];}

static short gLogRef=0,gLogVol=0; static long gLogDir=0;
static Str255 gLines[200]; static short gN=0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Reset Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<200){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}
static void Say(const char*s){Str255 L;L[0]=0;PCat(L,s);Out(L);}
static void Say1(const char*s,unsigned long v){Str255 L;L[0]=0;PCat(L,s);PCatDec(L,(long)v);Out(L);}
static void SayH(const char*s,unsigned long v,int d){Str255 L;L[0]=0;PCat(L,s);PCat(L,"0x");PCatHex(L,v,d);Out(L);}

static SsbBus gBus;

static UInt16 PhyRead(UInt16 reg)
{
    ssb_w16(gBus.bar0, B43_MMIO_PHY_CONTROL, reg);
    return ssb_r16(gBus.bar0, B43_MMIO_PHY_DATA);
}
static void SwitchAnalog(int on){ ssb_w16(gBus.bar0,B43_MMIO_PHY0,(UInt16)(on?0x0000:0x00F4)); }

static void PhyTakeOutOfReset(void)
{
    UInt32 t;
    t=ssb_r32(gBus.bar0,SSB_TMSLOW); t&=~B43_TMSLOW_PHYRESET; t&=~B43_TMSLOW_PHYCLKEN; t|=SSB_TMSLOW_FGC;
    ssb_w32(gBus.bar0,SSB_TMSLOW,t); (void)ssb_r32(gBus.bar0,SSB_TMSLOW); SsbSpinUs(1500);
    t=ssb_r32(gBus.bar0,SSB_TMSLOW); t&=~SSB_TMSLOW_FGC; t|=B43_TMSLOW_PHYCLKEN;
    ssb_w32(gBus.bar0,SSB_TMSLOW,t); (void)ssb_r32(gBus.bar0,SSB_TMSLOW); SsbSpinUs(1500);
}

int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    UInt32 flags, tmslow, phyver=0, phycck=0, tmshigh;
    int cyc, stateFails=0, disableFails=0, oracleA=0, oracleB=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+640;
    win=NewWindow(NULL,&bounds,"\pAirPort Core Reset Cycle " AP_RST_VER " - no reboot required",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT CORE RESET CYCLE " AP_RST_VER " ===");
    Say("  Question: can the 802.11 core be torn down and brought back WITHOUT a cold boot?");
    Say("  Oracle A: every disable leaves RESET asserted; every enable leaves CLOCK set and RESET");
    Say("            clear. Mechanical -- catches a teardown that silently does nothing.");
    Say("  Oracle B: after the last cycle, PHY register 0x0000 read through the banked port must");
    Say("            equal MMIO PHY_VER. A core can report 'reset released' and still be useless;");
    Say("            this proves it is genuinely alive, not just that a bit toggled.");
    Say("  ⚠ THIS PROBE DOES NOT NEED A FRESH BOOT. Run it twice in a row -- that is the point.");
    Say("  No firmware, no initvals, no GPIO: cycling the core is the only question asked.");
    Say("");

    if(SsbFindAndMap(&gBus)!=noErr){Say("!! could not find/map the BCM4306");goto verdict;}
    SayH("[1] BAR0 = ",(unsigned long)gBus.bar0,8);
    if(SsbScanCores(&gBus)!=noErr){Say("!! core scan failed");goto verdict;}
    SayH("    chip id = ",gBus.chipId,4);
    Say1("    chip rev = ",gBus.chipRev);
    Say1("    cores = ",gBus.nCores);
    Say1("    802.11 core index = ",gBus.idx80211);
    if(SsbSelectCore(&gBus,gBus.idx80211)!=noErr){Say("!! select failed");goto verdict;}

    tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
    SayH("    TMSLOW as found = ",tmslow,8);
    Say((tmslow&SSB_TMSLOW_RESET)?"      (core in reset -- a cold boot, or a previous run left it so)"
                                 :"      (core RUNNING -- so cycle 1 exercises the real teardown)");
    { UInt32 idlow=ssb_r32(gBus.bar0,SSB_IDLOW);
      SayH("    IDLOW = ",idlow,8);
      SayH("      backplane revision field = ",idlow&SSB_IDLOW_SSBREV,8);
      Say1("      this core is a backplane INITIATOR = ",(idlow&SSB_IDLOW_INITIATOR)?1:0);
      SayH("      reject bitmask chosen for it = ",SsbRejectBitmask(&gBus),8);
      Say("      ^ which TMSLOW bit means 'reject' depends on the backplane revision. Getting it");
      Say("        wrong means the handshake never completes, which is why this is printed."); }

    flags = B43_TMSLOW_GMODE | B43_TMSLOW_PHYCLKEN | B43_TMSLOW_PHYRESET;

    /* Bring it up once, so that cycle 1's disable has a RUNNING core to tear down. Starting from
     * reset would let ssb_device_disable() return immediately and test nothing. */
    Say("");
    Say("[2] PRIMING: one enable, so cycle 1 tears down a core that is actually running");
    SsbCoreEnable(&gBus,flags);
    SsbSpinUs(2500);
    PhyTakeOutOfReset();
    SwitchAnalog(1);
    tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
    SayH("    TMSLOW = ",tmslow,8);
    if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){
      Say("!! priming enable failed; nothing to cycle.");goto verdict;}

    Say("");
    Say1("[3] CYCLING THE CORE, times = ",kCycles);
    for(cyc=1;cyc<=kCycles;cyc++){
      Str255 L; int rc;
      L[0]=0;PCat(L,"  -- cycle ");PCatDec(L,cyc);Out(L);

      rc = SsbCoreDisable(&gBus,flags);
      tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
      tmshigh=ssb_r32(gBus.bar0,SSB_TMSHIGH);
      L[0]=0;PCat(L,"     disable rc=");PCatDec(L,rc);
      PCat(L,"  TMSLOW=0x");PCatHex(L,tmslow,8);
      PCat(L,"  TMSHIGH=0x");PCatHex(L,tmshigh,8);Out(L);
      if(rc!=0){ disableFails++;
        Say("     ⚠ a wait timed out inside the teardown. rc -1/-2 = TMSLOW reject or TMSHIGH busy;");
        Say("       rc -3 = the initiator IMSTATE busy wait. The core is in an unknown state."); }
      if(!(tmslow&SSB_TMSLOW_RESET)){ stateFails++;
        Say("     ✗ RESET is NOT asserted after disable -- the teardown did not take."); }

      SsbCoreEnable(&gBus,flags);
      SsbSpinUs(2500);
      PhyTakeOutOfReset();
      SwitchAnalog(1);
      tmslow=ssb_r32(gBus.bar0,SSB_TMSLOW);
      L[0]=0;PCat(L,"     enable      TMSLOW=0x");PCatHex(L,tmslow,8);
      PCat(L,"  CLOCK=");PCatDec(L,(tmslow&SSB_TMSLOW_CLOCK)?1:0);
      PCat(L,"  RESET=");PCatDec(L,(tmslow&SSB_TMSLOW_RESET)?1:0);Out(L);
      if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){ stateFails++;
        Say("     ✗ the core did not come back up."); }

      /* Functional check EVERY cycle, not just the last -- if the core degrades after repeated
       * cycling, the cycle it degrades on is the interesting datum. */
      phyver=(UInt32)ssb_r16(gBus.bar0,B43_MMIO_PHY_VER);
      phycck=(UInt32)PhyRead((UInt16)B43_PHY_VERSION_CCK);
      L[0]=0;PCat(L,"     PHY_VER=0x");PCatHex(L,phyver,4);
      PCat(L,"  VERSION_CCK=0x");PCatHex(L,phycck,4);
      PCat(L,(phycck==phyver&&phyver!=0&&phyver!=0xFFFF)?"   MATCH":"   MISMATCH");Out(L);
    }

    if(stateFails==0&&disableFails==0) oracleA=1;
    if(phycck==phyver&&phyver!=0&&phyver!=0xFFFF) oracleB=1;

    Say("");
    Say1("    state failures   = ",(unsigned long)stateFails);
    Say1("    disable timeouts = ",(unsigned long)disableFails);
    Say1("    core-select retries = ",gBus.selectRetries);

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB){
      Say("  ✓✓✓ THE CORE CAN BE CYCLED IN SOFTWARE. ssb_device_disable() works: the reject");
      Say("      handshake completes, the core returns to reset, comes back up, and is still");
      Say("      FUNCTIONAL afterwards -- the PHY port agrees with MMIO on the version word.");
      Say("      ⇒ Probes no longer need a cold boot between runs. 4b-ii can iterate.");
    } else if(oracleA){
      Say("  ~ The state bits cycle correctly but the PHY check failed. The core says it is back;");
      Say("    it is not usable. Suspect PhyTakeOutOfReset or switch_analog ordering after a");
      Say("    software teardown -- NOT the teardown itself, which evidently moved the bits.");
    } else if(oracleB){
      Say("  ~ The PHY works but a state check failed somewhere. Read the per-cycle lines: the");
      Say("    cycle number where it first went wrong is the whole diagnosis.");
    } else {
      Say("  ✗ Cycling does not work. Cold boots remain necessary. The per-cycle log shows whether");
      Say("    the teardown failed to assert RESET (the reject bitmask or the handshake) or the");
      Say("    re-enable failed to clear it.");
    }
    Say("");
    Say("  ⚠ The core is left RUNNING with no firmware. Harmless, and this probe can be re-run");
    Say("    immediately -- that is what it exists to demonstrate.");
    Say("");
    Say("=== done. Log: 'AirPort Reset Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
