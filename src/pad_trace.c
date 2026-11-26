#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d2.h"
#include "log.h"

/* Pad trace: a flight recorder for playing PD2 with a controller.
 *
 * Two bugs, both on the Xbox pad (user, 2026-11-26):
 *   1. moving the cursor with the left stick, it sometimes jumps somewhere else in one frame —
 *      to the centre, or up and to the left — with nothing pressed;
 *   2. after a while skills stop casting: a skill button changes the right-click skill icon,
 *      but nothing is cast. Moving the mouse a little and then going back to the pad fixes it.
 *
 * What PD2 does with a pad, read out of ProjectDiablo.dll (season 14):
 *
 *   * It keeps one input mode, a byte at +0x4AC2FE: 1 controller, 0 mouse and keyboard. Pad
 *     activity sets controller (+0x2EBD77 in menus, +0x2EE62F in a game). Its window procedure
 *     (+0x2EBDB0, a subclass of D2's) sets mouse in two ways: 40 WM_MOUSEMOVEs (or touch,
 *     gesture) in a row with no pad activity between them (the count is +0x4AF9BC), or a real
 *     left or right button press (+0x4ABDBC marks PD2's own click so it does not count).
 *   * Every change, and losing focus, goes through +0x2EB860, which resets the controller's
 *     cursor, hover and click state (+0x29C630) and — switching to mouse in a game — puts the
 *     Windows cursor in the middle of the screen with SetCursorPos.
 *   * In controller mode the game's cursor is not the Windows cursor: PD2 moves D2's own by
 *     handing D2's handlers a WM_MOUSEMOVE it made itself (+0x2964E0), and clicks the same way
 *     (+0x296640 left down, +0x296880 left up, +0x296A90 right down). Out of a game it moves
 *     the cursor through D2 instead, and clicks with SendInput.
 *
 * So both bugs could be the mode flipping, or the state that flip resets — but "could" is not
 * evidence. This records what happened, so the next time it happens there is a file to read:
 *
 *   events  the pad state PD2 is handed (focus_input.c calls in), the window messages PD2's
 *           procedure sees, every mode change and why, PD2's own cursor moves and clicks,
 *           SetCursorPos from any of the game's modules, PD2's SendInput, what the mouse itself
 *           did (a low-level hook: real or injected), D2's cursor and the Windows cursor every
 *           frame, the player's mode and skills, and every change in PD2's controller globals.
 *   ring    the last 65536 of them, in memory.
 *   files   pad-trace\<when>-<why>.txt beside the DLL, written 1.5 s after something worth a
 *           look: a switch to mouse mode, D2's cursor jumping in one frame, a right click PD2
 *           made that no cast followed, or a mark — Pause on the keyboard, or Back+Start held
 *           on the pad for 1.5 s. Each holds the 20 s before and the 1.5 s after, and the
 *           controller globals as they stood at that moment.
 *
 * Off unless pad-trace.cfg beside the DLL says `enabled = 1`. */

void **redirect_import(HMODULE module, const char *dll, const char *symbol, void *with,
                       void **out_original);
extern volatile LONG frame_serial;

/* --- Addresses --------------------------------------------------------------------------------- */

/* D2Client, 1.13c — BH's D2Ptrs.h. */
#define D2C_MOUSE_X   0x11B828
#define D2C_MOUSE_Y   0x11B824
#define D2C_PLAYER    0x11BBFC
#define D2C_SCREEN_W  0xDBC48
#define D2C_SCREEN_H  0xDBC4C

#define UNIT_MODE 0x10
#define UNIT_INFO 0xA8
#define INFO_LEFT 0x08
#define INFO_RIGHT 0x0C

/* found: D2Client+0xBE400, BH's GetUiVar_I, is `cmp eax,0x26 / jb / ... mov eax,[eax*4+
   0x6FBAAD80]` — the open panels (inventory, stash, ESC menu, ...) are one DWORD each here. */
#define D2C_UI_VARS   0xFAD80
#define D2C_UI_COUNT  0x26

/* ProjectDiablo.dll, season 14 (5252096 bytes), as RVAs. */
#define PD2_MODE        0x4AC2FE   /* byte: 1 controller, 0 mouse */
#define PD2_PAD_ACTIVE  0x4AC2FD   /* byte: the pad did something this tick */
#define PD2_MOVE_COUNT  0x4AF9BC   /* WM_MOUSEMOVEs since the pad last did; 40 = mouse mode */
#define PD2_OWN_CLICK   0x4ABDBC   /* byte: PD2's own click is in flight */
#define PD2_PARKED      0x4ABDBD   /* byte: +0x296B90 has the cursor parked at (115,547) */

/* The two places +0x296B90 moves the cursor from: parking it, and putting it back where it was
   (return addresses of its calls to the cursor move). */
#define SITE_PARK   0x296BD2
#define SITE_UNPARK 0x296C43

/* The controller's globals, watched for changes and copied into every file. */
typedef struct { DWORD rva, bytes; } watch_block;
static const watch_block WATCH[] = {
    { 0x46A314, 0x04 },   /* +0x46A316 byte: the window has focus */
    { 0x46C72C, 0x04 },
    { 0x4ABD9C, 0x124 },  /* cursor, hover, click and hold state; +0x4ABE88 the hovered unit */
    { 0x4AC2FC, 0x04 },   /* pad active, input mode */
    { 0x4AF990, 0x40 },   /* the mouse-move count at +0x4AF9BC */
    { 0x4BD994, 0x18 },
    { 0x5DCC1C, 0x04 },   /* controller settings: +0x5DCC1E on */
    { 0x5E32C8, 0xC8 },   /* per-game controller state, cleared on leaving a game */
};
#define WATCH_DWORDS 160

/* Entry probes. Each site's first bytes are checked before anything is written. */
enum { P_WNDPROC, P_MODE, P_MOVE, P_LDOWN, P_LUP, P_RDOWN, P_BELT, P_COUNT };
typedef struct { DWORD rva; BYTE stolen; BYTE expect[6]; const char *name; } probe_site;
static const probe_site SITES[P_COUNT] = {
    { 0x2EBDB0, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, "window procedure" },
    { 0x2EB860, 5, { 0xE8, 0x4B, 0xF5, 0xFD, 0xFF, 0x00 }, "mode change" },
    { 0x2964E0, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, "cursor move" },
    { 0x296640, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48 }, "left down" },
    { 0x296880, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x44 }, "left up" },
    { 0x296A90, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C }, "right down" },
    { 0x296B90, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, "belt hold" },
};

/* Every packet the client sends, for learning what a manual craft sends (2026-11-26: to automate
   one, the exact sequence matters — how an item, a jewel and the material tab's runes and gems
   reach the cube, the Transmute press, and how the result comes back). D2Net #10024 is
   SendPacket(len, flags, packet), __stdcall; it opens with a whole 5-byte `mov eax,[imm32]`. */
#define P_SEND 100
#define D2NET_SEND_ORDINAL 10024
#define D2C_FIND_CLIENT_UNIT 0xA5B20   /* BH: UnitAny* __fastcall(id, type) */
#define D2COMMON_ITEM_TEXT 10695        /* BH: ItemText* __stdcall(txt index); the code at +0x80 */

/* Who called the mode change, by return address: the reason. */
typedef struct { DWORD ret; const char *file; const char *why; } mode_reason;
static const mode_reason REASONS[] = {
    { 0x2BE4C9, "to-mouse-moves",    "40 mouse moves with the pad idle" },
    { 0x2BE4F7, "to-mouse-button",   "a mouse button pressed" },
    { 0x2BE548, NULL,                "a raw-input pad moved" },
    { 0x2EBAFB, NULL,                "game reset" },
    { 0x2EBD83, NULL,                "the pad moved (menus)" },
    { 0x2EBDDE, NULL,                "the window lost focus (reset only)" },
    { 0x2EE63B, NULL,                "the pad moved (in game)" },
    { 0x2EE692, "to-mouse-settings", "controller turned off in settings" },
};

