// Quickspell for the Witch (cPlWork03) and Oracle (cPlWork04).
//
// While chanting, each press goes to a matcher that keeps a bitmask of spells
// still consistent with the inputs so far. When exactly one remains, or when a
// spell's hotkey is pressed, we answer the chant loop's own button check with
// the rest of its buttons, one per check, waiting for the chant to take each.
// Only that check is touched, so nothing else ever sees a fake press. It all
// stops when the chant ends (cast, chant button pressed again, knocked out of
// it), another button is entered, or the player is hit.
//
// Solo only for now: with other players, every player's chant runs on every
// machine and inputs are synced, which needs more work to do safely.
#include "quickspells.h"
#include <xinput.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <intrin.h>

static uintptr_t g_base;  // set once the hooks are in

static const uint32_t SPELL_TABLE = 0x110B528;   // 20 x { int* seq; int len; int classMask (8 = Witch, 10h = Oracle) }
static const int RESULT_NONE = 0x15;             // matcher: no spell finished yet
static const uint32_t MATCH[QS_CLASSES] = { 0xD26BF0, 0xB6C000 };          // int (btn, int* mask, int index, float)
static const uint32_t CHANT_LOOP_RET[QS_CLASSES] = { 0xD247EF, 0xB6C91F };  // the chant loops' button checks
static const uint32_t BUTTON_TRIGGERED = 0xE07230;   // player: bool (btn)
static const char BTN_NAMES[] = "BAYX";          // button ids 1Ch-1Fh

// Spell ids from the game's help table, in the in-game Help order with its English names.
const QsSpell QS_SPELLS[QS_CLASSES][QS_SPELLS_PER_CLASS] = {
    { { 0, "powerup", "Power up" }, { 1, "homing", "Homing" }, { 2, "condense", "Condense" },
      { 5, "nature", "Enchant Nature" }, { 4, "ice", "Enchant Ice" }, { 3, "fire", "Enchant Fire" },
      { 6, "thunder", "Enchant Thunder" }, { 9, "heal", "Heal/Regen" }, { 8, "shine", "Shine" },
      { 7, "levitation", "Levitation" }, { 10, "statusboost", "Status Boost" }, { 11, "cure", "Cure Ailment" } },
    { { 0, "powerup", "Power up" }, { 1, "homing", "Homing" }, { 10, "regen", "Regen" },
      { 11, "cure", "Cure Ailment" }, { 16, "extension", "Extension" }, { 12, "imbue", "Imbue Element" },
      { 13, "mirage", "Mirage" }, { 15, "feather", "Feather" }, { 17, "protection", "Protection" },
      { 19, "quick", "Quick" }, { 18, "reduction", "Reduction" }, { 14, "armor", "Armor" } },
};
const char* const QS_CLASS_NAMES[QS_CLASSES] = { "Witch", "Oracle" };

using MatchFn = int(__fastcall*)(void*, void*, int, int*, int, float);
using BtnFn = char(__fastcall*)(void*, void*, int);
static MatchFn o_match[QS_CLASSES];
static BtnFn o_button;

volatile LONG g_quickspell = 1;
volatile LONG g_inputDelayMs = 160;
int g_binds[QS_CLASSES][QS_SPELLS_PER_CLASS];
CRITICAL_SECTION g_bindLock;

// ---- state (game thread only) ----
static void* g_player;          // player whose chant loop ran most recently
static int g_class;             // and their class (QS_WITCH / QS_ORACLE)
static uint32_t g_chantFrame;   // frame of the last chant-loop button check
static int g_entered[16], g_nEntered;  // buttons the chant has taken so far
static int g_queue[16], g_qLen, g_qPos;  // buttons still to feed
static uint32_t g_lastFrame;
static ULONGLONG g_lastPressMs;  // when the chant last took a button (ours or the player's)
static bool g_solo;
static bool g_prevHot[QS_SPELLS_PER_CLASS];


int QsSpellSeq(int id, int* out)
{
    if (!g_base || id < 0 || id >= 20) return 0;
    uint8_t* e = (uint8_t*)G(SPELL_TABLE) + 12 * id;
    int len = *(int*)(e + 4);
    if (len <= 0 || len > 16) return 0;
    memcpy(out, *(int**)e, len * sizeof(int));
    return len;
}

static void ClearQueue(const char* why)
{
    if (g_qLen && g_qPos < g_qLen && why) Log("quickspell: stopped (%s)", why);
    g_qLen = g_qPos = 0;
}

