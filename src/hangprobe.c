/* Where the game stands while it leaves a game. A diagnostic, off in every normal build.
 *
 * Leaving a game in its first seconds stalls for three or four before the menu comes back; leaving
 * after a minute does not. Nothing outside the process can look: Game.exe refuses every process
 * access right to other processes, even the most limited query, so an external sampler sees
 * thread contexts and nothing to read them against. This runs inside instead.
 *
 * When the player unit goes away -- the game is being left -- a thread of its own samples every
 * thread in the process for ten seconds: where each stands (EIP, and the return addresses on its
 * stack, each by module and offset) and how much CPU it burned. Then the summary goes to the log.
 *
 * Between suspending a thread and resuming it nothing is called but GetThreadContext and a copy of
 * its stack: a suspended thread may hold the heap lock or the loader lock, and anything that
 * wanted either would wait for it forever. Module lookups, formatting and logging all happen
 * after it runs again.
 *
 * Build with -DADDONS_HANGPROBE=1.
 */
#include "addons.h"

#if ADDONS_HANGPROBE

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d2.h"
#include "log.h"

#define OFF_GETPLAYERUNIT 0xA4D60   /* D2Client: the client's player unit, NULL outside a game */
typedef void *(__stdcall *get_player_unit_fn)(void);

#define BURST_MS     10000
#define PERIOD_MS    10
#define MAX_THREADS  128
#define MAX_SIGS     48
#define FRAMES       4             /* game frames kept per sample, after EIP */
#define STACK_WORDS  1024          /* 4 KB of stack scanned per sample */
#define MAX_MODULES  256

typedef struct { DWORD base, end; char name[32]; int system; } module_t;
typedef struct { DWORD eip, frames[FRAMES]; int count; } sig_t;
typedef struct {
    DWORD tid, start;
    HANDLE h;
    ULONGLONG cpu0, cpu1;
    int samples, nsig;
    sig_t sigs[MAX_SIGS];
} thread_t;

static module_t mods[MAX_MODULES];
static int nmods;
static thread_t threads[MAX_THREADS];
static int nthreads;
static DWORD stack_copy[STACK_WORDS];

/* The main thread's top game frame over time, to show the phases of the exit. */
#define TIMELINE 1200
static struct { DWORD ms, frame; } timeline[TIMELINE];
static int ntimeline;

static const char *SYSTEM[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "win32u.dll",
    "gdi32.dll", "gdi32full.dll", "ucrtbase.dll", "msvcrt.dll", "combase.dll", "rpcrt4.dll", "sechost.dll",
    "advapi32.dll", "msvcp_win.dll", "ws2_32.dll", "dsound.dll", "winmm.dll", "dinput8.dll", NULL };

static void load_modules(void)
{
    HMODULE list[MAX_MODULES];
    DWORD need = 0;
    nmods = 0;
    if (!EnumProcessModules(GetCurrentProcess(), list, sizeof list, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && nmods < MAX_MODULES; i++) {
        MODULEINFO info;
        if (!GetModuleInformation(GetCurrentProcess(), list[i], &info, sizeof info)) continue;
        module_t *m = &mods[nmods++];
        m->base = (DWORD)info.lpBaseOfDll;
        m->end = m->base + info.SizeOfImage;
        GetModuleBaseNameA(GetCurrentProcess(), list[i], m->name, sizeof m->name);
        m->system = 0;
        for (const char **s = SYSTEM; *s; s++) if (!lstrcmpiA(m->name, *s)) m->system = 1;
    }
}

static const module_t *module_of(DWORD a)
{
    for (int i = 0; i < nmods; i++) if (a >= mods[i].base && a < mods[i].end) return &mods[i];
    return NULL;
}

static void where(DWORD a, char *out, size_t cap)
{
    const module_t *m = module_of(a);
    if (m) _snprintf(out, cap, "%s+%lX", m->name, (unsigned long)(a - m->base));
    else _snprintf(out, cap, "%08lX", (unsigned long)a);
    out[cap - 1] = 0;
}

static int safe_copy(const void *at, void *into, SIZE_T bytes)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, bytes, &got) && got == bytes;
}

/* A return address is one the CPU was sent to by a call: the bytes before it are a call. */
static int after_call(DWORD a)
{
    BYTE b[7];
    if (!safe_copy((const void *)(a - 7), b, 7)) return 0;
    if (b[2] == 0xE8) return 1;                                         /* call rel32 */
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return 1;                /* call [disp32] / [reg+disp32] */
    if (b[0] == 0xFF && (b[1] & 0x38) == 0x10) return 1;                /* call [sib+disp32] */
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10) return 1;                /* call [reg+disp8] */
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return 1;                /* call reg / [reg] */
    return 0;
}

static void take_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    typedef LONG (NTAPI *query_fn)(HANDLE, int, PVOID, ULONG, PULONG);
    query_fn query = (query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    for (BOOL ok = Thread32First(snap, &te); ok && nthreads < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        int known = 0;
        for (int i = 0; i < nthreads; i++) if (threads[i].tid == te.th32ThreadID) known = 1;
        if (known) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        thread_t *t = &threads[nthreads++];
        memset(t, 0, sizeof *t);
        t->tid = te.th32ThreadID;
        t->h = h;
        if (query) query(h, 9, &t->start, sizeof t->start, NULL);
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u))
            t->cpu0 = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    }
    CloseHandle(snap);
}

