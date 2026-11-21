#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"

/* How each game session ended, one line per session in pd2addons-exits.log beside the DLL.
 *
 * WHY THIS EXISTS. On 2026-11-21 the game closed twice in ten minutes with nothing anywhere:
 * no "Halt" in D2's own D2YYMMDD.txt, no Application Error in the Windows log, and pd2addons.log
 * already overwritten by the next launch. D2's error handler (Fog) catches an ordinary crash and
 * writes its report, and Windows never hears of it — so a close that leaves neither trace did not
 * go through an exception at all, or went through one somebody swallowed. Telling those apart
 * needs a witness inside the process at the moment it ends.
 *
 * WHAT GETS WRITTEN, each line starting with the session's start and end:
 *
 *   closed    ExitProcess or TerminateProcess(self), with the exit code and the caller. Closing
 *             the game yourself lands here too — the caller is what separates the two.
 *   SUSPECT   the same, but within a few seconds of a serious exception that never reached any
 *             crash handler: something caught it and quit quietly.
 *   D2 HALT   ExitProcess called from Fog: D2 stopped itself, and its report is in D2YYMMDD.txt.
 *   CRASH     an exception nothing caught. Registers and the stack go to pd2addons.log, a
 *             minidump to crashes\, and only then is it handed on to Fog as before.
 *   NO TRACE  written at the next launch: the last session logged neither an exit nor a crash,
 *             so the process was killed from outside or died too hard to run any code.
 *
 * pd2addons.log of the previous session survives one launch as pd2addons.prev.log (log.c), which
 * is what the NO TRACE check reads.
 *
 * HOW. The exits are hooked through the hot-patch prologue (five spare bytes and `mov edi,edi`)
 * Windows builds most system functions with, so the hook sees the real caller before any thread
 * is torn down. kernel32!ExitProcess itself has no such prologue on Windows 11 (it opens with
 * push ebp / mov ebp,esp), so the hook sits one call further in, on ntdll!RtlExitUserProcess,
 * which every exit goes through, and takes the caller from kernel32's frame above it.
 * TerminateProcess is hooked at kernel32's own export, which does have the prologue. Unhandled exceptions are taken by holding the top-level
 * filter slot — Fog claims it during startup, so the slot is re-taken every second and Fog's filter
 * is called after ours. A vectored handler notes every serious exception first-chance. */

#define EXITS_FILE          "pd2addons-exits.log"
#define DUMP_DIR            "crashes"
#define DUMPS_KEPT          10
#define FIRST_CHANCE_LINES  20
#define SUSPECT_WINDOW_MS   5000

typedef BOOL (WINAPI *minidump_fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                   PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION,
                                   PMINIDUMP_CALLBACK_INFORMATION);
typedef VOID (NTAPI *exit_process_fn)(LONG);
typedef BOOL (WINAPI *terminate_process_fn)(HANDLE, UINT);

static minidump_fn write_minidump;
static exit_process_fn real_exit_process;
static terminate_process_fn real_terminate_process;
static LPTOP_LEVEL_EXCEPTION_FILTER chained;

static volatile LONG in_crash;
static volatile LONG crashed;
static volatile LONG ending_noted;
static volatile LONG first_chance_lines;

/* The last serious exception seen first-chance, caught later or not. Raw values only: this is
   written on every such exception, and describing one costs a module lookup. */
static volatile DWORD fatal_tick, fatal_code, fatal_params;
static void *volatile fatal_address;
static volatile ULONG_PTR fatal_op, fatal_target;
static void *volatile last_logged_address;

/* ---- describing things ------------------------------------------------------------------- */

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static HMODULE module_of(const void *address)
{
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &m))
        return NULL;
    return m;
}

static void module_name(HMODULE m, char *out, size_t size)
{
    char path[MAX_PATH];
    if (!m || !GetModuleFileNameA(m, path, MAX_PATH)) { _snprintf(out, size, "?"); }
    else _snprintf(out, size, "%s", base_name(path));
    out[size - 1] = '\0';
}

