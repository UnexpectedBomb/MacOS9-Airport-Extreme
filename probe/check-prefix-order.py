#!/usr/bin/env python3
"""check-prefix-order.py -- does the driver run chip_init's prefix in the SAME ORDER as ApBringUp?

★ WHY. 8-2d-2a removed the driver's transcribed COPIES of ap_bringup.h's code, on the principle
that "a hand-copied sequence drifts, a called function cannot". That fixed the bodies. It did not
fix the ORDER: the driver still decides for itself when to call PhyTakeOutOfReset, GpioInit,
SwitchAnalog and RadioOn, and an order is exactly as copyable -- and as driftable -- as a body.

⚠ THIS IS NOT HYPOTHETICAL. 8-2d-3 found the driver had been calling SsbCoreEnable with flags of
  0 instead of GMODE|PHYCLKEN|PHYRESET, and skipping PhyTakeOutOfReset, SwitchAnalog, GpioInit,
  RadioOn and the GEN_IRQ_REASON clear entirely -- six steps of chip_init's prefix, missing for
  three increments, while 8-2b/8-2c/8-2d-2 all passed on top of the gap. The oracles could not see
  it because none of them was looking at the prefix as a SEQUENCE.

So the sequence gets its own check, on the host, every build.

⚠ WHAT IT DELIBERATELY DOES NOT CHECK: the firmware upload. The app reads blobs with LoadFw
  (GetResource) and the driver has them compiled in, so that step legitimately differs in form.
  It is represented as a single UPLOAD_FIRMWARE token in both.

Usage:  check-prefix-order.py            exit 0 = the orders agree
"""
import re
import sys

# The steps that define chip_init's prefix. Order is what matters, not how many times.
STEPS = [
    "SsbFindAndMap", "SsbScanCores", "SsbSelectCore", "SsbCoreEnable",
    "PhyTakeOutOfReset", "RadioIdControl", "SwitchAnalog", "UPLOAD_FIRMWARE",
    "GpioInit", "ApplyIvs", "RadioOn", "ApPhyInitG",
]


def strip_noise(text):
    """Comments and string literals first -- a function name inside a comment is a phantom edge."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    return text


def body_of(src, name):
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j]
    return None


def sequence(src, entry, expand):
    """Ordered step list reached from `entry`, inlining the functions named in `expand`."""
    out = []

    def walk(fn):
        body = body_of(src, fn)
        if body is None:
            return
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', body):
            call = m.group(1)
            if call in expand:
                walk(call)
            elif call in STEPS:
                out.append(call)
            # the microcode upload is the one step that legitimately differs in form
            elif call == "ShmControl" and (not out or out[-1] != "UPLOAD_FIRMWARE"):
                out.append("UPLOAD_FIRMWARE")
    walk(entry)
    # collapse immediate repeats: the count is not the contract, the order is
    collapsed = []
    for s in out:
        if not collapsed or collapsed[-1] != s:
            collapsed.append(s)
    # ⚠ SsbSelectCore is WINDOW MANAGEMENT, not a prefix step, and the two sides legitimately
    #   differ. ApBringUp selects the 802.11 core once and keeps the BAR0 window there for the
    #   rest of the run. The driver cannot: the application owns the same window, so every
    #   driver function that touches core registers saves the window, selects, works, and
    #   restores. Counting those re-selects as sequence differences would make this check cry
    #   wolf on the one thing the driver is doing MORE carefully than the app.
    #   Only the FIRST select is kept, which is the one that is genuinely part of the order.
    seen_select = False
    pruned = []
    for s in collapsed:
        if s == "SsbSelectCore":
            if seen_select:
                continue
            seen_select = True
        pruned.append(s)
    return pruned


def main():
    brg = strip_noise(open("ap_bringup.h", encoding='utf-8').read())
    shim = strip_noise(open("ap_shim.c", encoding='utf-8').read())

    app = sequence(brg, "ApBringUp", expand=set())
    drv = sequence(shim, "EnetHAL_Entry", expand={
        "ApShimFindCard", "ApShimScanBackplane", "ApShimEnableCore", "ApShimLoadFirmware",
        "ApShimGpioInit", "ApShimApplyInitvals", "ApShimRadioOn", "ApShimPhyInit",
    })
    # ApBringUp stops before phy_init by design -- its header says so. The driver goes one further.
    if drv and drv[-1] == "ApPhyInitG":
        drv_cmp = drv[:-1]
        tail = "  (+ ApPhyInitG, which ApBringUp deliberately does not call -- see its header)"
    else:
        drv_cmp, tail = drv, ""

    print("  ApBringUp : " + " -> ".join(app))
    print("  driver    : " + " -> ".join(drv_cmp))
    if tail:
        print(tail)
    print()

    if drv_cmp == app:
        print("  [ok] the driver runs chip_init's prefix in ApBringUp's order.")
        return 0

    print("  ⛔ ORDER MISMATCH.")
    import difflib
    for line in difflib.unified_diff(app, drv_cmp, "ApBringUp", "driver", lineterm="", n=1):
        print("     " + line)
    print()
    print("  ⚠ A missing or reordered step here does NOT announce itself on hardware. 8-2b, 8-2c")
    print("    and 8-2d-2 all passed while six prefix steps were absent, because no oracle was")
    print("    looking at the prefix as a sequence. Fix the order, or if the difference is")
    print("    deliberate, say so in ap_shim.c and teach this script about it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
