// The in-game menu (Insert by default): Dear ImGui drawn on the game's D3D9 frames.
// Mods add their own tabs (see codemods/common/dmfd_mod_api.h); the loader's Mods tab
// lists what's installed and sets the menu key. The menu key is kept in mods\loader.ini.
#include "content.h"
#include <d3d9.h>
#include <cstdio>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "../codemods/common/dmfd_mod_api.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static PresentFn o_present;
static ResetFn o_reset;

static bool g_ready, g_failed;
static volatile bool g_open;
static HWND g_hwnd;
static WNDPROC g_gameWndProc;
static ULONGLONG g_toastUntil;
static volatile LONG g_frames;

// A notice at the top of the screen (e.g. the room's mods don't match), for a while.
static SRWLOCK g_noticeLock = SRWLOCK_INIT;
static std::string g_notice;
static ULONGLONG g_noticeUntil;
static volatile int g_noticeRect[4];  // where the notice was drawn last (x, y, w, h; w 0 = not shown)
void MenuNotice(const char* text, unsigned seconds)  // 0 seconds = until dismissed in the menu
{
    AcquireSRWLockExclusive(&g_noticeLock);
    g_notice = text ? text : "";
    g_noticeUntil = seconds ? GetTickCount64() + 1000ULL * seconds : ~0ULL;
    ReleaseSRWLockExclusive(&g_noticeLock);
}

static std::string NoticeNow()
{
    std::string n;
    AcquireSRWLockShared(&g_noticeLock);
    if (GetTickCount64() < g_noticeUntil) n = g_notice;
    ReleaseSRWLockShared(&g_noticeLock);
    return n;
}

// The Mod Manager: if it's open, ask it to come to the front (its event); else start it through
// Explorer, as a double-click would. Started by the game itself, it would be the game's child,
// and Steam counts the game's children as the game: "still running" until the manager closed.
static void OpenManager()
{
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\DMFDModManager.Show")) {
        SetEvent(ev);
        CloseHandle(ev);
        Log("menu: brought the open Mod Manager to the front");
        return;
    }
    std::string exe = GameFolder() + "\\DMFD Mod Manager.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("menu: no %s", exe.c_str());
        return;
    }
    std::string quoted = "\"" + exe + "\"";
    if ((INT_PTR)ShellExecuteA(nullptr, "open", "explorer.exe", quoted.c_str(), GameFolder().c_str(), SW_SHOWNORMAL) <= 32)
        Log("menu: could not open %s", exe.c_str());
    else
        Log("menu: opened the Mod Manager");
}
static ImGuiContext* g_ctx;

// ---- menu key (mods\loader.ini) ----
static volatile LONG g_menuKey = VK_INSERT;
static char g_iniPath[MAX_PATH];

