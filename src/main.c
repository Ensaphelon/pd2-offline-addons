#include <windows.h>
#include "d2.h"
#include "log.h"
#include "addons.h"

/* One DLL, one thread, three features.
 *
 * Each of these used to ship its own DllMain, its own worker thread and its own copy of the
 * logger, in its own repository, patched into Game.exe by its own copy of the same installer.
 * That arrangement had already produced three patchers drifting apart and a stack of backups
 * where the removal order silently mattered — but the thing that actually forces one binary is
 * the ESC menu: the mechanism points the game's own menu globals at a replacement array, and two
 * modules doing that independently means the last one to run wins and the other's entries vanish
 * without anything reporting a failure.
 *
 * DllMain still does as little as possible. It runs under the loader lock, where waiting on
 * another module is a deadlock waiting to happen, so it starts a thread and gets out of the way.
 */

/* Quick Restart — a RESTART line in the pause menu. Needs the D2 modules present before it can
 * find the menu at all, which is what gates the whole bootstrap below. */
void menu_install(void);
void menu_watch(void);

/* The Holy Grail beam — a shaft of light over a drop worth picking up. */
void beam_init(void *module);
void beam_reload(void);
BOOL frame_hook_install(void);
void charselect_install(void);
BOOL charselect_late(void);

/* The offline DPS meter — resolves its own hooks, retries until PD2's module is loaded. */
void dps_tick(void);

/* Each area's best reading, written beside the DLL for pd2-holy-inventory to read on its next
 * scan. Driven from dps_tick, which already has the number; this only needs to be told where to
 * put the file. */
void records_init(void *module);

/* Affix ranges on the item's own stat lines. Reads its table beside the DLL at startup, then
 * waits for BH.dll — PD2 loads the maphack after us — before patching its one call site. */
void affixes_init(void *module);
BOOL affixes_hook_install(void);
void affixes_reload(void);

/* Restock: PD2's gamble-window refresh, on an ordinary vendor's trade window too. Waits for
 * ProjectDiablo.dll to fill its own packet slot and pointers, then keeps its word in step with the
 * store panel every tick. */
void vendor_tick(void);

/* The controller only while the game window is in front: see focus_input.c. */
BOOL focus_input_install(void);

/* Storm's background reader without Fog's 256 KB/s brake: a game left in its first seconds no
 * longer waits for sounds that were still trickling in. */
void storm_io_tick(void);

#if ADDONS_HANGPROBE
void hangprobe_tick(void);
#endif

/* The development probes that mapped the game's memory and its sound table. They are driven by
 * F1..F12, which in a real game are the skill hotkeys, so they stay out of a normal build: a
 * plugin that answers a skill key by playing a sound and dumping memory is not something to ship
 * by accident. Build with -DADDONS_DEV=1 to get them back. */
#if ADDONS_DEV
void probe_init(void *module);
void probe_run(void);
void probe_play_line(int index);
void probe_survey(void);
void server_baseline(void);
void server_delta(void);
#endif

/* How often beam.txt is re-read. Time-based rather than a tick count: the loop's period is a
 * compromise between three features and has already changed once, and "about a second" should
 * not quietly become "about three" the next time it does. */
#define BEAM_RELOAD_MS 1000

/* 15ms, which is the menu watcher's requirement and the tightest of the three — it mirrors the
 * game's own menu array and wants to see a change in the frame it happens, not after. The other
 * two are cheap enough that running them at the same rate costs nothing worth measuring. */
#define TICK_MS 15

/* How each session ended, and the crashes on the way: see crash.c. */
void crash_init(void);
void crash_start(void);
void crash_tick(void);
void crash_process_detach(void);

static DWORD WINAPI bootstrap(LPVOID module)
{
    log_line("pd2-offline-addons attached");
    crash_start();

    /* Version and modules first. Everything below reads addresses inside D2's own DLLs, so a
     * build this was not mapped against, or a game that has not finished loading, must stop here
     * rather than read whatever happens to be at the offset. */
    if (!d2_version_is_supported()) return 0;

    /* D2Launch has to be patched before its startup runs, and PD2 loads it some way Game.exe's
       LoadLibraryA hook does not see — so look for it every couple of milliseconds until it is
       there. See charselect.c. */
    for (int waited = 0; waited < 3000 && !charselect_late(); waited += 2) Sleep(2);
    if (!d2_wait_for_modules(60000)) {
        log_line("D2's modules never turned up — nothing to attach to");
        return 0;
    }
    log_line("D2Client base %p", (void *)d2.d2client);

    menu_install();
    beam_init(module);
    records_init(module);
    affixes_init(module);
    beam_reload();
#if ADDONS_DEV
    probe_init(module);
#endif

    DWORD last_reload = GetTickCount();
    BOOL frame_hooked = FALSE;
    BOOL affixes_hooked = FALSE;
    BOOL input_hooked = FALSE;

    for (;;) {
        menu_watch();

        /* D2Gfx and the Glide wrapper both load after us, so the beam's frame hook goes in once
         * they are there rather than at startup. */
        if (!frame_hooked) frame_hooked = frame_hook_install();

        /* BH.dll is Project Diablo 2's maphack and lands after us too. */
        if (!affixes_hooked) affixes_hooked = affixes_hook_install();

        /* ProjectDiablo.dll reads the pad; it is loaded by the time the D2 modules are. */
        if (!input_hooked) input_hooked = focus_input_install();

        /* beam.txt is re-read while the game runs, so the light can be tuned by editing a line
         * and looking at the screen rather than by rebuilding and restarting. */
        DWORD now = GetTickCount();
        if (now - last_reload >= BEAM_RELOAD_MS) {
            last_reload = now;
            beam_reload();
            /* Same tick: affix-ranges.cfg decides whether the ranges show always or only while
             * a key is held, and that is worth being able to change without restarting. */
            affixes_reload();
            /* Fog takes the unhandled-exception slot during startup; take it back. */
            crash_tick();
        }

        dps_tick();
        vendor_tick();
        storm_io_tick();
#if ADDONS_HANGPROBE
        hangprobe_tick();
#endif

#if ADDONS_DEV
        static BOOL was_down[12];
        for (int i = 0; i < 12; i++) {
            BOOL down = (GetAsyncKeyState(VK_F1 + i) & 0x8000) != 0;
            if (down && !was_down[i]) {
                if (i == 11) server_delta();
                else if (i == 10) server_baseline();
                else if (i == 9) probe_survey();
                else probe_play_line(i);
            }
            was_down[i] = down;
        }
#endif
        Sleep(TICK_MS);
    }
    return 0;   /* not reached; the thread lives as long as the game does */
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved) {
        /* The process is ending (not an unload). Normally the ExitProcess hook already said so. */
        crash_process_detach();
    }
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        log_init(module);
        crash_init();
        /* Here rather than in bootstrap: it has to be in before Game.exe loads D2Launch, and
           Game.exe's own code has not started while the loader is still running this. */
        charselect_install();
        HANDLE thread = CreateThread(NULL, 0, bootstrap, module, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

/* Game.exe pulls this DLL in through its own import table, and an import needs a symbol to bind
 * to. This is that symbol and nothing more — all the work happens above. */
__declspec(dllexport) int pd2addons_anchor(void) { return 1; }