static void sample(thread_t *t, DWORD elapsed, int is_main)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int words = 0;

    if (SuspendThread(t->h) == (DWORD)-1) return;
    BOOL got = GetThreadContext(t->h, &ctx);
    if (got) {
        /* As much of the stack as is there, up to the scan size: the copy fails as a whole at the
           end of the committed stack, so halve until it fits. */
        for (int n = STACK_WORDS; n >= 64 && !words; n /= 2)
            if (safe_copy((const void *)ctx.Esp, stack_copy, n * sizeof(DWORD))) words = n;
    }
    ResumeThread(t->h);
    if (!got) return;

    sig_t s;
    memset(&s, 0, sizeof s);
    s.eip = ctx.Eip;
    int nf = 0;
    for (int i = 0; i < words && nf < FRAMES; i++) {
        const module_t *m = module_of(stack_copy[i]);
        if (!m || m->system) continue;
        if (!after_call(stack_copy[i])) continue;
        s.frames[nf++] = stack_copy[i];
    }
    t->samples++;
    for (int i = 0; i < t->nsig; i++)
        if (!memcmp(&t->sigs[i], &s, offsetof(sig_t, count))) { t->sigs[i].count++; goto timeline_add; }
    if (t->nsig < MAX_SIGS) { s.count = 1; t->sigs[t->nsig++] = s; }

timeline_add:
    if (is_main && ntimeline < TIMELINE) {
        const module_t *m = module_of(s.eip);
        timeline[ntimeline].ms = elapsed;
        timeline[ntimeline].frame = (m && !m->system) ? s.eip : s.frames[0];
        ntimeline++;
    }
}

static int by_count(const void *a, const void *b) { return ((const sig_t *)b)->count - ((const sig_t *)a)->count; }

static DWORD main_tid;

static DWORD WINAPI burst(LPVOID in_game_ms)
{
    load_modules();
    nthreads = 0;
    ntimeline = 0;
    take_threads();
    DWORD t0 = GetTickCount();
    log_line("hangprobe: the game is being left after %lu ms in it; sampling %d threads for %d ms",
             (unsigned long)(DWORD_PTR)in_game_ms, nthreads, BURST_MS);

    for (DWORD now = 0; now < BURST_MS; now = GetTickCount() - t0) {
        for (int i = 0; i < nthreads; i++) sample(&threads[i], now, threads[i].tid == main_tid);
        Sleep(PERIOD_MS);
    }

    char a[64], b[64], f[4][64];
    for (int i = 0; i < nthreads; i++) {
        thread_t *t = &threads[i];
        FILETIME c, e, k, u;
        if (GetThreadTimes(t->h, &c, &e, &k, &u))
            t->cpu1 = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        else
            t->cpu1 = t->cpu0;
        qsort(t->sigs, t->nsig, sizeof(sig_t), by_count);
        where(t->start, a, sizeof a);
        unsigned long cpu = (unsigned long)((t->cpu1 - t->cpu0) / 10000);
        /* An idle thread that sat in one place the whole time is one line; anything that moved or
           burned time gets its top stacks. */
        int busy = cpu > 20 || t->nsig > 1 || t->tid == main_tid;
        log_line("hangprobe: thread %lu%s start %s, cpu %lu ms, %d samples, %d distinct stacks",
                 (unsigned long)t->tid, t->tid == main_tid ? " (main)" : "", a, cpu, t->samples, t->nsig);
        for (int j = 0; j < t->nsig && j < (busy ? 4 : 1); j++) {
            sig_t *s = &t->sigs[j];
            where(s->eip, b, sizeof b);
            for (int x = 0; x < 4; x++) { if (s->frames[x]) where(s->frames[x], f[x], sizeof f[x]); else f[x][0] = 0; }
            log_line("hangprobe:   %3d%% at %s  <- %s <- %s <- %s <- %s", s->count * 100 / (t->samples ? t->samples : 1),
                     b, f[0], f[1], f[2], f[3]);
        }
        CloseHandle(t->h);
    }

    /* The main thread's phases: runs of the same top game frame, with when they started. */
    for (int i = 0; i < ntimeline; ) {
        int j = i;
        while (j + 1 < ntimeline && timeline[j + 1].frame == timeline[i].frame) j++;
        DWORD end = (j + 1 < ntimeline) ? timeline[j + 1].ms : timeline[j].ms;
        if (end - timeline[i].ms >= 100 || i == 0) {
            where(timeline[i].frame, a, sizeof a);
            log_line("hangprobe: main %5lu-%5lu ms  %s", (unsigned long)timeline[i].ms, (unsigned long)end, a);
        }
        i = j + 1;
    }
    log_line("hangprobe: done");
    return 0;
}

/* From the main loop. Starts a burst on the tick the player unit disappears. */
void hangprobe_tick(void)
{
    static get_player_unit_fn get_player_unit;
    static int was_in_game;
    static DWORD entered;
    if (!get_player_unit) {
        HMODULE client = GetModuleHandleA("D2Client.dll");
        if (!client) return;
        get_player_unit = (get_player_unit_fn)((BYTE *)client + OFF_GETPLAYERUNIT);
    }
    int in_game = get_player_unit() != NULL;
    if (in_game && !was_in_game) {
        entered = GetTickCount();
        /* The game's main thread is the one that started at Game.exe's entry point; it is found
           once, by elimination, the first time a game is entered. */
        if (!main_tid) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
            ULONGLONG oldest = ~0ULL;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                FILETIME c, e, k, u;
                if (h && GetThreadTimes(h, &c, &e, &k, &u)) {
                    ULONGLONG created = (ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime;
                    if (created < oldest) { oldest = created; main_tid = te.th32ThreadID; }
                }
                if (h) CloseHandle(h);
            }
            CloseHandle(snap);
            log_line("hangprobe: armed; the main thread is %lu", (unsigned long)main_tid);
        }
    }
    if (!in_game && was_in_game) {
        HANDLE h = CreateThread(NULL, 0, burst, (LPVOID)(DWORD_PTR)(GetTickCount() - entered), 0, NULL);
        if (h) CloseHandle(h);
    }
    was_in_game = in_game;
}

#endif