static void describe_address(const void *address, char *out, size_t size)
{
    HMODULE m = address ? module_of(address) : NULL;
    if (m) {
        char name[MAX_PATH];
        module_name(m, name, sizeof name);
        _snprintf(out, size, "%s+0x%lX", name,
                  (unsigned long)((const BYTE *)address - (const BYTE *)m));
    } else if (address) {
        _snprintf(out, size, "0x%08lX (in no module)", (unsigned long)(DWORD_PTR)address);
    } else {
        _snprintf(out, size, "an unknown caller");
    }
    out[size - 1] = '\0';
}

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:        return "access violation";
    case EXCEPTION_STACK_OVERFLOW:          return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:     return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:        return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW:            return "integer overflow";
    case EXCEPTION_IN_PAGE_ERROR:           return "in-page error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT:   return "misaligned data";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "float divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION:   return "invalid float operation";
    case 0xC0000374:                        return "heap corruption";
    case 0xC0000409:                        return "stack buffer overrun";
    case 0xC0000417:                        return "invalid CRT parameter";
    case 0xE06D7363:                        return "C++ exception";
    default:                                return NULL;
    }
}

static void describe_exception(DWORD code, void *address, DWORD params, ULONG_PTR op,
                               ULONG_PTR target, char *out, size_t size)
{
    char where[160], what[64];
    describe_address(address, where, sizeof where);
    const char *name = exception_name(code);
    if (name) _snprintf(what, sizeof what, "%s (0x%08lX)", name, (unsigned long)code);
    else      _snprintf(what, sizeof what, "exception 0x%08lX", (unsigned long)code);
    what[sizeof what - 1] = '\0';

    if (code == EXCEPTION_ACCESS_VIOLATION && params >= 2) {
        const char *verb = op == 0 ? "reading" : op == 8 ? "executing" : "writing";
        _snprintf(out, size, "%s %s 0x%08lX at %s", what, verb, (unsigned long)target, where);
    } else {
        _snprintf(out, size, "%s at %s", what, where);
    }
    out[size - 1] = '\0';
}

static void describe_record(const EXCEPTION_RECORD *r, char *out, size_t size)
{
    describe_exception(r->ExceptionCode, r->ExceptionAddress, r->NumberParameters,
                       r->NumberParameters >= 1 ? r->ExceptionInformation[0] : 0,
                       r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0, out, size);
}

/* Values on the stack that point into a module's code — return addresses, mostly, though a
   stale one from an earlier call can sit among them. A pointer to a module's very start is a
   module handle, not a return address, so the first page is skipped. */
static void scan_stack(const DWORD *sp, const void *stack_base, char *out, size_t size, int wanted)
{
    size_t used = 0;
    int found = 0;
    out[0] = '\0';
    for (const DWORD *p = sp; found < wanted && (const void *)(p + 1) <= stack_base
                              && p < sp + 1024; p++) {
        HMODULE m = module_of((const void *)(DWORD_PTR)*p);
        if (!m || (DWORD_PTR)*p - (DWORD_PTR)m < 0x1000) continue;
        char at[160];
        describe_address((const void *)(DWORD_PTR)*p, at, sizeof at);
        int n = _snprintf(out + used, size - used, " %s", at);
        if (n < 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
        found++;
    }
    if (!found) _snprintf(out, size, " (nothing that looks like code)");
    out[size - 1] = '\0';
}

/* ---- the exits file ---------------------------------------------------------------------- */

static void format_time(const SYSTEMTIME *t, int with_date, char *out, size_t size)
{
    if (with_date)
        _snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d",
                  t->wYear, t->wMonth, t->wDay, t->wHour, t->wMinute, t->wSecond);
    else
        _snprintf(out, size, "%02d:%02d:%02d", t->wHour, t->wMinute, t->wSecond);
    out[size - 1] = '\0';
}

