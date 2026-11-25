# Changelog

## 0.10 (2026-11-25): custom weapons

Everything a custom weapon needs, worked out and checked in the game. A step-by-step guide:
docs/weapon-example.md.

- **Where a weapon sits on each frame:** `_offsets.txt` in a weapon's (or any) sprite folder moves
  pictures: one picture, a whole animation, or one key of it.
- **Weapons in step with a retimed body:** `follow <body folder>` in a weapon's `_timing.txt` applies
  the body's retiming to the weapon's own copy of each animation.
- **Whole new weapon shapes:** `_replace.txt` works in weapon folders too.
- **Reach:** a swing's attack boxes are on its slash effect in the class's effect file
  (`plN_ef00`), so `_hitboxes.txt` there sets how far it reaches. `_hitboxes.txt` also sets hurt
  boxes (`hurt`), several boxes at once, and a whole animation at once.
- **Finding animations:** `tools\anim_names.py` lists a sprite file's animations with their names
  and hashes; animations can be named by hash (`0x...`) in `_offsets.txt`, `_timing.txt` and
  `_hitboxes.txt`.
- **Extra classes get their own weapon and effect files** by name (`<class>_pl0_cw00`,
  `<class>_ef00`), so a new body can place weapons and change reach without changing the class it
  borrows from.
- **Effect Tweaks 0.2.2** (new code mod for modders, on the mod list): shift, delay, speed up, start
  later in or swap any effect, from `effects.txt`.
- **Fixed:** at the inn, paging between an extra class and the class it borrows from showed the old
  body under the new colours.
- The modding tools zip now includes the name lists the tools read (`anim_names.txt`, `names.txt`).

## 0.9 (2026-11-23): first public release

The first version shared publicly. Earlier versions were private test builds; their history is
summarised below.

- **Saves:** a slot that held an extra class when the game started could later be restored from an
  empty copy, giving an Empress with a blank name and level 0. The loader now starts that class as
  a new character instead, or leaves the slot as it is. The extra-class save file also keeps a
  character's data when a save is written before its contract is read (it could drop it), and
  stops reading a damaged file instead of misreading the rest.
- **Holy Up 0.14:** research logging removed; it now hooks only the Holy damage path.
- **Modding tools:** they come with their own Python (Mod Manager → Build a mod → Get the modding
  tools) and find the game through Steam (or `DMFD_GAME`, or `tools\game.txt`).
- New in the tools since the test builds: `new_class.py` (start a class mod in one step),
  `anim_viewer.py` (play every animation of a body), `build_mod.py --mod <folder>`, and per-body
  `_timing.txt`, `_hitboxes.txt`, `_tracks.txt` and `_replace.txt` (see docs/modding.md).
- Documentation: FORMAT.md brought up to date (animation editing, hit boxes, how each finding was
  checked); new docs/modding.md, docs/moves.md and docs/holy-up.md.
- Versions: the loader and the Mod Manager share the kit's version.

## Test builds (November 2026, not public)

- **0.8**: the mod loader runs separate code mods (Rebalance, Quick Spells, Skip Quit Confirmation)
  with one in-game menu; the Mod Manager (install, profiles, save backups, combining data mods
  without Python, a Browse page for a mod list); online rooms are matched by mods, and joining a
  room with different mods is stopped with a message saying what to install or disable.
- **0.5 – 0.7**: extra classes on pages of character select, at the inn and in the bar, each with
  its own pictures, name and saved characters (`SystemData.bin.classes`, keyed by class id).
- **0.2 – 0.4**: item, chest, icon, palette, sprite and screen-layout editing through Build mod.
- **0.1**: the data overlay (`DataMod`) and the first data tools.
