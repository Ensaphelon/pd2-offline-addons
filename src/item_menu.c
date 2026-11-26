#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>
#include "d2.h"
#include "log.h"
#include "craft.h"

void calls_trace_near_mouse(void);

/* The item menu: LB on the pad, or ~ on the keyboard, over an item in a panel.
 *
 * It lists what craft.c can make of the item — each recipe family with its three ingredients and
 * how many of each there are, then corruption — and makes the chosen one. The proposal it follows
 * was drawn over the user's own screenshot (2026-11-26).
 *
 * While it is open it has the pad and the keys to itself: PD2 is handed an idle pad (the hook in
 * focus_input.c asks item_menu_pad first) and the keys it uses never reach the game's window
 * (a subclass on top of PD2's). Up and down choose, A or Enter makes, B, LB, Esc or ~ close.
 *
 * It is drawn in D2's own frame, from calls.c, after everything the game draws: D2gfx #10014 for
 * the boxes, D2Win #10150 for the text — BH's addresses, D2Ptrs.h, 1.13c column — and each
 * ingredient as the game's own inventory picture: its ItemsTxt record names the file at +0x20,
 * D2Win #10111 loads it, D2gfx #10019 draws it the way beam.c draws the light.
 *
 * The user's notes on the first version (2026-11-26): no highlight on the chosen line, LB did
 * nothing, the item's name is already in the tooltip above, and pictures rather than names. */

#define D2C_HOVERED_ITEM  0x11BC38   /* SelectedInvItem */
#define D2C_MOUSE_X       0x11B828
#define D2C_MOUSE_Y       0x11B824
#define D2C_SCREEN_W      0xDBC48
#define D2C_SCREEN_H      0xDBC4C
#define D2C_UI_VARS       0xFAD80
#define UI_CHAT           0x05

/* The sale sound. D2Client's handler for server packet 0x2A (the vendor's answer, +0xAF9B0 by way
   of +0x4BC80) plays sound 0xDD when the answer is "sold" (byte 2 = 1): D2Client +0x88A70, the
   sound id in ebx and four zeros on the stack, which it pops. A sale away from a vendor has no
   such packet, and sending one would write into the trade window's state, so the sound is played
   the same way here, on the client's thread. */
#define D2C_PLAY_SOUND    0x88A70
#define SOUND_SOLD        0xDD

void __stdcall call_play_sound(void *fn, DWORD sound);
__asm__(
    ".text\n"
    ".globl _call_play_sound@8\n"
    "_call_play_sound@8:\n\t"
    "pushl %ebx\n\tpushl %esi\n\tpushl %edi\n\tpushl %ebp\n\t"
    "movl 24(%esp), %ebx\n\t"     /* sound */
    "movl 20(%esp), %edx\n\t"     /* fn */
    "pushl $0\n\tpushl $0\n\tpushl $0\n\tpushl $0\n\t"
    "call *%edx\n\t"
    "popl %ebp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\t"
    "ret $8\n");

static void *play_sound_fn;
static LONG sales_heard;

typedef void (__fastcall *draw_text_fn)(const wchar_t *text, int x, int y, DWORD colour, DWORD unused);
typedef DWORD (__fastcall *set_font_fn)(DWORD font);
typedef DWORD (__fastcall *text_size_fn)(const wchar_t *text, DWORD *width, DWORD *file);
typedef void (__stdcall *rect_fn)(int x1, int y1, int x2, int y2, DWORD colour, DWORD blend);
typedef HWND (__stdcall *get_hwnd_fn)(void);
typedef void *(__fastcall *load_cells_fn)(const char *path, int type);
typedef void (__stdcall *draw_cell_fn)(void *context, int x, int y, int light, int trans, int colour);
typedef BYTE *(__stdcall *item_text_fn)(DWORD index);

static draw_text_fn draw_text;
static set_font_fn set_font;
static text_size_fn text_size;
static rect_fn rect;
static get_hwnd_fn get_hwnd;
static load_cells_fn load_cells;
static draw_cell_fn draw_cell;
static item_text_fn item_text;

/* D2's text colours. */
enum { WHITE = 0, RED = 1, GREEN = 2, BLUE = 3, GOLD = 4, GREY = 5, TAN = 7, ORANGE = 8, YELLOW = 9 };

