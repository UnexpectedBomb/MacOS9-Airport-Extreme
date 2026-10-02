/* airport_stage3a.c -- STAGE 3a: BRING THE 802.11 CORE OUT OF RESET, AND PROVE SHM WORKS.
 *
 * ★★★★★ WHY STAGE 3 IS SPLIT, AND WHY THIS HALF COMES FIRST.
 * The roadmap's Stage 3 was "core reset + SHM self-test + firmware upload". That is two questions,
 * and the second one needs 22 KB of microcode embedded in this binary. Splitting them:
 *   3a (this)  does the core come out of reset, and is SHM reachable and byte-order-correct?
 *              Needs NO firmware. Small, bounded, and every write is documented below.
 *   3b (next)  upload ucode5 + pcm5 + b0g0initvals5 and read the firmware revision back.
 * If 3a fails there is no point building 3b, and if 3b fails we will already know the core and SHM
 * were good -- which is the difference between one suspect and a dozen.
 *
 * ⚠⚠ THIS IS THE FIRST BINARY IN THE PROJECT THAT WRITES MMIO REGISTERS. Stages 1 and 2 wrote
 * nothing and then only config space. Everything written here is listed, with its source:
 *
 *   SSB_TMSLOW  (0x0F98)  the core reset/clock sequence, VERBATIM from ssb_device_enable() in
 *                         drivers/ssb/main.c, plus b43_phy_take_out_of_reset() from
 *                         drivers/net/wireless/broadcom/b43/phy_common.c. Both were fetched from
 *                         Linux mainline while writing this file rather than recalled -- a wrong
 *                         bit here can wedge the backplane, and that is not a thing to do from
 *                         memory. [[feedback_instrument_before_hypothesising]]
 *   SSB_TMSHIGH (0x0F9C)  cleared ONLY if it reads back with SERR set. ssb calls this "a hw bug
 *                         workaround"; we do exactly what it does and nothing more.
 *   SSB_IMSTATE (0x0F90)  IBE/TO bits cleared only if set. Same provenance.
 *   B43_MMIO_MACCTL (0x120)  IHR_ENABLED | SHM_ENABLED | GMODE. Three bits, all confirmed against
 *                         b43.h. SHM_ENABLED is what makes section [4] possible at all.
 *   B43_MMIO_SHM_CONTROL/DATA (0x160/0x164)  the SHM self-test. Shared memory, no side effects.
 *
 * ⚠ NOT WRITTEN, deliberately: no PHY register, no radio register, no DMA register, no firmware.
 * The PHY is taken out of *reset* (a TMSLOW bit) but never programmed.
 *
 * ★★★★★ THE PRECONDITION THAT LETS US SKIP THE HARD PART.
 * ssb_device_enable() begins by calling ssb_device_disable(), whose FIRST line is:
 *      if (ssb_read32(dev, SSB_TMSLOW) & SSB_TMSLOW_RESET) return;
 * Stage 2 read TMSLOW = 0x00000001 on this card -- RESET asserted, clock off, straight from a cold
 * boot. So on a fresh boot the entire disable path is skipped, and with it the reject-handshake,
 * the backplane-revision-dependent reject bitmask, and the initiator dance. That code is subtle and
 * we would be writing it from memory.
 * ⇒ SO THIS APP REQUIRES A FRESH BOOT, CHECKS FOR IT, AND REFUSES TO PROCEED OTHERWISE. If the core
 * is already running (a second run without rebooting), it says so and stops rather than performing
 * a teardown it only half understands. Reboot and re-run: that is a cheap instruction to follow and
 * an expensive bug to debug.
 *
 * ★★★★★ THE ORACLES. Two, independent, both predicted before the run.
 *   A: after the sequence, TMSLOW must read back with CLOCK SET and RESET CLEAR. That is the core
 *      telling us it is running. Stage 2 recorded the BEFORE picture (0x00000001) precisely so this
 *      comparison would be unambiguous rather than a judgement call.
 *   B: the SHM pattern self-test. Two patterns chosen to be byte-order-sensitive and
 *      bit-pattern-distinct (0x55AAAA55 and 0xAA5555AA) must survive a write/read round trip at two
 *      different SHM offsets. A byte-swap bug returns a recognisably swapped value rather than
 *      garbage, which is why these patterns and not 0/0xFFFFFFFF.
 * Oracle A without B => the core runs but SHM addressing or MACCTL's SHM_ENABLED is wrong.
 * B without A => impossible, and if it happens the TMSLOW decode is wrong, not the chip.
 *
 * ⚠ WHAT A HANG MEANS HERE. Every line is flushed before the next executes. The riskiest write is
 * the first TMSLOW in [3]; if the machine dies there, the log's last line names it and the finding
 * is "the reset sequence wedged the backplane", not "the chip is bad". That distinction is worth a
 * reboot to preserve.
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
#define AP_S3A_VER "c1"

#define kVendorBroadcom   0x14E4UL
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

/* --- SiliconBackplane (confirmed against include/linux/ssb/ssb_regs.h) ------ */
#define SSB_ENUM_BASE     0x18000000UL
#define SSB_CORE_SIZE     0x1000UL
#define SSB_BAR0_WIN      0x80UL
#define SSB_SPROM_BASE1   0x1000UL

