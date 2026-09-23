# pd2-offline-addons

Add-ons for Project Diablo 2 in offline single-player: one DLL, one patch to `Game.exe`, and each
feature turned on or off from the in-game pause menu.

Merged from three separate projects, whose histories are preserved here in full — the commit
messages carry the reverse engineering each feature rests on, which is the part that would be
expensive to rediscover:

| Feature | Was | What it does |
|---|---|---|
| Quick Restart | `pd2-quick-restart` | A `RESTART` line in the ESC menu that drops and remakes the game |
| Holy Grail beam | `pd2-holy-grail-tracker` | A shaft of light over a drop that would be new to the collection |
| Offline DPS meter | `pd2-dps-meter-offline` | Makes PD2's own DPS counter work without a realm server |

## Why one repository, and one DLL

Three reasons, in ascending order of how much they force the issue.

**The import-table patcher had been copied three times** and had already drifted to 479 / 382 /
385 lines. Each copy patches `Game.exe` the same way and every copy is a place the next fix has
to be remembered.

**The backups stacked.** Each installer kept its own backup of "the exe as it was before *I*
touched it", so removing the one installed first would restore a file that never knew about the
others and silently take them with it. A real installation reached
`61440 / 65536 / 69632 / 73728` bytes across three backups and the live file, with the pristine
original recoverable from exactly one of them.

**The ESC menu cannot have three owners.** The mechanism works by pointing the game's own menu
globals at a replacement array. Two plugins doing that independently means whichever ran last
wins and the other's entries vanish — with no error, because nothing failed. This one is not
untidiness; it is a resource that admits exactly one owner, and it is the reason the features
share a binary rather than merely a repository.

So: one patch, one backup, one DLL, one menu.

## Layout

```
beam.txt        the beam's default config — read from beside the DLL, re-read while the game runs
src/            one DLL: main.c drives menu/restart, the beam, and the DPS meter
pd2_offline_addons/   the installer — one patcher, one backup, one import entry
assets/         the ESC-menu label graphics, and the generator that sets them from D2's own font
docs/           each feature's own write-up, including the disassembly the offsets rest on
tools-beam/     the DCC->DC6 pipeline that produced the beam's artwork
```

`docs/` is worth keeping: the DPS meter's page carries the disassembly of PD2's own accumulator
and the gate that made it read zero offline, and the beam's carries what it took to get a real
game sprite drawn over a dropped item. Those are the expensive parts to rediscover.

## Building and installing

Needs `mingw-w64` (`brew install mingw-w64`) — the game is a 32-bit process, so the target is
i686 and that is not negotiable.

```sh
./build.sh
python3 -m pd2_offline_addons status    --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_offline_addons install   --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_offline_addons uninstall --game "<path to>/ProjectD2/Game.exe"
```

The game must not be running. `status` reads the real import table rather than remembering what
it did last time, because a PD2 update replaces `Game.exe` and silently takes the install with it.

## Status

Merged and building: one DLL, one patch, one backup. The development probes that used to answer
F1..F12 are off by default — those are the skill hotkeys, and a plugin that responds to one by
playing a sound and dumping memory is not something to ship by accident. Build with
`-DADDONS_DEV=1` to get them back.

Still to come: the per-feature on/off entries in the pause menu. Today Quick Restart owns the one
menu line there is; the mechanism already keeps its own array with room for twelve, so the work
is generalising it from one inserted entry to several — plus a label graphic per entry, which
`assets/make_restart_dc6.py` can now typeset from the game's own font.

One constraint that shapes that UI, discovered the hard way and worth repeating here: **Escape
activates the LAST entry in the menu** — that is how "Return to Game" doubles as the close key.
Anything placed last will fire on a second Escape.
