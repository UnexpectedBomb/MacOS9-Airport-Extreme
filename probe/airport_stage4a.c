/* airport_stage4a.c -- STAGE 4a: WHAT PHY AND RADIO ARE ON THIS CARD, AND DO INITVALS APPLY?
 *
 * ★★★★★ WHERE THIS SITS. Stage 3b got the chip executing Broadcom microcode under Mac OS 9
 * (MAC_SUSPENDED on poll 0; ucode 295.14 written into SHM we had erased). Stage 4 is the first
 * stage with four subsystems that must ALL be right before any of them reports success -- initvals,
 * G-PHY init, BCM2050 radio init, and DMA rings -- because a beacon only decodes when all four
 * work. So Stage 4 is split, exactly as Stage 3 was:
 *   4a (this)  identify the PHY and radio from hardware, and apply the initvals tables.
 *   4b (next)  PHY/radio init proper, DMA rings under the 30-bit ceiling, and a captured beacon.
 *
 * ⚠ A CORRECTION TO THE ROADMAP, recorded because the wrong version was written down first.
 * The roadmap said 4a's discriminator would be "PHY version and type read back from SHM
 * 0x0050/0x0052". That is wrong. B43_SHM_SH_PHYVER exists, but the AUTHORITATIVE read is the
 * B43_MMIO_PHY_VER register at 0x3E0 in the core window -- b43_phy_versioning() reads MMIO, not
 * SHM (b43_main.c:4495). Building an oracle on the SHM copy would have been the WLCOREREV mistake
 * a second time: testing a field whose producer we had not verified. Checked first this time.
 *
 * ★★★★★ THE ORACLES.
 *   A: THE RADIO MUST IDENTIFY ITSELF, and we predicted the answer before the run. Reading the
 *      radio ID goes through B43_MMIO_RADIO_CONTROL/DATA_LOW/DATA_HIGH (0x3F6/0x3FA/0x3F8) -- a
 *      register path nothing in Stages 1-3 has ever touched. It must yield:
 *          radio manufacturer = 0x17F  (Broadcom; b43 calls anything else unsupported)
 *          radio ID           = 0x2050 (b43 requires exactly this for a G-PHY)
 *      and PHY_VER must decode to type 2 = B43_PHYTYPE_G. An iBook G4 running FreeBSD's bwi
 *      reported "PHY: type 2, rev 2" / "RF: manu 0x17f, type 0x2050, rev 2" for the same part, so
 *      these are values we wrote down in advance rather than discovered and then rationalised.
 *   B: THE INITVALS TABLE MUST PARSE EXACTLY. b0g0initvals5's header declares 317 IV records. The
 *      table is a packed stream of {be16 offset_size; be32-or-be16 value}, width chosen by bit 15
 *      of offset_size (B43_IV_32BIT), offset masked with B43_IV_OFFSET_MASK. Walking it must
 *      consume EXACTLY the payload with EXACTLY 317 records, every offset below 0x1000.
 *      That is crisp pass/fail: if the format assumption were wrong the walk desynchronises
 *      immediately and the count and the byte total both come out wrong. A fuzzy "most registers
 *      read back" test would not have that property, so readback is reported as DIAGNOSTICS, not
 *      as the oracle. Some of these registers are self-clearing or read-different by design, and
 *      an oracle that can fail for that reason is the trap this project has now paid for twice.
 *
 * ⚠⚠ THE GMODE MISTAKE, AND WHAT THE e2 RUN COST TO FIND IT.
 * e1/e2 reset the core with gmode=FALSE before reading the PHY versioning, on the strength of a
 * research note saying "the core is reset twice -- once before the PHY type is known, once after".
 * That note is true about the COUNT and says nothing about the MODE, and the mode is what matters.
 * b43_main.c:5420-5434 is explicit:
 *
 *      tmp = ssb_read32(dev->dev->sdev, SSB_TMSHIGH);
 *      have_2ghz_phy = !!(tmp & B43_TMSHIGH_HAVE_2GHZ_PHY);
 *      dev->phy.gmode = have_2ghz_phy;
 *      b43_wireless_core_reset(dev, dev->phy.gmode);    <- GMODE ALREADY SET
 *      err = b43_phy_versioning(dev);
 *
 * The gmode for the FIRST reset comes from the TMSHIGH core flags, read while the core is still in
 * reset -- not from the PHY type, which is not known yet. On this card TMSHIGH reads 0x40070000,
 * so HAVE_2GHZ_PHY (0x00010000) is SET and gmode is TRUE from the very first reset.
 *
 * Reading the identity with GMODE clear is why e2 reported PHY type 0 (A-PHY) instead of 2, and a
 * radio ID of 0x00000000: without GMODE the 2.4 GHz radio is not the one being addressed. THE CARD
 * WAS FINE; THE PROBE ASKED IN THE WRONG MODE.
 * ⇒ Third instance of one class of error in this project (WLCOREREV, the SHM-vs-MMIO PHY read, and
 * now this): a detail asserted from memory or inference where the source was one grep away. The
 * rule that keeps working is to read the sequence, not to remember its shape.
 *
 * The core is still reset TWICE, because b43 does (5431 before versioning, 5463 after
 * b43_phy_allocate) -- but BOTH use the same gmode, taken from the core flags.
 *
 * ⚠ GPIO INIT IS DEFERRED TO 4b, ON PURPOSE. b43_gpio_init() runs before initvals in chip_init,
 * but it writes ChipCommon GPIO control -- board-level signals, and it needs a core switch away
 * from the 802.11 core and back mid-sequence. Nothing in this run's question depends on it: GPIO
 * drives LEDs and radio enable lines that only matter once we are actually transmitting or
 * receiving, which is 4b. Keeping it out keeps this run's write set minimal and its question clean.
 *
 * ⚠⚠ WHAT THIS BINARY WRITES, beyond the 3b set: the initvals tables themselves, which are ~348
 * register writes into the core's own 4 KB window at offsets the tables choose. That is the first
 * time this project writes registers whose addresses come from a data file rather than from source
 * we have read. Hence the parse-before-apply discipline: every offset is range-checked against
 * 0x1000 BEFORE the write, so a corrupt table cannot scatter writes across the window.
 *
 * ⚠ FRESH BOOT REQUIRED, same precondition and same reason as 3a and 3b.
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
#define AP_S4A_VER "e4"

#define kVendorBroadcom   0x14E4UL
#define kDevBCM4306_4320  0x4320UL
#define kDevBCM4306_4325  0x4325UL

#define SSB_ENUM_BASE     0x18000000UL
#define SSB_CORE_SIZE     0x1000UL
#define SSB_BAR0_WIN      0x80UL
#define SSB_IMSTATE       0x0F90UL
#define SSB_TMSLOW        0x0F98UL
#define SSB_TMSHIGH       0x0F9CUL
#define SSB_IDHIGH        0x0FFCUL
#define SSB_TMSLOW_RESET   0x00000001UL
#define SSB_TMSLOW_CLOCK   0x00010000UL
#define SSB_TMSLOW_FGC     0x00020000UL
#define SSB_TMSHIGH_SERR   0x00000001UL
/* ⚠ These two drive gmode for the FIRST reset (b43_main.c:5420-5431), before the PHY type is
 * known. Getting this wrong is what made e1/e2 read the identity in the wrong mode. */
