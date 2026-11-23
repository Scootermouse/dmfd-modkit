// Rebalance's tabs in the loader's in-game menu: Balance and Classes.
#include "mod.h"
#include <cstdio>
#include "imgui.h"

static const char* const LABELS[K_COUNT] = { "Damage taken", "Damage dealt", "Enemy HP", "Boss HP", "EXP", "Gold", "Rare drops" };
static const char* const TIPS[K_COUNT] = {
    "Damage you take from enemies. Healing is unaffected; hits always do at least 1.",
    "Damage enemies and bosses take.",
    "Max HP of regular enemies. Applies to enemies that spawn after the change.",
    "Max HP of bosses. Applies to bosses that spawn after the change.",
    "EXP gained from quests.",
    "Gold gained during quests. Shop sales are unaffected.",
    "Chance of rare drops (capped at 100%).",
};
static const float RANGE[K_COUNT][2] = { { 0, 2 }, { 0.25f, 4 }, { 0.1f, 2 }, { 0.1f, 2 }, { 0.5f, 5 }, { 0.5f, 5 }, { 0.5f, 5 } };

// Which set of values is in use right now, and why.
static void Status()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Solo values apply when you're the only player in a quest; Co-op values when others\n"
                        "play with you (combat values then come from the host, if everyone has Rebalance).");
    LONG why = g_soloWhy;
    NetState net = NetGet();
    const char* inUse;
    if (why < 0) inUse = "not known yet (it's checked once you're in a quest or a room)";
    else if (g_lastSolo == 1) inUse = "Solo values";
    else if (!net.inRoom) inUse = "Co-op values";
    else if (!net.synced) inUse = "Co-op values; combat is vanilla (not everyone in the room has Rebalance)";
    else if (net.hosting) inUse = "Co-op values; you're the host, so everyone uses your combat values";
    else inUse = "Co-op values; combat values come from the host";
    ImGui::Text("In use now: %s", inUse);
    ImGui::TextDisabled("Game state: %s", SoloText(why));
    if (net.inRoom) ImGui::TextDisabled("Online room: %d player(s), %s.", net.members, net.hosting ? "you're hosting" : "joined");
    ImGui::TextDisabled("Changes are saved to mods\\Rebalance\\dmfd_mod.ini.");
}

static void DrawBalanceTab()
{
    Settings s = SettingsGet();
    bool changed = false, commit = false;
    NetState net = NetGet();
    ImGui::TextDisabled("Preset");
    for (int i = 0; i < PRESET_COUNT; i++) {
        bool cur = !_stricmp(s.preset, PRESET_NAMES[i]);
        if (i) ImGui::SameLine();
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(PRESET_NAMES[i]) && !cur) {
            if (i == PRESET_COUNT - 1) {  // Custom starts from what's in effect now
                s.customSolo = s.solo;
                s.customCoop = s.coopHost;
            }
            strcpy_s(s.preset, PRESET_NAMES[i]);
            changed = commit = true;
        }
        if (cur) ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("vals", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Solo", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Co-op", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int k = 0; k < K_COUNT; k++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LABELS[k]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TIPS[k]);
            for (int col = 0; col < 2; col++) {
                ImGui::TableNextColumn();
                bool coop = col == 1;
                if (coop && KIND_COMBAT[k] && net.synced && !net.hosting) {
                    ImGui::TextDisabled("host: x%.2f", net.host.m[k]);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("In co-op, combat settings come from the host,\nso every player's game agrees.");
                    continue;
                }
                // Co-op combat sliders are your values as host; they apply only when
                // everyone in the room has the mod.
                float v = (coop ? s.coopHost : s.solo).m[k];
                ImGui::PushID(k * 2 + col);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##v", &v, RANGE[k][0], RANGE[k][1], "x%.2f")) {
                    if (_stricmp(s.preset, "Custom")) {  // editing a preset turns it into Custom
                        s.customSolo = s.solo;
                        s.customCoop = s.coopHost;
                        strcpy_s(s.preset, "Custom");
                    }
                    (coop ? s.customCoop : s.customSolo).m[k] = v;
                    changed = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nCtrl+click to type a value.", TIPS[k]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    Status();
    if (changed || commit) SettingsSet(s, commit);
}

static void DrawClassesTab()
{
    Settings s = SettingsGet();
    bool changed = false, commit = false;
    NetState net = NetGet();
    bool hostRules = net.synced && !net.hosting;
    ImGui::TextDisabled("Multiplies each class's damage, on top of the Balance tab.");
    if (hostRules)
        ImGui::TextDisabled("Co-op uses the host's values (shown in brackets). Yours apply solo.");
    else
        ImGui::TextDisabled("Used solo, and for everyone in your room when you host.");
    ImGui::Spacing();
    if (ImGui::BeginTable("cls", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Damage dealt", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Damage taken", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int c = 0; c < CLS_COUNT; c++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(CLASS_NAMES[c]);
            for (int col = 0; col < 2; col++) {
                ImGui::TableNextColumn();
                float& v = col ? s.classes.taken[c] : s.classes.dealt[c];
                float host = col ? net.hostClasses.taken[c] : net.hostClasses.dealt[c];
                char fmt[32];
                if (hostRules) sprintf_s(fmt, "x%%.2f  (x%.2f)", host);
                else strcpy_s(fmt, "x%.2f");
                ImGui::PushID(c * 2 + col);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##v", &v, col ? 0.0f : 0.25f, col ? 2.0f : 4.0f, fmt)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(col ? "Damage %s takes from enemies.\nCtrl+click to type a value."
                                          : "Damage %s's attacks and spells deal to enemies.\nCtrl+click to type a value.",
                                      CLASS_NAMES[c]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset all to x1.00")) {
        for (int c = 0; c < CLS_COUNT; c++) s.classes.dealt[c] = s.classes.taken[c] = 1.0f;
        changed = commit = true;
    }
    if (ImGui::Checkbox("Oracle and Bandit unlocked from the start", &s.unlockClasses)) changed = commit = true;
    Status();
    if (changed || commit) SettingsSet(s, commit);
}

void TabsInit()
{
    AddMenuTab("Balance", DrawBalanceTab);
    AddMenuTab("Classes", DrawClassesTab);
}
