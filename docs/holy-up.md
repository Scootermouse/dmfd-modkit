# Holy Up: how the example code mod works

Holy Up (`codemods/HolyUp`) is the kit's example of a code mod that rebalances a vanilla class.
The Empress's Holy shots mark enemies with stacks. Each stack lowers the enemy's PIE (shown with
the game's own red number and blue PIE-down badge) and raises the Holy damage the enemy takes
from the whole party. The chance to add a stack grows with her PIE. Every number is a slider in
the in-game menu (Insert), saved in `mods\HolyUp\holyup.ini`.

**How sure each part is.** *Confirmed in game* = seen working in the game (solo).
*Program only* = read from the disassembly, not checked. Addresses are in the unpacked exe at
base 0x400000.

## What it builds on

### Damage paths (*confirmed in game*)
- The hit handler is 0xCC6850. Its dispatcher 0xCC4560 picks the damage path from the attack
  entry and sets a flag in the result (+0x34): **1 physical** (0xCC4620), **2 magical**
  (0xCC4E20), **4 Holy** (0xCC55E0). All three are `__thiscall` on the target with five stack
  arguments (hit, a2, a3, a4, result), `ret 0x14`; the damage is added at result+0x24.
- **The Empress's fireballs and the Dragon Cannon barrage are Holy damage**, not magic. They come
  from a shot object (vtable 0x0110E314, owner player at +0x668). Attack entry 0 = fireball,
  entry 1 = the charged barrage. Her dragon-arm swings are physical.
- The attacker of a hit is `0x7B1810(hit)` (thiscall); the attack entry index is
  `attacker vtable+0x68(hit+0x38)`; the attack table is at attacker+0x36C (entries at +0x60,
  36 bytes each, count at +0x68). See `docs/moves.md` 1.5 for the entry layout.

### Stats (*confirmed in game*)
- An actor's status is `0x7B3150(*(obj+0x39C))`; its vtable+0x24 returns six 16-bit words
  **STR AGI INT VIT LUC PIE** (PIE = word 5, offset 0xA). Checked against the Empress's
  status screen.

### Holy hit chance (*confirmed in game*, from the game's own table)
- 0xCB7D00 looks up `table[clamp(attacker PIE − target PIE, ±25)]` at 0x28987E2 (filled at
  start-up). The value is the chance out of 512 that a roll misses: −25: 496 … −10: 368,
  −5: 288, 0: 208, +5: 128, +7: 96, then −2 per point (+10: 90, +25: 60).
- A Holy hit rolls up to 3 times (0xCB7E30); each miss halves the damage (×1, ½, ¼ or ⅛).
- Expected damage by PIE difference: −5 → 0.62×, 0 → 0.71×, +5 → 0.86×, +10 → 0.90×. So lowering
  a boss's PIE is worth real damage while it out-PIEs you; above +7 little more is gained.

### Stat-down effects and the enemy panel (*confirmed in game*)
- Writing an enemy's PIE word directly lasts about 2 seconds: the block is rebuilt.
- The game's own `cAppend_StatusDn{Rate,Fix}` effects work through a per-frame work area. Each frame
  the enemy's status (`cHitControlChara`, vtable 0x10FA170) runs its update (vtable +0x14,
  0xCBC620, `(arg, float dt)`, `ret 8`), whose effect manager ([status+0x78]) fills the area at
  manager+0x80:
  - +0x14..+0x1E fixed stat-down per stat (STR..PIE), +0x2C..+0x36 rate stat-down;
  - +0x9C flags: rate-down stat i = bit i, fixed-down stat i = bit 7+i (PIE fixed = 0x1000);
  - +0x124 one float timer per flag bit, counted in **frames** (the badge flashes under 180).
- Then the refresh (vtable +0x10, 0xCBBE10) computes the stats from it (0xCBFA60 on status+0x70),
  copies the flag words (+0x98/+0x9C/+0xA0) to status+0x70, and clears the area (0xCB7150).
- The enemy panel's buff/debuff icons (`cHudWork01`, coroutine body 0xCD6F90) are listed by
  0xCBC0C0 from the same work area, before the refresh clears it. Icon tables are at 0x28E9AB0 /
  0x28E9B18 / 0x28E9B80 / 0x28E996C; the art is in `Win/cp_o0000.osb` (battle HUD).
- So a mod that adds its stat-down **after the update and before the refresh** gets the game's own
  look: the red PIE number and the blue PIE-down badge.

## How the mod does it

- MinHook on the three damage paths, the status update (0xCBC620) and the refresh (0xCBBE10).
- On an Empress Holy hit: a chance of (her PIE × `chance_per_pie`) % to add a stack (×
  `barrage_chance_mult` for the barrage), up to `max_stacks`, lasting `seconds` (default 30, like
  Burn), refreshed by each new stack.
- Each stack: the enemy's PIE goes down by `pie_down_per_stack` (through the work area above) and
  every Holy hit on it is raised by `bonus_per_stack` % (whoever lands it).
- Online it is `online=same`: everyone in a room needs it. Whether the stacks match on every
  machine is **not tested yet** (*program only*: hits seem to be worked out per player; enemies
  have a network id every machine agrees on, +0x4C8/+0x4CC, which can carry the stacks).

## Lessons for other code mods

- Don't trust what a move looks like: log which damage path it takes (the fireballs looked magic).
- Stats that look editable are often rebuilt every frame; find the game's own effect system.
- Timers in the game are usually frames, not seconds.
- Several mods may hook the same function; a hook must not assume it is the only one (the
  loader's own select-screen hook once missed calls because another mod hooked the same function
  first and called it from a different place).
