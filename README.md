# pd2-holy-grail

Decorating a Holy Grail drop where it lies, inside Project Diablo 2.

Right now this is only the **probe**: a DLL that reads the running game's memory and writes a log.
It hooks nothing, draws nothing and writes nothing back into the game. That is the point — every
question still open can be answered by looking, and something that only looks cannot damage a real
save.

## What it is trying to settle

An item in the player's own save carries its unique/set identity even before it is identified
(measured). What is not known is whether the game's in-memory copy carries the same identity, in
the same numbering our catalog uses. If it does, the plugin can eventually say "this one really
completes the grail" rather than "something on this base might". If it does not, we need a
translation table, and it is far better to find that out now than after a renderer is built on
top of the assumption.

## Use it

```bash
./build.sh                                     # needs mingw-w64
python -m pd2_holy_grail install --game "…/ProjectD2"
```

Put `probe-targets.txt` and `pd2holygrail.dll` next to `Game.exe`. Start the game, get the items
in view, press **F9**. Findings land in `pd2holygrail.log` beside the DLL, one pass per press.

`probe-targets.txt` holds the values to hunt for — item guids, catalog ids, anything. It is read
at load, so a new experiment is a text edit rather than a rebuild.

```bash
python -m pd2_holy_grail uninstall --game "…/ProjectD2"
```

puts `Game.exe` back from its backup, byte for byte, and deletes the DLL.

Only Game.exe **1.0.13.60** is supported; anything else is refused rather than patched at
addresses that have moved. Close Diablo II before installing or removing.

## Layout

```
src/probe.c            the scan: find known values, print the memory around them
src/main.c             loads, waits for F9, otherwise does nothing at all
pd2_holy_grail/        the installer — one import-table entry, reversible from a backup
```

