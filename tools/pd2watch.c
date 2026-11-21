#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pd2watch.exe — the game's exit code, from outside the game.
 *
 * Three closes on 2026-11-21 left nothing behind: no exception reached any handler, none of
 * ExitProcess / TerminateProcess / NtTerminateProcess / RaiseFailFastException ran inside the
 * process, and Windows logged no crash. Whatever ended the game ran no code in it, and the one
 * thing that still tells such endings apart is the process's exit code:
 *
 *   0xC00000FD  a stack overflow with no room left to report it
 *   0xC0000409  a fail-fast (a /GS cookie, abort(), an invalid parameter)
 *   1           what taskkill /F and most other killers pass
 *
 * Game.exe refuses other processes every access right, so a watcher cannot open it. pd2addons.dll
 * hands one over instead: it duplicates its own process handle (which needs no permission) and
 * starts this program with that one handle inherited — see crash.c, start_watcher.
 *
 * usage: pd2watch.exe <inherited handle> <pid>
 *
 * When the game ends, and only if its own log has no "exit:" or "crash:" line for this session
 * (an ending the addon saw is already written down, with more detail than this has), it appends
 * a KILLED line to pd2addons-exits.log and an "exit:" line to pd2addons.log, so the next launch
 * does not also call the session NO TRACE. */

static const char *code_meaning(DWORD code)
{
    switch (code) {
    case 0: return "a clean exit code";
    case 1: return "what taskkill /F and most other killers pass";
    case 0xC00000FD: return "stack overflow, with no room left to report it";
    case 0xC0000409: return "fail-fast: a /GS cookie, abort() or an invalid parameter";
    case 0xC0000005: return "access violation";
    case 0xC0000374: return "heap corruption";
    case 0xC000001D: return "illegal instruction";
    case 0xC000013A: return "the console was closed";
    case 0x40010004: return "ended by a debugger";
    case 0xE06D7363: return "an unhandled C++ exception";
    default: return "an unrecognised code";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    HANDLE game = (HANDLE)(DWORD_PTR)strtoul(argv[1], NULL, 10);
    unsigned long pid = strtoul(argv[2], NULL, 10);

    WaitForSingleObject(game, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(game, &code);
    CloseHandle(game);

    SYSTEMTIME now;
    GetLocalTime(&now);

    char dir[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, dir, MAX_PATH);
    while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/') len--;
    dir[len] = '\0';

    /* Did the addon already write this session's ending? Its log names the session by pid. */
    char log_path[MAX_PATH], line[1200], header[64] = "";
    snprintf(log_path, MAX_PATH, "%spd2addons.log", dir);
    char wanted[64];
    snprintf(wanted, sizeof wanted, "pid %lu", pid);
    int ours = 0, ended = 0;
    FILE *log = fopen(log_path, "r");
    if (log) {
        while (fgets(line, sizeof line, log)) {
            if (strlen(line) < 15) continue;
            const char *body = line + 14;
            if (strncmp(body, "session ", 8) == 0) {
                ours = strstr(body, wanted) != NULL;
                if (ours) snprintf(header, sizeof header, "%.19s", body + 8);
                continue;
            }
            if (ours && (strncmp(body, "exit: ", 6) == 0 || strncmp(body, "crash: ", 7) == 0)) ended = 1;
        }
        fclose(log);
    }
    if (!ours || ended) return 0;

    /* With the game's error mode cleared (crash.c), Windows writes its own dump of a fail-fast:
       Game.exe.<pid>.dmp in %LOCALAPPDATA%\CrashDumps, a few seconds after the end. That dump
       names the module and instruction, so it is worth waiting for. */
    char dump[MAX_PATH] = "";
    const char *local = getenv("LOCALAPPDATA");
    if (local && code != 0 && code != 1) {
        char candidate[MAX_PATH];
        snprintf(candidate, MAX_PATH, "%s\\CrashDumps\\Game.exe.%lu.dmp", local, pid);
        for (int waited = 0; waited < 30000; waited += 500) {
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                Sleep(2000);   /* let WER finish writing it */
                snprintf(dump, MAX_PATH, "%s", candidate);
                break;
            }
            Sleep(500);
        }
    }

    char exits_path[MAX_PATH];
    snprintf(exits_path, MAX_PATH, "%spd2addons-exits.log", dir);
    FILE *exits = fopen(exits_path, "a");
    if (exits) {
        fprintf(exits, "%s -> %02d:%02d:%02d  %-8s  exit code 0x%08lX (%s), and nothing inside the game "
                       "saw it end%s%s\r\n",
                header, now.wHour, now.wMinute, now.wSecond, "KILLED", (unsigned long)code,
                code_meaning(code), dump[0] ? "; Windows' dump: " : "", dump);
        fclose(exits);
    }
    log = fopen(log_path, "a");
    if (log) {
        fprintf(log, "%02d:%02d:%02d.%03d  exit: seen only from outside — exit code 0x%08lX (%s)\n",
                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, (unsigned long)code,
                code_meaning(code));
        fclose(log);
    }
    return 0;
}