/* --- Settings ---------------------------------------------------------------------------------- */

static int cfg_enabled;
static int cfg_jump_px = 120;
static int cfg_keep = 60;
static int cfg_mark_key = VK_PAUSE;

/* What a file covers before its moment. A mark is pressed after something was seen, perhaps a
   while after (2026-11-26: a jump noticed, the game paused, and the moment long out of a 20 s
   window by the time anyone looked), so a mark takes everything the ring still holds, up to
   five minutes. */
#define WINDOW_MS 20000
#define MARK_WINDOW_MS 300000
#define POST_MS 1500
#define CAST_WAIT_MS 700

/* --- The ring ---------------------------------------------------------------------------------- */

enum {
    EV_XINPUT = 1, EV_MSG, EV_MODE, EV_MOVE, EV_CLICK, EV_BELT, EV_SETCURSOR, EV_SENDINPUT,
    EV_DEVICE, EV_FRAME, EV_JUMP, EV_PLAYER, EV_STATE, EV_MARK, EV_NOTE, EV_UI, EV_SEND, EV_SEND_ITEM
};
enum { NOTE_NO_CAST = 1, NOTE_NOISY, NOTE_ALSO, NOTE_START };

typedef struct {
    LONGLONG t;
    volatile LONG seq;
    WORD kind;
    WORD thread;
    DWORD frame;
    DWORD a, b, c, d, e;
} trace_ev;

#define RING_BITS 17
#define RING (1u << RING_BITS)
static trace_ev ring[RING];
static volatile LONG ring_next;
static volatile LONG tracing;
static LONGLONG qpc_freq;

