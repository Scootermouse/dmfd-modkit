# How a class move works: the Empress's Dragon Shot and the Shinobi's mark

How a move is put together in the game, traced through the Empress's basic shot and the Shinobi's
lock-on. Addresses are in the unpacked `game.exe` at its preferred base 0x400000 (the game loads
elsewhere, so code converts them). Coroutine names come from the game's own RTTI strings.

**How sure each part is.** Most of this was read from the program (disassembly), not watched in
the game. Where a finding was checked in game it says *confirmed in game*; *program only* means it
has not been. Section 5 lists what a test class mod confirmed.

## Short version

- **A move is code, not data.** Each class is a C++ object (`cPlWork00` = Empress … `cPlWork05`)
  whose moves are its own coroutines (`coActionStandShot`, `coAttackShot`, …). Which button does
  what, the timings, speeds, bursts, MP costs and damage numbers are in the exe, mostly as
  constants that start-up code copies into tables. Changing them needs a code mod.
- **The look is data.** Body animations (`Win/pl0_cb00.osb`) and the shot's picture
  (`Win/pl0_ef00.osb`, animation 炎) are files Build mod can replace, and so are a body's
  **hit boxes**, its animation **timing** and some animation **tracks** (section 5, FORMAT.md).
- **An extra class borrows the whole C++ object** of its base class (`moves` in classes.csv).
- **The Shinobi's mark** is stored on his own player object (a lock-on block at player+0x106C:
  a self-clearing link to the enemy plus a timer), set when his Dragon Dash hits. Homing shuriken
  copy the mark when thrown and steer onto it every frame.

## 1. The Empress's Dragon Shot, start to finish

The Empress's shot depends on her **contract element** (player byte +0xF5F: 0 none, 1 Wood,
2 Fire, 3 Earth, 4 Metal, 5 Water). The fireball is the **Fire** version; the other elements give
tornadoes, rocks, lasers and ice from the same code with other numbers.

### 1.1 Button press and input check (code)

| Step | Where | What |
|---|---|---|
| Command table | `0xD61B90` (Empress vtable slot 154) | Builds the class's command list at player+0xF2C: name hash → game action. **"Shot" = game action 0x25** (the shot button); StandSlash 0x22, ThrustDash 0x23, DBite/JumpDBite 0x24, Wire 0x21. Each entry has a 10-frame buffer. |
| Buffered press | `0xD73A90(hash)` | Takes a buffered command out of the set at player+0xF34 (true if it was pressed). |
| Can shoot? | `0xD52F80` | "Shot" pressed, not blocked by states 0x12/0x13, and enough MP: `0xD78560(cost)`. |
| Who asks | idle `coSubActionStay` 0xD52890, run 0xD53100, jump 0xD538E0, and inside other moves (thrust, bite…) | Each "free" state checks the command itself. Holding Shot for 2 frames+ turns it into the charge shot (`coActionChargeShotCharge`). |

Input goes through the game's own actions, so any controller works (as our rules require).

### 1.2 Picking the move (code)

- Standing: `coActionStandShot` (0xD5AE90) for the body, which starts `coAttackShot` (0xD5C080,
  the "attack layer" that also runs while running or jumping). Backing off: `coActionBackStepShot`
  (0xD5B2B0). In the air: `coAttackShot` directly from the jump state.
- A coroutine is started with `0x7B79B0(player, body, …)`; the running ones are remembered by name
  hash at player+0x330 (body) and +0x334 (attack layer).
- **Online:** the local player's `coAttackShot` sends a `Pl00ShotInfo` note (`0x7A7DA0`); the
  other machines' copies of her receive it in her switchboard **0xD515E0** (a list of
  `Pl00…Info` types, hashes at 0x28C241C…0x28C246C) and start the same coroutines. So every move
  exists twice: "decide locally" and "replay a note from the network".
