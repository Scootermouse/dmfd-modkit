r"""Build the content mod's DataMod folder from the spreadsheets in mod/.

    mod/items.csv   item changes and new items (one row per item)
    mod/text.csv    any other game text (optional)

Double-click "Build mod.bat" to run it; "List all items.bat" writes
mod/all_items.csv, a read-only reference of every item in the game.

Output goes to the game's DataMod folder and to build/DataMod (a copy to share).
Another mod's sheets: tools\py.bat tools\build_mod.py --mod <folder> (a folder laid out like mod/).
The game's own DataHash files are only read, never changed.
"""
import csv
import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dmfd_data  # noqa: E402
import drops  # noqa: E402
import icons  # noqa: E402
import layout  # noqa: E402
import osb  # noqa: E402
import palettes  # noqa: E402
import selectart  # noqa: E402
import sprites  # noqa: E402
import items as itemdb  # noqa: E402
import tb2  # noqa: E402

MOD = os.path.join(ROOT, "mod")
GAME = os.path.dirname(dmfd_data.DEFAULT_DH)
OUTS = [os.path.join(GAME, "DataMod"), os.path.join(ROOT, "build", "DataMod")]
MANIFEST = "_built_by_build_mod.txt"  # lists the files this script owns in DataMod

STAT_FIELDS = [name for name, _ in itemdb.STATS]  # str agi int vit luc pie atk matk def mdef acc macc crt
NUMBER_FIELDS = ["price", "stars", "power", "icon", "order", "unlock", "colour"] + STAT_FIELDS
TEXT_FIELDS = ["sprite", "sprite_palette"]  # weapons: sprite file and palette file names
# Effect columns -> raw record fields. effectN = kind (name), the rest are 16-bit parts.
EFFECT_COLUMNS = {}
for _n, _o in itemdb.EFFECT_SLOT.items():
    EFFECT_COLUMNS["effect%d" % _n] = (_o, 4)
    EFFECT_COLUMNS["effect%d_value" % _n] = (_o + 4, 2)
    EFFECT_COLUMNS["effect%d_value2" % _n] = (_o + 6, 2)
    EFFECT_COLUMNS["effect%d_extra" % _n] = (_o + 8, 2)
    EFFECT_COLUMNS["effect%d_time" % _n] = (_o + 10, 2)
SHOP_WORDS = {"none": 0x00, "no": 0x00, "tool": 0x13, "tools": 0x13, "tool-late": 0x11,
              "weapon": 0x23, "weapons": 0x23, "weapon-late": 0x21}
LANGS = ["zh-cn", "zh-tw", "de", "en", "es", "fr", "it", "ja", "ko"]

problems = []
PACKAGE_ONLY = False  # --package-only: make build/package/<folder> only (no install, no combine)


def problem(where, msg):
    problems.append("%s: %s" % (where, msg))


def read_csv(path):
    """Rows as dicts with lower-case keys. Copes with Excel's encodings and ; separators."""
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "cp1252"):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    sample = text[:4096]
    delim = ";" if sample.count(";") > sample.count(",") else ","
    rows = []
    for i, row in enumerate(csv.DictReader(io.StringIO(text), delimiter=delim), start=2):
        row = {(k or "").strip().lower(): (v or "").strip() for k, v in row.items()}
        if any(row.values()):
            row["_line"] = i
            rows.append(row)
    return rows


def write_csv(path, header, rows):
    with open(path, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def vanilla_table(name):
    raw = open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(name)), "rb").read()
    return tb2.parse(dmfd_data.decode(raw)[0])


def parse_equip(where, value):
    """'all', a number, or class names like 'empress+bandit'."""
    v = value.lower().strip()
    if v == "all":
        return 63
    if v in ("none", "nobody"):
        return 0
    try:
        return int(v, 0)
    except ValueError:
        pass
    mask = 0
    for word in re.split(r"[\s+/&|;]+", v):
        if not word:
            continue
        if word not in itemdb.CLASSES:
            problem(where, "equip: %r is not a class (use %s, all, or a number)"
                    % (word, ", ".join(itemdb.CLASSES)))
            return None
        mask |= 1 << itemdb.CLASSES.index(word)
    return mask


def equip_names(mask):
    if mask & 63 == 63:
        return "all"
    return "+".join(c for i, c in enumerate(itemdb.CLASSES) if mask >> i & 1) or "none"


def parse_effect(where, field, value):
    """Effect name (LEVEL_DAMAGE_UP, EFF_E_LEVEL_DAMAGE_UP, none) or a hash number."""
    by_name = {name: h for h, name in itemdb.effect_names().items()}
    v = value.strip().upper()
    for prefix in ("EFF_E_", "EFF_U_", "EFF_"):
        if v.startswith(prefix) and v[len(prefix):] in by_name:
            v = v[len(prefix):]
    if v in by_name:
        return by_name[v]
    try:
        return int(value, 0) & 0xFFFFFFFF
    except ValueError:
        problem(where, "%s: no effect called %r (see all_effects.csv)" % (field, value))
        return None


def parse_number(where, field, value):
    if field in EFFECT_COLUMNS and EFFECT_COLUMNS[field][1] == 4:
        return parse_effect(where, field, value)
    if field == "equip":
        return parse_equip(where, value)
    if field == "shop" and value.lower() in SHOP_WORDS:
        return SHOP_WORDS[value.lower()]
    try:
        v = int(value, 0)
    except ValueError:
        try:
            v = int(float(value))  # Excel sometimes writes 150.0
        except ValueError:
            if field == "shop":
                problem(where, "shop should be none, tool, tool-late, weapon, weapon-late or a number, not %r" % value)
            else:
                problem(where, "%s should be a number, not %r" % (field, value))
            return None
    if field in STAT_FIELDS and not -32768 <= v <= 32767:
        problem(where, "%s must be between -32768 and 32767, not %d" % (field, v))
        return None
    if field in EFFECT_COLUMNS and not 0 <= v <= 65535:
        problem(where, "%s must be between 0 and 65535, not %d" % (field, v))
        return None
    return v


