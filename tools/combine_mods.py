"""Combine every switched-on data mod in the game's mods folder into the game's DataMod folder.

A data mod is a folder in <game>\\mods with a mod.ini and a data\\package.json (made by Build
mod). Mods are combined in load order (mods\\order.txt first, then alphabetically); when two
mods change the same thing, the one later in the order is used and a warning says so.

- Files only one mod has (bodies, palettes, weapon pictures) are copied as they are.
- Text is merged entry by entry, items line by line (by item), chest drops by table and item.
- New icons and extra classes are numbered here, never by modders: icons in a fixed order,
  classes by their unique id (see build_mod.number_classes), so everyone with the same mods
  gets the same numbers.
- DataMod\\combined.txt lists what was combined, so the loader can warn when the mods folder
  changed without combining again.

The Mod Manager has its own C# copy of this (manager/src/Combine.cs) and runs it whenever mods
are added, removed, switched or reordered; this file stays the reference: after changing either,
run tools/test_combiner.py (it must find no differences). Developers:
python -I tools/combine_mods.py
"""
import configparser
import hashlib
import re
import json
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_mod as bm  # noqa: E402
import dmfd_data  # noqa: E402
import icons  # noqa: E402
import tb2  # noqa: E402

MODS = os.path.join(bm.GAME, "mods")
STAMP = "combined.txt"


def mod_order():
    """Folders in mods\\ that have a mod.ini, in load order."""
    if not os.path.isdir(MODS):
        return []
    found = sorted((f for f in os.listdir(MODS) if os.path.isfile(os.path.join(MODS, f, "mod.ini"))),
                   key=str.lower)
    ordered = []
    order = os.path.join(MODS, "order.txt")
    if os.path.exists(order):
        for line in open(order, encoding="utf-8-sig"):
            s = line.strip()
            if not s or s[0] in ";#":
                continue
            hit = next((f for f in found if f.lower() == s.lower()), None)
            if hit:
                ordered.append(hit)
                found.remove(hit)
    return ordered + found


def read_ini(folder):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        cp.read(os.path.join(MODS, folder, "mod.ini"), encoding="utf-8-sig")
    except configparser.Error:
        return {}
    return dict(cp["mod"]) if cp.has_section("mod") else {}


def data_mods():
    """[(folder, name, version, package path)] for switched-on mods that have data."""
    out = []
    for folder in mod_order():
        info = read_ini(folder)
        pkg = os.path.join(MODS, folder, "data", "package.json")
        if info.get("enabled", "1").strip() == "0" or not os.path.exists(pkg):
            continue
        out.append((folder, info.get("name", folder), info.get("version", "?"), pkg))
    return out


def stamp_lines(mods):
    """What the loader compares: one line per combined mod (folder, package size, package time)."""
    lines = []
    for folder, _, _, pkg in mods:
        st = os.stat(pkg)
        lines.append("%s %d %d" % (folder, st.st_size, int(st.st_mtime)))
    return lines


def tag(folder):
    """A short name prefix for one mod's new files, so two mods' "pl6_cb00" never clash."""
    return "m%s_" % hashlib.md5(folder.lower().encode()).hexdigest()[:6]


def renamer(folder, pkg):
    """-> function mapping one of this mod's file names (with or without Win/ and .osb) to its
    combined name; names that aren't the mod's own new files come back unchanged."""
    new = {a[4:-4] for a in pkg.get("new_assets", []) if a.startswith("Win/") and a.endswith(".osb")}
    t = tag(folder)

    def rn(name):
        m = re.match(r"^(Addon:\d+:)?(Win/)?(.*?)(\.osb)?$", name)
        stem = m.group(3)
        if stem not in new:
            return name
        return (m.group(1) or "") + (m.group(2) or "") + t + stem + (m.group(4) or "")
    return rn


def item_key(line):
    t = line.split()
    return t[1] if t and t[0] == "new" and len(t) > 1 else (t[0] if t else "")


