#include <windows.h>
#include <string.h>
#include "d2.h"
#include "log.h"

/* The character screen instead of the main menu, at both places the game would show the menu:
 * starting with -skiptobnet, and leaving a single-player game.
 *
 * The game is started with -skiptobnet (the Start-menu shortcut, and PD2Launcher's own
 * "skiptobnet" setting) because that switch skips the title screen and the slow first load of the
 * main menu. Its other half is a logon to Battle.net, which fails here with a "Version" dialog.
 *
 * D2Launch's startup runs every time the game comes back to its menus — at launch, and after each
 * game — and decides where to go in one place (D2Launch+0x19E89). Past the Battle.net, Open and
 * TCP/IP return paths, which are left alone:
 *
 *   config+0x359 set (-skiptobnet)   read "Skip To Open", clear the switch, mark the title screen
 *                                    shown (config+0x35B), then call +0x19290, the logon. It
 *                                    builds the main menu as a backdrop and connects on top.
 *   title screen not yet shown       the title screen (+0x19700), and the main menu after it.
 *   otherwise                        +0x17DF0, the main menu. This is where leaving a single-
 *                                    player game lands.
 *
 * Two calls are repointed, both at what the Single Player button does: its handler sets the
 * connection type to 0 and opens the character screen — or character creation, with no saves on
 * disk. Nothing connects anywhere, and no menu is built only to be thrown away.
 *
 *   D2Launch+0x19EE9   call logon       -> instead_of_logon
 *   D2Launch+0x19F24   call main menu   -> instead_of_main_menu
 *
 * Holding Shift keeps the original at either: the logon at launch, the main menu on leaving.
 * Exit on the character screen still reaches the main menu. Every byte this relies on is compared
 * before anything is written; a D2Launch that differs is left alone with a log line.
 *
 * WHEN: before D2Launch's startup first runs. PD2 loads D2Launch itself, early, some way Game.exe's
 * own LoadLibraryA (Game.exe+0x7554) never sees — so bootstrap polls for the module every couple of
 * milliseconds as well, and whichever gets there first patches it. Measured 2026-11-21: patched
 * 27 ms after attach, with D2Launch's startup not yet begun. */

#define OFF_SWITCH_TEST    0x19E93   /* cmp bl,[eax+0x359] / je +0x55 */
#define OFF_LOGON_CALL     0x19EE3   /* mov [connection],edx / call logon / jmp +0x39 */
#define OFF_TITLE_TEST     0x19EF0   /* cmp bl,[eax+0x35B] / push 1 / mov [connection],ebx / jne */
#define OFF_MENU_CALL      0x19F1F   /* call D2Win / call main menu */
#define OFF_LOGON          0x19290   /* void(void), returns with a plain ret */
#define OFF_MAIN_MENU      0x17DF0   /* void(void), likewise */
#define OFF_D2WIN_THUNK    0x09898   /* the import thunk the menu path calls first */
#define OFF_CONNECTION     0x25AD4   /* DWORD: 0 single player, 1 Battle.net, 3 Open */
#define OFF_CONFIG         0x25ABC   /* the config struct Game.exe hands D2Launch's startup */
#define OFF_CONFIG_SWITCH  0x359     /* in it: -skiptobnet, cleared once acted on */

void **redirect_import(HMODULE module, const char *dll, const char *symbol, void *with,
                       void **out_original);

typedef HMODULE (WINAPI *load_library_fn)(LPCSTR);
typedef int (__stdcall *press_fn)(void *press);
typedef void (*screen_fn)(void);

static load_library_fn real_load_library;
static void **load_library_slot;
static press_fn single_player;
static screen_fn logon, main_menu;

static BOOL shift_held(void) { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

static void open_character_screen(void)
{
    /* The shape D2Win hands a button's handler, the control first. This one never reads it. */
    DWORD press[8] = { 0 };
    single_player(press);
}

static void instead_of_logon(void)
{
    if (shift_held()) {
        log_line("charselect: Shift held — Battle.net logon as -skiptobnet asked");
        logon();
        return;
    }
    open_character_screen();
    log_line("charselect: -skiptobnet opened the character screen instead of Battle.net");
}

static void instead_of_main_menu(void)
{
    if (shift_held()) {
        log_line("charselect: Shift held — back to the main menu");
        main_menu();
        return;
    }
    open_character_screen();
    log_line("charselect: back to the character screen instead of the main menu");
}

static DWORD rel32(const BYTE *from_end, const void *to)
{
    return (DWORD)(DWORD_PTR)to - (DWORD)(DWORD_PTR)from_end;
}

static BOOL write_rel32(BYTE *operand, const void *to)
{
    DWORD previous, value = rel32(operand + 4, to);
    if (!VirtualProtect(operand, 4, PAGE_EXECUTE_READWRITE, &previous)) return FALSE;
    memcpy(operand, &value, sizeof value);
    VirtualProtect(operand, 4, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), operand, 4);
    return TRUE;
}

/* Both threads can get here — the LoadLibraryA hook on the game's, bootstrap on ours — and
   only the first may write. */
