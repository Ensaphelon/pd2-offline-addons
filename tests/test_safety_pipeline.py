"""The installer must never patch Game.exe while the game holds it open."""
from __future__ import annotations

from pd2_offline_addons import safety_pipeline


class _Completed:
    def __init__(self, stdout: str, returncode: int = 0) -> None:
        self.stdout = stdout
        self.returncode = returncode


def test_windows_reads_the_task_list(monkeypatch) -> None:
    """`tasklist` names images, not command lines, so the match is on the image alone."""
    monkeypatch.setattr(safety_pipeline.os, "name", "nt")
    monkeypatch.setattr(
        safety_pipeline.subprocess, "run",
        lambda *a, **k: _Completed('"Game.exe","4812","Console","1","412,904 K"\n'),
    )
    assert safety_pipeline.is_game_running() is True


def test_windows_with_the_game_closed(monkeypatch) -> None:
    monkeypatch.setattr(safety_pipeline.os, "name", "nt")
    monkeypatch.setattr(
        safety_pipeline.subprocess, "run",
        lambda *a, **k: _Completed('"explorer.exe","1234","Console","1","40,000 K"\n'),
    )
    assert safety_pipeline.is_game_running() is False


def test_a_probe_that_cannot_run_is_never_read_as_clear(monkeypatch) -> None:
    """The failure this guards: for as long as this module had only the POSIX probe, Windows got
    OSError from the missing `pgrep`, answered None, and __main__ — which accepts only a plain
    False — refused every install while blaming the game."""
    monkeypatch.setattr(safety_pipeline.os, "name", "nt")

    def explode(*_args, **_kwargs):
        raise OSError("tasklist is not on this machine")

    monkeypatch.setattr(safety_pipeline.subprocess, "run", explode)
    assert safety_pipeline.is_game_running() is None
    assert (safety_pipeline.is_game_running() is not False) is True  # the installer's own guard


def test_posix_ignores_a_command_that_merely_mentions_the_game(monkeypatch) -> None:
    monkeypatch.setattr(safety_pipeline.os, "name", "posix")
    monkeypatch.setattr(
        safety_pipeline.subprocess, "run",
        lambda *a, **k: _Completed("501 /bin/cp /a/Game.exe /b/Game.exe\n"),
    )
    assert safety_pipeline.is_game_running() is False


def test_posix_sees_the_game_itself(monkeypatch) -> None:
    monkeypatch.setattr(safety_pipeline.os, "name", "posix")
    monkeypatch.setattr(
        safety_pipeline.subprocess, "run",
        lambda *a, **k: _Completed("501 C:\\Program Files (x86)\\Diablo II\\Game.exe -3dfx\n"),
    )
    assert safety_pipeline.is_game_running() is True
