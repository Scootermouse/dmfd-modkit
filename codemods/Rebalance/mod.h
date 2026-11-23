// Rebalance: damage / HP / EXP / gold / rare-drop multipliers, per-class damage, the
// host's settings shared online, and Oracle and Bandit unlocked from the start.
#pragma once
#include "../common/modbase.h"

// ---- settings (settings.cpp, dmfd_mod.ini in the mod's folder) ----
enum Kind { K_TAKEN, K_DEALT, K_ENEMY_HP, K_BOSS_HP, K_EXP, K_GOLD, K_RARE, K_COUNT };

struct Profile { float m[K_COUNT]; };

// Per-class multipliers, on top of the profile's damage dealt/taken. Class
// index = the game's character type cPlWork00..05.
enum { CLS_COUNT = 6 };
struct ClassBal { float dealt[CLS_COUNT], taken[CLS_COUNT]; };
extern const char* const CLASS_NAMES[CLS_COUNT];  // Empress, Warrior, Shinobi, Witch, Oracle, Bandit
extern const char* const CLASS_KEYS[CLS_COUNT];   // ini names

struct Settings {
    char preset[32];           // a preset name or "Custom"
    Profile solo, coop;        // values in effect
    Profile customSolo, customCoop;  // what "Custom" uses (kept in the ini)
    Profile coopHost;          // our co-op values before syncing; combat ones are what we publish as host
    ClassBal classes;          // ours: used solo, and published when hosting
    ClassBal classesCoop;      // in effect in co-op: the host's, or all 1 when not synced
    bool unlockClasses;        // Oracle and Bandit available from the start
};

extern const char* const KIND_KEYS[K_COUNT];   // ini names
extern const bool KIND_COMBAT[K_COUNT];        // vanilla in co-op for now
extern const char* const PRESET_NAMES[];       // ..., "Custom"
extern const int PRESET_COUNT;                 // including Custom

// Live multipliers as 16.16 fixed point, read by the hooks: [0] = co-op, [1] = solo.
extern volatile int32_t g_fix[2][K_COUNT];
extern volatile int32_t g_classFix[2][2][CLS_COUNT];  // [co-op/solo][dealt/taken][class], 16.16
extern volatile LONG g_dropsDirty;  // set when rare drop values may have changed

void SettingsInit(const char* dir);      // load or create dir\dmfd_mod.ini
bool SettingsPoll();                     // reload if the file changed; true if reloaded
Settings SettingsGet();
void SettingsResolve(Settings& s);       // fill solo/coop from preset or custom values
void SettingsSet(const Settings& s, bool save);  // apply live; optionally write the ini

// ---- patches ----
bool CodeReady();         // game code decrypted and unmodified at every hook site
bool PatchGame();         // verify and install all hooks; false (and nothing changed) on mismatch
void ApplyDrops(bool solo);  // rewrite rare chances for the given profile
extern volatile LONG g_lastSolo;  // last solo state seen by a hook (-1 = none yet)
// Why IsSolo decided what it did; SOLO_ROOM + n = online room with n players.
enum { SOLO_NO_GAME, SOLO_NO_OBJ, SOLO_FLAG, SOLO_NO_ROOM, SOLO_ROOM_IDLE, SOLO_NOT_ONLINE, SOLO_ROOM };
extern volatile LONG g_soloWhy;
const char* SoloText(LONG why);  // e.g. "solo (offline)"
bool ModIsSolo();                // the game's own player-count check; call on the game thread

// ---- unlocks ----
bool UnlocksInit();  // hooks; needs MH_Initialize
extern volatile LONG g_unlockClasses;

// ---- online (net.cpp) ----
struct NetState {
    bool inRoom, hosting;
    int members;
    bool allModded;   // every member has the mod (same version)
    bool hostModded;  // the host published its values
    bool synced;      // both: co-op combat uses host values
    Profile host;     // the host's co-op combat values
    ClassBal hostClasses;
};
void NetInit();
void NetPoll();      // background thread, a few times a second
NetState NetGet();

// ---- menu tabs (tabs.cpp) ----
void TabsInit();
