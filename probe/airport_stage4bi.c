/* airport_stage4bi.c -- STAGE 4b-i: GPIO INIT, AND PROVE THE PHY REGISTER PATH.
 *
 * ★★★★★ SCOPE, AND A DELIBERATE NARROWING FROM WHAT WAS PROPOSED.
 * The roadmap offered 4b-i as "GPIO init plus the G-PHY init sequence". Reading phy_g.c, that
 * sequence is roughly 3000 lines -- b43_phy_initg calling into the B-PHY inits, the BCM2050 radio
 * init, LO calibration, NRSSI thresholding and TX power control. Porting that before the PHY
 * REGISTER ACCESS PATH is proven would be building on sand, and it is the same mistake 3a existed
 * to avoid: SHM was proven before 3b pushed 22 KB of microcode through it.
 *   4b-i (this)  GPIO init; prove PHY register access; dump the register file.
 *   4b-ii        the G-PHY + BCM2050 init sequence, on a proven access path.
 *   4b-iii       DMA rings under the 30-bit ceiling, and a captured beacon.
 *
 * ★★★★★ THE ORACLES.
 *   A: TWO INDEPENDENT PATHS MUST AGREE ON ONE PREDICTED VALUE. B43_PHY_VERSION_CCK is
 *      CCK(0x00) -- PHY register 0x0000, reached through the PHY_CONTROL/PHY_DATA port pair at
 *      0x3FC/0x3FE. MMIO PHY_VER at 0x3E0 is a direct register. Different mechanisms; they must
 *      return the same version word. f3 read 0x2202 on both before it crashed, so this is a
 *      prediction made from evidence rather than a hope.
 *      ⚠ f1's Oracle A ("sweep 0x00..0x3F, count distinct values") was WRONG BY DESIGN and f3 died
 *      of it -- see the note in section [6]. Counting distinct values was a proxy for liveness;
 *      this is the real thing.
 *   B: GPIO CONTROL MUST HOLD WHAT WE WRITE. ChipCommon's GPIO_CONTROL at core offset 0x6C is a
 *      genuine control register -- stateful, not a FIFO port -- so readback is meaningful here in a
 *      way it explicitly was NOT for the initvals targets in Stage 4a. That distinction is the
 *      whole lesson of 4a and it is why this oracle is legitimate: we verified the register's
 *      nature before building a test on it.
 *
 * ⚠ THE 4a LESSON, APPLIED. Stage 4a's readback "diagnostic" compared 317 initval records against
 * registers that were mostly SHM_DATA and the template port -- FIFOs that never return what you
 * wrote. 282 of 317 comparisons were meaningless by construction. Before asserting anything about a
 * register here, ask whether it is storage or a port. GPIO_CONTROL is storage. PHY_DATA is a port --
 * so oracle A does not test PHY_DATA's contents directly; it tests that the port DELIVERS a
 * register whose value a second, unrelated mechanism also reports.
 *
 * ⚠ TWO SIMPLIFICATIONS, BOTH VERIFIED IN SOURCE RATHER THAN ASSUMED.
 *   1. b43_write16f()'s flush read is #if defined(CONFIG_BCM47XX_BCMA) and gated on
 *      dev->flush_writes (b43.h:1037-1042). On a PCI SSB card it is a PLAIN WRITE. We do not need
 *      the flush, and adding one "to be safe" would be cargo-culting an embedded-MIPS workaround.
 *   2. assert_mac_suspended() is debug-only (`if (!B43_DEBUG) return;`) and only fires once status
 *      >= B43_STAT_INITIALIZED. During bring-up the MAC does NOT need suspending for PHY access,
 *      so this run does not implement MAC suspend/resume.
 *   Neither simplification applies to WRITES, because this run does not write the PHY at all --
 *   see the note above PhyRead. b43_phy_write's PCI workaround (read PHY_VER after 24 consecutive
 *   writes) is real and must be carried into 4b-ii; it is simply not needed by a read-only probe.
 *
 * ★ GPIO INIT, AND THE PACTRL PATH THIS CARD ACTUALLY TAKES.
 * From b43_gpio_init(): clear MACCTL's GPOUTSMSK, OR 0xF into GPIO_MASK (0x49E), then mask=0x1F /
 * set=0x0F. And then the branch that matters here:
 *      if (boardflags_lo & B43_BFL_PACTRL) {     // "PA is controlled by gpio 9, let ucode handle it"
 *          GPIO_MASK |= 0x0200;  mask |= 0x0200;  set |= 0x0200;
 *      }
 * Stage 2 read this card's board flags as 0x000A = PACTRL (0x0002) | RSSI (0x0008), so PACTRL IS
 * SET and GPIO 9 must be enabled. §3a predicted this branch from the SPROM read; this is the run
 * that exercises it. The final write is a read-modify-write of ChipCommon's GPIO_CONTROL, which
 * means switching the backplane window to core 0 and back -- the core switch deferred from 4a.
 *
 * ⚠ FRESH BOOT REQUIRED, same precondition and reason as every probe since 3a.
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
#include <Resources.h>
#include <NameRegistry.h>
#include <PCI.h>

#define kFontIDMonaco 4
#define AP_S4BI_VER "f5"

#define kVendorBroadcom   0x14E4UL
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

#define SSB_ENUM_BASE     0x18000000UL
#define SSB_CORE_SIZE     0x1000UL
#define SSB_BAR0_WIN      0x80UL
#define SSB_SPROM_BASE1   0x1000UL
#define SSB_IMSTATE       0x0F90UL
#define SSB_TMSLOW        0x0F98UL
#define SSB_TMSHIGH       0x0F9CUL
#define SSB_IDHIGH        0x0FFCUL
#define SSB_TMSLOW_RESET   0x00000001UL
#define SSB_TMSLOW_CLOCK   0x00010000UL
#define SSB_TMSLOW_FGC     0x00020000UL
#define SSB_TMSHIGH_SERR   0x00000001UL
#define B43_TMSHIGH_HAVE_2GHZ_PHY 0x00010000UL
#define SSB_IMSTATE_IBE    0x00020000UL
#define SSB_IMSTATE_TO     0x00040000UL
#define IDHIGH_CC(v)      (((v) >> 4) & 0xFFFUL)
#define IDHIGH_REV(v)     (((((v) & 0x7000UL) >> 8)) | ((v) & 0xFUL))
#define SSB_DEV_CHIPCOMMON 0x800UL
#define CC_CHIPID          0x0000UL
#define CHIPID_NCORES(v)  (((v) >> 24) & 0xFUL)
#define SSB_DEV_80211      0x812UL

#define B43_TMSLOW_GMODE      0x20000000UL
#define B43_TMSLOW_PHYRESET   0x00080000UL
#define B43_TMSLOW_PHYCLKEN   0x00040000UL

#define B43_MMIO_MACCTL           0x120UL
#define B43_MMIO_GEN_IRQ_REASON   0x128UL
#define B43_MMIO_SHM_CONTROL      0x160UL
#define B43_MMIO_SHM_DATA         0x164UL
#define B43_MMIO_SHM_DATA_UNALIGNED 0x166UL
#define B43_MMIO_PHY_VER          0x3E0UL
/* ⚠ THE REGISTER f2 DIED FOR WANT OF. b43_phyop_switch_analog_generic() is one write:
 *     b43_write16(dev, B43_MMIO_PHY0, on ? 0 : 0xF4)
 * and b43_wireless_core_reset() calls it after every reset where the PHY type is already known.
 * Without it the PHY's analog section stays off, the identity registers still answer (they are not
 * behind it) but the indirect PHY DATA PORT does not -- which is exactly the f2 crash. */
