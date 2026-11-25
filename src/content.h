#pragma once
#include <windows.h>
#include <cstdint>

#define CONTENT_VERSION "0.9"

void Log(const char* fmt, ...);
// Hooks are installed one at a time across the loader and every mod (a named mutex per game
// process): two modules hooking the same function at the same moment can each overwrite the
// other's jump, and one hook is silently lost (Rebalance and the loader both hook Steam's lobby
// functions from their own threads). Hold one while creating and switching on hooks. Same lock as the mods' (codemods/common).
struct HookLock {
    HookLock();
    ~HookLock();
    HookLock(const HookLock&) = delete;
    HookLock& operator=(const HookLock&) = delete;
private:
    void* h;
};
bool HookAt(uint32_t va, void* fn, void** orig);  // MinHook a game function (preferred-base address)
bool CodeAt(uint32_t va, const char* hex);         // game bytes at va match hex ("8bff558bec")
bool CodeAtOrHooked(uint32_t va, const char* hex); // same, or already hooked by another mod (starts with a jump)

// ---- mod loader (loader.cpp) ----
#include <string>
#include <vector>
struct LoadedMod {
    std::string folder, name, version, author, description;
    std::string state;  // "running", "switched off", "data only", or what went wrong
    bool started, error;
};
void LoadCodeMods();                          // start the code mods in <game>\mods (from a thread, not DllMain)
const std::vector<LoadedMod>& LoadedMods();   // filled by LoadCodeMods, read-only afterwards
std::string GameFolder();                     // folder of game.exe
const std::string& DataModsWarning();         // set if the data mods changed since they were combined

// ---- startup (startup.cpp, proxy.cpp) ----
void StartupInit();             // from DllMain: catch the game's first step
void GameStart(bool atEntry);   // set up the menu, the content engine and every mod (once)

// ---- in-game menu (menu.cpp) ----
struct DmfdApi;
void MenuInit();
void MenuStartingMod(const std::string& folder);  // pages added from now on belong to this mod                // hook D3D9 Present/Reset; read the menu key from mods\loader.ini
const DmfdApi* LoaderApi();     // what code mods get in DmfdModStart
void MenuNotice(const char* text, unsigned seconds);  // a message at the top of the screen (any thread)

// ---- data overlay (datamod.cpp) ----
bool DataModReady();  // game code at the hook sites is decrypted and as expected
bool DataModInit();   // hooks; serves <game>\DataMod\<md5> in place of DataHash\<md5>

// ---- extra icon numbers (icons.cpp) ----
bool IconsReady();    // the icon-name function is decrypted and as expected
bool IconsInit();     // hook it if DataMod\icons.txt lists new icon numbers

// ---- extra classes (classes.cpp) ----
bool ClassesReady();  // the class lookups are decrypted and as expected
bool SaveWatchInit();  // catch the game opening its save (installed in DllMain, before anything else)
bool ClassesInit();   // apply DataMod\classes.txt (longer per-class lists, test switch)
void ClassFilesSetup(int cls, int base, const char* body);  // an extra class's own effect / weapon files (classfiles.cpp)
void ClassFilesPreloaded(int cls, void* loader);           // queue them after the class's preload
bool ClassFilesInit();                                     // logs how many classes have some
const char* ClassFileFor(void* actor, const char* file);  // its class's own file for this sprite (null = none)
bool ClassDefined(int cls);                // classes.txt defines this class
void ClassesRemoteBase(const uint8_t has[6]);  // other players use these base classes (keep their look)

// ---- online (net.cpp) ----
void NetInit(bool classesOn);
bool NetInRoom();              // in an online room (Steam lobby) right now  // rooms by content files; party member hook and log

// ---- built-in tables (items.cpp) ----
void ItemsApply();    // apply <game>\DataMod\items.txt to the item table in memory
void DropsApply();    // apply <game>\DataMod\drops.txt to the chest drop tables (in place)
