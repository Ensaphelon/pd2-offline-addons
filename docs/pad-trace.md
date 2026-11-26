# Pad trace

A flight recorder for playing with a controller. It is a diagnostic, so it is off unless
`pad-trace.cfg` beside the DLL says `enabled = 1`.

It was built for two bugs on an Xbox pad (2026-11-26):

1. **The cursor jumps.** Moving the cursor with the left stick, it sometimes lands somewhere else
   in one frame, with nothing pressed. Sometimes that is the centre, sometimes up and to the left.
2. **Skills stop casting.** After a while a skill button still changes the right-click skill
   icon, but nothing is cast. Moving the mouse a little and going back to the pad fixes it.

## What PD2 does with a pad

These facts were read out of the season 14 ProjectDiablo.dll, at preferred base 0x10000000.

| | |
|---|---|
| Input mode | One byte at `+0x4AC2FE`: 1 is controller, 0 is mouse and keyboard. |
| Pad wins | Any pad activity sets controller mode and zeroes the mouse-move count. This happens at `+0x2EBD77` in menus and at `+0x2EE62F` in a game. |
| Mouse wins | PD2's window procedure (`+0x2EBDB0`, a subclass of D2's) counts `WM_MOUSEMOVE`, `WM_TOUCH` and `WM_GESTURE` at `+0x4AF9BC`. At 40 it sets mouse mode. A real `WM_LBUTTONDOWN` or `WM_RBUTTONDOWN` sets it at once. PD2's own clicks are marked at `+0x4ABDBC`, so they do not count. |
| On every change | `+0x2EB860` resets the controller's cursor, hover and click state (`+0x29C630`). When switching to mouse in a game, that reset also calls `SetCursorPos` to the middle of the screen. Losing focus goes through the same reset without changing the mode. |
| Cursor in controller mode | The game's cursor is not the Windows cursor. PD2 moves D2's cursor by passing D2's handlers a `WM_MOUSEMOVE` it built itself (`+0x2964E0`). It clicks the same way: `+0x296640` left down, `+0x296880` left up, `+0x296A90` right down. Out of a game it moves the cursor through D2, and clicks with `SendInput`. |
| Park | `+0x296B90` saves D2's cursor and parks it at (115, 547). It is called every tick from `+0x299140`. That caller parks when nothing is under the cursor and a table entry is set, and unparks otherwise, putting the cursor back where it was saved. The park state is `+0x4ABDBD`. If the saved spot is stale, for example the centre after a mode reset, the cursor comes back "from nowhere". |

## First findings (2026-11-26)

- **Joining a game.** On joining, 38 `WM_MOUSEMOVE` messages, all at one position, reached the window within 5 ms. The low-level hook saw no mouse movement at all. PD2 counted them, switched to mouse mode, and put the cursor in the centre.
- **Most jumps are parking.** Most recorded jumps were PD2 parking the cursor and unparking it. One unpark returned the cursor to that stale centre when A was pressed at the stash.
- **Another source.** Other jumps came from PD2's cursor move called from `+0x29B182`. One went from (552,304) to (273,89) and back 0.5 s later. Which feature that is, is not known yet.

These facts point at possible causes for both bugs. Nothing has shown that any of them is the cause. The trace exists to settle that.

## What is recorded

Events are kept in memory, in a ring of the last 65536.

- **Pad.** The XInput state PD2 is handed: buttons, triggers and sticks. It is logged when something changes, and the line says whether the window was in front. In the background, focus_input.c gives the game an idle pad.
- **Window messages.** Mouse, keyboard, focus and raw-input messages, as PD2's window procedure sees them. Each carries the current mode, the mouse-move count and `GetMessageExtraInfo`.
- **Mode changes.** Every change, with its reason taken from the caller: 40 mouse moves, a mouse button, the pad, focus loss, leaving a game, or the setting.
- **PD2's synthetic input.** Its cursor moves (target and caller), its left and right clicks, and its belt hold.
- **API calls.** `SetCursorPos` from PD2, D2Client, D2Win, D2gfx, D2Launch, Game.exe, BH and the Glide wrapper, each with its caller. `SendInput` from PD2.
- **The mouse itself.** A low-level hook, active while the game is in front. It shows whether a movement came from a device, or was injected by a program such as Steam, Sunshine or an emulator.
- **Every frame.** D2's cursor, the Windows cursor in window pixels, the mode, and the player's mode and left and right skills.
- **Panels.** Which D2 panels are open: D2Client's UI variables at `+0xFAD80`.
- **Controller globals.** Every change in PD2's controller globals, listed under `WATCH` in `src/pad_trace.c`. A word that changes more than 20 times a second is noted once and then left out of the list.

### Client packets

Every packet the client sends is logged. The hook is an entry probe on D2Net #10024, `SendPacket`, which opens with a whole 5-byte `mov eax,[imm32]`. Each line shows the packet id, its length and the first 16 bytes. Item ids at +1 and +5 are resolved to item codes through D2Client's `FindClientSideUnit` (+0xA5B20) and D2Common #10695.

This was added to learn exactly what a manual craft sends, so that one can be automated:
- how an item and a jewel reach the cube;
- how the material tab's runes and gems reach it;
- the Transmute press;
- how the result comes back.

## When a file is written

Files go to `pad-trace\<date>_<time>-<why>.txt` beside the DLL. Each one is written 1.5 s after its trigger. It holds the 20 s before and the 1.5 s after, the controller globals as they stood at the trigger, and a separator line at the moment itself.

| File | Trigger |
|---|---|
| `to-mouse-moves` | PD2 switched to mouse mode after 40 mouse moves. This is also what the "move the mouse a little" fix produces, so the file captures the state just before the fix. |
| `to-mouse-button` | PD2 switched to mouse mode on a mouse button. |
| `jump` | D2's cursor moved at least `jump_px` in one frame while the mouse device did not move. Out of a game, the menu cursor counts, scaled to window pixels. Each jump names its cause: a cursor move PD2 made that frame, with its call site, or a `SetCursorPos`, or nothing seen. PD2 parking the cursor at (115,547) and putting it back are named too, but they write no file, because they happen all the time. |
| `no-cast` | PD2 made a right click outside town, and the player did not enter an attack or cast mode within 700 ms. Written at most once a minute. |
| `mark` | Pause on the keyboard, or Back+Start held on the pad for 1.5 s. Use this when something looks wrong. A mark writes everything the ring still holds, up to five minutes back, so it can be pressed a while after the fact. |

A trigger of the same kind waits 3 s after the last one, except `no-cast`, which waits a minute. Events go on being recorded in between. On 2026-11-26 an 8 s gap swallowed exactly the jump the user saw.

Each trigger also writes a line to `pd2addons.log`, and so does every mode change. The newest `keep` files are kept, 60 by default.

## pad-trace.cfg

```
enabled = 1
jump_px = 120
keep = 60
mark_key = 0x13
```

## Safety

Each of the seven probe sites in PD2 is compared byte for byte with its expected opening before
anything is written. A site that differs is left alone and named in the log.

The stub builder is tested against a stand-in module in `tests/pad_trace_probe_test.c`. Build that
test 32-bit with `gcc -m32 -std=c99 -Isrc`.

The player unit is read with `IsBadReadPtr` guards, because the frames in which a game is torn
down still pass through this code.