static void put(WORD kind, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
{
    LONG n = InterlockedIncrement(&ring_next) - 1;
    trace_ev *ev = &ring[(DWORD)n & (RING - 1)];
    ev->seq = -1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ev->t = now.QuadPart;
    ev->kind = kind;
    ev->thread = (WORD)GetCurrentThreadId();
    ev->frame = (DWORD)frame_serial;
    ev->a = a; ev->b = b; ev->c = c; ev->d = d; ev->e = e;
    InterlockedExchange(&ev->seq, n);
}

/* --- Small readers ----------------------------------------------------------------------------- */

static BYTE *pd2;
static HMODULE self;
static HWND game_hwnd;

static BYTE pd2_byte(DWORD rva) { return pd2 ? pd2[rva] : 0; }
static DWORD pd2_dword(DWORD rva) { return pd2 ? *(DWORD *)(pd2 + rva) : 0; }

static DWORD pack_xy(int x, int y) { return (DWORD)(WORD)(SHORT)x | ((DWORD)(WORD)(SHORT)y << 16); }
static int px(DWORD v) { return (SHORT)(WORD)v; }
static int py(DWORD v) { return (SHORT)(WORD)(v >> 16); }

static DWORD d2_mouse(void)
{
    if (!d2.d2client) return 0;
    return pack_xy((int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_X), (int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_Y));
}

static DWORD os_cursor_client(void)
{
    POINT p;
    if (!GetCursorPos(&p)) return pack_xy(-1, -1);
    if (game_hwnd) ScreenToClient(game_hwnd, &p);
    return pack_xy(p.x, p.y);
}

static BOOL game_in_front(void)
{
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (!front) return FALSE;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
}

/* The player unit is read every frame, including the frames in which a game is being torn
   down; a pointer that has just been freed must cost a missing line, not the game. */
static BOOL readable(const void *p, UINT_PTR bytes) { return p && !IsBadReadPtr(p, bytes); }

static void *player_unit(void)
{
    void *unit = d2.d2client ? *D2CLIENT_VAR(void *, D2C_PLAYER) : NULL;
    return readable(unit, UNIT_INFO + 4) ? unit : NULL;
}

static DWORD skill_id(void *unit, DWORD slot)
{
    BYTE *info = *(BYTE **)((BYTE *)unit + UNIT_INFO);
    if (!readable(info, 0x10)) return 0xFFFF;
    BYTE *skill = *(BYTE **)(info + slot);
    if (!readable(skill, 4)) return 0xFFFF;
    WORD *txt = *(WORD **)skill;
    return readable(txt, 2) ? *txt : 0xFFFF;
}

/* Which of D2's panels are open, one bit per UI variable (0x00..0x25). */
static void open_panels(DWORD *low, DWORD *high)
{
    *low = *high = 0;
    if (!d2.d2client) return;
    const DWORD *vars = D2CLIENT_VAR(DWORD, D2C_UI_VARS);
    for (int i = 0; i < D2C_UI_COUNT; i++)
        if (vars[i]) {
            if (i < 32) *low |= 1u << i;
            else *high |= 1u << (i - 32);
        }
}

/* Attack, block, cast, throw, kick, the skill sequences. */
static BOOL is_action_mode(DWORD mode)
{
    return (mode >= 7 && mode <= 16) || mode == 18;
}

/* --- Incidents --------------------------------------------------------------------------------- */

static struct {
    volatile LONG state;     /* 0 idle, 1 filling, 2 waiting to be written */
    LONGLONG at;
    SYSTEMTIME wall;
    char why[96];
    char file[32];
    DWORD window_ms;
    DWORD snap[WATCH_DWORDS];
    DWORD mouse, cursor;
} inc;

static DWORD last_trigger[8];
enum { T_TO_MOUSE, T_JUMP, T_NO_CAST, T_MARK };
static const DWORD TRIGGER_GAP_MS[] = { 3000, 3000, 60000, 2000 };

static void snapshot(DWORD *out)
{
    int k = 0;
    for (size_t b = 0; b < sizeof WATCH / sizeof WATCH[0]; b++)
        for (DWORD o = 0; o < WATCH[b].bytes && k < WATCH_DWORDS; o += 4)
            out[k++] = pd2_dword(WATCH[b].rva + o);
}

/* One byte of a snapshot, by its ProjectDiablo.dll offset. */
static BYTE snap_byte(const DWORD *snap, DWORD rva)
{
    int k = 0;
    for (size_t b = 0; b < sizeof WATCH / sizeof WATCH[0]; b++) {
        for (DWORD o = 0; o < WATCH[b].bytes && k < WATCH_DWORDS; o += 4, k++)
            if (rva >= WATCH[b].rva + o && rva < WATCH[b].rva + o + 4)
                return (BYTE)(snap[k] >> (8 * (rva - WATCH[b].rva - o)));
    }
    return 0;
}

static void trigger(int kind, const char *file, const char *why)
{
    DWORD now = GetTickCount();
    if (last_trigger[kind] && now - last_trigger[kind] < TRIGGER_GAP_MS[kind]) return;
    if (InterlockedCompareExchange(&inc.state, 1, 0) != 0) {
        put(EV_NOTE, NOTE_ALSO, (DWORD)kind, 0, 0, 0);
        return;
    }
    last_trigger[kind] = now;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    inc.at = t.QuadPart;
    GetLocalTime(&inc.wall);
    _snprintf(inc.why, sizeof inc.why, "%s", why);
    inc.why[sizeof inc.why - 1] = '\0';
    _snprintf(inc.file, sizeof inc.file, "%s", file);
    inc.file[sizeof inc.file - 1] = '\0';
    inc.window_ms = kind == T_MARK ? MARK_WINDOW_MS : WINDOW_MS;
    snapshot(inc.snap);
    inc.mouse = d2_mouse();
    inc.cursor = os_cursor_client();
    InterlockedExchange(&inc.state, 2);
}

/* --- Pad state, from focus_input.c's XInputGetState ------------------------------------------ */

static XINPUT_GAMEPAD last_pad[4];
static DWORD last_result[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
static BOOL last_gated[4];
static DWORD combo_since[4];
static BOOL combo_fired[4];

static int far_apart(SHORT a, SHORT b) { return abs((int)a - (int)b) > 2000; }

void pad_trace_xinput(DWORD user, DWORD result, const XINPUT_STATE *state, BOOL gated)
{
    if (!tracing || user > 3) return;
    XINPUT_GAMEPAD pad;
    if (result == ERROR_SUCCESS && state) pad = state->Gamepad;
    else ZeroMemory(&pad, sizeof pad);

    XINPUT_GAMEPAD *was = &last_pad[user];
    BOOL changed = result != last_result[user] || gated != last_gated[user]
        || pad.wButtons != was->wButtons
        || abs((int)pad.bLeftTrigger - (int)was->bLeftTrigger) > 16
        || abs((int)pad.bRightTrigger - (int)was->bRightTrigger) > 16
        || far_apart(pad.sThumbLX, was->sThumbLX) || far_apart(pad.sThumbLY, was->sThumbLY)
        || far_apart(pad.sThumbRX, was->sThumbRX) || far_apart(pad.sThumbRY, was->sThumbRY);
    if (changed) {
        put(EV_XINPUT, pad.wButtons | ((DWORD)pad.bLeftTrigger << 16) | ((DWORD)pad.bRightTrigger << 24),
            pack_xy(pad.sThumbLX, pad.sThumbLY), pack_xy(pad.sThumbRX, pad.sThumbRY),
            user | ((DWORD)gated << 8), result);
        *was = pad;
        last_result[user] = result;
        last_gated[user] = gated;
    }

    /* Back + Start held together for 1.5 s: a mark from the couch, once per hold. */
    const WORD combo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    if (gated || (pad.wButtons & combo) != combo) {
        combo_since[user] = 0;
        combo_fired[user] = FALSE;
    } else if (!combo_since[user]) {
        combo_since[user] = GetTickCount() | 1;
    } else if (!combo_fired[user] && GetTickCount() - combo_since[user] >= 1500) {
        combo_fired[user] = TRUE;
        put(EV_MARK, 1, 0, 0, 0, 0);
        trigger(T_MARK, "mark", "mark (Back+Start held on the pad)");
    }
}

/* --- Probes ------------------------------------------------------------------------------------ */

static volatile LONG cast_armed;
static DWORD cast_armed_at, cast_skill;

/* The last cursor move PD2 made and the last SetCursorPos, so a jump can say what did it. */
static DWORD last_move_to, last_move_site, last_move_frame = 0xFFFFFF00;
static DWORD last_set_cursor_frame = 0xFFFFFF00, last_set_cursor_caller;
static DWORD raw_last_tick, raw_skipped;
static HANDLE raw_last_device;

static BOOL interesting_message(DWORD msg)
{
    return msg == WM_ACTIVATE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ACTIVATEAPP
        || msg == WM_INPUT_DEVICE_CHANGE || msg == WM_INPUT
        || (msg >= WM_KEYDOWN && msg <= WM_SYSKEYUP)
        || (msg >= WM_MOUSEMOVE && msg <= 0x20E)
        || msg == WM_CAPTURECHANGED || msg == 0x2A3 /* WM_MOUSELEAVE */
        || msg == 0x240 /* WM_TOUCH */ || msg == 0x119 /* WM_GESTURE */;
}

static void on_window_message(HWND hwnd, DWORD msg, DWORD wp, DWORD lp)
{
    if (!game_hwnd && hwnd) game_hwnd = hwnd;
    if (!interesting_message(msg)) return;
    DWORD mode = pd2_byte(PD2_MODE);
    DWORD count = pd2_dword(PD2_MOVE_COUNT);
    if (count > 255) count = 255;
    DWORD extra = 0, info = 0;
    if (msg >= WM_MOUSEMOVE && msg <= 0x20E) extra = (DWORD)GetMessageExtraInfo();
    if (msg == WM_INPUT) {
        RAWINPUTHEADER h;
        UINT size = sizeof h;
        if (GetRawInputData((HRAWINPUT)(UINT_PTR)lp, RID_HEADER, &h, &size, sizeof h) == sizeof h) {
            /* A pad sends hundreds of these a second; one in 100 ms per device is plenty. */
            DWORD now = GetTickCount();
            if (h.hDevice == raw_last_device && now - raw_last_tick < 100) { raw_skipped++; return; }
            raw_last_device = h.hDevice;
            raw_last_tick = now;
            extra = (DWORD)(UINT_PTR)h.hDevice;
            info = h.dwType | (raw_skipped << 8);
            raw_skipped = 0;
        }
    }
    put(EV_MSG, msg | (mode << 16) | (count << 24), wp, lp, extra, info);
}

static const mode_reason *reason_for(DWORD ret_rva)
{
    for (size_t i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++)
        if (REASONS[i].ret == ret_rva) return &REASONS[i];
    return NULL;
}

static void on_mode_change(DWORD ret_rva)
{
    DWORD mode = pd2_byte(PD2_MODE);
    put(EV_MODE, mode, ret_rva, d2_mouse(), os_cursor_client(), 0);
    const mode_reason *r = reason_for(ret_rva);
    if (r && r->ret == 0x2EBDDE) return;   /* focus changes are frequent and change no mode */
    log_line("pad trace: input mode -> %s (%s)", mode ? "controller" : "mouse",
             r ? r->why : "an unknown caller");
    if (!mode && (!r || r->file)) {
        char why[96];
        _snprintf(why, sizeof why, "switch to mouse mode: %s", r ? r->why : "unknown caller");
        why[sizeof why - 1] = '\0';
        trigger(T_TO_MOUSE, r ? r->file : "to-mouse-unknown", why);
    }
}

static void on_click(DWORD which, DWORD ecx, DWORD edx, DWORD arg, DWORD ret_rva)
{
    put(EV_CLICK, which, (ecx & 0xFF) | ((edx & 0xFF) << 8) | ((arg & 0xFF) << 16), d2_mouse(),
        pd2_byte(PD2_MODE), ret_rva);
    if (which != P_RDOWN || cast_armed) return;
    void *unit = player_unit();
    if (!unit) return;
    DWORD mode = *(DWORD *)((BYTE *)unit + UNIT_MODE);
    if (mode == 5 || mode == 6) return;   /* in town nothing is cast anyway */
    cast_armed_at = GetTickCount();
    cast_skill = skill_id(unit, INFO_RIGHT);
    InterlockedExchange(&cast_armed, 1);
}

typedef void *(__fastcall *find_unit_fn)(DWORD id, DWORD type);
typedef BYTE *(__stdcall *item_text_fn)(DWORD index);
static find_unit_fn find_client_unit;
static item_text_fn item_text;

/* An item the client knows by this id, as its four-letter code; 0 if there is none. */
static DWORD item_code(DWORD id)
{
    if (!find_client_unit || !item_text || !id || id == 0xFFFFFFFF) return 0;
    BYTE *unit = find_client_unit(id, 4);
    if (!readable(unit, 0x10) || *(DWORD *)unit != 4) return 0;
    BYTE *txt = item_text(*(DWORD *)(unit + 4));
    return readable(txt + 0x80, 4) ? *(DWORD *)(txt + 0x80) : 0;
}

static void on_send(DWORD len, const BYTE *packet)
{
    DWORD w[4] = { 0, 0, 0, 0 };
    if (!readable(packet, 1)) return;
    DWORD n = len < 16 ? len : 16;
    if (readable(packet, n)) memcpy(w, packet, n);
    put(EV_SEND, len, w[0], w[1], w[2], w[3]);
    /* Item ids sit at +1 in most item packets and at +5 in the rest. */
    DWORD at1 = 0, at5 = 0;
    if (len >= 5) memcpy(&at1, packet + 1, 4);
    if (len >= 9) memcpy(&at5, packet + 5, 4);
    DWORD c1 = item_code(at1), c5 = item_code(at5);
    if (c1 || c5) put(EV_SEND_ITEM, packet[0], at1, c1, at5, c5);
}

static void __stdcall probe_entry(DWORD id, DWORD *frame)
{
    /* frame: [0] eflags, [1] edi, [2] esi, [3] ebp, [4] esp, [5] ebx, [6] edx, [7] ecx, [8] eax,
       [9] the return address, [10..] the stack arguments. */
    DWORD saved_error = GetLastError();
    DWORD ret_rva = frame[9] - (DWORD)pd2;
    switch (id) {
    case P_WNDPROC: on_window_message((HWND)frame[10], frame[11], frame[12], frame[13]); break;
    case P_MODE:    on_mode_change(ret_rva); break;
    case P_MOVE: {
        /* PD2 hands D2 a move every tick the stick is held; only the ones that move anything. */
        DWORD to = pack_xy((int)frame[7], (int)frame[6]), now = d2_mouse();
        last_move_to = to;
        last_move_site = ret_rva;
        last_move_frame = (DWORD)frame_serial;
        if (to != now) put(EV_MOVE, to, now, pd2_byte(PD2_MODE), 0, ret_rva);
        break;
    }
    case P_LDOWN: case P_LUP: case P_RDOWN:
        on_click(id, frame[7], frame[6], frame[10], ret_rva);
        break;
    case P_BELT: {
        /* Asked every tick; it only acts on a park while unparked or an unpark while parked. */
        BOOL park = (frame[7] & 0xFF) != 0;
        if (park != (pd2_byte(PD2_PARKED) != 0))
            put(EV_BELT, (DWORD)park, frame[6] & 0xFF, d2_mouse(), 0, ret_rva);
        break;
    }
    case P_SEND: on_send(frame[10], (const BYTE *)frame[12]); break;
    }
    SetLastError(saved_error);
}

static BYTE *stubs;
static int stubs_used;

/* An entry probe on `fn`: the first `expect_len` bytes must be `expect`, and the first `stolen`
   bytes must be whole instructions that run the same anywhere (or one relative call or jump). */
static BOOL install_probe_at(DWORD id, BYTE *fn, const BYTE *expect, int expect_len, int stolen,
                             const char *name)
{
    if (!fn || memcmp(fn, expect, (size_t)expect_len) != 0) {
        log_line("pad trace: %s does not open as expected -- not traced", name);
        return FALSE;
    }
    if (!stubs) {
        stubs = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!stubs) return FALSE;
    }
    BYTE *q = stubs + stubs_used;
    BYTE *p = q;
    *p++ = 0x60;                              /* pushad */
    *p++ = 0x9C;                              /* pushfd */
    *p++ = 0x89; *p++ = 0xE0;                 /* mov eax, esp */
    *p++ = 0x50;                              /* push eax */
    *p++ = 0x68; memcpy(p, &(DWORD){ (DWORD)id }, 4); p += 4;   /* push id */
    *p++ = 0xE8;                              /* call probe_entry */
    LONG rel = (LONG)((BYTE *)probe_entry - (p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x9D;                              /* popfd */
    *p++ = 0x61;                              /* popad */
    if ((fn[0] == 0xE8 || fn[0] == 0xE9) && stolen == 5) {
        /* A relative call or jump moves with its target, not with us. */
        BYTE *target = fn + 5 + *(LONG *)(fn + 1);
        *p++ = fn[0];
        rel = (LONG)(target - (p + 4));
        memcpy(p, &rel, 4); p += 4;
    } else {
        memcpy(p, fn, stolen); p += stolen;
    }
    *p++ = 0xE9;                              /* jmp back */
    rel = (LONG)((fn + stolen) - (p + 4));
    memcpy(p, &rel, 4); p += 4;
    stubs_used += 48;
    FlushInstructionCache(GetCurrentProcess(), q, (SIZE_T)(p - q));

    DWORD was;
    if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &was)) return FALSE;
    long long old = *(volatile long long *)fn, patched = old;
    BYTE *b = (BYTE *)&patched;
    b[0] = 0xE9;
    rel = (LONG)(q - (fn + 5));
    memcpy(b + 1, &rel, 4);
    for (int i = 5; i < stolen; i++) b[i] = 0x90;
    __sync_val_compare_and_swap((volatile long long *)fn, old, patched);
    VirtualProtect(fn, 8, was, &was);
    FlushInstructionCache(GetCurrentProcess(), fn, 8);
    return TRUE;
}

static BOOL install_probe(int id)
{
    const probe_site *s = &SITES[id];
    char name[80];
    _snprintf(name, sizeof name, "%s at ProjectDiablo+0x%lX", s->name, (unsigned long)s->rva);
    name[sizeof name - 1] = '\0';
    return install_probe_at((DWORD)id, pd2 + s->rva, s->expect, s->stolen, s->stolen, name);
}

/* --- SetCursorPos from any of the game's modules, SendInput from PD2 -------------------------- */

typedef BOOL (WINAPI *set_cursor_fn)(int, int);
typedef UINT (WINAPI *send_input_fn)(UINT, LPINPUT, int);

static void note_set_cursor(int x, int y, DWORD caller)
{
    last_set_cursor_frame = (DWORD)frame_serial;
    last_set_cursor_caller = caller;
    put(EV_SETCURSOR, pack_xy(x, y), d2_mouse(), pd2_byte(PD2_MODE), 0, caller);
}

/* One hook per module, so each calls on to whatever its own import pointed at (a wrapper such
   as the Glide one may have its own SetCursorPos there). */
#define SC_SLOT(n) \
    static set_cursor_fn sc_next_##n; \
    static BOOL WINAPI sc_hook_##n(int x, int y) \
    { note_set_cursor(x, y, (DWORD)__builtin_return_address(0)); return sc_next_##n(x, y); }
SC_SLOT(0) SC_SLOT(1) SC_SLOT(2) SC_SLOT(3) SC_SLOT(4) SC_SLOT(5) SC_SLOT(6) SC_SLOT(7) SC_SLOT(8)

static struct { const char *module; set_cursor_fn *next; void *hook; int state; } sc_slots[] = {
    { "ProjectDiablo.dll", &sc_next_0, (void *)sc_hook_0, 0 },
    { "D2Client.dll",      &sc_next_1, (void *)sc_hook_1, 0 },
    { "D2Win.dll",         &sc_next_2, (void *)sc_hook_2, 0 },
    { "D2gfx.dll",         &sc_next_3, (void *)sc_hook_3, 0 },
    { "D2Launch.dll",      &sc_next_4, (void *)sc_hook_4, 0 },
    { "",                  &sc_next_5, (void *)sc_hook_5, 0 },   /* Game.exe */
    { "BH.dll",            &sc_next_6, (void *)sc_hook_6, 0 },
    { "glide3x.dll",       &sc_next_7, (void *)sc_hook_7, 0 },
    { "D2Common.dll",      &sc_next_8, (void *)sc_hook_8, 0 },
};
#define SC_COUNT (sizeof sc_slots / sizeof sc_slots[0])

/* 0 not loaded yet, 1 hooked, 2 loaded but does not import it. */
static void hook_set_cursor(void)
{
    for (size_t i = 0; i < SC_COUNT; i++) {
        if (sc_slots[i].state) continue;
        HMODULE m = sc_slots[i].module[0] ? GetModuleHandleA(sc_slots[i].module) : GetModuleHandleA(NULL);
        if (!m) continue;
        void *next = NULL;
        if (redirect_import(m, "USER32.dll", "SetCursorPos", sc_slots[i].hook, &next) && next) {
            *sc_slots[i].next = (set_cursor_fn)next;
            sc_slots[i].state = 1;
            log_line("pad trace: SetCursorPos traced in %s",
                     sc_slots[i].module[0] ? sc_slots[i].module : "Game.exe");
        } else {
            sc_slots[i].state = 2;
        }
    }
}

static send_input_fn send_input_next;

static UINT WINAPI on_send_input(UINT count, LPINPUT inputs, int size)
{
    DWORD type = 0xFF, flags = 0, dx = 0, dy = 0;
    if (count && inputs) {
        type = inputs[0].type;
        if (type == INPUT_MOUSE) {
            flags = inputs[0].mi.dwFlags;
            dx = (DWORD)inputs[0].mi.dx;
            dy = (DWORD)inputs[0].mi.dy;
        } else if (type == INPUT_KEYBOARD) {
            flags = inputs[0].ki.dwFlags;
            dx = inputs[0].ki.wVk;
        }
    }
    put(EV_SENDINPUT, count | (type << 16), flags, dx, dy, (DWORD)__builtin_return_address(0));
    return send_input_next(count, inputs, size);
}

/* --- What the mouse itself did --------------------------------------------------------------- */

static volatile DWORD last_device_move;

static LRESULT CALLBACK device_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && game_in_front()) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
        put(EV_DEVICE, (DWORD)wp, pack_xy(m->pt.x, m->pt.y), m->flags, (DWORD)m->dwExtraInfo,
            m->mouseData);
        if (wp == WM_MOUSEMOVE) last_device_move = GetTickCount();
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI device_thread(LPVOID unused)
{
    (void)unused;
    HHOOK hook = SetWindowsHookExA(WH_MOUSE_LL, device_proc, self, 0);
    log_line("pad trace: mouse device hook %s", hook ? "in" : "REFUSED");
    if (!hook) return 0;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* --- Every frame ------------------------------------------------------------------------------- */

static DWORD prev_mouse = 0xFFFFFFFF, prev_cursor, prev_flags;
static DWORD prev_unit_mode = 0xFFFFFFFF, prev_right = 0xFFFFFFFF, prev_left = 0xFFFFFFFF;
static DWORD prev_ui_low = 0xFFFFFFFF, prev_ui_high = 0xFFFFFFFF;
static DWORD shadow[WATCH_DWORDS];
static WORD changes[WATCH_DWORDS];
static BYTE noisy[WATCH_DWORDS];
static DWORD noise_window;
static BOOL shadow_ready;

static void watch_state(void)
{
    DWORD now = GetTickCount();
    if (now - noise_window >= 1000) {
        noise_window = now;
        memset(changes, 0, sizeof changes);
    }
    DWORD cur[WATCH_DWORDS];
    snapshot(cur);
    int k = 0, logged = 0;
    for (size_t b = 0; b < sizeof WATCH / sizeof WATCH[0]; b++) {
        for (DWORD o = 0; o < WATCH[b].bytes && k < WATCH_DWORDS; o += 4, k++) {
            if (!shadow_ready || cur[k] == shadow[k]) continue;
            if (!noisy[k] && ++changes[k] > 20) {
                noisy[k] = 1;
                put(EV_NOTE, NOTE_NOISY, WATCH[b].rva + o, 0, 0, 0);
            }
            if (!noisy[k] && logged < 48) {
                put(EV_STATE, WATCH[b].rva + o, shadow[k], cur[k], 0, 0);
                logged++;
            }
        }
    }
    memcpy(shadow, cur, sizeof shadow);
    shadow_ready = TRUE;
}

void pad_trace_frame(void)
{
    if (!tracing) return;
    DWORD mouse = d2_mouse();
    DWORD cursor = os_cursor_client();
    DWORD count = pd2_dword(PD2_MOVE_COUNT);
    if (count > 255) count = 255;
    DWORD flags = pd2_byte(PD2_MODE) | ((DWORD)pd2_byte(PD2_PAD_ACTIVE) << 1)
                | ((DWORD)game_in_front() << 2) | (count << 8);

    void *unit = player_unit();
    static BOOL prev_in_game;
    BOOL same_place = (unit != NULL) == prev_in_game;   /* joining or leaving is not a jump */
    prev_in_game = unit != NULL;

    /* In a game the cursor is D2Client's, in game pixels. Out of one D2Client's is not used: the
       menus follow the Windows cursor, which is in window pixels, so the limit is scaled up. */
    if (prev_mouse != 0xFFFFFFFF && same_place && (unit || (flags & 4))) {
        DWORD from = unit ? prev_mouse : prev_cursor, to = unit ? mouse : cursor;
        int limit = cfg_jump_px;
        RECT rc;
        DWORD sw = *D2CLIENT_VAR(DWORD, D2C_SCREEN_W);
        if (!unit && game_hwnd && sw && GetClientRect(game_hwnd, &rc) && rc.right > 0)
            limit = (int)((LONGLONG)cfg_jump_px * rc.right / (LONG)sw);
        int dx = px(to) - px(from), dy = py(to) - py(from);
        int step = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (step >= limit) {
            BOOL device = GetTickCount() - last_device_move < 150;
            /* What did it: a cursor move PD2 made this frame or the last one that lands here, or a
               SetCursorPos from any module. 0 is nothing seen. */
            DWORD serial = (DWORD)frame_serial, cause = 0;
            if (serial - last_move_frame <= 1 && abs(px(last_move_to) - px(to)) <= 3
                && abs(py(last_move_to) - py(to)) <= 3)
                cause = last_move_site;
            else if (serial - last_set_cursor_frame <= 2)
                cause = 0x80000000u | 1;
            BOOL parking = cause == SITE_PARK || cause == SITE_UNPARK;
            put(EV_JUMP, from, to, (DWORD)step, (DWORD)device | (unit ? 0 : 2), cause);
            if (!device && !parking) {
                char why[96];
                if (!cause) _snprintf(why, sizeof why, "%s jumped, nothing PD2 did explains it",
                                      unit ? "D2's cursor" : "the menu cursor");
                else if (cause & 0x80000000u) _snprintf(why, sizeof why, "%s jumped after a SetCursorPos",
                                                        unit ? "D2's cursor" : "the menu cursor");
                else _snprintf(why, sizeof why, "D2's cursor jumped: PD2 moved it from +0x%lX",
                               (unsigned long)cause);
                why[sizeof why - 1] = '\0';
                trigger(T_JUMP, "jump", why);
            }
        }
    }
    if (mouse != prev_mouse || cursor != prev_cursor || flags != prev_flags)
        put(EV_FRAME, mouse, cursor, flags, 0, 0);
    prev_mouse = mouse;
    prev_cursor = cursor;
    prev_flags = flags;

    DWORD mode = 0xFFFFFFFF, right = 0xFFFFFFFF, left = 0xFFFFFFFF;
    if (unit) {
        mode = *(DWORD *)((BYTE *)unit + UNIT_MODE);
        right = skill_id(unit, INFO_RIGHT);
        left = skill_id(unit, INFO_LEFT);
    }
    if (mode != prev_unit_mode || right != prev_right || left != prev_left)
        put(EV_PLAYER, mode, right, left, 0, 0);
    prev_unit_mode = mode;
    prev_right = right;
    prev_left = left;

    DWORD ui_low, ui_high;
    open_panels(&ui_low, &ui_high);
    if (ui_low != prev_ui_low || ui_high != prev_ui_high) put(EV_UI, ui_low, ui_high, 0, 0, 0);
    prev_ui_low = ui_low;
    prev_ui_high = ui_high;

    if (cast_armed) {
        if (!unit || is_action_mode(mode)) {
            InterlockedExchange(&cast_armed, 0);
        } else if (GetTickCount() - cast_armed_at > CAST_WAIT_MS) {
            InterlockedExchange(&cast_armed, 0);
            put(EV_NOTE, NOTE_NO_CAST, cast_skill, right, mode, 0);
            trigger(T_NO_CAST, "no-cast", "PD2 right-clicked and nothing was cast");
        }
    }

    watch_state();
}

/* --- Writing a file ---------------------------------------------------------------------------- */

static const char *message_name(DWORD msg)
{
    switch (msg) {
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
    case WM_INPUT_DEVICE_CHANGE: return "WM_INPUT_DEVICE_CHANGE";
    case WM_INPUT: return "WM_INPUT";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_CHAR: return "WM_CHAR";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP: return "WM_SYSKEYUP";
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
    case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MBUTTONDOWN: return "WM_MBUTTONDOWN";
    case WM_MBUTTONUP: return "WM_MBUTTONUP";
    case 0x20A: return "WM_MOUSEWHEEL";
    case 0x20B: return "WM_XBUTTONDOWN";
    case 0x20C: return "WM_XBUTTONUP";
    case 0x20E: return "WM_MOUSEHWHEEL";
    case WM_CAPTURECHANGED: return "WM_CAPTURECHANGED";
    case 0x2A3: return "WM_MOUSELEAVE";
    case 0x240: return "WM_TOUCH";
    case 0x119: return "WM_GESTURE";
    default: return NULL;
    }
}

static void buttons_text(WORD w, char *out, size_t size)
{
    static const struct { WORD bit; const char *name; } B[] = {
        { XINPUT_GAMEPAD_A, "A" }, { XINPUT_GAMEPAD_B, "B" }, { XINPUT_GAMEPAD_X, "X" },
        { XINPUT_GAMEPAD_Y, "Y" }, { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" }, { XINPUT_GAMEPAD_BACK, "Back" },
        { XINPUT_GAMEPAD_START, "Start" }, { XINPUT_GAMEPAD_LEFT_THUMB, "LS" },
        { XINPUT_GAMEPAD_RIGHT_THUMB, "RS" }, { XINPUT_GAMEPAD_DPAD_UP, "Up" },
        { XINPUT_GAMEPAD_DPAD_DOWN, "Down" }, { XINPUT_GAMEPAD_DPAD_LEFT, "Left" },
        { XINPUT_GAMEPAD_DPAD_RIGHT, "Right" },
    };
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        if (!(w & B[i].bit)) continue;
        int k = _snprintf(out + n, size - n, "%s%s", n ? "+" : "", B[i].name);
        if (k < 0) break;
        n += (size_t)k;
    }
    if (!n) _snprintf(out, size, "-");
    out[size - 1] = '\0';
}

static void where(DWORD address, char *out, size_t size)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      (LPCSTR)(UINT_PTR)address, &m) && m
        && GetModuleFileNameA(m, path, MAX_PATH)) {
        const char *name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        _snprintf(out, size, "%s+0x%lX", name, (unsigned long)(address - (DWORD)(UINT_PTR)m));
    } else {
        _snprintf(out, size, "0x%08lX", (unsigned long)address);
    }
    out[size - 1] = '\0';
}

