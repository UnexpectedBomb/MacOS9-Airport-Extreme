/* ap_ndrv.h -- the OPEN TRANSPORT driver shape: TheDriverDescription + ValidateHardware.
 *
 * ★★★ WHY. Our port does not exist until the test application calls EnetShimInstallDriver, so
 * Open Transport never learns about it at boot and the TCP/IP control panel has nothing to
 * offer. We need code that runs at boot with no process to own it.
 *
 * ⛔⛔ AND THE FIRST THREE ATTEMPTS AT THAT WERE ALL THE SAME MISTAKE. k100 made the file type
 *   'ndrv'; k101 added a cfrg member with architecture 'ndrv'; both assumed the DEVICE MANAGER
 *   was the thing that loads a network driver at boot, and both failed silently. The premise was
 *   wrong, and it came from mis-reading Apple's cfrg -- I recorded "OTModl$radio, pwpc AND ndrv
 *   arch" when the file actually declares THREE members, ALL pwpc, ALL kImportLibraryCFrag:
 *
 *       "WirelessAPI"      pwpc  ImportLibrary  dataFork off=0      len=73284
 *       "OTPortCfg$radio"  pwpc  ImportLibrary  dataFork off=73296  len=1341
 *       "OTModl$radio"     pwpc  ImportLibrary  dataFork off=74640  len=65261
 *
 *   There is no 'ndrv' architecture anywhere in Apple's AirPort Driver. Two hardware runs were
 *   spent testing a shape that does not exist, because I theorised from a note instead of
 *   re-reading the file the note came from. [[feedback_read_the_flag_not_the_prose]].
 *
 * ★★★ WHAT ACTUALLY LOADS IT -- read off four of Apple's own fragments, not reasoned about.
 *
 *   `Apple Enet` declares seven members, and two of them are named EXACTLY like Open Firmware
 *   node names: "pci1011,14" and "pci1011,9" (DEC 21140 and 21041), alongside "bmac" and "mace"
 *   which are the node names of the built-in Ethernet on those Macs. Their exports are not the
 *   Device Manager's:
 *
 *       bmac / pci1011,14 :  ValidateHardware  InitStreamModule  TerminateStreamModule
 *                            GetOTInstallInfo  TheDriverDescription      (and NO DoDriverIO,
 *                                                                        and NO main)
 *
 *   Those four are OpenTransportKernel.h's module contract, and its comment on the first one
 *   settles where boot-time execution comes from:
 *
 *       "This function will be called only once, at system boot time, before installing
 *        your driver into the Open Transport module registry."
 *
 *   So: OPEN TRANSPORT is the I/O expert for networking, it finds the driver as a CFM fragment
 *   NAMED AFTER THE NAME REGISTRY NODE, and it calls ValidateHardware at boot. The Device
 *   Manager is not involved at all -- which the descriptors say out loud, see driverRuntime.
 *
 * ⛔ k102 RAN THAT AND OT DID NOT CALL US EITHER. The witness worked this time (it sat in the
 *   app, ahead of CFM, and could say no) and it said no: no node carried "airport-ot-validate".
 *   The two-name cfrg was harmless -- GetSharedLibrary("AirPortShim") still returned 0 -- so
 *   nothing was broken, the hook simply never fired.
 *
 * ★★★★★ AND THE REASON IS THAT NODE-NAMED FRAGMENTS ARE THE **ROM/PCI-DISCOVERY** ROUTE, WHICH
 *   AN EXTENSION IS NOT PART OF. k102's own port listing is what gave it away:
 *
 *       [1] enet0  module=gmac   ACTIVE SYSTEM-REGISTERED
 *
 *   The live built-in Ethernet port names module "gmac" -- and there is NO fragment called
 *   "gmac", and no "OTModl$gmac", in ANY file in the Extensions folder. That driver comes from
 *   the Mac OS ROM. bmac / mace / pci1011,14 are the same kind of thing. Copying them was
 *   copying a route that starts before the Extensions folder is ever read.
 *
 * ★★★★★ THE RIGHT SIBLING IS APPLE'S **USB ETHERNET**, and it is the same EnetShimLib family we
 *   already build on. `Apple Enet DLPI Support` declares THREE fragments:
 *
 *       "OTModl$USBEnet"                the STREAMS/DLPI module
 *       "OTPortCfg$USBEnetCfgHelper"    exports OTGetUserPortName + LoadShim
 *       "OTKrnl$USBOTPortScanner"       exports **OTScanPorts** + CFMInitialize
 *
 *   The third one is the piece that was missing, and it is documented:
 *
 *       OpenTransport.h:245        #define kOTKernelPrefix "OTKrnl$"
 *       OpenTransportKernel.h:1361 "Your port-scanning ASLM function set must use the prefix
 *                                   kOTPortScannerPrefix."
 *       OpenTransportKernel.h:1380 "Your port-scanning function must be exported by the name
 *                                   \"OTScanPorts\"."   #define kOTScanPortsID "OTScanPorts"
 *       typedef CALLBACK_API_C(void, PortScanProcPtr)(UInt32 scanType);
 *       enum { kOTInitialScan = 0, kOTScanAfterSleep = 1 };
 *
 *   A PORT SCANNER is what creates the port record at boot, so TCP/IP has something to list.
 *   Apple's suffix is free-form ("USBOTPortScanner" contains no "pScnr"), so OT is finding
 *   scanners by the OTKrnl$ PREFIX, not by an exact tag. Ours is named to match that shape.
 */
