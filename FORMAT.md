# Dragon Marked for Death: data file format

Status as of 2026-11-23. Everything here comes from the unpacked exe (the game's `game.exe` unpacked from its Steam wrapper, preferred base 0x400000) plus tests against the real `DataHash` folder. Nothing in the game folder was modified.

**How sure each finding is.** This was worked out by reading the game's program and files, with an AI assistant (Claude) doing most of the reading and a human testing in the game. Findings say how they were checked:
- **Confirmed in game**: the change was made with the kit's tools and seen working in the game.
- **Checked offline**: the tools rebuild the game's own files byte for byte, or the change renders correctly, but nobody looked at it in the game.
- **Read from the program only**: from the disassembly; plausible, not tested. Treat it as a lead.

Where a line says nothing, it is a structure the tools rely on every day (the tools would fail visibly if it were wrong).

**Bottom line:** content modding is practical. Text tables, items, chests, icons, palettes and every sprite (bodies, weapons, portraits, menus) can be edited and extended, all confirmed in game. Character bodies can get whole new frames, new timing and new hit boxes per frame (section 7, "Animation editing"). Moves themselves are code in the exe, not data: see `docs/moves.md`.

## 1. File names

`DataHash/<md5(asset path)>`: the lowercase hex MD5 of the asset path string as the game spells it. No salt and no container: one asset per file.

| Asset kind | Hashed string | Example |
|---|---|---|
| text tables, stages, fonts, layouts | bare name | `md5("sys_item.tb2")` = `0239b2f0…` |
| sprites / objects (load type 2) | `"Win/" + name` | `md5("Win/pl00dc.osb")` |

- The path builder is `0xe81a30` (IceSStorageEng::IceReader open). Type 2 prepends the global at `[0x15271f8]`, which `0xe89410` sets to `"Win"`, plus `/`.
- MD5 plus hex conversion is `0xe82120`. It reuses the MD5 code at `0xec0fe0`/`0xec1850`.
- The file is opened with CRT `_wfopen` (`0xf61a8d`, mode `L"rb"`) at `0xe8206a`.
- The same builder also handles `"Patch:"` names (optional `Patch/` dir, flag `[0x15271fd]`) and `"Addon:"` names (`/../DataAddonHash/`, the DLC mechanism). Both are unused leads for adding new files.

The exe's string table names **647 of the 1,046 files** (`tools/names.txt`). The rest are 328 sound banks plus 71 sprite/scroll files whose names are built at runtime.

## 2. Layers: encryption and compression

`IceReader::readMain` runs ReadTask → DecryptTask → InflateTask (RTTI `IceDecryptTask@…readMain`, `IceInflateTask@…`), driven by a flags argument. Bit 0 means encrypted, bit 1 means deflated. Request builder: `0xe854b0(out, type, name, flags, key)`.

**Cipher** (`0xe816d0` init, `0xe81740` decrypt). This is a home-made stream cipher:

```
k = 0xA1B34F58CAD705B2                       # 64-bit
for c in key_string: k = (k + signed(c)) * 0x8D
for i, b in enumerate(data):
    out[i] = b ^ ((k >> (i & 31)) & 0xFF)
    k = (k + b) * 0x8D                       # b = ciphertext byte
```

**Compression:** a u32 LE uncompressed size, then a standard zlib stream (`0xec1b80` is zlib inflate, 0x60000-byte chunks).

**Keys** are literal strings in `.rdata` (0x111c940…, 0x1036e64):

| Extension | Count | Recipe | Key |
|---|---|---|---|
| `.osb` sprite / object bank | 558 named + 48 | decrypt → inflate | `obj90210` |
| `.tb2` text table | 58 named + 1 | decrypt → inflate | `x4NKvf3U` |
| (scroll / bg, unnamed) | 17 | decrypt → inflate | `scroll90210` |
| `.stb` stage layout | 20 | decrypt only | `set90210` |
| `.json` UI layout | 5 | **inflate → decrypt** | `xN5sUeRo` |
| `.bin`/`.ttf` fonts | 6 | plain OTF/TTF | – |
| `.ivc` movie | 2 (~190 MB) | plain; own container, 1280×720 @30 fps, 48 kHz audio | – |
| sound bank | 328 | plain; header `10 00 00 00 40 00 00 00 …`, 48 kHz, codec not identified | – |
| unknown | 3 | `56c1c804…`, `bb65b80b…`, `5e4d6424…` | – |

Other key strings present but not matched to a file: `snd90210`, `ssbpi90210`, `json180601`.

## 3. Text tables (`.tb2`): fully decoded, rebuilds byte-identical

Decoded layout (all u32 LE):

```
0x00 0xFFFFFFFE   0x04 version (8)
0x08 nLang        0x0C -> nLang string offsets ("zh-cn","zh-tw","de","en","es","fr","it","ja","ko"[,"nl","pt","ru"])
0x10 nLabel       0x14 -> label tree   (24 B: hash, left, right, nVariant, firstEntry, labelOfs)
0x18 nEntry       0x1C -> entry aux    (u32 ofs per (label,variant): speaker/portrait tag
                                         e.g. "<tname/Asio><tface/fa032_p00>", else "")
0x20 nString      0x24 -> string tree  (16 B: hash, left, right, textOfs)
then a UTF-8 NUL-terminated string pool
```

- Both trees are pre-order binary search trees on the hash, with the left child at i+1 and -1 meaning none.
- Hash: start at `h = 0xCDE723A5` (the seed the engine's string objects carry), then for each signed byte `h = (h + c) * 0x8D mod 2^32`.
- Label key = `H(label)`. Text key = `H(label + ":" + variant + ":" + lang)`, e.g. `"Oldem:0:en"`.
- Multi-line dialogue uses variants: `q_0c00.tb2` has up to 15 lines per label.

`tools/tb2.py` parses all 58 known `.tb2` files and **rebuilds every one byte-for-byte**. It also rebuilds after edits and added labels. Notable tables:

- `sys_item.tb2`: 2,010 item names and descriptions.
- `sys_quest.tb2`, `q_XXXX.tb2`: quests and quest dialogue.
- `sys_enemy.tb2`, `TalkerName.tb2`, `sys_shop.tb2`.

## 4. Sprites (`.osb`): textures and animations decoded

After decoding, a 0x6C/0x38/0x60-byte header leads into part/cell/animation tables, then textures. Each texture is preceded by a 24-byte header:

```
u32 dataOfs (= header + 24), u32 size (= w*h*4), u32 hash, u32 w, u32 h, u32 0
```

- **UI / item / effect sheets** are plain 8-bit RGBA, rows top-down. Example: `Win/com_itic.osb` holds the 128×128 item-icon atlas. The exact R/B order still needs one in-game colour check.
- **Character sheets are paletted.** Each pixel is `(0, 0, index, alpha)`. Example: `Win/pl00dc.osb` is the Empress's dragon form, 2048×1024.
- Palettes are in separate files: `Win/pl0_pal.osb` (48 palettes × 112 colours) and `Win/plN_cwXX_pal.osb` for weapons (8 × 13). Palette block: `hash, -1, -1, 0x34, 0x14, 4, 0, nPal, 0, nCol, 0x34, nPal, size`, then RGBA colours. Index 0 is transparent.
- Sheets are cut into parts: limbs and pieces, not whole frames. A new move or class therefore needs the cell, part and animation tables. Those are the next RE target, at the start of each `.osb`.

Sprite families by name: `plN_*` players (N = class 0–5), `bsNN_*` bosses, `zkNN_*`/`cbNN_*` enemies, `npNNN_*` NPCs, `stXXX_o00` stage objects, `com_*`/`title_*` UI, `efNN_*` effects.

## 5. Serving modified files (no edits to the originals)

All three layers are invertible. `dmfd_data.encode()` produces files the decoder accepts. 645/645 named assets round-trip, and the cipher re-encrypts byte-identically.

**Implemented and confirmed in-game** in `src/datamod.cpp`, part of the loader `winmm.dll`: a re-encoded `sys_item.tb2` with "MOD Herb" etc. showed in the shop. How it works:

- It hooks CRT `_wfopen_s` (`0xf61a8d`) and `_wstat` (`0xf63786`). Each has a single caller: the IceReader open (`0xe8206a`) and the size check in `0xe82350` (`0xe82724`).
- If `<game>\DataMod\<same md5 name>` exists, both calls use that path instead. The log shows `DataMod: serving <hash>`.
- The overlay files are made with `dmfd_data.py pack`, so the game still runs its normal decrypt and inflate. No other code changes are needed for text or texture swaps.
- New assets (new names) work the same way: the hash is only the filename.

Alternative to investigate: the built-in `Patch:` / `DataAddonHash` paths.

## 6. Tools (`tools/`)

All tools are pure Python and need no extra packages. Run them with `tools\py.bat <tool.py> ...` (the kit's own Python, or an installed one). Build mod (`build_mod.py`) drives the others; `sprites.py`, `palettes.py`, `layout.py`, `icons.py`, `items.py`, `drops.py`, `selectart.py` and `save.py` each describe their format in their opening comment.

| File | Purpose |
|---|---|
| `dmfd_data.py` | `scan` (classify all files to `scan.json`), `dump <name\|hash> [out]`, `all <outdir>` (decode everything, using `names.txt`), `pack <name> <decoded> <outdir>` (refuses to write into DataHash). Library: `file_name`, `crypt`, `encrypt`, `decode`, `encode`, `encode_asset`. |
| `tb2.py` | `dump <asset or file> out.json`, `build in.json out.dec`; library `parse`/`build`/`ice_hash`. |
| `osb.py` | `--asset Win/x.osb outdir [palette.osb [index]]` writes each texture as PNG; library `textures`, `palettes`, `write_png`, `read_png_rgba` (for re-import). |
| `names.txt` | 647 known asset names. |

Example: rename an item.

```
python -I tools/tb2.py dump sys_item.tb2 sys_item.json
# edit sys_item.json
python -I tools/tb2.py build sys_item.json sys_item.dec
python -I tools/dmfd_data.py pack sys_item.tb2 sys_item.dec overlay
```

## 7. Built-in tables (items, drops, quests…)

Game data tables are compiled into the exe's `.data` section, not shipped as files. Each table is one blob:

```
header: u32 count, recSize, treeOfs, recOfs, strOfs   (offsets from the header)
tree:   count x {hash, left, right, recordIndex}      (same hash as .tb2; key = label)
records: count x recSize
strings: NUL-terminated ASCII; record fields hold offsets into it
```

A startup function per table (`0x401000`, `0x401060`, …) stores pointers to the tree, records and strings in globals. The game only reaches the table through those globals. Editing records in place works, and a rebuilt (bigger) table can be swapped in by repointing the globals. 22 tables were found (`a1 ofs; 8d 80 base; a3 global` pattern):

| Header | Count | Rec size | What |
|---|---|---|---|
| 0x124ae10 | 1835 | 0x88 | **items** (globals 0x140a234 header, 238 tree, 23c records, 240 strings) |
| 0x1358318 | 4973 | 0x68 | drops (`DROP_…`) |
| 0x12d97e0 | 10764 | 0x10 | quest reward/encounter item lists (`ID<quest>_<a>_<b>` → item) |
| 0x1355ce8 | 59 | 0x5c | quests (`q0000_CAPTION`, stage file) |
| 0x1400a50 | 555 | 0x14 | treasure boxes |
| 0x12badb8 | 1115 | 0x30 | grade-ups (weapon upgrade paths) |
| 0x129a5f0 | 465 | 0x4c | exchanges |
| 0x12d3910 | 236 | 0x40 | enemy material drops |
| others | | | packs, bundles, grade-up rates/materials, EX dungeon, NM, food |

**Item record** (0x88 bytes; decoded so far):

| Ofs | Size | Field |
|---|---|---|
| 0x00 | u32 | label (string offset), e.g. `USE0000_`, also the `sys_item.tb2` label |
| 0x04 | u8 | category: 0 dagger, 1 sword, 2 axe, 3 greatsword, 4 kunai, 5 shuriken, 6 book, 7 crystal, 8 accessory, 9 consumable/material, 10 other |
| 0x0C | u32 | price. Buying in a shop costs 90% (`0x1120f08` = 0.9), truncated, min 1 |
| 0x14 | u8 | stars (rarity, 0–10) |
| 0x18 | u32 | weapons: sprite file (string offset), e.g. `pl0_cw00.osb` (the game adds `Win/`). Non-weapons hold a filler `pl3_cw03.osb` |
| 0x1C | u32 | weapons: palette file (string offset), e.g. `Addon:6:pl0_cw00_pal.osb` or `pl0_cw01_ex_pal.osb`. The plain name exists in DataHash; the `Addon:N:` prefix is not understood yet |
| 0x20 | u32 | weapons: colour = palette number in that palette file (e.g. starting-sword palette 4 has a red blade) |
| 0x24 | u32 | effect type hash (consumables: the use effect; weapons: a special effect) |
| 0x28 | u32 | effect parameters. Consumables: strength (Herb 20 = heal 20%). Weapons: packed, e.g. Shortsword +3 `0x000A0032` = "+50% P.dmg. to Lv.10+ enemies" (low 16 bits 50, high 16 bits 10) |
| 0x7C | u8 | shop flags: 0 = never sold; high nibble 1 = tool shop, 2 = weapon shop; low bits = shop tier |
| 0x7E | i16 | story progress needed before shops offer it (0 = from the start, negative = never; checked in `0xbcb9b0`) |
| 0x80 | u16 | item number |

Weapon stats are mapped (see the stat table above the shop note).

**Item effects:** three slots at `+0x24`, `+0x30`, `+0x3C`, each `{u32 kind, u32 params, u32 extra}`. `kind` is the hash of an `EFF_E_…` (while equipped) or `EFF_U_…` (on use) name. 90 names are in the exe; `EFF_NONE` = `0x51889489`, and `0x822bc24b` (Chief's Meat, Ashes of Life: revive) has no string. `params` = value (low 16 bits) | value2 (high 16 bits); `extra` high 16 bits = duration in seconds. Descriptions come from `sys_item.tb2` `ITM_SPEFxx_` templates (`<var/val0>`, `<var/val1>`, `<var/time>`).

The exe keeps each kind's hash in globals, set up by small init functions (`0x6935xx`–`0x6938xx` register them). The equip-effect code (`0xcb8000`–`0xcbc000`) and the description lookup (`0xccb000`–`0xccc000`) compare against those globals. 65 kinds are used by items. Of the 25 unused:
- 13 have both behaviour and description code, so they should work (`DAMAGE_UP`, `DRAGON_RESIST`, `MAGIC_RESIST`, `EFFECT_EXTENSION`, `TIME_UP_CANDY_ITEM`, …). Tested: `DAMAGE_UP` 100 on the Dragon Blade shows "Physical attack power +100%" and works.
- 8 have none. `NOT_CONSUMED_ITEM` is only registered: tested, it shows a blank description line and does nothing.
- 4 (`GET_MONEY` …) are map-pickup effects.

New behaviours would need new code in the mod (hooks), not data.

**Shops have no stock list.** `cShopItemListTableBuyTools` (vtable 0x10e7520, build `0xbcc910` → `0xbcb9b0`) walks every item. It shows those whose category fits the tab, whose shop flags match the shop, and whose progress requirement is met.

The content mod applies `DataMod\items.txt` (`LABEL field=value …`, see `samples/items.txt`) to the records at the game's first file access. `tools/items.py` lists all items as a spreadsheet.

**New items** (`new LABEL copy=EXISTING …`): the mod builds a bigger header + tree + records in its own memory and repoints `0x140a234/238/23c`. The string pool stays in place; new labels get 32-bit offsets that wrap around to the mod's memory. The name comes from adding the label to `sys_item.tb2`, and the description is generated from the record (it showed "Heal +75% HP"). Confirmed in-game: the Dragon Herb (copy of Quality Herb, price 150, power 75, 5 stars) was sold in the tool shop, could be equipped and used, and healed as set.

**Saves and removed items:** the save holds the item by its label hash. Loading a save that contains a modded item *without* the mod does not crash. The item is blank in the quick-use list, shows as `ERROR#00` in the equip slot, and disappears from the menus when unequipped. It stays in the save, though: reinstalling the mod brings it back (seen with the Dragon Blade). An equipped modded weapon that is no longer defined shows as a blank weapon slot. The character is drawn without a weapon and can still attack. So removing the mod hides those items but is otherwise safe.

### Item icons

The item's icon number (`+0x08`, 0–23) goes through `0xcccb20`, a lazily built table of 24 hashes of Japanese names (UTF-8 in the exe, e.g. 植物の実 = 16 herb, 薬 = 14 medicine). Larger numbers return 0, so there's no icon.

Each hash names an entry in `Win/com_itic.osb`:
- entry tree at `0x6c`, 0x1c bytes each: {size, hash, scale, left, right, count, block};
- block `+0x10` → key data; key `+0x1c` → mesh index;
- mesh table at header `+0x1c` (`0x11c70`): `n`, then records of 0x18 bytes {0x18, 0, vtxOfs, idxOfs, 4, 6}, offsets relative to the record;
- each vertex is `x, y, z, u, v` (20 bytes).

The texture is 128×128 **BGRA**, a 7×7 grid with an 18 px pitch (16 px icons at 1 + 18k). 33 cells are used (24 item icons plus 9 UI marks such as the "E" equipped mark), and 16 are free.

`tools/icons.py` exports and imports the 24 item icons. Confirmed in-game: a recoloured `16_herb.png` changed Herb, Quality Herb, Premium Herb and the Dragon Herbs.

**New icon numbers (24–39), working.**

`tools/icons.py rebuild()` re-serializes `com_itic.osb` section by section and adds an entry, block, key chunk, two part-table slots and a quad mesh for each new icon. Each new icon is named `MOD_ICON_nn`, with UVs on a free cell (cells 32–34, 36–48, in grid order).

The osb layout this relies on:

```
header 0x6c | tree n*0x1c | blocks n*0x48 (file order kept) | key chunks + pad 8 | part table | texture | meshes
```

Header fields: `[6]` mesh size, `[7]` mesh offset, `[8]` entries, `[9]` parts, `[10]` part table, `[19]` texture, `[20..23]` file size, `[25]`/`[26]` texture. The texture header's data offset is absolute.

Rebuilding with no additions is byte-identical to the original.

The content mod hooks `0xcccb20` (`src/icons.cpp`) and returns the `MOD_ICON_nn` hash for the numbers listed in `DataMod\icons.txt`. Confirmed in-game: the Dragon Blade with `icon=24` shows a gold sword icon while other swords stay red.

### Chests

**Chests** are `cTreasureBoxDB` at `0x1400a50` (555 records, 0x14 bytes each):

| Offset | Field |
|---|---|
| `+0` | label `TREASUREBOX_QUEST_q_s_c` |
| `+4` | quest key |
| `+8` | chest index |
| `+9` | ? |
| `+0xb` | count |
| `+0xc` | ? (100 / 500 / 2000) |
| `+0x10` | first drop table |

**Drop tables** are at `0x1358318` (4,973 records, 0x68 bytes each; records at `0x136b9fc`, strings at `0x13e9e44`):

| Offset | Field |
|---|---|
| `+0` | label `DROP_q_s_c_N<level>` / `_R<level>` |
| `+4` | rare flag |
| `+5` | rare chance % (the rebalance mod scales this) |
| `+6` | level |
| `+8` | 16 × u16 weight |
| `+0x28` | 16 × u32 item label (string offset) |

**How a chest opens:**
- `0xbd6150` finds the chest by quest key and chest index.
- `0xbd61b0` takes the first normal table and the first rare table in its range whose level is ≥ the given level.
- `0xbd62b0` rolls the rare chance; luck adds up to +10.
- `0xbd6280` rolls a quality grade from a 6-step table at `0x1031ed8`.
- `0xbd6320` builds the candidates: slots with weight > 0 whose item is `_GOLD`, equippable by the class (item `+0x48`), or not excluded. It then picks one with probability weight / total.
- Randomness is xorshift seeded per chest.

**Editing:** `src/drops.cpp` applies `DataMod\drops.txt` in place (fill a weight-0 slot, or change a weight). It never moves the table or touches `+4`/`+5`, so the rebalance mod's fixed-address rare scaling keeps working.

Confirmed in-game: the Dragon Blade at weight 2000 in every non-full table dropped from chests for the Empress, with both mods loaded.

**Enemy material drops** are a separate table, `cEmMaterialData` at `0x1296c68` (70 × 0x88, `EMMAT_EM_xx_yy` → `EMMAT` items). It uses a different weight layout and isn't decoded yet. `0x12d3910` is enemy stats (HP and so on), not drops.

### Sprite animations (character bodies)

Player bodies are `Win/plN_cb00.osb` (N = class 0–5). The Empress has 130 animations, 407 frame meshes and a 2048×1024 paletted atlas. Her palettes are in `Win/pl0_pal.osb`, stored **BGRA**: 48 = 6 contract elements (they colour the dragon arm and change through the game) × 8 highlight colours (picked at character creation). The Oracle's file has 49. Related files:
- `plN_cwXX`: weapon sprites (items point at them; e.g. the `DAGGER0000_` record's strings name `pl0_cw02.osb` and its `_ex_pal`).
- `plN_efXX`: effects.
- `plN0dc`: the dragon form.

**Structure:**

| Level | What it is |
|---|---|
| Entry (tree at `0x6c`, 0x1c bytes) | One animation: `{0x1c, name hash, f32 length in frames, left, right, node count, first node}`. The names aren't strings in the exe. |
| Node (0x48-byte block) | `+4` name hash, `+8` parent hash (0 = root; the root is always `0x8d154d2d`), `+0xc` track count, `+0x10` first track. |
| Track | Header `{0x14, key count, channel, 0, 1}`, then keys of 0x14 bytes `{f32 time, ?, value, ?, ?}`, bracketed by −∞/+∞ keys. |
| Frame mesh | A list of quads, each 4 vertices `(x, y, z, u, v)` mapping an atlas tile to a frame rectangle (pixels, y up). Tiles may be flipped or rotated. Frames are tiled and de-duplicated: one frame is about 4–20 quads. |

**Track channels:**
- `0`: frame mesh index. This is the flipbook, and it's the root's only track in 121 of 128 Empress animations.
- `7`/`8`: f32 x/y of a child node; attachment points.
- `6` and `14`: also seen; meaning unknown.

`tools/sprites.py` parses this and renders every animation to PNG strips with the right palette.

**Editing (working):** `export_frames` writes each frame on its animation's shared canvas, with the frame mesh and canvas origin in the file name. `import_frames` reverses the render with the same pixel mapping (`quad_pixels`):
- An edited pixel goes to the topmost opaque tile at that point, since tiles overlap.
- Erasing clears every visible tile under it.
- Colours map to the nearest palette index, keeping the original index when unchanged.

- Painted pixels outside every existing tile (export adds a `MARGIN` of 24 px to the canvas) are grouped into 16×16 canvas-aligned blocks. `add_tiles` de-duplicates them, places them on free 18 px grid cells of the atlas (1 px padding; `_occupied`/`_free_slots`), and appends one quad per block to that frame's mesh: vertices `(ax,ay)→(tx,ty+16)`, `(ax,by)→(tx,ty)`, `(bx,by)→(tx+16,ty)`, `(bx,ay)→(tx+16,ty+16)`, indices `0,1,2,0,2,3`.
- `rebuild_meshes` rewrites the mesh section (header `[6]` size and `[20..23]` file size updated). Rebuilding unchanged meshes is byte-identical.
- When no cells are free, `grow_texture` appends rows to the atlas (texture header size/height and header `[7]` mesh offset updated) and rescales every `v` by `oldH/newH`. Checked offline only: renders are unchanged.

**Full-colour sheets** store BGRA directly (bytes 0–1 nonzero where `is_paletted` checks). `import_frames(pal=None)` writes pixels as-is, keeps partial alpha, and uses alpha > 0 as "painted". Confirmed in-game: Empress portrait in `com_pl_icon` with a magenta background (equip menu, load screen).

Art files found: `com_pl_icon` (class portraits, 256x128), `cp_o0000` (HUD incl. small class portraits), `cutin00` (dragon orb cut-in illustrations, 1024x2048; its animations show pictures through child nodes, so `export_frames` also writes uncovered meshes as `mesh_m<N>_o<x>_<y>.png`), `face`/`EmFace` (NPC/enemy faces), `com_pl_change` (character-change frames and text), `title_common`.

**Character select screen = `s_o0a00.osb`** (2048x2048 full colour, 169 animations, 219 meshes). Found by following code, not by scanning: RTTI `.?AVcHudWorkCreateCharaSelect@@` -> complete object locator -> vtables (0x10c43a0 main, 27 functions); its functions push `s_o0a00.osb`, `com_butt.osb`, `PadInfo.tb2`, `sys_plselect.tb2` and part names `PL1`..`PL6`, `PL1_Lv`..`PL6_Lv`, `NEW`, `Lv2` (0xabcc3d..0xabda0a). `PL1`.. aren't animation hashes, so they're probably node/part names. Illustrations: colour narrow a004/a018/a020/a043(Empress)/a131/a144, colour wide a027/a073/a109/a119(Empress), sepia meshes 131-140. Green Empress test confirmed in-game. Adding a 7th panel needs the part layout (undecoded node/track data in this file) and the menu code. Other scenes: `cSceneCreateChara` (also uses `sub0200.osb`, `com_curs.osb` via cHudWorkCreateCharaFrame), `cHudWorkCreateCharaColor/Voice/Name/Confirmation`. The 0x38-header files are title/ending/story screens; scroll-key files are stage tile maps.

**Select screen layout (decoded).** Layouts are animations whose child nodes are named parts; per node, track 0 = picture (mesh), 1/2 = scale x/y, 7/8 = position x/y (screen ~640x360, origin centre, y up); keys are constant (3 keys incl. the -FLT_MAX sentinel). `tools/layout.py` lists and edits them in place. In `s_o0a00`: anims 130/136/137/138/159 (probably one per language) = 6-panel layout: `bg` (mesh 143), `PL1`..`PL6` (sepia meshes 135-140, scale 0.5, x -270..30 step 60, y 110), `PL1_Lv`..`PL6_Lv` (y -84), `name_text`, `color`, `voice_text`, `character`, `cv`. Anim 74 = the original 4-panel layout (meshes 131-134, step 84). Anims 68/70 hold `color`, `cursor`, buttons. Panel order = class order. Tested in-game: moving PL1 down 40 moves the sepia panel, the colour highlight and the level; the selector frame slides toward it but keeps its own height.

**Select screen code** (`cHudWorkCreateCharaSelect`, setup ~0xabcc30): greys locked panels by finding the part (0x7a3220 = name hash, 0xe4e8d0 = find node) and writing a colour to node+0xb4. PL1/PL2 locked unless DLC pack 6 (0xabc8f0(6) or flag [0x152636e]), PL3/PL4 pack 7 ([0x152636f]), PL5 save bit 1 + pack 6 or 7, PL6 save bit 3 + pack 2 ([0x152636a]). The `Addon:6:`/`Addon:7:` item palette prefixes are these packs (Basic Set: Empress, Warrior; Advanced: Shinobi, Witch). Level display (~0xabd9a0) caches six `PL1_Lv`..`PL6_Lv` hashes in a static table (0x28eb3dc, 6 entries) and reads per-class save records with `cmp esi,6` bounds: **the save holds six class slots**. A 7th class needs: a `PL7`/`PL7_Lv` node (structure edit of the node blocks), code hooks for the six-entry tables and loops, and room in the save. `dumpbin /disasm /range:` from the VS 2019 build tools works as a disassembler.

**Save file** (`SystemData.bin`, see `tools/save.py` for the full layout): header {0xae9f2304, 0x68, checksum(plain), checksum(encrypted)}; checksum = 16-bit folded byte sum (0xBD3B80). Data from 0x10 encrypted twice with the data-file cipher (0xE81590 encrypt / 0xE81630 decrypt): key `zZ2c9VTK`+account, then `gYjkJoTX`+account (Steam account ID, `%x`, built at 0xBD3970). **In memory the game keeps the file's exact layout** (loader 0x8EB310, defaults 0x8EB8A0), offsets count from the file start. Header: [0x1C] class record 0x5C, [0x34] slot size 0xC, [0x38] slots at 0x68, [0x3C] 3 contracts, [0x40] current contract (memory). Slot {A, B, C}: A+2 created flags, A+3 active class; B = six 0x5C records (name, then ten record-relative offsets: setup block and eight more blocks; field 2 is a number) followed by their blocks, 0x1140 bytes in all; C = inventory. Setup block: +0 class, +1 colour, +2 voice, +4 level, +0x14 four {item hash, equipped flag}; validated by 0xC928A0 (class <= 5, colour/voice <= 7, level 1..100). Current record: 0xC8F4B0 (thiscall(game, class), `class < 6 ? class : 0`). The save is written from a fresh copy (0xBD51E0) that is checksummed (0xBD3F4C) and encrypted (0xBD4055/0xBD40C1).

**Extra-class save data (loader):** an extra class rides on its base class's slot k (k = the class whose moves it borrows). While it is on screen, slot k's byte ranges (name area 0x34, field 2, then the nine blocks; e.g. 0x288 bytes for the Empress) hold the extra class's data and the base character waits in memory; the outgoing copy of the save gets the base class's bytes back before encryption, and the extra classes' bytes go to `SystemData.bin.classes` (version 5, keyed by class `id`). Swapping is done when the game first reads a class record (hook on 0xC8F4B0) and on page turns, with the setup validator's class limit raised to 127. A new extra class starts from a fresh-character template (`src/fresh_classes.inc`). Confirmed in game: the written save stays byte-identical to the real one; extra characters' levels and equipment persist across restarts. If the base character was never put aside (the save on disk already held an extra class in that slot), the loader starts the base class as a new character rather than writing an empty one (fixed 2026-11-23 after a character came back nameless).

**Palette files** (`plN_pal`, `plN_cwXX_pal`, `plN_cwXX_ex_pal`): osb header (`[7]` = 0x318-style offset of a small trailing section, `[19]`, `[20..23]` file size), then at 0x6c one block `{u32 ID, -1, -1, 0x34, 0x14, 4, 0, nPal, 0, nCol, 0x34, nPal', 0x34+nPal*nCol*4}`, nPal×nCol BGRA colours, then a tree of 20-byte entries `{hash, left, right, u16 1, u16 -1, offset}` naming each palette, a u16 list, and the trailing section. Not decoded enough to add palettes.
- Body palettes: 48 = 8 highlight × 6 element, row = highlight*6 + element. For the Empress only 9 of 112 colours vary: 33–36 element (dragon arm), 16 and 49–52 highlight. 17–24 are armour/clothes.
- **The ID matters.** It's the Ice hash of a source name continued from a fixed state: `0x09D154E5` + `pl0_cw00_pal` = `0xa508675e` (checked against the files; the `_ex_pal` files use another prefix). A copy under a new name with the old ID was loaded but drew the weapon invisible; giving it `ID = hash(0x09D154E5, new name)` fixed it (in-game, Dragon Blade with `mod_cw00_pal`). `tools/palettes.py` exports/imports palettes as swatch-grid PNGs (12 px cells) and sets new IDs.

**Weapon sprites** (`Win/plN_cwXX.osb`, Shinobi throwing weapons `pl2_twXX`) use the same format: 1024×1024 paletted sheet, about 250–460 frames. Each weapon item names its sprite file, palette file and palette number (`+0x18`/`+0x1C`/`+0x20` above), so 43 sprite files serve all weapons. Bandit has none of her own: her daggers use the Empress's `pl0_cw01/02/03/07/08`, her shuriken the Shinobi's `pl2_tw00–11` (one item names a missing `pl5_cw11.osb`). Weapon palettes have 8–16 colours; a few sheets have stray pixels with indices past the palette, which export and import both skip. `pl2_cw01`–`04` are full-colour BGRA, not paletted; they import as full-colour sheets (exact colours, no palette rows). A sprite file under a new name (new md5 file name, served by the overlay) works: confirmed in-game with `mod_cw00.osb`, a gold-bladed copy of `pl0_cw00`, on the Dragon Blade, while other swords stayed normal. The texture header hash is the same constant in every file, so copies don't clash.

Exporting and re-importing all 581 Empress frames unchanged is byte-identical (checked offline), and so do five weapon files tested (`pl0_cw00`, `pl1_cw00`, `pl1_cw10`, `pl3_cw03`, `pl2_tw11`). Confirmed in-game: the Empress's horn reds recoloured in all frames showed on her model, and a halo painted above her head on all 407 frames (1,815 new tiles, fitting in the free atlas space) showed correctly while standing, running and jumping. `tools/anim_viewer.py` makes a page that plays every animation of an exported body, numbered.

### Animation editing (new frames, timing, tracks, hit boxes)

All of these were worked out in November 2026 while building a test class with its own art on the Empress's moves, and **confirmed in game** unless marked otherwise. Build mod applies them from small text files in a body's sprite folder (`docs/modding.md`, "Animations"); the functions are in `tools/sprites.py`.

- **Whole new frames** (`import_frames(..., replace=True)`, a sprite folder with `_replace.txt`): each PNG becomes a complete new picture for its frame. The frame's old quads are dropped and its pixels are cut into new 16×16 tiles (de-duplicated, placed in free atlas cells, the atlas grown when full). A class can look entirely different while keeping the base class's animations.
- **Frame records** are `{0x18, 0, vertex offset, index offset, vertex count, index count}`; nothing else in the file ties a picture to a move (*read from the file and the program*).
- **Retiming** (`set_timing`): an animation entry's `f32 length` and the start times of its root node's channel-0 keys (the −∞/+∞ sentinels stay; a key just before +∞ that only marks the end moves to the new length). Times are game frames.
- **New picture tracks** (`set_keys`): a fresh channel-0 track showing the animation's own pictures in any order and any number of times. The game ignores data appended after the mesh section, so new tracks go at the **end of the key-data section** (`insert_tracks`): everything after it moves down, padded to 16 bytes to keep the texture aligned, and the moved pointers are fixed: header `[7]` mesh, `[10]` part table, `[19]`/`[25]`/`[26]` texture area, `[20..23]` file size, the part table's pair pointers and the texture header.
- **Fewer nodes** (`set_node_count`): lowering an entry's node count drops trailing nodes (the dropped blocks stay unused in the file). The Empress's wire animations carry an attach-point node that makes the game draw the tether; dropping it removes the tether.
- **Track channels** now known: 0 picture (mesh index), 1/2 scale x/y, 7/8 position x/y (attachment points), 10/11/12 a light's red/green/blue (0..1, e.g. the fireball's glow node). `set_track` sets every key of one track. A node can also show **another animation of the same file** (the fireball's orange aura is animation 72 shown at 30 % opacity through its node 1).
- **Hit boxes are in the body file, per frame picture.** The table at header `+0x28` has, for each mesh, two `{u32 count, u32 offset}` pairs: attack boxes, then hurt (body) boxes. A box is 5 int32: centre x, centre y (up from the feet), width, height, kind (0 = body; 10/11 on the Empress's sword strike). The game's collision uses the boxes of the picture being shown (logged at the hit handler: the attacker's box sizes matched these entries). `set_attack_box` rewrites a picture's first attack box, or takes over a donor picture's record when the picture has none. Shots are different: a shot's box is a constant in the exe (`docs/moves.md` 1.5).
- **Animation names** are hashed (`tb2.ice_hash` of the Japanese name, e.g. 龍_通常ブレス（攻撃）); the exe fills the hashes at start-up from string literals, so names can be recovered from the program (*read from the program*).

- **Weapons are placed by their own pictures, not by body nodes.** A weapon file (`plN_cwXX`) holds
  the class's animations under the same name hashes and key times as the body (Warrior `pl1_cw00`:
  66 of the body's 67; Empress `pl0_cw00`: 124 of 130, numbered differently); each weapon picture's
  quad corners are relative to the character's feet (x forward, y up: the Empress's idle body spans
  y -6..66). The Warrior's body has no child nodes at all; position tracks (7/8) on body nodes are
  effect attach points (Empress `Wire`, Shinobi and Oracle effect parts). `move_picture` shifts a
  picture's corners; Build mod's `_offsets.txt` uses it (*confirmed in game: the Empress's idle sword moved up 40 px*).

Still not possible: brand-new animations (entries) and new hit-box records beyond taking over a donor's.

## 8. Next steps, in order of value

1. ~~Overlay hook and in-game test~~: done.
2. ~~Item table~~: found; edits and **new items** working in-game. Next: map weapon stats so new weapons get proper attack values.
3. ~~`.osb` part/cell/animation tables~~: decoded enough for new frames, timing, tracks and hit boxes (above); extra classes work (6–127, paged).
4. New moves: code, not data (`docs/moves.md`); a test class mod has its own shot, dash and lock-on.
5. Decode the `.stb` stage layout and the quest table. This is needed for new missions.
6. Sound bank codec and `.ivc` movies: low priority.

## Key addresses (preferred base 0x400000)

| Address | What |
|---|---|
| 0xe81a30 | IceReader open: builds the path, hashes it, calls `_wfopen` |
| 0xe82120 | MD5 hex of path |
| 0xe854b0 | read request (type, name, flags, key) |
| 0xe83e90 | readMain worker |
| 0xe816d0 / 0xe81740 | cipher init / decrypt step |
| 0xe845e0 / 0xe84920 | DecryptTask / InflateTask step |
| 0xec1b80 | zlib inflate |
| 0xe89410 | sets `"Win"` subdir global `[0x15271f8]` |
| 0xf61a8d | CRT `_wfopen` |
