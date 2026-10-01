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

**Working.** 2026-09-23, in a real offline game:

```
hooked the DPS gate at 1026F527
SERVER  PlayerData 0D1D1800 | n=4 pending=7156 average=14484 window=747
CHANGED PlayerData 0D1D6800 | n=1 pending=0    average=14484 window=0
```

Damage flows (185,323 points over 147 hits in one fight), PD2's own accumulator averages it, and
the widget draws the result. The two structs track each other to the millisecond.

Note the dip to zero after five idle seconds in the log: that is PD2's own window expiring —
`n` drops back to 1 and the average resets — not anything here losing the value. The meter is
meant to read zero when nothing is being hit.

### How it got there

| Stage | Question | Answer |
|---|---|---|
| 1 | Does the widget read `PlayerData+0x1AC`? | Yes — writing `1337` put `1337` on screen. |
| 2 | Is PD2 already counting, on the client's copy? | No. `pending`/`window` never moved through a fight — while `n` moved *on its own*, so the struct was live and PD2 did write to it. |
| 3 | Does PD2's damage path run offline at all? | The hook on `+0x26F5B6` installed and never fired once. |
| 4 | Then what stops it? | The gate at `+0x26F527`, checking the server copy's flag — the one packet `0x5C` sets online and nothing sets offline. |

Two stores were all it needed in the end: set the flag the realm would have set, and carry one
DWORD from the server's copy to the client's.

### Logging

A normal run writes a handful of lines: the hooks going in, and one saying the mirror is alive
with its first reading. `DPS_VERBOSE` in `src/dps.c` turns on every field transition on both
structs plus a running damage total — a minute of fighting produced 150 lines, which is exactly
what is wanted when a PD2 update moves an offset and exactly what is not wanted otherwise. It
also installs the damage-counting hook, which is diagnostic only: the gate hook is what makes
the meter work, and counting costs two extra writes inside combat resolution.

---

## Writing the readings down

The number on screen is gone the moment the fight is. `src/records.c` writes each area's best to
`pd2dpsmeter-records.txt`, beside the DLL, which `pd2-holy-inventory` reads on its next scan and
turns into a table of personal bests per gear set.

One reading per line, tab-separated — tabs because a character name can hold a space:

```
<unix seconds>\t<character>\t<area id>\t<difficulty>\t<players>\t<dps>\t<guid,guid,...>
```

The last field is the player's own equipped item ids and it goes out **empty**; see "What is not
written" at the end. The trailing tab is still there, and the reader depends on it — a `strip()`
on that line eats it and leaves six fields where seven are required.

### Who knows what

| Field | Read from | Why here and not there |
|---|---|---|
| dps | the mirrored average | not in any file |
| area id | a walk from the player unit | not in any file |
| players | `ProjectDiablo.dll+0x4E2500` | not in any file |
| character | `PlayerData+0x00` | so a reading names itself, instead of being matched to whichever save happened to be written at the same moment |
| difficulty | `D2Client!GetDifficulty` | also in the save at byte `0xA8`, so the two halves can check each other |

**Why `players` is part of a record and not a note beside it**: PD2's damage hook clamps every
blow to the target's *remaining* life. On `/players 1` most of a big hit is overkill nobody counts;
on `/players 8` the same hit lands in full against four times the life. The same gear reads
differently, so it belongs in the key.

Found by disassembling this install, two independent sites for one global:

```asm
1023108E  mov ecx,[ecx+0x124] / inc ecx / mov [0x104E2500],ecx   ; the command raising it
1023CD60  mov ecx,[0x104E2500] / cmp ecx,1 / jle skip            ; and printing "players set to "
```

Zero is what it holds before the command is ever used, which is `/players 1`.

### Where the number comes from, and why not the one on screen

The records side reads the **server's** average, not the client's. The mirror that makes the
widget work only ever writes a non-zero value, so the client's copy keeps the last number it was
handed for as long as the game lasts. That is fine for a widget and useless to anything that needs
to know when a fight *ended*.

It matters because the number is a five-second mean, so walking out of a fight and into town
carries the tail of it through the door. The first real session put **36,544 dps in Harrogath** — a
town, where nothing can be hit at all. So an area does not start counting until the meter has read
zero once since the change, which is that window closing. A fight already in progress when the
area changes is therefore not recorded, which is the right way round: losing a reading is better
than filing it under the wrong place.

