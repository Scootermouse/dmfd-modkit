// SPDX-License-Identifier: GPL-3.0-or-later
// The approved mod list (mods.json, kept with the mod kit's releases): what the Browse page shows
// and installs. Every download is checked against the list (size and SHA-256) before it reaches
// the mods folder. The list is cached in ModManager\browse, so the page works offline.
//
// mods.json:
//   { "format": 1, "updated": "2026-11-10",
//     "mods": [ { "id": "HolyUp" (its folder), "name", "author", "summary", "description",
//                 "tags": ["classes", ...], "type": "data" | "code" | "code+data",
//                 "online": "same" | "any", "license", "homepage" (a page to discuss it),
//                 "images": ["url", ...],
//                 "versions": [ { "version", "date", "url", "size", "sha256",
//                                 "fingerprint" (data mods: the loader's package fingerprint),
//                                 "changes" }, ... newest first ] } ],
//     "tools": { "version", "date", "url", "size", "sha256" } (the modding tools, optional) }
//   Links may be relative to the list's own location.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace DmfdManager
{
    public class ListVersion
    {
        public string version, date, url, sha256, fingerprint, changes;
        public long size;
    }

    public class ListMod
    {
        public string id, name, author, summary, description, type, online, license, homepage;
        public List<string> tags = new List<string>(), images = new List<string>();
        public List<ListVersion> versions = new List<ListVersion>();
        public ListVersion Latest { get { return versions.FirstOrDefault(); } }
        public bool HasCode { get { return (type ?? "").Contains("code"); } }
    }

    public class ListFile
    {
        public int format;
        public string updated;
        public List<ListMod> mods = new List<ListMod>();
        public ListVersion tools;  // the modding tools zip (Build a mod page), optional
    }

    static class ModList
    {
        static string Dir { get { return Path.Combine(Paths.Data, "browse"); } }
        static string CacheFile { get { return Path.Combine(Dir, "mods.json"); } }
        static string CacheSource { get { return Path.Combine(Dir, "source.txt"); } }
        public static string Override;  // --mod-list (testing)

        // Where the list comes from: settings.ini [browse] list= (a URL or a file), else mods.json
        // next to this manager (a kit with its list), else none yet.
        public static string Source
        {
            get
            {
                if (!string.IsNullOrEmpty(Override)) return Override;
                string s = Ini.Get(Paths.SettingsFile, "browse", "list");
                if (s != "") return s;
                string here = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mods.json");
                return File.Exists(here) ? here : "";
            }
            set { Ini.Set(Paths.SettingsFile, "browse", "list", value ?? ""); }
        }

        public static ListFile Current;     // last loaded (fetched or cached)
        public static DateTime LoadedAt;    // when it was fetched
        public static bool Offline;         // the fetch failed: Current is the cached copy
        public static string Problem;       // why it failed

        static bool IsWeb(string s) { return s.StartsWith("http://", StringComparison.OrdinalIgnoreCase) || s.StartsWith("https://", StringComparison.OrdinalIgnoreCase); }

        static WebClient Web()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            var w = new WebClient();
            w.Headers[HttpRequestHeader.UserAgent] = "DMFD-Mod-Manager/" + MainWindow.Version;
            return w;
        }

        // A link from the list, made absolute against the list's location.
        public static string Resolve(string link)
        {
            if (string.IsNullOrEmpty(link) || IsWeb(link) || Path.IsPathRooted(link)) return link;
            string src = Source;
            if (IsWeb(src)) return new Uri(new Uri(src), link).ToString();
            return Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(src)), link.Replace('/', '\\')));
        }

        static ListFile Parse(string text)
        {
            var f = new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.Deserialize<ListFile>(text);
            if (f == null || f.mods == null) throw new Exception("the list isn't in the expected format");
            if (f.format > 1) throw new Exception("the list is newer than this manager: update the Mod Manager");
            foreach (var m in f.mods)
            {
                if (m.tags == null) m.tags = new List<string>();
                if (m.images == null) m.images = new List<string>();
                if (m.versions == null) m.versions = new List<ListVersion>();
            }
            f.mods.RemoveAll(m => string.IsNullOrEmpty(m.id) || m.versions.Count == 0);
            return f;
        }

        // Fetch the list (or use the cached one when that fails). Runs on any thread.
        public static void Load()
        {
            string src = Source;
            Problem = null;
            if (src == "")
            {
                Current = null;
                Problem = "No mod list is set up yet.";
                return;
            }
            try
            {
                string text;
                if (IsWeb(src)) using (var w = Web()) text = Encoding.UTF8.GetString(w.DownloadData(src));
                else text = File.ReadAllText(src, Encoding.UTF8);
                Current = Parse(text);
                Directory.CreateDirectory(Dir);
                File.WriteAllText(CacheFile, text, new UTF8Encoding(false));
                File.WriteAllText(CacheSource, src);
                LoadedAt = DateTime.Now;
                Offline = false;
                Log.Write("mod list: " + Current.mods.Count + " mod(s) from " + src);
            }
            catch (Exception e)
            {
                Problem = e.Message;
                Offline = true;
                try
                {
                    if (File.Exists(CacheFile) && File.Exists(CacheSource) && File.ReadAllText(CacheSource) == src)
                    {
                        Current = Parse(File.ReadAllText(CacheFile, Encoding.UTF8));
                        LoadedAt = File.GetLastWriteTime(CacheFile);
                    }
                }
                catch (Exception) { }
                Log.Write("mod list: couldn't fetch " + src + ": " + e.Message + (Current != null ? " (using the saved copy)" : ""));
            }
        }

        public static ListMod Find(string id)
        {
            return Current == null ? null : Current.mods.FirstOrDefault(m => string.Equals(m.id, id, StringComparison.OrdinalIgnoreCase));
        }

        // "1.10" > "1.9": compared number by number, then as text.
        public static int CompareVersions(string a, string b)
        {
            var pa = (a ?? "").Split('.', '-', ' ');
            var pb = (b ?? "").Split('.', '-', ' ');
            for (int i = 0; i < Math.Max(pa.Length, pb.Length); i++)
            {
                string x = i < pa.Length ? pa[i] : "0", y = i < pb.Length ? pb[i] : "0";
                int nx, ny;
                int c = int.TryParse(x, out nx) && int.TryParse(y, out ny) ? nx.CompareTo(ny) : string.CompareOrdinal(x, y);
                if (c != 0) return c;
            }
            return 0;
        }

        // A picture from the list, cached on disk; null if it can't be had. Any thread.
        public static string Picture(string link)
        {
            string url = Resolve(link);
            if (string.IsNullOrEmpty(url)) return null;
            if (!IsWeb(url)) return File.Exists(url) ? url : null;
            string name;
            using (var sha = SHA1.Create()) name = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(url))).Replace("-", "").ToLowerInvariant();
            string p = Path.Combine(Dir, "images", name + Path.GetExtension(new Uri(url).AbsolutePath));
            if (File.Exists(p)) return p;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(p));
                using (var w = Web()) w.DownloadFile(url, p + ".tmp");
                File.Move(p + ".tmp", p);
                return p;
            }
            catch (Exception) { return null; }
        }

        // Download one version and check it against the list; -> the checked zip. Any thread.
        public static string Download(ListMod m, ListVersion v) { return Download(m.name, m.id, v); }

        static string Download(string name, string id, ListVersion v)
        {
            if (string.IsNullOrEmpty(v.sha256)) throw new Exception(name + " " + v.version + " has no checksum on the list, so it can't be checked: not installed.");
            string url = Resolve(v.url);
            string dir = Path.Combine(Paths.Data, "downloads");
            Directory.CreateDirectory(dir);
            string zip = Path.Combine(dir, id + "-" + v.version + ".zip");
            if (IsWeb(url)) using (var w = Web()) w.DownloadFile(url, zip);
            else File.Copy(url, zip, true);
            var fi = new FileInfo(zip);
            string sum;
            using (var sha = SHA256.Create()) using (var s = File.OpenRead(zip)) sum = BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
            if ((v.size > 0 && fi.Length != v.size) || !string.Equals(sum, v.sha256.Trim(), StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(zip);
                throw new Exception(name + " " + v.version + ": the download doesn't match the list (damaged, or not the file the list means). Not installed.");
            }
            return zip;
        }

        // Download, check, install (keeping its on/off setting). -> the mod's name.
        public static string Install(ListMod m, ListVersion v)
        {
            string zip = Download(m, v);
            try
            {
                string name = ModStore.Install(zip);
                Log.Write("installed " + m.name + " " + v.version + " from the mod list");
                return name;
            }
            finally { try { File.Delete(zip); } catch (IOException) { } }
        }

        // ---- The modding tools (Build a mod page) ----
        // Where they are set up (settings.ini [tools] folder=, default Documents\DMFD Modding) and
        // which version is there ([tools] version=, "" = not set up).
        public static string ToolsFolder
        {
            get
            {
                string f = Ini.Get(Paths.SettingsFile, "tools", "folder");
                return f != "" ? f : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "DMFD Modding");
            }
        }
        public static string ToolsVersion
        {
            get { return File.Exists(Path.Combine(ToolsFolder, "Build mod.bat")) ? Ini.Get(Paths.SettingsFile, "tools", "version") : ""; }
        }

        // Download the tools, check them against the list and unpack them into the folder. The
        // modder's own work is never touched: mod\ isn't in the zip, and example files already
        // there are kept. tools\game.txt tells the tools where the game is. Any thread.
        public static void SetUpTools(ListVersion v, string folder)
        {
            string zip = Download("The modding tools", "ModdingTools", v);
            try
            {
                Directory.CreateDirectory(folder);
                string root = Path.GetFullPath(folder).TrimEnd('\\') + "\\";
                using (var z = System.IO.Compression.ZipFile.OpenRead(zip))
                    foreach (var e in z.Entries)
                    {
                        if (e.FullName.EndsWith("/")) continue;
                        string dst = Path.GetFullPath(Path.Combine(root, e.FullName.Replace('/', '\\')));
                        if (!dst.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;  // stays inside
                        if (e.FullName.StartsWith("examples/", StringComparison.OrdinalIgnoreCase) && File.Exists(dst)) continue;
                        Directory.CreateDirectory(Path.GetDirectoryName(dst));
                        System.IO.Compression.ZipFileExtensions.ExtractToFile(e, dst, true);
                    }
                File.WriteAllText(Path.Combine(root, "tools", "game.txt"), Paths.Game, new UTF8Encoding(false));
                Ini.Set(Paths.SettingsFile, "tools", "folder", folder);
                Ini.Set(Paths.SettingsFile, "tools", "version", v.version);
                Log.Write("modding tools " + v.version + " set up in " + folder);
            }
            finally { try { File.Delete(zip); } catch (IOException) { } }
        }

        // An installed mod against the list: null (not on it), or the list entry and whether it's
        // the latest, older (update) or newer.
        public static string State(ModInfo installed, ListMod m)
        {
            if (installed == null) return "missing";
            int c = CompareVersions(installed.Version, m.Latest.version);
            return c == 0 ? "latest" : c < 0 ? "update" : "newer";
        }

        // The exact version a room needs, if the list has it (and, for data mods, the same build).
        public static ListVersion Exact(RoomMod need, out ListMod mod)
        {
            mod = Find(need.Folder);
            if (mod == null) return null;
            return mod.versions.FirstOrDefault(v => v.version == need.Version &&
                (need.Fp == "-" || string.IsNullOrEmpty(v.fingerprint) || v.fingerprint == need.Fp));
        }
    }
}
