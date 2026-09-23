#include <windows.h>
#include "d2.h"
#include "log.h"

/* A "Restart" action for Diablo II 1.13c single player. Injected into the running game (see
   tools/inject.c); nothing in the install is modified.

   DllMain does as little as possible — it runs under the loader lock, where waiting on other
   modules would be a deadlock waiting to happen. It starts a thread and gets out of the way. */

void menu_watch(void);

static DWORD WINAPI bootstrap(LPVOID unused)
{
    (void)unused;

    log_line("pd2-restart starting up");

    if (!d2_version_is_supported()) return 0;
    if (!d2_wait_for_modules(60000)) return 0;

    BYTE *client = (BYTE *)d2.d2client;
    log_line("D2Client base %p", (void *)client);
    log_line("  ExitGame      %p", (void *)(client + OFF_D2CLIENT_EXITGAME));
    log_line("  GetDifficulty %p", (void *)(client + OFF_D2CLIENT_GETDIFFICULTY));
    log_line("  menu header   %p", (void *)(client + OFF_D2CLIENT_MENU));
    log_line("  menu entries  %p", (void *)(client + OFF_D2CLIENT_MENUENTRIES));
    log_line("D2Win!FirstControl %p", (void *)((BYTE *)d2.d2win + OFF_D2WIN_FIRSTCONTROL));

    void menu_install(void);
    menu_install();

    log_line("watching the ESC menu");
    for (;;) {
        menu_watch();
        Sleep(15);
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        log_init(module);
        HANDLE t = CreateThread(NULL, 0, bootstrap, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

/* The plugin is pulled in by Game.exe's own import table (see the app's game_patcher), and an
   import needs a symbol to bind to. This is that symbol and nothing more — all the work happens
   in DllMain. */
__declspec(dllexport) int pd2restart_anchor(void) { return 1; }