def build_items(tables, known):
    """items.csv -> items.txt lines, plus item names into sys_item.tb2."""
    path = os.path.join(MOD, "items.csv")
    if not os.path.exists(path):
        return []
    lines = ["# Generated by Build mod from mod/items.csv. Edit that file, not this one."]
    item_text = tables.setdefault("sys_item.tb2", vanilla_table("sys_item.tb2"))
    by_label = {}
    for r in item_text["rows"]:
        by_label.setdefault(r["label"], r)
    # Pass 1: check each row and read its numbers.
    rows = {}  # label -> (row, {field: value})
    for row in read_csv(path):
        where = "items.csv line %d" % row["_line"]
        label = row.get("label", "")
        copy = row.get("copy_from", "")
        if not label:
            problem(where, "no label")
            continue
        if label in rows:
            problem(where, "%s is listed twice" % label)
            continue
        if copy:
            if label in known:
                problem(where, "%s already exists in the game; leave copy_from empty to change it" % label)
                continue
            if not (row.get("name") or row.get("name_en")):
                problem(where, "new item %s needs a name" % label)
        elif label not in known:
            problem(where, "%s is not an item in the game (to add it, fill in copy_from)" % label)
            continue
        values = {}
        for f in NUMBER_FIELDS + ["shop", "equip"] + list(EFFECT_COLUMNS):
            if row.get(f):
                v = parse_number(where, f, row[f])
                if v is not None:
                    values[f] = v
        for f in TEXT_FIELDS:
            v = row.get(f, "")
            if not v:
                continue
            if re.search(r"[\s#]", v):
                problem(where, "%s can't contain spaces or #: %r" % (f, v))
                continue
            values[f] = v if v.lower().endswith(".osb") else v + ".osb"
        rows[label] = (row, values)

    # Pass 2: a copy starts from its source as this sheet leaves it. The
    # source can be a game item or another new item in the sheet; either way
    # the game copies the original game item, and we hand on the sheet's
    # changes along the way.
    def resolve(label, chain):
        row, values = rows[label]
        copy = row.get("copy_from", "")
        if not copy:
            return label, dict(values)  # a game item
        if copy in chain:
            problem("items.csv line %d" % row["_line"], "copy_from goes round in a circle: %s"
                    % " -> ".join(chain + [copy]))
            return None
        if copy in rows:
            base = resolve(copy, chain + [copy])
            if base is None:
                return None
            game_item, inherited = base
        elif copy in known:
            game_item, inherited = copy, {}
        else:
            problem("items.csv line %d" % row["_line"],
                    "copy_from %s is not an item in the game or in this sheet" % copy)
            return None
        inherited.update(values)
        return game_item, inherited

    for label, (row, values) in rows.items():
        copy = row.get("copy_from", "")
        if copy:
            base = resolve(label, [label])
            if base is None:
                continue
            game_item, values = base
            head = "new %s copy=%s" % (label, game_item)
        else:
            head = label
        fields = []
        for f, v in values.items():
            if f in EFFECT_COLUMNS:
                ofs, size = EFFECT_COLUMNS[f]
                fields.append("@%#x%s=%s" % (ofs, "" if size == 4 else ":2", hex(v) if size == 4 else v))
            else:
                fields.append("%s=%s" % (f, hex(v) if f == "shop" else v))
        if copy or fields:
            lines.append(" ".join([head] + fields))
        # names: "name" fills every language; name_xx overrides one
        names = {}
        if row.get("name"):
            names = {lang: row["name"] for lang in item_text["langs"]}
        for lang in item_text["langs"]:
            if row.get("name_" + lang):
                names[lang] = row["name_" + lang]
        if not names:
            continue
        if copy:
            src = by_label.get(game_item)
            text = {lang: names.get(lang, names.get("en", next(iter(names.values()))))
                    for lang in item_text["langs"]}
            item_text["rows"].append({"label": label, "variant": 0,
                                      "aux": src["aux"] if src else "", "text": text})
        else:
            for r in item_text["rows"]:
                if r["label"] == label:
                    r["text"].update(names)
    return lines


def build_text(tables):
    """text.csv: file, label, variant, lang, text. lang empty or * = every language."""
    path = os.path.join(MOD, "text.csv")
    if not os.path.exists(path):
        return
    for row in read_csv(path):
        where = "text.csv line %d" % row["_line"]
        fname, label = row.get("file", ""), row.get("label", "")
        if not fname or not label:
            continue
        if not fname.endswith(".tb2"):
            fname += ".tb2"
        try:
            t = tables.setdefault(fname, vanilla_table(fname))
        except FileNotFoundError:
            problem(where, "no text file %s in the game" % fname)
            continue
        variant = int(row.get("variant") or 0)
        lang = row.get("lang", "")
        langs = t["langs"] if lang in ("", "*") else [lang]
        if any(lg not in t["langs"] for lg in langs):
            problem(where, "language %s not in %s (it has %s)" % (lang, fname, ", ".join(t["langs"])))
            continue
        text = row.get("text", "").replace("\\n", "\n")
        hit = [r for r in t["rows"] if r["label"] == label and r["variant"] == variant]
        if hit:
            for lg in langs:
                hit[0]["text"][lg] = text
        else:
            t["rows"].append({"label": label, "variant": variant, "aux": "",
                              "text": {lg: (text if lg in langs else "") for lg in t["langs"]}})


def write_outputs(files):
    for out in OUTS:
        os.makedirs(out, exist_ok=True)
        man = os.path.join(out, MANIFEST)
        old = set()
        if os.path.exists(man):
            old = {l.strip() for l in open(man, encoding="utf-8") if l.strip() and not l.startswith("#")}
        for name, data in files.items():
            with open(os.path.join(out, name), "wb") as f:
                f.write(data)
        for name in old - set(files):  # files we made last time but not now
            p = os.path.join(out, name)
            if os.path.exists(p):
                os.remove(p)
        with open(man, "w", encoding="utf-8") as f:
            f.write("# Files written by Build mod. Others in this folder are left alone.\n")
            f.write("\n".join(sorted(files)) + "\n")


WEAPON_CATEGORIES = ("dagger", "sword", "axe", "greatsword", "kunai", "shuriken", "book", "crystal")


def list_items():
    names = {r["label"]: r["text"] for r in vanilla_table("sys_item.tb2")["rows"]}
    rows = []
    _load_full_effect_names()
    ALL_EFF_FULL.add("EFF_U_REVIVE")  # Chief's Meat / Ashes of Life; not named in the exe
    eff_names = itemdb.effect_names()
    used = {h: [] for h in eff_names}
    eff_cols = [c for n in (1, 2, 3) for c in ("effect%d" % n, "effect%d_value" % n,
                                                 "effect%d_value2" % n, "effect%d_time" % n)]
    for it in itemdb.load_items():
        if not it["label"]:
            continue  # record 0 is an empty placeholder
        n = names.get(it["label"], {})
        name = (n.get("en") or "").replace("\n", " ")
        effs = []
        for e in it["effects"]:
            kind = eff_names.get(e["hash"], "%#x" % e["hash"])
            if kind == "NONE":
                effs += ["", "", "", ""]
                continue
            effs += [kind, e["value"], e["value2"], e["time"]]
            used.setdefault(e["hash"], []).append((it["label"], name, e))
        rows.append([it["label"], name, it["category"], it["price"],
                     it["stars"], it["shop"], it["unlock"], equip_names(it["equip"])]
                    + [it[s] for s in STAT_FIELDS] + effs + [it["icon"], it["order"]]
                    + ([it["sprite"][:-4] if it["sprite"].endswith(".osb") else it["sprite"],
                        it["sprite_palette"], it["colour"]] if it["category"] in WEAPON_CATEGORIES
                       else ["", "", ""]))
    dst = os.path.join(MOD, "all_items.csv")
    write_csv(dst, ["label", "name", "category", "price", "stars", "shop", "unlock", "equip"]
              + STAT_FIELDS + eff_cols + ["icon", "order", "sprite", "sprite_palette", "colour"], rows)
    print("Wrote %d items to %s" % (len(rows), dst))

    erows = []
    for h, items_using in sorted(used.items(), key=lambda kv: (-len(kv[1]), eff_names.get(kv[0], ""))):
        kind = eff_names.get(h, "%#x" % h)
        if kind == "NONE":
            continue
        ex = items_using[0] if items_using else None
        erows.append([kind, "use" if "EFF_U_" + kind in ALL_EFF_FULL else "equip",
                      itemdb.effect_status(kind, len(items_using)), len(items_using),
                      ex[0] if ex else "", ex[1] if ex else "",
                      ex[2]["value"] if ex else "", ex[2]["value2"] if ex else "",
                      ex[2]["time"] if ex else ""])
    edst = os.path.join(MOD, "all_effects.csv")
    write_csv(edst, ["effect", "kind", "status", "items_using_it", "example_label", "example_name",
                     "example_value", "example_value2", "example_time"], erows)
    print("Wrote %d effects to %s" % (len(erows), edst))
    list_drops({lab: (t.get("en") or "").replace("\n", " ") for lab, t in names.items()})
    n = icons.export(os.path.join(MOD, "icons_original"))
    print("Wrote %d item icons to %s" % (n, os.path.join(MOD, "icons_original")))
    print("Copy a row's label into items.csv to change that item.")
    print("To redraw an icon, copy its PNG into mod\\icons and edit it there.")


