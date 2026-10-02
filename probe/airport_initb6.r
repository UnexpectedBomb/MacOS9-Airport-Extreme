/* airport_initb6.r -- resources for the Stage 4b-ii-2 probe.
 *
 * Carries the firmware blobs in its resource fork (ucode5, pcm5, both initvals tables), so the
 * partition matches the other firmware-bearing probes rather than the small reset-probe size. */
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

resource 'vers' (1, "AirPortInitB6") {
    0x01, 0x00, development, 0x00, verUS,
    "j7", "j7 AirPort 4b-ii-9b: init_pctl -- b43_phy_initg COMPLETE"
};
