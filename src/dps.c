/* The offline half of Project Diablo 2's own DPS meter.
 *
 * PD2 already ships every part of this feature inside ProjectDiablo.dll — the damage
 * attribution, the averaging, and the on-screen line. What it does not ship is a server to run
 * the first two. Online, the realm accumulates the damage and hands the client a number; offline
 * there is no realm, so the widget draws and stays at 0.
 *
 * This plugin supplies that number. Nothing is patched and nothing is drawn: the client's own
 * code reads one DWORD out of its own player struct, so writing that DWORD is the whole job.
 *
 * WHERE THOSE OFFSETS COME FROM
 * -----------------------------
 * Disassembled from the shipped ProjectDiablo.dll (image base 0x10000000), 2026-09-23. The draw
 * site is one function, and it is short enough to quote:
 *
 *     102589a0  call 0x1028f460                 ; the client's player unit -> edi
 *     102589cb  mov  eax,[edi+0x14]             ; UnitAny+0x14 = pPlayerData
 *     102589d6  cmp  DWORD [eax+0x1a4],0        ; tracking off -> draw nothing
 *     102589dd  je   .skip
 *     102589df  push DWORD [eax+0x1ac]          ; <-- the number on screen
 *     102589e5  mov  ecx,0x1037c780             ; "PlayerDpsDisplay" (the format string key)
 *     102589ea  call 0x102d0fb0
 *     ...       sprintf, then draw
 *
 * PD2 extends vanilla D2's PlayerData with its own fields; these four are the meter's whole
 * state. The accumulator that produces +0x1AC is also in the DLL, at 0x102ae5ff:
 *
 *     mov  eax,[ebx+0x1ac]    ; running average
 *     mov  ecx,[ebx+0x1a4]    ; sample count n
 *     mov  edx,[ebx+0x1a8]    ; damage since the last tick
 *     imul eax,ecx            ; avg*n
 *     inc  ecx                ; n+1
 *     add  eax,edx
 *     cdq / idiv ecx          ; (avg*n + sum) / (n+1)
 *     mov  [ebx+0x1ac],eax    ; new average
 *     mov  [eax+0x1a8],0      ; sum consumed
 *
 * ...with the window restarting once [ebx+0x265] + 125 ticks have passed — 125 ticks is 5
 * seconds at D2's 25 fps, so the displayed figure is a 5-second cumulative mean.
 *
 * And the attribution, at 0x1026f4fa, is a real damage hook rather than anything approximate:
 * it takes the raw damage in 256ths, shifts it down to whole points, CLAMPS it to the target's
 * remaining life (so an overkill blow counts only what it actually removed), and adds the result
 * to +0x1A8. That clamp is worth noting — it is exactly the correctness we would otherwise have
 * to reinvent by diffing monster health, and getting it wrong is how a home-made meter ends up
 * reporting damage that was never dealt.
 */

#include <windows.h>
#include <string.h>
#include "log.h"

/* The damage hook (hook.c) — PD2's own already-clamped numbers, straight off the instruction
 * that lands them. */
int hook_install(void);
int hook_install_gate(void);
extern volatile DWORD hook_damage_total;
extern volatile DWORD hook_hit_count;
extern void *volatile hook_server_player_data;

/* UnitAny, the only two fields this needs. */
#define UNIT_TYPE        0x00   /* 0 = player */
#define UNIT_PLAYER_DATA 0x14

/* PD2's own additions to PlayerData. See the disassembly above. */
#define PD_SAMPLE_COUNT  0x1A4  /* n; also the "is the meter tracking" gate the draw site reads */
#define PD_PENDING       0x1A8  /* damage dealt since the last averaging tick */
#define PD_AVERAGE       0x1AC  /* the number drawn on screen */
#define PD_WINDOW_START  0x265  /* game tick the current 5-second window began on */

/* D2Client.dll's own GetPlayerUnit, 1.13c. Same export-free ordinal-by-offset approach the
 * sibling pd2-holy-grail plugin uses, and the same value: this is the CLIENT's copy of the
 * player, which is the copy the meter draws from. */
