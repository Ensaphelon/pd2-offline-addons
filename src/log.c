#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "log.h"

static char log_path[MAX_PATH];
static char log_folder[MAX_PATH];
static SYSTEMTIME session_start;
static CRITICAL_SECTION log_lock;
static int ready = 0;

const char *log_dir(void) { return log_folder; }
const SYSTEMTIME *log_session_start(void) { return &session_start; }

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
    GetLocalTime(&session_start);
    memcpy(log_folder, dir, len + 1);
    _snprintf(log_path, MAX_PATH, "%spd2addons.log", dir);
    log_path[MAX_PATH - 1] = '\0';

    InitializeCriticalSection(&log_lock);
    ready = 1;

    /* One file per game session is what you actually want to read — but the one before is kept
       as pd2addons.prev.log, because a session that ended badly is read after the next launch,
       and truncating it on that launch is exactly how a silent close left nothing behind
       (2026-11-21). crash.c reads it to say how that session ended. */
    char previous[MAX_PATH];
    _snprintf(previous, MAX_PATH, "%spd2addons.prev.log", dir);
    previous[MAX_PATH - 1] = '\0';
    MoveFileExA(log_path, previous, MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* crash.c finds the session's date and start here; keep the shape. */
    log_line("session %04d-%02d-%02d %02d:%02d:%02d pid %lu",
             session_start.wYear, session_start.wMonth, session_start.wDay,
             session_start.wHour, session_start.wMinute, session_start.wSecond,
             (unsigned long)GetCurrentProcessId());
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