- **MP cost** (Holy Up's "piety" is unrelated): per element, ×100 internally. On the ground a flat
  cost (`0x28BFEF0`: none 500, Wood 750, **Fire 400**, Earth 900, Metal 700, Water 500). In the
  air each further press costs more, by a counter at player+0x1098 that resets on landing
  (`0x28BFF20`…`0x28BFF80`; Fire 400 → 1200 → 2400 → 4000). Equipment can cut it to ×0.85 / ×0.7
  (player+0xF4C bits 20/21).

### 1.3 The animation and the frame the shot comes out (animation = data, timing = code)

`coSubAttackShot` (0xD5BBF0) plays the arm animation in segments (tables 0x11126A0 count,
0x11126BC names, 0x11126D0 lengths in frames):

| Pose | Animations (in `pl0_cb00.osb`) | Frames |
|---|---|---|
| 0 standing | 龍_通常ブレス（攻撃） → （終了） | 12 + 12 |
| 1 jumping | 龍_ジャンプブレス（攻撃） → （終了） → （戻り） | 9 + 6 + 4 |
| 2 standing, aimed | 龍_通常ボルト２（攻撃） → （終了） | 12 + 12 |
| 3 jumping, diagonal | 龍_ジャンプ斜めショット（攻撃）→（終了）→（戻り） | 9 + 6 + 6 |
| 4 back-step | バックステップショット | 9 |

- **The shot is not tied to an animation frame.** `coAttackShot` fires the first shot at once
  (frame 0 of the attack segment), then the rest of the **burst** on a timer: per element
  burst size `0x28BFF98` = {3, 2, **3**, 1, 1, 5} shots, gap `0x28BFFB0` = {3, 4, **3**, 3, 3, 2}
  frames. Fire: 3 fireballs on frames 0, 3, 6.
- A new press is accepted after **19 frames** (`0x112114C`), i.e. at most ~3 bursts a second.

### 1.4 Making the shot object (code, except its picture)

`MakeShot` **0xD63AE0**(pose, element, charged, …):

- Creates a **`cPshWork04`** (0x7A0 bytes, vtable 0x0110E314, the Empress's shot class; owner
  player at +0x668). Spawn point = player position + a per-pose offset (0x28F4860: e.g. standing
  (30, 37), jumping (26, 43)), mirrored by facing.
- Its kind (shot+0x180) = `0x1111F00[element*2 + charged]` picks one of 12 set-up functions
  (table 0x28E9F58, filled at 0x6DAD00): Wood/Fire/Earth/Metal/Water/Metal-laser × normal/charged.
  **Fire normal = 0xD31B20** → coroutine `coActionFireNormal` 0xD335E0.
- The set-up points the shot at its **attack table** (shot+0x36C, entries at +0x60, count +0x68),
  adds a **hit box**, and loads its picture **`pl0_ef00.osb`** (string 0x110E680, by
  `0xE093C0`).
- **Sprite:** animation **炎** (flipbook meshes 5–9 plus a glow node, color 1/0.58/0.2) in
  `Win/pl0_ef00.osb` (1024×1024 full color, 87 animations). On a hit it plays 炎_破裂, at the end
  of its life 炎_消滅. **Data:** replaceable with Build mod (full-color import works).
- **Speed:** 12 px per frame forward (`coActionFireNormal`), vertical drift by shot number
  `0x110E248` = {0, +0.5, −0.5} px/frame (the 3-way fan), tilted 20° for the diagonal pose.
- **Lifetime:** 40 frames (≈ 480 px of travel), then it fades (`coActionFire_Move` 0xD36550).
- **How many at once:** no cap found in the code. In practice one press makes 3, a press every
  19 frames, each lives 40: up to ~6–9 fireballs from one Empress, limited by MP.

### 1.5 Hit box and hit (code)

- **Hit box:** one rectangle pushed into shot+0x6B4 (`0x986950`): Fire `{-8, 0, 52, 28}` (const
  0x1122940), Wood `{0, 16, 32, 32}`. Not in the osb file (shots only: character bodies keep theirs in the osb, see section 5).
- **Attack table** (bss, filled at start-up from exe constants; Fire at **0x28B6818**, init
  0x6DA8C0), 36-byte entries:

| Offset | Fire entry 0 (fireball) | Entry 1 (charged) | Meaning |
|---|---|---|---|
| +0x00 | 0 | 0 | kind, via status vtable +0x48 (unclear) |
| +0x04 | 2 | 2 | element (1 Wood … 5 Water) |
| +0x08 | **160** | **450** | power (motion value ×100) — the real one Holy Up was missing |
| +0x0C | 4 | 4 | ? (same in every shot table) |
| +0x10 | **7** | 7 | attribute: 0–5 physical, 6 magic, **7 Holy**, −1 = the weapon's; status vtable +0x4C may convert it |
| +0x14 | 1.0 | 1.0 | multiplier (what Holy Up logged as "motion value") |
| +0x18 / +0x1C / +0x20 | 0 / 100.0 / 100 | same | not identified (hit-stop / knock-back / stun?) |

  Other elements: Wood 425/567, Earth 155 (×3), Metal 800/550 (+2 laser entries), Water 150/700.
- **The hit:** the dispatcher 0xCC4560 takes the entry (index from `attacker vtable+0x68
  (hit+0x38)`), turns +0x10 into the path (0xCC44B0/C0/E0: <6 physical, 6 magic, 7 Holy) and calls
  0xCC4620 / 0xCC4E20 / 0xCC55E0. So Holy Up's 12/32 fireball/barrage damage ≈ 160/450 power. ✔
  matches. (Holy Up's "barrage", entry 1, is the charged shot's table entry.)
- *Program only:* the power values were not changed and re-measured in game; Holy Up's logged
  damage fits them.

## 2. Data or code: the map

| Piece | Data or code | Where |
|---|---|---|
| Which button (Shot = action 0x25) | code | 0xD61B90, literals 0x103563C |
| When the move may start (idle/run/jump checks, MP check) | code (per class) | 0xD52890, 0xD52F80 |
| Which move runs (stand/air/back-step, charge after 2 frames) | code (per class) | coroutines in cPlWork00 |
| Online replay of the move | code (per class) | 0xD515E0, Pl00ShotInfo |
| MP cost per element and per air shot | code constants | 0x6E5CF0…0x6E5E70 → 0x28BFEF0…0x28BFF80 |
| Body/arm animation frames | **data** | `Win/pl0_cb00.osb` (animations 龍_通常ブレス…) |
| Animation names and segment lengths | code | 0x11126A0/BC/D0 |
| Burst size, gap between shots, re-press delay | code constants | 0x28BFF98, 0x28BFFB0, 0x112114C |
| Shot picture and its hit/vanish animations | **data** | `Win/pl0_ef00.osb` (炎, 炎_破裂, 炎_消滅) |
| Which file the shot uses (`pl0_ef00.osb`) | code (a string literal) | 0x110E680, pushed in each set-up 0xD31750… |
| Spawn offsets, speed, spread, lifetime | code constants | 0x28F4860, coActionFireNormal 0xD335E0, 0x110E248, 40 frames |
| Hit box | code constant | 0x1122940 (Fire), shot+0x6B4 |
| Damage power, element, Holy attribute | code constants (start-up tables) | 0x28B67D0…0x28B6960, inits 0x6DA7F0…0x6DAC30 |
| Damage formula | shared code | 0xCC4560 and the three paths |

`tools/dmfd_data.py` finds **no** move data in DataHash: none of the info names, coroutine names or
their hashes appear in any of the 1,046 files. So Build mod can change only the looks; everything
else needs a code mod (MinHook hooks or patched constants, like Holy Up and Rebalance).

## 3. Where a class's move set lives, and how `moves` maps an extra class

- The move set **is** the class's C++ object: vtable (Empress 0x1112C5C, 173 slots; Shinobi
  0x11085F4) + its coroutines (54 for the Empress, list in `cobodies.txt`) + its shot classes
  (Empress `cPshWork04`; Shinobi `cPshWork02`, `cPlWork02LockOn`…) + its start-up tables.