static const struct { const char* n; int vk; } MENU_KEYS[] = {
    {"Insert", VK_INSERT}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Tilde", VK_OEM_3}};

static bool MenuKeyName(int vk, char* out, int n)
{
    if (vk >= VK_F1 && vk <= VK_F12) {
        sprintf_s(out, n, "F%d", vk - VK_F1 + 1);
        return true;
    }
    for (auto& k : MENU_KEYS)
        if (k.vk == vk) {
            strcpy_s(out, n, k.n);
            return true;
        }
    return false;
}

static int ParseMenuKey(const char* s)
{
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    for (auto& k : MENU_KEYS)
        if (!_stricmp(s, k.n)) return k.vk;
    return 0;
}

static void KeyName(int vk, char* out, int n)
{
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;
    if (vk >= VK_PRIOR && vk <= VK_HELP) lp |= 1 << 24;  // extended keys (Insert, Home, ...)
    if (!GetKeyNameTextA(lp, out, n)) strcpy_s(out, n, "?");
}

static void LoadMenuKey()
{
    std::string game = GameFolder();
    sprintf_s(g_iniPath, "%s\\mods\\loader.ini", game.c_str());
    char key[32] = "";
    GetPrivateProfileStringA("menu", "key", "", key, sizeof key, g_iniPath);
    if (!key[0]) {
        // first run: keep the key the Rebalance mod used to own
        const std::string olds[] = {game + "\\mods\\Rebalance\\dmfd_mod.ini", game + "\\dmfd_mod.ini"};
        for (const std::string& old : olds) {
            GetPrivateProfileStringA("mod", "menu_key", "", key, sizeof key, old.c_str());
            if (char* c = strchr(key, ';')) *c = 0;
            for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
            if (key[0]) break;
        }
        if (!ParseMenuKey(key)) strcpy_s(key, "Insert");
        WritePrivateProfileStringA("menu", "key", key, g_iniPath);
    }
    int vk = ParseMenuKey(key);
    g_menuKey = vk ? vk : VK_INSERT;
    if (!vk) Log("menu: unknown key '%s' in loader.ini, using Insert", key);
}

static void SaveMenuKey(int vk)
{
    char name[16];
    if (!MenuKeyName(vk, name, sizeof name)) return;
    g_menuKey = vk;
    WritePrivateProfileStringA("menu", "key", name, g_iniPath);
    Log("menu: key is now %s", name);
}

// ---- tabs from mods ----
struct Tab {
    std::string folder, name;  // the mod's folder in mods\ (set while the loader starts it)
    void (*draw)(void*);
    void* user;
};
static std::vector<Tab> g_tabs;
static CRITICAL_SECTION g_tabLock;
static std::string g_starting;  // folder of the mod the loader is starting right now

void MenuStartingMod(const std::string& folder) { g_starting = folder; }

static void ApiAddTab(const char* mod, const char* name, void (*draw)(void*), void* user)
{
    EnterCriticalSection(&g_tabLock);
    g_tabs.push_back({g_starting, name ? name : "?", draw, user});
    LeaveCriticalSection(&g_tabLock);
    Log("menu: %s added the %s page", mod, name);
}

static void* ApiContext() { return g_ctx; }
static void ApiAllocators(void** a, void** f, void** u)
{
    ImGuiMemAllocFunc af;
    ImGuiMemFreeFunc ff;
    ImGui::GetAllocatorFunctions(&af, &ff, u);
    *a = (void*)af;
    *f = (void*)ff;
}
static int ApiMenuKey() { return g_menuKey; }
static bool ApiMenuOpen() { return g_open; }
static uint32_t ApiFrame() { return g_frames ? (uint32_t)g_frames : (uint32_t)(GetTickCount64() / 16); }

// Mods' per-frame drawing (API 3): run in Present, state saved and restored around them.
struct FrameDraw { void (*draw)(void*, void*); void* user; };
static std::vector<FrameDraw> g_frameDraws;
static SRWLOCK g_frameDrawLock = SRWLOCK_INIT;
static void ApiAddFrameDraw(void (*draw)(void*, void*), void* user)
{
    if (!draw) return;
    AcquireSRWLockExclusive(&g_frameDrawLock);
    g_frameDraws.push_back({draw, user});
    ReleaseSRWLockExclusive(&g_frameDrawLock);
}
static void CallFrameDraw(void (*draw)(void*, void*), IDirect3DDevice9* dev, void* user)
{
    __try { draw(dev, user); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void RunFrameDraws(IDirect3DDevice9* dev)
{
    AcquireSRWLockShared(&g_frameDrawLock);
    std::vector<FrameDraw> list = g_frameDraws;
    ReleaseSRWLockShared(&g_frameDrawLock);
    if (list.empty()) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
    if (SUCCEEDED(dev->BeginScene())) {
        for (auto& f : list) CallFrameDraw(f.draw, dev, f.user);
        dev->EndScene();
    }
    if (sb) { sb->Apply(); sb->Release(); }
}

static std::string g_gameDirStr;
static DmfdApi g_api;

const DmfdApi* LoaderApi()
{
    if (!g_api.apiVersion) {
        g_gameDirStr = GameFolder();
        g_api = {DMFD_API_VERSION, IMGUI_VERSION_NUM, CONTENT_VERSION, g_gameDirStr.c_str(),
                 ApiAddTab, ApiContext, ApiAllocators, ApiMenuKey, ApiMenuOpen, ApiFrame, NetInRoom,
                 ApiAddFrameDraw};
    }
    return &g_api;
}

// ---- input ----
// Window messages arrive on the window's thread; ImGui runs on the render
// thread, so input is queued and replayed at the start of each frame.
struct Msg { UINT m; WPARAM w; LPARAM l; };
static CRITICAL_SECTION g_qLock;
static std::vector<Msg> g_queue;
static volatile bool g_capturingMenuKey;  // rebinding happens here, so the old key doesn't close the menu

static bool IsInput(UINT m)
{
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_SETFOCUS ||
           m == WM_KILLFOCUS;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_capturingMenuKey && m == WM_KEYDOWN) {
        char name[16];
        if ((int)w != VK_ESCAPE && MenuKeyName((int)w, name, sizeof name)) SaveMenuKey((int)w);
        if ((int)w == VK_ESCAPE || MenuKeyName((int)w, name, sizeof name)) g_capturingMenuKey = false;
        return 0;
    }
    if (m == WM_KEYDOWN && (int)w == g_menuKey) {
        if (!(l & 0x40000000)) g_open = !g_open;  // ignore key repeat
        return 0;
    }
    // a click on the notice (e.g. the room's mods don't match) opens the menu, where its buttons are
    if (!g_open && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) && g_noticeRect[2] > 0) {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (x >= g_noticeRect[0] && y >= g_noticeRect[1] && x < g_noticeRect[0] + g_noticeRect[2] && y < g_noticeRect[1] + g_noticeRect[3]) {
            if (m == WM_LBUTTONUP) g_open = true;
            return 0;  // not for the game
        }
    }
    if (g_open && IsInput(m)) {
        EnterCriticalSection(&g_qLock);
        g_queue.push_back({m, w, l});
        LeaveCriticalSection(&g_qLock);
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) return 0;  // keep clicks off the game window
    }
    return CallWindowProcW(g_gameWndProc, h, m, w, l);
}

