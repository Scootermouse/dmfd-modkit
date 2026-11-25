r"""Start a new class mod: a folder laid out like mod/ with one class in classes.csv, a placeholder
look (the class whose moves it borrows, recolored), and its select / portrait / HUD pictures.
Then replace the placeholder art with your own and run Build mod on the folder.

    python -I tools/new_class.py <folder> "<Class name>" [--moves empress] [--hue 210]
                                 [--title "Mod name"] [--author name]

<folder>  where the mod's sheets go (made if missing; files already there are kept)
--moves   which class's moves it borrows: empress, warrior, shinobi, witch, oracle, bandit
--hue     how far to turn the placeholder colors (degrees), so it can't be mistaken for the original

Build it: tools\py.bat tools\build_mod.py --mod <folder>   (tools\py.bat uses the kit's own Python)
"""
import csv
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_test_classes as mtc  # noqa: E402  (rotate, save_mesh, art_for)
import osb, palettes, sprites  # noqa: E402

BASES = ["empress", "warrior", "shinobi", "witch", "oracle", "bandit"]
COLUMNS = ["id", "name", "moves", "body", "palette", "description", "panel_art", "highlight_art",
           "portrait_art", "portrait_large_art", "bar_face_art", "quest_hud", "status_hud",
           "hud_face_art", "hud_face2_art", "hud_frame_art", "notes"]


def arg(argv, name, default):
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else default


def main(argv):
    pos = [a for i, a in enumerate(argv[1:], 1) if not a.startswith("--") and not argv[i - 1].startswith("--")]
    if len(pos) < 2:
        print(__doc__)
        return 1
    folder, name = os.path.abspath(pos[0]), pos[1]
    moves = arg(argv, "--moves", "empress").lower()
    if moves not in BASES:
        print("--moves should be one of " + ", ".join(BASES))
        return 1
    k = BASES.index(moves)
    deg = float(arg(argv, "--hue", "210"))
    stem = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "newclass"
    body, pal = stem + "_cb00", stem + "_pal"
    mod_folder = re.sub(r"[^A-Za-z0-9_-]", "", os.path.basename(folder)) or "NewClass"

    os.makedirs(os.path.join(folder, "sprites", body), exist_ok=True)
    os.makedirs(os.path.join(folder, "palettes"), exist_ok=True)
    os.makedirs(os.path.join(folder, "select_art"), exist_ok=True)

    ini = os.path.join(folder, "mod.ini")
    if not os.path.exists(ini):
        open(ini, "w", encoding="utf-8").write(
            "; About this mod. folder = its folder in the game's mods folder (letters, digits, _ and -).\n"
            "[mod]\nname=%s\nversion=0.1\nauthor=%s\ndescription=Adds the %s class.\nfolder=%s\n"
            % (arg(argv, "--title", name), arg(argv, "--author", ""), name, mod_folder))

    # body: a copy of the borrowed class's (redraw its frames in sprites\<body>); palette: its, recolored
    open(os.path.join(folder, "sprites", body, "_source.txt"), "w").write("pl%d_cb00\n" % k)
    tmp = os.path.join(folder, "palettes", "_tmp.png")
    palettes.export_png(sprites.load("Win/pl%d_pal.osb" % k), tmp)
    w, h, img = osb.read_png_rgba(tmp)
    os.remove(tmp)
    osb.write_png(os.path.join(folder, "palettes", "%s from pl%d_pal.png" % (pal, k)), w, h, mtc.rotate(img, deg))

    # select / portrait / HUD pictures: the borrowed class's, recolored (mtc writes into its MOD)
    mtc.MOD = folder
    banks = [sprites.load("Win/%s.osb" % n) for n in ("s_o0a00", "com_pl_icon", "s_o0200", "cp_o0000")]
    art = mtc.art_for(0, k, deg, banks)
    renamed = {}
    for col, rel in art.items():
        new = rel.replace("select_art/test0_", "select_art/%s_" % stem)
        os.replace(os.path.join(folder, *rel.split("/")), os.path.join(folder, *new.split("/")))
        renamed[col] = new

    row = dict.fromkeys(COLUMNS, "")
    row.update({"id": stem, "name": name.upper(), "moves": moves, "body": body, "palette": pal,
                "description": "%s\n(Placeholder look: the\n%s, recolored.)" % (name.upper(), moves.capitalize()),
                "notes": "made by new_class.py (%s's moves, hue +%g); replace the art with your own" % (moves, deg)})
    row.update(renamed)
    path = os.path.join(folder, "classes.csv")
    rows = []
    if os.path.exists(path):
        rows = [r for r in csv.DictReader(io.StringIO(open(path, encoding="utf-8-sig").read()))
                if r.get("id") != stem]
    rows.append(row)
    out = io.StringIO()
    wr = csv.DictWriter(out, fieldnames=COLUMNS, extrasaction="ignore", lineterminator="\r\n")
    wr.writeheader()
    wr.writerows(rows)
    open(path, "w", encoding="utf-8-sig", newline="").write(out.getvalue())
    print("%s: class %s (%s's moves), body %s, palette %s, %d pictures.\nBuild it: tools\\py.bat tools\\build_mod.py --mod \"%s\""
          % (folder, name.upper(), moves, body, pal, len(renamed), folder))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
