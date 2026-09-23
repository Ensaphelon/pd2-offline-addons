#include <windows.h>
#include "d2.h"
#include "log.h"

/* Leaving a game and coming straight back in.
 *
 * Two facts shape all of this. The interface loop is single-threaded — D2Launch's menus do not
 * return until they are done — so the driver cannot run on the game's own thread without
 * blocking the very code that builds the screens it is waiting for. And a control pointer read
 * before a sleep may be freed by the time the sleep ends, so nothing is ever cached across one.
 *
 * Every wait has a ceiling. On timeout the driver gives up and leaves you standing in the menu:
 * pressing something at a moment we do not understand is worse than doing nothing. */

static int restart_running = 0;

/* The game's own "Save and Exit Game" handler, taken from the menu it builds. */
static BOOL (__fastcall *leave_action)(d2_menu_entry *, void *) = NULL;
static d2_menu_entry *leave_entry = NULL;

void restart_set_leave_action(BOOL (__fastcall *fn)(d2_menu_entry *, void *), d2_menu_entry *entry)
{
    leave_action = fn;
    leave_entry = entry;
}

static void describe_controls(const char *when)
{
    d2_control *first = *D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL);
    int n = 0;
    for (d2_control *c = first; c && n < 40; c = c->next, n++) {
        log_line("  %s: control #%d type=%lu state=%lu at (%lu,%lu) %lux%lu press=%p",
                 when, n, (unsigned long)c->type, (unsigned long)c->state,
                 (unsigned long)c->x, (unsigned long)c->y,
                 (unsigned long)c->w, (unsigned long)c->h, (void *)c->on_press);
    }
    if (n == 0) log_line("  %s: no controls yet", when);
}

/* Waits for the out-of-game control list to settle: the same count seen twice in a row, with at
   least one pressable button in it. "A control exists" is not "the screen is ready" — the
   reference implementation for this papers over that with a fixed sleep, which is a guess that
   travels badly to a different machine. */
static int wait_for_screen(int timeout_ms)
{
    int last = -1, stable = 0;
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        Sleep(100);
        int count = 0, pressable = 0;
        for (d2_control *c = *D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL);
             c && count < 64; c = c->next) {
            count++;
            if (c->type == 6 && c->state == 5 && c->on_press) pressable++;
        }
        if (count && count == last && pressable) {
            if (++stable >= 3) return 1;
        } else {
            stable = 0;
        }
        last = count;
    }
    return 0;
}

/* Buttons are chosen by where they sit, never by their order in the list: the list order is an
   implementation detail of how the screen was built, and the very first attempt at this pressed
   list-position 0 on the main menu and landed on Battle.net's gateway dialog. Position is what a
   person actually uses, and it is stable.

   The list is re-walked immediately before every press, so a pointer cannot have gone stale
   while we waited. */
/* A button's state is not simply enabled/disabled. 5 is the ordinary enabled button of a full
   screen; the difficulty popup's own three buttons come up as 13, and the screen underneath it
   keeps its state-5 buttons in the same list. Matching only on 5 picked the character screen's
   EXIT out from under the popup, which is exactly how the first attempt landed back at the main
   menu. So the wanted state is part of the question. */
#define STATE_ENABLED 5
#define STATE_POPUP   13

enum pick { PICK_TOPMOST, PICK_BOTTOM_RIGHT, PICK_NTH_FROM_TOP };

static int press_button(enum pick how, int nth, DWORD state, const char *why)
{
    d2_control *best = NULL;
    int rank = 0;

    for (d2_control *c = *D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL); c; c = c->next) {
        if (c->type != 6 || c->state != state || !c->on_press) continue;

        if (how == PICK_TOPMOST) {
            if (!best || c->y < best->y) best = c;
        } else if (how == PICK_BOTTOM_RIGHT) {
            if (!best || c->y > best->y || (c->y == best->y && c->x > best->x)) best = c;
        } else {
            /* Nth counting down the screen. */
            int above = 0;
            for (d2_control *o = *D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL); o; o = o->next)
                if (o->type == 6 && o->state == state && o->on_press && o->y < c->y) above++;
            if (above == nth) { best = c; rank = above; }
        }
    }
    (void)rank;

    if (!best) { log_line("restart: no button to press for %s", why); return 0; }
    log_line("restart: pressing %s at (%lu,%lu)", why, (unsigned long)best->x, (unsigned long)best->y);
    best->on_press(best);
    return 1;
}

