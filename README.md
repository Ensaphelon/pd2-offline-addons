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