- Every player is created by **0xDE7B90** from the type table **0x111A1D4**, indexed by class.
  The content mod (`src/classes.cpp`) gives an extra class its base class's entry there, so the
  extra class *is* a `cPlWork00` object; only its class byte (player+0xF5C) tells them apart.
  `combine_mods.py` numbers extra classes by the moveset column (`moves` in classes.csv,
  `base=` in classes.txt).
- Consequences for custom moves:
  - A hook on an Empress function also runs for every class that borrows her moves. To change
    only one class, the hook
    checks `player[0xF5C]` (class number) and, for shots, the owner at shot+0x668.
  - The body and palette are already per class; the **shot effect file is not** (literal
    `pl0_ef00.osb`), so an extra class's shots look like the Empress's until a code mod redirects that file
    per class (*confirmed in game* with a test class: hook the effect loader `0xE093C0` and swap
    the name for shots whose owner, shot+0x668, is that class).
  - Online, the other players run the same code from the `Pl00…Info` notes; a changed move must be
    changed the same way on every machine (everyone has the same mods anyway).

## 4. The Shinobi's mark (lock-on)

From the wiki: Dragon Dash (`coActionDDash`) hitting an enemy does a somersault kick and puts him
in Lock-on status for 5 s (7.9 s with the Fire contract); homing shuriken then hit only that enemy.

