// Holy Up: the Empress's Holy shots (fireballs, Dragon Cannon barrage) mark enemies with stacks.
// Each stack lowers the enemy's PIE through the game's own stat-down effect (red PIE number and
// the blue PIE-down badge on the enemy panel) and raises the Holy damage it takes from everyone.
// The chance to add a stack grows with her PIE; barrage shots get a multiplier on it.
//
// How the game works out a hit (addresses at the exe's preferred base; docs/holy-up.md):
// 0xCC4560 picks the damage path and calls one of three functions, all __thiscall on the
// target with 5 stack arguments (hit info, ?, 8 bytes, result) and ret 14h:
//   0xCC4620 physical, 0xCC4E20 magical, 0xCC55E0 Holy.
// Each adds its damage to result+24h. Her fireballs and barrage take the Holy path, so we hook
// that one: add stacks on her hits, and raise the damage of every Holy hit on a marked enemy.
#include "../common/modbase.h"
#include <cstdio>
#include <cmath>
#include "imgui.h"

const char MOD_NAME_STR[] = "Holy Up";
const char MOD_VERSION_STR[] = "0.14";

static const uint32_t HOLY_PATH = 0xCC55E0;
static const uint32_t HIT_ATTACKER = 0x7B1810;  // thiscall on the hit info: the attacking object
static const uint32_t STATUS_CAST = 0x7B3150;   // thiscall on obj+39Ch: its status object, or null
static const uint32_t EMPRESS_VTABLE = 0x1112C5C;  // cPlWork00 (class 0 and classes that borrow her moves)
static const int ATTACK_OBJ_TYPE = 3;           // actor type at +18h of cPshWork (shots, slashes)
static const int BARRAGE_ENTRY = 1;            // attack entry of a barrage shot (entry 0 = fireball)
static const int PIE_WORD = 5;                 // stat words: STR AGI INT VIT LUC PIE

// ---- settings (holyup.ini in the mod's folder) ----
struct Settings {
    float chancePerPie = 0.5f;   // % chance per point of PIE for a fireball hit
    float barrageMult = 2.0f;    // barrage shots: chance times this
    float bonusPerStack = 4.0f;  // % more Holy damage taken per stack
    int maxStacks = 10;
    float seconds = 30.0f;       // a stack's life; each new stack refreshes all of them
    int lowerPie = 1;            // Piety Down: lower the marked enemy's own PIE by this per stack (0 = off)
};
static Settings g_set;
static char g_path[MAX_PATH];

static float ReadF(const char* key, float def)
{
    char buf[32], d[32];
    sprintf_s(d, "%g", def);
    GetPrivateProfileStringA("holyup", key, d, buf, sizeof buf, g_path);
    float v = (float)atof(buf);
    return std::isfinite(v) ? v : def;
}

static void WriteF(const char* key, float v)
{
    char buf[32];
    sprintf_s(buf, "%g", v);
    WritePrivateProfileStringA("holyup", key, buf, g_path);
}

static void LoadSettings()
{
    Settings d;
    g_set.chancePerPie = ReadF("chance_per_pie", d.chancePerPie);
    g_set.barrageMult = ReadF("barrage_chance_mult", d.barrageMult);
    g_set.bonusPerStack = ReadF("bonus_per_stack", d.bonusPerStack);
    g_set.maxStacks = GetPrivateProfileIntA("holyup", "max_stacks", d.maxStacks, g_path);
    g_set.seconds = ReadF("seconds", d.seconds);
    g_set.lowerPie = GetPrivateProfileIntA("holyup", "pie_down_per_stack", d.lowerPie, g_path);
}

static void SaveSettings()
{
    char buf[16];
    WriteF("chance_per_pie", g_set.chancePerPie);
    WriteF("barrage_chance_mult", g_set.barrageMult);
    WriteF("bonus_per_stack", g_set.bonusPerStack);
    sprintf_s(buf, "%d", g_set.maxStacks);
    WritePrivateProfileStringA("holyup", "max_stacks", buf, g_path);
    WriteF("seconds", g_set.seconds);
    sprintf_s(buf, "%d", g_set.lowerPie);
    WritePrivateProfileStringA("holyup", "pie_down_per_stack", buf, g_path);
}