static void Enqueue(const int* btns, int n, const char* why)
{
    if (n <= 0 || n > 16) return;
    memcpy(g_queue, btns, n * sizeof(int));
    g_qLen = n;
    g_qPos = 0;
    g_lastPressMs = GetTickCount64();
    char s[17];
    QsFormatSeq(btns, n, s, sizeof s);
    Log("quickspell: %s, entering %s", why, s);
}

// Once per frame while chanting: spell hotkeys and the stall timeout.
static void OnChantFrame(uint32_t f)
{
    static ULONGLONG padRetry;  // polling a missing controller is slow, so retry once a second
    XINPUT_STATE xs{};
    bool pad = false;
    if (GetTickCount64() >= padRetry) {
        pad = XInputGetState(0, &xs) == ERROR_SUCCESS;
        if (!pad) padRetry = GetTickCount64() + 1000;
    }
    EnterCriticalSection(&g_bindLock);
    for (int i = 0; i < QS_SPELLS_PER_CLASS; i++) {
        bool down = QsHotkeyDown(g_binds[g_class][i], pad ? &xs : nullptr);
        if (down && !g_prevHot[i] && g_qPos >= g_qLen) {
            const QsSpell& sp = QS_SPELLS[g_class][i];
            int seq[16];
            int len = QsSpellSeq(sp.id, seq);
            bool fits = len > g_nEntered && !memcmp(seq, g_entered, g_nEntered * sizeof(int));
            char why[48];
            sprintf_s(why, "%s hotkey", sp.name);
            if (fits) Enqueue(seq + g_nEntered, len - g_nEntered, why);
            else Log("quickspell: %s doesn't fit the buttons already entered", why);
        }
        g_prevHot[i] = down;
    }
    LeaveCriticalSection(&g_bindLock);

    if (g_qPos < g_qLen && GetTickCount64() - g_lastPressMs > (ULONGLONG)g_inputDelayMs + 1000)
        ClearQueue("chant stopped taking inputs");
}

static char __fastcall h_button(void* player, void* edx, int btn)
{
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    int cls = -1;
    for (int c = 0; c < QS_CLASSES; c++)
        if (ret == G(CHANT_LOOP_RET[c])) cls = c;
    if (cls < 0) return o_button(player, edx, btn);

    uint32_t f = Frame();
    if (player != g_player || f - g_chantFrame > 10) {  // a new chant
        ClearQueue(nullptr);
        g_nEntered = 0;
        for (bool& h : g_prevHot) h = true;  // a held hotkey must be pressed again
        g_solo = IsSolo();
        if (!g_solo) Log("quickspell: off for this chant (other players in the quest)");
    }
    g_player = player;
    g_class = cls;
    g_chantFrame = f;
    if (!g_solo) return o_button(player, edx, btn);
    if (f != g_lastFrame) {
        g_lastFrame = f;
        OnChantFrame(f);
    }
    // While auto-entering, the next queued button is "pressed" once the delay since the
    // last one has passed, so the chant keeps its rhythm (animation, voice, bubbles).
    if (g_qPos < g_qLen)
        return btn == g_queue[g_qPos] && GetTickCount64() - g_lastPressMs >= (ULONGLONG)g_inputDelayMs;
    return o_button(player, edx, btn);
}

static int AfterMatch(void* player, int btn, int* mask, int index, int result)
{
    if (player != g_player || !g_solo) return result;
    if (index == 0) g_nEntered = 0;
    if (g_nEntered < 16) g_entered[g_nEntered++] = btn;

    if (g_qPos < g_qLen) {
        if (btn == g_queue[g_qPos]) {
            g_qPos++;
            g_lastPressMs = GetTickCount64();
        } else ClearQueue("other button entered");
    }
    if (result != RESULT_NONE) {
        ClearQueue(nullptr);
        return result;
    }
    // Exactly one spell left: finish it.
    uint32_t m = (uint32_t)*mask;
    if (g_quickspell && g_qPos >= g_qLen && m && !(m & (m - 1))) {
        unsigned long i;
        _BitScanForward(&i, m);
        int seq[16];
        int len = QsSpellSeq((int)i, seq);
        if (index + 1 < len) Enqueue(seq + index + 1, len - index - 1, "one spell left");
    }
    return result;
}

