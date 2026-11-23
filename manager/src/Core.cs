// DMFD Mod Manager: everything that isn't drawing (finding the game, mods, combining,
// save backups, logs, launching). C# 7.3, .NET Framework 4.x (ships with Windows).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace DmfdManager
{
    static class Ini
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string sec, string key, string def, StringBuilder buf, int size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileString(string sec, string key, string val, string file);

        public static string Get(string file, string sec, string key, string def = "")
        {
            if (!File.Exists(file)) return def;
            // read ourselves (UTF-8): Windows' ini functions read UTF-8 files as ANSI
            string cur = null;
            foreach (string raw in File.ReadAllLines(file, Encoding.UTF8))
            {
                string line = raw.Trim().TrimStart('﻿');
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                if (line[0] == '[') { cur = line.Trim('[', ']').Trim(); continue; }
                int eq = line.IndexOf('=');
                if (eq < 0 || !string.Equals(cur, sec, StringComparison.OrdinalIgnoreCase)) continue;
                if (string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase))
                    return line.Substring(eq + 1).Trim();
            }
            return def;
        }

        // Rewrites one key, keeping the rest of the file (comments too) as it is.
        public static void Set(string file, string sec, string key, string val)
        {
            var lines = File.Exists(file) ? File.ReadAllLines(file, Encoding.UTF8).ToList() : new List<string>();
            string cur = null;
            int secEnd = -1;
            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i].Trim().TrimStart('﻿');
                if (line.StartsWith("[")) { cur = line.Trim('[', ']').Trim(); continue; }
                if (!string.Equals(cur, sec, StringComparison.OrdinalIgnoreCase)) continue;
                secEnd = i;
                int eq = line.IndexOf('=');
                if (eq > 0 && !line.StartsWith(";") && string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase))
                {
                    lines[i] = key + "=" + val;
                    File.WriteAllLines(file, lines, new UTF8Encoding(false));
                    return;
                }
            }
            if (secEnd < 0)
            {
                if (cur == null && lines.Count == 0) { }
                lines.Add("[" + sec + "]");
                lines.Add(key + "=" + val);
            }
            else
            {
                int at = secEnd + 1;
                lines.Insert(at, key + "=" + val);
            }
            File.WriteAllLines(file, lines, new UTF8Encoding(false));
        }
    }

    // Where everything is. Found through Steam, or set in Settings (kept in ModManager\settings.ini).
    static class Paths
    {
        public const string AppId = "1149440";
        public static string Game = "";        // folder with game.exe
        public static string Saves = "";       // folder with SystemData.bin
        public static string SteamPath = "";
        public static string FoundThrough = "";
        public static bool NoSteamArt;         // --no-steam-art (testing)
        public static string HeroOverride;     // --hero <picture> (testing)

        public static string Mods { get { return Path.Combine(Game, "mods"); } }
        public static string Data { get { return Path.Combine(Game, "ModManager"); } }
        public static string SettingsFile { get { return Path.Combine(Data, "settings.ini"); } }
        public static string Backups { get { return Path.Combine(Data, "save backups"); } }
        public static string LoaderDll { get { return Path.Combine(Game, "winmm.dll"); } }
        public static bool GameOk { get { return Game != "" && File.Exists(Path.Combine(Game, "game.exe")); } }

        public static string HeroArt
        {
            get
            {
                if (HeroOverride != null) return HeroOverride;
                if (NoSteamArt || SteamPath == "") return null;
                string p = Path.Combine(SteamPath, "appcache", "librarycache", AppId, "library_hero.jpg");
                return File.Exists(p) ? p : null;
            }
        }
        public static string LogoArt
        {
            get
            {
                if (NoSteamArt || SteamPath == "") return null;
                string p = Path.Combine(SteamPath, "appcache", "librarycache", AppId, "logo.png");
                return File.Exists(p) ? p : null;
            }
        }

        // Game folder: settings, then Steam (its library folders and the game's app manifest).
        public static void Find(string gameArg, string savesArg)
        {
            try { SteamPath = (Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", "") as string ?? "").Replace('/', '\\'); }
            catch { SteamPath = ""; }
            if (!string.IsNullOrEmpty(gameArg)) { Game = gameArg; FoundThrough = "Chosen when starting the manager"; }
            if (Game == "")
            {
                // next to the manager itself (where it's installed)
                string here = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
                if (File.Exists(Path.Combine(here, "game.exe"))) { Game = here; FoundThrough = "The manager's own folder"; }
            }
            if (Game == "" && SteamPath != "") { Game = FromSteam(); if (Game != "") FoundThrough = "Steam library"; }
            if (Game != "")
            {
                string saved = Ini.Get(SettingsFile, "paths", "game");
                if (saved != "" && File.Exists(Path.Combine(saved, "game.exe")) && string.IsNullOrEmpty(gameArg)) { Game = saved; FoundThrough = "Chosen in Settings"; }
            }
            Saves = !string.IsNullOrEmpty(savesArg) ? savesArg : "";
            if (Saves == "" && Game != "") Saves = Ini.Get(SettingsFile, "paths", "saves");
            if (Saves == "" || !Directory.Exists(Saves)) Saves = FindSaves();
        }

        static string FromSteam()
        {
            var libs = new List<string> { SteamPath };
            string vdf = Path.Combine(SteamPath, "steamapps", "libraryfolders.vdf");
            try
            {
                if (File.Exists(vdf))
                    foreach (string line in File.ReadAllLines(vdf))
                    {
                        string t = line.Trim();
                        if (t.StartsWith("\"path\""))
                            libs.Add(t.Substring(6).Trim().Trim('"').Replace("\\\\", "\\"));
                    }
            }
            catch { }
            foreach (string lib in libs.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                string acf = Path.Combine(lib, "steamapps", "appmanifest_" + AppId + ".acf");
                if (!File.Exists(acf)) continue;
                string dir = "Dragon Marked For Death";
                foreach (string line in File.ReadAllLines(acf))
                {
                    string t = line.Trim();
                    if (t.StartsWith("\"installdir\"")) dir = t.Substring(12).Trim().Trim('"');
                }
                string g = Path.Combine(lib, "steamapps", "common", dir);
                if (File.Exists(Path.Combine(g, "game.exe"))) return g;
            }
            return "";
        }

        // The newest SystemData.bin under Steam's userdata\<account>\1149440\local.
        static string FindSaves()
        {
            if (SteamPath == "") return "";
            try
            {
                string ud = Path.Combine(SteamPath, "userdata");
                if (!Directory.Exists(ud)) return "";
                return Directory.GetDirectories(ud)
                    .Select(a => Path.Combine(a, AppId, "local"))
                    .Where(d => File.Exists(Path.Combine(d, "SystemData.bin")))
                    .OrderByDescending(d => File.GetLastWriteTime(Path.Combine(d, "SystemData.bin")))
                    .FirstOrDefault() ?? "";
            }
            catch { return ""; }
        }
    }

    class ModInfo
    {
        public string Folder, Dir, Name, Version, Author, Description, Dll, License;
        public bool Enabled, HasData, DllMissing;
        public long SizeBytes;
        public int FileCount;
        public string Ini { get { return Path.Combine(Dir, "mod.ini"); } }
        public string Type
        {
            get
            {
                if (Dll != "" && HasData) return "Code + data";
                if (Dll != "") return "Code";
                if (HasData) return "Data";
                return "Empty";
            }
        }
    }

    static class ModStore
    {
        // Mods in load order: mods\order.txt first, then alphabetically in lower case, as the
        // loader (_stricmp) and the combiner do ("A_b" before "Ab": '_' sorts before letters).
        public static List<ModInfo> List()
        {
            var mods = new List<ModInfo>();
            if (!Directory.Exists(Paths.Mods)) return mods;
            var found = Directory.GetDirectories(Paths.Mods)
                .Where(d => File.Exists(Path.Combine(d, "mod.ini")))
                .Select(Path.GetFileName)
                .OrderBy(f => f.ToLowerInvariant(), StringComparer.Ordinal).ToList();
            var ordered = new List<string>();
            string order = Path.Combine(Paths.Mods, "order.txt");
            if (File.Exists(order))
                foreach (string raw in File.ReadAllLines(order))
                {
                    string s = raw.Trim();
                    if (s == "" || s[0] == ';' || s[0] == '#') continue;
                    string hit = found.FirstOrDefault(f => string.Equals(f, s, StringComparison.OrdinalIgnoreCase));
                    if (hit != null) { ordered.Add(hit); found.Remove(hit); }
                }
            ordered.AddRange(found);
            foreach (string f in ordered) mods.Add(Read(f));
            return mods;
        }

        static ModInfo Read(string folder)
        {
            string dir = Path.Combine(Paths.Mods, folder), ini = Path.Combine(dir, "mod.ini");
            var m = new ModInfo
            {
                Folder = folder, Dir = dir,
                Name = Ini.Get(ini, "mod", "name", folder),
                Version = Ini.Get(ini, "mod", "version", "?"),
                Author = Ini.Get(ini, "mod", "author"),
                Description = Ini.Get(ini, "mod", "description"),
                Dll = Ini.Get(ini, "mod", "dll"),
                License = Ini.Get(ini, "mod", "license"),
                Enabled = Ini.Get(ini, "mod", "enabled", "1") != "0",
                HasData = File.Exists(Path.Combine(dir, "data", "package.json")),
            };
            m.DllMissing = m.Dll != "" && !File.Exists(Path.Combine(dir, m.Dll));
            try
            {
                var files = Directory.GetFiles(dir, "*", SearchOption.AllDirectories);
                m.FileCount = files.Length;
                m.SizeBytes = files.Sum(p => new FileInfo(p).Length);
            }
            catch { }
            return m;
        }

        public static void SetEnabled(ModInfo m, bool on)
        {
            Ini.Set(m.Ini, "mod", "enabled", on ? "1" : "0");
            m.Enabled = on;
        }

        public static void SaveOrder(IEnumerable<ModInfo> mods)
        {
            Directory.CreateDirectory(Paths.Mods);
            File.WriteAllLines(Path.Combine(Paths.Mods, "order.txt"),
                new[] { "; Load order, written by the Mod Manager. Later mods win when two change the same thing." }
                    .Concat(mods.Select(m => m.Folder)), new UTF8Encoding(false));
        }

        // Install a mod from a zip or a folder: whatever holds a mod.ini becomes mods\<folder>.
        // Returns what was installed, or throws with a message in plain words.
        public static string Install(string source)
        {
            string tmp = null, root;
            try
            {
                if (File.Exists(source) && source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                {
                    tmp = Path.Combine(Path.GetTempPath(), "dmfd_mod_" + Guid.NewGuid().ToString("N"));
                    ZipFile.ExtractToDirectory(source, tmp);
                    root = tmp;
                }
                else if (Directory.Exists(source)) root = source;
                else throw new Exception("Only .zip files and folders can be installed (7z and rar: unpack them first).");
                // the folder with mod.ini: the top, or one folder down
                string modDir = File.Exists(Path.Combine(root, "mod.ini")) ? root
                    : Directory.GetDirectories(root).FirstOrDefault(d => File.Exists(Path.Combine(d, "mod.ini")));
                if (modDir == null) throw new Exception("This isn't a DMFD mod: there's no mod.ini in it.");
                string ini = Path.Combine(modDir, "mod.ini");
                string name = Ini.Get(ini, "mod", "name", Path.GetFileName(modDir));
                string folder = Path.GetFileName(modDir.TrimEnd('\\'));
                if (modDir == tmp || folder.StartsWith("dmfd_mod_")) folder = name;
                // letters, digits, _ and - only (what Build mod makes, and safe for every tool)
                folder = new string(folder.Where(c => c < 128 && (char.IsLetterOrDigit(c) || c == '_' || c == '-')).ToArray());
                if (folder == "") folder = "Mod";
                string dst = Path.Combine(Paths.Mods, folder);
                if (string.Equals(Path.GetFullPath(modDir).TrimEnd('\\'), Path.GetFullPath(dst).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                    throw new Exception(name + " is already in the mods folder.");
                string keepEnabled = Directory.Exists(dst) ? Ini.Get(Path.Combine(dst, "mod.ini"), "mod", "enabled", "1") : null;
                if (Directory.Exists(dst)) Recycle(dst);  // an older version: to the Recycle Bin
                CopyDir(modDir, dst);
                if (keepEnabled != null) Ini.Set(Path.Combine(dst, "mod.ini"), "mod", "enabled", keepEnabled);
                Log.Write("installed " + name + " into mods\\" + folder + " from " + source);
                return name;
            }
            finally
            {
                if (tmp != null) try { Directory.Delete(tmp, true); } catch { }
            }
        }

        public static void Remove(ModInfo m)
        {
            Recycle(m.Dir);
            Log.Write("removed " + m.Name + " (mods\\" + m.Folder + " to the Recycle Bin)");
        }

        public static void Recycle(string dir)
        {
            Microsoft.VisualBasic.FileIO.FileSystem.DeleteDirectory(dir,
                Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
        }

        public static void CopyDir(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (string f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
            foreach (string d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
        }

        // Extra classes now in the game (DataMod\classes.txt, written when combining).
        public static int ExtraClasses()
        {
            string p = Path.Combine(Paths.Game, "DataMod", "classes.txt");
            if (!File.Exists(p)) return 0;
            return File.ReadAllLines(p).Count(l => l.Length > 0 && char.IsDigit(l[0]));
        }
    }

    // The mod kit unpacked somewhere else than the game folder (the manager next to winmm.dll and a
    // mods folder): what of it isn't in the game yet, and copying it there.
    static class Kit
    {
        public static string Dir { get { return AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\'); } }
        static string KitLoader { get { return Path.Combine(Dir, "winmm.dll"); } }
        static string KitMods { get { return Path.Combine(Dir, "mods"); } }

        // A kit here, and not the game folder itself.
        public static bool Here
        {
            get
            {
                return Paths.GameOk && File.Exists(KitLoader) &&
                       !string.Equals(Path.GetFullPath(Dir), Path.GetFullPath(Paths.Game).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
            }
        }

        public static bool LoaderToCopy
        {
            get { return Here && (!File.Exists(Paths.LoaderDll) || !File.ReadAllBytes(KitLoader).SequenceEqual(File.ReadAllBytes(Paths.LoaderDll))); }
        }

        // Kit mods missing from the game's mods folder, or there in another version: (folder, name version).
        public static List<KeyValuePair<string, string>> ModsToCopy()
        {
            var list = new List<KeyValuePair<string, string>>();
            if (!Here || !Directory.Exists(KitMods)) return list;
            foreach (string d in Directory.GetDirectories(KitMods))
            {
                string ini = Path.Combine(d, "mod.ini");
                if (!File.Exists(ini)) continue;
                string folder = Path.GetFileName(d), ver = Ini.Get(ini, "mod", "version", "?");
                string have = Path.Combine(Paths.Mods, folder, "mod.ini");
                if (File.Exists(have) && Ini.Get(have, "mod", "version", "?") == ver) continue;
                list.Add(new KeyValuePair<string, string>(d, Ini.Get(ini, "mod", "name", folder) + " " + ver));
            }
            return list;
        }

        // Copy the loader (if different) and the kit's new or other-version mods into the game.
        public static string Install()
        {
            if (Launcher.GameRunning()) throw new Exception("Close the game first: the mod loader can't be replaced while it runs.");
            var done = new List<string>();
            if (LoaderToCopy)
            {
                File.Copy(KitLoader, Paths.LoaderDll, true);
                done.Add("the mod loader");
            }
            Directory.CreateDirectory(Paths.Mods);
            foreach (var m in ModsToCopy()) done.Add(ModStore.Install(m.Key));
            Log.Write("installed the mod kit from " + Dir + ": " + string.Join(", ", done));
            return done.Count == 0 ? "Nothing to copy: the game already has this kit." : "Installed " + string.Join(", ", done) + " into the game folder.";
        }
    }

    // The last online room whose mods didn't match: the loader writes its mod list to
    // mods\last_room.txt (lines "folder<TAB>version<TAB>fingerprint<TAB>name", or "none" for a host
    // without mods). Mods that matter online: data mods, and code mods with online=same.
    class RoomMod { public string Folder, Version, Fp, Name; }

    static class RoomCheck
    {
        public static string FilePath { get { return Path.Combine(Paths.Mods, "last_room.txt"); } }

        public static List<RoomMod> Read(out bool hostHasMods)
        {
            hostHasMods = false;
            var list = new List<RoomMod>();
            foreach (string raw in File.ReadAllLines(FilePath))
            {
                string line = raw.TrimEnd('\r');
                if (line == "" || line.StartsWith("#")) continue;
                if (line.Trim() == "none") return list;
                var f = line.Split('\t');
                if (f.Length < 2 || f[0] == "") continue;  // not a mod line (an older loader wrote plain text)
                list.Add(new RoomMod { Folder = f[0], Version = f.Length > 1 ? f[1] : "?", Fp = f.Length > 2 ? f[2] : "-", Name = f.Length > 3 && f[3] != "" ? f[3] : f[0] });
            }
            hostHasMods = true;
            return list;
        }

        // Everyone in a room needs it: data mods always; code mods unless mod.ini says online=any.
        public static bool MattersOnline(ModInfo m)
        {
            return m.HasData || (m.Dll != "" && !AnyRoom(m));
        }

        public static bool AnyRoom(ModInfo m)  // a code mod that only changes your own game
        {
            return m.Dll != "" && !m.HasData && Ini.Get(m.Ini, "mod", "online").Trim().Equals("any", StringComparison.OrdinalIgnoreCase);
        }

        // mods\loader.ini [online] modded_rooms=1: look for modded rooms even with only any-room mods.
        static string LoaderIni { get { return Path.Combine(Paths.Mods, "loader.ini"); } }
        public static bool ModdedRooms
        {
            get { return Ini.Get(LoaderIni, "online", "modded_rooms", "0").Trim() == "1"; }
            set { Directory.CreateDirectory(Paths.Mods); Ini.Set(LoaderIni, "online", "modded_rooms", value ? "1" : "0"); }
        }

        // The loader's fingerprint of a data mod: FNV-1a of its package.json, 8 hex digits.
        public static string Fingerprint(ModInfo m)
        {
            string pkg = Path.Combine(m.Dir, "data", "package.json");
            if (!File.Exists(pkg)) return "-";
            uint h = 2166136261;
            foreach (byte b in File.ReadAllBytes(pkg)) h = unchecked((h ^ b) * 16777619);
            return h.ToString("x8");
        }

        public class Result
        {
            public List<string> Install = new List<string>(), Disable = new List<string>(), Versions = new List<string>(), SwitchOn = new List<string>();
            public List<ModInfo> On = new List<ModInfo>(), Off = new List<ModInfo>();  // what a room profile changes
            public List<ModInfo> RoomOrder = new List<ModInfo>();  // installed room mods, in the room's order
            public List<RoomMod> Needed = new List<RoomMod>();     // room mods to install or change version
            public bool Matches { get { return Install.Count + Disable.Count + Versions.Count + SwitchOn.Count == 0; } }
            public bool CanFix { get { return On.Count + Off.Count > 0; } }
        }

        public static Result Compare(List<ModInfo> mods, List<RoomMod> room)
        {
            var r = new Result();
            var fits = new HashSet<ModInfo>();
            foreach (var rm in room)
            {
                var m = mods.FirstOrDefault(x => string.Equals(x.Folder, rm.Folder, StringComparison.OrdinalIgnoreCase));
                if (m == null) { r.Install.Add(rm.Name + " " + rm.Version); r.Needed.Add(rm); continue; }
                if (m.Version != rm.Version) { r.Versions.Add(rm.Name + " (room " + rm.Version + ", yours " + m.Version + ")"); r.Needed.Add(rm); continue; }
                if (rm.Fp != "-" && rm.Fp != Fingerprint(m)) { r.Versions.Add(rm.Name + " " + rm.Version + " (a different build of it)"); r.Needed.Add(rm); continue; }
                fits.Add(m);
                r.RoomOrder.Add(m);
                if (!m.Enabled) { r.SwitchOn.Add(m.Name + " " + m.Version); r.On.Add(m); }
            }
            foreach (var m in mods)
                if (m.Enabled && MattersOnline(m) && !fits.Contains(m) && !room.Any(x => string.Equals(x.Folder, m.Folder, StringComparison.OrdinalIgnoreCase)))
                {
                    r.Disable.Add(m.Name + " " + m.Version);
                    r.Off.Add(m);
                }
            return r;
        }

        // Switch to the room's setup and save it as a profile: its mods on, in its order (load order
        // decides overlaps), other mods that matter online off; any-room mods as they are. If a
        // profile already has exactly that setup, it's used instead. -> the profile's name.
        public static string MakeProfile(string name, List<ModInfo> mods, Result r)
        {
            foreach (var m in r.On) ModStore.SetEnabled(m, true);
            foreach (var m in r.Off) ModStore.SetEnabled(m, false);
            var ordered = r.RoomOrder.Concat(mods.Where(m => !r.RoomOrder.Contains(m))).ToList();
            ModStore.SaveOrder(ordered);
            var now = ModStore.List();
            string same = Profiles.Names().FirstOrDefault(n => Profiles.Matches(n, now));
            if (same != null)
            {
                Profiles.Last = same;
                Log.Write("set up the mods for an online room: the profile " + same + " already matches");
                return same;
            }
            Profiles.Save(name, now);
            Log.Write("made the profile " + name + " for an online room");
            return name;
        }
    }

    // Saved mod setups ("Solo", "Friday group", ...): which mods are on, and their order.
    // ModManager\profiles\<name>.txt: one "folder|1" (on) or "folder|0" (off) line per mod, in order.
    static class Profiles
    {
        static string Dir { get { return Path.Combine(Paths.Data, "profiles"); } }

        static string FileOf(string name)
        {
            string safe = new string(name.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c).ToArray()).Trim();
            return Path.Combine(Dir, (safe == "" ? "Profile" : safe) + ".txt");
        }

        // The profile last switched to or saved (settings.ini [profiles] last): when several have the
        // same setup, it's the one shown as current.
        public static string Last
        {
            get { return Ini.Get(Paths.SettingsFile, "profiles", "last"); }
            set { Ini.Set(Paths.SettingsFile, "profiles", "last", value ?? ""); }
        }

        // The profile to show as current: the last one picked if it still matches, else the first that does.
        public static string Current(List<ModInfo> mods)
        {
            var matching = Names().Where(n => Matches(n, mods)).ToList();
            string last = Last;
            return matching.FirstOrDefault(n => n == last) ?? matching.FirstOrDefault();
        }

        public static List<string> Names()
        {
            if (!Directory.Exists(Dir)) return new List<string>();
            return Directory.GetFiles(Dir, "*.txt").Select(f =>
            {
                string first = File.ReadLines(f).FirstOrDefault() ?? "";
                return first.StartsWith("# profile: ") ? first.Substring(11) : Path.GetFileNameWithoutExtension(f);
            }).OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
        }

        static List<KeyValuePair<string, bool>> Read(string name)
        {
            var list = new List<KeyValuePair<string, bool>>();
            foreach (string line in File.ReadAllLines(FileOf(name)))
            {
                if (line.StartsWith("#") || !line.Contains("|")) continue;
                int bar = line.LastIndexOf('|');
                list.Add(new KeyValuePair<string, bool>(line.Substring(0, bar), line.Substring(bar + 1).Trim() == "1"));
            }
            return list;
        }

        public static void Save(string name, List<ModInfo> mods)
        {
            Directory.CreateDirectory(Dir);
            File.WriteAllLines(FileOf(name), new[] { "# profile: " + name }.Concat(mods.Select(m => m.Folder + "|" + (m.Enabled ? "1" : "0"))), new UTF8Encoding(false));
            Last = name;
            Log.Write("saved the profile " + name);
        }

        public static void Delete(string name)
        {
            if (File.Exists(FileOf(name))) File.Delete(FileOf(name));
            Log.Write("deleted the profile " + name);
        }

        // Is this the setup right now? (same mods on, in the same order)
        public static bool Matches(string name, List<ModInfo> mods)
        {
            try
            {
                var want = Read(name).Where(p => p.Value).Select(p => p.Key.ToLowerInvariant()).ToList();
                var have = mods.Where(m => m.Enabled).Select(m => m.Folder.ToLowerInvariant()).ToList();
                return want.SequenceEqual(have);
            }
            catch { return false; }
        }

        // Switch to a profile: its mods on (in its order), every other mod off. Returns the
        // mods it lists that aren't installed.
        public static List<string> Apply(string name, List<ModInfo> mods)
        {
            var entries = Read(name);
            var missing = new List<string>();
            var ordered = new List<ModInfo>();
            foreach (var e in entries)
            {
                var m = mods.FirstOrDefault(x => string.Equals(x.Folder, e.Key, StringComparison.OrdinalIgnoreCase));
                if (m == null) { if (e.Value) missing.Add(e.Key); continue; }
                ordered.Add(m);
                if (m.Enabled != e.Value) ModStore.SetEnabled(m, e.Value);
            }
            foreach (var m in mods.Where(x => !ordered.Contains(x)))  // installed since: off, at the end
            {
                if (m.Enabled) ModStore.SetEnabled(m, false);
                ordered.Add(m);
            }
            ModStore.SaveOrder(ordered);
            Last = name;
            Log.Write("switched to the profile " + name + (missing.Count > 0 ? "; missing: " + string.Join(", ", missing) : ""));
            return missing;
        }
    }

    // Combining data mods into the game's DataMod folder (built in: see Combine.cs).
    static class Combiner
    {
        // What the loader compares (see src/loader.cpp CheckCombined): is DataMod up to date?
        public static bool UpToDate(List<ModInfo> mods)
        {
            var want = new StringBuilder();
            foreach (var m in mods)
            {
                string pkg = Path.Combine(m.Dir, "data", "package.json");
                if (!m.Enabled || !File.Exists(pkg)) continue;
                want.Append(Data.CombineJob.StampLine(m.Folder, pkg) + "\n");
            }
            string stamp = Path.Combine(Paths.Game, "DataMod", Data.CombineJob.STAMP);
            var have = new StringBuilder();
            if (File.Exists(stamp))
                foreach (string l in File.ReadAllLines(stamp))
                    if (l.Length > 0 && l[0] != '#') have.Append(l + "\n");
            return want.ToString() == have.ToString();
        }

        // Combines the switched-on data mods; lines of progress go to onLine. Returns (ok, overlap
        // warnings). outDir is for testing (default: the game's DataMod folder).
        public static bool Run(Action<string> onLine, out List<string> overlaps, string outDir = null)
        {
            overlaps = new List<string>();
            var job = new Data.CombineJob(Paths.Game, outDir ?? Path.Combine(Paths.Game, "DataMod"), Path.Combine(Paths.Data, "combine cache"), onLine);
            var watch = Stopwatch.StartNew();
            bool ok;
            try { ok = job.Run(); }
            catch (Exception e)
            {
                onLine("The mods couldn't be combined; DataMod was left as it was:");
                onLine("  - " + (e is Data.DataError || e is IOException || e is UnauthorizedAccessException ? e.Message : e.ToString()));
                Log.Write("combining failed: " + e);
                return false;
            }
            finally { Data.DataHash.Forget(); }
            overlaps = job.Warnings;
            Log.Write("combined mods: " + (ok ? "ok" : "failed") + " in " + watch.ElapsedMilliseconds + " ms" +
                      (overlaps.Count > 0 ? ", " + overlaps.Count + " overlap(s)" : ""));
            return ok;
        }
    }

    // Zips of the save folder's save files, before each launch; the last N are kept.
    static class SaveBackups
    {
        public static bool AutoBackup
        {
            get { return Ini.Get(Paths.SettingsFile, "saves", "backup_on_launch", "1") != "0"; }
            set { Ini.Set(Paths.SettingsFile, "saves", "backup_on_launch", value ? "1" : "0"); }
        }
        public static int Keep
        {
            get { int n; return int.TryParse(Ini.Get(Paths.SettingsFile, "saves", "keep", "5"), out n) && n > 0 ? n : 5; }
            set { Ini.Set(Paths.SettingsFile, "saves", "keep", Math.Max(1, Math.Min(99, value)).ToString()); }
        }

        public static string[] SaveFiles()
        {
            if (Paths.Saves == "" || !Directory.Exists(Paths.Saves)) return new string[0];
            return new[] { "SystemData.bin", "SystemData.bin.classes" }
                .Select(f => Path.Combine(Paths.Saves, f)).Where(File.Exists).ToArray();
        }

        public static List<FileInfo> List()
        {
            if (!Directory.Exists(Paths.Backups)) return new List<FileInfo>();
            return new DirectoryInfo(Paths.Backups).GetFiles("*.zip").OrderByDescending(f => f.LastWriteTime).ToList();
        }

        public static string Make(string why)
        {
            var files = SaveFiles();
            if (files.Length == 0) throw new Exception("No save found to back up.");
            Directory.CreateDirectory(Paths.Backups);
            string name = DateTime.Now.ToString("yyyy-MM-dd HHmmss") + " " + why + ".zip";
            string path = Path.Combine(Paths.Backups, name);
            using (var z = ZipFile.Open(path, ZipArchiveMode.Create))
                foreach (string f in files) z.CreateEntryFromFile(f, Path.GetFileName(f));
            foreach (var old in List().Skip(Keep)) try { old.Delete(); } catch { }
            Log.Write("save backed up: " + name);
            return path;
        }

        // Restore a backup: the save as it is now is backed up first, so this can be undone.
        public static void Restore(FileInfo zip)
        {
            if (Launcher.GameRunning()) throw new Exception("Close the game first.");
            Make("before restore");
            using (var z = ZipFile.OpenRead(zip.FullName))
                foreach (var e in z.Entries)
                    if (e.Name == "SystemData.bin" || e.Name == "SystemData.bin.classes")
                        e.ExtractToFile(Path.Combine(Paths.Saves, e.Name), true);
            Log.Write("save restored from " + zip.Name);
        }
    }

    static class Launcher
    {
        public static bool GameRunning()
        {
            try
            {
                return Process.GetProcessesByName("game").Any(p =>
                {
                    try { return string.Equals(Path.GetDirectoryName(p.MainModule.FileName), Paths.Game, StringComparison.OrdinalIgnoreCase); }
                    catch { return true; }
                });
            }
            catch { return false; }
        }

        // Starts the game through Steam. withoutMods: the loader skips every mod for this one start.
        public static void Start(bool withoutMods)
        {
            if (withoutMods)
            {
                Directory.CreateDirectory(Paths.Mods);
                File.WriteAllText(Path.Combine(Paths.Mods, "launch_vanilla_once"),
                    "The mod loader starts the game once without mods, then deletes this file.\r\n");
            }
            Process.Start(new ProcessStartInfo("steam://rungameid/" + Paths.AppId) { UseShellExecute = true });
            Log.Write(withoutMods ? "launched the game without mods" : "launched the game");
        }
    }

    // The manager's own log (ModManager\manager.log) and the game-side logs it shows.
    static class Log
    {
        public static void Write(string s)
        {
            try
            {
                Directory.CreateDirectory(Paths.Data);
                File.AppendAllText(Path.Combine(Paths.Data, "manager.log"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss ") + s + "\r\n");
            }
            catch { }
        }

        // (title, path) of every log there is: the loader's, then each mod's.
        public static List<KeyValuePair<string, string>> GameLogs()
        {
            var list = new List<KeyValuePair<string, string>>();
            string loader = Path.Combine(Paths.Game, "dmfd_content.log");
            if (File.Exists(loader)) list.Add(new KeyValuePair<string, string>("Loader", loader));
            foreach (var m in ModStore.List())
            {
                string p = Path.Combine(m.Dir, "mod.log");
                if (File.Exists(p)) list.Add(new KeyValuePair<string, string>(m.Name, p));
            }
            string mgr = Path.Combine(Paths.Data, "manager.log");
            if (File.Exists(mgr)) list.Add(new KeyValuePair<string, string>("Manager", mgr));
            return list;
        }

        // Read while the game may be writing it.
        public static string[] ReadShared(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var r = new StreamReader(fs, Encoding.UTF8))
                    return r.ReadToEnd().Split(new[] { "\r\n", "\n" }, StringSplitOptions.None);
            }
            catch { return new string[0]; }
        }

        // A zip of every log plus the mod list, for sending to whoever is helping.
        public static string Report()
        {
            string dir = Path.Combine(Paths.Data, "reports");
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "dmfd_report " + DateTime.Now.ToString("yyyy-MM-dd HHmmss") + ".zip");
            using (var z = ZipFile.Open(path, ZipArchiveMode.Create))
            {
                foreach (var kv in GameLogs())
                {
                    var e = z.CreateEntry(kv.Key.Replace(' ', '_') + ".log");
                    using (var w = new StreamWriter(e.Open(), new UTF8Encoding(false)))
                        w.Write(string.Join("\r\n", ReadShared(kv.Value)));
                }
                var list = z.CreateEntry("mods.txt");
                using (var w = new StreamWriter(list.Open(), new UTF8Encoding(false)))
                    foreach (var m in ModStore.List())
                        w.WriteLine("{0} {1} ({2}, {3}){4}", m.Name, m.Version, m.Folder, m.Type, m.Enabled ? "" : " off");
            }
            Log.Write("made a problem report: " + path);
            return path;
        }
    }
}
