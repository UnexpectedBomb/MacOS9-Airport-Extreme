/* airport_stage4bi.r -- resources for the Stage 4b-i GPIO + PHY-register-path probe.
 *
 * Same partition as 3b/4a: the resource fork carries ~26 KB of firmware that the Resource Manager
 * loads into the heap on GetResource. */
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

resource 'vers' (1, "AirPortStage4bi") {
    0x01, 0x00, development, 0x00, verUS,
    "f5", "f5 AirPort Stage 4b-i: GPIO init + PHY register path (needs a fresh boot)"
};
