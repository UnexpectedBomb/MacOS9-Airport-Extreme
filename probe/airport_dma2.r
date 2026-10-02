/* airport_dma2.r -- resources for the Stage 5-2 ring-programming probe.
 *
 * Carries the firmware blobs because ap_bringup.h requires them ('BCMu' 128, 'BCMp' 128,
 * 'BCMi' 128 and 129).
 *
 * ⚠ The partition is unchanged from k1 even though this probe allocates 32 RX buffer pages
 * plus two rings (~136 KB): NewPtrSysClear takes that from the SYSTEM heap, not the
 * application heap. */
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

resource 'vers' (1, "AirPortDma2") {
    0x01, 0x00, development, 0x00, verUS,
    "k2", "k2 AirPort 5-2: DMA rings programmed -- TX and RX, LE descriptors"
};
