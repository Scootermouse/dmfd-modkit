"""Screen layouts inside sprite files: list and move named parts.

Menu screens (e.g. Win/s_o0a00.osb, the character select screen) are
animations whose child nodes are named parts (PL1..PL6 = class panels,
PL1_Lv.. = their level labels, bg, cursor...). Each node has tracks:
    channel 0 = picture (mesh number), 1/2 = scale x/y, 7/8 = position x/y
(screen units, y up, origin at the screen centre; see FORMAT.md).
Keys are 20 bytes {f32 time, ?, value, ?, ?}; value is a u32 for channel 0
and an f32 otherwise. Edits overwrite every key of a track, in place.

usage: python -I layout.py Win/s_o0a00.osb          (prints all parts)
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sprites  # noqa: E402

CHANNELS = {0: "picture", 1: "scale_x", 2: "scale_y", 7: "x", 8: "y"}
FIELD_CHANNEL = {v: k for k, v in CHANNELS.items()}


def ice(s, h=0xCDE723A5):
    for c in s.encode():
        h = ((h + (c - 256 if c > 127 else c)) * 0x8D) & 0xFFFFFFFF
    return h


_names = None


def names():
    """hash -> name, from every short string in the game exe plus PL1..PL9 patterns."""
    global _names
    if _names is None:
        import os
        import dmfd_data
        exe = os.path.join(os.path.dirname(dmfd_data.DEFAULT_DH), "game_unpacked.exe")
        _names = {}
        try:
            data = open(exe, "rb").read()
            for m in set(re.findall(rb"[\x21-\x7e]{1,40}(?=\x00)", data)):
                _names.setdefault(ice(m.decode()), m.decode())
        except OSError:
            pass
        for i in range(1, 10):
            for s in ("PL%d" % i, "PL%d_Lv" % i):
                _names[ice(s)] = s
    return _names


def tracks(b):
    """[(anim index, anim hash, node hash, channel, [key value offsets], first value)]."""
    hs, n = sprites._u(b, 0), sprites._u(b, 0x20)
    out = []
    for i in range(n):
        _, h, _, _, _, nblk, blk = struct.unpack_from("<IIfIIII", b, hs + 0x1C * i)
        for k in range(nblk):
            bo = blk + 0x48 * k
            name = sprites._u(b, bo + 4)
            ntr, to = sprites._u(b, bo + 0x0C), sprites._u(b, bo + 0x10)
            for _ in range(ntr):
                stride, nkey, chan = sprites._u(b, to), sprites._u(b, to + 4), sprites._u(b, to + 8)
                offs = [to + 0x14 + stride * j + 8 for j in range(nkey)]
                first = (sprites._u(b, offs[-1]) if chan == 0 else struct.unpack_from("<f", b, offs[-1])[0]) \
                    if offs else None
                out.append((i, h, name, chan, offs, first))
                to += 0x14 + stride * nkey
    return out


def parts(b):
    """{(anim index, node name): {field: value}} for named child parts."""
    nm = names()
    res = {}
    for i, h, node, chan, offs, val in tracks(b):
        if node == 0x8D154D2D or chan not in CHANNELS:
            continue
        res.setdefault((i, nm.get(node, "%08x" % node)), {})[CHANNELS[chan]] = val
    return res


def apply(b, edits, report=print):
    """edits: [(anim index or None, part name, field, value)] -> new bytes (or None)."""
    nm = names()
    out = bytearray(b)
    changed = 0
    tr = tracks(b)
    for anim, part, field, value in edits:
        chan = FIELD_CHANNEL[field]
        hit = 0
        for i, h, node, c, offs, _ in tr:
            if c != chan or nm.get(node, "%08x" % node) != part or (anim is not None and i != anim):
                continue
            hit += 1
            for o in offs:
                if chan == 0:
                    struct.pack_into("<I", out, o, int(value))
                else:
                    struct.pack_into("<f", out, o, float(value))
                changed += 1
        if not hit:
            report("layout: no part %s with %s%s" % (part, field, "" if anim is None else " in animation %d" % anim))
    return bytes(out) if bytes(out) != b else None


def main(argv):
    b = sprites.load(argv[1])
    for (i, part), f in sorted(parts(b).items()):
        print("anim %3d  %-14s %s" % (i, part, "  ".join("%s=%g" % (k, v) for k, v in sorted(f.items()))))


if __name__ == "__main__":
    main(sys.argv)
