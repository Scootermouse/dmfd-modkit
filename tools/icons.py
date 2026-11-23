"""Item icons: Win/com_itic.osb, the 128x128 sheet the menus draw item icons from.

How the game picks an item's icon (see FORMAT.md, "Item icons"):
  item record +0x08 (icon number 0-23)
  -> 0xcccb20: a table of 24 Japanese names, hashed with the usual string hash
  -> the entry with that name in com_itic.osb (tree at 0x6c, 0x1c bytes each)
  -> its animation block (+0x18) -> key data (+0x10) -> mesh index (+0x1c)
  -> the mesh's 4 vertices (x, y, z, u, v) -> a 16x16 rectangle on the texture.
The texture is stored as BGRA; PNGs here are normal RGBA.

Only existing icons can be redrawn this way (every item using that icon
number changes). New icon numbers need new entries in the sheet and a hook
past the game's limit of 24; not done yet.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmfd_data  # noqa: E402
import osb  # noqa: E402
import tb2  # noqa: E402

ASSET = "Win/com_itic.osb"
# The game's names for icon numbers 0-23 (exe 0xcccb20), and what they show.
ICON_NAMES = [
    ("装備不可", "cannot-equip"), ("武器（皇女）", "sword"), ("武器（戦士１）", "axe"),
    ("武器（戦士２）", "greatsword"), ("武器（忍者１）", "kunai"), ("武器（忍者２）", "shuriken"),
    ("武器（魔女）", "book"), ("アクセサリ（男用）", "accessory-male"), ("アクセサリ（女用）", "accessory-female"),
    ("アクセサリ（皇女用）", "accessory-empress"), ("アクセサリ（戦士用）", "accessory-warrior"),
    ("アクセサリ（忍者用）", "accessory-shinobi"), ("アクセサリ（魔女用）", "accessory-witch"),
    ("アクセサリ（共通）", "accessory-all"), ("薬", "medicine"), ("肉", "food"), ("植物の実", "herb"),
    ("宝石", "gem"), ("お金", "money"), ("鍵", "key"), ("武器（皇女２）", "dagger"),
    ("武器（魔女２）", "crystal"), ("アクセサリ（巫女用）", "accessory-oracle"),
    ("アクセサリ（盗賊用）", "accessory-bandit"),
]


def name_hash(jp):
    return tb2.ice_hash(jp.encode("utf-8"))  # the exe stores these names as UTF-8


def load_sheet():
    raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(ASSET)), "rb").read()
    return dmfd_data.decode(raw)[0]


def icon_rects(b):
    """{icon number: (x, y, w, h)} on the sheet's texture."""
    u = lambda o: struct.unpack_from("<I", b, o)[0]
    tree, count, meshes = u(0), u(0x20), u(0x1C)
    tex = osb.textures(b)[0]
    by_hash = {}
    for i in range(count):
        e = tree + 0x1C * i
        by_hash[u(e + 4)] = u(e + 0x18)
    rects = {}
    for n, (jp, _) in enumerate(ICON_NAMES):
        block = by_hash.get(name_hash(jp))
        if block is None:
            continue
        mesh = u(u(block + 0x10) + 0x1C)
        rec = meshes + 4 + 0x18 * mesh          # {0x18, 0, vtxOfs, idxOfs, nVtx, nIdx}
        vtx = rec + u(rec + 8)                   # offsets are relative to the record
        uv = [struct.unpack_from("<2f", b, vtx + 20 * k + 12) for k in range(4)]
        xs = [round(a * tex["w"]) for a, _ in uv]
        ys = [round(c * tex["h"]) for _, c in uv]
        rects[n] = (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))
    return rects


def _swap_rb(px):
    out = bytearray(px)
    out[0::4], out[2::4] = px[2::4], px[0::4]
    return bytes(out)


def icon_file_name(n):
    return "%02d_%s.png" % (n, ICON_NAMES[n][1])


def export(outdir):
    b = load_sheet()
    tex = osb.textures(b)[0]
    W = tex["w"]
    os.makedirs(outdir, exist_ok=True)
    for n, (x, y, w, h) in sorted(icon_rects(b).items()):
        rows = [b[tex["ofs"] + ((y + r) * W + x) * 4: tex["ofs"] + ((y + r) * W + x + w) * 4] for r in range(h)]
        osb.write_png(os.path.join(outdir, icon_file_name(n)), w, h, _swap_rb(b"".join(rows)))
    return len(icon_rects(b))


