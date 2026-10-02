#!/usr/bin/env python3
"""check-exec-level.py -- is any task-level-only primitive reachable from a NON-task-level selector?

★ WHY BY SELECTOR AND NOT GLOBALLY. Until 8-2f the driver's audit asked one question: "is a
forbidden primitive reachable from EnetHAL_Entry at all?" That was right while the answer was
supposed to be no for everything. 8-2f changes it: the driver now ALLOCATES, and allocation is
legal -- Apple's USBEnetSample readme documents EnetHAL_Open as task time and says allocation is
safe there. No other selector carries that promise.

So a global audit would have to either fail on a legal call or be switched off, and a switched-off
audit is worse than none. It asks per selector instead:

    Open                allocation and Name Registry work are FINE  (task time, documented)
    everything else     they are a DEFECT                           (may run below task level)

⚠ THE FAILURE THIS GUARDS IS SILENT AND FATAL. Allocating below task level on OS 9 does not
  return an error, it corrupts or hangs -- the same family as the File Manager in an ISR, which
  this project has hung machines with three times. Nothing in a log says "you allocated at
  interrupt level"; the machine simply stops.

⚠ AND IT GUARDS A DRIFT, NOT A TYPO. The dangerous version of this bug is someone later moving a
  helper that allocates from the Open path into the Read or Start path -- each edit reasonable on
  its own, the call graph quietly changing underneath. That is precisely what CLAUDE.md means by
  "a comment saying 'this runs at task level' is a claim about the call graph, and it expires
  silently when callers change. Audit; don't trust."

⚠ IT DOES NOT EVALUATE THE PREPROCESSOR. This reads the source text, so a call inside a
  `#if SOMETHING 0` block still counts as reachable. That is the SAFE direction -- the audit
  over-approximates and can only be too strict -- but it means a "reaches X" verdict is not
  proof that the BUILD does. 8-9c hit this: with the k92 arm left in place behind
  `#if AP_ARM_IRQ_IN_OPEN 0`, this script reports EnetHAL_Open reaching ApShimIrqEnable, and
  the preprocessed source says otherwise:

      powerpc-apple-macos-gcc -E -I. ap_shim.c | <look at the Open arm>

  Same family as the MakePEF export checker that modelled the generator instead of reading the
  table. When the answer matters, ask the thing that actually decides.

Usage:  check-exec-level.py            exit 0 = clean
"""
import os, re
import sys

# Primitives that are safe ONLY at task level.
TASK_ONLY = {
    "NewPtrSysClear": "allocator", "NewPtr": "allocator", "NewPtrSys": "allocator",
    "NewHandle": "allocator", "DisposePtr": "allocator", "DisposeHandle": "allocator",
    "LockMemory": "VM (may page)", "UnlockMemory": "VM (may page)",
    "GetPhysical": "VM (may page)",
    "RegistryEntryIDInit": "Name Registry", "RegistryEntryIterateCreate": "Name Registry",
    # ⚠ 8-8: Apple's PCI book documents InstallInterruptFunctions as a TASK-LEVEL-ONLY call, and
    # RegistryPropertyGet walks the Name Registry. Both are reachable from the 8-7 arm/disarm
    # path, and the audit could not see them because they were never in this table.
    "InstallInterruptFunctions": "interrupt install (task level only)",
    "GetInterruptFunctions": "interrupt install (task level only)",
    "RegistryPropertyGet": "Name Registry",
    # ★ k204: the link layer's task-level pump. NewNMUPP is a macro over NewRoutineDescriptor, which
    # ALLOCATES (in TheZone -- hence SetZone); NMRemove is task level only (NMInstall is the legal
    # interrupt-level half, per Apple's USBMassStorageSupport RE); CancelTimer runs only at teardown.
    "NewNMUPP": "allocator (routine descriptor)", "NewRoutineDescriptor": "allocator",
    "DisposeNMUPP": "allocator (routine descriptor)", "DisposeRoutineDescriptor": "allocator",
    "SetZone": "Memory Manager zone switch", "NMRemove": "Notification Manager (task level only)",
    "CancelTimer": "timer teardown (task level here)",
    # ★ k205: the status block is published through the Gestalt Manager, which allocates -- task level.
    "Gestalt": "Gestalt Manager", "NewGestaltValue": "Gestalt Manager (allocates)",
    "ReplaceGestaltValue": "Gestalt Manager (allocates)",
}
# Never legal from any selector.
# ★★★ THE ONE GUARDED EXCEPTION, AND THE GUARD IS WHAT MAKES IT ONE.
#
# 8-21 needs the driver to write its own log, because k110 proved the application is connected to
# a DIFFERENT COPY of this fragment than the one Open Transport drives, so AirPortShimGetLog can
# never replay the ring that matters. The File Manager below task level hangs Mac OS 9 with no
# NMI and has bitten this project three times, so the call is not simply allowed: it sits behind
# a RUNTIME measurement of the execution level -- the same call Apple's own sample uses for the
# same worry (USBEnetDriver.c:537).
#
# A static audit cannot see a runtime guard. So rather than exempt the function, this REQUIRES
# the guard: the File Manager may be reached only through FM_GUARDED_BY, and that function must
# still contain the check. Delete the guard and this audit fails, which is the entire point --
# an exemption that cannot notice its own precondition disappearing is just a silenced warning.
# A SET, not one name: 8-25 added AirPortShimSnapshot, which writes the periodic status to its
# own appended file and carries its own copy of the same guard. Every member must contain the
# check, and the File Manager must be unreachable except through one of them.
FM_GUARDED_BY = ("ApShimDumpRingToFile", "AirPortShimSnapshot", "SnapW", "ApShimLoadKnown")
FM_GUARD_PATTERN = r"CurrentExecutionLevel\s*\(\s*\)\s*!=\s*0"

