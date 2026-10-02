#!/bin/sh
# build-init.sh -- the AirPort boot vehicle: a 68K 'INIT' code resource plus the PowerPC CFM
# fragment it loads, packed by Rez into one extension file.
#
# ★ Two toolchains, and that is the whole reason this is a script rather than a CMake target:
#   the PowerPC fragment comes from the same compiler as the driver, the INIT must come from the
#   m68k one ([[reference_retro68_no_ppc_init]] -- Retro68 cannot build a PowerPC INIT, but 68K
#   code resources are fine), and Rez then staples them together. usb2-ehci/resident/
#   build-appless-init.sh does exactly this and is the model.
#
# ⛔ RECOVERY if a bad INIT stops the machine booting: hold SHIFT at startup to disable
#   extensions, then remove "AirPort Extreme Startup" from the Extensions folder.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
RB="${RETRO68:-$HOME/Retro68-build}"
PPCBIN="$RB/toolchain/bin"
M68KGCC="$RB/toolchain-m68k/bin/m68k-apple-macos-gcc"
PPCGCC="$PPCBIN/powerpc-apple-macos-gcc"
MAKEPEF="$PPCBIN/MakePEF"
REZ="$PPCBIN/Rez"
# ⚠ TWO Rez include paths, and both are needed. Retro68.r (which defines RETRO68_CODE_TYPE for
#   the 'INIT' code resource) lives only in the m68k target's own RIncludes; the standard types
#   -- Types.r, which is what defines 'vers' -- live one level up. Rez itself is the same binary
#   for both architectures.
RINC="$RB/toolchain-m68k/m68k-apple-macos/RIncludes"
RINC2="$RB/toolchain-m68k/RIncludes"
BD="$HERE/build-init"
OUT="$HERE/dist"

rm -rf "$BD"; mkdir -p "$BD" "$OUT"

echo "=== 1. AirPortBoot.pef  (PowerPC CFM fragment: InstallMe) ==="
# ⚠ NameRegistryLib for the beacon and InterfaceLib for Gestalt / Notification Manager / the
#   File Manager. The driver target links the same two (plus PCILib and DriverServicesLib, which
#   this fragment deliberately does not get: it must not be able to touch the card).
"$PPCGCC" -O2 -Wno-multichar -shared \
    -Wl,-bE:"$HERE/ap_boot.exp" \
    "$HERE/ap_boot.c" -o "$BD/libAirPortBoot.so" \
    -lNameRegistryLib -lInterfaceLib
"$MAKEPEF" "$BD/libAirPortBoot.so" -o "$BD/AirPortBoot.pef"
ls -l "$BD/AirPortBoot.pef"

# ⚠ VERIFY THE EXPORT IS FINDABLE, not merely present. MakePEF's export-hash bug strands symbols
#   silently once a fragment has ten or more exports -- this one has one, so it cannot bite today,
#   but the check costs nothing and the failure mode reads exactly like a missing .exp entry.
#   [[reference_retro68_makepef_export_hash_bug]]
python3 "$HERE/check-pef-exports.py" "$BD/AirPortBoot.pef"

echo "=== 2. ap_init.flt  (68K INIT code resource) ==="
"$M68KGCC" -Wno-multichar -O2 -Wl,--mac-flat "$HERE/ap_init.c" -o "$BD/ap_init.flt"
ls -l "$BD/ap_init.flt"

echo "=== 3. Rez -> AirPortStartup ==="
cp "$HERE/ap_init.r" "$BD/ap_init.r"
( cd "$BD" && "$REZ" -I "$RINC" -I "$RINC2" ap_init.r \
      -o AirPortStartup --cc AirPortStartup.bin -t INIT -c RSED )

# ⚠ The container name on the share and the name the file DECODES to are different things, and
# both matter. MacBinary carries the internal name separately, so re-encode from a copy that is
# NAMED the way it must appear in the Extensions folder -- the same trick package-probe.sh uses
# to make AirPortShim.bin unpack as "AirPort Extreme Driver".
NAME="AirPort Extreme Startup"
rm -rf "$BD/pack"; mkdir -p "$BD/pack"
cp "$BD/AirPortStartup" "$BD/pack/$NAME"
/usr/bin/SetFile -t INIT -c RSED "$BD/pack/$NAME"
/usr/bin/macbinary encode -t 2 "$BD/pack/$NAME" -o "$OUT/AirPortStartup.bin" -n
/usr/bin/macbinary probe "$OUT/AirPortStartup.bin"
/usr/bin/binhex   encode    "$BD/pack/$NAME" -o "$OUT/AirPortStartup.hqx" -n
echo "build-init: $OUT/AirPortStartup.bin decodes to \"$NAME\"  ⚠ EXTENSIONS FOLDER, beside the driver"