static volatile LONG claimed;

/* Where the run is, for the log: before D2Launch's startup has read its switch, or after. */
static const char *timing(BYTE *d2launch)
{
    BYTE *config = *(BYTE **)(d2launch + OFF_CONFIG);
    if (!config) return "before D2Launch started";
    return config[OFF_CONFIG_SWITCH] ? "with the switch still unread"
                                     : "with the switch already read, or never given";
}

static void patch_d2launch(BYTE *d2launch, const char *how)
{
    if (InterlockedExchange(&claimed, 1)) return;

    /* The bytes as read from D2Launch 1.13c. Absolute and relative addresses are filled in from
       wherever the module actually loaded, so a relocated D2Launch still matches. */
    DWORD connection = (DWORD)(DWORD_PTR)(d2launch + OFF_CONNECTION);
    BYTE switch_test[] = { 0x38, 0x98, 0x59, 0x03, 0x00, 0x00, 0x74, 0x55 };
    BYTE logon_call[] = { 0x89, 0x15, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xEB, 0x39 };
    BYTE title_test[] = { 0x38, 0x98, 0x5B, 0x03, 0x00, 0x00, 0x6A, 0x01, 0x89, 0x1D, 0, 0, 0, 0,
                          0x75, 0x1F };
    BYTE menu_call[] = { 0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0 };
    BYTE press[] = { 0xC7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0xE8 };
    DWORD value;

    memcpy(logon_call + 2, &connection, 4);
    value = rel32(d2launch + OFF_LOGON_CALL + 11, d2launch + OFF_LOGON);
    memcpy(logon_call + 7, &value, 4);
    memcpy(title_test + 10, &connection, 4);
    value = rel32(d2launch + OFF_MENU_CALL + 5, d2launch + OFF_D2WIN_THUNK);
    memcpy(menu_call + 1, &value, 4);
    value = rel32(d2launch + OFF_MENU_CALL + 10, d2launch + OFF_MAIN_MENU);
    memcpy(menu_call + 6, &value, 4);
    memcpy(press + 2, &connection, 4);

    if (memcmp(d2launch + OFF_SWITCH_TEST, switch_test, sizeof switch_test) != 0
        || memcmp(d2launch + OFF_LOGON_CALL, logon_call, sizeof logon_call) != 0
        || memcmp(d2launch + OFF_TITLE_TEST, title_test, sizeof title_test) != 0
        || memcmp(d2launch + OFF_MENU_CALL, menu_call, sizeof menu_call) != 0
        || memcmp(d2launch + OFF_D2LAUNCH_SINGLE_PLAYER, press, sizeof press) != 0) {
        log_line("charselect: D2Launch is not the build this was mapped against — "
                 "its menus left as they are");
        return;
    }

    single_player = (press_fn)(d2launch + OFF_D2LAUNCH_SINGLE_PLAYER);
    logon = (screen_fn)(d2launch + OFF_LOGON);
    main_menu = (screen_fn)(d2launch + OFF_MAIN_MENU);

    if (!write_rel32(d2launch + OFF_LOGON_CALL + 7, (void *)instead_of_logon)
        || !write_rel32(d2launch + OFF_MENU_CALL + 6, (void *)instead_of_main_menu)) {
        log_line("charselect: could not write into D2Launch's code");
        return;
    }
    log_line("charselect: -skiptobnet and leaving a game now open the character screen (%s, %s)",
             how, timing(d2launch));
}

static void restore_load_library(void)
{
    void **slot = load_library_slot;
    load_library_slot = NULL;
    if (!slot) return;
    DWORD was;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &was)) {
        *slot = (void *)real_load_library;
        VirtualProtect(slot, sizeof(void *), was, &was);
    }
}

static HMODULE WINAPI on_load_library(LPCSTR name)
{
    HMODULE module = real_load_library(name);
    if (module && module == GetModuleHandleA("D2Launch.dll")) {
        restore_load_library();   /* one module was wanted; the rest load untouched */
        patch_d2launch((BYTE *)module, "as Game.exe loaded it");
    }
    return module;
}

/* From bootstrap, polled every couple of milliseconds until it returns TRUE — see the top. */
BOOL charselect_late(void)
{
    if (claimed) return TRUE;
    HMODULE d2launch = GetModuleHandleA("D2Launch.dll");
    if (!d2launch) return FALSE;
    restore_load_library();
    patch_d2launch((BYTE *)d2launch, "found loaded by someone else");
    return TRUE;
}

/* From DllMain: before Game.exe's own code has run, so before it can load D2Launch. */
void charselect_install(void)
{
    HMODULE d2launch = GetModuleHandleA("D2Launch.dll");
    if (d2launch) {
        patch_d2launch((BYTE *)d2launch, "loaded before us");
        return;
    }
    void *original = NULL;
    load_library_slot = redirect_import(GetModuleHandleA(NULL), "kernel32.dll", "LoadLibraryA",
                                        (void *)on_load_library, &original);
    if (load_library_slot) real_load_library = (load_library_fn)original;
}