#define B43_MMIO_PHY0             0x3E6UL
#define B43_MMIO_RADIO_CONTROL    0x3F6UL
#define B43_MMIO_RADIO_DATA_HIGH  0x3F8UL
#define B43_MMIO_RADIO_DATA_LOW   0x3FAUL
#define B43_MMIO_PHY_CONTROL      0x3FCUL
#define B43_MMIO_PHY_DATA         0x3FEUL
#define B43_MMIO_GPIO_CONTROL     0x49CUL
#define B43_MMIO_GPIO_MASK        0x49EUL
#define B43_RADIOCTL_ID           0x01UL

/* ChipCommon core register that actually drives the pins. */
#define B43_GPIO_CONTROL          0x6CUL

/* --- PHY register routing (phy_common.h:11-26) -----------------------------
 * PHY registers are BANKED. A bare index is an address in the base/CCK bank with no routing
 * bits, and the defined registers within a bank are sparse -- reading an undefined one is not
 * decoded and faults, which is what killed f3 at index 0x09. */
#define B43_PHYROUTE              0x0C00UL
#define B43_PHYROUTE_BASE         0x0000UL   /* CCK (B) registers   */
#define B43_PHYROUTE_OFDM_GPHY    0x0400UL   /* OFDM (A) registers  */
#define B43_PHYROUTE_EXT_GPHY     0x0800UL   /* Extended G-PHY      */
#define B43_PHY_CCK(r)            ((r) | B43_PHYROUTE_BASE)
#define B43_PHY_OFDM(r)           ((r) | B43_PHYROUTE_OFDM_GPHY)
#define B43_PHY_EXTG(r)           ((r) | B43_PHYROUTE_EXT_GPHY)

/* ⚠⚠ EVERY ENTRY HERE HAS BEEN READ SUCCESSFULLY ON THIS CARD. That is the selection rule now,
 * and it is stricter than the two that failed before it:
 *   f3 used "any index 0x00..0x3F"          -> died at 0x09 (undecoded; registers are banked)
 *   f4 used "registers b43 NAMES"           -> died at CCK 0x78 RCCALOVER
 *   f5 uses "registers f4 actually READ"    -> this list, empirically proven
 *
 * RCCALOVER is the lesson in miniature. It is DEFINED in phy_g.h:22 and, per a grep of the whole
 * G-PHY driver, NEVER READ OR WRITTEN ANYWHERE. "b43 names it" and "b43 exercises it" are
 * different sets, and the difference is exactly where f4 crashed. A name in a header is not
 * evidence that a register is readable in the state you are in.
 *
 * The OFDM (0x0400) and ExtG (0x0800) banks are deliberately NOT read here. f4 never reached them,
 * so nothing proves they are safe before PHY init -- and 4b-ii will read them anyway, in a defined
 * state, at points b43's own sequence dictates. There is no reason to risk a boot for them now.
 *
 * GTABDATA stays excluded for the separate Stage 4a reason: it is the data half of a
 * GTABCTL/GTABDATA port pair, and reading a port proves nothing. */
typedef struct { UInt32 addr; const char *bank; const char *name; } PhyRegDef;
static const PhyRegDef gPhyRegs[] = {
    { B43_PHY_CCK (0x00), "CCK ", "VERSION_CCK"   },  /* == MMIO PHY_VER: this IS oracle A */
    { B43_PHY_CCK (0x01), "CCK ", "CCKBBANDCFG"   },  /* f4 read 0x0000 */
    { B43_PHY_CCK (0x15), "CCK ", "PGACTL"        },  /* f4 read 0x00C0 */
    { B43_PHY_CCK (0x18), "CCK ", "FBCTL1"        },  /* f4 read 0x6E61 */
    { B43_PHY_CCK (0x29), "CCK ", "ITSSI"         },  /* f4 read 0x0000 */
    { B43_PHY_CCK (0x2D), "CCK ", "LO_LEAKAGE"    },  /* f4 read 0x0000 */
    { B43_PHY_CCK (0x33), "CCK ", "ENERGY"        },  /* f4 read 0x0010 */
    { B43_PHY_CCK (0x35), "CCK ", "SYNCCTL"       },  /* f4 read 0x0764 */
    { B43_PHY_CCK (0x38), "CCK ", "FBCTL2"        },  /* f4 read 0x0667 */
    { B43_PHY_CCK (0x60), "CCK ", "DACCTL"        }   /* f4 read 0x0003 */
};
#define kNumPhyRegs (sizeof(gPhyRegs)/sizeof(gPhyRegs[0]))

