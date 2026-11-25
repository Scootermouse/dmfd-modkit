"""Read Dragon Marked for Death's SystemData.bin and the content mod's SystemData.bin.classes (read-only).

File: 16-byte header {u32 magic 0xae9f2304, u32 0x68, u32 checksum of the plain
data, u32 checksum of the encrypted data}, then the data from 0x10 on, encrypted
twice with the data-file cipher (tools/dmfd_data.crypt): first with "zZ2c9VTK" +
account, then "gYjkJoTX" + account (account = Steam account ID in lowercase hex,
the number in userdata\\<id>\\). Checksum = the game's 16-bit folded byte sum
(0xBD3B80). In memory the game keeps exactly this layout; all offsets below count
from the start of the file.

Header: [0x1C] class record size 0x5C, [0x34] contract slot size 0xC, [0x38]
contract slots at 0x68, [0x3C] 3 contracts, [0x40] (byte, in memory) current
contract. Slot = three section offsets {A, B, C}:
  A: byte 2 = "created" flags (bit per class), byte 3 = active class
  B: six 0x5C-byte class records (class order), then their data blocks. A record:
     +0x00 name (UTF-8, full width), +0x34..+0x58 ten offsets relative to the record
     (field 2 is a number): +0x34 setup block, then eight more per-class blocks.
  C: inventory (0x800 x 8 bytes).
Setup block (0x34): +0 class, +1 color, +2 voice, +3 ?, +4 level, ..., +0x14.. four
equipped items {u32 label hash, u32 equipped flag}. The game's validator (0xC928A0)
clamps class <= 5, color/voice <= 7, level 1..100.
Six records and their blocks fill section B: there is no room for a seventh class,
which is why the content mod keeps extra classes in SystemData.bin.classes:
  {u32 'DMCX', u32 3, u32 contracts, u32 count} then per class {u32 class, per
  contract {u32 length, bytes}}: the class's data in its panel class's byte ranges
  (name area 0x34, field 2, then the nine blocks in field order).

usage: python -I save.py "D:\\...\\userdata\\<id>\\1149440\\local\\SystemData.bin"
This never writes to the save.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmfd_data  # noqa: E402

MAGIC = 0xAE9F2304
CLASSES = ["Empress", "Warrior", "Shinobi", "Witch", "Oracle", "Bandit"]
REC = 0x5C
OFFSET_FIELDS = [0, 1, 3, 4, 5, 6, 7, 8, 9]


def checksum(data):
    c = 0
    for x in data:
        c += x
        c = ((c >> 16) | c) & 0xFFFF
    return c


def account_from_path(path):
    m = re.search(r"userdata[\\/](\d+)[\\/]", path)
    return int(m.group(1)) if m else None


def decrypt(raw, account):
    """-> the whole file decrypted (header kept), so offsets match the game's."""
    magic, hsize, plain_sum, enc_sum = struct.unpack_from("<4I", raw, 0)
    if magic != MAGIC:
        raise ValueError("not a DMFD save (magic %#x)" % magic)
    if checksum(raw[0x10:]) != enc_sum:
        raise ValueError("encrypted checksum doesn't match (damaged file?)")
    acct = ("%x" % account).encode()
    plain = dmfd_data.crypt(dmfd_data.crypt(raw[0x10:], b"gYjkJoTX" + acct), b"zZ2c9VTK" + acct)
    if checksum(plain) != plain_sum:
        raise ValueError("wrong account ID: the decrypted checksum doesn't match")
    return raw[:0x10] + plain


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def class_zero_ranges(m, B, bsize):
    """Class 0's byte ranges in section B (offsets within B): what an extra class swaps."""
    starts = []
    for k in range(6):
        starts.append(k * REC)
        starts += [k * REC + u32(m, B + k * REC + 0x34 + 4 * f) for f in OFFSET_FIELDS]
    starts.append(bsize)
    out = [(0, 0x34), (0x3C, 4)]
    for f in OFFSET_FIELDS:
        a = u32(m, B + 0x34 + 4 * f)
        out.append((a, min([x for x in starts if x > a] + [bsize]) - a))
    return out


def contracts(m):
    out = []
    for c in range(u32(m, 0x3C)):
        A, B, C = (u32(m, u32(m, 0x38) + u32(m, 0x34) * c + 4 * i) for i in range(3))
        recs = []
        for k in range(6):
            r = B + k * u32(m, 0x1C)
            s = r + u32(m, r + 0x34)
            name = m[r:r + 0x34].split(b"\0")[0].decode("utf-8", "replace")
            recs.append({"name": name, "class": m[s], "color": m[s + 1], "voice": m[s + 2], "level": m[s + 4],
                         "items": [struct.unpack_from("<II", m, s + o) for o in (0x14, 0x1C, 0x24, 0x2C)]})
        out.append({"number": c + 1, "A": A, "B": B, "C": C, "created": m[A + 2], "active": m[A + 3], "classes": recs})
    return out


def read_classes_file(path, m):
    """SystemData.bin.classes (format 3 or 4) -> {class: {contract: setup info}} (None if absent).

    {u32 'DMCX', u32 3 or 4, u32 contracts, u32 count} then per class {u32 class, per contract
    {u32 length, (version 4, when length > 0: u32 flags, bit 0 created), bytes}}; the bytes are the class's data laid out like its panel's class 0..5
    ranges (name area 0x34, field 2, then the nine blocks; the setup block is at +0x38)."""
    if not os.path.exists(path):
        return None
    d = open(path, "rb").read()
    magic, ver, ncon, count = struct.unpack_from("<4I", d, 0)
    if magic != 0x58434D44 or ver not in (3, 4):
        return None
    o, out = 16, {}
    for _ in range(count):
        cls = u32(d, o)
        o += 4
        per = {}
        for c in range(ncon):
            size = u32(d, o)
            o += 4
            created = True
            if size and ver >= 4:  # version 4: u32 flags (bit 0 = made through character creation)
                created = bool(u32(d, o) & 1)
                o += 4
            blob = d[o:o + size]
            o += size
            if size:
                s = blob[0x38:0x38 + 0x34]
                per[c + 1] = {"class": s[0], "color": s[1], "voice": s[2], "level": s[4], "created": created,
                              "items": [struct.unpack_from("<II", s, x) for x in (0x14, 0x1C, 0x24, 0x2C)]}
        out[cls] = per
    return out


def main(argv):
    path = argv[1]
    acct = int(argv[2]) if len(argv) > 2 else account_from_path(path)
    m = decrypt(open(path, "rb").read(), acct)
    print("decrypted %d bytes; checksums match" % len(m))
    extra = read_classes_file(path + ".classes", m)
    for con in contracts(m):
        print("Contract %d: active class %d, created flags %#x" % (con["number"], con["active"], con["created"]))
        for name, r in zip(CLASSES, con["classes"]):
            eq = sum(1 for _, f in r["items"] if f)
            print("   %-8s class %d  Lv %3d  color %d voice %d  %d item(s) equipped" %
                  (name, r["class"], r["level"], r["color"], r["voice"], eq))
        for cls, per in sorted((extra or {}).items()):
            if con["number"] in per:
                r = per[con["number"]]
                eq = sum(1 for _, f in r["items"] if f)
                print("   class %-3d (SystemData.bin.classes) Lv %3d  color %d voice %d  %d item(s) equipped" %
                      (cls, r["level"], r["color"], r["voice"], eq))


if __name__ == "__main__":
    main(sys.argv)