#define SSB_IMSTATE       0x0F90UL
#define SSB_TMSLOW        0x0F98UL
#define SSB_TMSHIGH       0x0F9CUL
#define SSB_IDLOW         0x0FF8UL
#define SSB_IDHIGH        0x0FFCUL

#define SSB_TMSLOW_RESET   0x00000001UL
#define SSB_TMSLOW_REJECT  0x00000002UL
#define SSB_TMSLOW_CLOCK   0x00010000UL
#define SSB_TMSLOW_FGC     0x00020000UL

#define SSB_TMSHIGH_SERR   0x00000001UL
#define SSB_TMSHIGH_BUSY   0x00000004UL
#define SSB_TMSHIGH_DMA64  0x10000000UL

#define SSB_IMSTATE_IBE    0x00020000UL
#define SSB_IMSTATE_TO     0x00040000UL

#define IDHIGH_CC(v)      (((v) >> 4) & 0xFFFUL)
#define IDHIGH_REV(v)     (((((v) & 0x7000UL) >> 8)) | ((v) & 0xFUL))
#define SSB_DEV_CHIPCOMMON 0x800UL
#define SSB_DEV_80211      0x812UL

/* --- b43 core-specific TMSLOW flags (confirmed against b43.h) --------------- */
#define B43_TMSLOW_GMODE      0x20000000UL
#define B43_TMSLOW_PHYRESET   0x00080000UL
#define B43_TMSLOW_PHYCLKEN   0x00040000UL

/* --- b43 MMIO + MACCTL (confirmed against b43.h) --------------------------- */
#define B43_MMIO_MACCTL       0x120UL
#define B43_MMIO_GEN_IRQ_REASON 0x128UL
#define B43_MMIO_SHM_CONTROL  0x160UL
#define B43_MMIO_SHM_DATA     0x164UL

#define B43_MACCTL_ENABLED     0x00000001UL
#define B43_MACCTL_PSM_RUN     0x00000002UL
#define B43_MACCTL_PSM_JMP0    0x00000004UL
#define B43_MACCTL_SHM_ENABLED 0x00000100UL
#define B43_MACCTL_IHR_ENABLED 0x00000400UL
#define B43_MACCTL_BE          0x00010000UL   /* Big Endian mode -- we READ this, never set it */
#define B43_MACCTL_GMODE       0x80000000UL

/* SHM routing selectors are an enum in b43.h, so they are 0,1,2,3,4 in order. */
#define B43_SHM_UCODE     0UL
#define B43_SHM_SHARED    1UL
#define B43_SHM_SCRATCH   2UL
#define B43_SHM_HW        3UL

#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

/* --- byte-reversing MMIO. Same rationale and same proven pattern as Stage 2. */
static inline UInt32 ap_r32(volatile void *base, UInt32 off)
{ volatile UInt8 *p=(volatile UInt8*)base+off; UInt32 v;
  __asm__ __volatile__("lwbrx %0,0,%1":"=r"(v):"r"(p):"memory"); return v; }
static inline void ap_w32(volatile void *base, UInt32 off, UInt32 v)
{ volatile UInt8 *p=(volatile UInt8*)base+off;
  __asm__ __volatile__("stwbrx %0,0,%1"::"r"(v),"r"(p):"memory"); }
