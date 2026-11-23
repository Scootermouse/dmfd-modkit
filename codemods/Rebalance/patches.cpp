// In-memory version of the v0.7 exe patches (tools/patcher/dmfd_rebalance.py).
// Addresses are for the Steam build at its preferred base 0x400000; the exe is
// ASLR-enabled, so G() rebases them at run time.
#include "mod.h"
#include <cstring>
#include <cstdio>

static uintptr_t g_base;

// ---- locations (same as the Python patcher) ----
static const uint32_t ADD_HP = 0xCC1570;  // cActWork::AddHp(delta), vtable slot 33
static const uint32_t PLAYER_VTABLES[] = { 0x111334C, 0x1112C5C, 0x110F9C4, 0x11085F4, 0x110BD58, 0x10DB36C, 0x10DD5C4 };
static const uint32_t HIT_ADDHP_RET = 0xCC6A55;
static const uint32_t BONUS_ADDGOLD_RET = 0xC8E680;
static const uint32_t GET_GAME = 0x7C9770, GET_GAME_OBJ = 0x7B1770;
static const uint32_t DROP_TABLE = 0x136B9FC, DROP_POOL = 0x13E9E44;
static const int DROP_COUNT = 4973, DROP_SIZE = 0x68;

struct Site { uint32_t va; const char* orig; void* stub; };

// ---- runtime values used by the stubs ----
static uintptr_t g_addHp, g_hitAddHpRet, g_bonusRet;
volatile LONG g_lastSolo = -1;
volatile LONG g_soloWhy = -1;
static uint8_t g_rareOrig[DROP_COUNT];

// Same check the game uses for its party-size HP scaling: solo-mode flag, no
// online room, or a room with one player. When the game's state is unknown
// (hub town), we're solo unless we're in a Steam lobby.
static bool IsSolo()
{
    using GetGame = void*(__cdecl*)();
    using GetObj = uint8_t*(__fastcall*)(void* self, void* unused);
    bool solo = false;
    LONG why = SOLO_NO_GAME;
    if (void* g = ((GetGame)G(GET_GAME))()) {
        why = SOLO_NO_OBJ;
        if (uint8_t* o = ((GetObj)G(GET_GAME_OBJ))(g, nullptr)) {
            uint8_t* room = *(uint8_t**)(o + 0x3E8);
            if (o[0x1446]) why = SOLO_FLAG;
            else if (!room) why = SOLO_NO_ROOM;
            else if (!room[0x1A]) why = SOLO_ROOM_IDLE;
            else why = SOLO_ROOM + room[0x18];  // room with N players
            solo = why != SOLO_NO_OBJ && (why < SOLO_ROOM || why <= SOLO_ROOM + 1);
        }
    }
    if ((why == SOLO_NO_GAME || why == SOLO_NO_OBJ) && !NetGet().inRoom) {
        why = SOLO_NOT_ONLINE;
        solo = true;
    }
    g_soloWhy = why;
    g_lastSolo = solo;
    return solo;
}

bool ModIsSolo() { return IsSolo(); }

const char* SoloText(LONG why)
{
    static char buf[48];
    switch (why) {
    case -1: return "not checked yet";
    case SOLO_NO_GAME: case SOLO_NO_OBJ: return "co-op (game state unknown)";
    case SOLO_FLAG: return "solo (solo quest)";
    case SOLO_NO_ROOM: case SOLO_ROOM_IDLE: return "solo (offline)";
    case SOLO_NOT_ONLINE: return "solo (not in an online room)";
    }
    LONG n = why - SOLO_ROOM;
    sprintf_s(buf, "%s (online, %ld player%s)", n <= 1 ? "solo" : "co-op", n, n == 1 ? "" : "s");
    return buf;
}

static int Mul(int v, int32_t fix) { return (int)(((int64_t)v * fix) >> 16); }

static int __stdcall Scale(int kind, int v)
{
    return Mul(v, g_fix[IsSolo()][kind]);
}

static void __stdcall HpStub();