static DWORD WINAPI restart_thread(LPVOID difficulty_arg)
{

    BYTE difficulty = (BYTE)(DWORD_PTR)difficulty_arg;

    /* Main menu. Single Player is the top entry — pressing by list order landed on Battle.net. */
    if (!wait_for_screen(20000)) {
        log_line("restart: the main menu never settled — stopping, you are in the menu");
        restart_running = 0;
        return 0;
    }
    describe_controls("main-menu");
    if (!press_button(PICK_TOPMOST, 0, STATE_ENABLED, "Single Player")) { restart_running = 0; return 0; }

    /* Character select: OK sits bottom-right, Cancel bottom-left. */
    if (!wait_for_screen(20000)) {
        log_line("restart: the character screen never settled — stopping");
        restart_running = 0;
        return 0;
    }
    describe_controls("char-select");
    if (!press_button(PICK_BOTTOM_RIGHT, 0, STATE_ENABLED, "OK on the character")) { restart_running = 0; return 0; }

    /* What comes next depends on the character. One with more than one difficulty unlocked gets
       a popup over the character screen; one that has only Normal goes straight into the game —
       that is the game's own behaviour, not a failure, and waiting for a screen that is never
       coming is what made Restart look broken after a few tries: the driver sat out its timeout
       with the "already restarting" flag up, so every further press did nothing but click.

       So both endings are waited for at once. The out-of-game control list emptying means the
       game has started and there is nothing left to do. */
    int chose = 0;
    for (int waited = 0; waited < 20000; waited += 100) {
        Sleep(100);

        if (*D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL) == NULL) {
            log_line("restart: in the game — this character has only one difficulty");
            chose = 1;
            break;
        }

        int popup = 0;
        for (d2_control *c = *D2WIN_VAR(d2_control *, OFF_D2WIN_FIRSTCONTROL); c; c = c->next)
            if (c->type == 6 && c->state == STATE_POPUP && c->on_press) popup++;
        if (popup >= 3) {
            describe_controls("difficulty");
            press_button(PICK_NTH_FROM_TOP, (int)difficulty, STATE_POPUP, "the difficulty");
            chose = 1;
            break;
        }
    }
    if (!chose) log_line("restart: neither the difficulty popup nor the game appeared — stopping");

    restart_running = 0;
    return 0;
}

BOOL __fastcall restart_pressed(d2_menu_entry *entry, void *msg)
{
    (void)entry;
    if (restart_running) return TRUE;

    d2_get_difficulty_fn get_difficulty =
        (d2_get_difficulty_fn)((BYTE *)d2.d2client + OFF_D2CLIENT_GETDIFFICULTY);
    BYTE difficulty = get_difficulty();
    log_line("restart: asked for, difficulty %u", (unsigned)difficulty);

    if (!leave_action) {
        log_line("restart: no leave action captured — doing nothing");
        return TRUE;
    }

    restart_running = 1;
    /* The waiting half runs on its own thread — the menu loop we are standing in would block it.
       It is started BEFORE leaving so it is already watching when the screens come up. */
    HANDLE t = CreateThread(NULL, 0, restart_thread, (LPVOID)(DWORD_PTR)difficulty, 0, NULL);
    if (t) CloseHandle(t);

    /* And the leaving itself happens right here, in the same thread and the same way a click on
       "Save and Exit Game" would do it. */
    log_line("restart: invoking the game's own Save and Exit");
    leave_action(leave_entry, msg);
    return TRUE;
}
