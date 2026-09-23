#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "d2.h"
#include "log.h"

d2_modules d2 = { 0 };

/* The same check Project Diablo 2's own BH makes, for the same reason: the offsets below only
   mean anything on one build. Reading Game.exe's FileVersion is how BH tells 1.13c (1.0.13.60)
   from 1.13d (1.0.13.64). */
int d2_version_is_supported(void)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA("Game.exe", &handle);
    if (size == 0) {
        log_line("version: cannot read Game.exe version info (err %u) — standing down", GetLastError());
        return 0;
    }

    void *buffer = HeapAlloc(GetProcessHeap(), 0, size);
    if (!buffer) return 0;

    int ok = 0;
    if (GetFileVersionInfoA("Game.exe", handle, size, buffer)) {
        VS_FIXEDFILEINFO *info = NULL;
        UINT info_len = 0;
        if (VerQueryValueA(buffer, "\\", (LPVOID *)&info, &info_len) && info_len &&
            info->dwSignature == 0xfeef04bd) {
            char version[64];
            _snprintf(version, sizeof(version), "%u.%u.%u.%u",
                      HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                      HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
            version[sizeof(version) - 1] = '\0';
            ok = (strcmp(version, D2_REQUIRED_VERSION) == 0);
            log_line("version: Game.exe is %s — %s", version,
                     ok ? "1.13c, supported" : "not 1.13c, standing down");
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return ok;
}

/* Game.exe loads the interface DLLs itself, when it needs them, so "not there yet" is the normal
   state at plugin load — not an error. Poll, with a ceiling: never spin forever inside somebody
   else's process. */
int d2_wait_for_modules(int timeout_ms)
{
    const int step_ms = 100;
    for (int waited = 0; waited <= timeout_ms; waited += step_ms) {
        d2.d2client = GetModuleHandleA("D2Client.dll");
        d2.d2win    = GetModuleHandleA("D2Win.dll");
        d2.d2launch = GetModuleHandleA("D2Launch.dll");
        /* Optional: Project Diablo 2's own module owns the ESC menu when it is present. */
        d2.pd2mod   = GetModuleHandleA("ProjectDiablo.dll");
        if (d2.d2client && d2.d2win) {
            log_line("modules: D2Client=%p D2Win=%p D2Launch=%p ProjectDiablo=%p (after %d ms)",
                     (void *)d2.d2client, (void *)d2.d2win, (void *)d2.d2launch,
                     (void *)d2.pd2mod, waited);
            return 1;
        }
        Sleep(step_ms);
    }
    log_line("modules: D2Client/D2Win never showed up within %d ms — standing down", timeout_ms);
    return 0;
}
