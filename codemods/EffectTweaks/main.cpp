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
// Breath shots (0.3): "shot" lines move where a shot starts (see the shot section below).
// log_effects=1 (menu) logs every animation started through the call once, with the calling code,
// so a modder can see which hash an effect has (then tools\anim_names.py for its name).
#include "../common/modbase.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include "imgui.h"

const char MOD_NAME_STR[] = "Effect Tweaks";
const char MOD_VERSION_STR[] = "0.3";

static const uint32_t ANIM_PLAY = 0xE4EA70;  // thiscall(sprite, hash, parent, int, float start), ret 10h

struct Tweak {
    uint32_t hash;
    bool swap;
    uint32_t to;
    float start, dx, dy;
    bool move;
    float delay, speed;  // frames to wait hidden; playback speed (0 = unchanged)
};
static Tweak g_tweaks[256];
static int g_nTweaks;
static char g_path[MAX_PATH], g_ini[MAX_PATH];

// ---- breath shot start points (0.3) ----
// The Empress's breath shots (cPshWork04, made in 0xD63AE0) start at the player's position plus
// one of six offsets {forward, up, 0} picked by her pose (a table the game fills at 0x28F4860 the
// first time: 0 (30,37), 1 (30,37), 2 (30,56), 3 (30,37), 4 (26,43), 5 (0,86)). The offset is
// applied by 0xD31670 (thiscall(shot, player, const float* offset, int facing), ret 0Ch; forward is
// mirrored when the player faces left). Its other caller (a metal shot splitting) passes another
// offset and is left alone. A taller (or shorter) body needs these moved:
//   shot <pose 0-5 or all> dx=<px> dy=<px> [class=<0-5 or a classes.txt id>]
static const uint32_t SHOT_PLACE = 0xD31670;
static const uint32_t SHOT_TABLE = 0x28F4860;  // 6 x {forward, up, 0}
static const int SHOT_POSES = 6;
struct ShotTweak { int pose; int cls; float dx, dy; };  // pose / cls -1 = all
static ShotTweak g_shots[64];
static int g_nShots;
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

// Extra classes' numbers by id (<game>\DataMod\classes.txt, written by the mod manager:
// "<number> base=<n> id=<id> ..."), so a shot line can name a class that has no fixed number.
static char g_classIds[128][48];
static void LoadClassIds()
{
    memset(g_classIds, 0, sizeof g_classIds);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\DataMod\\classes.txt", GameDir());
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        int n = atoi(line);
        const char* id = strstr(line, " id=");
        if (n < 6 || n >= 128 || !id) continue;
        id += 4;
        size_t len = strcspn(id, " \t\r\n");
        if (len >= sizeof g_classIds[0]) len = sizeof g_classIds[0] - 1;
        memcpy(g_classIds[n], id, len);
    }
    fclose(f);
}
static int ClassNumber(const char* v)
{
    if (v[0] >= '0' && v[0] <= '9') return atoi(v);
    for (int n = 6; n < 128; n++)
        if (g_classIds[n][0] && !_stricmp(g_classIds[n], v)) return n;
    return -2;  // not in the game (yet): matches nothing
}
// "shot <pose|all> dx= dy= [class=]": the rest of the line after "shot"
static void ParseShot(char*& ctx, int no)
{
    ShotTweak t = {-1, -1, 0, 0};
    char* tok = strtok_s(nullptr, " \t\r\n", &ctx);
    if (!tok) { Log("effects.txt line %d: shot needs a pose (0-5 or all)", no); return; }
    if (_stricmp(tok, "all")) {
        t.pose = atoi(tok);
        if (tok[0] < '0' || tok[0] > '9' || t.pose >= SHOT_POSES) {
            Log("effects.txt line %d: shot pose '%s' should be 0-5 or all", no, tok);
            return;
        }
    }
    while ((tok = strtok_s(nullptr, " \t\r\n", &ctx)) != nullptr) {
        char* eq = strchr(tok, '=');
        if (!eq) { Log("effects.txt line %d: '%s' should be name=value", no, tok); continue; }
        *eq = 0;
        const char* v = eq + 1;
        if (!_stricmp(tok, "dx")) t.dx = (float)atof(v);
        else if (!_stricmp(tok, "dy")) t.dy = (float)atof(v);
        else if (!_stricmp(tok, "class")) {
            t.cls = ClassNumber(v);
            if (t.cls == -2) Log("effects.txt line %d: no class '%s' in DataMod\\classes.txt", no, v);
        } else Log("effects.txt line %d: unknown shot setting '%s'", no, tok);
    }
    if (g_nShots < 64) g_shots[g_nShots++] = t;
}