/* --- State ------------------------------------------------------------------------------------ */

static BOOL open_now;
static DWORD item_id;
static int chosen;
static int anchor_x, anchor_y;
static craft_info shown;
static BOOL have_info;
static DWORD asked_at;
static wchar_t toast[128];
static DWORD toast_at;
static LONG status_seen;

/* The menu's box and its lines as last drawn, in game pixels, for the mouse. */
static int box_x0, box_y0, box_x1, box_y1, rows_y0, row_h, rows_drawn;
static int mouse_was_x = -1, mouse_was_y = -1;
static BOOL click_eaten;

static int mouse_x(void) { return (int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_X); }
static int mouse_y(void) { return (int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_Y); }

/* Which line the mouse is over, or -1. */
static int row_under_mouse(void)
{
    int mx = mouse_x(), my = mouse_y();
    if (!rows_drawn || mx < box_x0 || mx >= box_x1 || my < rows_y0) return -1;
    int i = (my - rows_y0) / row_h;
    return i < rows_drawn ? i : -1;
}

static BOOL mouse_in_box(void)
{
    int mx = mouse_x(), my = mouse_y();
    return mx >= box_x0 && mx < box_x1 && my >= box_y0 && my < box_y1;
}

/* PD2 keeps the unit under its own cursor here (the hover label's unit, object_guard.c). */
#define PD2_HOVERED_UNIT 0x4ABE88

static BYTE *d2_hovered(void)
{
    if (!d2.d2client) return NULL;
    BYTE *item = *D2CLIENT_VAR(BYTE *, D2C_HOVERED_ITEM);
    return item && *(DWORD *)item == 4 ? item : NULL;
}

static BYTE *pd2_hovered(void)
{
    BYTE *pd2 = (BYTE *)GetModuleHandleA("ProjectDiablo.dll");
    BYTE *unit = pd2 ? *(BYTE **)(pd2 + PD2_HOVERED_UNIT) : NULL;
    if (!unit || IsBadReadPtr(unit, 0x18) || *(DWORD *)unit != 4) return NULL;
    BYTE *data = *(BYTE **)(unit + 0x14);
    /* Only an item in a panel: inventory, cube or stash, not one on the ground. */
    return data && !IsBadReadPtr(data, 0x48) && (data[0x45] == 0 || data[0x45] == 3 || data[0x45] == 4)
         ? unit : NULL;
}

static BYTE *hovered(void)
{
    BYTE *item = d2_hovered();
    return item ? item : pd2_hovered();
}

static BOOL chat_open(void)
{
    return d2.d2client && D2CLIENT_VAR(DWORD, D2C_UI_VARS)[UI_CHAT] != 0;
}

static void open_menu(BYTE *item)
{
    open_now = TRUE;
    item_id = *(DWORD *)(item + 0x0C);
    chosen = 0;
    have_info = FALSE;
    anchor_x = (int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_X);
    anchor_y = (int)*D2CLIENT_VAR(DWORD, D2C_MOUSE_Y);
    asked_at = GetTickCount();
    craft_query(item_id);
    /* Which call draws the cursor: one frame of the draws near the mouse, the first two opens. */
    static int traced;
    if (traced++ < 2) calls_trace_near_mouse();
}

static void close_menu(void) { open_now = FALSE; }

/* For calls.c: the menu is open, and where the mouse is, in game pixels. */
BOOL item_menu_open(void) { return open_now; }
void item_menu_mouse(int *x, int *y) { *x = mouse_x(); *y = mouse_y(); }

static int option_count(void) { return have_info ? shown.count : 0; }

static void move(int step)
{
    int n = option_count();
    if (n) chosen = (chosen + step + n) % n;
}

/* What the cube's state (no cube, not empty, ...) stops: the crafts, not a sale. */
static BOOL blocked(const craft_option *o)
{
    return !o->possible || (shown.problem[0] && o->action != CRAFT_ACTION_SELL);
}

static void make(void)
{
    if (!have_info || chosen >= shown.count) return;
    const craft_option *o = &shown.option[chosen];
    if (blocked(o)) return;
    craft_request(item_id, o->action);
    close_menu();
}

