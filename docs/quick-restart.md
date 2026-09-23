# pd2-quick-restart

A **Restart** line in Project Diablo 2's own ESC menu. It leaves the current game and comes
straight back in with the same character on the same difficulty, instead of walking the menus by
hand.

```
Options
Save and Exit Game
Restart            <- this
Return to Game
```

Not last, and that is not a preference: pressing Escape with the menu open activates the LAST
entry, which is how "Return to Game" doubles as the close key. With Restart there, a second
Escape restarted the game.

## Use it

```bash
python -m pd2_quick_restart status    --game "/path/to/Diablo II/ProjectD2"
python -m pd2_quick_restart install   --game "/path/to/Diablo II/ProjectD2"
python -m pd2_quick_restart uninstall --game "/path/to/Diablo II/ProjectD2"
```

`--game` takes either `Game.exe` itself or the folder holding it. Close Diablo II first — every
command refuses to write while it is running.

Installing touches three files, and each one is reversible:

| file | what happens | how it comes back |
|---|---|---|
| `Game.exe` | the DLL is added to the import table | restored from `Game.exe.pd2restart-backup`, byte for byte |
| `pd2assets.mpq` | the RESTART label graphic is appended | restored from its own backup |
| `glide3x.dll` (D2GL) | one HD-text entry is renamed to RESTART | restored from its own backup |

**PD2Launcher's file check puts its own `Game.exe` back on every launch.** `status` says so when
it is on; turn it off in the launcher, or the install is undone the next time you start the game.

Only Game.exe **1.0.13.60** is supported. On anything else the tools refuse rather than write to
addresses that have moved.

## Build the plugin

```bash
./build.sh          # needs mingw-w64: brew install mingw-w64
```

Diablo II 1.13c is a 32-bit process, so the result is an i686 DLL. The built `pd2restart.dll` is
committed so installing needs no cross-compiler.

## Layout

```
src/                 the plugin's C sources
assets/              the RESTART label (DC6) and the generator that made it
pd2_quick_restart/   the installer: import-table patch, MPQ injection, D2GL text
tests/               run against copies, never a live install
PLUGIN.md            how the plugin works inside the game, and why it is built that way
```

## History

Built inside [pd2-holy-inventory](https://github.com/Ensaphelon/pd2-holy-inventory) and moved
here once it stood on its own.
