# The controller only while the game is in front

The user switched between PD2 and DuckStation with one pad. A button pressed in one moved the
other (2026-11-24).

Windows gives a pad's state to every program that asks for it. Each program decides for itself
whether to listen while it is in the background.

- **DuckStation** has a setting for this, *Disable Background Input*
  (`DisableBackgroundInput` in `settings.ini`). It is now on.
- **PD2** has none. `ProjectDiablo.json` carries the pad's sensitivity, deadzone, aim assist and
  button style, and nothing about focus.

## What the plugin does

ProjectDiablo.dll reads the pad two ways, and both are redirected through its imports
(`src/focus_input.c`).

| Way | Import | While another window is in front |
|---|---|---|
| XInput | `XINPUT1_4.dll` #2, `XInputGetState`, by ordinal | the state comes back with every button released and every stick centred |
| Raw input (PlayStation pads) | `USER32!RegisterRawInputDevices` | gamepad, joystick and multi-axis registrations lose `RIDEV_INPUTSINK` / `RIDEV_EXINPUTSINK`, so Windows sends their input only to the window in front |

The details:
- Raw-input registrations PD2 made before the hook went in are read back with
  `GetRegisteredRawInputDevices` and registered again without the background flags.
- A button held while switching away reads as released, so nothing stays stuck down.
- "In front" means the foreground window belongs to this process.
- Keyboard and mouse are left alone, because Windows already sends them only to the window in
  front.

`redirect_import` (frame.c) now also takes `"#N"` for an import by ordinal.

`pd2addons.log` shows `focus input: the pad is read only while the game is in front (XInput
gated, raw input gated)` once the hooks are in.