#ifndef AP_NDRV_H
#define AP_NDRV_H

/* Hand-rolled so the initialiser below is readable and its layout is exactly what the expert
 * expects. Mirrors DriverFamilyMatching.h's DriverDescription field for field. */
typedef struct { UInt8 len; char s[31]; }            ApDStr31;
typedef struct { UInt8 a, b, c, d; }                 ApDNumVer;
typedef struct { OSType category, type; ApDNumVer v; } ApDServiceInfo;
typedef struct {
    OSType     sig;
    UInt32     descVersion;
    ApDStr31   nameInfoStr;
    ApDNumVer  typeVersion;
    UInt32     driverRuntime;
    ApDStr31   driverName;
    UInt32     reserved[8];
    UInt32     nServices;
    ApDServiceInfo service0;
} ApDriverDesc;

/* OpenTransportKernel.h's cookie, declared here rather than dragging the whole kernel header
 * into a file that only needs three fields. Field for field from that header. */
typedef struct { RegEntryID fTheID; void *fConfigurationInfo; ByteCount fConfigurationLength; }
        ApOTPCIInfo;

/* ★★★ nameInfoStr, READ OFF THE MACHINE AT k98 RATHER THAN GUESSED.
 *
 * The convention for an Open Firmware PCI node is "pci<vendor>,<device>", so with a 14e4:4320
 * card the obvious guess was "pci14e4,4320". k98 asked the machine and got "pci80211" -- a
 * GENERIC CLASS name, which nothing about the convention would have told us.
 *
 * ⚠ And this field is load-bearing twice over now: Apple's three descriptors each carry the
 *   fragment's OWN NAME here ("radio", "pci1011,14", "bmac"), so nameInfoStr and the cfrg member
 *   name are the same string. Ours must therefore be "pci80211" in BOTH places -- see
 *   make-cfrg.py, which emits the second member under exactly this name. */
#define AP_NDRV_NODE_NAME "pci80211"

/* ★ EVERY FIELD BELOW WAS EXTRACTED FROM A SHIPPING APPLE DRIVER, NOT CHOSEN.
 *
 * Decoding TheDriverDescription out of OTModl$radio, pci1011,14 and bmac gives the same shape
 * in all three (the PEF's pattern-initialised data unpacked to exactly the section's initSz in
 * each case, which is what proves the offsets were read correctly):
 *
 *     signature     'mtej'
 *     nameInfoStr   "radio" / "pci1011,14" / "bmac"      == the fragment name
 *     driverRuntime 0x00000004                           == kDriverIsUnderExpertControl
 *     driverName    "radio" / "enet" / "enet"
 *     nServices     1
 *     service[0]    category 'otan'  type 0x000A0B01
 *
 * ⚠⚠ driverRuntime WAS OUR BUG. We had 0x03 = kDriverIsLoadedUponDiscovery|kDriverIsOpenedUponLoad,
 *   which asks the DEVICE MANAGER to auto-load us on discovery. Apple's drivers set 0x04, and
 *   DriverFamilyMatching.h:96 glosses it "I/O expert handles loads/opens" -- they are explicitly
 *   telling the Device Manager NOT to. We spent k100 and k101 asking the DM to do something that
 *   Apple's own descriptor asks it not to do.
 *
 * 0x000A0B01 decodes through OTPCIServiceType(devType, framing, isTPI, isDLPI) as
 *   devType 0x0A = kOTEthernetDevice, framing 0x0B = kOTFramingEthernet|kOTFramingEthernetIPX|
 *   kOTFraming8022, isTPI 0, isDLPI 1. Ethernet and DLPI is exactly what our EnetHAL path is. */