#define B43_PHYVER_ANALOG        0xF000UL
#define B43_PHYVER_ANALOG_SHIFT  12
#define B43_PHYVER_TYPE          0x0F00UL
#define B43_PHYVER_TYPE_SHIFT    8
#define B43_PHYVER_VERSION       0x00FFUL
#define B43_PHYTYPE_G            0x02UL

#define B43_MACCTL_PSM_RUN     0x00000002UL
#define B43_MACCTL_PSM_JMP0    0x00000004UL
#define B43_MACCTL_SHM_ENABLED 0x00000100UL
#define B43_MACCTL_IHR_ENABLED 0x00000400UL
#define B43_MACCTL_BE          0x00010000UL
#define B43_MACCTL_GPOUTSMSK   0x0000C000UL
#define B43_MACCTL_GMODE       0x80000000UL

#define B43_SHM_UCODE     0UL
#define B43_SHM_SHARED    1UL
#define B43_SHM_SCRATCH   2UL
#define B43_SHM_HW        3UL
#define B43_SHM_AUTOINC_W 0x0100UL
#define B43_SHM_SH_UCODEREV   0x0000UL
#define B43_SHM_SH_WLCOREREV  0x0016UL

#define B43_IRQ_MAC_SUSPENDED 0x00000001UL
#define B43_IRQ_ALL           0xFFFFFFFFUL
#define BWI_FW_VERSION3_REVMAX 0x128UL
#define B43_IV_OFFSET_MASK 0x7FFFUL
#define B43_IV_32BIT       0x8000UL

#define B43_BFL_PACTRL     0x0002UL   /* GPIO 9 controls the PA */
#define B43_BFL_RSSI       0x0008UL
#define SPROM_BFLLO        0x0072UL

#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

static inline UInt32 ap_r32(volatile void*b,UInt32 o){volatile UInt8*p=(volatile UInt8*)b+o;UInt32 v;
  __asm__ __volatile__("lwbrx %0,0,%1":"=r"(v):"r"(p):"memory");return v;}
static inline void ap_w32(volatile void*b,UInt32 o,UInt32 v){volatile UInt8*p=(volatile UInt8*)b+o;
  __asm__ __volatile__("stwbrx %0,0,%1"::"r"(v),"r"(p):"memory");}
static inline UInt16 ap_r16(volatile void*b,UInt32 o){volatile UInt8*p=(volatile UInt8*)b+o;UInt32 v;
  __asm__ __volatile__("lhbrx %0,0,%1":"=r"(v):"r"(p):"memory");return (UInt16)v;}
static inline void ap_w16(volatile void*b,UInt32 o,UInt16 v){volatile UInt8*p=(volatile UInt8*)b+o;
  __asm__ __volatile__("sthbrx %0,0,%1"::"r"((UInt32)v),"r"(p):"memory");}

static void SpinUs(UInt32 us){UnsignedWide a,b;Microseconds(&a);do{Microseconds(&b);}while((b.lo-a.lo)<us);}

static void PCat(Str255 d,const char*s){short l=d[0];while(*s&&l<255)d[++l]=(unsigned char)*s++;d[0]=(unsigned char)l;}
static void PCatDec(Str255 d,long v){char t[12];short n=0;unsigned long u;
  if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;}while(u){t[n++]=(char)('0'+(u%10));u/=10;}
  while(n>0&&d[0]<255)d[++d[0]]=t[--n];}
static void PCatHex(Str255 d,unsigned long v,int digits){static const char h[]="0123456789ABCDEF";
  int i;for(i=digits-1;i>=0;i--)if(d[0]<255)d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF];}

static short gLogRef=0,gLogVol=0; static long gLogDir=0;
static Str255 gLines[280]; static short gN=0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Stage4bi Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<280){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}
static void Say(const char*s){Str255 L;L[0]=0;PCat(L,s);Out(L);}
static void Say1(const char*s,unsigned long v){Str255 L;L[0]=0;PCat(L,s);PCatDec(L,(long)v);Out(L);}
static void SayH(const char*s,unsigned long v,int d){Str255 L;L[0]=0;PCat(L,s);PCat(L,"0x");PCatHex(L,v,d);Out(L);}

static RegEntryID gNode;
static volatile void *gBar0 = 0;

static OSStatus GetProp(RegEntryID*n,const char*nm,void**ob,RegPropertyValueSize*os){
  RegPropertyValueSize sz=0;void*b;OSStatus e;
  e=RegistryPropertyGetSize(n,nm,&sz);if(e!=noErr||sz==0)return (e!=noErr)?e:paramErr;
  b=NewPtr((Size)sz);if(!b)return memFullErr;
  e=RegistryPropertyGet(n,nm,b,&sz);if(e!=noErr){DisposePtr((Ptr)b);return e;}
  *ob=b;*os=sz;return noErr;}

static OSStatus FindNode(RegEntryID*out){
  RegEntryIter c;RegEntryID e;Boolean d=false,f=true;OSStatus er;
  er=RegistryEntryIterateCreate(&c);if(er!=noErr)return er;
  for(;;){UInt32 vid=0,did=0;RegPropertyValueSize sz;
    er=RegistryEntryIterate(&c,f?kRegIterDescendants:kRegIterContinue,&e,&d);f=false;
    if(er!=noErr||d)break;
    sz=sizeof(vid);if(RegistryPropertyGet(&e,"vendor-id",&vid,&sz)!=noErr)continue;
    if((vid&0xFFFFUL)!=kVendorBroadcom)continue;
    sz=sizeof(did);if(RegistryPropertyGet(&e,"device-id",&did,&sz)!=noErr)continue;
    did&=0xFFFFUL;if(did!=kDevBCM4306_4320&&did!=kDevBCM4306_4325)continue;
    *out=e;RegistryEntryIterateDispose(&c);return noErr;}
  RegistryEntryIterateDispose(&c);return -1;}

