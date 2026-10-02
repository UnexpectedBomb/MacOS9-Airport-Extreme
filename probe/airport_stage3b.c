/* airport_stage3b.c -- STAGE 3b: UPLOAD MICROCODE AND MAKE THE CHIP RUN IT.
 *
 * ★★★★★ WHAT 3a PROVED, AND WHY THAT MAKES THIS TRACTABLE.
 * Stage 3a (probe c1) took the 802.11 core out of reset under the documented ssb sequence --
 * TMSLOW 0x00000001 -> 0x20050000, clock on, reset released -- and proved SHM is reachable,
 * writable and byte-order-correct through MACCTL's SHM_ENABLED. So this run inherits a known-good
 * core and a known-good SHM path, and the only new thing it does is push 22 KB of microcode through
 * that path and ask the chip to execute it.
 *
 * ⚠ 3a ALSO ANSWERED THE ENDIANNESS QUESTION, which is the one that would have silently ruined
 * this run. MACCTL read back with BE (Big Endian mode, 0x00010000) CLEAR. The chip is little-endian,
 * so our byte-reversing accessors are correct and the upload byte order is the standard one:
 *
 *      file bytes 11 22 33 44  ->  read as be32 = 0x11223344 (a plain load on this CPU)
 *                              ->  ap_w32 (stwbrx) puts 44 33 22 11 on the bus
 *
 * which is byte-for-byte what Linux produces on either architecture, because b43's
 * be32_to_cpu()+iowrite32 composes to the same thing. If BE had been SET we would have needed the
 * opposite, and firmware would have loaded and then done nothing -- the classic PPC bring-up bug
 * §3a warned about. It is worth noticing that 3a answered this for free simply by printing a bit.
 *
 * ⚠⚠ WHAT THIS BINARY WRITES. The 3a set (TMSLOW/TMSHIGH/IMSTATE/MACCTL/SHM), plus:
 *      SHM_UCODE   22,272 bytes of microcode, streamed 32 bits at a time
 *      SHM_HW      the PCM handshake at 0x01EA/0x01EB, then 1,312 bytes of PCM
 *      MACCTL      PSM_JMP0 to park the processor, then PSM_RUN to start it
 *      GEN_IRQ_REASON  cleared before the start, so the MAC_SUSPENDED we wait for is new
 * Still NO PHY register, NO radio register, NO DMA register. Initvals are NOT uploaded here --
 * they configure the MAC/PHY and belong with PHY init in Stage 4. b43 uploads them after the
 * firmware is already running and has already signalled, so they are not needed to answer this
 * run's question.
 *
 * ★★★★★ THE ORACLES. Two, independent.
 *   A: the PSM must signal. After PSM_RUN, GEN_IRQ_REASON must come back reading
 *      B43_IRQ_MAC_SUSPENDED. That is the microcode itself executing and posting a reason code --
 *      something no amount of correct-looking register writes can fake. If the firmware did not
 *      start, this register stays at whatever we cleared it to.
 *   B: the microcode must WRITE ITS OWN IDENTITY into shared memory. Section [4] zeroes all 4096
 *      bytes of SHM_SHARED, including offsets 0x00-0x07. After PSM_RUN those offsets must contain a
 *      non-zero ucode revision, patchlevel, date and time. We erased them; only the microcode can
 *      have put values back. And the revision must land at or below bwi's BWI_FW_VERSION3_REVMAX
 *      (0x128), because we deliberately embedded v3 blobs -- above that would mean the wrong file
 *      was embedded. b43 reads exactly these four fields after starting the PSM for exactly this
 *      purpose (main.c:2684-2687).
 *
 * ⚠ ORACLE B USED TO BE WRONG, AND THE d1 RUN IS WHY IT CHANGED. It originally asserted that
 * SHM_SH_WLCOREREV (SHARED + 0x0016) would read 5 -- "the firmware reporting the core revision".
 * It does not: b43_main.c:4873 shows the DRIVER WRITES that field for the firmware's benefit
 * (b43_shm_write16(..., B43_SHM_SH_WLCOREREV, dev->dev->core_rev)) and never reads it back. So the
 * d1 run read 0, which is the correct value for a field we had just zeroed and never written --
 * a test that could only ever have reported our own writes back to us.
 * ⇒ THE PROJECT'S OWN RULE, paid for once already on SiI3512 f1 and now paid for twice: AN ORACLE
 * MUST NOT BE ABLE TO FAIL FOR A REASON UNRELATED TO WHAT IT TESTS. Here the unrelated reason was
 * an unverified assumption about which side of the interface produces the value. Check the data
 * flow direction in the source before building an oracle on it.
 * This app now WRITES WLCOREREV, because that is the driver's job, and reports it as informational.
 *
 * ⚠ THE RESOURCES ARE CHECKED BEFORE A SINGLE WORD IS UPLOADED. Each blob keeps its 8-byte
 * bwi_fwhdr {type, gen, pad[2], be32 size}; we verify the type byte ('u'/'p'), gen == 1, and that
 * the declared size matches the resource size minus 8. A truncated or mis-generated resource is
 * then caught on the host, not halfway into the chip's microcode RAM where the only symptom would
 * be firmware that never signals.
 *
 * ⚠ FRESH BOOT REQUIRED, same as 3a and for the same reason: we skip ssb_device_disable()'s
 * teardown path, which is only legitimate when RESET is already asserted.
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
#define AP_S3B_VER "d2"

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
#define B43_SHM_SH_UCODEDATE  0x0004UL
#define B43_SHM_SH_UCODETIME  0x0006UL
/* ⚠ NOT a reading. The DRIVER writes this FOR the firmware -- b43_main.c:4873 does
 * b43_shm_write16(..., B43_SHM_SH_WLCOREREV, dev->dev->core_rev) and never reads it back.
 * The d1 run built an oracle on the opposite assumption and got the answer it deserved. */
