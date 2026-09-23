"""Installing the offline DPS meter into the game, and taking it back out.

pd2dpsmeter.dll is loaded by the game itself, by adding it to Game.exe's import table, from a
backup that puts the original back byte for byte. See game_patcher for why that is the only
moment a plugin can get in, and for why installing this alongside the sibling Holy Grail plugin
is safe.
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
