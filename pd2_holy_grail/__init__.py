"""Holy Grail probe: a read-only look inside the running game.

 is loaded by the game and writes a log; it hooks nothing and changes nothing.
This package installs it the same way its sibling project installs its own plugin — by adding the
DLL to Game.exe's import table, from a backup that puts the original back byte for byte.
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
