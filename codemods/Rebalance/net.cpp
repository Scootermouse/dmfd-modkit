// Online: modded-only rooms and the host's co-op combat settings, through Steam
// lobby data (the game's rooms are Steam lobbies).
//
// - Rooms: the mod loader keeps modded and vanilla rooms apart and checks that everyone
//   in a room has the same mods (this mod's mod.ini says online=same), so this mod no
//   longer shifts the rooms' "GameMode" itself (GAMEMODE_SHIFT 0; it was 7100000).
// - The host also sets "dmfdmod" = version + its co-op combat values; every
//   modded member sets member data "dmfdmod" = version. When everyone in the
//   room has it, all machines use the host's values, so they all agree.
#include "mod.h"
#include "MinHook.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int GAMEMODE_SHIFT = 0;  // the loader separates rooms now
static const char MOD_KEY[] = "dmfdmod";
static const int NET_VERSION = 2;  // bump when the synced settings change meaning

// ISteamMatchmaking009 vtable slots
enum { MM_ADD_NUM_FILTER = 6, MM_JOIN = 14, MM_LEAVE = 15, MM_NUM_MEMBERS = 17, MM_MEMBER_BY_INDEX = 18,
       MM_GET_DATA = 19, MM_SET_DATA = 20, MM_GET_MEMBER_DATA = 24, MM_SET_MEMBER_DATA = 25, MM_OWNER = 35 };
enum { USER_GET_STEAMID = 2 };  // ISteamUser020

typedef unsigned long long u64;
using AddNumFilterFn = void(__fastcall*)(void*, void*, const char*, int, int);
using SetDataFn = bool(__fastcall*)(void*, void*, u64, const char*, const char*);
using GetDataFn = const char*(__fastcall*)(void*, void*, u64, const char*);
using GetMemberDataFn = const char*(__fastcall*)(void*, void*, u64, u64, const char*);
using SetMemberDataFn = void(__fastcall*)(void*, void*, u64, const char*, const char*);
using JoinFn = u64(__fastcall*)(void*, void*, u64);  // returns SteamAPICall_t
using LeaveFn = void(__fastcall*)(void*, void*, u64);
using NumMembersFn = int(__fastcall*)(void*, void*, u64);
using SteamIdAtFn = u64*(__fastcall*)(void*, void*, u64*, u64, int);  // CSteamID returned through a hidden pointer
using SteamIdFn = u64*(__fastcall*)(void*, void*, u64*);
using OwnerFn = u64*(__fastcall*)(void*, void*, u64*, u64);

static void* g_mm;    // ISteamMatchmaking
static void* g_user;  // ISteamUser
static AddNumFilterFn o_addNumFilter;
static SetDataFn o_setData;
static GetDataFn o_getData;
static JoinFn o_join;
static LeaveFn o_leave;

static CRITICAL_SECTION g_netLock;
static volatile u64 g_lobby;  // the lobby we're in (0 = none)
static bool g_published;      // our member data set for g_lobby
NetState g_net;

static void** VT(void* iface) { return *(void***)iface; }

static bool IsGameMode(const char* key) { return key && !strcmp(key, "GameMode"); }

static void __fastcall h_addNumFilter(void* mm, void* edx, const char* key, int value, int cmp)
{
    if (IsGameMode(key)) value += GAMEMODE_SHIFT;
    o_addNumFilter(mm, edx, key, value, cmp);
}

static void HostPayload(char* out, int n)
{
    Settings s = SettingsGet();
    int len = snprintf(out, n, "%d|%.3f|%.3f|%.3f|%.3f", NET_VERSION, s.coopHost.m[K_TAKEN], s.coopHost.m[K_DEALT],
                       s.coopHost.m[K_ENEMY_HP], s.coopHost.m[K_BOSS_HP]);
    for (int c = 0; c < CLS_COUNT; c++)
        len += snprintf(out + len, n - len, "|%.3f|%.3f", s.classes.dealt[c], s.classes.taken[c]);
}

// "version|taken|dealt|enemy|boss" then dealt|taken per class.
static bool ParsePayload(const char* p, Profile& host, ClassBal& cls)
{
    float v[4 + 2 * CLS_COUNT];
    char* end;
    if (strtol(p, &end, 10) != NET_VERSION) return false;
    for (float& f : v) {
        if (*end != '|') return false;
        p = end + 1;
        f = strtof(p, &end);
        if (end == p) return false;
    }
    host.m[K_TAKEN] = v[0];
    host.m[K_DEALT] = v[1];
    host.m[K_ENEMY_HP] = v[2];
    host.m[K_BOSS_HP] = v[3];
    for (int c = 0; c < CLS_COUNT; c++) {
        cls.dealt[c] = v[4 + 2 * c];
        cls.taken[c] = v[5 + 2 * c];
    }
    return true;
}

