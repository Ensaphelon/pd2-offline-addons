#include <windows.h>
#include <string.h>
#include "log.h"

/* The automap open from the first frame of every game, instead of after a press of its key.
 *
 * D2 closes it on every join: the handler for the server's game-logon packet (0x01, at
 * D2Client+0xAFA90) ends by jumping into the function that zeroes all 38 UI flags, the automap's
 * among them (D2Client+0xC3540, the store at +0xC3644). Nothing in PD2 or BH puts it back —
 * neither binary has an option for it.
 *
 * Opening it is done the way the key does it, by calling the game's own toggle rather than
 * writing the flag. The toggle does two things the flag alone does not: it closes whatever panel
 * would cover the map, and it registers five keydown handlers on the game window — the arrows
 * and End, which pan and re-centre the map. With only the flag set those keys would stay dead
 * until the map was closed and opened by hand, and closing it would unregister handlers that were
 * never registered.
 *
 *   D2Client+0x3B8B0  void(void)   found. The automap key's entry in D2Client's own hotkey table
 *                                  (the pointer sits at file offset 0xF4F64); PD2 keeps a pointer
 *                                  to it too. Flips UI flag 0x0A; when it goes 0 -> 1, registers
 *                                  the handlers listed at D2Client+0xD0780 through Storm.
 *   D2Client+0xFADA8  DWORD        BH. UI flag 0x0A, the automap. PD2 reads the same flag and
 *                                  draws it as its corner mini map (automapmode 1).
 *
 * When: the first frame that has a player unit after one without. The logon packet that closes
 * the map arrives before the one that assigns the player (0x59), so by the time there is a
 * player the map has already been closed for this game and will not be closed again — those are
 * the only two stores to the flag in D2Client. The call runs from the frame hook, on the game's
 * own thread, between frames, which is where a key press would have run it.
 *
 * Once per game, and only if the map is shut: a map closed by hand stays closed. */

#define OFF_D2CLIENT_TOGGLE_AUTOMAP 0x3B8B0
#define OFF_D2CLIENT_AUTOMAP_ON     0xFADA8
#define OFF_GETPLAYERUNIT           0xA4D60   /* BH. void *__stdcall(void), NULL outside a game */

typedef void (*toggle_fn)(void);
typedef void *(__stdcall *get_player_unit_fn)(void);

/* push ecx / mov eax,[automap flag] / mov [esp],eax — the flag's address is checked against
   wherever D2Client actually loaded, so a relocated module still matches. */
static const BYTE TOGGLE_HEAD[] = { 0x51, 0xA1, 0, 0, 0, 0, 0x89, 0x04, 0x24 };

void automap_frame(void)
{
    static enum { UNCHECKED, READY, REFUSED } state;
    static toggle_fn toggle;
    static get_player_unit_fn get_player_unit;
    static volatile DWORD *automap_on;
    static BOOL was_in_game;

    if (state == REFUSED) return;
    if (state == UNCHECKED) {
        BYTE *client = (BYTE *)GetModuleHandleA("D2Client.dll");
        if (!client) return;
        BYTE expected[sizeof(TOGGLE_HEAD)];
        memcpy(expected, TOGGLE_HEAD, sizeof(expected));
        DWORD flag_va = (DWORD)(DWORD_PTR)(client + OFF_D2CLIENT_AUTOMAP_ON);
        memcpy(expected + 2, &flag_va, sizeof(flag_va));
        if (memcmp(client + OFF_D2CLIENT_TOGGLE_AUTOMAP, expected, sizeof(expected)) != 0) {
            state = REFUSED;
            log_line("automap: D2Client+%X is not the automap toggle this was mapped against "
                     "— leaving the map to its key", OFF_D2CLIENT_TOGGLE_AUTOMAP);
            return;
        }
        toggle = (toggle_fn)(client + OFF_D2CLIENT_TOGGLE_AUTOMAP);
        get_player_unit = (get_player_unit_fn)(client + OFF_GETPLAYERUNIT);
        automap_on = (volatile DWORD *)(client + OFF_D2CLIENT_AUTOMAP_ON);
        state = READY;
    }

    BOOL in_game = get_player_unit() != NULL;
    if (in_game && !was_in_game && *automap_on == 0) {
        toggle();
        log_line("automap: opened on joining (flag now %lu)", (unsigned long)*automap_on);
    }
    was_in_game = in_game;
}
