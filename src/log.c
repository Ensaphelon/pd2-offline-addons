#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "log.h"

static char log_path[MAX_PATH];
static CRITICAL_SECTION log_lock;
static int ready = 0;

void log_init(void *module)
{
    char dir[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)module, dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        /* No path means no log, but the plugin must still run — logging is a convenience. */
        return;
    }
    while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/') len--;
    dir[len] = '\0';
    _snprintf(log_path, MAX_PATH, "%spd2holygrail.log", dir);
    log_path[MAX_PATH - 1] = '\0';

    InitializeCriticalSection(&log_lock);
    ready = 1;

    /* Truncate on load: one file per game session is what you actually want to read. */
    HANDLE h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void log_line(const char *fmt, ...)
{
    if (!ready) return;

    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int head = _snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d  ",
                         t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (head < 0) return;

    va_list args;
    va_start(args, fmt);
    int body = _vsnprintf(line + head, sizeof(line) - head - 2, fmt, args);
    va_end(args);
    if (body < 0) body = (int)(sizeof(line) - head - 2);

    int total = head + body;
    line[total++] = '\n';
    line[total] = '\0';

    EnterCriticalSection(&log_lock);
    HANDLE h = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)total, &written, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&log_lock);
}
