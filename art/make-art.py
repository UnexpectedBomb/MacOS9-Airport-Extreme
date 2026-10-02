#!/usr/bin/env python3
"""make-art.py -- the AirPort Extreme artwork, from the source files to what the builds include.

    python3 art/make-art.py                                  regenerate everything from art/src
    python3 art/make-art.py --extract-apple-dots <file>      first (re)lift Apple's dot bitmaps
                                                             (<file>: Apple's "AirPort Control Strip" --
                                                             its AppleDouble "._" sidecar, a raw resource
                                                             fork, or the file itself on a fork-ful volume)
    (from airport-extreme/; needs Pillow)

SOURCES (art/src/):
    strip-icon.rsrc       resource fork of the user's "AirPort Extreme Strip"      icon family, ID 128
    extension-icon.rsrc   resource fork of the user's "Airport Extreme extension"  family + icns, ID -16455
    cpanel-icon.rsrc      resource fork of the user's "AirPort Extreme C Panel"    icon family, ID 128
    strip-cell.png        the user's "airport-16x16-16bit.png"                    the strip cell's glyph
    apple-dots.png        Apple's three signal-dot bitmaps, 8x8 each, side by side: lit | unlit | off ring
  The user's four were delivered 2026-09-30 in the Pi share's "AirPort files". ⚠ The .rsrc files are the
  raw fork BYTES in an ordinary data fork: git does not store resource forks (the lesson recorded in
  bluetooth/cpanel/scripts/icon-family-to-r.py).

OUTPUTS (committed, so a build never needs Pillow):
    csm/ap_csm_icon.r     the strip module's Finder icon: the family at -16455 (custom icon) and 128 (BNDL)
    csm/ap_csm_art.h      the cell's pixels: the glyph and the three dot bitmaps, RGBA
    cpanel/ap_panel_icon.r the panel's Finder icon: the family at 128 (BNDL) and -16455 (custom icon)
    probe/ap_driver_icon.r the extension's Finder icon: the family and its icns at -16455
    art/preview-cell.png  every cell state on the strip's face, 1:1 and x6 (not used by any build)

★ THE ICON FAMILIES ARE COPIED BYTE FOR BYTE -- no re-drawing, no palette mapping: the bytes the user's
icon editor wrote are the bytes Rez writes (the UT-launcher lesson, os9_control_strip_module).

★ THE CELL (ap_csm.c) IS APPLE'S AirPort CONTROL STRIP MODULE'S, WITH THE USER'S GLYPH. Apple draws one of
its PICTs 130-136 ('off', 'on - 0' ... 'on - 5'): 96 x 16; the glyph at x 5-20; an etched separator at
x 26 (#555555) and 27 (#FFFFFF); five dots every 11 px from x 33; the arrow at x 90-93, rows 4-11.
The glyph is the user's PNG, unmodified. The dots are APPLE'S OWN BITMAPS (the user asked for exactly
Apple's), lifted by --extract-apple-dots from PICT 136 (lit), 131 (unlit) and 130 (the 'off' ring):
  - in each PICT the five dots are ONE identical 8x8 bitmap at (33 + 11 i, 3), with no ink between
    them, and 'on - 1' ... 'on - 4' are exactly lit + unlit side by side -- so three bitmaps reproduce
    every Apple state (checked on extraction);
  - a pixel that is exactly Apple's face colour (#C6C6C6) becomes transparent; every other pixel is
    copied as Apple drew it, the halo's blend into the face included -- identical to Apple's on the
    standard strip face, as Apple's own module is on any other;
  - extraction then RE-ASSEMBLES all seven of Apple's pictures from this script's layout (the one
    ap_csm.c draws, Apple's glyph in the glyph slot) and requires ZERO differing pixels, which proves
    the separator, dot and arrow geometry against Apple's own bitmaps, not against a measurement.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, 'src')
FAMILY = ['ICN#', 'icl4', 'icl8', 'ics#', 'ics4', 'ics8']
SIZES = {'ICN#': 256, 'icl4': 512, 'icl8': 1024, 'ics#': 64, 'ics4': 128, 'ics8': 256}

# The cell's geometry -- ⚠ the same numbers as ap_csm.c's kGlyphX ... kArrowY; change them together.
CELL_W, CELL_H = 96, 16
GLYPH_X = 5
SEP_X = 26
DOT0_X, DOT_STEP, DOT_Y, DOTS = 33, 11, 3, 5
ARROW_X, ARROW_Y = 90, 4
ARROW_ROWS = (1, 2, 3, 4, 4, 3, 2, 1)
APPLE_FACE = (0xC6, 0xC6, 0xC6)


def resources(rf):
    """{(type, id): bytes} from raw resource-fork bytes."""
    doff, moff, _dlen, mlen = struct.unpack('>IIII', rf[:16])
    m = rf[moff:moff + mlen]
    tl_off = struct.unpack('>H', m[24:26])[0]
    tl = m[tl_off:]
    res = {}
    for t in range(struct.unpack('>H', tl[:2])[0] + 1):
        rtype, cnt, ref = struct.unpack('>4sHH', tl[2 + 8 * t:10 + 8 * t])
        for k in range(cnt + 1):
            rid, _nameoff, ao = struct.unpack('>hHI', tl[ref + 12 * k:ref + 12 * k + 8])
            off = ao & 0xFFFFFF
            ln = struct.unpack('>I', rf[doff + off:doff + off + 4])[0]
            res[(rtype.decode('mac-roman'), rid)] = rf[doff + off + 4:doff + off + 4 + ln]
    return res


def read_fork(path):
    """A resource fork from an AppleDouble sidecar, a raw fork dump, or a fork-ful file."""
    d = open(path, 'rb').read()
    if len(d) >= 26 and struct.unpack('>I', d[:4])[0] == 0x00051607:        # AppleDouble
        n = struct.unpack('>H', d[24:26])[0]
        for i in range(n):
            eid, off, ln = struct.unpack('>III', d[26 + 12 * i:38 + 12 * i])
            if eid == 2:
                return d[off:off + ln]
        sys.exit('%s: AppleDouble with no resource fork' % path)
    if len(d) >= 16 and struct.unpack('>I', d[:4])[0] == 256:                # a raw fork's data offset
        return d
    return open(path + '/..namedfork/rsrc', 'rb').read()


def family(path, rid, extra=()):
    """The six classic members at rid (plus any `extra` types), checked for completeness and size."""
    res = resources(open(path, 'rb').read())
    fam = {}
    for t in FAMILY + list(extra):
        b = res.get((t, rid))
        if b is None:
            sys.exit('%s: no %r at ID %d -- the family is incomplete' % (path, t, rid))
        if t in SIZES and len(b) != SIZES[t]:
            sys.exit('%s: %r at ID %d is %d bytes, not %d' % (path, t, rid, len(b), SIZES[t]))
        fam[t] = b
    return fam


def rez_data(t, rid, b, comment):
    lines = ['/* %s */' % comment, "data '%s' (%d, purgeable) {" % (t, rid)]
    for i in range(0, len(b), 32):
        chunk = b[i:i + 32].hex().upper()
        lines.append('    $"%s"' % ' '.join(chunk[j:j + 4] for j in range(0, len(chunk), 4)))
    lines.append('};')
    return '\n'.join(lines)


def write(path, text):
    tmp = path + '.tmp'                       # CLAUDE.md rule 2: temp file, then rename
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(text)
    os.replace(tmp, path)
    print('wrote', os.path.relpath(path, ROOT))


def emit_family_r(out, title, source, fam, ids):
    parts = ['/* %s -- GENERATED by art/make-art.py from art/src/%s. Do not edit; rerun the script.' % (
        os.path.basename(out), source),
             ' * %s. Copied byte for byte from the user\'s icon file.' % title,
             ' * ⚠ Rez #includes are not tracked by CMake: this file is a Rez INPUT or a DEPENDS in its',
             ' * CMakeLists.txt, so an artwork change re-Rezzes. */', '']
    for rid, why in ids:
        for t in FAMILY + [t for t in fam if t not in FAMILY]:
            parts.append(rez_data(t, rid, fam[t], '%s %d: %s' % (t, rid, why)))
        parts.append('')
    write(out, '\n'.join(parts))


# ---- PICT (only what Apple's strip-module pictures use: v2, PackBitsRect, 8-bit indexed) -----------------
def unpackbits(data, n):
    out = bytearray()
    i = 0
    while len(out) < n and i < len(data):
        c = data[i]
        i += 1
        if c < 128:
            out += data[i:i + c + 1]
            i += c + 1
        elif c > 128:
            out += bytes([data[i]]) * (257 - c)
            i += 1
    return bytes(out[:n])


def decode_pict(b):
    """A PICT v2 made of PackBitsRect/Rgn opcodes -> (width, height, [(r, g, b)] row-major)."""
    top, left, bottom, right = struct.unpack('>hhhh', b[2:10])
    W, H = right - left, bottom - top
    canvas = [(255, 0, 255)] * (W * H)                       # magenta = never drawn
    p = 10
    if b[p:p + 4] != b'\x00\x11\x02\xff':
        sys.exit('not a version-2 PICT')
    p += 4
    while p < len(b):
        if p & 1:
            p += 1
        op = struct.unpack('>H', b[p:p + 2])[0]
        p += 2
        if op == 0x00FF:
            break
        if op == 0x0C00:
            p += 24
        elif op in (0x0000, 0x001E):
            pass
        elif op == 0x0001:
            p += struct.unpack('>H', b[p:p + 2])[0]
        elif op == 0x00A0:
            p += 2
        elif op == 0x00A1:
            p += 2
            p += 2 + struct.unpack('>H', b[p:p + 2])[0]
        elif op in (0x0098, 0x0099):
            rowBytes = struct.unpack('>H', b[p:p + 2])[0] & 0x3FFF
            btop, bleft, bbottom, bright = struct.unpack('>hhhh', b[p + 2:p + 10])
            pixelSize = struct.unpack('>H', b[p + 28:p + 30])[0]     # PixMap: rowBytes 0, bounds 2, ...
            p += 46                                                  # ... pixelType 26, pixelSize 28
            _seed, flags, ctSize = struct.unpack('>IHH', b[p:p + 8])
            p += 8
            clut = {}
            for k in range(ctSize + 1):
                v, r, g, bl = struct.unpack('>HHHH', b[p:p + 8])
                p += 8
                clut[k if (flags & 0x8000) else v] = (r >> 8, g >> 8, bl >> 8)
            stop, sleft, _sbottom, _sright = struct.unpack('>hhhh', b[p:p + 8])
            dtop, dleft, dbottom, dright = struct.unpack('>hhhh', b[p + 8:p + 16])
            p += 18                                          # src, dst, mode
            if op == 0x0099:
                p += struct.unpack('>H', b[p:p + 2])[0]
            bw, bh = bright - bleft, bbottom - btop
            if (dright - dleft, dbottom - dtop) != (bw, bh) or (stop, sleft) != (btop, bleft):
                sys.exit('scaled or offset CopyBits: not handled')
            for y in range(bh):
                if rowBytes < 8:
                    row = b[p:p + rowBytes]
                    p += rowBytes
                else:
                    if rowBytes > 250:
                        cnt = struct.unpack('>H', b[p:p + 2])[0]
                        p += 2
                    else:
                        cnt = b[p]
                        p += 1
                    row = unpackbits(b[p:p + cnt], rowBytes)
                    p += cnt
                for x in range(bw):
                    if pixelSize == 8:
                        v = row[x]
                    elif pixelSize == 4:
                        v = (row[x // 2] >> (4 if x % 2 == 0 else 0)) & 15
                    else:
                        sys.exit('pixel size %d not handled' % pixelSize)
                    cx, cy = dleft - left + x, dtop - top + y
                    if 0 <= cx < W and 0 <= cy < H:
                        canvas[cy * W + cx] = clut.get(v, (255, 0, 255))
        else:
            sys.exit('PICT opcode %04x not handled' % op)
    return W, H, canvas


# ---- the cell, composed exactly as ap_csm.c's DrawCell composes it ----------------------------------------
def blend(px, face, dim):
    r, g, b, a = px
    if dim:
        lum = (r * 30 + g * 59 + b * 11) // 100
        r, g, b = (lum + face[0]) // 2, (lum + face[1]) // 2, (lum + face[2]) // 2
    return ((r * a + face[0] * (255 - a)) // 255, (g * a + face[1] * (255 - a)) // 255,
            (b * a + face[2] * (255 - a)) // 255)


def compose_cell(glyph, dots, lit, rings, dim, face=APPLE_FACE):
    """glyph: 16x16 RGBA; dots: (lit, unlit, ring) 8x8 RGBA. Returns 96x16 RGB, row-major.
    ⚠ Mirrors ap_csm.c's DrawCell: change the two together."""
    cell = [face] * (CELL_W * CELL_H)

    def put(x, y, c):
        cell[y * CELL_W + x] = c

    def blit(px, w, h, x0, y0, dimmed):
        for y in range(h):
            for x in range(w):
                if px[y * w + x][3]:
                    put(x0 + x, y0 + y, blend(px[y * w + x], face, dimmed))

    blit(glyph, 16, 16, GLYPH_X, 0, dim)
    for y in range(CELL_H):
        put(SEP_X, y, (0x55, 0x55, 0x55))
        put(SEP_X + 1, y, (0xFF, 0xFF, 0xFF))
    for i in range(DOTS):
        spr = dots[2] if rings else (dots[0] if i < lit else dots[1])
        blit(spr, 8, 8, DOT0_X + i * DOT_STEP, DOT_Y, False)
    for i, w in enumerate(ARROW_ROWS):
        for x in range(w):
            put(ARROW_X + x, ARROW_Y + i, (0, 0, 0))
    return cell