ApDriverDesc TheDriverDescription = {
    0x6D74656AUL,                       /* 'mtej' */
    0,                                  /* kInitialDriverDescriptor -- as pci1011,14 and bmac */
    { 10, "AirPortBCM" },               /* 8-82 k188: nameInfo = the MODULE name, as OTModl$USBEnet
                                         * carries "USBEnet" (its module name). Was the node name
                                         * "pci80211" for the abandoned Device-Manager/node route; the
                                         * OTKrnl$ scanner route keys on fModuleName instead. */
    { 1, 0, 0x80, 0 },                  /* 1.0 final */
    0x00000004UL,                       /* kDriverIsUnderExpertControl -- OT loads us, not the DM */
    { 4, "enet" },                      /* both of Apple's wired PCI drivers use this */
    { 0,0,0,0,0,0,0,0 },
    1,
    { 0x6F74616EUL /* 'otan' = kServiceCategoryOpenTransport */,
      0x000A0B01UL /* OTPCIServiceType(kOTEthernetDevice, Ether|EtherIPX|8022, 0, 1) */,
      { 1, 0, 0x80, 0 } }
};

/* How far the expert got, if it ever reaches us. Read back through AirPortShimGetNdrv. */
static volatile UInt32 gNdrvInitSeen = 0, gNdrvOpenSeen = 0, gNdrvLastCode = 0xFFFFFFFFUL;
static volatile UInt32 gVhSeen = 0, gVhHadID = 0, gVhBeaconErr = 0xFFFFFFFFUL;
static volatile UInt32 gScanSeen = 0, gScanType = 0xFFFFFFFFUL, gScanBeaconErr = 0xFFFFFFFFUL;

/* ★★★ THE BOOT-TIME ENTRY POINT. Open Transport calls this once, at boot, before installing us
 * into its module registry -- OpenTransportKernel.h:2507.
 *
 * ⚠ 8-82 k188 UPDATE: this now ACCEPTS (returns 0). The block below is the k8-11 rationale for why
 *   it once declined -- kept for history. Back then the OTModl was incomplete and there was no scanner,
 *   so installing us at boot risked driving qinit procedures that did not exist. Both are resolved now:
 *   the OTModl is the full DLPI module and the OTKrnl$ scanner registers the port, so accepting is
 *   correct and required. The original decline reasoning follows.
 *
 * ⛔⛔ (HISTORICAL, k8-11) IT DELIBERATELY RETURNS AN ERROR, AND THAT IS THE WHOLE DESIGN OF THIS BUILD.
 *
 *   Returning noErr tells OT to go on and install us as a STREAMS driver, at which point it
 *   calls GetOTInstallInfo and starts driving qinit procedures we have not written. At boot.
 *   With no debugger. k92 grey-screened this machine by shipping two changes at once on a
 *   surface exactly this dangerous, and the recovery was a power cycle.
 *
 *   So this build proves ONLY that the hook fires. Declining is not a failure path we are
 *   inventing -- it is the ordinary one, taken every boot by whichever of pci1011,14 and
 *   pci1011,9 is not the card that is present. OT unloads us and carries on, the machine boots
 *   normally, and the app still connects to "AirPortShim" afterwards because the cfrg's first
 *   member registers the library by name regardless.
 *
 * ⚠ The beacon goes on the RegEntryID that OT HANDS US, not on a node we looked up. If OT calls
 *   us with a different node than the one our bring-up found, the beacon lands there and our
 *   search finds it anyway -- and the mismatch is itself the answer. */
OSStatus ValidateHardware(ApOTPCIInfo *param)
{
    gVhSeen++;
    if(param){
        gVhHadID = 1;
        /* A Name Registry property outlives this call and is visible to any fragment copy --
         * [[reference_os9_two_fragment_copies_own_globals]]. gVhSeen only says what THIS copy
         * saw; the property says what ANY copy saw. k100's beacon could not be read at all
         * because the only code that read it lived behind the connection the test had broken,
         * so the run answered nothing. The reader for this one is in the app, ahead of CFM. */
        { UInt32 mark = 0x56484F4BUL;   /* 'VHOK' */
          gVhBeaconErr = (UInt32)RegistryPropertyCreate(&param->fTheID, "airport-ot-validate",
                                                        (void*)&mark,
                                                        (RegPropertyValueSize)sizeof(mark)); }
    }
    return (OSStatus)0;                 /* 8-82 k188: ACCEPT (kOTNoError). Was -1 to DECLINE in the
                                         * k8-11 "prove the hook fires without installing" era -- but the
                                         * OTModl is complete now and our OTKrnl$ scanner registers the
                                         * port, so we WANT OT to install OTModl$AirPortBCM into its
                                         * module registry. OTModl$USBEnet's ValidateHardware is just
                                         * `li r3,0; blr` (accept); the real card check is InitStreamModule,
                                         * which fails gracefully (returns an error, no crash) if the card
                                         * is absent, so OT then declines cleanly. */
}