Sibling project: [pd2-quick-restart](https://github.com/Ensaphelon/pd2-quick-restart), whose
injection and logging this reuses.

## Decisions made while probing

**The item-appears-on-the-ground sound is 4686**, `act1\bloodravenresolution.wav` — one of the
game's fifteen quest stings. It sits in sound group 10 with the quest effects, so it obeys that
slider rather than LOOT FILTER; shipping our own copy into the archive with a group-12 row is the
alternative, and is not worth doing until somebody minds.

**The play call is** `play(unit, soundId, volume, priority, flags)`, exported by
`ProjectDiablo.dll` as `_D2Client_PlaySoundWithCustomVolumeOrPriority@20`. Volume is the third
argument and zero is silence. `unit` is dereferenced when non-zero — pass 0 unless you have a
real one, or the game goes down.

**What the running game knows**, all measured rather than assumed:

| | |
|---|---|
| the player | `D2Client.dll+0x10A60C` |
| an item is on the ground | `UnitAny+0x10` (dwMode) is 3 |
| its quality | `ItemData+0x00` — 5 set, 7 unique |
| its identity | `ItemData+0x28`, in the same numbering pd2-holy-inventory's catalog uses |

`D2Client.dll+0x10AE08` was written here as "the item row of the unit table" and that was wrong:
every entry in it carries unit id 1 and no identity, and the real items — the ones found by their
guid, at quite different addresses — are not in it at all. It is a pool. Ground detection works
by searching memory, not by reading that table.

The identity is present on the ground even before the item is identified — but only on the
server-side unit, which exists in the same process in single player. The client's own copy of a
ground item carries a zero there, which is why community loot filters hard-code 107 item names
instead of reading one.

## Drawing

Solved, and the answer was published offsets rather than another memory sweep. Six earlier
attempts each failed for its own real reason:

| attempt | why not |
|---|---|
| a line through Glide's `grDrawLine` | draws at the frame swap, when the frame is already composed |
| the frame buffer via `grLfbLock` | D2GL refuses the lock outright |
| `D2gfx` ordinal #10010 from the swap | same timing problem |
| the same from inside the game's frame | the call fires, nothing appears — it is DrawLine, given rectangle arguments |
| an overlay attached to the item's unit | needs the game's own attach function, which nothing public names |
| a client-side object spawned like a town portal | in single player no packet is involved, so there is nothing to replay |

What works, all from SlashDiablo Maphack's `D2Ptrs.h` (AGPL, so published) unless noted:

| | |
|---|---|
| `D2Gfx #10014` | `DrawRectangle(x1, y1, x2, y2, colour, transparency)` |
| `D2Gfx #10041` | `DrawAutomapCell2(context, x, y, bright2, bright, colour table)` — what d2bs draws its own images through |
| `D2Cmp #10006` | `InitCellFile(buffer, &out, source, line, version, name)` |
| `D2Client+0x1630` / `+0x1660` | `GetUnitX` / `GetUnitY`, `__fastcall` |
| `D2Client+0x11C1F8` | the view's origin on screen, a `POINT` |
| `D2Client+0xF16B0` | the view's divisor, one in play |
| `D2Client+0x10A608` | the unit table, six rows of 128 buckets, items in row four, linked through `+0xEC` |

Drawing has to happen inside the game's own frame and through D2Gfx, which is what D2Client
itself draws through — so it works on DDraw, Direct3D, Glide and D2GL alike, and D2GL improves it
the same way it improves the game's own effects.

**World to screen** is measured, not quoted:

```
x = (wx - wy) * 16 - GetMouseXOffset()
y = (wx + wy) * 8  - GetMouseYOffset() + 24
```

With the player at world 3993,5228 on a 1068x600 screen the origin has to be -20294,73468, and
`GetMouseXOffset` returns exactly -20294 — it is the origin the game converts the mouse through,
so it already knows the view has slid. Open the inventory and it moves to -20027: 267 pixels, a
quarter of the screen width, which is how far the world shifts to make room. The y wants a
constant 24 on top.

Two things it is not. `D2Client+0x11C1F8`, BH's automap origin, reads 0,0 here with a divisor of
20. The variables at `+0x119960` and `+0x11995C` hold the right pair but do not move when a panel
opens, which is exactly the bug being fixed.

## The art

The light is the game's own. `data\global\overlays\HoradricLightBeam.dcc` is the shaft the
Horadric quest shines, twenty-one frames of it, and `LIGHTJET.dcc` is a fan of rays that grows out
of the ground and fades — thirteen.

Both are DCC, and the only cell loader the game exposes understands DC6. So the DCC is decoded
outside the game by `tools-beam/` (a port of OpenDiablo2's reader) and the frames are re-coded as
a DC6 that never came from a file; `D2Cmp`'s `InitCellFile` takes it exactly the same way, which
is how BH shows images of its own. `src/art.c` is generated:

```bash
python tools-beam/build_dc6.py src/art.c
```

`tools-beam/preview.py` and `roundtrip.py` render both the decode and the re-encode to PNG, which
is how the format was checked before the game ever saw it.

`#10019 DrawCellContextEx` was the first guess and took the game down on the first drop. Both
calls are six `__stdcall` arguments — confirmed by the `ret 0x18` at the end of each, which is
how the ordinals were checked rather than trusted — but #10041 is the one with working code
behind it, and its last argument is a 256-byte colour table where a bare zero had been passed.
Two guards came out of that round: the buffer is read back after `InitCellFile` and nothing is
drawn from it unless the first cell reports the size we put in, and `test 1` draws one sprite at
a fixed spot on screen so a bad draw falls over on the way into the game rather than over a rare
item.

**The CellContext is bigger than BH and d2bs say.** D2Cmp's cell lookup, at `D2CMP+0x122E0` —
the function whose assertion halts the game with `Unrecoverable internal error 6fe2232e` —
accepts a context only if the cell file at `+0x34` is there and says version 6, the DIRECTION at
**`+0x40`** is under 64, and the frame number at `+0x00` is within the file's cell count. Both
public headers describe the structure as ending at `+0x38`, so that direction falls off the end
of it and reads whatever was on the stack. That is the halt, and it is also why the first
attempt took the game down on the first drop rather than at a coordinate, as was suspected.

Neither of the first two calls was tried with a large enough context. `#10041` returns without putting anything on the screen, so the
guessing stopped: the thunks now hand over the caller's argument pointer as well as its index,
and for a few dozen frames every hooked call whose first argument is a CellContext — a pointer
whose +0x34 leads to a cell file whose first cell has a sensible size — is logged with all six
of its real arguments. The game makes hundreds of these calls a frame; one of them is the one
that draws a sprite, and it will be reading its own arguments out that says which.

**The blend is 3.** All eight were drawn side by side over grass rather than guessed at one per
session: 0 is nearly invisible, 3 glows and lets the ground show through, 5 is the flat opaque
one the game uses for its own panels, and the rest are slabs. The art is 81% near-black — index
0 is only a fifth of it — so it is made to be added to the background, and anything that lays it
over the ground shows a black brick. That is what the first look at it was.

`beam.txt` sits beside the DLL and is re-read while the game runs, so which art, which blend,
which speed and where exactly it sits are a text edit and not a rebuild.