static const char *player_mode_name(DWORD m)
{
    static const char *N[] = { "death", "neutral", "walk", "run", "get hit", "town neutral",
        "town walk", "attack 1", "attack 2", "block", "cast", "throw", "kick", "skill 1",
        "skill 2", "skill 3", "skill 4", "dead", "sequence", "knockback" };
    return m < sizeof N / sizeof N[0] ? N[m] : "?";
}

static const char *click_name(DWORD which)
{
    return which == P_LDOWN ? "left down" : which == P_LUP ? "left up" : "right down";
}

/* Names for the client packets a craft is likely to involve (1.13c numbering). */
static const char *packet_name(BYTE id)
{
    switch (id) {
    case 0x16: return "(pick up from ground)";
    case 0x17: return "(drop to ground)";
    case 0x18: return "(put in buffer x,y)";
    case 0x19: return "(pick from buffer)";
    case 0x1A: return "(equip)";
    case 0x1C: return "(unequip)";
    case 0x1F: return "(swap in buffer)";
    case 0x20: return "(use item)";
    case 0x21: return "(stack)";
    case 0x22: return "(unstack)";
    case 0x27: return "(identify)";
    case 0x28: return "(socket)";
    case 0x2A: return "(item to cube)";
    case 0x3C: return "(select skill)";
    case 0x4F: return "(button)";
    case 0x5C: return "(PD2 custom)";
    default: return "";
    }
}

