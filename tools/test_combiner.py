"""Check the Mod Manager's built-in combiner against tools/combine_mods.py (the reference).

Makes a fake game folder in <work> (its DataHash is a link to the real game's, only read) with
test data mods: Test Classes from the game's mods folder (developers: make it with make_test_classes.py
and Build mod first; it isn't part of the kit) plus two made here that cover text,
items, drops, new and redrawn icons, layout edits, renamed sprite and palette files, classes
with partial art, and every kind of overlap. Both combiners run on it, then
compare_datamod.py compares the results by decoded content.

usage: python -I tools/test_combiner.py <work folder>     (build the manager first)
"""
import json
import os
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dmfd_data  # noqa: E402
import osb  # noqa: E402
import sprites  # noqa: E402
import tb2  # noqa: E402

REAL = os.path.dirname(dmfd_data.DEFAULT_DH)


def write_pkg(mods, folder, ini, pkg, assets, files):
    d = os.path.join(mods, folder)
    os.makedirs(os.path.join(d, "data"), exist_ok=True)
    with open(os.path.join(d, "mod.ini"), "w", encoding="utf-8") as f:
        f.write("[mod]\n" + "".join("%s=%s\n" % kv for kv in ini.items()))
    for name, b in assets.items():
        p = os.path.join(d, "data", "assets", *name.split("/")) + ".z"
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(zlib.compress(b, 9))
    for rel, src in files.items():
        p = os.path.join(d, "data", *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if isinstance(src, bytes):
            open(p, "wb").write(src)
        else:
            shutil.copy(src, p)
    pkg = dict(pkg, format=1, assets=sorted(assets))
    with open(os.path.join(d, "data", "package.json"), "w", encoding="utf-8") as f:
        json.dump(pkg, f, ensure_ascii=False, indent=1)


def png_bytes(w, h, rgba):
    tmp = os.path.join(os.environ.get("TEMP", "."), "_tmp_icon.png")
    osb.write_png(tmp, w, h, rgba)
    b = open(tmp, "rb").read()
    os.remove(tmp)
    return b


def make(fake):
    if os.path.exists(os.path.join(fake, "mods")):
        shutil.rmtree(os.path.join(fake, "mods"))
    os.makedirs(fake, exist_ok=True)
    if not os.path.exists(os.path.join(fake, "DataHash")):
        subprocess.check_call(["cmd", "/c", "mklink", "/J", os.path.join(fake, "DataHash"), dmfd_data.DEFAULT_DH])
    open(os.path.join(fake, "game.exe"), "wb").close()
    mods = os.path.join(fake, "mods")
    shutil.copytree(os.path.join(REAL, "mods", "TestClasses"), os.path.join(mods, "TestClasses"))
    art = os.path.join(REAL, "mods", "TestClasses", "data", "art", "t009_test_9_witch")
    pngs = {f.split("_m")[0].split("test9_")[1]: os.path.join(art, f) for f in os.listdir(art)}

    witch = sprites.load("Win/pl3_cb00.osb")
    witchcopy, n = sprites.rename_self_refs(witch, "pl3_cb00.osb", "witchcopy.osb")
    assert n, "pl3_cb00 should name itself"
    pal = sprites.load("Win/pl3_pal.osb")
    elic = bytearray(sprites.load("Win/com_elic.osb"))
    elic[-1] ^= 1
    items = tb2.parse(dmfd_data.decode(open(os.path.join(dmfd_data.DEFAULT_DH, dmfd_data.file_name("sys_item.tb2")), "rb").read())[0])
    label = items["rows"][5]["label"]
    langs = items["langs"]
    red = bytes((220, 20, 60, 255)) * 256
    blue = bytes((30, 60, 220, 255)) * 256

    write_pkg(mods, "Alpha", {"name": "Alpha Mod", "version": "1.0", "enabled": "1"}, {
        "new_assets": ["Win/witchcopy.osb", "Win/witchcopy_pal.osb"],
        "palettes": ["Win/witchcopy_pal.osb"],
        "text": [{"file": "sys_item.tb2", "label": label, "variant": 0, "aux": "", "text": {l: "Alpha's text" for l in langs}},
                 {"file": "sys_item.tb2", "label": "MOD_ALPHA_NEW", "variant": 0, "aux": "", "text": {l: "Brand new" for l in langs}}],
        "items": ["new MOD_ALPHA_SWORD price=100 icon=24 sprite=witchcopy sprite_palette=Win/witchcopy_pal.osb",
                  "%s price=0x20 icon=3" % label],
        "drops": ["chest01 MOD_ALPHA_SWORD 5", "chest02 POTION 1"],
        "icons": [{"n": 24, "png": "icons/24_red.png"}, {"n": 3, "png": "icons/03_axe.png"}],
        "layout": [["Win/s_o0a00.osb", None, "PL1", "x", 12.5], ["Win/witchcopy.osb", 0, "8d154d2d", "picture", 3]],
        "classes": [
            {"id": "alpha_witch", "name": "ALPHA WITCH", "moves": 3, "body": "witchcopy", "palette": "witchcopy_pal",
             "description": "Page one\\nline two|Page two", "test_saves": True,
             "art": {"panel": "art/a/" + os.path.basename(pngs["panel"]), "portrait": "art/a/" + os.path.basename(pngs["portrait"]),
                     "hud_frame": "art/a/" + os.path.basename(pngs["hud_frame"])}},
            {"id": "shared_one", "name": "ALPHA SHARED", "moves": 0, "body": "pl0_cb00", "palette": "pl0_pal", "art": {}},
            {"id": "zz_bandit", "name": "LATE BANDIT", "moves": 5, "body": "pl5_cb00", "palette": "pl5_pal", "was": 0,
             "art": {"bar_face": "art/a/" + os.path.basename(pngs["bar_face"]), "hud_face": "art/a/" + os.path.basename(pngs["hud_face1"])}},
        ],
    }, {"Win/witchcopy.osb": witchcopy, "Win/witchcopy_pal.osb": pal, "Win/com_elic.osb": bytes(elic)},
        {"icons/24_red.png": png_bytes(16, 16, red), "icons/03_axe.png": png_bytes(16, 16, blue),
         **{"art/a/" + os.path.basename(p): p for p in pngs.values()}})

    write_pkg(mods, "beta", {"name": "Beta", "version": "2"}, {
        "text": [{"file": "sys_item.tb2", "label": label, "variant": 0, "aux": "", "text": {l: "Beta's text" for l in langs}}],
        "items": ["%s price=7" % label],
        "drops": ["chest01 MOD_ALPHA_SWORD 9"],
        "icons": [{"n": 24, "png": "icons/24_blue.png"}],
        "classes": [{"id": "shared_one", "name": "BETA SHARED", "moves": 0, "body": "pl0_cb00", "palette": "pl0_pal", "art": {}}],
    }, {"Win/com_elic.osb": bytes(elic)}, {"icons/24_blue.png": png_bytes(16, 16, blue)})

    write_pkg(mods, "Gamma_off", {"name": "Switched off", "enabled": "0"}, {"text": []}, {}, {})
    with open(os.path.join(mods, "order.txt"), "w", encoding="utf-8") as f:
        f.write("; test order\nbeta\nTestClasses\n")


def run_python(fake, out):
    dmfd_data.DEFAULT_DH = os.path.join(fake, "DataHash")
    import build_mod as bm
    import combine_mods as cm
    bm.GAME = fake
    bm.OUTS = [out]
    cm.MODS = os.path.join(fake, "mods")
    return cm.combine()


MANAGER = os.path.join(os.path.dirname(HERE), "manager", "build", "DMFD Mod Manager.exe")


def main(work):
    import compare_datamod
    fake, py, cs = (os.path.join(work, x) for x in ("fake", "out_python", "out_manager"))
    for d in (py, cs):
        if os.path.exists(d):
            shutil.rmtree(d)
    make(fake)
    print("python combiner:")
    if not run_python(fake, py):
        return False
    print("manager:")
    r = subprocess.run([MANAGER, "--game", fake, "--combine-to", cs])
    log = open(os.path.join(fake, "ModManager", "manager.log"), encoding="utf-8").read().splitlines()
    print("\n".join(l for l in log[-40:] if "combine" in l and "class " not in l))
    if r.returncode != 0:
        return False
    return compare_datamod.main(py, cs)


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv[1]) else 1)
