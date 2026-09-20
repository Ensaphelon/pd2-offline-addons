#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
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
static char player_name[32];

/* PD2 exports exactly one function, and it is this: the call its own loot filter uses so that a
   drop alert obeys the LOOT FILTER slider in Sound Options rather than playing at full blast
   outside every volume control the game offers. Anything we play has to go through it for the
   same reason.
   Five arguments, and reading the exported code settles what each one is — guessing at them in
   a running game got as far as crashing it:

     arg0  a unit pointer, dereferenced when non-zero, so the sound is placed in the world.
           Zero plays it without a position. Passing 1 here is what took the game down.
     arg1  the sound id. Checked >= 1 and used to index Sounds.txt (stride 0x92).
     arg2  clamped to 0..255 — the VOLUME. Zero is silence; this is the one that matters.
     arg3  clamped to 0..255 — the priority. Changing it alone is inaudible.

   The code shows arg2 going into a dword field and arg3 into a byte one, and reading that as
   "the byte must be the volume" was a guess that the ear then disproved: F1 and F2 differed only
   in arg3 and sounded identical, while F3 and F4 held arg2 at zero and made no sound at all. The
   order in the exported name — Volume, then Priority — had said so all along.
     arg4  stored verbatim; purpose unknown, zero is what the game's own callers appear to use.

   Which slider governs the result is not an argument at all: it is the Sound Group column of
   the sound's own row in Sounds.txt. Group 12 is the LOOT FILTER channel. */
typedef int (__stdcall *play_sound_fn)(int, int, int, int, int);
static play_sound_fn play_sound;

#define MAX_SOUNDS 16
typedef struct { int arg[5]; char label[64]; } sound_probe;
static sound_probe sounds[MAX_SOUNDS];
static int sound_count, sound_next;

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
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        {
            sound_probe s;
            memset(&s, 0, sizeof(s));
            int got = sscanf(line, "sound %i %i %i %i %i %63[^\r\n]",
                             &s.arg[0], &s.arg[1], &s.arg[2], &s.arg[3], &s.arg[4], s.label);
            if (got >= 5) {
                if (sound_count < MAX_SOUNDS) sounds[sound_count++] = s;
                continue;
            }
        }
        if (sscanf(line, "player %31[^\r\n]", player_name) == 1) {
            log_line("probe: the character to anchor on is \"%s\"", player_name);
            continue;
        }
        if (target_count >= MAX_TARGETS) continue;
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

/* Found by sweeping D2Client's data for slots pointing at something unit-shaped: 127 consecutive
   ones, every last of them an item. That is the item row of the engine's unit hash table — 128
   buckets, each the head of a chain — and it is a fixed address in a fixed module, which is the
   whole reason for having gone looking. A renderer cannot scan two gigabytes per frame; it can
   read this. */
#define OFF_D2CLIENT_UNIT_TABLE_ITEMS 0x10AE08
#define UNIT_BUCKETS 128

/* Where the next unit in a bucket lives. Not assumed — the two candidates are tried and whichever
   produces another item unit is reported, so the log says which one this build actually uses. */
static const int LINK_CANDIDATES[] = { 0xEC };

typedef struct {
    DWORD type, txtfile, unit_id, mode, item_data, seed;
} unit_head;

static BOOL read_unit(DWORD address, unit_head *out)
{
    /* Through +0x20, because the seed is what the save calls the item's guid and it is the only
       field that names one particular item. Matching a unit by its base number instead left two
       readings of the same drop looking like a contradiction. */
    DWORD head[9];
    if (!safe_read((const void *)(UINT_PTR)address, head, sizeof(head))) return FALSE;
    out->type = head[0]; out->txtfile = head[1];
    out->unit_id = head[3]; out->mode = head[4]; out->item_data = head[5];
    out->seed = head[8];
    return TRUE;
}

