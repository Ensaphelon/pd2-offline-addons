#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
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
/* Our own module's address range. The first pass found the probe's own target labels and dumped
   them as if they were game data — a value we are hunting for is, by construction, also sitting
   in the config we loaded it from. */
static const BYTE *self_start, *self_end;

#define CHUNK 0x10000
static BYTE chunk[CHUNK];

static BOOL safe_read(const void *at, void *into, SIZE_T count);


/* Every UnitAny this pass managed to identify. Enumerating items one per frame means walking the
   game's own list of them, and the way to find that list is to ask who points AT a unit we have
   already located. A referrer inside D2Client's own data is the prize: that is a fixed address,
   the same in every session, and therefore something the plugin can just read. */
#define MAX_UNITS 32
static const BYTE *found_units[MAX_UNITS];
static int found_unit_count;

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

/* Prints `count` bytes from `at`, offsets relative to `origin`, or says why it could not. */
static void dump_range(const BYTE *at, const BYTE *origin, int count)
{
    for (int row = 0; row < count / 16; row++) {
        const BYTE *line = at + row * 16;
        BYTE bytes[16];
        if (!safe_read(line, bytes, sizeof(bytes))) continue;
        DWORD w[4];
        memcpy(w, bytes, sizeof(w));
        char ascii[17] = {0};
        for (int i = 0; i < 16; i++) {
            BYTE ch = bytes[i];
            ascii[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        log_line("    %+05d  %08X %08X %08X %08X  |%s|",
                 (int)(line - origin), w[0], w[1], w[2], w[3], ascii);
    }
}

static void remember_unit(const BYTE *unit)
{
    for (int i = 0; i < found_unit_count; i++) if (found_units[i] == unit) return;
    if (found_unit_count < MAX_UNITS) found_units[found_unit_count++] = unit;
}

/* An item's guid sits at UnitAny+0x20 — established from this probe's own first pass, where every
   hit had dwType == 4 thirty-two bytes back and a pointer eight dwords in. That pointer is
   pItemData, and it is where the identity we are actually after lives. Following it is the whole
   reason for a second pass: the first one could see the item but not what the item IS. */
#define GUID_TO_UNIT       0x20
#define UNIT_TO_ITEMDATA   0x14

static void follow_item_data(const BYTE *hit)
{
    const BYTE *unit = hit - GUID_TO_UNIT;
    const BYTE *slot = unit + UNIT_TO_ITEMDATA;
    DWORD pointer = 0;
    if (!safe_read(slot, &pointer, 4)) return;
    const BYTE *item_data = (const BYTE *)(UINT_PTR)pointer;
    BYTE head[0x40];
    if (!safe_read(item_data, head, sizeof(head))) {
        log_line("    -> pItemData %p is not readable", (void *)item_data);
        return;
    }
    log_line("    -> pItemData %p (offsets below are from ITS start)", (void *)item_data);
    dump_range(item_data, item_data, 0xB0);
    /* dwType 4 is an item and quality is 1..9; anything else reached this far by coincidence. */
    DWORD type = 0, quality = 0;
    if (!safe_read(unit, &type, 4) || !safe_read(item_data, &quality, 4)) return;
    if (type == 4 && quality >= 1 && quality <= 9) remember_unit(unit);
    else log_line("    (not an item unit: dwType=%lu quality=%lu — not traced)",
                  (unsigned long)type, (unsigned long)quality);
}

static void dump_around(const BYTE *hit, const probe_target *target)
{
    char where[128];
    describe(hit, where, sizeof(where));
    log_line("  hit %p  (%s)", (void *)hit, where);

    /* Offsets are relative to the hit, so a field two dwords before the guid reads as -0x08
       rather than as an absolute address nobody can compare between runs. */
    dump_range(hit - WINDOW_BEFORE, hit, WINDOW_BEFORE + WINDOW_AFTER);
    follow_item_data(hit);
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
            /* Copied out a chunk at a time rather than read in place: the game is running while
               we look, and a region can be freed between the query and the read. */
            for (SIZE_T done = 0; done < mbi.RegionSize; done += CHUNK) {
                SIZE_T count = mbi.RegionSize - done;
                if (count > CHUNK) count = CHUNK;
                if (!safe_read(base + done, chunk, count)) continue;
                /* Aligned scan: every field we care about is dword-aligned, and it is four times
                   less work than a byte-wise sweep of a 2GB address space. */
                for (SIZE_T offset = 0; offset + 4 <= count; offset += 4) {
                    if (*(const DWORD *)(chunk + offset) != target->value) continue;
                    const BYTE *at = base + done + offset;
                    if (at >= self_start && at < self_end) continue;   /* our own config strings */
                    dump_around(at, target);
                    if (++hits >= MAX_HITS_PER_TARGET) {
                        log_line("  (stopping at %d hits)", hits);
                        break;
                    }
                }
                if (hits >= MAX_HITS_PER_TARGET) break;
            }
        }
        if (next <= address) break;
        address = next;
    }
    if (hits == 0) log_line("  not found anywhere in committed memory");
    log_line("");
}

/* One sweep looking for a pointer to any unit we found, rather than one sweep per unit: a sweep
   costs about 70ms, and this way the cost does not grow with the number of items. */
