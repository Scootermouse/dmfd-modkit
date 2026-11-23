"""Palette files (Win/plN_pal.osb, Win/plN_cwXX_pal.osb, ..._ex_pal.osb) <-> swatch PNGs.

A palette file is an osb holding one palette block:
    u32 hash, -1, -1, 0x34, 0x14, 4, 0, nPal, 0, nCol, 0x34, nPal', 0x34 + nPal*nCol*4
then nPal palettes of nCol colours, stored BGRA (colour 0 is transparent), then
a small tree naming the palettes. Edits keep the counts, so only colours change.

The PNG is a grid: one row per palette (colour scheme), one square per colour,
CELL pixels apart; the colour is read from the middle of each square.

Body palettes (plN_pal) hold 48 schemes = 8 highlight colours x 6 contract
elements, row = highlight * 6 + element. Most colours are the same in every
row; for those, changing the first row changes all rows (unless other rows
were edited too).
"""
import re
import struct

import osb

CELL, SWATCH = 12, 11      # 11x11 colour squares on a 12-pixel grid
GAP = b"\x50\x50\x50\xff"  # grid line colour


def find_block(b):
    """-> (offset of the colour data, nPal, nCol) of the palette block."""
    o = 0
    while True:
        o = b.find(b"\xff" * 8, o)
        if o < 0:
            raise ValueError("no palette block")
        if o >= 4 and o + 48 <= len(b):
            v = struct.unpack_from("<10I", b, o + 8)
            if v[0] == 0x34 and v[1] == 0x14 and v[4] and v[6] and v[9] == 0x34 + v[4] * v[6] * 4:
                return o - 4 + 0x34, v[4], v[6]
        o += 1  # byte by byte: the ID just before the marker can end in 0xFF too


def colours(b):
    """-> list of rows, each a list of RGBA bytes."""
    base, npal, ncol = find_block(b)
    out = []
    for p in range(npal):
        q = base + p * ncol * 4
        out.append([bytes((b[q + 4 * k + 2], b[q + 4 * k + 1], b[q + 4 * k], b[q + 4 * k + 3]))
                    for k in range(ncol)])
    return out


def export_png(b, path):
    rows = colours(b)
    ncol = len(rows[0])
    W, H = ncol * CELL + 1, len(rows) * CELL + 1
    img = bytearray(GAP * (W * H))
    for r, row in enumerate(rows):
        for c, rgba in enumerate(row):
            for y in range(SWATCH):
                o = ((r * CELL + 1 + y) * W + c * CELL + 1) * 4
                img[o:o + SWATCH * 4] = rgba * SWATCH
    osb.write_png(path, W, H, bytes(img))
    return len(rows), ncol


def read_png(path, npal, ncol):
    """-> rows of RGBA from a swatch PNG, or raise ValueError if the grid size is wrong."""
    W, H, img = osb.read_png_rgba(path)
    if (W, H) != (ncol * CELL + 1, npal * CELL + 1):
        raise ValueError("picture is %dx%d; this palette file needs %dx%d (%d schemes x %d colours). "
                         "Don't resize it." % (W, H, ncol * CELL + 1, npal * CELL + 1, npal, ncol))
    mid = 1 + SWATCH // 2
    out = []
    for r in range(npal):
        row = []
        for c in range(ncol):
            o = ((r * CELL + mid) * W + c * CELL + mid) * 4
            row.append(bytes(img[o:o + 4]))
        out.append(row)
    return out


def ice(s):
    """The game's string hash (0x7A3220)."""
    h = 0xCDE723A5
    for c in s.encode():
        h = ((h + (c - 256 if c > 127 else c)) * 0x8D) & 0xFFFFFFFF
    return h


def osp_id(sprite):
    """The palette ID the game looks for with a sprite: the hash of "<sprite>.osp".

    pl0_cb00.osp = the Empress's palette, pl0_cw00.osp = the starting sword's,
    pl5_pal.osp = the Bandit's (her only exception). While drawing, the game finds a
    class's palette by the hash of the palette file name plus this ID (0xD66730)."""
    return ice(re.sub(r"\.osb$", "", sprite) + ".osp")


def sprite_for(palette_name):
    """Best guess of the sprite a palette file belongs to: plN_pal -> plN_cb00, X_pal -> X."""
    m = re.fullmatch(r"pl(\d)_pal", palette_name)
    if m:
        return "pl%s_cb00" % m.group(1)
    return palette_name[:-4] if palette_name.endswith("_pal") else palette_name


def get_id(b):
    return struct.unpack_from("<I", b, find_block(b)[0] - 0x34)[0]


def set_id(b, name, base_name=None, sprite=None):
    """Copy of palette file b with the ID the game expects for the palette `name`
    (or for `sprite` when given, e.g. a class palette used with its body file)."""
    out = bytearray(b)
    struct.pack_into("<I", out, find_block(b)[0] - 0x34, osp_id(sprite or sprite_for(name)))
    return bytes(out)


def import_png(b, path, report=print):
    """-> edited palette file bytes, or None when nothing changed."""
    base, npal, ncol = find_block(b)
    old = colours(b)
    new = [list(r) for r in read_png(path, npal, ncol)]
    spread = 0
    for c in range(ncol):
        if npal < 2 or len({old[r][c] for r in range(npal)}) != 1:
            continue
        if new[0][c] != old[0][c] and all(new[r][c] == old[r][c] for r in range(1, npal)):
            for r in range(1, npal):
                new[r][c] = new[0][c]
            spread += 1
    if spread:
        report("palettes: %d colour(s) changed in the first row, applied to all %d rows" % (spread, npal))
    if new == old:
        return None
    out = bytearray(b)
    changed = 0
    for r in range(npal):
        for c in range(ncol):
            if new[r][c] != old[r][c]:
                R, G, B, A = new[r][c]
                q = base + (r * ncol + c) * 4
                out[q:q + 4] = bytes((B, G, R, A))
                changed += 1
    report("palettes: %d colour(s) changed" % changed)
    return bytes(out)
