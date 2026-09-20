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

#define PROBE_KEY VK_F9

static DWORD WINAPI bootstrap(LPVOID module)
{
    log_init(module);
    log_line("pd2-holy-grail probe attached");
    probe_init(module);
    log_line("press F9 in game to take a pass; each one writes its findings here");

    /* Polling a key rather than installing a hook: a keyboard hook is a hook, and the whole point
       of this build is that it changes nothing about how the game runs. */
    BOOL was_down = FALSE;
    for (;;) {
        BOOL down = (GetAsyncKeyState(PROBE_KEY) & 0x8000) != 0;
        if (down && !was_down) probe_run();
        was_down = down;
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
