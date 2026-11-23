// The mod loader: starts code mods from <game>\mods.
//
// A mod is a folder in mods\ with a mod.ini:
//   [mod]
//   name=Rebalance          shown in the log and the manager
//   version=0.15
//   author=...
//   dll=Rebalance.dll       code to start (optional; a mod can be data only)
//   enabled=1               0 = installed but switched off
//   online=any              this code mod only changes your own game: it works in any online
//                           room. Without it (or online=same) everyone in a room needs it, same
//                           version; data mods always count. See net.cpp.
// Order: the folders listed in mods\order.txt (one per line) first, in that order, then
// the rest alphabetically. The manager writes order.txt and the enabled flags.
//
// Each code mod is an ordinary DLL whose DllMain starts it (its own thread, waiting
// for the game code like this DLL does). The loader runs from its own thread, not from
// DllMain, so mods load outside the Windows loader lock.
#include "content.h"
#include <algorithm>
#include <string>
#include <vector>
#include "../codemods/common/dmfd_mod_api.h"

struct ModInfo {
    std::string folder, name, version, dll;
    bool enabled;
};

static std::vector<LoadedMod> g_mods;
const std::vector<LoadedMod>& LoadedMods() { return g_mods; }

std::string GameFolder()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string game = exe;
    return game.substr(0, game.find_last_of('\\'));
}

static std::string IniStr(const std::string& ini, const char* key, const char* def = "")
{
    char buf[512];
    GetPrivateProfileStringA("mod", key, def, buf, sizeof buf, ini.c_str());
    return buf;
}

static std::vector<std::string> ModFolders(const std::string& mods)
{
    std::vector<std::string> found;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((mods + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (GetFileAttributesA((mods + "\\" + fd.cFileName + "\\mod.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
            found.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });

    // mods\order.txt first
    std::vector<std::string> ordered;
    FILE* f = nullptr;
    if (!fopen_s(&f, (mods + "\\order.txt").c_str(), "r") && f) {
        char line[MAX_PATH];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s[0] == ';' || s[0] == '#') continue;
            auto it = std::find_if(found.begin(), found.end(), [&](const std::string& x) { return !_stricmp(x.c_str(), s.c_str()); });
            if (it != found.end()) {
                ordered.push_back(*it);
                found.erase(it);
            }
        }
        fclose(f);
    }
    ordered.insert(ordered.end(), found.begin(), found.end());
    return ordered;
}

// Data mods are combined into <game>\DataMod by the Mod Manager (tools/combine_mods.py), which
// leaves DataMod\combined.txt: one line per combined mod, "<folder> <package size> <package
// time>". If the switched-on data mods no longer match, the game is still using the old
// combination: say so (log, start-up message, the menu's Mod loader page).
static std::string g_dataWarning;
const std::string& DataModsWarning() { return g_dataWarning; }

static void CheckCombined(const std::string& game, const std::vector<std::string>& folders)
{
    std::string mods = game + "\\mods", want;
    for (const std::string& folder : folders) {
        std::string ini = mods + "\\" + folder + "\\mod.ini", pkg = mods + "\\" + folder + "\\data\\package.json";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetPrivateProfileIntA("mod", "enabled", 1, ini.c_str()) == 0 ||
            !GetFileAttributesExA(pkg.c_str(), GetFileExInfoStandard, &a))
            continue;
        ULONGLONG t = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime);
        char line[MAX_PATH + 64];
        sprintf_s(line, "%s %lu %llu\n", folder.c_str(), a.nFileSizeLow, (t - 116444736000000000ULL) / 10000000ULL);
        want += line;
    }
    std::string have;
    FILE* f = nullptr;
    if (!fopen_s(&f, (game + "\\DataMod\\combined.txt").c_str(), "r") && f) {
        char line[MAX_PATH + 64];
        while (fgets(line, sizeof line, f))
            if (line[0] != '#' && line[0] != '\n') have += line;
        fclose(f);
    }
    if (want.empty() && have.empty()) return;  // no data mods
    if (want != have) {
        g_dataWarning = "The data mods changed since they were last combined, so the game is using the old "
                        "combination. Open the Mod Manager (or run Combine mods), then restart the game.";
        Log("loader: WARNING %s", g_dataWarning.c_str());
    } else {
        Log("loader: data mods match what was combined");
    }
}

void LoadCodeMods()
{
    std::string game = GameFolder();
    std::string mods = game + "\\mods";

    if (GetFileAttributesA((game + "\\dinput8.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        Log("loader: WARNING dinput8.dll is in the game folder; if it's the old stand-alone Rebalance, "
            "it runs as well as mods\\Rebalance. Remove it.");

    std::vector<std::string> folders = ModFolders(mods);
    CheckCombined(game, folders);
    if (folders.empty()) {
        Log("loader: no mods in %s", mods.c_str());
        return;
    }
    int started = 0;
    for (const std::string& folder : folders) {
        std::string dir = mods + "\\" + folder, ini = dir + "\\mod.ini";
        ModInfo m{folder, IniStr(ini, "name", folder.c_str()), IniStr(ini, "version", "?"), IniStr(ini, "dll"),
                  GetPrivateProfileIntA("mod", "enabled", 1, ini.c_str()) != 0};
        LoadedMod rec{folder, m.name, m.version, IniStr(ini, "author"), IniStr(ini, "description"), "", false, false};
        if (!m.enabled) {
            Log("loader: %s %s is switched off", m.name.c_str(), m.version.c_str());
            rec.state = "switched off";
        } else if (m.dll.empty()) {
            Log("loader: %s %s (data only)", m.name.c_str(), m.version.c_str());
            rec.state = "data only";
        } else if (GetFileAttributesA((dir + "\\" + m.dll).c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log("loader: ERROR %s: %s\\%s is missing", m.name.c_str(), dir.c_str(), m.dll.c_str());
            rec.state = m.dll + " is missing";
            rec.error = true;
        } else {
            // the mod's own folder is searched first for anything its DLL needs
            MenuStartingMod(folder);
            HMODULE h = LoadLibraryExA((dir + "\\" + m.dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (h) {
                Log("loader: started %s %s (%s)", m.name.c_str(), m.version.c_str(), m.dll.c_str());
                rec.state = "running";
                rec.started = true;
                started++;
                if (auto start = (DmfdModStartFn)GetProcAddress(h, "DmfdModStart")) start(LoaderApi());
            } else {
                DWORD err = GetLastError();
                Log("loader: ERROR %s: could not load %s (Windows error %lu)", m.name.c_str(), m.dll.c_str(), err);
                rec.state = "could not start (error " + std::to_string(err) + ")";
                rec.error = true;
            }
            MenuStartingMod("");
        }
        g_mods.push_back(rec);
    }
    Log("loader: %d code mod(s) started", started);
}
