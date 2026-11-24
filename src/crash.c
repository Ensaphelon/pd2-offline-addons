#define _WIN32_WINNT 0x0601   /* the inherited-handle list for pd2watch.exe */
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
 * TerminateProcess is hooked at kernel32's own export, which does have the prologue.
 *
 * WIDER NET (2026-11-21, after a close that none of the above saw: no exit, no exception, nothing
 * from Windows). Every way out of a process from user mode ends in ntdll!NtTerminateProcess, so
 * that is hooked too: its first instruction is a five-byte `mov eax, <syscall number>`, which a
 * jump replaces and a two-instruction trampoline carries out. kernelbase!TerminateProcess (what
 * modern code reaches through api-set imports, skipping kernel32) and RaiseFailFastException are
 * hooked by their hot-patch prologues. C++ exceptions are logged first-chance with their type,
 * a stack overflow is written down by a watchdog thread (the overflowing one has no stack left to
 * do it), and pd2addons.alive is rewritten every two seconds so a session that vanishes still says
 * roughly when. Unhandled exceptions are taken by holding the top-level
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
typedef LONG (NTAPI *nt_terminate_fn)(HANDLE, LONG);
typedef VOID (WINAPI *fail_fast_fn)(PEXCEPTION_RECORD, PCONTEXT, DWORD);

static minidump_fn write_minidump;
static exit_process_fn real_exit_process;
static terminate_process_fn real_terminate_process;
static terminate_process_fn real_base_terminate_process;
static nt_terminate_fn real_nt_terminate;
static fail_fast_fn real_fail_fast;
typedef BOOL (WINAPI *feature_fn)(DWORD);
static feature_fn real_feature_k32, real_feature_base;
static volatile LONG fail_fast_noted;
static HANDLE overflow_event;
static volatile LONG cpp_lines;
#define CPP_LINES 20
#define ALIVE_FILE "pd2addons.alive"
#ifndef PF_FASTFAIL_AVAILABLE
#define PF_FASTFAIL_AVAILABLE 23
#endif
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

/* Walks out of ntdll/kernel32/kernelbase through their EBP frames, so the caller named is whoever
   asked to end the process rather than the plumbing it went through. */
static BOOL is_plumbing(HMODULE m);
static void *outer_caller(void *caller, void **frame)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    for (int hops = 0; hops < 4 && is_plumbing(module_of(caller)); hops++) {
        void **up = (void **)frame[0];
        if (up <= frame || (void *)(up + 2) > tib->StackBase) break;
        frame = up;
        caller = frame[1];
    }
    return caller;
}

static BOOL is_self(HANDLE process)
{
    return process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId();
}

__attribute__((noinline, optimize("no-omit-frame-pointer")))
static BOOL WINAPI on_base_terminate_process(HANDLE process, UINT code)
{
    if (is_self(process))
        note_ending("TerminateProcess (kernelbase)", code,
                    outer_caller(__builtin_return_address(0), (void **)__builtin_frame_address(0)),
                    (const DWORD *)&process);
    return real_base_terminate_process(process, code);
}

/* NULL is "every thread but this one", which only a process on its way out asks for. */
__attribute__((noinline, optimize("no-omit-frame-pointer")))
static LONG NTAPI on_nt_terminate(HANDLE process, LONG status)
{
    if (!process || is_self(process))
        note_ending("NtTerminateProcess", (UINT)status,
                    outer_caller(__builtin_return_address(0), (void **)__builtin_frame_address(0)),
                    (const DWORD *)&process);
    return real_nt_terminate(process, status);
}

__attribute__((noinline, optimize("no-omit-frame-pointer")))
static VOID WINAPI on_fail_fast(PEXCEPTION_RECORD record, PCONTEXT context, DWORD flags)
{
    if (record) {
        char what[320];
        describe_record(record, what, sizeof what);
        log_line("exit: fail-fast requested for %s", what);
    }
    note_ending("RaiseFailFastException", record ? (UINT)record->ExceptionCode : 0,
                outer_caller(__builtin_return_address(0), (void **)__builtin_frame_address(0)),
                (const DWORD *)&record);
    real_fail_fast(record, context, flags);
}