def extract_apple_dots(path):
    """Lift Apple's three dot bitmaps into art/src/apple-dots.png, after proving they reproduce every Apple
    state and that our layout rebuilds Apple's pictures exactly."""
    from PIL import Image
    res = resources(read_fork(path))
    pict = {rid: decode_pict(res[('PICT', rid)]) for rid in range(130, 137)}
    for rid, (w, h, _px) in pict.items():
        if (w, h) != (CELL_W, CELL_H):
            sys.exit('PICT %d is %dx%d, not %dx%d' % (rid, w, h, CELL_W, CELL_H))

    def dot(rid, i):
        _w, _h, px = pict[rid]
        return [px[(DOT_Y + y) * CELL_W + DOT0_X + DOT_STEP * i + x] for y in range(8) for x in range(8)]

    lit, unlit, ring = dot(136, 0), dot(131, 0), dot(130, 0)
    for rid, sprites in ((136, [lit] * 5), (131, [unlit] * 5), (130, [ring] * 5)):
        if any(dot(rid, i) != sprites[i] for i in range(5)):
            sys.exit('PICT %d: the five dots are not one bitmap' % rid)
    for n in range(1, 5):
        if any(dot(131 + n, i) != (lit if i < n else unlit) for i in range(5)):
            sys.exit("PICT %d ('on - %d') is not %d lit + %d unlit" % (131 + n, n, n, 5 - n))

    def rgba(px):
        return [(r, g, b, 0) if (r, g, b) == APPLE_FACE else (r, g, b, 255) for (r, g, b) in px]

    dots = (rgba(lit), rgba(unlit), rgba(ring))
    # Rebuild all seven of Apple's pictures from OUR layout, Apple's glyph in the glyph slot: zero differences.
    for rid in range(130, 137):
        _w, _h, px = pict[rid]
        glyph = rgba([px[y * CELL_W + GLYPH_X + x] for y in range(16) for x in range(16)])
        built = compose_cell(glyph, dots, lit=(rid - 131 if rid > 130 else 0), rings=(rid == 130), dim=False)
        diff = sum(1 for a, b in zip(built, px) if a != b)
        print("  rebuilt Apple's PICT %d from our layout: %d differing pixels of %d" % (rid, diff, len(px)))
        if diff:
            sys.exit('layout mismatch against PICT %d' % rid)
    sheet = Image.new('RGBA', (24, 8))
    sheet.putdata([dots[k][y * 8 + x] for y in range(8) for k in range(3) for x in range(8)])
    out = os.path.join(SRC, 'apple-dots.png')
    sheet.save(out + '.tmp.png')
    os.replace(out + '.tmp.png', out)
    print('wrote', os.path.relpath(out, ROOT), '(lit | unlit | ring, from PICTs 136, 131, 130)')