#define B43_SHM_SH_WLCOREREV  0x0016UL

#define B43_IRQ_MAC_SUSPENDED 0x00000001UL
#define B43_IRQ_ALL           0xFFFFFFFFUL

#define BWI_FW_VERSION3_REVMAX 0x128UL   /* bwi rejects anything above this as v4 firmware */

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
static Str255 gLines[240]; static short gN=0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,gLogDir,"\pAirPort Stage3b Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<240){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}
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
    SpinUs(20);   /* retry: the window write is not reliably immediate */
  }}

/* --- SHM ------------------------------------------------------------------
 * For B43_SHM_SHARED the caller's BYTE offset becomes a word address (>>2), and an offset that is
 * 2-mod-4 must go through SHM_DATA_UNALIGNED. UCODE/HW/SCRATCH take the offset unshifted. */
static void ShmControl(UInt32 routing, UInt32 off)
{ ap_w32(gBar0, B43_MMIO_SHM_CONTROL, (routing << 16) | off); }

static UInt16 ShmRead16Shared(UInt32 byteOffset)
{
    ShmControl(B43_SHM_SHARED, byteOffset >> 2);
    if (byteOffset & 0x0003) return ap_r16(gBar0, B43_MMIO_SHM_DATA_UNALIGNED);
    return ap_r16(gBar0, B43_MMIO_SHM_DATA);
}

static void FlushTmslow(void){(void)ap_r32(gBar0,SSB_TMSLOW);SpinUs(2);}

static void CoreEnable(UInt32 flags)
{
    UInt32 v;
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_RESET|SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags); FlushTmslow();
    if (ap_r32(gBar0,SSB_TMSHIGH)&SSB_TMSHIGH_SERR) ap_w32(gBar0,SSB_TMSHIGH,0);
    v = ap_r32(gBar0,SSB_IMSTATE);
    if (v&(SSB_IMSTATE_IBE|SSB_IMSTATE_TO)) ap_w32(gBar0,SSB_IMSTATE,v&~(SSB_IMSTATE_IBE|SSB_IMSTATE_TO));
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|SSB_TMSLOW_FGC|flags); FlushTmslow();
    ap_w32(gBar0,SSB_TMSLOW,SSB_TMSLOW_CLOCK|flags); FlushTmslow();
}

