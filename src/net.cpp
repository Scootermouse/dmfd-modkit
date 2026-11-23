// Online, for the content mod.
//
// - Rooms: the game tags its rooms (Steam lobbies) with a numeric "GameMode" and
//   searches for rooms with an equal value. A player with mods that matter online (any
//   data mod, or a code mod whose mod.ini says online=same) shifts that number by one
//   fixed amount, when hosting and when searching, and back when the game reads it. So
//   modded players see every modded room and no vanilla ones, and vanilla players
//   never see modded rooms. Players with only local mods (Quick Spells...) stay vanilla.
// - Mods: the host puts its list of those mods in the room's data ("dmfd_mods"). A
//   player about to join compares it with its own first (Steam JoinLobby): if they differ
//   the join is stopped and a notice says what to install, disable or update; the list
//   is saved to mods\last_room.txt for the Mod Manager ("set up this room's mods").
//   If the room's data can't be had before joining (an invite, say), the same check
//   runs right after entering and the notice says to leave.
// - Party members: each player's record (4 x 0x3E0 bytes, class at +8, name at
//   +0x5C) arrives through 0xC93060(index, source). We log them, and note which
//   of the six base classes other players are using, so the content mod's
//   slot redirect (a slot's class drawn as the extra class in it) leaves those
//   players' look alone.
#include "content.h"
#include "MinHook.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <set>

typedef unsigned long long u64;

// ISteamMatchmaking009 vtable slots
enum { MM_ADD_NUM_FILTER = 6, MM_JOIN = 14, MM_LEAVE = 15, MM_NUM_MEMBERS = 17, MM_MEMBER_BY_INDEX = 18, MM_GET_DATA = 19,
       MM_SET_DATA = 20, MM_GET_MEMBER_DATA = 24, MM_SET_MEMBER_DATA = 25, MM_REQUEST_DATA = 28, MM_OWNER = 35 };

using AddNumFilterFn = void(__fastcall*)(void*, void*, const char*, int, int);
using SetDataFn = bool(__fastcall*)(void*, void*, u64, const char*, const char*);
using GetDataFn = const char*(__fastcall*)(void*, void*, u64, const char*);
using JoinFn = u64(__fastcall*)(void*, void*, u64);
using LeaveFn = void(__fastcall*)(void*, void*, u64);

static AddNumFilterFn o_addNumFilter;
static SetDataFn o_setData;
static GetDataFn o_getData;
static JoinFn o_join;
static LeaveFn o_leave;
static const int MODDED_SHIFT = 5300000;  // far from the game's small values and older kits' shifts
static std::string OurMods();
static int RoomShift()  // 0 = vanilla rooms
{
    static int shift = -1;
    if (shift < 0) {
        // mods\loader.ini [online] modded_rooms=1 (the Mod Manager's Settings): a player whose mods
        // all work in any room (Quick Spells...) can still look for modded rooms
        bool optIn = GetPrivateProfileIntA("online", "modded_rooms", 0, (GameFolder() + "\\mods\\loader.ini").c_str()) != 0;
        bool need = !OurMods().empty();
        shift = need || optIn ? MODDED_SHIFT : 0;
        Log(need ? "net: modded rooms (mods that must match online are installed)"
                 : optIn ? "net: modded rooms (looking for them is switched on)" : "net: no mods that matter online; vanilla rooms");
    }
    return shift;
}

static bool IsGameMode(const char* key) { return key && !strcmp(key, "GameMode"); }

// ---- lobby hooks ----
static void __fastcall h_addNumFilter(void* mm, void* edx, const char* key, int value, int cmp)
{
    if (IsGameMode(key)) value += RoomShift();
    o_addNumFilter(mm, edx, key, value, cmp);
}

static volatile u64 g_room;  // the Steam lobby we're in (0 = none), for NetInRoom
bool NetInRoom() { return g_room != 0; }

static volatile bool g_hosting;
static volatile LONG g_checkRoom;  // 1 = a joined room's mods still to be compared

