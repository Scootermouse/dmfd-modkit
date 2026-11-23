"""Treasure chest drop tables (built into the exe; see FORMAT.md, "Chests").

Chest TREASUREBOX_QUEST_q_s_c (quest q-s, chest c) uses the tables
DROP_q_s_c_N<level> (normal) and DROP_q_s_c_R<level> (rare). On opening, the
game takes the first normal and rare table whose level is >= the current
level, rolls the rare table's chance, then picks an item from that table's
16 slots by weight, among the items the player's class can use.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import items as itemdb  # noqa: E402

DROP_DB = 0x1358318
SLOTS = 16


def load_tables(exe_path=itemdb.EXE):
    x = itemdb.Exe(exe_path)
    d = x.d
    h = x.off(DROP_DB)
    n, rs, _, ro, so = struct.unpack_from("<5I", d, h)
    pool = h + so

    def cstr(o):
        return d[pool + o:d.index(b"\0", pool + o)].decode("ascii", "replace")
    out = []
    for i in range(n):
        r = d[h + ro + rs * i:h + ro + rs * (i + 1)]
        weights = struct.unpack_from("<16H", r, 8)
        labels = struct.unpack_from("<16I", r, 0x28)
        out.append({
            "name": cstr(struct.unpack_from("<I", r, 0)[0]),
            "rare": r[4] == 1,
            "rare_chance": r[5],
            "level": r[6],
            "slots": [(cstr(labels[s]), weights[s]) for s in range(SLOTS)],
        })
    return out


def chest_of(table_name):
    """DROP_1_0_0_N9 -> 'quest 1-0 chest 0'."""
    p = table_name.split("_")
    return "quest %s-%s chest %s" % (p[1], p[2], p[3]) if len(p) >= 5 else ""