// ---- stacks per enemy ----
struct Mark { void* enemy; int stacks; uint32_t until; void* status; };
static Mark g_marks[64];
static CRITICAL_SECTION g_cs;
static volatile LONG g_lastStacks, g_lastPie = -1, g_stacksAdded, g_holyBoosted;

static uint32_t LifeFrames() { return (uint32_t)(g_set.seconds * 60.0f); }

static Mark* FindMark(void* enemy, bool make)
{
    uint32_t now = Frame();
    Mark* freeSlot = nullptr;
    for (Mark& m : g_marks) {
        if (m.enemy && (int32_t)(m.until - now) <= 0) m.enemy = nullptr;  // expired
        if (m.enemy == enemy) return &m;
        if (!m.enemy && !freeSlot) freeSlot = &m;
    }
    if (!make) return nullptr;
    if (!freeSlot) {  // full: reuse the one that runs out first
        freeSlot = &g_marks[0];
        for (Mark& m : g_marks) if ((int32_t)(m.until - freeSlot->until) < 0) freeSlot = &m;
    }
    freeSlot->enemy = enemy;
    freeSlot->stacks = 0;
    freeSlot->status = nullptr;
    return freeSlot;
}

static int StacksOn(void* enemy)
{
    EnterCriticalSection(&g_cs);
    Mark* m = FindMark(enemy, false);
    int n = m ? m->stacks : 0;
    LeaveCriticalSection(&g_cs);
    return n;
}

// ---- reading the game's objects ----
static uint8_t* Attacker(void* hit)
{
    using Fn = uint8_t*(__fastcall*)(void*, void*);
    return ((Fn)G(HIT_ATTACKER))(hit, nullptr);
}

static uint8_t* StatusOf(uint8_t* obj)
{
    using Cast = uint8_t*(__fastcall*)(void*, void*);
    return ((Cast)G(STATUS_CAST))(*(void**)(obj + 0x39C), nullptr);
}

// The player behind an attacking object: the player itself, or the owner of its shot/slash.
static uint8_t* OwnerPlayer(uint8_t* atk)
{
    if (!atk) return nullptr;
    if (*(uintptr_t*)atk == G(EMPRESS_VTABLE)) return atk;
    if (*(int*)(atk + 0x18) == ATTACK_OBJ_TYPE) {
        uint8_t* p = *(uint8_t**)(atk + 0x668);
        if (p && *(uintptr_t*)p == G(EMPRESS_VTABLE)) return p;
    }
    return nullptr;
}

static int PieOf(uint8_t* player)
{
    uint8_t* st = StatusOf(player);
    if (!st) return 0;
    using Get = const int16_t*(__fastcall*)(void*, void*);
    const int16_t* w = (*(Get*)(*(uintptr_t*)st + 0x24))(st, nullptr);
    return w ? w[PIE_WORD] : 0;
}

static bool IsEnemy(uint8_t* target)
{
    int type = *(int*)(target + 0x18);
    return type == 1 || type == 2;
}

// The attack's entry in its object's attack table: index from hit+38h through the attacker's
// vtable +68h, table at attacker+36Ch (+60h entries of 36 bytes, +68h count).
static int AttackIndex(void* hit, uint8_t* atk)
{
    void* key = *(void**)((uint8_t*)hit + 0x38);
    if (!atk || !key) return -1;
    using Idx = uint32_t(__fastcall*)(void*, void*, void*);
    uint32_t i = (*(Idx*)(*(uintptr_t*)atk + 0x68))(atk, nullptr, key);
    uint8_t* tbl = *(uint8_t**)(atk + 0x36C);
    if (!tbl || i >= *(uint32_t*)(tbl + 0x68)) return -1;
    return (int)i;
}

static uint32_t g_rng = 0x1234567u;
static float Roll()  // 0..100
{
    g_rng = g_rng * 1664525u + 1013904223u + GetTickCount();
    return (g_rng >> 8) * (100.0f / 16777216.0f);
}

