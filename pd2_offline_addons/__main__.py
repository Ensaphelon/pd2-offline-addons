"""`python -m pd2_offline_addons` — look at the install, turn it on, turn it off.

  status      what the files on disk say, which is the only thing asked
  install     the plugin into Game.exe's imports, and the RESTART label into an archive
  uninstall   all of it, Game.exe back from its backup
  recolour    a unique item drawn in another of the game's own colours
  uncolour    the data archive back from its backup
  affixes     affix names and their roll ranges into the loot filter's tooltips
  unaffixes   that block back out of the filter


Every command reads the real files rather than remembering what it did last time, so a game the
launcher has since replaced, or a Game.exe somebody restored by hand, is reported as it actually
is. Nothing is written while Diablo II is running.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import affix_labels, game_patcher, item_colour, mpq_asset
from .affix_labels import AffixLabelError
from .game_patcher import GamePatchError
from .item_colour import ItemColourError
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


def _parse_colours(given: list[str] | None) -> dict[str, str] | None:
    """`--colour Toothrow=dgry`, repeatable. Nothing given means this project's own choices."""
    if not given:
        return None
    wanted = {}
    for pair in given:
        name, separator, code = pair.partition("=")
        if not separator or not name.strip() or not code.strip():
            raise ItemColourError(f"--colour wants NAME=CODE, not {pair!r}")
        wanted[name.strip()] = code.strip()
    return wanted


def _report_colours(game_exe: Path, wanted: dict[str, str] | None) -> None:
    """Each recoloured item as the archive actually has it, beside what PD2 ships."""
    try:
        state = item_colour.status(game_exe.parent, wanted)
    except (ItemColourError, mpq_asset.MpqError, OSError) as error:
        print(f"colours    cannot be read ({error})")
        return
    if state.archive is None:
        return
    for name in sorted(state.now):
        now, stock = state.now[name] or "none", state.stock[name] or "none"
        same = " (as PD2 ships it)" if now == stock else f"  was {stock}"
        print(f"colour     {name}: {now}{same}")


def _filter_file(game_exe: Path, given: str | None) -> Path:
    """The filter to act on: the one named, else the one PD2Launcher has selected."""
    if given:
        return Path(given).expanduser()
    return affix_labels.selected_filter(game_exe.parent)


def _report_affixes(game_exe: Path, given: str | None) -> None:
    target = _filter_file(game_exe, given)
    try:
        state = affix_labels.status(target)
    except (AffixLabelError, OSError) as error:
        print(f"affixes    cannot be read ({error})")
        return
    where = target.name if target.parent.name != "local" else f"local/{target.name}"
    if state.filter_path is None:
        print(f"affixes    no filter at {target}")
    elif state.installed:
        print(f"affixes    {state.rules} rules in {where}")
    else:
        print(f"affixes    not in {where}")


def _report(game_exe: Path, wanted: dict[str, str] | None = None,
            filter_path: str | None = None) -> int:
    state = game_patcher.status(game_exe)
    print(f"Game.exe   {game_exe}")
    print(f"version    {state.game_version or 'unknown'}"
          f"{'' if state.supported else f'  (this plugin is built for {game_patcher.SUPPORTED_GAME_VERSION})'}")
    print(f"installed  {'yes' if state.installed else 'no'}")
    print(f"backup     {'present' if state.backup_present else 'none'}")
    _report_colours(game_exe, wanted)
    _report_affixes(game_exe, filter_path)
    if game_patcher.launcher_restores_game_exe(game_exe):
        print("note       PD2Launcher's file check is on and puts its own Game.exe back on every"
              " launch — turn it off in the launcher, or this will be undone.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pd2-offline-addons", description=__doc__)
    parser.add_argument("command",
                        choices=("status", "install", "uninstall", "recolour", "uncolour",
                                 "affixes", "unaffixes"))
    parser.add_argument("--game", help="Game.exe, or the folder holding it")
    parser.add_argument("--colour", action="append", metavar="NAME=CODE",
                        help="for recolour: a unique item and the Colors.txt code to draw it in,"
                             " e.g. Toothrow=dgry. Repeatable. Omitted means this project's own"
                             " choices.")
    parser.add_argument("--filter", metavar="PATH",
                        help="the .filter to write affix labels into. Omitted means the one"
                             " PD2Launcher has selected, which is the local file when a local"
                             " filter is chosen and loot.filter otherwise.")
    args = parser.parse_args(argv)

    try:
        game_exe = _resolve(args.game)
        wanted = _parse_colours(args.colour)
        if args.command == "status":
            return _report(game_exe, wanted, args.filter)
        # Every write here lands on a file the game holds open while it runs — Game.exe, or an
        # archive it reads its tables out of — and a half-written one is not something a player
        # should have to recover from. An inability to tell counts as running, never as clear.
        if is_game_running() is not False:
            raise GamePatchError("Diablo II is running (or cannot be checked) — close it first.")
        if args.command == "install":
            plugin = Path(__file__).resolve().parent.parent / game_patcher.PLUGIN_DLL_NAME
            game_patcher.enable(game_exe, plugin)
            print("Installed.")
        elif args.command == "recolour":
            item_colour.enable(game_exe.parent, wanted)
            print("Recoloured.")
        elif args.command == "affixes":
            install_dir = game_exe.parent
            target = _filter_file(game_exe, args.filter)
            state = affix_labels.enable(target, install_dir,
                                        install_dir / item_colour.ARCHIVE_NAME)
            print(f"Affix labels in {target.name}: {state.rules} rules.")
        elif args.command == "unaffixes":
            target = _filter_file(game_exe, args.filter)
            removed = affix_labels.disable(target)
            print(f"{target.name} "
                  f"{'no longer carries the block.' if removed else 'did not carry it.'}")
        elif args.command == "uncolour":
            restored = item_colour.disable(game_exe.parent)
            print(f"{item_colour.ARCHIVE_NAME} "
                  f"{'put back from its backup.' if restored else 'was not changed.'}")
        else:
            game_patcher.disable(game_exe)
            # A full uninstall leaves nothing of ours behind, including in the data archive —
            # otherwise "uninstall" would quietly keep one feature installed.
            item_colour.disable(game_exe.parent)
            affix_labels.disable(_filter_file(game_exe, args.filter))
            print("Removed, and Game.exe put back from its backup.")
        return _report(game_exe, wanted, args.filter)
    except (GamePatchError, ItemColourError, AffixLabelError,
            mpq_asset.MpqError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
