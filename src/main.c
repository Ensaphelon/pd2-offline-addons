#include <windows.h>
#include "log.h"

/* Holy Grail probe. It reads the running game's memory and writes a log; it hooks nothing, draws
 * nothing and writes nothing back. That is deliberate for this stage: every question we still
 * have can be answered by looking, and a plugin that only looks cannot break a real save.
 *
 * DllMain starts a thread and returns immediately — it runs under the loader lock, where waiting
 * on anything is a deadlock waiting to happen. */

void probe_init(void *module);
void probe_run(void);
void probe_play_line(int index);
BOOL frame_hook_install(void);
void probe_dump_paths(void);
void probe_survey(void);
void server_baseline(void);
void server_delta(void);

/* One F-key per configured line: F1 plays line 1, F2 line 2, and so on. Sequences and paced
   presses both failed for the same reason — they asked the listener to keep time. A key that
   always plays the same thing can be pressed whenever, as often as you like. */
#define AUTO_PASS_MS 30000

static DWORD WINAPI bootstrap(LPVOID module)
{
    log_init(module);
    log_line("pd2-holy-grail probe attached");
    probe_init(module);
    log_line("F1..F12 each play one configured line; the memory pass runs by itself in %d seconds",
             AUTO_PASS_MS / 1000);

    /* The automatic pass is the one that matters: a keyboard that does not send F9 the way the
       game expects would otherwise leave us with an empty log and no idea why. F9 stays as a way
       to take another look after moving something.

       Polling rather than installing a keyboard hook: a hook is a hook, and the point of this
       build is that it changes nothing about how the game runs. */
    DWORD attached = GetTickCount();
    BOOL auto_done = FALSE;
    BOOL was_down[12] = {0};
    BOOL hooked = FALSE;
    for (;;) {
        if (!auto_done && GetTickCount() - attached >= AUTO_PASS_MS) {
            auto_done = TRUE;
            log_line("(automatic memory pass)");
            probe_run();
        }
        /* D2Gfx and the Glide wrapper both load after us, so the hook goes in once they are
           there rather than at startup. */
        if (!hooked) hooked = frame_hook_install();

        for (int i = 0; i < 12; i++) {
            BOOL down = (GetAsyncKeyState(VK_F1 + i) & 0x8000) != 0;
            /* F12 is the position probe rather than a sound; nine lines is all the config has. */
            if (down && !was_down[i]) { if (i == 11) server_delta();
            else if (i == 10) server_baseline();
            else if (i == 9) probe_survey();
            else probe_play_line(i); }
            was_down[i] = down;
        }
        Sleep(40);
    }
    return 0;   /* not reached; the thread lives as long as the game does */
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(NULL, 0, bootstrap, module, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

/* The import-table patch needs a symbol to import; nothing ever calls it. */
__declspec(dllexport) void pd2holygrail_anchor(void) { }
