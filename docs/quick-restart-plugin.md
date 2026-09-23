# Quick Restart plugin

A "Restart" line in the game's own ESC menu: it leaves the current game and comes straight back
in with the same character on the same difficulty, instead of walking the menus by hand.

`pd2restart.dll` is committed built: installing has to work on a machine with no Windows
cross-compiler. `build.sh` rebuilds it from
`src/` with mingw-w64 (`brew install mingw-w64`), and the result is a 32-bit DLL because Diablo
II 1.13c is a 32-bit process.

## How it gets into the game

`pd2_quick_restart/game_patcher.py` adds the DLL to `Game.exe`'s import table, so the loader pulls it in
at process creation. That is the only moment it can get in: injecting into a game that is
already running does not work here — every other process in the bottle's session opens fine,
`Game.exe` refuses `OpenProcess` outright (measured 2026-09-19).

## What it does once inside

* Waits for the interface DLLs, checks `Game.exe` is 1.13c, and stands down on anything else.
* Copies the three-entry ESC menu into its own array with a fourth entry appended, and points
  the game's own menu globals at the copy. The original array is never written to.
* On press: remembers the difficulty, calls the game's own "Save and Exit Game" handler, then
  walks the three screens that follow (main menu, character select, difficulty) by pressing
  their buttons through their own handlers.

Buttons are chosen by where they sit and what state they are in, never by their order in the
control list — order is an artefact of how a screen was built, and the difficulty popup's own
buttons carry a different state (13) from the screen underneath it (5), which is what makes
them tellable apart.

Everything is written to `pd2restart.log` next to the DLL.