/* Space or Y: sell at once, whichever line is chosen (user, 2026-11-26). */
static void sell_now(void)
{
    for (int i = 0; have_info && i < shown.count; i++) {
        if (shown.option[i].action != CRAFT_ACTION_SELL) continue;
        if (blocked(&shown.option[i])) return;
        craft_request(item_id, CRAFT_ACTION_SELL);
        close_menu();
        return;
    }
}

static BOOL can_sell(void)
{
    for (int i = 0; have_info && i < shown.count; i++)
        if (shown.option[i].action == CRAFT_ACTION_SELL && !blocked(&shown.option[i])) return TRUE;
    return FALSE;
}

/* What a line says after its ingredients: what it adds, or, with nothing to add, why it cannot be
   done (a sale refused: "open the stash", "at a vendor"). */
static const char *note_of(const craft_option *o)
{
    return o->effect[0] ? o->effect : o->why;
}

/* --- The pad: focus_input.c asks this before PD2 sees a state ------------------------------- */

static WORD pad_was;
static BOOL hold_until_released;

/* TRUE: PD2 is to see an idle pad. */
BOOL item_menu_pad(XINPUT_STATE *state)
{
    if (!craft_ready()) return FALSE;
    WORD now = state->Gamepad.wButtons, pressed = now & ~pad_was;
    pad_was = now;
    if (!open_now) {
        if (hold_until_released) {
            if (now) return TRUE;
            hold_until_released = FALSE;
        }
        if (!(pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)) return FALSE;
        BYTE *item = hovered();
        static int told;
        if (told < 6) {
            told++;
            log_line("item menu: LB -- D2's hovered item %p, PD2's hovered unit %p -> %s",
                     (void *)d2_hovered(), (void *)pd2_hovered(), item ? "opening" : "nothing to open");
        }
        if (!item) return FALSE;
        open_menu(item);
        return TRUE;
    }
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) move(-1);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) move(1);
    if (pressed & XINPUT_GAMEPAD_Y) sell_now();
    else if (pressed & XINPUT_GAMEPAD_A) make();
    else if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_LEFT_SHOULDER)) close_menu();
    if (!open_now) hold_until_released = TRUE;
    return TRUE;
}

/* --- The keyboard: a subclass on top of the game's window ------------------------------------ */

static WNDPROC next_proc;
static BOOL eat_char;

static LRESULT CALLBACK menu_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (craft_ready() && !chat_open()) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (!open_now) {
                BYTE *item = wp == VK_OEM_3 ? hovered() : NULL;
                if (item) { open_menu(item); eat_char = TRUE; return 0; }
            } else {
                if (wp == VK_UP) move(-1);
                else if (wp == VK_DOWN) move(1);
                else if (wp == VK_RETURN) make();
                else if (wp == VK_SPACE) sell_now();
                else if (wp == VK_ESCAPE || wp == VK_OEM_3) close_menu();
                eat_char = TRUE;
                return 0;
            }
        } else if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (open_now || eat_char)) {
            return 0;
        } else if (msg == WM_CHAR && eat_char) {
            eat_char = FALSE;
            return 0;
        } else if (open_now && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)) {
            if (mouse_in_box()) {
                int i = row_under_mouse();
                if (i >= 0 && msg == WM_LBUTTONDOWN) { chosen = i; make(); }
                click_eaten = TRUE;
                return 0;
            }
            close_menu();   /* a click elsewhere closes it, and still counts for the game */
        } else if ((msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) && click_eaten) {
            click_eaten = FALSE;
            return 0;
        }
    }
    return CallWindowProcA(next_proc, window, msg, wp, lp);
}

static void hook_window(void)
{
    static BOOL tried;
    if (next_proc || tried || !get_hwnd) return;
    HWND window = get_hwnd();
    if (!window) return;
    tried = TRUE;
    next_proc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)menu_proc);
    log_line("item menu: keys %s", next_proc ? "hooked (~ over an item)" : "NOT hooked");
}

/* --- Ingredient pictures ------------------------------------------------------------------- */

typedef struct { DWORD code; void *cells; } picture;
static picture pictures[24];
static int picture_count;

