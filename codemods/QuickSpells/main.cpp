// Quick Spells mod: started by the mod loader from mods\QuickSpells. Settings in
// quickspells.ini there; the Spells tab in the in-game menu edits them.
#include "quickspells.h"
#include <cstdio>
#include "imgui.h"

const char MOD_NAME_STR[] = "Quick Spells";
const char MOD_VERSION_STR[] = "1.0";

static CRITICAL_SECTION g_lock;
static QsSettings g_s;
static char g_path[MAX_PATH];
static FILETIME g_mtime;

static const char DEFAULT_INI[] =
    "; Quick Spells: Witch and Oracle chanting (solo only for now).\n"
    "; enabled = 1: once only one spell matches what you've entered, the rest is entered for you.\n"
    "; Spell hotkeys (set them in the in-game menu's Spells tab), e.g. witch_condense=LB: while\n"
    ";   chanting, the hotkey enters the whole spell. Hotkeys: LB RB LT RT, a game button\n"
    ";   (Pad1A), or a keyboard key (R, F5, Tab, ...).\n"
    "; input_delay_ms = time between the buttons entered for you (50-400; vanilla-like rhythm is ~160).\n"
    "[quickspell]\n"
    "enabled=0\n"
    "input_delay_ms=160\n";

static void BindKey(int cls, int i, char* out, int n)
{
    sprintf_s(out, n, "%s_%s", cls == QS_WITCH ? "witch" : "oracle", QS_SPELLS[cls][i].slug);
}

static void Trim(char* s)
{
    if (char* sc = strchr(s, ';')) *sc = 0;
    for (char* e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
}

static bool FileTime(FILETIME* ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &a)) return false;
    *ft = a.ftLastWriteTime;
    return true;
}

QsSettings QsGet()
{
    EnterCriticalSection(&g_lock);
    QsSettings s = g_s;
    LeaveCriticalSection(&g_lock);
    return s;
}

void QsSet(const QsSettings& s, bool save)
{
    EnterCriticalSection(&g_lock);
    g_s = s;
    g_quickspell = s.autoFinish;
    g_inputDelayMs = s.inputDelayMs;
    EnterCriticalSection(&g_bindLock);
    memcpy(g_binds, s.binds, sizeof g_binds);
    LeaveCriticalSection(&g_bindLock);
    if (save) {
        WritePrivateProfileStringA("quickspell", "enabled", s.autoFinish ? "1" : "0", g_path);
        char delay[16];
        sprintf_s(delay, "%d", s.inputDelayMs);
        WritePrivateProfileStringA("quickspell", "input_delay_ms", delay, g_path);
        for (int c = 0; c < QS_CLASSES; c++)
            for (int i = 0; i < QS_SPELLS_PER_CLASS; i++) {
                char key[40], val[32];
                BindKey(c, i, key, sizeof key);
                QsKeyName(s.binds[c][i], val, sizeof val);
                WritePrivateProfileStringA("quickspell", key, s.binds[c][i] ? val : nullptr, g_path);
            }
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_path);  // flush the profile cache
        FileTime(&g_mtime);
    }
    LeaveCriticalSection(&g_lock);
}

static void Load()
{
    QsSettings s{};
    s.autoFinish = GetPrivateProfileIntA("quickspell", "enabled", 0, g_path) != 0;
    s.inputDelayMs = GetPrivateProfileIntA("quickspell", "input_delay_ms", 160, g_path);
    if (s.inputDelayMs < 0) s.inputDelayMs = 0;
    if (s.inputDelayMs > 1000) s.inputDelayMs = 1000;
    int nb = 0;
    for (int c = 0; c < QS_CLASSES; c++)
        for (int i = 0; i < QS_SPELLS_PER_CLASS; i++) {
            char k[40], v[64];
            BindKey(c, i, k, sizeof k);
            GetPrivateProfileStringA("quickspell", k, "", v, sizeof v, g_path);
            Trim(v);
            if (!v[0]) continue;
            s.binds[c][i] = QsParseKey(v);
            if (!s.binds[c][i]) Log("unknown hotkey '%s' for %s", v, k);
            else nb++;
        }
    QsSet(s, false);
    Log("settings: auto-finish %s, %d ms between buttons, %d spell hotkey(s)", s.autoFinish ? "on" : "off",
        s.inputDelayMs, nb);
}

// First run: the [quickspell] section of the Rebalance mod's settings (where these
// used to live), or a default file.
static void CreateIni()
{
    char olds[2][MAX_PATH];
    sprintf_s(olds[0], "%s\\mods\\Rebalance\\dmfd_mod.ini", GameDir());
    sprintf_s(olds[1], "%s\\dmfd_mod.ini", GameDir());
    for (auto& old : olds) {
        char sec[8192];
        DWORD n = GetPrivateProfileSectionA("quickspell", sec, sizeof sec, old);
        if (n > 0 && n < sizeof sec - 2) {
            FILE* f;
            if (!fopen_s(&f, g_path, "w")) {
                fputs(DEFAULT_INI, f);
                fclose(f);
            }
            WritePrivateProfileSectionA("quickspell", sec, g_path);
            WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_path);
            Log("settings carried over from %s", old);
            return;
        }
    }
    FILE* f;
    if (!fopen_s(&f, g_path, "w")) {
        fputs(DEFAULT_INI, f);
        fclose(f);
        Log("wrote default %s", g_path);
    }
}