/* ⚠ DoDriverIO and the patched PEF `main` are now known to be IRRELEVANT to this path: none of
 *   Apple's four comparable fragments export DoDriverIO and three of them have no main at all.
 *   They are left in place only because removing them is a second variable in a build whose
 *   question is "does ValidateHardware fire", and k99/k101 already proved they do not break CFM.
 *   They come out once the expert route is confirmed. */
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    OSErr err = noErr;
    (void)spaceID; (void)contents;
    gNdrvLastCode = (UInt32)code;
    switch(code){
      case kInitializeCommand:
        gNdrvInitSeen++;
        { UInt32 mark = 0x4E445256UL;   /* 'NDRV' */
          (void)RegistryPropertyCreate(&gBus.node, "airport-ndrv-init",
                                       (void*)&mark, (RegPropertyValueSize)sizeof(mark)); }
        err = noErr; break;
      case kOpenCommand:        gNdrvOpenSeen++;  err = noErr; break;
      case kCloseCommand:
      case kFinalizeCommand:
      case kSupersededCommand:
      case kReplaceCommand:
      case kKillIOCommand:      err = noErr;      break;
      case kControlCommand:     err = noErr;      break;
      case kReadCommand:
      case kWriteCommand:       err = ioErr;      break;
      case kStatusCommand:      err = statusErr;  break;
      default:                  err = paramErr;   break; }
    if(kind & kImmediateIOCommandKind) return err;
    return (OSErr)IOCommandIsComplete(cmdID,(short)err);
}

UInt32 AirPortShimGetNdrv(UInt32 *initSeen, UInt32 *openSeen, UInt32 *lastCode)
{
    if(initSeen) *initSeen = gNdrvInitSeen;
    if(openSeen) *openSeen = gNdrvOpenSeen;
    if(lastCode) *lastCode = gNdrvLastCode;
    return gNdrvInitSeen;
}

/* ★★★ THE PORT SCANNER. Open Transport calls this at boot to ask what ports exist.
 *
 * ⛔ IT REGISTERS NOTHING YET, FOR THE SAME REASON ValidateHardware DECLINES.
 *   OTRegisterPort() here is what finally puts "AirPort" in the TCP/IP control panel, and it is
 *   also the first thing this project will have done at boot that OTHER SOFTWARE THEN USES. A
 *   port record naming a module we have not finished, consumed by OT during startup, is a boot
 *   hang. So this increment counts the call and returns, exactly as k102's ValidateHardware did,
 *   and the registration is its own build with its own run.
 *
 * ⚠ The beacon goes on the device-tree ROOT, not on the card's node: OTScanPorts is handed no
 *   RegEntryID (its only argument is scanType), and at boot we have not found the card yet.
 *   Whoever writes it, any fragment copy can read it -- which is the whole point, because CFM
 *   may hand OT and the app separate copies with separate globals
 *   [[reference_os9_two_fragment_copies_own_globals]]. */
void OTScanPorts(UInt32 scanType)
{
    RegEntryID root;
    gScanSeen++;
    gScanType = scanType;                   /* kOTInitialScan 0 / kOTScanAfterSleep 1 */
    if(RegistryCStrEntryLookup(NULL, "Devices:device-tree", &root) == noErr){
        UInt32 mark = 0x53434E31UL;         /* 'SCN1' */
        gScanBeaconErr = (UInt32)RegistryPropertyCreate(&root, "airport-ot-scan",
                                                        (void*)&mark,
                                                        (RegPropertyValueSize)sizeof(mark));
        (void)RegistryEntryIDDispose(&root);
    }
}

UInt32 AirPortShimGetScan(UInt32 *scanType, UInt32 *beaconErr)
{
    if(scanType) *scanType = gScanType;
    if(beaconErr) *beaconErr = gScanBeaconErr;
    return gScanSeen;
}

UInt32 AirPortShimGetVh(UInt32 *hadID, UInt32 *beaconErr)
{
    if(hadID)     *hadID     = gVhHadID;
    if(beaconErr) *beaconErr = gVhBeaconErr;
    return gVhSeen;
}

#endif /* AP_NDRV_H */
