// Data overlay: serve replacement asset files without touching DataHash.
//
// The game names each asset file DataHash\<md5 of asset name> and opens it in
// IceReader (0xE81A30) with _wfopen_s, the only caller of that CRT function.
// Before reading, it stats the same path (0xE82350 -> _wstat, also its only
// caller) to get the size. If <game>\DataMod\<same 32-hex name> exists, both
// calls get that path instead. Overlay files are encoded exactly like the
// originals (tools/dmfd_data.py pack), so the game decrypts and inflates
// them as usual.
#include "content.h"
#include <cstdio>
#include <cwchar>

static const uint32_t WFOPEN_S = 0xF61A8D;  // errno_t __cdecl (FILE**, const wchar_t* path, const wchar_t* mode)
static const uint32_t WSTAT = 0xF63786;     // int __cdecl (const wchar_t* path, struct stat*)

static wchar_t g_modDir[MAX_PATH];  // <game>\DataMod\ (with trailing slash)

using WfopenFn = int(__cdecl*)(FILE**, const wchar_t*, const wchar_t*);
using WstatFn = int(__cdecl*)(const wchar_t*, void*);
static WfopenFn o_wfopen;
static WstatFn o_wstat;

// Overlay path for a DataHash file that has a replacement, else nullptr.
static const wchar_t* Redirect(const wchar_t* path, wchar_t* buf, size_t n)
{
    if (!path) return nullptr;
    size_t len = wcslen(path);
    if (len < 32 + 9) return nullptr;
    const wchar_t* name = path + len - 32;
    for (int i = 0; i < 32; i++) {
        wchar_t c = name[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return nullptr;
    }
    wchar_t sep = name[-1];
    if (sep != L'\\' && sep != L'/') return nullptr;
    if (_wcsnicmp(name - 9, L"DataHash", 8) != 0) return nullptr;
    swprintf_s(buf, n, L"%s%s", g_modDir, name);
    DWORD a = GetFileAttributesW(buf);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return buf;
}

// Built-in table edits go in once the game is up: its first data-file access
// happens after its startup code has run, and before it reads any items.
static volatile LONG g_tablesDone;
static void ApplyTablesOnce()
{
    if (InterlockedCompareExchange(&g_tablesDone, 1, 0) == 0) {
        ItemsApply();
        DropsApply();
    }
}

static int __cdecl h_wfopen(FILE** f, const wchar_t* path, const wchar_t* mode)
{
    ApplyTablesOnce();
    wchar_t buf[MAX_PATH];
    if (mode && mode[0] == L'r')
        if (const wchar_t* alt = Redirect(path, buf, MAX_PATH)) {
            Log("DataMod: serving %ls", alt + wcslen(g_modDir));
            return o_wfopen(f, alt, mode);
        }
    return o_wfopen(f, path, mode);
}

static int __cdecl h_wstat(const wchar_t* path, void* st)
{
    ApplyTablesOnce();
    wchar_t buf[MAX_PATH];
    if (const wchar_t* alt = Redirect(path, buf, MAX_PATH)) return o_wstat(alt, st);
    return o_wstat(path, st);
}

bool DataModReady()
{
    return CodeAt(WFOPEN_S, "8bff558bec568b750885f6") &&  // _wfopen_s prologue
           CodeAt(WSTAT, "8bff558bec5de93df4ffff");       // _wstat: jmp to the real one
}

bool DataModInit()
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    swprintf_s(g_modDir, L"%s\\DataMod\\", dir);
    int files = 0;
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s*", g_modDir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files++;
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Safe to call again after a failure: each hook is installed only once.
    if (!o_wfopen) HookAt(WFOPEN_S, h_wfopen, (void**)&o_wfopen);
    if (!o_wstat) HookAt(WSTAT, h_wstat, (void**)&o_wstat);
    bool ok = o_wfopen && o_wstat;
    if (ok) Log("DataMod hooks installed, %d overlay file(s)", files);
    return ok;
}