def import_icons(pngs, problem=print):
    """pngs: {icon number: path}; 0-23 redraw game icons, 24+ add new ones.
    Returns (edited decoded sheet or None, [new icon numbers])."""
    if not pngs:
        return None, []
    b = load_sheet()
    rects = icon_rects(b)
    new_numbers = sorted(n for n in pngs if n >= FIRST_NEW_ICON)
    if new_numbers:
        cells = new_icon_cells(b)
        for n in new_numbers:
            if n not in cells:
                problem("icons: no room for icon %d; new icons can be numbered %d to %d"
                        % (n, FIRST_NEW_ICON, max(cells)))
        new_numbers = [n for n in new_numbers if n in cells]
        b = rebuild(b, [(tb2.ice_hash(new_icon_name(n)), cells[n]) for n in new_numbers])
        rects.update({n: cell_rect(cells[n]) for n in new_numbers})
    b = bytearray(b)
    tex = osb.textures(bytes(b))[0]
    W = tex["w"]
    changed = 0
    for n, path in sorted(pngs.items()):
        if n not in rects:
            if n < FIRST_NEW_ICON:
                problem("icons: no icon number %d" % n)
            continue
        x, y, w, h = rects[n]
        try:
            pw, ph, px = osb.read_png_rgba(path)
        except Exception as e:  # noqa: BLE001 - report any unreadable PNG
            problem("icons: %s could not be read (%s); save it as a normal PNG" % (os.path.basename(path), e))
            continue
        if (pw, ph) != (w, h):
            problem("icons: %s is %dx%d, it must be %dx%d" % (os.path.basename(path), pw, ph, w, h))
            continue
        px = _swap_rb(px)
        for r in range(h):
            o = tex["ofs"] + ((y + r) * W + x) * 4
            b[o:o + w * 4] = px[r * w * 4:(r + 1) * w * 4]
        changed += 1
    return (bytes(b) if changed else None), new_numbers


# ---------------------------------------------------------------------------
# New icon slots: rebuild com_itic.osb with extra entries pointing at free
# cells of the 7x7 grid. The game only knows icon numbers 0-23; the content
# mod's DLL maps 24+ to the names below (hook on 0xcccb20).
#
# File layout (offsets absolute, sections in this order):
#   header 0x6c | tree n*0x1c | blocks n*0x48 | key chunks (+pad) |
#   part table 2*parts*8 | texture header + data | mesh section
# Header u32: [6] mesh section size, [7] mesh offset, [8] entries, [9] parts,
#   [10] part table offset, [19] texture offset, [20..23] file size,
#   [25] [26] texture offset.
# Tree entry: {0x1c, hash, f32 scale, left, right, 1, block}. Block (0x48):
#   +0x10 key chunk offset. Key chunk (0x64): mesh index at 28/48/68/88.
# Part table: (0, texture header offset) x 2 per part.
# Mesh section: count, records {0x18, 0, vtx, idx, 4, 6} (offsets relative to
#   the record), then vertices (x, y, z, u, v), then u16 indices.
FIRST_NEW_ICON = 24
GRID, PITCH = 7, 18


def new_icon_name(n):
    return "MOD_ICON_%02d" % n