static void append_exit_line(const char *line)
{
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s" EXITS_FILE, log_dir());
    path[MAX_PATH - 1] = '\0';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    CloseHandle(h);
}

/* One line for this session: when it ran, the verdict, and what was seen. */
static void record_exit(const char *verdict, const char *fmt, ...)
{
    char start[32], end[16], text[1024], line[1200];
    SYSTEMTIME now;
    GetLocalTime(&now);
    format_time(log_session_start(), 1, start, sizeof start);
    format_time(&now, 0, end, sizeof end);

    va_list args;
    va_start(args, fmt);
    _vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    text[sizeof text - 1] = '\0';

    _snprintf(line, sizeof line, "%s -> %s  %-8s  %s", start, end, verdict, text);
    line[sizeof line - 1] = '\0';
    append_exit_line(line);
}

static void d2_log_name(const SYSTEMTIME *t, char *out, size_t size)
{
    _snprintf(out, size, "D2%02d%02d%02d.txt", t->wYear % 100, t->wMonth, t->wDay);
    out[size - 1] = '\0';
}

/* ---- the end of the process -------------------------------------------------------------- */

static void note_ending(const char *how, UINT code, void *caller, const DWORD *sp)
{
    if (InterlockedExchange(&ending_noted, 1)) return;

    char from[160], trail[700];
    describe_address(caller, from, sizeof from);
    scan_stack(sp, ((NT_TIB *)NtCurrentTeb())->StackBase, trail, sizeof trail, 8);
    log_line("exit: %s(%u) called from %s", how, code, from);
    log_line("exit: on the stack above it:%s", trail);

    if (crashed) return;   /* the CRASH line is already written */

    DWORD tick = fatal_tick;
    DWORD since = GetTickCount() - tick;
    char caller_module[MAX_PATH];
    module_name(caller ? module_of(caller) : NULL, caller_module, sizeof caller_module);

    if (tick && since < SUSPECT_WINDOW_MS) {
        char what[320];
        describe_exception(fatal_code, fatal_address, fatal_params, fatal_op, fatal_target,
                           what, sizeof what);
        record_exit("SUSPECT", "%s(%u) from %s, %lu ms after an exception no handler reported: %s",
                    how, code, from, (unsigned long)since, what);
    } else if (_stricmp(caller_module, "Fog.dll") == 0) {
        char d2log[32];
        SYSTEMTIME now;
        GetLocalTime(&now);
        d2_log_name(&now, d2log, sizeof d2log);
        record_exit("D2 HALT", "%s(%u) from %s; D2's own report is in %s", how, code, from, d2log);
    } else {
        record_exit("closed", "%s(%u) from %s", how, code, from);
    }
}

static BOOL is_plumbing(HMODULE m)
{
    char name[MAX_PATH];
    module_name(m, name, sizeof name);
    return _stricmp(name, "kernel32.dll") == 0 || _stricmp(name, "kernelbase.dll") == 0
        || _stricmp(name, "ntdll.dll") == 0;
}

/* Entered by a jump from RtlExitUserProcess's first instruction, so the return address is
   whoever called it — kernel32!ExitProcess, normally. That function keeps a plain EBP frame, and
   its return address one frame up is the caller worth naming. */
__attribute__((noinline, optimize("no-omit-frame-pointer")))
static VOID NTAPI on_exit_process(LONG code)
{
    void *caller = __builtin_return_address(0);
    void **frame = (void **)__builtin_frame_address(0);
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    for (int hops = 0; hops < 2 && is_plumbing(module_of(caller)); hops++) {
        void **up = (void **)frame[0];
        if (up <= frame || (void *)(up + 2) > tib->StackBase) break;
        frame = up;
        caller = frame[1];
    }
    note_ending("ExitProcess", (UINT)code, caller, (const DWORD *)&code);
    real_exit_process(code);
}

static BOOL WINAPI on_terminate_process(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        note_ending("TerminateProcess", code, __builtin_return_address(0),
                    (const DWORD *)&process);
    return real_terminate_process(process, code);
}