An area also does not start counting until PD2's own averaging window has turned over since the
door. The accumulator keeps the tick its current five-second window began on, and when that value
moves, that window has closed — everything after it was dealt since, so there is nothing left to
carry over.

Waiting for the number to read ZERO instead, which is what this did first, was wrong in a way
that only showed up in play: the average returns to zero after a LULL, so walking into an area and
fighting straight away armed nothing until the fighting stopped, and the first fight — the big one
— was thrown away. A real Inner Cloister run recorded 6,131 where the player had plainly seen
more; the next visit to the same place, with a pause after entering, recorded 76,026.

The town case still holds, and for a better reason than before: nothing is hit in a town, so the
accumulator never runs, the window never moves, and nothing arms at all.

### When a line is written

On leaving an area, and on leaving the game — not per reading. PD2's number is a five-second
cumulative mean that moves continuously through a fight; writing every sample would be thousands
of lines saying the same thing more and more weakly. The best reached in that area is the one
worth keeping.

Leaving an area is also when the game writes the save, so by the time the app reads the line it
has just re-read the gear the line is about. That is why the flush happens there and not on a
timer.

### The area walk, and why it is checked rather than trusted

`UnitAny+0x2C` is the path, which `beam.c` already reads the player's sub-tile position out of, so
the front of the walk is not in question. Past that it runs
`Path -> Room1 -> Room2 -> Level -> dwLevelNo`, and **those four links are published layouts, not
instructions read out of this install** — the one part of this feature that disassembly did not
pin.

So they prove themselves instead. The rooms point back:

* `Room2+0x30` is the Room1 that owns it, so a candidate pair (`Path->Room1`, `Room1->Room2`) is
  accepted only when following it and coming back lands on the Room1 it started from.
* `Level+0x10` is the level's first Room2, and that room's own `pLevel` is the level again — so a
  candidate `Room2->Level` offset is checked with itself, on a different object.

Neither can pass by accident: a wrong offset lands on some other field and the round trip does not
close. The level check is tried first and then dropped, so that one unproven offset cannot decide
whether the feature works at all.

What stays unproven is only **which word of `Level` holds the number**. All three candidates are
logged with their values the first time the chain resolves, beside the act from `UnitAny+0x18`, so
a wrong pick reads as a wrong area *name* on the records page — and the log says what it should
have been. Every read goes through `ReadProcessMemory`, the same way `beam.c` does it and for the
same reason: `IsBadReadPtr` and `VirtualQuery`-then-read have each taken this game down. The cost
of being wrong anywhere in the walk is a missing line.

### Corrupted zones

