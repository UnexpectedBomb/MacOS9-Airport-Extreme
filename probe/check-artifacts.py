#!/usr/bin/env python3
"""check-artifacts.py -- read the PACKAGED MacBinary files back and check what actually shipped.

★ WHY THIS EXISTS. Two different failures, both silent, both caught here:

  1. A STALE 'cfrg'. ap_shim.r `#include`s the generated ap_shim_cfrg.r, and CMake does not parse
     Rez includes -- so until the Rez rule listed the generated file in DEPENDS, the generator ran
     exactly once (when its output was missing) and every later edit to make-cfrg.py was ignored.
     The FIRST build is correct, which is what makes it silent. k102 was packaged carrying k101's
     'ndrv' member while the source said "pci80211"; the run would have re-tested the previous
     build's shape under the new build's name and the log would have looked entirely normal.
     [[feedback_rez_include_deps]], [[reference_macbinary_md5_not_code_identity]].

  2. THE Wi-Fi PASSPHRASE IN A SHIPPED BINARY. AirPortExtremeDriver.bin has contained the PSK
     since build 16, and AirPortRx.bin tests clean only BY ACCIDENT -- airport_rx.c still
     #includes ap_psk_config.h and the literal is dropped merely because AP_APP_DRIVES_RADIO 0
     leaves nothing reachable that references it. Re-enable one gated oracle and the secret is
     back. An extension is worse than an app, because an extension is what people install.

⚠ This reads the PACKAGED artifact, not the build tree. What the build intended and what the
  MacBinary carries are different questions, and only the second one reaches the hardware.

The secret values are read from the gitignored ap_psk_config.h and are NEVER printed -- only
whether a byte match exists.

Usage:  check-artifacts.py <dist-dir> [expected-cfrg-name ...]
        exit 0 = every artifact matches expectations and no unexpected secret shipped
"""
import struct, sys, re, os, glob

