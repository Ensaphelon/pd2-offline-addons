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
void probe_play_next_sound(void);

#define PROBE_KEY VK_F9
#define SOUND_KEY VK_F10
#define AUTO_PASS_MS 30000

static DWORD WINAPI bootstrap(LPVOID module)
{
    log_init(module);
    log_line("pd2-holy-grail probe attached");
    probe_init(module);
    log_line("a pass runs by itself %d seconds from now; F9 takes another one any time",
             AUTO_PASS_MS / 1000);
    log_line("F10 plays the next configured sound");

    /* The automatic pass is the one that matters: a keyboard that does not send F9 the way the
       game expects would otherwise leave us with an empty log and no idea why. F9 stays as a way
       to take another look after moving something.

       Polling rather than installing a keyboard hook: a hook is a hook, and the point of this
       build is that it changes nothing about how the game runs. */
    DWORD attached = GetTickCount();
    BOOL auto_done = FALSE;
    BOOL was_down = FALSE, sound_was_down = FALSE;
    for (;;) {
        if (!auto_done && GetTickCount() - attached >= AUTO_PASS_MS) {
            auto_done = TRUE;
            log_line("(automatic pass)");
            probe_run();
        }
        BOOL down = (GetAsyncKeyState(PROBE_KEY) & 0x8000) != 0;
        if (down && !was_down) probe_run();
        was_down = down;

        BOOL sound_down = (GetAsyncKeyState(SOUND_KEY) & 0x8000) != 0;
        if (sound_down && !sound_was_down) probe_play_next_sound();
        sound_was_down = sound_down;
        Sleep(50);
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
