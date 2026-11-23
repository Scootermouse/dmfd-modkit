// dmfd_mod.ini (in the mod's folder): a preset name, or "Custom" to use the [solo]/[coop] values.
#include "mod.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

volatile int32_t g_fix[2][K_COUNT];
volatile LONG g_dropsDirty;

const char* const KIND_KEYS[K_COUNT] = { "damage_taken", "damage_dealt", "enemy_hp", "boss_hp", "exp", "gold", "rare_drops" };
// Combat settings stay vanilla in co-op until they can be synced between players.
const bool KIND_COMBAT[K_COUNT] = { true, true, true, true, false, false, false };

//                                    taken  dealt  enemy  boss   exp    gold   rare
static const Profile PRESETS[] = {
    { 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f },  // Vanilla
    { 0.8f,  1.0f,  0.9f,  0.85f, 1.25f, 1.25f, 1.5f },  // Light
    { 0.6f,  1.0f,  0.8f,  0.7f,  1.5f,  1.5f,  2.0f },  // Easy
    { 0.35f, 1.5f,  0.6f,  0.5f,  2.0f,  2.0f,  3.0f },  // Story
};
const char* const CLASS_NAMES[CLS_COUNT] = { "Empress", "Warrior", "Shinobi", "Witch", "Oracle", "Bandit" };
const char* const CLASS_KEYS[CLS_COUNT] = { "empress", "warrior", "shinobi", "witch", "oracle", "bandit" };

const char* const PRESET_NAMES[] = { "Vanilla", "Light", "Easy", "Story", "Custom" };
const int PRESET_COUNT = 5;

static CRITICAL_SECTION g_lock;
static Settings g_settings;
static char g_path[MAX_PATH];
static FILETIME g_mtime;

static const char DEFAULT_INI[] =
    "; Rebalance settings. 1.0 = vanilla. Everything starts vanilla;\n"
    "; change things here or in the in-game menu (its Balance and Classes tabs).\n"
    "; Changes apply while the game runs.\n"
    ";\n"
    "; preset = Vanilla, Light, Easy or Story (the same for everyone who picks it),\n"
    ";          or Custom to use the [solo] and [coop] values below.\n"
    "; [solo] applies when you're the only player in the quest, [coop] otherwise.\n"
    "; In co-op, damage and HP settings come from the host, and only when everyone in the\n"
    "; room has the mod (otherwise combat is vanilla). Your own [coop] combat values are\n"
    "; what you use when you host. EXP, gold and drops are always your own.\n"
    "; unlock_classes = 1: Oracle and Bandit are unlocked from the start.\n"
    "[mod]\n"
    "preset=Vanilla\n"
    "unlock_classes=1\n"
    "config_version=3\n"
    "\n"
    "[solo]\n"
    "damage_taken=1.0   ; damage you take (healing unaffected; hits do at least 1)\n"
    "damage_dealt=1.0   ; damage enemies and bosses take from any source\n"
    "enemy_hp=1.0       ; regular enemy max HP\n"
    "boss_hp=1.0        ; boss max HP\n"
    "exp=1.0            ; EXP gained\n"
    "gold=1.0           ; gold gained during quests (shop sales unaffected)\n"
    "rare_drops=1.0     ; rare drop chance (capped at 100%)\n"
    "\n"
    "[coop]\n"
    "damage_taken=1.0\n"
    "damage_dealt=1.0\n"
    "enemy_hp=1.0\n"
    "boss_hp=1.0\n"
    "exp=1.0\n"
    "gold=1.0\n"
    "rare_drops=1.0\n"
    "\n"
    "; Per-class damage, on top of damage_dealt / damage_taken above (1.0 = no change),\n"
    "; e.g. witch_dealt=1.4. Used solo and when you host; in co-op the host's apply.\n"
    "[classes]\n";

static void Trim(char* s)
{
    if (char* sc = strchr(s, ';')) *sc = 0;
    for (char* e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
}

static float ReadF(const char* sec, const char* key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, g_path);
    char* end;
    double v = strtod(buf, &end);
    if (end == buf || !std::isfinite(v)) return def;
    return (float)(v < 0 ? 0 : v > 100 ? 100 : v);
}

static bool FileTime(FILETIME* ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &a)) return false;
    *ft = a.ftLastWriteTime;
    return true;
}

void SettingsResolve(Settings& s)
{
    int p = -1;
    for (int i = 0; i < PRESET_COUNT - 1; i++)
        if (!_stricmp(PRESET_NAMES[i], s.preset)) p = i;
    if (p >= 0) {
        strcpy_s(s.preset, PRESET_NAMES[p]);
        s.solo = s.coop = PRESETS[p];
    } else {
        strcpy_s(s.preset, "Custom");
        s.solo = s.customSolo;
        s.coop = s.customCoop;
    }
    // Combat in co-op must match on every machine: the host's values when the
    // whole room has the mod, vanilla otherwise.
    s.coopHost = s.coop;
    NetState net = NetGet();
    for (int k = 0; k < K_COUNT; k++)
        if (KIND_COMBAT[k]) s.coop.m[k] = net.synced ? net.host.m[k] : 1.0f;
    for (int c = 0; c < CLS_COUNT; c++) {
        s.classesCoop.dealt[c] = net.synced ? net.hostClasses.dealt[c] : 1.0f;
        s.classesCoop.taken[c] = net.synced ? net.hostClasses.taken[c] : 1.0f;
    }
}

