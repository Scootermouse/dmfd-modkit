// An extra class's own effect and weapon files.
//
// An extra class has its own body (classes.txt body=X_cb00.osb) but used its base class's
// effect file (plN_ef00: slash effects carry the swings' attack boxes, so reach lives there) and
// the weapon files every item names (plN_cwXX, Shinobi throwing weapons pl2_twXX; each holds the
// weapon's place on every frame). A new body with other proportions needs both changed without
// changing the base class. By name, when the data mod has the file:
//   X_ef00.osb           in place of the base class's plN_ef00.osb
//   X_<weapon file>.osb  in place of that weapon file, e.g. myclass_pl0_cw00.osb for pl0_cw00.osb
// (X = the body's name without _cb00). Two places:
//  - the class's quest preload (h_preloadLog in classes.cpp) also queues its own files: a sprite
//    is only made from a loaded file;
//  - making a sprite (0xE093C0, thiscall(actor, file name, a, b), hooked in classes.cpp as
//    h_makeSprite) for this class's player (or an attack object it owns) asks for its own file
//    (ClassFileFor); if that one isn't loaded (no sprite comes back), the original is used.
// Combining data mods gives each mod's new files its own prefix (m<id>_myclass_cb00.osb), so a
// class's own files must be in the same mod as its body.
#include "content.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_set>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

static uint8_t* Game(uint32_t va) { return (uint8_t*)GetModuleHandleA(nullptr) + va - 0x400000; }
static const int MAXC = 128;
static const uint32_t STRING_CTOR = 0x7A2A40;     // thiscall(IceString*, const char*)
static const uint32_t LOADER_ADD = 0xB120B0;      // thiscall(loader, IceString*): queue a file
static const uint32_t PLAYER_VTABLES[] = {0x1112C5C, 0x110F9C4, 0x11085F4, 0x110BD58, 0x10DB36C, 0x10DD5C4};

struct Swap { char from[32]; char to[96]; };
struct ClassFiles { int n; Swap s[48]; };
static ClassFiles* g_files[MAXC];

static HCRYPTPROV g_prov;
static bool Md5Hex(const char* s, char out[33])
{
    if (!g_prov && !CryptAcquireContextA(&g_prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    HCRYPTHASH h = 0;
    bool ok = false;
    if (CryptCreateHash(g_prov, CALG_MD5, 0, 0, &h)) {
        BYTE dig[16];
        DWORD n = sizeof dig;
        if (CryptHashData(h, (const BYTE*)s, (DWORD)strlen(s), 0) && CryptGetHashParam(h, HP_HASHVAL, dig, &n, 0)) {
            for (int i = 0; i < 16; i++) sprintf_s(out + 2 * i, 3, "%02x", dig[i]);
            ok = true;
        }
        CryptDestroyHash(h);
    }
    return ok;
}
// The data mod serves Win/<name> from DataMod\<md5 of that path>: its file names, read once.
static std::unordered_set<std::string>* g_dataMod;
static bool InDataMod(const char* name)
{
    if (!g_dataMod) {
        g_dataMod = new std::unordered_set<std::string>();
        char dir[MAX_PATH], pat[MAX_PATH];
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* s = strrchr(dir, '\\')) *s = 0;
        sprintf_s(pat, "%s\\DataMod\\*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string n = fd.cFileName;
                    for (char& c : n) c = (char)tolower((unsigned char)c);
                    g_dataMod->insert(n);
                }
            } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
    }
    char path[160], md5[33];
    sprintf_s(path, "Win/%s", name);
    return Md5Hex(path, md5) && g_dataMod->count(md5) > 0;
}

static void Add(int cls, const char* from, const char* to)
{
    if (!g_files[cls]) g_files[cls] = (ClassFiles*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ClassFiles));
    ClassFiles* f = g_files[cls];
    if (f->n >= 48) return;
    strcpy_s(f->s[f->n].from, from);
    strcpy_s(f->s[f->n].to, to);
    f->n++;
}