// ---- who hit whom ----
volatile int32_t g_classFix[2][2][CLS_COUNT];

// cPlWork00..05 (PLAYER_VTABLES[1..6]), or -1.
static int PlayerClass(const void* obj)
{
    if (!obj) return -1;
    uintptr_t vt = *(const uintptr_t*)obj;
    for (int c = 0; c < CLS_COUNT; c++)
        if (vt == G(PLAYER_VTABLES[c + 1])) return c;
    return -1;
}

// The hitting object is the player itself or one of its attack objects
// (cPshWork, actor type 3: slashes, shots, spells), which keeps its player at +668h.
static int AttackerClass(uint8_t* atk)
{
    __try {
        if (!atk) return -1;
        int c = PlayerClass(atk);
        if (c < 0 && *(int*)(atk + 0x18) == 3) c = PlayerClass(*(void**)(atk + 0x668));
        return c;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Final hit damage, before the popup and the HP loss. Players take K_TAKEN and
// their class's taken; enemies and bosses take K_DEALT and the attacker's
// class's dealt; then the game's "at least 1" clamp. callerEbp is the hit
// handler's frame, which holds the attacker at [ebp-60h].
static int __stdcall HitDamage(uint8_t* target, int dmg, uint8_t* callerEbp)
{
    bool solo = IsSolo();
    int c = PlayerClass(target);
    if (c >= 0) {
        dmg = Mul(Mul(dmg, g_fix[solo][K_TAKEN]), g_classFix[solo][1][c]);
    }
    else {
        int type = *(int*)(target + 0x18);
        if (type == 1 || type == 2) {
            dmg = Mul(dmg, g_fix[solo][K_DEALT]);
            c = AttackerClass(*(uint8_t**)(callerEbp - 0x60));
            if (c >= 0) {
                static volatile LONG logged[CLS_COUNT];
                int before = dmg;
                dmg = Mul(dmg, g_classFix[solo][0][c]);
                if (!InterlockedExchange(&logged[c], 1))
                    Log("class balance: first %s hit this session, %d -> %d", CLASS_NAMES[c], before, dmg);
            }
        }
    }
    return dmg < 1 ? 1 : dmg;
}

// EXP breakdown on the results screen: both bonus lines scaled, base is what's
// left of the (already scaled) total. Returns what vanilla leaves in eax.
static int __stdcall ExpSplit(uint8_t* quest, uint8_t* res)
{
    int a = Scale(K_EXP, *(int*)(quest + 0x540C));
    int b = Scale(K_EXP, *(int*)(quest + 0x5410));
    *(int*)(res + 0x3020) = a;
    *(int*)(res + 0x3024) = b;
    *(int*)(res + 0x301C) = *(int*)(res + 0x3008) - a - b;
    return b;
}

// Enemy spawn: lower HP-per-VIT (0 = the game's default 25) and the flat HP add.
static int __stdcall SpawnHp(uint8_t* actor, int* flat, int perVit)
{
    int type = *(int*)(actor + 0x18);
    if (type != 1 && type != 2) return perVit;
    bool solo = IsSolo();
    int32_t m = g_fix[solo][type == 2 ? K_BOSS_HP : K_ENEMY_HP];
    if (m == 65536) return perVit;
    if (!perVit) perVit = 25;
    perVit = Mul(perVit, m);
    *flat = Mul(*flat, m);
    return perVit < 1 ? 1 : perVit;
}

// ---- stubs ----
// Player AddHp (vtable slot 33): scale HP loss that didn't come through the
// hit path (already scaled there), then continue into the real AddHp.
static __declspec(naked) void __stdcall HpStub()
{
    __asm {
        mov eax, [esp]
        cmp eax, g_hitAddHpRet
        je go
        mov eax, [esp + 4]
        test eax, eax
        jns go
        push ecx
        push eax
        push K_TAKEN
        call Scale
        pop ecx
        mov [esp + 4], eax
    go:
        jmp dword ptr [g_addHp]
    }
}

// Replaces the min-1 clamp at the end of the damage finalizer (ebx = hit info, [ebp-10h] = target).
static __declspec(naked) void DmgStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [ebp]
        push dword ptr [ebx + 24h]
        push dword ptr [ebp - 10h]
        call HitDamage
        mov [ebx + 24h], eax
        pop edx
        pop ecx
        pop eax
        ret
    }
}