ALL_EFF_FULL = set()  # filled by list_items via effect_names' source strings


def _load_full_effect_names():
    import re as _re
    d = itemdb.Exe(itemdb.EXE).d
    ALL_EFF_FULL.update(m.group().decode() for m in _re.finditer(rb"EFF_[A-Z0-9_]+(?=\x00)", d))


def build_drops(item_labels):
    """drops.csv (table, item, weight) -> drops.txt lines. table may use * wildcards."""
    import fnmatch
    path = os.path.join(MOD, "drops.csv")
    if not os.path.exists(path):
        return []
    tables = drops.load_tables()
    by_name = {t["name"]: t for t in tables}
    # what each table will hold after our edits, to catch full tables here
    state = {t["name"]: [list(s) for s in t["slots"]] for t in tables}
    lines = ["# Generated by Build mod from mod/drops.csv. Edit that file, not this one."]
    for row in read_csv(path):
        where = "drops.csv line %d" % row["_line"]
        pat, item, w = row.get("table", ""), row.get("item", ""), row.get("weight", "")
        if not pat or not item:
            problem(where, "needs a table and an item")
            continue
        if item not in item_labels:
            problem(where, "%s is not an item in the game or in items.csv" % item)
            continue
        weight = parse_number(where, "weight", w or "0")
        if weight is None:
            continue
        if not 0 <= weight <= 65535:
            problem(where, "weight must be between 0 and 65535")
            continue
        names = [n for n in by_name if fnmatch.fnmatchcase(n.upper(), pat.upper())]
        if not names:
            problem(where, "no chest table matches %s (see all_drops.csv)" % pat)
            continue
        wildcard = any(c in pat for c in "*?[")
        skipped = 0
        for name in names:
            slots = state[name]
            hit = [s for s in slots if s[1] and s[0] == item]
            if hit:
                hit[0][1] = weight
            elif weight:
                free = [s for s in slots if not s[1]]
                if not free:
                    if wildcard:
                        skipped += 1  # a pattern just leaves full tables alone
                    else:
                        problem(where, "%s has no free slot left for %s" % (name, item))
                    continue
                free[0][0], free[0][1] = item, weight
            lines.append("%s %s %d" % (name, item, weight))
        if skipped:
            print("Note: %s: %d of %d tables matching %s were full and were skipped."
                  % (where, skipped, len(names), pat))
    return lines


def list_drops(item_names):
    rows = []
    for t in drops.load_tables():
        items_txt = "; ".join("%s %s (%d)" % (lab, item_names.get(lab, ""), w)
                              for lab, w in t["slots"] if w)
        free = sum(1 for _, w in t["slots"] if not w)
        rows.append([t["name"], drops.chest_of(t["name"]), "rare" if t["rare"] else "normal",
                     t["level"], t["rare_chance"] if t["rare"] else "", free, items_txt])
    dst = os.path.join(MOD, "all_drops.csv")
    write_csv(dst, ["table", "chest", "kind", "up_to_level", "rare_chance_%", "free_slots",
                    "items (weight)"], rows)
    print("Wrote %d chest tables to %s" % (len(rows), dst))


SPRITE_FILES = ["pl0_cb00", "pl1_cb00", "pl1_cb01", "pl2_cb00", "pl3_cb00", "pl4_cb00", "pl5_cb00"]
SOURCE_TXT = "_source.txt"  # written by export: which game sprite file a folder came from


def asset_exists(asset):
    return os.path.exists(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name(asset)))


def palette_asset(name):
    """Item palette names like 'Addon:6:pl0_cw00_pal.osb' -> 'Win/pl0_cw00_pal.osb'."""
    return "Win/" + re.sub(r"^Addon:\d+:", "", name)


_weapons = None


def weapon_files():
    """{sprite file stem: {"palette": asset, "items": [(label, colour)]}} for every weapon sprite."""
    global _weapons
    if _weapons is None:
        _weapons = {}
        for it in itemdb.load_items():
            if it["category"] not in WEAPON_CATEGORIES or not it["sprite"].endswith(".osb"):
                continue
            stem = it["sprite"][:-4]
            if not asset_exists("Win/" + it["sprite"]):
                continue
            if stem not in _weapons:
                full = not sprites.is_paletted(sprites.load("Win/" + it["sprite"]))
                _weapons[stem] = {"pals": {}, "items": [], "full": full}
            w = _weapons[stem]
            pal = palette_asset(it["sprite_palette"])
            if asset_exists(pal):
                w["pals"][pal] = w["pals"].get(pal, 0) + 1
            w["items"].append((it["label"], it["colour"]))
        for stem, w in list(_weapons.items()):
            if not w["pals"]:
                if w["full"]:
                    w["palette"] = ""
                else:
                    del _weapons[stem]
                continue
            # prefer the main palette file (most colours) over the _ex_pal one
            w["palette"] = max(w["pals"], key=lambda p: (len(osb.palettes(sprites.load(p))), w["pals"][p]))
    return _weapons


def sprite_palette(folder):
    """Palette 0 for a body (plN_cb00 -> Win/plN_pal.osb) or weapon sprite file, else None.

    Always the game's own palette, as exported: sprites are painted in the
    original colours, and edited palettes then change what those become."""
    if folder in weapon_files():
        w = weapon_files()[folder]
        return None if w["full"] else palettes.colours(sprites.load(w["palette"]))[0]
    m = re.match(r"pl(\d)_cb\d\d$", folder)
    return palettes.colours(sprites.load("Win/pl%s_pal.osb" % m.group(1)))[0] if m else None


