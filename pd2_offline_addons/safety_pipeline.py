"""Is Diablo II running right now?

Lifted out of pd2-holy-inventory's own write-safety pipeline, where it earned every line of its
comments: patching Game.exe while the game holds it open is how a patch gets half-written, and
a careless `pgrep` pattern is how a toggle refuses to move because some unrelated command
happens to carry the game's path as an argument.
"""

from __future__ import annotations

import subprocess

# Deliberately NOT a generic substring like "Diablo II" or "PlugY": both appear inside our own
# subprocess's --mpq/--save-dir arguments (the CrossOver bottle is itself named "Diablo II"), so a
# broad pattern self-matches cain_write_worker's own process under `pgrep -f` (full command line) —
# this is exactly what caused a real Sort run to falsely refuse mid-plan and needed a save restore.
# Verified this session: the real game executable under CrossOver/Wine is "Game.exe" — match only
# that, plus d2se (the loader some users run instead) — and never anything that only appears in a
# path our own code passes around.
_GAME_PROCESS_PATTERNS = ("Game.exe", "d2se.exe")
# Extra safety net regardless of pattern precision: never let a match against our own script's
# argv (which necessarily contains the same save/mpq paths) count as "the game is running".
_SELF_PROCESS_MARKERS = ("pd2_offline_addons",)


def is_game_running() -> bool | None:
    """True/False if determinable, None if this platform has no way to check (caller must then
    treat the precondition as unsatisfied — never assume "not running" from an inability to check)."""
    try:
        result = subprocess.run(
            ["pgrep", "-fli", "|".join(_GAME_PROCESS_PATTERNS)],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return False
    matches = [line for line in result.stdout.splitlines() if _line_is_the_game(line)]
    return len(matches) > 0


def _line_is_the_game(line: str) -> bool:
    r"""Whether a pgrep line is the game ITSELF, rather than something that merely mentions it.

    `pgrep -f` matches the whole command line, which is what makes it able to see a Wine-hosted
    process at all (the game shows up as `C:\...\Game.exe -3dfx -skiptobnet`). The cost is that
    any command carrying that path as an ARGUMENT matches too — a checksum being taken of the
    file, a backup tool, this app's own tooling. That really happened: the Quick Restart toggle
    refused to move, insisting the game was open, while nothing was running but a shell command
    with the path in it.

    So what gets tested is argv[0] — the program being run — not the arguments. argv[0] cannot
    simply be "the first token": the game's own path has spaces in it
    (`C:\Program Files (x86)\...`). It ends at the first ".exe", and it is only argv[0] if no
    second path has started before then, which is what tells `C:\...\Game.exe -3dfx` (the game)
    apart from `/bin/cp /a/Game.exe /b/Game.exe` (a file being copied).
    """
    if any(marker in line for marker in _SELF_PROCESS_MARKERS):
        return False

    # pgrep -l prefixes the pid; the rest is the command line.
    _, _, command = line.strip().partition(" ")
    command = command.strip().strip('"\'')

    lowered = command.lower()
    at = lowered.find(".exe")
    if at < 0:
        return False
    executable = command[:at + 4]
    if " /" in executable or " -" in executable:
        # Another path or a switch came first, so this .exe is an argument to something else.
        return False
    return any(executable.lower().endswith(name.lower()) for name in _GAME_PROCESS_PATTERNS)
