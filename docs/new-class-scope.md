# Adding a custom class: scope

> Planning notes kept for their findings. Since they were written, extra classes became working (up to 127, paged on character select; see README.md "Extra classes"), so some "not possible yet" lines are out of date.

Written 2026-11-04, after items, icons, chests and sprite editing were working. It's based on what the exe and data show; effort estimates are rough.

## What a class is made of

| Part | Where it lives | Status |
|---|---|---|
| Body art: 128 animations, about 580 frames (Empress) | `Win/plN_cb00.osb` | Decoded. Frames can be redrawn, including outside the old outline (new tiles; confirmed in-game). |
| Colours: 6 contract elements (dragon arm) × 8 highlight colours = 48 palettes | `Win/plN_pal.osb` | Editable now (swatch pictures in `mod\palettes`), including new palette files; confirmed in-game. Adding extra schemes isn't possible yet. |
| Weapon art | `Win/plN_cwXX.osb` (+ `_pal`, `_ex_pal`), named by the weapon item records | Editable now; new weapon sprite files work, picked per item (`sprite`, `colour` in items.csv; confirmed in-game). |
| Effects, dragon form | `Win/plN_efXX.osb`, `Win/plN0dc.osb` | Same format. |
| Portraits, class-select art, HUD icons | `com_pl_icon` (menu portraits), `cp_o0000` (HUD portraits), `cutin00` (dragon orb cut-in) | Portraits, cut-in and character-select art (`s_o0a00`) editable and confirmed in-game. A 7th select panel needs the screen's part layout and menu code (not decoded). |
| Names, descriptions, class-select text | `sys_plselect.tb2` (`PL04_INFO`…), `TalkerName.tb2` | Editable now (`text.csv`). |
| Weapons and items | built-in item table (equip mask bit per class; 16 bits, 6 used) | Editable now (`items.csv`); new weapons and icons work. |
| Voices: 8 voice sets per class (`VOICE_EMP_A`…`H`) | sound banks in DataHash | **Not decoded** (unknown codec). |
| Behaviour: moves, attacks, skills, dragon form, magic | **game code**: `cPlWork00`–`05`, each a C++ class with ~173 virtual methods and 25–73 action routines of its own (`coActionSlashX`, `coAttackJumpShot`, `coActionDragonBoost`…), plus ~50 shared ones and attack objects (`cPshWork`) | Not modified yet; the rebalance mod already hooks some combat code. |
| Being a class at all | engine type registry (`MyDTI@cPlWork0N`), class-number checks hard-coded to 0–5 (e.g. the availability switch at `0xC92540`), character creation with 6 slots, per-class save data | Not looked at in depth. |

## Three levels of "custom class"

### A. Reskin an existing class (recommended first goal)
Replace one class's look and identity, keeping its moves: new body, weapon and effect art; new palettes; new name, descriptions and dialogue name; new weapons with their own icons, stats and effects; rebalanced numbers (rebalance mod).

- **Needs:** "bigger sprite changes" (new tiles, so art isn't limited to the old outline); weapon-sprite and palette editing in Build mod; a look at portraits and class-select art.
- **Doesn't need:** game code beyond what exists.
- **Effort:** a few sessions of tooling. Then the art itself, which is the real bulk: about 580 frames for a full redraw. That's done in your art program, not by me.
- **Risk:** low. Players without the mod see the original class; saves stay valid.

### B. Reskin plus changed moves
A, plus changes to how some moves behave: damage, speed, projectiles, which animation plays. This is done with hooks on that class's action routines, like the rebalance mod's damage hooks, reusing the existing animations or redrawn ones.

- **Effort:** roughly one session per move to understand and change, depending on the move.
- **Risk:** medium. Online co-op with unmodded players may disagree about what a move does.

### C. A true seventh class
Everything in A, plus:
- A new class object written from scratch inside the mod: ~173 engine methods' worth of interface and 25–70 move routines, matching the engine's internal conventions without its source code.
- Registering it with the engine's type system.
- Teaching every place that assumes classes 0–5 about class 6: character creation (6 slots and its UI art), availability/unlock checks, save data, per-class tables (stat growth, spells), equip masks (bit 6 is free).
- Voices, which need the sound format decoded first.
- Deciding what happens in online co-op when other players don't have the mod. Likely a crash or desync, so it would need blocking or careful handling.

**Effort:** months of work, with real uncertainty, because each assumption about class 0–5 has to be found before it breaks something. **Risk:** high (saves, online).

**Update:** the user chose route C (a true seventh class). The detailed map of six-class limits is in [seventh-class-limits.md](seventh-class-limits.md).

## Recommendation

Aim for **A** as "the new class": a new character on an existing class's moveset. Add **B**-style move changes selectively. Revisit **C** only if A and B leave something essential missing.

Bigger sprite changes (new tiles, so redrawn frames can change shape) are done. Weapon sprites and palettes are done too, including new weapon sprite and palette files. Left for A: a look at portraits and class-select art; then it's mostly drawing.