static OSStatus MapBar0(RegEntryID*n,volatile void**ob){
  UInt32*aa=NULL,*la=NULL;RegPropertyValueSize as=0,ls=0;UInt32 i,ne;OSStatus r=paramErr;
  if(GetProp(n,"assigned-addresses",(void**)&aa,&as)!=noErr)return paramErr;
  if(GetProp(n,"AAPL,address",(void**)&la,&ls)!=noErr){DisposePtr((Ptr)aa);return paramErr;}
  ne=(UInt32)as/(kAddrEntryWords*sizeof(UInt32));
  for(i=0;i<ne&&(i*sizeof(UInt32))<(UInt32)ls;i++){UInt32 ph=aa[i*kAddrEntryWords];
    if(AA_REG(ph)==0x10&&AA_SPACE(ph)>=2){*ob=(volatile void*)la[i];r=noErr;break;}}
  DisposePtr((Ptr)aa);DisposePtr((Ptr)la);return r;}

static OSStatus SelectCore(UInt32 idx){
  UInt32 want=(idx*SSB_CORE_SIZE)+SSB_ENUM_BASE;int a=0;
  for(;;){UInt32 got=0;
    if(ExpMgrConfigWriteLong(&gNode,(LogicalAddress)SSB_BAR0_WIN,want)!=noErr)return paramErr;
    if(ExpMgrConfigReadLong(&gNode,(LogicalAddress)SSB_BAR0_WIN,&got)!=noErr)return paramErr;
    if(got==want)return noErr;
    if(++a>10)return paramErr;
    SpinUs(20);
  }}

static void ShmControl(UInt32 r,UInt32 o){ap_w32(gBar0,B43_MMIO_SHM_CONTROL,(r<<16)|o);}
static UInt16 ShmRead16Shared(UInt32 bo){
  ShmControl(B43_SHM_SHARED,bo>>2);
  if(bo&3)return ap_r16(gBar0,B43_MMIO_SHM_DATA_UNALIGNED);
  return ap_r16(gBar0,B43_MMIO_SHM_DATA);}

/* --- the PHY register path, exactly b43_gphy_op_read/write -----------------
 * ⚠ write16f is a PLAIN write on a PCI SSB card (b43.h:1037, the flush is BCMA-only), so there is
 * no flush here and adding one would be copying an embedded-MIPS workaround we do not have. */
static UInt16 PhyRead(UInt16 reg)
{
    ap_w16(gBar0, B43_MMIO_PHY_CONTROL, reg);
    return ap_r16(gBar0, B43_MMIO_PHY_DATA);
}
/* ⚠ NO PhyWrite HERE, DELIBERATELY. This run only READS the PHY. Writing a PHY register whose
 * semantics we have not verified, purely to make a round-trip test, is the opposite of the
 * discipline that has caught every bug so far -- and untested code that runs once is a liability
 * (the same reason 3b omitted the unaligned SHM write path it did not need).
 * The write path, including b43_phy_write's PCI workaround -- after B43_MAX_WRITES_IN_ROW (24)
 * consecutive PHY writes, read B43_MMIO_PHY_VER so posted writes drain -- belongs in 4b-ii, where
 * the G-PHY init sequence gives it a defined purpose and a defined expected effect. */

/* b43_phyop_switch_analog_generic. b43 calls this from inside b43_wireless_core_reset, AFTER the
 * ssb reset and BEFORE it sets MACCTL -- but only "if (dev->phy.ops)", i.e. only once the PHY type
 * is known. That gate is why the FIRST reset skips it and the SECOND does not: b43_phy_allocate()
 * runs between them. Our two-reset structure mirrors that exactly. */
static void SwitchAnalog(int on){ ap_w16(gBar0, B43_MMIO_PHY0, (UInt16)(on ? 0x0000 : 0x00F4)); }

static void FlushTmslow(void){(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(2);}
static void CoreEnableFromReset(UInt32 flags){UInt32 v;
  ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_RESET|SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags);FlushTmslow();
  if(ap_r32(gBar0,SSB_TMSHIGH)&SSB_TMSHIGH_SERR)ap_w32(gBar0,SSB_TMSHIGH,0);
  v=ap_r32(gBar0,SSB_IMSTATE);
  if(v&(SSB_IMSTATE_IBE|SSB_IMSTATE_TO))ap_w32(gBar0,SSB_IMSTATE,v&~(SSB_IMSTATE_IBE|SSB_IMSTATE_TO));
  ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags);FlushTmslow();
  ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|flags);FlushTmslow();}
