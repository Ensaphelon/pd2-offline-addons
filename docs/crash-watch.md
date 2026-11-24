# Crash watch

On 2026-11-21 the game closed twice in ten minutes and left nothing anywhere. D2's own log
(`D2YYMMDD.txt`) had no `Halt`. Windows' Application log had no Application Error. The addon's own
`pd2addons.log` had already been truncated by the next launch.

D2's error handler (Fog) catches an ordinary crash, writes its report, and exits, so Windows
never hears about it. When neither of them recorded anything, there are two likely causes:

- the process ended without an exception at all;
- something caught an exception and quit quietly.

Telling those apart needs a witness inside the process at the moment it ends.

## What it writes

`pd2addons-exits.log`, beside the DLL, gets one line per session: the start time, the end time and
a verdict.

| Verdict | Meaning |
|---|---|
| `closed` | `ExitProcess` or `TerminateProcess` on itself, with the exit code and the caller. Closing the game yourself lands here too; the caller tells the two apart. |
| `SUSPECT` | The same, but within 5 s of a serious exception that never reached a crash handler. Something swallowed it and quit. |
| `D2 HALT` | D2 stopped itself: either `ExitProcess` was called from Fog, or the next launch found a `Halt` in `D2YYMMDD.txt`. Fog's report has the details. |
| `CRASH` | An exception nothing caught. `pd2addons.log` gets the registers and the stack, and `crashes\pd2-<date>_<time>.dmp` gets a minidump (the newest 10 are kept). It is then handed on to Fog as before. |
| `KILLED` | Written by `pd2watch.exe` the moment the game ends, when nothing inside it saw the end. It carries the process's exit code. |
| `NO TRACE` | Written at the next launch. The previous session logged neither an exit nor a crash, and no watcher wrote for it either. |

`pd2addons.log` of the previous session survives one launch as `pd2addons.prev.log`. That file is
what the `NO TRACE` check reads, and what to read after a bad ending.

Serious exceptions are also logged first-chance in `pd2addons.log`, at most 20 per session. C++
exceptions are logged too, with their type and the code that threw them, up to another 20. Some of
them are caught and harmless. The last one before a bad ending is the one to look at.

`pd2addons.alive` is rewritten every two seconds. A `NO TRACE` line quotes it, so a session that
vanished still says roughly when it died.

## How

- **Exits.** Both hooks use the hot-patch prologue: five spare bytes and `mov edi,edi`. The hook
  sees the real caller before any thread is torn down.
  - On Windows 11, `kernel32!ExitProcess` opens with `push ebp / mov ebp,esp`, without that
    prologue. So the exit hook sits on `ntdll!RtlExitUserProcess`, which every exit goes through,
    and takes the caller from kernel32's frame above it.
  - `TerminateProcess` is hooked at kernel32's own export, which does have the prologue.
  - **The wider net.** It was added after a close on 2026-11-21 that none of the above saw: no
    exit, no exception, and nothing from Windows. The close came while shift+right-clicking a
    crafted map from the cube into the inventory.
    - `kernelbase!TerminateProcess` is what modern code reaches through api-set imports, skipping
      kernel32. It is hooked by its hot-patch prologue, and so is `RaiseFailFastException`.
    - `ntdll!NtTerminateProcess` is where every user-mode way out ends. It opens with a five-byte
      `mov eax, <syscall number>`. A jump replaces that instruction, and a trampoline does the
      `mov` and jumps back.
    - A `NULL` process handle there means "every thread but this one", which only a process on its
      way out asks for.
  - Every patch compares the bytes first. A function that does not match is left alone, and the
    log says `NOT hooked`. If none of the exit hooks took, the session's end is seen only at DLL
    detach, without a caller.
  - Not seen in-process: a `__fastfail` (the `int 0x29` instruction goes straight to the
    kernel), a stack overflow with no room left to report it, and a kill from another process.
    The same day showed that Windows does not reliably write these down either. A test
    `int 0x29` left no WER report and no Application Error.
- **The moment before a fail-fast.** MSVC's runtime calls
  `IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE)` on each of its fatal paths: a failed /GS
  cookie, `abort()` and an invalid parameter. It then runs `int 0x29`, which no handler sees.
  - Both kernel32's export and kernelbase's are hooked. On feature 23 the hook writes down which
    module asked, logs the stack and writes a dump with the thread's own captured context.
  - The stack holds the return address into the function whose cookie failed.
  - The hook also clears the thread's error mode and the process's, so Windows' own report goes
    through as well.
  - Added on 2026-11-21, after a `0xC0000409` that the BH buffer fix did not stop.
  - Only a call followed within 16 bytes by `int 0x29` (`CD 29`) counts. VCRUNTIME140 asks the
    same question at startup as a plain capability check (+0x176E7). On 2026-11-24, taking that
    for a fail-fast stalled the start for 20 seconds: a thread was started under the loader lock
    and waited on. The worker then crashed the game, because its job had been left on a stack
    frame that had already returned.
  - The report's job now lives in static storage, and no worker is started while the thread holds
    the loader lock.
- **The exit code, from outside.** `pd2watch.exe` beside the DLL (`tools/pd2watch.c`) waits for
  the game to end and reads its exit code. Three codes tell the endings above apart:
  `0xC0000409` for a fail-fast, `0xC00000FD` for a stack overflow, and `1` for a `taskkill /F`.
  - Game.exe denies every other process access to itself, so the watcher could not open it. The
    DLL duplicates its own process handle instead (from the pseudo-handle, which needs no
    permission). That handle is the only one the watcher inherits
    (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`).
  - If the game's own log has no `exit:` or `crash:` line for the session, the watcher writes a
    `KILLED` line with the code to the exits file. It also writes an `exit:` line to the log, so
    the next launch does not add `NO TRACE` as well.
- **Crashes.** The top-level filter slot is held by re-taking it once a second, because Fog
  claims it during startup. Fog's filter is called after ours.
  - The slow work (formatting, the stack scan, the dump) runs on a thread of its own. A thread
    that overflowed its stack has no room for it.
- **First-chance exceptions.** A vectored handler notes every exception with a `0xC…` code, and
  every MSVC C++ throw (`0xE06D7363`). For a throw it reads the type from the throw metadata,
  checking each pointer with `VirtualQuery`. The thrower comes from the stack, since the exception
  address is always inside `RaiseException`.
  - A stack overflow is written down by a watchdog thread that the handler wakes. The thread that
    overflowed has no room left to do it.

Tested on a stand-in executable for each ending:

- `ExitProcess(42)` and `TerminateProcess(7)`, both with the caller named;
- a null write, which produced a dump;
- an access violation swallowed by a later handler and then `ExitProcess(1)`, which produced
  `SUSPECT`;
- a `taskkill /F`, which produced `NO TRACE` at the next start;
- direct calls to `kernelbase!TerminateProcess(5)`, `ntdll!NtTerminateProcess(9)` and
  `RaiseFailFastException`, each with the caller named;
- a stand-in C++ throw, logged with its type and the throwing call;
- endless recursion, which gave the watchdog's overflow line, a `CRASH` line and a dump;
- with the watcher running: a clean exit, which it left alone; a `taskkill /F`, which it wrote
  down as `KILLED` with exit code `0x00000001`; and a real `int 0x29`, which it wrote down as
  `KILLED` with `0xC0000409`.
