"""Quick Restart's patch to Game.exe, and putting it back.

Every test runs against a COPY of the real Game.exe — the patch has to be exercised on the
actual binary (a synthetic PE would not prove the import directory was rebuilt in a way the
Windows loader accepts), and the real one is never written to.
"""
from pathlib import Path

import pytest
from pd2_offline_addons import game_patcher
from pd2_offline_addons.safety_pipeline import _line_is_the_game

REAL_GAME_EXE = Path(
    "/Users/ramil/Library/Application Support/CrossOver/Bottles/Diablo II/drive_c/"
    "Program Files (x86)/Diablo II/ProjectD2/Game.exe"
)


@pytest.fixture
def game(tmp_path: Path) -> Path:
    """A throwaway copy of the real 1.13c Game.exe, with the plugin beside it."""
    if not REAL_GAME_EXE.is_file():
        pytest.skip("no Project Diablo 2 install on this machine")
    # The live install may have the feature switched on right now, in which case its Game.exe is
    # already patched and would make "enable changes the file" vacuously false. The backup this
    # code itself leaves is the pristine original, so that is the better source when it exists.
    source = REAL_GAME_EXE
    backup = REAL_GAME_EXE.with_suffix(REAL_GAME_EXE.suffix + game_patcher.BACKUP_SUFFIX)
    if game_patcher.status(REAL_GAME_EXE).installed:
        if not backup.is_file():
            pytest.skip("the installed Game.exe is patched and there is no original to copy")
        source = backup

    copy = tmp_path / "Game.exe"
    copy.write_bytes(source.read_bytes())
    assert not game_patcher.status(copy).installed
    return copy


def _imported_dlls(path: Path) -> list[str]:
    return game_patcher._PE(bytearray(path.read_bytes())).imported_dlls()


def test_enabling_adds_the_plugin_to_the_import_table(game: Path) -> None:
    """The whole feature rests on this: the loader pulls the plugin in at process creation
    because Game.exe says it needs it. Injecting into a game that is already up does not work
    (Game.exe refuses OpenProcess), so the import is the only way in."""
    before = _imported_dlls(game)
    assert game_patcher.PLUGIN_DLL_NAME not in before

    status = game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)

    assert status.installed
    after = _imported_dlls(game)
    # Every original import survives, in order, with ours appended — nothing is rewritten.
    assert after == [*before, game_patcher.PLUGIN_DLL_NAME]
    assert (game.parent / game_patcher.PLUGIN_DLL_NAME).is_file()


def test_disabling_restores_the_original_byte_for_byte(game: Path) -> None:
    """The patch is only acceptable because it is exactly reversible: this is somebody's game."""
    original = game.read_bytes()

    game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)
    assert game.read_bytes() != original

    status = game_patcher.disable(game)

    assert not status.installed
    assert game.read_bytes() == original
    assert not (game.parent / game_patcher.PLUGIN_DLL_NAME).exists()
    assert not status.backup_present


def test_enabling_twice_keeps_the_original_backup(game: Path) -> None:
    """A second enable must not overwrite the backup with an already-patched file — that would
    leave nothing to go back to."""
    original = game.read_bytes()

    game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)
    game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)

    backup = game.with_suffix(game.suffix + game_patcher.BACKUP_SUFFIX)
    assert backup.read_bytes() == original
    assert _imported_dlls(game).count(game_patcher.PLUGIN_DLL_NAME) == 1


def test_status_reads_the_file_rather_than_remembering(game: Path) -> None:
    """A Project Diablo 2 update replaces Game.exe and silently takes the patch with it (seen
    2026-09-19: the patched file came back as the pristine original). Anything that remembered
    "installed" would go on claiming the feature is there."""
    original = game.read_bytes()
    game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)
    assert game_patcher.status(game).installed

    game.write_bytes(original)  # what the launcher does

    assert not game_patcher.status(game).installed


def test_an_unsupported_game_version_is_refused(game: Path, tmp_path: Path) -> None:
    """The plugin's addresses are 1.13c's. 1.13d moved almost all of them, so patching it in
    would not fail politely — it would write to addresses that mean something else."""
    data = bytearray(game.read_bytes())
    at = data.find(bytes.fromhex("BD04EFFE"))
    assert at > 0, "no version resource to corrupt"
    data[at + 12:at + 16] = (99).to_bytes(4, "little")  # a different FileVersion
    game.write_bytes(bytes(data))

    with pytest.raises(game_patcher.GamePatchError):
        game_patcher.enable(game, game_patcher.PLUGIN_SOURCE)


@pytest.mark.parametrize(
    ("line", "is_game"),
    [
        (r"27187 C:\Program Files (x86)\Diablo II\ProjectD2\Game.exe -3dfx -skiptobnet", True),
        (r"789 C:\Diablo II\d2se.exe", True),
        # Real cause of a real bug: the toggle refused to move, insisting the game was open,
        # while the only match was a command carrying the path as an argument.
        (r"123 shasum -a 256 /path/Diablo II/ProjectD2/Game.exe", False),
        (r"456 /bin/cp /a/Game.exe /b/Game.exe", False),
        (r"999 python -m pd2_offline_addons", False),
    ],
)
def test_only_the_game_itself_counts_as_the_game_running(line: str, is_game: bool) -> None:
    assert _line_is_the_game(line) is is_game
