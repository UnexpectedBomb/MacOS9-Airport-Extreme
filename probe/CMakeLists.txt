cmake_minimum_required(VERSION 3.13)
project(AirPortProbe C)

# Stage 1 probe for the AirPort Extreme (BCM4306) project, as a double-clickable PPC app.
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# ⚠ THIS TARGET DELIBERATELY LINKS NO DRIVER SOURCES. There are none yet -- but the constraint is
# the point, not an accident of timing: Stage 1 answers "does OS 9 enumerate the card" and nothing
# else. Any code that could touch a BAR belongs in Stage 2, behind its own boot.
#
# LIBRARY CHOICES, and why each is here:
#   NameRegistryLib  the device-tree walk and every property read -- the whole probe.
#   PCILib           ExpMgrConfigRead{Byte,Word,Long} only. Config space is safe against an
#                    uninitialised card in a way MMIO is not, which is why Stage 1 reads it and
#                    Stage 2 reads BAR0.
#   InterfaceLib     NewPtr, FindFolder, the File Manager for the log, and QuickDraw for the window.
#
# ⚠ DriverServicesLib and DriverLoaderLib are deliberately ABSENT. An app has no business holding
# driver-loader imports (the same rule SiI3512FlashProbe follows), and their absence is a structural
# guarantee that this binary cannot install anything.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# ★★★ CORE RESET CYCLE -- the probe that removes the cold-boot requirement.
#
# Tests ssb_core.h's SsbCoreDisable(), a faithful port of ssb_device_disable(): the reject
# handshake, the backplane-revision-dependent reject bitmask, and the initiator dance. Every probe
# since Stage 3a has needed a fresh boot because none of them implemented that path. Five boots went
# on Stage 4b-i alone, and 4b-ii will iterate harder.
#
# ⚠ Needs NO firmware, so it builds even on a checkout that has never fetched the blobs -- which is
# why it sits outside the firmware_blobs.r guard below.
add_application(AirPortReset
    airport_reset.c
    airport_reset.r
    TYPE "APPL" CREATOR "AErs")

