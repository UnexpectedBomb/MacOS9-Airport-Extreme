/* airport_stage3a.r -- resources for the Stage 3a core-reset / SHM self-test probe. */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    450 * 1024, 400 * 1024
};

resource 'vers' (1, "AirPortStage3a") {
    0x01, 0x00, development, 0x00, verUS,
    "c1", "c1 AirPort Stage 3a: 802.11 core reset + SHM self-test (needs a fresh boot)"
};
