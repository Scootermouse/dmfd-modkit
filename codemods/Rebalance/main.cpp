// Rebalance mod: started by the mod loader from mods\Rebalance.
#include "mod.h"

const char MOD_NAME_STR[] = "Rebalance";
const char MOD_VERSION_STR[] = "0.17";

static volatile LONG g_patchState;  // 0 = not yet, 1 = in progress, 2 = done

// Patch once. Steam's wrapper decrypts the game code after DLLs load, so this
// waits until the hook sites hold the expected code.
static void TryPatch(bool final)
{
    if (InterlockedCompareExchange(&g_patchState, 1, 0) != 0) return;
    if (!final && !CodeReady()) {
        g_patchState = 0;
        return;
    }
    if (PatchGame()) UnlocksInit();
    g_patchState = 2;
}

static DWORD WINAPI ModThread(LPVOID)
{
    for (int i = 0; i < 600 && g_patchState != 2; i++) {
        TryPatch(false);
        Sleep(50);
    }
    TryPatch(true);

    LONG dropsFor = 1;  // PatchGame starts with the solo values
    LONG why = -1;
    for (;;) {
        Sleep(250);
        if (g_soloWhy != why) {
            why = g_soloWhy;
            Log("player state: %s", SoloText(why));
        }
        SettingsPoll();
        NetPoll();
        LONG solo = g_lastSolo < 0 ? 1 : g_lastSolo;
        if (InterlockedExchange(&g_dropsDirty, 0) || solo != dropsFor) {
            dropsFor = solo;
            ApplyDrops(solo != 0);
        }
    }
}

void ModMain()
{
    NetInit();
    SettingsInit(ModDir());
    TabsInit();
}

void ModGameReady()
{
    TryPatch(false);  // normally the code is ready now; if not, the thread keeps trying
    if (g_patchState != 2) Log("game code not ready yet; waiting for it");
    CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
}
