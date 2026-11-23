# The mod list: how the Mod Manager finds mods (and the tools)

The Mod Manager's **Browse** page, its **Get the modding tools** button (Build a mod page) and the
**Get these mods** button (when an online room needs mods you don't have) all read one file:
**`mods.json`**, the mod list. It names each mod, its versions, where to download each version,
and the size and SHA-256 checksum of each download. The Manager checks every download against the
list before anything reaches the game, so a damaged or swapped file is refused.

The format is described at the top of `manager/src/Browse.cs`. `tools/make_release.py` writes it.

## Where this is going: a list fetched from GitHub

The plan is a single, curated list kept in this repository:

- The Manager fetches the list from GitHub each time it opens the Browse page (a web address set
  in Settings › Mod list), and keeps a copy so the page still works offline.
- Mods by anyone can be added to it. A submission is checked automatically (it builds, its
  checksums match, no game files are inside) and reviewed by a person; code mods always get a
  person's approval, because a code mod runs inside the game like any program.
- New mods and updates then appear in everyone's Manager without a new kit release, and online
  rooms can offer "Get these mods" for anything on the list.

None of this is in place yet: there is no submission process and no list on GitHub for the
Manager to fetch.

## For now (0.9): a fixed list in the kit

Until then, each kit release carries its own list:

- The player zip has a `mods.json` next to `DMFD Mod Manager.exe`. When no other list is set in
  Settings, the Manager reads that one.
- Its download links point at the files attached to that release on this repository's
  Releases page (`https://github.com/Scootermouse/dmfd-modkit/releases/download/v0.9/...`): the
  four mods and the modding tools, each with its checksum.
- So Browse shows exactly the mods of that release, and Get the modding tools downloads the tools
  zip of that release. Newer mods or versions only show up with a newer kit (or when you point
  Settings › Mod list at a newer `mods.json`).

To test another list, set it in Settings › Mod list (a file or an `https://` address), or start
the Manager with `--mod-list <file or address>`.
