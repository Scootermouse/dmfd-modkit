"""Make the player zip: dist/DMFD-ModKit-<version>.zip, laid out like the game folder.

Build everything first (tools\\build_dll.bat, tools\\build_codemods.bat, manager\\build.bat) and
the data mod packages with Build mod (python -I tools/build_mod.py --package-only makes
build/package/<folder> without touching the game). The version is the loader's (src/content.h).
The zip holds no saves and no DataMod: the manager prepares that on the player's machine.

usage: tools\py.bat tools/make_release.py [data mod package folder ...]
       tools\py.bat tools/make_release.py --tools      (only the modding tools zip)

Makes, in dist/, everything a GitHub release of version <v> holds (upload them all as its files):
- mods/<Mod>-<version>.zip, one per mod, to install one at a time;
- DMFD-ModdingTools-<v>.zip: the .bat files, the tools, docs, and the portable Python in
  tools/python (unpack python.org's "Windows embeddable package (64-bit)", 3.11, there first; it
  isn't in git);
- mods.json, the mod list the Mod Manager reads (Browse page, "Get the modding tools"). Its links
  point at this release's files on GitHub (RELEASE_URL), with their sizes and SHA-256; a copy also
  goes into the player zip next to the Manager, which reads it when no other list is set
  (docs/mod-list.md);
- DMFD-ModKit-<v>.zip, the player kit, laid out like the game folder;
- DMFD-ModKit-<v>-source.zip, the source of everything (the files git tracks), as the GPL asks.
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE_MODS = ["Rebalance", "QuickSpells", "SkipQuit", "HolyUp"]
# where a release's files are downloaded from (GitHub: the files attached to release v<version>)
RELEASE_URL = "https://github.com/Scootermouse/dmfd-modkit/releases/download/v%s/"


def version():
    m = re.search(r'CONTENT_VERSION "([^"]+)"', open(os.path.join(ROOT, "src", "content.h"), encoding="utf-8").read())
    return m.group(1)


def main(packages):
    ver = version()
    files = {  # path in the zip -> source
        "winmm.dll": os.path.join(ROOT, "build", "winmm.dll"),
        "DMFD Mod Manager.exe": os.path.join(ROOT, "manager", "build", "DMFD Mod Manager.exe"),
        "LICENSE.txt": os.path.join(ROOT, "LICENSE"),
    }
    for name in CODE_MODS:
        files["mods/%s/%s.dll" % (name, name)] = os.path.join(ROOT, "build", "mods", name, name + ".dll")
        files["mods/%s/mod.ini" % name] = os.path.join(ROOT, "codemods", name, "mod.ini")
    for pkg in packages:
        folder = os.path.basename(os.path.normpath(pkg))
        for dirpath, _, names in os.walk(pkg):
            for n in names:
                src = os.path.join(dirpath, n)
                files["mods/%s/%s" % (folder, os.path.relpath(src, pkg).replace(os.sep, "/"))] = src
    missing = [s for s in files.values() if not os.path.isfile(s)]
    if missing:
        sys.exit("missing (build it first): " + ", ".join(missing))
    if os.path.isfile(os.path.join(ROOT, "tools", "python", "python.exe")):
        tools_zip()  # first: the mod list names it
    mod_zips(files)  # writes dist/mods.json
    files["mods.json"] = os.path.join(ROOT, "dist", "mods.json")
    readme = open(os.path.join(ROOT, "dist", "README-kit.txt"), encoding="utf-8").read().replace("{version}", ver)
    out = os.path.join(ROOT, "dist", "DMFD-ModKit-%s.zip" % ver)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("README.txt", readme.replace("\n", "\r\n").encode("utf-8-sig"))
        for arc, src in sorted(files.items()):
            z.write(src, arc)
    print("%s: %d files, %.1f MB" % (out, len(files) + 1, os.path.getsize(out) / 1e6))
    source_zip(ver)


def source_zip(ver):
    """dist/DMFD-ModKit-<version>-source.zip: every file git tracks (the kit's complete source)."""
    import subprocess
    names = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True, check=True).stdout.splitlines()
    out = os.path.join(ROOT, "dist", "DMFD-ModKit-%s-source.zip" % ver)
    n = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in names:
            if f and os.path.isfile(os.path.join(ROOT, f)):
                z.write(os.path.join(ROOT, f), "DMFD-ModKit-%s/%s" % (ver, f))
                n += 1
    print("%s: %d files, %.1f MB" % (out, n, os.path.getsize(out) / 1e6))