// For each extra class (from classes.cpp, as its line is read): which of its own files the data
// mod has.
void ClassFilesSetup(int cls, int base, const char* body)
{
    if (cls < 6 || cls >= MAXC || !body || base < 0 || base > 5) return;
    char x[96];
    strcpy_s(x, body);
    char* p = strstr(x, "_cb00.osb");
    if (!p) return;
    *p = 0;
    char name[128], orig[32];
    sprintf_s(name, "%s_ef00.osb", x);
    sprintf_s(orig, "pl%d_ef00.osb", base);
    if (InDataMod(name)) Add(cls, orig, name);
    // weapon files of every class (a class borrows others' weapons: the Bandit's are the Empress's
    // daggers and the Shinobi's throwing weapons)
    static const char* const kinds[] = {"cw", "tw"};
    for (int c = 0; c < 6; c++)
        for (const char* k : kinds)
            for (int i = 0; i < 32; i++) {
                sprintf_s(orig, "pl%d_%s%02d.osb", c, k, i);
                sprintf_s(name, "%s_%s", x, orig);
                if (InDataMod(name)) Add(cls, orig, name);
            }
    if (g_files[cls]) {
        Log("classes: class %d has %d file(s) of its own:", cls, g_files[cls]->n);
        for (int i = 0; i < g_files[cls]->n; i++) Log("  %s for %s", g_files[cls]->s[i].to, g_files[cls]->s[i].from);
    }
}

// After the class's quest preload: its own files are queued too.
void ClassFilesPreloaded(int cls, void* loader)
{
    if (cls < 6 || cls >= MAXC || !g_files[cls] || !loader) return;
    __try {
        for (int i = 0; i < g_files[cls]->n; i++) {
            uint8_t* str = (uint8_t*)calloc(1, 0x80);  // the game's string object (kept: tiny)
            ((void(__fastcall*)(void*, void*, const char*))Game(STRING_CTOR))(str, nullptr, g_files[cls]->s[i].to);
            ((void(__fastcall*)(void*, void*, void*))Game(LOADER_ADD))(loader, nullptr, str);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool IsPlayer(uint8_t* a)
{
    uintptr_t vt = *(uintptr_t*)a;
    for (uint32_t v : PLAYER_VTABLES)
        if (vt == (uintptr_t)Game(v)) return true;
    return false;
}
static uint8_t* PlayerOf(uint8_t* actor)
{
    if (!actor) return nullptr;
    if (IsPlayer(actor)) return actor;
    if (*(int*)(actor + 0x18) == 3) {  // attack object (shot, slash): its player
        uint8_t* p = *(uint8_t**)(actor + 0x668);
        if (p && IsPlayer(p)) return p;
    }
    return nullptr;
}

static volatile LONG g_swapLogs, g_otherLogs;
// The class's own file for a sprite this actor asks for (null = none): from h_makeSprite.
const char* ClassFileFor(void* actor, const char* file)
{
    __try {
        if (!file || file[0] != 'p' || file[1] != 'l') return nullptr;
        uint8_t* pl = PlayerOf((uint8_t*)actor);
        if (!pl) {
            if (strstr(file, "_cw") && InterlockedIncrement(&g_otherLogs) <= 5)
                Log("classes: weapon file %s asked for by an actor that isn't a player (vtable %p)", file,
                    (void*)(*(uintptr_t*)actor - (uintptr_t)Game(0x400000) + 0x400000));
            return nullptr;
        }
        int c = pl[0xF5C];
        if (c < 6 || c >= MAXC || !g_files[c]) return nullptr;
        for (int i = 0; i < g_files[c]->n; i++)
            if (!_stricmp(file, g_files[c]->s[i].from)) {
                if (InterlockedIncrement(&g_swapLogs) <= 10) Log("classes: %s in place of %s", g_files[c]->s[i].to, file);
                return g_files[c]->s[i].to;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

bool ClassFilesInit()
{
    int n = 0;
    for (int c = 6; c < MAXC; c++) n += g_files[c] ? 1 : 0;
    if (n) Log("classes: %d class(es) with own effect / weapon files", n);
    return true;
}
