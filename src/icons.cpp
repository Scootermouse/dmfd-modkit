// Extra item icon numbers (24 and up).
//
// The game turns an item's icon number (record +0x08) into the hash of one of
// 24 names (0xCCCB20) and draws that entry of Win/com_itic.osb; numbers above
// 23 get 0 (no icon). Build mod can add entries named MOD_ICON_24, MOD_ICON_25,
// ... to the sheet (tools/icons.py) and lists their numbers in
// DataMod\icons.txt. For those numbers this hook returns the new names' hash;
// everything else goes to the game's own table.
#include "content.h"
#include <cstdio>

static const uint32_t ICON_HASH = 0xCCCB20;  // uint32 __fastcall (int iconNumber)
static const int FIRST_NEW = 24, MAX_NEW = 64;

static uint32_t g_hash[MAX_NEW];   // hash per icon number - FIRST_NEW, 0 = not provided
static int g_count;

using IconHashFn = uint32_t(__fastcall*)(int);
static IconHashFn o_iconHash;

static uint32_t IceHash(const char* s)
{
    uint32_t h = 0xCDE723A5;
    for (; *s; s++) h = (h + (uint32_t)(int)(signed char)*s) * 0x8D;
    return h;
}

static uint32_t __fastcall h_iconHash(int n)
{
    if (n >= FIRST_NEW && n < FIRST_NEW + MAX_NEW && g_hash[n - FIRST_NEW]) return g_hash[n - FIRST_NEW];
    return o_iconHash(n);
}

bool IconsReady()
{
    return CodeAt(ICON_HASH, "64a12c000000568bf1");
}

bool IconsInit()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    strcat_s(path, "\\DataMod\\icons.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return true;  // no new icons: leave the game alone
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int n = atoi(line);
        if (n >= FIRST_NEW && n < FIRST_NEW + MAX_NEW) {
            char name[32];
            sprintf_s(name, "MOD_ICON_%02d", n);
            g_hash[n - FIRST_NEW] = IceHash(name);
            g_count++;
        }
    }
    fclose(f);
    if (!g_count) return true;
    bool ok = HookAt(ICON_HASH, h_iconHash, (void**)&o_iconHash);
    Log(ok ? "icons: %d new icon number(s)" : "icons: hook failed (%d new icons not shown)", g_count);
    return ok;
}
