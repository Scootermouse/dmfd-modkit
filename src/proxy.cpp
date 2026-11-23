// winmm.dll proxy for the DMFD content mod.
//
// The game imports winmm.dll, and Windows looks for it in the game folder
// first, so this DLL loads with the game. Every winmm export jumps straight
// to the real system DLL; the mod itself only installs its hooks. It is
// independent of the rebalance mod (dinput8.dll): either can be installed
// alone. Where both hook the same function (the class-owned check, Steam's
// lobby functions) each accepts the other's hook and chains onto it.
#include "content.h"
#include <cstdio>
#include <share.h>
#include "MinHook.h"

// ---- forwarding ----
#define X(i, o, n) +1
static const int N_EXPORTS = 0
#include "winmm_exports.inc"
    ;
#undef X
static FARPROC g_fwd[N_EXPORTS];

#define X(i, o, n) \
    extern "C" __declspec(naked) void fwd_##i() { __asm { jmp dword ptr [g_fwd + 4 * i] } }
#include "winmm_exports.inc"
#undef X

static bool LoadReal()
{
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for this 32-bit process
    strcat_s(path, "\\winmm.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) return false;
    struct E { int i, ord; const char* name; };
#define X(i, o, n) {i, o, n},
    static const E exps[] = {
#include "winmm_exports.inc"
    };
#undef X
    for (const E& e : exps)
        g_fwd[e.i] = GetProcAddress(real, e.name ? e.name : MAKEINTRESOURCEA(e.ord));
    return true;
}

// ---- log ----
static FILE* g_log;
static CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ---- hooks ----
static uint8_t* Game(uint32_t va) { return (uint8_t*)GetModuleHandleA(nullptr) + va - 0x400000; }

static bool g_codeLog;  // log failed code checks (only on a last attempt)

bool CodeAt(uint32_t va, const char* hex)
{
    const uint8_t* p = Game(va);
    // Steam's wrapper unpacks into writable memory and only later marks it
    // executable; MinHook refuses to hook until then.
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        if (g_codeLog) Log("code check %08X: not executable yet", va);
        return false;
    }
    const uint8_t* start = p;
    for (; hex[0] && hex[1]; hex += 2, p++) {
        unsigned b;
        if (sscanf_s(hex, "%2x", &b) != 1 || *p != b) {
            if (g_codeLog)
                Log("code check %08X: found %02X %02X %02X %02X %02X %02X", va, start[0], start[1], start[2], start[3],
                    start[4], start[5]);
            return false;
        }
    }
    return true;
}

// Like CodeAt, but also accepts a function another mod has already hooked
// (starts with a jump), e.g. the rebalance mod's class-unlock hook on 0xC92540.
// MinHook chains onto the jump, so both hooks keep working.
bool CodeAtOrHooked(uint32_t va, const char* hex)
{
    if (CodeAt(va, hex)) return true;
    MEMORY_BASIC_INFORMATION mi;
    const uint8_t* p = Game(va);
    return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           p[0] == 0xE9;
}

// While setting up at the game's first step, hooks are queued and switched on together:
// MinHook pauses every thread for each switch, which costs tens of milliseconds a time.
static bool g_hookBatch;

HookLock::HookLock()
{
    wchar_t name[64];
    swprintf_s(name, L"Local\\DMFD.HookLock.%lu", GetCurrentProcessId());
    h = CreateMutexW(nullptr, FALSE, name);
    if (h) WaitForSingleObject(h, 10000);
}

HookLock::~HookLock()
{
    if (h) {
        ReleaseMutex(h);
        CloseHandle(h);
    }
}

bool HookAt(uint32_t va, void* fn, void** orig)
{
    HookLock lock;
    void* target = Game(va);
    MH_STATUS st = MH_CreateHook(target, fn, orig);
    if (st == MH_OK) st = g_hookBatch ? MH_QueueEnableHook(target) : MH_EnableHook(target);
    if (st != MH_OK) Log("hook at %#x failed: %s", va, MH_StatusToString(st));
    return st == MH_OK;
}

