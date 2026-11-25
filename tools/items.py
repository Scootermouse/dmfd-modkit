"""List the built-in item table as a spreadsheet (tab-separated, opens in Excel).

Reads the item table from an unpacked game exe (the Steam exe is wrapped, so
use the unpacked copy) and the English names from DataHash/sys_item.tb2.

usage: python -I tools/items.py [items.tsv] [path\\to\\game_unpacked.exe]

Columns match the field names DataMod/items.txt accepts (price, stars,
power, shop, unlock); the raw record follows as hex for fields not decoded yet.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmfd_data  # noqa: E402
import tb2  # noqa: E402

GAME = os.path.dirname(dmfd_data.DEFAULT_DH)
EXE = os.path.join(GAME, "game_unpacked.exe")
ITEM_DB = 0x124AE10
# Equipment stats (signed 16-bit), in the order the game's stat panel shows them.
STATS = [("str", 0x66), ("agi", 0x68), ("int", 0x6A), ("vit", 0x6C), ("luc", 0x6E), ("pie", 0x70),
         ("atk", 0x4C), ("matk", 0x50), ("def", 0x4E), ("mdef", 0x52), ("acc", 0x54), ("macc", 0x56),
         ("crt", 0x58)]
CLASSES = ["empress", "warrior", "shinobi", "witch", "oracle", "bandit"]  # equip mask bits 0-5
CATEGORY = {0: "dagger", 1: "sword", 2: "axe", 3: "greatsword", 4: "kunai", 5: "shuriken",
            6: "book", 7: "crystal", 8: "accessory", 9: "consumable/material", 10: "other"}


class Exe:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        so = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = [struct.unpack_from("<8sIIII", d, so + 40 * i)[1:] for i in range(nsec)]

    def off(self, va):
        r = va - self.base
        for vsz, va_, rsz, rp in self.secs:
            if va_ <= r < va_ + rsz:
                return rp + r - va_
        raise ValueError(hex(va))


def load_items(exe_path=EXE):
    x = Exe(exe_path)
    o = x.off(ITEM_DB)
    n, rs, to, ro, so = struct.unpack_from("<5I", x.d, o)
    pool = o + so

    def cstr(ofs):
        return x.d[pool + ofs:x.d.index(b"\0", pool + ofs)].decode("ascii", "replace")
    out = []
    for i in range(n):
        r = x.d[o + ro + rs * i:o + ro + rs * (i + 1)]
        out.append({
            "label": cstr(struct.unpack_from("<I", r, 0)[0]),
            "category": CATEGORY.get(r[4], r[4]),
            "icon": struct.unpack_from("<I", r, 0x08)[0],
            "order": struct.unpack_from("<I", r, 0x84)[0],
            "price": struct.unpack_from("<I", r, 0x0C)[0],
            "stars": r[0x14],
            "power": struct.unpack_from("<I", r, 0x28)[0],
            "shop": "0x%02x" % r[0x7C],
            "unlock": struct.unpack_from("<h", r, 0x7E)[0],
            "equip": struct.unpack_from("<H", r, 0x48)[0],
            # weapons: sprite file, palette file (strings) and color (palette number)
            "sprite": cstr(struct.unpack_from("<I", r, 0x18)[0]) if struct.unpack_from("<I", r, 0x18)[0] else "",
            "sprite_palette": cstr(struct.unpack_from("<I", r, 0x1C)[0]) if struct.unpack_from("<I", r, 0x1C)[0] else "",
            "color": struct.unpack_from("<I", r, 0x20)[0],
            **{name: struct.unpack_from("<h", r, ofs)[0] for name, ofs in STATS},
            "effects": [read_effect(r, n) for n in (1, 2, 3)],
            "raw": r.hex(),
        })
    return out


# Three effect slots: +0x24/+0x30/+0x3C, each {u32 kind hash, u32 params, u32 extra}.
# params: low 16 bits = value, high 16 bits = value2; extra: high 16 bits = time (sec.), low = extra.
EFFECT_SLOT = {1: 0x24, 2: 0x30, 3: 0x3C}
EFF_NONE = 0x51889489
EXTRA_EFFECT_NAMES = {0x822BC24B: "REVIVE"}  # Chief's Meat, Ashes of Life; name not in the exe

# Effects no game item uses, sorted by what the exe has for them. Each effect's
# hash is kept in globals; the equip-effect code (0xcb8000-0xcbc000) and the
# description lookup (0xccb000-0xccc000) compare against them. See FORMAT.md.
UNUSED_WITH_CODE = {  # behavior and description code both present: likely to work
    "DAMAGE_UP", "DAMAGE_UP_MAGIC", "DAMAGE_UP_TO_ELEMENT", "DRAGON_RESIST", "MAGIC_RESIST",
    "MAGIC_UP_NORMAL", "EFFECT_EXTENSION", "TIME_UP_CANDY_ITEM", "SPECIFIC_BREED_DRAGON",
    "RETRY_DAMAGE_UP_DRAGON", "DRAGON_DAMAGE_UP_DRAGON", "MAGIC_UP_ENCHANT", "HEAL_HP_RATE_STATUS_UP",
}
UNUSED_NO_CODE = {  # registered only, or description only: tested NOT_CONSUMED_ITEM does nothing
    "NOT_CONSUMED_ITEM", "MC_UP", "AC_UP", "WC_UP", "FORCE_BAD_STATUS",
    "MAGIC_DMG_RATE", "PHYSICS_DMG_RATE", "DRAGON_DMG_RATE",
}
PICKUP_EFFECTS = {"GET_MONEY", "GET_JEWEL", "GET_ICE", "GET_KEY"}  # used by map pickups, not items


TESTED_UNUSED = {"DAMAGE_UP"}  # unused by the game's items, confirmed working in-game


def effect_status(name, used_by):
    if used_by:
        return "works (used by game items)"
    if name in TESTED_UNUSED:
        return "unused by the game; tested, works"
    if name in PICKUP_EFFECTS:
        return "pickup effect, not for items"
    if name in UNUSED_WITH_CODE:
        return "unused; game has code for it, likely works"
    if name in UNUSED_NO_CODE:
        return "unused; no game code, does nothing"
    return "unused; unknown"


def read_effect(r, n):
    h, p, x = struct.unpack_from("<3I", r, EFFECT_SLOT[n])
    return {"hash": h, "value": p & 0xFFFF, "value2": p >> 16, "time": x >> 16, "extra": x & 0xFFFF}


_effect_names = None


def effect_names(exe_path=EXE):
    """{hash: short name}, e.g. LEVEL_DAMAGE_UP (from the exe's EFF_E_/EFF_U_ strings)."""
    global _effect_names
    if _effect_names is None:
        import re
        d = Exe(exe_path).d
        _effect_names = dict(EXTRA_EFFECT_NAMES)
        for m in re.finditer(rb"EFF_[A-Z0-9_]+(?=\x00)", d):
            full = m.group().decode()
            short = "NONE" if full == "EFF_NONE" else full[6:]  # drop EFF_E_ / EFF_U_
            _effect_names[tb2.ice_hash(full)] = short
    return _effect_names


def main(argv):
    dst = argv[1] if len(argv) > 1 else "items.tsv"
    exe = argv[2] if len(argv) > 2 else EXE
    items = load_items(exe)
    raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name("sys_item.tb2")), "rb").read()
    names = {r["label"]: r["text"]["en"] for r in tb2.parse(dmfd_data.decode(raw)[0])["rows"]}
    cols = ["label", "name", "category", "price", "stars", "power", "shop", "unlock", "raw"]
    with open(dst, "w", encoding="utf-8-sig", newline="") as f:
        f.write("\t".join(cols) + "\n")
        for it in items:
            it["name"] = (names.get(it["label"]) or "").replace("\n", " ")
            f.write("\t".join(str(it[c]) for c in cols) + "\n")
    print(len(items), "items ->", dst)


if __name__ == "__main__":
    main(sys.argv)