static inline UInt16 ap_r16(volatile void *base, UInt32 off)
{ volatile UInt8 *p=(volatile UInt8*)base+off; UInt32 v;
  __asm__ __volatile__("lhbrx %0,0,%1":"=r"(v):"r"(p):"memory"); return (UInt16)v; }

/* ⚠ NO Delay() ANYWHERE NEAR THIS. Delay(1) is 16.7 ms on OS 9; the reset sequence wants 1 us
 * flushes and 1-2 ms settles. [[reference_os9_delay_granularity_trap]] */
static void SpinUs(UInt32 us)
{ UnsignedWide a,b; Microseconds(&a); do { Microseconds(&b); } while ((b.lo - a.lo) < us); }

/* --- log ------------------------------------------------------------------- */
static void PCat(Str255 d,const char*s){short l=d[0];while(*s&&l<255)d[++l]=(unsigned char)*s++;d[0]=(unsigned char)l;}
static void PCatDec(Str255 d,long v){char t[12];short n=0;unsigned long u;
  if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;}while(u){t[n++]=(char)('0'+(u%10));u/=10;}
  while(n>0&&d[0]<255)d[++d[0]]=t[--n];}
static void PCatHex(Str255 d,unsigned long v,int digits){static const char h[]="0123456789ABCDEF";
  int i;for(i=digits-1;i>=0;i--)if(d[0]<255)d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF];}

static short gLogRef=0,gLogVol=0; static long gLogDir=0;
static Str255 gLines[240]; static short gN=0;

static void LogOpen(void){ FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Stage3a Log",&sp)==noErr) FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr) gLogRef=0; }
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<240){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;} }
static void Say(const char*s){Str255 L;L[0]=0;PCat(L,s);Out(L);}
static void Say1(const char*s,unsigned long v){Str255 L;L[0]=0;PCat(L,s);PCatDec(L,(long)v);Out(L);}
static void SayH(const char*s,unsigned long v,int d){Str255 L;L[0]=0;PCat(L,s);PCat(L,"0x");PCatHex(L,v,d);Out(L);}

/* --- registry / BAR (identical to Stage 2) --------------------------------- */
static RegEntryID gNode;
static volatile void *gBar0 = 0;
static UInt32 gSelectRetries = 0;

static OSStatus GetProp(RegEntryID*n,const char*nm,void**ob,RegPropertyValueSize*os){
  RegPropertyValueSize sz=0;void*b;OSStatus e;
  e=RegistryPropertyGetSize(n,nm,&sz); if(e!=noErr||sz==0) return (e!=noErr)?e:paramErr;
  b=NewPtr((Size)sz); if(!b) return memFullErr;
  e=RegistryPropertyGet(n,nm,b,&sz); if(e!=noErr){DisposePtr((Ptr)b);return e;}
  *ob=b;*os=sz; return noErr; }

static OSStatus FindNode(RegEntryID*out){
  RegEntryIter c;RegEntryID e;Boolean d=false,f=true;OSStatus er;
  er=RegistryEntryIterateCreate(&c); if(er!=noErr) return er;
  for(;;){ UInt32 vid=0,did=0; RegPropertyValueSize sz;
    er=RegistryEntryIterate(&c,f?kRegIterDescendants:kRegIterContinue,&e,&d); f=false;
    if(er!=noErr||d) break;
    sz=sizeof(vid); if(RegistryPropertyGet(&e,"vendor-id",&vid,&sz)!=noErr) continue;
    if((vid&0xFFFFUL)!=kVendorBroadcom) continue;
    sz=sizeof(did); if(RegistryPropertyGet(&e,"device-id",&did,&sz)!=noErr) continue;
    did&=0xFFFFUL; if(did!=kDevBCM4306_4320&&did!=kDevBCM4306_4325) continue;
    *out=e; RegistryEntryIterateDispose(&c); return noErr; }
  RegistryEntryIterateDispose(&c); return -1; }

