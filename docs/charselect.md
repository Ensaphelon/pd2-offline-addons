# Straight to the character screen

The game now opens on the character screen in two places where it used to show the main menu:

- when it starts with `-skiptobnet`;
- when you leave a single-player game.

Nothing connects to Battle.net.

`-skiptobnet` comes from the Start-menu shortcut, and from PD2Launcher's own `skiptobnet` setting.
The switch is there for its first half: it skips the title screen and the slow first load of the
main menu.

Its second half is a logon to Battle.net. Here that logon fails with a "Version" dialog. Every
session then went through the main menu to Single Player by hand. "Save and Exit Game" went back
to the main menu too.

## Where the game decides

D2Launch's startup runs each time the game comes back to its menus: at launch, and after every
game. It decides where to go in one place, `D2Launch+0x19E89`, reading the config struct that
Game.exe passes in.

The Battle.net, Open and TCP/IP return paths come first, and they are left alone. After them:

| Case | What D2Launch does |
|---|---|
| `config+0x359` set (`-skiptobnet`) | Reads `Skip To Open`, clears the switch, and marks the title screen shown (`config+0x35B`). Then it calls `+0x19290`, the logon, which builds the main menu as a backdrop and connects on top. |
| Title screen not yet shown | Shows the title screen (`+0x19700`), then the main menu. |
| Otherwise | Calls `+0x17DF0`, the main menu. Leaving a single-player game lands here. |

## What is changed

Two calls are repointed:

| Site | Was | Now |
|---|---|---|
| `D2Launch+0x19EE9` | call to the logon | `instead_of_logon` |
| `D2Launch+0x19F24` | call to the main menu | `instead_of_main_menu` |

Both new targets do what the Single Player button does. They call its handler, `D2Launch+0x1B030`,
named in the main menu's control table under string 5106 "SINGLE PLAYER". The handler sets the
connection type to 0 and opens the character screen, or character creation when there are no
saves on disk.

The title screen stays skipped, and no menu is built only to be thrown away.

## Timing

The patch has to be in place before D2Launch's startup first runs.

PD2 loads D2Launch itself, early, and Game.exe's own `LoadLibraryA` never sees it happen. So two
paths race to patch it, and the first to arrive writes:

- Game.exe's `LoadLibraryA` import, redirected from DllMain;
- `bootstrap`, which looks for the module every 2 ms.

Measured: D2Launch was patched 27 ms after the add-on attached, before its startup began.

## Quick Restart

Quick Restart used to wait for the main menu and press its topmost button. Leaving a game now lands
on the character screen, where the topmost button is something else.

So it now presses Single Player only when that button is on screen. It recognises the button by its
handler, and otherwise goes straight on to the character screen's OK.

## Limits

- **Shift keeps the original behaviour.** At launch, holding Shift keeps the Battle.net logon. On
  leaving a game, it brings back the main menu.
- **Exit reaches the main menu.** Exit on the character screen still goes to the main menu.
- **Fingerprint checks.** Every byte this relies on is compared before anything is written. A
  D2Launch that differs is left alone, with a line in `pd2addons.log`.

## What did not work

**Pressing the button from the frame hook.** An earlier version watched for the main menu from the
frame hook and pressed Single Player once the menu appeared. Under `-skiptobnet` that menu is only
the logon's backdrop. The press therefore landed while the logon was in flight, and the logon's
"Version" dialog then opened on top of the characters.

**Hooking `LoadLibraryA` alone.** The next version relied only on the `LoadLibraryA` hook. It
missed D2Launch entirely, because PD2 had already loaded the module by another route.
