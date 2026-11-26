# Making mods for Dragon Marked for Death

How to make data mods (items, chests, text, art, classes) with the kit's tools. For what the kit
is and how to install it, see the [README](../README.md). For the file formats behind the tools,
see [FORMAT.md](../FORMAT.md).

## How data mods work

- Every game asset is a file in `DataHash`, named by the MD5 of the asset name. The game's files
  are never changed.
- The loader (`winmm.dll`) hooks the game's file open: when `DataMod` has a file with the same name,
  the game reads that one instead. Replacement files are encoded like the originals, so the game's
  own decryption and decompression still run.
- Each data mod is a **package** in `mods\<folder>\` (made by Build mod). The Mod Manager combines
  every switched-on package into `DataMod`, merging what can be merged (text, items, chests,
  classes, icons) and warning when two mods change the same thing.

## Making content

Everything you edit is in a mod folder (`mod\` by default), as spreadsheets that open in Excel and
pictures. The tools are Python scripts; the modding tools download (Mod Manager → Build a mod →
Get the modding tools) carries its own Python in `tools\python`, so nothing needs installing.
Without that folder the `.bat` files use an installed Python 3.11 or later. Players never need
Python.

1. Double-click **List all items.bat**. It writes `mod\all_items.csv`, a reference list of every item with its label (code name) and current values. Changes to this file do nothing.
2. Edit **`mod\items.csv`**. One row per item you want to change or add.
3. Set your mod's name, version and author in **`mod\mod.ini`** (Build mod writes a starter one).
4. Double-click **Build mod.bat** (with the game closed). It checks your rows, makes the package in `build\package\<folder>`, installs it into the game's `mods` folder and combines. If something is wrong it lists the line and the problem, and changes nothing.
5. Start the game.

To share the mod, zip its folder from `build\package\`: anyone can drop the zip on their Mod
Manager. Another mod folder builds with `tools\py.bat tools\build_mod.py --mod <folder>`.

### items.csv

| Column | Meaning |
|---|---|
| `label` | The item's code name from `all_items.csv`, e.g. `USE0000_` (Herb). For a new item, make one up, e.g. `MOD_HERB_`. |
| `copy_from` | Only for new items: an existing label to start from (stats, icon, category). Leave empty to change an existing item. |
| `name` | New name, used for every language. `name_en`, `name_ja`, `name_de`, … set one language. |
| `price` | Shop price. Shops charge 90% of it. |
| `stars` | Rarity stars, 0–10. |
| `power` | Effect strength of consumables, e.g. 30 = heals 30%. |
| `shop` | `none`, `tool`, `tool-late`, `weapon`, `weapon-late`, or a number like `0x13`. |
| `unlock` | Story progress needed before shops sell it: `0` = from the start, `-1` = never (even with `shop` set). |
| `equip` | Who can equip it: `all`, or class names joined with `+`, e.g. `shinobi+bandit` (empress, warrior, shinobi, witch, oracle, bandit). |
| `str` `agi` `int` `vit` `luc` `pie` | Stat bonuses while equipped, as on the stat panel's left column. |
| `atk` `matk` `def` `mdef` `acc` `macc` `crt` | Combat bonuses while equipped: ATK, M.ATK, DEF, M.DEF, ACC, M.ACC, CRT. |
| `icon` | Icon number (Herb 16, bottles 14, food 15). |
| `order` | Sort position in lists. |
| `sprite` | Weapons: which weapon picture it shows, e.g. `pl0_cw00` (the starting sword), or a new sprite folder of yours (see Weapon sprites). |
| `sprite_palette` | Weapons: the palette file for that picture, as in `all_items.csv`. Usually leave it. |
| `color` | Weapons: which color scheme of the palette, e.g. starting sword `0` gray blade, `4` red blade. `all_items.csv` shows what each weapon uses. (The older spelling `colour` still works. With the 0.10 tools, spell it `colour`.) |
| `notes` | Anything; ignored. |

| `effect1` `effect2` `effect3` | The item's three effect slots: an effect name from `all_effects.csv` (e.g. `LEVEL_DAMAGE_UP`), or `none`. |
| `effect1_value` `effect1_value2` `effect1_time` | That effect's numbers, e.g. `LEVEL_DAMAGE_UP` 50 / 10 = "+50% P.dmg. to Lv.10+ enemies"; time is seconds for timed effects. Same for slots 2 and 3. Copy the numbers from an example item. |

Empty cells keep the game's value. **List all items.bat** also writes `mod\all_effects.csv`: every effect, whether it works, how many items use it, and an example item with its numbers. Effects marked "no game code" do nothing. Stats can be negative (a cursed ring with `luc` -10). A weapon still has to suit the class's weapon type, so `equip` on a weapon is best left as it is.

### Icons

**List all items.bat** exports the 24 item icons to `mod\icons_original\`, e.g. `16_herb.png` and `01_sword.png`.

- To redraw one, copy it into `mod\icons\` and edit it.
- Keep it 16×16, and keep the number at the start of the file name.
- **Build mod.bat** puts it into the game.

An item's `icon` column picks which icon it shows. Redrawing one of the game's 24 changes every item that uses that number.

**Brand-new icons:** name a 16×16 PNG with a number from 24 to 39, e.g. `mod\icons\24_my-weapon.png`, and set an item's `icon` to 24. Build mod adds it to a free space on the icon sheet, and the content mod lets the game use numbers above 23. There's room for 16. Copying an existing icon and redrawing it keeps the frame style.

### drops.csv (treasure chests)

Puts items into treasure chests, or changes how likely they are.

| Column | Meaning |
|---|---|
| `table` | A chest drop table from `all_drops.csv`, e.g. `DROP_1_0_0_N9`. `*` matches many: `DROP_1_0_0_*` is every level bracket of quest 1-0's chest 0, `DROP_*` is every chest. |
| `item` | An item label, from the game or your `items.csv`. |
| `weight` | How likely compared with the table's other items (often around 65 each). `0` takes the item out. |

When a chest opens, the game picks the table for your level, rolls whether its rare table applies, then picks one item by weight from those your class can equip. **List all items.bat** writes `mod\all_drops.csv`: every table, its chest, level bracket, rare chance, free slots, and current items. Each table has 16 slots. A `*` pattern skips tables that are full; a single full table is an error.

### Sprites (character art)

1. **Export sprites.bat** writes every animation frame of the six class bodies to `mod\sprites_original\<file>\`:
   `pl0_cb00` Empress, `pl1_cb00`/`pl1_cb01` Warrior, `pl2_cb00` Shinobi, `pl3_cb00` Witch, `pl4_cb00` Oracle, `pl5_cb00` Bandit.
2. Copy the frames you want to change into `mod\sprites\<same folder>\` and edit them. **Keep the file names**: they say which frame and position the picture is, e.g. `a001_f05_m348_o-41_92.png` is animation 1, frame 5.
3. Run **Build mod.bat**.

Things to know:
- Colors are matched to the class's palette, so edits follow whichever color scheme the player picks. Unchanged colors come back exactly.
- Frames are built from shared tiles, so redrawing a piece in one frame can change other frames that use the same tile.
- You can paint anywhere in the picture, including outside the character's original outline. Exported frames have a 24-pixel empty border for this. Don't resize or crop the pictures, because the position in the file name would no longer match. For more room, raise `MARGIN` in `tools\sprites.py` and export again.
- Pixels inside the original pieces are edited in place. Pixels outside them become new 16×16 tiles on the sprite sheet (Build mod reports how many). Identical tiles are stored once. If the sheet is full, Build mod makes it taller automatically.
- Frames exported before the margin was added still import fine.

### Weapon sprites

A full walk-through (new art, item, placement, timing, reach, effects, and a custom class's own
weapons): [weapon-example.md](weapon-example.md).

**Export weapon sprites.bat** writes every weapon picture to `mod\sprites_original\<file>\` (`pl0_cw00` … `pl4_cw00`, Shinobi throwing weapons `pl2_tw00` …), plus `mod\sprites_original\weapons.csv`: which items use each picture, with their color numbers.

- **Change an existing weapon picture:** copy frames into `mod\sprites\<same folder>\` and edit them, like bodies. Every item using that picture changes, for every class that uses it (Bandit's daggers are Empress pictures, his shuriken Shinobi pictures).
- **Make a new weapon picture:** copy a **whole** exported folder into `mod\sprites\` and give it a new name, using only letters, digits and `_`, e.g. `mod_cw00`. Redraw what you like (the `_source.txt` inside says which picture it started from; keep it). Then set your item's `sprite` to `mod_cw00`. The game's own weapons stay as they are.
- Edits use the palette's first color scheme. Other schemes (`color`) recolor your edit the same way they recolor the original.
- Four Shinobi weapons (`pl2_cw01`–`pl2_cw04`) are full-color pictures: edits keep your exact colors, and `color` doesn't apply to them.

### Portraits and other full-color art

**Export sprites.bat** also exports these full-color pictures to `mod\sprites_original\`. Edit them like body frames (copy into `mod\sprites\<same folder>\`); colors and soft edges are kept exactly.

| Folder | What it is |
|---|---|
| `com_pl_icon` | Class portraits in menus (equip menu, load screen). Large: Empress `a011`, Warrior `a001`, Witch `a002`, Shinobi `a003`, Oracle `a004`, Bandit `a010`; the other six are the small versions. |
| `s_o0a00` | The character select screen: class illustrations in color (narrow: Empress `a043`; wide/highlighted: Empress `a119`) and sepia (`mesh_m131`–`m140`, unselected), background, contract-color icons, labels in every language, frames. |
| `cutin00` | The dragon orb cut-in: one tall illustration per class (Empress `mesh_m3…`) and colored backgrounds. |
| `cp_o0000` | The in-quest HUD, including its own small class portraits. |
| `face`, `EmFace` | NPC and enemy faces in dialogue. |

Pictures that no animation shows by itself are exported as `mesh_m<number>_….png`; they edit the same way. A reskinned class needs its art changed in all of these: body, portraits (`com_pl_icon` and `cp_o0000`), select screen (`s_o0a00`, color and sepia) and cut-in (`cutin00`).

### Extra classes (classes.csv)

`classes.csv` defines classes beyond the six, one row each. The quickest start is
`tools\py.bat tools\new_class.py <folder> "<Class name>" --moves empress`: it makes a mod folder with
one class, a placeholder look (the borrowed class recolored) and all its pictures. Build it with
`tools\py.bat tools\build_mod.py --mod <folder>`.

| Column | Meaning |
|---|---|
| `id` | The class's own name for the program, e.g. `cleric` (letters, digits, `_`). Saves are kept by it. |
| `name` | Its name in the game, e.g. `CLERIC`. |
| `moves` | Which class's moves it uses: `empress`, `warrior`, `shinobi`, `witch`, `oracle`, `bandit`. |
| `body` | Its body sprite folder in `sprites\`, e.g. `cleric_cb00` (a copy of a body: the folder's `_source.txt` names the original, e.g. `pl0_cb00`). |
| `palette` | Its palette, e.g. `cleric_pal` (from `palettes\cleric_pal from pl0_pal.png`). |
| `description` | Text for the info box when creating a character (pages separated by `\|`; missing pages are the base class's). |
| `panel_art`, `highlight_art`, `portrait_art`, `portrait_large_art`, `bar_face_art`, `hud_face_art`, `hud_face2_art`, `hud_frame_art` | Its pictures for character select, menus, the bar and the quest HUD (file paths in the mod folder; `new_class.py` makes them). An empty one shows the game's gray "empty" dash. |
| `test_saves` | `1` while testing: the game's save writes go to `SystemData.bin.class-test` instead of your save. |

**Numbers and pages are given out automatically.** The Mod Manager (or Build mod) numbers every
installed class: each sits in the column of the class whose moves it borrows, on page 2, 3, …
of character select, sorted by `id`. So installing another class can move yours to another page;
saves follow the `id`, so characters stay with their class.

**Playing one:** on character select, at the inn and in the bar, **Page Down / Page Up** or
**RT / LT** change page. The first time you pick an extra class it shows NEW: you choose its color,
voice and name, and it starts at level 1.

**Saves:** your `SystemData.bin` always holds the six classes' own data, so the game works the same
without the mod. Each extra class's character is in **`SystemData.bin.classes`** next to it; keep
the two together. `tools\py.bat tools\save.py <path to SystemData.bin>` lists every class.

**Its own weapon and effect files.** A new body with other proportions needs its weapons placed
differently (`_offsets.txt`), and maybe its swings' reach changed (the slash effects' boxes,
`_hitboxes.txt`), without changing the class it borrows from. It gets its own copies by name, from
its body folder's name without `_cb00` (body `cleric_cb00` -> `cleric`):
- `cleric_ef00`: used instead of the borrowed class's effect file (`pl0_ef00` for an
  Empress-based class);
- `cleric_` + a weapon file's name, e.g. `cleric_pl0_cw00`: used instead of that weapon file
  (`pl0_cw00`, the starting sword), for every item that shows it; `cleric_pl2_tw00` for a Shinobi
  throwing weapon. Make only the ones you change.

Each is a copied sprite folder like any new sprite file (with a `_source.txt` naming the original,
e.g. `pl0_cw00`), and must be in **the same mod as the class** (combining gives each mod's new
files its own prefix). They're used during quests (*confirmed in game: an Empress-based class's
own swords 40 px higher, the Empress's unchanged*); character pictures in menus (status, inn)
still show the original files. `dmfd_content.log` lists each class's own files at start-up.

**Its own moves** need a code mod: moves are written into the game's program (see
[moves.md](moves.md)). Its look, timing and hit boxes are data (next section).

### Animations: timing, hit boxes and tracks

Three optional text files in a body's sprite folder (`sprites\<body>\`) change its animations.
Animation numbers are the `aNNN` in the frame file names; `tools\py.bat tools\anim_viewer.py
<exported folder> out.html` makes a page that plays every animation, numbered. Lines starting
with `#` are comments.