### Where the mark is stored

On **the Shinobi's own player object**, not on the enemy:

| Field | What |
|---|---|
| player+0x1098 / +0x109C | the marked enemy's **network id** (two dwords, −1 = none; copied from the enemy's +0x4C8/+0x4CC) |
| player+0x10A0 | "new mark" request flag |
| player+0x106C | the lock-on block (`cPlWork02LockOn`): |
| …+0x00/+0x04 | a link in the enemy's list of watchers, so the mark clears itself when the enemy dies |
| …+0x08 | the enemy object |
| …+0x0C | the marker effect over the enemy (`cPlWork02EffLockOnMarker`, made by 0xD16250) |
| …+0x10 | timer in frames, from `0x28A7ADC[element]` = {255, 300, **480 (Fire)**, 300, 300, 300} |
| …+0x14 / +0x15 / +0x16 | new-target flag / don't-expire flag / show-marker flag |

### How it gets set

1. **Hit callback, vtable slot 91 (+0x16C)**, Shinobi 0xD15AD0 (called by the hit system at
   0xE9E7F0; Empress 0xD642E0, base 0x7B22F0). Only on the machine that controls him
   (vtable+0x1DC). If the running move (+0x330) is `coActionDDash` and the victim's hit info
   (+0x36C)+0x75 says it can be locked, it casts the victim to `cActWork`, stores its network id at
   +0x1098/+0x109C and sets +0x10A0.
2. **`coLockOn`** (0xD17240, runs all the time): on a new mark it sends **`Pl02NoticeLockOnInfo`**
   (id pair) to the other players, then turns the id into the enemy object through the app's actor
   map (0x7C9770 → 0xD83C00, map at +0x70, lookup 0xA56E50), trying for 30 frames, and stores it
   (0xD17800). Remote copies get the note in the Shinobi's switchboard (0xD0235D) and do the same.
3. **Each frame** 0xD17660: starts the timer for a new mark, counts it down, removes the marker
   and the link when it runs out. 0xD7C7A0 = "mark still valid" (enemy alive, vtable+0x7C).

### How homing reads it

- When a shuriken is thrown (`coSubThrow` 0xD07A70), `0xD17860(lockon)` asks "is something
  marked?"; if yes the throw becomes the homing version (4 shuriken), and `0xD01070` hands the
  enemy to each shuriken, which keeps its **own link** at shot+0x6E0 (`cPshWork02`).
- `coActionHomingMove` (0xD28340): every frame, if the link is still valid it asks the enemy for its
  aim point (enemy vtable+0x1AC → shot+0x6F8) and moves along a curve (0xD2AD40) that reaches it
  after **22 frames**, turning the sprite to face its direction. So it can't miss while the mark
  lives; if the enemy dies mid-flight it carries on straight.

### Using it for another class

- The pattern to copy: **"on my hit during the dash → remember that enemy (by a self-clearing
  link or by its network id) with a timer → my shots steer to it"**.
- On a class built on the Empress we can't reuse player+0x106C (those offsets mean other things
  for the Empress: +0x1098 is her air-shot counter, +0x10A8 her Metal lock target). A code mod keeps
  its own small table: tagger player → enemy + timer, as Holy Up keeps its stacks.
- Hooks: the Empress's hit callback (slot 91, 0xD642E0) for "my dash hit", or (as the
  test mod did) note the class's own body hitting an enemy at the damage roll 0xCC4560 while its
  dash runs (*confirmed in game*).
