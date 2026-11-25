"""Makes manager/art/icon.ico (the Mod Manager's program icon) from icon_source.png.

The D is found (black reached from the edges is background), it's cropped square with a little margin, shrunk smoothly to the usual
icon sizes, and those are packed into one .ico (PNG entries, Windows Vista and later).
usage: python -I manager/art/make_icon.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import osb  # noqa: E402  (PNG reading and writing)

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def transparent_background(w, h, px, limit=40):
    """Black-ish pixels connected to the border -> fully transparent."""
    def dark(i):
        return px[i] < limit and px[i + 1] < limit and px[i + 2] < limit
    seen = bytearray(w * h)
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h or seen[y * w + x]:
            continue
        i = 4 * (y * w + x)
        if not dark(i):
            continue
        seen[y * w + x] = 1
        px[i + 3] = 0
        stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]


def crop_square(w, h, px, margin=0.06):
    xs = [x for y in range(h) for x in range(w) if px[4 * (y * w + x) + 3]]
    ys = [y for y in range(h) for x in range(w) if px[4 * (y * w + x) + 3]]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    side = int(max(x1 - x0, y1 - y0) * (1 + 2 * margin))
    ox, oy = x0 - (side - (x1 - x0)) // 2, y0 - (side - (y1 - y0)) // 2
    out = bytearray(side * side * 4)
    for y in range(side):
        for x in range(side):
            sx, sy = ox + x, oy + y
            if 0 <= sx < w and 0 <= sy < h:
                out[4 * (y * side + x):4 * (y * side + x) + 4] = px[4 * (sy * w + sx):4 * (sy * w + sx) + 4]
    return side, out


def shrink(side, px, n):
    """Area-average resize side x side -> n x n (color weighted by alpha)."""
    out = bytearray(n * n * 4)
    scale = side / n
    for y in range(n):
        y0, y1 = y * scale, (y + 1) * scale
        for x in range(n):
            x0, x1 = x * scale, (x + 1) * scale
            acc = [0.0, 0.0, 0.0, 0.0]
            total = 0.0
            for sy in range(int(y0), min(side, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(side, int(x1) + 1)):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0:
                        continue
                    wgt = wx * wy
                    i = 4 * (sy * side + sx)
                    a = px[i + 3] / 255.0
                    acc[0] += px[i] * a * wgt
                    acc[1] += px[i + 1] * a * wgt
                    acc[2] += px[i + 2] * a * wgt
                    acc[3] += a * wgt
                    total += wgt
            o = 4 * (y * n + x)
            if acc[3] > 0:
                out[o:o + 3] = bytes(min(255, int(round(c / acc[3]))) for c in acc[:3])
            out[o + 3] = min(255, int(round(255 * acc[3] / total)))
    return out


def main():
    w, h, px = osb.read_png_rgba(os.path.join(HERE, "icon_source.png"))
    px = bytearray(px)
    for y in range(h):  # the source's last two columns are a gray screenshot edge, not the drawing
        for x in range(w - 2, w):
            px[4 * (y * w + x):4 * (y * w + x) + 4] = bytes(4)
    # keep the black background (it reads better on any desktop); only find the D for cropping
    probe = bytearray(px)
    transparent_background(w, h, probe)
    side, sq = crop_square(w, h, probe)
    for i in range(0, len(sq), 4):  # the background (and the margin) black and solid
        if sq[i + 3] == 0:
            sq[i:i + 4] = bytes((0, 0, 0, 255))
    pngs = []
    for n in SIZES:
        p = os.path.join(HERE, "icon_%d.png" % n)
        osb.write_png(p, n, n, bytes(shrink(side, sq, n)))
        pngs.append((n, open(p, "rb").read()))
    # .ico: header, one directory entry per size, then the PNG data
    out = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    data = b""
    for n, b in pngs:
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(b), offset + len(data))
        data += b
    with open(os.path.join(HERE, "icon.ico"), "wb") as f:
        f.write(out + data)
    print("wrote icon.ico (%s)" % ", ".join(str(n) for n, _ in pngs))


if __name__ == "__main__":
    main()
