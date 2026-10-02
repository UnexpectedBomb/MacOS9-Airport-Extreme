cmake_minimum_required(VERSION 3.13)
project(NetBench C)
# NetBench -- TCP throughput benchmark for Mac OS 9 (QuickBench for the network). Double-click to run;
# the Pi end is netbench-server.py, started with scripts/pi-netbench.sh.
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build --target NetBench_APPL        (⚠ the bare NetBench target builds only the .xcoff)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(NetBench netbench.c netbench.r TYPE "APPL" CREATOR "NBch")

# Open Transport exactly as client/CMakeLists.txt links it (Apple Universal Interfaces 3.4):
# the OS 9 shared-library stubs plus Apple's static ASLM init objects.
target_link_libraries(NetBench
    OpenTransportLib        # OTOpenEndpoint, OTSnd/OTRcv, OTBind/OTConnect, OTLook...
    OpenTransportAppPPC     # static: InitOpenTransport / CloseOpenTransport (ASLM-wrapped)
    OpenTptInternetLib      # OTInitDNSAddress, OTInetGetInterfaceInfo
    OpenTptInetPPC          # static: ASLM init for inet
)