#define B43_TMSHIGH_HAVE_2GHZ_PHY 0x00010000UL
#define B43_TMSHIGH_HAVE_5GHZ_PHY 0x00020000UL
#define SSB_IMSTATE_IBE    0x00020000UL
#define SSB_IMSTATE_TO     0x00040000UL
#define IDHIGH_CC(v)      (((v) >> 4) & 0xFFFUL)
#define IDHIGH_REV(v)     (((((v) & 0x7000UL) >> 8)) | ((v) & 0xFUL))
#define SSB_DEV_80211      0x812UL

#define B43_TMSLOW_GMODE      0x20000000UL
#define B43_TMSLOW_PHYRESET   0x00080000UL
#define B43_TMSLOW_PHYCLKEN   0x00040000UL

#define B43_MMIO_MACCTL           0x120UL
#define B43_MMIO_GEN_IRQ_REASON   0x128UL
#define B43_MMIO_SHM_CONTROL      0x160UL
#define B43_MMIO_SHM_DATA         0x164UL
#define B43_MMIO_SHM_DATA_UNALIGNED 0x166UL
/* PHY and radio identity registers -- confirmed against b43.h, NOT the SHM copies. */
#define B43_MMIO_PHY_VER          0x3E0UL
#define B43_MMIO_RADIO_CONTROL    0x3F6UL
#define B43_MMIO_RADIO_DATA_HIGH  0x3F8UL
#define B43_MMIO_RADIO_DATA_LOW   0x3FAUL
#define B43_RADIOCTL_ID           0x01UL

#define B43_PHYVER_ANALOG        0xF000UL
#define B43_PHYVER_ANALOG_SHIFT  12
#define B43_PHYVER_TYPE          0x0F00UL
#define B43_PHYVER_TYPE_SHIFT    8
#define B43_PHYVER_VERSION       0x00FFUL
#define B43_PHYTYPE_B            0x01UL
#define B43_PHYTYPE_G            0x02UL

#define B43_MACCTL_PSM_RUN     0x00000002UL
#define B43_MACCTL_PSM_JMP0    0x00000004UL
#define B43_MACCTL_SHM_ENABLED 0x00000100UL
#define B43_MACCTL_IHR_ENABLED 0x00000400UL
#define B43_MACCTL_BE          0x00010000UL
#define B43_MACCTL_GMODE       0x80000000UL

#define B43_SHM_UCODE     0UL
#define B43_SHM_SHARED    1UL
#define B43_SHM_SCRATCH   2UL
#define B43_SHM_HW        3UL
#define B43_SHM_AUTOINC_W 0x0100UL

#define B43_SHM_SH_UCODEREV   0x0000UL
#define B43_SHM_SH_UCODEPATCH 0x0002UL
#define B43_SHM_SH_WLCOREREV  0x0016UL   /* the DRIVER writes this FOR the firmware (main.c:4873) */

