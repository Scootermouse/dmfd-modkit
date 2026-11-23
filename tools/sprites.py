"""Character / object sprite banks (.osb): animations, frames, rendering.

Format (all offsets absolute; see FORMAT.md "Sprites"):
  entry (tree at 0x6c, 0x1c bytes): {0x1c, name hash, f32 length in frames,
                                     left, right, block count, first block}
  block (0x48) = a node: +0x04 node name hash, +0x08 parent node hash (0 for
         the root), +0x0c track count, +0x10 first track
  track: {0x14, key count, channel, 0, 1} then keys of 0x14 bytes:
         {f32 time, ?, value, ?, ?}; first/last keys are -inf/+inf.
         channel 0: value = frame mesh index (the root node's flipbook);
         channel 7/8: value = f32 x/y position (attachment points); others seen.
  frame mesh (mesh section at header +0x1c): quads, each 4 vertices
         (x, y, z, u, v) mapping a texture rectangle to a rectangle in the
         frame (pixels, y up). Tiles may be flipped/rotated in the atlas.
  texture: paletted (pixel = 0, 0, index, alpha) for characters; palette in
         Win/plN_pal.osb.

usage: python -I sprites.py <asset> <outdir> [palette asset [palette index]] [first-last]
  e.g. python -I sprites.py Win/pl0_cb00.osb out Win/pl0_pal.osb 0 0-9
writes one PNG strip per animation (frames left to right, in key order).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmfd_data  # noqa: E402
import osb  # noqa: E402


def _u(b, o):
    return struct.unpack_from("<I", b, o)[0]


def load(asset):
    raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(asset)), "rb").read()
    return dmfd_data.decode(raw)[0]


def is_paletted(b):
    """Paletted sheets store pixels as (0, 0, index, alpha); a few are full colour instead."""
    tex = osb.textures(b)[0]
    px = b[tex["ofs"]:tex["ofs"] + tex["w"] * tex["h"] * 4]
    return not (px[0::4].strip(b"\0") or px[1::4].strip(b"\0"))


def animations(b):
    """[{index, hash, length, nodes: [{name, parent, tracks: {channel: [(time, value)]}}]}].
    "tracks" (compat) = the root node's channel-0 keys as [[(time, mesh)]]."""
    hs, n = _u(b, 0), _u(b, 0x20)
    out = []
    for i in range(n):
        _, h, length, _, _, nblk, blk = struct.unpack_from("<IIfIIII", b, hs + 0x1C * i)
        nodes = []
        for k in range(nblk):
            bo = blk + 0x48 * k
            node = {"name": _u(b, bo + 4), "parent": _u(b, bo + 8), "tracks": {}}
            ntr, to = _u(b, bo + 0x0C), _u(b, bo + 0x10)
            for _ in range(ntr):
                stride, nkey, chan = _u(b, to), _u(b, to + 4), _u(b, to + 8)
                keys = []
                for j in range(nkey):
                    ko = to + 0x14 + stride * j
                    t = struct.unpack_from("<f", b, ko)[0]
                    val = _u(b, ko + 8) if chan == 0 else struct.unpack_from("<f", b, ko + 8)[0]
                    keys.append((t, val))
                node["tracks"][chan] = keys
                to += 0x14 + stride * nkey
            nodes.append(node)
        root = nodes[0]["tracks"].get(0, []) if nodes else []
        out.append({"index": i, "hash": h, "length": length, "nodes": nodes, "tracks": [root] if root else []})
    return out


def frames(anim):
    """Distinct frames of the first track: [(start time, mesh)] without the +-inf sentinels."""
    if not anim["tracks"]:
        return []
    keys = [(t, m) for t, m in anim["tracks"][0] if abs(t) < 1e30]
    out = []
    for t, m in keys:
        if not out or out[-1][1] != m:
            out.append((t, m))
    return out


