"""Character names in a series of save backups (the Mod Manager's ModManager\\save backups\\*.zip),
oldest first, to find when names changed or went blank. Read-only.

usage: python -I tools/save_names.py <backup folder, zips or SystemData.bin files...> <Steam account ID>
       (the account ID is the number in ...\\userdata\\<id>\\1149440\\local)
"""
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import save  # noqa: E402

SLOTS = ["Emp", "War", "Shi", "Wit", "Ora", "Ban"]


def text(b):
    """A name as the save keeps it: UTF-8 (full-width letters), NUL-terminated."""
    return b.split(b"\0")[0].decode("utf-8", "replace")


def sidecar_names(d):
    """SystemData.bin.classes (version 5): {class id: {contract: (name, created)}}."""
    out = {}
    if not d or len(d) < 16:
        return out
    magic, ver, ncon, count = struct.unpack_from("<4I", d, 0)
    if magic != 0x58434D44 or ver != 5:
        return {"(format %d)" % ver: {}}
    o = 16
    for _ in range(count):
        n = struct.unpack_from("<I", d, o)[0]
        cid = d[o + 4:o + 4 + n].decode("utf-8", "replace")
        o += 4 + n
        per = {}
        for c in range(ncon):
            size = struct.unpack_from("<I", d, o)[0]
            o += 4
            if size:
                flags = struct.unpack_from("<I", d, o)[0]
                blob = d[o + 4:o + 4 + size]
                o += 4 + size
                per[c + 1] = (blob[:0x34].split(b"\0")[0].decode("utf-8", "replace"), bool(flags & 1))
        out[cid] = per
    return out


def show(label, main, classes, account):
    m = save.decrypt(main, account)
    print("== " + label)
    for con in save.contracts(m):
        if not con["created"]:
            continue
        cells = []
        for k, r in enumerate(con["classes"]):
            made = con["created"] >> k & 1
            name = text(m[con["B"] + k * save.u32(m, 0x1C):][:0x34]) or ("(BLANK)" if made else "-")
            cells.append("%s %s%s" % (SLOTS[k], name, "" if r["class"] == k else " [holds class %d]" % r["class"]))
        print("  contract %d (active %d): %s" % (con["number"], con["active"], " | ".join(cells)))
        extras = ["%s %s%s" % (cid, (per[con["number"]][0] or "(BLANK)"), "" if per[con["number"]][1] else " NEW")
                  for cid, per in sorted(sidecar_names(classes).items()) if con["number"] in per]
        if extras:
            print("     extras: " + ", ".join(extras))


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")
    account = int(argv[-1])
    items = []
    for a in argv[1:-1]:
        if os.path.isdir(a):
            items += [os.path.join(a, f) for f in os.listdir(a) if f.lower().endswith(".zip")]
        else:
            items.append(a)
    for path in sorted(items, key=lambda p: os.path.basename(p)):
        try:
            if path.lower().endswith(".zip"):
                z = zipfile.ZipFile(path)
                names = z.namelist()
                main_b = z.read("SystemData.bin")
                cls_b = z.read("SystemData.bin.classes") if "SystemData.bin.classes" in names else None
            else:
                main_b = open(path, "rb").read()
                cls_b = open(path + ".classes", "rb").read() if os.path.exists(path + ".classes") else None
            show(os.path.basename(path), main_b, cls_b, account)
        except Exception as e:  # noqa: BLE001 - keep going through the series
            print("== %s: %s" % (os.path.basename(path), e))


if __name__ == "__main__":
    main(sys.argv)