static bool __fastcall h_setData(void* mm, void* edx, u64 lobby, const char* key, const char* value)
{
    char shifted[32];
    if (IsGameMode(key) && value) {
        g_room = lobby;  // only the host sets the room's GameMode
        g_hosting = true;
        InterlockedExchange(&g_checkRoom, 0);
        static u64 listed;
        if (listed != lobby) {
            listed = lobby;
            std::string mods = OurMods();
            o_setData(mm, edx, lobby, "dmfd_mods", mods.c_str());
            Log("net: the room lists %d mod(s) that must match", (int)std::count(mods.begin(), mods.end(), '\n'));
        }
        sprintf_s(shifted, "%d", atoi(value) + RoomShift());
        value = shifted;
    }
    return o_setData(mm, edx, lobby, key, value);
}

static const char* __fastcall h_getData(void* mm, void* edx, u64 lobby, const char* key)
{
    const char* v = o_getData(mm, edx, lobby, key);
    if (IsGameMode(key) && v && *v) {
        static char buf[8][32];  // the game may hold a couple of these at once
        static int next;
        char* b = buf[next++ & 7];
        sprintf_s(b, 32, "%d", atoi(v) - RoomShift());
        return b;
    }
    return v;
}

static void ForgetMembers();
static bool JoinAllowed(void* mm, u64 lobby);

// ---- a stopped join, answered as failed ----
// The game waits for JoinLobby's answer (a LobbyEnter_t call result, callback 504) on "Preparing
// to join...". Returning no call (0) left it waiting forever. So a stopped join returns FAKE_CALL:
// the game registers its call result for it (SteamAPI_RegisterCallResult, caught here), and on its
// next SteamAPI_RunCallbacks it gets LobbyEnter_t with "error" (5): its own failure path.
static const u64 FAKE_CALL = 0xD3F0D3F0D3F00001ULL;
static void* volatile g_fakeCb;
static volatile u64 g_fakeLobby;
using RegisterFn = void(__cdecl*)(void*, u64);
using RunCallbacksFn = void(__cdecl*)();
static RegisterFn o_register, o_unregister;
static RunCallbacksFn o_runCallbacks;

static void __cdecl h_register(void* cb, u64 call)
{
    if (call == FAKE_CALL) {
        g_fakeCb = cb;
        return;
    }
    o_register(cb, call);
}

static void __cdecl h_unregister(void* cb, u64 call)
{
    if (call == FAKE_CALL) {
        if (g_fakeCb == cb) g_fakeCb = nullptr;
        return;
    }
    o_unregister(cb, call);
}

static uint8_t* Follow(uint8_t* f)  // through jump thunks
{
    for (int i = 0; i < 4 && f[0] == 0xE9; i++) f = f + 5 + *(int32_t*)(f + 1);
    return f;
}

// Which of the call result's two Run methods the vtable slot is: 4 = Run(param),
// 16 = Run(param, ioFailure, call) (its "ret N"), 0 = can't tell.
static int RetBytes(void* fn)
{
    uint8_t* f = Follow((uint8_t*)fn);
    for (int i = 0; i < 96; i++)
        if (f[i] == 0xC2 && f[i + 2] == 0 && (f[i + 1] == 4 || f[i + 1] == 0x10)) return f[i + 1];
    return 0;
}