/* A five-byte `mov eax, imm32` at the function's start (every WoW64 ntdll syscall stub opens so):
   the jump to the hook takes its place, and the trampoline does the mov and jumps back past it. */
static void *hook_mov_eax(BYTE *fn, void *hook)
{
    if (!fn || fn[0] != 0xB8) return NULL;
    BYTE *tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    LONG back = (LONG)((fn + 5) - (tramp + 10));
    memcpy(tramp + 6, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);

    DWORD was;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &was)) return NULL;
    BYTE patch[5] = { 0xE9 };
    LONG rel = (LONG)((BYTE *)hook - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy(fn, patch, 5);
    VirtualProtect(fn, 5, was, &was);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return tramp;
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

/* Does this thread hold the loader lock? A thread started now would wait for it to run its own
   DLL attach, so starting one and waiting on it is a deadlock until the wait gives up. */
static BOOL holds_loader_lock(void)
{
    BYTE *peb = *(BYTE **)((BYTE *)NtCurrentTeb() + 0x30);
    RTL_CRITICAL_SECTION *lock = peb ? *(RTL_CRITICAL_SECTION **)(peb + 0xA0) : NULL;
    return lock && (DWORD)(DWORD_PTR)lock->OwningThread == GetCurrentThreadId();
}

/* The slow half of a crash report, on a thread of its own (see crash_worker). The job lives in
   static storage, not on this stack: if the wait below gives up, the worker may still start
   later, and on 2026-11-24 a job left on a returned stack frame was read back as garbage and
   crashed the game on startup. One report per process is all there ever is. */
static void run_crash_worker(EXCEPTION_POINTERS *ep)
{
    static struct crash_job job;
    static EXCEPTION_RECORD record;
    static CONTEXT context;
    static EXCEPTION_POINTERS pointers;
    if (holds_loader_lock()) {
        log_line("crash: under the loader lock, so no dump (a thread could not start)");
        return;
    }
    record = *ep->ExceptionRecord;
    context = *ep->ContextRecord;
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    job.ep = &pointers;
    job.thread_id = GetCurrentThreadId();
    job.stack_base = tib->StackBase;
    job.stack_limit = tib->StackLimit;
    HANDLE t = CreateThread(NULL, 0, crash_worker, &job, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 20000);
        CloseHandle(t);
    }
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    /* A filter further down that hands back to whatever it replaced can lead here again. */
    if (InterlockedExchange(&in_crash, 1)) return EXCEPTION_CONTINUE_SEARCH;
    crashed = 1;

    run_crash_worker(ep);
    LPTOP_LEVEL_EXCEPTION_FILTER next = chained;
    return next ? next(ep) : EXCEPTION_CONTINUE_SEARCH;
}

/* The moment before a fail-fast. MSVC's runtime asks IsProcessorFeaturePresent(
   PF_FASTFAIL_AVAILABLE) on each of its fatal paths — a failed /GS cookie (__report_gsfailure),
   abort(), an invalid parameter — and then executes `int 0x29`, which no handler ever sees. The
   question is the last moment anything can be written down, so it is answered by writing:
   which module asked, the stack (whose return addresses include the function whose cookie
   failed), and a dump with this thread's own context. The error modes are cleared too, the
   thread's as well as the process's, so Windows' own report is not suppressed either.

   2026-11-21: after the BH buffer fix the game still closed with 0xC0000409, and nothing so far
   could name the module.

   Not every caller is about to die, though: VCRUNTIME140 asks the same question at startup
   (+0x176E7, a capability check that goes on to `mov eax,1`), and on 2026-11-24 treating that as
   a fail-fast stalled the start for 20 seconds and then crashed it. A real fail-fast site runs
   `int 0x29` (CD 29) within a few bytes of the call — __report_gsfailure, abort() and
   _invoke_watson all do `test eax,eax / je / push n / pop ecx / int 0x29` — so only a caller
   whose next bytes hold it counts. */
