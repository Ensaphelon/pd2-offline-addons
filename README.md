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

## Status

Freshly merged. The three projects are still here as they were, side by side; unification is the
next step.
