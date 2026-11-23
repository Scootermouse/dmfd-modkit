// Item table edits and new items from <game>\DataMod\items.txt.
//
// The item database is built into the exe (.data): a header, a hash tree
// {hash, left, right, recordIndex} and 0x88-byte records, plus a string pool
// that records point into by offset. Record 0 is an empty placeholder, so the
// tree has count-1 entries. The game reaches the table only through four
// globals (header, tree, records, strings), set by a startup function. To add
// items we build a bigger header+tree+records in our own memory and repoint
// the first three; the string pool stays where it is, and new labels live in
// our memory with offsets that wrap around (the game just adds them).
//
// items.txt, one item per line, '#' starts a comment:
//   USE0000_  price=8 power=30                     change an item
//   new MOD_HERB_ copy=USE0001_ price=150 power=75  add an item, starting from a copy
//   SWORD0000_ @0x40=60                            raw u32 at a record offset (@0x7c:1 = byte, :2 = u16)
//   SWORD0000_ sprite=pl0_cw20.osb colour=2         weapon look: sprite file, palette number
// A new item also needs a name: add its label to sys_item.tb2 (tools/tb2.py).
#include "content.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint32_t G_HEADER = 0x140A234, G_TREE = 0x140A238, G_RECORDS = 0x140A23C, G_STRINGS = 0x140A240;
static const uint32_t ITEM_DB = 0x124AE10;  // the built-in table (what G_HEADER points to at startup)
static const uint32_t ITEM_COUNT = 1835, ITEM_RECSIZE = 0x88;

struct Field { const char* name; uint32_t ofs; int size; };
static const Field FIELDS[] = {
    {"icon", 0x08, 4},    // icon number on the item icon sheet (Herb 16, bottles 14, food 15)
    {"price", 0x0C, 4},   // shop buy price; shops charge 90%
    {"stars", 0x14, 1},   // rarity shown as stars, 0-10
    {"power", 0x28, 4},   // consumables: effect strength (Herb 20 = heal 20% HP)
    {"equip", 0x48, 2},   // who can equip: bit per class, 1 Empress 2 Warrior 4 Shinobi 8 Witch 16 Oracle 32 Bandit
    // Equipment stats, signed 16-bit; the game adds them up over the equipped items.
    {"atk", 0x4C, 2}, {"def", 0x4E, 2}, {"matk", 0x50, 2}, {"mdef", 0x52, 2},
    {"acc", 0x54, 2}, {"macc", 0x56, 2}, {"crt", 0x58, 2},
    {"str", 0x66, 2}, {"agi", 0x68, 2}, {"int", 0x6A, 2}, {"vit", 0x6C, 2},
    {"luc", 0x6E, 2}, {"pie", 0x70, 2},
    {"shop", 0x7C, 1},    // 0 = not sold; 0x1N tool shop, 0x2N weapon shop; low bits = shop tier
    {"unlock", 0x7E, 2},  // story progress needed before shops offer it (0 = from the start, -1 = never)
    {"order", 0x84, 4},   // sort position in lists
    {"colour", 0x20, 4},  // weapons: palette number in the sprite's palette file
};
// Text fields: string-pool offsets. The new text is kept in our memory for good.
static const Field TEXT_FIELDS[] = {
    {"sprite", 0x18, 4},          // weapons: sprite file, e.g. pl0_cw00.osb
    {"sprite_palette", 0x1C, 4},  // weapons: palette file, e.g. Addon:6:pl0_cw00_pal.osb
};

static uint8_t* Game(uint32_t va) { return (uint8_t*)GetModuleHandleA(nullptr) + va - 0x400000; }
template <class T> static T& Global(uint32_t va) { return *(T*)Game(va); }

static uint32_t IceHash(const char* s)
{
    uint32_t h = 0xCDE723A5;
    for (; *s; s++) h = (h + (uint32_t)(int)(signed char)*s) * 0x8D;
    return h;
}

static uint8_t* FindItem(const char* label)
{
    uint32_t* hdr = Global<uint32_t*>(G_HEADER);
    uint32_t* tree = Global<uint32_t*>(G_TREE);
    uint8_t* recs = Global<uint8_t*>(G_RECORDS);
    uint32_t h = IceHash(label);
    for (uint32_t i = 0; i + 1 < hdr[0]; i++)
        if (tree[i * 4] == h) return recs + tree[i * 4 + 3] * hdr[1];
    return nullptr;
}

