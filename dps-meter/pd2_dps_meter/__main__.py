"""`python -m pd2_dps_meter` — look at the install, turn it on, turn it off.

Every command reads the real files rather than remembering what it did last time, so a game the
launcher has since replaced, or a Game.exe somebody restored by hand, is reported as it actually
is. Nothing is written while Diablo II is running.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import game_patcher
from .game_patcher import GamePatchError
from .safety_pipeline import is_game_running


def _resolve(raw: str | None) -> Path:
    """The Game.exe to act on: given directly, or found from a save folder beside it."""
    if raw:
        given = Path(raw).expanduser()
        if given.is_file() and given.name.lower() == "game.exe":
            return given
        if given.is_dir():
            found = game_patcher.find_game_exe(str(given / "Save"))
            if found:
                return found
            candidate = given / "Game.exe"
            if candidate.is_file():
                return candidate
        raise GamePatchError(f"No Game.exe at {given}")
    raise GamePatchError("Say where the game is: --game <path to Game.exe or the install folder>")


def _report(game_exe: Path) -> int:
    state = game_patcher.status(game_exe)
    print(f"Game.exe   {game_exe}")
    print(f"version    {state.game_version or 'unknown'}"
          f"{'' if state.supported else f'  (this plugin is built for {game_patcher.SUPPORTED_GAME_VERSION})'}")
    print(f"installed  {'yes' if state.installed else 'no'}")
    print(f"backup     {'present' if state.backup_present else 'none'}")
    if game_patcher.launcher_restores_game_exe(game_exe):
        print("note       PD2Launcher's file check is on and puts its own Game.exe back on every"
              " launch — turn it off in the launcher, or this will be undone.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pd2-dps-meter", description=__doc__)
    parser.add_argument("command", choices=("status", "install", "uninstall"))
    parser.add_argument("--game", help="Game.exe, or the folder holding it")
    args = parser.parse_args(argv)

    try:
        game_exe = _resolve(args.game)
        if args.command == "status":
            return _report(game_exe)
        # Both halves of an install are edits to files the game holds open while it runs, and a
        # half-written import table is not something a player should have to recover from. An
        # inability to tell counts as running, never as clear.
        if is_game_running() is not False:
            raise GamePatchError("Diablo II is running (or cannot be checked) — close it first.")
        if args.command == "install":
            plugin = Path(__file__).resolve().parent.parent / game_patcher.PLUGIN_DLL_NAME
            game_patcher.enable(game_exe, plugin)
            print("Installed.")
        else:
            game_patcher.disable(game_exe)
            print("Removed, and Game.exe put back from its backup.")
        return _report(game_exe)
    except GamePatchError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