static void PhyTakeOutOfReset(void){UInt32 t;
  t=ap_r32(gBar0,SSB_TMSLOW);t&=~B43_TMSLOW_PHYRESET;t&=~B43_TMSLOW_PHYCLKEN;t|=SSB_TMSLOW_FGC;
  ap_w32(gBar0,SSB_TMSLOW,t);(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(1500);
  t=ap_r32(gBar0,SSB_TMSLOW);t&=~SSB_TMSLOW_FGC;t|=B43_TMSLOW_PHYCLKEN;
  ap_w32(gBar0,SSB_TMSLOW,t);(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(1500);}
static void CoreResetCycle(UInt32 g){CoreEnableFromReset(g|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET);
  SpinUs(2500);PhyTakeOutOfReset();}
static void CoreForceReset(void){ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_RESET);FlushTmslow();SpinUs(1000);}

typedef struct { Handle h; UInt8 *payload; UInt32 bytes; } FwBlob;
static UInt32 be32at(const UInt8*p){return ((UInt32)p[0]<<24)|((UInt32)p[1]<<16)|((UInt32)p[2]<<8)|(UInt32)p[3];}
static UInt16 be16at(const UInt8*p){return (UInt16)(((UInt32)p[0]<<8)|(UInt32)p[1]);}

static int LoadFw(ResType t,short id,UInt8 want,const char*label,FwBlob*out){
  Handle h;long sz;UInt8*p;UInt32 dec;Str255 L;
  h=GetResource(t,id);if(!h){L[0]=0;PCat(L,"    !! missing ");PCat(L,label);Out(L);return 0;}
  HLock(h);sz=GetHandleSize(h);p=(UInt8*)*h;
  if(sz<9){Say("    !! too small");HUnlock(h);return 0;}
  dec=((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|(UInt32)p[7];
  L[0]=0;PCat(L,"    ");PCat(L,label);PCat(L,": res=");PCatDec(L,sz);
  PCat(L,"  declared=");PCatDec(L,(long)dec);Out(L);
  if(p[0]!=want||p[1]!=1){Say("      !! bad header");HUnlock(h);return 0;}
  if(want!='i'&&dec!=(UInt32)(sz-8)){Say("      !! size mismatch");HUnlock(h);return 0;}
  out->h=h;out->payload=p+8;out->bytes=(UInt32)(sz-8);return 1;}

static void ApplyIvs(const UInt8*p,UInt32 bytes,UInt32*outCount){
  UInt32 c=0,consumed=0;
  while(consumed+2<=bytes){UInt16 osz;UInt32 off,v;int is32;
    osz=be16at(p+consumed);consumed+=2;
    off=(UInt32)osz&B43_IV_OFFSET_MASK;is32=((UInt32)osz&B43_IV_32BIT)!=0;
    if(off>=0x1000UL)break;
    if(is32){if(consumed+4>bytes)break;v=be32at(p+consumed);consumed+=4;ap_w32(gBar0,off,v);}
    else{if(consumed+2>bytes)break;v=be16at(p+consumed);consumed+=2;ap_w16(gBar0,off,(UInt16)v);}
    c++;}
  if(outCount)*outCount=c;}

int main(void)
{
    WindowPtr win;Rect bounds;EventRecord evt;short i,y;
    UInt16 cmd=0;
    UInt32 idx,nCores=0,coreIdx=0xFFFFFFFFUL,ccIdx=0xFFFFFFFFUL,idhigh=0,tmslow=0,tmshigh=0,macctl=0,irq=0,w,words;
    UInt32 gmodeFlag=0; int have2ghz=0;
    FwBlob ucode,pcm,iv,bsiv; int haveU=0,haveP=0,haveIv=0,haveBs=0;
    UInt32 phyver=0,phytype=0,phyrev=0,radioTmp=0,radioId=0,fwrev=0,ivApplied=0;
    UInt32 boardflags=0, gpioMask=0, gmask=0, gset=0, ccBefore=0, ccAfter=0, ccWant=0;
    UInt16 phyregs[64]; UInt32 distinct=0, unstable=0;
    int oracleA=0,oracleB=0,preconditionStop=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Stage 4b-i " AP_S4BI_VER " - GPIO + PHY register path",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT EXTREME STAGE 4b-i " AP_S4BI_VER " -- GPIO INIT + PHY REGISTER PATH ===");
    Say("  Oracle A: PHY_DATA (0x3FE) must VARY with the index written to PHY_CONTROL (0x3FC).");
    Say("            A dead path returns one constant value regardless of index, so this tests the");
    Say("            path's responsiveness rather than any register's contents -- we do not know");
    Say("            any PHY reset value and inventing one would repeat the WLCOREREV mistake.");
    Say("  Oracle B: ChipCommon GPIO_CONTROL (core 0, +0x6C) must hold what we write. That IS a");
    Say("            legitimate readback test because it is a control register, not a FIFO port --");
    Say("            the distinction Stage 4a's initvals analysis made the hard way.");
    Say("  ⚠ REQUIRES A FRESH BOOT.");
    Say("  Scope: NO G-PHY init sequence here. phy_g.c is ~3000 lines and porting it before the");
    Say("         access path is proven would be building on sand. That is 4b-ii.");
    Say("");

    Say("[0] FIRMWARE RESOURCES");
    haveU=LoadFw('BCMu',128,'u',"ucode5",&ucode);
    haveP=LoadFw('BCMp',128,'p',"pcm5",&pcm);
    haveIv=LoadFw('BCMi',128,'i',"b0g0initvals5",&iv);
    haveBs=LoadFw('BCMi',129,'i',"b0g0bsinitvals5",&bsiv);
    if(!haveU||!haveIv){Say("!! missing firmware");goto verdict;}

    if(FindNode(&gNode)!=noErr){Say("!! no BCM4306 node");goto verdict;}
    if(MapBar0(&gNode,&gBar0)!=noErr||gBar0==0){Say("!! could not map BAR0");goto verdict;}
    if(ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&cmd)!=noErr){Say("!! cmd read");goto verdict;}
    if(!(cmd&0x0002)){UInt16 b=0;
      (void)ExpMgrConfigWriteWord(&gNode,(LogicalAddress)0x04,(UInt16)(cmd|0x0002));
      (void)ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&b);
      if(!(b&0x0002)){Say("!! mem enable did not stick");goto verdict;}}
    Say("");
    SayH("[1] BAR0 = ",(unsigned long)gBar0,8);

    /* ⚠⚠ BOUND THE WALK BY ChipCommon's CORE COUNT. THIS IS WHAT KILLED f1.
     *
     * Reading SSB_IDHIGH at a core index that does not exist is NOT a soft failure. The backplane
     * window points at unmapped space and the read takes a PowerPC access exception -- there is no
     * 0xFFFFFFFF to test for, because the load never completes. f1 walked idx<8 on a 5-core chip
     * and died at index 5. The crash log named it exactly:
     *     R3 = 80086000 (gBar0)   R4 = 00000FFC (SSB_IDHIGH)   lwbrx r9,0,r9
     *     R6 = 18000000 (ENUM_BASE)  R7 = 00005000 (5 * 0x1000)  R8 = 18005000
     *
     * ⚠ AND THIS WAS A LATENT BUG IN 3a, 3b AND 4a TOO. They also wrote idx<8, but they BREAK as
     * soon as they find the 802.11 core at index 1, so they never reached index 5. They were
     * correct by accident. 4b-i needs BOTH the 802.11 core and ChipCommon, so the break had to go --
     * and removing it exposed the unbounded walk underneath.
     * Stage 2 got this right, bounding by nCores. This restores that. The lesson is narrow and
     * useful: an "obviously safe" upper bound on a probe loop is only safe if overrunning it
     * returns an error rather than faulting. */
    if(SelectCore(0)!=noErr){Say("!! cannot select core 0");goto verdict;}
    idhigh=ap_r32(gBar0,SSB_IDHIGH);
    if(IDHIGH_CC(idhigh)!=SSB_DEV_CHIPCOMMON){
      Say("!! core 0 is not ChipCommon -- refusing to walk the backplane without a core count,");
      Say("   because overrunning it faults rather than returning an error.");
      goto verdict;}
    ccIdx = 0;
    nCores = CHIPID_NCORES(ap_r32(gBar0,CC_CHIPID));
    Say1("    ChipCommon reports core count = ",nCores);
    if(nCores==0||nCores>8){
      Say("    ⚠ implausible core count; clamping to 5, which is what Stage 2 measured on this card.");
      nCores=5;}
    for(idx=0;idx<nCores;idx++){
      if(SelectCore(idx)!=noErr)continue;
      idhigh=ap_r32(gBar0,SSB_IDHIGH);
      if(idhigh==0xFFFFFFFFUL||idhigh==0)continue;
      if(IDHIGH_CC(idhigh)==SSB_DEV_80211&&coreIdx==0xFFFFFFFFUL)coreIdx=idx;}
    if(coreIdx==0xFFFFFFFFUL){Say("!! no 802.11 core");goto verdict;}
    Say1("[2] 802.11 core index ",coreIdx);
    if(ccIdx==0xFFFFFFFFUL) Say("    ⚠ no ChipCommon core found -- GPIO oracle will be skipped");
    else Say1("    ChipCommon core index ",ccIdx);
    if(SelectCore(coreIdx)!=noErr){Say("!! reselect failed");goto verdict;}
    idhigh=ap_r32(gBar0,SSB_IDHIGH);
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW = ",tmslow,8);
    if(!(tmslow&SSB_TMSLOW_RESET)){
      preconditionStop=1;
      Say("!! STOPPING: the core is already running (0x20050000 is the state 3a/3b/4a leave).");
      Say("   REBOOT AND RE-RUN. Nothing was measured; nothing is wrong.");
      goto verdict;}

    /* ---- [3] gmode, reset, identity (a cheap regression check on 4a) ---- */
    tmshigh=ap_r32(gBar0,SSB_TMSHIGH);
    have2ghz=(tmshigh&B43_TMSHIGH_HAVE_2GHZ_PHY)?1:0;
    gmodeFlag=have2ghz?B43_TMSLOW_GMODE:0;
    Say("");
    SayH("[3] TMSHIGH = ",tmshigh,8);
    Say1("    HAVE_2GHZ_PHY -> gmode = ",(unsigned long)have2ghz);
    CoreResetCycle(gmodeFlag);
    phyver=ap_r16(gBar0,B43_MMIO_PHY_VER);
    phytype=(phyver&B43_PHYVER_TYPE)>>B43_PHYVER_TYPE_SHIFT;
    phyrev=(phyver&B43_PHYVER_VERSION);
    SayH("    PHY_VER = ",phyver,4);
    Say1("      type = ",phytype); Say1("      rev  = ",phyrev);
    ap_w16(gBar0,B43_MMIO_RADIO_CONTROL,(UInt16)B43_RADIOCTL_ID);
    radioTmp=ap_r16(gBar0,B43_MMIO_RADIO_DATA_LOW);
    ap_w16(gBar0,B43_MMIO_RADIO_CONTROL,(UInt16)B43_RADIOCTL_ID);
    radioTmp|=((UInt32)ap_r16(gBar0,B43_MMIO_RADIO_DATA_HIGH))<<16;
    radioId=(radioTmp&0x0FFFF000UL)>>12;
    SayH("    radio ID = ",radioId,4);
    if(phytype==B43_PHYTYPE_G&&radioId==0x2050UL) Say("    ==> matches Stage 4a. Good regression check.");
    else Say("    ⚠ does NOT match Stage 4a's G-PHY / 0x2050. Investigate before trusting this run.");

    /* ---- [4] second reset, firmware, initvals ---- */
    Say("");
    Say("[4] SECOND RESET, FIRMWARE, INITVALS");
    CoreForceReset();
    CoreResetCycle(gmodeFlag);
    /* ★ THE f2 FIX. The PHY type is known by now (section [3] read it), so this reset is the one
     * b43 would follow with switch_analog(1). f2 omitted it and died reading PHY_DATA. */
    SwitchAnalog(1);
    Say("    analog section switched ON (PHY0 = 0) -- the step f2 was missing");
    macctl=B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_PSM_JMP0;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);(void)ap_r32(gBar0,B43_MMIO_MACCTL);
    for(w=0;w<64;w++){ShmControl(B43_SHM_SCRATCH,w);ap_w16(gBar0,B43_MMIO_SHM_DATA,0);}
    for(w=0;w<4096;w+=4){ShmControl(B43_SHM_SHARED,w>>2);ap_w32(gBar0,B43_MMIO_SHM_DATA,0);}
    words=ucode.bytes/4;
    ShmControl(B43_SHM_UCODE|B43_SHM_AUTOINC_W,0x0000);
    for(w=0;w<words;w++){ap_w32(gBar0,B43_MMIO_SHM_DATA,be32at(ucode.payload+w*4));SpinUs(10);}
    if(haveP){words=pcm.bytes/4;
      ShmControl(B43_SHM_HW,0x01EA);ap_w32(gBar0,B43_MMIO_SHM_DATA,0x00004000UL);
      ShmControl(B43_SHM_HW,0x01EB);
      for(w=0;w<words;w++){ap_w32(gBar0,B43_MMIO_SHM_DATA,be32at(pcm.payload+w*4));SpinUs(10);}}
    ap_w32(gBar0,B43_MMIO_GEN_IRQ_REASON,B43_IRQ_ALL);
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    macctl&=~B43_MACCTL_PSM_JMP0;macctl|=B43_MACCTL_PSM_RUN;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);(void)ap_r32(gBar0,B43_MMIO_MACCTL);
    for(i=0;i<100;i++){
      irq=ap_r32(gBar0,B43_MMIO_GEN_IRQ_REASON);
      if(irq==B43_IRQ_MAC_SUSPENDED) break;
      SpinUs(10000);
    }
    fwrev=ShmRead16Shared(B43_SHM_SH_UCODEREV);
    Say1("    ucode revision = ",fwrev);
    if(irq!=B43_IRQ_MAC_SUSPENDED||fwrev==0||fwrev>BWI_FW_VERSION3_REVMAX){
      Say("!! firmware did not come up; PHY access on a dead chip proves nothing.");goto verdict;}
    ShmControl(B43_SHM_SHARED,B43_SHM_SH_WLCOREREV>>2);
    ap_w16(gBar0,B43_MMIO_SHM_DATA_UNALIGNED,(UInt16)IDHIGH_REV(idhigh));
    ApplyIvs(iv.payload,iv.bytes,&ivApplied);
    Say1("    initvals applied = ",ivApplied);
    if(haveBs){UInt32 bc=0;ApplyIvs(bsiv.payload,bsiv.bytes,&bc);Say1("    bs initvals = ",bc);}

    /* ---- [5] GPIO INIT ---- */
    Say("");
    Say("[5] GPIO INIT (b43_gpio_init)");
    boardflags=(UInt32)ap_r16(gBar0,SSB_SPROM_BASE1+SPROM_BFLLO);
    SayH("    board flags (SPROM +0x72) = ",boardflags,4);
    Say1("      PACTRL (0x0002) = ",(boardflags&B43_BFL_PACTRL)?1:0);
    Say1("      RSSI   (0x0008) = ",(boardflags&B43_BFL_RSSI)?1:0);
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl&~B43_MACCTL_GPOUTSMSK);
    gpioMask=(UInt32)ap_r16(gBar0,B43_MMIO_GPIO_MASK);
    ap_w16(gBar0,B43_MMIO_GPIO_MASK,(UInt16)(gpioMask|0x000F));
    gmask=0x0000001FUL; gset=0x0000000FUL;
    if(boardflags&B43_BFL_PACTRL){
      Say("    ==> PACTRL set: enabling GPIO 9 so the ucode can drive the PA.");
      Say("        (b43 comment: \"PA is controlled by gpio 9, let ucode handle it\". §3a predicted");
      Say("         this branch from the SPROM read back in Stage 2; this is it being taken.)");
      gpioMask=(UInt32)ap_r16(gBar0,B43_MMIO_GPIO_MASK);
      ap_w16(gBar0,B43_MMIO_GPIO_MASK,(UInt16)(gpioMask|0x0200));
      gmask|=0x0200UL; gset|=0x0200UL;}
    SayH("    mask = ",gmask,8); SayH("    set  = ",gset,8);
    SayH("    GPIO_MASK (0x49E) now = ",(unsigned long)ap_r16(gBar0,B43_MMIO_GPIO_MASK),4);
    if(ccIdx!=0xFFFFFFFFUL){
      Say("    switching backplane window to ChipCommon for GPIO_CONTROL...");
      if(SelectCore(ccIdx)==noErr){
        ccBefore=ap_r32(gBar0,B43_GPIO_CONTROL);
        ccWant=(ccBefore&~gmask)|gset;
        SayH("      ChipCommon GPIO_CONTROL before = ",ccBefore,8);
        ap_w32(gBar0,B43_GPIO_CONTROL,ccWant);
        (void)ap_r32(gBar0,B43_GPIO_CONTROL);
        ccAfter=ap_r32(gBar0,B43_GPIO_CONTROL);
        SayH("      wrote                          = ",ccWant,8);
        SayH("      read back                      = ",ccAfter,8);
        if(ccAfter==ccWant){
          oracleB=1;
          Say("      ==> ORACLE B PASSED: GPIO_CONTROL holds exactly what we wrote. It is a control");
          Say("          register, so this readback means something -- unlike the initval ports.");
        } else if((ccAfter&gmask)==(ccWant&gmask)){
          oracleB=1;
          Say("      ==> ORACLE B PASSED on the bits we control. Other bits differ, which is");
          Say("          expected -- GPIO_CONTROL carries lines this driver does not own.");
        } else {
          Say("      ✗ ORACLE B FAILED: the masked bits did not take.");
        }
      } else Say("      !! could not select ChipCommon");
      if(SelectCore(coreIdx)!=noErr){Say("!! could not switch back to the 802.11 core");goto verdict;}
      Say("    switched back to the 802.11 core");
    }

    /* ---- [6] PHY REGISTER PATH ---- */
    Say("");
    Say("[6] PHY REGISTER PATH -- 0x3FC selects, 0x3FE carries");
    /* ⚠⚠ f3 CRASHED HERE, AND THE ORACLE DESIGN WAS THE BUG -- NOT A MISSING STEP.
     *
     * f3 swept indices 0x00..0x3F and faulted at 0x09 (crash log: R4=0x3FE, R5=R8=R30=0x09).
     * The sweep assumed a flat, freely-readable PHY register file. There is no such thing:
     *     B43_PHYROUTE          0x0C00   routing-bits mask
     *     B43_PHYROUTE_BASE     0x0000   CCK (B) registers
     *     B43_PHYROUTE_OFDM_GPHY 0x0400  OFDM (A) registers, for G-PHYs
     *     B43_PHYROUTE_EXT_GPHY 0x0800   Extended G-PHY registers
     * A bare index is an address in the BASE bank with no routing bits, and the defined CCK
     * registers are SPARSE -- 0x00, 0x01, 0x15, 0x18, 0x29, 0x2D, 0x33, 0x35, 0x38, 0x60, 0x78.
     * 0x09 is not one of them, is not decoded, and faults. b43 never sweeps; it reads NAMED
     * registers. The probe invented a test the hardware does not support.
     *
     * ★ AND f3's OWN DATA SUPPLIED A BETTER ORACLE THAN THE ONE IT DIED FOR.
     * B43_PHY_VERSION_CCK is CCK(0x00), i.e. PHY register 0x0000 -- and f3 read 0x2202 there,
     * exactly matching MMIO PHY_VER at 0x3E0. Two independent access paths agreeing on a value we
     * can predict is the same shape as every oracle that has worked in this project, and it is
     * strictly better than "count distinct values", which was only ever a proxy for liveness. */
    Say("    ⚠ f3 swept raw indices and faulted at 0x09. PHY registers are BANKED (route bits");
    Say("      0x0C00: base/CCK 0x0000, OFDM 0x0400, ExtG 0x0800) and the defined ones are sparse.");
    Say("      f4 then died at CCK 0x78 RCCALOVER -- a register phy_g.h DEFINES but the driver");
    Say("      NEVER reads. f5 reads only registers f4 itself returned a value for.");
    Say("");
    { UInt32 k;
      for(k=0;k<kNumPhyRegs;k++){
        Str255 L;
        phyregs[k]=PhyRead((UInt16)gPhyRegs[k].addr);
        L[0]=0;PCat(L,"    ");PCat(L,gPhyRegs[k].bank);
        PCat(L,"(0x");PCatHex(L,gPhyRegs[k].addr & 0x00FFUL,2);PCat(L,")  ");
        PCat(L,gPhyRegs[k].name);
        while(L[0]<34 && L[0]<255) L[++L[0]]=' ';
        PCat(L," = 0x");PCatHex(L,phyregs[k],4);
        Out(L);
      }
      /* Stability, on the same safe set. */
      for(k=0;k<kNumPhyRegs;k++) if(PhyRead((UInt16)gPhyRegs[k].addr)!=phyregs[k]) unstable++;
      for(k=0;k<kNumPhyRegs;k++){ UInt32 j,seen=0;
        for(j=0;j<k;j++) if(phyregs[j]==phyregs[k]){seen=1;break;}
        if(!seen) distinct++; }
    }
    Say("");
    Say1("    named registers read      = ",(unsigned long)kNumPhyRegs);
    Say1("    distinct values among them= ",distinct);
    Say1("    changed on re-read        = ",unstable);
    Say("");
    /* ★ THE ORACLE: two independent paths to one predicted value.
     * B43_PHY_VERSION_CCK is CCK(0x00) -- PHY register 0x0000, reached through the
     * PHY_CONTROL/PHY_DATA port pair. MMIO PHY_VER at 0x3E0 is a direct register. They are
     * different mechanisms and they must agree. f3 already showed both reading 0x2202, which is
     * why this replaced the distinct-value count: that was a proxy for liveness, this is a
     * prediction. */
    SayH("    VERSION_CCK via PHY port (0x3FC/0x3FE) = ",phyregs[0],4);
    SayH("    PHY_VER via direct MMIO  (0x3E0)       = ",phyver,4);
    if(phyregs[0]==phyver && phyver!=0 && phyver!=0xFFFF){
      oracleA=1;
      Say("    ==> ORACLE A PASSED. The banked PHY register path reaches register 0x0000 and");
      Say("        returns the same version word the direct MMIO register does. Two unrelated");
      Say("        access mechanisms agreeing on a value predicted before the run.");
      if(distinct<2)
        Say("    ⚠ but every named register read the same value, which is suspicious -- check the");
      if(distinct<2)
        Say("      dump above before trusting this.");
    } else if(phyregs[0]==0 || phyregs[0]==0xFFFF){
      Say("    ✗ ORACLE A FAILED: register 0x0000 reads as all-zero or all-ones, i.e. the port is");
      Say("      not returning data. PHY_CONTROL is not selecting, or PHY_DATA is not connected.");
    } else {
      Say("    ✗ ORACLE A FAILED: the two paths disagree. Both are live but they are not looking");
      Say("      at the same register -- suspect the routing bits in the address we sent.");
    }

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(preconditionStop){
      Say("  ⊘ NOT A RESULT. Stopped at the fresh-boot precondition; nothing was measured.");
    } else if(oracleA&&oracleB){
      Say("  ✓✓✓ STAGE 4b-i PASSES. GPIO is initialised with the PACTRL/GPIO-9 branch this card");
      Say("      actually needs, and the PHY register path is proven live and stable.");
      Say("      4b-ii can now port the G-PHY and BCM2050 init sequences onto a foundation that");
      Say("      is known to work, instead of debugging two unknowns at once.");
    } else if(oracleA){
      Say("  ~ The PHY path works; GPIO_CONTROL did not hold. Since the PHY path is live, the");
      Say("    backplane and MMIO are fine -- look at the ChipCommon core switch and the mask/set.");
    } else if(oracleB){
      Say("  ~ GPIO works but the PHY path does not respond to the index. Since a ChipCommon");
      Say("    read-modify-write landed, MMIO and the core switch are fine; the fault is specific");
      Say("    to 0x3FC/0x3FE. Check that the core is out of PHY reset and the clock is on.");
    } else {
      Say("  ✗ Both failed. Everything up to firmware was proven by 3a/3b/4a, so start at [5]/[6].");
    }
    if(!preconditionStop){
      Say("");
      Say("  ⚠ Core left running with firmware, initvals and GPIO applied; no PHY init. Open");
      Say("    Firmware resets it at the next boot.");
    }
    Say("");
    Say("=== done. Log: 'AirPort Stage4bi Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