static void scan_for_referrers(void)
{
    if (found_unit_count == 0) {
        log_line("referrers: no units were identified, nothing to trace");
        return;
    }
    log_line("referrers: looking for anything pointing at the %d unit(s) found", found_unit_count);

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    BYTE *address = (BYTE *)info.lpMinimumApplicationAddress;
    BYTE *limit = (BYTE *)info.lpMaximumApplicationAddress;
    int shown = 0;

    while (address < limit && shown < 64) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(address, &mbi, sizeof(mbi))) break;
        BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && readable(mbi.Protect)) {
            const BYTE *base = (const BYTE *)mbi.BaseAddress;
            for (SIZE_T done = 0; done < mbi.RegionSize && shown < 64; done += CHUNK) {
              SIZE_T count = mbi.RegionSize - done;
              if (count > CHUNK) count = CHUNK;
              if (!safe_read(base + done, chunk, count)) continue;
              for (SIZE_T offset = 0; offset + 4 <= count; offset += 4) {
                const BYTE *at = base + done + offset;
                if (at >= self_start && at < self_end) continue;
                DWORD value = *(const DWORD *)(chunk + offset);
                for (int i = 0; i < found_unit_count; i++) {
                    if (value != (DWORD)(UINT_PTR)found_units[i]) continue;
                    char where[128];
                    describe(at, where, sizeof(where));
                    /* A referrer in a loaded module is a STATIC slot — the thing worth having. */
                    log_line("  %s  %p -> unit #%d (%p)",
                             where[0] == 'h' ? "heap  " : "STATIC", (void *)at, i,
                             (void *)found_units[i]);
                    log_line("         at %s", where);
                    shown++;
                    break;
                }
                if (shown >= 64) break;
              }
            }
        }
        if (next <= address) break;
        address = next;
    }
    if (shown == 0) log_line("  nothing points at them, which would be surprising");
    log_line("");
}

/* Two attempts at reading another thread's memory safely have now faulted in the live game.
   IsBadReadPtr is documented to be unreliable, and asking VirtualQuery first is no better here:
   the game is running while we look, so a region that answers "committed" can be freed before
   the very next instruction reads it, and a guard page faults whatever the query said.

   ReadProcessMemory on our own process is the way out. It does the read in the kernel and
   returns FALSE on anything it cannot touch, so a bad pointer costs a failed call instead of
   taking the game down with it. */
static BOOL safe_read(const void *at, void *into, SIZE_T count)
{
    if ((DWORD)(UINT_PTR)at < 0x10000) return FALSE;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, count, &got) && got == count;
}

/* Does this address hold something shaped like a UnitAny? dwType is 0..5 in this engine (player,
   monster, object, missile, item, tile), and every unit has a readable data pointer at +0x14.
   Two cheap tests, enough to tell a real unit from a number that happens to look like a pointer. */
static BOOL looks_like_unit(const BYTE *candidate, DWORD *out_type, DWORD *out_txtfile)
{
    DWORD head[6];
    if (!safe_read(candidate, head, sizeof(head))) return FALSE;
    if (head[0] > 5) return FALSE;
    /* A base-item/monster/object row number, not an address or a flag word. */
    if (head[1] > 8192) return FALSE;
    DWORD first = 0;
    if (!safe_read((const void *)(UINT_PTR)head[UNIT_TO_ITEMDATA / 4], &first, 4)) return FALSE;
    /* Whatever that points at starts with something small — a quality, a class, a flag set. It
       is emphatically not another pointer, which is what every false positive in the code
       section turned out to hold. */
    if (first >= 0x1000) return FALSE;
    *out_type = head[0];
    *out_txtfile = head[1];
    return TRUE;
}

/* The game reaches its units through a static hash table — an array of list heads living in
   D2Client's own data. Nothing points AT a unit from there except the head of its bucket, which
   is why tracing referrers found only heap links. So this looks from the other end: every slot in
   D2Client's image that holds a pointer to something unit-shaped. The table shows up as a run of
   them at consecutive addresses, and its address is fixed for the build — which is the whole
   point, because a renderer cannot scan memory every frame. */
static void scan_client_image_for_unit_slots(void)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) { log_line("unit table: D2Client.dll is not loaded"); return; }

    const BYTE *base = (const BYTE *)client;
    log_line("unit table: sweeping D2Client.dll DATA for slots pointing at units (image %p)",
             (void *)base);

    /* Data sections only. The first version swept the whole image, and the code section is full
       of instruction bytes that read as plausible pointers — that is what made it fault. A list
       head lives in writable data, never in .text. */
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    int found = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections && found < 200; i++, section++) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const BYTE *from = base + section->VirtualAddress;
        SIZE_T size = section->Misc.VirtualSize;
        log_line("  section %.8s at +0x%X, %lu bytes",
                 section->Name, (unsigned)section->VirtualAddress, (unsigned long)size);
        {
            for (SIZE_T offset = 0; offset + 4 <= size; offset += 4) {
                const BYTE *slot = from + offset;
                DWORD value = 0;
                if (!safe_read(slot, &value, 4)) continue;
                DWORD type = 0, txtfile = 0;
                if (!looks_like_unit((const BYTE *)(UINT_PTR)value, &type, &txtfile)) continue;
                log_line("    +0x%06X -> %08X  dwType=%lu txtfile=%lu",
                         (unsigned)(slot - base), value,
                         (unsigned long)type, (unsigned long)txtfile);
                if (++found >= 200) { log_line("  (stopping at %d)", found); break; }
            }
        }
    }
    if (found == 0) log_line("  nothing — the table is not reached by a plain pointer");
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
    found_unit_count = 0;
    for (int i = 0; i < target_count; i++) scan_for(&targets[i]);
    scan_for_referrers();
    scan_client_image_for_unit_slots();
    log_line("=== probe pass done in %lu ms ===", (unsigned long)(GetTickCount() - started));
}

void probe_init(void *module)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(module, &mbi, sizeof(mbi))) {
        self_start = (const BYTE *)mbi.AllocationBase;
        self_end = self_start + 0x100000;   /* generous: the whole image, never game memory */
    }
    load_targets(module);
}