edited_palettes = {}  # asset -> palette file bytes made by build_palettes()
PALETTE_NEW_RE = re.compile(r"^([A-Za-z0-9_]+) from ([A-Za-z0-9_]+)$", re.I)


def body_palette_files():
    return ["Win/pl%d_pal.osb" % n for n in range(6)]


def weapon_palette_files():
    return sorted({p for w in weapon_files().values() for p in w["pals"]})


def export_palettes(assets):
    out = os.path.join(MOD, "palettes_original")
    os.makedirs(out, exist_ok=True)
    for asset in assets:
        npal, ncol = palettes.export_png(sprites.load(asset), os.path.join(out, asset[4:-4] + ".png"))
    print("Exported %d palette file(s) to %s" % (len(assets), out))


def build_palettes():
    """mod/palettes/<name>.png edits Win/<name>.osb; '<new> from <name>.png' makes Win/<new>.osb."""
    folder = os.path.join(MOD, "palettes")
    edited_palettes.clear()
    if not os.path.isdir(folder):
        return {}
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(".png"):
            continue
        where = "palettes\\" + f
        stem = f[:-4].strip()
        m = PALETTE_NEW_RE.match(stem)
        name, base = (m.group(1), m.group(2)) if m else (stem, stem)
        asset, base_asset = "Win/%s.osb" % name, "Win/%s.osb" % base
        if not asset_exists(base_asset):
            problem(where, "%s isn't a game palette file. To make a new one, name the picture like "
                           "'mod_cw00_pal from pl0_cw00_pal.png' (new name, then the file it starts from)" % base)
            continue
        if m and asset_exists(asset):
            problem(where, "%s already exists in the game; name the picture %s.png to edit it" % (name, name))
            continue
        if asset in edited_palettes:
            problem(where, "two pictures for %s" % name)
            continue
        b = sprites.load(base_asset)
        try:
            new = palettes.import_png(b, os.path.join(folder, f), report=lambda s: print("  note: " + s))
        except ValueError as e:
            problem(where, str(e))
            continue
        print("Palette %s%s" % (name, "" if name == base else " (new, from %s)" % base))
        if m:  # a new file needs its own palette ID, or the game confuses it with its source
            edited_palettes[asset] = palettes.set_id(new or b, name, base)
        elif new:
            edited_palettes[asset] = new
    return dict(edited_palettes)


# full colour: class portraits, character-select illustrations, NPC and enemy faces
ART_FILES = ["com_pl_icon", "s_o0a00", "s_o0200", "cutin00", "cp_o0000", "face", "EmFace"]


def _export(folder):
    out = os.path.join(MOD, "sprites_original", folder)
    b = sprites.load("Win/%s.osb" % folder)
    n = sprites.export_frames(b, out, sprite_palette(folder) if sprites.is_paletted(b) else None)
    with open(os.path.join(out, SOURCE_TXT), "w", encoding="utf-8") as f:
        f.write(folder + "\n")
    print("Exported %d frames to %s" % (n, out))
    return n


def export_sprites(weapons=False):
    total = 0
    if not weapons:
        for folder in SPRITE_FILES + ART_FILES:
            total += _export(folder)
        export_palettes(body_palette_files())
    else:
        export_palettes(weapon_palette_files())
        names = {r["label"]: (r["text"].get("en") or "").replace("\n", " ")
                 for r in vanilla_table("sys_item.tb2")["rows"]}
        rows = []
        for folder, w in sorted(weapon_files().items()):
            total += _export(folder)
            rows.append([folder, w["palette"][4:-4], len(w["items"]),
                         "; ".join("%s (%s, colour %d)" % (names.get(l, ""), l, c) for l, c in w["items"])])
        dst = os.path.join(MOD, "sprites_original", "weapons.csv")
        write_csv(dst, ["sprite", "sprite_palette", "items", "used by (name, label, colour)"], rows)
        print("Wrote the list of weapon sprites and the items using them to %s" % dst)
    print("Copy the frames you want to change into mod\\sprites\\<same folder>, edit them, then run Build mod.")
    return total


REPLACE_TXT = "_replace.txt"  # in a sprite folder: its frames replace whole pictures


TRACKS_TXT = "_tracks.txt"  # in a sprite folder: animation values to set (e.g. an effect's light colour)


