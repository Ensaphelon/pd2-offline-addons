# pd2-dps-meter-offline

Project Diablo 2's DPS meter, working in offline single-player.

PD2's own counter draws in offline games and never leaves `0`. This plugin makes it show a real
number — **PD2's own widget, PD2's own formatting**, not an overlay next to it.

## Why it needs a server

Short version: it doesn't, quite. It needs *someone* to run the accumulation, and offline nobody
does.

Diablo II's client deliberately knows nothing about damage. Damage is resolved in the game-server
module; the client is told outcomes — animations, states, a health bar — never numbers. So PD2
counts damage where the numbers are and ships the client a finished figure to print. Offline there
is no realm to do the counting, so the widget prints the zero it was born with.

The interesting part is what the disassembly of the shipped `ProjectDiablo.dll` says: **the entire
feature is already in the client DLL.** Attribution, clamping, averaging and drawing are all
there. What is missing offline is only the call that drives it.

## What the client actually does

From `ProjectDiablo.dll`, image base `0x10000000` (2026-09-23 build). PD2 extends vanilla D2's
`PlayerData` with its own fields; four of them are this feature's whole state:

| Offset into `PlayerData` | Meaning |
|---|---|
| `+0x1A4` | sample count `n`, and the gate the draw site checks |
| `+0x1A8` | damage dealt since the last averaging tick |
| `+0x1AC` | **the running average — the number on screen** |
| `+0x265` | game tick the current window started on |

**The draw site** (`0x102589A0`) is short enough to quote in full:

```asm
102589a0  call 0x1028f460            ; the client's player unit -> edi
102589cb  mov  eax,[edi+0x14]        ; UnitAny+0x14 = pPlayerData
102589d6  cmp  DWORD [eax+0x1a4],0   ; not tracking -> draw nothing
102589dd  je   .skip
102589df  push DWORD [eax+0x1ac]     ; <-- the number on screen
102589e5  mov  ecx,0x1037c780        ; "PlayerDpsDisplay", the format-string key
102589ea  call 0x102d0fb0
          ...                        ; sprintf, then draw
```

**The accumulator** (`0x102AE5FF`) is a cumulative mean over a window:

```asm
mov  eax,[ebx+0x1ac]   ; running average
mov  ecx,[ebx+0x1a4]   ; n
mov  edx,[ebx+0x1a8]   ; damage since last tick
imul eax,ecx           ; avg*n
inc  ecx               ; n+1
add  eax,edx
cdq / idiv ecx         ; (avg*n + sum) / (n+1)
mov  [ebx+0x1ac],eax   ; new average
mov  [eax+0x1a8],0     ; sum consumed
```

The window restarts once `[ebx+0x265] + 125` ticks have passed. 125 ticks is 5 seconds at D2's
25 fps, so the figure on screen is a **5-second cumulative mean**, not an instantaneous rate.

**The attribution** (`0x1026F4FA`) is a real damage hook, and it is stricter than anything a
home-made meter gets for free: it takes the raw damage in 256ths, shifts it down to whole points,
then **clamps it to the target's remaining life** before adding it to `+0x1A8`. An overkill blow
contributes only what it actually removed.

**And the handshake** (`0x1023CC30`): when the launcher's `dps` setting changes, the client copies
it into `PlayerData+0x1A4` and sends packet `0x5C` — "start/stop sending me DPS". Offline that
packet goes nowhere, which is the whole bug in one line.

## The plan

Stage 1 — **prove the display path** (this is what the code does today). Write a recognisable
constant into `PlayerData+0x1AC` and see whether the meter prints it. If it does, everything left
is a question of *what number to write*. If it doesn't, the theory above is wrong and it is worth
finding that out before building anything on it.

Stage 2 — **find out whether PD2 is already computing it.** Offline, the game server runs inside
the same process, so the damage hook at `0x1026F4FA` may well be running and writing to the
*server-side* player struct, while the widget reads the *client-side* one. D2 keeps two separate
unit lists even in single player. If that is what is happening, the fix is a one-DWORD copy per
frame and the number is identical to online — PD2's own maths, including the overkill clamp.

Stage 3 — only if stage 2 comes up empty: feed `+0x1A8` ourselves and let PD2's own accumulator
do the averaging. See "Counting damage ourselves" below.

## Counting damage ourselves

The obvious approach is to sum monster health and diff it. It is worth writing down where that
goes wrong before relying on it, because the failure modes all push the number the same way.