static void format_event(const trace_ev *ev, char *s, size_t size)
{
    char a[64];
    switch (ev->kind) {
    case EV_XINPUT:
        buttons_text((WORD)ev->a, a, sizeof a);
        _snprintf(s, size, "pad%lu  %-14s LT=%3lu RT=%3lu  L(%6d,%6d) R(%6d,%6d)%s%s",
                  (unsigned long)(ev->d & 0xFF), a, (unsigned long)((ev->a >> 16) & 0xFF),
                  (unsigned long)(ev->a >> 24), px(ev->b), py(ev->b), px(ev->c), py(ev->c),
                  ev->e ? "  NOT CONNECTED" : "",
                  (ev->d >> 8) & 1 ? "  [window not in front: the game was given nothing]" : "");
        break;
    case EV_MSG: {
        DWORD msg = ev->a & 0xFFFF;
        const char *name = message_name(msg);
        char head[48];
        if (name) _snprintf(head, sizeof head, "%s", name);
        else _snprintf(head, sizeof head, "msg 0x%lX", (unsigned long)msg);
        head[sizeof head - 1] = '\0';
        char tail[48];
        _snprintf(tail, sizeof tail, "  | mode=%s count=%lu",
                  ((ev->a >> 16) & 0xFF) ? "pad" : "mouse", (unsigned long)(ev->a >> 24));
        tail[sizeof tail - 1] = '\0';
        if (msg >= WM_MOUSEMOVE && msg <= 0x20E)
            _snprintf(s, size, "window: %-16s at (%d,%d) keys=0x%lX extra=0x%lX%s", head,
                      px(ev->c), py(ev->c), (unsigned long)ev->b, (unsigned long)ev->d, tail);
        else if (msg == WM_INPUT)
            _snprintf(s, size, "window: %-16s type=%lu device=0x%lX (+%lu more in 100 ms)%s", head,
                      (unsigned long)(ev->e & 0xFF), (unsigned long)ev->d,
                      (unsigned long)(ev->e >> 8), tail);
        else if (msg >= WM_KEYDOWN && msg <= WM_SYSKEYUP)
            _snprintf(s, size, "window: %-16s vk=0x%02lX%s", head, (unsigned long)ev->b, tail);
        else
            _snprintf(s, size, "window: %-16s wp=0x%lX lp=0x%lX%s", head,
                      (unsigned long)ev->b, (unsigned long)ev->c, tail);
        break;
    }
    case EV_MODE: {
        const mode_reason *r = reason_for(ev->b);
        _snprintf(s, size, "INPUT MODE -> %s  (%s; from ProjectDiablo+0x%lX)  D2 cursor (%d,%d)  "
                  "Windows cursor (%d,%d)", ev->a ? "CONTROLLER" : "MOUSE",
                  r ? r->why : "unknown caller", (unsigned long)ev->b,
                  px(ev->c), py(ev->c), px(ev->d), py(ev->d));
        break;
    }
    case EV_MOVE:
        _snprintf(s, size, "PD2 moves D2's cursor to (%d,%d), was (%d,%d)  mode=%s  from ProjectDiablo+0x%lX",
                  px(ev->a), py(ev->a), px(ev->b), py(ev->b), ev->c ? "pad" : "mouse",
                  (unsigned long)ev->e);
        break;
    case EV_CLICK:
        _snprintf(s, size, "PD2 %s  shift=%lu ctrl=%lu arg=%lu  at (%d,%d)  mode=%s  from ProjectDiablo+0x%lX",
                  click_name(ev->a), (unsigned long)(ev->b & 0xFF), (unsigned long)((ev->b >> 8) & 0xFF),
                  (unsigned long)((ev->b >> 16) & 0xFF), px(ev->c), py(ev->c),
                  ev->d ? "pad" : "mouse", (unsigned long)ev->e);
        break;
    case EV_BELT:
        _snprintf(s, size, "PD2 %s  cursor was at (%d,%d)  from ProjectDiablo+0x%lX",
                  ev->a ? "PARKS the cursor at (115,547), keeping where it was"
                        : ev->b ? "UNPARKS the cursor, back to where it was" : "unparks, leaving the cursor",
                  px(ev->c), py(ev->c), (unsigned long)ev->e);
        break;
    case EV_SETCURSOR:
        where(ev->e, a, sizeof a);
        _snprintf(s, size, "SetCursorPos(%d,%d) screen  D2 cursor (%d,%d)  mode=%s  from %s",
                  px(ev->a), py(ev->a), px(ev->b), py(ev->b), ev->c ? "pad" : "mouse", a);
        break;
    case EV_SENDINPUT:
        where(ev->e, a, sizeof a);
        _snprintf(s, size, "SendInput n=%lu type=%lu flags=0x%lX dx=%ld dy=%ld  from %s",
                  (unsigned long)(ev->a & 0xFFFF), (unsigned long)(ev->a >> 16),
                  (unsigned long)ev->b, (long)ev->c, (long)ev->d, a);
        break;
    case EV_DEVICE: {
        const char *name = message_name(ev->a);
        _snprintf(s, size, "MOUSE DEVICE: %s at screen (%d,%d)%s%s extra=0x%lX",
                  name ? name : "?", px(ev->b), py(ev->b),
                  (ev->c & LLMHF_INJECTED) ? "  INJECTED by a program" : "  physical",
                  (ev->c & LLMHF_LOWER_IL_INJECTED) ? " (lower integrity)" : "",
                  (unsigned long)ev->d);
        break;
    }
    case EV_FRAME:
        _snprintf(s, size, "frame: D2 cursor (%d,%d)  Windows cursor (%d,%d) in the window  mode=%s%s%s count=%lu",
                  px(ev->a), py(ev->a), px(ev->b), py(ev->b), (ev->c & 1) ? "pad" : "mouse",
                  (ev->c & 2) ? " pad-active" : "", (ev->c & 4) ? "" : " NOT-IN-FRONT",
                  (unsigned long)((ev->c >> 8) & 0xFF));
        break;
    case EV_JUMP: {
        char cause[96];
        if (!ev->e) _snprintf(cause, sizeof cause, "nothing PD2 did explains it");
        else if (ev->e & 0x80000000u) _snprintf(cause, sizeof cause, "after a SetCursorPos");
        else if (ev->e == SITE_PARK) _snprintf(cause, sizeof cause, "PD2 parked it at (115,547)");
        else if (ev->e == SITE_UNPARK) _snprintf(cause, sizeof cause, "PD2 put it back from the park");
        else _snprintf(cause, sizeof cause, "PD2 moved it from ProjectDiablo+0x%lX", (unsigned long)ev->e);
        cause[sizeof cause - 1] = '\0';
        _snprintf(s, size, "JUMP: %s (%d,%d) -> (%d,%d) in one frame, %lu px; %s%s",
                  (ev->d & 2) ? "menu (Windows) cursor" : "D2 cursor",
                  px(ev->a), py(ev->a), px(ev->b), py(ev->b), (unsigned long)ev->c, cause,
                  (ev->d & 1) ? "  (the mouse was moving)" : "  (the mouse was NOT moving)");
        break;
    }
    case EV_PLAYER:
        if (ev->a == 0xFFFFFFFF) _snprintf(s, size, "player: none (not in a game)");
        else _snprintf(s, size, "player: mode %lu %s  right skill %lu  left skill %lu",
                       (unsigned long)ev->a, player_mode_name(ev->a), (unsigned long)ev->b,
                       (unsigned long)ev->c);
        break;
    case EV_STATE:
        _snprintf(s, size, "PD2 +0x%06lX: %08lX -> %08lX", (unsigned long)ev->a,
                  (unsigned long)ev->b, (unsigned long)ev->c);
        break;
    case EV_MARK:
        _snprintf(s, size, "MARK (%s)", ev->a ? "Back+Start on the pad" : "Pause key");
        break;
    case EV_NOTE:
        if (ev->a == NOTE_NO_CAST)
            _snprintf(s, size, "NO CAST: PD2 right-clicked with skill %lu and the player stayed in "
                      "mode %lu %s for %d ms (right skill now %lu)", (unsigned long)ev->b,
                      (unsigned long)ev->d, player_mode_name(ev->d), CAST_WAIT_MS,
                      (unsigned long)ev->c);
        else if (ev->a == NOTE_NOISY)
            _snprintf(s, size, "PD2 +0x%06lX changes more than 20 times a second; not logged further",
                      (unsigned long)ev->b);
        else if (ev->a == NOTE_ALSO)
            _snprintf(s, size, "(another trigger, kind %lu, while this file was pending)",
                      (unsigned long)ev->b);
        else
            _snprintf(s, size, "trace started");
        break;
    case EV_SEND: {
        const BYTE *b = (const BYTE *)&ev->b;   /* b, c, d, e: the first 16 bytes, in order */
        DWORD n = ev->a < 16 ? ev->a : 16;
        size_t k = (size_t)_snprintf(s, size, "SEND 0x%02X %-22s %3lu bytes:", b[0], packet_name(b[0]),
                                     (unsigned long)ev->a);
        for (DWORD i = 0; i < n && k + 4 < size; i++) k += (size_t)_snprintf(s + k, size - k, " %02x", b[i]);
        if (ev->a > 16 && k + 4 < size) _snprintf(s + k, size - k, " ...");
        break;
    }
    case EV_SEND_ITEM: {
        char c1[5] = { 0 }, c5[5] = { 0 };
        memcpy(c1, &ev->c, 4);
        memcpy(c5, &ev->e, 4);
        _snprintf(s, size, "     items in it: +1 id %lu = '%s'   +5 id %lu = '%s'",
                  (unsigned long)ev->b, ev->c ? c1 : "-", (unsigned long)ev->d, ev->e ? c5 : "-");
        break;
    }
    case EV_UI: {
        size_t n = (size_t)_snprintf(s, size, "D2 panels open (UI variables):");
        for (int i = 0; i < D2C_UI_COUNT && n + 4 < size; i++) {
            BOOL on = i < 32 ? (ev->a >> i) & 1 : (ev->b >> (i - 32)) & 1;
            if (on) n += (size_t)_snprintf(s + n, size - n, " %02X", i);
        }
        if (!ev->a && !ev->b) _snprintf(s + n, size - n, " none");
        break;
    }
    default:
        _snprintf(s, size, "kind %u", ev->kind);
    }
    s[size - 1] = '\0';
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void prune(const char *dir)
{
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof pattern, "%s\\*.txt", dir);
    pattern[sizeof pattern - 1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    static char names[512][64];
    int n = 0;
    do {
        if (n < 512 && strlen(fd.cFileName) < 64) strcpy(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n <= cfg_keep) return;
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (int i = 0; i < n - cfg_keep; i++) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        path[sizeof path - 1] = '\0';
        DeleteFileA(path);
    }
}

static double ms_from(LONGLONG t, LONGLONG origin)
{
    return (double)(t - origin) * 1000.0 / (double)qpc_freq;
}

static void write_incident(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    _snprintf(dir, sizeof dir, "%spad-trace", log_dir());
    dir[sizeof dir - 1] = '\0';
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof path, "%s\\%04d-%02d-%02d_%02d-%02d-%02d-%s.txt", dir,
              inc.wall.wYear, inc.wall.wMonth, inc.wall.wDay, inc.wall.wHour, inc.wall.wMinute,
              inc.wall.wSecond, inc.file);
    path[sizeof path - 1] = '\0';
    FILE *f = fopen(path, "w");
    if (!f) {
        log_line("pad trace: cannot write %s", path);
        return;
    }

    fprintf(f, "pad trace: %s\n", inc.why);
    fprintf(f, "at %04d-%02d-%02d %02d:%02d:%02d.%03d (times below are ms from this moment)\n",
            inc.wall.wYear, inc.wall.wMonth, inc.wall.wDay, inc.wall.wHour, inc.wall.wMinute,
            inc.wall.wSecond, inc.wall.wMilliseconds);
    DWORD sw = d2.d2client ? *D2CLIENT_VAR(DWORD, D2C_SCREEN_W) : 0;
    DWORD sh = d2.d2client ? *D2CLIENT_VAR(DWORD, D2C_SCREEN_H) : 0;
    RECT rc = { 0, 0, 0, 0 };
    if (game_hwnd) GetClientRect(game_hwnd, &rc);
    fprintf(f, "game screen %lux%lu, window client %ldx%ld; D2 cursor (%d,%d), Windows cursor (%d,%d) in the window\n",
            (unsigned long)sw, (unsigned long)sh, (long)rc.right, (long)rc.bottom,
            px(inc.mouse), py(inc.mouse), px(inc.cursor), py(inc.cursor));

    /* The controller's globals as they stood at the trigger. */
    int k = 0;
    fprintf(f, "\nPD2 controller globals at the trigger (ProjectDiablo.dll offsets):\n");
    for (size_t b = 0; b < sizeof WATCH / sizeof WATCH[0]; b++) {
        for (DWORD o = 0; o < WATCH[b].bytes && k < WATCH_DWORDS; o += 4, k++) {
            if (o % 32 == 0) fprintf(f, "%s  +%06lX:", o ? "\n" : "", (unsigned long)(WATCH[b].rva + o));
            fprintf(f, " %08lX", (unsigned long)inc.snap[k]);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "  at the trigger: input mode +4AC2FE = %s, pad active +4AC2FD = %u, mouse-move count "
            "+4AF9BC = %u, PD2's own click +4ABDBC = %u, window focus +46A316 = %u\n",
            snap_byte(inc.snap, PD2_MODE) ? "controller" : "mouse",
            snap_byte(inc.snap, PD2_PAD_ACTIVE), snap_byte(inc.snap, PD2_MOVE_COUNT),
            snap_byte(inc.snap, PD2_OWN_CLICK), snap_byte(inc.snap, 0x46A316));
    fprintf(f, "  %d ms later, as this was written: input mode = %s, mouse-move count = %lu\n", POST_MS,
            pd2_byte(PD2_MODE) ? "controller" : "mouse", (unsigned long)pd2_dword(PD2_MOVE_COUNT));

    /* Events: the window before, and what came after up to now. */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG from = inc.at - (LONGLONG)inc.window_ms * qpc_freq / 1000;
    LONG newest = ring_next - 1;
    LONG first = newest;
    while (newest - first < (LONG)RING - 64) {
        const trace_ev *ev = &ring[(DWORD)(first - 1) & (RING - 1)];
        if (first - 1 < 0 || ev->seq != first - 1 || ev->t < from) break;
        first--;
    }
    fprintf(f, "\nevents (%ld):\n", (long)(newest - first + 1));
    char line[320];
    BOOL marked = FALSE;
    for (LONG i = first; i <= newest; i++) {
        trace_ev copy = ring[(DWORD)i & (RING - 1)];
        if (copy.seq != i) continue;
        if (!marked && copy.t >= inc.at) {
            fprintf(f, "  ---------- %s ----------\n", inc.why);
            marked = TRUE;
        }
        format_event(&copy, line, sizeof line);
        fprintf(f, "%+10.1f  f%-7lu %s\n", ms_from(copy.t, inc.at), (unsigned long)copy.frame, line);
    }
    fclose(f);
    log_line("pad trace: %s -> %s", inc.why, path);
    prune(dir);
}

/* --- Bootstrap's side -------------------------------------------------------------------------- */

static void read_config(void)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%spad-trace.cfg", log_dir());
    path[sizeof path - 1] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char value[64];
        if (line[0] == '#' || line[0] == ';') continue;
        if (sscanf(line, " %63[a-z_] = %63s", key, value) != 2) continue;
        int v = (int)strtol(value, NULL, 0);
        if (!strcmp(key, "enabled")) cfg_enabled = v != 0;
        else if (!strcmp(key, "jump_px") && v > 0) cfg_jump_px = v;
        else if (!strcmp(key, "keep") && v > 0) cfg_keep = v;
        else if (!strcmp(key, "mark_key") && v > 0 && v < 256) cfg_mark_key = v;
    }
    fclose(f);
}