static void MaybeStack(uint8_t* target, uint8_t* player, float mult)
{
    int pie = PieOf(player);
    g_lastPie = pie;
    if (Roll() >= pie * g_set.chancePerPie * mult) return;
    uint8_t* st = StatusOf(target);
    EnterCriticalSection(&g_cs);
    Mark* m = FindMark(target, true);
    if (m->stacks < g_set.maxStacks) m->stacks++;
    m->until = Frame() + LifeFrames();
    m->status = st;  // Piety Down finds the enemy by its status object
    g_lastStacks = m->stacks;
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_stacksAdded);
}

// ---- Piety Down, through the game's own stat-down effects ----
// An actor's status object (cHitControlChara) gathers its effects every frame into a work area at
// [status+78h]+80h (update, vtable +14h). Then its refresh (vtable +10h, 0xCBBE10) turns that
// area into the final stats, copies the effect flags to the stats object (what the enemy panel
// shows: red numbers, badges) and clears the area. The HUD's badge list (0xCBC0C0) reads the area
// in between. Work area layout (from cAppend_StatusDnFix's apply, 0xA8E580):
//   +14h..+1Eh  int16 fixed stat-down per stat, STR AGI INT VIT LUC PIE (largest one wins)
//   +9Ch        flags: fixed stat-down of stat i = bit (7 + i); PIE = 0x1000
//   +124h       float frames left per flag bit; the badge flashes under 180
// So after the update and before the refresh we add "PIE fixed down by stacks x lowerPie" for
// marked enemies; the game does the rest, and when the stacks run out we stop adding it.
static const uint32_t STATUS_REFRESH = 0xCBBE10, STATUS_UPDATE = 0xCBC620;
static const uint32_t PIE_DOWN_FLAG = 0x1000, PIE_DOWN_BIT = 12;

