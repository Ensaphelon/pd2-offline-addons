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
| `NO TRACE` | Written at the next launch. The previous session logged neither an exit nor a crash, so it was killed from outside or died too hard to run any code. |

`pd2addons.log` of the previous session survives one launch as `pd2addons.prev.log`. That file is
what the `NO TRACE` check reads, and what to read after a bad ending.

Serious exceptions are also logged first-chance in `pd2addons.log`, at most 20 per session. Some
of them are caught and harmless. The last one before a bad ending is the one to look at.

## How

- **Exits.** Both hooks use the hot-patch prologue: five spare bytes and `mov edi,edi`. The hook
  sees the real caller before any thread is torn down.
  - On Windows 11, `kernel32!ExitProcess` opens with `push ebp / mov ebp,esp`, without that
    prologue. So the exit hook sits on `ntdll!RtlExitUserProcess`, which every exit goes through,
    and takes the caller from kernel32's frame above it.
  - `TerminateProcess` is hooked at kernel32's own export, which does have the prologue.
  - Both patches compare the bytes first. A function that does not match is left alone, and the
    log says `NOT hooked`. The session's end is then seen only at DLL detach, without a caller.
- **Crashes.** The top-level filter slot is held by re-taking it once a second, because Fog
  claims it during startup. Fog's filter is called after ours.
  - The slow work (formatting, the stack scan, the dump) runs on a thread of its own. A thread
    that overflowed its stack has no room for it.
- **First-chance exceptions.** A vectored handler notes every exception with a `0xC…` code.

Tested on a stand-in executable for each ending:

- `ExitProcess(42)` and `TerminateProcess(7)`, both with the caller named;
- a null write, which produced a dump;
- an access violation swallowed by a later handler and then `ExitProcess(1)`, which produced
  `SUSPECT`;
- a `taskkill /F`, which produced `NO TRACE` at the next start.