def mod_zips(files):
    """dist/mods/<folder>-<version>.zip per mod (its folder inside), for installing one at a time
    with the Mod Manager (drop it on the window)."""
    import configparser
    folders = sorted({arc.split("/")[1] for arc in files if arc.startswith("mods/") and arc.count("/") >= 2})
    outdir = os.path.join(ROOT, "dist", "mods")
    os.makedirs(outdir, exist_ok=True)
    entries = []
    for folder in folders:
        mine = {arc: src for arc, src in files.items() if arc.startswith("mods/%s/" % folder)}
        cp = configparser.ConfigParser(interpolation=None, strict=False)
        ini = mine["mods/%s/mod.ini" % folder]
        cp.read(ini, encoding="utf-8-sig")
        ver = cp.get("mod", "version", fallback="0")
        out = os.path.join(outdir, "%s-%s.zip" % (folder, ver))
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for arc, src in sorted(mine.items()):
                z.write(src, arc[len("mods/"):])
        print("  %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))
        entries.append((folder, cp, mine, out))
    mod_list(entries)


# What the mod list says about each mod beyond its mod.ini (tags, a picture from its files, a page
# to discuss it). Mods made by others will bring this with their submission.
LISTING = {
    "Rebalance": {"tags": ["gameplay", "difficulty"]},
    "QuickSpells": {"tags": ["gameplay", "quality of life"]},
    "SkipQuit": {"tags": ["quality of life"]},
    "HolyUp": {"tags": ["gameplay", "classes"]},
}


def fnv_hex(path):
    """The loader's fingerprint of a data mod: FNV-1a of its package.json."""
    h = 2166136261
    for b in open(path, "rb").read():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return "%08x" % h


def mod_list(entries):
    """dist/mods.json: the approved mod list the Mod Manager's Browse page reads (format: see
    manager/src/Browse.cs). Versions already in it are kept (older ones stay installable)."""
    import datetime
    import hashlib
    import json
    import shutil
    path = os.path.join(ROOT, "dist", "mods.json")
    old, prev_list = {}, {}
    if os.path.exists(path):
        prev_list = json.load(open(path, encoding="utf-8"))
        old = {m["id"]: m for m in prev_list.get("mods", [])}
    mods = []
    today = datetime.date.today().isoformat()
    for folder, cp, mine, zpath in entries:
        get = lambda k, d="": cp.get("mod", k, fallback=d).strip()  # noqa: E731
        data = any(a.startswith("mods/%s/data/" % folder) for a in mine)
        code = bool(get("dll"))
        info = LISTING.get(folder, {})
        ver = {"version": get("version", "0"), "date": today, "url": RELEASE_URL % version() + os.path.basename(zpath),
               "size": os.path.getsize(zpath), "sha256": hashlib.sha256(open(zpath, "rb").read()).hexdigest()}
        if data:
            ver["fingerprint"] = fnv_hex(mine["mods/%s/data/package.json" % folder])
        prev = old.get(folder, {})
        versions = [ver] + [v for v in prev.get("versions", []) if v.get("version") != ver["version"]]
        entry = {"id": folder, "name": get("name", folder), "author": get("author"), "summary": get("description"),
                 "description": prev.get("description", ""), "tags": info.get("tags", prev.get("tags", [])),
                 "type": "code+data" if code and data else "code" if code else "data",
                 "online": "same" if data else (get("online", "same").lower() or "same"),
                 "license": get("license"), "homepage": prev.get("homepage", ""), "images": prev.get("images", []),
                 "versions": versions}
        if info.get("image"):
            src = mine.get("mods/%s/%s" % (folder, info["image"]))
            if src:
                os.makedirs(os.path.join(ROOT, "dist", "mods", "images"), exist_ok=True)
                shutil.copyfile(src, os.path.join(ROOT, "dist", "mods", "images", folder + ".png"))
                entry["images"] = ["mods/images/%s.png" % folder]
        mods.append(entry)
    with open(path, "w", encoding="utf-8") as f:
        listing = {"format": 1, "updated": today, "mods": mods}
        if "tools" in prev_list:  # the modding tools entry (written by --tools) is kept
            listing["tools"] = prev_list["tools"]
        json.dump(listing, f, ensure_ascii=False, indent=1)
    print("  %s (%d mod(s))" % (path, len(mods)))


DEV_TOOLS = {"gen_winmm_proxy.py", "re_gen_class_sites.py", "make_fresh_templates.py", "make_release.py",
             "test_combiner.py", "compare_datamod.py", "make_test_classes.py"}  # for working on the kit itself, not for modders


def tools_zip():
    """The modder download: everything Build mod and the other .bat files need, Python included."""
    if not os.path.isfile(os.path.join(ROOT, "tools", "python", "python.exe")):
        sys.exit("tools/python is missing: unpack python.org's Windows embeddable package (64-bit) there")
    files = {}
    for n in os.listdir(ROOT):
        if n.endswith(".bat"):
            files[n] = os.path.join(ROOT, n)
    for n in os.listdir(os.path.join(ROOT, "tools")):
        if (n.endswith(".py") and n not in DEV_TOOLS) or n in ("py.bat", "anim_names.txt", "names.txt"):
            files["tools/" + n] = os.path.join(ROOT, "tools", n)
    for rel in (os.path.join("tools", "python"), "docs"):
        for dirpath, dirs, names in os.walk(os.path.join(ROOT, rel)):
            dirs[:] = [d for d in dirs if d not in ("__pycache__", "images")]  # docs/images: README screenshots
            for n in names:
                src = os.path.join(dirpath, n)
                files[os.path.relpath(src, ROOT).replace(os.sep, "/")] = src
    for n, arc in (("README.md", "README.md"), ("FORMAT.md", "FORMAT.md"), ("CHANGELOG.md", "CHANGELOG.md"), ("LICENSE", "LICENSE.txt")):
        files[arc] = os.path.join(ROOT, n)
    out = os.path.join(ROOT, "dist", "DMFD-ModdingTools-%s.zip" % version())
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, src in sorted(files.items()):
            z.write(src, arc)
    print("%s: %d files, %.1f MB" % (out, len(files), os.path.getsize(out) / 1e6))
    # the Mod Manager's "Get the modding tools" (Build a mod page) downloads it through the list
    import datetime
    import hashlib
    import json
    path = os.path.join(ROOT, "dist", "mods.json")
    listing = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {"format": 1, "mods": []}
    listing["tools"] = {"version": version(), "date": datetime.date.today().isoformat(),
                        "url": RELEASE_URL % version() + os.path.basename(out), "size": os.path.getsize(out),
                        "sha256": hashlib.sha256(open(out, "rb").read()).hexdigest()}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(listing, f, ensure_ascii=False, indent=1)
    print("  %s (tools %s)" % (path, version()))


if __name__ == "__main__":
    if "--tools" in sys.argv:
        tools_zip()
    elif any(a.startswith("--") for a in sys.argv[1:]):
        sys.exit(__doc__)
    else:
        main(sys.argv[1:])
