#include <windows.h>
#include "log.h"

/* Offline DPS meter for Project Diablo 2 — see src/dps.c for what it does and why.
 *
 * DllMain starts a thread and returns immediately: it runs under the loader lock, where waiting
 * on anything is a deadlock waiting to happen. Same shape as the sibling pd2-holy-grail plugin,
 * which has run in this exact CrossOver/Wine setup for weeks. */

void dps_init(void);
void dps_tick(void);

static DWORD WINAPI bootstrap(LPVOID module)
{
    log_init(module);
    log_line("pd2-dps-meter-offline attached");

    /* D2Client.dll loads after us, so the first attempt usually fails; retry on the tick rather
     * than sleeping a guessed amount at startup. */
    for (;;) {
        static int initialised;
        if (!initialised) {
            dps_init();
            initialised = 1;
        }
        dps_tick();
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

/* The import-table patch used to load this needs a symbol to import; nothing ever calls it. */
__declspec(dllexport) void pd2dpsmeter_anchor(void) { }
