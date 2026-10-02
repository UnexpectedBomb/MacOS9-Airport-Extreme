/* airport_probe.r -- resources for the Stage 1 AirPort Extreme enumeration probe.
 *
 * The partition holds gLines[300] of Str255 (~77 KB of static line buffer, larger than the SiI3512
 * probes because a full PCI census plus a 60-property sweep produces far more output than a flash
 * dump's summary) plus the Name Registry property buffers, which are NewPtr'd one at a time and
 * released immediately. There is no DMA buffer and no dump file, so this is smaller than the flash
 * probe despite the longer log. */
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

resource 'vers' (1, "AirPortProbe") {
    0x01, 0x00, development, 0x00, verUS,
    "a1", "a1 AirPort Extreme Stage 1: does OS 9 enumerate a BCM4306? (enumeration only, no MMIO)"
};
