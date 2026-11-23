"""Ice engine .osb sprite bank (decoded form) -> PNG textures.

Only the texture blocks are understood so far.  Each texture is introduced by
a 24-byte header that sits immediately before its pixels:
    u32 dataOfs (== header_ofs + 24), u32 dataSize, u32 hash,
    u32 width, u32 height, u32 0
dataSize == width*height*4: uncompressed 8-bit RGBA, rows top-down.
Character sheets are paletted: pixel bytes are (0, 0, index, alpha); the
palette lives in a separate Win/plN_pal.osb (or *_cw??_pal.osb for weapons):
    u32 hash, -1, -1, 0x34, 0x14, 4, 0, nPal, 0, nCol, 0x34, nPal, 0x34+nPal*nCol*4
    then nPal palettes of nCol BGRA colors (index 0 transparent); palettes()
    returns them as RGBA.
The rest of the file (header 0x6C/0x38/0x60 bytes, part/cell/animation tables)
is not decoded yet.

usage: python -I osb.py <decoded.osb> <outdir>
       python -I osb.py --asset Win/pl0_cw00.osb <outdir> [Win/pl0_cw00_pal.osb [palIndex]]
"""
import os
import struct
import sys
import zlib


def textures(b):
    out = []
    o = 0
    n = len(b)
    while o + 24 <= n:
        ofs, sz, hh, w, h, z = struct.unpack_from("<6I", b, o)
        if ofs == o + 24 and 0 < w <= 8192 and 0 < h <= 8192 and sz == w * h * 4 \
                and ofs + sz <= n:
            out.append({"hdr": o, "ofs": ofs, "w": w, "h": h, "hash": hh,
                        "flags": z})
            o = ofs + sz
            continue
        o += 4
    return out


def palettes(b):
    """Return list of palettes (each a list of 4-byte RGBA colors)."""
    out = []
    o = 0
    while True:
        o = b.find(b"\xff" * 8, o)
        if o < 4:
            if o < 0:
                return out
            o += 4
            continue
        if o + 48 > len(b):
            return out
        # after hash,-1,-1: 0x34, 0x14, 4, 0, nPal, 0, nCol, 0x34, nPal', size
        # (nPal' can be one less than nPal, e.g. the Oracle's 49 palettes; the size decides)
        v = struct.unpack_from("<10I", b, o + 8)
        if v[0] == 0x34 and v[1] == 0x14 and v[4] and v[6] \
                and v[9] == 0x34 + v[4] * v[6] * 4:
            npal, ncol = v[4], v[6]
            base = o - 4 + 0x34
            for p in range(npal):
                q = base + p * ncol * 4
                out.append([bytes((b[q + 4 * k + 2], b[q + 4 * k + 1], b[q + 4 * k], b[q + 4 * k + 3]))
                            for k in range(ncol)])  # stored BGRA
            o = base + npal * ncol * 4
        else:
            o += 4


def apply_palette(px, pal):
    out = bytearray(len(px))
    n = len(pal)
    for k in range(0, len(px), 4):
        if px[k + 3]:
            i = px[k + 2]
            out[k:k + 4] = pal[i] if i < n else b"\xff\x00\xff\xff"
    return bytes(out)


def write_png(fn, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(fn, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def read_png_rgba(fn):
    """Minimal reader for 8-bit RGBA, non-interlaced PNGs (what write_png emits)."""
    d = open(fn, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    o, idat, w = 8, b"", 0
    while o < len(d):
        ln, t = struct.unpack_from(">I4s", d, o)
        body = d[o + 8:o + 8 + ln]
        if t == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            assert (bd, ct, il) == (8, 6, 0), "need 8-bit RGBA non-interlaced"
        elif t == b"IDAT":
            idat += body
        o += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            up = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + up) & 255
            elif ft == 3:
                line[i] = (line[i] + ((a + up) >> 1)) & 255
            elif ft == 4:
                pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                pr = a if pa <= pb and pa <= pc else (up if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    return w, h, bytes(out)


def main(argv):
    pal = None
    if argv[1] == "--asset":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dmfd_data

        def load(n):
            raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(n)), "rb").read()
            return dmfd_data.decode(raw)[0]
        name, outdir = argv[2], argv[3]
        b = load(name)
        stem = name.replace("/", "_")
        if len(argv) > 4:
            pals = palettes(load(argv[4]))
            pal = pals[int(argv[5]) if len(argv) > 5 else 0]
    else:
        b = open(argv[1], "rb").read()
        outdir = argv[2]
        stem = os.path.basename(argv[1])
    os.makedirs(outdir, exist_ok=True)
    for i, t in enumerate(textures(b)):
        fn = os.path.join(outdir, "%s_tex%d_%dx%d.png" % (stem, i, t["w"], t["h"]))
        px = b[t["ofs"]:t["ofs"] + t["w"] * t["h"] * 4]
        if pal:
            px = apply_palette(px, pal)
        write_png(fn, t["w"], t["h"], px)
        print(fn)


if __name__ == "__main__":
    main(sys.argv)