// effects.txt: one effect a line, '#' comments:
//   <animation> swap=<animation> start=<frames> dx=<px> dy=<px>
//   shot <pose> dx=<px> dy=<px> [class=]
static void LoadTweaks()
{
    g_nTweaks = 0;
    g_nShots = 0;
    LoadClassIds();
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
        if (!_stricmp(tok, "shot")) {
            ParseShot(ctx, no);
            continue;
        }
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
            else if (!_stricmp(tok, "delay")) { t.delay = (float)atof(v); any = true; }
            else if (!_stricmp(tok, "speed")) { t.speed = (float)atof(v); any = true; }
            else Log("effects.txt line %d: unknown setting '%s'", no, tok);
        }
        if (any) g_tweaks[g_nTweaks++] = t;
    }
    fclose(f);
    Log("%d effect change(s) and %d shot start point(s) from effects.txt", g_nTweaks, g_nShots);
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

// ---- delay and speed (0.2) ----
// The sprite's per-frame update (0xE4EE80): nothing if +1A2h (removed); the step is the frame time
// x the rate of its root node (sprite +40h, 0xE51FB0: its own speed at node +E4h = sprite +124h,
// x its parent's when node +ECh has bit 40h); +1A0h = 0 pauses it (step 0). While it waits it is
// moved far off screen through its offset from its parent (+7Ch, as dy=); 0.2 tried an alpha at
// +100h, but that isn't one (it read 0 on a visible effect), and the engine counter 0x1526DC4
// didn't count game frames, so the effect never started and its move (which waits for it) hung.
static uint32_t Now() { return Frame(); }
static const float AWAY = 100000.0f;
static volatile LONG g_tickOn;  // the per-frame callback is running (registered once the game is ready)
struct Waiting { uint8_t* sprite; uintptr_t vt; uint32_t until; uint8_t playing; };
static Waiting g_wait[64];
static volatile LONG g_probeLogs;
static void Pace(uint8_t* sp, const Tweak* t)
{
    __try {
        if (InterlockedIncrement(&g_probeLogs) <= 4) {
            float* c = (float*)(sp + 0xF4);
            Log("effect 0x%08x sprite: color? %.2f %.2f %.2f %.2f, speed %.2f, playing %d, removed %d", t->hash,
                c[0], c[1], c[2], c[3], *(float*)(sp + 0x124), sp[0x1A0], sp[0x1A2]);
        }
        if (t->speed > 0.0f) *(float*)(sp + 0x124) *= t->speed;
        if (t->delay >= 1.0f && g_tickOn) {  // (without the callback nothing would start it again)
            Waiting* w = nullptr;
            for (Waiting& x : g_wait) if (!x.sprite) { w = &x; break; }
            if (!w) return;
            w->sprite = sp;
            w->vt = *(uintptr_t*)sp;
            w->until = Now() + (uint32_t)t->delay;
            w->playing = sp[0x1A0];
            *(float*)(sp + 0x7C) += AWAY;  // out of sight
            sp[0x1A0] = 0;                 // and paused at its start
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Each frame: waiting effects start (shown, playing) when their delay is over.
static void Tick(void*, void*)
{
    uint32_t now = Now();
    for (Waiting& w : g_wait) {
        if (!w.sprite || (int32_t)(now - w.until) < 0) continue;
        __try {
            if (*(uintptr_t*)w.sprite == w.vt && !w.sprite[0x1A2]) {
                *(float*)(w.sprite + 0x7C) -= AWAY;
                w.sprite[0x1A0] = w.playing ? w.playing : 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        w.sprite = nullptr;
    }
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
    if (sprite && (t->speed > 0.0f || t->delay > 0.0f)) Pace(sprite, t);
    if (InterlockedIncrement(&g_applied) <= 10)
        Log("effect 0x%08x changed%s", hash, t->swap ? " (swapped)" : "");
    return r;
}

// The most specific line for this class and pose: class and pose, class, pose, then all.
static const ShotTweak* FindShot(int cls, int pose)
{
    const ShotTweak* best = nullptr;
    int bestScore = -1;
    for (int i = 0; i < g_nShots; i++) {
        const ShotTweak& t = g_shots[i];
        if ((t.cls != -1 && t.cls != cls) || (t.pose != -1 && t.pose != pose)) continue;
        int score = (t.cls != -1 ? 2 : 0) + (t.pose != -1 ? 1 : 0);
        if (score >= bestScore) { best = &t; bestScore = score; }  // (a later line wins a tie)
    }
    return best;
}
static uint8_t g_shotSeen[128][SHOT_POSES];
using ShotPlaceFn = void(__fastcall*)(uint8_t* shot, void* edx, uint8_t* player, const float* offset, int facing);
static ShotPlaceFn o_shotPlace;
static void __fastcall h_shotPlace(uint8_t* shot, void* edx, uint8_t* player, const float* offset, int facing)
{
    float mine[3];
    __try {
        uintptr_t tab = (uintptr_t)G(SHOT_TABLE);
        uintptr_t o = (uintptr_t)offset;
        if (player && o >= tab && o < tab + SHOT_POSES * 12 && (o - tab) % 12 == 0) {
            int pose = (int)((o - tab) / 12);
            int cls = player[0xF5C];
            if (g_logEffects && cls < 128 && !g_shotSeen[cls][pose]) {
                g_shotSeen[cls][pose] = 1;
                Log("breath shot: class %d, pose %d, starts %.0f forward %.0f up", cls, pose, offset[0], offset[1]);
            }
            const ShotTweak* t = g_enabled ? FindShot(cls, pose) : nullptr;
            if (t) {
                mine[0] = offset[0] + t->dx;
                mine[1] = offset[1] + t->dy;
                mine[2] = offset[2];
                offset = mine;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_shotPlace(shot, edx, player, offset, facing);
}

static bool g_animHooked, g_shotHooked;
static bool Hook()
{
    if (!g_animHooked && CodeAt(ANIM_PLAY, "558becf30f104514")) {
        g_animHooked = true;
        bool ok = HookAt(ANIM_PLAY, h_animPlay, (void**)&o_animPlay);
        Log(ok ? "play-animation hook installed" : "play-animation hook failed");
    }
    // (its next bytes hold a relocated address; the ones after it are checked too)
    if (!g_shotHooked && CodeAt(SHOT_PLACE, "558bec83ec10") && CodeAt(SHOT_PLACE + 0x10, "568bf1578b7d08")) {
        g_shotHooked = true;
        bool ok = HookAt(SHOT_PLACE, h_shotPlace, (void**)&o_shotPlace);
        Log(ok ? "shot start hook installed" : "shot start hook failed");
    }
    return g_animHooked && g_shotHooked;
}

static DWORD WINAPI WaitThread(LPVOID)
{
    for (int i = 0; i < 1200; i++) {
        if (Hook()) return 0;
        Sleep(50);
    }
    if (!g_animHooked) Log("game code not as expected: effect lines are off");
    if (!g_shotHooked) Log("game code not as expected: shot lines are off");
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
    ImGui::Text("%d effect change(s), %d shot line(s) loaded", g_nTweaks, g_nShots);
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
    // (in ModMain the loader hasn't handed over its API yet: 0.2.1's registration failed quietly)
    g_tickOn = AddFrameDraw(Tick, nullptr) ? 1 : 0;
    Log(g_tickOn ? "per-frame callback on: delay= works" : "no per-frame callback (loader too old): delay= is ignored");
    if (!Hook()) CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}
