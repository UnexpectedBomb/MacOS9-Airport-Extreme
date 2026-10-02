/* airport_stage4a.r -- resources for the Stage 4a PHY/radio identity + initvals probe.
 *
 * Same partition as 3b: the app's resource fork carries ~26 KB of firmware (ucode5, pcm5, and now
 * BOTH initvals tables) which the Resource Manager loads into the heap when GetResource'd. */
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

resource 'vers' (1, "AirPortStage4a") {
    0x01, 0x00, development, 0x00, verUS,
    "e3", "e3 AirPort Stage 4a: PHY/radio identity + initvals (needs a fresh boot)"
};