def mesh_quads(b, m):
    """[(4 x (x, y), 4 x (u, v))] in pixels for frame mesh m."""
    sec = _u(b, 0x1C)
    tex = osb.textures(b)[0]
    rec = sec + 4 + 0x18 * m
    _, _, vo, io, nv, ni = struct.unpack_from("<6I", b, rec)
    v = [struct.unpack_from("<5f", b, rec + vo + 20 * k) for k in range(nv)]
    quads = []
    for q in range(nv // 4):
        c = v[4 * q:4 * q + 4]
        quads.append(([(p[0], p[1]) for p in c], [(p[3] * tex["w"], p[4] * tex["h"]) for p in c]))
    return quads


def quad_pixels(quads, TW):
    """Yield (x, y, atlas byte offset) for every frame pixel the quads cover.
    Shared by render and import, so a round trip maps pixels identically."""
    for d, t in quads:
        dx = [p[0] for p in d]
        dy = [p[1] for p in d]
        ax, bx, ay, by = min(dx), max(dx), min(dy), max(dy)
        if bx <= ax or by <= ay:
            continue
        corner = {}  # texture coordinate of each destination corner (bilinear map)
        for (x, y), uv in zip(d, t):
            corner[(x == bx, y == by)] = uv
        if len(corner) < 4:
            continue
        c00, c10, c01, c11 = corner[(False, False)], corner[(True, False)], corner[(False, True)], corner[(True, True)]
        for yy in range(int(ay), int(by)):
            ty = (yy + 0.5 - ay) / (by - ay)
            for xx in range(int(ax), int(bx)):
                tx = (xx + 0.5 - ax) / (bx - ax)
                u = (1 - tx) * (1 - ty) * c00[0] + tx * (1 - ty) * c10[0] + (1 - tx) * ty * c01[0] + tx * ty * c11[0]
                v = (1 - tx) * (1 - ty) * c00[1] + tx * (1 - ty) * c10[1] + (1 - tx) * ty * c01[1] + tx * ty * c11[1]
                yield xx, yy, (int(v) * TW + int(u)) * 4


def canvas(b, meshes):
    """Shared frame canvas for these meshes: (x0, y1, W, H) in sprite pixels (y up)."""
    allq = [q for m in set(meshes) for q in mesh_quads(b, m)]
    xs = [x for d, _ in allq for x, _ in d]
    ys = [y for d, _ in allq for _, y in d]
    if not xs:
        return 0, 1, 1, 1
    x0, x1, y0, y1 = int(min(xs)), int(max(xs)), int(min(ys)), int(max(ys))
    return x0, y1, x1 - x0, y1 - y0


def render(b, meshes, pal=None, box=None):
    """Render frames on a shared canvas. Returns (w, h, [rgba bytes per frame])."""
    tex = osb.textures(b)[0]
    TW = tex["w"]
    px = b[tex["ofs"]:tex["ofs"] + TW * tex["h"] * 4]
    x0, y1, W, H = box or canvas(b, meshes)
    out = []
    for m in meshes:
        img = bytearray(W * H * 4)
        for xx, yy, so in quad_pixels(mesh_quads(b, m), TW):
            if not px[so + 3]:
                continue
            if pal:
                i = px[so + 2]
                if i >= len(pal):
                    continue  # a few stray pixels point past the palette; import ignores them too
                c = pal[i]
            else:
                c = px[so + 2:so + 3] + px[so + 1:so + 2] + px[so:so + 1] + px[so + 3:so + 4]  # BGRA
            if not c[3]:
                continue
            do = ((y1 - 1 - yy) * W + (xx - x0)) * 4  # y up -> image rows down
            img[do:do + 4] = c
        out.append(bytes(img))
    return W, H, out


# ---------------------------------------------------------------------------
# Editing: export frames as PNGs, import edited ones back into the atlas.
# File names carry what import needs:  a001_f05_m348_o-41_92.png
#   animation 1, frame 5, frame mesh 348, canvas origin x=-41, top y=92.
FRAME_RE = r"_m(\d+)_o(-?\d+)_(-?\d+)\.png$"


MARGIN = 24  # empty border around exported frames, room to paint beyond the outline


def export_frames(b, outdir, pal, which=None, margin=MARGIN):
    os.makedirs(outdir, exist_ok=True)
    n = 0
    covered = set()
    for a in animations(b):
        if which and a["index"] not in which:
            continue
        fr = frames(a)
        if not fr:
            continue
        meshes = [m for _, m in fr]
        x0, y1, cw, ch = canvas(b, meshes)
        box = (x0 - margin, y1 + margin, cw + 2 * margin, ch + 2 * margin)
        W, H, imgs = render(b, meshes, pal, box)
        for k, ((t, m), img) in enumerate(zip(fr, imgs)):
            fn = "a%03d_f%02d_m%d_o%d_%d.png" % (a["index"], k, m, box[0], box[1])
            osb.write_png(os.path.join(outdir, fn), W, H, img)
            n += 1
        covered.update(meshes)
    # Pictures no animation frame shows by itself (menu art built from parts,
    # like cutin00): export each mesh on its own.
    if not which:
        for m in range(len(_meshes_raw(b))):
            if m in covered or not mesh_quads(b, m):
                continue
            x0, y1, cw, ch = canvas(b, [m])
            box = (x0 - margin, y1 + margin, cw + 2 * margin, ch + 2 * margin)
            W, H, imgs = render(b, [m], pal, box)
            osb.write_png(os.path.join(outdir, "mesh_m%d_o%d_%d.png" % (m, box[0], box[1])), W, H, imgs[0])
            n += 1
    return n


def nearest_index(rgba, pal, cache):
    c = cache.get(rgba)
    if c is None:
        best, bd = 1, None
        for i in range(1, len(pal)):
            p = pal[i]
            dist = (p[0] - rgba[0]) ** 2 + (p[1] - rgba[1]) ** 2 + (p[2] - rgba[2]) ** 2
            if bd is None or dist < bd:
                best, bd = i, dist
        c = cache[rgba] = (best, bd)
    return c


def import_frames(b, pngs, pal, report=print, replace=False):
    """Write edited frame PNGs back into the atlas. Returns the new decoded file
    (or None if nothing changed). Colours are matched to the palette; a pixel
    whose colour didn't change keeps its exact palette index. pal=None means a
    full-colour sheet: pixels (including partial transparency) are stored as they are.

    replace=True: each PNG is a whole new picture for its frame. The frame drops its old
    tiles (other frames that shared them keep them) and gets new tiles of its own, so a
    redrawn frame never leaks into another one. Use it for drawings that don't follow the
    old outline; frames showing the same picture (same _m number) must match."""
    import re
    out = bytearray(b)
    tex = osb.textures(b)[0]
    TW, base = tex["w"], tex["ofs"]
    cache = {}
    changed = approx = outside = 0
    new_tiles = {}  # frame mesh -> [(dest x, dest y bottom, 16x16 tile pixels)]
    cleared = set()  # replace: meshes whose old tiles are dropped
    for path in pngs:
        m = re.search(FRAME_RE, os.path.basename(path))
        if not m:
            report("sprites: %s: name must end like _m348_o-41_92.png (keep the exported name)"
                   % os.path.basename(path))
            continue
        mesh, ox, oy = int(m.group(1)), int(m.group(2)), int(m.group(3))
        W, H, img = osb.read_png_rgba(path)
        if replace:
            if mesh in cleared:
                continue  # the same picture again (another animation showing it)
            cleared.add(mesh)
            new_tiles.setdefault(mesh, [])

        def opaque(a):
            if pal is None:
                return b[a + 3]
            return b[a + 3] and b[a + 2] < len(pal) and pal[b[a + 2]][3]
        # Tiles overlap: like render, the last opaque tile at a pixel is the
        # one you see, so that is the one an edit changes.
        layers = {}
        for xx, yy, so in ([] if replace else quad_pixels(mesh_quads(b, mesh), TW)):
            col, row = xx - ox, oy - 1 - yy
            if 0 <= col < W and 0 <= row < H:
                layers.setdefault((col, row), []).append(base + so)
        covered = set(layers)
        for (col, row), offs in layers.items():
            p = img[(row * W + col) * 4:(row * W + col) * 4 + 4]
            vis = [a for a in offs if opaque(a)]
            if p[3] < (1 if pal is None else 128):  # erased (or still empty): nothing may show here
                for a in vis:
                    if out[a:a + 4] != b"\0\0\0\0":
                        out[a:a + 4] = b"\0\0\0\0"
                        changed += 1
                continue
            owner = vis[-1] if vis else offs[-1]
            if pal is None:  # full colour: store the pixel as it is (BGRA)
                new = bytes((p[2], p[1], p[0], p[3]))
                if vis and b[owner:owner + 4] == new:
                    continue
            else:
                if vis and pal[b[owner + 2]][:3] == bytes(p[:3]):
                    continue  # unchanged colour: keep the exact index
                i, dist = nearest_index(bytes(p[:3]) + b"\xff", pal, cache)
                if dist:
                    approx += 1
                new = bytes((0, 0, i, 255))
            if out[owner:owner + 4] != new:
                out[owner:owner + 4] = new
                changed += 1
        # Painted pixels no existing tile covers go into new 16x16 tiles,
        # cut on a grid aligned to the canvas and added to this frame on top.
        blocks = {}
        for row in range(H):
            for col in range(W):
                if img[(row * W + col) * 4 + 3] >= (1 if pal is None else 128) and (col, row) not in covered:
                    blocks.setdefault((col // TILE, row // TILE), []).append((col, row))
                    outside += 1
        for (bc, br), pixels in sorted(blocks.items()):
            tile = bytearray(TILE * TILE * 4)
            for col, row in pixels:
                p = img[(row * W + col) * 4:(row * W + col) * 4 + 4]
                t = ((row - br * TILE) * TILE + (col - bc * TILE)) * 4
                if pal is None:
                    tile[t:t + 4] = bytes((p[2], p[1], p[0], p[3]))
                    continue
                i, dist = nearest_index(bytes(p[:3]) + b"\xff", pal, cache)
                if dist:
                    approx += 1
                tile[t:t + 4] = bytes((0, 0, i, 255))
            ax = ox + bc * TILE
            top = oy - br * TILE  # dest y just above the block's first row
            new_tiles.setdefault(mesh, []).append((ax, top - TILE, bytes(tile)))
    if approx:
        report("sprites: %d pixel(s) weren't palette colours and got the nearest one" % approx)
    if cleared:
        meshes = _meshes_raw(bytes(out))
        out = bytearray(rebuild_meshes(bytes(out), [(b"", b"") if k in cleared else mv
                                                    for k, mv in enumerate(meshes)]))
        report("sprites: %d frame picture(s) replaced whole" % len(cleared))
    if new_tiles:
        n = sum(len(v) for v in new_tiles.values())
        if not replace:
            report("sprites: %d painted pixel(s) outside the old tiles -> %d new tile(s) on %d frame(s)"
                   % (outside, n, len(new_tiles)))
        return add_tiles(bytes(out), new_tiles, report)
    return bytes(out) if changed else None


def strip(W, H, imgs, gap=4, bg=b"\xe8\xe8\xe8\xff"):
    n = len(imgs)
    SW = W * n + gap * (n - 1)
    out = bytearray(bg * (SW * H))
    for k, img in enumerate(imgs):
        for y in range(H):
            for x in range(W):
                o = (y * W + x) * 4
                if img[o + 3]:
                    d = (y * SW + k * (W + gap) + x) * 4
                    out[d:d + 4] = img[o:o + 4]
    return SW, H, bytes(out)


# ---------------------------------------------------------------------------
# New tiles: free atlas space, texture growth, mesh section rebuild.
TILE, PAD = 16, 1   # new tiles are 16x16 with a 1-pixel empty border around them


def _meshes_raw(b):
    """[(vertex bytes, index bytes)] for every frame mesh, in order."""
    sec = _u(b, 0x1C)
    out = []
    for m in range(_u(b, sec)):
        rec = sec + 4 + 0x18 * m
        _, _, vo, io, nv, ni = struct.unpack_from("<6I", b, rec)
        out.append((b[rec + vo:rec + vo + 20 * nv], b[rec + io:rec + io + 2 * ni]))
    return out


def rebuild_meshes(b, meshes):
    """Replace the mesh section (the last section of the file) with these meshes.
    Records, then all vertices, then all indices, as the game's files are laid out."""
    sec = _u(b, 0x1C)
    n = len(meshes)
    vstart = 4 + 0x18 * n
    istart = vstart + sum(len(v) for v, _ in meshes)
    blob = bytearray(struct.pack("<I", n))
    vo = io = 0
    for m, (v, ix) in enumerate(meshes):
        rec = 4 + 0x18 * m
        blob += struct.pack("<6I", 0x18, 0, vstart + vo - rec, istart + io - rec, len(v) // 20, len(ix) // 2)
        vo += len(v)
        io += len(ix)
    blob += b"".join(v for v, _ in meshes) + b"".join(ix for _, ix in meshes)
    out = bytearray(b[:sec]) + blob
    struct.pack_into("<I", out, 0x18, len(blob))           # [6] mesh section size
    for f in (20, 21, 22, 23):                             # [20..23] file size
        struct.pack_into("<I", out, 4 * f, len(out))
    return bytes(out)


def grow_texture(b, new_h):
    """Make the (only) texture taller. Rows are added at the bottom, so every
    v coordinate is rescaled; the mesh section moves down by the added bytes."""
    tex = osb.textures(b)[0]
    W, H = tex["w"], tex["h"]
    add = W * (new_h - H) * 4
    end = tex["ofs"] + W * H * 4
    sec = _u(b, 0x1C)
    assert end <= sec
    out = bytearray(b[:end]) + bytes(add) + bytearray(b[end:])
    struct.pack_into("<I", out, tex["hdr"] + 4, W * new_h * 4)   # data size
    struct.pack_into("<I", out, tex["hdr"] + 16, new_h)          # height
    struct.pack_into("<I", out, 0x1C, sec + add)                 # [7] mesh section offset
    for f in (20, 21, 22, 23):
        struct.pack_into("<I", out, 4 * f, len(out))
    meshes = []
    for v, ix in _meshes_raw(bytes(out)):
        v = bytearray(v)
        for k in range(len(v) // 20):
            vv = struct.unpack_from("<f", v, 20 * k + 16)[0]
            struct.pack_into("<f", v, 20 * k + 16, vv * H / new_h)
        meshes.append((bytes(v), ix))
    return rebuild_meshes(bytes(out), meshes)


def _occupied(b):
    """Atlas pixels any existing tile uses (with a 1-pixel border)."""
    tex = osb.textures(b)[0]
    W, H = tex["w"], tex["h"]
    used = bytearray(W * H)
    for v, _ in _meshes_raw(b):
        for q in range(len(v) // 80):
            uv = [struct.unpack_from("<2f", v, 80 * q + 20 * k + 12) for k in range(4)]
            x0 = max(0, int(min(a for a, _ in uv) * W) - PAD)
            x1 = min(W, int(round(max(a for a, _ in uv) * W)) + PAD)
            y0 = max(0, int(min(c for _, c in uv) * H) - PAD)
            y1 = min(H, int(round(max(c for _, c in uv) * H)) + PAD)
            for y in range(y0, y1):
                used[y * W + x0:y * W + x1] = b"\1" * (x1 - x0)
    return used, W, H


def _free_slots(used, W, H, y_from=0):
    """Top-left corners of free 16x16 tile spots on an 18-pixel grid."""
    step = TILE + 2 * PAD
    for cy in range(y_from, H - step + 1, step):
        for cx in range(0, W - step + 1, step):
            if not any(any(used[(cy + r) * W + cx:(cy + r) * W + cx + step]) for r in range(step)):
                yield cx + PAD, cy + PAD


def add_tiles(b, new_tiles, report=print, max_h=4096):
    """new_tiles: {frame mesh: [(dest x, dest y bottom, 16x16 tile pixels)]}.
    Identical tiles are stored once. Grows the texture if the atlas is full."""
    unique = {}
    for tiles in new_tiles.values():
        for _, _, px in tiles:
            unique.setdefault(px, None)
    used, W, H = _occupied(b)
    slots = list(_free_slots(used, W, H))
    while len(slots) < len(unique):
        if H * 2 > max_h:
            raise ValueError("not enough atlas space for %d new tiles" % len(unique))
        report("sprites: atlas full, growing the texture to %dx%d" % (W, H * 2))
        b = grow_texture(b, H * 2)
        used, W, H = _occupied(b)
        slots = list(_free_slots(used, W, H))
    tex = osb.textures(b)[0]
    out = bytearray(b)
    for (px, _), (tx, ty) in zip(list(unique.items()), slots):
        unique[px] = (tx, ty)
        for r in range(TILE):
            o = tex["ofs"] + ((ty + r) * W + tx) * 4
            out[o:o + TILE * 4] = px[r * TILE * 4:(r + 1) * TILE * 4]
    meshes = _meshes_raw(bytes(out))
    for mesh, tiles in new_tiles.items():
        v, ix = meshes[mesh]
        v, ix = bytearray(v), bytearray(ix)
        for ax, ay, px in tiles:
            tx, ty = unique[px]
            base = len(v) // 20
            bx, by = ax + TILE, ay + TILE
            for x, y, uu, vv in ((ax, ay, tx, ty + TILE), (ax, by, tx, ty), (bx, by, tx + TILE, ty),
                                 (bx, ay, tx + TILE, ty + TILE)):
                v += struct.pack("<5f", x, y, 0.0, uu / W, vv / H)
            ix += struct.pack("<6H", base, base + 1, base + 2, base, base + 2, base + 3)
        meshes[mesh] = (bytes(v), bytes(ix))
    return rebuild_meshes(bytes(out), meshes)


def set_track(b, anim_hash, node_hash, channel, value):
    """Set every key of one animation track (a node's channel) to value: e.g. the colour of an
    effect's light (channels 10/11/12 = red/green/blue, 0..1). Not for channel 0 (pictures).
    -> (new bytes, number of keys changed)."""
    hs, n = _u(b, 0), _u(b, 0x20)
    out = bytearray(b)
    changed = 0
    for i in range(n):
        _, h, _, _, _, nblk, blk = struct.unpack_from("<IIfIIII", b, hs + 0x1C * i)
        if h != anim_hash:
            continue
        for k in range(nblk):
            bo = blk + 0x48 * k
            ntr, to = _u(b, bo + 0x0C), _u(b, bo + 0x10)
            for _ in range(ntr):
                stride, nkey, chan = _u(b, to), _u(b, to + 4), _u(b, to + 8)
                if _u(b, bo + 4) == node_hash and chan == channel and chan != 0:
                    for j in range(nkey):
                        struct.pack_into("<f", out, to + 0x14 + stride * j + 8, value)
                        changed += 1
                to += 0x14 + stride * nkey
    return bytes(out), changed


def hitbox_table(b):
    """Per frame picture (mesh) collision: the table at header +0x28 has, for each mesh, two
    {u32 count, u32 offset} pairs: attack boxes, then hurt (body) boxes. A box is 5 int32:
    centre x, centre y (up from the feet), width, height, kind (0 = body; 10/11 seen on the
    Empress's sword strike). The game's collision uses the boxes of the picture being shown."""
    return _u(b, 0x28)


def hitboxes(b, mesh):
    """(attack boxes, hurt boxes) of a frame mesh, each a list of (x, y, w, h, kind)."""
    e = hitbox_table(b) + 16 * mesh
    out = []
    for k in range(2):
        n, o = struct.unpack_from("<II", b, e + 8 * k)
        out.append([struct.unpack_from("<5i", b, o + 20 * i) for i in range(n)])
    return tuple(out)


def set_attack_box(b, mesh, box, donor=None):
    """Sets a frame mesh's attack boxes to one box (x, y, w, h, kind), or none (box None). The
    box is written over the mesh's first attack box, so it needs at least one already, or a
    donor mesh whose first attack record it takes over (the donor deals no hit afterwards;
    several meshes may share one donor's record when they get the same box)."""
    e = hitbox_table(b) + 16 * mesh
    n, o = struct.unpack_from("<II", b, e)
    out = bytearray(b)
    if donor is not None and box is not None:
        de = hitbox_table(b) + 16 * donor
        dn, do = struct.unpack_from("<II", b, de)
        if dn < 1 and do == 0:
            raise ValueError("donor mesh %d has no attack record" % donor)
        struct.pack_into("<I", out, de, 0)          # the donor deals nothing now
        struct.pack_into("<II", out, e, 1, do)      # the mesh points at the donor's record
        struct.pack_into("<5i", out, do, *box)
        return bytes(out)
    if box is None:
        struct.pack_into("<I", out, e, 0)
        return bytes(out)
    if n < 1:
        raise ValueError("mesh %d has no attack box to replace (it needs one already)" % mesh)
    struct.pack_into("<5i", out, o, *box)
    struct.pack_into("<I", out, e, 1)
    return bytes(out)


def set_node_count(b, index, n):
    """Keeps only the first n nodes (blocks) of animation index: the entry's block count is
    lowered, the dropped blocks stay in the file unused. E.g. the Empress's tether animations
    carry an attach-point node that makes the game draw the tether."""
    hs = _u(b, 0)
    eo = hs + 0x1C * index
    have = _u(b, eo + 0x14)
    if not 1 <= n <= have:
        raise ValueError("animation %d has %d node(s); can't keep %d" % (index, have, n))
    out = bytearray(b)
    struct.pack_into("<I", out, eo + 0x14, n)
    return bytes(out)


def set_timing(b, index, length, times):
    """Animation <index>: set its length and the start times of its root picture keys (channel 0,
    without the -inf/+inf sentinels; the key just before +inf, if it only marks the end, moves to
    length). times: one per key in order. -> new bytes; raises ValueError on a count mismatch."""
    hs, n = _u(b, 0), _u(b, 0x20)
    if not 0 <= index < n:
        raise ValueError("no animation %d" % index)
    out = bytearray(b)
    eo = hs + 0x1C * index
    _, _, _, _, _, nblk, blk = struct.unpack_from("<IIfIIII", b, eo)
    struct.pack_into("<f", out, eo + 8, float(length))
    to = _u(b, blk + 0x10)
    for _ in range(_u(b, blk + 0x0C)):
        stride, nkey, chan = _u(b, to), _u(b, to + 4), _u(b, to + 8)
        if chan == 0:
            inner = [to + 0x14 + stride * j for j in range(nkey)
                     if abs(struct.unpack_from("<f", b, to + 0x14 + stride * j)[0]) < 1e30]
            end = None
            if len(inner) == len(times) + 1:   # a closing key at the old length
                end = inner.pop()
            if len(inner) != len(times):
                raise ValueError("animation %d has %d picture keys, %d times given" % (index, len(inner), len(times)))
            for ko, t in zip(inner, times):
                struct.pack_into("<f", out, ko, float(t))
            if end is not None:
                struct.pack_into("<f", out, end, float(length))
            return bytes(out)
        to += 0x14 + stride * nkey
    raise ValueError("animation %d has no picture track" % index)


def set_keys(b, index, length, keys):
    """Animation <index>: new length and a new root picture track. keys = [(time, k)], k = which
    of the animation's own picture keys (0-based, sentinels not counted) to show from that time;
    so only pictures the animation already has are used, as often as wanted. The new track (and
    the node's other tracks, copied) goes at the end of the key data (insert_tracks) and the
    node points there. -> new bytes."""
    hs, n = _u(b, 0), _u(b, 0x20)
    if not 0 <= index < n:
        raise ValueError("no animation %d" % index)
    eo = hs + 0x1C * index
    _, _, _, _, _, nblk, blk = struct.unpack_from("<IIfIIII", b, eo)
    ntr, to = _u(b, blk + 0x0C), _u(b, blk + 0x10)
    tracks = []
    for _ in range(ntr):
        stride, nkey, chan = _u(b, to), _u(b, to + 4), _u(b, to + 8)
        size = 0x14 + stride * nkey
        if chan == 0:
            raw = [bytes(b[to + 0x14 + stride * j:to + 0x14 + stride * (j + 1)]) for j in range(nkey)]
            inner = [k for k in raw if abs(struct.unpack_from("<f", k)[0]) < 1e30]
            first, last = raw[0], raw[-1]
            meshes = []
            for k in inner:                      # distinct pictures in key order
                m = _u(k, 8)
                if not meshes or meshes[-1] != m:
                    meshes.append(m)
            def key(tmpl, t, m):
                k = bytearray(tmpl)
                struct.pack_into("<f", k, 0, t)
                struct.pack_into("<I", k, 8, m)
                return bytes(k)
            new = [key(first, struct.unpack_from("<f", first)[0], meshes[keys[0][1]])]
            for t, k in keys:
                if not 0 <= k < len(meshes):
                    raise ValueError("animation %d has %d pictures, no picture %d" % (index, len(meshes), k))
                new.append(key(inner[0], t, meshes[k]))
            new.append(key(inner[0], length, meshes[keys[-1][1]]))
            new.append(key(last, struct.unpack_from("<f", last)[0], meshes[keys[-1][1]]))
            hdr = bytearray(b[to:to + 0x14])
            struct.pack_into("<I", hdr, 4, len(new))
            tracks.append(bytes(hdr) + b"".join(new))
        else:
            tracks.append(bytes(b[to:to + size]))
        to += size
    return insert_tracks(b, blk, eo, length, b"".join(tracks))


def insert_tracks(b, blk, eo, length, data):
    """Put track data at the end of the key-data section (where the part table starts, header
    [10]) and point node blk at it; entry eo gets the new length. The game ignores data appended
    after the mesh section, so it has to go here: everything after moves down, padded to 16 bytes
    to keep the texture's alignment. Moved pointers: header [7] mesh, [10] part table, [19]/[25]/
    [26] texture area, [20..23] file size, the part table's pair pointers, the texture header."""
    H = list(struct.unpack_from("<27I", b, 0))
    P0, P1 = H[10], H[19]
    data = bytes(data) + bytes(-len(data) % 16)
    S = len(data)
    texs = osb.textures(b)
    out = bytearray(b[:P0]) + data + bytearray(b[P0:])
    for o in range(P0 + S, P1 + S, 8):              # part table: (n, pointer) pairs, then records
        a, ptr = struct.unpack_from("<II", out, o)
        if not (a < 16 and P0 <= ptr < P1):
            break
        struct.pack_into("<I", out, o + 4, ptr + S)
    for t in texs:
        struct.pack_into("<I", out, t["hdr"] + S, t["ofs"] + S)
    for f in (7, 10, 19, 25, 26):
        if H[f] >= P0:
            H[f] += S
    H[20] = H[21] = H[22] = H[23] = len(out)
    struct.pack_into("<27I", out, 0, *H)
    struct.pack_into("<I", out, blk + 0x10, P0)
    struct.pack_into("<f", out, eo + 8, float(length))
    return bytes(out)


def rename_self_refs(b, old_asset, new_asset):
    """A copy of sprite file old_asset saved as new_asset: animation nodes that show an
    animation "from this file" name the file by hash (followed by the animation's hash),
    e.g. the Witch's dash pulls its pose from pl3_cb00.osb, the Oracle's from
    pl4_cb00.osb. Point them at the copy, or the copy keeps using the original (not
    loaded for an extra class: nothing is drawn, or the game crashes). Only the
    animation data (before header word 6, where the pictures start) is searched.
    old/new like "pl3_cb00.osb". -> (new bytes, count)."""
    import layout
    old = struct.pack("<I", layout.ice(old_asset))
    new = struct.pack("<I", layout.ice(new_asset))
    end = _u(b, 0x18)
    out = bytearray(b)
    n, i = 0, out.find(old, 0, end)
    while i >= 0:
        if i % 4 == 0:
            out[i:i + 4] = new
            n += 1
        i = out.find(old, i + 1, end)
    return bytes(out), n


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return
    asset, outdir = argv[1], argv[2]
    rest = argv[3:]
    rng = None
    if rest and "-" in rest[-1] and rest[-1].replace("-", "").isdigit():
        a, z = rest.pop().split("-")
        rng = range(int(a), int(z) + 1)
    pal = None
    if rest:
        pals = osb.palettes(load(rest[0]))
        pal = pals[int(rest[1]) if len(rest) > 1 else 0]
    b = load(asset)
    os.makedirs(outdir, exist_ok=True)
    for a in animations(b):
        if rng and a["index"] not in rng:
            continue
        fr = frames(a)
        if not fr:
            continue
        W, H, imgs = render(b, [m for _, m in fr], pal)
        SW, SH, img = strip(W, H, imgs)
        fn = os.path.join(outdir, "anim%03d_%08x_%dframes.png" % (a["index"], a["hash"], len(fr)))
        osb.write_png(fn, SW, SH, img)
        print(fn, "length %g, keys at %s" % (a["length"], [t for t, _ in fr]))


if __name__ == "__main__":
    main(sys.argv)