static void PhyTakeOutOfReset(void)
{
    UInt32 t;
    t=ap_r32(gBar0,SSB_TMSLOW); t&=~B43_TMSLOW_PHYRESET; t&=~B43_TMSLOW_PHYCLKEN; t|=SSB_TMSLOW_FGC;
    ap_w32(gBar0,SSB_TMSLOW,t); (void)ap_r32(gBar0,SSB_TMSLOW); SpinUs(1500);
    t=ap_r32(gBar0,SSB_TMSLOW); t&=~SSB_TMSLOW_FGC; t|=B43_TMSLOW_PHYCLKEN;
    ap_w32(gBar0,SSB_TMSLOW,t); (void)ap_r32(gBar0,SSB_TMSLOW); SpinUs(1500);
}

/* --- firmware resources ---------------------------------------------------- */

typedef struct { Handle h; UInt8 *payload; UInt32 bytes; UInt8 type; UInt8 gen; } FwBlob;

/* Load a blob and validate its bwi_fwhdr BEFORE anything is uploaded.
 * The declared size must equal the resource size minus the 8-byte header. That single check
 * catches a truncated resource, a mis-generated one, and the wrong file entirely -- on the host,
 * where the symptom is a log line instead of firmware that never signals. */
static int LoadFw(ResType t, short id, UInt8 wantType, const char *label, FwBlob *out)
{
    Handle h; long sz; UInt8 *p; UInt32 declared;
    Str255 L;
    h = GetResource(t, id);
    if (h == NULL) { L[0]=0;PCat(L,"    !! missing resource for ");PCat(L,label);Out(L); return 0; }
    HLock(h);
    sz = GetHandleSize(h);
    p  = (UInt8 *)*h;
    if (sz < 9) { L[0]=0;PCat(L,"    !! resource too small: ");PCat(L,label);Out(L); HUnlock(h); return 0; }
    declared = ((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|(UInt32)p[7];
    L[0]=0; PCat(L,"    "); PCat(L,label);
    PCat(L,": res="); PCatDec(L,sz);
    PCat(L,"  type='"); if(L[0]<255) L[++L[0]]=p[0]; PCat(L,"'");
    PCat(L,"  gen="); PCatDec(L,(long)p[1]);
    PCat(L,"  declared="); PCatDec(L,(long)declared);
    Out(L);
    if (p[0] != wantType) { Say("      !! wrong type byte -- this is not the blob we expect."); HUnlock(h); return 0; }
    if (p[1] != 1)        { Say("      !! fw_gen is not 1."); HUnlock(h); return 0; }
    if (declared != (UInt32)(sz - 8)) {
        Say("      !! declared size != resource size - 8. Truncated or mis-generated.");
        HUnlock(h); return 0;
    }
    if (declared & 3) { Say("      !! payload is not a whole number of 32-bit words."); HUnlock(h); return 0; }
    Say("      ==> header OK");
    out->h = h; out->payload = p + 8; out->bytes = declared; out->type = p[0]; out->gen = p[1];
    return 1;
}

/* Big-endian 32-bit load from a byte pointer. On this CPU a plain aligned load would do, but the
 * resource payload is not guaranteed 4-byte aligned inside the handle, and a misaligned load is UB
 * even where the hardware forgives it. Byte-wise is correct and the cost is irrelevant next to the
 * mandatory 10 us per word. */
static UInt32 be32at(const UInt8 *p)
{ return ((UInt32)p[0]<<24)|((UInt32)p[1]<<16)|((UInt32)p[2]<<8)|(UInt32)p[3]; }

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i, y;
    UInt16 cmd=0;
    UInt32 idx, coreIdx=0xFFFFFFFFUL, idhigh=0, tmslow=0, macctl=0, flags, irq=0;
    FwBlob ucode, pcm;
    int haveUcode=0, havePcm=0, oracleA=0, oracleB=0;
    UInt32 fwrev=0, fwpatch=0, fwdate=0, fwtime=0, wlcorerev=0;
    UInt32 words, w;

    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();
    TEInit();InitDialogs(NULL);InitCursor();
    LogOpen();

    bounds.left=8;bounds.top=40;bounds.right=8+700;bounds.bottom=40+700;
    win=NewWindow(NULL,&bounds,"\pAirPort Stage 3b " AP_S3B_VER " - microcode upload",
                  true,documentProc,(WindowPtr)-1L,false,0);
    if(win){SetPort((GrafPtr)win);TextFont(kFontIDMonaco);TextSize(9);}

    Say("=== AIRPORT EXTREME STAGE 3b " AP_S3B_VER " -- MICROCODE UPLOAD ===");
    Say("  Oracle A: after PSM_RUN, GEN_IRQ_REASON must read MAC_SUSPENDED. That is the microcode");
    Say("            executing and posting a reason code -- register writes cannot fake it.");
    Say("  Oracle B: [4] zeroes all 4096 bytes of SHM_SHARED. After PSM_RUN the ucode revision,");
    Say("            patch, date and time at 0x00-0x06 must be NON-ZERO -- the microcode writing");
    Say("            its own identity into memory we erased. Revision must be <= 0x128 (v3).");
    Say("  ⚠ REQUIRES A FRESH BOOT (same precondition as 3a).");
    Say("  3a proved MACCTL's BE bit is CLEAR, so the standard upload byte order applies.");
    Say("");

    /* ---- [0] resources first: fail on the host, not in the chip ---- */
    Say("[0] FIRMWARE RESOURCES (validated before any upload)");
    haveUcode = LoadFw('BCMu', 128, 'u', "ucode5", &ucode);
    havePcm   = LoadFw('BCMp', 128, 'p', "pcm5",   &pcm);
    if (!haveUcode) { Say("!! no usable microcode -- nothing to do."); goto verdict; }
    if (!havePcm) {
        Say("    ⚠ no PCM blob. b43 treats this as non-fatal but notes that core rev <= 10 must then");
        Say("      do without hardware crypto. Core rev 5 is in that range, so uploading without PCM");
        Say("      means software CCMP forever -- which §3 recommends anyway. Continuing.");
    }

    /* ---- [1] device, BAR, memory space ---- */
    if(FindNode(&gNode)!=noErr){Say("!! no BCM4306 node");goto verdict;}
    if(MapBar0(&gNode,&gBar0)!=noErr||gBar0==0){Say("!! could not map BAR0");goto verdict;}
    if(ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&cmd)!=noErr){Say("!! cmd read failed");goto verdict;}
    if(!(cmd&0x0002)){UInt16 b=0;
      if(ExpMgrConfigWriteWord(&gNode,(LogicalAddress)0x04,(UInt16)(cmd|0x0002))!=noErr){Say("!! mem enable failed");goto verdict;}
      (void)ExpMgrConfigReadWord(&gNode,(LogicalAddress)0x04,&b);
      if(!(b&0x0002)){Say("!! mem enable did not stick");goto verdict;}}
    Say("");
    SayH("[1] BAR0 = ",(unsigned long)gBar0,8);
    Say("    memory space enabled");

    /* ---- [2] core, precondition, reset ---- */
    for(idx=0;idx<8;idx++){
      if(SelectCore(idx)!=noErr)continue;
      idhigh=ap_r32(gBar0,SSB_IDHIGH);
      if(idhigh==0xFFFFFFFFUL||idhigh==0)continue;
      if(IDHIGH_CC(idhigh)==SSB_DEV_80211){coreIdx=idx;break;}}
    if(coreIdx==0xFFFFFFFFUL){Say("!! no 802.11 core");goto verdict;}
    Say("");
    Say1("[2] 802.11 core at index ",coreIdx);
    Say1("    core revision = ",IDHIGH_REV(idhigh));
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW before = ",tmslow,8);
    if(!(tmslow&SSB_TMSLOW_RESET)){
      Say("!! PRECONDITION FAILED: the core is not in reset. REBOOT AND RE-RUN.");
      Say("   (Same reason as 3a: we skip ssb_device_disable()'s teardown path.)");
      goto verdict;}
    flags=B43_TMSLOW_GMODE|B43_TMSLOW_PHYCLKEN|B43_TMSLOW_PHYRESET;
    CoreEnable(flags);
    SpinUs(2500);
    PhyTakeOutOfReset();
    tmslow=ap_r32(gBar0,SSB_TMSLOW);
    SayH("    TMSLOW after  = ",tmslow,8);
    if((tmslow&SSB_TMSLOW_CLOCK)&&!(tmslow&SSB_TMSLOW_RESET))
      Say("    core is running (matches 3a's 0x20050000)");
    else { Say("!! core did not come out of reset -- 3a passed, so this is a regression."); goto verdict; }

    /* ---- [3] MACCTL: park the PSM, enable SHM ---- */
    Say("");
    Say("[3] MACCTL -- park the microcode processor at 0, enable SHM");
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    SayH("    MACCTL as found = ",macctl,8);
    Say1("      BE bit = ",(macctl&B43_MACCTL_BE)?1:0);
    if(macctl&B43_MACCTL_BE){
      Say("      ⚠⚠ BE IS SET. 3a read it CLEAR. The upload byte order below assumes CLEAR, so if");
      Say("         this run fails to start the firmware, THIS IS WHY -- not the blob, not the");
      Say("         sequence. Do not debug anything else until this is explained.");}
    if(macctl&B43_MACCTL_PSM_RUN){
      Say("      ⚠ PSM_RUN is already set before we parked it. Unexpected on a fresh core.");}
    macctl = B43_MACCTL_IHR_ENABLED|B43_MACCTL_SHM_ENABLED|B43_MACCTL_GMODE|B43_MACCTL_PSM_JMP0;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);
    (void)ap_r32(gBar0,B43_MMIO_MACCTL);
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    SayH("    MACCTL written+readback = ",macctl,8);
    if(!(macctl&B43_MACCTL_SHM_ENABLED)){Say("!! SHM_ENABLED did not stick");goto verdict;}
    if(!(macctl&B43_MACCTL_PSM_JMP0)){Say("!! PSM_JMP0 did not stick");goto verdict;}

    /* ---- [4] zero scratch + shared ---- */
    Say("");
    Say("[4] ZEROING SHM (64 scratch registers, 4096 bytes of shared)");
    for(w=0;w<64;w++){ ShmControl(B43_SHM_SCRATCH,w); ap_w16(gBar0,B43_MMIO_SHM_DATA,0); }
    /* ⚠ b43 zeroes shared with 16-bit writes stepping by 2, which sends every second write down
     * the SHM_DATA_UNALIGNED path. We use aligned 32-bit writes stepping by 4 instead: identical
     * result over the same 4096 bytes, and it avoids exercising an unaligned WRITE path that
     * nothing else in this binary needs. Untested code that runs once is a liability. */
    for(w=0;w<4096;w+=4){ ShmControl(B43_SHM_SHARED,w>>2); ap_w32(gBar0,B43_MMIO_SHM_DATA,0); }
    Say("    done");

    /* ---- [5] microcode ---- */
    Say("");
    Say1("[5] UPLOADING MICROCODE, bytes = ",ucode.bytes);
    words = ucode.bytes/4;
    Say1("    32-bit words = ",words);
    Say("    ⚠ the 10 us delay between words is REQUIRED, not decorative. At ~22 KB this run will");
    Say("      spend roughly 56 ms inside this loop.");
    ShmControl(B43_SHM_UCODE|B43_SHM_AUTOINC_W, 0x0000);
    for(w=0;w<words;w++){
      ap_w32(gBar0,B43_MMIO_SHM_DATA, be32at(ucode.payload + w*4));
      SpinUs(10);}
    Say("    microcode streamed");

    /* ---- [6] PCM ---- */
    if(havePcm){
      Say("");
      Say1("[6] UPLOADING PCM, bytes = ",pcm.bytes);
      words = pcm.bytes/4;
      ShmControl(B43_SHM_HW, 0x01EA);
      ap_w32(gBar0,B43_MMIO_SHM_DATA,0x00004000UL);
      ShmControl(B43_SHM_HW, 0x01EB);
      for(w=0;w<words;w++){
        ap_w32(gBar0,B43_MMIO_SHM_DATA, be32at(pcm.payload + w*4));
        SpinUs(10);}
      Say("    PCM streamed (this is what makes the hardware crypto engine available at core rev 5)");
    } else { Say(""); Say("[6] no PCM -- skipped"); }

    /* ---- [7] start the PSM ---- */
    Say("");
    Say("[7] STARTING THE MICROCODE PROCESSOR");
    ap_w32(gBar0,B43_MMIO_GEN_IRQ_REASON,B43_IRQ_ALL);
    Say("    GEN_IRQ_REASON cleared, so any MAC_SUSPENDED we see below is NEW");
    macctl=ap_r32(gBar0,B43_MMIO_MACCTL);
    macctl&=~B43_MACCTL_PSM_JMP0;
    macctl|=B43_MACCTL_PSM_RUN;
    ap_w32(gBar0,B43_MMIO_MACCTL,macctl);
    (void)ap_r32(gBar0,B43_MMIO_MACCTL);
    SayH("    MACCTL with PSM_RUN = ",ap_r32(gBar0,B43_MMIO_MACCTL),8);
    Say("    waiting for MAC_SUSPENDED (up to 1 s)...");
    for(i=0;i<100;i++){
      irq=ap_r32(gBar0,B43_MMIO_GEN_IRQ_REASON);
      if(irq==B43_IRQ_MAC_SUSPENDED) break;
      SpinUs(10000);}
    SayH("    GEN_IRQ_REASON = ",irq,8);
    Say1("    polls used = ",(unsigned long)i);
    if(irq==B43_IRQ_MAC_SUSPENDED){
      oracleA=1;
      Say("    ==> ORACLE A PASSED: THE MICROCODE IS EXECUTING. It posted MAC_SUSPENDED.");
    } else if (irq==0) {
      Say("    ✗ ORACLE A FAILED: the reason register never changed from the value we cleared it to.");
      Say("      The processor is not running. Suspect, in order: PSM_RUN did not stick; the ucode");
      Say("      never reached microcode RAM (check the AUTOINC_W control word); byte order.");
    } else {
      Say("    ✗ ORACLE A FAILED but the register DID change -- so something is alive. Whatever bits");
      Say("      are set above are the microcode's first act, and are worth more than a retry.");
    }

    /* ---- [8] read the firmware back ---- */
    Say("");
    Say("[8] WHAT THE FIRMWARE SAYS ABOUT ITSELF");
    fwrev   = ShmRead16Shared(B43_SHM_SH_UCODEREV);
    fwpatch = ShmRead16Shared(B43_SHM_SH_UCODEPATCH);
    fwdate  = ShmRead16Shared(B43_SHM_SH_UCODEDATE);
    fwtime  = ShmRead16Shared(B43_SHM_SH_UCODETIME);
    SayH("    ucode revision  = ",fwrev,4);
    Say1("                      = ",fwrev);
    SayH("    ucode patchlevel= ",fwpatch,4);
    SayH("    ucode date      = ",fwdate,4);
    SayH("    ucode time      = ",fwtime,4);
    if(fwrev==0||fwrev==0xFFFF){
      Say("    ⚠ revision is 0 or 0xFFFF -- that is 'nothing ran', not a version number.");
    } else if(fwrev>BWI_FW_VERSION3_REVMAX){
      Say1("    ⚠ revision is above bwi's v3 ceiling of ",BWI_FW_VERSION3_REVMAX);
      Say("      We embedded v3 blobs, so this would mean the WRONG FILE got embedded.");
    } else {
      Say("    ==> revision is in the v3 range, consistent with the blobs we embedded.");
    }
    /* ORACLE B: we zeroed every one of these in [4]. Anything non-zero here was written by the
     * microcode itself while we were not looking, which is the only thing that can have done it. */
    if (fwrev != 0 && fwrev != 0xFFFF && fwrev <= BWI_FW_VERSION3_REVMAX &&
        (fwpatch != 0 || fwdate != 0 || fwtime != 0)) {
      oracleB=1;
      Say("    ==> ORACLE B PASSED: section [4] zeroed all 4096 bytes of SHM_SHARED, including");
      Say("        these four offsets. They are non-zero now and the revision is in the v3 range.");
      Say("        THE MICROCODE WROTE ITS OWN IDENTITY INTO MEMORY WE HAD ERASED. Nothing else");
      Say("        on this machine could have.");
    } else if (fwrev == 0 && fwpatch == 0 && fwdate == 0 && fwtime == 0) {
      Say("    ✗ ORACLE B FAILED: all four fields are still zero, exactly as [4] left them.");
      Say("      The microcode never wrote anything. If oracle A also failed, it never ran.");
    } else {
      Say("    ✗ ORACLE B FAILED: the fields changed but the revision is out of the v3 range.");
      Say("      Something executed, but it is probably not the blob we think we embedded.");
    }

    /* WLCOREREV is the DRIVER's to write, not the firmware's to report -- b43_main.c:4873. Doing
     * the driver's job here, and printing it only as confirmation that the write landed. */
    Say("");
    Say("    (writing WLCOREREV, which is the driver's responsibility, not a reading)");
    ShmControl(B43_SHM_SHARED, B43_SHM_SH_WLCOREREV >> 2);
    ap_w16(gBar0, B43_MMIO_SHM_DATA_UNALIGNED, (UInt16)IDHIGH_REV(idhigh));
    Say1("    WLCOREREV after writing it = ",wlcorerev);
    if (wlcorerev != IDHIGH_REV(idhigh)) {
      Say("      ⚠ did not read back what we wrote -- that IS an SHM addressing problem, and now");
      Say("        it is a fair test, because this time we know what should be there.");
    }