static BOOL readable(const void *p, size_t n);

static BOOL fail_fast_site(const BYTE *caller)
{
    if (!readable(caller, 16)) return FALSE;
    for (int i = 0; i + 1 < 16; i++)
        if (caller[i] == 0xCD && caller[i + 1] == 0x29) return TRUE;
    return FALSE;
}

static void note_fail_fast(void *caller)
{
    if (!fail_fast_site((const BYTE *)caller)) return;
    if (InterlockedExchange(&fail_fast_noted, 1) || crashed) return;
    SetThreadErrorMode(0, NULL);
    SetErrorMode(GetErrorMode() & ~SEM_NOGPFAULTERRORBOX);

    char from[160];
    describe_address(caller, from, sizeof from);
    log_line("crash: a fail-fast was asked for by %s — writing down what is known before it lands",
             from);
    crashed = 1;

    CONTEXT context;
    memset(&context, 0, sizeof context);
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof record);
    record.ExceptionCode = 0xC0000409;
    record.ExceptionAddress = caller;
    EXCEPTION_POINTERS pointers = { &record, &context };
    run_crash_worker(&pointers);
}

__attribute__((noinline))
static BOOL WINAPI on_feature_k32(DWORD feature)
{
    if (feature == PF_FASTFAIL_AVAILABLE) note_fail_fast(__builtin_return_address(0));
    return real_feature_k32(feature);
}

__attribute__((noinline))
static BOOL WINAPI on_feature_base(DWORD feature)
{
    /* Reached straight by api-set imports, and by kernel32's own stub after on_feature_k32 —
       whose caller is plumbing, and the note is taken once anyway. */
    if (feature == PF_FASTFAIL_AVAILABLE && !is_plumbing(module_of(__builtin_return_address(0))))
        note_fail_fast(__builtin_return_address(0));
    return real_feature_base(feature);
}

/* Readable memory, asked of the memory manager rather than found out by faulting. */
static BOOL readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

/* MSVC's throw: [0] magic 0x19930520, [1] the object, [2] its ThrowInfo. The type's decorated
   name sits behind ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor. */
static void note_cpp_exception(const EXCEPTION_RECORD *r, const CONTEXT *c)
{
    if (cpp_lines >= CPP_LINES) return;
    LONG n = InterlockedIncrement(&cpp_lines);
    if (n > CPP_LINES) return;
    const char *type = "?";
    if (r->NumberParameters >= 3 && r->ExceptionInformation[0] == 0x19930520) {
        const DWORD *info = (const DWORD *)r->ExceptionInformation[2];
        if (readable(info, 16)) {
            const DWORD *array = (const DWORD *)(DWORD_PTR)info[3];
            if (readable(array, 8) && array[0] > 0) {
                const DWORD *catchable = (const DWORD *)(DWORD_PTR)array[1];
                if (readable(catchable, 8)) {
                    const char *descriptor = (const char *)(DWORD_PTR)catchable[1];
                    if (readable(descriptor, 9)) type = descriptor + 8;
                }
            }
        }
    }
    /* The throw itself is always inside RaiseException; who threw is further up the stack. */
    char trail[400];
    size_t used = 0;
    int found = 0;
    trail[0] = 0;
    const DWORD *sp = (const DWORD *)(DWORD_PTR)c->Esp;
    const void *base = ((NT_TIB *)NtCurrentTeb())->StackBase;
    for (const DWORD *q = sp; found < 5 && (const void *)(q + 1) <= base && q < sp + 512; q++) {
        HMODULE m = module_of((const void *)(DWORD_PTR)*q);
        if (!m || (DWORD_PTR)*q - (DWORD_PTR)m < 0x1000 || is_plumbing(m)) continue;
        char at[160];
        describe_address((const void *)(DWORD_PTR)*q, at, sizeof at);
        int k = _snprintf(trail + used, sizeof trail - used, " %s", at);
        if (k < 0 || (size_t)k >= sizeof trail - used) break;
        used += (size_t)k;
        found++;
    }
    log_line("exception, first chance: C++ throw of %.80s, thread %lu, from:%s%s", type,
             GetCurrentThreadId(), found ? trail : " ?",
             n == CPP_LINES ? " -- the last such line this session" : "");
}

