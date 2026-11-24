# Object guard

An object with no data gets an answer instead of a crash.

## What crashed

On 2026-11-24 the game died in Harrogath reading address 0 at `D2Common+0x2EADF`. The function
there, D2Common #11059, takes a unit. For an object (type 2) it reads the ObjectData at `+0x14`,
then the Objects.txt record that ObjectData starts with, and checks neither:

```
6fd7ead7  cmp  [eax],2 / jne -> return 0      ; not an object: 0
6fd7eadc  mov  eax,[eax+0x14]                  ; ObjectData
6fd7eadf  mov  ecx,[eax]                       ; its txt record   <- crashed, eax = 0
6fd7eae1  mov  eax,[ecx+0x168]
```

The caller was `ProjectDiablo.dll+0x2ED600`.
- Every frame it takes the unit PD2 keeps for what the cursor is over: the global at
  `+0x4ABE88`, behind its hover label ("Blood Moon" on the screenshot).
- If that unit is an object, it asks #11059.
- In the crash dump the unit was not a live object: unit id `-1`, ObjectData `0`. The hovered
  object had gone, and PD2 still held the pointer.

## What the guard does

Clearing that pointer is PD2's job, and PD2 does not do it. What this plugin can do is make the
one question PD2 asks safe.
- An object whose data or txt record is missing gets `0`. That is what the function already
  returns for anything that is not an object.
- The only thing that can change is the hover label of an object that is no longer there.
- Everything else goes to the original unchanged, including a null unit, which D2 asserts on as
  before.

`src/object_guard.c`:
- #11059 opens with `mov eax,[esp+4] / test eax,eax / jne`.
- Those six bytes are compared first, then replaced with a jump to the guard.
- The trampoline runs the same `mov` and `test` and jumps back to the `jne`, whose flags they set.
- A D2Common that opens differently is left alone, with a line in the log.

`pd2addons.log` shows `object guard: D2Common #11059 answers 0 for an object with no data` once
the guard is in. It shows `object guard: object class … (unit id …) has no data; answered 0
instead of crashing` each time it saves the game, at most 5 times per session.

Tested against a stand-in `D2Common.dll` exporting #11059 with the same prologue and body. An
object with data returned its field (77). An object with no data returned 0 and did not crash. A
monster and a null unit returned what the original returns.
