// SPDX-License-Identifier: MIT (see LICENSE-MIT.txt here; the rest of the kit is GPL-3.0-or-later)
// Shared plumbing for the kit's code mods: the mod's folder, its log, game addresses,
// MinHook, and the loader's in-game menu. Each mod defines MOD_NAME_STR, MOD_VERSION_STR
// and ModMain().
#pragma once
#include <windows.h>
#include <cstdint>
#include "dmfd_mod_api.h"

void Log(const char* fmt, ...);
const char* ModDir();             // mods\<name> folder, no trailing backslash
const char* GameDir();            // folder of game.exe
uintptr_t G(uint32_t va);         // game address at the exe's preferred base (0x400000) -> real address
bool HookAt(uint32_t va, void* fn, void** orig);  // MinHook a game function; logs failures
bool CodeAt(uint32_t va, const char* hex);        // bytes there match hex, or another mod already hooked it (jmp)
bool CodeExecutable(uint32_t va);                 // Steam's wrapper has finished with that code

// The loader's API, or null if the mod was loaded some other way.
const DmfdApi* Api();
uint32_t Frame();                 // frames drawn so far (falls back to ~60 a second)
int MenuKey();                    // the loader's menu key (VK code)
bool InOnlineRoom();              // in an online room (Steam lobby) right now
// Draws every frame (render thread, inside BeginScene/EndScene, device state restored after);
// false if the loader is older than API 3. device is an IDirect3DDevice9*.
bool AddFrameDraw(void (*draw)(void* device, void* user), void* user);
// Hooks are installed one at a time across the loader and every mod (a named mutex per game
// process): two modules hooking the same function at the same moment can each overwrite the
// other's jump, and one hook is silently lost (Rebalance and the loader both hook Steam's lobby
// functions from their own threads). Hold one while creating and switching on hooks.
struct HookLock {
    HookLock();
    ~HookLock();
    HookLock(const HookLock&) = delete;
    HookLock& operator=(const HookLock&) = delete;
private:
    void* h;
};
// Add a tab to the in-game menu. draw runs inside the menu window with ImGui ready to use.
void AddMenuTab(const char* tab, void (*draw)());

extern const char MOD_NAME_STR[];     // defined by each mod, e.g. "Rebalance"
extern const char MOD_VERSION_STR[];  // e.g. "0.16" (keep in step with its mod.ini)

// Defined by each mod: runs once, on DLL load, after the log is open and MinHook is up.
// Settings and menu pages go here; no game hooks yet.
void ModMain();
// Defined by each mod: the game's code is unpacked and the game hasn't started yet (the
// loader calls this at the game's first step, on its own thread). Put the game hooks in here.
void ModGameReady();