static void walk_unit_table(void)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) { log_line("walk: D2Client.dll is not loaded"); return; }
    const BYTE *table = (const BYTE *)client + OFF_D2CLIENT_UNIT_TABLE_ITEMS;
    log_line("walk: item units through D2Client.dll+0x%X", OFF_D2CLIENT_UNIT_TABLE_ITEMS);

    for (unsigned li = 0; li < sizeof(LINK_CANDIDATES) / sizeof(LINK_CANDIDATES[0]); li++) {
        int link = LINK_CANDIDATES[li];
        int seen = 0, chained = 0;
        log_line("  trying next-pointer at +0x%02X", link);
        for (int bucket = 0; bucket < UNIT_BUCKETS; bucket++) {
            DWORD address = 0;
            if (!safe_read(table + bucket * 4, &address, 4)) continue;
            int depth = 0;
            while (address && depth < 64) {
                unit_head unit;
                if (!read_unit(address, &unit) || unit.type != 4) break;
                DWORD quality = 0, file_index = 0;
                safe_read((const void *)(UINT_PTR)unit.item_data, &quality, 4);
                safe_read((const void *)(UINT_PTR)(unit.item_data + 0x28), &file_index, 4);
                /* Only the interesting ones: an inventory full of junk would bury the answer. */
                if (quality == 5 || quality == 7) {
                    log_line("    unit %p guid=%08X txtfile=%-4lu mode=%lu %s id=%lu",
                             (void *)(UINT_PTR)address, unit.seed,
                             (unsigned long)unit.txtfile, (unsigned long)unit.mode,
                             quality == 7 ? "unique" : "set   ", (unsigned long)file_index);
                }
                seen++;
                if (depth > 0) chained++;
                DWORD next = 0;
                if (!safe_read((const void *)(UINT_PTR)(address + link), &next, 4)) break;
                address = next;
                depth++;
            }
        }
        log_line("  +0x%02X: %d item unit(s), %d of them reached by following the link",
                 link, seen, chained);
    }
    log_line("");
}

/* The player unit is the anchor a renderer actually needs: from it come the act, the room, and
   the room's own list of what is lying in it — a handful of pointers per frame instead of a
   sweep. Finding it needs no string search and no guessing, because the items already told us
   who owns them: every stored item's ItemData carries owner id 1, and the one on the ground
   carries -1. So the player is the unit with dwType 0 and dwUnitId 1. */