ALWAYS_FORBIDDEN = {
    "FSWrite": "File Manager", "FlushVol": "File Manager", "FSpCreate": "File Manager",
    "FSpOpenDF": "File Manager", "FindFolder": "File Manager", "FSMakeFSSpec": "File Manager",
    "SetFPos": "File Manager", "FSClose": "File Manager", "FSpDelete": "File Manager",   # k204: the append path
    "GetResource": "Resource Manager", "HLock": "Resource Manager",
    "LoadFw": "calls GetResource", "TxProbeHere": "PROBE-ONLY: transmits a frame",
}
TASK_TIME_SELECTORS = {"EnetHAL_Open", "ValidateHardware", "OTScanPorts",
                       "AirPortShimRejoin", "ApLinkNmResp"}
# ★ k204: ApLinkNmResp is the Notification Manager response (nmStr = 0): task level in whatever process
#   runs its event loop. It measures CurrentExecutionLevel() itself before doing anything, and it runs the
#   join (config space, spins) and the log flush -- so it is AUDITED as a task-time entry, like the
#   AirPortShimRejoin it succeeds, rather than trusted.
# EnetHAL_Open is the only one Apple documents as task time.
# ⚠ ValidateHardware is task time too, but it is the one entry point that runs AT BOOT --
#   OpenTransportKernel.h:2507, "called only once, at system boot time". Nothing of ours has run
#   before it: no bring-up, no globals, no mapped BAR. It is listed here so it is an AUDITED entry
#   point rather than an invisible one; a later change that reaches allocation or hardware from
#   it would be a boot hang, recoverable only from another volume.


def strip_noise(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    return re.sub(r'"(\\.|[^"\\])*"', '""', t)


def bodies(src):
    out = {}
    for m in re.finditer(r'^(?:static\s+)?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{',
                         src, re.M):
        i = m.end() - 1
        d = 0
        for j in range(i, len(src)):
            if src[j] == '{':
                d += 1
            elif src[j] == '}':
                d -= 1
                if d == 0:
                    out[m.group(1)] = src[i:j]
                    break
    return out


def reached(defs, roots, skip=()):
    """Call-graph closure. `skip` prunes named functions, which is how the guarded File Manager
    exception below asks "is this still reachable if we do NOT go through the guard?"."""
    seen, stack = set(), list(roots)
    while stack:
        fn = stack.pop()
        if fn in seen or fn in skip:
            continue
        seen.add(fn)
        body = defs.get(fn)
        if body:
            stack.extend(set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body)))
    return seen


