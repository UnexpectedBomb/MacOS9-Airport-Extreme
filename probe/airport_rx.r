/* airport_rx.r -- resources for the Stage 5-3 first-received-frame probe (k4).
 *
 * k4 links ap_phy_initg.h, which carries all of Stage 4 -- roughly 2,750 lines more code than
 * k3 had. Still comfortably inside the partition: the binary grew, the heap use did not.
 *
 * Carries the firmware blobs because ap_bringup.h requires them ('BCMu' 128, 'BCMp' 128,
 * 'BCMi' 128 and 129).
 *
 * ⚠ The partition is unchanged: the 32 RX buffer pages and both rings come from the SYSTEM
 * heap via NewPtrSysClear, not the application heap. */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    700 * 1024, 600 * 1024
};

resource 'vers' (1, "AirPortRx") {
    0x01, 0x00, development, 0x00, verUS,
    "k43","k43 AirPort: does the receiver need a transmission first? One dummy TX before the scan"
};
