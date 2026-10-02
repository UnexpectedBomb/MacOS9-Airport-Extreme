/* airport_reset.r -- resources for the core reset-cycle probe.
 *
 * Small partition: no firmware is embedded and nothing is loaded. The probe only cycles the
 * 802.11 core and reads two registers per cycle. */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    400 * 1024, 350 * 1024
};

resource 'vers' (1, "AirPortReset") {
    0x01, 0x00, development, 0x00, verUS,
    "r1", "r1 AirPort core reset cycle: ssb_device_disable -- NO fresh boot required"
};
