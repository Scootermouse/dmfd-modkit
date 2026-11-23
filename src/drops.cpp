// Treasure chest contents from <game>\DataMod\drops.txt.
//
// Chests (cTreasureBoxDB, TREASUREBOX_QUEST_q_s_c) each point at a run of
// drop tables named DROP_q_s_c_N<level> / DROP_q_s_c_R<level>. When a chest
// opens the game takes the first normal and first rare table whose level
// (+6) is at least the current level, rolls the rare table's chance (+5),
// then picks one of its 16 slots by weight among the items the player's
// class can use (0xbd6320). Record (0x68 bytes): +0 label, +4 rare flag,
// +5 rare chance, +6 level, +8 16 x u16 weight, +0x28 16 x u32 item label
// (offsets into the table's string pool).
//
// We edit slots and weights in place and never move the table or touch
// +4/+5, which the rebalance mod (dinput8.dll) scales for its rare-drop
// setting at the same fixed address.
//
// drops.txt, one change per line ('#' comments):
//   DROP_1_0_0_N9 MOD_SWORD_ 500     item into the table (or change its weight)
//   DROP_1_0_0_N9 USE003E_ 0         weight 0 = never drops
#include "content.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

static const uint32_t DROP_DB = 0x1358318;  // header: count, recSize, treeOfs, recOfs, strOfs
static const uint32_t DROP_COUNT = 4973, DROP_RECSIZE = 0x68, SLOTS = 16;

static uint8_t* Game(uint32_t va) { return (uint8_t*)GetModuleHandleA(nullptr) + va - 0x400000; }

void DropsApply()
{
    uint8_t* db = Game(DROP_DB);
    uint32_t* hdr = (uint32_t*)db;
    if (hdr[0] != DROP_COUNT || hdr[1] != DROP_RECSIZE) {
        Log("drops: chest table not as expected (count %u, size %#x); edits skipped", hdr[0], hdr[1]);
        return;
    }
    uint8_t* recs = db + hdr[3];
    const char* pool = (const char*)db + hdr[4];

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    strcat_s(path, "\\DataMod\\drops.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return;

    std::unordered_map<std::string, uint8_t*> byName;
    byName.reserve(DROP_COUNT);
    for (uint32_t i = 0; i < DROP_COUNT; i++)
        byName.emplace(pool + *(uint32_t*)(recs + DROP_RECSIZE * i), recs + DROP_RECSIZE * i);

    DWORD old;
    VirtualProtect(recs, DROP_RECSIZE * DROP_COUNT, PAGE_READWRITE, &old);
    char line[256];
    int no = 0, changed = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        if (char* c = strchr(line, '#')) *c = 0;
        char table[96], label[96];
        unsigned weight;
        if (sscanf_s(line, "%95s %95s %u", table, (unsigned)sizeof table, label, (unsigned)sizeof label, &weight) != 3)
            continue;
        auto it = byName.find(table);
        uint8_t* rec = it == byName.end() ? nullptr : it->second;
        if (!rec) {
            Log("drops.txt line %d: no chest table %s", no, table);
            continue;
        }
        uint16_t* w = (uint16_t*)(rec + 8);
        uint32_t* slot = (uint32_t*)(rec + 0x28);
        int at = -1;
        for (uint32_t s = 0; s < SLOTS && at < 0; s++)  // already there?
            if (w[s] && !strcmp(pool + slot[s], label)) at = (int)s;
        if (at < 0 && weight)
            for (uint32_t s = 0; s < SLOTS && at < 0; s++)
                if (!w[s]) at = (int)s;
        if (at < 0) {
            if (weight) Log("drops.txt line %d: %s has no free slot for %s", no, table, label);
            continue;
        }
        if (strcmp(pool + slot[at], label)) {
            // The label must live somewhere the game reads as pool + offset;
            // our copy is kept for the life of the game (offsets wrap).
            size_t n = strlen(label) + 1;
            char* copy = (char*)HeapAlloc(GetProcessHeap(), 0, n);
            if (!copy) continue;
            memcpy(copy, label, n);
            slot[at] = (uint32_t)((uintptr_t)copy - (uintptr_t)pool);
        }
        w[at] = (uint16_t)(weight > 65535 ? 65535 : weight);
        changed++;
    }
    fclose(f);
    VirtualProtect(recs, DROP_RECSIZE * DROP_COUNT, old, &old);
    Log("drops: %d chest slot(s) changed", changed);
}