static void LogSettings(const Settings& s)
{
    Log("settings: preset %s%s", s.preset, s.unlockClasses ? ", Oracle and Bandit unlocked" : "");
    for (int k = 0; k < K_COUNT; k++)
        Log("  %-13s solo %.2f  coop %.2f", KIND_KEYS[k], s.solo.m[k], s.coop.m[k]);
    for (int c = 0; c < CLS_COUNT; c++)
        if (s.classes.dealt[c] != 1.0f || s.classes.taken[c] != 1.0f || s.classesCoop.dealt[c] != 1.0f ||
            s.classesCoop.taken[c] != 1.0f)
            Log("  %-13s dealt %.2f taken %.2f  coop dealt %.2f taken %.2f", CLASS_NAMES[c], s.classes.dealt[c],
                s.classes.taken[c], s.classesCoop.dealt[c], s.classesCoop.taken[c]);
}

static void WriteF(const char* sec, const char* key, float v)
{
    char buf[32];
    sprintf_s(buf, "%.2f", v);
    WritePrivateProfileStringA(sec, key, buf, g_path);
}

void SettingsSet(const Settings& in, bool save)
{
    Settings s = in;
    SettingsResolve(s);
    EnterCriticalSection(&g_lock);
    g_settings = s;
    for (int k = 0; k < K_COUNT; k++) {
        g_fix[0][k] = (int32_t)lround(s.coop.m[k] * 65536.0);
        g_fix[1][k] = (int32_t)lround(s.solo.m[k] * 65536.0);
    }
    for (int c = 0; c < CLS_COUNT; c++) {
        g_classFix[0][0][c] = (int32_t)lround(s.classesCoop.dealt[c] * 65536.0);
        g_classFix[0][1][c] = (int32_t)lround(s.classesCoop.taken[c] * 65536.0);
        g_classFix[1][0][c] = (int32_t)lround(s.classes.dealt[c] * 65536.0);
        g_classFix[1][1][c] = (int32_t)lround(s.classes.taken[c] * 65536.0);
    }
    g_dropsDirty = 1;
    g_unlockClasses = s.unlockClasses;
    if (save) {
        WritePrivateProfileStringA("mod", "preset", s.preset, g_path);
        WritePrivateProfileStringA("mod", "unlock_classes", s.unlockClasses ? "1" : "0", g_path);
        for (int k = 0; k < K_COUNT; k++) {
            WriteF("solo", KIND_KEYS[k], s.customSolo.m[k]);
            WriteF("coop", KIND_KEYS[k], s.customCoop.m[k]);
        }
        for (int c = 0; c < CLS_COUNT; c++) {
            char key[32];
            sprintf_s(key, "%s_dealt", CLASS_KEYS[c]);
            WriteF("classes", key, s.classes.dealt[c]);
            sprintf_s(key, "%s_taken", CLASS_KEYS[c]);
            WriteF("classes", key, s.classes.taken[c]);
        }
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_path);  // flush the profile cache
        FileTime(&g_mtime);  // our own write, not an outside edit
    }
    LeaveCriticalSection(&g_lock);
    if (save) LogSettings(s);
}

Settings SettingsGet()
{
    EnterCriticalSection(&g_lock);
    Settings s = g_settings;
    LeaveCriticalSection(&g_lock);
    return s;
}

static void Load()
{
    Settings s{};
    GetPrivateProfileStringA("mod", "preset", "Vanilla", s.preset, sizeof s.preset, g_path);
    Trim(s.preset);
    s.unlockClasses = GetPrivateProfileIntA("mod", "unlock_classes", 1, g_path) != 0;
    for (int k = 0; k < K_COUNT; k++) {
        s.customSolo.m[k] = ReadF("solo", KIND_KEYS[k], 1.0f);
        s.customCoop.m[k] = ReadF("coop", KIND_KEYS[k], 1.0f);
    }
    for (int c = 0; c < CLS_COUNT; c++) {
        char key[32];
        sprintf_s(key, "%s_dealt", CLASS_KEYS[c]);
        s.classes.dealt[c] = ReadF("classes", key, 1.0f);
        sprintf_s(key, "%s_taken", CLASS_KEYS[c]);
        s.classes.taken[c] = ReadF("classes", key, 1.0f);
    }
    bool known = !_stricmp(s.preset, "Custom");
    for (int i = 0; i < PRESET_COUNT; i++) known |= !_stricmp(PRESET_NAMES[i], s.preset);
    if (!known) Log("unknown preset '%s', using Custom values", s.preset);
    SettingsSet(s, false);
    LogSettings(SettingsGet());
}

void SettingsInit(const char* dir)
{
    InitializeCriticalSection(&g_lock);
    sprintf_s(g_path, "%s\\dmfd_mod.ini", dir);
    if (GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) {
        // carried over once from the stand-alone Rebalance (dinput8.dll) in the game folder
        char old[MAX_PATH];
        sprintf_s(old, "%s\\dmfd_mod.ini", GameDir());
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES && CopyFileA(old, g_path, TRUE)) {
            Log("settings carried over from %s", old);
        } else {
            FILE* f;
            if (!fopen_s(&f, g_path, "w")) {
                fputs(DEFAULT_INI, f);
                fclose(f);
                Log("wrote default %s", g_path);
            }
        }
    }
    FileTime(&g_mtime);
    Load();
}

bool SettingsPoll()
{
    FILETIME ft;
    EnterCriticalSection(&g_lock);
    bool changed = FileTime(&ft) && CompareFileTime(&ft, &g_mtime);
    if (changed) g_mtime = ft;
    LeaveCriticalSection(&g_lock);
    if (!changed) return false;
    Sleep(100);  // let the editor finish writing
    Load();
    return true;
}