static void AddPietyDown(void* status)
{
    if (g_set.lowerPie <= 0) return;
    int amount = 0;
    float frames = 0;
    EnterCriticalSection(&g_cs);
    uint32_t now = Frame();
    for (Mark& m : g_marks) {
        if (m.enemy && m.status == status && m.stacks > 0 && (int32_t)(m.until - now) > 0) {
            amount = m.stacks * g_set.lowerPie;
            frames = (float)(m.until - now);
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (amount <= 0) return;
    uint8_t* work = *(uint8_t**)((uint8_t*)status + 0x78) + 0x80;
    int16_t* pieDown = (int16_t*)(work + 0x1E);
    if (*pieDown < amount) *pieDown = (int16_t)amount;
    *(uint32_t*)(work + 0x9C) |= PIE_DOWN_FLAG;
    float* t = (float*)(work + 0x124) + PIE_DOWN_BIT;
    if (*t < frames) *t = frames;
}

using RefreshFn = void(__fastcall*)(void* status, void* edx);
static RefreshFn o_refresh;
static void __fastcall h_refresh(void* status, void* edx)
{
    __try { AddPietyDown(status); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_refresh(status, edx);
}

using UpdateFn = void(__fastcall*)(void* status, void* edx, void* arg, float dt);
static UpdateFn o_update;
static void __fastcall h_update(void* status, void* edx, void* arg, float dt)
{
    o_update(status, edx, arg, dt);
    __try { AddPietyDown(status); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- the Holy damage path ----
using PathFn = void(__fastcall*)(void* target, void* edx, void* hit, void* a2, uint32_t a3, uint32_t a4, uint8_t* res);
static PathFn o_holy;

static void __fastcall h_holy(void* target, void* edx, void* hit, void* a2, uint32_t a3, uint32_t a4, uint8_t* res)
{
    int before = *(int*)(res + 0x24);
    o_holy(target, edx, hit, a2, a3, a4, res);
    __try {
        uint8_t* t = (uint8_t*)target;
        int dealt = *(int*)(res + 0x24) - before;
        if (!IsEnemy(t) || dealt <= 0) return;
        if (int stacks = StacksOn(t)) {  // anyone's Holy hit on a marked enemy
            *(int*)(res + 0x24) = before + (int)(dealt * (1.0f + stacks * g_set.bonusPerStack / 100.0f) + 0.5f);
            InterlockedIncrement(&g_holyBoosted);
        }
        uint8_t* atk = Attacker(hit);
        if (uint8_t* player = OwnerPlayer(atk))  // her shot: attack entry 0 = fireball, 1 = barrage
            MaybeStack(t, player, AttackIndex(hit, atk) == BARRAGE_ENTRY ? g_set.barrageMult : 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool Hook()
{
    if (!CodeExecutable(HOLY_PATH) || !CodeAt(HOLY_PATH, "558bec6aff68") || !CodeAt(STATUS_REFRESH, "558bec83ec1c")
        || !CodeAt(STATUS_UPDATE, "558becf30f10550c"))
        return false;
    HookLock lock;
    bool ok = HookAt(HOLY_PATH, h_holy, (void**)&o_holy) && HookAt(STATUS_REFRESH, h_refresh, (void**)&o_refresh)
        && HookAt(STATUS_UPDATE, h_update, (void**)&o_update);
    Log(ok ? "Holy damage and status hooks installed" : "hooks failed");
    return true;
}

static DWORD WINAPI WaitThread(LPVOID)
{
    for (int i = 0; i < 1200; i++) {
        if (Hook()) return 0;
        Sleep(50);
    }
    Log("game code not as expected; Holy Up is off");
    return 0;
}

// ---- menu ----
static void DrawPage()
{
    bool changed = false;
    ImGui::TextWrapped("Empress's Holy shots (fireballs, Dragon Cannon) can mark enemies with stacks. "
                       "Each stack lowers the enemy's PIE (fewer halved Holy hits) and raises the Holy "
                       "damage it takes, from the whole party. Set either effect to 0 to switch it off.");
    ImGui::Separator();
    ImGui::TextDisabled("Getting stacks");
    changed |= ImGui::SliderFloat("Chance per PIE point (%)", &g_set.chancePerPie, 0.0f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Barrage chance multiplier", &g_set.barrageMult, 0.0f, 5.0f, "%.1fx");
    changed |= ImGui::SliderInt("Most stacks", &g_set.maxStacks, 1, 30);
    changed |= ImGui::SliderFloat("Seconds a stack lasts", &g_set.seconds, 5.0f, 120.0f, "%.0f");
    ImGui::TextDisabled("Per stack");
    changed |= ImGui::SliderInt("Piety Down: enemy PIE lowered by", &g_set.lowerPie, 0, 10);
    changed |= ImGui::SliderFloat("Holy Up: Holy damage taken (%)", &g_set.bonusPerStack, 0.0f, 20.0f, "+%.1f");
    if (changed) SaveSettings();
    ImGui::Separator();
    LONG pie = g_lastPie;
    if (pie >= 0)
        ImGui::Text("Your PIE (last hit): %ld, fireball chance %.0f%%", pie, pie * g_set.chancePerPie);
    else
        ImGui::TextDisabled("Hit an enemy with a fireball to see your chance.");
    ImGui::Text("Last marked enemy: %ld stack(s). Stacks added: %ld. Holy hits raised: %ld.",
                (long)g_lastStacks, (long)g_stacksAdded, (long)g_holyBoosted);
    ImGui::TextDisabled("Saved to mods\\HolyUp\\holyup.ini.");
}

void ModMain()
{
    InitializeCriticalSection(&g_cs);
    sprintf_s(g_path, "%s\\holyup.ini", ModDir());
    LoadSettings();
    Log("settings: %.2f%% per PIE, barrage x%.1f, +%.1f%% per stack, %d stacks, %.0f s, PIE -%d per stack",
        g_set.chancePerPie, g_set.barrageMult, g_set.bonusPerStack, g_set.maxStacks, g_set.seconds, g_set.lowerPie);
    AddMenuTab("Settings", DrawPage);
}

void ModGameReady()
{
    if (!Hook()) CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}