#define OFF_GETPLAYERUNIT 0xA4D60

typedef void *(__stdcall *get_player_unit_fn)(void);

static get_player_unit_fn get_player_unit;
static int ready;

/* Resolve D2Client once it exists. Called from the tick rather than at startup: Game.exe imports
 * this DLL, so we run BEFORE D2Client.dll is loaded and the first look is always going to come up
 * empty. Doing this once at startup meant doing it exactly once, unsuccessfully, and then sitting
 * silent for the whole session — which is precisely what the first in-game run produced. */
static int ensure_ready(void)
{
    if (ready) return 1;

    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return 0;              /* not yet; ask again on the next tick */

    get_player_unit = (get_player_unit_fn)((BYTE *)client + OFF_GETPLAYERUNIT);
    ready = 1;
    log_line("D2Client at %p, GetPlayerUnit at %p", (void *)client, (void *)get_player_unit);
    return 1;
}

/* The client's PlayerData, or NULL when there is no player in a game right now (menus, loading).
 * Every read below goes through this, so a stale pointer cannot outlive a game. */
static BYTE *player_data(void)
{
    if (!ready) return NULL;
    BYTE *unit = (BYTE *)get_player_unit();
    if (!unit) return NULL;
    if (*(DWORD *)(unit + UNIT_TYPE) != 0) return NULL;   /* not a player */
    return *(BYTE **)(unit + UNIT_PLAYER_DATA);
}

/* Stage one is done: writing 1337 to PlayerData+0x1AC put 1337 on screen (2026-09-23, in game).
 * The draw site reads that DWORD and nothing else, so what is left is only choosing the number.
 *
 * Stage two is the open question, and the first session could not answer it: the log showed
 * n/pending/average/window all zero, but nothing was fought, so "the fields never moved" meant
 * nothing. Worse, forcing a constant every tick would MASK anything PD2 wrote there. So the
 * constant is off and this now only watches.
 *
 * What to look for while fighting:
 *   * `pending` or `window` moving  -> PD2's own damage hook IS running offline against this
 *     struct, and the fix is to let it, not to reimplement it.
 *   * `average` moving on its own   -> the whole pipeline runs and something else entirely is
 *     keeping the widget at zero.
 *   * nothing moving at all         -> the hook runs against the SERVER's copy of the player (D2
 *     keeps separate unit lists even in single player) or does not run offline. Next step then is
 *     a hook at ProjectDiablo.dll+0x26F5B6, the `add [eax+0x1a8],ebx` that lands the damage —
 *     which answers both at once and hands us PD2's own already-clamped number.
 *
 * Set this back to a value to force the display again. */
#define DPS_PROBE_CONSTANT 0

/* Every field transition on both structs, plus a running damage total. Invaluable while working
 * out where the number comes from, far too loud once it is known — a single minute of fighting
 * produced a hundred and fifty lines. Off by default; turn it on to diagnose a PD2 update that
 * moves an offset. */
#define DPS_VERBOSE 0