static bool __fastcall h_setData(void* mm, void* edx, u64 lobby, const char* key, const char* value)
{
    char shifted[32];
    if (IsGameMode(key) && value) {
        sprintf_s(shifted, "%d", atoi(value) + GAMEMODE_SHIFT);
        value = shifted;
    }
    bool r = o_setData(mm, edx, lobby, key, value);
    // Only the owner can set lobby data, so the game doing it means we're hosting.
    if (r && key && strcmp(key, MOD_KEY)) {
        EnterCriticalSection(&g_netLock);
        bool first = g_lobby != lobby || !g_net.hosting;
        g_lobby = lobby;
        g_net.hosting = true;
        LeaveCriticalSection(&g_netLock);
        if (first) {
            char p[256];
            HostPayload(p, sizeof p);
            o_setData(mm, edx, lobby, MOD_KEY, p);
            Log("net: hosting a modded room");
        }
    }
    return r;
}

static const char* __fastcall h_getData(void* mm, void* edx, u64 lobby, const char* key)
{
    const char* v = o_getData(mm, edx, lobby, key);
    if (IsGameMode(key) && v && *v) {
        static char buf[8][32];  // the game may hold a couple of these at once
        static int next;
        char* b = buf[next++ & 7];
        sprintf_s(b, 32, "%d", atoi(v) - GAMEMODE_SHIFT);
        return b;
    }
    return v;
}

static u64 __fastcall h_join(void* mm, void* edx, u64 lobby)
{
    EnterCriticalSection(&g_netLock);
    g_lobby = lobby;
    g_published = false;
    g_net.hosting = false;
    LeaveCriticalSection(&g_netLock);
    Log("net: joining a room");
    return o_join(mm, edx, lobby);
}

static void __fastcall h_leave(void* mm, void* edx, u64 lobby)
{
    EnterCriticalSection(&g_netLock);
    if (lobby == g_lobby) {
        g_lobby = 0;
        g_net = NetState{};
        g_published = false;
    }
    LeaveCriticalSection(&g_netLock);
    Log("net: left the room");
    o_leave(mm, edx, lobby);
}

static bool HookSlot(void* iface, int slot, void* fn, void** orig, const char* name)
{
    void* target = VT(iface)[slot];
    MH_STATUS st = MH_CreateHook(target, fn, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) Log("net: hook %s failed: %s", name, MH_StatusToString(st));
    return st == MH_OK;
}

// Steam's interfaces exist once the game has called SteamAPI_Init.
static bool FindInterfaces()
{
    HMODULE sa = GetModuleHandleA("steam_api.dll");
    if (!sa) return false;
    using GetUserFn = int(__cdecl*)();
    using FindFn = void*(__cdecl*)(int, const char*);
    auto getUser = (GetUserFn)GetProcAddress(sa, "SteamAPI_GetHSteamUser");
    auto find = (FindFn)GetProcAddress(sa, "SteamInternal_FindOrCreateUserInterface");
    if (!getUser || !find) return false;
    int user = getUser();
    if (!user) return false;
    g_mm = find(user, "SteamMatchMaking009");
    g_user = find(user, "SteamUser020");
    return g_mm && g_user;
}