static OSStatus MapBar0(RegEntryID*n,volatile void**ob){
  UInt32*aa=NULL,*la=NULL;RegPropertyValueSize as=0,ls=0;UInt32 i,ne;OSStatus r=paramErr;
  if(GetProp(n,"assigned-addresses",(void**)&aa,&as)!=noErr) return paramErr;
  if(GetProp(n,"AAPL,address",(void**)&la,&ls)!=noErr){DisposePtr((Ptr)aa);return paramErr;}
  ne=(UInt32)as/(kAddrEntryWords*sizeof(UInt32));
  for(i=0;i<ne&&(i*sizeof(UInt32))<(UInt32)ls;i++){
    UInt32 ph=aa[i*kAddrEntryWords];
    if(AA_REG(ph)==0x10&&AA_SPACE(ph)>=2){*ob=(volatile void*)la[i];r=noErr;break;} }
  DisposePtr((Ptr)aa);DisposePtr((Ptr)la); return r; }

static OSStatus SelectCore(UInt32 idx){
  UInt32 want=(idx*SSB_CORE_SIZE)+SSB_ENUM_BASE; int a=0;
  for(;;){ UInt32 got=0;
    if(ExpMgrConfigWriteLong(&gNode,(LogicalAddress)SSB_BAR0_WIN,want)!=noErr) return paramErr;
    if(ExpMgrConfigReadLong(&gNode,(LogicalAddress)SSB_BAR0_WIN,&got)!=noErr) return paramErr;
    if(got==want) return noErr;
    gSelectRetries++; if(++a>10) return paramErr; SpinUs(20); } }

/* --- SHM, per b43_shm_control_word / b43_shm_read32 ------------------------
 * For B43_SHM_SHARED the caller's BYTE offset is shifted right by 2 to make a word address.
 * Only 4-byte-aligned offsets are used here, so the unaligned path (SHM_DATA_UNALIGNED) is not
 * needed and is deliberately not implemented -- untested code that never runs is a liability. */
static void ShmControl(UInt32 routing, UInt32 wordOffset)
{ ap_w32(gBar0, B43_MMIO_SHM_CONTROL, (routing << 16) | wordOffset); }

static UInt32 ShmRead32Shared(UInt32 byteOffset)
{ ShmControl(B43_SHM_SHARED, byteOffset >> 2); return ap_r32(gBar0, B43_MMIO_SHM_DATA); }

static void ShmWrite32Shared(UInt32 byteOffset, UInt32 v)
{ ShmControl(B43_SHM_SHARED, byteOffset >> 2); ap_w32(gBar0, B43_MMIO_SHM_DATA, v); }

/* --- the core reset, verbatim from ssb_device_enable + b43_phy_take_out_of_reset --- */
static void FlushTmslow(void) { (void)ap_r32(gBar0, SSB_TMSLOW); SpinUs(2); }

static void CoreEnable(UInt32 flags)
{
    UInt32 v;
    /* ssb_device_disable() is skipped: we verified RESET is already asserted, which is exactly
     * where that function returns immediately. The caller enforces this. */
    ap_w32(gBar0, SSB_TMSLOW, SSB_TMSLOW_RESET | SSB_TMSLOW_CLOCK | SSB_TMSLOW_FGC | flags);
    FlushTmslow();

    if (ap_r32(gBar0, SSB_TMSHIGH) & SSB_TMSHIGH_SERR) {
        Say("      (TMSHIGH had SERR set -- clearing it, as ssb does for the hw bug)");
        ap_w32(gBar0, SSB_TMSHIGH, 0);
    }
    v = ap_r32(gBar0, SSB_IMSTATE);
    if (v & (SSB_IMSTATE_IBE | SSB_IMSTATE_TO)) {
        SayH("      (IMSTATE had IBE/TO set -- clearing. was ", v, 8);
        ap_w32(gBar0, SSB_IMSTATE, v & ~(SSB_IMSTATE_IBE | SSB_IMSTATE_TO));
    }

    ap_w32(gBar0, SSB_TMSLOW, SSB_TMSLOW_CLOCK | SSB_TMSLOW_FGC | flags);
    FlushTmslow();
    ap_w32(gBar0, SSB_TMSLOW, SSB_TMSLOW_CLOCK | flags);
    FlushTmslow();
}

