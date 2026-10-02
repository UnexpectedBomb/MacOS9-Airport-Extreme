#!/usr/bin/env python3
"""check-stack-depth.py -- the worst-case STACK DEPTH from each interrupt root, from gcc's own frame sizes.

check-exec-level.py answers "what may run at interrupt level". This answers "how deep does it go", which
it cannot: an interrupt-level call chain that is legal in every function can still run off the end of a
stack nobody documents the size of. k208 is why it exists. Answering the group key renewal inside the
receive pump took the secondary handler's worst case from 1248 bytes (k207: the CCMP decrypt) to 1824 --
ApAesUnwrap's frame alone is 816 at -O0. Queued out of the pump, with the unwrap's scratch static, it
is 1488.

THE BUDGET is 2048 for every root. OS 9 documents no secondary-interrupt stack size; the evidence is the
Bluetooth driver, which runs all of BTstack in USB completions -- secondary interrupt level on the same
MDD -- with an everyday HID-report chain of about 1.4 KB and static chains past 2 KB, shipped and run for
weeks. Past the budget, the change needs that kind of evidence first, not a bigger number here.

HOW: compiles ap_shim.c and ap_otmodl.c with the build's own flags plus -fstack-usage into a temporary
directory (never the build tree), takes the call graph from check-exec-level.py's parser, and walks it.
Blind to function pointers, like every static graph here -- which is why the roots are named explicitly.

  usage:  check-stack-depth.py            exit 0 = every root within budget
"""
import importlib.util, os, re, shutil, subprocess, sys, tempfile

GCC = os.path.expanduser('~/Retro68-build/toolchain/bin/powerpc-apple-macos-gcc')
FLAGS = ['-std=gnu99', '-fPIC', '-Wno-multichar', '-Wall', '-Wno-unused-function', '-w']   # = build/CMakeFiles/*/flags.make
BUDGET = 2048
ROOTS = ['ApShimSecondaryHandler', 'ApShimIsr', 'ApLinkTimer', 'ApOtmRxDeferred']

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location('cel', os.path.join(here, 'check-exec-level.py'))
    cel = importlib.util.module_from_spec(spec); spec.loader.exec_module(cel)
    tmp = tempfile.mkdtemp(prefix='ap-stack-')
    try:
        frames = {}
        for c in ('ap_shim.c', 'ap_otmodl.c'):
            obj = os.path.join(tmp, c + '.o')
            r = subprocess.run([GCC] + FLAGS + ['-I', here, '-fstack-usage', '-c', os.path.join(here, c), '-o', obj],
                               cwd=tmp, capture_output=True, text=True)
            if r.returncode != 0:
                sys.stderr.write(r.stderr[-2000:]); sys.exit('check-stack-depth: %s did not compile' % c)
            for line in open(obj[:-2] + '.su'):
                m = re.match(r'.*:(\w+)\t(\d+)\t(\w+)', line.strip())
                if m:
                    if m.group(3) != 'static':
                        print('  ⚠ %s has a %s frame -- its size is a lower bound' % (m.group(1), m.group(3)))
                    frames[m.group(1)] = max(frames.get(m.group(1), 0), int(m.group(2)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    src = ''
    for f in sorted(os.listdir(here)):   # every header the driver compiles, and the two sources
        if f in ('ap_shim.c', 'ap_otmodl.c') or (f.endswith('.h') and (f.startswith('ap_') or f.startswith('ssb_'))
                                                  and f != 'ap_psk_config.h'):
            src += cel.strip_noise(open(os.path.join(here, f), encoding='utf-8', errors='replace').read())
    defs = cel.bodies(src)
    calls = {fn: sorted(set(w for w in re.findall(r'\b([A-Za-z_]\w*)\s*\(', b) if w in defs and w != fn))
             for fn, b in defs.items()}

    def worst(fn, path=()):
        if fn in path: return (0, [fn + ' (recursion)'])
        best = (0, [])
        for c in calls.get(fn, []):
            d = worst(c, path + (fn,))
            if d[0] > best[0]: best = d
        return (frames.get(fn, 0) + best[0], [fn] + best[1])

    bad = 0
    print('  interrupt root            worst-case stack (budget %d bytes)' % BUDGET)
    for root in ROOTS:
        if root not in calls:
            print('   %-24s  ⚠ NOT FOUND -- renamed? this check is blind to it' % root); bad += 1; continue
        d, p = worst(root)
        verdict = 'ok' if d <= BUDGET else '⛔ OVER BUDGET'
        bad += d > BUDGET
        print('   %-24s %5d  %s' % (root, d, verdict))
        print('      ' + ' > '.join('%s(%d)' % (f, frames.get(f, 0)) for f in p))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
