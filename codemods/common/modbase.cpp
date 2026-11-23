// SPDX-License-Identifier: MIT (see LICENSE-MIT.txt here; the rest of the kit is GPL-3.0-or-later)
#include "modbase.h"
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <vector>
#include "MinHook.h"
#include "imgui.h"


static HMODULE g_self;
static char g_modDir[MAX_PATH], g_gameDir[MAX_PATH];
static FILE* g_log;
static CRITICAL_SECTION g_logLock;
static const DmfdApi* g_api;

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

const char* ModDir() { return g_modDir; }
const char* GameDir() { return g_gameDir; }

uintptr_t G(uint32_t va) { return (uintptr_t)GetModuleHandleA(nullptr) + va - 0x400000; }

bool CodeExecutable(uint32_t va)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((void*)G(va), &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

bool CodeAt(uint32_t va, const char* hex)
{
    if (!CodeExecutable(va)) return false;
    const uint8_t* p = (const uint8_t*)G(va);
    if (p[0] == 0xE9) return true;  // another mod hooked it first; MinHook chains onto that
    for (; hex[0] && hex[1]; hex += 2, p++) {
        unsigned b;
        if (sscanf_s(hex, "%2x", &b) != 1 || *p != b) return false;
    }
    return true;
}

// During ModGameReady hooks are queued and switched on together afterwards: MinHook pauses
// every thread for each switch, which costs tens of milliseconds a time.
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
    void* target = (void*)G(va);
    MH_STATUS st = MH_CreateHook(target, fn, orig);
    if (st == MH_OK) st = g_hookBatch ? MH_QueueEnableHook(target) : MH_EnableHook(target);
    if (st != MH_OK) Log("hook at %#x failed: %s", va, MH_StatusToString(st));
    return st == MH_OK;
}

// ---- the loader's menu ----
const DmfdApi* Api() { return g_api; }

uint32_t Frame()
{
    return g_api ? g_api->frame() : (uint32_t)(GetTickCount64() / 16);
}

int MenuKey() { return g_api ? g_api->menuKey() : VK_INSERT; }

bool InOnlineRoom() { return g_api && g_api->apiVersion >= 2 && g_api->inOnlineRoom(); }

bool AddFrameDraw(void (*draw)(void*, void*), void* user)
{
    if (!g_api || g_api->apiVersion < 3 || !g_api->addFrameDraw) return false;
    g_api->addFrameDraw(draw, user);
    return true;
}

struct Tab { const char* name; void (*draw)(); };
static std::vector<Tab> g_pending;  // tabs added before the loader handed over its API

// Runs in the loader's menu: point this DLL's ImGui at the loader's context and memory.
static void DrawThunk(void* user)
{
    static bool allocSet;
    if (!allocSet) {
        void *a, *f, *u;
        g_api->imguiAllocators(&a, &f, &u);
        ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)a, (ImGuiMemFreeFunc)f, u);
        allocSet = true;
    }
    ImGui::SetCurrentContext((ImGuiContext*)g_api->imguiContext());
    ((void (*)())user)();
}

static void Register(const Tab& t)
{
    if (g_api->imguiVersion != IMGUI_VERSION_NUM) {
        Log("menu: the loader's ImGui is %d, this mod's is %d; the %s tab is left out", g_api->imguiVersion,
            IMGUI_VERSION_NUM, t.name);
        return;
    }
    g_api->addTab(MOD_NAME_STR, t.name, DrawThunk, (void*)t.draw);
}

void AddMenuTab(const char* tab, void (*draw)())
{
    Tab t{tab, draw};
    if (g_api) Register(t);
    else g_pending.push_back(t);
}

extern "C" __declspec(dllexport) void DmfdModStart(const DmfdApi* api)
{
    if (!api || api->apiVersion < 1) return;
    g_api = api;
    Log("mod loader %s, menu API %d", api->loaderVersion, api->apiVersion);
    for (const Tab& t : g_pending) Register(t);
    g_pending.clear();
    g_hookBatch = true;
    ModGameReady();
    g_hookBatch = false;
    MH_STATUS st;
    {
        HookLock lock;
        st = MH_ApplyQueued();
    }
    if (st != MH_OK) Log("switching on the hooks failed: %s", MH_StatusToString(st));
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        InitializeCriticalSection(&g_logLock);
        GetModuleFileNameA(self, g_modDir, MAX_PATH);
        if (char* s = strrchr(g_modDir, '\\')) *s = 0;
        GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
        if (char* s = strrchr(g_gameDir, '\\')) *s = 0;
        char path[MAX_PATH];
        sprintf_s(path, "%s\\mod.log", g_modDir);
        g_log = _fsopen(path, "w", _SH_DENYWR);  // the manager can read it while the game runs
        Log("%s %s loaded, game base %p", MOD_NAME_STR, MOD_VERSION_STR, GetModuleHandleA(nullptr));
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) Log("MinHook: %s", MH_StatusToString(st));
        ModMain();
    }
    return TRUE;
}
