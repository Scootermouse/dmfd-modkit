"""Ice engine .tb2 text table (decoded form, i.e. after dmfd_data decrypt+inflate).

Layout (all u32 LE, offsets from file start):
  0x00 magic 0xFFFFFFFE
  0x04 version? (8)
  0x08 nLang            0x0C langNameOfsTable (nLang x u32 -> "zh-cn", ...)
  0x10 nLabel           0x14 labelTree  (nLabel x 24 bytes)
  0x18 nEntry          0x1C entryAuxOfs (nEntry x u32). One entry per
                        (label, variant). Its string is per-line metadata, e.g.
                        "<tname/Asio><tface/fa032_p00>" (speaker name + portrait)
                        in quest dialogue; empty in plain tables.
  0x20 nString          0x24 stringTree (nString x 16 bytes)
  then the UTF-8 string pool (NUL-terminated).

labelTree record:  hash, left, right, nVariant, firstEntry, labelOfs
  (labels are numbered in hash order; firstEntry = sum of nVariant before it)
stringTree record: hash, left, right, textOfs
Both trees are a pre-order binary search tree on hash (left child = i+1,
-1 = none).  Hash: h = 0xCDE723A5; for each (signed) byte c: h = (h + c) * 0x8D mod 2^32.
  label hash  = H(label)
  string hash = H(label + ":" + variant + ":" + lang)      e.g. "Oldem:0:en"
"""
import struct

M = 0xFFFFFFFF


def ice_hash(s):
    if isinstance(s, str):
        s = s.encode("utf-8")
    h = 0xCDE723A5
    for c in s:
        h = ((h + (c - 256 if c >= 128 else c)) * 0x8D) & M
    return h


def _cstr(b, o):
    return b[o:b.index(b"\0", o)].decode("utf-8")


def parse(b):
    u = lambda o: struct.unpack_from("<I", b, o)[0]
    magic, ver, nl, lt, nlab, labt, nent, aux, nstr, strt = (u(4 * i) for i in range(10))
    assert magic == 0xFFFFFFFE, hex(magic)
    langs = [_cstr(b, u(lt + 4 * i)) for i in range(nl)]
    strings = {}
    for i in range(nstr):
        hh, l, r, o = struct.unpack_from("<4I", b, strt + 16 * i)
        strings[hh] = _cstr(b, o)
    rows = []
    for i in range(nlab):
        hh, l, r, nvar, sid, lo = struct.unpack_from("<6I", b, labt + 24 * i)
        label = _cstr(b, lo)
        assert ice_hash(label) == hh, label
        for v in range(nvar):
            t = {}
            for lang in langs:
                k = ice_hash("%s:%d:%s" % (label, v, lang))
                t[lang] = strings.get(k)
            rows.append({"label": label, "variant": v, "entry": sid + v,
                         "aux": _cstr(b, u(aux + 4 * (sid + v))), "text": t})
    rows.sort(key=lambda r: r["entry"])
    for r in rows:
        del r["entry"]
    return {"version": ver, "langs": langs, "rows": rows}


def _bst(items):
    """items sorted by hash -> list in pre-order with (item, left, right)."""
    out = []

    def rec(lo, hi):
        if lo >= hi:
            return -1
        mid = (lo + hi) // 2
        idx = len(out)
        out.append([items[mid], -1, -1])
        out[idx][1] = rec(lo, mid)
        out[idx][2] = rec(mid + 1, hi)
        return idx
    rec(0, len(items))
    return out


def build(tbl):
    """Inverse of parse(). Rebuilds trees and pool; labels may be added or removed."""
    langs = tbl["langs"]
    labels = {}
    for r in tbl["rows"]:
        L = labels.setdefault(r["label"], {"vars": {}})
        L["vars"][r["variant"]] = (r.get("aux", ""), r["text"])
    pool = bytearray()
    pool_ofs = {}

    def add(s):  # no dedup: keep the game's own layout style
        o = len(pool)
        pool.extend(s.encode("utf-8") + b"\0")
        return o
    lang_ofs = [add(x) for x in langs]
    lab_items, str_items = [], []
    for name in sorted(labels, key=ice_hash):
        L = labels[name]
        lo = add(name)
        assert sorted(L["vars"]) == list(range(len(L["vars"]))), name
        aos = []
        for v in sorted(L["vars"]):
            aux_s, text = L["vars"][v]
            aos.append(add(aux_s))
            for lang in langs:
                s = text.get(lang)
                if s is None:
                    continue
                str_items.append((ice_hash("%s:%d:%s" % (name, v, lang)), add(s)))
        lab_items.append((ice_hash(name), name, len(L["vars"]), lo, aos))
    lab_items.sort()
    str_items.sort()
    nl, nlab, nstr = len(langs), len(lab_items), len(str_items)
    nent = sum(it[2] for it in lab_items)
    lt = 0x28
    labt = lt + 4 * nl
    aux = labt + 24 * nlab
    strt = aux + 4 * nent
    base = strt + 16 * nstr
    out = bytearray(struct.pack("<10I", 0xFFFFFFFE, tbl.get("version", 8), nl, lt,
                                nlab, labt, nent, aux, nstr, strt))
    out += b"".join(struct.pack("<I", base + o) for o in lang_ofs)
    sid, acc = {}, 0
    for it in lab_items:
        sid[it[0]] = acc
        acc += it[2]
    for (hh, name, nvar, lo, ao), l, r in _bst(lab_items):
        out += struct.pack("<6I", hh, l & M, r & M, nvar, sid[hh], base + lo)
    out += b"".join(struct.pack("<I", base + ao) for it in lab_items for ao in it[4])
    for (hh, so), l, r in _bst(str_items):
        out += struct.pack("<4I", hh, l & M, r & M, base + so)
    assert len(out) == base
    return bytes(out + pool)


USAGE = """usage:
  python -I tb2.py dump  <asset.tb2 | decoded file> <out.json>
  python -I tb2.py build <in.json> <out.tb2.dec>     (then: dmfd_data.py pack)
"""


def main(argv):
    import json
    import os
    import sys
    if len(argv) < 4:
        print(USAGE)
        return
    cmd, src, dst = argv[1], argv[2], argv[3]
    if cmd == "dump":
        if os.path.exists(src):
            b = open(src, "rb").read()
        else:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dmfd_data
            raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(src)), "rb").read()
            b = dmfd_data.decode(raw)[0]
        json.dump(parse(b), open(dst, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    elif cmd == "build":
        tbl = json.load(open(src, encoding="utf-8"))
        open(dst, "wb").write(build(tbl))
    else:
        print(USAGE)
        return
    print(dst)


if __name__ == "__main__":
    import sys
    main(sys.argv)
