// SPDX-License-Identifier: GPL-3.0-or-later
// Combining data mods into the game's DataMod folder, built into the manager so players need no
// Python. A port of tools/combine_mods.py and the helpers it uses (dmfd_data, tb2, osb, osbedit,
// sprites, selectart, palettes, icons, layout); the Python tools stay the reference: the same mods
// give the same DataMod files (compare decoded, see tools/compare_datamod.py).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace DmfdManager.Data
{
    // Python's ValueError: a problem with the mods' data, reported to the player.
    class DataError : Exception { public DataError(string m) : base(m) { } }

    static class Bin
    {
        public static uint U(byte[] b, int o) { return BitConverter.ToUInt32(b, o); }
        public static int I(byte[] b, int o) { return (int)BitConverter.ToUInt32(b, o); }
        public static float F(byte[] b, int o) { return BitConverter.ToSingle(b, o); }
        public static void PutU(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
        public static void PutI(byte[] b, int o, int v) { PutU(b, o, (uint)v); }
        public static void PutF(byte[] b, int o, float v) { Buffer.BlockCopy(BitConverter.GetBytes(v), 0, b, o, 4); }
        public static int Mod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }  // Python's %
        public static int FloorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
        public static byte[] Slice(byte[] b, int from, int to) { var r = new byte[to - from]; Buffer.BlockCopy(b, from, r, 0, to - from); return r; }
        public static byte[] Copy(byte[] b) { return (byte[])b.Clone(); }
        public static string Hex(byte[] h) { var s = new StringBuilder(); foreach (byte x in h) s.Append(x.ToString("x2")); return s.ToString(); }

        // The game's string hash (0x7A3220): signed bytes of the UTF-8 text.
        public static uint Ice(string s) { return Ice(Encoding.UTF8.GetBytes(s)); }
        public static uint Ice(byte[] s)
        {
            uint h = 0xCDE723A5;
            unchecked { foreach (byte c in s) h = (h + (uint)(sbyte)c) * 0x8D; }
            return h;
        }

        // Python's int(text, 0): decimal, or 0x / 0o / 0b prefixed.
        public static int ParseInt0(string s)
        {
            string t = s.Trim().Replace("_", "");
            bool neg = t.StartsWith("-");
            if (neg || t.StartsWith("+")) t = t.Substring(1);
            int v;
            string low = t.ToLowerInvariant();
            if (low.StartsWith("0x")) v = Convert.ToInt32(t.Substring(2), 16);
            else if (low.StartsWith("0o")) v = Convert.ToInt32(t.Substring(2), 8);
            else if (low.StartsWith("0b")) v = Convert.ToInt32(t.Substring(2), 2);
            else if (t.Length > 1 && t.TrimStart('0') != "" && t[0] == '0') throw new FormatException("invalid number " + s);
            else v = int.Parse(t, System.Globalization.CultureInfo.InvariantCulture);
            return neg ? -v : v;
        }
    }

    // A growing byte array.
    class Buf
    {
        byte[] d = new byte[256];
        public int Len;
        public Buf() { }
        public Buf(byte[] start) { Add(start); }
        void Room(int n) { if (Len + n > d.Length) Array.Resize(ref d, Math.Max(d.Length * 2, Len + n)); }
        public void Add(byte[] b) { Add(b, 0, b.Length); }
        public void Add(byte[] b, int off, int n) { Room(n); Buffer.BlockCopy(b, off, d, Len, n); Len += n; }
        public void Byte(byte x) { Room(1); d[Len++] = x; }
        public void U(uint v) { Room(4); Bin.PutU(d, Len, v); Len += 4; }
        public void I(int v) { U((uint)v); }
        public void F(float v) { Room(4); Bin.PutF(d, Len, v); Len += 4; }
        public void U16(int v) { if (v < 0 || v > 0xFFFF) throw new DataError("too many vertices in one picture"); Room(2); d[Len++] = (byte)v; d[Len++] = (byte)(v >> 8); }
        public void Zeros(int n) { if (n < 0) throw new DataError("sprite file layout overlaps"); Room(n); Array.Clear(d, Len, n); Len += n; }
        public byte[] ToArray() { var r = new byte[Len]; Buffer.BlockCopy(d, 0, r, 0, Len); return r; }
        public uint GetU(int o) { return Bin.U(d, o); }
        public void PutU(int o, uint v) { Bin.PutU(d, o, v); }
    }

    // Byte arrays compared by content (identical tiles are stored once).
    class BytesKey : IEqualityComparer<byte[]>
    {
        public static readonly BytesKey Instance = new BytesKey();
        public bool Equals(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }
        public int GetHashCode(byte[] a) { unchecked { int h = 17; foreach (byte x in a) h = h * 31 + x; return h; } }
    }

    static class Zlib
    {
        // zlib stream (2-byte header, deflate, adler32) -> data
        public static byte[] Decompress(byte[] z, int off = 0)
        {
            using (var ms = new MemoryStream(z, off + 2, z.Length - off - 2))
            using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
            using (var o = new MemoryStream())
            {
                ds.CopyTo(o);
                return o.ToArray();
            }
        }

        public static byte[] Compress(byte[] data)
        {
            using (var o = new MemoryStream())
            {
                o.WriteByte(0x78); o.WriteByte(0xDA);
                using (var ds = new DeflateStream(o, CompressionLevel.Optimal, true)) ds.Write(data, 0, data.Length);
                uint a = 1, b = 0;
                for (int i = 0; i < data.Length;)
                {
                    int n = Math.Min(5552, data.Length - i);
                    for (int k = 0; k < n; k++) { a += data[i++]; b += a; }
                    a %= 65521; b %= 65521;
                }
                uint ad = (b << 16) | a;
                o.WriteByte((byte)(ad >> 24)); o.WriteByte((byte)(ad >> 16)); o.WriteByte((byte)(ad >> 8)); o.WriteByte((byte)ad);
                return o.ToArray();
            }
        }
    }

    // DataHash files (tools/dmfd_data.py): md5 file names, the rolling XOR cipher, u32 size + zlib.
    static class DataHash
    {
        public static string Dir;  // <game>\DataHash
        static readonly Dictionary<string, byte[]> vanilla = new Dictionary<string, byte[]>();

        public static string FileName(string asset)
        {
            using (var md5 = MD5.Create()) return Bin.Hex(md5.ComputeHash(Encoding.UTF8.GetBytes(asset)));
        }

        static ulong Seed(byte[] key)
        {
            ulong k = 0xA1B34F58CAD705B2;
            unchecked { foreach (byte c in key) k = (k + (ulong)(long)(sbyte)c) * 0x8D; }
            return k;
        }

        public static byte[] Crypt(byte[] data, byte[] key, bool decrypt)
        {
            ulong k = Seed(key);
            var o = new byte[data.Length];
            unchecked
            {
                for (int i = 0; i < data.Length; i++)
                {
                    byte b = data[i];
                    byte x = (byte)(b ^ (byte)(k >> (i & 31)));
                    o[i] = x;
                    k = (k + (decrypt ? b : x)) * 0x8D;  // the feedback is always the ciphertext byte
                }
            }
            return o;
        }

        static readonly string[] Keys = { "obj90210", "x4NKvf3U", "scroll90210", "set90210", "snd90210", "ssbpi90210", "json180601", "" };

        static string[] Recipe(string name)
        {
            string ext = name.Substring(name.LastIndexOf('.') + 1).ToLowerInvariant();
            switch (ext)
            {
                case "osb": return new[] { "crypt+deflate", "obj90210" };
                case "tb2": return new[] { "crypt+deflate", "x4NKvf3U" };
                case "stb": return new[] { "crypt", "set90210" };
                case "json": return new[] { "deflate+crypt", "xN5sUeRo" };
                case "bin": case "ttf": case "ivc": return new[] { "plain", null };
            }
            throw new DataError("don't know how the game stores ." + ext + " files");
        }

        static byte[] Inflate(byte[] buf)
        {
            if (buf.Length < 6 || buf[4] != 0x78 || ((buf[4] << 8) | buf[5]) % 31 != 0) return null;
            try
            {
                var o = Zlib.Decompress(buf, 4);
                return o.Length == Bin.U(buf, 0) ? o : null;
            }
            catch (InvalidDataException) { return null; }
        }

        static byte[] Deflate(byte[] x)
        {
            var b = new Buf();
            b.U((uint)x.Length);
            b.Add(Zlib.Compress(x));
            return b.ToArray();
        }

        public static byte[] Encode(string name, byte[] data)
        {
            var r = Recipe(name);
            var steps = r[0].Split('+');
            for (int i = steps.Length - 1; i >= 0; i--)
                if (steps[i] == "crypt") data = Crypt(data, Encoding.ASCII.GetBytes(r[1]), false);
                else if (steps[i] == "deflate") data = Deflate(data);
            return data;
        }

        public static byte[] Decode(string name, byte[] raw)
        {
            var r = Recipe(name);
            if (r[0] == "crypt+deflate")
            {
                var d = Inflate(Crypt(raw, Encoding.ASCII.GetBytes(r[1]), true));
                foreach (string k in Keys)  // the usual key didn't fit: try the others, as dmfd_data.classify does
                {
                    if (d != null) break;
                    d = Inflate(Crypt(raw, Encoding.ASCII.GetBytes(k), true));
                }
                if (d == null) throw new DataError(name + ": the game's file couldn't be read");
                return d;
            }
            var data = raw;
            foreach (string step in r[0].Split('+'))
                if (step == "crypt") data = Crypt(data, Encoding.ASCII.GetBytes(r[1]), true);
                else if (step == "deflate") { data = Inflate(data); if (data == null) throw new DataError(name + ": the game's file couldn't be read"); }
            return data;
        }

        // The game's own copy of an asset, decoded (kept: they're read several times). Never changed.
        public static byte[] Vanilla(string asset)
        {
            byte[] d;
            lock (vanilla)
                if (vanilla.TryGetValue(asset, out d)) return d;
            string p = Path.Combine(Dir, FileName(asset));
            if (!File.Exists(p)) throw new DataError("the game has no file " + asset);
            d = Decode(asset, File.ReadAllBytes(p));
            lock (vanilla) vanilla[asset] = d;
            return d;
        }

        // Lets go of the decoded game files (they're tens of megabytes).
        public static void Forget() { lock (vanilla) vanilla.Clear(); }

        public static bool Exists(string asset) { return File.Exists(Path.Combine(Dir, FileName(asset))); }
    }

    // ---- text tables (tools/tb2.py) ----
    class TbRow
    {
        public string Label, Aux;
        public int Variant;
        public Dictionary<string, string> Text;  // language -> text (null = none)
    }

    class TbTable
    {
        public uint Version;
        public List<string> Langs = new List<string>();
        public List<TbRow> Rows = new List<TbRow>();
    }

    static class Tb2
    {
        static string CStr(byte[] b, int o)
        {
            int e = Array.IndexOf(b, (byte)0, o);
            return Encoding.UTF8.GetString(b, o, e - o);
        }

        public static TbTable Parse(byte[] b)
        {
            var u = new Func<int, int>(i => Bin.I(b, 4 * i));
            if (Bin.U(b, 0) != 0xFFFFFFFE) throw new DataError("not a text table");
            int nl = u(2), lt = u(3), nlab = u(4), labt = u(5), aux = u(7), nstr = u(8), strt = u(9);
            var t = new TbTable { Version = Bin.U(b, 4) };
            for (int i = 0; i < nl; i++) t.Langs.Add(CStr(b, Bin.I(b, lt + 4 * i)));
            var strings = new Dictionary<uint, string>();
            for (int i = 0; i < nstr; i++) strings[Bin.U(b, strt + 16 * i)] = CStr(b, Bin.I(b, strt + 16 * i + 12));
            var rows = new List<KeyValuePair<int, TbRow>>();
            for (int i = 0; i < nlab; i++)
            {
                int o = labt + 24 * i;
                int nvar = Bin.I(b, o + 12), sid = Bin.I(b, o + 16);
                string label = CStr(b, Bin.I(b, o + 20));
                for (int v = 0; v < nvar; v++)
                {
                    var text = new Dictionary<string, string>();
                    foreach (string lang in t.Langs)
                    {
                        string s;
                        text[lang] = strings.TryGetValue(Bin.Ice(label + ":" + v + ":" + lang), out s) ? s : null;
                    }
                    rows.Add(new KeyValuePair<int, TbRow>(sid + v, new TbRow { Label = label, Variant = v, Aux = CStr(b, Bin.I(b, aux + 4 * (sid + v))), Text = text }));
                }
            }
            t.Rows = rows.OrderBy(r => r.Key).Select(r => r.Value).ToList();
            return t;
        }

        // Pre-order balanced tree over n sorted items: (item, left, right), -1 for none.
        public static List<int[]> Bst(int n)
        {
            var o = new List<int[]>();
            Func<int, int, int> rec = null;
            rec = (lo, hi) =>
            {
                if (lo >= hi) return -1;
                int mid = (lo + hi) / 2, at = o.Count;
                o.Add(new[] { mid, -1, -1 });
                o[at][1] = rec(lo, mid);
                o[at][2] = rec(mid + 1, hi);
                return at;
            };
            rec(0, n);
            return o;
        }

        class LabelItem { public uint Hash; public string Name; public int NVar, Lo; public List<int> Aos; }

        public static byte[] Build(TbTable tbl)
        {
            var langs = tbl.Langs;
            var labels = new Dictionary<string, Dictionary<int, TbRow>>();
            var labelOrder = new List<string>();
            foreach (var r in tbl.Rows)
            {
                Dictionary<int, TbRow> L;
                if (!labels.TryGetValue(r.Label, out L)) { labels[r.Label] = L = new Dictionary<int, TbRow>(); labelOrder.Add(r.Label); }
                L[r.Variant] = r;
            }
            var pool = new Buf();
            Func<string, int> add = s => { int o = pool.Len; pool.Add(Encoding.UTF8.GetBytes(s)); pool.Byte(0); return o; };
            var langOfs = langs.Select(x => add(x)).ToList();
            var labItems = new List<LabelItem>();
            var strItems = new List<KeyValuePair<uint, int>>();
            foreach (string name in labelOrder.OrderBy(x => Bin.Ice(x)))
            {
                var L = labels[name];
                int lo = add(name);
                var vars = L.Keys.OrderBy(x => x).ToList();
                for (int i = 0; i < vars.Count; i++)
                    if (vars[i] != i) throw new DataError("text " + name + ": variants must be numbered 0, 1, 2...");
                var aos = new List<int>();
                foreach (int v in vars)
                {
                    var row = L[v];
                    aos.Add(add(row.Aux ?? ""));
                    foreach (string lang in langs)
                    {
                        string s;
                        if (row.Text == null || !row.Text.TryGetValue(lang, out s) || s == null) continue;
                        strItems.Add(new KeyValuePair<uint, int>(Bin.Ice(name + ":" + v + ":" + lang), add(s)));
                    }
                }
                labItems.Add(new LabelItem { Hash = Bin.Ice(name), Name = name, NVar = vars.Count, Lo = lo, Aos = aos });
            }
            labItems = labItems.OrderBy(x => x.Hash).ThenBy(x => x.Name, StringComparer.Ordinal).ThenBy(x => x.NVar).ThenBy(x => x.Lo).ToList();
            strItems = strItems.OrderBy(x => x.Key).ThenBy(x => x.Value).ToList();
            int nl = langs.Count, nlab = labItems.Count, nstr = strItems.Count;
            int nent = labItems.Sum(x => x.NVar);
            int lt = 0x28, labt = lt + 4 * nl, aux = labt + 24 * nlab, strt = aux + 4 * nent, bas = strt + 16 * nstr;
            var o2 = new Buf();
            foreach (int v in new[] { unchecked((int)0xFFFFFFFE), (int)tbl.Version, nl, lt, nlab, labt, nent, aux, nstr, strt }) o2.I(v);
            foreach (int x in langOfs) o2.I(bas + x);
            var sid = new Dictionary<uint, int>();
            int acc = 0;
            foreach (var it in labItems) { sid[it.Hash] = acc; acc += it.NVar; }
            foreach (var n in Bst(labItems.Count))
            {
                var it = labItems[n[0]];
                o2.U(it.Hash); o2.I(n[1]); o2.I(n[2]); o2.I(it.NVar); o2.I(sid[it.Hash]); o2.I(bas + it.Lo);
            }
            foreach (var it in labItems) foreach (int ao in it.Aos) o2.I(bas + ao);
            foreach (var n in Bst(strItems.Count))
            {
                var it = strItems[n[0]];
                o2.U(it.Key); o2.I(n[1]); o2.I(n[2]); o2.I(bas + it.Value);
            }
            if (o2.Len != bas) throw new DataError("text table layout error");
            o2.Add(pool.ToArray());
            return o2.ToArray();
        }
    }

    // ---- sprite files (tools/osb.py, sprites.py) ----
    class Tex { public int Hdr, Ofs, W, H; }

    struct Pt { public double X, Y; public Pt(double x, double y) { X = x; Y = y; } }

    class Quad { public Pt[] D = new Pt[4], T = new Pt[4]; }

    class Node
    {
        public uint Name, Parent;
        public Dictionary<uint, List<KeyValuePair<double, double>>> Tracks = new Dictionary<uint, List<KeyValuePair<double, double>>>();
    }

    class Anim { public int Index; public uint Hash; public float Length; public List<Node> Nodes = new List<Node>(); }

    class Mesh
    {
        public byte[] V, Ix;
        public Mesh(byte[] v, byte[] ix) { V = v; Ix = ix; }
    }

    static class Sprites
    {
        public const int TILE = 16, PAD = 1;
        public const string FRAME_RE = @"_m(\d+)_o(-?\d+)_(-?\d+)\.png$";

        // Byte arrays are never changed once made, so what's read from them can be kept.
        static readonly ConditionalWeakTable<byte[], List<Tex>> texCache = new ConditionalWeakTable<byte[], List<Tex>>();
        static readonly ConditionalWeakTable<byte[], List<Anim>> animCache = new ConditionalWeakTable<byte[], List<Anim>>();

        public static byte[] Load(string asset) { return DataHash.Vanilla(asset); }

        public static List<Tex> Textures(byte[] b)
        {
            return texCache.GetValue(b, x =>
            {
                var o = new List<Tex>();
                int p = 0, n = x.Length;
                while (p + 24 <= n)
                {
                    long ofs = Bin.U(x, p), sz = Bin.U(x, p + 4), w = Bin.U(x, p + 12), h = Bin.U(x, p + 16);
                    if (ofs == p + 24 && w > 0 && w <= 8192 && h > 0 && h <= 8192 && sz == w * h * 4 && ofs + sz <= n)
                    {
                        o.Add(new Tex { Hdr = p, Ofs = (int)ofs, W = (int)w, H = (int)h });
                        p = (int)(ofs + sz);
                        continue;
                    }
                    p += 4;
                }
                return o;
            });
        }

        public static Tex FirstTex(byte[] b)
        {
            var t = Textures(b);
            if (t.Count == 0) throw new DataError("sprite file has no picture sheet");
            return t[0];
        }

        public static List<Anim> Animations(byte[] b)
        {
            return animCache.GetValue(b, x =>
            {
                int hs = Bin.I(x, 0), n = Bin.I(x, 0x20);
                var o = new List<Anim>();
                for (int i = 0; i < n; i++)
                {
                    int e = hs + 0x1C * i;
                    var a = new Anim { Index = i, Hash = Bin.U(x, e + 4), Length = Bin.F(x, e + 8) };
                    int nblk = Bin.I(x, e + 20), blk = Bin.I(x, e + 24);
                    for (int k = 0; k < nblk; k++)
                    {
                        int bo = blk + 0x48 * k;
                        var node = new Node { Name = Bin.U(x, bo + 4), Parent = Bin.U(x, bo + 8) };
                        int ntr = Bin.I(x, bo + 0x0C), to = Bin.I(x, bo + 0x10);
                        for (int t = 0; t < ntr; t++)
                        {
                            int stride = Bin.I(x, to), nkey = Bin.I(x, to + 4);
                            uint chan = Bin.U(x, to + 8);
                            var keys = new List<KeyValuePair<double, double>>();
                            for (int j = 0; j < nkey; j++)
                            {
                                int ko = to + 0x14 + stride * j;
                                keys.Add(new KeyValuePair<double, double>(Bin.F(x, ko), chan == 0 ? Bin.U(x, ko + 8) : (double)Bin.F(x, ko + 8)));
                            }
                            node.Tracks[chan] = keys;
                            to += 0x14 + stride * nkey;
                        }
                        a.Nodes.Add(node);
                    }
                    o.Add(a);
                }
                return o;
            });
        }

        // Distinct frames of the root node's picture track: (start time, mesh), without the +-inf keys.
        public static List<KeyValuePair<double, int>> Frames(Anim a)
        {
            var o = new List<KeyValuePair<double, int>>();
            List<KeyValuePair<double, double>> keys;
            if (a.Nodes.Count == 0 || !a.Nodes[0].Tracks.TryGetValue(0, out keys)) return o;
            foreach (var k in keys)
            {
                if (!(Math.Abs(k.Key) < 1e30)) continue;
                int m = (int)k.Value;
                if (o.Count == 0 || o[o.Count - 1].Value != m) o.Add(new KeyValuePair<double, int>(k.Key, m));
            }
            return o;
        }

        public static List<Quad> MeshQuads(byte[] b, int m)
        {
            int sec = Bin.I(b, 0x1C);
            var tex = FirstTex(b);
            int rec = sec + 4 + 0x18 * m;
            int vo = Bin.I(b, rec + 8), nv = Bin.I(b, rec + 16);
            var o = new List<Quad>();
            for (int q = 0; q < nv / 4; q++)
            {
                var quad = new Quad();
                for (int c = 0; c < 4; c++)
                {
                    int p = rec + vo + 20 * (4 * q + c);
                    quad.D[c] = new Pt(Bin.F(b, p), Bin.F(b, p + 4));
                    quad.T[c] = new Pt(Bin.F(b, p + 12) * (double)tex.W, Bin.F(b, p + 16) * (double)tex.H);
                }
                o.Add(quad);
            }
            return o;
        }

        // Every frame pixel the quads cover: (x, y, atlas byte offset), bilinear like sprites.quad_pixels.
        public static void QuadPixels(List<Quad> quads, int TW, Action<int, int, int> each)
        {
            foreach (var q in quads)
            {
                double ax = q.D.Min(p => p.X), bx = q.D.Max(p => p.X), ay = q.D.Min(p => p.Y), by = q.D.Max(p => p.Y);
                if (bx <= ax || by <= ay) continue;
                var corner = new Pt?[4];  // index: (x == bx) + 2 * (y == by)
                for (int c = 0; c < 4; c++) corner[(q.D[c].X == bx ? 1 : 0) + (q.D[c].Y == by ? 2 : 0)] = q.T[c];
                if (corner.Any(c => c == null)) continue;
                Pt c00 = corner[0].Value, c10 = corner[1].Value, c01 = corner[2].Value, c11 = corner[3].Value;
                for (int yy = (int)ay; yy < (int)by; yy++)
                {
                    double ty = (yy + 0.5 - ay) / (by - ay);
                    for (int xx = (int)ax; xx < (int)bx; xx++)
                    {
                        double tx = (xx + 0.5 - ax) / (bx - ax);
                        double u = (1 - tx) * (1 - ty) * c00.X + tx * (1 - ty) * c10.X + (1 - tx) * ty * c01.X + tx * ty * c11.X;
                        double v = (1 - tx) * (1 - ty) * c00.Y + tx * (1 - ty) * c10.Y + (1 - tx) * ty * c01.Y + tx * ty * c11.Y;
                        each(xx, yy, ((int)v * TW + (int)u) * 4);
                    }
                }
            }
        }

        // Shared canvas for these meshes: (x0, y1, W, H) in sprite pixels (y up).
        public static int[] Canvas(byte[] b, IEnumerable<int> meshes)
        {
            var qs = meshes.Distinct().SelectMany(m => MeshQuads(b, m)).ToList();
            if (qs.Count == 0) return new[] { 0, 1, 1, 1 };
            double minx = qs.Min(q => q.D.Min(p => p.X)), maxx = qs.Max(q => q.D.Max(p => p.X));
            double miny = qs.Min(q => q.D.Min(p => p.Y)), maxy = qs.Max(q => q.D.Max(p => p.Y));
            int x0 = (int)minx, x1 = (int)maxx, y0 = (int)miny, y1 = (int)maxy;
            return new[] { x0, y1, x1 - x0, y1 - y0 };
        }

        // Frames drawn on a shared canvas (full color, RGBA): (w, h, picture per mesh).
        public static List<byte[]> Render(byte[] b, int[] meshes, int[] box, out int W, out int H)
        {
            var tex = FirstTex(b);
            int TW = tex.W, baseOfs = tex.Ofs, end = tex.Ofs + TW * tex.H * 4;
            var bx = box ?? Canvas(b, meshes);
            int x0 = bx[0], y1 = bx[1], w = bx[2], h = bx[3];
            W = w; H = h;
            var o = new List<byte[]>();
            foreach (int m in meshes)
            {
                var img = new byte[w * h * 4];
                QuadPixels(MeshQuads(b, m), TW, (xx, yy, so) =>
                {
                    int s = baseOfs + so;
                    if (so < 0 || s + 4 > end || b[s + 3] == 0) return;
                    int d = ((y1 - 1 - yy) * w + (xx - x0)) * 4;
                    if (d < 0 || d + 4 > img.Length) return;
                    img[d] = b[s + 2]; img[d + 1] = b[s + 1]; img[d + 2] = b[s]; img[d + 3] = b[s + 3];  // stored BGRA
                });
                o.Add(img);
            }
            return o;
        }

        public static List<Mesh> MeshesRaw(byte[] b)
        {
            int sec = Bin.I(b, 0x1C), n = Bin.I(b, sec);
            var o = new List<Mesh>();
            for (int m = 0; m < n; m++)
            {
                int rec = sec + 4 + 0x18 * m;
                int vo = Bin.I(b, rec + 8), io = Bin.I(b, rec + 12), nv = Bin.I(b, rec + 16), ni = Bin.I(b, rec + 20);
                o.Add(new Mesh(Bin.Slice(b, rec + vo, rec + vo + 20 * nv), Bin.Slice(b, rec + io, rec + io + 2 * ni)));
            }
            return o;
        }

        // The mesh section: records, then all vertices, then all indices.
        public static byte[] MeshBlob(List<Mesh> meshes)
        {
            int n = meshes.Count, vstart = 4 + 0x18 * n, istart = vstart + meshes.Sum(m => m.V.Length);
            var blob = new Buf();
            blob.I(n);
            int vo = 0, io = 0;
            for (int m = 0; m < n; m++)
            {
                int rec = 4 + 0x18 * m;
                blob.I(0x18); blob.I(0); blob.I(vstart + vo - rec); blob.I(istart + io - rec); blob.I(meshes[m].V.Length / 20); blob.I(meshes[m].Ix.Length / 2);
                vo += meshes[m].V.Length;
                io += meshes[m].Ix.Length;
            }
            foreach (var m in meshes) blob.Add(m.V);
            foreach (var m in meshes) blob.Add(m.Ix);
            return blob.ToArray();
        }

        public static byte[] RebuildMeshes(byte[] b, List<Mesh> meshes)
        {
            int sec = Bin.I(b, 0x1C);
            var blob = MeshBlob(meshes);
            var o = new byte[sec + blob.Length];
            Buffer.BlockCopy(b, 0, o, 0, sec);
            Buffer.BlockCopy(blob, 0, o, sec, blob.Length);
            Bin.PutI(o, 0x18, blob.Length);                      // [6] mesh section size
            for (int f = 20; f <= 23; f++) Bin.PutI(o, 4 * f, o.Length);  // [20..23] file size
            return o;
        }

        // Taller texture (rows added at the bottom, v coordinates rescaled).
        public static byte[] GrowTexture(byte[] b, int newH)
        {
            var tex = FirstTex(b);
            int W = tex.W, H = tex.H, add = W * (newH - H) * 4, end = tex.Ofs + W * H * 4, sec = Bin.I(b, 0x1C);
            if (end > sec) throw new DataError("sprite file layout not understood");
            var o = new byte[b.Length + add];
            Buffer.BlockCopy(b, 0, o, 0, end);
            Buffer.BlockCopy(b, end, o, end + add, b.Length - end);
            Bin.PutI(o, tex.Hdr + 4, W * newH * 4);
            Bin.PutI(o, tex.Hdr + 16, newH);
            Bin.PutI(o, 0x1C, sec + add);
            for (int f = 20; f <= 23; f++) Bin.PutI(o, 4 * f, o.Length);
            var meshes = new List<Mesh>();
            foreach (var m in MeshesRaw(o))
            {
                var v = Bin.Copy(m.V);
                for (int k = 0; k < v.Length / 20; k++)
                {
                    double vv = Bin.F(v, 20 * k + 16);
                    Bin.PutF(v, 20 * k + 16, (float)(vv * H / newH));
                }
                meshes.Add(new Mesh(v, m.Ix));
            }
            return RebuildMeshes(o, meshes);
        }

        static byte[] Occupied(byte[] b, out int W, out int H)
        {
            var tex = FirstTex(b);
            W = tex.W; H = tex.H;
            var used = new byte[W * H];
            foreach (var m in MeshesRaw(b))
            {
                var v = m.V;
                for (int q = 0; q < v.Length / 80; q++)
                {
                    double minu = double.MaxValue, maxu = double.MinValue, minv = double.MaxValue, maxv = double.MinValue;
                    for (int k = 0; k < 4; k++)
                    {
                        double a = Bin.F(v, 80 * q + 20 * k + 12), c = Bin.F(v, 80 * q + 20 * k + 16);
                        minu = Math.Min(minu, a); maxu = Math.Max(maxu, a); minv = Math.Min(minv, c); maxv = Math.Max(maxv, c);
                    }
                    int x0 = Math.Max(0, (int)(minu * W) - PAD), x1 = Math.Min(W, (int)Math.Round(maxu * W) + PAD);
                    int y0 = Math.Max(0, (int)(minv * H) - PAD), y1 = Math.Min(H, (int)Math.Round(maxv * H) + PAD);
                    if (x1 <= x0) continue;
                    for (int y = y0; y < y1; y++)
                        for (int x = x0; x < x1; x++) used[y * W + x] = 1;
                }
            }
            return used;
        }

        static List<int[]> FreeSlots(byte[] used, int W, int H)
        {
            int step = TILE + 2 * PAD;
            var o = new List<int[]>();
            for (int cy = 0; cy < H - step + 1; cy += step)
                for (int cx = 0; cx < W - step + 1; cx += step)
                {
                    bool free = true;
                    for (int r = 0; r < step && free; r++)
                        for (int x = 0; x < step; x++)
                            if (used[(cy + r) * W + cx + x] != 0) { free = false; break; }
                    if (free) o.Add(new[] { cx + PAD, cy + PAD });
                }
            return o;
        }

        // A new 16x16 tile on a picture: where it goes (x, y bottom, in sprite pixels) and its pixels.
        public class Tile { public int X, Y; public byte[] Px; public Tile(int x, int y, byte[] px) { X = x; Y = y; Px = px; } }

        // newTiles: (picture, its tiles) in order. Identical tiles are stored once; the texture
        // grows when the sheet is full.
        public static byte[] AddTiles(byte[] b, List<KeyValuePair<int, List<Tile>>> newTiles, Action<string> report, int maxH = 4096)
        {
            var unique = new Dictionary<byte[], int[]>(BytesKey.Instance);
            var order = new List<byte[]>();
            foreach (var kv in newTiles)
                foreach (var t in kv.Value)
                    if (!unique.ContainsKey(t.Px)) { unique[t.Px] = null; order.Add(t.Px); }
            int W, H;
            var used = Occupied(b, out W, out H);
            var slots = FreeSlots(used, W, H);
            while (slots.Count < order.Count)
            {
                if (H * 2 > maxH) throw new DataError("not enough atlas space for " + order.Count + " new tiles");
                report("sprites: atlas full, growing the texture to " + W + "x" + (H * 2));
                b = GrowTexture(b, H * 2);
                used = Occupied(b, out W, out H);
                slots = FreeSlots(used, W, H);
            }
            var tex = FirstTex(b);
            var o = Bin.Copy(b);
            for (int i = 0; i < order.Count; i++)
            {
                var px = order[i];
                int tx = slots[i][0], ty = slots[i][1];
                unique[px] = slots[i];
                for (int r = 0; r < TILE; r++)
                    Buffer.BlockCopy(px, r * TILE * 4, o, tex.Ofs + ((ty + r) * W + tx) * 4, TILE * 4);
            }
            var meshes = MeshesRaw(o);
            foreach (var kv in newTiles)
            {
                var v = new Buf(meshes[kv.Key].V);
                var ix = new Buf(meshes[kv.Key].Ix);
                foreach (var t in kv.Value)
                {
                    var slot = unique[t.Px];
                    int tx = slot[0], ty = slot[1];
                    int bas = v.Len / 20, ax = t.X, ay = t.Y, bx = ax + TILE, by = ay + TILE;
                    foreach (var c in new[] { new[] { ax, ay, tx, ty + TILE }, new[] { ax, by, tx, ty }, new[] { bx, by, tx + TILE, ty }, new[] { bx, ay, tx + TILE, ty + TILE } })
                    {
                        v.F(c[0]); v.F(c[1]); v.F(0f); v.F((float)((double)c[2] / W)); v.F((float)((double)c[3] / H));
                    }
                    foreach (int k in new[] { bas, bas + 1, bas + 2, bas, bas + 2, bas + 3 }) ix.U16(k);
                }
                meshes[kv.Key] = new Mesh(v.ToArray(), ix.ToArray());
            }
            return RebuildMeshes(o, meshes);
        }

        // A copy of sprite file oldAsset saved as newAsset: animation nodes that name "this file"
        // by hash must name the copy (sprites.rename_self_refs). -> (bytes, count)
        public static byte[] RenameSelfRefs(byte[] b, string oldAsset, string newAsset, out int count)
        {
            uint oldH = Bin.Ice(oldAsset), newH = Bin.Ice(newAsset);
            int end = Math.Min(Bin.I(b, 0x18), b.Length);
            var o = Bin.Copy(b);
            count = 0;
            for (int i = 0; i + 4 <= end; i++)
                if (Bin.U(o, i) == oldH && i % 4 == 0) { Bin.PutU(o, i, newH); count++; }
            return o;
        }
    }

    // Minimal PNG reader (osb.read_png_rgba): 8-bit RGBA, not interlaced.
    static class Png
    {
        public static byte[] ReadRgba(string path, out int w, out int h)
        {
            var d = File.ReadAllBytes(path);
            var sig = new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
            if (d.Length < 8 || !sig.SequenceEqual(d.Take(8))) throw new DataError(Path.GetFileName(path) + " isn't a PNG picture");
            int o = 8;
            w = 0; h = 0;
            var idat = new MemoryStream();
            Func<int, int> be = p => (d[p] << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
            while (o + 8 <= d.Length)
            {
                int ln = be(o);
                string t = Encoding.ASCII.GetString(d, o + 4, 4);
                if (t == "IHDR")
                {
                    w = be(o + 8); h = be(o + 12);
                    if (d[o + 16] != 8 || d[o + 17] != 6 || d[o + 20] != 0)
                        throw new DataError(Path.GetFileName(path) + ": need 8-bit RGBA non-interlaced");
                }
                else if (t == "IDAT") idat.Write(d, o + 8, ln);
                o += 12 + ln;
            }
            var raw = Zlib.Decompress(idat.ToArray());
            int stride = w * 4;
            var outp = new byte[stride * h];
            var prev = new byte[stride];
            int pos = 0;
            for (int y = 0; y < h; y++)
            {
                int ft = raw[pos];
                var line = new byte[stride];
                Buffer.BlockCopy(raw, pos + 1, line, 0, stride);
                pos += 1 + stride;
                for (int i = 0; i < stride; i++)
                {
                    int a = i >= 4 ? line[i - 4] : 0, up = prev[i], c = i >= 4 ? prev[i - 4] : 0;
                    if (ft == 1) line[i] = (byte)(line[i] + a);
                    else if (ft == 2) line[i] = (byte)(line[i] + up);
                    else if (ft == 3) line[i] = (byte)(line[i] + ((a + up) >> 1));
                    else if (ft == 4)
                    {
                        int pa = Math.Abs(up - c), pb = Math.Abs(a - c), pc = Math.Abs(a + up - 2 * c);
                        int pr = pa <= pb && pa <= pc ? a : (pb <= pc ? up : c);
                        line[i] = (byte)(line[i] + pr);
                    }
                }
                Buffer.BlockCopy(line, 0, outp, y * stride, stride);
                prev = line;
            }
            return outp;
        }
    }

    // ---- structural editing of sprite files (tools/osbedit.py) ----
    class OBlock { public byte[] Block, Tracks; public int KeyAt, KeyAtNew; public bool New; }
    class OAnim { public uint Hash; public float Length; public int First, Count; }

    class OModel
    {
        public uint[] H;
        public List<OAnim> Anims = new List<OAnim>();
        public List<OBlock> Blocks = new List<OBlock>();
        public byte[] Keys, Tex, Head, Orig;
        public int Keys0;
        public Buf Part;
        public List<Mesh> Meshes = new List<Mesh>();
    }

    static class OsbEdit
    {
        public static OModel Parse(byte[] b)
        {
            var H = new uint[27];
            for (int i = 0; i < 27; i++) H[i] = Bin.U(b, 4 * i);
            int hs = (int)H[0], n = (int)H[8];
            if (hs != 0x6C || H[20] != b.Length || H[7] + H[6] != b.Length) throw new DataError("not a sprite file this editor understands");
            int tree0 = hs, blocks0 = hs + 0x1C * n;
            var m = new OModel { H = H, Orig = b, Head = Bin.Slice(b, 0, hs) };
            for (int i = 0; i < n; i++)
            {
                int e = tree0 + 0x1C * i;
                m.Anims.Add(new OAnim { Hash = Bin.U(b, e + 4), Length = Bin.F(b, e + 8), Count = Bin.I(b, e + 20), First = (Bin.I(b, e + 24) - blocks0) / 0x48 });
            }
            int nblocks = m.Anims.Max(a => a.First + a.Count);
            int keys0 = blocks0 + 0x48 * nblocks;
            for (int k = 0; k < nblocks; k++)
            {
                var blk = Bin.Slice(b, blocks0 + 0x48 * k, blocks0 + 0x48 * (k + 1));
                int ntr = Bin.I(blk, 0xC), to = Bin.I(blk, 0x10), o = to;
                for (int t = 0; t < ntr; t++) o += 0x14 + Bin.I(b, o) * Bin.I(b, o + 4);
                m.Blocks.Add(new OBlock { Block = blk, Tracks = Bin.Slice(b, to, o), KeyAt = to });
            }
            int sec = (int)H[7], nm = Bin.I(b, sec);
            for (int k = 0; k < nm; k++)
            {
                int rec = sec + 4 + 0x18 * k;
                int vo = Bin.I(b, rec + 8), io = Bin.I(b, rec + 12), nv = Bin.I(b, rec + 16), ni = Bin.I(b, rec + 20);
                m.Meshes.Add(new Mesh(Bin.Slice(b, rec + vo, rec + vo + 20 * nv), Bin.Slice(b, rec + io, rec + io + 2 * ni)));
            }
            m.Keys = Bin.Slice(b, keys0, (int)H[10]);
            m.Keys0 = keys0;
            m.Part = new Buf(Bin.Slice(b, (int)H[10], (int)H[19]));
            m.Tex = Bin.Slice(b, (int)H[19], (int)H[7]);
            return m;
        }

        public static byte[] Build(OModel m)
        {
            var b = m.Orig;
            var H = (uint[])m.H.Clone();
            int hs = (int)H[0], n = m.Anims.Count, nb = m.Blocks.Count;
            int tree0 = hs, blocks1 = tree0 + 0x1C * n, keys1 = blocks1 + 0x48 * nb;
            int shift = keys1 - m.Keys0;
            var keys = new Buf(m.Keys);
            bool anyNew = false;
            foreach (var blk in m.Blocks)
                if (blk.New)
                {
                    if (!anyNew) { while (keys.Len % 4 != 0) keys.Byte(0); anyNew = true; }
                    blk.KeyAtNew = keys1 + keys.Len;
                    keys.Add(blk.Tracks);
                }
            if (anyNew) keys.Zeros(Bin.Mod(-(keys1 + keys.Len), 8));
            var blocks = new Buf();
            foreach (var blk in m.Blocks)
            {
                var bb = Bin.Copy(blk.Block);
                Bin.PutI(bb, 0x10, blk.New ? blk.KeyAtNew : blk.KeyAt + shift);
                blocks.Add(bb);
            }
            int part1 = keys1 + keys.Len;
            var part = m.Part.ToArray();
            int tex1 = part1 + part.Length;
            tex1 += Bin.Mod((int)H[19] - tex1, 16);  // keep the texture's 16-byte phase
            int texShift = tex1 - (int)H[19];
            for (int o = 0; o < part.Length - 7; o += 8)
                if (Bin.U(part, o + 4) != 0) Bin.PutI(part, o + 4, Bin.I(part, o + 4) + texShift);
            var tex = Bin.Copy(m.Tex);
            foreach (var t in Sprites.Textures(b)) Bin.PutI(tex, t.Hdr - (int)H[19], t.Ofs + texShift);
            int mesh1 = tex1 + tex.Length;
            mesh1 += Bin.Mod((int)H[7] - mesh1, 16);
            var meshBlob = Sprites.MeshBlob(m.Meshes);
            var tree = new Buf();
            var order = Enumerable.Range(0, n).OrderBy(i => m.Anims[i].Hash).ToList();
            foreach (var x in Tb2.Bst(n))
            {
                var a = m.Anims[order[x[0]]];
                tree.I(0x1C); tree.U(a.Hash); tree.F(a.Length); tree.I(x[1]); tree.I(x[2]); tree.I(a.Count); tree.I(blocks1 + 0x48 * a.First);
            }
            var o2 = new Buf(m.Head);
            o2.Add(tree.ToArray()); o2.Add(blocks.ToArray()); o2.Add(keys.ToArray()); o2.Add(part);
            o2.Zeros(tex1 - o2.Len); o2.Add(tex);
            o2.Zeros(mesh1 - o2.Len); o2.Add(meshBlob);
            H[6] = (uint)meshBlob.Length; H[7] = (uint)mesh1; H[8] = (uint)n; H[9] = (uint)m.Meshes.Count; H[10] = (uint)part1; H[19] = (uint)tex1;
            H[20] = H[21] = H[22] = H[23] = (uint)o2.Len;
            foreach (int w in new[] { 25, 26 })  // texture offset, or (body sheets) a place in the part table
            {
                uint o = m.H[w];
                if (m.H[10] <= o && o < m.H[19]) H[w] = (uint)(o + (part1 - (int)m.H[10]));
                else if (o == m.H[19]) H[w] = (uint)tex1;
            }
            for (int i = 0; i < 27; i++) o2.PutU(4 * i, H[i]);
            return o2.ToArray();
        }

        public static int FindAnim(OModel m, uint h)
        {
            for (int i = 0; i < m.Anims.Count; i++) if (m.Anims[i].Hash == h) return i;
            return -1;
        }

        public static int AddMesh(OModel m)
        {
            m.Meshes.Add(new Mesh(new byte[0], new byte[0]));
            var head = m.Part.ToArray();
            m.Part.Add(head, 0, 16);
            return m.Meshes.Count - 1;
        }

        public static byte[] SetTrack(byte[] tracks, uint chan, double value)
        {
            var t = Bin.Copy(tracks);
            int o = 0;
            bool hit = false;
            while (o < t.Length)
            {
                int stride = Bin.I(t, o), nkey = Bin.I(t, o + 4);
                if (Bin.U(t, o + 8) == chan)
                {
                    for (int j = 0; j < nkey; j++)
                    {
                        int ko = o + 0x14 + stride * j + 8;
                        if (chan == 0) Bin.PutU(t, ko, (uint)(long)value);
                        else Bin.PutF(t, ko, (float)value);
                    }
                    hit = true;
                }
                o += 0x14 + stride * nkey;
            }
            if (!hit) throw new DataError("node has no track for channel " + chan);
            return t;
        }

        public static double TrackValue(byte[] tracks, uint chan)
        {
            int o = 0;
            while (o < tracks.Length)
            {
                int stride = Bin.I(tracks, o), nkey = Bin.I(tracks, o + 4);
                if (Bin.U(tracks, o + 8) == chan)
                    return chan == 0 ? Bin.U(tracks, o + 0x14 + 8) : (double)Bin.F(tracks, o + 0x14 + 8);
                o += 0x14 + stride * nkey;
            }
            throw new DataError("node has no track for channel " + chan);
        }

        public class Changes : Dictionary<uint, Dictionary<uint, double>>
        {
            public Changes Set(uint node, uint chan, double v)
            {
                Dictionary<uint, double> d;
                if (!TryGetValue(node, out d)) this[node] = d = new Dictionary<uint, double>();
                d[chan] = v;
                return this;
            }
        }

        public class ExtraNode { public uint Template, Name; public Dictionary<uint, double> Vals; }

        public static int CloneAnim(OModel m, uint srcHash, uint newHash, Changes changes = null, List<ExtraNode> extra = null)
        {
            int i = FindAnim(m, srcHash);
            if (i < 0) throw new DataError("no animation " + srcHash.ToString("x8"));
            var a = m.Anims[i];
            int first = m.Blocks.Count;
            var srcs = m.Blocks.GetRange(a.First, a.Count);
            foreach (var blk in srcs)
            {
                uint name = Bin.U(blk.Block, 4);
                var tracks = blk.Tracks;
                Dictionary<uint, double> ch;
                if (changes != null && changes.TryGetValue(name, out ch))
                    foreach (var kv in ch) tracks = SetTrack(tracks, kv.Key, kv.Value);
                m.Blocks.Add(new OBlock { Block = Bin.Copy(blk.Block), Tracks = tracks, New = true });
            }
            extra = extra ?? new List<ExtraNode>();
            foreach (var e in extra)
            {
                var src = srcs.First(x => Bin.U(x.Block, 4) == e.Template);
                var bb = Bin.Copy(src.Block);
                Bin.PutU(bb, 4, e.Name);
                var tracks = src.Tracks;
                foreach (var kv in e.Vals) tracks = SetTrack(tracks, kv.Key, kv.Value);
                m.Blocks.Add(new OBlock { Block = bb, Tracks = tracks, New = true });
            }
            m.Anims.Add(new OAnim { Hash = newHash, Length = a.Length, First = first, Count = a.Count + extra.Count });
            return m.Anims.Count - 1;
        }

        public static void AddNodes(OModel m, uint animHash, List<ExtraNode> extra)
        {
            int old = FindAnim(m, animHash);
            CloneAnim(m, animHash, animHash, null, extra);
            m.Anims.RemoveAt(old);
        }
    }

    // ---- class pictures in the shared art sheets (tools/selectart.py) ----
    class ClassArt
    {
        public int Base;
        public Dictionary<string, string> Pics = new Dictionary<string, string>();  // panel, highlight, portrait, ... -> png
        public string Get(string k) { string v; return Pics.TryGetValue(k, out v) && !string.IsNullOrEmpty(v) ? v : null; }
    }

    static class SelectArt
    {
        const string LAYOUT = "キャラクターセレクト2", PROMPT_LAYOUT = "キャラセレクト_レイアウト2";
        static readonly string[] HIGHLIGHTS = { "皇女2", "戦士2", "忍者2", "魔女2", "巫女", "盗賊" };
        const uint ROOT = 0x8D154D2D;
        const double PROMPT_STEP = -15.0;
        static readonly int[] EMPTY_PANEL = { 108, 68, 40, 255 };
        static readonly string[] HUD_FACE_NAMES = { "皇女", "戦士", "忍者", "魔女", "巫女", "盗賊" };
        const int EMPTY_MAX_SCALE = 3;

        static uint Ice(string s) { return Bin.Ice(s); }

        public static List<Sprites.Tile> ImageTiles(int w, int h, byte[] img, int ox, int oy)
        {
            const int T = Sprites.TILE;
            var blocks = new Dictionary<long, byte[]>();
            for (int row = 0; row < h; row++)
                for (int col = 0; col < w; col++)
                {
                    int p = (row * w + col) * 4;
                    if (img[p + 3] == 0) continue;
                    long key = ((long)(col / T) << 32) | (uint)(row / T);
                    byte[] tile;
                    if (!blocks.TryGetValue(key, out tile)) blocks[key] = tile = new byte[T * T * 4];
                    int t = ((row - (row / T) * T) * T + (col - (col / T) * T)) * 4;
                    tile[t] = img[p + 2]; tile[t + 1] = img[p + 1]; tile[t + 2] = img[p]; tile[t + 3] = img[p + 3];
                }
            var o = new List<Sprites.Tile>();
            foreach (var kv in blocks.OrderBy(k => k.Key))
            {
                int bc = (int)(kv.Key >> 32), br = (int)(kv.Key & 0xFFFFFFFF);
                o.Add(new Sprites.Tile(ox + bc * T, oy - br * T - T, kv.Value));
            }
            return o;
        }

        static byte[] PngWithOrigin(string path, out int w, out int h, out int ox, out int oy)
        {
            var mt = Regex.Match(Path.GetFileName(path), Sprites.FRAME_RE);
            if (!mt.Success) throw new DataError(Path.GetFileName(path) + ": keep the exported file name (it ends like _m135_o-24_24.png)");
            var img = Png.ReadRgba(path, out w, out h);
            ox = int.Parse(mt.Groups[2].Value);
            oy = int.Parse(mt.Groups[3].Value);
            return img;
        }

        static List<Sprites.Tile> PngTiles(string path)
        {
            int w, h, ox, oy;
            var img = PngWithOrigin(path, out w, out h, out ox, out oy);
            return ImageTiles(w, h, img, ox, oy);
        }

        static int PanelMesh(OModel m, int panel)
        {
            var a = m.Anims[FindOrLast(m, Ice(LAYOUT))];
            for (int k = a.First; k < a.First + a.Count; k++)
                if (Bin.U(m.Blocks[k].Block, 4) == Ice("PL" + (panel + 1)))
                    return (int)OsbEdit.TrackValue(m.Blocks[k].Tracks, 0);
            throw new DataError("the select layout has no panel PL" + (panel + 1));
        }

        static int FindOrLast(OModel m, uint h)
        {
            int i = OsbEdit.FindAnim(m, h);
            return i >= 0 ? i : m.Anims.Count - 1;  // as Python's index -1
        }

        static void AddPromptSlot(OModel m)
        {
            var pos = new Dictionary<uint, byte[]>();
            var a = m.Anims[FindOrLast(m, Ice(PROMPT_LAYOUT))];
            for (int k = a.First; k < a.First + a.Count; k++) pos[Bin.U(m.Blocks[k].Block, 4)] = m.Blocks[k].Tracks;
            var nodes = new List<OsbEdit.ExtraNode>();
            foreach (var p in new[] { new[] { "button2", "button3" }, new[] { "button_text2", "button_text3" } })
            {
                if (pos.ContainsKey(Ice(p[1]))) continue;
                double y = OsbEdit.TrackValue(pos[Ice(p[0])], 8);
                nodes.Add(new OsbEdit.ExtraNode { Template = Ice(p[0]), Name = Ice(p[1]), Vals = new Dictionary<uint, double> { { 8, y + PROMPT_STEP } } });
            }
            if (nodes.Count > 0) OsbEdit.AddNodes(m, Ice(PROMPT_LAYOUT), nodes);
        }

        // (root node name, picture) of an animation.
        static void Root(byte[] b, uint animHash, out uint root, out int mesh)
        {
            foreach (var a in Sprites.Animations(b))
                if (a.Hash == animHash)
                {
                    var fr = Sprites.Frames(a);
                    if (fr.Count == 0) throw new DataError("animation " + animHash.ToString("x8") + " has no picture");
                    root = a.Nodes[0].Name;
                    mesh = fr[0].Value;
                    return;
                }
            throw new DataError("no animation " + animHash.ToString("x8"));
        }

        public static byte[] Portraits(byte[] b, SortedDictionary<int, ClassArt> classes, Action<string> report, out Dictionary<int, int> has)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            has = new Dictionary<int, int>();
            foreach (var kv in classes)
            {
                var art = kv.Value;
                foreach (var spec in new[] { Tuple.Create("portrait", "", 1), Tuple.Create("portrait_large", "_24", 2) })
                {
                    string png = art.Get(spec.Item1);
                    if (png == null) continue;
                    var tiles = PngTiles(png);
                    int mesh = OsbEdit.AddMesh(m);
                    fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(mesh, tiles));
                    OsbEdit.CloneAnim(m, Ice("PlIcon_" + (art.Base + 1) + spec.Item2), Ice("MOD_PlIcon_" + kv.Key + spec.Item2), new OsbEdit.Changes().Set(ROOT, 0, mesh));
                    int prev;
                    has.TryGetValue(kv.Key, out prev);
                    has[kv.Key] = prev | spec.Item3;
                }
            }
            if (has.Count == 0) return b;
            return Sprites.AddTiles(OsbEdit.Build(m), fill, report);
        }

        public static byte[] BarFaces(byte[] b, SortedDictionary<int, ClassArt> classes, Action<string> report, out HashSet<int> has)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            has = new HashSet<int>();
            foreach (var kv in classes)
            {
                string png = kv.Value.Get("bar_face");
                if (png == null) continue;
                var tiles = PngTiles(png);
                int mesh = OsbEdit.AddMesh(m);
                fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(mesh, tiles));
                uint tpl = Ice("PlFace_" + (kv.Value.Base + 1));
                uint root; int _m;
                Root(b, tpl, out root, out _m);
                OsbEdit.CloneAnim(m, tpl, Ice("MOD_PlFace_" + kv.Key), new OsbEdit.Changes().Set(root, 0, mesh));
                has.Add(kv.Key);
            }
            if (has.Count == 0) return b;
            return Sprites.AddTiles(OsbEdit.Build(m), fill, report);
        }

        public static byte[] HudFaces(byte[] b, SortedDictionary<int, ClassArt> classes, Action<string> report, out HashSet<int> has)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            has = new HashSet<int>();
            foreach (var kv in classes)
            {
                var art = kv.Value;
                if (art.Get("hud_face") == null) continue;
                foreach (var spec in new[] { Tuple.Create(1, "hud_face"), Tuple.Create(2, "hud_face2") })
                {
                    var tiles = PngTiles(art.Get(spec.Item2) ?? art.Get("hud_face"));
                    int mesh = OsbEdit.AddMesh(m);
                    fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(mesh, tiles));
                    OsbEdit.CloneAnim(m, Ice(HUD_FACE_NAMES[art.Base] + "顔" + spec.Item1.ToString("00")),
                                      Ice("MOD_HudFace_" + kv.Key + "_" + spec.Item1), new OsbEdit.Changes().Set(ROOT, 0, mesh));
                }
                has.Add(kv.Key);
            }
            if (has.Count == 0) return b;
            return Sprites.AddTiles(OsbEdit.Build(m), fill, report);
        }

        public static byte[] HudFrames(byte[] b, SortedDictionary<int, ClassArt> classes, Action<string> report, out HashSet<int> has)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            has = new HashSet<int>();
            foreach (var kv in classes)
            {
                string png = kv.Value.Get("hud_frame");
                if (png == null) continue;
                var tiles = PngTiles(png);
                int mesh = OsbEdit.AddMesh(m);
                fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(mesh, tiles));
                uint tpl = Ice(HUD_FACE_NAMES[kv.Value.Base] + "ライフ");
                uint root; int _m;
                Root(b, tpl, out root, out _m);
                OsbEdit.CloneAnim(m, tpl, Ice("MOD_HudFrame_" + kv.Key), new OsbEdit.Changes().Set(root, 0, mesh));
                has.Add(kv.Key);
            }
            if (has.Count == 0) return b;
            return Sprites.AddTiles(OsbEdit.Build(m), fill, report);
        }

        // ---- "empty" pictures: the game's gray "no element" dash (com_elic, sixth picture) ----
        static byte[] Dash(out int w, out int h)
        {
            var b = Sprites.Load("Win/com_elic.osb");
            var a = Sprites.Animations(b)[5];
            int mesh = Sprites.Frames(a)[0].Value;
            return Sprites.Render(b, new[] { mesh }, null, out w, out h)[0];
        }

        static byte[] EmptyPicture(byte[] b, int mesh, int[] silhouette, out int w, out int h, out int ox, out int oy)
        {
            var c = Sprites.Canvas(b, new[] { mesh });
            int x0 = c[0], y1 = c[1];
            w = c[2]; h = c[3];
            var img = new byte[w * h * 4];
            if (silhouette != null)
            {
                int rw, rh;
                var src = Sprites.Render(b, new[] { mesh }, c, out rw, out rh)[0];
                for (int i = 0; i < src.Length; i += 4)
                    if (src[i + 3] != 0)
                    {
                        img[i] = (byte)silhouette[0]; img[i + 1] = (byte)silhouette[1]; img[i + 2] = (byte)silhouette[2];
                        img[i + 3] = (byte)(src[i + 3] * silhouette[3] / 255);
                    }
            }
            int dw, dh;
            var dash = Dash(out dw, out dh);
            int s = Math.Max(1, Math.Min(EMPTY_MAX_SCALE, Math.Min(w, h) / 2 / Math.Max(dw, dh)));
            int left = Bin.FloorDiv(w - dw * s, 2), top = Bin.FloorDiv(h - dh * s, 2);
            for (int y = 0; y < dh * s; y++)
                for (int x = 0; x < dw * s; x++)
                {
                    int o2 = ((y / s) * dw + x / s) * 4;
                    if (dash[o2 + 3] != 0 && 0 <= left + x && left + x < w && 0 <= top + y && top + y < h)
                    {
                        int o = ((top + y) * w + left + x) * 4;
                        Buffer.BlockCopy(dash, o2, img, o, 4);
                    }
                }
            ox = x0; oy = y1;
            return img;
        }

        static readonly int[][] FRAME_SHAPES = { new[] { 0, 4, 62, 90 }, new[] { 56, 30, 290, 62 }, new[] { 78, 14, 108, 32 } };
        static readonly int[][] FRAME_KEEP = { new[] { 80, 16, 106, 32 }, new[] { 60, 32, 288, 60 } };
        static readonly int[] G_EDGE = { 36, 36, 44 }, G_LIGHT = { 196, 196, 200 }, G_FILL = { 132, 132, 138 }, G_SHADOW = { 84, 84, 90 };

        static byte[] EmptyFrame(byte[] b, int mesh, out int w, out int h, out int ox, out int oy)
        {
            var cv = Sprites.Canvas(b, new[] { mesh });
            int x0 = cv[0], y1 = cv[1];
            int W = cv[2], Hh = cv[3];
            w = W; h = Hh;
            int rw, rh;
            var src = Sprites.Render(b, new[] { mesh }, cv, out rw, out rh)[0];
            const int S = 2;
            int bw = (W + S - 1) / S, bh = (Hh + S - 1) / S;
            var inside = new bool[bh, bw];
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++)
                    inside[by, bx] = FRAME_SHAPES.Any(r => r[0] <= bx * S && bx * S < r[2] && r[1] <= by * S && by * S < r[3]);
            Func<int, int, bool> at = (bx, by) => 0 <= bx && bx < bw && 0 <= by && by < bh && inside[by, bx];
            var img = new byte[W * Hh * 4];
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++)
                {
                    if (!inside[by, bx]) continue;
                    int[] c;
                    if (!(at(bx - 1, by) && at(bx + 1, by) && at(bx, by - 1) && at(bx, by + 1))) c = G_EDGE;
                    else if (!(at(bx - 2, by) && at(bx, by - 2))) c = G_LIGHT;
                    else if (!(at(bx + 2, by) && at(bx, by + 2))) c = G_SHADOW;
                    else c = G_FILL;
                    for (int y = by * S; y < Math.Min(Hh, by * S + S); y++)
                        for (int x = bx * S; x < Math.Min(W, bx * S + S); x++)
                        {
                            int o = (y * W + x) * 4;
                            img[o] = (byte)c[0]; img[o + 1] = (byte)c[1]; img[o + 2] = (byte)c[2]; img[o + 3] = 255;
                        }
                }
            foreach (var k in FRAME_KEEP)
                for (int y = k[1]; y < Math.Min(Hh, k[3]); y++)
                    for (int x = k[0]; x < Math.Min(W, k[2]); x++)
                    {
                        int o = (y * W + x) * 4;
                        int mx = Math.Max(src[o], Math.Max(src[o + 1], src[o + 2])), mn = Math.Min(src[o], Math.Min(src[o + 1], src[o + 2]));
                        if (src[o + 3] != 0 && (mx < 40 || mn > 200)) Buffer.BlockCopy(src, o, img, o, 4);
                    }
            ox = x0; oy = y1;
            return img;
        }

        // For each (template animation, new name, is life frame): a copy showing the empty picture.
        public static byte[] AddEmpties(byte[] b, IEnumerable<Tuple<string, string, bool>> names, Action<string> report)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            foreach (var e in names)
            {
                uint h = Ice(e.Item1);
                if (OsbEdit.FindAnim(m, Ice(e.Item2)) >= 0) continue;
                uint root; int mesh;
                Root(b, h, out root, out mesh);
                int w, hh, ox, oy;
                var img = e.Item3 ? EmptyFrame(b, mesh, out w, out hh, out ox, out oy) : EmptyPicture(b, mesh, null, out w, out hh, out ox, out oy);
                int nm = OsbEdit.AddMesh(m);
                fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(nm, ImageTiles(w, hh, img, ox, oy)));
                OsbEdit.CloneAnim(m, h, Ice(e.Item2), new OsbEdit.Changes().Set(root, 0, nm));
            }
            if (fill.Count == 0) return b;
            return Sprites.AddTiles(OsbEdit.Build(m), fill, report);
        }

        // The character select screen (s_o0a00): highlights, per-page panel layouts, info layout, third prompt.
        public static byte[] Build(byte[] b, SortedDictionary<int, ClassArt> classes, Action<string> report,
                                   out HashSet<int> highlights, out HashSet<int> pages)
        {
            var m = OsbEdit.Parse(b);
            var fill = new List<KeyValuePair<int, List<Sprites.Tile>>>();
            highlights = new HashSet<int>();
            pages = new HashSet<int>();
            var pagePanels = new SortedDictionary<int, List<KeyValuePair<int, int>>>();
            var empty = new Dictionary<int, int>();
            Func<int, int> emptyMesh = k =>
            {
                int em;
                if (!empty.TryGetValue(k, out em))
                {
                    int w, h, ox, oy;
                    var img = EmptyPicture(b, PanelMesh(m, k), EMPTY_PANEL, out w, out h, out ox, out oy);
                    empty[k] = em = OsbEdit.AddMesh(m);
                    fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(em, ImageTiles(w, h, img, ox, oy)));
                }
                return em;
            };
            Action<int, int, int> setPanel = (page, panel, mesh) =>
            {
                List<KeyValuePair<int, int>> l;
                if (!pagePanels.TryGetValue(page, out l)) pagePanels[page] = l = new List<KeyValuePair<int, int>>();
                int at = l.FindIndex(x => x.Key == panel);
                if (at >= 0) l[at] = new KeyValuePair<int, int>(panel, mesh); else l.Add(new KeyValuePair<int, int>(panel, mesh));
            };
            foreach (var kv in classes)
            {
                int cls = kv.Key, page = cls / 6, panel = cls % 6;
                var art = kv.Value;
                int mesh = OsbEdit.AddMesh(m);  // the selected-panel picture (none drawn when it has no art)
                if (art.Get("highlight") != null) fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(mesh, PngTiles(art.Get("highlight"))));
                OsbEdit.CloneAnim(m, Ice(HIGHLIGHTS[panel]), Ice("MOD_SEL_" + cls), new OsbEdit.Changes().Set(ROOT, 0, mesh));
                highlights.Add(cls);
                if (art.Get("panel") != null)
                {
                    var tiles = PngTiles(art.Get("panel"));
                    int pm = OsbEdit.AddMesh(m);
                    fill.Add(new KeyValuePair<int, List<Sprites.Tile>>(pm, tiles));
                    setPanel(page, panel, pm);
                }
                else setPanel(page, panel, emptyMesh(panel));
            }
            int npages = classes.Count > 0 ? classes.Keys.Max() / 6 + 1 : 1;
            var emptyOn = new Dictionary<int, List<int>>();
            for (int page = 1; page < npages; page++)
            {
                if (!pagePanels.ContainsKey(page)) pagePanels[page] = new List<KeyValuePair<int, int>>();
                for (int k = 0; k < 6; k++)
                    if (!classes.ContainsKey(6 * page + k))
                    {
                        setPanel(page, k, emptyMesh(k));
                        if (!emptyOn.ContainsKey(page)) emptyOn[page] = new List<int>();
                        emptyOn[page].Add(k);
                    }
            }
            foreach (var pp in pagePanels)
            {
                if (pp.Value.Count == 0) continue;
                var changes = new OsbEdit.Changes();
                foreach (var kv in pp.Value) changes.Set(Ice("PL" + (kv.Key + 1)), 0, kv.Value);
                List<int> eo;
                if (emptyOn.TryGetValue(pp.Key, out eo))
                    foreach (int k in eo) changes.Set(Ice("PL" + (k + 1) + "_Lv"), 8, -2000.0);  // no level shown: off screen
                OsbEdit.CloneAnim(m, Ice(LAYOUT), Ice("MOD_SEL_PAGE" + pp.Key), changes);
                pages.Add(pp.Key);
            }
            // the info panel shows the same layout and must not be switched: a copy without panel pictures
            int blank = OsbEdit.AddMesh(m);
            var none = new OsbEdit.Changes();
            for (int k = 0; k < 6; k++) none.Set(Ice("PL" + (k + 1)), 0, blank);
            OsbEdit.CloneAnim(m, Ice(LAYOUT), Ice("MOD_SEL_INFO"), none);
            AddPromptSlot(m);
            var o = OsbEdit.Build(m);
            if (fill.Count > 0) o = Sprites.AddTiles(o, fill, report);
            return o;
        }
    }

    // ---- palette files (tools/palettes.py) ----
    static class Palettes
    {
        public static int FindBlock(byte[] b, out int npal, out int ncol)
        {
            for (int o = 0; o + 8 <= b.Length; o++)
            {
                bool marker = true;
                for (int k = 0; k < 8; k++) if (b[o + k] != 0xFF) { marker = false; break; }
                if (!marker) continue;
                if (o >= 4 && o + 48 <= b.Length)
                {
                    long v0 = Bin.U(b, o + 8), v1 = Bin.U(b, o + 12), v4 = Bin.U(b, o + 24), v6 = Bin.U(b, o + 32), v9 = Bin.U(b, o + 44);
                    if (v0 == 0x34 && v1 == 0x14 && v4 != 0 && v6 != 0 && v9 == 0x34 + v4 * v6 * 4)
                    {
                        npal = (int)v4; ncol = (int)v6;
                        return o - 4 + 0x34;
                    }
                }
            }
            throw new DataError("no palette block");
        }

        // The palette ID the game looks for with a sprite: the hash of "<sprite>.osp".
        public static uint OspId(string sprite) { return Bin.Ice(Regex.Replace(sprite, @"\.osb\z", "") + ".osp"); }

        public static string SpriteFor(string name)
        {
            var m = Regex.Match(name, @"\Apl([0-9])_pal\z");
            if (m.Success) return "pl" + m.Groups[1].Value + "_cb00";
            return name.EndsWith("_pal") ? name.Substring(0, name.Length - 4) : name;
        }

        public static byte[] SetId(byte[] b, string name, string sprite = null)
        {
            int np, nc;
            var o = Bin.Copy(b);
            Bin.PutU(o, FindBlock(b, out np, out nc) - 0x34, OspId(sprite ?? SpriteFor(name)));
            return o;
        }
    }

    // ---- moving named parts of screen layouts (tools/layout.py) ----
    static class Layout
    {
        static readonly Dictionary<string, uint> Channel = new Dictionary<string, uint> { { "picture", 0 }, { "scale_x", 1 }, { "scale_y", 2 }, { "x", 7 }, { "y", 8 } };

        public class Edit { public int? Anim; public string Part, Field; public double Value; }

        // A part matches by its name's hash, or by the hash written out ("%08x", for parts with no known name).
        public static byte[] Apply(byte[] b, List<Edit> edits, Action<string> report)
        {
            var o = Bin.Copy(b);
            var tr = new List<Tuple<int, uint, uint, List<int>>>();  // anim index, node, channel, value offsets
            int hs = Bin.I(b, 0), n = Bin.I(b, 0x20);
            for (int i = 0; i < n; i++)
            {
                int nblk = Bin.I(b, hs + 0x1C * i + 20), blk = Bin.I(b, hs + 0x1C * i + 24);
                for (int k = 0; k < nblk; k++)
                {
                    int bo = blk + 0x48 * k;
                    uint name = Bin.U(b, bo + 4);
                    int ntr = Bin.I(b, bo + 0xC), to = Bin.I(b, bo + 0x10);
                    for (int t = 0; t < ntr; t++)
                    {
                        int stride = Bin.I(b, to), nkey = Bin.I(b, to + 4);
                        uint chan = Bin.U(b, to + 8);
                        var offs = new List<int>();
                        for (int j = 0; j < nkey; j++) offs.Add(to + 0x14 + stride * j + 8);
                        tr.Add(Tuple.Create(i, name, chan, offs));
                        to += 0x14 + stride * nkey;
                    }
                }
            }
            foreach (var e in edits)
            {
                uint chan;
                if (!Channel.TryGetValue(e.Field, out chan)) { report("layout: no field " + e.Field); continue; }
                uint want = Bin.Ice(e.Part);
                int hit = 0;
                foreach (var t in tr)
                {
                    if (t.Item3 != chan || (t.Item2 != want && t.Item2.ToString("x8") != e.Part) || (e.Anim.HasValue && t.Item1 != e.Anim.Value)) continue;
                    hit++;
                    foreach (int off in t.Item4)
                        if (chan == 0) Bin.PutU(o, off, (uint)(long)e.Value); else Bin.PutF(o, off, (float)e.Value);
                }
                if (hit == 0) report("layout: no part " + e.Part + " with " + e.Field + (e.Anim.HasValue ? " in animation " + e.Anim.Value : ""));
            }
            return o.SequenceEqual(b) ? null : o;
        }
    }

    // ---- item icons: Win/com_itic.osb (tools/icons.py) ----
    static class Icons
    {
        public const string ASSET = "Win/com_itic.osb";
        public const int FIRST_NEW_ICON = 24, GRID = 7, PITCH = 18;
        static readonly string[] ICON_NAMES = {
            "装備不可", "武器（皇女）", "武器（戦士１）", "武器（戦士２）", "武器（忍者１）", "武器（忍者２）", "武器（魔女）",
            "アクセサリ（男用）", "アクセサリ（女用）", "アクセサリ（皇女用）", "アクセサリ（戦士用）", "アクセサリ（忍者用）",
            "アクセサリ（魔女用）", "アクセサリ（共通）", "薬", "肉", "植物の実", "宝石", "お金", "鍵", "武器（皇女２）",
            "武器（魔女２）", "アクセサリ（巫女用）", "アクセサリ（盗賊用）" };

        static int PyRound(double x) { return (int)Math.Round(x, MidpointRounding.ToEven); }

        static Dictionary<int, int[]> IconRects(byte[] b)
        {
            int tree = Bin.I(b, 0), count = Bin.I(b, 0x20), meshes = Bin.I(b, 0x1C);
            var tex = Sprites.FirstTex(b);
            var byHash = new Dictionary<uint, int>();
            for (int i = 0; i < count; i++) { int e = tree + 0x1C * i; byHash[Bin.U(b, e + 4)] = Bin.I(b, e + 0x18); }
            var rects = new Dictionary<int, int[]>();
            for (int n = 0; n < ICON_NAMES.Length; n++)
            {
                int block;
                if (!byHash.TryGetValue(Bin.Ice(ICON_NAMES[n]), out block)) continue;
                int mesh = Bin.I(b, Bin.I(b, block + 0x10) + 0x1C);
                int rec = meshes + 4 + 0x18 * mesh, vtx = rec + Bin.I(b, rec + 8);
                var xs = new List<int>(); var ys = new List<int>();
                for (int k = 0; k < 4; k++) { xs.Add(PyRound(Bin.F(b, vtx + 20 * k + 12) * (double)tex.W)); ys.Add(PyRound(Bin.F(b, vtx + 20 * k + 16) * (double)tex.H)); }
                rects[n] = new[] { xs.Min(), ys.Min(), xs.Max() - xs.Min(), ys.Max() - ys.Min() };
            }
            return rects;
        }

        static byte[] SwapRB(byte[] px)
        {
            var o = Bin.Copy(px);
            for (int i = 0; i + 3 < px.Length; i += 4) { o[i] = px[i + 2]; o[i + 2] = px[i]; }
            return o;
        }

        static void UvRect(byte[] vtx, int tw, int th, out int x, out int y)
        {
            x = int.MaxValue; y = int.MaxValue;
            for (int k = 0; k < vtx.Length / 20; k++)
            {
                x = Math.Min(x, PyRound(Bin.F(vtx, 20 * k + 12) * (double)tw));
                y = Math.Min(y, PyRound(Bin.F(vtx, 20 * k + 16) * (double)th));
            }
        }

        static Dictionary<int, int> NewIconCells(byte[] b)
        {
            var tex = Sprites.FirstTex(b);
            var used = new HashSet<int>();
            foreach (var m in Sprites.MeshesRaw(b))
            {
                int x, y;
                UvRect(m.V, tex.W, tex.H, out x, out y);
                used.Add(Bin.FloorDiv(y - 1, PITCH) * GRID + Bin.FloorDiv(x - 1, PITCH));
            }
            var o = new Dictionary<int, int>();
            int i = 0;
            for (int c = 0; c < GRID * GRID; c++) if (!used.Contains(c)) o[FIRST_NEW_ICON + i++] = c;
            return o;
        }

        static int[] CellRect(int c) { return new[] { 1 + PITCH * (c % GRID), 1 + PITCH * (c / GRID), 16, 16 }; }

        // Re-serialize the sheet with extra entries (name hash, cell).
        static byte[] Rebuild(byte[] b, List<KeyValuePair<uint, int>> extra)
        {
            var H = new uint[27];
            for (int i = 0; i < 27; i++) H[i] = Bin.U(b, 4 * i);
            int hs = (int)H[0], n = (int)H[8], parts = (int)H[9];
            if (hs != 0x6C || H[20] != b.Length || H[7] + H[6] != b.Length) throw new DataError("the icon sheet isn't laid out as expected");
            int tree0 = hs, blocks0 = hs + 0x1C * n, keys0 = blocks0 + 0x48 * n;
            var hashes = new List<uint>(); var scales = new List<float>(); var cnts = new List<int>(); var blockIdx = new List<int>();
            for (int i = 0; i < n; i++)
            {
                int e = tree0 + 0x1C * i;
                hashes.Add(Bin.U(b, e + 4)); scales.Add(Bin.F(b, e + 8)); cnts.Add(Bin.I(b, e + 20)); blockIdx.Add((Bin.I(b, e + 24) - blocks0) / 0x48);
            }
            var oldBlocks = new List<byte[]>();
            for (int k = 0; k < n; k++) oldBlocks.Add(Bin.Slice(b, blocks0 + 0x48 * k, blocks0 + 0x48 * (k + 1)));
            var keysRegion = Bin.Slice(b, keys0, (int)H[10]);
            var partTab = new Buf(Bin.Slice(b, (int)H[10], (int)H[19]));
            var texSec = Bin.Slice(b, (int)H[19], (int)H[7]);
            var meshes = Sprites.MeshesRaw(b);
            var tex = Sprites.FirstTex(b);
            var tplV = meshes[0].V; var tplI = meshes[0].Ix;
            int tx0, ty0;
            UvRect(tplV, tex.W, tex.H, out tx0, out ty0);
            byte[] tplKey = null;
            foreach (var blk in oldBlocks)
            {
                int ko = Bin.I(blk, 0x10);
                if (Bin.U(b, ko + 28) == Bin.U(b, ko + 48) && Bin.U(b, ko) == 0x14) { tplKey = Bin.Slice(b, ko, ko + 0x64); break; }
            }
            var newMesh = new List<int>();
            foreach (var e in extra)
            {
                var r = CellRect(e.Value);
                var v = Bin.Copy(tplV);
                for (int k = 0; k < v.Length / 20; k++)
                {
                    double ou = Bin.F(v, 20 * k + 12), ov = Bin.F(v, 20 * k + 16);
                    Bin.PutF(v, 20 * k + 12, (float)((double)(r[0] + PyRound(ou * tex.W) - tx0) / tex.W));
                    Bin.PutF(v, 20 * k + 16, (float)((double)(r[1] + PyRound(ov * tex.H) - ty0) / tex.H));
                }
                meshes.Add(new Mesh(v, tplI));
                newMesh.Add(meshes.Count - 1);
            }
            int N = n + extra.Count, P = parts + extra.Count;
            int blocks1 = tree0 + 0x1C * N, keys1 = blocks1 + 0x48 * N, keyShift = keys1 - keys0;
            var keysBlob = new Buf(keysRegion);
            var newKey = new List<int>();
            foreach (int mi in newMesh)
            {
                var k = Bin.Copy(tplKey);
                foreach (int o in new[] { 28, 48, 68, 88 }) Bin.PutI(k, o, mi);
                newKey.Add(keys1 + keysBlob.Len);
                keysBlob.Add(k);
            }
            keysBlob.Zeros(Bin.Mod(-(keys1 + keysBlob.Len), 8));
            int part1 = keys1 + keysBlob.Len;
            var head16 = Bin.Slice(partTab.ToArray(), 0, 16);
            for (int i = 0; i < extra.Count; i++) partTab.Add(head16);
            var pt = partTab.ToArray();
            int tex1 = part1 + pt.Length;
            tex1 += Bin.Mod((int)H[19] - tex1, 16);
            int texShift = tex1 - (int)H[19];
            for (int o = 0; o < pt.Length; o += 8)
                if (Bin.U(pt, o + 4) != 0) Bin.PutI(pt, o + 4, Bin.I(pt, o + 4) + texShift);
            foreach (var t in Sprites.Textures(b)) Bin.PutI(texSec, t.Hdr - (int)H[19], t.Ofs + texShift);
            int mesh1 = tex1 + texSec.Length;
            mesh1 += Bin.Mod((int)H[7] - mesh1, 16);
            var meshBlob = Sprites.MeshBlob(meshes);
            var blocks = new Buf();
            foreach (var blk in oldBlocks) { Bin.PutI(blk, 0x10, Bin.I(blk, 0x10) + keyShift); blocks.Add(blk); }
            for (int k = 0; k < extra.Count; k++)
            {
                var blk = Bin.Copy(oldBlocks[0]);
                Bin.PutI(blk, 0x10, newKey[k]);
                blocks.Add(blk);
                hashes.Add(extra[k].Key); scales.Add(1.0f); cnts.Add(1); blockIdx.Add(n + k);
            }
            var tree = new Buf();
            var order = Enumerable.Range(0, N).OrderBy(i => hashes[i]).ToList();
            foreach (var x in Tb2.Bst(N))
            {
                int i = order[x[0]];
                tree.I(0x1C); tree.U(hashes[i]); tree.F(scales[i]); tree.I(x[1]); tree.I(x[2]); tree.I(cnts[i]); tree.I(blocks1 + 0x48 * blockIdx[i]);
            }
            var o2 = new Buf(Bin.Slice(b, 0, hs));
            o2.Add(tree.ToArray()); o2.Add(blocks.ToArray()); o2.Add(keysBlob.ToArray()); o2.Add(pt);
            o2.Zeros(tex1 - o2.Len); o2.Add(texSec);
            o2.Zeros(mesh1 - o2.Len); o2.Add(meshBlob);
            var hdr = (uint[])H.Clone();
            hdr[6] = (uint)meshBlob.Length; hdr[7] = (uint)mesh1; hdr[8] = (uint)N; hdr[9] = (uint)P; hdr[10] = (uint)part1; hdr[19] = (uint)tex1;
            hdr[20] = hdr[21] = hdr[22] = hdr[23] = (uint)o2.Len;
            hdr[25] = hdr[26] = (uint)tex1;
            for (int i = 0; i < 27; i++) o2.PutU(4 * i, hdr[i]);
            return o2.ToArray();
        }

        // pngs: icon number -> path; 0-23 redraw game icons, 24+ add new ones. -> edited sheet or null.
        public static byte[] Import(SortedDictionary<int, string> pngs, Action<string> problem, out List<int> newNumbers)
        {
            newNumbers = new List<int>();
            if (pngs.Count == 0) return null;
            var b = DataHash.Vanilla(ASSET);
            var rects = IconRects(b);
            newNumbers = pngs.Keys.Where(n => n >= FIRST_NEW_ICON).ToList();
            if (newNumbers.Count > 0)
            {
                var cells = NewIconCells(b);
                foreach (int n in newNumbers)
                    if (!cells.ContainsKey(n))
                        problem("icons: no room for icon " + n + "; new icons can be numbered " + FIRST_NEW_ICON + " to " + (cells.Count > 0 ? cells.Keys.Max() : FIRST_NEW_ICON - 1));
                newNumbers = newNumbers.Where(cells.ContainsKey).ToList();
                b = Rebuild(b, newNumbers.Select(n => new KeyValuePair<uint, int>(Bin.Ice("MOD_ICON_" + n.ToString("00")), cells[n])).ToList());
                foreach (int n in newNumbers) rects[n] = CellRect(cells[n]);
            }
            b = Bin.Copy(b);
            var tex = Sprites.FirstTex(b);
            int W = tex.W, changed = 0;
            foreach (var kv in pngs)
            {
                int n = kv.Key;
                int[] r;
                if (!rects.TryGetValue(n, out r)) { if (n < FIRST_NEW_ICON) problem("icons: no icon number " + n); continue; }
                int pw, ph;
                byte[] px;
                try { px = Png.ReadRgba(kv.Value, out pw, out ph); }
                catch (Exception e) { problem("icons: " + Path.GetFileName(kv.Value) + " could not be read (" + e.Message + "); save it as a normal PNG"); continue; }
                if (pw != r[2] || ph != r[3]) { problem("icons: " + Path.GetFileName(kv.Value) + " is " + pw + "x" + ph + ", it must be " + r[2] + "x" + r[3]); continue; }
                px = SwapRB(px);
                for (int row = 0; row < r[3]; row++) Buffer.BlockCopy(px, row * r[2] * 4, b, tex.Ofs + ((r[1] + row) * W + r[0]) * 4, r[2] * 4);
                changed++;
            }
            return changed > 0 ? b : null;
        }
    }

    // ---- the combiner (tools/combine_mods.py + build_mod.number_classes / apply_classes) ----
    class ClassDef
    {
        public string Id, Name, Body, Palette, QuestHud, StatusHud, Description;
        public int Moves, Cls, Was;
        public bool TestSaves;
        public Dictionary<string, string> Art = new Dictionary<string, string>();
    }

    // A dictionary that keeps the order keys were first added (as Python's dict).
    class Ordered<K, V> : IEnumerable<KeyValuePair<K, V>>
    {
        readonly Dictionary<K, V> d;
        readonly List<K> keys = new List<K>();
        public Ordered(IEqualityComparer<K> cmp = null) { d = new Dictionary<K, V>(cmp ?? EqualityComparer<K>.Default); }
        public V this[K k]
        {
            get { return d[k]; }
            set { if (!d.ContainsKey(k)) keys.Add(k); d[k] = value; }
        }
        public bool ContainsKey(K k) { return d.ContainsKey(k); }
        public bool TryGetValue(K k, out V v) { return d.TryGetValue(k, out v); }
        public int Count { get { return keys.Count; } }
        public IEnumerable<K> Keys { get { return keys; } }
        public IEnumerable<V> Values { get { return keys.Select(k => d[k]); } }
        public IEnumerator<KeyValuePair<K, V>> GetEnumerator() { return keys.Select(k => new KeyValuePair<K, V>(k, d[k])).GetEnumerator(); }
        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() { return GetEnumerator(); }
    }

    class CombineJob
    {
        public const string STAMP = "combined.txt", MANIFEST = "_built_by_build_mod.txt";
        static readonly string[] BASE_NAMES = { "empress", "warrior", "shinobi", "witch", "oracle", "bandit" };

        readonly string mods, outDir, cacheDir;
        readonly Action<string> say;
        public readonly List<string> Problems = new List<string>(), Warnings = new List<string>();

        // cacheDir (may be null): encoded game files from the last run, named by their decoded
        // content's MD5, so unchanged files (most class bodies) aren't compressed again.
        public CombineJob(string game, string outDir, string cacheDir, Action<string> say)
        {
            mods = Path.Combine(game, "mods");
            this.outDir = outDir;
            this.cacheDir = cacheDir;
            this.say = say;
            DataHash.Dir = Path.Combine(game, "DataHash");
        }

        readonly HashSet<string> cacheUsed = new HashSet<string>();
        int cacheHits;

        byte[] EncodeCached(string asset, byte[] data)
        {
            if (cacheDir == null) return DataHash.Encode(asset, data);
            string key;
            using (var md5 = MD5.Create()) key = Bin.Hex(md5.ComputeHash(data)) + "." + asset.Substring(asset.LastIndexOf('.') + 1).ToLowerInvariant();
            lock (cacheUsed) cacheUsed.Add(key);
            string p = Path.Combine(cacheDir, key);
            try
            {
                if (File.Exists(p))
                {
                    var hit = File.ReadAllBytes(p);
                    try { File.SetLastWriteTimeUtc(p, DateTime.UtcNow); } catch (UnauthorizedAccessException) { }  // last used (PruneCache)
                    System.Threading.Interlocked.Increment(ref cacheHits);
                    return hit;
                }
            }
            catch (IOException) { }
            var enc = DataHash.Encode(asset, data);
            try
            {
                Directory.CreateDirectory(cacheDir);
                string tmp = p + "." + System.Threading.Thread.CurrentThread.ManagedThreadId + ".tmp";
                File.WriteAllBytes(tmp, enc);
                if (File.Exists(p)) File.Delete(tmp); else File.Move(tmp, p);  // whole files only
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return enc;
        }

        // The cache keeps what was used lately (switching profiles back and forth stays quick):
        // files unused for 30 days go, and the oldest go while it's over 300 MB.
        const long CACHE_MAX = 300L << 20;
        void PruneCache()
        {
            if (cacheDir == null || !Directory.Exists(cacheDir)) return;
            var old = new List<FileInfo>();
            long total = 0;
            foreach (var f in new DirectoryInfo(cacheDir).GetFiles())
            {
                if (cacheUsed.Contains(f.Name)) { total += f.Length; continue; }
                if (f.Name.EndsWith(".tmp") || DateTime.UtcNow - f.LastWriteTimeUtc > TimeSpan.FromDays(30)) { Delete(f); continue; }
                old.Add(f);
                total += f.Length;
            }
            foreach (var f in old.OrderBy(x => x.LastWriteTimeUtc))
            {
                if (total <= CACHE_MAX) break;
                total -= f.Length;
                Delete(f);
            }
        }

        static void Delete(FileInfo f)
        {
            try { f.Delete(); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        void Problem(string where, string msg) { Problems.Add(where + ": " + msg); }
        void Note(string m) { say("  note: " + m); }

        static string Lower(string s) { return s.ToLowerInvariant(); }

        // Folders in mods\ that have a mod.ini, in load order (order.txt, then alphabetically by lower case).
        List<string> ModOrder()
        {
            if (!Directory.Exists(mods)) return new List<string>();
            var found = Directory.GetDirectories(mods).Select(Path.GetFileName)
                .Where(f => File.Exists(Path.Combine(mods, f, "mod.ini")))
                .OrderBy(f => Lower(f), StringComparer.Ordinal).ToList();
            var ordered = new List<string>();
            string order = Path.Combine(mods, "order.txt");
            if (File.Exists(order))
                foreach (string line in File.ReadAllLines(order, new UTF8Encoding(false)))
                {
                    string s = line.Trim().TrimStart('﻿');
                    if (s == "" || s[0] == ';' || s[0] == '#') continue;
                    string hit = found.FirstOrDefault(f => Lower(f) == Lower(s));
                    if (hit != null) { ordered.Add(hit); found.Remove(hit); }
                }
            ordered.AddRange(found);
            return ordered;
        }

        class DataMod { public string Folder, Name, Version, Pkg; }

        List<DataMod> DataMods()
        {
            var o = new List<DataMod>();
            foreach (string folder in ModOrder())
            {
                string ini = Path.Combine(mods, folder, "mod.ini"), pkg = Path.Combine(mods, folder, "data", "package.json");
                if (Ini.Get(ini, "mod", "enabled", "1").Trim() == "0" || !File.Exists(pkg)) continue;
                o.Add(new DataMod { Folder = folder, Name = Ini.Get(ini, "mod", "name", folder), Version = Ini.Get(ini, "mod", "version", "?"), Pkg = pkg });
            }
            return o;
        }

        public static string StampLine(string folder, string pkg)
        {
            var fi = new FileInfo(pkg);
            long unix = (long)Math.Floor((fi.LastWriteTimeUtc - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds);
            return folder + " " + fi.Length + " " + unix;
        }

        // A short name prefix for one mod's new files, so two mods' "pl6_cb00" never clash.
        static string Tag(string folder)
        {
            using (var md5 = MD5.Create()) return "m" + Bin.Hex(md5.ComputeHash(Encoding.UTF8.GetBytes(Lower(folder)))).Substring(0, 6) + "_";
        }

        static readonly Regex NameRe = new Regex(@"^(Addon:\d+:)?(Win/)?(.*?)(\.osb)?$", RegexOptions.Singleline);

        static Func<string, string> Renamer(string folder, Dictionary<string, object> pkg)
        {
            var nw = new HashSet<string>(List(pkg, "new_assets").Select(x => (string)x).Where(a => a.StartsWith("Win/") && a.EndsWith(".osb")).Select(a => a.Substring(4, a.Length - 8)));
            string t = Tag(folder);
            return name =>
            {
                var m = NameRe.Match(name);
                if (!m.Success || !nw.Contains(m.Groups[3].Value)) return name;
                return m.Groups[1].Value + m.Groups[2].Value + t + m.Groups[3].Value + m.Groups[4].Value;
            };
        }

        static object[] List(Dictionary<string, object> d, string k)
        {
            object v;
            if (!d.TryGetValue(k, out v) || v == null) return new object[0];
            if (v is object[]) return (object[])v;
            if (v is System.Collections.ArrayList) return ((System.Collections.ArrayList)v).ToArray();
            return new object[0];
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v;
            return d.TryGetValue(k, out v) && v != null ? Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture) : null;
        }

        static bool Truthy(object v)
        {
            if (v == null) return false;
            if (v is bool) return (bool)v;
            if (v is string) return ((string)v).Length > 0;
            if (v is int) return (int)v != 0;
            if (v is decimal) return (decimal)v != 0;
            return true;
        }

        static string ItemKey(string line)
        {
            var t = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            return t.Length > 0 && t[0] == "new" && t.Length > 1 ? t[1] : (t.Length > 0 ? t[0] : "");
        }

        readonly Dictionary<string, TbTable> tables = new Dictionary<string, TbTable>();

        TbTable Table(string name)
        {
            TbTable t;
            if (!tables.TryGetValue(name, out t)) tables[name] = t = Tb2.Parse(DataHash.Vanilla(name));
            return t;
        }

        static byte[] Utf8(string s) { return new UTF8Encoding(false).GetBytes(s); }

        readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
        readonly List<string> times = new List<string>();
        void Time(string what) { times.Add(what + " " + clock.ElapsedMilliseconds); }

        public bool Run()
        {
            var all = DataMods();
            var spriteFiles = new Dictionary<string, byte[]>();
            var owner = new Dictionary<string, string>();
            var textOwner = new Dictionary<string, string>();
            var items = new Ordered<string, string>();
            var itemOwner = new Dictionary<string, string>();
            var drops = new Ordered<string, string>();
            var dropOwner = new Dictionary<string, string>();
            var layoutRows = new List<object[]>();
            var defs = new Ordered<string, ClassDef>();
            var defOwner = new Dictionary<string, string>();
            var iconSrcs = new List<Tuple<string, int, string>>();
            var pkgs = new List<Tuple<string, string, Dictionary<string, object>, string>>();
            var json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue, RecursionLimit = 1000 };
            foreach (var dm in all)
            {
                var pkg = (Dictionary<string, object>)json.DeserializeObject(File.ReadAllText(dm.Pkg, new UTF8Encoding(false)));
                object fmt;
                if (pkg.TryGetValue("format", out fmt) && Convert.ToInt32(fmt) > 1) { Problem(dm.Name, "made with a newer Build mod; update the mod tools"); continue; }
                string data = Path.GetDirectoryName(dm.Pkg);
                pkgs.Add(Tuple.Create(dm.Folder, dm.Name, pkg, data));
                foreach (Dictionary<string, object> icon in List(pkg, "icons"))
                    iconSrcs.Add(Tuple.Create(dm.Folder, Convert.ToInt32(icon["n"]), Path.Combine(data, Path.Combine(((string)icon["png"]).Split('/')))));
            }

            // new icons: numbered from 24 in a fixed order (mod folder, then the modder's own number)
            var iconMap = new Dictionary<string, int>();
            var iconPngs = new SortedDictionary<int, string>();
            int ii = 0;
            foreach (var s in iconSrcs.OrderBy(x => Lower(x.Item1), StringComparer.Ordinal).ThenBy(x => x.Item2))
            {
                iconMap[s.Item1 + "\0" + s.Item2] = 24 + ii;
                iconPngs[24 + ii] = s.Item3;
                ii++;
            }

            foreach (var p in pkgs)
            {
                string folder = p.Item1, name = p.Item2, data = p.Item4;
                var pkg = p.Item3;
                var rn = Renamer(folder, pkg);
                var pals = new HashSet<string>(List(pkg, "palettes").Select(x => (string)x));
                foreach (string src in List(pkg, "assets"))
                {
                    var b = Zlib.Decompress(File.ReadAllBytes(Path.Combine(data, "assets", Path.Combine(src.Split('/'))) + ".z"));
                    string asset = rn(src);
                    if (asset != src)  // one of the mod's own new files, renamed: fix what points at its old name
                    {
                        if (pals.Contains(src)) b = Palettes.SetId(b, asset.Substring(4, asset.Length - 8));
                        else { int n; b = Sprites.RenameSelfRefs(b, src.Substring(4), asset.Substring(4), out n); }
                    }
                    if (owner.ContainsKey(asset)) Warnings.Add(owner[asset] + " and " + name + " both change " + asset.Substring(4) + "; using " + name + "'s");
                    spriteFiles[asset] = b;
                    owner[asset] = name;
                }
                foreach (Dictionary<string, object> row in List(pkg, "text"))
                {
                    string file = (string)row["file"], label = (string)row["label"];
                    int variant = Convert.ToInt32(row["variant"]);
                    string key = file + "\0" + label + "\0" + variant;
                    if (textOwner.ContainsKey(key) && textOwner[key] != name) Warnings.Add(textOwner[key] + " and " + name + " both change the text " + label + "; using " + name + "'s");
                    textOwner[key] = name;
                    var t = Table(file);
                    t.Rows.RemoveAll(r => r.Label == label && r.Variant == variant);
                    var text = new Dictionary<string, string>();
                    object tx;
                    if (row.TryGetValue("text", out tx) && tx is Dictionary<string, object>)
                        foreach (var kv in (Dictionary<string, object>)tx) text[kv.Key] = kv.Value as string;
                    t.Rows.Add(new TbRow { Label = label, Variant = variant, Aux = Str(row, "aux") ?? "", Text = text });
                }
                foreach (string line0 in List(pkg, "items"))
                {
                    var toks = new List<string>();
                    foreach (string tok0 in line0.Split((char[])null, StringSplitOptions.RemoveEmptyEntries))
                    {
                        string tok = tok0;
                        if (tok.StartsWith("icon="))
                        {
                            int n = Bin.ParseInt0(tok.Substring(5));
                            int mapped;
                            if (n > 23) tok = "icon=" + (iconMap.TryGetValue(folder + "\0" + n, out mapped) ? mapped : n);
                        }
                        else if (tok.StartsWith("sprite=") || tok.StartsWith("sprite_palette="))
                        {
                            int eq = tok.IndexOf('=');
                            tok = tok.Substring(0, eq) + "=" + rn(tok.Substring(eq + 1));
                        }
                        toks.Add(tok);
                    }
                    string line = string.Join(" ", toks), key = ItemKey(line);
                    if (itemOwner.ContainsKey(key) && itemOwner[key] != name) Warnings.Add(itemOwner[key] + " and " + name + " both change the item " + key + "; using " + name + "'s");
                    itemOwner[key] = name;
                    items[key] = line;
                }
                foreach (string line in List(pkg, "drops"))
                {
                    var t = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    string key = string.Join(" ", t.Take(2));
                    if (dropOwner.ContainsKey(key) && dropOwner[key] != name) Warnings.Add(dropOwner[key] + " and " + name + " both change the drop " + key + "; using " + name + "'s");
                    dropOwner[key] = name;
                    drops[key] = line;
                }
                foreach (object[] r in List(pkg, "layout"))
                {
                    var row = (object[])r.Clone();
                    row[0] = rn((string)row[0]);
                    layoutRows.Add(row);
                }
                foreach (Dictionary<string, object> d in List(pkg, "classes"))
                {
                    var c = new ClassDef
                    {
                        Id = Str(d, "id"), Name = Str(d, "name") ?? "", Moves = Convert.ToInt32(d["moves"]),
                        Description = Str(d, "description"),
                        Was = d.ContainsKey("was") && d["was"] != null ? Convert.ToInt32(d["was"]) : 0,
                        TestSaves = d.ContainsKey("test_saves") && Truthy(d["test_saves"]),
                    };
                    Func<string, string> field = f => { string v = Str(d, f); return string.IsNullOrEmpty(v) ? null : rn(v); };
                    c.Body = field("body"); c.Palette = field("palette"); c.QuestHud = field("quest_hud"); c.StatusHud = field("status_hud");
                    object art;
                    if (d.TryGetValue("art", out art) && art is Dictionary<string, object>)
                        foreach (var kv in (Dictionary<string, object>)art) c.Art[kv.Key] = Path.Combine(data, Path.Combine(((string)kv.Value).Split('/')));
                    if (defOwner.ContainsKey(c.Id)) Warnings.Add(defOwner[c.Id] + " and " + name + " both add the class " + c.Id + "; using " + name + "'s");
                    defs[c.Id] = c;
                    defOwner[c.Id] = name;
                }
            }

            Time("read");
            if (layoutRows.Count > 0) ApplyLayout(layoutRows, spriteFiles);
            var numbered = NumberClasses(defs.Values.ToList());
            var classLines = numbered.Count > 0 ? ApplyClasses(numbered, spriteFiles) : new List<string>();
            byte[] sheet = null;
            var newIcons = new List<int>();
            if (iconPngs.Count > 0) sheet = Icons.Import(iconPngs, m => Problems.Add(m), out newIcons);
            Time("classes+icons");
            if (Problems.Count > 0)
            {
                say("The mods couldn't be combined; DataMod was left as it was:\n");
                foreach (string p in Problems) say("  - " + p);
                return false;
            }

            var files = new Dictionary<string, byte[]>();
            if (items.Count > 0) files["items.txt"] = Utf8("# Combined from the mods folder. Edit the mods, not this file.\n" + string.Join("\n", items.Values) + "\n");
            var toEncode = new List<KeyValuePair<string, byte[]>>();  // game files: asset name, decoded content
            foreach (var kv in tables) toEncode.Add(new KeyValuePair<string, byte[]>(kv.Key, Tb2.Build(kv.Value)));
            if (sheet != null) toEncode.Add(new KeyValuePair<string, byte[]>(Icons.ASSET, sheet));
            foreach (var kv in spriteFiles) toEncode.Add(new KeyValuePair<string, byte[]>(kv.Key, kv.Value));
            var encoded = new byte[toEncode.Count][];
            System.Threading.Tasks.Parallel.For(0, toEncode.Count, i => encoded[i] = EncodeCached(toEncode[i].Key, toEncode[i].Value));
            for (int i = 0; i < toEncode.Count; i++) files[DataHash.FileName(toEncode[i].Key)] = encoded[i];
            if (newIcons.Count > 0) files["icons.txt"] = Utf8(string.Concat(newIcons.Select(n => n + "\n")));
            if (classLines.Count > 0) files["classes.txt"] = Utf8(string.Join("\n", classLines) + "\n");
            if (drops.Count > 0) files["drops.txt"] = Utf8("# Combined from the mods folder.\n" + string.Join("\n", drops.Values) + "\n");
            files[STAMP] = Utf8("# The data mods combined into this folder (folder, package size, package time).\n" +
                                "# The mod loader warns when the mods folder no longer matches.\n" +
                                string.Concat(all.Select(m => StampLine(m.Folder, m.Pkg) + "\n")));
            Time("encode");
            WriteOutputs(files);
            Time("write");
            PruneCache();
            Log.Write("combine times (ms since start): " + string.Join(", ", times) + "; " + cacheHits + " of " + toEncode.Count + " game files from the cache");
            foreach (string w in Warnings) say("Overlap: " + w);
            say("Combined " + all.Count + " data mod(s): " + (all.Count > 0 ? string.Join(", ", all.Select(m => m.Name + " " + m.Version)) : "none"));
            foreach (string line in classLines)
            {
                var t = line.Split(' ');
                say("  class " + t[0] + ": " + t.First(x => x.StartsWith("id=")).Substring(3));
            }
            return true;
        }

        void WriteOutputs(Dictionary<string, byte[]> files)
        {
            Directory.CreateDirectory(outDir);
            string man = Path.Combine(outDir, MANIFEST);
            var old = new HashSet<string>();
            if (File.Exists(man))
                foreach (string l in File.ReadAllLines(man))
                    if (l.Trim() != "" && !l.StartsWith("#")) old.Add(l.Trim());
            foreach (var kv in files) File.WriteAllBytes(Path.Combine(outDir, kv.Key), kv.Value);
            foreach (string name in old)
                if (!files.ContainsKey(name))
                {
                    string p = Path.Combine(outDir, name);
                    if (File.Exists(p)) File.Delete(p);
                }
            // a text file, as Python writes it on Windows (CRLF)
            File.WriteAllBytes(man, Utf8("# Files written by Build mod. Others in this folder are left alone.\r\n" +
                                         string.Join("\r\n", files.Keys.OrderBy(k => k, StringComparer.Ordinal)) + "\r\n"));
        }

        void ApplyLayout(List<object[]> rows, Dictionary<string, byte[]> spriteFiles)
        {
            var edits = new Ordered<string, List<Layout.Edit>>();
            foreach (var r in rows)
            {
                string asset = (string)r[0];
                List<Layout.Edit> l;
                if (!edits.TryGetValue(asset, out l)) edits[asset] = l = new List<Layout.Edit>();
                l.Add(new Layout.Edit
                {
                    Anim = r[1] == null ? (int?)null : Convert.ToInt32(r[1]),
                    Part = Convert.ToString(r[2]), Field = Convert.ToString(r[3]),
                    Value = Convert.ToDouble(r[4], System.Globalization.CultureInfo.InvariantCulture),
                });
            }
            foreach (var kv in edits)
            {
                byte[] b;
                if (!spriteFiles.TryGetValue(kv.Key, out b)) b = Sprites.Load(kv.Key);
                var nb = Layout.Apply(b, kv.Value, m => Problems.Add("layout: " + m.Substring(8)));
                say("Layout: " + kv.Value.Count + " change(s) in " + kv.Key);
                if (nb != null) spriteFiles[kv.Key] = nb;
            }
        }

        // Class numbers: a class rides in the slot of the class whose moves it borrows, on the first
        // free page of that column (6 * page + moves), in id order, so everyone gets the same numbers.
        List<ClassDef> NumberClasses(List<ClassDef> defs)
        {
            var used = new HashSet<int>();
            var o = new List<ClassDef>();
            foreach (var d in defs.OrderBy(x => x.Id, StringComparer.Ordinal))
            {
                int n = 6 + d.Moves;
                while (used.Contains(n)) n += 6;
                if (n > 127)
                {
                    Problem("classes", "too many classes borrow the " + BASE_NAMES[d.Moves] + "'s moves (no free page left for " + d.Id + ")");
                    continue;
                }
                used.Add(n);
                d.Cls = n;
                o.Add(d);
            }
            return o.OrderBy(x => x.Cls).ToList();
        }

        static readonly Tuple<string, Tuple<string, string, bool>[]>[] EMPTY_PICTURES = {
            Tuple.Create("Win/com_pl_icon.osb", new[] { Tuple.Create("PlIcon_1", "MOD_EMPTY_PlIcon", false), Tuple.Create("PlIcon_1_24", "MOD_EMPTY_PlIcon_24", false) }),
            Tuple.Create("Win/s_o0200.osb", new[] { Tuple.Create("PlFace_1", "MOD_EMPTY_PlFace", false) }),
            Tuple.Create("Win/cp_o0000.osb", new[] { Tuple.Create("皇女顔01", "MOD_EMPTY_HudFace_1", false), Tuple.Create("皇女顔02", "MOD_EMPTY_HudFace_2", false),
                                                      Tuple.Create("皇女ライフ", "MOD_EMPTY_HudFrame", true) }),
        };

        byte[] Sheet(Dictionary<string, byte[]> spriteFiles, string asset)
        {
            byte[] b;
            return spriteFiles.TryGetValue(asset, out b) ? b : Sprites.Load(asset);
        }

        // Numbered classes -> classes.txt lines; their names, descriptions and pictures go into the shared game files.
        List<string> ApplyClasses(List<ClassDef> defs, Dictionary<string, byte[]> spriteFiles)
        {
            var lines = new List<string>();
            var art = new SortedDictionary<int, ClassArt>();
            foreach (var d in defs)
            {
                int cls = d.Cls, bas = d.Moves;
                var line = new StringBuilder(cls + " base=" + bas + " id=" + d.Id);
                if (d.Was != 0) line.Append(" was=" + d.Was);
                if (d.Body != null) line.Append(" body=" + d.Body + ".osb");
                if (d.Palette != null) line.Append(" palette=" + d.Palette + ".osb");
                if (d.QuestHud != null) line.Append(" quest_hud=" + d.QuestHud + ".osb");
                if (d.StatusHud != null) line.Append(" status_hud=" + d.StatusHud + ".osb");
                string palAsset = "Win/" + d.Palette + ".osb";
                if (d.Body != null && d.Palette != null && spriteFiles.ContainsKey(palAsset))
                    // the game finds a class's palette by the hash of "<body>.osp"
                    spriteFiles[palAsset] = Palettes.SetId(spriteFiles[palAsset], d.Palette, d.Body);
                if (d.TestSaves) line.Append(" test_saves=1");
                if (!string.IsNullOrEmpty(d.Description))
                {
                    // select-screen info box: PLxx_INFO in sys_plselect.tb2, pages split by "|"; missing pages are the base class's
                    var t = Table("sys_plselect.tb2");
                    string label = "PL" + cls.ToString("00") + "_INFO", baseLabel = "PL" + bas.ToString("00") + "_INFO";
                    var pages = d.Description.Split('|').Select(x => x.Trim().Replace("\\n", "\n")).ToList();
                    t.Rows.RemoveAll(r => r.Label == label);
                    foreach (var r in t.Rows.Where(r => r.Label == baseLabel).ToList())
                    {
                        int v = r.Variant;
                        var text = v < pages.Count ? t.Langs.ToDictionary(l => l, l => pages[v]) : new Dictionary<string, string>(r.Text);
                        t.Rows.Add(new TbRow { Label = label, Variant = v, Aux = r.Aux, Text = text });
                    }
                    line.Append(" info=" + label);
                }
                var ca = new ClassArt { Base = bas };
                foreach (var kv in d.Art) ca.Pics[kv.Key] = kv.Value;
                art[cls] = ca;
                string nameLabel = "CLASS_" + cls.ToString("00");
                foreach (string table in new[] { "sys_common.tb2", "sys_status_help.tb2" })
                {
                    var t = Table(table);
                    var tpl = t.Rows.FirstOrDefault(r => r.Label == "CLASS_00");
                    if (tpl == null) continue;
                    t.Rows.RemoveAll(r => r.Label == nameLabel);
                    t.Rows.Add(new TbRow { Label = nameLabel, Variant = 0, Aux = tpl.Aux, Text = t.Langs.ToDictionary(l => l, l => d.Name) });
                }
                line.Append(" name=" + nameLabel);
                lines.Add(line + " empty=1");
            }
            if (art.Count == 0) return lines;
            Action<string> note = Note;
            Func<int, int> clsOf = i => int.Parse(lines[i].Split(' ')[0]);
            try
            {
                // character select art (s_o0a00)
                HashSet<int> highlights, pages;
                Time("text");
                spriteFiles["Win/s_o0a00.osb"] = SelectArt.Build(Sheet(spriteFiles, "Win/s_o0a00.osb"), art, note, out highlights, out pages);
                for (int i = 0; i < lines.Count; i++)
                {
                    int cls = clsOf(i);
                    if (highlights.Contains(cls)) lines[i] += " highlight=1";
                    if (pages.Contains(cls / 6)) lines[i] += " pagelayout=1";
                }
                Time("select");
                // portraits (com_pl_icon)
                Dictionary<int, int> has;
                var nb = SelectArt.Portraits(Sheet(spriteFiles, "Win/com_pl_icon.osb"), art, note, out has);
                if (has.Count > 0)
                {
                    spriteFiles["Win/com_pl_icon.osb"] = nb;
                    for (int i = 0; i < lines.Count; i++) { int v; if (has.TryGetValue(clsOf(i), out v) && v != 0) lines[i] += " portrait=" + v; }
                    say("Portraits: " + has.Count + " class(es)");
                }
                Time("portraits");
                // the bar party menu's big face (s_o0200)
                HashSet<int> bf;
                nb = SelectArt.BarFaces(Sheet(spriteFiles, "Win/s_o0200.osb"), art, note, out bf);
                if (bf.Count > 0)
                {
                    spriteFiles["Win/s_o0200.osb"] = nb;
                    for (int i = 0; i < lines.Count; i++) if (bf.Contains(clsOf(i))) lines[i] += " barface=1";
                }
                Time("barfaces");
                // the "empty" picture for every class picture spot
                foreach (var e in EMPTY_PICTURES)
                {
                    try { spriteFiles[e.Item1] = SelectArt.AddEmpties(Sheet(spriteFiles, e.Item1), e.Item2, note); }
                    catch (DataError x) { Problem("classes", "empty picture in " + e.Item1 + ": " + x.Message); return lines; }
                }
                Time("empties");
                // quest HUD faces and life frames (cp_o0000)
                HashSet<int> hf;
                nb = SelectArt.HudFaces(Sheet(spriteFiles, "Win/cp_o0000.osb"), art, note, out hf);
                if (hf.Count > 0)
                {
                    spriteFiles["Win/cp_o0000.osb"] = nb;
                    for (int i = 0; i < lines.Count; i++) if (hf.Contains(clsOf(i))) lines[i] += " hudface=1";
                    say("HUD faces: " + hf.Count + " class(es)");
                }
                HashSet<int> fr;
                nb = SelectArt.HudFrames(Sheet(spriteFiles, "Win/cp_o0000.osb"), art, note, out fr);
                if (fr.Count > 0)
                {
                    spriteFiles["Win/cp_o0000.osb"] = nb;
                    for (int i = 0; i < lines.Count; i++) if (fr.Contains(clsOf(i))) lines[i] += " hudframe=1";
                    say("HUD life frames: " + fr.Count + " class(es)");
                }
                say("Select screen art: " + art.Values.Sum(a => (a.Get("panel") != null ? 1 : 0) + (a.Get("highlight") != null ? 1 : 0)) +
                    " class picture(s), " + pages.Count + " page layout(s)");
            }
            catch (DataError x) { Problem("classes", x.Message); return lines; }
            // page prompt texts for the select screen ("RT Page 2/3"): PadInfo.tb2 MOD_PAGE_<page>
            int npages = art.Keys.Max() / 6 + 1;
            var pad = Table("PadInfo.tb2");
            var ret = pad.Rows.First(r => r.Label == "RETURN");
            pad.Rows.RemoveAll(r => r.Label.StartsWith("MOD_PAGE_"));
            for (int page = 0; page < npages; page++)
                pad.Rows.Add(new TbRow { Label = "MOD_PAGE_" + (page + 1), Variant = 0, Aux = ret.Aux, Text = pad.Langs.ToDictionary(l => l, l => "Page " + (page + 1) + "/" + npages) });
            return lines;
        }
    }
}