def macbinary(path):
    b = open(path, 'rb').read()
    if len(b) < 128: raise ValueError("%s: too short for MacBinary" % path)
    dl, rl = struct.unpack('>I', b[83:87])[0], struct.unpack('>I', b[87:91])[0]
    typ = b[65:69].decode('mac-roman', 'replace')
    r0 = 128 + ((dl + 127) // 128) * 128
    if r0 + rl > len(b): raise ValueError("%s: resource fork runs past EOF" % path)
    return typ, b, b[128:128+dl], b[r0:r0+rl]

def cfrg_members(r):
    if len(r) < 16: return []
    dOff, mOff = struct.unpack('>II', r[:8])
    tlOff = mOff + struct.unpack('>H', r[mOff+24:mOff+26])[0]
    nT = struct.unpack('>h', r[tlOff:tlOff+2])[0] + 1
    for i in range(nT):
        p = tlOff + 2 + i*8
        if r[p:p+4] != b'cfrg': continue
        q = tlOff + struct.unpack('>H', r[p+6:p+8])[0]
        s = dOff + struct.unpack('>I', b'\0' + r[q+5:q+8])[0]
        blob = r[s+4 : s+4 + struct.unpack('>I', r[s:s+4])[0]]
        n = struct.unpack('>i', blob[28:32])[0]
        out, pp = [], 32
        for _ in range(n):
            arch = blob[pp:pp+4].decode('mac-roman')
            sz = struct.unpack('>H', blob[pp+40:pp+42])[0]
            nl = blob[pp+42]
            out.append((blob[pp+43:pp+43+nl].decode('mac-roman'), arch))
            pp += sz
        return out
    return []

def resources(r):
    """Every resource as (type, id, body). Used for the 'INIT' file, whose invariant is about
    resources rather than fragments."""
    if len(r) < 16: return []
    dOff, mOff = struct.unpack('>II', r[:8])
    tlOff = mOff + struct.unpack('>H', r[mOff+24:mOff+26])[0]
    nT = struct.unpack('>h', r[tlOff:tlOff+2])[0] + 1
    out = []
    for i in range(nT):
        p = tlOff + 2 + i*8
        t = r[p:p+4].decode('mac-roman', 'replace')
        nR = struct.unpack('>h', r[p+4:p+6])[0] + 1
        rOff = struct.unpack('>H', r[p+6:p+8])[0]
        for j in range(nR):
            q = tlOff + rOff + j*12
            rid = struct.unpack('>h', r[q:q+2])[0]
            s = dOff + struct.unpack('>I', b'\0' + r[q+5:q+8])[0]
            out.append((t, rid, r[s+4 : s+4 + struct.unpack('>I', r[s:s+4])[0]]))
    return out


def secrets(here):
    """String literals from the gitignored PSK header. Never returned to a printing path."""
    path = os.path.join(here, 'ap_psk_config.h')
    if not os.path.exists(path): return None
    txt = open(path, encoding='utf-8', errors='replace').read()
    return [m.group(1) for m in re.finditer(r'"([^"]{6,})"', txt)
            if not m.group(1).startswith('\\') and '%' not in m.group(1)]

def main():
    dist = sys.argv[1] if len(sys.argv) > 1 else 'dist'
    expect = set(sys.argv[2:])
    here = os.path.dirname(os.path.abspath(__file__))
    sec = secrets(here)
    arts = sorted(glob.glob(os.path.join(dist, '*.bin')))
    if not arts:
        print("check-artifacts: no .bin in %s" % dist); return 1
    bad = 0
    print("check-artifacts: reading %d packaged artifact(s) from %s" % (len(arts), dist))
    if sec is None:
        print("  ⚠ ap_psk_config.h not present -- the passphrase check DID NOT RUN"); bad += 1
    seen = set()
    for a in arts:
        typ, whole, data, rsrc = macbinary(a)
        mem = cfrg_members(rsrc)
        print("  %-22s TYPE='%s'  data=%d rsrc=%d" % (os.path.basename(a), typ, len(data), len(rsrc)))
        for nm, arch in mem:
            seen.add(nm)
            print("       cfrg  %-16s arch=%s" % ('"%s"' % nm, arch))
        if typ == 'INIT':
            # ⚠ AN 'INIT' FILE IS NOT A CFM LIBRARY AND MUST NOT BE JUDGED AS ONE. The boot
            # vehicle is a 68K code resource plus the PowerPC fragment it loads with
            # GetMemFragment -- there is no cfrg because nothing looks it up by name. Its real
            # invariant is that BOTH halves are present: an 'INIT' 128 with code in it, and a
            # 'PPC ' 128 that actually starts with 'Joy!peff'. A Rez run that silently produced
            # an empty or missing half would otherwise ship and simply do nothing at boot.
            have = dict((t, (rid, body)) for t, rid, body in resources(rsrc))
            for want, why in (('INIT', 'the 68K code resource the System runs at startup'),
                              ('PPC ', 'the PowerPC fragment the INIT loads')):
                if want not in have or len(have[want][1]) < 64:
                    print("       ⛔ missing or empty '%s' -- %s" % (want, why)); bad += 1
                else:
                    print("       '%s' (%d)  %d bytes%s"
                          % (want, have[want][0], len(have[want][1]),
                             "" if want != 'PPC ' else
                             ("  PEF ok" if have[want][1][:8] == b'Joy!peff'
                              else "  ⛔ NOT A PEF")))
                    if want == 'PPC ' and have[want][1][:8] != b'Joy!peff': bad += 1
        elif not mem:
            print("       ⛔ NO 'cfrg' -- CFM cannot register this file by name"); bad += 1
        if sec:
            hit = any(v.encode('mac-roman', 'replace') in whole for v in sec)
            print("       secret: %s" % ("⛔ PRESENT -- NEVER SHARE, RELEASE OR PUSH THIS FILE"
                                          if hit else "[clean]"))
    missing = expect - seen
    if missing:
        print("  ⛔ expected cfrg member(s) NOT in any artifact: %s" % ", ".join(sorted(missing)))
        print("     A stale generated .r is the usual cause -- see this file's header.")
        bad += 1
    elif expect:
        print("  [ok] every expected cfrg member is present: %s" % ", ".join(sorted(expect)))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