def _u(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _meshes(b):
    """[(vertex bytes, index bytes)] in mesh order."""
    sec = _u(b, 0x1C)
    out = []
    for m in range(_u(b, sec)):
        rec = sec + 4 + 0x18 * m
        _, _, vo, io, nv, ni = struct.unpack_from("<6I", b, rec)
        out.append((b[rec + vo:rec + vo + 20 * nv], b[rec + io:rec + io + 2 * ni]))
    return out


def _uv_rect(vtx, tw, th):
    uv = [struct.unpack_from("<2f", vtx, 20 * k + 12) for k in range(len(vtx) // 20)]
    xs = [round(a * tw) for a, _ in uv]
    ys = [round(c * th) for _, c in uv]
    return min(xs), min(ys)


def free_cells(b):
    tex = osb.textures(b)[0]
    used = set()
    for vtx, _ in _meshes(b):
        x, y = _uv_rect(vtx, tex["w"], tex["h"])
        used.add(((y - 1) // PITCH) * GRID + (x - 1) // PITCH)
    return [c for c in range(GRID * GRID) if c not in used]


def new_icon_cells(b):
    """{icon number: free cell} for the extra slots, in grid order."""
    return {FIRST_NEW_ICON + i: c for i, c in enumerate(free_cells(b))}


def cell_rect(c):
    return 1 + PITCH * (c % GRID), 1 + PITCH * (c // GRID), 16, 16


def _bst(order):
    out = []

    def rec(lo, hi):
        if lo >= hi:
            return 0xFFFFFFFF
        mid = (lo + hi) // 2
        at = len(out)
        out.append([order[mid], 0, 0])
        out[at][1] = rec(lo, mid)
        out[at][2] = rec(mid + 1, hi)
        return at
    rec(0, len(order))
    return out


def rebuild(b, extra=()):
    """Re-serialize the sheet, adding entries extra = [(name hash, cell), ...].
    With no extras the result is byte-identical to the input (checked)."""
    H = list(struct.unpack_from("<27I", b, 0))
    hs, n, parts = H[0], H[8], H[9]
    assert hs == 0x6C and H[20] == len(b) and H[7] + H[6] == len(b)
    tree0, blocks0 = hs, hs + 0x1C * n
    keys0 = blocks0 + 0x48 * n
    entries = []
    for i in range(n):
        _, h, scale, _, _, cnt, blk = struct.unpack_from("<IIfIIII", b, tree0 + 0x1C * i)
        entries.append({"hash": h, "scale": scale, "cnt": cnt, "blockidx": (blk - blocks0) // 0x48})
    old_blocks = [bytearray(b[blocks0 + 0x48 * k:blocks0 + 0x48 * (k + 1)]) for k in range(n)]
    keys_region = b[keys0:H[10]]          # old key chunks (+ alignment padding)
    part_tab = bytearray(b[H[10]:H[19]])
    tex_sec = bytearray(b[H[19]:H[7]])
    meshes = _meshes(b)
    assert len(meshes) == parts
    tex = osb.textures(b)[0]

    # new meshes and key chunks
    tpl_v, tpl_i = meshes[0]
    tx0, ty0 = _uv_rect(tpl_v, tex["w"], tex["h"])
    tpl_key = None
    for blk in old_blocks:
        ko = _u(blk, 0x10)
        if _u(b, ko + 28) == _u(b, ko + 48) and b[ko:ko + 4] == b"\x14\0\0\0":
            tpl_key = b[ko:ko + 0x64]
            break
    new = []
    for h, cell in extra:
        x, y, _, _ = cell_rect(cell)
        v = bytearray(tpl_v)
        for k in range(len(v) // 20):
            ou, ov = struct.unpack_from("<2f", v, 20 * k + 12)
            struct.pack_into("<2f", v, 20 * k + 12,
                             (x + round(ou * tex["w"]) - tx0) / tex["w"],
                             (y + round(ov * tex["h"]) - ty0) / tex["h"])
        meshes.append((bytes(v), tpl_i))
        new.append({"hash": h, "scale": 1.0, "cnt": 1, "mesh": len(meshes) - 1})

    N, P = n + len(new), parts + len(new)
    blocks1 = tree0 + 0x1C * N
    keys1 = blocks1 + 0x48 * N
    key_shift = keys1 - keys0
    keys_blob = bytearray(keys_region)
    for e in new:
        k = bytearray(tpl_key)
        for o in (28, 48, 68, 88):
            struct.pack_into("<I", k, o, e["mesh"])
        e["key"] = keys1 + len(keys_blob)
        keys_blob += k
    keys_blob += bytes((-(keys1 + len(keys_blob))) % 8)
    part1 = keys1 + len(keys_blob)
    part_tab += part_tab[:16] * len(new)
    tex1 = part1 + len(part_tab)
    tex1 += (H[19] - tex1) % 16           # keep the texture's 16-byte phase
    tex_shift = tex1 - H[19]
    for o in range(0, len(part_tab), 8):
        if _u(part_tab, o + 4):
            struct.pack_into("<I", part_tab, o + 4, _u(part_tab, o + 4) + tex_shift)
    for t in osb.textures(b):
        struct.pack_into("<I", tex_sec, t["hdr"] - H[19], t["ofs"] + tex_shift)
    mesh1 = tex1 + len(tex_sec)
    mesh1 += (H[7] - mesh1) % 16
    vstart = 4 + 0x18 * len(meshes)
    istart = vstart + sum(len(v) for v, _ in meshes)
    mesh_blob = bytearray(struct.pack("<I", len(meshes)))
    vo = io = 0
    for m, (v, ix) in enumerate(meshes):
        rec = 4 + 0x18 * m
        mesh_blob += struct.pack("<6I", 0x18, 0, vstart + vo - rec, istart + io - rec, len(v) // 20, len(ix) // 2)
        vo += len(v)
        io += len(ix)
    mesh_blob += b"".join(v for v, _ in meshes) + b"".join(ix for _, ix in meshes)

    # blocks keep their original order; new ones go after them
    blocks = bytearray()
    for blk in old_blocks:
        struct.pack_into("<I", blk, 0x10, _u(blk, 0x10) + key_shift)
        blocks += blk
    for k, e in enumerate(new):
        blk = bytearray(old_blocks[0])
        struct.pack_into("<I", blk, 0x10, e["key"])
        e["blockidx"] = n + k
        blocks += blk
    allents = entries + new
    tree = bytearray()
    for i, l, r in _bst(sorted(range(N), key=lambda i: allents[i]["hash"])):
        e = allents[i]
        tree += struct.pack("<IIfIIII", 0x1C, e["hash"], e["scale"], l, r, e["cnt"], blocks1 + 0x48 * e["blockidx"])

    out = bytearray(b[:hs]) + tree + blocks + keys_blob + part_tab
    out += bytes(tex1 - len(out)) + tex_sec
    out += bytes(mesh1 - len(out)) + mesh_blob
    hdr = list(H)
    hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[19] = len(mesh_blob), mesh1, N, P, part1, tex1
    hdr[20] = hdr[21] = hdr[22] = hdr[23] = len(out)
    hdr[25] = hdr[26] = tex1
    struct.pack_into("<27I", out, 0, *hdr)
    return bytes(out)


if __name__ == "__main__":
    print(export(sys.argv[1] if len(sys.argv) > 1 else "icons_original"), "icons exported")