static void FireFakeJoin()
{
    void* cb = g_fakeCb;
    g_fakeCb = nullptr;
    __try {
        if (*(int*)((uint8_t*)cb + 8) != 504) {  // CCallbackBase: vtable, flags, callback number
            Log("net: the game's join answer isn't the expected kind (callback %d); left alone", *(int*)((uint8_t*)cb + 8));
            return;
        }
        struct { u64 lobby; uint32_t perms; uint8_t locked, pad[3]; uint32_t response, pad2; } enter = {g_fakeLobby, 0, 0, {0, 0, 0}, 5, 0};
        void** vt = *(void***)cb;
        for (int s = 0; s < 2; s++) {
            int ret = RetBytes(vt[s]);
            if (ret == 0x10) {
                ((void(__thiscall*)(void*, void*, bool, u64))vt[s])(cb, &enter, false, FAKE_CALL);
                Log("net: told the game the join failed");
                return;
            }
            if (ret == 4) {
                ((void(__thiscall*)(void*, void*))vt[s])(cb, &enter);
                Log("net: told the game the join failed");
                return;
            }
        }
        Log("net: couldn't answer the stopped join (the game may keep waiting)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("net: answering the stopped join failed");
    }
}

static void __cdecl h_runCallbacks()
{
    o_runCallbacks();
    if (g_fakeCb) FireFakeJoin();  // on the game's thread, like any Steam answer
}

static bool HookExport(HMODULE m, const char* name, void* fn, void** orig)
{
    void* target = (void*)GetProcAddress(m, name);
    if (!target) return false;
    MH_STATUS st = MH_CreateHook(target, fn, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) Log("net: hook %s failed: %s", name, MH_StatusToString(st));
    return st == MH_OK;
}

static u64 __fastcall h_join(void* mm, void* edx, u64 lobby)
{
    if (!JoinAllowed(mm, lobby)) {
        if (o_runCallbacks) {  // answered as failed (see above)
            g_fakeLobby = lobby;
            return FAKE_CALL;
        }
        return 0;
    }
    ForgetMembers();
    g_room = lobby;
    g_hosting = false;
    Log("net: joining a room");
    return o_join(mm, edx, lobby);
}

static void __fastcall h_leave(void* mm, void* edx, u64 lobby)
{
    ForgetMembers();
    if (lobby == g_room) {
        g_room = 0;
        g_hosting = false;
        InterlockedExchange(&g_checkRoom, 0);
    }
    Log("net: left the room");
    o_leave(mm, edx, lobby);
}

static bool HookSlot(void* iface, int slot, void* fn, void** orig, const char* name)
{
    void* target = (*(void***)iface)[slot];
    MH_STATUS st = MH_CreateHook(target, fn, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) Log("net: hook %s failed: %s", name, MH_StatusToString(st));
    return st == MH_OK;
}

// Steam's interfaces exist once the game has called SteamAPI_Init.
static void* FindMatchmaking()
{
    HMODULE sa = GetModuleHandleA("steam_api.dll");
    if (!sa) return nullptr;
    using GetUserFn = int(__cdecl*)();
    using FindFn = void*(__cdecl*)(int, const char*);
    auto getUser = (GetUserFn)GetProcAddress(sa, "SteamAPI_GetHSteamUser");
    auto find = (FindFn)GetProcAddress(sa, "SteamInternal_FindOrCreateUserInterface");
    if (!getUser || !find) return nullptr;
    int user = getUser();
    return user ? find(user, "SteamMatchMaking009") : nullptr;
}

// ---- party member records ----
// 0xC93060: thiscall(game data, member index, source record), ret 8.
static const uint32_t MEMBER_SET = 0xC93060;
using MemberSetFn = void(__fastcall*)(void*, void*, int, const uint8_t*);
static MemberSetFn o_memberSet;
static volatile LONG g_memberClass[4] = {-1, -1, -1, -1};  // class of each received record, -1 = none

static void UpdateRemote()
{
    uint8_t has[6] = {};
    for (int i = 0; i < 4; i++) {
        LONG c = g_memberClass[i];
        if (c >= 0 && c < 6) has[c] = 1;  // your own record is never a base class whose slot holds an extra class
    }
    ClassesRemoteBase(has);
}

static void ForgetMembers()
{
    for (int i = 0; i < 4; i++) InterlockedExchange(&g_memberClass[i], -1);
    UpdateRemote();
}

static void __fastcall h_memberSet(void* data, void* edx, int index, const uint8_t* src)
{
    o_memberSet(data, edx, index, src);
    __try {
        if (index >= 0 && index < 4 && data) {
            const uint8_t* rec = (const uint8_t*)data + 0x4478 + 0x3E0 * index;
            int cls = rec[8];
            char name[33];
            memcpy(name, rec + 0x5C, 32);
            name[32] = 0;
            if (InterlockedExchange(&g_memberClass[index], cls) != cls)
                Log("net: party member %d is class %d%s (%s)", index, cls,
                    cls >= 6 ? (ClassDefined(cls) ? ", an extra class" : ", an extra class NOT in this content")
                             : "",
                    name);
            UpdateRemote();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("net: could not read a party member record");
    }
}

// ---- the mods that must match online ----
// One line per mod: "<folder>\t<version>\t<fingerprint>\t<name>". Data mods: those combined
// into DataMod (combined.txt), fingerprint = hash of their package.json. Code mods: running
// ones, unless their mod.ini says online=any (they only change your own game, like Quick
// Spells), fingerprint "-" (the version decides).
struct RoomMod { std::string folder, version, fp, name; };

static std::string Fnv(const std::string& path)
{
    FILE* f;
    if (fopen_s(&f, path.c_str(), "rb")) return "?";
    uint32_t h = 2166136261u;
    int c;
    while ((c = fgetc(f)) != EOF) h = (h ^ (uint8_t)c) * 16777619u;
    fclose(f);
    char b[16];
    sprintf_s(b, "%08x", h);
    return b;
}

static std::string IniStr(const std::string& ini, const char* key, const char* def = "")
{
    char buf[256];
    GetPrivateProfileStringA("mod", key, def, buf, sizeof buf, ini.c_str());
    return buf;
}

static std::string Clean(std::string s)  // no tabs or line breaks inside a field
{
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

static std::string OurMods()
{
    static std::string cached;
    static bool done;
    if (done) return cached;
    std::string game = GameFolder(), mods = game + "\\mods", out;
    FILE* f = nullptr;
    if (!fopen_s(&f, (game + "\\DataMod\\combined.txt").c_str(), "r") && f) {
        char line[MAX_PATH + 64];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            std::string folder = line;
            folder = folder.substr(0, folder.find(' '));
            std::string dir = mods + "\\" + folder, ini = dir + "\\mod.ini";
            out += Clean(folder) + "\t" + Clean(IniStr(ini, "version", "?")) + "\t" + Fnv(dir + "\\data\\package.json") +
                   "\t" + Clean(IniStr(ini, "name", folder.c_str())) + "\n";
        }
        fclose(f);
    }
    for (const LoadedMod& m : LoadedMods()) {
        if (!m.started) continue;
        std::string ini = mods + "\\" + m.folder + "\\mod.ini";
        if (!_stricmp(IniStr(ini, "online").c_str(), "any")) continue;  // works in any room
        out += Clean(m.folder) + "\t" + Clean(m.version) + "\t-\t" + Clean(m.name) + "\n";
    }
    cached = out;
    done = true;
    return out;
}

static std::vector<RoomMod> ParseMods(const char* text)
{
    std::vector<RoomMod> v;
    std::string s = text ? text : "";
    size_t at = 0;
    while (at < s.size()) {
        size_t nl = s.find('\n', at);
        std::string line = s.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? s.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string f[4];
        size_t p = 0;
        for (int i = 0; i < 4 && p <= line.size(); i++) {
            size_t t = i < 3 ? line.find('\t', p) : std::string::npos;
            f[i] = line.substr(p, t == std::string::npos ? std::string::npos : t - p);
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (!f[0].empty()) v.push_back({f[0], f[1], f[2], f[3].empty() ? f[0] : f[3]});
    }
    return v;
}

static std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
    return s;
}

// What differs between the room's list and ours, in words; "" when they match.
static std::string CompareMods(const char* roomText, bool roomHasList, bool joined)
{
    std::vector<RoomMod> room = ParseMods(roomText), ours = ParseMods(OurMods().c_str());
    std::vector<std::string> install, disable, versions;
    auto find = [](std::vector<RoomMod>& v, const std::string& folder) -> RoomMod* {
        for (auto& m : v)
            if (!_stricmp(m.folder.c_str(), folder.c_str())) return &m;
        return nullptr;
    };
    if (roomHasList)
        for (auto& r : room) {
            RoomMod* o = find(ours, r.folder);
            if (!o) install.push_back(r.name + " " + r.version);
            else if (o->version != r.version) versions.push_back(r.name + " (room " + r.version + ", yours " + o->version + ")");
            else if (o->fp != r.fp) versions.push_back(r.name + " " + r.version + " (a different build of it)");
        }
    for (auto& o : ours)
        if (!find(room, o.folder)) disable.push_back(o.name + " " + o.version);
    if (install.empty() && disable.empty() && versions.empty()) return "";
    std::string msg = joined ? "This room's mods don't match yours: leave the room, you can't play here together.\n"
                             : "You can't join this room: its mods don't match yours.\n";
    if (!roomHasList) msg += "The host plays without mods (or with an older mod kit).\n";
    if (!install.empty()) msg += "Need to install: " + Join(install) + "\n";
    if (!disable.empty()) msg += "Need to disable: " + Join(disable) + "\n";
    if (!versions.empty()) msg += "Different versions: " + Join(versions) + "\n";
    msg += joined ? "To play here: leave the room, close the game, set up these mods in the Mod Manager (it can make a profile for this room), then start the game again and rejoin.\n"
                  : "To play here: close the game, set up these mods in the Mod Manager (it can make a profile for this room), then start the game again and join.\n";
    return msg;
}

static void ShowMismatch(const std::string& msg, const char* roomList)
{
    Log("net: %s", msg.c_str());
    MenuNotice(msg.c_str(), 0);  // until dismissed
    // for the Mod Manager ("set up this room's mods"): the room's own list
    std::string path = GameFolder() + "\\mods\\last_room.txt";
    FILE* f;
    if (!fopen_s(&f, path.c_str(), "wb")) {
        fputs("# The mods of the last online room whose mods didn't match (written by the mod loader).\n", f);
        fputs(roomList && *roomList ? roomList : "none\n", f);
        fclose(f);
    }
}

// Compares the room's list with ours; on a difference shows it. -> true = they match.
static bool CheckList(void* mm, u64 lobby, bool joined)
{
    const char* list = o_getData(mm, nullptr, lobby, "dmfd_mods");
    std::string text = list ? list : "";
    std::string msg = CompareMods(text.c_str(), !text.empty(), joined);
    if (msg.empty()) {
        Log("net: the room's mods match ours");
        return true;
    }
    if (!joined) Log("net: join stopped");
    ShowMismatch(msg, text.c_str());
    return false;
}

// About to join (the game's JoinLobby): the room's data is usually there already (from the
// room list); for an invite it's asked for and waited on briefly. false = don't join.
static bool JoinAllowed(void* mm, u64 lobby)
{
    __try {
        const char* mode = o_getData(mm, nullptr, lobby, "GameMode");
        if (!(mode && *mode)) {
            ((bool(__fastcall*)(void*, void*, u64))(*(void***)mm)[MM_REQUEST_DATA])(mm, nullptr, lobby);
            for (int i = 0; i < 50 && !(mode && *mode); i++) {
                Sleep(50);
                mode = o_getData(mm, nullptr, lobby, "GameMode");
            }
        }
        if (!(mode && *mode)) {
            Log("net: the room's data isn't here yet; its mods are compared after joining");
            InterlockedExchange(&g_checkRoom, 1);
            return true;
        }
        InterlockedExchange(&g_checkRoom, 0);
        return CheckList(mm, lobby, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("net: could not read the room's data before joining");
        InterlockedExchange(&g_checkRoom, 1);
        return true;
    }
}

// Joined a room: its data (GameMode, dmfd_mods) arrives a moment after entering.
static void CheckRoom(void* mm)
{
    static DWORD since;
    static u64 room;
    if (!g_checkRoom || !g_room || g_hosting) {
        room = 0;
        return;
    }
    if (room != g_room) {
        room = g_room;
        since = GetTickCount();
    }
    const char* mode = o_getData(mm, nullptr, room, "GameMode");
    bool arrived = mode && *mode;
    if (!arrived && GetTickCount() - since < 15000) return;
    InterlockedExchange(&g_checkRoom, 0);
    if (!arrived) {
        Log("net: the room's data didn't arrive; mods not compared");
        return;
    }
    CheckList(mm, room, true);
}

// A solo preview / developers: <game>\mods\test_room_mods.txt (lines as in "dmfd_mods", or
// the single word "none" for a host without the kit) is compared once, as if just joined.
static void TestRoomFile()
{
    std::string path = GameFolder() + "\\mods\\test_room_mods.txt";
    FILE* f;
    if (fopen_s(&f, path.c_str(), "rb")) return;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    bool none = text.compare(0, 4, "none") == 0;
    Log("net: comparing with mods\\test_room_mods.txt (a pretend room)");
    std::string msg = CompareMods(text.c_str(), !none, false);
    if (msg.empty()) Log("net: the pretend room's mods match ours");
    else ShowMismatch(msg, none ? "" : text.c_str());
}

// ---- every player's list on its own room entry; the host checks the others' ----
// A guest can end up in a room with other mods anyway (an older kit, or a join the check above
// never saw), and a quest together can then crash. Each player puts its list on its own entry
// in the room (member data "dmfd_mods"; "-" = the kit, no mods that matter online). The host
// compares every other player's list with its own and is warned: Steam gives us no way to
// remove a player from the room.
using NumMembersFn = int(__fastcall*)(void*, void*, u64);
using MemberAtFn = u64*(__fastcall*)(void*, void*, u64*, u64, int);  // CSteamID through a hidden pointer
using OwnerFn = u64*(__fastcall*)(void*, void*, u64*, u64);
using GetMemberDataFn = const char*(__fastcall*)(void*, void*, u64, u64, const char*);
using SetMemberDataFn = void(__fastcall*)(void*, void*, u64, const char*, const char*);

// A guest's list against ours, said to the host; "" when they match.
static std::string GuestMismatch(const std::string& theirsText)
{
    std::vector<RoomMod> theirs = ParseMods(theirsText.c_str()), ours = ParseMods(OurMods().c_str());
    auto find = [](std::vector<RoomMod>& v, const std::string& folder) -> RoomMod* {
        for (auto& m : v)
            if (!_stricmp(m.folder.c_str(), folder.c_str())) return &m;
        return nullptr;
    };
    std::vector<std::string> install, disable, versions;
    for (auto& o : ours) {
        RoomMod* t = find(theirs, o.folder);
        if (!t) install.push_back(o.name + " " + o.version);
        else if (t->version != o.version) versions.push_back(o.name + " (yours " + o.version + ", theirs " + t->version + ")");
        else if (t->fp != o.fp) versions.push_back(o.name + " " + o.version + " (a different build of it)");
    }
    for (auto& t : theirs)
        if (!find(ours, t.folder)) disable.push_back(t.name + " " + t.version);
    if (install.empty() && disable.empty() && versions.empty()) return "";
    std::string msg = "A player in your room has different mods. Departing on a quest together could cause unwanted behavior or crashes.\n";
    if (!install.empty()) msg += "They need to install: " + Join(install) + "\n";
    if (!disable.empty()) msg += "They need to disable: " + Join(disable) + "\n";
    if (!versions.empty()) msg += "Different versions: " + Join(versions) + "\n";
    msg += "They can leave, set up the same mods in the Mod Manager, restart the game and join again.\n";
    return msg;
}

static void MemberCheck(void* mm)
{
    static DWORD last, published;
    static u64 room;
    static std::set<u64> done;          // members checked (warned, or matching)
    static std::map<u64, DWORD> since;  // members with no list yet: first seen
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;
    u64 r = g_room;
    if (r != room) {
        room = r;
        done.clear();
        since.clear();
        published = 0;
    }
    if (!r) return;
    void** vt = *(void***)mm;
    if (!published || now - published > 10000) {  // ours (again now and then: it only sticks once we're in)
        std::string mine = OurMods();
        ((SetMemberDataFn)vt[MM_SET_MEMBER_DATA])(mm, nullptr, r, "dmfd_mods", mine.empty() ? "-" : mine.c_str());
        published = now;
    }
    if (!g_hosting) return;
    u64 owner = 0;
    ((OwnerFn)vt[MM_OWNER])(mm, nullptr, &owner, r);
    int n = ((NumMembersFn)vt[MM_NUM_MEMBERS])(mm, nullptr, r);
    for (int i = 0; i < n && i < 8; i++) {
        u64 id = 0;
        ((MemberAtFn)vt[MM_MEMBER_BY_INDEX])(mm, nullptr, &id, r, i);
        if (!id || id == owner || done.count(id)) continue;
        const char* d = ((GetMemberDataFn)vt[MM_GET_MEMBER_DATA])(mm, nullptr, r, id, "dmfd_mods");
        std::string text = d ? d : "";
        std::string msg;
        if (text.empty()) {
            if (!since.count(id)) since[id] = now;
            if (now - since[id] < 15000) continue;  // give its game a moment
            msg = "A player in your room has no mod kit (or an older one), so their mods can't be checked. "
                  "Departing on a quest together could cause unwanted behavior or crashes.\n";
        } else {
            msg = GuestMismatch(text == "-" ? "" : text);
        }
        done.insert(id);
        if (msg.empty()) {
            Log("net: a player's mods match ours");
            continue;
        }
        Log("net: %s", msg.c_str());
        MenuNotice(msg.c_str(), 0);
    }
}

// ---- background ----
static bool InstallSteamHooks(void* mm)
{
    bool ok;
    {
        HookLock lock;  // Rebalance hooks the same functions from its own thread
        ok = HookSlot(mm, MM_ADD_NUM_FILTER, h_addNumFilter, (void**)&o_addNumFilter, "filter") &
             HookSlot(mm, MM_SET_DATA, h_setData, (void**)&o_setData, "set data") &
             HookSlot(mm, MM_GET_DATA, h_getData, (void**)&o_getData, "get data") &
             HookSlot(mm, MM_JOIN, h_join, (void**)&o_join, "join") &
             HookSlot(mm, MM_LEAVE, h_leave, (void**)&o_leave, "leave");
        HMODULE sa = GetModuleHandleA("steam_api.dll");
        if (ok && !(sa && HookExport(sa, "SteamAPI_RegisterCallResult", h_register, (void**)&o_register) &&
                    HookExport(sa, "SteamAPI_UnregisterCallResult", h_unregister, (void**)&o_unregister) &&
                    HookExport(sa, "SteamAPI_RunCallbacks", h_runCallbacks, (void**)&o_runCallbacks))) {
            o_runCallbacks = nullptr;  // a stopped join then returns no call (the game waits)
            Log("net: Steam call result hooks not installed");
        }
    }
    Log(ok ? "net: Steam lobby hooks installed" : "net: some Steam lobby hooks failed");
    return ok;
}

static DWORD WINAPI NetThread(LPVOID)
{
    void* mm = nullptr;
    while (!(mm = FindMatchmaking())) Sleep(250);  // until Steam is up (the game starts it a little after loading)
    if (!InstallSteamHooks(mm)) return 0;
    Sleep(8000);  // the code mods have started and the menu is up by now
    TestRoomFile();
    for (;;) {
        Sleep(500);
        __try {
            CheckRoom(mm);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_checkRoom, 0);
            Log("net: could not read the room's data");
        }
        __try {
            MemberCheck(mm);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("net: could not check the room's players");
        }
    }
}

void NetInit(bool classesOn)
{
    if (classesOn) {
        if (CodeAt(MEMBER_SET, "558bec6aff68") && HookAt(MEMBER_SET, h_memberSet, (void**)&o_memberSet))
            Log("net: party member hook installed");
        else
            Log("net: party member hook not installed (game code not as expected)");
    }
    CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);  // the room hooks (modded or not: the check on joining)
}