// Crash logger: records where an access violation happened (as an address in
// the unpacked exe, so it can be looked up in the disassembly) plus game-code
// return addresses found on the stack. First-chance, so it also sees crashes
// the game would otherwise swallow; limited to a few entries.
static LONG CALLBACK CrashLog(EXCEPTION_POINTERS* e)
{
    static volatile LONG count;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery((void*)base, &mi, sizeof mi);
    auto toVa = [&](uintptr_t a) -> uint32_t {
        return (a >= base && a < base + 0x2000000) ? (uint32_t)(a - base + 0x400000) : 0;
    };
    CONTEXT* c = e->ContextRecord;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    Log("CRASH %08lx at %p (exe %#x), access %s %p", code, (void*)at, toVa(at),
        e->ExceptionRecord->NumberParameters > 1 ? (e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read") : "?",
        e->ExceptionRecord->NumberParameters > 1 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : nullptr);
    Log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    char buf[512];
    int len = 0, found = 0;
    uint32_t* sp = (uint32_t*)c->Esp;
    for (int i = 0; i < 512 && found < 16; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if (uint32_t va = toVa(sp[i])) {
            if (va >= 0x401000 && va < 0x1000000) {  // code range of the exe
                len += sprintf_s(buf + len, sizeof buf - len, " %x", va);
                found++;
            }
        }
    }
    Log("  stack (exe addresses):%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The content engine's hooks, once the game code is unpacked. True if they went in.
static bool ContentInit()
{
    if (!DataModReady() || !DataModInit()) return false;
    if (IconsReady()) IconsInit();
    else Log("icons: game code not as expected; new icon numbers disabled");
    bool ready = ClassesReady();
    if (!ready) {
        g_codeLog = true;
        ready = ClassesReady();
        g_codeLog = false;
    }
    bool classesOn = ready && ClassesInit();
    if (!ready) Log("classes: game code not as expected; extra classes disabled");
    NetInit(classesOn);
    return true;
}

// The old way (only if GameStart couldn't do it): wait for Steam's wrapper to unpack the code.
static DWORD WINAPI ModThread(LPVOID)
{
    for (int i = 0; i < 6000; i++) {  // up to a minute
        if (DataModReady() && DataModInit()) {
            if (IconsReady()) IconsInit();
            else Log("icons: game code not as expected; new icon numbers disabled");
            // parts of the code can still be unpacking: retry for a few seconds
            bool ready = false;
            for (int j = 0; j < 500 && !(ready = ClassesReady()); j++) Sleep(10);
            if (!ready) {
                g_codeLog = true;
                ready = ClassesReady();
                g_codeLog = false;
            }
            bool classesOn = ready && ClassesInit();
            if (!ready) Log("classes: game code not as expected; extra classes disabled");
            NetInit(classesOn);
            return 0;
        }
        Sleep(10);
    }
    Log("game code never became ready (or is a different version); content mod disabled");
    return 0;
}

// Everything the loader sets up: the menu, every mod, the content engine. Called once, at
// the game's first step on its own thread (atEntry), or later from a watchdog if that was missed.
void GameStart(bool atEntry)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Log("startup: %s", atEntry ? "the game's code is unpacked and hasn't started yet; setting up now"
                               : "setting up late");
    MenuInit();                    // first: mods add their menu pages as they start
    g_hookBatch = true;
    bool content = ContentInit();  // before the mods, so its code checks see the game's own code
    g_hookBatch = false;
    MH_STATUS st;
    {
        HookLock lock;
        st = MH_ApplyQueued();  // all at once; in place before any mod hooks the same code
    }
    if (st != MH_OK) Log("startup: switching on the content hooks failed: %s", MH_StatusToString(st));
    LoadCodeMods();                // each mod puts its hooks in as it starts
    QueryPerformanceCounter(&t1);
    Log("startup: done in %d ms", (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart));
    if (!content) CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!LoadReal()) return FALSE;
        InitializeCriticalSection(&g_logLock);
        char dir[MAX_PATH], path[MAX_PATH];
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* s = strrchr(dir, '\\')) *s = 0;
        sprintf_s(path, "%s\\dmfd_content.log", dir);
        g_log = _fsopen(path, "w", _SH_DENYWR);  // others (the manager) can read it while the game runs
        Log("dmfd content mod %s loaded, game base %p", CONTENT_VERSION, GetModuleHandleA(nullptr));
        // "Launch without mods" (the Mod Manager): one start with nothing changed at all
        char once[MAX_PATH];
        sprintf_s(once, "%s\\mods\\launch_vanilla_once", dir);
        if (GetFileAttributesA(once) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(once);
            Log("starting without mods this once (Launch without mods); nothing is changed");
            return TRUE;
        }
        MH_Initialize();
        AddVectoredExceptionHandler(0, CrashLog);
        SaveWatchInit();  // the game can read its save within a second or two
        StartupInit();    // everything else runs at the game's first step (startup.cpp)
    }
    return TRUE;
}
