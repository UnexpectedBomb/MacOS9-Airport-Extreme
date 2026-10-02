#!/bin/bash
# package-probe.sh -- re-encode the Retro68 build into a MacBinary-II with a CORRECT CRC, plus a
# BinHex that is pure 7-bit ASCII.
#
# ★ WHY THIS EXISTS, and it is not hypothetical.
# On 2026-09-20 the MDD dropped into MacsBug on a k43 run. The stack crawl named the culprit:
#
#     Address 5DBA2BFC is in the "Stuffit Expander" heap at 5DB08BB0
#     ... LThread::Yield ... CVersCheck::~CVersCheck ... AEProcessAppleEvent
#
# Not our probe at all -- StuffIt Expander, a PowerPlant application, executing zeros in the
# middle of its own class-name string table. StuffIt was invoked to decode AirPortRx.bin and died
# doing it.
#
# (An earlier version of this header said "the AirPort probe never ran; there is no log for that
#  attempt". That was wrong -- the log existed and simply had not been copied to the share yet.
#  The probe ran to completion. The crash diagnosis is unaffected; only the claim about the log
#  was false.)
#
# ⚠ AND THE PROJECT ALREADY KNEW. cpu-temp/csm/scripts/package-dist.sh says in its header that
#   Retro68 writes a MacBinary CRC that strict decoders reject, "so the raw build .bin often
#   can't be un-binned on the target Mac", and it has produced a clean .bin plus a .hqx since.
#   The AirPort probe shipped the raw build output for forty-three increments and got away with
#   it until StuffIt crashed instead of merely refusing. Same fix, applied here.
#
# Usage:  ./package-probe.sh [build/AirPortRx.bin] [dist]
set -eu
SRC="${1:-build/AirPortRx.bin}"
OUT="${2:-dist}"
NAME="AirPortRx"
TYPE="APPL"; CREATOR="AiRx"