/* A cell file the renderer can still use: its first cell readable and of a sane size. */
static BOOL cells_ok(void *cells)
{
    if (!cells || IsBadReadPtr(cells, 0x1C)) return FALSE;
    BYTE *cell = *(BYTE **)((BYTE *)cells + 0x18);
    if (!cell || IsBadReadPtr(cell, 0x0C)) return FALSE;
    DWORD w = *(DWORD *)(cell + 4), hgt = *(DWORD *)(cell + 8);
    return w >= 1 && w <= 256 && hgt >= 1 && hgt <= 256;
}

/* The item's inventory picture: its ItemsTxt record names the file at +0x20 (the code is at
   +0x80), D2Win #10111 loads it. Kept, and loaded again if a renderer restart takes it away. */
static void *picture_of(DWORD code)
{
    for (int i = 0; i < picture_count; i++)
        if (pictures[i].code == code) {
            if (cells_ok(pictures[i].cells)) return pictures[i].cells;
            pictures[i].code = 0;
        }
    if (!load_cells || !item_text) return NULL;
    for (DWORD i = 0; i < 8192; i++) {
        BYTE *txt = item_text(i);
        if (!txt) break;
        if (*(DWORD *)(txt + 0x80) != code) continue;
        char path[80];
        _snprintf(path, sizeof path, "data\\global\\items\\%.31s", (const char *)(txt + 0x20));
        path[sizeof path - 1] = 0;
        void *cells = load_cells(path, 0);
        if (!cells_ok(cells)) {
            static int told;
            if (told++ < 4) log_line("item menu: no picture from %s (%p)", path, cells);
            return NULL;
        }
        int slot = -1;
        for (int k = 0; k < picture_count && slot < 0; k++) if (!pictures[k].code) slot = k;
        if (slot < 0 && picture_count < 24) slot = picture_count++;
        if (slot < 0) slot = 0;
        pictures[slot].code = code;
        pictures[slot].cells = cells;
        return cells;
    }
    return NULL;
}

/* The picture with its bottom-left at x,y — D2gfx #10019 takes the bottom, and (-1, 5, 4) is what
   the game itself passes for a UI picture (captured off its skill button, 2026-11-26). Returns
   the width drawn, 0 if nothing was. */
static int draw_picture(DWORD code, int x, int y)
{
    void *cells = picture_of(code);
    if (!cells || !draw_cell) return 0;
    DWORD context[64];
    memset(context, 0, sizeof context);
    context[0x34 / 4] = (DWORD)(UINT_PTR)cells;   /* frame 0 at +0x00, direction 0 at +0x40 */
    draw_cell(context, x, y, -1, 5, 4);
    BYTE *cell = *(BYTE **)((BYTE *)cells + 0x18);
    return (int)*(DWORD *)(cell + 4);
}

/* --- Drawing ---------------------------------------------------------------------------------- */

/* The menu's dark background. */
/* The menu's dark background. */
static void background(int x0, int y0, int x1, int y1)
{
    rect(x0, y0, x1, y1, 0, 1);
    rect(x0, y0, x1, y1, 0, 1);
}

/* The cursor, again, over the menu. However late the menu went out — even just before the game's
   own cursor call — D2GL put the cursor under it (2026-11-26, eight ways of drawing the box
   tried). A sprite drawn by D2gfx #10019 after the box does land on top, so the cursor is drawn a
   second time that way: from the game's own call (calls.c hands it over), so it is the same
   picture, frame and place, and the dimmed original underneath is simply covered. */
void item_menu_cursor_again(void *context, int x, int y)
{
    if (!open_now || !draw_cell || !context || !mouse_in_box()) return;
    draw_cell(context, x, y, -1, 5, 4);
}

static int width_of(const wchar_t *text)
{
    DWORD width = 0, file = 0;
    if (text_size) text_size(text, &width, &file);
    return (int)width;
}

/* Text whose top is at y (D2 draws from the baseline). */
static int text_at(const wchar_t *text, int x, int y, int colour, int line)
{
    draw_text(text, x, y + line - 3, (DWORD)colour, 0);
    return x + width_of(text);
}

static void capitalised(const char *family, wchar_t *out, size_t size)
{
    _snwprintf(out, size, L"%hs", family);
    out[size - 1] = 0;
    if (out[0] >= L'a' && out[0] <= L'z') out[0] = (wchar_t)(out[0] - 32);
}

