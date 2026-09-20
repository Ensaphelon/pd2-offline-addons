"""Quick Restart — a Restart line in Project Diablo 2's own ESC menu.

Two halves. `pd2restart.dll` is the plugin the game loads: it builds its own copy of the mod's
menu with one extra entry, hands that copy to the game's own register/draw/release passes, and on
a click leaves the game and comes straight back in with the same character on the same difficulty.
The rest of this package installs it: `game_patcher` adds the DLL to Game.exe's import table (and
takes it back out, byte for byte, from a backup), `mpq_asset` injects the RESTART label graphic
into the game's own MPQ archive so the entry has something to draw, and `d2gl_patcher` teaches
D2GL's HD text renderer the same word.
"""

from .game_patcher import (
    GamePatchError,
    PatchStatus,
    disable,
    enable,
    find_game_exe,
    launcher_restores_game_exe,
    status,
)

__all__ = [
    "GamePatchError",
    "PatchStatus",
    "disable",
    "enable",
    "find_game_exe",
    "launcher_restores_game_exe",
    "status",
]