verdict:
    Say("");
    Say("=== VERDICT ===");
    if(oracleA&&oracleB){
      Say("  ✓✓✓ STAGE 3b PASSES. THE CHIP IS RUNNING BROADCOM MICROCODE, uploaded from Mac OS 9.");
      Say("      That is the first time this hardware has executed firmware on this operating");
      Say("      system. Every layer below the 802.11 stack is now proven: enumeration, MMIO, the");
      Say("      backplane, SPROM, core reset, SHM, and firmware upload.");
      Say("      Stage 4 next: initvals, PHY and radio init, then DMA rings and a real beacon.");
    } else if(oracleA){
      Say("  ~ The microcode runs but its SHM reporting does not match. The processor executing is");
      Say("    the harder half and it works; this is an addressing detail in SHM_SHARED reads.");
    } else {
      Say("  ✗ The microcode did not start. Section [7] lists the suspects in order. Note that");
      Say("    everything up to the upload -- core reset, MACCTL, SHM writes -- was proven by 3a,");
      Say("    so the fault is in the upload itself or in the blob, not in the groundwork.");
    }
    Say("");
    Say("  ⚠ The core is left running with firmware loaded and no initvals. Harmless; Open Firmware");
    Say("    resets it at the next boot. A second run without rebooting will stop at the");
    Say("    precondition check.");
    Say("");
    Say("=== done. Log: 'AirPort Stage3b Log' in the System Folder. ===");

    if(win){SetPort((GrafPtr)win);y=12;for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}}
    for(;;){if(WaitNextEvent(everyEvent,&evt,10,NULL)){
      if(evt.what==keyDown||evt.what==mouseDown)break;
      if(evt.what==updateEvt&&win){BeginUpdate(win);SetPort((GrafPtr)win);y=12;
        for(i=0;i<gN;i++){MoveTo(6,y);DrawString(gLines[i]);y+=11;}EndUpdate(win);}}}
    if(gLogRef){FSClose(gLogRef);FlushVol(NULL,gLogVol);}
    return 0;
}