[ -f "$SRC" ] || { echo "package-probe: no $SRC -- build first" >&2; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# 1. Split the build's MacBinary back into a real forked file. Both forks matter: Retro68 puts
#    the PEF in the data fork and everything else -- including the firmware blobs -- in the
#    resource fork, and an earlier version of the cpu-temp script lost the PEF by assuming the
#    data fork was empty.
python3 - "$SRC" "$TMP/$NAME" <<'PY'
import sys, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
dlen = struct.unpack('>I', d[83:87])[0]
rlen = struct.unpack('>I', d[87:91])[0]
dstart = 128
rstart = 128 + ((dlen + 127) // 128) * 128        # forks are padded to 128 bytes
open(dst, 'wb').write(d[dstart:dstart+dlen])
open(dst + '/..namedfork/rsrc', 'wb').write(d[rstart:rstart+rlen])
print("  data fork %d bytes, resource fork %d bytes" % (dlen, rlen))
PY

/usr/bin/SetFile -t "$TYPE" -c "$CREATOR" "$TMP/$NAME"

mkdir -p "$OUT"
/usr/bin/macbinary encode -t 2 "$TMP/$NAME" -o "$OUT/$NAME.bin" -n
/usr/bin/macbinary probe   "$OUT/$NAME.bin"
/usr/bin/binhex   encode      "$TMP/$NAME" -o "$OUT/$NAME.hqx" -n
/usr/bin/binhex   probe    "$OUT/$NAME.hqx"
echo "package-probe: $OUT/$NAME.bin (MacBinary II, correct CRC) and $OUT/$NAME.hqx (BinHex)"

# ── Stage 8-5b: the shim driver, same treatment ────────────────────────────────────────────────
#
# Rez's own `--cc` MacBinary verifies its CRC but writes reader/writer versions of 0/0, and
# "verifies but looks odd" is precisely the shape of the k43 StuffIt crash. The driver has to
# survive the same decoder the probe does, so it gets re-encoded the same way rather than trusted
# because it happens to pass `macbinary probe` here.
#
# ⚠ THE SHIM IS A SHARED LIBRARY AND ITS RESOURCE FORK IS LOAD-BEARING. The 'cfrg' lives there and
#   is the only thing that lets CFM resolve the fragment by name; a repack that dropped it would
#   produce a file that looks right and can never be found. Both forks are checked below.
SHIM_SRC="${3:-build/AirPortShim.shlb.bin}"
if [ -f "$SHIM_SRC" ]; then
  # ⚠ THE FILE IS NAMED FOR WHAT IT IS, AND THAT IS NOT THE FRAGMENT NAME.
  #   The Extensions file is "AirPort Extreme Driver" -- the user's name, and an accurate one:
  #   this stopped being a shim at 8-2d-3 and is now the whole b43 bring-up plus a receiver.
  #   CFM does NOT resolve by filename. It resolves by the name in the 'cfrg' resource, which is
  #   still "AirPortShim", and the application's GetSharedLibrary must keep matching THAT.
  #   Renaming the fragment and the AirPortShim* exports is a separate, purely mechanical change
  #   and is deliberately not bundled with functional work -- a packaging failure and a driver
  #   failure look identical in the log, and one of them is much cheaper to avoid.
  SHIM_NAME="AirPort Extreme Driver"
  TMP2="$(mktemp -d)"; trap 'rm -rf "$TMP" "$TMP2"' EXIT
  python3 - "$SHIM_SRC" "$TMP2/$SHIM_NAME" <<'PY'
import sys, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
dlen = struct.unpack('>I', d[83:87])[0]
rlen = struct.unpack('>I', d[87:91])[0]
rstart = 128 + ((dlen + 127) // 128) * 128
open(dst, 'wb').write(d[128:128+dlen])
open(dst + '/..namedfork/rsrc', 'wb').write(d[rstart:rstart+rlen])
if rlen == 0:
    sys.exit("package-probe: the shim has NO resource fork -- the 'cfrg' is missing and CFM "
             "will never find the fragment. Refusing to ship it.")
print("  shim: data fork %d bytes (PEF), resource fork %d bytes ('cfrg')" % (dlen, rlen))
PY
  # ⛔ GATE: are allocation and VM calls confined to the one selector Apple documents as task
  # time? 8-2f introduced this and it refused the very first build -- EnetHAL_Close was freeing
  # DMA memory, and only Open is documented task time. Allocating below task level on OS 9 does
  # not return an error; it corrupts or hangs, with nothing in the log to say why.
  if ! python3 "$(dirname "$0")/check-exec-level.py"; then
      echo "package-probe: ⛔ REFUSING TO PACKAGE -- execution-level violation." >&2
      exit 1
  fi

  # ⛔ GATE: does the driver run chip_init's prefix in ApBringUp's order?
  #
  # 8-2d-3 found six prefix steps missing from the driver -- SsbCoreEnable's flags,
  # PhyTakeOutOfReset, both switch_analog calls, GpioInit, RadioOn and the GEN_IRQ_REASON clear --
  # which had been absent for three increments while 8-2b, 8-2c and 8-2d-2 all passed on top of
  # the gap. No oracle saw it because none was looking at the prefix as a SEQUENCE.
  if ! python3 "$(dirname "$0")/check-prefix-order.py"; then
      echo "package-probe: ⛔ REFUSING TO PACKAGE -- prefix order differs from ApBringUp." >&2
      exit 1
  fi

  # ⛔ GATE: refuse to package a fragment whose exports CFM cannot find.
  #
  # MakePEF places a symbol in `key % sz` while CFM looks in `(word ^ (word >> power)) & mask`.
  # Those agree only while the table has ONE bucket, which MakePEF uses up to nine exports. The
  # tenth export silently strands every symbol whose hash has bit 1 set -- and the symbol that
  # breaks is not the one that was added. k66 lost AirPortShimGetFw and AirPortShimStats this
  # way and burned a hardware cycle reading it as a missing .exp entry.
  #
  # ⚠ This is a HARD gate rather than a warning, because the failure it catches is invisible
  #   downstream: the fragment loads, CFM reports the symbols as absent, and the log is
  #   indistinguishable from a build where they were never exported at all.
  SHIM_PEF="$(dirname "$SHIM_SRC")/AirPortShim.pef"
  if [ ! -f "$SHIM_PEF" ]; then
      echo "package-probe: ⛔ $SHIM_PEF is missing -- cannot verify the export table." >&2
      exit 1
  fi
  if ! python3 "$(dirname "$0")/check-pef-exports.py" "$SHIM_PEF"; then
      echo "package-probe: ⛔ REFUSING TO PACKAGE -- see the export report above." >&2
      exit 1
  fi
  # ⚠⚠ TYPE IS 'shlb', AND k100 PROVED IT HAS TO BE.
  #
  # 8-12 set this to 'ndrv' on the strength of Apple's BNDL (FREF 128 'shlb', FREF 130 'ndrv').
  # The run came back 103 lines long with:
  #
  #     GetSharedLibrary("AirPortShim") -> 4294964492      (-2804, cfragNoLibraryErr)
  #
  # CFM scans Extensions at boot and registers fragments from SHARED LIBRARY files. Typed 'ndrv',
  # our file is not scanned, the fragment is never registered by name, and nothing downstream can
  # find it. The file type is therefore NOT the thing that makes the Device Manager look at a
  # fragment -- and it cannot be, because Apple's single file has to serve both roles at once:
  # WirelessAPI is an import library others connect to BY NAME, and OTModl$radio is the driver.
  #
  # What distinguishes them in Apple's file is the 'cfrg': OTModl$radio is declared twice, once
  # with architecture 'pwpc' and once with architecture 'ndrv'. That is where 8-13 looks next.
  /usr/bin/SetFile -t "shlb" -c "????" "$TMP2/$SHIM_NAME"
  # ⚠ BOTH HALVES OR NEITHER. ap_shim_icons.r puts the icon family in the resource fork at
  # ID -16455; this sets kHasCustomIcon, the Finder attribute that tells the Finder to look for
  # it. Resources without the flag show the generic icon and look like the icons never landed;
  # the flag without resources shows a blank. Neither failure names itself, so they are set
  # together, here, next to the SetFile that types the file.
  /usr/bin/SetFile -a C "$TMP2/$SHIM_NAME"
  /usr/bin/macbinary encode -t 2 "$TMP2/$SHIM_NAME" -o "$OUT/AirPortShim.bin" -n
  /usr/bin/macbinary probe   "$OUT/AirPortShim.bin"
  /usr/bin/binhex   encode      "$TMP2/$SHIM_NAME" -o "$OUT/AirPortShim.hqx" -n
  /usr/bin/binhex   probe    "$OUT/AirPortShim.hqx"
  echo "package-probe: $OUT/AirPortShim.bin decodes to \"$SHIM_NAME\"  ⚠ EXTENSIONS FOLDER"
else
  echo "package-probe: no $SHIM_SRC -- shim not packaged (build AirPortShimLib first)"
fi

# ★ READ BACK WHAT ACTUALLY SHIPPED. Everything above states an intention; this decodes the
# MacBinary and checks it. It exists because a stale generated cfrg packaged k101's shape into
# k102 with no sign of it anywhere in the build output, and because the shim binary carries the
# Wi-Fi passphrase and must never leave this machine.
python3 "$(dirname "$0")/check-artifacts.py" "$OUT" AirPortShim pci80211 'OTKrnl$AirPortPortScanner' AirPortRx || {
  echo "package-probe: ⛔ ARTIFACT CHECK FAILED -- do not stage these files"; exit 1; }