void pad_trace_init(void *module)
{
    self = (HMODULE)module;
    read_config();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_freq = f.QuadPart ? f.QuadPart : 1;
    if (!cfg_enabled) log_line("pad trace: off (pad-trace.cfg does not say enabled = 1)");
}

/* From bootstrap's loop until it returns TRUE: needs ProjectDiablo.dll and D2Client. */
BOOL pad_trace_install(void)
{
    if (!cfg_enabled) return TRUE;
    HMODULE pd2_module = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2_module || !d2.d2client) return FALSE;
    pd2 = (BYTE *)pd2_module;

    int probes = 0;
    for (int i = 0; i < P_COUNT; i++) probes += install_probe(i);

    HMODULE net = GetModuleHandleA("D2Net.dll");
    HMODULE common = GetModuleHandleA("D2Common.dll");
    find_client_unit = (find_unit_fn)(void *)((BYTE *)d2.d2client + D2C_FIND_CLIENT_UNIT);
    item_text = common ? (item_text_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_ITEM_TEXT) : NULL;
    static const BYTE SEND_OPENS[] = { 0xA1 };   /* mov eax,[imm32] */
    BOOL sends = net && install_probe_at(P_SEND, (BYTE *)GetProcAddress(net, (LPCSTR)(DWORD_PTR)D2NET_SEND_ORDINAL),
                                         SEND_OPENS, 1, 5, "D2Net #10024 (SendPacket)");
    hook_set_cursor();
    void *next = NULL;
    if (redirect_import(pd2_module, "USER32.dll", "SendInput", (void *)on_send_input, &next) && next)
        send_input_next = (send_input_fn)next;
    HANDLE t = CreateThread(NULL, 0, device_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);

    put(EV_NOTE, NOTE_START, 0, 0, 0, 0);
    InterlockedExchange(&tracing, 1);
    log_line("pad trace: on -- %d of %d PD2 sites traced, SendInput %s, client packets %s; files go "
             "to pad-trace\\ (jump %d px, mark key 0x%02X or Back+Start held)", probes, P_COUNT,
             send_input_next ? "traced" : "NOT FOUND", sends ? "logged" : "NOT LOGGED",
             cfg_jump_px, cfg_mark_key);
    return TRUE;
}

void pad_trace_tick(void)
{
    if (!tracing) return;
    static DWORD started, last_retry;
    static BOOL key_was_down;
    DWORD now = GetTickCount();
    if (!started) started = now;

    /* BH and the Glide wrapper land after us: keep looking for them for a few minutes. */
    if (now - started < 180000 && now - last_retry >= 1000) {
        last_retry = now;
        hook_set_cursor();
    }

    BOOL down = (GetAsyncKeyState(cfg_mark_key) & 0x8000) != 0;
    if (down && !key_was_down && game_in_front()) {
        put(EV_MARK, 0, 0, 0, 0, 0);
        trigger(T_MARK, "mark", "mark (Pause key)");
    }
    key_was_down = down;

    if (inc.state == 2) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (ms_from(t.QuadPart, inc.at) >= POST_MS) {
            write_incident();
            InterlockedExchange(&inc.state, 0);
        }
    }
}