static int __fastcall h_matchWitch(void* pl, void* edx, int btn, int* mask, int index, float t)
{
    return AfterMatch(pl, btn, mask, index, o_match[QS_WITCH](pl, edx, btn, mask, index, t));
}

static int __fastcall h_matchOracle(void* pl, void* edx, int btn, int* mask, int index, float t)
{
    return AfterMatch(pl, btn, mask, index, o_match[QS_ORACLE](pl, edx, btn, mask, index, t));
}

// ---- hotkeys ----
// QS_ACT | id: one of the game's own buttons (0x00-0x38), asked from the game
// (0xD82B40 on the app object 0x7C9770, the same check the chant uses), so any
// controller the game supports works, with the player's own button setup.
// QS_PAD | XINPUT_GAMEPAD_* bit and QS_LT / QS_RT: older settings, Xbox-style pads
// only. Anything else: a keyboard virtual-key code.
static const uint32_t APP_OBJECT = 0x7C9770, GAME_BUTTON = 0xD82B40;
static const int ACT_COUNT = 0x39;
static const struct { const char* name; int key; } PAD_KEYS[] = {
    { "LB", QS_ACT | 0x18 }, { "RB", QS_ACT | 0x19 }, { "LT", QS_ACT | 0x1A }, { "RT", QS_ACT | 0x1B },
    { "LS", QS_PAD | XINPUT_GAMEPAD_LEFT_THUMB }, { "RS", QS_PAD | XINPUT_GAMEPAD_RIGHT_THUMB },
    { "Back", QS_PAD | XINPUT_GAMEPAD_BACK }, { "Start", QS_PAD | XINPUT_GAMEPAD_START },
    { "DUp", QS_PAD | XINPUT_GAMEPAD_DPAD_UP }, { "DDown", QS_PAD | XINPUT_GAMEPAD_DPAD_DOWN },
    { "DLeft", QS_PAD | XINPUT_GAMEPAD_DPAD_LEFT }, { "DRight", QS_PAD | XINPUT_GAMEPAD_DPAD_RIGHT },
    { "PadA", QS_PAD | XINPUT_GAMEPAD_A }, { "PadB", QS_PAD | XINPUT_GAMEPAD_B },
    { "PadX", QS_PAD | XINPUT_GAMEPAD_X }, { "PadY", QS_PAD | XINPUT_GAMEPAD_Y },
    { "LB", QS_PAD | XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", QS_PAD | XINPUT_GAMEPAD_RIGHT_SHOULDER },
    { "LT", QS_LT }, { "RT", QS_RT },  // older settings (names above win when reading the ini)
};

static bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool GameButton(int id)
{
    if (!g_base) return false;
    __try {
        void* app = ((void*(__cdecl*)())G(APP_OBJECT))();
        return app && ((char(__thiscall*)(void*, int))G(GAME_BUTTON))(app, id) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool QsHotkeyDown(int key, const XINPUT_STATE* xs)
{
    if (!key) return false;
    if ((key & ~0xFF) == QS_ACT) return GameButton(key & 0xFF);
    if (key == QS_LT) return xs && xs->Gamepad.bLeftTrigger > 100;
    if (key == QS_RT) return xs && xs->Gamepad.bRightTrigger > 100;
    if (key & QS_PAD) return xs && (xs->Gamepad.wButtons & (key & 0xFFFF));
    return GameFocused() && (GetAsyncKeyState(key) & 0x8000);
}

void QsKeyName(int key, char* out, int n)
{
    for (auto& k : PAD_KEYS)
        if (k.key == key) { strcpy_s(out, n, k.name); return; }
    if ((key & ~0xFF) == QS_ACT) { sprintf_s(out, n, "Pad%02X", key & 0xFF); return; }
    if (!key) { strcpy_s(out, n, "(none)"); return; }
    UINT sc = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;
    if ((key >= VK_PRIOR && key <= VK_HELP) || key == VK_DIVIDE || key == VK_RCONTROL || key == VK_RMENU) lp |= 1 << 24;
    if (!GetKeyNameTextA(lp, out, n)) sprintf_s(out, n, "Key%d", key);
}

int QsParseKey(const char* s)
{
    for (auto& k : PAD_KEYS)
        if (!_stricmp(s, k.name)) return k.key;
    if (!strncmp(s, "Key", 3) && atoi(s + 3) > 0) return atoi(s + 3);
    if (!_strnicmp(s, "Pad", 3) && isxdigit((unsigned char)s[3])) {  // a game button: Pad1A
        int id = (int)strtol(s + 3, nullptr, 16);
        if (id >= 0 && id < ACT_COUNT) return QS_ACT | id;
    }
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    for (int vk = 1; vk < 256; vk++) {  // F1, Tab, Shift, Page Up, ... as Windows names them
        char name[32];
        QsKeyName(vk, name, sizeof name);
        if (!_stricmp(s, name)) return vk;
    }
    return 0;
}

// Any hotkey being pressed right now (for rebinding in the menu); 0 if none.
// Esc first (it cancels), then the game's buttons (shoulders and triggers first;
// not A/B/X/Y, 0x1C-0x1F, which the chant itself uses), then other keys.
int QsCapture(int ignoreVk)
{
    if (GameFocused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) return VK_ESCAPE;
    for (int id = 0x18; id <= 0x1B; id++)
        if (GameButton(id)) return QS_ACT | id;
    for (int id = 0; id < ACT_COUNT; id++)
        if ((id < 0x18 || id > 0x1F) && GameButton(id)) {
            Log("quickspell: captured game button %02X", id);
            return QS_ACT | id;
        }
    if (!GameFocused()) return 0;
    for (int vk = 8; vk < 256; vk++) {
        if (vk == ignoreVk || (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

// "XAYABA" -> button ids; returns length, 0 if invalid.
int QsParseSeq(const char* s, int* out)
{
    int n = 0;
    for (; *s; s++) {
        if (*s == ' ') continue;
        const char* p = strchr(BTN_NAMES, toupper((unsigned char)*s));
        if (!p || n >= 16) return 0;
        out[n++] = 0x1C + (int)(p - BTN_NAMES);
    }
    return n;
}

void QsFormatSeq(const int* seq, int len, char* out, int n)
{
    int i = 0;
    for (; i < len && i < n - 1; i++) out[i] = BTN_NAMES[seq[i] - 0x1C];
    out[i] = 0;
}

// The solo check the Rebalance mod also uses (the game's party-size HP scaling): solo-mode
// flag, no online room, or a room with one player. When the game's state is unknown
// (the hub town), we're solo unless we're in an online room.
static const uint32_t GET_GAME = 0x7C9770, GET_GAME_OBJ = 0x7B1770;

bool IsSolo()
{
    __try {
        using GetGame = void*(__cdecl*)();
        using GetObj = uint8_t*(__fastcall*)(void* self, void* unused);
        void* g = ((GetGame)G(GET_GAME))();
        uint8_t* o = g ? ((GetObj)G(GET_GAME_OBJ))(g, nullptr) : nullptr;
        if (!o) return !InOnlineRoom();
        uint8_t* room = *(uint8_t**)(o + 0x3E8);
        return o[0x1446] || !room || !room[0x1A] || room[0x18] <= 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Final hit damage (0xCC6850, thiscall on the target, 4 args): the player being hit
// stops auto-entering.
static const uint32_t HIT_FINAL = 0xCC6850;
using HitFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*);
static HitFn o_hit;
static void __fastcall h_hit(void* target, void* edx, void* a, void* b, void* c, void* d)
{
    if (target && target == g_player) ClearQueue("hit");
    o_hit(target, edx, a, b, c, d);
}

// Every hook site holds the game's code (Steam's wrapper has finished with it).
bool QuickspellReady()
{
    return CodeAt(MATCH[QS_WITCH], "558bec83ec2ca1") && CodeAt(MATCH[QS_ORACLE], "558bec83ec2ca1") &&
           CodeAt(BUTTON_TRIGGERED, "558bec51568b750857568bf9") && CodeAt(HIT_FINAL, "558bec6aff68");
}

bool QuickspellInit()
{
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    struct { uint32_t va; void* fn; void** orig; } hooks[] = {
        { MATCH[QS_WITCH], (void*)h_matchWitch, (void**)&o_match[QS_WITCH] },
        { MATCH[QS_ORACLE], (void*)h_matchOracle, (void**)&o_match[QS_ORACLE] },
        { BUTTON_TRIGGERED, (void*)h_button, (void**)&o_button },
        { HIT_FINAL, (void*)h_hit, (void**)&o_hit },
    };
    bool ok = true;
    for (auto& h : hooks) ok &= HookAt(h.va, h.fn, h.orig);
    Log(ok ? "quickspell hooks installed" : "quickspell hooks failed");
    return ok;
}