// ---- the Spells tab ----
static int g_capturing = -1;  // spell row (class * 16 + index) waiting for a new hotkey
static bool g_captureArmed;   // the click that started capturing has been released
static uint32_t g_lastDrawn;  // frame the tab was last drawn (capturing stops when it isn't)

static void DrawSpellList(QsSettings& s, int cls, bool& changed, bool& commit)
{
    if (!ImGui::BeginTable("spells", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("Spell", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Buttons");
    ImGui::TableSetupColumn("Hotkey");
    ImGui::TableHeadersRow();
    float keyW = ImGui::GetFontSize() * 6.5f;
    for (int i = 0; i < QS_SPELLS_PER_CLASS; i++) {
        const QsSpell& sp = QS_SPELLS[cls][i];
        int id = cls * 16 + i;
        int& bind = s.binds[cls][i];
        ImGui::PushID(id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sp.name);
        ImGui::TableNextColumn();
        int seq[16];
        char btns[20];
        QsFormatSeq(seq, QsSpellSeq(sp.id, seq), btns, sizeof btns);
        ImGui::TextUnformatted(btns);
        ImGui::TableNextColumn();
        char key[32];
        if (g_capturing == id) strcpy_s(key, "press a key...");
        else if (bind) QsKeyName(bind, key, sizeof key);
        else strcpy_s(key, "-");
        if (ImGui::Button(key, ImVec2(keyW, 0))) {
            g_capturing = g_capturing == id ? -1 : id;
            g_captureArmed = false;
        }
        if (ImGui::IsItemHovered() && g_capturing != id)
            ImGui::SetTooltip("Click, then press a controller button or key.\nEsc cancels; right-click clears.");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && bind) {
            bind = 0;
            g_capturing = -1;
            changed = commit = true;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void DrawSpellsTab()
{
    QsSettings s = QsGet();
    bool changed = false, commit = false;
    uint32_t f = Frame();
    if (f - g_lastDrawn > 2) g_capturing = -1;  // the tab was closed meanwhile
    g_lastDrawn = f;

    if (ImGui::Checkbox("Finish spells automatically", &s.autoFinish)) changed = commit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Once only one spell matches what you've entered, the rest is\n"
                          "entered for you. Press the chant button again to cancel.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::SliderInt("Time between buttons", &s.inputDelayMs, 50, 400, "%d ms")) changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How fast auto-finish and hotkeys enter buttons.\n"
                          "Around 160 ms keeps the chant's normal rhythm, sounds and effects.");
    ImGui::TextDisabled("Hotkeys work while chanting: start the chant, then press the hotkey.");
    ImGui::TextDisabled("Solo only for now.");

    if (g_capturing >= 0) {
        int k = QsCapture(MenuKey());
        if (!k) g_captureArmed = true;
        else if (g_captureArmed) {
            if (k != VK_ESCAPE) {
                s.binds[g_capturing / 16][g_capturing % 16] = k;
                changed = commit = true;
            }
            g_capturing = -1;
        }
    }

    if (ImGui::BeginTabBar("classes")) {
        for (int c = 0; c < QS_CLASSES; c++)
            if (ImGui::BeginTabItem(QS_CLASS_NAMES[c])) {
                DrawSpellList(s, c, changed, commit);
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("Changes are saved to mods\\QuickSpells\\quickspells.ini.");
    if (changed || commit) QsSet(s, commit);
}

static bool g_hooked;

static DWORD WINAPI ModThread(LPVOID)
{
    if (!g_hooked) {  // the code wasn't ready at the game's first step: wait for it (up to a minute)
        bool ready = false;
        for (int i = 0; i < 1200 && !(ready = QuickspellReady()); i++) Sleep(50);
        if (!ready || !QuickspellInit()) {
            Log("game code not as expected; Quick Spells is off");
            return 0;
        }
    }
    for (;;) {
        Sleep(500);
        FILETIME ft;
        EnterCriticalSection(&g_lock);
        bool changed = FileTime(&ft) && CompareFileTime(&ft, &g_mtime);
        if (changed) g_mtime = ft;
        LeaveCriticalSection(&g_lock);
        if (changed) {
            Sleep(100);  // let the editor finish writing
            Load();
        }
    }
}

void ModMain()
{
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_bindLock);
    sprintf_s(g_path, "%s\\quickspells.ini", ModDir());
    if (GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) CreateIni();
    FileTime(&g_mtime);
    Load();
    AddMenuTab("Spells", DrawSpellsTab);
}

void ModGameReady()
{
    g_hooked = QuickspellReady() && QuickspellInit();
    CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);  // settings file watch (and the hooks, if not in yet)
}
