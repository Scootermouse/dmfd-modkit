// Skip Quit Confirmation: Alt+F4 or the window's X closes the game right away.
//
// The window procedure's WM_CLOSE handler saves the window position, then asks
// CLOSE_CHECK whether to close. Vanilla opens the "really quit?" dialog
// (cAppCloseConfirm) and says no; the dialog's Yes then calls DestroyWindow. Saying
// yes right away lets DefWindowProc do the same. The menu page (skipquit.ini) can
// bring the dialog back without switching the mod off.
#include "../common/modbase.h"
#include <cstdio>
#include "imgui.h"

const char MOD_NAME_STR[] = "Skip Quit Confirmation";
const char MOD_VERSION_STR[] = "1.1";

static const uint32_t CLOSE_CHECK = 0xE0C650;  // char __cdecl (); only caller is WM_CLOSE

static volatile LONG g_skip = 1;
static char g_path[MAX_PATH];

using CloseCheckFn = char(__cdecl*)();
static CloseCheckFn o_closeCheck;

static char __cdecl h_closeCheck()
{
    if (!g_skip) return o_closeCheck();
    Log("quitting without the confirmation dialog");
    return 1;
}

static bool Hook()
{
    if (!CodeAt(CLOSE_CHECK, "558bec6aff68")) return false;
    bool ok = HookAt(CLOSE_CHECK, h_closeCheck, (void**)&o_closeCheck);
    Log(ok ? "quit confirmation hook installed" : "quit confirmation hook failed");
    return true;
}

static DWORD WINAPI WaitThread(LPVOID)
{
    // the code wasn't ready at the game's first step: wait for it (up to a minute)
    for (int i = 0; i < 1200; i++) {
        if (Hook()) return 0;
        Sleep(50);
    }
    Log("game code not as expected; Skip Quit Confirmation is off");
    return 0;
}

static void DrawPage()
{
    bool on = g_skip != 0;
    if (ImGui::Checkbox("Quit without the confirmation dialog", &on)) {
        g_skip = on;
        WritePrivateProfileStringA("skipquit", "enabled", on ? "1" : "0", g_path);
        Log("skipping the dialog: %s", on ? "on" : "off");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Alt+F4 or the window's X closes the game right away,\n"
                          "without the game's \"really quit?\" dialog.");
    ImGui::TextDisabled("Saved to mods\\SkipQuit\\skipquit.ini.");
}

void ModMain()
{
    sprintf_s(g_path, "%s\\skipquit.ini", ModDir());
    g_skip = GetPrivateProfileIntA("skipquit", "enabled", 1, g_path) != 0;
    AddMenuTab("Settings", DrawPage);
}

void ModGameReady()
{
    if (!Hook()) CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}
