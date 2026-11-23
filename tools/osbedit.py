"""Structural editing of sprite files (.osb): add pictures (meshes) and animations.

Layout (offsets in the 0x6C-byte header, u32 [i] = header word i):
    header | tree n*0x1C | blocks (file order kept) | key chunks (+pad 8) | part table |
    texture | meshes
  [6] mesh section size, [7] mesh offset, [8] entries, [9] parts (= meshes),
  [10] part table, [19] texture, [20..23] file size, [25]/[26] texture (or a place in the part table).
Tree entry {0x1C, hash, f32 length, left, right, node count, first block}: the
animations, a binary tree by hash. Block (0x48) = one node: +4 name hash, +8
parent hash, +0xC track count, +0x10 first track (absolute). A node's tracks are
contiguous: {stride, nkeys, channel, ...} header 0x14 then nkeys*stride bytes.
Part table: 16 bytes per mesh (two 8-byte slots; a slot's second word, when
non-zero, is an absolute texture offset).

parse() + build() with no changes gives the identical file (checked for
s_o0a00, com_itic and a body sheet). add_mesh() adds an empty picture
(fill it with sprites.add_tiles), clone_anim() copies an animation under a new
name, changing node tracks and adding nodes.
"""
import struct

import osb


def _u(b, o):
    return struct.unpack_from("<I", b, o)[0]