void item_menu_draw(void)
{
    if (!draw_text || !rect || !set_font) return;
    hook_window();

    if (craft_sale_sound_get(&sales_heard) && play_sound_fn) call_play_sound(play_sound_fn, SOUND_SOLD);

    char said[128];
    if (craft_status_get(&status_seen, said, sizeof said)) {
        _snwprintf(toast, 128, L"%hs", said);
        toast[127] = 0;
        toast_at = GetTickCount();
    }

    DWORD old_font = set_font(0);
    const int line = 16, row = 32, icon = 29;
    int screen_w = (int)*D2CLIENT_VAR(DWORD, D2C_SCREEN_W), screen_h = (int)*D2CLIENT_VAR(DWORD, D2C_SCREEN_H);

    if (!open_now && toast[0] && GetTickCount() - toast_at < 3000) {
        int w = width_of(toast) + 16;
        int x = anchor_x + 18, y = anchor_y - 10;
        if (x + w > screen_w) x = screen_w - w;
        if (y < 0) y = 0;
        rect(x, y, x + w, y + line + 6, 0, 1);
        text_at(toast, x + 8, y + 3, GOLD, line);
    }
    if (!open_now) { set_font(old_font); return; }

    craft_info got;
    if (craft_info_get(&got) && got.item_id == item_id) {
        if (!have_info || got.seq != shown.seq) { shown = got; have_info = TRUE; }
        if (chosen >= shown.count) chosen = 0;
    }
    if (!have_info && GetTickCount() - asked_at > 1500) { close_menu(); set_font(old_font); return; }

    /* The mouse picks the line it is over, when it moves: the pad's choice stands until then. */
    int mx = mouse_x(), my = mouse_y();
    if (mx != mouse_was_x || my != mouse_was_y) {
        int under = row_under_mouse();
        if (under >= 0) chosen = under;
        mouse_was_x = mx;
        mouse_was_y = my;
    }

    BOOL nothing = have_info && shown.count == 0;
    int rows = !have_info || nothing ? 1 : shown.count;
    BOOL crafts = FALSE;
    for (int i = 0; have_info && i < shown.count; i++) crafts |= shown.option[i].action != CRAFT_ACTION_SELL;
    BOOL problem = have_info && !nothing && crafts && shown.problem[0];

    /* Columns: the name, three ingredients, then what the line adds. */
    const int name_w = 92, need_w = 74;
    int effect_x = name_w + 12 + 3 * need_w + 4, effect_w = 0;
    for (int i = 0; have_info && i < shown.count; i++) {
        wchar_t e[100];
        _snwprintf(e, 100, L"%hs", note_of(&shown.option[i]));
        e[99] = 0;
        int ew = width_of(e);
        if (ew > effect_w) effect_w = ew;
    }
    int w = nothing ? 220 : effect_x + effect_w + 14;
    int h = 8 + (problem ? line + 8 : 0) + rows * row + 6 + line + 8;
    int x = anchor_x + 24, y = anchor_y - 16;
    if (x + w > screen_w) x = anchor_x - 24 - w;
    if (x < 0) x = 0;
    if (y + h > screen_h - 50) y = screen_h - 50 - h;
    if (y < 0) y = 0;

    background(x, y, x + w, y + h);
    box_x0 = x; box_y0 = y; box_x1 = x + w; box_y1 = y + h;
    rows_drawn = 0;
    int cy = y + 8;

    if (!have_info) {
        text_at(L"...", x + 10, cy + 8, GREY, line);
        cy += row;
    } else if (nothing) {
        text_at(L"No actions available", x + 12, cy + (row - line) / 2, GREY, line);
        cy += row;
    } else {
        if (problem) {
            wchar_t p[100];
            _snwprintf(p, 100, L"%hs", shown.problem);
            p[99] = 0;
            text_at(p, x + 12, cy, RED, line);
            cy += line + 8;
        }
        rows_y0 = cy;
        row_h = row;
        rows_drawn = shown.count;
        for (int i = 0; i < shown.count; i++) {
            const craft_option *o = &shown.option[i];
            BOOL can = !blocked(o);
            if (i == chosen) {
                /* A frame, not a fill. A blended rectangle only darkens under D2GL whatever its
                   colour (0x9A, light blue, at blend 1 did not show); a solid one shows, but D2GL
                   puts rectangles over text, so a filled line hid its own words (2026-11-26).
                   0x58 is (160,120,64) in ACT1's pal.dat, a light gold. */
                rect(x + 3, cy, x + w - 3, cy + 2, 0x58, 5);
                rect(x + 3, cy + row - 3, x + w - 3, cy + row - 1, 0x58, 5);
                rect(x + 3, cy, x + 7, cy + row - 1, 0x58, 5);
                rect(x + w - 5, cy, x + w - 3, cy + row - 1, 0x58, 5);
            }
            wchar_t name[24];
            capitalised(o->family, name, 24);
            int colour = !can ? GREY : i == chosen ? WHITE
                       : o->action == CRAFT_ACTION_CORRUPT ? RED : o->action == CRAFT_ACTION_SELL ? GOLD : ORANGE;
            text_at(name, x + 12, cy + (row - line) / 2, colour, line);
            for (int k = 0; k < o->needs; k++) {
                int cx = x + name_w + 12 + k * need_w;
                int drawn = draw_picture(o->need[k].code, cx, cy + row - 2);
                if (!drawn) drawn = icon;
                wchar_t count[12];
                _snwprintf(count, 12, L"%d", o->need[k].have);
                count[11] = 0;
                text_at(count, cx + drawn + 2, cy + (row - line) / 2,
                        o->need[k].have < 1 ? RED : can ? WHITE : GREY, line);
            }
            wchar_t effect[100];
            _snwprintf(effect, 100, L"%hs", note_of(o));
            effect[99] = 0;
            text_at(effect, x + effect_x, cy + (row - line) / 2, can ? BLUE : GREY, line);
            cy += row;
        }
    }
    cy += 6;
    text_at(nothing ? L"B: close" : can_sell() ? L"A: make    Y / Space: sell    B: close"
                                              : L"A: make    B: close", x + 12, cy, GREY, line);
    set_font(old_font);
}

