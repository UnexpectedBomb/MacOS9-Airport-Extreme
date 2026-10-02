/* netbench.r -- resources for NetBench. Memory covers two 1 MB test buffers + Open Transport. */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    4096 * 1024, 3072 * 1024
};

resource 'vers' (1) {
    0x01, 0x00, release, 0x00, verUS,
    "1.0",
    "NetBench 1.0 -- TCP throughput benchmark (ping, 3 x 1 MB up, 3 x 1 MB down, every byte verified)"
};