/* The overflowing thread cannot write anything down; this one can, while the process lasts. */
static DWORD WINAPI overflow_watch(LPVOID unused)
{
    (void)unused;
    WaitForSingleObject(overflow_event, INFINITE);
    char where[160];
    describe_address(fatal_address, where, sizeof where);
    log_line("exception, first chance: STACK OVERFLOW at %s", where);
    return 0;
}

static LONG WINAPI on_first_chance(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode == 0xE06D7363) { note_cpp_exception(r, ep->ContextRecord); return EXCEPTION_CONTINUE_SEARCH; }
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
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        if (overflow_event) SetEvent(overflow_event);
        return EXCEPTION_CONTINUE_SEARCH;
    }

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

    char alive[16] = "";
    _snprintf(path, MAX_PATH, "%s" ALIVE_FILE, log_dir());
    path[MAX_PATH - 1] = 0;
    FILE *af = fopen(path, "r");
    if (af) {
        if (fgets(alive, sizeof alive, af)) alive[strcspn(alive, "\r\n")] = 0;
        fclose(af);
    }

    char text[1500];
    if (halt[0])
        _snprintf(text, sizeof text, "%s -> %.8s  %-8s  %s", since, last, "D2 HALT", halt);
    else
        _snprintf(text, sizeof text,
                  "%s -> %s  %-8s  neither an exit nor a crash reached the addon: killed from "
                  "outside, or died too hard to run any code. Alive at %s; last line logged: %s",
                  since, alive[0] ? alive : "?", "NO TRACE", alive[0] ? alive : "?", last + 14);
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
    real_base_terminate_process = (terminate_process_fn)hotpatch(
        resolve_export("kernelbase.dll", "TerminateProcess"), (void *)on_base_terminate_process);
    real_nt_terminate = (nt_terminate_fn)hook_mov_eax(
        resolve_export("ntdll.dll", "NtTerminateProcess"), (void *)on_nt_terminate);
    real_fail_fast = (fail_fast_fn)hotpatch(
        resolve_export("kernelbase.dll", "RaiseFailFastException"), (void *)on_fail_fast);
    /* kernel32's stub first, resolved without following its jump: modules import it from there. */
    real_feature_k32 = (feature_fn)hotpatch(
        (BYTE *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsProcessorFeaturePresent"),
        (void *)on_feature_k32);
    real_feature_base = (feature_fn)hotpatch(
        (BYTE *)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "IsProcessorFeaturePresent"),
        (void *)on_feature_base);
}

/* Re-takes the unhandled-exception slot; whatever held it is called after us. Once a second
   from bootstrap, because Fog claims the slot during startup, after we have loaded. */
void crash_tick(void)
{
    /* The heartbeat: when a session vanishes, the next launch can say roughly when. */
    static DWORD last_alive;
    DWORD now_tick = GetTickCount();
    if (now_tick - last_alive >= 2000) {
        last_alive = now_tick;
        char path[MAX_PATH], stamp[16];
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf(path, MAX_PATH, "%s" ALIVE_FILE, log_dir());
        path[MAX_PATH - 1] = 0;
        int n = _snprintf(stamp, sizeof stamp, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, stamp, (DWORD)n, &written, NULL);
            CloseHandle(h);
        }
    }

    /* The game runs with SEM_NOGPFAULTERRORBOX, and with it Windows writes nothing at all for a
       fail-fast: no Application Error, no WER report, no dump (2026-11-21: a game closed with
       0xC0000409 and left nothing; a test program did the same until the flag was cleared, then
       got an event naming the faulting module and a dump in %LOCALAPPDATA%\CrashDumps). The flag
       is only about Windows' own reporting — Fog still takes an ordinary crash before Windows
       sees it — so it is cleared, every second, in case the game sets it again. */
    UINT mode = GetErrorMode();
    if (mode & SEM_NOGPFAULTERRORBOX) {
        SetErrorMode(mode & ~SEM_NOGPFAULTERRORBOX);
        static int said;
        if (!said) {
            said = 1;
            log_line("crashwatch: error mode was 0x%X; Windows may now report a fail-fast "
                     "(Application Error, and a dump in CrashDumps)", mode);
        }
    }

    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(on_unhandled);
    if (previous == on_unhandled) return;
    chained = previous;
    char who[160];
    describe_address((void *)previous, who, sizeof who);
    log_line("crashwatch: watching for crashes, handing each on to %s",
             previous ? who : "Windows (nothing else was watching)");
}

