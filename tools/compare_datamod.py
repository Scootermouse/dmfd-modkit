"""Compare two DataMod folders file by file, by decoded content (the game's files are
compressed, and two zlib builds needn't compress alike). Used to check the Mod Manager's
built-in combiner against tools/combine_mods.py.

usage: python -I tools/compare_datamod.py <folder A> <folder B>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmfd_data  # noqa: E402


def content(path):
    raw = open(path, "rb").read()
    name = os.path.basename(path)
    if len(name) != 32 or any(c not in "0123456789abcdef" for c in name):
        return raw  # items.txt, classes.txt, combined.txt ...
    return dmfd_data.decode(raw)[0]


def main(a, b):
    fa, fb = set(os.listdir(a)), set(os.listdir(b))
    bad = 0
    for f in sorted(fa ^ fb):
        print("only in %s: %s" % (a if f in fa else b, f))
        bad += 1
    for f in sorted(fa & fb):
        ca, cb = content(os.path.join(a, f)), content(os.path.join(b, f))
        if ca != cb:
            first = next((i for i in range(min(len(ca), len(cb))) if ca[i] != cb[i]), min(len(ca), len(cb)))
            print("differ: %s (%d vs %d bytes, first difference at %#x)" % (f, len(ca), len(cb), first))
            bad += 1
    print("%d file(s) compared, %d difference(s)" % (len(fa | fb), bad))
    return bad == 0


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv[1], sys.argv[2]) else 1)