static void find_player_and_its_slot(void)
{
    if (!player_name[0]) { log_line("player: no 'player <name>' line in the config"); return; }
    log_line("player: anchoring on the name \"%s\"", player_name);

    /* Two guesses at what the player unit looks like both matched rubbish — dwType 0 with
       dwUnitId 1 is a zero and a one, and memory is full of those. The character's own name is
       not: it appears at the start of PlayerData, so finding the name and then finding who
       points at it walks straight to the unit with nothing left to guess. */
    SIZE_T name_len = strlen(player_name) + 1;
    /* Every place the name appears, not the first: it turns up in save buffers and UI text too,
       and the first hit was at an odd address — PlayerData is heap-allocated and aligned, so
       that one could never have been it. */
    #define MAX_NAMES 64
    DWORD names[MAX_NAMES];
    int name_count = 0;
    DWORD unit = 0;

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    BYTE *limit = (BYTE *)info.lpMaximumApplicationAddress;

    for (int phase = 0; phase < 2 && !unit; phase++) {
        BYTE *address = (BYTE *)info.lpMinimumApplicationAddress;
        while (address < limit) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(address, &mbi, sizeof(mbi))) break;
            BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            /* Skip only OUR OWN image. An earlier version wrote `>= self_end`, which threw
               away every region below this DLL — that is the whole game heap, and it is why the
               character's own name could not be found anywhere in memory. */
            if (mbi.State == MEM_COMMIT && readable(mbi.Protect) &&
                !((const BYTE *)mbi.BaseAddress >= self_start &&
                  (const BYTE *)mbi.BaseAddress < self_end)) {
                const BYTE *base = (const BYTE *)mbi.BaseAddress;
                for (SIZE_T done = 0; done < mbi.RegionSize; done += CHUNK) {
                    SIZE_T count = mbi.RegionSize - done;
                    if (count > CHUNK) count = CHUNK;
                    if (!safe_read(base + done, chunk, count)) continue;
                    if (phase == 0) {
                        for (SIZE_T o = 0; o + name_len <= count && name_count < MAX_NAMES; o++) {
                            if (memcmp(chunk + o, player_name, name_len)) continue;
                            names[name_count++] = (DWORD)(UINT_PTR)(base + done + o);
                        }
                    } else {
                        for (SIZE_T o = 0; o + 4 <= count; o += 4) {
                            DWORD value = *(const DWORD *)(chunk + o);
                            int matched = 0;
                            for (int n = 0; n < name_count; n++)
                                if (value == names[n]) { matched = 1; break; }
                            if (!matched) continue;
                            DWORD slot = (DWORD)(UINT_PTR)(base + done + o);
                            /* PlayerData hangs off UnitAny+0x14, so the unit starts there. */
                            DWORD candidate = slot - UNIT_TO_ITEMDATA;
                            DWORD head[4];
                            if (!safe_read((const void *)(UINT_PTR)candidate, head, sizeof(head)))
                                continue;
                            log_line("  %08X points at the name; unit would start at %08X "
                                     "(dwType=%lu class=%lu)", slot, candidate,
                                     (unsigned long)head[0], (unsigned long)head[1]);
                            if (head[0] != 0) continue;
                            unit = candidate;
                            log_line("  -> dwType=0, that is the player");
                            break;
                        }
                        if (unit) break;
                    }
                }
            }
            if (next <= address) break;
            address = next;
        }
        if (phase == 0) {
            log_line("  the name appears %d time(s) in memory", name_count);
            /* PlayerData starts with the name and continues with quest/waypoint pointers, so the
               bytes AFTER each occurrence say which one it is. Guessing from the address alone
               has now failed twice; this shows the evidence instead. */
            for (int n = 0; n < name_count; n++) {
                log_line("    occurrence %d at %08X%s", n, names[n],
                         (names[n] & 3) ? "  (unaligned — cannot be a struct start)" : "");
                if ((names[n] & 3) == 0)
                    dump_range((const BYTE *)(UINT_PTR)names[n],
                               (const BYTE *)(UINT_PTR)names[n], 0x40);
            }
            if (!name_count) { log_line("  nowhere at all"); return; }
        }
    }
    if (!unit) { log_line("  nothing unit-shaped points at the name"); return; }

    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return;
    const BYTE *base = (const BYTE *)client;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const BYTE *from = base + section->VirtualAddress;
        for (SIZE_T offset = 0; offset + 4 <= section->Misc.VirtualSize; offset += 4) {
            DWORD value = 0;
            if (!safe_read(from + offset, &value, 4)) continue;
            if (value != unit) continue;
            log_line("  STATIC D2Client.dll+0x%06X holds the player unit",
                     (unsigned)(from + offset - base));
            found++;
        }
    }
    if (!found) log_line("  nothing in D2Client's data points at it directly");
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
    walk_unit_table();
    find_player_and_its_slot();
    log_line("=== probe pass done in %lu ms ===", (unsigned long)(GetTickCount() - started));
}

/* Plays the next configured line, so one key press walks the whole list and the log says which
   one was just heard. Finding both the right sound and the right argument order is a listening
   exercise, and this is the shortest loop between a guess and hearing it. */
static void resolve_play_sound(void)
{
    if (play_sound) return;
    /* Looked up on use, not at load. This plugin comes in through Game.exe's import table, which
       is the earliest moment there is — ProjectDiablo.dll is not loaded yet, and asking for it
       then simply answers "no". */
    HMODULE pd2 = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2) { log_line("sound: ProjectDiablo.dll still is not loaded"); return; }
    play_sound = (play_sound_fn)GetProcAddress(
        pd2, "_D2Client_PlaySoundWithCustomVolumeOrPriority@20");
    log_line("sound: ProjectDiablo.dll at %p, function %s", (void *)pd2,
             play_sound ? "found" : "NOT found by that name");
    if (!play_sound) {
        /* Some toolchains export it undecorated; try that before giving up. */
        play_sound = (play_sound_fn)GetProcAddress(
            pd2, "D2Client_PlaySoundWithCustomVolumeOrPriority");
        if (play_sound) log_line("sound: found under the undecorated name");
    }
}