static bool SetField(uint8_t* rec, uint32_t ofs, int size, long long v)  // low bits are stored
{
    if (ofs + size > ITEM_RECSIZE) return false;
    if (size == 1) rec[ofs] = (uint8_t)v;
    else if (size == 2) *(int16_t*)(rec + ofs) = (int16_t)v;
    else *(int32_t*)(rec + ofs) = (int32_t)v;
    return true;
}

struct Line { int no; std::vector<std::string> tok; };

static std::vector<Line> ReadLines(const char* path)
{
    std::vector<Line> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return out;
    char buf[512];
    int no = 0;
    while (fgets(buf, sizeof buf, f)) {
        no++;
        if (char* c = strchr(buf, '#')) *c = 0;
        Line l{no, {}};
        char* ctx = nullptr;
        for (char* t = strtok_s(buf, " \t\r\n", &ctx); t; t = strtok_s(nullptr, " \t\r\n", &ctx)) l.tok.push_back(t);
        if (!l.tok.empty()) out.push_back(l);
    }
    fclose(f);
    return out;
}

struct TreeNode { uint32_t hash, left, right, index; };

// Pre-order balanced BST over hash-sorted nodes; left child is the next entry.
static uint32_t BuildTree(const std::vector<TreeNode>& sorted, size_t lo, size_t hi, std::vector<TreeNode>& out)
{
    if (lo >= hi) return 0xFFFFFFFF;
    size_t mid = (lo + hi) / 2;
    uint32_t at = (uint32_t)out.size();
    out.push_back(sorted[mid]);
    uint32_t l = BuildTree(sorted, lo, mid, out);
    uint32_t r = BuildTree(sorted, mid + 1, hi, out);
    out[at].left = l;
    out[at].right = r;
    return at;
}

// Add new items: returns how many were added (0 leaves the game's table alone).
static int AddItems(const std::vector<Line>& lines)
{
    struct New { std::string label; uint8_t* src; };
    std::vector<New> add;
    for (const Line& l : lines) {
        if (_stricmp(l.tok[0].c_str(), "new") != 0) continue;
        if (l.tok.size() < 3 || _strnicmp(l.tok[2].c_str(), "copy=", 5) != 0) {
            Log("items.txt line %d: expected: new LABEL copy=EXISTING_LABEL ...", l.no);
            continue;
        }
        const std::string& label = l.tok[1];
        uint8_t* src = FindItem(l.tok[2].c_str() + 5);
        bool dup = FindItem(label.c_str()) != nullptr;
        for (const New& n : add) dup |= n.label == label || IceHash(n.label.c_str()) == IceHash(label.c_str());
        if (!src) Log("items.txt line %d: no item %s to copy", l.no, l.tok[2].c_str() + 5);
        else if (dup || label.size() > 60) Log("items.txt line %d: %s already exists (or is too long)", l.no, label.c_str());
        else add.push_back({label, src});
    }
    if (add.empty()) return 0;

    uint32_t* oldHdr = Global<uint32_t*>(G_HEADER);
    uint32_t* oldTree = Global<uint32_t*>(G_TREE);
    uint8_t* oldRecs = Global<uint8_t*>(G_RECORDS);
    uint8_t* pool = Global<uint8_t*>(G_STRINGS);
    uint32_t n = oldHdr[0], rs = oldHdr[1], k = (uint32_t)add.size();
    uint32_t nTree = n - 1 + k;
    size_t labelBytes = 0;
    for (const New& a : add) labelBytes += a.label.size() + 1;
    size_t treeOfs = 0x14, recOfs = treeOfs + 16 * (size_t)nTree;
    size_t strOfs = recOfs + (size_t)rs * (n + k);
    size_t total = strOfs + labelBytes;
    uint8_t* blob = (uint8_t*)VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!blob) {
        Log("items: out of memory for the bigger item table");
        return 0;
    }
    uint32_t* hdr = (uint32_t*)blob;
    uint8_t* recs = blob + recOfs;
    memcpy(recs, oldRecs, (size_t)rs * n);

    std::vector<TreeNode> nodes((TreeNode*)oldTree, (TreeNode*)oldTree + (n - 1));
    char* str = (char*)blob + strOfs;
    for (uint32_t i = 0; i < k; i++) {
        uint8_t* r = recs + (size_t)rs * (n + i);
        memcpy(r, add[i].src, rs);
        strcpy_s(str, add[i].label.size() + 1, add[i].label.c_str());
        *(uint32_t*)r = (uint32_t)((uintptr_t)str - (uintptr_t)pool);  // offset from the game's string pool
        str += add[i].label.size() + 1;
        nodes.push_back({IceHash(add[i].label.c_str()), 0, 0, n + i});
    }
    std::sort(nodes.begin(), nodes.end(), [](const TreeNode& a, const TreeNode& b) { return a.hash < b.hash; });
    std::vector<TreeNode> tree;
    tree.reserve(nodes.size());
    BuildTree(nodes, 0, nodes.size(), tree);
    memcpy(blob + treeOfs, tree.data(), 16 * tree.size());

    hdr[0] = n + k;
    hdr[1] = rs;
    hdr[2] = (uint32_t)treeOfs;
    hdr[3] = (uint32_t)recOfs;
    hdr[4] = (uint32_t)((uintptr_t)pool - (uintptr_t)blob);
    // Records before tree before header, so a reader never sees a count
    // larger than the tables it points to.
    Global<uint8_t*>(G_RECORDS) = recs;
    Global<uint8_t*>(G_TREE) = blob + treeOfs;
    Global<uint32_t*>(G_HEADER) = hdr;
    for (const New& a : add) Log("items: added %s", a.label.c_str());
    return (int)k;
}

