/* airport_dma.r -- resources for the Stage 5-1 addressing probe.
 *
 * Carries the firmware blobs because ap_bringup.h requires them ('BCMu' 128, 'BCMp' 128,
 * 'BCMi' 128 and 129), so the partition matches the other firmware-bearing probes.
 *
 * ⚠ The partition is unchanged from initb6 even though this probe allocates ~76 KB of DMA
 * test memory: NewPtrSysClear takes that from the SYSTEM heap, not the application heap. */
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

resource 'vers' (1, "AirPortDma") {
    0x01, 0x00, development, 0x00, verUS,
    "k1", "k1 AirPort 5-1: DMA addressing probe -- width, translation, GetPhysical"
};
