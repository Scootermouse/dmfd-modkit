// Extra classes (prototype), from <game>\DataMod\classes.txt.
//
// The game has six classes (0-5) and keeps per-class lists that it indexes by
// class number, mostly without a bounds check. For extra classes (6 and up)
// we give the game longer lists of our own by repointing every instruction
// that reads them (SITES below, from a full disassembly). The game is loaded
// at a random base, so each instruction is checked (opcode bytes + relocated
// table address) before it is changed; if any differs, nothing is changed.
//
// Lists (all six entries long in the game, in class order):
//   TYPE    0x111A1D4  type-registry object that creates the player (0xDE7B90)
//   PRELOAD 0x111A1A4  function loading a class's files from its setup block
//   FUNC_A  0x111A174  another per-class function run after the preload
//   NUM_A   0x111A18C  per-class number (0,1,4,1,3,6)
//   NUM_B   0x111A1BC  per-class number (all 0x3e)
//   PAL     0x111A15C  body palette file
//   BODY    0x1112F90  body sprite file  (in 0xD79D80, bounds "class <= 5" raised,
//   WEAPON  0x1112FA8  default weapon     and in its wrapper 0xD79E30)
// An extra class copies its base class's entries, except body and palette.
//
// classes.txt, one class per line ('#' comments):
//   6 base=0 body=pl6_cb00.osb palette=pl6_pal.osb [test_from=0]
// base = existing class whose moves (C++ player object) it uses.
//
// test_from=N (prototype only): class N plays as this class. The class is
// changed wherever a player's setup block is loaded, including the save in
// memory, so while it is on every write to SystemData.bin is redirected to
// SystemData.bin.class-test: the real save is never written.
#include "content.h"
#include "MinHook.h"
#pragma comment(lib, "user32.lib")
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <intrin.h>

static const int MAX_CLASSES = 128;  // class numbers are stored in signed bytes

struct Site { uint32_t va; int opLen; const char* op; uint32_t table; };
#include "classes_sites.inc"
#include "fresh_classes.inc"  // FRESH[k], FRESH_LEN[k]: the six classes as a new contract holds them (tools/make_fresh_templates.py)  // CLASS_TABLES[] and SITES[] (generated from a disassembly, see docs/seventh-class-limits.md)

static const uint32_t BODY_TABLE = 0x1112F90, PAL_TABLE = 0x111A15C;
static const int NTAB = sizeof CLASS_TABLES / sizeof CLASS_TABLES[0];
static const uint32_t SPRITE_CMP = 0xD79D91, SPRITE_WRAP_CMP = 0xD79E73;  // cmp esi,5 / cmp edi,5
static const uint32_t SETUP_CHECK = 0xC928B0;  // setup-block validator: cmp byte [ecx],5 (class), then colour/voice <= 7, level 1..100
static const uint32_t INFO_BUILD = 0xD86F60;  // builds the player-info package (see h_info)
static const uint32_t SAVE_LOAD = 0x8EB310;   // thiscall(obj, decrypted file buffer), ret 4; save = [[obj+0x1C]+4]
static const uint32_t CLASS_RECORD = 0xC8F4B0;  // a class's record in the current contract (see h_record)
static const uint32_t SAVE_CRYPT = 0xE81590;  // (ecx = key, edx = data, [esp+4] = length), caller cleans

static uintptr_t g_tab[NTAB][MAX_CLASSES];  // our longer copy of each table

// Diagnostics: every class file preload goes through h_preloadLog (our table's entries
// point at it), which logs class, mode (0 = full with effects, 1 = light) and caller.
using PreloadFn = void(__cdecl*)(void*, uint8_t*, int);
static PreloadFn g_preloadOrig[MAX_CLASSES];
static void __cdecl h_preloadLog(void* loader, uint8_t* block, int arg);

// palette pair table (see ClassesInit): {hash of palette file, hash of "<body>.osp"} per class
static uint32_t g_pair[MAX_CLASSES][2];
static const uint32_t OSP_NAME[6] = {0x1113A5C, 0x1113AA0, 0x1113A84, 0x1113AD8, 0x1113ABC, 0x1113B0C};  // pl0_cb00.osp .. pl5_pal.osp
struct PairSite { uint32_t va; const char* op; uint32_t table; bool second; };
static const PairSite PAIR_SITES[] = {
    {0xD66B7F, "8b04dd", 0x28ED298, false}, {0xD66BEF, "8b04dd", 0x28ED298, false}, {0xD66C18, "ff34dd", 0x28ED29C, true},
};

static uint32_t IceHash(const char* s)
{
    uint32_t h = 0xCDE723A5;
    for (; s && *s; s++) h = (h + (uint32_t)(int)(signed char)*s) * 0x8D;
    return h;
}

static int TableIndex(uint32_t va)
{
    for (int i = 0; i < NTAB; i++)
        if (CLASS_TABLES[i] == va) return i;
    return -1;
}

static uint8_t* Game(uint32_t va) { return (uint8_t*)GetModuleHandleA(nullptr) + va - 0x400000; }
static uint32_t Rel(uint32_t va) { return (uint32_t)(uintptr_t)Game(va); }

static bool SiteOk(const Site& s)
{
    return CodeAt(s.va, s.op) && *(uint32_t*)(Game(s.va) + s.opLen) == Rel(s.table);
}

static void Write(uint32_t va, const void* src, size_t n)
{
    DWORD old;
    uint8_t* p = Game(va);
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, src, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

bool ClassesReady()
{
    // The key sites must match exactly (else this is another game version);
    // other sites that don't match are skipped one by one in ClassesInit.
    static const uint32_t KEY[] = {0xDE7BA6, 0xBC52B2, 0xD79E1D, 0xE3500B};
    for (uint32_t k : KEY)
        for (const Site& s : SITES)
            if (s.va == k && !SiteOk(s)) return false;
    return CodeAt(SPRITE_CMP, "83fe05") && CodeAt(SPRITE_WRAP_CMP, "83ff05") &&
           CodeAt(INFO_BUILD, "558bec6aff68") && CodeAt(SAVE_LOAD, "558bec83ec0856578b7d") &&
           CodeAt(SAVE_CRYPT, "558bec83e4f883ec0c53") && CodeAt(CLASS_RECORD, "558bec837d0806") &&
           CodeAt(0xDAA420, "538bdc83ec08") && CodeAt(0xAB8740, "558bec83ec20") &&
           CodeAt(0xAB90C0, "558bec8b4508568b") && CodeAt(0xABE2D0, "558bec64a12c000000") &&
           CodeAt(0xE34FD0, "558bec6aff68") && CodeAt(0xD79E30, "558bec6aff68") && CodeAt(0xE4EA70, "558becf30f104514") &&
           CodeAt(0xA92060, "558bec6aff68") && CodeAt(0xABD73A, "e8") &&
           CodeAt(0xD82EB0, "558bec83ec08") && CodeAt(0xDB9110, "558bec6aff68") &&
           CodeAt(0xC90BE0, "8b814c440000") && CodeAt(0xC922C0, "558bec5156") && CodeAt(0xDB985B, "e8") &&
           CodeAt(0xDB9844, "e8") && CodeAt(0xDB9715, "e8") &&
           CodeAt(0xAF2A70, "558bec83e4f8") && CodeAt(0xAF3090, "558bec6aff68") &&
           CodeAtOrHooked(0xC92540, "558bec518b4508") && CodeAt(0xCCCD80, "64a12c000000") && CodeAt(0xAB92A0, "558bec") &&  CodeAt(0xAAE820, "558bec6aff68") && CodeAt(0xAD8A09, "e8") &&
           CodeAt(SETUP_CHECK, "803905");
}

// ---- extra classes on select-screen pages, and their save data ----
//
// The select screen has six panels (classes 0-5). Extra classes live on more
// pages: page p shows class 6p+i on panel i (class 6 behind the Empress panel,
// 7 behind the Warrior, ...). Page Up/Page Down or LT/RT on the character select screen
// change page.
//
// Turning a page doesn't touch the screen code: it swaps save data. The save
// (SystemData.bin; in memory it keeps the file's layout, see FORMAT.md) has
// three contracts, each with six 0x5C-byte class records in section B (class
// order), each pointing (record-relative offsets) to its class's data blocks.
// For every panel whose page class is an extra class, the bytes that belong to
// that panel's base class (record name area, field 2, and the nine blocks the
// record points to; each block runs up to the next block of any class) hold the
// extra class's bytes instead, and the base class's bytes are kept aside. The
// setup block's first byte is the class number, so the swapped slot says 6, 7,
// ... and the game's file loader, palette and player creation follow it.
//
// The game copies the save between buffers, so swaps are (re)applied when the
// game reads a class record (0xC8F4B0) from a save we haven't seen. When the
// game writes the save, the outgoing copy gets every base class's bytes back
// (checksum recomputed) and the extra classes' bytes go to
// SystemData.bin.classes next to the save. The game's own save never holds an
// extra class.

static const uint32_t SAVE_MAGIC = 0xAE9F2304;
static const int REC = 0x5C, NCON = 3, MAXR = 12, MAXBLOB = 0x1000;
static const int OFFSET_FIELDS[] = {0, 1, 3, 4, 5, 6, 7, 8, 9};  // record +0x34 + 4*f; field 2 is a number
static const uint32_t SELECT_STEP = 0xDAA420;  // character-select step (thiscall, 2 args, ret 8), runs every frame on that screen

struct Range { uint32_t at, len; };  // offset within section B
struct Slot {
    Range r[MAXR];
    int nr;
    uint32_t len, setupAt;
    int occupant;             // class whose bytes are in this slot (base class or an extra class)
    uint8_t stash[MAXBLOB];   // the base class's bytes while an extra class occupies the slot
    int createdBase;          // 1 + the base class's "created" bit while an extra class occupies the slot (0 = none kept)
    bool stashed;             // stash holds real base class bytes (never scatter an unfilled stash: all zeros
                              // = an Empress with a blank name)
};
struct ExtraData {            // an extra class's bytes, per contract
    bool have[NCON];
    bool created[NCON];       // made through character creation (the slot's "created" bit while it's there)
    uint32_t len[NCON];       // bytes in blob (kept from the file until this contract's layout is known)
    uint8_t blob[NCON][MAXBLOB];
};
static Slot g_slot[NCON][6];
static ExtraData* g_extra[MAX_CLASSES];  // only for defined extra classes
static bool g_defined[MAX_CLASSES];
static int g_baseOf[MAX_CLASSES];
static int g_page, g_pages = 1;
static uint8_t* g_lastMem;
static void* g_game;  // the game object (save = SaveOf(g_game))
static wchar_t g_sidecar[MAX_PATH];      // <save folder>\SystemData.bin.classes
static bool g_redirect = false;          // test_saves=1: game save writes go to SystemData.bin.class-test
static int g_bindMap[6] = {0, 1, 2, 3, 4, 5};  // panel class -> class drawn (palette binding)

static uint32_t U(const uint8_t* p) { return *(const uint32_t*)p; }

static uint16_t SaveChecksum(const uint8_t* d, uint32_t n)  // the game's 0xBD3B80
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c += d[i];
        c = ((c >> 16) | c) & 0xFFFF;
    }
    return (uint16_t)c;
}

// Class k's byte ranges in a section B (b = its start, bsize = its size).
static bool ClassRanges(const uint8_t* b, uint32_t bsize, int k, Slot& s)
{
    uint32_t starts[6 * 10 + 1];
    int n = 0;
    for (int j = 0; j < 6; j++) {
        starts[n++] = j * REC;
        for (int f : OFFSET_FIELDS) {
            uint32_t a = j * REC + U(b + j * REC + 0x34 + 4 * f);
            if (a < 6 * REC || a >= bsize) return false;
            starts[n++] = a;
        }
    }
    starts[n++] = bsize;
    s.nr = 0;
    s.r[s.nr++] = {(uint32_t)k * REC, 0x34};         // name area
    s.r[s.nr++] = {(uint32_t)k * REC + 0x3C, 4};     // field 2 (a number)
    uint32_t total = 0x38;
    for (int f : OFFSET_FIELDS) {
        uint32_t a = k * REC + U(b + k * REC + 0x34 + 4 * f), end = bsize;
        for (int i = 0; i < n; i++)
            if (starts[i] > a && starts[i] < end) end = starts[i];
        if (f == 0) s.setupAt = a;
        s.r[s.nr++] = {a, end - a};
        total += end - a;
    }
    s.len = total;
    return total <= MAXBLOB;
}

static void Gather(const uint8_t* b, const Slot& s, uint8_t* out)
{
    for (int i = 0; i < s.nr; i++) {
        memcpy(out, b + s.r[i].at, s.r[i].len);
        out += s.r[i].len;
    }
}

static void Scatter(uint8_t* b, const Slot& s, const uint8_t* in)
{
    for (int i = 0; i < s.nr; i++) {
        memcpy(b + s.r[i].at, in, s.r[i].len);
        in += s.r[i].len;
    }
}

// ---- SystemData.bin.classes: {u32 'DMCX', u32 version, u32 contracts, u32 count} then per class:
//   version 5: {u32 id length, id bytes (the class's unique name from classes.txt id=)}
//   version 3/4: {u32 class number} (old builds; matched by classes.txt was=, else by number)
// then per contract {u32 length, [version 4+: u32 flags (bit 0 created)], bytes}.
// Characters of classes that aren't installed now are kept as they are and written back, so
// switching a class mod off (or renumbering) never loses them. ----
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileWFn o_cfw;
static const uint32_t SIDECAR_MAGIC = 0x58434D44, SIDECAR_VERSION = 5;

static volatile LONG g_sidecarRead;  // the file was read (or there was none to read)
static const char* g_classId[MAX_CLASSES];  // classes.txt id= (unique name)
static int g_classWas[MAX_CLASSES];         // classes.txt was= (its number in older builds), 0 = none
static std::string g_orphans;               // version 5 entries of classes not installed now, as read
static uint32_t g_orphanCount;

static std::string IdOf(int cls)
{
    if (g_classId[cls] && *g_classId[cls]) return g_classId[cls];
    char b[16];
    sprintf_s(b, "old_%d", cls);  // no id (an old classes.txt): keyed by number
    return b;
}

static void WriteSidecar()
{
    if (!g_sidecar[0]) return;
    // Never replace a class save file we haven't read: it would lose the extra characters in it.
    if (!g_sidecarRead && GetFileAttributesW(g_sidecar) != INVALID_FILE_ATTRIBUTES) {
        Log("classes: SystemData.bin.classes was never read this session; left as it is");
        return;
    }
    wchar_t tmp[MAX_PATH + 8];
    swprintf_s(tmp, L"%s.tmp", g_sidecar);
    HANDLE h = o_cfw(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("classes: could not write the class save file");
        return;
    }
    DWORD w;
    uint32_t count = g_orphanCount;
    for (int c = 6; c < MAX_CLASSES; c++) count += g_extra[c] ? 1 : 0;
    uint32_t head[4] = {SIDECAR_MAGIC, SIDECAR_VERSION, NCON, count};
    WriteFile(h, head, sizeof head, &w, nullptr);
    if (!g_orphans.empty()) WriteFile(h, g_orphans.data(), (DWORD)g_orphans.size(), &w, nullptr);
    for (int cls = 6; cls < MAX_CLASSES; cls++) {
        if (!g_extra[cls]) continue;
        std::string id = IdOf(cls);
        uint32_t idLen = (uint32_t)id.size();
        WriteFile(h, &idLen, 4, &w, nullptr);
        WriteFile(h, id.data(), idLen, &w, nullptr);
        for (int c = 0; c < NCON; c++) {
            // the slot's length once this contract was read; until then the length the file had
            // (writing 0 would drop the character)
            uint32_t slotLen = g_slot[c][(cls - 6) % 6].len;
            uint32_t len = !g_extra[cls]->have[c] ? 0 : slotLen ? slotLen : g_extra[cls]->len[c];
            WriteFile(h, &len, 4, &w, nullptr);
            if (!len) continue;
            uint32_t flags = g_extra[cls]->created[c] ? 1 : 0;
            WriteFile(h, &flags, 4, &w, nullptr);
            WriteFile(h, g_extra[cls]->blob[c], len, &w, nullptr);
        }
    }
    CloseHandle(h);
    MoveFileExW(tmp, g_sidecar, MOVEFILE_REPLACE_EXISTING);
}