- Online: tag by the enemy's network id (+0x4C8/+0x4CC), which every machine agrees on — that is
  how the game itself shares the Shinobi's mark. This also answers Holy Up's open question ("an
  enemy id both games agree on").

## 5. What a test class mod confirmed in game (November 2026)

A test class that borrows the Empress's moves, with its own art and a code mod, checked these:

- **Body hit boxes are sprite data, per frame picture** (*confirmed in game*). The table at
  header +0x28 of a body `.osb` lists, per frame mesh, attack boxes then hurt boxes (5 int32 each:
  center x, center y up from the feet, width, height, kind; kind 0 = body, 10/11 on the
  Empress's sword strike). The game's collision uses the boxes of the picture being shown, so a
  class with its own body file gets its own hit boxes. Build mod: `_hitboxes.txt` (FORMAT.md).
  Shots are different: their box is a code constant (section 1.5).
- **Some hits are code, tied to an animation** (*confirmed in game*): the Empress's air sword
  strike deals its damage whenever body animation 75 plays, through the general collision chain
  (0xE8ACE0 → 0xCC16E0 → 0xCC3970 → 0xCC3E40 → hit handler 0xCC6850 → 0xCC4560), using that
  animation's frame boxes. Removing the frame's attack box removes the hit.
- **Per-class shot pictures** (*confirmed in game*): see section 3; the effect file is preloaded
  with the class through a hook on 0xD62D30.
- **The fireball's look has three parts** (*confirmed in game*): animation 炎 (52 frames, pictures
  5–9 then a loop of 9–16), a node showing another animation of the same file (index 72, a big
  orange glow at 30 % opacity) and a light node colored by track channels 10–12 (red/green/blue,
  0..1; Build mod `_tracks.txt`). The fiery burst on hit comes from the attack entry's element
  (+0x04), not from the picture.
- **The shot mover** is 0xD36550 (state block at stack+0x10: +0x14 life, +0x18 fan time, +0x1C
  vx, +0x20 vy; vy only applies while the fan time is above 0). Changing vx/vy each frame steers
  a shot (*confirmed in game*: homing shots).
- **Enemies are actor type 1**; every machine agrees on an enemy's network id (+0x4C8/+0x4CC).
  Looking one up: `0x7C9770()` → `0xD83C00` → map at +0x70 → `0xA56E50(map, &node, id)`, actor
  at node+0x18 (node == map head means gone). Alive = vtable+0x7C returns false. Lockable =
  +0x3DC bit 4 and not +0x654 bit 6. Aim point = vtable+0x1AC. Position = actor +0x1C8/+0x1CC.
  (*Confirmed in game* solo; use online is *program only*.)
- **The running move:** player +0x330 names the body coroutine, +0x334 the attack layer's. Her
  stand-shot move sets +0x334 to the air shot's name hash and waits until it is cleared; the fall
  plays its animation only when +0x334 is 0. A mod that kills a move group must clear +0x334 too,
  or the player gets stuck (*confirmed in game*).
- **Movement curves** are a `std::multimap<int slot, curve>` at the move block +0x12C (node +0x10
  slot, +0x14/+0x18 values, +0x1C time, +0x20 length, FLT_MAX = endless); 0x9DC530 equal_range,
  0x7A3B50 ++it, 0xE51A50 erase (*program only*, used by a test mod). The shared dash
  `coActionDash@cPlWork` (0xD6C410) has no hit.
- **Playing an animation** is vtable +0x24C (0xD68A40, thiscall(player, hash, flag)); it also
  plays the same name on her floating weapon (+0x698) through vtable +0x254 (0xB62390). An
  animation's hash is the hash of its name, so mods can use fixed values (70 = 0x895884CF,
  75 = 0x8B15A7AD, 116 = 0xDD66AF2D, 9 = 0x1317530D).

## 6. Tools for this research

- A disassembly from `dumpbin /disasm` (Visual Studio 2019 Build Tools) is the quickest way to
  find every write to a field (search for `+00001098h]`).
- Coroutine bodies can be named from the `IceCoLocalImpl` vtable each body writes; start sites are
  `push body; call 0x7B79B0`.
- Name hashes (`Pl00ShotInfo`, `Shot`, animation names) are filled by start-up functions at
  0x6C…–0x6E… that load a string into edx. Strings are UTF-8.
- Logging code mods (MinHook, like Holy Up) answer what static reading can't: which path a hit
  takes, which frame boxes are used, and so on.