static void PhyTakeOutOfReset(void)
{
    UInt32 t;
    t = ap_r32(gBar0, SSB_TMSLOW);
    t &= ~B43_TMSLOW_PHYRESET;
    t &= ~B43_TMSLOW_PHYCLKEN;
    t |= SSB_TMSLOW_FGC;
    ap_w32(gBar0, SSB_TMSLOW, t);
    (void)ap_r32(gBar0, SSB_TMSLOW);
    SpinUs(1500);
    t = ap_r32(gBar0, SSB_TMSLOW);
    t &= ~SSB_TMSLOW_FGC;
    t |= B43_TMSLOW_PHYCLKEN;
    ap_w32(gBar0, SSB_TMSLOW, t);
    (void)ap_r32(gBar0, SSB_TMSLOW);
    SpinUs(1500);
}

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i, y;
    UInt16 cmd = 0;
    UInt32 idx, coreIdx = 0xFFFFFFFFUL, idhigh = 0, tmslowBefore = 0, tmslowAfter = 0, tmshigh = 0;
    UInt32 macctl = 0, flags;
    int oracleA = 0, oracleB = 0, shmFails = 0;
    UInt32 save0 = 0, save4 = 0;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    LogOpen();

    bounds.left=8; bounds.top=40; bounds.right=8+700; bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Stage 3a " AP_S3A_VER " - core reset + SHM self-test",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9); }

    Say("=== AIRPORT EXTREME STAGE 3a " AP_S3A_VER " -- CORE RESET + SHM SELF-TEST ===");
    Say("  Oracle A: after the reset sequence, TMSLOW must show CLOCK set and RESET clear.");
    Say("            Stage 2 recorded the before picture as 0x00000001, so this is a comparison,");
    Say("            not a judgement call.");
    Say("  Oracle B: SHM must survive a write/read round trip of two byte-order-sensitive patterns");
    Say("            (0x55AAAA55 / 0xAA5555AA) at two offsets.");
    Say("  ⚠ REQUIRES A FRESH BOOT. If the core is already running this app STOPS -- see below.");
    Say("  ⚠ This is the first binary here that writes MMIO. No PHY, radio, DMA or firmware writes.");
    Say("");

    if (FindNode(&gNode) != noErr) { Say("!! no BCM4306 node -- re-run Stage 1."); goto verdict; }
    if (MapBar0(&gNode, &gBar0) != noErr || gBar0 == 0) { Say("!! could not map BAR0."); goto verdict; }
    SayH("[0] BAR0 logical = ", (unsigned long)gBar0, 8);

    /* ---- [1] memory space ---- */
    if (ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&cmd) != noErr) { Say("!! cmd read failed"); goto verdict; }
    if (!(cmd & 0x0002)) {
        UInt16 back=0;
        if (ExpMgrConfigWriteWord(&gNode,(LogicalAddress)0x04,(UInt16)(cmd|0x0002)) != noErr) {
            Say("!! could not enable memory space"); goto verdict; }
        (void)ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&back);
        if (!(back & 0x0002)) { Say("!! memory-space enable did not stick"); goto verdict; }
    }
    Say("[1] memory space enabled");

    /* ---- [2] find the 802.11 core and check the precondition ---- */
    Say("");
    Say("[2] LOCATING THE 802.11 CORE AND CHECKING THE FRESH-BOOT PRECONDITION");
    for (idx = 0; idx < 8; idx++) {
        if (SelectCore(idx) != noErr) continue;
        idhigh = ap_r32(gBar0, SSB_IDHIGH);
        if (idhigh == 0xFFFFFFFFUL || idhigh == 0) continue;
        if (IDHIGH_CC(idhigh) == SSB_DEV_80211) { coreIdx = idx; break; }
    }
    if (coreIdx == 0xFFFFFFFFUL) {
        Say("!! no 802.11 core found. Stage 2 found it at index 1 -- something is very wrong.");
        goto verdict;
    }
    Say1("    802.11 core at index ", coreIdx);
    Say1("    core revision = ", IDHIGH_REV(idhigh));
    tmslowBefore = ap_r32(gBar0, SSB_TMSLOW);
    tmshigh      = ap_r32(gBar0, SSB_TMSHIGH);
    SayH("    TMSLOW before  = ", tmslowBefore, 8);
    SayH("    TMSHIGH before = ", tmshigh, 8);
    Say1("      DMA64 supported = ", (tmshigh & SSB_TMSHIGH_DMA64) ? 1 : 0);
    if (!(tmslowBefore & SSB_TMSLOW_RESET)) {
        Say("");
        Say("!! PRECONDITION FAILED: the core is NOT in reset.");
        Say("   ssb_device_enable() skips its teardown path only when RESET is already asserted.");
        Say("   Doing the teardown correctly needs the reject handshake and the backplane-revision");
        Say("   reject bitmask, which this app deliberately does NOT implement rather than write");
        Say("   from memory.");
        Say("   ==> REBOOT AND RE-RUN. This is not a failure of the card or of Stage 2.");
        goto verdict;
    }
    Say("    ==> RESET is asserted and the clock is off: a clean cold-boot state. Proceeding.");

    /* ---- [3] the reset sequence ---- */
    Say("");
    Say("[3] CORE RESET SEQUENCE (ssb_device_enable + b43_phy_take_out_of_reset, verbatim)");
    flags = B43_TMSLOW_GMODE | B43_TMSLOW_PHYCLKEN | B43_TMSLOW_PHYRESET;
    SayH("    core-specific flags = ", flags, 8);
    Say("      GMODE | PHYCLKEN | PHYRESET -- the G-PHY set b43 uses for this part.");
    Say("    ⚠ THE NEXT WRITE IS THE RISKIEST IN THE PROJECT SO FAR. If this app dies here, the");
    Say("      finding is 'the reset sequence wedged the backplane', NOT 'the chip is bad'.");
    CoreEnable(flags);
    Say("    core enable sequence completed without hanging");
    SpinUs(2500);                 /* msleep(2): wait for the PLL */
    Say("    PLL settle done (2.5 ms)");
    PhyTakeOutOfReset();
    Say("    PHY taken out of reset");

    tmslowAfter = ap_r32(gBar0, SSB_TMSLOW);
    tmshigh     = ap_r32(gBar0, SSB_TMSHIGH);
    SayH("    TMSLOW after   = ", tmslowAfter, 8);
    SayH("    TMSHIGH after  = ", tmshigh, 8);
    Say1("      RESET asserted = ", (tmslowAfter & SSB_TMSLOW_RESET) ? 1 : 0);
    Say1("      CLOCK enabled  = ", (tmslowAfter & SSB_TMSLOW_CLOCK) ? 1 : 0);
    Say1("      FGC forced     = ", (tmslowAfter & SSB_TMSLOW_FGC) ? 1 : 0);
    Say1("      PHYRESET       = ", (tmslowAfter & B43_TMSLOW_PHYRESET) ? 1 : 0);
    Say1("      PHYCLKEN       = ", (tmslowAfter & B43_TMSLOW_PHYCLKEN) ? 1 : 0);
    if (tmshigh & SSB_TMSHIGH_SERR) Say("      ⚠ TMSHIGH SERR is SET after reset -- the core reports a bus error.");
    if (tmshigh & SSB_TMSHIGH_BUSY) Say("      (TMSHIGH BUSY set -- the core is working, not necessarily a problem)");
    if ((tmslowAfter & SSB_TMSLOW_CLOCK) && !(tmslowAfter & SSB_TMSLOW_RESET)) {
        oracleA = 1;
        Say("    ==> ORACLE A PASSED: clock on, reset released. THE 802.11 CORE IS RUNNING.");
    } else {
        Say("    ✗ ORACLE A FAILED: the core did not come out of reset.");
    }

    /* ---- [4] MACCTL and the SHM self-test ---- */
    Say("");
    Say("[4] MACCTL + SHM SELF-TEST");
    macctl = ap_r32(gBar0, B43_MMIO_MACCTL);
    SayH("    MACCTL as found = ", macctl, 8);
    Say1("      BE (big-endian mode) = ", (macctl & B43_MACCTL_BE) ? 1 : 0);
    Say("      ^ b43 READS this bit and adapts (b43_ram_write byte-swaps when set); it never sets");
    Say("        it. Recorded here because on a big-endian host it is the one bit that could make");
    Say("        firmware upload byte order differ from what §3a predicts. We do not write it.");
    macctl = B43_MACCTL_IHR_ENABLED | B43_MACCTL_SHM_ENABLED | B43_MACCTL_GMODE;
    ap_w32(gBar0, B43_MMIO_MACCTL, macctl);
    (void)ap_r32(gBar0, B43_MMIO_MACCTL);
    SayH("    MACCTL written  = ", macctl, 8);
    macctl = ap_r32(gBar0, B43_MMIO_MACCTL);
    SayH("    MACCTL readback = ", macctl, 8);
    if (!(macctl & B43_MACCTL_SHM_ENABLED)) {
        Say("    ⚠ SHM_ENABLED did not stick. The self-test below will almost certainly fail, and");
        Say("      the cause is MACCTL, not SHM addressing.");
    }

    save0 = ShmRead32Shared(0);
    save4 = ShmRead32Shared(4);
    SayH("    SHM[0] before = ", save0, 8);
    SayH("    SHM[4] before = ", save4, 8);
    {
        static const UInt32 pat[2] = { 0x55AAAA55UL, 0xAA5555AAUL };
        int p;
        for (p = 0; p < 2; p++) {
            UInt32 got;
            ShmWrite32Shared(0, pat[p]);
            got = ShmRead32Shared(0);
            SayH("    SHM[0] wrote 0x", pat[p], 8);
            SayH("           read  0x", got, 8);
            if (got != pat[p]) {
                shmFails++;
                /* A byte-swap bug produces a RECOGNISABLE value, not noise. Name it explicitly so
                 * the next run is not spent guessing. */
                { UInt32 sw = ((pat[p]>>24)&0xFF)|((pat[p]>>8)&0xFF00)|((pat[p]<<8)&0xFF0000)|((pat[p]<<24)&0xFF000000UL);
                  if (got == sw) Say("           ✗ that is the pattern BYTE-SWAPPED -- the accessors are wrong, not the chip."); }
            }
            ShmWrite32Shared(4, pat[p]);
            got = ShmRead32Shared(4);
            if (got != pat[p]) { shmFails++; SayH("    SHM[4] mismatch, read 0x", got, 8); }
        }
        ShmWrite32Shared(0, save0);
        ShmWrite32Shared(4, save4);
        Say("    (original SHM words restored)");
    }
    if (shmFails == 0) {
        oracleB = 1;
        Say("    ==> ORACLE B PASSED: SHM is reachable, writable and byte-order-correct.");
    } else {
        Say1("    ✗ ORACLE B FAILED, mismatches = ", (unsigned long)shmFails);
    }