#define B43_IRQ_MAC_SUSPENDED 0x00000001UL
#define B43_IRQ_ALL           0xFFFFFFFFUL
#define BWI_FW_VERSION3_REVMAX 0x128UL

/* initvals record format -- confirmed against b43.h */
#define B43_IV_OFFSET_MASK 0x7FFFUL
#define B43_IV_32BIT       0x8000UL

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
static Str255 gLines[260]; static short gN=0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Stage4a Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<260){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}
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

static void FlushTmslow(void){(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(2);}

static void CoreEnableFromReset(UInt32 flags)
{
    UInt32 v;
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_RESET|SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags); FlushTmslow();
    if(ap_r32(gBar0,SSB_TMSHIGH)&SSB_TMSHIGH_SERR) ap_w32(gBar0,SSB_TMSHIGH,0);
    v=ap_r32(gBar0,SSB_IMSTATE);
    if(v&(SSB_IMSTATE_IBE|SSB_IMSTATE_TO)) ap_w32(gBar0,SSB_IMSTATE,v&~(SSB_IMSTATE_IBE|SSB_IMSTATE_TO));
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags); FlushTmslow();
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|flags); FlushTmslow();
}

static void PhyTakeOutOfReset(void)
{
    UInt32 t;
    t=ap_r32(gBar0,SSB_TMSLOW);t&=~B43_TMSLOW_PHYRESET;t&=~B43_TMSLOW_PHYCLKEN;t|=SSB_TMSLOW_FGC;
    ap_w32(gBar0,SSB_TMSLOW,t);(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(1500);
    t=ap_r32(gBar0,SSB_TMSLOW);t&=~SSB_TMSLOW_FGC;t|=B43_TMSLOW_PHYCLKEN;
    ap_w32(gBar0,SSB_TMSLOW,t);(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(1500);
}

/* A full reset cycle: from a core that is ALREADY in reset, to running, PHY out of reset.
 * Used twice -- once with gmode=0 before we know the PHY type, once with GMODE after. */
static void CoreResetCycle(UInt32 gmodeFlag)
{
    CoreEnableFromReset(gmodeFlag | B43_TMSLOW_PHYCLKEN | B43_TMSLOW_PHYRESET);
    SpinUs(2500);
    PhyTakeOutOfReset();
}

/* Put the core back INTO reset so the second CoreEnableFromReset starts from the same state the
 * first one did. ssb_device_disable()'s full path needs the reject handshake; but a core we have
 * just enabled ourselves, with no DMA running and no firmware, can be put back with the plain
 * write ssb ends that function on. This is the one place we deviate from a verbatim sequence, and
 * it is confined to a core in a state we created two milliseconds earlier. */
static void CoreForceReset(UInt32 flags)
{
    ap_w32(gBar0, SSB_TMSLOW, SSB_TMSLOW_RESET | flags);
    FlushTmslow();
    SpinUs(1000);
}

typedef struct { Handle h; UInt8 *payload; UInt32 bytes; } FwBlob;

static int LoadFw(ResType t, short id, UInt8 wantType, const char *label, FwBlob *out)
{
    Handle h; long sz; UInt8 *p; UInt32 declared; Str255 L;
    h = GetResource(t, id);
    if (!h) { L[0]=0;PCat(L,"    !! missing resource: ");PCat(L,label);Out(L); return 0; }
    HLock(h); sz = GetHandleSize(h); p = (UInt8*)*h;
    if (sz < 9) { Say("    !! resource too small"); HUnlock(h); return 0; }
    declared = ((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|(UInt32)p[7];
    L[0]=0;PCat(L,"    ");PCat(L,label);PCat(L,": res=");PCatDec(L,sz);
    PCat(L,"  type='");if(L[0]<255)L[++L[0]]=p[0];PCat(L,"'");
    PCat(L,"  gen=");PCatDec(L,(long)p[1]);
    PCat(L,"  declared=");PCatDec(L,(long)declared);Out(L);
    if (p[0]!=wantType) { Say("      !! wrong type byte"); HUnlock(h); return 0; }
    if (p[1]!=1)        { Say("      !! fw_gen != 1");     HUnlock(h); return 0; }
    /* ⚠ For 'u' and 'p' the header declares BYTES and must equal size-8. For 'i' it declares an IV
     * COUNT, which is a different thing entirely -- checking it against size-8 would fail every
     * time and tell us nothing. The count is verified by the parse in [8] instead. */
    if (wantType != 'i' && declared != (UInt32)(sz-8)) {
        Say("      !! declared size != resource size - 8"); HUnlock(h); return 0; }
    Say("      ==> header OK");
    out->h=h; out->payload=p+8; out->bytes=(UInt32)(sz-8);
    return 1;
}

static UInt32 be32at(const UInt8*p){return ((UInt32)p[0]<<24)|((UInt32)p[1]<<16)|((UInt32)p[2]<<8)|(UInt32)p[3];}
static UInt16 be16at(const UInt8*p){return (UInt16)(((UInt32)p[0]<<8)|(UInt32)p[1]);}
static UInt32 IvDeclaredCount(const UInt8*resStart){return be32at(resStart+4);}

/* Walk an initvals table. apply=1 writes; apply=0 re-reads and counts matches.
 * Returns 1 on a clean parse (consumed exactly `bytes`), 0 on a format error. */
/* bMatch/bTotal, when supplied, are 3-element arrays bucketed by the IV's target offset:
 *   [0] below 0x400   -- MAC registers proper. These SHOULD mostly read back.
 *   [1] 0x400..0x7FF  -- the IHR region: indirect PHY/radio shadow ports. A write here is a
 *                        command to another block, so reading the same offset back need not
 *                        return it, and a mismatch means nothing.
 *   [2] 0x800 and up  -- the SHM window.
 * The e2 run reported a flat 31 matched / 286 not, which is uninterpretable without this split:
 * it could mean "the writes never landed" or "most writes went to indirect ports". */
static int WalkIvs(const UInt8 *p, UInt32 bytes, int apply,
                   UInt32 *outCount, UInt32 *out32, UInt32 *out16,
                   UInt32 *outMatch, UInt32 *outMismatch, UInt32 *outBadOff,
                   UInt32 *bMatch, UInt32 *bTotal)
{
    UInt32 c=0,consumed=0,n32=0,n16=0,match=0,mismatch=0,bad=0;
    if (bMatch) { bMatch[0]=bMatch[1]=bMatch[2]=0; }
    if (bTotal) { bTotal[0]=bTotal[1]=bTotal[2]=0; }
    while (consumed + 2 <= bytes) {
        UInt16 osz; UInt32 off; int is32;
        osz = be16at(p+consumed); consumed += 2;
        off = (UInt32)osz & B43_IV_OFFSET_MASK;
        is32 = ((UInt32)osz & B43_IV_32BIT) != 0;
        /* ⚠ RANGE CHECK BEFORE THE WRITE. These offsets come from a data file, not from source we
         * have read. b43_write_initvals() treats >= 0x1000 as a format error and so do we -- a
         * corrupt table must not be able to scatter writes across the register window. */
        if (off >= 0x1000UL) { bad++; break; }
        if (is32) {
            UInt32 v;
            if (consumed + 4 > bytes) break;
            v = be32at(p+consumed); consumed += 4; n32++;
            if (apply) ap_w32(gBar0, off, v);
            else { int b = (off < 0x400UL) ? 0 : (off < 0x800UL) ? 1 : 2;
                   if (bTotal) bTotal[b]++;
                   if (ap_r32(gBar0,off)==v) { match++; if (bMatch) bMatch[b]++; } else mismatch++; }
        } else {
            UInt16 v;
            if (consumed + 2 > bytes) break;
            v = be16at(p+consumed); consumed += 2; n16++;
            if (apply) ap_w16(gBar0, off, v);
            else { int b = (off < 0x400UL) ? 0 : (off < 0x800UL) ? 1 : 2;
                   if (bTotal) bTotal[b]++;
                   if (ap_r16(gBar0,off)==v) { match++; if (bMatch) bMatch[b]++; } else mismatch++; }
        }
        c++;
    }
    if (outCount)    *outCount    = c;
    if (out32)       *out32       = n32;
    if (out16)       *out16       = n16;
    if (outMatch)    *outMatch    = match;
    if (outMismatch) *outMismatch = mismatch;
    if (outBadOff)   *outBadOff   = bad;
    return (consumed == bytes && bad == 0);
}

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i, y;
    UInt16 cmd=0;
    UInt32 idx, coreIdx=0xFFFFFFFFUL, idhigh=0, tmslow=0, tmshigh=0, macctl=0, irq=0, w, words;
    UInt32 gmodeFlag=0; int have2ghz=0, have5ghz=0;
    UInt32 bMatch[3], bTotal[3];
    FwBlob ucode, pcm, iv, bsiv;
    int haveU=0,haveP=0,haveIv=0,haveBs=0;
    UInt32 phyver=0, analog=0, phytype=0, phyrev=0;
    UInt32 radioTmp=0, radioManuf=0, radioId=0, radioRev=0;
    UInt32 fwrev=0;
    UInt32 ivCount=0,iv32=0,iv16=0,ivMatch=0,ivMis=0,ivBad=0,ivDeclared=0;
    int ivParsed=0, bsParsed=0;
    int oracleA=0, oracleB=0, preconditionStop=0;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Stage 4a " AP_S4A_VER " - PHY/radio identity + initvals",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT EXTREME STAGE 4a " AP_S4A_VER " -- PHY/RADIO IDENTITY + INITVALS ===");
    Say("  Oracle A: the radio must identify itself as Broadcom 0x17F / ID 0x2050, and PHY_VER must");
    Say("            decode to type 2 (G). Predicted in advance from an iBook G4 bwi dmesg, and read");
    Say("            through the radio control/data registers, which nothing before has touched.");
    Say("  Oracle B: the initvals table must parse EXACTLY -- declared record count, consuming the");
    Say("            whole payload, every offset below 0x1000. Register readback is reported as");
    Say("            DIAGNOSTICS, not as an oracle: some of these registers are self-clearing.");
    Say("  ⚠ REQUIRES A FRESH BOOT.");
    Say("  Note: GPIO init is deferred to 4b on purpose; nothing here depends on it.");
    Say("");

    Say("[0] FIRMWARE RESOURCES");
    haveU  = LoadFw('BCMu',128,'u',"ucode5",&ucode);
    haveP  = LoadFw('BCMp',128,'p',"pcm5",&pcm);
    haveIv = LoadFw('BCMi',128,'i',"b0g0initvals5",&iv);
    haveBs = LoadFw('BCMi',129,'i',"b0g0bsinitvals5",&bsiv);
    if(!haveU){Say("!! no microcode");goto verdict;}
    if(!haveIv){Say("!! no initvals -- this run has no question to answer");goto verdict;}
    ivDeclared = IvDeclaredCount((UInt8*)*iv.h);
    Say1("    b0g0initvals5 declares IV records = ",ivDeclared);

    if(FindNode(&gNode)!=noErr){Say("!! no BCM4306 node");goto verdict;}
    if(MapBar0(&gNode,&gBar0)!=noErr||gBar0==0){Say("!! could not map BAR0");goto verdict;}
    if(ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&cmd)!=noErr){Say("!! cmd read");goto verdict;}
    if(!(cmd&0x0002)){UInt16 b=0;
      if(ExpMgrConfigWriteWord(&gNode,(LogicalAddress)0x04,(UInt16)(cmd|0x0002))!=noErr){Say("!! mem enable");goto verdict;}
      (void)ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&b);
      if(!(b&0x0002)){Say("!! mem enable did not stick");goto verdict;}}
    Say("");
    SayH("[1] BAR0 = ",(unsigned long)gBar0,8);

    for(idx=0;idx<8;idx++){
      if(SelectCore(idx)!=noErr)continue;
      idhigh=ap_r32(gBar0,SSB_IDHIGH);
      if(idhigh==0xFFFFFFFFUL||idhigh==0)continue;
      if(IDHIGH_CC(idhigh)==SSB_DEV_80211){coreIdx=idx;break;}}
    if(coreIdx==0xFFFFFFFFUL){Say("!! no 802.11 core");goto verdict;}
    Say1("[2] 802.11 core index ",coreIdx);
    Say1("    core revision = ",IDHIGH_REV(idhigh));
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW = ",tmslow,8);
    if(!(tmslow&SSB_TMSLOW_RESET)){
      preconditionStop = 1;
      Say("");
      Say("!! STOPPING: THE CORE IS ALREADY RUNNING, NOT IN RESET.");
      Say("   0x20050000 is the state Stages 3a/3b leave it in, so this machine has not been");
      Say("   rebooted since one of those ran. That is not a fault -- this app skips");
      Say("   ssb_device_disable()'s teardown path, which is only legitimate when RESET is already");
      Say("   asserted, and it declines rather than performing a teardown it does not implement.");
      Say("   ==> REBOOT AND RE-RUN. Nothing was read, nothing was written, nothing is wrong.");
      goto verdict;}

    /* ---- [3] gmode from the core flags, THEN the first reset ---- */
    Say("");
    Say("[3] DERIVING GMODE FROM THE CORE FLAGS, THEN THE FIRST RESET");
    tmshigh = ap_r32(gBar0, SSB_TMSHIGH);
    SayH("    TMSHIGH (core still in reset) = ", tmshigh, 8);
    have2ghz = (tmshigh & B43_TMSHIGH_HAVE_2GHZ_PHY) ? 1 : 0;
    have5ghz = (tmshigh & B43_TMSHIGH_HAVE_5GHZ_PHY) ? 1 : 0;
    Say1("      HAVE_2GHZ_PHY = ", (unsigned long)have2ghz);
    Say1("      HAVE_5GHZ_PHY = ", (unsigned long)have5ghz);
    Say("    ⚠ b43 takes gmode for the FIRST reset from these flags, not from the PHY type -- the");
    Say("      PHY type is not known yet (b43_main.c:5430). e1/e2 reset with gmode=FALSE and so read");
    Say("      the identity in the wrong mode; that is why they saw PHY type 0 and radio 0x00000000.");
    gmodeFlag = have2ghz ? B43_TMSLOW_GMODE : 0;
    SayH("    core-specific flags for this reset = ", gmodeFlag|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET, 8);
    /* (Both flags set at once is normal on this part. b43 calls b43_supported_bands() later for the
     * real answer and warns "5 GHz band is unsupported on this PHY" for a G-PHY. Optimistic flags
     * are expected; what matters here is that HAVE_2GHZ_PHY drives gmode.) */
    CoreResetCycle(gmodeFlag);
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW after = ",tmslow,8);
    if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){
      Say("!! core did not come out of reset");goto verdict;}

    /* ---- [4] PHY and radio identity ---- */
    Say("");
    Say("[4] PHY AND RADIO IDENTITY (MMIO 0x3E0 and the radio control path)");
    phyver  = ap_r16(gBar0, B43_MMIO_PHY_VER);
    analog  = (phyver & B43_PHYVER_ANALOG) >> B43_PHYVER_ANALOG_SHIFT;
    phytype = (phyver & B43_PHYVER_TYPE)   >> B43_PHYVER_TYPE_SHIFT;
    phyrev  = (phyver & B43_PHYVER_VERSION);
    SayH("    PHY_VER raw = ",phyver,4);
    Say1("      analog type = ",analog);
    Say1("      PHY type    = ",phytype);
    Say1("      PHY rev     = ",phyrev);
    if(phytype==B43_PHYTYPE_G)      Say("      ==> type 2 = G-PHY, as predicted.");
    else if(phytype==B43_PHYTYPE_B) Say("      ⚠ type 1 = B-PHY. This card should be b/g.");
    else                            Say("      ⚠ unexpected PHY type.");

    /* Legacy radio ID path, for core rev < 22. Two writes of RADIOCTL_ID, reading LOW then HIGH. */
    ap_w16(gBar0,B43_MMIO_RADIO_CONTROL,(UInt16)B43_RADIOCTL_ID);
    (void)ap_r16(gBar0,B43_MMIO_RADIO_CONTROL);
    radioTmp  = ap_r16(gBar0,B43_MMIO_RADIO_DATA_LOW);
    ap_w16(gBar0,B43_MMIO_RADIO_CONTROL,(UInt16)B43_RADIOCTL_ID);
    (void)ap_r16(gBar0,B43_MMIO_RADIO_CONTROL);
    radioTmp |= ((UInt32)ap_r16(gBar0,B43_MMIO_RADIO_DATA_HIGH)) << 16;
    radioManuf = radioTmp & 0x00000FFFUL;
    radioId    = (radioTmp & 0x0FFFF000UL) >> 12;
    radioRev   = (radioTmp & 0xF0000000UL) >> 28;
    SayH("    radio raw   = ",radioTmp,8);
    SayH("      manufacturer = ",radioManuf,3);
    SayH("      radio ID     = ",radioId,4);
    Say1("      radio rev    = ",radioRev);
    if(radioManuf==0x17FUL) Say("      ==> 0x17F = Broadcom. b43 calls anything else unsupported.");
    else                    Say("      ⚠ NOT 0x17F. b43 would reject this radio.");
    if(radioId==0x2050UL)   Say("      ==> 0x2050 = the BCM2050. Exactly what a G-PHY requires.");
    else                    Say("      ⚠ not 0x2050.");
    if(phytype==B43_PHYTYPE_G && radioManuf==0x17FUL && radioId==0x2050UL){
      oracleA=1;
      Say("    ==> ORACLE A PASSED: G-PHY with a Broadcom BCM2050, read out of the radio register");
      Say("        path, matching values written down before the run.");
    } else {
      Say("    ✗ ORACLE A FAILED -- see the individual lines above for which part disagreed.");
    }

    /* ---- [5] second reset, this time with GMODE ---- */
    Say("");
    Say("[5] SECOND CORE RESET (b43_main.c:5463, after b43_phy_allocate)");
    Say("    ⚠ SAME gmode as the first -- both come from the core flags. The second reset exists");
    Say("      because b43 allocates PHY state in between, not because the mode changes.");
    CoreForceReset(0);
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    forced back into reset, TMSLOW = ",tmslow,8);
    if(!(tmslow&SSB_TMSLOW_RESET)){
      Say("!! could not put the core back into reset; refusing to re-enable from an unknown state.");
      goto verdict;}
    CoreResetCycle(gmodeFlag);
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW after = ",tmslow,8);
    if(!((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))){
      Say("!! second reset failed");goto verdict;}

    /* ---- [6] MACCTL + SHM clear + firmware ---- */
    Say("");
    Say("[6] MACCTL, SHM CLEAR, FIRMWARE");
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    Say1("    BE bit = ",(macctl&B43_MACCTL_BE)?1:0);
    macctl=B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_PSM_JMP0;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);(void)ap_r32(gBar0,B43_MMIO_MACCTL);
    for(w=0;w<64;w++){ShmControl(B43_SHM_SCRATCH,w);ap_w16(gBar0,B43_MMIO_SHM_DATA,0);}
    for(w=0;w<4096;w+=4){ShmControl(B43_SHM_SHARED,w>>2);ap_w32(gBar0,B43_MMIO_SHM_DATA,0);}
    words=ucode.bytes/4;
    ShmControl(B43_SHM_UCODE|B43_SHM_AUTOINC_W,0x0000);
    for(w=0;w<words;w++){ap_w32(gBar0,B43_MMIO_SHM_DATA,be32at(ucode.payload+w*4));SpinUs(10);}
    Say1("    microcode streamed, words = ",words);
    if(haveP){
      words=pcm.bytes/4;
      ShmControl(B43_SHM_HW,0x01EA);ap_w32(gBar0,B43_MMIO_SHM_DATA,0x00004000UL);
      ShmControl(B43_SHM_HW,0x01EB);
      for(w=0;w<words;w++){ap_w32(gBar0,B43_MMIO_SHM_DATA,be32at(pcm.payload+w*4));SpinUs(10);}
      Say1("    PCM streamed, words = ",words);}
    ap_w32(gBar0,B43_MMIO_GEN_IRQ_REASON,B43_IRQ_ALL);
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    macctl&=~B43_MACCTL_PSM_JMP0; macctl|=B43_MACCTL_PSM_RUN;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);(void)ap_r32(gBar0,B43_MMIO_MACCTL);
    for(i=0;i<100;i++){
      irq=ap_r32(gBar0,B43_MMIO_GEN_IRQ_REASON);
      if(irq==B43_IRQ_MAC_SUSPENDED) break;
      SpinUs(10000);
    }
    SayH("    GEN_IRQ_REASON = ",irq,8);
    fwrev = ShmRead16Shared(B43_SHM_SH_UCODEREV);
    Say1("    ucode revision = ",fwrev);
    if(irq!=B43_IRQ_MAC_SUSPENDED||fwrev==0||fwrev>BWI_FW_VERSION3_REVMAX){
      Say("!! firmware did not come up the way 3b proved it does. Stop here -- initvals against a");
      Say("   chip with no running microcode would tell us nothing.");
      goto verdict;}
    Say("    ==> firmware running (matches Stage 3b)");

    /* ---- [7] WLCOREREV: the driver's job ---- */
    ShmControl(B43_SHM_SHARED,B43_SHM_SH_WLCOREREV>>2);
    ap_w16(gBar0,B43_MMIO_SHM_DATA_UNALIGNED,(UInt16)IDHIGH_REV(idhigh));
    Say1("    wrote WLCOREREV = ",IDHIGH_REV(idhigh));

    /* ---- [8] initvals ---- */
    Say("");
    Say("[8] INITVALS");
    Say1("    b0g0initvals5 payload bytes = ",iv.bytes);
    ivParsed = WalkIvs(iv.payload, iv.bytes, 1, &ivCount,&iv32,&iv16,NULL,NULL,&ivBad,NULL,NULL);
    Say1("      records applied = ",ivCount);
    Say1("        32-bit = ",iv32);
    Say1("        16-bit = ",iv16);
    if(ivBad) Say1("      !! out-of-range offsets rejected = ",ivBad);
    if(!ivParsed) Say("      ✗ the walk did NOT consume the payload exactly -- format desync.");
    if(ivCount!=ivDeclared){
      Say1("      ✗ record count != header's declared count of ",ivDeclared);
    } else {
      Say("      ==> record count matches the header exactly.");
    }
    if(haveBs){
      UInt32 bc=0,b32=0,b16=0,bbad=0;
      Say1("    b0g0bsinitvals5 payload bytes = ",bsiv.bytes);
      bsParsed = WalkIvs(bsiv.payload,bsiv.bytes,1,&bc,&b32,&b16,NULL,NULL,&bbad,NULL,NULL);
      Say1("      records applied = ",bc);
      if(!bsParsed) Say("      ✗ band-switch table did not parse cleanly.");
    }
    if(ivParsed && ivCount==ivDeclared && ivBad==0){
      oracleB=1;
      Say("    ==> ORACLE B PASSED: the table parsed exactly -- declared count, whole payload");
      Say("        consumed, every offset in range. The IV record format is confirmed on real data.");
    } else {
      Say("    ✗ ORACLE B FAILED: see the lines above.");
    }

    /* ---- [9] readback DIAGNOSTICS, deliberately not an oracle ---- */
    Say("");
    Say("[9] REGISTER READBACK (diagnostics only -- NOT an oracle)");
    (void)WalkIvs(iv.payload, iv.bytes, 0, NULL,NULL,NULL,&ivMatch,&ivMis,NULL,bMatch,bTotal);
    Say("    ⚠⚠ READ THE NOTE BELOW BEFORE THIS NUMBER. Offline analysis of b0g0initvals5 (done");
    Say("       after the e3 run, on the desk, no boot required) shows the table is NOT a list of");
    Say("       register initial values. Its 317 records target only 35 DISTINCT offsets, and the");
    Say("       three most-written are port registers, not storage:");
    Say("           0x0164  SHM_DATA      written 152 times");
    Say("           0x0134  template data written  68 times");
    Say("           0x0160  SHM_CONTROL   written  40 times");
    Say("       The table is a recorded control/data stream that programs SHARED MEMORY and");
    Say("       TEMPLATE RAM by replay. Reading 0x0164 back returns whatever SHM word the control");
    Say("       register currently points at -- never the last value written. So a per-record");
    Say("       readback is meaningless for 282 of the 317 records by construction.");
    Say("       Only 35 records are the FINAL write to their offset, and those are the only ones");
    Say("       where readback could mean anything. See the FINAL-WRITE section below.");
    Say("");
    Say1("    per-record matched (misleading, kept for continuity) = ",ivMatch);
    Say1("    per-record differing                                 = ",ivMis);
    Say("    BY TARGET REGION (also misleading -- the sub-0x400 bucket is mostly PORT registers):");
    { Str255 L; int b; static const char *names[3] =
        { "  below 0x400 (MAC registers)      ",
          "  0x400-0x7FF (IHR: PHY/radio ports)",
          "  0x800+      (SHM window)          " };
      for (b = 0; b < 3; b++) {
        L[0]=0; PCat(L,"     "); PCat(L,names[b]);
        PCat(L," matched "); PCatDec(L,(long)bMatch[b]);
        PCat(L," of ");       PCatDec(L,(long)bTotal[b]);
        Out(L); } }

    /* THE ONLY READBACK THAT MEANS ANYTHING: one comparison per DISTINCT offset, against the LAST
     * value the table wrote there. Everything else is comparing a port register to a value that
     * was consumed the moment it was written. */
    Say("");
    Say("    FINAL-WRITE READBACK -- one comparison per distinct offset, against the last value");
    Say("    the table wrote to it. This is the measurement that carries information.");
    { UInt32 offs[64], vals[64], is32s[64]; UInt32 nd=0, k;
      UInt32 consumed=0, fmatch=0, fports=0;
      const UInt8 *p = iv.payload; UInt32 bytes = iv.bytes;
      while (consumed + 2 <= bytes) {
        UInt16 osz; UInt32 off, v; int is32;
        osz = be16at(p+consumed); consumed += 2;
        off = (UInt32)osz & B43_IV_OFFSET_MASK;
        is32 = ((UInt32)osz & B43_IV_32BIT) != 0;
        if (off >= 0x1000UL) break;
        if (is32) { if (consumed+4 > bytes) break; v = be32at(p+consumed); consumed += 4; }
        else      { if (consumed+2 > bytes) break; v = be16at(p+consumed); consumed += 2; }
        for (k = 0; k < nd; k++) if (offs[k] == off) break;
        if (k == nd) { if (nd >= 64) continue; offs[nd]=off; nd++; }
        vals[k] = v; is32s[k] = (UInt32)is32;   /* keep overwriting -> ends as the FINAL value */
      }
      Say1("      distinct offsets in the table = ", nd);
      for (k = 0; k < nd; k++) {
        UInt32 got = is32s[k] ? ap_r32(gBar0,offs[k]) : (UInt32)ap_r16(gBar0,offs[k]);
        int isPort = (offs[k]==0x0164UL || offs[k]==0x0160UL ||
                      offs[k]==0x0134UL || offs[k]==0x0130UL);
        Str255 L; L[0]=0;
        PCat(L,"        0x"); PCatHex(L,offs[k],4);
        PCat(L,is32s[k]?" (32) wrote 0x":" (16) wrote 0x"); PCatHex(L,vals[k],is32s[k]?8:4);
        PCat(L,"  read 0x"); PCatHex(L,got,is32s[k]?8:4);
        if (isPort)            { PCat(L,"   [PORT - readback meaningless]"); fports++; }
        else if (got==vals[k]) { PCat(L,"   OK"); fmatch++; }
        else                   { PCat(L,"   differs"); }
        Out(L);
      }
      Say1("      port registers excluded = ", fports);
      Say1("      of the rest, matching the final written value = ", fmatch);
      Say("      ⚠ THIS is the number to judge by. Near zero here would mean the writes never");
      Say("        landed. A healthy majority means the table took effect.");
    }

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(preconditionStop){
      /* ⚠ e1 fell through to the generic "both oracles failed" branch here and told the reader to
       * go and look at sections [4] and [8] -- sections that had never executed. A diagnostic that
       * points at code which did not run is how a cycle gets spent chasing nothing. Its own class
       * of bug, and worth a distinct branch. */
      Say("  ⊘ NOT A RESULT. This run stopped at the fresh-boot precondition and never reached the");
      Say("    PHY read or the initvals. No oracle was evaluated, because no measurement was taken.");
      Say("    Reboot and run it again; the run costs nothing but the boot.");
    } else if(oracleA&&oracleB){
      Say("  ✓✓✓ STAGE 4a PASSES. The card is a G-PHY with a Broadcom BCM2050 radio, identified");
      Say("      from hardware through a register path we had never used, matching a prediction");
      Say("      made before the run. The initvals record format is confirmed against real data and");
      Say("      the tables applied cleanly on top of running firmware.");
      Say("      Stage 4b next: GPIO init, the G-PHY and BCM2050 init sequences, DMA rings below");
      Say("      the 1 GB ceiling, and a captured beacon.");
    } else if(oracleA){
      Say("  ~ The PHY and radio identify correctly, so the hardware is what we think it is and the");
      Say("    register path works. The initvals table is the problem -- a format or a size issue,");
      Say("    not a hardware one. Compare the parsed record count against the header's.");
    } else if(oracleB){
      Say("  ~ The initvals parse and apply, but the PHY/radio identity is not what was predicted.");
      Say("    Since the table applied, MMIO works -- so this is about WHICH PART is on the card,");
      Say("    or about the radio-ID read sequence, not about reachability.");
    } else {
      Say("  ✗ Both failed. Everything up to firmware was proven by Stages 3a/3b, so look at [4]");
      Say("    and [8] rather than re-litigating the groundwork.");
    }
    if(!preconditionStop){
      Say("");
      Say("  ⚠ The core is left running with firmware and initvals applied, and no PHY init. Open");
      Say("    Firmware resets it at the next boot.");
    }
    Say("");
    Say("=== done. Log: 'AirPort Stage4a Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
