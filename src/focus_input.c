#include <windows.h>
#include <xinput.h>
#include "log.h"

/* The controller only while the game is the window in front.
 *
 * Switching between PD2 and DuckStation with one pad, a button pressed in one moved the other
 * (user, 2026-11-24). Windows hands a pad's state to every program that asks, and each decides for
 * itself whether to listen while in the background. DuckStation has a setting for it ("Disable
 * Background Input"), PD2 has none: ProjectDiablo.json carries the pad's sensitivity, deadzone,
 * aim assist and button style, and nothing about focus.
 *
 * PD2 reads the pad two ways, both imported by ProjectDiablo.dll:
 *
 *   XInput     XINPUT1_4.dll #2, XInputGetState, by ordinal. While another window is in front
 *              the state comes back with every button released and every stick centred, so a
 *              press in DuckStation is never seen and a button held when switching away lets go.
 *   Raw input  RegisterRawInputDevices / GetRawInputData — the way PlayStation pads are read.
 *              Raw input reaches a window in the background only if it registered with
 *              RIDEV_INPUTSINK (or EXINPUTSINK), so that flag is taken off every gamepad, joystick
 *              and multi-axis registration: the ones made from now on, through the import, and the
 *              ones PD2 made before this ran, re-registered without it.
 *
 * Keyboard and mouse are left as they are; Windows already sends those to the window in front. */

void **redirect_import(HMODULE module, const char *dll, const char *symbol, void *with,
                       void **out_original);

typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE *);
typedef BOOL (WINAPI *register_fn)(PCRAWINPUTDEVICE, UINT, UINT);

static get_state_fn real_get_state;
static register_fn real_register;

static BOOL game_in_front(void)
{
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (!front) return FALSE;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
}

/* pad_trace.c records what the pad said, and whether the game was given it. */
void pad_trace_xinput(DWORD user, DWORD result, const XINPUT_STATE *state, BOOL gated);

static DWORD WINAPI on_get_state(DWORD user, XINPUT_STATE *state)
{
    DWORD result = real_get_state(user, state);
    BOOL gated = result == ERROR_SUCCESS && state && !game_in_front();
    pad_trace_xinput(user, result, state, gated);
    if (gated) ZeroMemory(&state->Gamepad, sizeof state->Gamepad);
    return result;
}

/* Generic desktop page: 4 joystick, 5 gamepad, 8 multi-axis controller. */
static BOOL is_pad(const RAWINPUTDEVICE *device)
{
    return device->usUsagePage == 1
        && (device->usUsage == 4 || device->usUsage == 5 || device->usUsage == 8);
}

#define BACKGROUND_FLAGS (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)

static BOOL WINAPI on_register(PCRAWINPUTDEVICE devices, UINT count, UINT size)
{
    RAWINPUTDEVICE copy[16];
    if (!devices || count == 0 || count > 16 || size != sizeof(RAWINPUTDEVICE))
        return real_register(devices, count, size);
    int changed = 0;
    for (UINT i = 0; i < count; i++) {
        copy[i] = devices[i];
        if (is_pad(&copy[i]) && (copy[i].dwFlags & BACKGROUND_FLAGS)) {
            copy[i].dwFlags &= ~BACKGROUND_FLAGS;
            changed++;
        }
    }
    if (changed) log_line("focus input: a pad registration asked for background input; taken off");
    return real_register(copy, count, size);
}

/* Registrations PD2 made before the import was redirected: read back, and any pad one that
   listens in the background is made again without that. */
static void fix_existing_registrations(void)
{
    UINT count = 0;
    if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1 || !count)
        return;
    RAWINPUTDEVICE list[32];
    if (count > 32) count = 32;
    if (GetRegisteredRawInputDevices(list, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1) return;
    for (UINT i = 0; i < count; i++) {
        if (!is_pad(&list[i]) || !(list[i].dwFlags & BACKGROUND_FLAGS)) continue;
        RAWINPUTDEVICE device = list[i];
        device.dwFlags &= ~BACKGROUND_FLAGS;
        BOOL ok = RegisterRawInputDevices(&device, 1, sizeof device);
        log_line("focus input: pad usage %u re-registered without background input (%s)",
                 (unsigned)device.usUsage, ok ? "done" : "refused");
    }
}

/* From bootstrap's loop until ProjectDiablo.dll is there (it is, by the time the D2 modules are). */
BOOL focus_input_install(void)
{
    HMODULE pd2 = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2) return FALSE;

    void *original = NULL;
    if (redirect_import(pd2, "XINPUT1_4.dll", "#2", (void *)on_get_state, &original))
        real_get_state = (get_state_fn)original;
    original = NULL;
    if (redirect_import(pd2, "USER32.dll", "RegisterRawInputDevices", (void *)on_register, &original))
        real_register = (register_fn)original;
    fix_existing_registrations();

    log_line("focus input: the pad is read only while the game is in front (XInput %s, raw input %s)",
             real_get_state ? "gated" : "NOT FOUND", real_register ? "gated" : "NOT FOUND");
    return TRUE;
}