target_link_libraries(AirPortReset NameRegistryLib PCILib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(AirPortReset retrocrt)
endif()

# ★★★ STAGE 4b-ii-1 -- prove the accessor layer before porting anything that uses it.
#
# 4b-ii is not one probe. Measured against source, the G-PHY init sequence is
# phy_g.c 3057 + wa.c 376 + lo.c 1003 + tables.c 453 = 4889 lines, with no clean slice: initb6
# calls channel_switch and set_txpower_g, and the 18-line inita calls b43_wa_all(), a whole file.
# So the first increment is the four primitives all of it rests on -- PHY write, radio read, radio
# write, host flags -- three of which have never executed on this hardware.
#
# A wrong accessor does NOT fault. It silently addresses the wrong register and surfaces thousands
# of lines later.
#
# ⚠ g1 ran with NO firmware and NO initvals and crashed on the first general radio read. b43 runs
# b43_phy_initg AFTER upload_microcode and upload_initvals, so initb6 reads radio 0x7A in that
# state. g2 restores the precondition, which is why this target moved inside the firmware guard
# below. Still needs no fresh boot.
add_application(AirPortProbe
    airport_probe.c
    airport_probe.r         # SIZE partition + vers
    TYPE "APPL" CREATOR "AEpr")

target_link_libraries(AirPortProbe NameRegistryLib PCILib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(AirPortProbe retrocrt)
endif()

# ★★★ STAGE 2 -- the first binary in this project that WRITES, and the first that does MMIO.
#
# Exactly two config-space registers are written: 0x04 (memory space enable, which Stage 1 proved
# Open Firmware leaves clear) and 0x80 (the SiliconBackplane core-select window). There is no MMIO
# write path in the source -- absent, not gated.
#
# Same library set as Stage 1 and for the same reasons. PCILib now supplies the Write entry points
# as well as the Read ones, which is the one deliberate escalation between the two targets; the
# import table is the place to verify that claim after a build.
add_application(AirPortStage2
    airport_stage2.c
    airport_stage2.r
    TYPE "APPL" CREATOR "AEs2")

target_link_libraries(AirPortStage2 NameRegistryLib PCILib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(AirPortStage2 retrocrt)
endif()

# ★★★ STAGE 3a -- the first binary in this project that writes MMIO REGISTERS.
#
# Writes TMSLOW / TMSHIGH / IMSTATE (the ssb_device_enable reset sequence, fetched verbatim from
# Linux mainline while writing it rather than recalled), MACCTL (three bits), and SHM shared memory.
# It writes NO PHY, radio, DMA or firmware register.
#
# Requires a fresh boot. It skips ssb_device_disable()'s teardown path, which is only legitimate
# when RESET is already asserted -- so it checks for that and refuses to proceed otherwise, rather
# than implementing the backplane reject handshake from memory.
add_application(AirPortStage3a
    airport_stage3a.c
    airport_stage3a.r
    TYPE "APPL" CREATOR "AE3a")

target_link_libraries(AirPortStage3a NameRegistryLib PCILib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(AirPortStage3a retrocrt)
endif()

# ★★★ STAGE 3b -- microcode upload. The first time this hardware executes firmware under OS 9.
#
# ⚠ REQUIRES THE FIRMWARE BLOBS, which are NOT in this repository (proprietary Broadcom microcode;
# see ../firmware/README.md for the fetch and hash). Generate the embedded resource first:
#
#     ./embed-fw.sh ../firmware/v3 firmware_blobs.r
#
# firmware_blobs.r is gitignored for the same reason the blobs are. If it is absent this target is
# skipped rather than failing the whole build, so Stages 1-3a still build on a checkout that has
# never fetched firmware.
#
# Embedding firmware in the app's resource fork is not a workaround -- it is what Apple's own OS 9
# AirPort Driver does, carrying Lucent Hermes firmware as 'Fdat' resources and loading them through
# the Resource Manager at task level.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/firmware_blobs.r)
    add_application(AirPortStage3b
        airport_stage3b.c
        airport_stage3b.r
        firmware_blobs.r        # ucode5 'BCMu' 128, pcm5 'BCMp' 128, initvals 'BCMi' 128/129
        TYPE "APPL" CREATOR "AE3b")

    target_link_libraries(AirPortStage3b NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortStage3b retrocrt)
    endif()
    # ★★★ STAGE 4a -- PHY/radio identity, then the initvals tables.
    #
    # First binary that writes registers whose ADDRESSES come from a data file rather than from
    # source we have read, so every IV offset is range-checked against 0x1000 before the write.
    # Resets the core twice, exactly as b43 does: once with gmode=false to read the PHY versioning,
    # then again with GMODE once the PHY type is known. GPIO init is deferred to 4b.
    add_application(AirPortStage4a
        airport_stage4a.c
        airport_stage4a.r
        firmware_blobs.r        # ucode5 + pcm5 + BOTH initvals tables
        TYPE "APPL" CREATOR "AE4a")

    target_link_libraries(AirPortStage4a NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortStage4a retrocrt)
    endif()
    # ★★★ STAGE 4b-ii-1 -- prove the accessor layer before porting the 4889-line PHY sequence.
    #
    # ⚠ g1 (no firmware) and g2 (firmware + initvals) BOTH faulted at the identical instruction:
    # R4 = 0x3FA (RADIO_DATA_LOW), R5 = 0x7A. g2 therefore falsified its own hypothesis rather
    # than confirming it -- "firmware + initvals" was not the missing precondition. g3 stops
    # guessing one step at a time and ports b43_chip_init's prefix in full; measured against that
    # list, g2 was missing THREE steps, not one: b43_gpio_init (step 3), upload_initvals_band
    # (step 5), and b43_software_rfkill(dev,false) (inside step 7) -- the radio power-on itself.
    #
    # Every write in the power-on path is a PHY write or a CHANNEL/CHANNEL_EXT write; not one is a
    # radio-register access. So it runs entirely over ports g1 already proved work with no
    # firmware at all, which is why adding it is not additional risk.
    add_application(AirPortAccessors
        airport_accessors.c
        airport_accessors.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "AEac")

    target_link_libraries(AirPortAccessors NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortAccessors retrocrt)
    endif()

    # ★★★ STAGE 4b-ii-2 -- b43_phy_initb6, ported as a faithful PREFIX.
    #
    # ⚠ THE ROADMAP LISTED 4b-ii-2 AS "wa_all + inita". That was wrong: b43_phy_initg runs
    # b43_phy_initb6 FIRST and b43_phy_inita after it, so porting inita now would run the
    # sequence out of dependency order -- the same class of error that cost 4b-ii-1 nine runs.
    #
    # Runs initb6's statements IN ORDER from the first to `b43_phy_write(dev, 0x0038, 0x0668)`
    # and stops, because the next statement is b43_set_txpower_g -> b43_lo_g_adjust, which is
    # lo.c's 1003 lines and all of 4b-ii-4. Stopping early is not the same as skipping: for every
    # statement this runs, everything before it has also run. The three statements AFTER
    # set_txpower_g are deliberately not run.
    #
    # ⚠ FIRST PROBE BUILT ON ap_bringup.h, which holds the chip_init prefix and the accessor
    # layer as ONE definition extracted verbatim from the h3 commit. Hand-copying that sequence
    # is what let g2 silently omit three steps; a called function cannot drift.
    add_application(AirPortInitB6
        airport_initb6.c
        airport_initb6.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "AIb6")

    target_link_libraries(AirPortInitB6 NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortInitB6 retrocrt)
    endif()

    # ★★★ STAGE 5-1 -- the addressing probe. First Stage 5 binary.
    #
    # Stage 5 is OS 9 integration rather than register translation, and it has four independent
    # failure classes: addressing, cache coherency, ring programming and interrupts. Running them
    # together would make any failure ambiguous, so this one isolates addressing. It allocates no
    # ring, programs no descriptor, enables no engine and installs NO INTERRUPT HANDLER -- polled
    # throughout, so the flushed-per-line log that carried all of Stage 4 keeps working.
    #
    # ★ NO NEW LIBRARY, AND THAT IS A MEASURED CLAIM, NOT A PREFERENCE. The probe needs
    # GetPhysical and LockMemory. Those look like DriverServicesLib calls and are used that way in
    # our own ehci_os.c, but they are declared in MacMemory.h and exported by InterfaceLib 7.1+ --
    # verified with `nm libInterfaceLib.a`, which lists .GetPhysical, .LockMemory, .HoldMemory and
    # .UnlockMemory as text symbols. So the link line below is IDENTICAL to every probe before it.
    #
    # ⚠ DriverServicesLib and DriverLoaderLib therefore remain ABSENT through Stage 5-1, and the
    # structural guarantee this file has carried since Stage 1 -- that no probe binary can install
    # a driver -- survives. When interrupts arrive in a later increment that will have to be
    # revisited explicitly, as its own decision, not as a side effect of adding a source file.
    add_application(AirPortDma
        airport_dma.c
        airport_dma.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "AEdm")

    target_link_libraries(AirPortDma NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortDma retrocrt)
    endif()

    # ★★★ STAGE 5-2 -- the DMA rings, programmed. TX and RX in ONE increment.
    #
    # Not a scope decision -- a structural one. dmacontroller_setup (b43_dma.c:672) has a TX arm
    # and an RX arm, and alloc_initial_descbuffers is called from INSIDE the RX arm. Splitting
    # them across two probes would put one function's statements in two places, which is the
    # drift ap_bringup.h exists to prevent. "Program RX but skip its descriptors" is not a subset
    # of the reference, it is a deviation from it.
    #
    # ⚠⚠ FIRST PROBE THAT WRITES LITTLE-ENDIAN DATA TO ORDINARY MEMORY. Every MMIO access since
    # Stage 2 has gone through ssb_core.h's lwbrx/stwbrx accessors, so byte order has been
    # handled and invisible. Descriptors are different: they live in RAM, and a plain PowerPC
    # store writes big-endian. op32_fill_descriptor ends in cpu_to_le32 for both words, so this
    # probe routes every descriptor word through the same stwbrx aimed at RAM, and Oracle B
    # reads the bytes back individually to prove it.
    #
    # ⚠ Same link line again -- GetPhysical and LockMemory are InterfaceLib. DriverServicesLib
    # and DriverLoaderLib still absent.
    add_application(AirPortDma2
        airport_dma2.c
        airport_dma2.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "AEd2")

    target_link_libraries(AirPortDma2 NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortDma2 retrocrt)
    endif()

    # ★★★ STAGE 5-3 -- enable the MAC and catch a frame off the air.
    #
    # The first increment where the card must act on its OWN and we read the result, rather than
    # verifying that it accepted something we wrote. One beacon settles four things at once: the
    # card reads our descriptor ring, writes our buffers, PowerMac PCI DMA is cache-coherent, and
    # Stage 4's PHY and radio genuinely receive.
    #
    # ★ THE ORACLE WAS PLANTED IN k2. b43_poison_rx_buffer writes 0xFF over the eight bytes at the
    # frame offset and b43_rx_buffer_is_poisoned ANDs them back together, so the reference supplies
    # its own "has the card written here" detector and this probe does not invent one.
    #
    # ⚠ THE CHANNEL SWEEP IS NOT A CONVENIENCE. ApBringUp tunes to channel 1, and if the AP is on
    # 6 or 11 the result would be an empty log that looks exactly like a broken receiver. Channels
    # 1, 6 and 11 each get a clean 32-slot budget -- ring reset, buffers re-poisoned, descriptors
    # rewritten -- so a busy channel cannot starve the next one and make it look empty.
    #
    # ⚠ Runs b43_wireless_core_init's five REMAINING statements in order (qos_init,
    # set_synth_pu_delay, bluetooth_coext_enable, upload_card_macaddress, security_init) rather
    # than guessing which are preconditions for reception. g2 made that guess and lost three
    # chip_init steps. b43_mac_suspend/_enable come with it, including the refcount that starts
    # at 1 -- get that wrong and the receiver stays off while every readback looks fine.
    add_application(AirPortRx
        airport_rx.c
        airport_rx.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "AErx")

    # ⚠ OpenTransportLib is here for Stage 8-5d ONLY: InitOpenTransport + OTGetIndexedPort, to ask
    # OT's own registry which ports exist. It is a CLIENT-side import in an application, which is
    # its supported use -- this is not the driver, and the driver (AirPortShim) deliberately links
    # nothing but InterfaceLib.
    # ⚠ BOTH OT libraries, and they are not interchangeable: OpenTransportLib carries the port
    # registry calls (OTGetIndexedPort) while InitOpenTransport/CloseOpenTransport live only in
    # the application-side OpenTransportAppPPC. Linking one and not the other fails at link time
    # with a bare "undefined reference", which is a cheap failure -- but worth naming here so the
    # next person does not go looking for the wrong library.
    # ⚠ ORDER MATTERS AND IT IS NOT ALPHABETICAL. OpenTransportAppPPC is static glue that expands
    # the OTOpenEndpoint / OTCloseProvider macros into their *Priv entry points; OpenTransportLib
    # is the import library that actually exports those. The glue must come FIRST so its
    # references are still unresolved when the importer is scanned. Reversed, the link fails with
    # a bare "undefined reference to .OTOpenEndpointPriv" while both libraries demonstrably
    # contain the symbol -- which reads like a missing library and is not one.
    target_link_libraries(AirPortRx
        NameRegistryLib PCILib OpenTransportAppPPC OpenTransportLib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortRx retrocrt)
    endif()

    # ★★★ STAGE 4b-i -- GPIO init, and proving the PHY register access path.
    #
    # Deliberately does NOT port the G-PHY init sequence: phy_g.c is ~3000 lines and porting it
    # before the access path is proven would be building on sand -- the same reason 3a proved SHM
    # before 3b pushed microcode through it. That sequence is 4b-ii.
    #
    # First probe that switches the backplane window mid-run (to ChipCommon for GPIO_CONTROL at
    # +0x6C) and switches back, which is the core switch deferred from 4a.
    add_application(AirPortStage4bi
        airport_stage4bi.c
        airport_stage4bi.r
        firmware_blobs.r
        TYPE "APPL" CREATOR "A4bi")

    target_link_libraries(AirPortStage4bi NameRegistryLib PCILib InterfaceLib)

    if(TARGET retrocrt)
        add_dependencies(AirPortStage4bi retrocrt)
    endif()
else()
    message(STATUS "AirPortStage3b/4a/4bi SKIPPED: firmware_blobs.r absent. Run ./embed-fw.sh first.")
endif()

# ============================================================================
# ★★★ STAGE 8-5b -- THE AIRPORT ETHERNET SHIM DRIVER (a CFM SHARED LIBRARY).
#
# k52 proved EnetShimLib is present and exports all three of its entry points, which settled the
# Stage 8 architecture: no hand-written STREAMS/DLPI driver. We export ONE function,
# EnetHAL_Entry, and Apple's shim provides DLPI on top of it.
#
# ⚠ A SHARED LIBRARY ('shlb'), NOT AN ndrv. Apple's sample comments the registration argument
#   "CFrag Connection ID (mine)": the shim takes a connection to the DRIVER'S OWN fragment and
#   FindSymbols EnetHAL_Entry inside it, so CFM must find us by fragment name -- which is what the
#   'cfrg' in ap_shim.r declares. Consequently there is NO patch-pef-main.py step here. `main`
#   matters for VerifyFragmentAsDriver on a Device Manager ndrv and we are not one; esata-sil3512
#   needs that step and copying it over would be cargo.
#
# ⚠⚠ THE "InterfaceLib ONLY" GUARANTEE IS GONE AS OF 8-5g, DELIBERATELY, AND THIS COMMENT SAYS SO
#   RATHER THAN BEING QUIETLY LEFT IN PLACE. Through k58 this target linked InterfaceLib and
#   nothing else, and the absent imports were a structural guarantee that the driver could not
#   touch hardware. k58 earned the right to remove it: OT drove the driver sixteen times and
#   stopped at GetMACAddress, which cannot be answered without reading the card's SPROM.
#
#   NameRegistryLib + PCILib are the SMALLEST set that lets the driver find its own node, take
#   BAR0 and read six bytes. ⚠ DriverServicesLib is still absent, and should stay absent until
#   there is an interrupt handler that needs it -- that is the next guarantee worth keeping.
#
#   This is the first step of the migration that has to happen: the probe application does not
#   exist in the shipped product, so the driver must own its hardware eventually.
add_library(AirPortShim SHARED ap_shim.c)

set_target_properties(AirPortShim PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/ap_shim.exp")

# ⚠ LINK_DEPENDS, because CMake does NOT infer it from LINK_FLAGS. Without this, editing
# ap_shim.exp changes nothing: make sees no dirty input, skips the relink, and the .pef keeps the
# PREVIOUS export list. That cost a confusing minute on 2026-09-21 -- the export checker reported
# ten exports immediately after the edit that removed one, which reads as the edit not working.
# Same family as the Rez #include problem: a real dependency the build system cannot see.
set_target_properties(AirPortShim PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ap_shim.exp")

# ⚠ 8-7 adds DriverServicesLib: GetInterruptFunctions, InstallInterruptFunctions and
# QueueSecondaryInterruptHandler all live there. The PCI interrupt ABI note is explicit that
# these are in Retro68's STOCK libDriverServicesLib.a -- no CodeWarrior, no custom stub.
target_link_libraries(AirPortShim NameRegistryLib PCILib DriverServicesLib InterfaceLib)

# ⚠ ssb_core.h is a whole backplane library and the shim uses one function from it, so
# SsbCoreEnable and SsbScanCores come through unused. Those two warnings are ACCURATE and in a
# sense reassuring -- they are precisely the core-reset and core-enumeration entry points the
# driver must NOT be calling while the application owns the card. But two permanent warnings are
# how a real one goes unnoticed, so they are silenced here, on this target only, with the reason
# recorded rather than the flag dropped in anonymously.
target_compile_options(AirPortShim PRIVATE -Wno-unused-function)

# ⚠⚠ 8-11: MakePEF GIVES YOU main XOR THE EXPORT, NEVER BOTH.
#
# A native driver must export DoDriverIO AND have the fragment's `main` point at it. Retro68
# builds a shared library with no main at all; adding -Wl,-e,DoDriverIO sets main but then
# MakePEF DROPS DoDriverIO from the export table, and VerifyFragmentAsDriver answers -2802.
# The eSATA work paid for this and the fix is to build normally -- the symbol stays exported --
# and post-patch the PEF loader header's mainSection/mainOffset to the export's own (sec,off).
#
# ⚠ This runs on every build even though the 'cfrg' does not yet declare an 'ndrv' member, so
#   nothing loads us as a driver. Patching now means the shape is verified by the same rule that
#   will matter later, rather than being switched on in the same increment as the exposure.
# 8-14: the two-member cfrg ("AirPortShim" + "pci80211", both pwpc, over one PEF).
#
# ⚠⚠ THE Rez TARGET BELOW MUST LIST ap_shim_cfrg.r IN ITS DEPENDS, AND THAT IS NOT COSMETIC.
#   ap_shim.r #includes it, but CMake does not parse Rez includes -- so with only ap_shim.r
#   listed, nothing ever consumes this OUTPUT, the generator never re-runs, and Rez never
#   re-reads the file. The FIRST build works (the output is missing, so CMake makes it) and
#   every edit to make-cfrg.py afterwards is silently ignored. That is exactly how it bit here:
#   the k102 packaging still carried k101's 'ndrv' member while the source said "pci80211",
#   and the run would have re-tested the previous build's shape under the new build's name.
#   Same trap as [[feedback_rez_include_deps]], which cost a cycle on PICTs.
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ap_shim_cfrg.r
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make-cfrg.py
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_shim_cfrg.r
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make-cfrg.py)

add_custom_command(OUTPUT AirPortShim.pef
    COMMAND ${MAKE_PEF} libAirPortShim.so -o AirPortShim.pef
    COMMAND ${CMAKE_COMMAND} -E echo "  patching PEF main -> DoDriverIO"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../../esata-sil3512/patch-pef-main.py
            AirPortShim.pef DoDriverIO
    DEPENDS AirPortShim)

if (REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# 'shlb' is the file type CFM looks for when resolving an import library by name.
add_custom_command(OUTPUT AirPortShim.shlb
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/ap_shim.r
            ${REZ_INCLUDE_FLAG}
            -o AirPortShim.shlb --cc AirPortShim.shlb.bin
            -t "shlb" -c "????"
            --data AirPortShim.pef
    DEPENDS AirPortShim.pef ${CMAKE_CURRENT_SOURCE_DIR}/ap_shim.r
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_shim_cfrg.r)

add_custom_target(AirPortShimLib ALL DEPENDS AirPortShim.shlb)

# ============================================================================================
# ★★★ 8-58: THE NATIVE OPEN TRANSPORT DLPI MODULE -- OTModl$AirPortBCM.
#
# WHY A SECOND FRAGMENT. The USBEnet-shim path above works up to a point and then hits a wall we
# cannot move: Apple's shim/DLPI-stub binary DROPS L2-BROADCAST frames on this port (proven k152 --
# OT pulls ~24 bcast/boot in EnetHAL_Read and its upper layers act on none), so ARP and DHCP never
# reach TCP/IP. Apple's own AirPort is seamless because it is a NATIVE DLPI STREAMS module, not a
# shim client. This target builds ours: ap_otmodl.c (the DLPI module) + ap_shim.c (the radio HAL),
# in ONE fragment so the module drives the radio through in-fragment calls (ApOtmHal* bridges) with
# NO cross-fragment two-copy-globals problem ([[reference_os9_two_fragment_copies_own_globals]]).
#
# ⚠ ONE fragment, THREE exports only (ap_otmodl.exp: GetOTInstallInfo / InitStreamModule /
#   TerminateStreamModule). ap_shim.c's own exports (EnetHAL_Entry, DoDriverIO, ...) are compiled
#   in but NOT exported here -- the -bE export list restricts the table, so the same HAL source
#   serves both fragments without either leaking the other's contract.
#
# ⚠ NO patch-pef-main.py, unlike AirPortShim. OT enters a STREAMS module through GetOTInstallInfo
#   (GetSharedLibrary("OTModl$AirPortBCM")+FindSymbol), never through the fragment `main`/DoDriverIO
#   that VerifyFragmentAsDriver wants. Patching main here would be cargo.
add_library(AirPortOTModl SHARED ap_otmodl.c ap_shim.c)

set_target_properties(AirPortOTModl PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl.exp")
set_target_properties(AirPortOTModl PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl.exp")

target_link_libraries(AirPortOTModl NameRegistryLib PCILib DriverServicesLib InterfaceLib)
target_compile_options(AirPortOTModl PRIVATE -Wno-unused-function)

# Single-member cfrg naming the fragment "OTModl$AirPortBCM". Same DEPENDS caveat as the shim: the
# Rez #include of this file is invisible to CMake, so it must be listed on the Rez command below.
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl_cfrg.r
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make-cfrg.py
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl_cfrg.r "OTModl$AirPortBCM"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make-cfrg.py)

add_custom_command(OUTPUT AirPortOTModl.pef
    COMMAND ${MAKE_PEF} libAirPortOTModl.so -o AirPortOTModl.pef
    DEPENDS AirPortOTModl)

add_custom_command(OUTPUT AirPortOTModl.shlb
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl.r
            ${REZ_INCLUDE_FLAG}
            -o AirPortOTModl.shlb --cc AirPortOTModl.shlb.bin
            -t "shlb" -c "????"
            --data AirPortOTModl.pef
    DEPENDS AirPortOTModl.pef ${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl.r
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_otmodl_cfrg.r)

add_custom_target(AirPortOTModlLib ALL DEPENDS AirPortOTModl.shlb)

# ============================================================================================
# 8-82 k188: OTKrnl$AirPortScanner -- the OPEN TRANSPORT PORT SCANNER fragment (ap_scanner.c).
# OT discovers it by the 'otan' cfrg-member extension whose tag is "OTKrnl$pScnr" (disassembled
# from Apple's OTKrnl$USBOTPortScanner) and calls its exported OTScanPorts at boot. The scanner
# registers the OTModl$AirPortBCM port THROUGH OT's own scan, so the driver installs into the OT
# module registry and the port becomes ACTIVATABLE -- the thing hand-registration never achieved.
# Built as a standalone PEF here; make-cfrg.py + the combined-file rule staple all three fragments
# (OTModl$ + OTPortCfg$ + OTKrnl$) into one shlb, matching Apple's "Apple Enet DLPI Support".
# ============================================================================================
add_library(AirPortScanner SHARED ap_scanner.c)
set_target_properties(AirPortScanner PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/ap_scanner.exp")
set_target_properties(AirPortScanner PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ap_scanner.exp")
target_link_libraries(AirPortScanner NameRegistryLib InterfaceLib)
target_compile_options(AirPortScanner PRIVATE -Wno-unused-function)

add_custom_command(OUTPUT AirPortScanner.pef
    COMMAND ${MAKE_PEF} libAirPortScanner.so -o AirPortScanner.pef
    DEPENDS AirPortScanner)

add_custom_target(AirPortScannerPEF ALL DEPENDS AirPortScanner.pef)

# ============================================================================================
# 8-82 k188: OTPortCfg$AirPortCfgHelper -- the OT CONFIGURATION HELPER fragment (ap_portcfg.c).
# OT loads it (named by the scanner's fResourceInfo="AirPortCfgHelper") to get the port's
# user-visible name + icon for the TCP/IP control panel. Two exports per the kPortConfigLibPrefix
# contract: OTGetUserPortName + OTGetPortIcon. Pure, no imports, no hardware. Fragment 2 of 3.
# ============================================================================================
add_library(AirPortPortCfg SHARED ap_portcfg.c)
set_target_properties(AirPortPortCfg PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/ap_portcfg.exp")
set_target_properties(AirPortPortCfg PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ap_portcfg.exp")
target_link_libraries(AirPortPortCfg InterfaceLib)
target_compile_options(AirPortPortCfg PRIVATE -Wno-unused-function)

add_custom_command(OUTPUT AirPortPortCfg.pef
    COMMAND ${MAKE_PEF} libAirPortPortCfg.so -o AirPortPortCfg.pef
    DEPENDS AirPortPortCfg)

add_custom_target(AirPortPortCfgPEF ALL DEPENDS AirPortPortCfg.pef)

# ============================================================================================
# 8-82 k188: THE COMBINED DRIVER -- one shlb, all three fragments, Apple's exact shape.
# make-airport-driver.py concatenates the three PEFs (16-byte aligned, as Apple does) into
# AirPortDriver.data and emits the 3-member cfrg (ap_driver_cfrg.r) whose SCANNER member carries the
# 'otan'/"OTKrnl$pScnr" extension OT keys on and whose OTModl member carries the 'shlb' extension.
# Rez then staples that cfrg + the 'vers' onto the concatenated data fork. This one file REPLACES both
# the old OTModl-only extension AND the separate Startup INIT: OT discovers the scanner in this cfrg at
# boot and installs the driver itself, so no boot vehicle is needed. Cold boot; see ap_driver.r vers.
# ============================================================================================
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver_cfrg.r AirPortDriver.data
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make-airport-driver.py
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver_cfrg.r AirPortDriver.data
            AirPortOTModl.pef AirPortPortCfg.pef AirPortScanner.pef
    DEPENDS AirPortOTModl.pef AirPortPortCfg.pef AirPortScanner.pef
            ${CMAKE_CURRENT_SOURCE_DIR}/make-airport-driver.py)

# ★ k206: THE EXTENSION'S ICON. ap_driver_icon.r (the user's "Airport Extreme extension" family, generated
# by ../art/make-art.py) is a second Rez INPUT, so it is a real DEPENDS -- an #included .r is invisible to
# CMake (feedback_rez_include_deps). Rez cannot set Finder flags, so kHasCustomIcon goes on afterwards,
# on BOTH copies: the native file (SetFile; the .hqx is encoded from it) and the MacBinary (the strip
# module's set-custom-icon.py, which recomputes the MacBinary CRC and writes a NEW file, moved into place).
# The extension has no bundle, so no --bundle.
set(AP_SET_ICON ${CMAKE_CURRENT_SOURCE_DIR}/../csm/scripts/set-custom-icon.py)
add_custom_command(OUTPUT AirPortExtreme.shlb
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver.r
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver_icon.r
            ${REZ_INCLUDE_FLAG}
            -o AirPortExtreme.shlb --cc AirPortExtreme.shlb.bin
            -t "shlb" -c "cfrg"
            --data AirPortDriver.data
    COMMAND /usr/bin/SetFile -a C AirPortExtreme.shlb
    COMMAND /usr/bin/env python3 ${AP_SET_ICON} AirPortExtreme.shlb.bin AirPortExtreme.shlb.bin.tmp
            --name "AirPort Extreme Driver"
    COMMAND ${CMAKE_COMMAND} -E rename AirPortExtreme.shlb.bin.tmp AirPortExtreme.shlb.bin
    DEPENDS AirPortDriver.data ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver.r
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver_cfrg.r
            ${CMAKE_CURRENT_SOURCE_DIR}/ap_driver_icon.r ${AP_SET_ICON})

add_custom_target(AirPortDriver ALL DEPENDS AirPortExtreme.shlb)
