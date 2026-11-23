// Run all setup at the game's first step.
//
// Steam's wrapper unpacks the game's code after our DLL loads, then jumps to the game's
// own entry point. The game's startup (the C runtime) begins by calling a few Windows
// functions: IsProcessorFeaturePresent, GetSystemTimeAsFileTime, InitializeSListHead,
// GetStartupInfoW, GetCommandLineA/W. The first of those calls that comes from the game's
// own code (its .text section) means: the code is unpacked and the game hasn't done
// anything yet. Right there, on the game's thread, the loader installs its hooks and
// starts every mod, then lets the call through. Nothing can race the game any more.
//
// If that moment is missed (say Steam's wrapper changes), a watchdog thread falls back to
// the old way: waiting for the code to unpack, on a background thread.
#include "content.h"
#include "MinHook.h"

static volatile LONG g_started;  // GameStart ran (or is running)
static uintptr_t g_textLo, g_textHi;  // the game's code section

static bool FromGameCode(void* ret)
{
    return (uintptr_t)ret >= g_textLo && (uintptr_t)ret < g_textHi;
}

static void Fire(void* ret);

// One detour per function and module (kernel32 may forward to kernelbase or not).
#define TRIGGER(name, ret_t, params, args)                                         \
    static ret_t(WINAPI* o_##name##_k32) params;                                    \
    static ret_t(WINAPI* o_##name##_kb) params;                                     \
    static ret_t WINAPI h_##name##_k32 params                                       \
    {                                                                               \
        Fire(_ReturnAddress());                                                     \
        return o_##name##_k32 args;                                                 \
    }                                                                               \
    static ret_t WINAPI h_##name##_kb params                                        \
    {                                                                               \
        Fire(_ReturnAddress());                                                     \
        return o_##name##_kb args;                                                  \
    }

#include <intrin.h>
TRIGGER(IsProcessorFeaturePresent, BOOL, (DWORD f), (f))
TRIGGER(GetSystemTimeAsFileTime, void, (LPFILETIME t), (t))
TRIGGER(InitializeSListHead, void, (PSLIST_HEADER h), (h))
TRIGGER(GetStartupInfoW, void, (LPSTARTUPINFOW s), (s))
TRIGGER(GetCommandLineA, LPSTR, (), ())
TRIGGER(GetCommandLineW, LPWSTR, (), ())

static struct {
    const char* name;
    void* k32;
    void* kb;
    void** o_k32;
    void** o_kb;
} TRIGGERS[] = {
#define T(name) {#name, (void*)h_##name##_k32, (void*)h_##name##_kb, (void**)&o_##name##_k32, (void**)&o_##name##_kb}
    T(IsProcessorFeaturePresent), T(GetSystemTimeAsFileTime), T(InitializeSListHead),
    T(GetStartupInfoW), T(GetCommandLineA), T(GetCommandLineW),
#undef T
};
static void* g_hooked[2 * ARRAYSIZE(TRIGGERS)];
static int g_nHooked;

static void Unhook()
{
    for (int i = 0; i < g_nHooked; i++) MH_DisableHook(g_hooked[i]);
}

static void Fire(void* ret)
{
    if (g_started || !FromGameCode(ret) || !DataModReady()) return;  // and the code really is unpacked
    if (InterlockedExchange(&g_started, 1)) return;
    Unhook();  // our detour keeps running; the original stays reachable through its trampoline
    GameStart(true);
}

// Backup, in case the game's first step is never seen: watch for the code being unpacked
// from the start, and if the first step still hasn't come a quarter second after that, set
// up from here (the way it worked before: right away, alongside the game's own startup).
static DWORD WINAPI Watchdog(LPVOID)
{
    int readyFor = 0;  // ms since the code was seen unpacked
    for (int i = 0; i < 6000 && !g_started; i++) {  // up to a minute
        Sleep(10);
        if (g_textLo && DataModReady()) readyFor += 10;
        else if (!g_textLo && i >= 25) readyFor = 250;  // no watch points at all: just go
        if (readyFor >= 250) break;
    }
    if (InterlockedExchange(&g_started, 1)) return 0;
    Unhook();
    Log("startup: didn't see the game's first step; setting up alongside the game instead");
    GameStart(false);
    return 0;
}

void StartupInit()
{
    // the game's .text section, from its headers in memory
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) {
            g_textLo = (uintptr_t)base + sec[i].VirtualAddress;
            g_textHi = g_textLo + sec[i].Misc.VirtualSize;
        }
    if (!g_textLo) {
        Log("startup: game code section not found; setting up the slow way");
        CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
        return;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll"), kb = GetModuleHandleA("kernelbase.dll");
    for (auto& t : TRIGGERS) {
        void* a = k32 ? (void*)GetProcAddress(k32, t.name) : nullptr;
        void* b = kb ? (void*)GetProcAddress(kb, t.name) : nullptr;
        if (a && MH_CreateHook(a, t.k32, t.o_k32) == MH_OK && MH_EnableHook(a) == MH_OK) g_hooked[g_nHooked++] = a;
        if (b && b != a && MH_CreateHook(b, t.kb, t.o_kb) == MH_OK && MH_EnableHook(b) == MH_OK) g_hooked[g_nHooked++] = b;
    }
    Log("startup: waiting for the game's first step (%d watch points)", g_nHooked);
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
}
