r"""List a sprite file's animations: number (the aNNN in exported frame names), name hash, length
in frames, picture keys, and the name when known (anim_names.txt: the class bodies', weapons' and
effects' animation names, read from the game program).

    tools\py.bat tools\anim_names.py pl0_cb00          the Empress's body
    tools\py.bat tools\anim_names.py pl0_cw00 剣_通常   only names containing 剣_通常

The hash is what _offsets.txt, _timing.txt and _hitboxes.txt take as 0x<hash>; it is the same in a
body and its weapon files, while the numbers differ.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sprites  # noqa: E402


def load_names():
    names = {}
    path = os.path.join(HERE, "anim_names.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or "\t" not in line:
                continue
            h, n = line.rstrip("\n").split("\t", 1)
            names[int(h, 16)] = n
    return names


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    name = argv[1]
    asset = name if name.lower().endswith(".osb") else "Win/%s.osb" % name
    b = sprites.load(asset)
    names = load_names()
    filt = argv[2] if len(argv) > 2 else None
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    print("%s: %d animations" % (asset, len(sprites.animations(b))))
    print("  n    hash        length  keys  name")
    for a in sprites.animations(b):
        n = names.get(a["hash"], "")
        if filt and filt not in n:
            continue
        print("a%03d  0x%08x  %6g  %4d  %s" % (a["index"], a["hash"], a["length"], len(sprites.frames(a)), n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
