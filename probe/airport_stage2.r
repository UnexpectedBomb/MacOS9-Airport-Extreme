/* airport_stage2.r -- resources for the Stage 2 ChipCommon / core census / SPROM probe.
 *
 * Same partition shape as Stage 1: gLines[260] of Str255 plus the Name Registry property buffers.
 * The SPROM is only 128 bytes and is read into a stack array, so there is no large buffer here. */
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

resource 'vers' (1, "AirPortStage2") {
    0x01, 0x00, development, 0x00, verUS,
    "b1", "b1 AirPort Stage 2: ChipCommon, backplane core census, SPROM (no MMIO writes)"
};