PD2 corrupts a group of zones per game (the client says so in chat: "Corruption spreads in the
Worldstone Keep and Throne of Destruction..."), and a map can be corrupted with a Worldstone
Shard. A corrupted zone is not the same test as the plain one, so the flag is part of a record's
key rather than a note beside it.

Three cheaper answers were tried against real data first, and each is wrong:

| Tried | Why it fails |
|---|---|
| The area id | A corrupted zone keeps the ordinary id. A real corrupted run came back as 129/130, the plain Worldstone Keep levels. |
| The announced name | Checked across all 37 groups against the installed `Levels.txt`: six match nothing, Uber Tristram is matched by mistake, and PD2 renamed "Frigid Highlands" to "Rigid Highlands" — so the match misses exactly the zone that prompted this. |
| The monster level | Corruption forces the level to 85, and Hell's Worldstone Keep 2 is *already* 85 in `Levels.txt`. Blind precisely where it is needed. |

What does say is the game itself. `ProjectDiablo.dll+0x26C310` picks the group (`nCorruptedZone`,
a random `1..n` kept at `pGame+0x26F2`, chosen once per game), then walks that group's level list
and marks each one:

```asm
1026c4d7  mov eax, [edi + esi*4 + 0xf0]     ; edi = pGame, esi = the level
1026c4de  mov dword [eax + 0x2dc], 0x55     ; <- hooked here; 85 is the corrupted level
1026c4ef  mov dword [eax + 0x2e0], 0x55
```

`src/corrupted.c` hooks that store and keeps the ids it sees. That reads the game's own answer
rather than reconstructing it: no group table to find, no struct layout assumed past what is
already patched, and nothing a PD2 rename can rot. ESI is the level at that instant and ESI is
all it takes. Ten bytes is a roomy landing site, and `mov` sets no flags.

The set is cleared when a mark arrives long after the last one: a group is written in one tight
loop, so its own marks land microseconds apart while the next game's run is a whole game away.

### Who took the damage

A second worth 100k reads as a good setup. It is a different thing when it was one unique
absorbing a whole screen of damage than when it was a pack of fallen dying to splash, and the
totals cannot tell those apart (user, 2026-10-01). So every blow is written down with the unit it
landed on, and each second of a run carries what it was spent on:

```
<second>[!]=<kind>:<row>:<damage>:<hits>:<instances>:<flags>[,...][;<second>=...]
```

`kind` is `UnitAny+0x00` — 1 a monster, 2 an object, so a barrel is never mistaken for a boss —
and `row` is the row in that kind's own table, left for the side that can read MonStats. Only the
seconds that saw damage appear, which on a real run is a minority of them. `instances` is how many
**different** ones of that kind were hit in the second, so twelve fallen are one entry rather than
twelve, and a monster hit forty times is still one monster.

#### Where the target comes from

At the hooked instruction EDI holds the damage context, and `[edi+0x0C]` is the unit being hit.
That is not a published layout — it was read out of this install, where the same word is used for
three things only a target can be:

```asm
1026f512  mov eax,[edi+0xc] / mov [esp+0xc],eax     ; kept in a local for the rest of the function
1026f59a  push [esp+0x14] -> call 0x10273cb0(_,6,0) ; stat 6 is `hitpoints`, in 256ths...
1026f5a3  shr eax,8 / cmp ebx,eax / cmovg ebx,eax   ; ...and the damage is CLAMPED to it
1026f5e9  cmp [eax],1 / mov eax,[eax+4]             ; type 1 = monster, then its txt row,
          cmp eax,0x3a7 / cmp eax,0x3a8             ; compared against MonStats 935 and 936
```

The clamp is the proof: a damage function clamps to the **target's** remaining life and nothing
else's. The pair at the bottom then confirms the two words this reads — `+0x00` the unit type,
`+0x04` the row — against the game's own comparisons rather than against a document: MonStats 935
and 936 are `rathmaBoneClone` and `rathmaPoisonClone`, and the lines after that comparison move
health from one to the other. Two clones sharing a life pool, which is exactly what that code
is for.

It also means the pointer needs no validating. The game dereferences it itself, unconditionally,
eight instructions earlier.

#### Why it is still assembly

A call into C from a trampoline in the middle of someone else's arithmetic cost this game its
monster density for a day — see "Corrupted zones" above. The cure there was `fxsave`/`fxrstor`.
The cure here is to have nothing to save: fourteen instructions, no call, so no ABI to obey. They
push the three registers they use and pop them back, so nothing about which registers are live at
the site has to be reasoned about correctly for this to be safe. Flags are destroyed, and that was
checked at the site — the next instruction to read them, at `1026f5d9`, is its own `test`.

The entries go into a 1024-slot ring that the game's thread appends to and the plugin's own thread
drains 66 times a second. The published cursor is a **count**, not an index, so a reader that fell
more than a ring behind can see that it did rather than silently read torn entries; when that
happens the line's totals are still complete (they come from the hook's running sum) and only the
breakdown is short, which is what the log says.

#### What it does not know

* **Which one of them it was.** A unique, a champion and a trash mob of the same kind share a
  MonStats row, so a boss shows up under the base monster's name. Telling them apart means reading
  `MonsterData` (`UnitAny+0x14`), whose layout has not been established on this install.
* **More than eight kinds in one second**, or more than sixty-four individuals. Past the first the
  second says so with a `!` and keeps its total; past the second the instance count stops rising
  while the damage keeps adding, so it reads as "at least this many". Both are deliberate dead
  ends rather than approximations that creep.
* **Who dealt it.** PD2's own meter counts the player's damage only — its gate checks that the
  attacker is a player with a `PlayerData` — so a mercenary's and a minion's damage is in neither
  the total nor the breakdown.

### What is not written

The guid list. The app falls back to the save for what was equipped, which is right nearly always
and wrong in one case: a weapon swapped mid-fight and still held at the area change is recorded as
having been worn for the whole reading. Filling it means a second walk through structures this
plugin does not own, for a correction the save already makes at the next area change. Left undone
deliberately.