A first cut — *"ΣHP over monsters present in both this second and the last"* — correctly avoids
counting a freshly-streamed map chunk as damage. But:

- **Kills vanish, and with them the killing blow.** A monster that dies leaves the set, so the
  damage that killed it is dropped. That undercounts exactly when DPS is highest. Death has to be
  detected by the unit's *mode* (D2 leaves corpses around for a while), not by absence — a unit
  that disappears without ever entering a death mode simply walked out of range and should be
  dropped silently.
- **Regeneration reads as negative damage.** Clamp each monster's contribution at `max(0, …)`
  rather than letting it subtract.
- **One-second buckets are coarse and jumpy.** Sample at ~10 Hz into a ring buffer and report the
  sum over a trailing window — and use PD2's own window, 5 seconds, so the number means the same
  thing it means online.
- **Overkill inflates.** A 9000-damage hit on a monster with 300 life left is 300 damage dealt.
  PD2's own hook clamps this; a health-diffing meter gets the clamp for free only because health
  cannot go below zero — but it loses the amount, which is the point above about kills.
- **Attribution.** Health diffing counts your mercenary, your minions and any other source. PD2's
  hook counts damage whose attacker is the player. These are different numbers; the meter is
  called `PlayerDpsDisplay`.

Which is a long way of saying: stage 2 is worth real effort, because PD2's own hook already
handles every one of these correctly and we would only be approximating it.

## Building

Needs `mingw-w64` (`brew install mingw-w64`) — Diablo II 1.13c is a 32-bit process, so the target
is i686 and that is not negotiable.

```sh
./build.sh
```

Produces `pd2dpsmeter.dll`.

## Installing

The plugin has to be inside the game process to read its memory, and the only moment it can get
in is process creation — so Game.exe is made to load it, by adding one entry to its import table.
The edit is additive and reversible, and nothing belonging to PD2 itself is touched.

```sh
./build.sh
python3 -m pd2_dps_meter status    --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_dps_meter install   --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_dps_meter uninstall --game "<path to>/ProjectD2/Game.exe"
```

The game must not be running, and a PD2 update replaces Game.exe and silently takes the install
with it — so `status` reads the real import table rather than remembering what it did last time.

This can live alongside the sibling Holy Grail plugin: the patcher copies every import descriptor
already in the file and appends its own, and each plugin keeps its own backup suffix. Sharing one
suffix would let the second installer overwrite the first's backup of the *pristine* exe with a
backup of the already-patched one.

**Uninstall in the reverse order you installed.** Each plugin's backup is "the exe as it was
before *that* plugin touched it", and uninstalling restores it wholesale — so removing the one
installed *first* puts back an exe that never knew about the second, silently taking it with it.
Removing the last one installed is always safe. `status` reads the real import table, so if this
ever happens it is visible rather than merely surprising.

It writes `pd2dpsmeter.log` next to itself, truncated on each attach — one file per game session
is what you actually want to read.

## Status

**Stage 1 done** (2026-09-23, in game): writing `1337` into `PlayerData+0x1AC` put `1337` on
screen. The draw site reads that DWORD and nothing else, so everything from here is only a
question of what number to write — every offset in this README is now confirmed against a
running game, not just read out of a disassembly.

That session also showed `n=0 pending=0 average=0 window=0` before anything was written, and our
`1337` surviving untouched for fifteen seconds — so nothing else writes that field while idle.
It does **not** settle stage 2, because nothing was fought: fields that never move during a
session with no combat say nothing at all about whether PD2 is counting.

**Stage 2 answered, and it is the third outcome.** Through a real fight, the client-side
`pending` (+0x1A8) and `window` (+0x265) never moved — while `n` (+0x1A4) went from 0 to 1 *on
its own*. That last detail is what makes it conclusive rather than merely negative: something in
PD2 does write to this struct (the handshake at +0x23CC30, copying the launcher's `dps` setting
into the gate), so it is live and reachable — and the damage fields stay dead through combat
anyway. The damage accounting is running against the server's copy of the player, which offline
lives in this same process.

**Stage 3 in progress: hooking the damage instruction.** `ProjectDiablo.dll+0x26F5B6`,
`add [eax+0x1a8], ebx` — whatever struct the damage is being added to, the amount is in EBX, and
EBX is ours to read. It arrives already clamped to the target's remaining life, so overkill is
taken off before we ever see it. See `src/hook.c`. The current build only counts and logs; it
writes nothing into the game.