// Background thread, a few times a second: our member data, and whether the
// room is all modded (and with which host values).
void NetPoll()
{
    static bool hooked, failed;
    if (failed) return;
    if (!hooked) {
        if (!FindInterfaces()) return;
        HookLock lock;  // the loader hooks the same functions
        hooked = HookSlot(g_mm, MM_ADD_NUM_FILTER, h_addNumFilter, (void**)&o_addNumFilter, "filter") &
                 HookSlot(g_mm, MM_SET_DATA, h_setData, (void**)&o_setData, "set data") &
                 HookSlot(g_mm, MM_GET_DATA, h_getData, (void**)&o_getData, "get data") &
                 HookSlot(g_mm, MM_JOIN, h_join, (void**)&o_join, "join") &
                 HookSlot(g_mm, MM_LEAVE, h_leave, (void**)&o_leave, "leave");
        failed = !hooked;
        Log(hooked ? "net: Steam lobby hooks installed" : "net: Steam lobby hooks failed; online stays vanilla");
        return;
    }

    EnterCriticalSection(&g_netLock);
    u64 lobby = g_lobby;
    bool hosting = g_net.hosting;
    LeaveCriticalSection(&g_netLock);
    if (!lobby) return;

    void** mm = VT(g_mm);
    u64 me = 0;
    ((SteamIdFn)VT(g_user)[USER_GET_STEAMID])(g_user, nullptr, &me);
    char ver[16];
    sprintf_s(ver, "%d", NET_VERSION);
    auto memberData = (GetMemberDataFn)mm[MM_GET_MEMBER_DATA];
    if (!g_published) {
        ((SetMemberDataFn)mm[MM_SET_MEMBER_DATA])(g_mm, nullptr, lobby, MOD_KEY, ver);
        const char* mine = memberData(g_mm, nullptr, lobby, me, MOD_KEY);
        g_published = mine && !strcmp(mine, ver);
    }
    if (hosting) {  // keep the published host values current
        char p[256];
        HostPayload(p, sizeof p);
        const char* cur = o_getData(g_mm, nullptr, lobby, MOD_KEY);
        if (!cur || strcmp(cur, p)) o_setData(g_mm, nullptr, lobby, MOD_KEY, p);
    }

    NetState ns{};
    ns.inRoom = true;
    ns.hosting = hosting;
    int n = ((NumMembersFn)mm[MM_NUM_MEMBERS])(g_mm, nullptr, lobby);
    ns.members = n;
    ns.allModded = n > 0;
    for (int i = 0; i < n; i++) {
        u64 id = 0;
        ((SteamIdAtFn)mm[MM_MEMBER_BY_INDEX])(g_mm, nullptr, &id, lobby, i);
        const char* v = memberData(g_mm, nullptr, lobby, id, MOD_KEY);
        if (!v || strcmp(v, ver)) ns.allModded = false;
    }
    const char* host = o_getData(g_mm, nullptr, lobby, MOD_KEY);
    if (host && ParsePayload(host, ns.host, ns.hostClasses)) ns.hostModded = true;
    ns.synced = ns.allModded && ns.hostModded;

    EnterCriticalSection(&g_netLock);
    bool status = ns.synced != g_net.synced || ns.members != g_net.members || ns.allModded != g_net.allModded;
    bool values = memcmp(&ns.host, &g_net.host, sizeof ns.host) != 0 ||
                  memcmp(&ns.hostClasses, &g_net.hostClasses, sizeof ns.hostClasses) != 0;
    if (g_lobby == lobby) g_net = ns;
    LeaveCriticalSection(&g_netLock);
    if (status)
        Log("net: %d player(s), %s; combat %s", n, ns.allModded ? "all modded" : "not all modded",
            ns.synced ? "uses the host's settings" : "vanilla");
    if (status || values) SettingsSet(SettingsGet(), false);  // re-resolve the co-op combat values

    // Log the combat values in use once the host stops changing them (sliders
    // republish many times a second while dragged).
    static Profile logged;  // all zero = nothing logged yet
    static ClassBal loggedCls;
    static ULONGLONG changedAt;
    ULONGLONG now = GetTickCount64();
    if (status || values) changedAt = now;
    if (!ns.synced) {
        logged = Profile{};
        loggedCls = ClassBal{};
    }
    else if ((memcmp(&logged, &ns.host, sizeof logged) || memcmp(&loggedCls, &ns.hostClasses, sizeof loggedCls)) &&
             now - changedAt >= 1000) {
        Log("net: host's combat values: damage taken %.2f, damage dealt %.2f, enemy HP %.2f, boss HP %.2f",
            ns.host.m[K_TAKEN], ns.host.m[K_DEALT], ns.host.m[K_ENEMY_HP], ns.host.m[K_BOSS_HP]);
        for (int c = 0; c < CLS_COUNT; c++)
            if (ns.hostClasses.dealt[c] != 1.0f || ns.hostClasses.taken[c] != 1.0f)
                Log("net:   %s dealt %.2f, taken %.2f", CLASS_NAMES[c], ns.hostClasses.dealt[c], ns.hostClasses.taken[c]);
        logged = ns.host;
        loggedCls = ns.hostClasses;
    }
}

NetState NetGet()
{
    EnterCriticalSection(&g_netLock);
    NetState s = g_net;
    LeaveCriticalSection(&g_netLock);
    return s;
}

void NetInit()
{
    InitializeCriticalSection(&g_netLock);
}