/* kernel32 hands most of its exports to kernelbase through a `jmp [import]` stub; the function
   with the hot-patch prologue is at the far end of it. */
static BYTE *resolve_export(const char *module, const char *name)
{
    BYTE *fn = (BYTE *)GetProcAddress(GetModuleHandleA(module), name);
    for (int hops = 0; fn && hops < 3 && fn[0] == 0xFF && fn[1] == 0x25; hops++)
        fn = **(BYTE ***)(fn + 2);
    return fn;
}

/* `mov edi,edi` with five spare bytes in front: the long jump goes in the spare bytes, a short
   jump back to it over `mov edi,edi`, and the original carries on from fn+2. Both writes are
   checked against what should be there first; a function that does not look like this is left
   alone. */
static void *hotpatch(BYTE *fn, void *hook)
{
    if (!fn || fn[0] != 0x8B || fn[1] != 0xFF) return NULL;
    for (int i = 1; i <= 5; i++)
        if (fn[-i] != 0xCC && fn[-i] != 0x90) return NULL;

    DWORD was;
    if (!VirtualProtect(fn - 5, 7, PAGE_EXECUTE_READWRITE, &was)) return NULL;
    LONG rel = (LONG)((BYTE *)hook - fn);
    fn[-5] = 0xE9;
    memcpy(fn - 4, &rel, 4);
    *(volatile WORD *)fn = 0xF9EB;   /* jmp short fn-5 */
    VirtualProtect(fn - 5, 7, was, &was);
    FlushInstructionCache(GetCurrentProcess(), fn - 5, 7);
    return fn + 2;
}

/* ---- crashes ----------------------------------------------------------------------------- */

struct crash_job {
    EXCEPTION_POINTERS *ep;
    DWORD thread_id;
    const void *stack_base, *stack_limit;
};

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Dump names sort by time, so the oldest are the first in name order. */
static void prune_dumps(const char *dir)
{
    static char names[64][MAX_PATH];
    char pattern[MAX_PATH];
    int n = 0;
    _snprintf(pattern, MAX_PATH, "%s\\pd2-*.dmp", dir);
    pattern[MAX_PATH - 1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64) { strncpy(names[n], fd.cFileName, MAX_PATH - 1); names[n][MAX_PATH - 1] = 0; n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, compare_names);
    for (int i = 0; i + DUMPS_KEPT < n; i++) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\%s", dir, names[i]);
        path[MAX_PATH - 1] = '\0';
        DeleteFileA(path);
    }
}

static void write_dump(struct crash_job *job, char *shown, size_t size)
{
    shown[0] = '\0';
    if (!write_minidump) { log_line("crash: no dbghelp, so no dump"); return; }

    char dir[MAX_PATH], path[MAX_PATH], name[64];
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf(dir, MAX_PATH, "%s" DUMP_DIR, log_dir());
    dir[MAX_PATH - 1] = '\0';
    CreateDirectoryA(dir, NULL);
    _snprintf(name, sizeof name, "pd2-%04d-%02d-%02d_%02d-%02d-%02d.dmp",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    _snprintf(path, MAX_PATH, "%s\\%s", dir, name);
    path[MAX_PATH - 1] = '\0';

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        log_line("crash: could not create %s (error %lu)", path, GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info = { job->thread_id, job->ep, FALSE };
    BOOL ok = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), f,
                             (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory
                                             | MiniDumpScanMemory),
                             &info, NULL, NULL);
    DWORD error = GetLastError();
    CloseHandle(f);
    if (!ok) {
        log_line("crash: the dump failed (error %lu)", error);
        DeleteFileA(path);
        return;
    }
    _snprintf(shown, size, DUMP_DIR "\\%s", name);
    shown[size - 1] = '\0';
    log_line("crash: dump written to %s", shown);
    prune_dumps(dir);
}