/* --- Installing ------------------------------------------------------------------------------- */

BOOL item_menu_install(void)
{
    HMODULE win = GetModuleHandleA("D2Win.dll");
    HMODULE gfx = GetModuleHandleA("D2gfx.dll");
    if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
    if (!win || !gfx || !d2.d2client) return FALSE;
    {
        /* Past the first instruction, which holds an address and moves if D2Client does. */
        static const BYTE SOUND_OPENS[] = { 0x83, 0xEC, 0x0C, 0x85, 0xC0, 0x55, 0x8B, 0x6C, 0x24, 0x14 };
        BYTE *fn = (BYTE *)d2.d2client + D2C_PLAY_SOUND;
        if (fn[0] == 0xA1 && memcmp(fn + 5, SOUND_OPENS, sizeof SOUND_OPENS) == 0) play_sound_fn = fn;
        else log_line("item menu: D2Client's sound routine is not the one read -- sales away from a vendor are silent");
    }
    draw_text = (draw_text_fn)(void *)GetProcAddress(win, MAKEINTRESOURCEA(10150));
    set_font = (set_font_fn)(void *)GetProcAddress(win, MAKEINTRESOURCEA(10184));
    text_size = (text_size_fn)(void *)GetProcAddress(win, MAKEINTRESOURCEA(10177));
    rect = (rect_fn)(void *)GetProcAddress(gfx, MAKEINTRESOURCEA(10014));
    get_hwnd = (get_hwnd_fn)(void *)GetProcAddress(gfx, MAKEINTRESOURCEA(10048));
    load_cells = (load_cells_fn)(void *)GetProcAddress(win, MAKEINTRESOURCEA(10111));
    draw_cell = (draw_cell_fn)(void *)GetProcAddress(gfx, MAKEINTRESOURCEA(10019));
    HMODULE common = GetModuleHandleA("D2Common.dll");
    item_text = common ? (item_text_fn)(void *)GetProcAddress(common, MAKEINTRESOURCEA(10695)) : NULL;
    log_line("item menu: %s%s", draw_text && set_font && rect && get_hwnd ? "ready" : "MISSING a D2 export",
             load_cells && draw_cell && item_text ? "" : " (no ingredient pictures)");
    return TRUE;
}
