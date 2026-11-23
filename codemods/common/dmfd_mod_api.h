// SPDX-License-Identifier: MIT (see LICENSE-MIT.txt here; the rest of the kit is GPL-3.0-or-later)
// What the DMFD mod loader (winmm.dll) offers code mods.
//
// A code mod is a DLL in mods\<name>\ (named by dll= in its mod.ini). The loader loads it
// at startup and, if it exports DmfdModStart, calls it once with this table:
//
//   extern "C" __declspec(dllexport) void DmfdModStart(const DmfdApi* api);
//
// The in-game menu (Insert by default) belongs to the loader. A mod adds tabs with addTab;
// their draw functions run on the game's render thread, inside the menu window. ImGui
// memory belongs to the loader: before drawing, a mod points its own ImGui at the loader's
// context and allocator (modbase.cpp's DrawThunk does this). Mods must be built against
// the same ImGui as the loader (third_party/imgui); imguiVersion says which.
#pragma once
#include <cstdint>

#define DMFD_API_VERSION 3  // 2: inOnlineRoom; 3: addFrameDraw

struct DmfdApi {
    int apiVersion;              // DMFD_API_VERSION the loader was built with
    int imguiVersion;            // IMGUI_VERSION_NUM the loader was built with
    const char* loaderVersion;
    const char* gameDir;         // folder of game.exe, no trailing backslash

    // Adds a page to the in-game menu, under the mod being started (call it from DllMain or
    // DmfdModStart). A mod with several pages shows them as tabs, in the order added.
    void (*addTab)(const char* modName, const char* tabName, void (*draw)(void* user), void* user);
    void* (*imguiContext)();     // the menu's ImGuiContext*, null until the first frame
    void (*imguiAllocators)(void** allocFn, void** freeFn, void** userData);
    int (*menuKey)();            // virtual-key code that opens and closes the menu
    bool (*menuOpen)();
    uint32_t (*frame)();         // frames drawn so far (about 60 a second)
    bool (*inOnlineRoom)();      // in an online room (Steam lobby) right now; API 2+

    // Registers a function the loader calls every frame on the render thread, between
    // BeginScene and EndScene, before the menu draws, with the IDirect3DDevice9* (as void*).
    // The device's state is saved before and restored after, so a mod may change any state.
    // API 3+.
    void (*addFrameDraw)(void (*draw)(void* device, void* user), void* user);
};

using DmfdModStartFn = void (*)(const DmfdApi*);