def parse(b):
    H = list(struct.unpack_from("<27I", b, 0))
    hs, n, parts = H[0], H[8], H[9]
    if hs != 0x6C or H[20] != len(b) or H[7] + H[6] != len(b):
        raise ValueError("not a sprite file this editor understands")
    tree0, blocks0 = hs, hs + 0x1C * n
    anims = []
    for i in range(n):
        _, h, length, _, _, cnt, blk = struct.unpack_from("<IIfIIII", b, tree0 + 0x1C * i)
        anims.append({"hash": h, "length": length, "first": (blk - blocks0) // 0x48, "count": cnt})
    nblocks = max(a["first"] + a["count"] for a in anims)
    keys0 = blocks0 + 0x48 * nblocks
    blocks = []
    for k in range(nblocks):
        blk = bytearray(b[blocks0 + 0x48 * k:blocks0 + 0x48 * (k + 1)])
        ntr, to = _u(blk, 0xC), _u(blk, 0x10)
        tracks = bytearray()
        o = to
        for _ in range(ntr):
            stride, nkey = _u(b, o), _u(b, o + 4)
            tracks += b[o:o + 0x14 + stride * nkey]
            o += 0x14 + stride * nkey
        blocks.append({"block": blk, "tracks": bytes(tracks), "key_at": to})
    mesh_sec = H[7]
    nm = _u(b, mesh_sec)
    meshes = []
    for m in range(nm):
        rec = mesh_sec + 4 + 0x18 * m
        _, _, vo, io, nv, ni = struct.unpack_from("<6I", b, rec)
        meshes.append((b[rec + vo:rec + vo + 20 * nv], b[rec + io:rec + io + 2 * ni]))
    return {"H": H, "anims": anims, "blocks": blocks, "keys": b[keys0:H[10]], "keys0": keys0,
            "part": bytearray(b[H[10]:H[19]]), "tex": bytearray(b[H[19]:H[7]]), "meshes": meshes,
            "head": b[:hs], "orig": b}


def _bst(order):
    """Pre-order balanced tree over a sorted index list: [(index, left, right)] with -1 for none."""
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
    return [tuple(x) for x in out]


def build(m):
    """Serialize. Unchanged models reuse the original bytes for the key region."""
    b, H = m["orig"], list(m["H"])
    hs = H[0]
    n = len(m["anims"])
    nb = len(m["blocks"])
    tree0 = hs
    blocks1 = tree0 + 0x1C * n
    keys1 = blocks1 + 0x48 * nb
    # key chunks: original region (shifted) followed by tracks of new blocks
    old_keys0 = m["keys0"]
    shift = keys1 - old_keys0
    keys = bytearray(m["keys"])
    orig_len = len(keys)
    new_tracks_at = None
    for blk in m["blocks"]:
        if blk.get("new"):
            if new_tracks_at is None:
                while len(keys) % 4:
                    keys += b"\0"
                new_tracks_at = len(keys)
            blk["key_at_new"] = keys1 + len(keys)
            keys += blk["tracks"]
    keys += bytes((-(keys1 + len(keys))) % 8) if new_tracks_at is not None else b""
    blocks = bytearray()
    for blk in m["blocks"]:
        bb = bytearray(blk["block"])
        if blk.get("new"):
            struct.pack_into("<I", bb, 0x10, blk["key_at_new"])
        else:
            struct.pack_into("<I", bb, 0x10, blk["key_at"] + shift)
        blocks += bb
    part1 = keys1 + len(keys)
    part = bytearray(m["part"])
    tex1 = part1 + len(part)
    tex1 += (H[19] - tex1) % 16  # keep the texture's 16-byte phase
    tex_shift = tex1 - H[19]
    for o in range(0, len(part) - 7, 8):
        if _u(part, o + 4):
            struct.pack_into("<I", part, o + 4, _u(part, o + 4) + tex_shift)
    tex = bytearray(m["tex"])
    for t in osb.textures(b):
        struct.pack_into("<I", tex, t["hdr"] - H[19], t["ofs"] + tex_shift)
    mesh1 = tex1 + len(tex)
    mesh1 += (H[7] - mesh1) % 16
    meshes = m["meshes"]
    vstart = 4 + 0x18 * len(meshes)
    istart = vstart + sum(len(v) for v, _ in meshes)
    mesh_blob = bytearray(struct.pack("<I", len(meshes)))
    vo = io = 0
    for k, (v, ix) in enumerate(meshes):
        rec = 4 + 0x18 * k
        mesh_blob += struct.pack("<6I", 0x18, 0, vstart + vo - rec, istart + io - rec, len(v) // 20, len(ix) // 2)
        vo += len(v)
        io += len(ix)
    mesh_blob += b"".join(v for v, _ in meshes) + b"".join(ix for _, ix in meshes)
    tree = bytearray()
    order = sorted(range(n), key=lambda i: m["anims"][i]["hash"])
    for i, l, r in _bst(order):
        a = m["anims"][i]
        tree += struct.pack("<IIfIIII", 0x1C, a["hash"], a["length"], l, r, a["count"], blocks1 + 0x48 * a["first"])
    out = bytearray(m["head"]) + tree + blocks + keys + part
    out += bytes(tex1 - len(out)) + tex
    out += bytes(mesh1 - len(out)) + mesh_blob
    H[6], H[7], H[8], H[9], H[10], H[19] = len(mesh_blob), mesh1, n, len(meshes), part1, tex1
    H[20] = H[21] = H[22] = H[23] = len(out)
    for w in (25, 26):  # texture offset, or (body sheets) a place in the part table
        o = m["H"][w]
        if m["H"][10] <= o < m["H"][19]:
            H[w] = o + (part1 - m["H"][10])
        elif o == m["H"][19]:
            H[w] = tex1
    struct.pack_into("<27I", out, 0, *H)
    return bytes(out)


def find_anim(m, h):
    for i, a in enumerate(m["anims"]):
        if a["hash"] == h:
            return i
    return -1


def add_mesh(m):
    """A new, empty picture; returns its number (fill it with sprites.add_tiles)."""
    m["meshes"].append((b"", b""))
    m["part"] += m["part"][:16]
    return len(m["meshes"]) - 1


def set_track(tracks, chan, value):
    """Copy of a node's tracks with every key of `chan` set to value (int for 0, float otherwise)."""
    t = bytearray(tracks)
    o = 0
    hit = False
    while o < len(t):
        stride, nkey, c = struct.unpack_from("<3I", t, o)
        if c == chan:
            for j in range(nkey):
                ko = o + 0x14 + stride * j + 8
                if chan == 0:
                    struct.pack_into("<I", t, ko, int(value))
                else:
                    struct.pack_into("<f", t, ko, float(value))
            hit = True
        o += 0x14 + stride * nkey
    if not hit:
        raise ValueError("node has no track for channel %d" % chan)
    return bytes(t)


def clone_anim(m, src_hash, new_hash, changes=None, extra_nodes=()):
    """Copy animation src_hash as new_hash. changes = {node name hash: {channel: value}};
    extra_nodes = [(template node name hash, new name hash, {channel: value})], added at the end."""
    i = find_anim(m, src_hash)
    if i < 0:
        raise ValueError("no animation %08x" % src_hash)
    a = m["anims"][i]
    first = len(m["blocks"])
    srcs = m["blocks"][a["first"]:a["first"] + a["count"]]
    for blk in srcs:
        name = _u(blk["block"], 4)
        tracks = blk["tracks"]
        for chan, val in (changes or {}).get(name, {}).items():
            tracks = set_track(tracks, chan, val)
        m["blocks"].append({"block": bytearray(blk["block"]), "tracks": tracks, "new": True})
    for tpl, name, vals in extra_nodes:
        src = next(x for x in srcs if _u(x["block"], 4) == tpl)
        bb = bytearray(src["block"])
        struct.pack_into("<I", bb, 4, name)
        tracks = src["tracks"]
        for chan, val in vals.items():
            tracks = set_track(tracks, chan, val)
        m["blocks"].append({"block": bb, "tracks": tracks, "new": True})
    m["anims"].append({"hash": new_hash, "length": a["length"], "first": first,
                       "count": a["count"] + len(extra_nodes)})
    return len(m["anims"]) - 1


def track_value(tracks, chan):
    """First key's value of `chan` in a node's tracks (int for 0, float otherwise)."""
    o = 0
    while o < len(tracks):
        stride, nkey, c = struct.unpack_from("<3I", tracks, o)
        if c == chan:
            return struct.unpack_from("<I" if chan == 0 else "<f", tracks, o + 0x14 + 8)[0]
        o += 0x14 + stride * nkey
    raise ValueError("node has no track for channel %d" % chan)


def add_nodes(m, anim_hash, extra_nodes):
    """Add nodes to an existing animation (same name): it is copied with the new nodes
    and the copy replaces it."""
    old = find_anim(m, anim_hash)
    clone_anim(m, anim_hash, anim_hash, extra_nodes=extra_nodes)
    del m["anims"][old]
