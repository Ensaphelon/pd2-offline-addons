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
#include "log.h"

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

void dps_init(void)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) {
        log_line("D2Client.dll is not loaded yet");
        return;
    }
    get_player_unit = (get_player_unit_fn)((BYTE *)client + OFF_GETPLAYERUNIT);
    ready = 1;
    log_line("attached; D2Client at %p, GetPlayerUnit at %p", (void *)client,
             (void *)get_player_unit);
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

/* Stage one: prove the display path, before a single line of damage accounting is written.
 *
 * The claim to test is that the widget reads nothing but PlayerData+0x1AC. If it does, writing a
 * recognisable constant there puts that constant on screen — and everything else in this project
 * is then only a question of what number to write. If it does NOT, the theory is wrong and it is
 * worth knowing that now rather than after building a damage tracker on top of it.
 *
 * Set DPS_PROBE_CONSTANT to 0 to stop forcing a value. */
#define DPS_PROBE_CONSTANT 1337

void dps_tick(void)
{
    static DWORD last_report;
    static int said_no_player;

    BYTE *data = player_data();
    if (!data) {
        if (!said_no_player) {
            log_line("no player unit yet (main menu, or not in a game)");
            said_no_player = 1;
        }
        return;
    }
    said_no_player = 0;

    DWORD *count   = (DWORD *)(data + PD_SAMPLE_COUNT);
    DWORD *pending = (DWORD *)(data + PD_PENDING);
    DWORD *average = (DWORD *)(data + PD_AVERAGE);
    DWORD *window  = (DWORD *)(data + PD_WINDOW_START);

    /* Once a second is plenty: this is a log to read afterwards, not a trace. */
    DWORD now = GetTickCount();
    if (now - last_report >= 1000) {
        last_report = now;
        log_line("PlayerData %p | n=%lu pending=%lu average=%lu window=%lu",
                 (void *)data, (unsigned long)*count, (unsigned long)*pending,
                 (unsigned long)*average, (unsigned long)*window);
    }

#if DPS_PROBE_CONSTANT
    /* The gate first, or the draw site returns before it reads the value at all. A real session
     * has this set from the launcher's own "dps" setting, but forcing it makes the probe
     * independent of that. */
    if (*count == 0) *count = 1;
    *average = DPS_PROBE_CONSTANT;
#endif
}
