# Worked example: a custom weapon, start to finish

This walks through making one new sword for the Empress: new art, its own item, where it sits on
each frame, its timing, how far its swings reach, and its slash effect. Then it covers the extra
step for a custom class with its own body. Every step uses the kit's tools; the details of each
file are in [modding.md](modding.md).

Commands are run from the kit folder. `tools\py.bat` is the kit's own Python.

## 1. Get the original pictures

Double-click **Export weapon sprites.bat**. It writes every weapon's frames to
`mod\sprites_original\<weapon file>\`, and `mod\sprites_original\weapons.csv`, which lists the
items using each file. The Empress's starting sword (Shortsword) is `pl0_cw00`.

## 2. A new weapon file

Copy the **whole** folder `mod\sprites_original\pl0_cw00` to `mod\sprites\` and rename the copy,
e.g. `mod\sprites\mod_cw00` (letters, digits and `_` only). Keep the `_source.txt` inside: it says
the copy starts from `pl0_cw00`.

Redraw the frames you want. Each PNG is one picture; the `aNNN` in its name is the animation, the
`mNNN` the picture. To draw a quite different shape, not a paint-over of the old blade, add an empty
file named `_replace.txt` to the folder: each PNG then becomes a whole new picture.

## 3. Its item

In `mod\items.csv`, add a row (one per weapon):

| label | copy_from | name | atk | sprite | colour |
|---|---|---|---|---|---|
| `MOD_SWORD_` | `SWORD0000_` | My Sword | 30 | `mod_cw00` | 0 |

`copy_from` starts it from the Shortsword (its category, icon and stats); `sprite` points it at
your new file. Leave `sprite_palette` empty to keep the Shortsword's colours. Add `shop`, `price`,
stats and effects as you like (all columns: modding.md, items.csv).

## 4. Find the animations you want to change

The weapon file has its own copy of the Empress's animations, with the same names. List them:

```
tools\py.bat tools\anim_names.py pl0_cw00
tools\py.bat tools\anim_names.py pl0_cw00 剣_通常
```

The second line shows only her normal sword combo: hits 1, 2 and 3 (`剣_通常１段`, `２段`, `３段`),
each in four parts: wind-up (準備), slash (斬り), end (終了) and return (戻り). Her idle is `待機`
(`0x67d7af17`). The tool prints each one's number and **hash**; the hash is the same in her body,
every weapon file and her effect file, so it's the handy way to name an animation.

## 5. Where it sits on each frame

Make `mod\sprites\mod_cw00\_offsets.txt` (x forward as drawn facing right, y up, in pixels):

```
# a little higher while idle
0x67d7af17 0 6
# hit 2's slash: 10 px further forward
0x984c24b9 10 0
# only the second key of hit 1's slash
0xb33bd878 k1 0 -4
```

Build mod lists any picture that's also shown elsewhere (it moves there too).

## 6. Timing that follows the body

If you retime one of her animations (`mod\sprites\pl0_cb00\_timing.txt`, e.g. a slower wind-up),
her weapons don't follow by themselves. Add to `mod\sprites\mod_cw00\_timing.txt`:

```
follow pl0_cb00
```

Every plain retiming line of the body's `_timing.txt` is then applied to the weapon's animation
of the same name. The strike itself (the slash part of a swing) is timed by the game's code and
plays at full speed whatever its animation's length.

## 7. How far the swings reach

A swing's hits come from its slash effect (the white arc), not from the body or weapon pictures.
The Empress's slash effects are in `pl0_ef00`. Make `mod\sprites\pl0_ef00\_hitboxes.txt`:

```
# hit 2's slash: one long box from about 40 to 260 px ahead (centre 150, up 30, 220 x 80, kind 2)
0x984c24b9 attack 150 30 220 80 2
```

That changes hit 2 for the Empress whatever she holds (reach is per move, not per item). Several
boxes on one line replace a picture's own group of boxes, up to as many as it has.

## 8. The slash effect's look and timing

Its pictures can be redrawn like any sprite file (`mod\sprites\pl0_ef00\`). To shift it, delay
it or change its speed, install the **Effect Tweaks** code mod and edit
`mods\EffectTweaks\effects.txt` (modding.md, Effect Tweaks). Careful: a swing's slash effect has
the same animation name as the body's and the weapon's, so a line for that name changes all three.

## 9. Build and test

Double-click **Build mod.bat** (or `tools\py.bat tools\build_mod.py`), start the game, buy or give
yourself the item, and try it. `dmfd_content.log` in the game folder records problems.

## For a custom class with its own body

A class made with `new_class.py` (modding.md, Extra classes) has its own body, say `cleric_cb00`,
but shares the weapon and effect files of the class whose moves it uses. To place weapons for its
body, or change its reach, without changing that class, give it its own copies, **in the same mod**
as the class:

- `mod\sprites\cleric_pl0_cw00\`: a folder with a `_source.txt` containing `pl0_cw00`, plus your
  `_offsets.txt` / `_timing.txt` (and frames only if you redraw them). Used instead of `pl0_cw00`
  for every item showing it, when this class holds it. Make one per weapon file you change (for a
  new weapon file like `mod_cw00` above: `cleric_mod_cw00` isn't supported; place a new weapon for
  the class in the new file itself).
- `mod\sprites\cleric_ef00\`: a folder with a `_source.txt` containing `pl0_ef00`, plus your
  `_hitboxes.txt`. Used instead of the Empress's effect file for this class.

These are used during quests; menu pictures (status, inn) still show the original files.