/* Everything slow happens here, on a thread of its own: the crashed thread may have almost no
   stack left (an overflow) and must not be asked to format, open files or walk modules. */
static DWORD WINAPI crash_worker(LPVOID arg)
{
    struct crash_job *job = arg;
    const CONTEXT *c = job->ep->ContextRecord;
    char what[320], trail[900], dump[MAX_PATH];

    describe_record(job->ep->ExceptionRecord, what, sizeof what);
    log_line("crash: %s, thread %lu, and nothing caught it", what, job->thread_id);
    log_line("crash: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX "
             "ebp=%08lX esp=%08lX eip=%08lX",
             c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);

    /* Only a stack pointer inside the crashed thread's own stack is worth reading from. */
    const BYTE *esp = (const BYTE *)(DWORD_PTR)c->Esp;
    if (esp >= (const BYTE *)job->stack_limit - 0x10000 && esp < (const BYTE *)job->stack_base) {
        scan_stack((const DWORD *)esp, job->stack_base, trail, sizeof trail, 16);
        log_line("crash: on the stack:%s", trail);
    } else {
        log_line("crash: the stack pointer is outside the thread's stack — not read");
    }

    write_dump(job, dump, sizeof dump);
    record_exit("CRASH", "%s%s%s", what, dump[0] ? "; dump " : "", dump);
    return 0;
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    /* A filter further down that hands back to whatever it replaced can lead here again. */
    if (InterlockedExchange(&in_crash, 1)) return EXCEPTION_CONTINUE_SEARCH;
    crashed = 1;

    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    struct crash_job job = { ep, GetCurrentThreadId(), tib->StackBase, tib->StackLimit };
    HANDLE t = CreateThread(NULL, 0, crash_worker, &job, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 20000);
        CloseHandle(t);
    }

    LPTOP_LEVEL_EXCEPTION_FILTER next = chained;
    return next ? next(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI on_first_chance(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000) != 0xC0000000) return EXCEPTION_CONTINUE_SEARCH;

    fatal_code = r->ExceptionCode;
    fatal_address = r->ExceptionAddress;
    fatal_params = r->NumberParameters;
    fatal_op = r->NumberParameters >= 1 ? r->ExceptionInformation[0] : 0;
    fatal_target = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    DWORD now = GetTickCount();
    fatal_tick = now ? now : 1;

    /* An overflowing thread has no room for a log line. The unhandled filter, which does its
       work on another thread, is where an overflow gets written down. */
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;

    if (r->ExceptionAddress != last_logged_address && first_chance_lines < FIRST_CHANCE_LINES) {
        last_logged_address = r->ExceptionAddress;
        LONG n = InterlockedIncrement(&first_chance_lines);
        char what[320];
        describe_record(r, what, sizeof what);
        log_line("exception, first chance (something may still catch it): %s, thread %lu%s",
                 what, GetCurrentThreadId(),
                 n == FIRST_CHANCE_LINES ? " — the last such line this session" : "");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- the previous session ---------------------------------------------------------------- */

/* D2's own halts, as Fog writes them: a "TIME:" line, then "Expression : ..." a few lines on.
   The last one at or after `since` ("YYYY-MM-DD HH:MM:SS") is copied out. */
static void find_d2_halt(const char *since, const SYSTEMTIME *day, char *out, size_t size)
{
    char name[32], path[MAX_PATH], line[1024], when[24] = "";
    out[0] = '\0';
    d2_log_name(day, name, sizeof name);
    _snprintf(path, MAX_PATH, "%s%s", log_dir(), name);
    path[MAX_PATH - 1] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *time_at = strstr(line, "TIME:");
        if (time_at) {
            time_at += 5;
            while (*time_at == ' ') time_at++;
            if (strlen(time_at) >= 19 && strncmp(time_at, since, 19) >= 0) {
                memcpy(when, time_at, 19);
                when[19] = '\0';
            } else {
                when[0] = '\0';
            }
        } else if (when[0] && strncmp(line, "Expression : ", 13) == 0) {
            _snprintf(out, size, "%s at %s; D2's own report is in %s", line + 13, when + 11, name);
            out[size - 1] = '\0';
        }
    }
    fclose(f);
}

