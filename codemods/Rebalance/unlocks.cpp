// Oracle and Bandit (classes 4 and 5) unlock through two account flags in the
// save (bits 0x2 and 0x8 at +58h), which the game sets once a character reaches
// the unlock point. With unlock_classes on, we set them whenever the game is
// about to look. Once the game saves, the unlock is part of the save file.
#include "mod.h"
#include "MinHook.h"

static uintptr_t g_base;

static const uint32_t GET_GAME = 0x7C9770;
static const uint32_t GET_SAVE_OBJ = 0xD83C00;     // thiscall on the game object; returns the object holding +444Ch
static const uint32_t CLASS_AVAILABLE = 0xC92540;  // bool (int classId), reads the flags
static const uint32_t UNLOCK_SCAN = 0xC92630;      // sets the flags from character progress
static const uint32_t CHARA_HUD_CTOR = 0xABCAE0;   // character creation HUD setup, reads the flags directly
static const uint32_t UNLOCK_BITS = 0x2 | 0x8;

volatile LONG g_unlockClasses = 1;

using ClassAvailFn = char(__fastcall*)(uint8_t*, void*, int);
using ScanFn = void(__fastcall*)(uint8_t*, void*);
using HudCtorFn = void*(__fastcall*)(void*, void*, int, int, int, int, int);  // ret 14h: five stack args
static ClassAvailFn o_classAvail;
static ScanFn o_scan;
static HudCtorFn o_hudCtor;

static void SetBits(uint8_t* obj)
{
    if (!g_unlockClasses || !obj) return;
    uint8_t* p = *(uint8_t**)(obj + 0x444C);
    if (p) p = *(uint8_t**)(p + 0x1C);
    if (p) p = *(uint8_t**)(p + 4);
    if (!p) return;
    uint32_t* flags = (uint32_t*)(p + 0x58);
    if ((*flags & UNLOCK_BITS) != UNLOCK_BITS) {
        *flags |= UNLOCK_BITS;
        Log("unlocked Oracle and Bandit");
    }
}

static char __fastcall h_classAvail(uint8_t* obj, void* edx, int cls)
{
    SetBits(obj);
    return o_classAvail(obj, edx, cls);
}

static void __fastcall h_scan(uint8_t* obj, void* edx)
{
    o_scan(obj, edx);
    SetBits(obj);
}

static void* __fastcall h_hudCtor(void* hud, void* edx, int a, int b, int c, int d, int e)
{
    using GetGame = void*(__cdecl*)();
    using GetSave = uint8_t*(__fastcall*)(void*, void*);
    if (g_unlockClasses)
        if (void* g = ((GetGame)G(GET_GAME))())
            SetBits(((GetSave)G(GET_SAVE_OBJ))(g, nullptr));
    return o_hudCtor(hud, edx, a, b, c, d, e);
}

bool UnlocksInit()
{
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    bool ok = HookAt(CLASS_AVAILABLE, h_classAvail, (void**)&o_classAvail) &
              HookAt(UNLOCK_SCAN, h_scan, (void**)&o_scan) &
              HookAt(CHARA_HUD_CTOR, h_hudCtor, (void**)&o_hudCtor);
    Log(ok ? "class unlock hooks installed" : "class unlock hooks failed");
    return ok;
}