static void ReadSidecar()
{
    if (!g_sidecar[0]) return;
    HANDLE h = o_cfw(g_sidecar, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD r;
    uint32_t head[4];
    int loaded = 0, kept = 0;
    g_orphans.clear();
    g_orphanCount = 0;
    if (ReadFile(h, head, sizeof head, &r, nullptr) && r == sizeof head && head[0] == SIDECAR_MAGIC &&
        head[1] >= 3 && head[1] <= SIDECAR_VERSION && head[2] == NCON) {
        if (head[1] < 5) {  // an older file: keep a copy before it's ever rewritten in the new format
            wchar_t bak[MAX_PATH + 32];
            swprintf_s(bak, L"%s.before-v5", g_sidecar);
            if (CopyFileW(g_sidecar, bak, TRUE)) Log("classes: old class save backed up to SystemData.bin.classes.before-v5");
        }
        for (uint32_t i = 0; i < head[3]; i++) {
            std::string id;
            int cls = -1;
            if (head[1] >= 5) {
                uint32_t idLen;
                if (!ReadFile(h, &idLen, 4, &r, nullptr) || r != 4 || idLen > 200) break;
                id.resize(idLen);
                if (idLen && (!ReadFile(h, &id[0], idLen, &r, nullptr) || r != idLen)) break;
                for (int k = 6; k < MAX_CLASSES && cls < 0; k++)
                    if (g_extra[k] && IdOf(k) == id) cls = k;
            } else {
                uint32_t num;
                if (!ReadFile(h, &num, 4, &r, nullptr) || r != 4) break;
                for (int k = 6; k < MAX_CLASSES && cls < 0; k++)  // its old number, as classes.txt remembers it
                    if (g_extra[k] && g_classWas[k] == (int)num) cls = k;
                if (cls < 0 && num < MAX_CLASSES && g_extra[num] && !g_classWas[num]) cls = num;
                char b[16];
                sprintf_s(b, "old_%u", num);
                id = cls >= 0 ? IdOf(cls) : b;
            }
            // an entry for a class that isn't installed is kept whole (as a version 5 entry)
            std::string orphan;
            if (cls < 0) {
                uint32_t idLen = (uint32_t)id.size();
                orphan.append((const char*)&idLen, 4).append(id);
            }
            bool bad = false;
            for (int c = 0; c < NCON && !bad; c++) {
                uint32_t len;
                if (!ReadFile(h, &len, 4, &r, nullptr) || r != 4 || len > MAXBLOB) { bad = true; break; }
                uint32_t flags = 1;  // version 3 files: every saved class was made already
                if (len && head[1] >= 4 && (!ReadFile(h, &flags, 4, &r, nullptr) || r != 4)) { bad = true; break; }
                if (cls >= 0 && len) {
                    if (ReadFile(h, g_extra[cls]->blob[c], len, &r, nullptr) && r == len) {
                        g_extra[cls]->have[c] = true;
                        g_extra[cls]->created[c] = (flags & 1) != 0;
                        g_extra[cls]->len[c] = len;
                    } else { bad = true; break; }  // out of step from here: stop rather than misread
                } else if (cls < 0) {
                    orphan.append((const char*)&len, 4);
                    if (len) {
                        std::string blob(len, '\0');
                        if (!ReadFile(h, &blob[0], len, &r, nullptr) || r != len) { bad = true; break; }
                        orphan.append((const char*)&flags, 4).append(blob);
                    }
                }
            }
            if (bad) break;
            if (cls >= 0) loaded++;
            else {
                g_orphans += orphan;
                g_orphanCount++;
                kept++;
            }
        }
    }
    CloseHandle(h);
    if (loaded || kept)
        Log("classes: read SystemData.bin.classes (%d class(es)%s)", loaded,
            kept ? (", plus characters of " + std::to_string(kept) + " class(es) not installed now, kept").c_str() : "");
}

// The save's path comes from the game opening it (h_cfw), which can happen before or after
// the extra classes are set up (ClassesInit); the file is read once both have happened.
static volatile bool g_extraReady;

static void ReadSidecarOnce()
{
    if (!g_sidecar[0] || !g_extraReady) return;
    if (InterlockedExchange(&g_sidecarRead, 1) == 0) {
        ReadSidecar();
        if (GetFileAttributesW(g_sidecar) == INVALID_FILE_ATTRIBUTES) Log("classes: no SystemData.bin.classes yet");
    }
}

// Put the right class's bytes into each slot of one contract, for the current page.
// An extra class's first time in a contract: it starts as a brand-new character (level 1, the
// starting gear and default colour of its slot's class), never as a copy of the player's own
// character, and not yet created: the select screen shows NEW and runs the game's own colour,
// voice and name screens. The slot's "created" bit (section A byte 2, a bit per slot) follows
// the class in the slot; the base class's own bit comes back with its bytes.
static void FreshExtra(int cls, int c, int k, const Slot& s, uint8_t* out)
{
    if (FRESH_LEN[k] == s.len) {
        memcpy(out, FRESH[k], s.len);
        Log("classes: class %d starts fresh in contract %d (new character, level 1)", cls, c + 1);
    } else {
        memcpy(out, s.stash, s.len);
        Log("classes: class %d: no fresh template fits slot %d (%u bytes, template %u); copied the slot's character",
            cls, k, s.len, FRESH_LEN[k]);
    }
}

// Before the base class's bytes go back into slot k: if they were never put aside this session
// (the save on disk already held an extra class there), use a brand-new base character rather
// than the empty stash, which would come back as an Empress with a blank name.
static void EnsureStash(int c, int k, Slot& s)
{
    if (s.stashed) return;
    if (FRESH_LEN[k] == s.len) {
        memcpy(s.stash, FRESH[k], s.len);
        s.createdBase = 1;  // not created: the game offers to make the character again
        Log("classes: contract %d slot %d: the base class's own character wasn't found (the save held an extra "
            "class there); it starts as a new character", c + 1, k);
    } else {  // no template fits: keep the slot as it is rather than write zeros
        Log("classes: contract %d slot %d: the base class's own character wasn't found and no template fits; "
            "slot left as it is", c + 1, k);
        return;
    }
    s.stashed = true;
}

static void ApplyPage(uint8_t* mem, int c)
{
    const uint8_t* entry = mem + 0x68 + 0xC * c;
    uint32_t B = U(entry + 4), bsize = U(entry + 8) - B;
    uint8_t* b = mem + B;
    uint8_t* created = mem + U(entry) + 2;
    if (bsize < 0x400 || bsize > 0x10000) return;
    for (int k = 0; k < 6; k++) {
        Slot& s = g_slot[c][k];
        if (!ClassRanges(b, bsize, k, s)) {
            Log("classes: contract %d: save layout not as expected; extra classes not loaded", c + 1);
            return;
        }
        // a contract nobody has started (section A byte 0) and isn't the one being chosen
        // keeps the base classes, so the title's contract list shows it as the game left it
        bool idle = c != (mem[0x40] < NCON ? mem[0x40] : 0) && mem[U(entry)] == 0;
        int want = !idle && g_page > 0 && g_defined[6 * g_page + k] ? 6 * g_page + k : k;
        int now = b[s.setupAt];
        if (now == want) {
            s.occupant = want;
            if (want >= 6) EnsureStash(c, k, s);  // already there on load: nothing was put aside
            continue;
        }
        if (idle && now != k && (now < 6 || !g_extra[now])) {  // never set up (a fresh install's spare contracts hold zeros)
            s.occupant = k;
            continue;
        }
        if (now != k && (now < 6 || !g_extra[now])) {
            Log("classes: contract %d slot %d holds class %d unexpectedly; left alone", c + 1, k, now);
            continue;
        }
        if (now != k) {  // put the extra class's bytes away, the base class's back
            Gather(b, s, g_extra[now]->blob[c]);
            g_extra[now]->have[c] = true;
            g_extra[now]->created[c] = (*created >> k & 1) != 0;
            g_extra[now]->len[c] = s.len;
            EnsureStash(c, k, s);
            if (!s.stashed) { s.occupant = now; continue; }
            Scatter(b, s, s.stash);
            if (s.createdBase) *created = (uint8_t)((*created & ~(1 << k)) | ((s.createdBase - 1) << k));
            s.createdBase = 0;
            s.stashed = false;
        }
        if (want != k) {
            Gather(b, s, s.stash);
            s.stashed = true;
            ExtraData* e = g_extra[want];
            if (!e->have[c]) {
                FreshExtra(want, c, k, s, e->blob[c]);
                e->have[c] = true;
                e->created[c] = false;
            }
            Scatter(b, s, e->blob[c]);
            b[s.setupAt] = (uint8_t)want;
            s.createdBase = 1 + (*created >> k & 1);
            *created = (uint8_t)((*created & ~(1 << k)) | ((e->created[c] ? 1 : 0) << k));
        }
        s.occupant = want;
    }
}

// Lists the game reads by slot (the current class is a slot, 0-5) rather than by class:
// class names (CLASS_<nn>: title info box, inn, Status menu) and the HUD sheets
// (sub_quest_13xx, sub_status_13xx). Their six slot entries follow the classes now in
// the slots; an extra class's own entry is set from classes.txt.
static const uint32_t CLASS_NAME_TABLE = 0x11022C4;
static const uint32_t SLOT_TABLES[] = {CLASS_NAME_TABLE, 0x1033D0C, 0x1117294, 0x1033D24};
static const int NSLOT_TABLES = sizeof SLOT_TABLES / sizeof SLOT_TABLES[0];
static uintptr_t g_slotOrig[NSLOT_TABLES][6];
static bool g_slotOrigSaved;

static void SlotTables(uint8_t* mem)
{
    if (!g_slotOrigSaved) {
        for (int i = 0; i < NSLOT_TABLES; i++) {
            int t = TableIndex(SLOT_TABLES[i]);
            for (int k = 0; k < 6; k++) g_slotOrig[i][k] = t >= 0 ? g_tab[t][k] : 0;
        }
        g_slotOrigSaved = true;
    }
    int cur = mem[0x40] < NCON ? mem[0x40] : 0;
    for (int i = 0; i < NSLOT_TABLES; i++) {
        int t = TableIndex(SLOT_TABLES[i]);
        if (t < 0) continue;
        for (int k = 0; k < 6; k++) {
            int c = g_slot[cur][k].nr ? g_slot[cur][k].occupant : k;
            g_tab[t][k] = c >= 6 && c < MAX_CLASSES ? g_tab[t][c] : g_slotOrig[i][k];
        }
    }
}

static void ApplyPageAll(uint8_t* mem)
{
    __try {
        for (int c = 0; c < NCON; c++) ApplyPage(mem, c);
        g_lastMem = mem;
        int cur = mem[0x40] < NCON ? mem[0x40] : 0;
        for (int k = 0; k < 6; k++) g_bindMap[k] = g_slot[cur][k].occupant;
        SlotTables(mem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not read the save in memory; extra classes not loaded");
    }
}

static uint8_t* SaveOf(void* game)
{
    return *(uint8_t**)(*(uint8_t**)(*(uint8_t**)((uint8_t*)game + 0x444C) + 0x1C) + 4);
}

// The game object the way the game's own code finds it: 0x7C9770 (no args)
// gives a manager, 0xD83C00 (thiscall on it) the game object; its save is
// SaveOf(game). Used before the game has read a class record.
static void* FindGame()
{
    if (g_game) return g_game;
    __try {
        void* m = ((void*(__cdecl*)())Game(0x7C9770))();
        if (m) g_game = ((void*(__fastcall*)(void*, void*))Game(0xD83C00))(m, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return g_game;
}

// 0xC8F4B0 returns a class's record of the current contract (thiscall(game,
// class), ret 4). Everything that reads class data goes through it, on the
// game's thread: apply the page to a save we haven't seen (a reload or a copy).
using RecordFn = uint8_t*(__fastcall*)(void*, void*, int);
static RecordFn o_record;
static uint8_t* __fastcall h_record(void* game, void* edx, int cls)
{
    __try {
        g_game = game;
        uint8_t* mem = SaveOf(game);
        if (mem != g_lastMem) ApplyPageAll(mem);
        int cur = mem[0x40] < NCON ? mem[0x40] : 0;
        for (int k = 0; k < 6; k++) g_bindMap[k] = g_slot[cur][k].nr ? g_slot[cur][k].occupant : k;
        // Asked for an extra class by its own number (the select preview does): the game
        // answers class 6+ with record 0 (the Empress's: her weapon in the preview). Its
        // record is the slot it sits in on its page.
        if (cls >= 6 && cls < MAX_CLASSES && g_slot[cur][cls % 6].nr && g_slot[cur][cls % 6].occupant == cls)
            cls = cls % 6;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return o_record(game, edx, cls);
}

// The outgoing copy of the save, just before its first encryption pass:
// every base class's bytes go back in, the extra classes go to our file.
extern "C" void __cdecl OnSaveEncrypt(const char* key, uint8_t* data, uint32_t len)
{
    __try {
        if (!key || strncmp(key, "zZ2c9VTK", 8) || len < 0x1000 || U(data - 0x10) != SAVE_MAGIC) return;
        uint8_t* file = data - 0x10;  // offsets in the save count from the file start
        bool any = false;
        for (int c = 0; c < NCON; c++) {
            uint8_t* b = file + U(file + 0x68 + 0xC * c + 4);
            for (int k = 0; k < 6; k++) {
                Slot& s = g_slot[c][k];
                int occ = s.occupant;
                if (occ < 6 || !s.nr) continue;
                if (b[s.setupAt] != occ) {  // the game reset this contract: nothing of ours in it
                    s.occupant = k;
                    continue;
                }
                Gather(b, s, g_extra[occ]->blob[c]);
                g_extra[occ]->have[c] = true;
                g_extra[occ]->len[c] = s.len;
                EnsureStash(c, k, s);
                if (!s.stashed) continue;
                Scatter(b, s, s.stash);
                if (s.createdBase) {  // the file keeps the base class's own "created" bit
                    uint8_t* created = file + U(file + 0x68 + 0xC * c) + 2;
                    g_extra[occ]->created[c] = (*created >> k & 1) != 0;
                    *created = (uint8_t)((*created & ~(1 << k)) | ((s.createdBase - 1) << k));
                }
                any = true;
            }
        }
        if (any) *(uint32_t*)(file + 8) = SaveChecksum(data, len);  // header +8: checksum of the plain data
        WriteSidecar();
        static int logged;
        if (logged++ < 3) Log("classes: save written with the six classes' own data; extra classes saved to SystemData.bin.classes");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: error while preparing the save; it was written unchanged");
    }
}

static void* o_crypt;
__declspec(naked) static void h_crypt()
{
    __asm {
        pushad
        push dword ptr [esp + 36]  // length (the caller's first stack argument)
        push edx                   // data
        push ecx                   // key
        call OnSaveEncrypt
        add esp, 12
        popad
        jmp o_crypt
    }
}

// 0xD86F60 builds the player-info package (class at +8) from the save; the
// player is created from it (0xDE7B90). The save's active class is a panel
// (0-5); the class really in that slot is its occupant.
static bool g_logBinds = false;  // diagnostics: log palette binds, palette loads and info builds

static bool RawBodies(bool on);  // the body-name redirect off (true) / back on (false)
static bool FromSelectSetup(uintptr_t from);

static void __cdecl h_preloadLog(void* loader, uint8_t* block, int arg)
{
    int cls = block ? block[0] : 0;
    uintptr_t from = (uintptr_t)_ReturnAddress();
    if (g_logBinds)
        Log("preload class %d (%s) from %p", cls, arg ? "light" : "full", (void*)(from - (uintptr_t)Game(0)));
    // The select screen loads the six base classes' files itself. They must be the base
    // classes' own bodies whatever page it opens on: redirected (slot -> class in it), a
    // screen opened on page 2+ for a new contract loaded only the extra classes' bodies,
    // and page 1 drew invisible characters. The extra classes are queued separately.
    bool raw = cls < 6 && FromSelectSetup(from) && RawBodies(true);
    g_preloadOrig[cls < MAX_CLASSES ? cls : 0](loader, block, arg);
    if (raw) RawBodies(false);
    // its own effect / weapon files too, in the full (quest) preload only: added to the inn's light
    // preload they mixed up its character preview
    if (cls >= 6 && !arg) ClassFilesPreloaded(cls, loader);
}
using InfoFn = void(__fastcall*)(void*, void*, uint8_t*);
static InfoFn o_info;
static bool g_ownBuild;  // inside 0xD86F60: it binds OUR palette with the slot class (before we fix +8)
static void __fastcall h_info(void* self, void* edx, uint8_t* info)
{
    g_ownBuild = true;
    o_info(self, edx, info);
    g_ownBuild = false;
    if (g_logBinds && info) Log("info class %d (map -> %d) from %p", info[8], info[8] < 6 ? g_bindMap[info[8]] : info[8],
                               (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));
    if (info && info[8] < 6 && g_bindMap[info[8]] != info[8]) {
        info[8] = (uint8_t)g_bindMap[info[8]];
        static int logged;
        if (logged++ < 5) Log("classes: playing class %d", info[8]);
    }
}

// 0xD66730 binds a class's palette for drawing (class in edx, caller cleans the
// stack): a panel's class becomes the class in that slot. Online, a base class
// another player is using keeps its own palette (their record carries their real
// class; only our own slot lookups need the redirect), see net.cpp.
static const uint32_t PAL_BIND = 0xD66730;
static uint8_t g_remoteHas[6];

// Two base classes name their own body file directly instead of asking the body list:
// the Witch's dash afterimage (0xD27535, mov ecx, "pl3_cb00.osb") and a Shinobi effect
// (0xD2A1E4, push "pl2_cb00.osb"). An extra class has its own body file (the base's
// may not even be loaded), so both ask for the body of the player's class (player
// object +0xF5C), mapped like the body list (h_bodyNames).
// The code can run on one of the player's attack objects (cPshWork, actor type 3, e.g. the
// Shinobi's giant shuriken in flight), which keeps its player at +0x668: reading a class off
// the shuriken pointed at an Empress-based class's body, and the effect crashed. And the
// redirect only ever picks a class that borrows these moves (base = 3 Witch, 2 Shinobi).
static int g_bodyTab = -1;
static const char* __cdecl PlayerBody(uint8_t* obj, const char* orig, int base)
{
    __try {
        uint8_t* player = obj;
        if (*(int*)(obj + 0x18) == 3 && *(uint8_t**)(obj + 0x668)) player = *(uint8_t**)(obj + 0x668);
        int c = player[0xF5C];
        if (c != base) c = base;  // couldn't tell whose object it is: this code only runs for these moves
        if (!g_remoteHas[c]) c = g_bindMap[c];
        if (c >= 6 && c < MAX_CLASSES && g_defined[c] && g_baseOf[c] == base && g_bodyTab >= 0)
            return (const char*)g_tab[g_bodyTab][c];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig;
}
static const char* g_witchBody;    // the game's "pl3_cb00.osb"
static const char* g_shinobiBody;  // the game's "pl2_cb00.osb"
__declspec(naked) static void h_witchBody()  // replaces mov ecx, imm32 (edi = player; keep eax, edx)
{
    __asm {
        push eax
        push edx
        push 3             // the Witch
        push g_witchBody
        push edi
        call PlayerBody
        add esp, 12
        mov ecx, eax
        pop edx
        pop eax
        ret
    }
}
__declspec(naked) static void h_shinobiBody()  // replaces push imm32 (edi = player)
{
    __asm {
        pop edx            // return address
        push edx
        push 2             // the Shinobi
        push g_shinobiBody
        push edi
        call PlayerBody
        add esp, 12
        pop edx
        push eax           // the name the game pushed before
        jmp edx
    }
}

bool ClassDefined(int cls) { return cls >= 0 && cls < MAX_CLASSES && (cls < 6 || g_defined[cls]); }

void ClassesRemoteBase(const uint8_t has[6])
{
    for (int k = 0; k < 6; k++) {
        if (g_remoteHas[k] != has[k] && g_bindMap[k] != k)
            Log(has[k] ? "classes: another player is class %d; it keeps its own look (your class %d's redirect paused)"
                       : "classes: no other player is class %d now; redirect to class %d back on",
                k, g_bindMap[k]);
        g_remoteHas[k] = has[k];
    }
}
static void* o_palBind;
static void __cdecl LogBind(int asked, int drawn, uintptr_t from)
{
    static uint64_t seen[256];
    static int n;
    uint64_t key = (uint64_t)(asked << 8 | drawn) << 32 | (uint32_t)from;
    for (int i = 0; i < n; i++)
        if (seen[i] == key) return;
    if (n < 256) seen[n++] = key;
    Log("bind class %d -> %d from %p (page %d)", asked, drawn, (void*)(from - (uintptr_t)Game(0)), g_page);
}

static void __cdecl LogBindFull(int asked, int drawn, uintptr_t from, int colour, int element)
{
    Log("bind class %d -> %d colour %d element %d from %p (page %d)", asked, drawn, colour, element,
        (void*)(from - (uintptr_t)Game(0)), g_page);
}
__declspec(naked) static void h_palBind()
{
    __asm {
        push eax
        mov eax, edx
        cmp edx, 6
        jae pass
        cmp g_ownBuild, 0          // our own character: always the class in our slot
        jne map
        cmp byte ptr [g_remoteHas + edx], 0
        je map
        cmp dword ptr [g_bindMap + edx * 4], edx
        je pass
        pushad                     // another player's base class: keep it, and log where from (once each)
        push dword ptr [esp + 36]
        push edx
        push edx
        call LogBind
        add esp, 12
        popad
        jmp pass
    map:
        mov edx, dword ptr [g_bindMap + edx * 4]
    pass:
        cmp g_logBinds, 0
        je go
        pushad
        push dword ptr [esp + 44]      // element
        push dword ptr [esp + 44]      // colour
        push dword ptr [esp + 44]      // return address
        push edx
        push eax
        call LogBindFull
        add esp, 20
        popad
    go:
        pop eax
        jmp o_palBind
    }
}

// 0xD79E30 picks a class's body sprite names (class is its first stack argument,
// caller cleans). Screens that draw a character by slot (the quest results, status
// pictures) pass the slot's class; its palette bind above is redirected to the class
// in that slot, so the body must follow the same rule or the two don't match
// (palette for one body, the other body loaded: the character is invisible).
static const uint32_t BODY_NAMES = 0xD79E30;
static void* o_bodyNames;
static void __cdecl LogBody(int cls, uintptr_t from, int weapon)
{
    Log("body names class %d weapon %d from %p", cls, weapon, (void*)(from - (uintptr_t)Game(0)));
}
static int g_rawBodies;  // set while we queue the six classes' own files: no redirect
__declspec(naked) static void h_bodyNames()
{
    __asm {
        push eax
        mov eax, dword ptr [esp + 8]   // class
        cmp eax, 6
        jae go
        cmp g_rawBodies, 0
        jne go
        cmp byte ptr [g_remoteHas + eax], 0
        jne go
        mov eax, dword ptr [g_bindMap + eax * 4]
        mov dword ptr [esp + 8], eax
    go:
        cmp g_logBinds, 0
        je done
        pushad
        push dword ptr [esp + 44]      // weapon (second argument)
        push dword ptr [esp + 40]      // return address (after the push above)
        push dword ptr [esp + 48]      // class
        call LogBody
        add esp, 12
        popad
    done:
        pop eax
        jmp o_bodyNames
    }
}

// 0xAF6210 (thiscall(picture, class, weapon, ?, colour, element, weapon 2, scale), ret 0x1C)
// draws a character picture (inn and bar status panels, the title select preview). It keeps
// the class it last built at picture+0xC0 and rebuilds the body only when the class number
// changes; the palette (0xD66C80) is bound every time. The inn's status panel passes a slot,
// so turning a page from an Empress-based extra class to an Empress (both slot 0) kept the
// extra class's body under the Empress palette, and back. When the class really drawn for that number changes, the
// cached class is cleared so the body is built again.
static const uint32_t PICTURE = 0xAF6210;
using PictureFn = void(__fastcall*)(void*, void*, int, int, int, int, int, int, float);
static PictureFn o_picture;
static struct { void* pic; int drawn; } g_picDrawn[16];
static void __fastcall h_picture(void* pic, void* edx, int cls, int a2, int a3, int a4, int a5, int a6, float a7)
{
    __try {
        int drawn = cls >= 0 && cls < 6 && !g_rawBodies && !g_remoteHas[cls] ? g_bindMap[cls] : cls;
        int i = 0, freeAt = -1;
        for (; i < 16 && g_picDrawn[i].pic != pic; i++)
            if (!g_picDrawn[i].pic && freeAt < 0) freeAt = i;
        if (i < 16) {
            if (g_picDrawn[i].drawn != drawn && *(int*)((uint8_t*)pic + 0xC0) == cls) {
                if (g_logBinds) Log("picture %p: slot %d now draws class %d (was %d), body rebuilt", pic, cls, drawn,
                                    g_picDrawn[i].drawn);
                *(int*)((uint8_t*)pic + 0xC0) = -1;
            }
        } else {
            i = freeAt >= 0 ? freeAt : (int)(((uintptr_t)pic >> 4) & 15);  // full: reuse an entry
            g_picDrawn[i].pic = pic;
        }
        g_picDrawn[i].drawn = drawn;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_picture(pic, edx, cls, a2, a3, a4, a5, a6, a7);
}

// Select-screen panels. The preview (0xAB8740) and the info box (0xAB90C0) take
// a panel number; the HUD (0xABE2D0) takes the cursor. All thiscall, 1 arg, ret 4.
// The preview gets the class really in that panel's slot; the info box shows
// PLxx_INFO through our longer list, whose six panel entries follow the page.
static const uint32_t SEL_PREVIEW = 0xAB8740, SEL_INFOBOX = 0xAB90C0, SEL_HUD = 0xABE2D0;
static const uint32_t PLINFO_TABLE = 0x10C3DA4;
static const char* g_info[MAX_CLASSES];  // PLxx_INFO label of an extra class (null = use its panel's)
using PanelFn = void(__fastcall*)(void*, void*, int);
static PanelFn o_preview, o_infobox, o_hud;
static void* g_previewThis;
static int g_previewArg = -1;
static void* g_infoboxThis;
static int g_infoboxArg = -1;
static void* g_hudThis;
static int g_hudArg = -1;
static uintptr_t g_plinfoOrig[6];

// The preview has its own copy of the record lookup (0xAB8773: class >= 6 -> record 0,
// the Empress's: her weapon in the picture). For class >= 6 it now asks h_record,
// which finds an extra class's record in its slot. Patched at 0xAB8778 (the >= 6
// branch, esi = class, result in esi, continues at 0xAB87AA).
static const uint32_t PREVIEW_REC = 0xAB8778, PREVIEW_REC_DONE = 0xAB87AA;
static uintptr_t g_previewRecDone;
static uint8_t* __cdecl PreviewRecord(int cls)
{
    void* game = FindGame();
    uint8_t* rec = game ? h_record(game, nullptr, cls) : nullptr;
    if (g_logBinds && rec) {
        __try {
            uint8_t* mem = SaveOf(game);
            int cur = mem[0x40] < NCON ? mem[0x40] : 0;
            const uint8_t* setup = rec + *(uint32_t*)(rec + 0x34);
            Log("preview record class %d: slot %d holds %d (page %d), record class %d element %d",
                cls, cls % 6, g_slot[cur][cls % 6].occupant, g_page, setup[0], setup[3]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return rec;
}
__declspec(naked) static void h_previewRecord()
{
    __asm {
        push eax
        push ecx
        push edx
        push esi
        call PreviewRecord
        add esp, 4
        pop edx
        pop ecx
        test eax, eax
        jz keep
        mov esi, eax
        pop eax
        jmp g_previewRecDone
    keep:                       // no game object: the game's own way (record 0)
        pop eax
        mov ecx, dword ptr [eax + 4]
        movzx eax, byte ptr [ecx + 0x40]
        imul eax, dword ptr [ecx + 0x34]
        add eax, dword ptr [ecx + 0x38]
        mov esi, dword ptr [eax + ecx + 4]
        add esi, ecx
        jmp g_previewRecDone
    }
}

// The Oracle's second item (her pumpkin etc.) and pose in character pictures: the
// preview (0xAB87DA cmp bl,4) and the picture builder 0xAF6210 (four cmp edi,4) test
// "class == 4". Each test now asks whether the class's base is the Oracle.
static int g_isOracle;
static uintptr_t g_oj[10];  // jump targets / data (set when patched)
static void __cdecl OracleTest(int c)
{
    c &= 0xFF;
    if (c < 6 && !g_remoteHas[c]) c = g_bindMap[c];
    g_isOracle = (c < 6 ? c : (c < MAX_CLASSES && g_defined[c] ? g_baseOf[c] : -1)) == 4;
}
#define ORACLE_TEST(reg) __asm pushad __asm movzx eax, reg __asm push eax __asm call OracleTest __asm add esp, 4 __asm popad
__declspec(naked) static void h_oracle1()  // af63f9: cmp edi,4 / jne af641d
{
    ORACLE_TEST(di)
    __asm {
        cmp g_isOracle, 0
        je no
        jmp dword ptr [g_oj + 0]
    no: jmp dword ptr [g_oj + 4]
    }
}
__declspec(naked) static void h_oracle2()  // af65d2: cmp edi,4 / cmove ecx,[27fcee4]
{
    ORACLE_TEST(di)
    __asm {
        cmp g_isOracle, 0
        je no
        mov ecx, dword ptr [g_oj + 8]
        mov ecx, dword ptr [ecx]
    no: jmp dword ptr [g_oj + 12]
    }
}
__declspec(naked) static void h_oracle3()  // af662c: cmp edi,4 / setne cl
{
    ORACLE_TEST(di)
    __asm {
        cmp g_isOracle, 0
        sete cl
        jmp dword ptr [g_oj + 16]
    }
}
__declspec(naked) static void h_oracle4()  // af663f: cmp edi,4 / jne af6899
{
    ORACLE_TEST(di)
    __asm {
        cmp g_isOracle, 0
        je no
        jmp dword ptr [g_oj + 20]
    no: jmp dword ptr [g_oj + 24]
    }
}
__declspec(naked) static void h_oracle5()  // ab87da: cmp bl,4 / jne ab87eb
{
    ORACLE_TEST(bl)
    __asm {
        cmp g_isOracle, 0
        je no
        jmp dword ptr [g_oj + 28]
    no: jmp dword ptr [g_oj + 32]
    }
}
static void PatchOracleTests()
{
    struct P { uint32_t va; const char* code; int len; void* stub; };
    const P ps[] = {{0xAF63F9, "83ff04751f", 5, (void*)&h_oracle1}, {0xAF65D2, "83ff040f440d", 10, (void*)&h_oracle2},
                    {0xAF662C, "83ff040f95c1", 6, (void*)&h_oracle3}, {0xAF663F, "83ff040f8551020000", 9, (void*)&h_oracle4},
                    {0xAB87DA, "80fb04750c", 5, (void*)&h_oracle5}};
    for (const P& p : ps)
        if (!CodeAt(p.va, p.code) || (p.va == 0xAF65D2 && *(uint32_t*)Game(0xAF65D8) != Rel(0x27FCEE4))) {
            Log("classes: Oracle picture tests not as expected, left alone");
            return;
        }
    g_oj[0] = (uintptr_t)Game(0xAF63FE); g_oj[1] = (uintptr_t)Game(0xAF641D);
    g_oj[2] = (uintptr_t)Game(0x27FCEE4); g_oj[3] = (uintptr_t)Game(0xAF65DC);
    g_oj[4] = (uintptr_t)Game(0xAF6632);
    g_oj[5] = (uintptr_t)Game(0xAF6648); g_oj[6] = (uintptr_t)Game(0xAF6899);
    g_oj[7] = (uintptr_t)Game(0xAB87DF); g_oj[8] = (uintptr_t)Game(0xAB87EB);
    for (const P& p : ps) {
        uint8_t buf[16];
        memset(buf, 0x90, sizeof buf);
        buf[0] = 0xE9;
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)p.stub - (uintptr_t)Game(p.va + 5));
        Write(p.va, buf, p.len);
    }
}

// The COLOR change (0xAB8140) has the same inlined lookup for the HUD's class (+0x56C):
// 0xAB819B jb, the >= 6 branch at 0xAB819D leaves the record's offset from the save
// (edx) in ecx and goes on at 0xAB81C5.
static const uint32_t COLOUR_REC = 0xAB819D, COLOUR_REC_DONE = 0xAB81C5;
static uintptr_t g_colourRecDone;
__declspec(naked) static void h_colourRecord()
{
    __asm {
        push edx
        push esi
        call PreviewRecord
        add esp, 4
        pop edx
        test eax, eax
        jz keep
        sub eax, edx
        mov ecx, eax
        jmp g_colourRecDone
    keep:
        movzx eax, byte ptr [edx + 0x40]
        imul eax, dword ptr [edx + 0x34]
        add eax, dword ptr [edx + 0x38]
        mov ecx, dword ptr [eax + edx + 4]
        jmp g_colourRecDone
    }
}

// Equipment: "can this class use this item" tests the item's mask (item+0x48, a bit
// per class, Empress 1 .. Bandit 32) with 1 << class after a "class <= 5" (or < 6)
// guard, so an extra class could equip nothing new (the Shinobi's second weapon
// slot got stuck). At each site the guard is raised and 1 << class becomes
// 1 << base class (stubs below replace "mov eax,1 [mov ecx,ebx] shl eax,cl").
static uint32_t __cdecl ClassBit(int c)
{
    c &= 0xFF;
    int b = c < 6 ? c : (c < MAX_CLASSES && g_defined[c] ? g_baseOf[c] : -1);
    return b >= 0 ? 1u << b : 0;
}
__declspec(naked) static void h_bitEcx()  // eax = bit for class ecx (ecx, edx kept)
{
    __asm {
        push edx
        push ecx
        push ecx
        call ClassBit
        add esp, 4
        pop ecx
        pop edx
        ret
    }
}
__declspec(naked) static void h_bitEbx()  // ecx = ebx, eax = bit for class ebx
{
    __asm {
        mov ecx, ebx
        jmp h_bitEcx
    }
}
__declspec(naked) static void h_bitEcxToEdx()  // edx = bit for class ecx (eax, ecx kept)
{
    __asm {
        push eax
        push ecx
        push ecx
        call ClassBit
        add esp, 4
        pop ecx
        mov edx, eax
        pop eax
        ret
    }
}
// Weapon slots: the player's weapon code asks "Shinobi or Oracle?" (both weapons
// at once; the others swap between two) and switches on the class for the weapon
// type, reading the class byte (player+0xF5C) at 0xD78D23 / 0xD7902E (al),
// 0xD78E84 (cl) and 0xD79FC4 (bl). Those reads now give the base class.
static int __cdecl PlayerBase(uint8_t* player)
{
    int c = player[0xF5C];
    return c >= 6 && c < MAX_CLASSES && g_defined[c] ? g_baseOf[c] : c;
}
__declspec(naked) static void h_baseAl()  // al = base class of player esi (rest of eax kept)
{
    __asm {
        push ecx
        push edx
        push eax
        push esi
        call PlayerBase
        add esp, 4
        mov dl, al
        pop eax
        mov al, dl
        pop edx
        pop ecx
        ret
    }
}
__declspec(naked) static void h_baseCl()
{
    __asm {
        push eax
        push edx
        push ecx
        push esi
        call PlayerBase
        add esp, 4
        pop ecx
        mov cl, al
        pop edx
        pop eax
        ret
    }
}
__declspec(naked) static void h_baseBl()
{
    __asm {
        push eax
        push ecx
        push edx
        push esi
        call PlayerBase
        add esp, 4
        mov bl, al
        pop edx
        pop ecx
        pop eax
        ret
    }
}
// Stats in menus (status screen, inn): 0xCBF1E0 copies each class's stat growth
// (6 x 16-bit) into a cache of six at 0x28EC3F0 and reads cache[class] (class at
// stats+0xA4, read at 0xCBF408): class 6+ read past it (STR -20117, HP 10). Quests
// take another path. The read now gives the base class.
static const uint32_t GROWTH_CLASS = 0xCBF408;
static int __cdecl GrowthClass(int c)
{
    if (c >= 0 && c < 6) return c;
    return c >= 6 && c < MAX_CLASSES && g_defined[c] ? g_baseOf[c] : 0;
}
__declspec(naked) static void h_growthClass()  // replaces mov eax,[edi+0xA4] (call + nop)
{
    __asm {
        push ecx
        push edx
        push dword ptr [edi + 0xA4]
        call GrowthClass
        add esp, 4
        pop edx
        pop ecx
        ret
    }
}

// The same for the base stats (cache 0x28EC460, read in 0xCBEDB0 at 0xCBEFD7, esi = stats).
static const uint32_t BASESTAT_CLASS = 0xCBEFD7;
__declspec(naked) static void h_baseStatClass()  // replaces mov eax,[esi+0xA4] (call + nop)
{
    __asm {
        push ecx
        push edx
        push dword ptr [esi + 0xA4]
        call GrowthClass
        add esp, 4
        pop edx
        pop ecx
        ret
    }
}

// The Warrior's greatsword body: 0xD79D80 swaps to pl1_cb01.osb when "class == 1 and
// the weapon is that kind" (ebx = 1), at 0xD79E07 (cmp esi,1 / jne / cmp ebx,esi / jne).
// A Warrior-based class gets it too (palettes go by class, so its own colours apply).
static const uint32_t WARRIOR_BODY = 0xD79E07;
static uintptr_t g_warriorYes, g_warriorNo;
static int g_warriorBase;
__declspec(naked) static void h_warriorBody()
{
    __asm {
        push eax
        push ecx
        push edx
        push esi
        call GrowthClass
        add esp, 4
        mov g_warriorBase, eax
        pop edx
        pop ecx
        pop eax
        cmp g_warriorBase, 1
        jne no
        cmp ebx, 1
        jne no
        jmp g_warriorYes
    no:
        jmp g_warriorNo
    }
}

static void PatchWeaponSlots()
{
    struct P { uint32_t va; const char* code; void* stub; };
    const P ps[] = {{0xD78D23, "8a865c0f0000", (void*)&h_baseAl}, {0xD7902E, "8a865c0f0000", (void*)&h_baseAl},
                    {0xD78E84, "8a8e5c0f0000", (void*)&h_baseCl}, {0xD79FC4, "8a9e5c0f0000", (void*)&h_baseBl}};
    for (const P& p : ps)
        if (!CodeAt(p.va, p.code)) {
            Log("classes: weapon slot class reads not as expected, left alone");
            return;
        }
    for (const P& p : ps) {
        uint8_t buf[6] = {0xE8, 0, 0, 0, 0, 0x90};
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)p.stub - (uintptr_t)Game(p.va + 5));
        Write(p.va, buf, 6);
    }
}

// After character creation the select screen's coroutine (0xDA7F90) picks where the new
// character goes. Opened from the title (no [screen+58Ch]): a slot that had no character
// gets the opening cinematic and its class's tutorial (0xDA8235: builds the player, starts
// cSceneStage 0x28CF700); a slot that had one goes to the city (0xDA81DF: cSceneCity
// 0x28C8C48, state 5). (0xDA8189 is NOT town: it returns to whoever opened the screen,
// i.e. the title.) An extra class takes the city route at level 1 (a class-specific
// starting quest can come later).
static const uint32_t NEWCHAR_TUTORIAL = 0xDA8235, NEWCHAR_TOWN = 0xDA81DF, NEWCHAR_BACK = 0xDA8033,
                      NEWCHAR_TUTORIAL_GO = 0xDA823F;
static uintptr_t g_ncTown, g_ncBack, g_ncTutorial;
static int g_ncExtra;
static int __cdecl ExtraAtCursor(uint8_t* sel)
{
    __try {
        int k = *(int*)(sel + 0x3EC);
        if (k >= 0 && k < 6 && g_bindMap[k] >= 6) {
            Log("classes: new class %d goes to the city (no tutorial)", g_bindMap[k]);
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
__declspec(naked) static void h_newCharDest()  // at 0xDA8235 (cmp byte [edi+14h],0 / jne), ebp-18h = select screen
{
    __asm {
        pushad
        push dword ptr [ebp - 0x18]
        call ExtraAtCursor
        add esp, 4
        mov g_ncExtra, eax
        popad
        cmp g_ncExtra, 0
        jne town
        cmp byte ptr [edi + 0x14], 0
        jne back
        jmp g_ncTutorial
    back:
        jmp g_ncBack
    town:
        jmp g_ncTown
    }
}

static void PatchEquipTests()
{
    // {guard immediate's address, its value, code start, code bytes, stub}
    struct P { uint32_t guard; uint8_t was; uint32_t va; const char* code; void* stub; };
    const P ps[] = {
        {0xAC786A, 5, 0xAC7873, "b801000000d3e0", (void*)&h_bitEcx},
        {0xC90DE6, 5, 0xC90DED, "b8010000008bcbd3e0", (void*)&h_bitEbx},
        {0xC9189C, 5, 0xC9189F, "b801000000d3e0", (void*)&h_bitEcx},
        {0xDBD873, 5, 0xDBD876, "b801000000d3e0", (void*)&h_bitEcx},
        {0xDCB4AE, 5, 0xDCB4B1, "b801000000d3e0", (void*)&h_bitEcx},
        {0xBD63A4, 5, 0xBD63D3, "b801000000d3e0", (void*)&h_bitEcx},
        {0xB09294, 5, 0xB09297, "ba01000000d3e2", (void*)&h_bitEcxToEdx},
        {0xAC454B, 6, 0xAC454E, "b8010000008bcbd3e0", (void*)&h_bitEbx},
        {0xCF0EF7, 6, 0xCF0EFA, "b801000000d3e0", (void*)&h_bitEcx},
    };
    int n = 0;
    for (const P& p : ps) {
        if (*Game(p.guard) != p.was || !CodeAt(p.va, p.code)) {
            Log("classes: equipment test at %08X not as expected, left alone", p.va);
            continue;
        }
        uint8_t g = (uint8_t)(MAX_CLASSES - 1);  // signed byte: 127 is the most it holds
        Write(p.guard, &g, 1);
        uint8_t buf[9];
        size_t len = strlen(p.code) / 2;
        memset(buf, 0x90, sizeof buf);
        buf[0] = 0xE8;
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)p.stub - (uintptr_t)Game(p.va + 5));
        Write(p.va, buf, len);
        n++;
    }
    Log("classes: %d equipment test(s) follow the base class", n);
}

static void __fastcall h_preview(void* self, void* edx, int panel)
{
    g_previewThis = self;
    g_previewArg = panel;
    int k = panel & 0xFF;
    if (k < 6) panel = (panel & ~0xFF) | g_bindMap[k];
    o_preview(self, edx, panel);
}

// The new-character screen builds the info box itself (0xAB92A0, thiscall(box, x),
// ret 4; the panel is at box+0x4C8): remember it so page changes redraw it too.
using BoxBuildFn = void(__fastcall*)(void*, void*, int);
static BoxBuildFn o_boxBuild;
static void __fastcall h_boxBuild(void* box, void* edx, int x)
{
    g_infoboxThis = box;
    __try {
        int panel = *(int*)((uint8_t*)box + 0x4C8);
        if (panel >= 0 && panel < 6) g_infoboxArg = panel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_boxBuild(box, edx, x);
}

static void __fastcall h_infobox(void* self, void* edx, int panel)
{
    g_infoboxThis = self;
    g_infoboxArg = panel;
    o_infobox(self, edx, panel);
}

static bool PageHighlights();
static void __fastcall h_hud(void* self, void* edx, int cursor)
{
    g_hudThis = self;
    g_hudArg = cursor;
    o_hud(self, edx, cursor);
    bool redo = false;
    __try {
        redo = PageHighlights();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (redo) o_hud(self, edx, cursor);
}

static void PageTexts()
{
    int t = TableIndex(PLINFO_TABLE);
    if (t < 0) return;
    for (int k = 0; k < 6; k++) {
        int cls = g_page > 0 ? 6 * g_page + k : k;
        g_tab[t][k] = (cls >= 6 && g_defined[cls] && g_info[cls]) ? (uintptr_t)g_info[cls] : g_plinfoOrig[k];
    }
}

static void RefreshPanels()
{
    __try {
        if (g_previewThis && g_previewArg >= 0) h_preview(g_previewThis, nullptr, g_previewArg);
        if (g_infoboxThis && g_infoboxArg >= 0) {
            *(int*)((uint8_t*)g_infoboxThis + 0x4C8) = -1;  // it only redraws when the panel changes
            o_infobox(g_infoboxThis, nullptr, g_infoboxArg);
        }
        if (g_hudThis && g_hudArg >= 0) o_hud(g_hudThis, nullptr, g_hudArg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not refresh the select screen after a page change");
    }
}

// The select screen queues each panel's character files while it opens
// (0xDA8380..0xDA8D00): palette (0xE34FD0, fastcall(loader, setup block)) then
// the class's preload function (per-class list 0x111A1A4, cdecl(loader, setup
// block, 0)), and loads the queue once. Requests made later aren't loaded, so
// when the screen queues its first palette, the extra classes' files are queued
// too, with their own setup blocks, ready for when the page changes.
static const uint32_t PAL_LOAD = 0xE34FD0, PRELOAD_TABLE = 0x111A1A4;
static const uint32_t SELECT_SETUP_LO = 0xDA8380, SELECT_SETUP_HI = 0xDA8D00, TOWN_PAL_RET = 0xDF4E24;
static bool FromSelectSetup(uintptr_t from)
{
    return from >= (uintptr_t)Game(SELECT_SETUP_LO) && from < (uintptr_t)Game(SELECT_SETUP_HI);
}
static bool RawBodies(bool on)
{
    if (on && g_rawBodies) return false;  // already off (our own queueing): leave it to that
    g_rawBodies = on ? 1 : 0;
    return true;
}
using PalLoadFn = void(__fastcall*)(void*, uint8_t*);
static PalLoadFn o_palLoad;
static void* g_queuedFor;  // loader we already queued the extra classes for

static bool QueueExtraClasses(void* loader, int preloadArg, const char* where)
{
    int t = TableIndex(PRELOAD_TABLE);
    if (t < 0 || !FindGame()) return false;
    uint8_t* mem = SaveOf(g_game);
    if (mem != g_lastMem) ApplyPageAll(mem);
    int cur = mem[0x40] < NCON ? mem[0x40] : 0;
    int queued = 0;
    for (int cls = 6; cls < MAX_CLASSES; cls++) {
        if (!g_defined[cls] || !g_extra[cls]) continue;
        int k = (cls - 6) % 6;
        const Slot& s = g_slot[cur][k];
        if (!s.nr) continue;
        uint8_t block[0x34];
        if (g_extra[cls]->have[cur]) {
            memcpy(block, g_extra[cls]->blob[cur] + 0x38, sizeof block);  // its saved setup block
        } else {
            memcpy(block, mem + U(mem + 0x68 + 0xC * cur + 4) + s.setupAt, sizeof block);  // as its panel's class
        }
        block[0] = (uint8_t)cls;
        o_palLoad(loader, block);
        ((void(__cdecl*)(void*, uint8_t*, int))g_tab[t][cls])(loader, block, preloadArg);
        queued++;
    }
    // Slots that hold an extra class right now (the page was left on 2+): the screen
    // queued that class, so queue the slot's own class too, for when the page turns back.
    // The body-name redirect (slot -> class in it) is off meanwhile, or it would load
    // the extra class's body again instead of the slot's own (page 1 drawn invisible).
    g_rawBodies = 1;
    for (int k = 0; k < 6; k++) {
        const Slot& s = g_slot[cur][k];
        if (!s.nr || s.occupant < 6) continue;
        uint8_t block[0x34];
        memcpy(block, s.stash + 0x38, sizeof block);  // the base class's own setup block
        block[0] = (uint8_t)k;
        o_palLoad(loader, block);
        ((void(__cdecl*)(void*, uint8_t*, int))g_tab[t][k])(loader, block, preloadArg);
        queued++;
    }
    g_rawBodies = 0;
    if (queued) Log("classes: queued %d more class(es)' files for the %s", queued, where);
    return true;
}

static void __fastcall h_palLoad(void* loader, uint8_t* block)
{
    uintptr_t from = (uintptr_t)_ReturnAddress();
    if (g_logBinds && block) Log("palette load class %d from %p (page %d)", block[0], (void*)(from - (uintptr_t)Game(0)), g_page);
    o_palLoad(loader, block);
    // The town loads the six slots' classes one by one (0xDF4CF0, preload arg 1) for
    // the inn's previews: queue the extra classes there too, once per town load.
    static void* townLoader;
    static DWORD townAt;
    if (from == (uintptr_t)Game(TOWN_PAL_RET) && (loader != townLoader || GetTickCount() - townAt > 5000)) {
        townLoader = loader;
        townAt = GetTickCount();
        __try {
            QueueExtraClasses(loader, 1, "town");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_rawBodies = 0;
            Log("classes: could not queue the extra classes' files in town");
        }
    }
    if (loader != g_queuedFor && from >= (uintptr_t)Game(SELECT_SETUP_LO) && from < (uintptr_t)Game(SELECT_SETUP_HI)) {
        __try {
            if (QueueExtraClasses(loader, 0, "select screen")) g_queuedFor = loader;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_rawBodies = 0;
            Log("classes: could not queue the extra classes' files");
        }
    }
}

// Page art (built by tools/selectart.py into s_o0a00): the panel layout is the
// animation キャラクターセレクト2, set on a screen object with 0xE4EA70
// (thiscall(obj, name hash, ptr, flag, float), ret 0x10); each page with art has
// a copy MOD_SEL_PAGE<p>. The selected panel's coloured picture is an animation
// per panel, whose name hashes the HUD caches in six words at 0x28EB3A8; an extra
// class with art has MOD_SEL_<class>.
static const uint32_t SET_ANIM = 0xE4EA70, HIGHLIGHT_CACHE = 0x28EB3A8;
static const uint32_t SEL_LAYOUT = 0xE00638C3;  // キャラクターセレクト2
static const uint32_t SEL_HIGHLIGHT[6] = {0x44226404, 0x02F5DB6E, 0xC0B94671, 0xDB5C45C2, 0x1231F6DD, 0x18FB25B7};
static bool g_hasHighlight[MAX_CLASSES], g_hasPageLayout[MAX_CLASSES / 6 + 1];
using SetAnimFn = uint32_t(__fastcall*)(void*, void*, uint32_t, void*, int, float);
static SetAnimFn o_setAnim;
// The HUD objects the panel layout was set on when the screen opened (those set
// together: a gap of over a second starts a new list).
struct LayoutUse {
    void* obj;
    void* ptr;
};
static LayoutUse g_layoutUse[4];
static int g_layoutUses;
static DWORD g_layoutAt;

static uint32_t NameHash(const char* fmt, int n)
{
    char name[32];
    sprintf_s(name, fmt, n);
    return IceHash(name);
}

static uint32_t PageLayout()
{
    return g_hasPageLayout[g_page] ? NameHash("MOD_SEL_PAGE%d", g_page) : SEL_LAYOUT;
}

// The panel layout is set on two objects: the select HUD's (0xABCCC1) and the info
// panel's (0xAB704B, which hangs the COLOR icon on it; changing that object's
// animation later breaks the icon, and the next cursor move crashed). The info
// panel gets MOD_SEL_INFO (no panel pictures) once; only the HUD's is switched.
// That call is found by its own call site (0xAB704B, patched to InfoLayout below), not
// by the return address: another mod hooking 0xE4EA70 too makes our
// hook see that mod's return address, and the info panel then got switched on every
// page change; its COLOR icon lost its animation set and the next cursor move crashed
// in 0xE4EA95. The return-address test stays as a fallback if the call isn't patched.
static const uint32_t INFO_LAYOUT_CALL = 0xAB704B, INFO_LAYOUT_CALL_RET = 0xAB7050;
static uint32_t __fastcall InfoLayout(void* obj, void* edx, uint32_t hash, void* ptr, int flag, float f)
{
    if (hash == SEL_LAYOUT && g_pages > 1) hash = IceHash("MOD_SEL_INFO");
    // through the whole hook chain at 0xE4EA70, like the game's own call
    return ((SetAnimFn)Game(SET_ANIM))(obj, edx, hash, ptr, flag, f);
}
static bool g_logSounds = false;  // diagnostics: log sounds, animations (with callers) and inn states
static uint32_t __fastcall h_setAnim(void* obj, void* edx, uint32_t hash, void* ptr, int flag, float f)
{
    if (g_logSounds) {  // distinct (animation, caller) pairs
        static uint64_t seen[512];
        static int nseen;
        uint64_t key = (uint64_t)hash << 32 | (uint32_t)(uintptr_t)_ReturnAddress();
        bool found = false;
        for (int i = 0; i < nseen && !found; i++) found = seen[i] == key;
        if (!found && nseen < 512) {
            seen[nseen++] = key;
            Log("anim %08X on %p from %p", hash, obj, (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));
        }
    }
    if (hash == SEL_LAYOUT && g_pages > 1) {
        if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(INFO_LAYOUT_CALL_RET)) {
            hash = IceHash("MOD_SEL_INFO");
        } else {
            DWORD now = GetTickCount();
            if (now - g_layoutAt > 1000) g_layoutUses = 0;
            g_layoutAt = now;
            int i = 0;
            while (i < g_layoutUses && g_layoutUse[i].obj != obj) i++;
            if (i < 4) {
                g_layoutUse[i] = {obj, ptr};
                if (i == g_layoutUses) g_layoutUses++;
            }
            hash = PageLayout();
        }
    }
    return o_setAnim(obj, edx, hash, ptr, flag, f);
}

// Set the HUD's six highlight names for the current page; true if any changed.
static bool PageHighlights()
{
    uint32_t* cache = (uint32_t*)Game(HIGHLIGHT_CACHE);
    bool changed = false;
    for (int k = 0; k < 6; k++) {
        int cls = 6 * g_page + k;
        uint32_t want = g_page > 0 && cls < MAX_CLASSES && g_defined[cls] && g_hasHighlight[cls]
                            ? NameHash("MOD_SEL_%d", cls) : SEL_HIGHLIGHT[k];
        if (cache[k] != want && cache[k]) {
            cache[k] = want;
            changed = true;
        }
    }
    return changed;
}


// Page prompt "RT Page n/N" under Confirm / Return, made the way the screen makes
// those two. Prompts: 0xA92060 (ecx = the screen's text container (screen+0x3F8),
// edx = list of {text part, text part, PadInfo.tb2 label}, stack: count, prompt
// layout, "PadInfo.tb2", spacing, 0, 0, 0; caller cleans) makes a text per entry at
// its part; the screen then gives each text colour 1 (0xCCD580) and priority 0x1E.
// Icons: a com_butt.osb sprite (0xE093C0, priority 0xE4E6A0) playing a button
// animation (its name hash in a global; ZRボタン = RT), pinned to a layout part by
// pointing its +0x44 at the part's transform. Build mod adds the parts
// button3 / button_text3 and the labels MOD_PAGE_<n>.
static const uint32_t PROMPTS = 0xA92060, PROMPTS_FROM_SELECT = 0xABD73F;
static const uint32_t MAKE_SPRITE = 0xE093C0, SPRITE_PRIORITY = 0xE4E6A0, MAP_FIND = 0x7B2870;
static const uint32_t MAP_FIND_ADD = 0x9E00C0, TEXT_COLOUR = 0xCCD580, BUTTON_RT = 0x28A0608;
struct PromptEntry {
    uint32_t part, part2;
    const char* label;
};
static void* o_prompts;
static void* g_promptSelf;  // text container of the open select screen
static uint32_t g_promptArgs[7];
static PromptEntry g_selPrompts[3];
static char g_pageLabel[16];
static uint8_t g_iconAnim[0x40];

static void CallPrompts(void* self, PromptEntry* list, uint32_t* a)
{
    void* fn = o_prompts;
    __asm {
        mov esi, a
        push dword ptr [esi + 24]
        push dword ptr [esi + 20]
        push dword ptr [esi + 16]
        push dword ptr [esi + 12]
        push dword ptr [esi + 8]
        push dword ptr [esi + 4]
        push dword ptr [esi]
        mov ecx, self
        mov edx, list
        call fn
        add esp, 28
    }
}

// The new prompt text, styled as the screen styles its own.
static void StylePageText(void* self)
{
    uint32_t key = g_selPrompts[2].part;
    void* it = nullptr;
    ((void**(__thiscall*)(void*, void**, uint32_t*))Game(MAP_FIND_ADD))(self, &it, &key);
    void* text = *(void**)((uint8_t*)it + 0x14);
    void** vt = *(void***)text;
    void* colour = ((void*(__fastcall*)(int))Game(TEXT_COLOUR))(1);
    ((void(__thiscall*)(void*, void*))vt[0x1C / 4])(text, colour);
    ((void(__thiscall*)(void*, int))vt[0x28 / 4])(text, 0x1E);
}

static void PageIcon(uint8_t* screen)
{
    void* icon = ((void*(__thiscall*)(void*, const char*, int, int))Game(MAKE_SPRITE))(screen, "com_butt.osb", 0, 0);
    if (!icon) return;
    ((void(__thiscall*)(void*, int))Game(SPRITE_PRIORITY))(icon, 0x1E);
    memset(g_iconAnim, 0, sizeof g_iconAnim);
    o_setAnim(icon, nullptr, *(uint32_t*)Game(BUTTON_RT), g_iconAnim, 1, 0.0f);
    uint8_t* lay = *(uint8_t**)(screen + 0x3A8);
    void* map = lay + 0x150;
    uint32_t key = IceHash("button3");
    void* it = nullptr;
    ((void**(__thiscall*)(void*, void**, uint32_t*))Game(MAP_FIND))(map, &it, &key);
    if (it && it != *(void**)map) *(uint8_t**)((uint8_t*)icon + 0x44) = *(uint8_t**)((uint8_t*)it + 0x14) + 0x14;
    else Log("classes: the select screen layout has no part button3 (rebuild the mod)");
}

// Called instead of the prompt maker; true = handled here.
static bool g_promptsMade;
static void MakeSelectPrompts(void* self, PromptEntry* list, uint32_t* args)
{
    uint32_t t3 = IceHash("button_text3");
    g_selPrompts[0] = list[0];
    g_selPrompts[1] = list[1];
    sprintf_s(g_pageLabel, "MOD_PAGE_%d", g_page + 1);
    g_selPrompts[2] = {t3, t3, g_pageLabel};
    memcpy(g_promptArgs, args, sizeof g_promptArgs);
    uint32_t a[7];
    memcpy(a, args, sizeof a);
    a[0] = 3;
    CallPrompts(self, g_selPrompts, a);
    g_promptsMade = true;
    g_promptSelf = self;
    StylePageText(self);
    PageIcon((uint8_t*)self - 0x3F8);
}

// Called instead of the prompt maker; 1 = handled here.
static bool g_logPrompts = false;  // diagnostics: log every prompt list
static int __cdecl PromptHook(void* self, PromptEntry* list, uint32_t* args, uintptr_t from)
{
    if (g_logPrompts) {
        __try {
            char buf[256];
            int n = 0;
            for (uint32_t i = 0; i < args[0] && i < 6; i++)
                n += sprintf_s(buf + n, sizeof buf - n, " [%08X %08X %s]", list[i].part, list[i].part2,
                               list[i].label ? list[i].label : "-");
            Log("prompts from %p self %p count %u layout %p table %s spacing %.1f %u %u %u:%s",
                (void*)(from - (uintptr_t)Game(0)), self, args[0], (void*)args[1], (const char*)args[2],
                (double)*(float*)&args[3], args[4], args[5], args[6], buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (from != (uintptr_t)Game(PROMPTS_FROM_SELECT) || g_pages < 2 || args[0] != 2) return 0;
    g_promptsMade = false;
    g_promptSelf = nullptr;
    __try {
        MakeSelectPrompts(self, list, args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not add the page prompt");
    }
    return g_promptsMade;  // otherwise the game makes its own two
}


__declspec(naked) static void h_prompts()
{
    __asm {
        push ecx
        push edx
        lea eax, [esp + 12]
        push dword ptr [esp + 8]
        push eax
        push dword ptr [esp + 8]
        push dword ptr [esp + 16]
        call PromptHook
        add esp, 16
        pop edx
        pop ecx
        test eax, eax
        jnz done
        jmp o_prompts
    done:
        ret
    }
}

static void PagePrompt()
{
    if (!g_promptSelf) return;
    sprintf_s(g_pageLabel, "MOD_PAGE_%d", g_page + 1);
    PromptEntry one = g_selPrompts[2];
    uint32_t a[7];
    memcpy(a, g_promptArgs, sizeof a);
    a[0] = 1;
    CallPrompts(g_promptSelf, &one, a);
    StylePageText(g_promptSelf);
}

static void PageArt()
{
    __try {
        for (int i = 0; i < g_layoutUses; i++)
            o_setAnim(g_layoutUse[i].obj, nullptr, PageLayout(), g_layoutUse[i].ptr, 0, 0.0f);
        PageHighlights();
        PagePrompt();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not change the select screen art for the page");
    }
}

// The game's own input: 0xD82B40(action) (thiscall on the app object, ret 4) says
// whether a logical action (0..0x38; 0x2F confirm, 0x30 back) fired, for whatever
// pad or keyboard is in use. 0x7C9770() returns the app object. With debug logging
// on, actions that fire are logged.
static const uint32_t GAME_ACTION = 0xD82B40, APP_OBJECT = 0x7C9770;
static const int ACTION_LT = 0x1A, ACTION_RT = 0x1B;
using GameActionFn = bool(__fastcall*)(void*, void*, int);
static GameActionFn o_gameAction;
static bool g_logActions;
static bool __fastcall h_gameAction(void* app, void* edx, int action)
{
    bool r = o_gameAction(app, edx, action);
    if (g_logActions && r) {
        static DWORD lastAt[0x40];
        DWORD now = GetTickCount();
        if (action >= 0 && action < 0x40 && now - lastAt[action] > 300)
            Log("action 0x%02X fired (asked from %p)", action, (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));
        if (action >= 0 && action < 0x40) lastAt[action] = now;
    }
    return r;
}

// Page keys, polled each frame of the character select screen.
static bool PageKey(int dir)
{
    bool down = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) down = (GetAsyncKeyState(dir < 0 ? VK_PRIOR : VK_NEXT) & 0x8000) != 0;  // Page Up / Page Down
    // LT / RT as the game sees them (actions 0x1A / 0x1B; LB/RB are 0x18 / 0x19 and turn
    // the description pages), so any pad or key binding the game supports works.
    if (!down && o_gameAction) {
        __try {
            if (void* app = ((void*(__cdecl*)())Game(APP_OBJECT))())
                down = o_gameAction(app, nullptr, dir < 0 ? ACTION_LT : ACTION_RT);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return down;
}


// Panels that can be picked: a bit per panel at select+0x3E8 (the game clears the
// bits of classes whose DLC isn't owned; the cursor skips those panels and Confirm
// buzzes), cursor at +0x3EC. On a page above the first only panels with a class are on.
static uint8_t* g_maskSel;
static uint32_t g_maskOrig;

static bool PageMask(uint8_t* sel)
{
    uint32_t* mask = (uint32_t*)(sel + 0x3E8);
    int* cursor = (int*)(sel + 0x3EC);
    if (sel != g_maskSel) {  // a newly opened screen: its mask is the game's own
        g_maskSel = sel;
        g_maskOrig = *mask;
    }
    uint32_t want = g_maskOrig;
    if (g_page > 0) {
        want = 0;
        for (int k = 0; k < 6; k++)
            if (6 * g_page + k < MAX_CLASSES && g_defined[6 * g_page + k]) want |= 1u << k;
    }
    *mask = want;
    if (want && (*cursor < 0 || *cursor > 5 || !(want >> *cursor & 1)))
        for (int k = 0; k < 6; k++)
            if (want >> k & 1) {
                *cursor = k;
                return true;
            }
    return false;
}

// After moving the cursor ourselves, update the screen as its own cursor move does
// (0xDAA6F8): preview (0xAB8740) and COLOR icon (0xAB8140, the panel's colour at
// +0x3F0 + 4*panel) on the HUD at +0x594, then 0xDAB390.
static void CursorMoved(uint8_t* sel)
{
    int cur = *(int*)(sel + 0x3EC);
    void* hud = *(void**)(sel + 0x594);
    ((void(__thiscall*)(void*, int))Game(0xAB8740))(hud, (uint8_t)cur);
    ((void(__thiscall*)(void*, int))Game(0xAB8140))(hud, sel[0x3F0 + 4 * cur]);
    ((void(__thiscall*)(void*))Game(0xDAB390))(sel);
    if (g_infoboxArg >= 0) g_infoboxArg = cur;
    if (g_hudArg >= 0) g_hudArg = cur;
}

// ---- pages on the bar party menu's Character screen (the online lobby) ----
// Coroutine 0xD9CA70 (thiscall(scene, a1, frame), ret 8), state at frame+0x18, data at
// frame+0x10: +0x1C cursor row, +0x1D rows, +0x28 the rows' slots (0xC92300 lists the
// owned, created slots), +0x14 rows panel, +0x18 status panel. 0x15D builds it all (and
// replaces both panels), 0x15F waits for it to open, 0x163 takes input (Confirm plays
// its sound from 0xD9CEEF, then 0xC90C00(slot) switches), 0x165 closes. A page change
// swaps the page and runs 0x15D again, so the game rebuilds the rows itself.
static const uint32_t BAR_STEP = 0xD9CA70, BAR_CONFIRM_RET = 0xD9CEF4, BAR_ROW_PORTRAIT_RET = 0xAA031C;
static DWORD g_barStepAt;
static int g_barState = -1;
static int g_barOpenPage = -1;  // page when the screen opened (restored unless a class was picked)
static bool g_barConfirmed;
static bool g_barForceRefresh;  // next lobby "did the class change?" check answers yes
static const uint32_t BAR_CUR_RET = 0xD93255;

static bool BarListShown()
{
    return GetTickCount() - g_barStepAt < 500 && (g_barState == 0x15D || g_barState == 0x15F || g_barState == 0x163);
}

// Sound effects: 0xD82EB0 (stdcall: handle out, sound hash, flags; the handle is
// released with 0x7B7530). Some hashes (flags 0x8005, menus): 2451015F, CE8F84FD
// cursor; 6718B1C8 buzzer (can't do that); E94E103D back; AAF9C38C shop page flip.
// g_logSounds logs every sound played, to find more.
static const uint32_t PLAY_SOUND = 0xD82EB0, SOUND_RELEASE = 0x7B7530, SOUND_PAGE = 0xAAF9C38C;
using PlaySoundFn = void*(__stdcall*)(void*, uint32_t, uint32_t);
static PlaySoundFn o_playSound;
static uint8_t* g_stepSel;  // set while the select screen's step runs
static int g_stepCursor;
static void InnBack();
static void* __stdcall h_playSound(void* out, uint32_t hash, uint32_t flags)
{
    // a cursor move that ends on the same panel (the others on this page are empty): buzz
    if (hash == 0xCE8F84FD && g_stepSel && g_page > 0 && *(int*)(g_stepSel + 0x3EC) == g_stepCursor)
        hash = 0x6718B1C8;
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(BAR_CONFIRM_RET)) g_barConfirmed = true;
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(0xDB971A)) {
        __try {
            InnBack();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_logSounds) Log("sound %08X %X from %p", hash, flags, (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));  // exe address
    return o_playSound(out, hash, flags);
}

static void PlayMenuSound(uint32_t hash)
{
    __try {
        uint32_t handle[2] = {};
        o_playSound(handle, hash, 0x8005);
        ((void(__thiscall*)(void*))Game(SOUND_RELEASE))(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int g_refreshIn;  // frames until the preview is redrawn after a page change
using StepFn = uint8_t(__fastcall*)(void*, void*, void*, void*);
static StepFn o_step;
static uint8_t __fastcall h_step(void* self, void* edx, void* ctx, void* stack)
{
    static bool wasL, wasR;
    bool l = PageKey(-1), r = PageKey(1);
    if (g_pages > 1 && (uint8_t*)self != g_maskSel) {
        __try {
            if (PageMask((uint8_t*)self)) CursorMoved((uint8_t*)self);
            if (g_page > 0) {  // opened on a later page: its texts, and redraw the panels once they're up
                PageTexts();
                if (FindGame()) ApplyPageAll(SaveOf(g_game));
                g_refreshIn = 10;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_pages > 1 && ((l && !wasL) || (r && !wasR)) && FindGame()) {
        g_page = (g_page + (r && !wasR ? 1 : g_pages - 1)) % g_pages;
        bool moved = false;
        __try {
            moved = PageMask((uint8_t*)self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            uint8_t* mem = SaveOf(g_game);
            g_lastMem = nullptr;
            ApplyPageAll(mem);
            int cur = mem[0x40] < NCON ? mem[0x40] : 0;
            for (int k = 0; k < 6; k++) g_bindMap[k] = g_slot[cur][k].nr ? g_slot[cur][k].occupant : k;
            // each panel's COLOR (+0x3F0 + 4*panel) is the colour of the class now in it
            // (setup block byte 1); the screen filled them for the classes it opened with
            for (int k = 0; k < 6; k++) {
                if (!g_slot[cur][k].nr) continue;
                uint8_t* rec = o_record(g_game, nullptr, k);
                const uint8_t* setup = rec + *(uint32_t*)(rec + 0x34);
                if (setup[1] <= 7) *(int*)((uint8_t*)self + 0x3F0 + 4 * k) = setup[1];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        PlayMenuSound(SOUND_PAGE);
        PageTexts();
        PageArt();
        (void)moved;  // always: the preview and COLOR follow the class now under the cursor
        {
            __try {
                CursorMoved((uint8_t*)self);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("classes: could not move the select cursor for the page");
            }
        }
        RefreshPanels();
        g_refreshIn = 30;  // files load in the background: draw the preview again shortly
        Log("classes: select page %d of %d", g_page + 1, g_pages);
    }
    wasL = l;
    wasR = r;
    if (g_refreshIn > 0 && --g_refreshIn == 0) RefreshPanels();
    g_stepSel = (uint8_t*)self;
    g_stepCursor = *(int*)((uint8_t*)self + 0x3EC);
    uint8_t r2 = o_step(self, edx, ctx, stack);
    g_stepSel = nullptr;
    return r2;
}


// ---- pages on the inn's CHARACTER SELECT ----
// Scene coroutine 0xDB9110 (thiscall(scene, a1, frame), ret 8): state at
// frame+0x18, its data at frame+0x10: +0x35 cursor (row = save slot), +0x34
// sub-mode (0 = the list), +0x2C rows HUD, scene+0x430 status panel. The list
// takes input in state 0xB3 (0x91 while it opens). Rows are a list widget in the rows HUD's part map
// (+0x478) under "List" (0x10C6504 in the exe); 0xAF7200(cursor, top, 0) then
// 0xAF72A0 refill it, 0xAD78F0(row) moves the highlight, 0xAAE820(row) redraws
// the status panel. Return plays its sound from 0xDB971A; Confirm compares the
// row with the current class (0xC90BE0, called from 0xDB985B) after checking the
// row can be switched to (0xC922C0, from 0xDB9844); the switch itself is
// 0xC90C00(slot), which only loads the new slot's record.
static const uint32_t INN_STEP = 0xDB9110, INN_STATUS = 0xAAE820, INN_HILITE = 0xAD78F0;
static const uint32_t LIST_SELECT = 0xAF7200, LIST_REFILL = 0xAF72A0, LIST_NAME = 0x10C6504;
static const uint32_t CUR_CLASS = 0xC90BE0, CAN_SWITCH = 0xC922C0;
static const uint32_t INN_CUR_RET = 0xDB9860, INN_CAN_RET = 0xDB9849, INN_BACK_RET = 0xDB971A;
static int g_innOpenPage = -1;  // page when the list opened (restored on Return); -1 = not open

static void SwapToPage(int page)
{
    g_page = page;
    uint8_t* mem = SaveOf(g_game);
    g_lastMem = nullptr;
    ApplyPageAll(mem);
    int cur = mem[0x40] < NCON ? mem[0x40] : 0;
    for (int k = 0; k < 6; k++) g_bindMap[k] = g_slot[cur][k].nr ? g_slot[cur][k].occupant : k;
}

static void InnRefresh(uint8_t* scene, uint8_t* so)
{
    int cur = so[0x35];
    uint8_t* hud = *(uint8_t**)(so + 0x2C);
    uint32_t key = IceHash((const char*)Game(LIST_NAME));
    void* it = nullptr;
    ((void**(__thiscall*)(void*, void**, uint32_t*))Game(MAP_FIND))(hud + 0x478, &it, &key);
    if (it && it != *(void**)(hud + 0x478)) {
        void* list = *(void**)((uint8_t*)it + 0x14);
        ((void(__thiscall*)(void*, int, int, int))Game(LIST_SELECT))(list, cur, 0, 0);
        ((void(__thiscall*)(void*))Game(LIST_REFILL))(list);
    } else {
        Log("classes: inn list not found");
    }
    ((void(__thiscall*)(void*, int))Game(INN_HILITE))(hud, cur);
    ((void(__thiscall*)(void*, int))Game(INN_STATUS))(*(void**)(scene + 0x430), cur);
}


// The guide bar at the bottom of town menus (the inn's included): 0xAF2A70 (thiscall,
// no args) rebuilds it from its prompt groups, adding each prompt with 0xAF3090
// (thiscall(bar, &count, &x, button animation, PadInfo.tb2 label), ret 0x10), which
// lays them out left to right; the bar is then stretched to their width. On the
// inn's character list a page prompt (RT + MOD_PAGE_<n>) follows Return.
static const uint32_t BAR_BUILD = 0xAF2A70, BAR_ADD = 0xAF3090;
static DWORD g_innStepAt;
static int g_innStepState = -1;
static void* g_lastBar;  // bar that last added a Return prompt
static DWORD g_lastBarAt;
static char g_barPageLabel[16];

static bool InnListShown()
{
    return GetTickCount() - g_innStepAt < 500 &&
           (g_innStepState == 0 || g_innStepState == 0x91 || g_innStepState == 0xB3);
}

using BarAddFn = void(__fastcall*)(void*, void*, int*, float*, uint32_t, const char*);
static BarAddFn o_barAdd;
static void __fastcall h_barAdd(void* bar, void* edx, int* count, float* x, uint32_t anim, const char* label)
{
    o_barAdd(bar, edx, count, x, anim, label);
    __try {
        if (label && !strcmp(label, "RETURN")) {
            g_lastBar = bar;
            g_lastBarAt = GetTickCount();
            if (g_pages > 1 && (InnListShown() || BarListShown())) {
                sprintf_s(g_barPageLabel, "MOD_PAGE_%d", g_page + 1);
                o_barAdd(bar, edx, count, x, *(uint32_t*)Game(BUTTON_RT), g_barPageLabel);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not add the page prompt to the guide bar");
    }
}

static void InnBar(bool onlyIfFresh)
{
    if (!g_lastBar || (onlyIfFresh && GetTickCount() - g_lastBarAt > 3000)) return;
    __try {
        ((void(__thiscall*)(void*))Game(BAR_BUILD))(g_lastBar);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: could not rebuild the guide bar");
    }
}


// Rows with no class on this page look and act like a class you don't own:
// 0xC92540 (thiscall(game, class), ret 4) answers whether a class is owned (DLC);
// the inn's rows, status panel and Confirm all ask it.
static const uint32_t CLASS_OWNED = 0xC92540;
using OwnedFn = uint8_t(__fastcall*)(void*, void*, int);
static OwnedFn o_owned;
static uint8_t __fastcall h_owned(void* game, void* edx, int cls)
{
    if (g_page > 0 && cls >= 0 && cls < 6 && (InnListShown() || BarListShown()) &&
        !(6 * g_page + cls < MAX_CLASSES && g_defined[6 * g_page + cls]))
        return 0;
    return o_owned(game, edx, cls);
}


// Class names (CLASS_<nn> in sys_common.tb2) through the longer name list (0x11022C4,
// one of the extended tables), and portraits: 0xCCCD80 (fastcall(class) -> animation
// hash) picks PlIcon_<class+1> in com_pl_icon; an extra class's is MOD_PlIcon_<class>.
// The inn's rows and status panel look these up by slot, so while its list shows a
// page, the six slot entries follow the classes in the slots.
static const uint32_t PORTRAIT = 0xCCCD80, INN_PORTRAIT_RET = 0xAD8A0E;
static const char* g_name[MAX_CLASSES];
static int g_portrait[MAX_CLASSES];  // 1 small, 2 large, 3 both
static bool g_barFace[MAX_CLASSES];  // has its own bar party menu face (MOD_PlFace_<class>)
// Build mod made the "empty" pictures (MOD_EMPTY_*: the game's grey "no element" dash):
// shown for a class with no picture of its own for a spot, and for empty slots.
static bool g_hasEmpty;

static int SlotClass(int k)
{
    if (!g_game || k < 0 || k >= 6) return k;
    uint8_t* mem = SaveOf(g_game);
    int cur = mem[0x40] < NCON ? mem[0x40] : 0;
    return g_slot[cur][k].nr ? g_slot[cur][k].occupant : k;
}

using PortraitFn = uint32_t(__fastcall*)(int);
static PortraitFn o_portrait;
static uint32_t __fastcall h_portrait(int cls)
{
    int c = cls;
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(BAR_ROW_PORTRAIT_RET) && cls >= 0 && cls < 6 && BarListShown()) {
        __try {
            c = SlotClass(cls);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(INN_PORTRAIT_RET) && cls >= 0 && cls < 6) {
        bool empty = false;
        __try {
            c = SlotClass(cls);
            empty = g_hasEmpty && g_page > 0 && c == cls;  // a later page with no class in this row
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (empty) return IceHash("MOD_EMPTY_PlIcon");
    }
    if (g_logBinds) Log("portrait class %d (slot %d) from %p", c, cls, (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));
    if (c >= 6 && c < MAX_CLASSES && (g_portrait[c] & 1)) return NameHash("MOD_PlIcon_%d", c);
    // no portrait of its own: the empty picture (the game has none past class 5)
    if (c >= 6 && c < MAX_CLASSES && g_defined[c])
        return g_hasEmpty ? IceHash("MOD_EMPTY_PlIcon") : o_portrait(g_baseOf[c]);
    return o_portrait(c < 6 ? c : cls);
}

// 0xCCD0E0 (fastcall(class) -> animation hash, PlFace_<n>): the big face in the bar
// party menu's status box, looked up by each member's real class (0xA94408). The
// game has none past class 5 (returns 0: empty box, or the last face left showing),
// so an extra class shows its own face (bar_face_art) or the empty picture.
static const uint32_t BIG_FACE = 0xCCD0E0;
static PortraitFn o_bigFace;
static uint32_t __fastcall h_bigFace(int cls)
{
    if (cls >= 6 && cls < MAX_CLASSES && g_defined[cls]) {
        if (g_barFace[cls]) return NameHash("MOD_PlFace_%d", cls);
        return g_hasEmpty ? IceHash("MOD_EMPTY_PlFace") : o_bigFace(g_baseOf[cls]);
    }
    return o_bigFace(cls);
}



// Diagnostics: sprite objects created by file name (0xE093C0: thiscall(owner, file, a, b), ret 0xC)
static bool g_logSpriteFiles = false;  // diagnostics: log sprite files opened by name
using MakeSpriteFn = void*(__fastcall*)(void*, void*, const char*, int, int);
static MakeSpriteFn o_makeSprite;
static void* __fastcall h_makeSprite(void* owner, void* edx, const char* file, int a, int b)
{
    if (g_logSpriteFiles) {
        static char seen[64][48];
        static int n;
        bool dup = false;
        __try {
            for (int i = 0; i < n && !dup; i++) dup = !strncmp(seen[i], file ? file : "", 47);
            if (!dup) {
                if (n < 64) strncpy_s(seen[n++], file ? file : "", 47);
                Log("sprite file %s from %p", file ? file : "-", (void*)((uintptr_t)_ReturnAddress() - (uintptr_t)Game(0)));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // an extra class's own effect / weapon file (classfiles.cpp); the original if it isn't loaded
    if (const char* own = ClassFileFor(owner, file)) {
        if (void* r = o_makeSprite(owner, edx, own, a, b)) return r;
        static volatile LONG missing;
        if (InterlockedIncrement(&missing) <= 5) Log("classes: %s isn't loaded; %s used", own, file);
    }
    return o_makeSprite(owner, edx, file, a, b);
}


static bool g_hudFace[MAX_CLASSES];  // has its own quest HUD faces (MOD_HudFace_<class>_1/_2)
static bool g_hudFrame[MAX_CLASSES];  // has its own quest HUD life frame (MOD_HudFrame_<class>)

using InnStepFn = uint8_t(__fastcall*)(void*, void*, void*, void*);
static InnStepFn o_innStep;
static uint8_t __fastcall h_innStep(void* scene, void* edx, void* a1, void* frame)
{
    static bool wasL, wasR;
    bool l = PageKey(-1), r = PageKey(1);
    __try {
        int state = *(int*)((uint8_t*)frame + 0x18);
        uint8_t* so = *(uint8_t**)((uint8_t*)frame + 0x10);
        g_innStepAt = GetTickCount();
        g_innStepState = state;
        static int lastState = -1;
        if (state != lastState && g_logSounds) {
            Log("inn state %X (mode %d, rows %p)", state, so ? so[0x34] : -1, so ? *(void**)(so + 0x2C) : nullptr);
            lastState = state;
        }
        if (state == 0x95) g_innOpenPage = -1;  // switching: keep the page
        bool list = g_pages > 1 && state == 0xB3 && so && so[0x34] == 0 && *(void**)(so + 0x2C);
        if (list) {
            if (g_innOpenPage < 0) {
                g_innOpenPage = g_page;
                InnBar(true);  // the bar may have been built before the list showed: add the page prompt
            }
            if (((l && !wasL) || (r && !wasR)) && FindGame()) {
                PlayMenuSound(SOUND_PAGE);
                SwapToPage((g_page + (r && !wasR ? 1 : g_pages - 1)) % g_pages);
                PageTexts();
                InnRefresh((uint8_t*)scene, so);
                InnBar(false);
                Log("classes: inn page %d of %d", g_page + 1, g_pages);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: inn page change failed");
    }
    wasL = l;
    wasR = r;
    return o_innStep(scene, edx, a1, frame);
}

// Return from the list: back to the page it opened on (the current character's).
static void InnBack()
{
    if (g_innOpenPage >= 0 && g_innOpenPage != g_page && FindGame()) {
        SwapToPage(g_innOpenPage);
        PageTexts();
        Log("classes: inn closed, back to page %d", g_page + 1);
    }
    g_innOpenPage = -1;
}

using CurClassFn = int(__fastcall*)(void*);
static CurClassFn o_curClass;
static int __fastcall h_curClass(void* game)
{
    int c = o_curClass(game);
    // Confirm on another page: the row may be the current character's slot, but
    // it now holds another class, so let the game switch to it
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(INN_CUR_RET) && g_innOpenPage >= 0 && g_page != g_innOpenPage)
        return 0x7F;
    // the bar lobby, after its Character screen closes, compares the current slot with
    // the one it opened with (0xD93250) and only then refreshes and sends our record
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(BAR_CUR_RET) && g_barForceRefresh) {
        g_barForceRefresh = false;
        Log("classes: bar picked another page's class in the same slot, refreshing the lobby");
        return 0x7F;
    }
    return c;
}

using CanSwitchFn = uint8_t(__fastcall*)(void*, void*, void*, int);
static CanSwitchFn o_canSwitch;
static uint8_t __fastcall h_canSwitch(void* game, void* edx, void* con, int slot)
{
    // a row with no class on this page can't be picked (buzzer)
    if ((uintptr_t)_ReturnAddress() == (uintptr_t)Game(INN_CAN_RET) && g_page > 0 && slot >= 0 && slot < 6 &&
        !(6 * g_page + slot < MAX_CLASSES && g_defined[6 * g_page + slot]))
        return 0;
    return o_canSwitch(game, edx, con, slot);
}

using BarStepFn = uint8_t(__fastcall*)(void*, void*, void*, void*);
static BarStepFn o_barStep;

// After a page change the bar screen is built again (state 0x15D), which puts the cursor on
// the current character's row (0xD9CCA3). Put it back on the row it was on, the way a cursor
// move does (0xD9D199): row highlight 0xAA05C0(rows panel, row), status panel
// 0xAAE820(status panel, the row's slot).
static const uint32_t BAR_ROW_HILITE = 0xAA05C0;
static int g_barKeepRow = -1;
static void BarKeepRow(uint8_t* data)
{
    int keep = g_barKeepRow;
    g_barKeepRow = -1;
    if (!data) return;
    int rows = data[0x1D];
    if (rows <= 0) return;
    int row = keep < rows ? keep : rows - 1;
    if (row == data[0x1C]) return;
    data[0x1C] = (uint8_t)row;
    void* rowsPanel = *(void**)(data + 0x14);
    void* status = *(void**)(data + 0x18);
    if (rowsPanel) ((void(__thiscall*)(void*, int))Game(BAR_ROW_HILITE))(rowsPanel, row);
    if (status) ((void(__thiscall*)(void*, int))Game(INN_STATUS))(status, *(int*)(data + 0x28 + 4 * row));
}
static uint8_t __fastcall h_barStep(void* scene, void* edx, void* a1, void* frame)
{
    static bool wasL, wasR;
    bool l = PageKey(-1), r = PageKey(1);
    __try {
        int* state = (int*)((uint8_t*)frame + 0x18);
        g_barStepAt = GetTickCount();
        g_barState = *state;
        if (*state == 0) {  // opening
            g_barOpenPage = g_page;
            g_barConfirmed = false;
            g_barForceRefresh = false;
        }
        if (g_pages > 1 && *state == 0x163 && ((l && !wasL) || (r && !wasR)) && FindGame()) {
            uint8_t* data = *(uint8_t**)((uint8_t*)frame + 0x10);
            g_barKeepRow = data ? data[0x1C] : -1;  // the rebuild puts the cursor on the current character
            PlayMenuSound(SOUND_PAGE);
            SwapToPage((g_page + (r && !wasR ? 1 : g_pages - 1)) % g_pages);
            PageTexts();
            *state = 0x15D;  // build the screen again for this page
            g_barState = 0x15D;
            InnBar(false);  // the page prompt shows the new page number
            Log("classes: bar page %d of %d", g_page + 1, g_pages);
        }
        if ((*state == 0x165 || *state == -1) && g_barOpenPage >= 0) {
            // picked a class on another page: maybe the same slot as before, which the
            // lobby would take for "no change" and not show or send
            g_barForceRefresh = g_barConfirmed && g_page != g_barOpenPage;
            if (!g_barConfirmed && g_page != g_barOpenPage && FindGame()) {
                SwapToPage(g_barOpenPage);
                PageTexts();
                Log("classes: bar character screen closed, back to page %d", g_page + 1);
            }
            g_barOpenPage = -1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("classes: bar page change failed");
    }
    wasL = l;
    wasR = r;
    uint8_t ret = o_barStep(scene, edx, a1, frame);
    if (g_barKeepRow >= 0) {
        __try {
            if (*(int*)((uint8_t*)frame + 0x18) != 0x15D) BarKeepRow(*(uint8_t**)((uint8_t*)frame + 0x10));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_barKeepRow = -1;
            Log("classes: bar could not keep the cursor row");
        }
    }
    return ret;
}

// Diagnostics: the bar lobby's member rows (0xA97110: thiscall(hud, member), ret 4). Each
// row keeps a copy of the member's record at hud+0x4C8+0x3E8*member: +0xD bit 0 present,
// +8 class, +0x5C name. Logged when a row changes (a guest's name went blank online).
static const uint32_t LOBBY_ROW = 0xA97110;
using LobbyRowFn = void(__fastcall*)(void*, void*, int);
static LobbyRowFn o_lobbyRow;
static void __fastcall h_lobbyRow(void* hud, void* edx, int m)
{
    __try {
        static uint8_t seen[4][0x60];
        if (m >= 0 && m < 4) {
            const uint8_t* row = (const uint8_t*)hud + 0x4C8 + 0x3E8 * m;
            uint8_t now[0x60];
            memcpy(now, row, 0x10);
            memcpy(now + 0x10, row + 0x5C, 0x40);
            if (memcmp(now, seen[m], 0x50)) {
                memcpy(seen[m], now, 0x50);
                char hex[3 * 24 + 1] = "";
                for (int i = 0; i < 24; i++) sprintf_s(hex + 3 * i, 4, "%02X ", row[0x5C + i]);
                Log("lobby row %d: present %d class %d name bytes %s", m, row[0xD] & 1, row[8], hex);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_lobbyRow(hud, edx, m);
}

// Save file location (from the game's own open), and the testing redirect.
static HANDLE WINAPI h_cfw(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t)
{
    size_t n = name ? wcslen(name) : 0;
    if (n >= 14 && n < MAX_PATH - 16 && !_wcsicmp(name + n - 14, L"SystemData.bin")) {
        if (!g_sidecar[0]) {
            swprintf_s(g_sidecar, L"%s.classes", name);
            MemoryBarrier();
            ReadSidecarOnce();  // now, or when the extra classes are set up
        }
        if (g_redirect && (acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))) {
            wchar_t alt[MAX_PATH + 16];
            swprintf_s(alt, L"%s.class-test", name);
            static int logged;
            if (logged++ < 3) Log("classes: testing: save write redirected to SystemData.bin.class-test");
            return o_cfw(alt, acc, share, sa, disp, fl, t);
        }
    }
    return o_cfw(name, acc, share, sa, disp, fl, t);
}

// The game reads its save within a couple of seconds of starting, sometimes before the
// class hooks are in, so this goes in first thing (it doesn't need the game code).
bool SaveWatchInit()
{
    void* w = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileW");
    bool ok = w && MH_CreateHook(w, (void*)h_cfw, (void**)&o_cfw) == MH_OK && MH_EnableHook(w) == MH_OK;
    Log(ok ? "classes: watching for the save file" : "classes: could not watch for the save file; extra classes' saves won't load");
    return ok;
}

static bool PageHooks()
{
    return o_cfw &&
           HookAt(CLASS_RECORD, h_record, (void**)&o_record) && HookAt(SAVE_CRYPT, (void*)h_crypt, &o_crypt) &&
           HookAt(INFO_BUILD, h_info, (void**)&o_info) && CodeAt(PAL_BIND, "558bec6aff68") &&
           HookAt(PAL_BIND, (void*)h_palBind, &o_palBind) && HookAt(SELECT_STEP, h_step, (void**)&o_step) &&
           HookAt(SEL_PREVIEW, h_preview, (void**)&o_preview) && HookAt(SEL_INFOBOX, h_infobox, (void**)&o_infobox) &&
           HookAt(SEL_HUD, h_hud, (void**)&o_hud) && HookAt(PAL_LOAD, h_palLoad, (void**)&o_palLoad) &&
           HookAt(BODY_NAMES, h_bodyNames, &o_bodyNames) &&
           CodeAt(PICTURE, "558bec6aff68") && CodeAt(PICTURE + 0x1C1, "39bbc0000000") &&
           HookAt(PICTURE, h_picture, (void**)&o_picture) && CodeAt(GAME_ACTION, "558bec5180b994020000") &&
           HookAt(GAME_ACTION, h_gameAction, (void**)&o_gameAction) &&
           HookAt(SET_ANIM, h_setAnim, (void**)&o_setAnim) && HookAt(PROMPTS, (void*)h_prompts, &o_prompts) &&
           HookAt(PLAY_SOUND, h_playSound, (void**)&o_playSound) && HookAt(INN_STEP, h_innStep, (void**)&o_innStep) &&
           HookAt(CUR_CLASS, h_curClass, (void**)&o_curClass) && HookAt(CAN_SWITCH, h_canSwitch, (void**)&o_canSwitch) &&
           HookAt(BAR_ADD, h_barAdd, (void**)&o_barAdd) && HookAt(CLASS_OWNED, h_owned, (void**)&o_owned) &&
           HookAt(PORTRAIT, (void*)h_portrait, (void**)&o_portrait) && CodeAt(BIG_FACE, "64a12c000000") &&
           HookAt(BIG_FACE, (void*)h_bigFace, (void**)&o_bigFace) &&
           HookAt(0xAB92A0, h_boxBuild, (void**)&o_boxBuild) && HookAt(MAKE_SPRITE, h_makeSprite, (void**)&o_makeSprite) &&
           CodeAt(BAR_STEP, "558bec6aff68") && HookAt(BAR_STEP, h_barStep, (void**)&o_barStep) &&
           CodeAt(LOBBY_ROW, "558bec6aff68") && HookAt(LOBBY_ROW, h_lobbyRow, (void**)&o_lobbyRow);
}

// ---- class list ----

static const char* Keep(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)HeapAlloc(GetProcessHeap(), 0, n);  // never freed: the game keeps the pointer
    memcpy(p, s, n);
    return p;
}

static const char* Field(const char* line, const char* key, char* out, size_t n)
{
    char pat[32];
    sprintf_s(pat, " %s=", key);
    const char* p = strstr(line, pat);
    if (!p) return nullptr;
    p += strlen(pat);
    size_t k = 0;
    while (p[k] && p[k] != ' ' && p[k] != '\t' && p[k] != '\r' && p[k] != '\n' && k + 1 < n) { out[k] = p[k]; k++; }
    out[k] = 0;
    return out;
}

bool ClassesInit()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    strcat_s(path, "\\DataMod\\classes.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return true;  // no extra classes: leave the game alone

    for (int t = 0; t < NTAB; t++) {
        const uintptr_t* game = (const uintptr_t*)Game(CLASS_TABLES[t]);
        for (int i = 0; i < MAX_CLASSES; i++) g_tab[t][i] = game[i < 6 ? i : 0];  // unknown classes act like the Empress
    }
    const int BODY = TableIndex(BODY_TABLE), PAL = TableIndex(PAL_TABLE);
    if (int t = TableIndex(PLINFO_TABLE); t >= 0)
        for (int k = 0; k < 6; k++) g_plinfoOrig[k] = g_tab[t][k];
    int count = 0;
    char line[512], v[128];
    while (fgets(line, sizeof line, f)) {
        if (char* c = strchr(line, '#')) *c = 0;
        int cls = atoi(line);
        if (cls < 6 || cls >= MAX_CLASSES) continue;
        char padded[520];
        sprintf_s(padded, " %s", line);
        int base = Field(padded, "base", v, sizeof v) ? atoi(v) : 0;
        g_baseOf[cls] = base;
        if (base < 0 || base > 5) base = 0;
        for (int t = 0; t < NTAB; t++) g_tab[t][cls] = g_tab[t][base];
        if (Field(padded, "body", v, sizeof v)) g_tab[BODY][cls] = (uintptr_t)Keep(v);
        ClassFilesSetup(cls, base, (const char*)g_tab[BODY][cls]);  // its own effect / weapon files (classfiles.cpp)
        if (Field(padded, "palette", v, sizeof v)) g_tab[PAL][cls] = (uintptr_t)Keep(v);
        // its own pause-menu sheets (sub_quest_13xx, sub_status_13xx). NOT READY: a test copy
        // for class 6 crashed the pause menu's Quest page (the sheet wasn't loaded there)
        if (Field(padded, "quest_hud", v, sizeof v)) {
            const char* f = Keep(v);
            static const uint32_t questTables[] = {0x1033D0C, 0x1117294};
            for (uint32_t tv : questTables)
                if (TableIndex(tv) >= 0) g_tab[TableIndex(tv)][cls] = (uintptr_t)f;
        }
        if (Field(padded, "status_hud", v, sizeof v) && TableIndex(0x1033D24) >= 0)
            g_tab[TableIndex(0x1033D24)][cls] = (uintptr_t)Keep(v);
        if (Field(padded, "test_saves", v, sizeof v)) g_redirect = atoi(v) != 0;
        if (Field(padded, "id", v, sizeof v)) g_classId[cls] = Keep(v);
        if (Field(padded, "was", v, sizeof v)) g_classWas[cls] = atoi(v);
        g_defined[cls] = true;
        if (Field(padded, "info", v, sizeof v)) {
            g_info[cls] = Keep(v);
            int it = TableIndex(PLINFO_TABLE);  // its own entry too (the new-character box asks by class)
            if (it >= 0) g_tab[it][cls] = (uintptr_t)g_info[cls];
        }
        if (Field(padded, "highlight", v, sizeof v)) g_hasHighlight[cls] = atoi(v) != 0;
        if (Field(padded, "name", v, sizeof v)) {
            g_name[cls] = Keep(v);
            int nt = TableIndex(CLASS_NAME_TABLE);
            if (nt >= 0) g_tab[nt][cls] = (uintptr_t)g_name[cls];
        }
        if (Field(padded, "portrait", v, sizeof v)) g_portrait[cls] = atoi(v);
        if (Field(padded, "hudface", v, sizeof v)) g_hudFace[cls] = atoi(v) != 0;
        if (Field(padded, "hudframe", v, sizeof v)) g_hudFrame[cls] = atoi(v) != 0;
        if (Field(padded, "barface", v, sizeof v)) g_barFace[cls] = atoi(v) != 0;
        if (Field(padded, "empty", v, sizeof v) && atoi(v)) g_hasEmpty = true;
        if (Field(padded, "pagelayout", v, sizeof v) && atoi(v)) g_hasPageLayout[cls / 6] = true;
        if (!g_extra[cls])
            g_extra[cls] = (ExtraData*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ExtraData));
        if (cls / 6 + 1 > g_pages) g_pages = cls / 6 + 1;
        Log("classes: class %d = base %d, body %s, palette %s", cls, base, (const char*)g_tab[BODY][cls],
            (const char*)g_tab[PAL][cls]);
        count++;
    }
    fclose(f);
    if (!count) return true;

    int patched = 0, skipped = 0;
    for (const Site& s : SITES) {
        int t = TableIndex(s.table);
        if (t < 0 || !SiteOk(s)) {
            skipped++;
            continue;
        }
        uint32_t a = (uint32_t)(uintptr_t)g_tab[t];
        Write(s.va + s.opLen, &a, 4);
        patched++;
    }
    if (skipped) Log("classes: %d list read(s) not as expected, left alone", skipped);

    // Drawing (0xD66730) finds a class's palette by a pair of name hashes per
    // class, cached in writable memory at 0x28ED298 and read as [class*8]:
    // hash("plN_pal.osb") = which palette file, hash("plN_cb00.osp") = the ID
    // the palette must carry. Our copy has a pair for every class number.
    for (int i = 0; i < MAX_CLASSES; i++) {
        const char* pal = (const char*)g_tab[PAL][i];
        g_pair[i][0] = IceHash(pal);
        char osp[160];
        if (i < 6) {
            strcpy_s(osp, (const char*)Game(OSP_NAME[i]));
        } else {
            strcpy_s(osp, (const char*)g_tab[BODY][i]);
            if (char* dot = strrchr(osp, '.')) *dot = 0;
            strcat_s(osp, ".osp");
        }
        g_pair[i][1] = IceHash(osp);
    }
    if (int t = TableIndex(0x111A1A4); t >= 0)
        for (int i = 0; i < MAX_CLASSES; i++) {
            g_preloadOrig[i] = (PreloadFn)g_tab[t][i];
            g_tab[t][i] = (uintptr_t)&h_preloadLog;
        }
    for (const PairSite& s : PAIR_SITES) {
        if (!CodeAt(s.va, s.op) || *(uint32_t*)(Game(s.va) + 3) != Rel(s.table)) {
            Log("classes: palette pair read at %#x not as expected, left alone", s.va);
            continue;
        }
        uint32_t a = (uint32_t)(uintptr_t)&g_pair[0][s.second ? 1 : 0];
        Write(s.va + 3, &a, 4);
        patched++;
    }
    const uint8_t cmpEsi[] = {0x83, 0xFE, MAX_CLASSES - 1}, cmpEdi[] = {0x83, 0xFF, MAX_CLASSES - 1};
    Write(SPRITE_CMP, cmpEsi, 3);
    Write(SPRITE_WRAP_CMP, cmpEdi, 3);
    const uint8_t setupMax = MAX_CLASSES - 1;  // the validator would turn class 6 into 5
    Write(SETUP_CHECK + 2, &setupMax, 1);
    // Class-name lookups (list 0x11022C4) check "class < 6, else entry 0" first; with the
    // longer list, classes up to 126 have their own entry (Status menu, title info box, ...)
    static const struct { uint32_t va; const char* op; } NAME_BOUNDS[] = {
        {0xAB016F, "83fb06"}, {0xAB301D, "83f906"}, {0xAB9353, "83f806"}, {0xCCC0AF, "83f806"}, {0xAAEBC3, "83fe06"}};
    // The quest HUD: per class {life frame, face 01, face 02} animation names at 0x28E9878
    // (12 bytes a class, Empress .. Bandit). 0xCD1B50 ("class < 6" at 0xCD1B6F) puts the
    // frame (read 0xCD1BE4) on part PLPlateShake and face 01 (0xCD1C19) on PLFace; the
    // party list (0xCD214A) uses face 02. A longer copy: an extra class has its own
    // frame and faces, or the empty ones.
    static uint32_t hud[MAX_CLASSES][3];
    static const char* HUD_NAMES[6] = {"\xe7\x9a\x87\xe5\xa5\xb3", "\xe6\x88\xa6\xe5\xa3\xab", "\xe5\xbf\x8d\xe8\x80\x85", "\xe9\xad\x94\xe5\xa5\xb3", "\xe5\xb7\xab\xe5\xa5\xb3", "\xe7\x9b\x97\xe8\xb3\x8a"};  // Empress .. Bandit (UTF-8)
    for (int c = 0; c < MAX_CLASSES; c++) {
        int b = c < 6 ? c : (g_defined[c] ? g_baseOf[c] : 0);
        char n[64];
        sprintf_s(n, "%s" "\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x95", HUD_NAMES[b]);  // <name> life frame
        hud[c][0] = IceHash(n);
        sprintf_s(n, "%s" "\xe9\xa1\x94" "01", HUD_NAMES[b]);  // <name> face 01 (large, own HUD)
        hud[c][1] = IceHash(n);
        sprintf_s(n, "%s" "\xe9\xa1\x94" "02", HUD_NAMES[b]);  // <name> face 02 (small, party list)
        hud[c][2] = IceHash(n);
        if (c >= 6 && g_defined[c] && g_hudFace[c]) {
            hud[c][1] = NameHash("MOD_HudFace_%d_1", c);
            sprintf_s(n, "MOD_HudFace_%d_2", c);
            hud[c][2] = IceHash(n);
        } else if (c >= 6 && g_defined[c] && g_hasEmpty) {  // no faces of its own: the empty picture
            hud[c][1] = IceHash("MOD_EMPTY_HudFace_1");
            hud[c][2] = IceHash("MOD_EMPTY_HudFace_2");
        }
        if (c >= 6 && g_defined[c] && g_hudFrame[c])
            hud[c][0] = NameHash("MOD_HudFrame_%d", c);
        else if (c >= 6 && g_defined[c] && g_hasEmpty)  // no frame of its own: the grey frame
            hud[c][0] = IceHash("MOD_EMPTY_HudFrame");
    }
    static const struct { uint32_t va, table; int field; } HUD_READS[] = {
        {0xCD1BE4, 0x28E9878, 0}, {0xCD1C19, 0x28E987C, 1}, {0xCD214A, 0x28E9880, 2}};
    bool hudOk = CodeAt(0xCD1B6F, "83fb06");
    for (const auto& r : HUD_READS) hudOk = hudOk && CodeAt(r.va, "ff34b5") && *(uint32_t*)Game(r.va + 3) == Rel(r.table);
    if (hudOk) {
        for (const auto& r : HUD_READS) {
            uint32_t a = (uint32_t)(uintptr_t)&hud[0][r.field];
            Write(r.va + 3, &a, 4);
        }
        const uint8_t hudMax = MAX_CLASSES - 1;
        Write(0xCD1B6F + 2, &hudMax, 1);
    } else {
        Log("classes: quest HUD name reads not as expected, left alone");
    }
    PatchOracleTests();
    PatchEquipTests();
    PatchWeaponSlots();
    if (CodeAt(NEWCHAR_TUTORIAL, "807f14000f85f4fdffff") && CodeAt(NEWCHAR_TOWN, "807f14000f854afeffff")) {
        g_ncTown = (uintptr_t)Game(NEWCHAR_TOWN);
        g_ncBack = (uintptr_t)Game(NEWCHAR_BACK);
        g_ncTutorial = (uintptr_t)Game(NEWCHAR_TUTORIAL_GO);
        uint8_t buf[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)&h_newCharDest - (uintptr_t)Game(NEWCHAR_TUTORIAL + 5));
        Write(NEWCHAR_TUTORIAL, buf, 10);
    } else {
        Log("classes: new-character destination not as expected, left alone");
    }
    if (CodeAt(WARRIOR_BODY, "83fe0175113bde750d")) {
        g_warriorYes = (uintptr_t)Game(WARRIOR_BODY + 9);
        g_warriorNo = (uintptr_t)Game(0xD79E1D);
        uint8_t buf[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)&h_warriorBody - (uintptr_t)Game(WARRIOR_BODY + 5));
        Write(WARRIOR_BODY, buf, 9);
    } else {
        Log("classes: Warrior greatsword body test not as expected, left alone");
    }
    if (CodeAt(GROWTH_CLASS, "8b87a4000000") && CodeAt(GROWTH_CLASS + 6, "0fb67508")) {
        uint8_t buf[6] = {0xE8, 0, 0, 0, 0, 0x90};
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)&h_growthClass - (uintptr_t)Game(GROWTH_CLASS + 5));
        Write(GROWTH_CLASS, buf, 6);
    } else {
        Log("classes: stat growth read not as expected, left alone");
    }
    if (CodeAt(BASESTAT_CLASS, "8b86a4000000") && CodeAt(BASESTAT_CLASS + 6, "8b4dfc")) {
        uint8_t buf[6] = {0xE8, 0, 0, 0, 0, 0x90};
        *(int32_t*)(buf + 1) = (int32_t)((uintptr_t)&h_baseStatClass - (uintptr_t)Game(BASESTAT_CLASS + 5));
        Write(BASESTAT_CLASS, buf, 6);
    } else {
        Log("classes: base stat read not as expected, left alone");
    }
    if (CodeAt(COLOUR_REC - 5, "83fe067211") && CodeAt(COLOUR_REC, "0fb642400faf4234")) {
        g_colourRecDone = (uintptr_t)Game(COLOUR_REC_DONE);
        uint8_t jmp[5] = {0xE9};
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)&h_colourRecord - (uintptr_t)Game(COLOUR_REC + 5));
        Write(COLOUR_REC, jmp, 5);
    } else {
        Log("classes: COLOR record lookup not as expected, left alone");
    }
    // the info panel's layout call (see InfoLayout)
    if (*Game(INFO_LAYOUT_CALL) == 0xE8 &&
        (uintptr_t)Game(INFO_LAYOUT_CALL_RET) + *(int32_t*)Game(INFO_LAYOUT_CALL + 1) == (uintptr_t)Game(SET_ANIM)) {
        uint8_t call[5] = {0xE8};
        *(int32_t*)(call + 1) = (int32_t)((uintptr_t)&InfoLayout - (uintptr_t)Game(INFO_LAYOUT_CALL_RET));
        Write(INFO_LAYOUT_CALL, call, 5);
    } else {
        Log("classes: select info panel layout call not as expected, left alone");
    }
    // the select preview's own record lookup (see h_previewRecord)
    if (CodeAt(PREVIEW_REC - 5, "83fe067216") && CodeAt(PREVIEW_REC, "8b48040fb64140")) {
        g_previewRecDone = (uintptr_t)Game(PREVIEW_REC_DONE);
        uint8_t jmp[5] = {0xE9};
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)&h_previewRecord - (uintptr_t)Game(PREVIEW_REC + 5));
        Write(PREVIEW_REC, jmp, 5);
    } else {
        Log("classes: select preview record lookup not as expected, left alone");
    }
    // body files named directly by the Witch / Shinobi code (see PlayerBody)
    g_bodyTab = BODY;
    static const struct { uint32_t va; uint8_t op; uint32_t str; const char** keep; void* stub; } BODY_REFS[] = {
        {0xD27535, 0xB9, 0x110B4F0, &g_witchBody, (void*)&h_witchBody},
        {0xD2A1E4, 0x68, 0x110C080, &g_shinobiBody, (void*)&h_shinobiBody}};
    for (const auto& r : BODY_REFS) {
        if (*Game(r.va) != r.op || *(uint32_t*)Game(r.va + 1) != Rel(r.str)) {
            Log("classes: body name at %08X not as expected, left alone", r.va);
            continue;
        }
        *r.keep = (const char*)Game(r.str);
        uint8_t call[5] = {0xE8};
        *(int32_t*)(call + 1) = (int32_t)((uintptr_t)r.stub - (uintptr_t)Game(r.va + 5));
        Write(r.va, call, 5);
    }
    const uint8_t nameMax = MAX_CLASSES - 1;
    for (const auto& nb : NAME_BOUNDS) {
        if (CodeAt(nb.va, nb.op)) Write(nb.va + 2, &nameMax, 1);
        else Log("classes: class-name bound at %#x not as expected, left alone", nb.va);
    }
    // The quest HUD's face for each party member (0xAB2254: "class <= 6" then a list of
    // PlIcon_<n>_24 hashes at 0x28E7200): a longer list, an extra class's own large
    // portrait (MOD_PlIcon_<class>_24) or its base class's face.
    static uint32_t large[MAX_CLASSES];
    if (CodeAt(0xAB224F, "83f806") && CodeAt(0xAB2254, "8b3485") && *(uint32_t*)Game(0xAB2257) == Rel(0x28E7200)) {
        for (int c = 0; c < MAX_CLASSES; c++) {  // the game's names, computed (it fills its list later)
            int b = c < 6 ? c : (g_defined[c] ? g_baseOf[c] : 0);
            large[c] = c >= 6 && g_defined[c] && (g_portrait[c] & 2) ? NameHash("MOD_PlIcon_%d_24", c)
                     : c >= 6 && g_defined[c] && g_hasEmpty        ? IceHash("MOD_EMPTY_PlIcon_24")
                                                                    : NameHash("PlIcon_%d_24", b + 1);
        }
        uint32_t a = (uint32_t)(uintptr_t)large;
        Write(0xAB2257, &a, 4);
        const uint8_t largeMax = MAX_CLASSES - 1;
        Write(0xAB224F + 2, &largeMax, 1);
    } else {
        Log("classes: quest HUD face lookup not as expected, left alone");
    }
    bool pages = PageHooks();
    if (!pages) Log("classes: select-page hooks failed; extra classes can't be picked");
    g_extraReady = true;
    MemoryBarrier();
    ReadSidecarOnce();  // if the game already opened its save
    Log("classes: %d extra class(es), %d tables, %d list reads extended; %d select page(s)%s", count, NTAB, patched,
        g_pages, g_redirect ? "; testing: game save writes go to SystemData.bin.class-test" : "");
    // Testing without a second player: <game>/dmfd_test_remote.txt holding a base class
    // number (0-5) acts as if another player in the room were that class.
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    strcat_s(path, "\\dmfd_test_remote.txt");
    if (FILE* t = nullptr; !fopen_s(&t, path, "r") && t) {
        int k = -1;
        if (fscanf_s(t, "%d", &k) == 1 && k >= 0 && k < 6) {
            g_remoteHas[k] = 1;
            Log("classes: TESTING (dmfd_test_remote.txt): acting as if another player were class %d", k);
        }
        fclose(t);
    }
    // <game>\dmfd_debug.txt (any content): log palette loads/binds and sprite files.
    strcpy_s(strrchr(path, '\\'), 16, "\\dmfd_debug.txt");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        g_logBinds = g_logSpriteFiles = g_logSounds = g_logActions = true;
        Log("classes: DEBUG LOGGING on (dmfd_debug.txt)");
    }
    ClassFilesInit();
    return true;
}