static void Init(IDirect3DDevice9* dev)
{
    IDirect3DSwapChain9* sc = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
        sc->GetPresentParameters(&pp);
        sc->Release();
    }
    D3DDEVICE_CREATION_PARAMETERS cp{};
    dev->GetCreationParameters(&cp);
    g_hwnd = pp.hDeviceWindow ? pp.hDeviceWindow : cp.hFocusWindow;
    if (!g_hwnd) {
        Log("menu: no game window found; menu disabled");
        g_failed = true;
        return;
    }

    g_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // the game decides whether its cursor shows
    float h = pp.BackBufferHeight ? (float)pp.BackBufferHeight : 720.0f;
    float size = h / 1080.0f * 22.0f;
    if (size < 15.0f) size = 15.0f;
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", size);
    else
        io.FontGlobalScale = size / 13.0f;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.ScaleAllSizes(size / 15.0f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(dev);
    g_gameWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    g_toastUntil = GetTickCount64() + 10000;
    g_ready = true;
    Log("menu ready: window %p, %ux%u", g_hwnd, pp.BackBufferWidth, pp.BackBufferHeight);
}

static void ErrorText(const char* s) { ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1), "%s", s); }

// The loader's own page: what's installed, and the menu key.
static void DrawLoaderPage()
{
    ImGui::TextDisabled("Switch mods on or off in the Mod Manager (or each mod's mod.ini), then restart the game.");
    if (!DataModsWarning().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1));
        ImGui::TextWrapped("%s", DataModsWarning().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Your installed mods");
    if (ImGui::BeginTable("mods", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (const LoadedMod& m : LoadedMods()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.version.c_str());
            ImGui::TableNextColumn();
            if (m.error) ErrorText(m.state.c_str());
            else ImGui::TextUnformatted(m.state.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    char key[32];
    if (g_capturingMenuKey) strcpy_s(key, "press a key...");
    else KeyName(g_menuKey, key, sizeof key);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Menu key");
    ImGui::SameLine();
    if (ImGui::Button(key, ImVec2(ImGui::GetFontSize() * 6.5f, 0))) g_capturingMenuKey = !g_capturingMenuKey;
    if (ImGui::IsItemHovered() && !g_capturingMenuKey)
        ImGui::SetTooltip("Click, then press F1-F12, Insert, Home, End, Page Up,\nPage Down or the tilde key. Esc cancels.");
}

static bool SafeDraw(void (*draw)(void*), void* user)
{
    __try {
        draw(user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void DrawTab(const Tab& t)
{
    if (!SafeDraw(t.draw, t.user)) {
        ImGui::SetCurrentContext(g_ctx);
        ErrorText("This page crashed; see the mod's mod.log.");
    }
    ImGui::SetCurrentContext(g_ctx);
}

// Mods on the left (like the Mod Manager), the chosen mod's pages on the right.
static int g_sel = -1;  // index in LoadedMods(), -1 = the loader's page (shown first)

static void DrawMenu()
{
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(em * 46, em * 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(em * 30, em * 16), ImVec2(FLT_MAX, FLT_MAX));
    bool open = true;
    if (!ImGui::Begin("DMFD Mods###menu", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    std::string notice = NoticeNow();
    if (!notice.empty()) {  // e.g. an online room's mods didn't match: a framed box of its own
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.72f, 0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.20f, 0.05f, 0.55f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::BeginChild("notice", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1));
        ImGui::TextUnformatted("ATTENTION: ONLINE ROOM");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", notice.c_str());
        if (ImGui::Button("Open the Mod Manager")) OpenManager();
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) MenuNotice("", 1);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }
    std::vector<Tab> tabs;
    EnterCriticalSection(&g_tabLock);
    tabs = g_tabs;
    LeaveCriticalSection(&g_tabLock);
    const std::vector<LoadedMod>& mods = LoadedMods();
    if (g_sel >= (int)mods.size()) g_sel = -1;

    char key[32];
    KeyName(g_menuKey, key, sizeof key);
    float footer = ImGui::GetFrameHeightWithSpacing();

    ImGui::BeginChild("list", ImVec2(em * 12, -footer), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("Mod loader", g_sel == -1)) g_sel = -1;
    ImGui::Separator();
    for (size_t i = 0; i < mods.size(); i++) {
        const LoadedMod& m = mods[i];
        if (!m.started) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable((m.name + "##" + m.folder).c_str(), g_sel == (int)i)) g_sel = (int)i;
        if (!m.started) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !m.started) ImGui::SetTooltip("%s", m.state.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("page", ImVec2(0, -footer));
    bool loaderPage = g_sel < 0;
    if (loaderPage) {
        ImGui::Text("DMFD mod loader %s", CONTENT_VERSION);
        ImGui::Separator();
        DrawLoaderPage();
    } else {
        const LoadedMod& m = mods[g_sel];
        ImGui::Text("%s %s", m.name.c_str(), m.version.c_str());
        if (!m.author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("by %s", m.author.c_str());
        }
        if (!m.description.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", m.description.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        std::vector<const Tab*> own;
        for (const Tab& t : tabs)
            if (t.folder == m.folder) own.push_back(&t);
        if (!m.started) {
            if (m.error) ErrorText(m.state.c_str());
            else ImGui::TextDisabled("This mod is %s.", m.state.c_str());
        } else if (own.empty()) {
            ImGui::TextDisabled("This mod has no settings; it works while it's switched on in the Mod Manager.");
        } else if (own.size() == 1) {
            ImGui::PushID(own[0]->name.c_str());
            DrawTab(*own[0]);
            ImGui::PopID();
        } else if (ImGui::BeginTabBar("pages")) {
            for (const Tab* t : own) {
                ImGui::PushID(t->name.c_str());
                if (ImGui::BeginTabItem(t->name.c_str())) {
                    DrawTab(*t);
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    if (!loaderPage) g_capturingMenuKey = false;
    ImGui::TextDisabled("%s closes this menu.", key);
    ImGui::End();
    if (!open) {
        g_open = false;
        g_capturingMenuKey = false;
    }
}

static void DrawToast()
{
    char key[32];
    KeyName(g_menuKey, key, sizeof key);
    int code = 0;
    for (const LoadedMod& m : LoadedMods()) code += m.started;
    ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 20), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::Text("DMFD mod loader %s: %d mod(s) running. Press %s for settings.", CONTENT_VERSION, code, key);
    if (!DataModsWarning().empty()) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "%s", DataModsWarning().c_str());
    ImGui::End();
}

static void DrawNotice(const std::string& text)
{
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float w = ds.x * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.06f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, ds.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    char key[32];
    KeyName(g_menuKey, key, sizeof key);
    ImGui::TextDisabled("Click here or press %s to open the Mod Manager or dismiss this.", key);
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();  // display = client pixels (the click test)
    g_noticeRect[0] = (int)p.x;
    g_noticeRect[1] = (int)p.y;
    g_noticeRect[3] = (int)sz.y;
    g_noticeRect[2] = (int)sz.x;
    ImGui::End();
}

static HRESULT STDMETHODCALLTYPE h_present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    InterlockedIncrement(&g_frames);
    if (!g_ready && !g_failed) Init(dev);
    RunFrameDraws(dev);
    if (g_ready) {
        std::vector<Msg> q;
        EnterCriticalSection(&g_qLock);
        q.swap(g_queue);
        LeaveCriticalSection(&g_qLock);

        bool toast = GetTickCount64() < g_toastUntil;
        std::string notice = NoticeNow();
        if (notice.empty() || g_open) g_noticeRect[2] = 0;  // nothing to click
        if (g_open || toast || !notice.empty()) {
            ImGui::SetCurrentContext(g_ctx);
            for (auto& m : q) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.m, m.w, m.l);
            // Draw our own cursor only where the game hides Windows' (it does in
            // some modes); drawing it over a visible one leaves a lagging double.
            CURSORINFO ci{sizeof ci};
            ImGui::GetIO().MouseDrawCursor = g_open && GetCursorInfo(&ci) && (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor);
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (g_open) DrawMenu();
            else if (toast) DrawToast();
            if (!notice.empty() && !g_open) DrawNotice(notice);
            else g_noticeRect[2] = 0;
            ImGui::EndFrame();
            ImGui::Render();
            if (SUCCEEDED(dev->BeginScene())) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
        }
    }
    return o_present(dev, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE h_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = o_reset(dev, pp);
    if (g_ready && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// Present and Reset are shared by every D3D9 device, so a throwaway device on a
// hidden window is enough to find them.
// Finding Present needs a throwaway graphics device, which takes a moment: that runs in the
// background so the game's start isn't held up (the menu starts from whichever frame it catches).
static DWORD WINAPI HookThread(LPVOID)
{
    WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), 0, 0, 0, 0, "dmfd_loader_dummy"};
    RegisterClassA(&wc);
    HWND w = CreateWindowA("dmfd_loader_dummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, wc.hInstance, 0);
    bool ok = false;
    if (IDirect3D9* d9 = Direct3DCreate9(D3D_SDK_VERSION)) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = w;
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev))) {
            void** vt = *(void***)dev;
            HookLock lock;
            ok = MH_CreateHook(vt[17], h_present, (void**)&o_present) == MH_OK &&
                 MH_CreateHook(vt[16], h_reset, (void**)&o_reset) == MH_OK && MH_EnableHook(vt[17]) == MH_OK &&
                 MH_EnableHook(vt[16]) == MH_OK;
            dev->Release();
        }
        d9->Release();
    }
    DestroyWindow(w);
    UnregisterClassA("dmfd_loader_dummy", wc.hInstance);
    char key[16] = "?";
    MenuKeyName(g_menuKey, key, sizeof key);
    Log(ok ? "menu: hooks installed (key %s)" : "menu: hooks failed; in-game menu disabled", key);
    return 0;
}

void MenuInit()
{
    InitializeCriticalSection(&g_qLock);
    InitializeCriticalSection(&g_tabLock);
    LoadMenuKey();
    CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
}
