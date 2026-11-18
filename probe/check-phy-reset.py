#!/usr/bin/env python3
"""check-phy-reset.py -- does ApPhyInitGResetState put EVERY file-scope variable of ap_phy_initg.h back to its
declared value?

★ WHY. k248 runs ApPhyInitG a second time in one load when a Stage 4 oracle fails (ap_shim.c ApShimRadioBringUp,
ap_phy_reset.h). A fresh load gets ApPhyInitG's ~110 file-scope variables from the data section; a second call in
the same load inherits the first call's, and some of them would fail the retry on their own (gBad, the verdict's
running count; gNrssiRestoreBad; the sentinels gLlMin, gTxLoop1, gLoTxBias). ap_phy_reset.h lists every one of them
by hand, and a hand-written list drifts: a variable added to ap_phy_initg.h later is a retry that silently inherits
it. This check is what keeps the list honest, the way check-prefix-order.py keeps the prefix's ORDER honest.

It parses ap_phy_initg.h's top-level declarations (comments, strings and preprocessor lines stripped; brace depth
tracked, so nothing inside a function counts; `const` tables and typedefs skipped) and ApPhyInitGResetState's body
(statements `name = value;` and `memset(name, 0, sizeof name);`). Every variable must be reset exactly once, to
exactly its declared initializer (whitespace ignored); an array declared without one must be memset to 0, and a
scalar declared without one must be set to 0. Anything the reset touches that ap_phy_initg.h does not declare is an
error too (a misspelt name compiles if it happens to exist elsewhere).

Usage:  check-phy-reset.py            exit 0 = complete and exact
"""
import re
import sys

HEADER = "ap_phy_initg.h"
RESET = "ap_phy_reset.h"
FUNC = "ApPhyInitGResetState"


def strip(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(\\.|[^'\\])'", "''", text)
    text = re.sub(r'^[ \t]*#[^\n]*(\\\n[^\n]*)*', ' ', text, flags=re.M)
    return text


def top_level_statements(src):
    depth, cur, out = 0, [], []
    for ch in src:
        if ch == '{':
            if depth == 0:
                cur = []          # a function body or a struct/initializer: what came before is not a statement
            depth += 1
            continue
        if ch == '}':
            depth -= 1
            if depth == 0:
                cur = []
            continue
        if depth:
            continue
        if ch == ';':
            s = ' '.join(''.join(cur).split())
            cur = []
            if s:
                out.append(s)
        else:
            cur.append(ch)
    return out


def split_top_commas(s):
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append(''.join(cur))
    return [p.strip() for p in parts]


def declared(header_src):
    """{name: (initializer or None, is_array)} for every top-level variable; errors for what cannot be read."""
    out, errors = {}, []
    for s in top_level_statements(header_src):
        if s.startswith('typedef') or re.search(r'\bconst\b', s) or '(' in s:
            continue
        if re.fullmatch(r'[A-Za-z_]\w*', s):           # the tail of `typedef struct {...} Name;`
            continue
        if not s.startswith('static '):
            errors.append("a top-level declaration that is not static, not handled: " + s)
            continue
        body = s[len('static '):]
        m = re.match(r'(?:(?:unsigned|signed|volatile)\s+)*[A-Za-z_]\w*\s+', body)
        if not m:
            errors.append("could not read the type of: " + s)
            continue
        for d in split_top_commas(body[m.end():]):
            dm = re.fullmatch(r'\*?\s*([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(?:=\s*(.+))?', d)
            if not dm:
                errors.append("could not read the declarator: " + d)
                continue
            name, arr, init = dm.group(1), dm.group(2), dm.group(3)
            if name in out:
                errors.append("declared twice: " + name)
            out[name] = (None if init is None else ''.join(init.split()), arr is not None)
    return out, errors


def reset_body(reset_src):
    m = re.search(r'\b' + FUNC + r'\s*\(\s*void\s*\)\s*\{', reset_src)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    for j in range(i, len(reset_src)):
        if reset_src[j] == '{':
            depth += 1
        elif reset_src[j] == '}':
            depth -= 1
            if depth == 0:
                return reset_src[i + 1:j]
    return None


def resets(body):
    """{name: [values]}: 'memset0' for a memset to 0, else the assigned text (whitespace removed)."""
    out, errors = {}, []
    for st in body.split(';'):
        st = ' '.join(st.split())
        if not st:
            continue
        mm = re.fullmatch(r'memset\s*\(\s*([A-Za-z_]\w*)\s*,\s*0\s*,\s*sizeof\s*\(?\s*([A-Za-z_]\w*)\s*\)?\s*\)', st)
        if mm:
            if mm.group(1) != mm.group(2):
                errors.append("memset of one name with the size of another: " + st)
            out.setdefault(mm.group(1), []).append('memset0')
            continue
        am = re.fullmatch(r'([A-Za-z_]\w*)\s*=\s*(.+)', st)
        if am:
            out.setdefault(am.group(1), []).append(''.join(am.group(2).split()))
            continue
        errors.append("a statement that is neither an assignment nor a memset to 0: " + st)
    return out, errors


def main():
    hdr = strip(open(HEADER, encoding='utf-8').read())
    rst = strip(open(RESET, encoding='utf-8').read())
    decl, errs = declared(hdr)
    body = reset_body(rst)
    if body is None:
        print("  ⛔ " + FUNC + " not found in " + RESET)
        return 1
    got, errs2 = resets(body)
    errs += errs2
    for name, (init, is_arr) in sorted(decl.items()):
        vals = got.get(name)
        if not vals:
            errs.append("NOT RESET: %s (declared %s)" % (name, "array" if is_arr else ("= " + init if init else "with no initializer")))
            continue
        if len(vals) > 1:
            errs.append("reset more than once: %s %s" % (name, vals))
        want = 'memset0' if (is_arr and init is None) else (init if init is not None else '0')
        if vals[0] != want:
            errs.append("WRONG VALUE: %s reset to %s, declared %s" % (name, vals[0], want))
    for name in sorted(set(got) - set(decl)):
        errs.append("reset but not declared in %s: %s" % (HEADER, name))
    print("  %s declares %d file-scope variables; %s resets %d" % (HEADER, len(decl), FUNC, len(got)))
    if errs:
        print("  ⛔ %d problem(s):" % len(errs))
        for e in errs:
            print("     " + e)
        print("  ⚠ A variable the reset misses is a retry that silently inherits the first run's value.")
        return 1
    print("  [ok] every file-scope variable of %s is reset to its declared value." % HEADER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