def combine():
    bm.problems.clear()
    mods = data_mods()
    warnings = []
    sprite_files, owner = {}, {}
    tables, text_owner = {}, {}
    items, item_owner = {}, {}
    drops, drop_owner = {}, {}
    layout_rows = []
    defs, def_owner = {}, {}
    icon_srcs = []  # (folder, local number, png path)
    pkgs = []
    for folder, name, version, path in mods:
        with open(path, encoding="utf-8") as f:
            pkg = json.load(f)
        if pkg.get("format", 1) > 1:
            bm.problem(name, "made with a newer Build mod; update the mod tools")
            continue
        pkgs.append((folder, name, pkg, os.path.dirname(path)))
        for icon in pkg.get("icons", []):
            icon_srcs.append((folder, icon["n"], os.path.join(os.path.dirname(path), *icon["png"].split("/"))))

    # new icons: numbered from 24 in a fixed order (mod folder, then the modder's own number)
    icon_map = {}  # (folder, local) -> number
    icon_pngs = {}
    for i, (folder, n, png) in enumerate(sorted(icon_srcs, key=lambda x: (x[0].lower(), x[1]))):
        icon_map[(folder, n)] = 24 + i
        icon_pngs[24 + i] = png

    for folder, name, pkg, data in pkgs:
        rn = renamer(folder, pkg)
        pals = set(pkg.get("palettes", []))
        for src in pkg.get("assets", []):
            with open(os.path.join(data, "assets", *src.split("/")) + ".z", "rb") as f:
                b = zlib.decompress(f.read())
            asset = rn(src)
            if asset != src:  # one of the mod's own new files, renamed: fix what points at its old name
                if src in pals:
                    b = bm.palettes.set_id(b, asset[4:-4])
                else:
                    b, _ = bm.sprites.rename_self_refs(b, src[4:], asset[4:])
            if asset in owner:
                warnings.append("%s and %s both change %s; using %s's" % (owner[asset], name, asset[4:], name))
            sprite_files[asset] = b
            owner[asset] = name
        for row in pkg.get("text", []):
            key = (row["file"], row["label"], row["variant"])
            if key in text_owner and text_owner[key] != name:
                warnings.append("%s and %s both change the text %s; using %s's" % (text_owner[key], name, row["label"], name))
            text_owner[key] = name
            t = tables.setdefault(row["file"], bm.vanilla_table(row["file"]))
            t["rows"] = [r for r in t["rows"] if (r["label"], r["variant"]) != (row["label"], row["variant"])]
            t["rows"].append({"label": row["label"], "variant": row["variant"], "aux": row["aux"], "text": row["text"]})
        for line in pkg.get("items", []):
            toks = []
            for tok in line.split():
                if tok.startswith("icon="):
                    n = int(tok[5:], 0)
                    if n > 23:
                        tok = "icon=%d" % icon_map.get((folder, n), n)
                elif tok.startswith("sprite=") or tok.startswith("sprite_palette="):
                    k, _, v = tok.partition("=")
                    tok = k + "=" + rn(v)
                toks.append(tok)
            line = " ".join(toks)
            key = item_key(line)
            if key in item_owner and item_owner[key] != name:
                warnings.append("%s and %s both change the item %s; using %s's" % (item_owner[key], name, key, name))
            item_owner[key] = name
            items[key] = line
        for line in pkg.get("drops", []):
            t = line.split()
            key = tuple(t[:2])
            if key in drop_owner and drop_owner[key] != name:
                warnings.append("%s and %s both change the drop %s; using %s's" % (drop_owner[key], name, " ".join(key), name))
            drop_owner[key] = name
            drops[key] = line
        layout_rows += [[rn(r[0])] + r[1:] for r in pkg.get("layout", [])]
        for d in pkg.get("classes", []):
            d = dict(d)
            for field in ("body", "palette", "quest_hud", "status_hud"):
                if d.get(field):
                    d[field] = rn(d[field])
            d["art"] = {k: os.path.join(data, *v.split("/")) for k, v in d.get("art", {}).items()}
            if d["id"] in def_owner:
                warnings.append("%s and %s both add the class %s; using %s's" % (def_owner[d["id"]], name, d["id"], name))
            defs[d["id"]] = d
            def_owner[d["id"]] = name

    if layout_rows:
        bm.apply_layout(layout_rows, sprite_files)
    numbered = bm.number_classes(list(defs.values()))
    class_lines = bm.apply_classes(numbered, sprite_files, tables) if numbered else []
    sheet, new_icons = (None, [])
    if icon_pngs:
        sheet, new_icons = icons.import_icons(icon_pngs, problem=lambda m: bm.problems.append(m))
    if bm.problems:
        print("The mods couldn't be combined; DataMod was left as it was:\n")
        for p in bm.problems:
            print("  - " + p)
        return False

    files = {}
    if items:
        files["items.txt"] = ("# Combined from the mods folder. Edit the mods, not this file.\n"
                              + "\n".join(items.values()) + "\n").encode("utf-8")
    for fname, t in tables.items():
        files[dmfd_data.file_name(fname)] = dmfd_data.encode_asset(fname, tb2.build(t))
    if sheet:
        files[dmfd_data.file_name(icons.ASSET)] = dmfd_data.encode_asset(icons.ASSET, sheet)
    if new_icons:  # tells the content mod which new icon numbers exist
        files["icons.txt"] = "".join("%d\n" % n for n in new_icons).encode()
    for asset, data in sprite_files.items():
        files[dmfd_data.file_name(asset)] = dmfd_data.encode_asset(asset, data)
    if class_lines:
        files["classes.txt"] = ("\n".join(class_lines) + "\n").encode()
    if drops:
        files["drops.txt"] = ("# Combined from the mods folder.\n" + "\n".join(drops.values()) + "\n").encode()
    files[STAMP] = ("# The data mods combined into this folder (folder, package size, package time).\n"
                    "# The mod loader warns when the mods folder no longer matches.\n"
                    + "".join(l + "\n" for l in stamp_lines(mods))).encode()
    bm.write_outputs(files)
    for w in warnings:
        print("Overlap: " + w)
    print("Combined %d data mod(s): %s" % (len(mods), ", ".join("%s %s" % (m[1], m[2]) for m in mods) or "none"))
    for line in class_lines:
        t = line.split()
        print("  class %s: %s" % (t[0], next(x[3:] for x in t if x.startswith("id="))))
    return True


if __name__ == "__main__":
    sys.exit(0 if combine() else 1)