// Replaces mov ecx,[edi+446Ch] (quest EXP total).
static __declspec(naked) void ExpTotalStub()
{
    __asm {
        push eax
        push edx
        push dword ptr [edi + 446Ch]
        push K_EXP
        call Scale
        mov ecx, eax
        pop edx
        pop eax
        ret
    }
}

// Replaces the EXP breakdown block (edi = quest, esi = results); the site adds 154h to edi after.
static __declspec(naked) void ExpSplitStub()
{
    __asm {
        push ecx
        push edx
        push esi
        push edi
        call ExpSplit
        pop edx
        pop ecx
        ret
    }
}

// Replaces mov eax,[ebp+8] ; add [ecx+4170h],eax (in-quest AddGold), skipping the clear bonus.
static __declspec(naked) void GoldStub()
{
    __asm {
        push ecx
        push edx
        mov eax, [ebp + 8]
        mov edx, [ebp + 4]
        cmp edx, g_bonusRet
        je skip
        push eax
        push K_GOLD
        call Scale
    skip:
        pop edx
        pop ecx
        add [ecx + 4170h], eax
        ret
    }
}

// Replaces mov [ebx+5408h],eax (quest clear gold bonus).
static __declspec(naked) void BonusStub()
{
    __asm {
        push ecx
        push edx
        push eax
        push K_GOLD
        call Scale
        pop edx
        pop ecx
        mov [ebx + 5408h], eax
        ret
    }
}

// Replaces push [ebp+2Ch] ; mov ecx,edi at the start of the spawn status setup
// (esi = actor, ebx = HP per VIT, [ebp+20h] = flat HP add).
static __declspec(naked) void SpawnStub()
{
    __asm {
        push eax
        push ecx
        push edx
        lea eax, [ebp + 20h]
        push ebx
        push eax
        push esi
        call SpawnHp
        mov ebx, eax
        pop edx
        pop ecx
        pop eax
        push dword ptr [esp]        ; return address, twice
        push eax
        mov eax, [ebp + 2Ch]
        mov [esp + 8], eax          ; lower copy becomes the replaced push
        pop eax
        mov ecx, edi
        ret
    }
}

static const Site SITES[] = {
    { 0xCC6941, "837b24017d07c7432401000000", (void*)DmgStub },
    { 0xC8FAB9, "8b8f6c440000", (void*)ExpTotalStub },
    { 0xC8FB53, "8b86083000002b87105400002b870c54000089861c3000008b870c540000"
                "8986203000008b871054000081c754010000898624300000", (void*)ExpSplitStub },
    { 0xC90880, "8b4508018170410000", (void*)GoldStub },
    { 0xC8E642, "898308540000", (void*)BonusStub },
    { 0xCC2F18, "ff752c8bcf", (void*)SpawnStub },
};

static int Unhex(const char* h, uint8_t* out)
{
    int n = 0;
    for (; h[0] && h[1]; h += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        out[n++] = (uint8_t)(nib(h[0]) << 4 | nib(h[1]));
    }
    return n;
}

