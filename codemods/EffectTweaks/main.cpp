// Effect Tweaks: change how the game's effects play, from a text file (effects.txt).
//
// The game has no single "spawn effect" routine: each move loads its effect file (the Empress's
// pl0_ef00 at 19 places) and sets the sprite up itself. All of them start the effect with the
// engine's play-animation call 0xE4EA70 (thiscall(sprite, animation hash, parent, int, float
// start time), ret 10h), which this mod hooks. An effect is known by its animation's name hash:
// - swap=<animation>  play another animation of the same file instead;
// - start=<frames>    start that many frames into it (skips its beginning);
// - dx=, dy=          shift it (forward as drawn facing right, up), added to the sprite's offset
//                     from its parent (+78h / +7Ch), set after the call.
// log_effects=1 (menu) logs every animation started through the call once, with the calling code,
// so a modder can see which hash an effect has (then tools\anim_names.py for its name).
#include "../common/modbase.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include "imgui.h"

const char MOD_NAME_STR[] = "Effect Tweaks";
const char MOD_VERSION_STR[] = "0.1";

static const uint32_t ANIM_PLAY = 0xE4EA70;  // thiscall(sprite, hash, parent, int, float start), ret 10h

struct Tweak {
    uint32_t hash;
    bool swap;
    uint32_t to;
    float start, dx, dy;
    bool move;
};
static Tweak g_tweaks[256];
static int g_nTweaks;
static char g_path[MAX_PATH], g_ini[MAX_PATH];
static volatile LONG g_enabled = 1, g_logEffects = 0;
static volatile LONG g_applied;

// The game's name hash (the same as tools\anim_names.py shows).
static uint32_t NameHash(const char* s)
{
    uint32_t v = 0xCDE723A5u;
    for (; *s; s++) v = (v + (uint32_t)(int32_t)(signed char)*s) * 0x8Du;
    return v;
}
// "0x1234abcd" or an animation name (UTF-8)
static uint32_t ParseAnim(const char* s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (uint32_t)strtoul(s + 2, nullptr, 16);
    return NameHash(s);
}

// effects.txt: one effect a line, '#' comments:
//   <animation> swap=<animation> start=<frames> dx=<px> dy=<px>
static void LoadTweaks()
{
    g_nTweaks = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "rb") || !f) {
        Log("no %s: nothing to change", g_path);
        return;
    }
    char line[512];
    int no = 0;
    while (fgets(line, sizeof line, f) && g_nTweaks < 256) {
        no++;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char* ctx = nullptr;
        char* tok = strtok_s(line, " \t\r\n", &ctx);
        if (!tok) continue;
        if (no == 1 && (uint8_t)tok[0] == 0xEF && (uint8_t)tok[1] == 0xBB && (uint8_t)tok[2] == 0xBF) tok += 3;  // UTF-8 mark
        Tweak t = {};
        t.hash = ParseAnim(tok);
        bool any = false;
        while ((tok = strtok_s(nullptr, " \t\r\n", &ctx)) != nullptr) {
            char* eq = strchr(tok, '=');
            if (!eq) { Log("effects.txt line %d: '%s' should be name=value", no, tok); continue; }
            *eq = 0;
            const char* v = eq + 1;
            if (!_stricmp(tok, "swap")) { t.swap = true; t.to = ParseAnim(v); any = true; }
            else if (!_stricmp(tok, "start")) { t.start = (float)atof(v); any = true; }
            else if (!_stricmp(tok, "dx")) { t.dx = (float)atof(v); t.move = true; any = true; }
            else if (!_stricmp(tok, "dy")) { t.dy = (float)atof(v); t.move = true; any = true; }
            else Log("effects.txt line %d: unknown setting '%s'", no, tok);
        }
        if (any) g_tweaks[g_nTweaks++] = t;
    }
    fclose(f);
    Log("%d effect change(s) from effects.txt", g_nTweaks);
}

static const Tweak* Find(uint32_t hash)
{
    for (int i = 0; i < g_nTweaks; i++)
        if (g_tweaks[i].hash == hash) return &g_tweaks[i];
    return nullptr;
}

// effects logging: each animation hash once (with the code that started it)
static uint32_t g_seen[1024];
static int g_nSeen;
static void LogOnce(uint32_t hash, void* ret)
{
    for (int i = 0; i < g_nSeen; i++)
        if (g_seen[i] == hash) return;
    if (g_nSeen < 1024) g_seen[g_nSeen++] = hash;
    Log("animation 0x%08x started (from game code %06x)", hash, (unsigned)((uintptr_t)ret - G(0x400000) + 0x400000));
}

using AnimFn = void*(__fastcall*)(uint8_t* sprite, void* edx, uint32_t hash, void* parent, int a, float start);
static AnimFn o_animPlay;
static void* __fastcall h_animPlay(uint8_t* sprite, void* edx, uint32_t hash, void* parent, int a, float start)
{
    if (g_logEffects) LogOnce(hash, _ReturnAddress());
    const Tweak* t = g_enabled ? Find(hash) : nullptr;
    if (!t) return o_animPlay(sprite, edx, hash, parent, a, start);
    uint32_t h = t->swap ? t->to : hash;
    void* r = o_animPlay(sprite, edx, h, parent, a, start + t->start);
    if (t->move && sprite) {
        __try {
            *(float*)(sprite + 0x78) += t->dx;
            *(float*)(sprite + 0x7C) += t->dy;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (InterlockedIncrement(&g_applied) <= 10)
        Log("effect 0x%08x changed%s", hash, t->swap ? " (swapped)" : "");
    return r;
}

static bool Hook()
{
    if (!CodeAt(ANIM_PLAY, "558becf30f104514")) return false;
    bool ok = HookAt(ANIM_PLAY, h_animPlay, (void**)&o_animPlay);
    Log(ok ? "play-animation hook installed" : "play-animation hook failed");
    return true;
}

static DWORD WINAPI WaitThread(LPVOID)
{
    for (int i = 0; i < 1200; i++) {
        if (Hook()) return 0;
        Sleep(50);
    }
    Log("game code not as expected; Effect Tweaks is off");
    return 0;
}

static void DrawPage()
{
    bool on = g_enabled != 0;
    if (ImGui::Checkbox("Apply effects.txt", &on)) {
        g_enabled = on;
        WritePrivateProfileStringA("effecttweaks", "enabled", on ? "1" : "0", g_ini);
    }
    bool lg = g_logEffects != 0;
    if (ImGui::Checkbox("Log animations as they start (to find an effect's hash)", &lg)) {
        g_logEffects = lg;
        WritePrivateProfileStringA("effecttweaks", "log_effects", lg ? "1" : "0", g_ini);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each animation started through the game's play call is written once to\n"
                          "mods\\EffectTweaks\\mod.log. Do the move, then look for the new lines.");
    if (ImGui::Button("Reload effects.txt")) LoadTweaks();
    ImGui::SameLine();
    ImGui::Text("%d change(s) loaded", g_nTweaks);
}

void ModMain()
{
    sprintf_s(g_path, "%s\\effects.txt", ModDir());
    sprintf_s(g_ini, "%s\\effecttweaks.ini", ModDir());
    g_enabled = GetPrivateProfileIntA("effecttweaks", "enabled", 1, g_ini) != 0;
    g_logEffects = GetPrivateProfileIntA("effecttweaks", "log_effects", 0, g_ini) != 0;
    LoadTweaks();
    AddMenuTab("Settings", DrawPage);
}

void ModGameReady()
{
    if (!Hook()) CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}