/* One F-key per line: F1 plays the first configured sound, F2 the second, and so on. */
void probe_play_line(int index)
{
    resolve_play_sound();
    if (!play_sound) return;
    if (index >= sound_count) return;
    sound_probe *s = &sounds[index];
    /* arg0 is a pointer the callee dereferences. A small integer there is not a unit, it is a
       crash — which is exactly how this was learned. */
    if (s->arg[0] != 0 && s->arg[0] < 0x10000) {
        log_line("F%d: refusing arg0=%d — it is a pointer, and that is not one",
                 index + 1, s->arg[0]);
        return;
    }
    log_line("F%d: (%d, %d, %d, %d, %d) — %s", index + 1,
             s->arg[0], s->arg[1], s->arg[2], s->arg[3], s->arg[4], s->label);
    play_sound(s->arg[0], s->arg[1], s->arg[2], s->arg[3], s->arg[4]);
}

/* The camera in this game is centred on the player, so an item's place on screen is a function
   of how far it is from the player and nothing else — no camera globals to hunt for. What is
   still unknown is which words inside a unit's path structure hold the position, so this prints
   them for the player and for whatever is lying on the ground, to be read side by side.

   UnitAny+0x2C is the path pointer for every unit type. */
#define UNIT_TO_PATH 0x2C

/* Everything worth knowing about what the game is holding right now, in one press.
 *
 * The aim it serves: a light beam should be an OBJECT the game animates by itself — Objects.txt
 * is full of non-interactive "Dummy" ones that exist only to glow, and the game draws, animates
 * and lights them with no help from us. To put one next to a dropped item we first need to see
 * what an object looks like in memory next to an item, which is what this prints.
 *
 * The unit table has a row per type: 0 players, 1 monsters, 2 objects, 3 missiles, 4 items,
 * 5 tiles. 128 buckets each, and the item row we already use sits at type 4. */
#define UNIT_TABLE_BASE 0x10A608
#define UNIT_ROW_BYTES  (UNIT_BUCKETS * 4)

static const char *const TYPE_NAMES[] = { "player", "monster", "object", "missile", "item", "tile" };

static int walk_row(const BYTE *client, int type, int list_limit, int dump_first)
{
    const BYTE *row = client + UNIT_TABLE_BASE + type * UNIT_ROW_BYTES;
    int seen = 0, dumped = 0;
    for (int bucket = 0; bucket < UNIT_BUCKETS; bucket++) {
        DWORD address = 0;
        if (!safe_read(row + bucket * 4, &address, 4)) continue;
        int depth = 0;
        while (address && depth++ < 64) {
            unit_head unit;
            if (!read_unit(address, &unit)) break;
            if (unit.type != (DWORD)type) break;
            seen++;
            if (seen <= list_limit) {
                DWORD path = 0, x = 0, y = 0;
                safe_read((const void *)(UINT_PTR)(address + UNIT_TO_PATH), &path, 4);
                if (path) {
                    /* Items keep whole numbers further in; everything else keeps 16.16 up front. */
                    if (type == 4) {
                        safe_read((const void *)(UINT_PTR)(path + 0x0C), &x, 4);
                        safe_read((const void *)(UINT_PTR)(path + 0x10), &y, 4);
                    } else {
                        safe_read((const void *)(UINT_PTR)(path + 0x00), &x, 4);
                        safe_read((const void *)(UINT_PTR)(path + 0x04), &y, 4);
                        x >>= 16; y >>= 16;
                    }
                }
                log_line("    %-8s %08X txt=%-5lu mode=%-3lu at (%lu,%lu)",
                         TYPE_NAMES[type], address, (unsigned long)unit.txtfile,
                         (unsigned long)unit.mode, (unsigned long)x, (unsigned long)y);
            }
            if (dumped < dump_first) {
                dumped++;
                log_line("      ^ full structure:");
                dump_range((const BYTE *)(UINT_PTR)address, (const BYTE *)(UINT_PTR)address, 0x100);
            }
            DWORD next = 0;
            if (!safe_read((const void *)(UINT_PTR)(address + 0xEC), &next, 4)) break;
            address = next;
        }
    }
    return seen;
}