void dps_tick(void)
{
#if DPS_VERBOSE
    static DWORD last_heartbeat;
#endif
    static DWORD seen_count, seen_pending, seen_average, seen_window;
    static int said_no_player, have_seen, said_working;

    if (!ensure_ready()) return;
    hook_install_gate();
#if DPS_VERBOSE
    /* Only for diagnosis. The gate hook is what makes the meter work; this one just counts, and
     * counting costs two extra writes inside someone else's combat resolution. */
    hook_install();
#endif

    /* The server's own copy of the same four fields, once a blow has landed and the gate hook
     * has handed us the pointer. This is where PD2's arithmetic actually happens offline; the
     * widget reads the client's copy, which is why the number has to be carried across. */
    BYTE *server = (BYTE *)hook_server_player_data;
#if DPS_VERBOSE
    static DWORD srv_n, srv_pending, srv_average, srv_window;
    if (server) {
        DWORD n = *(DWORD *)(server + PD_SAMPLE_COUNT);
        DWORD pending = *(DWORD *)(server + PD_PENDING);
        DWORD average = *(DWORD *)(server + PD_AVERAGE);
        DWORD window = *(DWORD *)(server + PD_WINDOW_START);
        if (n != srv_n || pending != srv_pending || average != srv_average
            || window != srv_window) {
            log_line("SERVER   PlayerData %p | n=%lu pending=%lu average=%lu window=%lu",
                     (void *)server, (unsigned long)n, (unsigned long)pending,
                     (unsigned long)average, (unsigned long)window);
            srv_n = n; srv_pending = pending; srv_average = average; srv_window = window;
        }
    }
#endif

    /* What the hook has seen. Reported separately from the struct watch below, because these two
     * answer different questions: this one says whether PD2's damage path runs offline at all,
     * that one says whether it reaches the copy of the player the widget draws from. */
#if DPS_VERBOSE
    static DWORD seen_damage, seen_hits;
    if (hook_damage_total != seen_damage) {
        log_line("DAMAGE total=%lu over %lu hits (+%lu since last)",
                 (unsigned long)hook_damage_total, (unsigned long)hook_hit_count,
                 (unsigned long)(hook_damage_total - seen_damage));
        seen_damage = hook_damage_total;
        seen_hits = hook_hit_count;
    }
    (void)seen_hits;
#endif

    BYTE *data = player_data();
    if (!data) {
        if (!said_no_player) {
            log_line("no player unit yet (main menu, or not in a game)");
            said_no_player = 1;
        }
        return;
    }
    said_no_player = 0;

    /* The gate and the number are all a normal run touches on the client's copy; `pending` and
     * `window` belong to the accumulator, which runs on the server's copy. */
    DWORD *count   = (DWORD *)(data + PD_SAMPLE_COUNT);
    DWORD *average = (DWORD *)(data + PD_AVERAGE);
#if DPS_VERBOSE
    DWORD *pending = (DWORD *)(data + PD_PENDING);
    DWORD *window  = (DWORD *)(data + PD_WINDOW_START);
#endif

#if DPS_VERBOSE
    int changed = !have_seen || *count != seen_count || *pending != seen_pending
                  || *average != seen_average || *window != seen_window;
    DWORD now = GetTickCount();
    if (changed || now - last_heartbeat >= 10000) {
        last_heartbeat = now;
        seen_count = *count; seen_pending = *pending;
        seen_average = *average; seen_window = *window;
        have_seen = 1;
        log_line("%s PlayerData %p | n=%lu pending=%lu average=%lu window=%lu",
                 changed ? "CHANGED" : "  still", (void *)data,
                 (unsigned long)*count, (unsigned long)*pending,
                 (unsigned long)*average, (unsigned long)*window);
    }
#else
    (void)seen_count; (void)seen_pending; (void)seen_average; (void)seen_window; (void)have_seen;
#endif

    /* The whole point, in one line: the widget draws the client's copy, PD2 computes into the
     * server's. Mirrored rather than recomputed, so what shows up is PD2's own number — its
     * averaging, its window, its clamp on overkill — and not an approximation of it.
     *
     * The gate is set here too. The client's copy has its own, maintained by the handshake from
     * the launcher setting, but a zero there means the draw site returns before reading anything
     * at all, so it is not worth depending on. */
    if (server) {
        DWORD server_average = *(DWORD *)(server + PD_AVERAGE);
        if (server_average != 0) {
            if (*count == 0) *count = 1;
            *average = server_average;
            if (!said_working) {
                said_working = 1;
                log_line("mirroring: server %p -> client %p, first reading %lu dps",
                         (void *)server, (void *)data, (unsigned long)server_average);
            }
        }
    }

#if DPS_PROBE_CONSTANT
    /* The gate first, or the draw site returns before it reads the value at all. */
    if (*count == 0) *count = 1;
    *average = DPS_PROBE_CONSTANT;
#endif
}