static void review_previous(void)
{
    char path[MAX_PATH], line[1200], last[1200] = "", date[16] = "", start[16] = "";
    int saw_header = 0, saw_end = 0;
    _snprintf(path, MAX_PATH, "%spd2addons.prev.log", log_dir());
    path[MAX_PATH - 1] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) < 15) continue;
        const char *body = line + 14;
        if (!saw_header && strncmp(body, "session ", 8) == 0 && strlen(body) >= 27) {
            saw_header = 1;
            memcpy(date, body + 8, 10);   date[10] = '\0';
            memcpy(start, body + 19, 8);  start[8] = '\0';
        }
        if (strncmp(body, "exit: ", 6) == 0 || strncmp(body, "crash: ", 7) == 0) saw_end = 1;
        strcpy(last, line);
    }
    fclose(f);

    /* A log from a build that wrote no session line cannot say how it ended. Only lines the end of
       a session writes count: "exit: " and "crash: " — the watcher's own startup lines say
       "crashwatch: " for exactly this reason. */
    if (!saw_header || saw_end) return;

    char since[24], halt[600];
    SYSTEMTIME day = { 0 };
    _snprintf(since, sizeof since, "%s %s", date, start);
    since[sizeof since - 1] = '\0';
    day.wYear = (WORD)atoi(date);
    day.wMonth = (WORD)atoi(date + 5);
    day.wDay = (WORD)atoi(date + 8);
    find_d2_halt(since, &day, halt, sizeof halt);

    char text[1500];
    if (halt[0])
        _snprintf(text, sizeof text, "%s -> %.8s  %-8s  %s", since, last, "D2 HALT", halt);
    else
        _snprintf(text, sizeof text,
                  "%s -> %.8s  %-8s  neither an exit nor a crash reached the addon: killed from "
                  "outside, or died too hard to run any code. Last line logged: %s",
                  since, last, "NO TRACE", last + 14);
    text[sizeof text - 1] = '\0';
    append_exit_line(text);
    log_line("crashwatch: the previous session ended without an exit — written to " EXITS_FILE);
}

/* ---- wiring ------------------------------------------------------------------------------ */

/* From DllMain: cheap, no waiting, nothing loaded. */
void crash_init(void)
{
    AddVectoredExceptionHandler(1, on_first_chance);

    real_exit_process = (exit_process_fn)hotpatch(resolve_export("ntdll.dll", "RtlExitUserProcess"),
                                                  (void *)on_exit_process);
    real_terminate_process = (terminate_process_fn)hotpatch(
        resolve_export("kernel32.dll", "TerminateProcess"), (void *)on_terminate_process);
}

/* Re-takes the unhandled-exception slot; whatever held it is called after us. Once a second
   from bootstrap, because Fog claims the slot during startup, after we have loaded. */
void crash_tick(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(on_unhandled);
    if (previous == on_unhandled) return;
    chained = previous;
    char who[160];
    describe_address((void *)previous, who, sizeof who);
    log_line("crashwatch: watching for crashes, handing each on to %s",
             previous ? who : "Windows (nothing else was watching)");
}

/* From bootstrap, before anything else. */
void crash_start(void)
{
    log_line("crashwatch: ExitProcess %s, TerminateProcess %s",
             real_exit_process ? "hooked" : "NOT hooked (prologue not as expected)",
             real_terminate_process ? "hooked" : "NOT hooked (prologue not as expected)");
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (dbghelp) write_minidump = (minidump_fn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    review_previous();
    crash_tick();
}

/* From DllMain at process detach — only says anything if the ExitProcess hook never ran. */
void crash_process_detach(void)
{
    DWORD here = 0;
    note_ending("process detach (ExitProcess not hooked)", 0, NULL, &here);
}