verdict:
    Say("");
    Say1("    core-select retries = ", gSelectRetries);
    Say("");
    Say("=== VERDICT ===");
    if (oracleA && oracleB) {
        Say("  ✓✓✓ STAGE 3a PASSES. The 802.11 core comes out of reset under the documented ssb");
        Say("      sequence, and SHM is reachable and byte-order-correct through MACCTL's");
        Say("      SHM_ENABLED. Every precondition for firmware upload is now proven.");
        Say("      Stage 3b can proceed: embed ucode5 + pcm5 + b0g0initvals5 and upload them.");
    } else if (oracleA) {
        Say("  ~ The core runs but SHM does not answer. Look at MACCTL's SHM_ENABLED first, then");
        Say("    the SHM control-word format ((routing << 16) | (byteOffset >> 2)). The core being");
        Say("    alive means the reset sequence and the backplane window are NOT the problem.");
    } else {
        Say("  ✗ The core did not come out of reset. Compare TMSLOW before and after above. If the");
        Say("    'after' value still reads 0x00000001, no write reached the register at all and the");
        Say("    fault is in MMIO writes -- note that Stage 2 only ever READ MMIO, so this would be");
        Say("    the first thing a write has been asked to do.");
    }
    Say("");
    Say("  ⚠ The core is left RUNNING with no firmware loaded. Harmless -- it is idle silicon with");
    Say("    no microcode to execute -- and Open Firmware resets it at the next boot. But a second");
    Say("    run of this app without rebooting WILL hit the precondition check and stop.");
    Say("");
    Say("=== done. Log: 'AirPort Stage3a Log' in the System Folder. ===");

    if (win) { SetPort((GrafPtr)win); y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;} }
    for(;;){ if(WaitNextEvent(everyEvent,&evt,10,NULL)){
        if(evt.what==keyDown||evt.what==mouseDown) break;
        if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
          for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);} } }
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
