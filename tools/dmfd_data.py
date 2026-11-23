"""Dragon Marked for Death DataHash reader.

File names:  DataHash/<md5(asset_name).hexdigest()>   e.g. md5("sys_item.tb2")
Container:   none - one asset per file.
Layers (flags passed to IceSStorageEng::IceReader::readMain, game 0xe854b0):
  bit0 = encrypted  (rolling 64-bit XOR keystream, key string per asset family)
  bit1 = deflated   (u32 LE uncompressed size, then a zlib stream)
Decrypt is applied to the whole file first, then inflate.

Usage:
  python -I dmfd_data.py scan  [DataHash dir]           classify every file
  python -I dmfd_data.py dump  <name-or-hash> [out]      decode one asset
  python -I dmfd_data.py all   <outdir> [DataHash dir]   decode everything
  python -I dmfd_data.py pack  <asset-name> <decoded-file> <outdir>
        writes <outdir>/<md5(name)> encoded for the game (never into DataHash)
"""
import hashlib
import json
import os
import struct
import sys
import zlib

GAME_DIR_NAME = "Dragon Marked For Death"


def find_game():
    """The game folder (with game.exe): the DMFD_GAME environment variable, else tools\\game.txt
    (the Mod Manager writes it when it sets up the modding tools), else Steam's libraries."""
    def ok(p):
        return p and os.path.isfile(os.path.join(p, "game.exe"))
    here = os.path.dirname(os.path.abspath(__file__))
    tries = [os.environ.get("DMFD_GAME", "").strip().strip('"')]
    try:
        tries.append(open(os.path.join(here, "game.txt"), encoding="utf-8-sig").read().strip().strip('"'))
    except OSError:
        pass
    for p in tries:
        if ok(p):
            return p
    steam = []
    try:
        import winreg
        for root, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(root, key) as k:
                    for name in ("SteamPath", "InstallPath"):
                        try:
                            steam.append(winreg.QueryValueEx(k, name)[0])
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    libs = []
    for s in steam:
        libs.append(s)
        try:  # other Steam libraries: "path" lines in libraryfolders.vdf
            import re
            vdf = open(os.path.join(s, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace").read()
            libs += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', vdf)]
        except OSError:
            pass
    for lib in libs + [r"C:\Program Files (x86)\Steam"]:
        p = os.path.join(lib, "steamapps", "common", GAME_DIR_NAME)
        if ok(p):
            return os.path.normpath(p)
    return ""


DEFAULT_DH = os.path.join(find_game() or os.path.join(r"C:\Program Files (x86)\Steam\steamapps\common", GAME_DIR_NAME), "DataHash")
M64 = (1 << 64) - 1
# Key strings found in the exe (.rdata 0x111c940.., 0x1036e64). Ordered by frequency.
KEYS = [b"obj90210", b"x4NKvf3U", b"scroll90210", b"set90210", b"snd90210",
        b"ssbpi90210", b"json180601", b""]


def file_name(asset):
    """Asset name -> DataHash file name (game 0xe82120: MD5 then lowercase hex)."""
    if isinstance(asset, str):
        asset = asset.encode()
    return hashlib.md5(asset).hexdigest()


def _seed(key):
    # game 0xe816d0; chars are sign-extended (movsx)
    k = 0xA1B34F58CAD705B2
    for c in key:
        k = ((k + (c - 256 if c >= 128 else c)) * 0x8D) & M64
    return k


def crypt(data, key, decrypt=True):
    """game 0xe81740. out = in ^ (k >> (i & 31)); k = (k + cipherbyte) * 0x8d."""
    k = _seed(key)
    out = bytearray(len(data))
    for i, b in enumerate(data):
        o = b ^ ((k >> (i & 31)) & 0xFF)
        out[i] = o
        c = b if decrypt else o          # feedback is always the ciphertext byte
        k = ((k + c) * 0x8D) & M64
    return bytes(out)


def encrypt(data, key):
    return crypt(data, key, decrypt=False)


def _try_inflate(buf, full=True):
    if len(buf) < 6 or buf[4] != 0x78 or ((buf[4] << 8) | buf[5]) % 31:
        return None
    size = struct.unpack_from("<I", buf)[0]
    try:
        if full:
            out = zlib.decompress(buf[4:])
            return out if len(out) == size else None
        z = zlib.decompressobj()
        z.decompress(buf[4:4 + 4096], 64)
        return b"ok"
    except zlib.error:
        return None


def sniff(head):
    """Name the inner format from the first bytes of decoded data."""
    sigs = [(b"OTTO", "otf"), (b"\x00\x01\x00\x00", "ttf"), (b"\x89PNG", "png"),
            (b"DDS ", "dds"), (b"OggS", "ogg"), (b"RIFF", "riff"), (b"{", "json"),
            (b"[", "json")]
    for s, n in sigs:
        if head.startswith(s):
            return n
    return head[:4].hex()


# Layer recipes per extension, as the game loads them.
#   "crypt+deflate": decrypt whole file, then u32 size + zlib   (readMain flags 3)
#   "crypt":         decrypt only                               (flags 1)
#   "deflate+crypt": u32 size + zlib, then decrypt the result   (UI layout .json)
#   "plain":         stored as-is (fonts, .ivc movies, sound banks)
RECIPES = {
    "osb": ("crypt+deflate", b"obj90210"),
    "tb2": ("crypt+deflate", b"x4NKvf3U"),
    "stb": ("crypt", b"set90210"),
    "json": ("deflate+crypt", b"xN5sUeRo"),
    "bin": ("plain", None),
    "ttf": ("plain", None),
    "ivc": ("plain", None),
}


def classify(raw):
    """Return (layers, key, inner_sniff) without full decode (fast)."""
    head = raw[:4096 + 8]
    if raw[:4] in (b"OTTO", b"\x00\x01\x00\x00"):
        return "plain", None, sniff(raw)
    if raw[:8] == bytes.fromhex("1000000040000000"):
        return "plain", None, "snd"      # sound bank, 48 kHz, codec not identified
    if raw[:4] == bytes.fromhex("28000100"):
        return "plain", None, "ivc"      # movie: 30 fps 1280x720 + 48 kHz audio
    if _try_inflate(head, full=False):
        inner = zlib.decompressobj().decompress(raw[4:4 + 256], 64)
        if crypt(inner[:2], b"xN5sUeRo") in (b"{\n", b"[\n", b"{\r"):
            return "deflate+crypt", b"xN5sUeRo", "json"
        return "deflate", None, None
    for k in KEYS:
        dec = crypt(head, k)
        if _try_inflate(dec, full=False):
            return "crypt+deflate", k, None
    # encrypted but not compressed: .stb stage files start with u32 0x14
    dec = crypt(raw[:16], b"set90210")
    if dec[:4] == b"\x14\x00\x00\x00":
        return "crypt", b"set90210", "stb"
    return "unknown", None, raw[:8].hex()


def decode(raw, layers=None, key=None):
    if layers is None:
        layers, key, _ = classify(raw)
    data = raw
    for step in layers.split("+"):
        if step == "crypt":
            data = crypt(data, key)
        elif step == "deflate":
            data = _try_inflate(data)
            if data is None:
                raise ValueError("inflate failed")
    return data, layers, key


def encode(data, layers, key, level=9):
    """Inverse of decode(): build a file the game will accept."""
    def deflate(x):
        return struct.pack("<I", len(x)) + zlib.compress(x, level)
    for step in reversed(layers.split("+")):
        if step == "crypt":
            data = encrypt(data, key)
        elif step == "deflate":
            data = deflate(data)
    return data


def encode_asset(name, data):
    ext = name.rsplit(".", 1)[-1].lower()
    layers, key = RECIPES[ext]
    return encode(data, layers, key)


def _resolve(arg, dh):
    if len(arg) == 32 and all(c in "0123456789abcdef" for c in arg):
        return arg, None
    return file_name(arg), arg


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "scan"
    if cmd == "scan":
        dh = argv[2] if len(argv) > 2 else DEFAULT_DH
        from collections import Counter
        cnt = Counter()
        rows = []
        for f in sorted(os.listdir(dh)):
            raw = open(os.path.join(dh, f), "rb").read()
            layers, key, inner = classify(raw)
            if inner is None:
                inner = sniff(decode(raw, layers, key)[0][:16])
            rows.append({"hash": f, "size": len(raw), "layers": layers,
                         "key": key.decode() if key is not None else None, "inner": inner})
            cnt[(layers, key, inner)] += 1
        for k, v in cnt.most_common():
            print(v, k)
        json.dump(rows, open("scan.json", "w"), indent=1)
        print("wrote scan.json")
    elif cmd == "dump":
        dh = DEFAULT_DH
        h, name = _resolve(argv[2], dh)
        raw = open(os.path.join(dh, h), "rb").read()
        data, layers, key = decode(raw)
        out = argv[3] if len(argv) > 3 else (name or h) + ".dec"
        open(out, "wb").write(data)
        print(h, layers, key, len(raw), "->", len(data), out)
    elif cmd == "all":
        outdir = argv[2]
        dh = argv[3] if len(argv) > 3 else DEFAULT_DH
        names = {}
        nf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "names.txt")
        if os.path.exists(nf):
            for line in open(nf, encoding="utf-8"):
                line = line.strip()
                if line:
                    names[file_name(line)] = line
        os.makedirs(outdir, exist_ok=True)
        for f in sorted(os.listdir(dh)):
            raw = open(os.path.join(dh, f), "rb").read()
            if len(raw) > 50_000_000:
                continue  # sound banks: copy not needed
            try:
                data, layers, key = decode(raw)
            except Exception as e:
                print("fail", f, e)
                continue
            if layers == "unknown":
                continue
            nm = names.get(f, f + "." + sniff(data[:16]))
            open(os.path.join(outdir, nm), "wb").write(data)
        print("done")
    elif cmd == "pack":
        name, src, outdir = argv[2], argv[3], argv[4]
        if os.path.abspath(outdir).lower() == os.path.abspath(DEFAULT_DH).lower():
            sys.exit("refusing to write into the game's DataHash folder")
        os.makedirs(outdir, exist_ok=True)
        out = os.path.join(outdir, file_name(name))
        open(out, "wb").write(encode_asset(name, open(src, "rb").read()))
        print(name, "->", out)
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