/* The watcher outside the process: tools/pd2watch.c, beside the DLL. Some endings run no code in
   here at all (three on 2026-11-21: nothing hooked, nothing logged by Windows), and the exit code
   is the one thing that still tells them apart. Game.exe denies every other process access to
   itself, so the watcher could not open it; it is handed this process's own handle instead —
   duplicated from the pseudo-handle, which needs no permission — as the only handle it inherits. */
static void start_watcher(void)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 64];
    _snprintf(exe, MAX_PATH, "%spd2watch.exe", log_dir());
    exe[MAX_PATH - 1] = '\0';
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        log_line("crashwatch: no pd2watch.exe beside the DLL, so no exit code from outside");
        return;
    }
    HANDLE self = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        log_line("crashwatch: could not duplicate the process handle (error %lu)", GetLastError());
        return;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = HeapAlloc(GetProcessHeap(), 0, size);
    BOOL listed = list && InitializeProcThreadAttributeList(list, 1, 0, &size);
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.StartupInfo.cb = sizeof si;
    BOOL started = FALSE;
    DWORD error = 0;
    if (listed && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self,
                                            sizeof self, NULL, NULL)) {
        si.lpAttributeList = list;
        _snprintf(cmd, sizeof cmd, "\"%s\" %lu %lu", exe, (unsigned long)(DWORD_PTR)self,
                  (unsigned long)GetCurrentProcessId());
        cmd[sizeof cmd - 1] = '\0';
        started = CreateProcessA(exe, cmd, NULL, NULL, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
                                 &si.StartupInfo, &pi);
    }
    error = GetLastError();
    if (listed) DeleteProcThreadAttributeList(list);
    if (list) HeapFree(GetProcessHeap(), 0, list);
    CloseHandle(self);
    if (!started) {
        log_line("crashwatch: could not start pd2watch.exe (error %lu)", error);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_line("crashwatch: pd2watch.exe (pid %lu) will read the exit code from outside",
             (unsigned long)pi.dwProcessId);
}

/* From bootstrap, before anything else. */
void crash_start(void)
{
    log_line("crashwatch: ExitProcess %s, TerminateProcess %s, kernelbase TerminateProcess %s, "
             "NtTerminateProcess %s, RaiseFailFastException %s",
             real_exit_process ? "hooked" : "NOT hooked",
             real_terminate_process ? "hooked" : "NOT hooked",
             real_base_terminate_process ? "hooked" : "NOT hooked",
             real_nt_terminate ? "hooked" : "NOT hooked",
             real_fail_fast ? "hooked" : "NOT hooked");
    log_line("crashwatch: IsProcessorFeaturePresent (the runtime's last call before a fail-fast) "
             "%s in kernel32, %s in kernelbase",
             real_feature_k32 ? "hooked" : "NOT hooked", real_feature_base ? "hooked" : "NOT hooked");
    overflow_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (overflow_event) {
        HANDLE watch = CreateThread(NULL, 0, overflow_watch, NULL, 0, NULL);
        if (watch) CloseHandle(watch);
    }
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (dbghelp) write_minidump = (minidump_fn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    review_previous();
    start_watcher();
    crash_tick();
}

/* From DllMain at process detach — only says anything if the ExitProcess hook never ran. */
void crash_process_detach(void)
{
    DWORD here = 0;
    note_ending("process detach (ExitProcess not hooked)", 0, NULL, &here);
}