def main():
    # ⚠⚠ EVERY HEADER THE DRIVER COMPILES MUST BE IN THIS LIST, OR THE AUDIT PRINTS CLEAN OVER
    # A HOLE. A call graph that cannot see a callee does not report it as unknown -- it reports
    # the caller as having no such edge, which looks exactly like safety. The project's own
    # "static-audit blind spots" note is about this failure and it has already cost a defect.
    #
    # 8-6a is precisely when it would have bitten: EnetHAL_Read now runs re-entrantly, possibly
    # below task level, and reaches ApRxToEnet -> ApCcmpDecap -> AES. Without ap_enet.h and
    # ap_ccmp.h below, the audit would have declared Read clean having never looked at it.
    # ⛔ AND A HAND-MAINTAINED LIST IS ITSELF THE HOLE. The list used to name fourteen headers;
    # ap_ndrv.h was added at k98 and never joined it, so DoDriverIO -- and then ValidateHardware,
    # the one entry point that runs at BOOT -- sat outside the audit while it printed clean. The
    # list did not fail loudly, it just stopped covering the newest code.
    #
    # ⚠ AND GLOBBING ap_*.h IS THE OPPOSITE ERROR, WHICH IS JUST AS BAD. Tried first, it pulled
    #   in ap_log.h -- which ap_shim.c does not include, the shim logs to ap_ring.h's memory sink
    #   precisely so it cannot touch the File Manager -- and the audit promptly reported
    #   "EnetHAL_Open and EnetHAL_Start reach FSWrite, NEVER legal". Two alarming findings, both
    #   phantom, on the project's most-bitten rule. Preprocessing the real translation unit shows
    #   no gLogRef in it at all. A false ⛔ on that rule is expensive in a different way: it
    #   trains you to discount the one report that matters.
    #
    # So: follow what ap_shim.c ACTUALLY includes, transitively. Self-maintaining and honest.
    here = os.path.dirname(os.path.abspath(__file__))
    files, seen = [], set()
    def walk(f):
        if f in seen or not os.path.exists(os.path.join(here, f)): return
        seen.add(f); files.append(f)
        txt = open(os.path.join(here, f), encoding='utf-8').read()
        for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', txt, re.M):
            walk(inc)
    walk("ap_shim.c")
    # 8-58: ap_otmodl.c is a SEPARATE .c linked into the same fragment (not #included), so the walk
    # above never reaches it -- yet ApShimPumpLocked now calls ApOtmRxInject, which lives there and
    # runs at secondary-interrupt level. Parse it too, or the ISR path has a hole exactly where the
    # new interrupt code is. [[reference_static_audit_blind_spots]]
    walk("ap_otmodl.c")
    # ⚠ This reaches ap_psk_config.h, which holds the Wi-Fi passphrase, because the shim really
    #   does include it. strip_noise() blanks every string literal before anything looks at the
    #   text, and this script prints only function names and counts -- but excluding the file
    #   would put a hole back in the graph, so it is read and neutered rather than skipped.
    print("  reading ap_shim.c's own include graph, %d file(s):" % len(files))
    print("    %s" % " ".join(files))
    src = "".join(strip_noise(open(os.path.join(here, f), encoding='utf-8').read())
                  for f in files)
    defs = bodies(src)
    entry = defs.get("EnetHAL_Entry")
    if entry is None:
        sys.exit("check-exec-level: EnetHAL_Entry not found")

    # Split the dispatch switch into per-selector arms.
    arms, cur = {}, []
    labels = []
    for line in entry.split('\n'):
        m = re.findall(r'case\s+(EnetHAL_\w+)\s*:', line)
        if m:
            if labels and cur:
                for L in labels:
                    arms.setdefault(L, []).extend(cur)
                cur = []
            if not cur:
                labels = labels + m if not cur else m
            labels = m if not cur else labels + m
            continue
        cur.append(line)
    for L in labels:
        arms.setdefault(L, []).extend(cur)

    # ★ 8-14: ValidateHardware is NOT in the dispatch switch. Open Transport calls it directly,
    # once, at system boot time -- so a per-selector audit built only from EnetHAL_Entry would
    # leave the single most dangerous entry point in the driver unexamined, which is the exact
    # blind spot the two interrupt roots below were added to close.
    # ⚠ Keep this list equal to the boot-time entry points the .exp exports. Each one was
    #   invisible to this audit until it was named here, and a boot-time fault is the one class
    #   that needs another volume to recover from.
    # ⚠ AirPortShimRejoin re-runs the WHOLE bring-up, allocations and all, from the boot
    #   vehicle's Notification Manager pump. It measures CurrentExecutionLevel() itself, but an
    #   entry point that allocates and resets the 802.11 core must be AUDITED, not trusted --
    #   that is the same reasoning that put ValidateHardware and OTScanPorts in this list.
    for boot_entry in ("ValidateHardware", "OTScanPorts", "AirPortShimRejoin", "ApLinkNmResp"):
        if boot_entry in defs:
            arms[boot_entry] = defs[boot_entry].split('\n')

    bad = []
    guard_notes = []
    # ⛔⛔ 8-7: THE TWO INTERRUPT ROOTS. These are not selectors and nothing in the dispatch
    # switch reaches them -- the hardware does. A per-selector audit would have declared the
    # whole driver clean while the most dangerous code in it went unexamined.
    #
    # The rule here is stricter than for any selector: NOTHING task-only is permissible, not
    # even from Open's list, because these run at hardware and secondary interrupt level where
    # an allocation does not fail -- it corrupts or hangs, with nothing in the log to say why.
    # That is the failure mode this project has hard-hung machines with three times.
    irq_roots = {
        "ApShimIsr":              "HARDWARE interrupt level",
        "ApShimSecondaryHandler": "SECONDARY interrupt level",
        # ★ k204: the link heartbeat -- a SetInterruptTimer handler runs at secondary interrupt level.
        "ApLinkTimer":            "SECONDARY interrupt level (k204 heartbeat)",
        # ★ k204: OT's deferred task. Never a root before (a blind spot); the link layer now schedules it
        # from task level as well as from the secondary, so name it and hold it to the same rule.
        "ApOtmRxDeferred":        "OT DEFERRED-TASK level",
    }
    print("  interrupt root                 calls reachable   verdict")
    for fn, lvl in sorted(irq_roots.items()):
        if fn not in defs:
            print("   %-28s     ?          ⚠ NOT FOUND -- renamed? the audit is blind" % fn)
            bad.append((fn, ["<missing>"], "root not found"))
            continue
        r = reached(defs, {fn})
        hits = sorted((r & set(TASK_ONLY)) | (r & set(ALWAYS_FORBIDDEN)))
        # ExpMgr config-space access is task-level only and is the specific hazard 8-7 designs
        # around: the ISR cannot re-point the BAR0 window, it can only verify it.
        cfg = sorted(r & {"ExpMgrConfigReadLong", "ExpMgrConfigWriteLong", "SsbSelectCore"})
        # ★ k204: the log ring (ap_ring.h) updates its cursor with three non-atomic stores and has exactly
        # ONE writer, task level; an interrupt root that reaches it can interleave with a task-level Say()
        # and corrupt a line. Written in ap_ring.h since 8-2d-1 and never enforced until now.
        ringw = sorted(r & {"Out", "ApRingPut", "ApRingReset"})
        # ★ k207: the radio's off/on path SPINS (MacSuspend up to ~40 ms, DrainTxStatus) and rewrites the
        # radio's override registers -- task level only, from the pump's command handler. Spins in general
        # are NOT in TASK_ONLY: the receive pump reaches RxWaitFor with a 0 timeout, which a call graph
        # cannot tell from a real wait (tried 2026-09-30: listing them flags three legitimate paths). So the
        # power path is named here, for interrupt roots only, where none of it may ever appear.
        power = sorted(r & {"MacSuspend", "DrainTxStatus", "ApLinkSetPower", "ApShimRadioSoftOff",
                            "ApShimRadioSoftOn", "ApShimTxGoodbye"})
        # ★ k208: the receive pump now TRANSMITS (a group key message 2), and only through ApShimTxFromPump --
        # ApShimTxCore without the shim era's after-send probes. The join's one-buffer transmitter resets the
        # TX engine under the running ring (ApShimTxArm, PostTxFrameAt), and ApShimWriteFrame carries the
        # probes (DrainTxStatus spins and prints; the DHCP poll spins 20 ms). None of them may be reachable
        # from an interrupt root. (OT reaches ApShimWriteFrame through a qinit pointer, which no call graph
        # sees -- that path is ApShimTxCore's contract, stated at the function.)
        txpath = sorted(r & {"ApShimTxArm", "PostTxFrameAt", "ApShimTxShimProbes", "ApShimWriteFrame",
                             "ApShimSendM2"})
        note = ""
        if hits:
            bad.append((fn, hits, lvl)); note = "⛔ " + ", ".join(hits)
        elif ringw:
            bad.append((fn, ringw, lvl + " -- the log ring has one writer, task level (ap_ring.h)"))
            note = "⛔ log ring: " + ", ".join(ringw)
        elif power:
            bad.append((fn, power, lvl + " -- the k207 radio off/on path (spins; task level only)"))
            note = "⛔ radio power: " + ", ".join(power)
        elif txpath:
            bad.append((fn, txpath, lvl + " -- k208: an interrupt root transmits only through ApShimTxFromPump"))
            note = "⛔ transmit path: " + ", ".join(txpath)
        elif cfg:
            bad.append((fn, cfg, lvl + " -- PCI config space")); note = "⛔ config space: " + ", ".join(cfg)
        else:
            note = "ok (%s)" % lvl
        print("   %-28s %5d          %s" % (fn, len(r), note))
    print()

    print("  selector                       calls reachable   task-only primitives")
    for sel in sorted(arms):
        called = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', '\n'.join(arms[sel])))
        r = reached(defs, called)
        task_hits = sorted(r & set(TASK_ONLY))
        always = sorted(r & set(ALWAYS_FORBIDDEN))
        # Strip the File Manager hits that are reachable ONLY via the guarded dumper, and only
        # while the guard is actually in it.
        if always and any(fn in defs for fn in FM_GUARDED_BY):
            # SnapW is a leaf helper reached only from AirPortShimSnapshot and gated by that
            # function's guard plus its own gSnapLog!=0 check; the two entry points must each
            # carry the execution-level test.
            guarded = all(fn not in defs or re.search(FM_GUARD_PATTERN, defs[fn]) is not None
                          for fn in ("ApShimDumpRingToFile", "AirPortShimSnapshot"))
            fm = sorted(k for k in always if ALWAYS_FORBIDDEN[k] == "File Manager")
            if fm:
                without = reached(defs, called, skip=set(FM_GUARDED_BY))
                only_via = not (set(fm) & without)
                if guarded and only_via:
                    always = [k for k in always if k not in fm]
                    guard_notes.append((sel, fm))
                elif not guarded:
                    bad.append((sel, fm, "the CurrentExecutionLevel()==0 guard is GONE from "
                                         + "/".join(FM_GUARDED_BY) + " -- the File Manager is now "
                                         "reachable UNGUARDED, which hangs OS 9 with no NMI"))
                elif not only_via:
                    bad.append((sel, fm, "reached OUTSIDE " + "/".join(FM_GUARDED_BY)
                                         + " -- the guard does not cover this path"))
        # ⚠⚠ 8-8 ADDED THIS, AFTER NOTICING THE AUDIT'S OWN BLIND SPOT. PCI config space was
        # being checked for the interrupt roots and for nothing else -- but EnetHAL_Read and
        # EnetHAL_Write may both run below task level, OT decides, and neither may re-point the
        # BAR0 window there any more than an ISR may. The audit was looking for the right thing
        # in only two of the places it can happen.
        cfg = sorted(r & {"ExpMgrConfigReadLong", "ExpMgrConfigWriteLong", "SsbSelectCore"})
        note = ""
        if always:
            bad.append((sel, always, "NEVER legal"))
        elif cfg and sel not in TASK_TIME_SELECTORS:
            bad.append((sel, cfg, "PCI config space below task level"))
            note = "⛔ config space: " + ", ".join(cfg)
            note = "⛔ " + ", ".join(always)
        elif task_hits and sel not in TASK_TIME_SELECTORS:
            bad.append((sel, task_hits, "not task time"))
            note = "⛔ " + ", ".join(task_hits)
        elif task_hits:
            note = "ok (task time): " + ", ".join(task_hits[:3]) + ("…" if len(task_hits) > 3 else "")
        print("   %-28s %5d          %s" % (sel, len(r), note or "-"))

    print()
    if not bad:
        for sel, fm in guard_notes:
            print("  [ok] %s reaches %s ONLY through %s(), which still begins with the"
                  % (sel, ", ".join(fm), "/".join(FM_GUARDED_BY)))
            print("       CurrentExecutionLevel()==0 check. Guarded, not exempted.")
        print("  [ok] allocation and VM calls are confined to EnetHAL_Open; nothing forbidden "
              "anywhere.")
        return 0
    for sel, syms, why in bad:
        print("  ⛔ %s reaches %s -- %s" % (sel, ", ".join(syms), why))
    print()
    print("  ⚠ Apple documents ONLY EnetHAL_Open as task time. Allocating or paging from any")
    print("    other selector may run below task level, where it does not fail -- it corrupts")
    print("    or hangs, with nothing in the log to say why.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