def c_table(name, px, w, h):
    rows = []
    for y in range(h):
        rows.append('    ' + ', '.join('{0x%02X,0x%02X,0x%02X,0x%02X}' % p for p in px[y * w:(y + 1) * w]) + ',')
    return 'static const ApCsmPx %s[%d * %d] = {\n%s\n};' % (name, w, h, '\n'.join(rows))


def main():
    from PIL import Image, ImageDraw
    if len(sys.argv) == 3 and sys.argv[1] == '--extract-apple-dots':
        extract_apple_dots(sys.argv[2])
    elif len(sys.argv) != 1:
        sys.exit(__doc__)

    # ---- the three Finder icon families, byte for byte ----
    strip = family(os.path.join(SRC, 'strip-icon.rsrc'), 128)
    cpanel = family(os.path.join(SRC, 'cpanel-icon.rsrc'), 128)
    ext = family(os.path.join(SRC, 'extension-icon.rsrc'), -16455, extra=['icns'])
    emit_family_r(os.path.join(ROOT, 'csm', 'ap_csm_icon.r'), 'The AirPort Extreme Strip file\'s Finder icon',
                  'strip-icon.rsrc', strip,
                  [(-16455, 'the custom icon (kHasCustomIcon; needs no desktop database)'),
                   (128, 'the bundle\'s icon (BNDL ICN# local 0)')])
    emit_family_r(os.path.join(ROOT, 'cpanel', 'ap_panel_icon.r'), 'The AirPort Extreme control panel\'s Finder icon',
                  'cpanel-icon.rsrc', cpanel,
                  [(128, 'the bundle\'s icon (BNDL ICN# local 0)'),
                   (-16455, 'the custom icon (kHasCustomIcon; needs no desktop database)')])
    emit_family_r(os.path.join(ROOT, 'probe', 'ap_driver_icon.r'), 'The AirPort Extreme extension\'s Finder icon',
                  'extension-icon.rsrc', ext,
                  [(-16455, 'the custom icon (kHasCustomIcon; the extension has no bundle)')])

    # ---- the cell: the user's glyph, every pixel as delivered, and Apple's three dot bitmaps ----
    img = Image.open(os.path.join(SRC, 'strip-cell.png')).convert('RGBA')
    if img.size != (16, 16):
        sys.exit('strip-cell.png is %dx%d, not 16x16' % img.size)
    glyph = list(img.getdata())
    sheet = Image.open(os.path.join(SRC, 'apple-dots.png')).convert('RGBA')
    if sheet.size != (24, 8):
        sys.exit('apple-dots.png is %dx%d, not 24x8' % sheet.size)
    sp = list(sheet.getdata())
    dots = tuple([sp[y * 24 + k * 8 + x] for y in range(8) for x in range(8)] for k in range(3))
    hdr = ['/* ap_csm_art.h -- GENERATED by art/make-art.py. Do not edit; rerun the script.',
           ' *',
           ' * The strip cell\'s pixels, drawn by ap_csm.c with SetCPixel after blending each against the strip\'s',
           ' * own face colour (sampled at draw time, so a theme\'s face is honoured). RGBA, row-major, 0..255.',
           ' *   kCellGlyph  the user\'s "airport-16x16-16bit.png", unmodified: exact at thousands/millions of colours,',
           ' *               the nearest colours at 256 (an icon suite would have been 8-bit even on a millions screen)',
           ' *   kDotLit     Apple\'s lit signal dot       } Apple\'s own bitmaps, from its AirPort Control Strip',
           ' *   kDotDim     Apple\'s unlit signal dot     } module\'s PICTs 136 / 131 / 130; a pixel of Apple\'s',
           ' *   kDotRing    Apple\'s "off" ring           } face colour (#C6C6C6) is transparent (a = 0)',
           ' */',
           '#ifndef AP_CSM_ART_H', '#define AP_CSM_ART_H', '',
           'typedef struct { unsigned char r, g, b, a; } ApCsmPx;', '',
           '#define kGlyphW 16', '#define kGlyphH 16', '#define kDotW   8', '#define kDotH   8', '',
           c_table('kCellGlyph', glyph, 16, 16), '',
           c_table('kDotLit', dots[0], 8, 8), '',
           c_table('kDotDim', dots[1], 8, 8), '',
           c_table('kDotRing', dots[2], 8, 8), '',
           '#endif /* AP_CSM_ART_H */', '']
    write(os.path.join(ROOT, 'csm', 'ap_csm_art.h'), '\n'.join(hdr))

    # ---- the preview: every state, composed exactly as ap_csm.c composes it ----
    states = [('no driver', 0, True, True), ('off', 0, True, False), ('looking / joining', 0, False, False)]
    states += [('joined, %s' % w, n + 1, False, False)
               for n, w in enumerate(['Out of Range', 'Weak', 'Average', 'Good', 'Strong'])]
    Z = 6
    out_img = Image.new('RGB', (CELL_W * Z + CELL_W + 40, len(states) * (CELL_H * Z + 18) + 10), (255, 255, 255))
    d = ImageDraw.Draw(out_img)
    for k, (label, lit, rings, dim) in enumerate(states):
        c = Image.new('RGB', (CELL_W, CELL_H))
        c.putdata(compose_cell(glyph, dots, lit, rings, dim))
        y = 5 + k * (CELL_H * Z + 18)
        d.text((10, y), label, fill=(0, 0, 0))
        out_img.paste(c.resize((CELL_W * Z, CELL_H * Z), Image.NEAREST), (10, y + 14))
        out_img.paste(c, (CELL_W * Z + 30, y + 14))
    out = os.path.join(HERE, 'preview-cell.png')
    out_img.save(out + '.tmp.png')
    os.replace(out + '.tmp.png', out)
    print('wrote', os.path.relpath(out, ROOT))


if __name__ == '__main__':
    main()
