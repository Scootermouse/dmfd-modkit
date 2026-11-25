Dragon Marked for Death - Mod Kit {version}
==========================================

An unofficial, fan-made mod kit: a mod loader, a Mod Manager and four
mods, without changing any of the game's own files.

WORK IN PROGRESS: 0.9 is a pre-release. Expect rough edges, and please
report problems: https://github.com/Scootermouse/dmfd-modkit/issues

- Rebalance: damage, enemy and boss HP, EXP, gold and rare-drop
  multipliers, per-class damage. Online, the host's settings are used.
- Quick Spells: auto-finish and spell hotkeys for the Witch and the
  Oracle (solo only).
- Skip Quit Confirmation: quitting from the menu skips the "are you
  sure?" box (can be switched off in the in-game menu).
- Holy Up: the Empress's Holy shots mark enemies; each stack lowers their
  PIE and raises the Holy damage they take from the whole party.

Each mod can be switched on and off in the Mod Manager, and its settings
are in the in-game menu (Insert).


INSTALL
-------
1. In Steam, right-click Dragon Marked For Death > Manage > Browse local
   files.
2. Copy everything in this zip into that folder (next to game.exe):
   winmm.dll, "DMFD Mod Manager.exe", mods.json, LICENSE.txt and the
   mods folder.
3. Start "DMFD Mod Manager.exe". It finds the game and your saves through
   Steam, prepares the mods (a few seconds) and has a Launch game button.
   Starting the game from Steam also works once the mods are prepared.

Windows or your antivirus may warn about an unknown program or DLL;
that's because the files aren't signed.


YOUR SAVES
----------
The manager backs up your save before each launch from it (Saves page;
it keeps the last 5). The six normal classes are saved as usual; extra
classes added by mods go into an extra file next to the game's save
(SystemData.bin.classes). Keep the two together.


UNINSTALL
---------
In the manager: Settings > Reset modding. The loader, the mods and the
prepared data move into the ModManager folder (nothing is deleted), so
the game is back to its own files. To remove everything, then delete the
ModManager folder, "DMFD Mod Manager.exe" and mods.json too. Or by hand:
delete winmm.dll, "DMFD Mod Manager.exe", mods.json and the mods,
DataMod and ModManager folders.


PLAYING ONLINE
--------------
Everyone in a room needs the same mods that matter online (Rebalance and
Holy Up here; Quick Spells and Skip Quit don't count). With them you see
other modded rooms, not vanilla ones. Joining a room whose mods differ is
stopped, and the game says what to install, disable or update; the Mod
Manager's Home page can then make a profile for that room.


MAKING MODS
-----------
Mod Manager > Build a mod > Get the modding tools. They come with their
own Python and their guide (docs\modding.md).


IF SOMETHING GOES WRONG
-----------------------
Close the game, then in the manager: Log > Report a problem. It collects
the logs into one zip (and opens its folder). The logs are rewritten each
time the game starts, so do this before launching again. Then open an
issue at https://github.com/Scootermouse/dmfd-modkit/issues , say what you
did and what happened, and attach the zip. The logs hold no saves, but
they do contain folder paths (which can include your Windows user name).


LICENSE AND SOURCE
------------------
The mod loader, the Mod Manager and the tools are free software under the
GNU GPL, version 3 or later (LICENSE.txt). The complete source is in the
DMFD-ModKit-{version}-source.zip released alongside this zip:
https://github.com/Scootermouse/dmfd-modkit/releases . Each mod
belongs to its maker; see its mod.ini.

Made by Scootermouse, with Claude (Anthropic's AI) writing the code.
Dragon Marked for Death is (c) Inti Creates; this kit is not made or
endorsed by them and contains none of the game's files.
