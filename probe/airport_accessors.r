/* airport_accessors.r -- resources for the Stage 4b-ii-1 accessor-layer probe.
 *
 * Small partition: no firmware embedded, nothing loaded. The probe brings the core up and does a
 * handful of register round-trips. */
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

resource 'vers' (1, "AirPortAccessors") {
    0x01, 0x00, development, 0x00, verUS,
    "h3", "h3 AirPort: radio 0x7A path PROVEN via PHY 0x003E; forbidden access removed"
};