void ItemsApply()
{
    uint32_t* hdr = Global<uint32_t*>(G_HEADER);
    if (hdr != (uint32_t*)Game(ITEM_DB) || hdr[0] != ITEM_COUNT || hdr[1] != ITEM_RECSIZE ||
        Global<uint8_t*>(G_RECORDS) != Game(ITEM_DB) + hdr[3]) {
        Log("items: item table not as expected (count %u, size %#x); edits skipped", hdr[0], hdr[1]);
        return;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) *s = 0;
    strcat_s(path, "\\DataMod\\items.txt");
    std::vector<Line> lines = ReadLines(path);
    if (lines.empty()) return;

    int added = AddItems(lines);
    int items = 0, values = 0;
    for (const Line& l : lines) {
        size_t first = 0;
        if (!_stricmp(l.tok[0].c_str(), "new")) first = 1;
        if (first >= l.tok.size()) continue;
        uint8_t* rec = FindItem(l.tok[first].c_str());
        if (!rec) {
            Log("items.txt line %d: no item %s", l.no, l.tok[first].c_str());
            continue;
        }
        items++;
        for (size_t t = first + 1; t < l.tok.size(); t++) {
            std::string tok = l.tok[t];
            size_t eq = tok.find('=');
            if (eq == std::string::npos) { Log("items.txt line %d: expected name=value, got %s", l.no, tok.c_str()); continue; }
            std::string name = tok.substr(0, eq);
            if (first && !_stricmp(name.c_str(), "copy")) continue;
            long long v = _strtoi64(tok.c_str() + eq + 1, nullptr, 0);  // 64-bit: hashes like 0x822bc24b fit
            bool ok = false;
            const Field* text = nullptr;
            for (const Field& fd : TEXT_FIELDS)
                if (!_stricmp(fd.name, name.c_str())) text = &fd;
            if (text) {
                std::string s = tok.substr(eq + 1);
                size_t n = s.size() + 1;
                char* keep = (char*)HeapAlloc(GetProcessHeap(), 0, n);  // never freed: the game keeps using it
                if (keep && n > 1) {
                    memcpy(keep, s.c_str(), n);
                    uint8_t* pool = Global<uint8_t*>(G_STRINGS);
                    ok = SetField(rec, text->ofs, 4, (long long)(uint32_t)((uintptr_t)keep - (uintptr_t)pool));
                }
            } else if (name[0] == '@') {  // @ofs or @ofs:size
                size_t colon = name.find(':');
                int size = colon != std::string::npos ? atoi(name.c_str() + colon + 1) : 4;
                if (size == 1 || size == 2 || size == 4)
                    ok = SetField(rec, (uint32_t)strtoul(name.c_str() + 1, nullptr, 0), size, v);
            } else {
                for (const Field& fd : FIELDS)
                    if (!_stricmp(fd.name, name.c_str())) ok = SetField(rec, fd.ofs, fd.size, v);
            }
            if (ok) values++;
            else Log("items.txt line %d: unknown field %s", l.no, name.c_str());
        }
    }
    Log("items: %d new item(s); %d value(s) changed on %d item(s)", added, values, items);
}