- **`_timing.txt`**: new lengths and frame timing (*confirmed in game*). Times are in game
  frames (60 a second).
  - `a<n> <length> <t0> <t1> ...` sets when each of the animation's pictures starts.
  - `a<n> <length> <time>:<picture> ... [loop <from> <every> <picture> ...]` gives it a new picture
    track that shows its own pictures (0-based) in any order, as often as wanted; `loop` repeats
    some of them every few frames up to the end.
  - `a<n> nodes <count>` keeps only its first nodes (e.g. drops the tether attach point the
    Empress's wire animations carry).
  - `0x<hash>` (an animation's name hash) can stand in for `a<n>` in every line.
  - **Weapons don't follow a body's timing by themselves** (*confirmed in game*): each weapon file
    has its own copy of the animations with the old timing, so a slowed body animation leaves the
    weapon running ahead. Put a `_timing.txt` in the weapon's folder (`sprites\<weapon file>\`)
    with `follow <body folder>`: every plain retiming line of that body's `_timing.txt` is applied
    to the weapon's animation of the same name, each weapon key moved to where its moment falls in
    the new timing (a weapon animation needn't have the same keys as the body's). `follow <body
    folder> <anim> ...` follows only the listed ones (the body's `a<n>` or `0x<hash>`). Picture-track
    lines (`time:picture`) can't carry over; Build mod says so. Like any weapon edit this changes
    every item and class using that weapon file.
  - **Some parts of a move are timed by the game's code**, not by the animation: slowing the
    Empress's sword combo slowed the wind-up and recovery, but the strike itself (the part with the
    slash effect) still played at full speed and the move went on as usual (*seen in game*).
- **`_hitboxes.txt`**: where a frame hits (*confirmed in game*). Each frame picture (`m<number>` in
  its file name) has its own attack boxes.
  - `m<mesh> attack <x> <y> <w> <h> <kind>`: that picture's attack boxes become this one box
    (center x, center y measured up from the feet, width, height; kind 0 = body, 10/11 = the
    Empress's sword strike).
  - `m<mesh> attack none`: that picture deals no hit.
  - `m<mesh> attack <x> <y> <w> <h> <kind> from m<donor>`: for a picture with no attack box, take
    over another picture's (that one then deals no hit).
  - Some moves' damage is dealt by the game's code while an animation plays; the boxes decide
    where it lands.
  - `m<mesh> hurt ...`, with the same forms: the picture's **hurt boxes** (where the character can
    be hit), e.g. the Empress's idle body box is `0 28 32 56 0` (*confirmed in game: with
    `0x67d7af17 hurt none` nothing hit her while she stood idle, and she was hit as usual otherwise*).
  - `a<n>` or `0x<hash>` in place of `m<mesh>`: the line applies to every picture of that animation.
  - `m<mesh> attack <x y w h kind> <x y w h kind> ...`: several boxes at once, written over the
    picture's own; at most as many as it already has (the file has no room for more).
  - **Reach of a class's normal attacks is in its effect file** (*confirmed in game*). A sword
    swing's hits come from the slash effect that plays with it: the class's effect file (`plN_ef00`)
    has an animation of the same name as the body's (e.g. `剣_通常２段（斬り）`), started together
    with the body and the weapon, and its pictures carry the attack boxes (the Empress's combo hit 1:
    three boxes up to about 80 px ahead; each hit has its own kind, 1, 2, 3). So to change how far a
    swing reaches, put a `_hitboxes.txt` in `sprites\pl0_ef00\` (or the class's own effect folder),
    e.g. `0x984c24b9 attack 150 30 220 80 2` made the Empress's hit 2 reach about 260 px ahead. It is
    per move for the whole class, not per weapon item (every weapon uses the same effect). The body's
    own pictures carry boxes only for some moves (e.g. the Dragon Sword strike); weapon files carry
    none.
- **`_offsets.txt`**: move frame pictures, e.g. **where a weapon sits on each frame**. x is
  forward (as drawn facing right), y is up, in pixels.
  - `m<picture> <dx> <dy>`: move that frame picture (`m<number>` in its file name).
  - `a<n> <dx> <dy>`: move every picture of animation `n`.
  - `a<n> k<key> <dx> <dy>`: move only the picture its key `<key>` shows (0-based, in key order).
  - `0x<hash>` can stand in for `a<n>`: an animation's name hash is the same in a body and its
    weapons, while the numbers differ (see below).
  - A picture shown in other places moves there too; Build mod lists them. Moves of one picture
    add up. A folder may hold only this file (no PNGs).

  **How weapons are placed:** a weapon isn't attached to a point on the body. Each weapon file
  (`plN_cwXX`, Shinobi throwing weapons `pl2_twXX`) carries its own copy of the class's animations,
  under the same names and with the same key times, and the game plays the one matching the body.
  Each weapon picture is drawn relative to the character's feet, so its place on a frame is simply
  where that picture sits. To move a weapon on one move, put an `_offsets.txt` in
  `sprites\<weapon file>\`; a weapon file numbers its animations its own way (the `aNNN` in an
  export of that file), so `0x<hash>` is handy: e.g. `0x67d7af17 0 40` lifts the weapon 40 pixels
  in the Empress's idle (`待機`). Every item using that weapon file changes; for one new weapon only,
  copy the folder under a new name first (Weapon sprites). Body nodes with positions
  (channels 7/8, see `_tracks.txt`) are effect attach points such as the Empress's tether, not
  weapons. (*Confirmed in game: the Empress's sword 40 px higher in her idle.*)
- **`_tracks.txt`**: other animation values (*confirmed in game* for light colors):
  `<animation name or 0xhash> <node 0xhash> <channel> <value>` sets every key of that track, e.g.
  an effect's light color (channels 10/11/12 = red/green/blue, 0 to 1).
- **`_replace.txt`** (an empty file): each frame PNG in the folder is a whole new picture for its
  frame instead of an edit of the old one, so a class can look completely different
  (*confirmed in game*). It works in weapon folders too (`plN_cwXX`, `pl2_twXX` and copied new
  weapon folders), so a weapon of a quite different shape needn't be painted over the old one
  (*confirmed in game: the Empress's idle sword drawn as a ring*).

**Swing trails and effects.** A sword swing has three parts playing the same animation name: the
body, the weapon (its blurred blade) and a slash effect in the class's effect file (`plN_ef00`: the
white arc, which also carries the swing's attack boxes, see `_hitboxes.txt`). Other effects, such as
the Empress's walking-slash trail or her fireball, are started by the move's code from the same
file. All of them can be redrawn like any sprite file. Each
weapon file also has a couple of animations its body doesn't (2 in `pl0_cw00`), unused as far as
known.

**Effect Tweaks** (a code mod, `codemods/EffectTweaks`) changes how effects play, from
`mods\EffectTweaks\effects.txt`, one effect a line:
`<animation> swap=<animation> start=<frames> dx=<pixels> dy=<pixels> delay=<frames> speed=<x>`.
- `<animation>` is `0x<name hash>` or the animation's name. To find an effect's hash, tick "Log
  animations as they start" on the mod's page in the in-game menu, do the move, and read the new
  lines in `mods\EffectTweaks\mod.log` (each animation is logged once, with the game code that
  started it; the Empress's own effects come from code addresses `d30000`-`d70000`).
  `tools\anim_names.py` gives names where known.
- `swap=` plays another animation of the same file instead.
- `start=` begins that many frames into the effect (skips its beginning).
- `dx=`/`dy=` shift it forward (as drawn facing right) and up. This works for effects attached to
  the character (*confirmed in game: the Empress's walking-slash trail 30 px higher*); a shot's own
  picture ignores it (it is placed at the shot every frame; *seen in game with her fireball*).
- `delay=` waits that many frames (60 a second) before the effect plays; it's kept out of sight
  and paused meanwhile. `speed=` plays it faster or slower (0.5 = half speed). (*Both confirmed in
  game on the Empress's walking-slash trail.*)
- Some moves wait for their effect to finish (the Empress's walking slash does): a delay or a
  slower speed holds the move up by as long.
- A swing's slash effect has the same animation name as the body's and the weapon's (see Swing
  trails above), so a line for that name changes all three.
- "Reload effects.txt" in the menu applies changes without restarting.

Every class's effects go through the same engine call, so this works for all of them.

**Where breath shots start** (Effect Tweaks 0.3). The Empress's breath shots (fire, wood, earth,
metal, water) start at a point fixed in the game's code, picked by the pose she breathes from. A
taller or shorter body needs them moved. In the same `effects.txt`:
`shot <pose> dx=<pixels> dy=<pixels> class=<class>`
- `<pose>` is 0-5 or `all`. The game's points (forward, up from her feet): 0 (30, 37), 1 (30, 37),
  2 (30, 56), 3 (30, 37), 4 (26, 43), 5 (0, 86); 0 is standing and 4 a jump (*seen in game*; the
  others not yet identified). With "Log animations as they start" ticked, the
  log also shows each pose (and class) the first time a shot starts from it, to tell which is which.
- `dx=`/`dy=` are added to that point (forward as drawn facing right, up).
- `class=` is 0-5 (0 = Empress) or an extra class's id (from its class definition); without it, the
  line is for every class using the Empress's moves. The most specific line wins: class and pose,
  then class, then pose, then `all`.
- This moves where shots hit, so the mod is `online=same`: everyone in a party needs the same
  `effects.txt`.
- The shots' own art is in their sprite files; their hit box sizes are still fixed in the game's code.

Example, an Empress 20 px taller: `shot all dy=20 class=0`. (*Confirmed in game: standing, jumping
and charged shots all moved.*)

**Finding a move's animations:** `tools\py.bat tools\anim_names.py pl0_cb00` lists a sprite file's
animations: number (the `aNNN` in exported frame names), name hash, length, picture keys and name
(from `tools\anim_names.txt`: 503 names, most body and weapon animations; many effect ones are
unnamed). A second argument filters by name, e.g. `anim_names.py pl0_cw00 剣_通常`. Animations are
known by number and by name hash. For example, the Empress's normal sword combo
is `剣_通常１段`/`２段`/`３段`, each in four parts: wind-up 準備, slash 斬り, end 終了, return 戻り
(hit 2: `0x02ff4ca0`, `0x984c24b9`, `0x7d1ac120`, `0xe72fda56`); her Dragon Sword moves are the
`龍_ソード` ones. How names become hashes: FORMAT.md, "Animation names".

The game's file opens are logged; if the game crashes, `dmfd_content.log` records the crash address and call chain.

### Screen layout (layout.csv)

Menu screens are built from named parts with a position and scale. `tools\py.bat tools\layout.py Win/s_o0a00.osb` lists them; `mod\layout.csv` moves them.

| Column | Meaning |
|---|---|
| `file` | Sprite file, e.g. `s_o0a00` (character select). |
| `anim` | Which layout (animation number); empty = every layout in the file that has the part. |
| `part` | Part name, e.g. `PL1` (Empress panel), `PL1_Lv` (its level), `name_text`, `color`. |
| `x`, `y` | Position: screen is about 640×360, center is 0,0, y goes up. |
| `scale_x`, `scale_y`, `picture` | Size (panels are 0.5) and which picture it shows. |

Character select: `PL1`–`PL6` = Empress, Warrior, Shinobi, Witch, Oracle, Bandit at x −270…30 (60 apart), y 110; levels at y −84. The panel and its color highlight follow the layout; the selector frame slides to the panel but keeps its own height.

### Palettes (color schemes)

**Export sprites.bat** writes the class palettes and **Export weapon sprites.bat** the weapon palettes to `mod\palettes_original\`, one picture per palette file (`pl0_pal.png` is the Empress, `pl0_cw00_pal.png` the starting sword's).

- Each picture is a grid of squares: **one row per color scheme, one square per color.** Sprites are painted with the first row's colors; each scheme swaps those for its own.
- Class palettes have 48 rows: 8 highlight colors (picked at character creation) × 6 contract elements, row = highlight × 6 + element. For the Empress, squares 33–36 are the dragon arm (element) and 16, 49–52 the highlight color; everything else (skin, hair, armor…) is the same in all 48 rows.
- **Edit:** copy the picture into `mod\palettes\`, fill squares with new colors (paint bucket), run **Build mod.bat**. Don't resize it.
- **Shortcut:** a color that's the same in every row only needs changing in the **first row**; Build mod copies it to all rows (unless you also edited other rows of that column).
- A weapon's `color` in items.csv is the row number in its palette.
- **New palette file**, e.g. for a new weapon: name the picture `<new name> from <game palette>.png`, e.g. `mod_cw00_pal from pl0_cw00_pal.png`, and set the item's `sprite_palette` to `mod_cw00_pal`.
- Adding extra rows (more color schemes) isn't possible yet.
- To find which square a color in a sprite is, use a color picker on an exported frame and look for the same color in the first row.

### text.csv

Any other game text: one row per line of text, with columns `file`, `label`, `variant`, `lang`, `text`.

- `file` is a text table such as `sys_quest` or `q_0500`.
- Leave `lang` empty to set every language.
- Write `\n` for a line break.

`toolspy.bat tools	b2.py dump sys_quest.tb2 out.json` shows a table's labels.

### Removing the mod

If a player removes the mod while owning a modded item, the save still loads. The item shows as `ERROR#00` (or a blank slot) and vanishes from the menus when unequipped, but it isn't deleted: reinstall the mod and it's back. A missing equipped weapon leaves the character empty-handed but still able to attack. Tested with a consumable and with an equipped sword.

### Lower-level tools

```
tools\py.bat tools\tb2.py dump sys_item.tb2 sys_item.json      # text table -> editable JSON
tools\py.bat tools\dmfd_data.py dump Win/com_itic.osb out.osb  # decode any asset
tools\py.bat tools\osb.py --asset Win/pl0_cw00.osb out Win/pl0_cw00_pal.osb   # sprite textures -> PNG
tools\py.bat tools\sprites.py Win/pl0_cb00.osb out Win/pl0_pal.osb 0        # every animation -> PNG strips
```

The tools find the game through Steam. If yours is elsewhere, set `DMFD_GAME` to its folder, or put
the folder's path in `tools\game.txt` (the Mod Manager writes that file when it sets the tools up).

## Code mods

Code mods are DLLs the loader starts from `mods\<Name>\` (each with a `mod.ini` naming its `dll=`).
They change what the game's program does, which data can't: damage rules, moves, menus. See
`codemods/` in the source: `common/` is the small API they share (logging, hooks with MinHook, a tab
in the in-game Insert menu; MIT licensed, so mods can use any license), and Holy Up
(`codemods/HolyUp`, explained in [holy-up.md](holy-up.md)) is a complete example.
Build them with `tools\build_codemods.bat` (Visual Studio 2019 Build Tools, 32-bit).

A code mod's `online=` in `mod.ini` says whether everyone in a room needs it (`same`) or not (`any`).