def apply_tracks(b, path, where):
    """_tracks.txt lines: <animation name or 0xhash> <node 0xhash> <channel> <value>, '#' comments.
    Every key of that track gets the value. -> new bytes, or None if nothing changed."""
    out, total = b, 0
    for no, line in enumerate(open(path, encoding="utf-8-sig"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        try:
            anim, node, chan, value = parts[0], int(parts[1], 16), int(parts[2]), float(parts[3])
        except (IndexError, ValueError):
            problem(where, "%s line %d should be: animation node channel value" % (TRACKS_TXT, no))
            continue
        ah = int(anim, 16) if anim.lower().startswith("0x") else tb2.ice_hash(anim)
        out, n = sprites.set_track(out, ah, node, chan, value)
        if not n:
            problem(where, "%s line %d: no such track (%s, node %08x, channel %d)" % (TRACKS_TXT, no, anim, node, chan))
        total += n
    if total:
        print("  %s: %d animation value(s) set" % (TRACKS_TXT, total))
    return out if total else None


TIMING_TXT = "_timing.txt"  # in a sprite folder: new animation lengths / picture key times
HITBOX_TXT = "_hitboxes.txt"  # in a sprite folder: attack boxes of frame pictures


def apply_timing(b, path, where):
    """_timing.txt lines, '#' comments:
        a<index> <length> <t0> <t1> ...       new start time for each picture key, in order
        a<index> <length> <time>:<key> ... [loop <from> <every> <key> <key> ...]
            a new picture track: show the animation's own picture <key> (0-based) from <time>;
            "loop" then cycles the listed keys every <every> ticks from <from> up to <length>.
        a<index> nodes <n>                    keep only the animation's first n nodes (drops e.g.
            the tether attach point of the Empress's wire animations)
    -> new bytes, or None if nothing changed."""
    out, done = b, 0
    for no, line in enumerate(open(path, encoding="utf-8-sig"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        try:
            if len(parts) == 3 and parts[1].lower() == "nodes":
                out = sprites.set_node_count(out, int(parts[0].lstrip("aA")), int(parts[2]))
                done += 1
                continue
            index, length, rest = int(parts[0].lstrip("aA")), float(parts[1]), parts[2:]
            if any(":" in x for x in rest) or "loop" in rest:
                keys, loop = [], None
                if "loop" in rest:
                    i = rest.index("loop")
                    rest, loop = rest[:i], rest[i + 1:]
                for x in rest:
                    t, k = x.split(":")
                    keys.append((float(t), int(k)))
                if loop:
                    t, every, cyc = float(loop[0]), float(loop[1]), [int(x) for x in loop[2:]]
                    j = 0
                    while t < length:
                        keys.append((t, cyc[j % len(cyc)]))
                        t += every
                        j += 1
                out = sprites.set_keys(out, index, length, keys)
            else:
                out = sprites.set_timing(out, index, length, [float(x) for x in rest])
            done += 1
        except (IndexError, ValueError) as e:
            problem(where, "%s line %d: %s" % (TIMING_TXT, no, e))
    if done:
        print("  %s: %d animation(s) retimed" % (TIMING_TXT, done))
    return out if done else None


def apply_hitboxes(b, path, where):
    """_hitboxes.txt lines, '#' comments:
        m<mesh> attack <x> <y> <w> <h> <kind>   the frame picture's attack boxes become this one box
                                                (centre x, centre y up from the feet, size, kind)
        m<mesh> attack none                     the frame picture deals no hit
        m<mesh> attack <x> <y> <w> <h> <kind> from m<donor>
            for a frame with no attack record: it takes over the donor frame's first one (the
            donor then deals no hit; frames with the same box may share one donor)
    -> new bytes, or None if nothing changed."""
    out, done = b, 0
    for no, line in enumerate(open(path, encoding="utf-8-sig"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        try:
            mesh = int(parts[0].lstrip("mM"))
            if parts[1].lower() != "attack":
                raise ValueError("expected 'attack'")
            donor = None
            if len(parts) >= 9 and parts[7].lower() == "from":
                donor = int(parts[8].lstrip("mM"))
                parts = parts[:7]
            box = None if parts[2].lower() == "none" else tuple(int(x) for x in parts[2:7])
            if box is not None and len(box) != 5:
                raise ValueError("a box is x y w h kind")
            out = sprites.set_attack_box(out, mesh, box, donor)
            done += 1
        except (IndexError, ValueError) as e:
            problem(where, "%s line %d: %s" % (HITBOX_TXT, no, e))
    if done:
        print("  %s: %d frame(s) changed" % (HITBOX_TXT, done))
    return out if done else None


def class_palette_for(folder):
    """(palette name, colour row 0) of the class in classes.csv whose body is this sprite
    folder and whose palette this mod makes, else None. A new class's body is then painted
    in its own colours: its palette can hold colours the base class never used."""
    path = os.path.join(MOD, "classes.csv")
    if not os.path.exists(path):
        return None
    for row in read_csv(path):
        if row.get("body", "").strip() != folder:
            continue
        name = row.get("palette", "").strip()
        b = edited_palettes.get("Win/%s.osb" % name)
        if b:
            return name, palettes.colours(b)[0]
    return None


def build_sprites():
    """mod/sprites/<folder>/*.png -> {asset: decoded sprite file}.

    A folder named after a game sprite file (pl0_cb00, pl0_cw03) edits that file.
    Any other name makes a new sprite file Win/<folder>.osb, starting from the
    file named in the folder's _source.txt (export writes it), for new weapons.
    """
    root = os.path.join(MOD, "sprites")
    out = {}
    if not os.path.isdir(root):
        return out
    for folder in sorted(os.listdir(root)):
        path = os.path.join(root, folder)
        if not os.path.isdir(path):
            continue
        where = "sprites\\" + folder
        pngs = sorted(os.path.join(path, f) for f in os.listdir(path) if f.lower().endswith(".png"))
        asset = "Win/%s.osb" % folder
        base = folder
        if not asset_exists(asset):
            src = os.path.join(path, SOURCE_TXT)
            base = open(src, encoding="utf-8-sig").read().strip() if os.path.exists(src) else ""
            if not re.match(r"^[A-Za-z0-9_]+$", folder):
                problem(where, "a new sprite file's folder name may only use letters, digits and _")
                continue
            if not base or not asset_exists("Win/%s.osb" % base):
                problem(where, "this isn't a game sprite file; for a new one, copy a whole exported folder "
                               "(it has a %s saying which file it starts from)" % SOURCE_TXT)
                continue
        elif not pngs:
            continue
        b = sprites.load("Win/%s.osb" % base)
        full = not sprites.is_paletted(b)
        pal = None if full else sprite_palette(base)
        own = None if full else class_palette_for(folder)
        if own and pngs:
            print("  colours are matched to %s (the palette of the class using this body)" % own[0])
            pal = own[1]
        replace = os.path.exists(os.path.join(path, REPLACE_TXT))
        if pal is None and not full:
            problem(where, "%s uses a palette Build mod doesn't know; class bodies (pl0_cb00), weapons "
                           "(pl0_cw00) and full-colour pictures (com_pl_icon) can be edited" % base)
            continue
        print("Importing %d frame(s) into %s%s%s ..." % (len(pngs), asset,
                                                         "" if base == folder else " (new, from %s)" % base,
                                                         " (full colour)" if full else ""))
        if replace and pngs:
            print("  %s: each frame here replaces its whole picture (own tiles, nothing shared)" % REPLACE_TXT)
        new = sprites.import_frames(b, pngs, pal, report=lambda m: print("  note: " + m),
                                    replace=replace) if pngs else None
        tracks = os.path.join(path, TRACKS_TXT)
        if os.path.exists(tracks):
            new = apply_tracks(new or b, tracks, where) or new
        timing = os.path.join(path, TIMING_TXT)
        if os.path.exists(timing):
            new = apply_timing(new or b, timing, where) or new
        hitbox = os.path.join(path, HITBOX_TXT)
        if os.path.exists(hitbox):
            new = apply_hitboxes(new or b, hitbox, where) or new
        if base != folder:
            new, n = sprites.rename_self_refs(new or b, base + ".osb", folder + ".osb")
            if n:
                print("  note: %d reference(s) to %s.osb inside it now name %s.osb" % (n, base, folder))
        if new or base != folder:
            out[asset] = new or b
    return out


BASE_CLASSES = {"empress": 0, "warrior": 1, "shinobi": 2, "witch": 3, "oracle": 4, "bandit": 5}
# class picture spots without art show the empty picture (never the base class's art)
MISSING_ART = [("panel_art", "select panel"), ("highlight_art", "select highlight"),
               ("portrait_art", "small portrait"), ("portrait_large_art", "large portrait"),
               ("bar_face_art", "bar face"), ("hud_face_art", "quest HUD face"),
               ("hud_frame_art", "quest HUD life frame")]
EMPTY_PICTURES = [
    ("Win/com_pl_icon.osb", [("PlIcon_1", "MOD_EMPTY_PlIcon"), ("PlIcon_1_24", "MOD_EMPTY_PlIcon_24")]),
    ("Win/s_o0200.osb", [("PlFace_1", "MOD_EMPTY_PlFace")]),
    ("Win/cp_o0000.osb", [("皇女顔01", "MOD_EMPTY_HudFace_1"), ("皇女顔02", "MOD_EMPTY_HudFace_2"),
                           ("皇女ライフ", "MOD_EMPTY_HudFrame", "frame")]),
]


ART_COLUMNS = ("panel_art", "highlight_art", "portrait_art", "portrait_large_art", "bar_face_art",
               "hud_face_art", "hud_face2_art", "hud_frame_art")


def class_id(text):
    """A class's unique name as the loader keys it: letters, digits, '.', '_' and '-' only."""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text.strip()).strip("_").lower()


def collect_classes(sprite_files):
    """mod/classes.csv -> class definitions (no numbers: the combiner hands those out).

    Columns: id (optional, a unique name; defaults to the name), name, moves (which existing
    class's moves it borrows; 'base' is the old name for it), body, palette, quest_hud,
    status_hud, description, the *_art pictures, test_saves. A 'class' column (old test
    sheets) is kept only as a hint for carrying old saves over.
    """
    path = os.path.join(MOD, "classes.csv")
    if not os.path.exists(path):
        return []
    defs = []
    seen = set()
    for row in read_csv(path):
        where = "classes.csv line %d" % row["_line"]
        name = row.get("name", "").strip()
        cid = class_id(row.get("id", "") or name)
        if not cid:
            problem(where, "needs a name (or an id)")
            continue
        if cid in seen:
            problem(where, "two classes named %s; give each class its own name or id" % cid)
            continue
        seen.add(cid)
        moves = (row.get("moves", "") or row.get("base", "")).strip().lower() or "empress"
        if moves not in BASE_CLASSES:
            problem(where, "moves should be one of %s" % ", ".join(BASE_CLASSES))
            continue
        need = [c for c in ("body", "palette") if not row.get(c, "").strip()]
        if need or not name:
            problem(where, "%s needs %s (without them it would be invisible or unnamed)"
                    % (cid, " and ".join(need + ([] if name else ["name"]))))
            continue
        d = {"id": cid, "name": name, "moves": BASE_CLASSES[moves]}
        ok = True
        for field in ("body", "palette", "quest_hud", "status_hud"):  # sprite files (made in mod\sprites)
            v = re.sub(r"\.osb$", "", row.get(field, "").strip())
            if not v:
                continue
            asset = "Win/%s.osb" % v
            if asset not in sprite_files and not asset_exists(asset):
                problem(where, "%s %s isn't in the game or made by mod\\sprites / mod\\palettes" % (field, v))
                ok = False
                continue
            d[field] = v
        if row.get("description", "").strip():
            d["description"] = row["description"]
        if row.get("test_saves", "").strip().lower() in ("1", "yes", "true"):
            d["test_saves"] = True
        if row.get("class", "").strip().isdigit():
            d["was"] = int(row["class"])  # its number in older builds (carries old saves over)
        art = {}
        for col in ART_COLUMNS:
            v = row.get(col, "").strip()
            if not v:
                continue
            pth = v if os.path.isabs(v) else os.path.join(MOD, v)
            if not os.path.exists(pth):
                problem(where, "%s: no file %s" % (col, pth))
                ok = False
            else:
                art[col[:-4]] = pth  # panel, highlight, portrait, portrait_large, ...
        d["art"] = art
        missing = [what for col, what in MISSING_ART if not row.get(col, "").strip()]
        if missing:
            print("  %s has no %s: those spots show the empty picture" % (name, ", no ".join(missing)))
        if ok:
            defs.append(d)
    if defs:
        print("Classes: %d" % len(defs))
    return defs


def number_classes(defs):
    """Hand out class numbers. A class rides in the slot of the class whose moves it borrows
    (the game picks moves by slot), so it gets the first free page in that column: number
    6 * page + moves. The order is fixed (by id), so everyone with the same mods gets the same
    numbers. Returns the defs with "cls" set; problems if a column runs past class 127."""
    used = set()
    out = []
    for d in sorted(defs, key=lambda d: d["id"]):
        n = 6 + d["moves"]
        while n in used:
            n += 6
        if n > 127:
            problem("classes", "too many classes borrow the %s's moves (no free page left for %s)"
                    % ([k for k, v in BASE_CLASSES.items() if v == d["moves"]][0], d["id"]))
            continue
        used.add(n)
        out.append(dict(d, cls=n))
    return sorted(out, key=lambda d: d["cls"])


def apply_classes(defs, sprite_files, tables):
    """Numbered class definitions -> classes.txt lines; adds their names, descriptions and
    pictures to the shared game files (text tables, select art, portraits, faces)."""
    lines = []
    art = {}  # class -> {"panel": png, ..., "base": moves}
    for d in defs:
        cls, base = d["cls"], d["moves"]
        line = "%d base=%d id=%s" % (cls, base, d["id"])
        if d.get("was"):
            line += " was=%d" % d["was"]
        for field in ("body", "palette", "quest_hud", "status_hud"):
            if d.get(field):
                line += " %s=%s.osb" % (field, d[field])
        body, pal = d.get("body"), d.get("palette")
        if body and pal and "Win/%s.osb" % pal in sprite_files:
            # the game finds a class's palette by the hash of "<body>.osp" (see palettes.osp_id)
            sprite_files["Win/%s.osb" % pal] = palettes.set_id(sprite_files["Win/%s.osb" % pal], pal, sprite=body)
        if d.get("test_saves"):
            line += " test_saves=1"  # testing: the game's save writes go to SystemData.bin.class-test
        if d.get("description"):
            # select-screen info box: PLxx_INFO in sys_plselect.tb2, four pages ("|" separates
            # them in the sheet, \n is a line break); missing pages are the base class's.
            t = tables.setdefault("sys_plselect.tb2", vanilla_table("sys_plselect.tb2"))
            label, base_label = "PL%02d_INFO" % cls, "PL%02d_INFO" % base
            pages = [x.strip().replace("\\n", "\n") for x in d["description"].split("|")]
            t["rows"] = [r for r in t["rows"] if r["label"] != label]
            for r in [r for r in t["rows"] if r["label"] == base_label]:
                v = r["variant"]
                text = {lang: pages[v] for lang in t["langs"]} if v < len(pages) else dict(r["text"])
                t["rows"].append({"label": label, "variant": v, "aux": r["aux"], "text": text})
            line += " info=%s" % label
        art[cls] = dict(d.get("art", {}))  # every extra class's page gets a layout (with its page hint)
        art[cls]["base"] = base
        # the class's name in menus (CLASS_<nn>, like CLASS_00 = EMPRESS)
        label = "CLASS_%02d" % cls
        for table in ("sys_common.tb2", "sys_status_help.tb2"):
            t = tables.setdefault(table, vanilla_table(table))
            tpl = next((r for r in t["rows"] if r["label"] == "CLASS_00"), None)
            if not tpl:
                continue
            t["rows"] = [r for r in t["rows"] if r["label"] != label]
            t["rows"].append({"label": label, "variant": 0, "aux": tpl["aux"],
                              "text": {lang: d["name"] for lang in t["langs"]}})
        line += " name=%s" % label
        lines.append(line + " empty=1")
    if not art:
        return lines
    # character-select art: new pictures, highlight animations and per-page layouts in s_o0a00
    asset = "Win/s_o0a00.osb"
    try:
        new, highlights, pages = selectart.build(sprite_files.get(asset) or sprites.load(asset), art,
                                                 report=lambda m: print("  note: " + m))
    except ValueError as e:
        problem("classes", str(e))
        return lines
    sprite_files[asset] = new
    for i, line in enumerate(lines):
        cls = int(line.split()[0])
        if highlights.get(cls):
            line += " highlight=1"
        if pages.get(cls // 6):
            line += " pagelayout=1"
        lines[i] = line
    # portraits (com_pl_icon): small PlIcon_<n> (inn, lobby) and large PlIcon_<n>_24
    asset = "Win/com_pl_icon.osb"
    try:
        new, has = selectart.portraits(sprite_files.get(asset) or sprites.load(asset), art,
                                       report=lambda m: print("  note: " + m))
    except ValueError as e:
        problem("classes", str(e))
        return lines
    if has:
        sprite_files[asset] = new
        for i, line in enumerate(lines):
            cls = int(line.split()[0])
            if has.get(cls):
                lines[i] = line + " portrait=%d" % has[cls]  # 1 small, 2 large, 3 both
        print("Portraits: %d class(es)" % len(has))
    # the bar party menu's big face (s_o0200)
    asset = "Win/s_o0200.osb"
    try:
        new, bf = selectart.bar_faces(sprite_files.get(asset) or sprites.load(asset), art,
                                      report=lambda m: print("  note: " + m))
    except ValueError as e:
        problem("classes", str(e))
        return lines
    if bf:
        sprite_files[asset] = new
        for i, line in enumerate(lines):
            if bf.get(int(line.split()[0])):
                lines[i] = line + " barface=1"
    # the "empty" picture (the game's grey "no element" dash) for every class picture spot
    for asset, names in EMPTY_PICTURES:
        try:
            sprite_files[asset] = selectart.add_empties(sprite_files.get(asset) or sprites.load(asset), names,
                                                        report=lambda m: print("  note: " + m))
        except ValueError as e:
            problem("classes", "empty picture in %s: %s" % (asset, e))
            return lines
    # quest HUD faces (cp_o0000)
    asset = "Win/cp_o0000.osb"
    try:
        new, hf = selectart.hud_faces(sprite_files.get(asset) or sprites.load(asset), art,
                                      report=lambda m: print("  note: " + m))
    except ValueError as e:
        problem("classes", str(e))
        return lines
    if hf:
        sprite_files[asset] = new
        for i, line in enumerate(lines):
            if hf.get(int(line.split()[0])):
                lines[i] = line + " hudface=1"
        print("HUD faces: %d class(es)" % len(hf))
    # quest HUD life frames (cp_o0000)
    try:
        new, fr = selectart.hud_frames(sprite_files.get(asset) or sprites.load(asset), art,
                                       report=lambda m: print("  note: " + m))
    except ValueError as e:
        problem("classes", str(e))
        return lines
    if fr:
        sprite_files[asset] = new
        for i, line in enumerate(lines):
            if fr.get(int(line.split()[0])):
                lines[i] = line + " hudframe=1"
        print("HUD life frames: %d class(es)" % len(fr))
    print("Select screen art: %d class picture(s), %d page layout(s)" %
          (sum(1 for a in art.values() for k in ("panel", "highlight") if a.get(k)), len(pages)))
    # page prompt texts for the select screen ("RT Page 2/3"), drawn by the game from
    # PadInfo.tb2 like its Confirm / Return; the content mod picks MOD_PAGE_<page>
    npages = max(art) // 6 + 1
    t = tables.setdefault("PadInfo.tb2", vanilla_table("PadInfo.tb2"))
    tpl = next(r for r in t["rows"] if r["label"] == "RETURN")
    t["rows"] = [r for r in t["rows"] if not r["label"].startswith("MOD_PAGE_")]
    for page in range(npages):
        t["rows"].append({"label": "MOD_PAGE_%d" % (page + 1), "variant": 0, "aux": tpl["aux"],
                          "text": {lang: "Page %d/%d" % (page + 1, npages) for lang in t["langs"]}})
    return lines




def read_layout(sprite_files):
    """mod/layout.csv: file, anim, part, x, y, scale_x, scale_y, picture -> [[asset, anim, part, field, value]]."""
    path = os.path.join(MOD, "layout.csv")
    rows = []
    if not os.path.exists(path):
        return rows
    for row in read_csv(path):
        where = "layout.csv line %d" % row["_line"]
        f, part = row.get("file", ""), row.get("part", "")
        if not f or not part:
            problem(where, "needs file and part")
            continue
        asset = "Win/%s.osb" % re.sub(r"\.osb$", "", f)
        if asset not in sprite_files and not asset_exists(asset):
            problem(where, "no sprite file %s" % f)
            continue
        anim = None
        if row.get("anim"):
            try:
                anim = int(row["anim"])
            except ValueError:
                problem(where, "anim should be a number (or empty for all)")
                continue
        for field in ("x", "y", "scale_x", "scale_y", "picture"):
            if row.get(field):
                try:
                    v = float(row[field])
                except ValueError:
                    problem(where, "%s should be a number" % field)
                    continue
                rows.append([asset, anim, part, field, v])
    return rows


def apply_layout(rows, sprite_files, where="layout"):
    """Layout edits -> changes sprite_files in place."""
    edits = {}  # asset -> [(anim, part, field, value)]
    for asset, anim, part, field, v in rows:
        edits.setdefault(asset, []).append((anim, part, field, v))
    for asset, ed in edits.items():
        b = sprite_files.get(asset) or sprites.load(asset)
        new = layout.apply(b, ed, report=lambda m: problems.append("%s: %s" % (where, m[8:])))
        print("Layout: %d change(s) in %s" % (len(ed), asset))
        if new:
            sprite_files[asset] = new


def find_icons():
    """mod/icons/NN*.png -> {NN: path}; the file name must start with the icon number."""
    folder = os.path.join(MOD, "icons")
    found = {}
    if not os.path.isdir(folder):
        return found
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(".png"):
            continue
        m = re.match(r"(\d+)", f)
        if not m:
            problem("icons\\" + f, "the file name must start with the icon number, e.g. 16_herb.png")
            continue
        n = int(m.group(1))
        if n in found:
            problem("icons\\" + f, "two files for icon %d" % n)
            continue
        found[n] = os.path.join(folder, f)
    return found


def mod_info():
    """mod/mod.ini: the mod's name, version, author, description and folder name."""
    path = os.path.join(MOD, "mod.ini")
    if not os.path.exists(path):
        with open(path, "w", encoding="utf-8") as f:
            f.write("; About this mod. folder = its folder in the game's mods folder (letters, digits, _ and -).\n"
                    "; license = the licence you share the mod under, e.g. CC BY 4.0 or MIT (optional; the\n"
                    "; Mod Manager shows it). Your mod is yours: the kit's own licence doesn't cover it.\n"
                    "[mod]\nname=My Mod\nversion=1.0\nauthor=\ndescription=\nlicense=\nfolder=MyMod\n")
        print("Wrote mod\\mod.ini: set your mod's name there.")
    import configparser
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(path, encoding="utf-8-sig")
    info = dict(cp["mod"]) if cp.has_section("mod") else {}
    info.setdefault("name", "My Mod")
    info.setdefault("version", "1.0")
    folder = re.sub(r"[^A-Za-z0-9_-]+", "", info.get("folder", "") or info["name"])
    if not folder:
        problem("mod.ini", "folder should use letters, digits, _ or -")
    info["folder"] = folder
    return info


def text_changes(tables):
    """What the mod changed in each text table, compared with the game: added or changed rows."""
    out = []
    for fname, t in sorted(tables.items()):
        vanilla = {(r["label"], r["variant"]): r for r in vanilla_table(fname)["rows"]}
        for r in t["rows"]:
            v = vanilla.get((r["label"], r["variant"]))
            if v is None or v["text"] != r["text"] or v["aux"] != r["aux"]:
                out.append({"file": fname, "label": r["label"], "variant": r["variant"], "aux": r["aux"],
                            "text": r["text"]})
    return out


def write_package(info, package, assets, files):
    """build/package/<folder>: mod.ini + data/ (package.json, assets, pictures)."""
    import json
    import shutil
    out = os.path.join(ROOT, "build", "package", info["folder"])
    if os.path.isdir(out):
        shutil.rmtree(out)
    data = os.path.join(out, "data")
    os.makedirs(data)
    import zlib
    for asset, b in assets.items():  # compressed (art files are large unpacked)
        p = os.path.join(data, "assets", *asset.split("/")) + ".z"
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zlib.compress(b, 9))
    for rel, src in files.items():
        p = os.path.join(data, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        shutil.copyfile(src, p)
    package["assets"] = sorted(assets)
    # new files (not the game's own): the combiner gives them names of their own per mod
    package["new_assets"] = sorted(a for a in assets if not asset_exists(a))
    package["palettes"] = sorted(a for a in assets if a in edited_palettes)
    with open(os.path.join(data, "package.json"), "w", encoding="utf-8") as f:
        json.dump(package, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "mod.ini"), "w", encoding="utf-8") as f:
        f.write("; A data mod for the DMFD mod loader, made with Build mod.\n[mod]\n")
        for k in ("name", "version", "author", "description", "license"):
            f.write("%s=%s\n" % (k, info.get(k, "").replace("\n", " ")))
        f.write("enabled=1\n")
    return out


def install_package(out, folder):
    """Copy the package to <game>\\mods\\<folder> (keeping its enabled= setting)."""
    import shutil
    dst = os.path.join(GAME, "mods", folder)
    enabled = None
    ini = os.path.join(dst, "mod.ini")
    if os.path.exists(ini):
        for line in open(ini, encoding="utf-8-sig"):
            if line.strip().lower().startswith("enabled="):
                enabled = line.strip()[8:]
    if os.path.isdir(os.path.join(dst, "data")):
        shutil.rmtree(os.path.join(dst, "data"))
    shutil.copytree(out, dst, dirs_exist_ok=True)
    if enabled is not None:
        text = open(ini, encoding="utf-8").read().replace("enabled=1", "enabled=" + enabled)
        open(ini, "w", encoding="utf-8").write(text)
    return dst


def build():
    """mod/ -> a data mod package (build/package/<folder>), installed into the game's mods
    folder; then every switched-on data mod is combined into the game's DataMod."""
    info = mod_info()
    known = {it["label"] for it in itemdb.load_items() if it["label"]}
    tables = {}
    item_lines = build_items(tables, known)
    build_text(tables)
    icon_pngs = find_icons()
    palette_files = build_palettes()
    sprite_files = build_sprites()
    sprite_files.update(palette_files)
    layout_rows = read_layout(sprite_files)
    class_defs = collect_classes(sprite_files)
    for line in item_lines[1:]:  # weapon pictures and palettes must exist
        for tok in line.split():
            key, _, val = tok.partition("=")
            if key in ("sprite", "sprite_palette"):
                asset = palette_asset(val) if key == "sprite_palette" else "Win/" + val
                if asset not in sprite_files and not asset_exists(asset):
                    problems.append("items.csv: %s uses %s %s, which isn't in the game or in mod\\%s"
                                    % (line.split()[1] if line.startswith("new ") else line.split()[0],
                                       key, val[:-4], "palettes" if key == "sprite_palette" else "sprites"))
    for line in item_lines[1:]:  # icon numbers must exist
        for tok in line.split():
            if tok.startswith("icon="):
                n = int(tok[5:], 0)
                if n > 23 and n not in icon_pngs:
                    problems.append("items.csv: %s uses icon %d, but there is no mod\\icons\\%d_....png"
                                    % (line.split()[1] if line.startswith("new ") else line.split()[0], n, n))
    new_labels = {line.split()[1] for line in item_lines[1:] if line.startswith("new ")}
    drop_lines = build_drops(known | new_labels)
    if problems:
        print("Nothing was changed. Please fix these first:\n")
        for p in problems:
            print("  - " + p)
        return False

    files = {}  # path in data/ -> source picture
    for d in class_defs:  # the class pictures travel with the mod
        for k, src in list(d["art"].items()):
            rel = "art/%s/%s" % (d["id"], os.path.basename(src))
            files[rel] = src
            d["art"][k] = rel
    icon_list = []
    for n, src in sorted(icon_pngs.items()):
        rel = "icons/%s" % os.path.basename(src)
        files[rel] = src
        icon_list.append({"n": n, "png": rel})
    package = {
        "format": 1,
        "classes": class_defs,
        "text": text_changes(tables),
        "items": item_lines[1:],
        "drops": drop_lines[1:],
        "icons": icon_list,
        "layout": layout_rows,
    }
    out = write_package(info, package, sprite_files, files)
    print("Built %s %s: %d class(es), %d item line(s), %d text change(s), %d icon(s), %d chest slot(s), "
          "%d sprite/palette file(s)." % (info["name"], info["version"], len(class_defs), max(0, len(item_lines) - 1),
                                         len(package["text"]), len(icon_list), max(0, len(drop_lines) - 1),
                                         len(sprite_files)))
    print("Package to share: %s" % out)
    if PACKAGE_ONLY:  # making a release: don't touch the game
        return True
    dst = install_package(out, info["folder"])
    print("Installed in: %s" % dst)
    import combine_mods
    if not combine_mods.combine():
        return False
    print("Start the game to see the changes (restart it if it is running).")
    return True


def main(argv):
    global PACKAGE_ONLY, MOD
    PACKAGE_ONLY = "--package-only" in argv
    if "--mod" in argv:  # --mod <folder>: build another mod's sheets (default: mod\)
        i = argv.index("--mod")
        if i + 1 >= len(argv):
            print("--mod needs a folder")
            sys.exit(1)
        MOD = os.path.abspath(argv[i + 1])
    os.makedirs(MOD, exist_ok=True)
    try:
        if "--list" in argv:
            ok = list_items() or True
        elif "--export-sprites" in argv:
            ok = export_sprites() > 0
        elif "--export-weapons" in argv:
            ok = export_sprites(weapons=True) > 0
        else:
            ok = build()
    except FileNotFoundError as e:
        print("Could not find a game file: %s" % e.filename)
        ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main(sys.argv)