static void Write(uintptr_t at, const void* src, size_t n)
{
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, src, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

static const char* DropName(int i)
{
    return (const char*)G(DROP_POOL) + *(uint32_t*)(G(DROP_TABLE) + DROP_SIZE * i);
}

// Starts of the functions hooked with MinHook (unlocks), up to the
// first relocated operand. Steam's wrapper decrypts the code while our thread
// is already polling, so every one of these must be checked, not just SITES.
static const struct { uint32_t va; const char* orig; } HOOKED[] = {
    { 0xC92540, "558bec518b450883f8050f87" }, { 0xC92630, "558bec83ec088b814c440000" },
    { 0xABCAE0, "558bec6aff68" },
};

bool CodeReady()
{
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    uint8_t buf[64];
    for (auto& s : SITES) {
        int n = Unhex(s.orig, buf);
        if (memcmp((void*)G(s.va), buf, n)) return false;
    }
    for (auto& h : HOOKED) {
        int n = Unhex(h.orig, buf);
        // A jump here means another mod (the content mod) hooked it first;
        // MinHook chains onto it, so both hooks keep working.
        if (memcmp((void*)G(h.va), buf, n) && *(uint8_t*)G(h.va) != 0xE9) return false;
        // The wrapper also leaves the code writable-only until it's done; MinHook
        // refuses non-executable targets, so wait for the protection to come back.
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)G(h.va), &mbi, sizeof mbi) ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
    }
    return true;
}

bool PatchGame()
{
    // Verify everything first so a mismatch changes nothing.
    if (!CodeReady()) {
        Log("game code is not what the mod expects (still using a patched game.exe?); mod disabled");
        return false;
    }
    uint8_t buf[64];
    for (uint32_t vt : PLAYER_VTABLES)
        if (*(uintptr_t*)G(vt + 33 * 4) != G(ADD_HP)) {
            Log("player vtable %#x unexpected; mod disabled", vt);
            return false;
        }
    if (strcmp(DropName(0), "DROP_1_0_0_N9") || strcmp(DropName(DROP_COUNT - 1), "DROP_e_7_7_R255")) {
        Log("drop table check failed; mod disabled");
        return false;
    }

    g_addHp = G(ADD_HP);
    g_hitAddHpRet = G(HIT_ADDHP_RET);
    g_bonusRet = G(BONUS_ADDGOLD_RET);

    for (auto& s : SITES) {
        int n = Unhex(s.orig, buf);
        uint8_t code[64];
        memset(code, 0x90, n);
        code[0] = 0xE8;
        *(int32_t*)(code + 1) = (int32_t)((uintptr_t)s.stub - (G(s.va) + 5));
        if (s.va == 0xC8FB53)  // EXP split: keep add edi,154h after the call
            memcpy(code + 5, "\x81\xc7\x54\x01\x00\x00", 6);
        Write(G(s.va), code, n);
    }
    uintptr_t stub = (uintptr_t)HpStub;
    for (uint32_t vt : PLAYER_VTABLES)
        Write(G(vt + 33 * 4), &stub, 4);

    for (int i = 0; i < DROP_COUNT; i++) {
        uint8_t* e = (uint8_t*)G(DROP_TABLE) + DROP_SIZE * i;
        g_rareOrig[i] = e[4] == 1 ? e[5] : 0;
    }
    ApplyDrops(true);
    Log("patched %d code sites and %d player vtables", (int)(sizeof SITES / sizeof *SITES), (int)(sizeof PLAYER_VTABLES / 4));
    return true;
}

void ApplyDrops(bool solo)
{
    int32_t m = g_fix[solo][K_RARE];
    static int32_t lastM = -1;
    if (m == lastM) return;
    lastM = m;
    int changed = 0;
    DWORD old;
    uint8_t* t = (uint8_t*)G(DROP_TABLE);
    VirtualProtect(t, DROP_SIZE * DROP_COUNT, PAGE_READWRITE, &old);
    for (int i = 0; i < DROP_COUNT; i++) {
        if (!g_rareOrig[i]) continue;
        int v = (int)(((int64_t)g_rareOrig[i] * m + 32768) >> 16);
        int cap = g_rareOrig[i] > 100 ? g_rareOrig[i] : 100;  // never push a chance past 100 (or above its own value if it already is)
        uint8_t nv = (uint8_t)(v > cap ? cap : v);
        if (nv != g_rareOrig[i]) changed++;
        t[DROP_SIZE * i + 5] = nv;
    }
    VirtualProtect(t, DROP_SIZE * DROP_COUNT, old, &old);
    Log("rare drops for %s profile: %d tables changed", solo ? "solo" : "co-op", changed);
}
