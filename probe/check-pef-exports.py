#!/usr/bin/env python3
"""check-pef-exports.py -- refuse to ship a PEF whose exports CFM cannot find.

★ WHY THIS EXISTS. On 2026-09-21 the k66 run reported AirPortShimGetFw and AirPortShimStats as
unexported. They were exported, correctly, and present in the fragment's loader table. CFM simply
could not find them, because MakePEF and CFM disagree about which hash bucket a symbol belongs in:

    MakePEF.cc:212   table[sym.key % sz]                  -- a plain mask of the low bits
    CFM              (word ^ (word >> power)) & (size-1)  -- Apple's XOR fold

The two agree only when bit `power` of the symbol's hash word is clear. MakePEF.cc:206-208 sizes
the table with `while(count / sz >= 10) sz *= 2`, so a fragment with NINE OR FEWER exports gets a
single bucket, both rules trivially yield 0, and the bug is invisible. The tenth export creates a
second bucket and silently strands every symbol whose hash has bit 1 set.

⚠ THE SYMPTOM IS INDISTINGUISHABLE FROM A CODE DEFECT, AND IT LANDS ON THE WRONG SYMBOL. Adding
  AirPortShimGetBuild broke AirPortShimGetFw and AirPortShimStats -- two symbols that had not been
  touched in weeks. The log read "not exported", which is exactly what a missing .exp entry looks
  like. It cost a hardware cycle, and a stale-shim diagnosis one cycle before that.

★ THIS CHECKS THE INVARIANT, NOT A PROXY. "Fewer than ten exports" would also have worked today,
  but it would go stale the moment MakePEF is patched -- and this project has lost three
  recommendations to guards that tested a stand-in instead of the thing. This recomputes both
  placements and compares them, so a FIXED MakePEF passes at any export count, and a broken one
  fails as soon as it actually strands a symbol.

Usage:  check-pef-exports.py <file.pef>        exit 0 = every export is findable
"""
import struct
import sys


def loader_section(d):
    if d[:8] != b"Joy!peff":
        sys.exit("check-pef-exports: not a PEF container (bad 'Joy!peff' tag)")
    for i in range(struct.unpack(">H", d[32:34])[0]):
        v = struct.unpack(">iIIIIIB", d[40 + i * 28: 40 + i * 28 + 25])
        if v[6] == 4:                      # kPEFLoaderSection
            return d[v[5]:]
    sys.exit("check-pef-exports: no loader section -- this fragment exports nothing")


def main(path):
    L = loader_section(open(path, "rb").read())
    h = struct.unpack(">iIiIiIIIIIIIII", L[:56])
    str_off, hash_off, power, count = h[10], h[11], h[12], h[13]
    nbuckets = 1 << power
    key_off = hash_off + nbuckets * 4
    sym_off = key_off + count * 4

    # ⚠ WHERE A SYMBOL ACTUALLY IS, read from the table's own chains -- NOT recomputed from
    #   MakePEF's formula. The first version of this script modelled the generator instead, and
    #   the moment MakePEF was patched the model went stale: it reported three stranded symbols
    #   in a fragment whose table was, in fact, correct. A checker that describes the tool rather
    #   than reading the artifact is the "read the flag, not the prose about the flag" trap, and
    #   it fails in the most dangerous direction -- it would also keep passing a generator that
    #   started writing chains some third way.
    actual = {}
    for b in range(nbuckets):
        slot = struct.unpack(">I", L[hash_off + b * 4: hash_off + b * 4 + 4])[0]
        chain_count, first = (slot >> 18) & 0x3FFF, slot & 0x3FFFF
        for k in range(chain_count):
            actual[first + k] = b

    stranded = []
    for i in range(count):
        klen, khash = struct.unpack(">HH", L[key_off + i * 4: key_off + i * 4 + 4])
        cn, _, _ = struct.unpack(">IIh", L[sym_off + i * 10: sym_off + i * 10 + 10])
        name = L[str_off + (cn & 0xFFFFFF): str_off + (cn & 0xFFFFFF) + klen].decode("latin-1")
        word = (klen << 16) | khash
        put_in    = actual.get(i)                               # where it really sits
        looked_in = (word ^ (word >> power)) & (nbuckets - 1)   # where CFM will look
        if put_in != looked_in:
            stranded.append((name, put_in, looked_in))

    print("check-pef-exports: %s -- %d exports, %d hash bucket(s)"
          % (path.split("/")[-1], count, nbuckets))
    if not stranded:
        print("  [ok] every export is in the bucket CFM will look in.")
        return 0

    print("  ⛔ %d EXPORT(S) ARE UNREACHABLE BY FindSymbol:" % len(stranded))
    for name, put_in, looked_in in stranded:
        print("       %-26s MakePEF put it in bucket %d, CFM looks in %d"
              % (name, put_in, looked_in))
    print()
    print("  ⚠ These symbols ARE in the fragment and ARE correctly listed in the .exp file.")
    print("    CFM will still report them as missing, and the symbol that breaks is NOT")
    print("    necessarily the one you just added -- see the header of this script.")
    print()
    print("  ⚠⚠ MakePEF WAS PATCHED ON 2026-09-21 TO FIX EXACTLY THIS. If you are seeing it")
    print("     again, the most likely cause is that the patch is GONE, not that you did")
    print("     something new: build-toolchain.bash rebuilds MakePEF from source and will")
    print("     silently restore the buggy version.")
    print()
    print("     Re-apply it:   airport-extreme/tools/makepef-export-hash-fix.patch")
    print("       cd ~/Retro68 && git apply <that patch>")
    print("       cd ~/Retro68-build/build-host && make MakePEF")
    print("       cp PEFTools/MakePEF ~/Retro68-build/toolchain/bin/MakePEF")
    print("       cp PEFTools/MakePEF ~/Retro68-build/toolchain-m68k/bin/MakePEF")
    print()
    print("     Stop-gap if you cannot rebuild the tool: get back to NINE OR FEWER exports.")
    print("     MakePEF uses a single bucket below ten and the disagreement cannot bite.")
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
