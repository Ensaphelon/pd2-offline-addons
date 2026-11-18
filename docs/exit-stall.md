# Leaving a game without the stall

Leaving a game in its first seconds hung for three or four before the menu came back; leaving
after a minute did not. Project Diablo 2's own behaviour, and gone with this plugin.

## Where the time went

Nothing outside the process can look: `Game.exe` refuses other processes every access right,
even the most limited query, so an external sampler sees thread contexts and has no memory to read
them against. The answer came from inside — [`hangprobe.c`](../src/hangprobe.c), a sampler built
into the plugin behind its own switch. When the player unit disappears it samples every thread for
ten seconds and logs each one's CPU, where it stood and the return addresses that got it there.

An exit 2.5 s into a game, main thread by phase:

| after the player unit went | the main thread |
|---|---|
| 0 – 1.3 s | `Fog+1E0CB`, `WaitForSingleObject(…, INFINITE)`, from `ProjectDiablo.dll+2C9D65` |
| 1.4 – 2.3 s | `D2Win` / `D2Launch` building the main menu |
| 2.5 – 2.7 s | `D2Sound` |
| 2.9 s on | presenting frames: the menu is up |

The first phase is the stall. `ProjectDiablo.dll+2C6F10` is PD2's own sound code
(`client/Sound/SoundSample.cpp`) freeing every sound sample as the game is left, one by one
(`+2C9CE0` each). A sample whose file is still loading is an asynchronous request — Fog marks them
`0x41737960` — and freeing it waits for the read to finish, with no timeout. Rightly: the read is
still writing into it.

## Why the loads were still running

Storm's async reader is throttled. Its thread (`Storm+29200`) takes the next non-urgent request no
sooner than `chunk × 1000 / rate` milliseconds after the last, and Fog sets both at startup
(`Fog+1DF3B`):

```
push 0x40000 ; call Storm #284   -> rate:  256 KB a second
push 0x4000  ; call Storm #285   -> chunk: 16 KB
```

One request every 62 ms — about sixteen sound files a second — so the queue a new game starts takes
the better part of a minute to drain, and an exit before then waits on its tail. The sampler caught
the reader spending most of the exit in that pacing wait (`Storm+29531`), not reading.

The throttle was for the hard drives and CD-ROMs of 2000, where background reads competing with
the game's own made it stutter.

## The fix

[`storm_io.c`](../src/storm_io.c) calls Storm's own setter, `#284`, with 0. The reader skips the
division when the rate is 0 (`test ecx,ecx` / `je`) and the pause comes out as nothing; no code is
patched. It is set once a second, so if anything restores Fog's 256 KB/s it lasts a second at most.
The setter's bytes are compared before the first call — `mov eax,[esp+4] / mov [rate],eax / ret 4`
— and a Storm where `#284` is something else is left alone, with a line in the log.

What remains of an exit is the part every exit has: about a second and a half of building the menu
and restarting the sound.

## Running the probe again

```sh
CC=... gcc -shared -o pd2addons.dll src/*.c ... -lversion -lpsapi -DADDONS_HANGPROBE=1
```

It has its own switch rather than `ADDONS_DEV` because the dev probes hook hundreds of call sites
per frame, and a timing question cannot be answered by a build that changes the timing. It needs
the game left open for ten seconds after the exit, or the summary is never written.