void probe_survey(void)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) { log_line("survey: D2Client is not loaded"); return; }
    const BYTE *base = (const BYTE *)client;
    log_line("=== survey ===");
    for (int type = 0; type <= 5; type++) {
        /* Objects get listed generously and dumped: they are the point of this survey. Items get
           listed too, for the comparison. Everything else is just a count. */
        int list = (type == 2) ? 24 : (type == 4 ? 8 : 0);
        int dump = (type == 2 || type == 4) ? 1 : 0;
        log_line("  type %d (%s):", type, TYPE_NAMES[type]);
        int n = walk_row(base, type, list, dump);
        log_line("    %d unit(s) of this type", n);
    }
    log_line("=== survey done ===");
}

void probe_dump_paths(void)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return;
    log_line("paths: the player first, then anything on the ground");

    DWORD player = 0;
    if (safe_read((const BYTE *)client + 0x10A60C, &player, 4) && player) {
        DWORD path = 0;
        if (safe_read((const void *)(UINT_PTR)(player + UNIT_TO_PATH), &path, 4) && path) {
            log_line("  player unit %08X path %08X", player, path);
            dump_range((const BYTE *)(UINT_PTR)path, (const BYTE *)(UINT_PTR)path, 0x40);
        }
    }

    const BYTE *table = (const BYTE *)client + OFF_D2CLIENT_UNIT_TABLE_ITEMS;
    for (int bucket = 0; bucket < UNIT_BUCKETS; bucket++) {
        DWORD address = 0;
        if (!safe_read(table + bucket * 4, &address, 4)) continue;
        int depth = 0;
        while (address && depth++ < 64) {
            unit_head unit;
            if (!read_unit(address, &unit) || unit.type != 4) break;
            if (unit.mode == 3) {
                DWORD quality = 0, identity = 0, path = 0;
                safe_read((const void *)(UINT_PTR)unit.item_data, &quality, 4);
                safe_read((const void *)(UINT_PTR)(unit.item_data + 0x28), &identity, 4);
                safe_read((const void *)(UINT_PTR)(address + UNIT_TO_PATH), &path, 4);
                log_line("  ground item %08X quality=%lu identity=%lu path %08X",
                         address, (unsigned long)quality, (unsigned long)identity, path);
                if (path)
                    dump_range((const BYTE *)(UINT_PTR)path, (const BYTE *)(UINT_PTR)path, 0x40);
            }
            DWORD next = 0;
            if (!safe_read((const void *)(UINT_PTR)(address + 0xEC), &next, 4)) break;
            address = next;
        }
    }
    log_line("paths: done");
}

/* Runs on every rendered frame. Counting the grail candidates lying in view is the cheapest
   thing that proves the whole chain works — the hook fires, the unit table reads, the items are
   there and their quality and identity come out — before a single pixel is drawn. */
void frame_tick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 2000) return;     /* the log is for a person, not for 60fps */
    last = now;

    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return;
    const BYTE *table = (const BYTE *)client + OFF_D2CLIENT_UNIT_TABLE_ITEMS;
    int on_ground = 0, uniques = 0, sets = 0;
    for (int bucket = 0; bucket < UNIT_BUCKETS; bucket++) {
        DWORD address = 0;
        if (!safe_read(table + bucket * 4, &address, 4)) continue;
        int depth = 0;
        while (address && depth++ < 64) {
            unit_head unit;
            if (!read_unit(address, &unit) || unit.type != 4) break;
            if (unit.mode == 3) {
                DWORD quality = 0;
                safe_read((const void *)(UINT_PTR)unit.item_data, &quality, 4);
                on_ground++;
                if (quality == 7) uniques++;
                else if (quality == 5) sets++;
            }
            DWORD next = 0;
            if (!safe_read((const void *)(UINT_PTR)(address + 0xEC), &next, 4)) break;
            address = next;
        }
    }
    if (on_ground)
        log_line("frame: %d item(s) on the ground right now — %d unique, %d set",
                 on_ground, uniques, sets);
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
