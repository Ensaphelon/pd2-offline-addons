#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

/* A read-only look at what the running game holds about an item we already know everything about.
 *
 * It deliberately walks NO game structures. Every attempt to do that starts by guessing where a
 * field lives, and a wrong guess in another process's heap is a crash in somebody's real game.
 * Instead this hunts for values we know are unique to one item — its guid, straight out of the
 * save — and prints the memory around each hit. The layout then falls out of the dump by
 * comparison, the same way the menu entry's label pointer was found: by looking, not by assuming.
 *
 * What we are trying to settle: whether an item in memory carries the unique/set identity our
 * catalog uses. If the dump around a guid contains 274 for Dwarf Star, the two id spaces are the
 * same and the plugin can say "this one really completes the grail". If it contains some other
 * number, we need a translation table. If it contains nothing recognisable, the identity is not
 * there at all. All three answers are useful and none of them require touching the game. */

#define MAX_TARGETS 16
#define WINDOW_BEFORE 0x40      /* bytes to print before a hit  */
#define WINDOW_AFTER  0x120     /* and after — enough to cover a UnitAny and its ItemData  */
#define MAX_HITS_PER_TARGET 8   /* one item can legitimately appear a few times (cached copies) */

typedef struct {
    DWORD value;
    char label[64];
} probe_target;

static probe_target targets[MAX_TARGETS];
static int target_count;

/* The values to hunt live in a file beside the DLL rather than in the build, so a new experiment
   is a text edit rather than a recompile — and so this repository never carries anybody's real
   item ids. One per line: "<decimal or 0xhex> <label>". */
static void load_targets(void *module)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA((HMODULE)module, path, MAX_PATH)) return;
    char *slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "probe-targets.txt");

    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("probe: no probe-targets.txt beside the DLL (%s) — nothing to look for", path);
        return;
    }
    char line[256];
    while (target_count < MAX_TARGETS && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned long value = 0;
        char label[64] = {0};
        if (sscanf(line, "%li %63[^\r\n]", (long *)&value, label) < 1) continue;
        targets[target_count].value = (DWORD)value;
        strncpy(targets[target_count].label, label[0] ? label : "(unnamed)", 63);
        target_count++;
    }
    fclose(f);
    log_line("probe: %d target value(s) loaded from probe-targets.txt", target_count);
}

static BOOL readable(DWORD protect)
{
    if (protect & PAGE_GUARD) return FALSE;
    if (protect & PAGE_NOACCESS) return FALSE;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Where an address lives, so a hit inside a loaded module can be told apart from one on the heap.
   A heap hit is the interesting kind: that is a live object, not a constant baked into code. */
static void describe(const BYTE *address, char *out, size_t out_size)
{
    HMODULE module = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module) {
        char name[MAX_PATH] = {0};
        GetModuleFileNameA(module, name, MAX_PATH);
        const char *base = strrchr(name, '\\');
        snprintf(out, out_size, "%.90s+0x%X", base ? base + 1 : name,
                 (unsigned)(address - (const BYTE *)module));
        return;
    }
    snprintf(out, out_size, "heap");
}

static void dump_around(const BYTE *hit, const probe_target *target)
{
    char where[128];
    describe(hit, where, sizeof(where));
    log_line("  hit %p  (%s)", (void *)hit, where);

    const BYTE *start = hit - WINDOW_BEFORE;
    for (int row = 0; row < (WINDOW_BEFORE + WINDOW_AFTER) / 16; row++) {
        const BYTE *line = start + row * 16;
        if (IsBadReadPtr(line, 16)) continue;
        DWORD w[4];
        memcpy(w, line, sizeof(w));
        char ascii[17] = {0};
        for (int i = 0; i < 16; i++) {
            BYTE ch = line[i];
            ascii[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        /* The offset is relative to the hit, so a field two dwords before the guid reads as
           -0x08 rather than as an absolute address nobody can compare between runs. */
        log_line("    %+05d  %08X %08X %08X %08X  |%s|",
                 (int)(line - hit), w[0], w[1], w[2], w[3], ascii);
    }
    (void)target;
}

static void scan_for(const probe_target *target)
{
    log_line("probe: looking for %lu (0x%08X) — %s",
             (unsigned long)target->value, target->value, target->label);

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    BYTE *address = (BYTE *)info.lpMinimumApplicationAddress;
    BYTE *limit = (BYTE *)info.lpMaximumApplicationAddress;
    int hits = 0;

    while (address < limit && hits < MAX_HITS_PER_TARGET) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(address, &mbi, sizeof(mbi))) break;
        BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && readable(mbi.Protect)) {
            const BYTE *base = (const BYTE *)mbi.BaseAddress;
            SIZE_T size = mbi.RegionSize;
            /* Aligned scan: every structure field we care about is dword-aligned, and it is four
               times less work than a byte-wise sweep of a 2GB address space. */
            for (SIZE_T offset = 0; offset + 4 <= size; offset += 4) {
                const BYTE *at = base + offset;
                if (*(const DWORD *)at != target->value) continue;
                dump_around(at, target);
                if (++hits >= MAX_HITS_PER_TARGET) {
                    log_line("  (stopping at %d hits)", hits);
                    break;
                }
            }
        }
        if (next <= address) break;
        address = next;
    }
    if (hits == 0) log_line("  not found anywhere in committed memory");
    log_line("");
}

void probe_run(void)
{
    if (target_count == 0) {
        log_line("probe: nothing to do");
        return;
    }
    log_line("=== probe pass starting ===");
    DWORD started = GetTickCount();
    for (int i = 0; i < target_count; i++) scan_for(&targets[i]);
    log_line("=== probe pass done in %lu ms ===", (unsigned long)(GetTickCount() - started));
}

void probe_init(void *module)
{
    load_targets(module);
}
