#include <windows.h>
#include <string.h>
#include "log.h"

/* Which of D2Client's calls into D2gfx happen while the world is being drawn.
 *
 * Drawing has to happen inside the game's own frame, using the game's own primitives, or it only
 * works on one renderer — a lesson paid for by drawing through the Glide wrapper and seeing
 * nothing. D2Client reaches its drawing through D2gfx — note the lower-case g, which is how it
 * appears in the import table and which hid it from an earlier search that compared names
 * exactly. Fifty-eight ordinals, and which is which is not written down anywhere we have.
 *
 * D2Win was watched first and answered a different question: its #10190 takes three bytes and
 * feeds a palette, and its #10024 is simply the last of its calls each frame. Neither draws the
 * world.
 *
 * So all thirty-eight get counted at once. Each import is pointed at a twelve-byte thunk that
 * increments a counter and jumps on to the real function — no stack touched, no arguments
 * inspected, nothing that can go wrong with a calling convention we have not checked. What comes
 * back is a frequency table: the one called once per frame is where a renderer belongs, and the
 * ones called hundreds of times are the primitives themselves. */

#define MAX_HOOKS 96

static void *originals[MAX_HOOKS];
static volatile LONG counts[MAX_HOOKS];
static WORD ordinals[MAX_HOOKS];
static void **slots[MAX_HOOKS];
static int hook_count;
static BYTE *thunks;

/* Which call was the last one of the frame. Counting said which ordinals are frame boundaries;
   this says which of them comes last, and that is where something drawn over the world belongs.
   A full ordering would need real logic inside a hand-assembled thunk; "the most recent one"
   needs one more instruction, and the frame hook reads it at exactly the right moment. */
static volatile LONG last_index = -1;

/* inc dword ptr [counter]   FF 05 <abs32>
   jmp dword ptr [original]  FF 25 <abs32>  */
static void write_thunk(BYTE *at, int index, volatile LONG *counter, void **original)
{
    volatile LONG *last = &last_index;
    at[0] = 0xC7; at[1] = 0x05; memcpy(at + 2, &last, 4); memcpy(at + 6, &index, 4);
    at[10] = 0xFF; at[11] = 0x05; memcpy(at + 12, &counter, 4);
    at[16] = 0xFF; at[17] = 0x25; memcpy(at + 18, &original, 4);
}


static void **redirect_ordinal(HMODULE module, const char *dll, WORD ordinal, void *with,
                               void **out_original)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *import = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    for (; import->Name; import++) {
        if (_stricmp((const char *)(base + import->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *lookup = (IMAGE_THUNK_DATA *)
            (base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        IMAGE_THUNK_DATA *address = (IMAGE_THUNK_DATA *)(base + import->FirstThunk);
        for (; lookup->u1.AddressOfData; lookup++, address++) {
            if (!IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) continue;
            if (IMAGE_ORDINAL(lookup->u1.Ordinal) != ordinal) continue;
            void **slot = (void **)&address->u1.Function;
            DWORD was;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &was)) return NULL;
            *out_original = *slot;
            *slot = with;
            VirtualProtect(slot, sizeof(void *), was, &was);
            return slot;
        }
    }
    return NULL;
}

/* Every ordinal D2Client imports from D2Win, read out of its import table rather than listed. */
static int collect_ordinals(HMODULE module, const char *dll, WORD *out, int limit)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    int found = 0;
    IMAGE_IMPORT_DESCRIPTOR *import = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    for (; import->Name && found < limit; import++) {
        if (_stricmp((const char *)(base + import->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *lookup = (IMAGE_THUNK_DATA *)
            (base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        for (; lookup->u1.AddressOfData && found < limit; lookup++)
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal))
                out[found++] = (WORD)IMAGE_ORDINAL(lookup->u1.Ordinal);
    }
    return found;
}

BOOL calls_watch_install(void)
{
    if (hook_count) return TRUE;
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return FALSE;

    WORD wanted[MAX_HOOKS];
    int count = collect_ordinals(client, "D2gfx.dll", wanted, MAX_HOOKS);
    if (!count) { log_line("calls: D2Client imports no D2gfx ordinals"); return FALSE; }

    thunks = (BYTE *)VirtualAlloc(NULL, count * 32, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!thunks) { log_line("calls: could not allocate thunks"); return FALSE; }

    for (int i = 0; i < count; i++) {
        BYTE *thunk = thunks + i * 32;
        write_thunk(thunk, i, &counts[i], &originals[i]);
        void *previous = NULL;
        void **slot = redirect_ordinal(client, "D2gfx.dll", wanted[i], thunk, &previous);
        if (!slot) continue;
        originals[i] = previous;
        slots[i] = slot;
        ordinals[i] = wanted[i];
        hook_count++;
    }
    log_line("calls: watching %d of D2Client's %d calls into D2gfx", hook_count, count);
    return hook_count > 0;
}

/* Record one frame's worth of ordinals in arrival order, then stop. Done from the counting side
   rather than inside the thunks: a thunk that writes two places is two more chances to get a
   hand-assembled instruction wrong, and this is only needed once. */
/* Called from the frame hook, so "per frame" means per frame. */
void calls_report(DWORD frames)
{
    if (!hook_count || !frames) return;
    LONG last = last_index;
    if (last >= 0 && last < hook_count)
        log_line("calls: the LAST D2Win call before the frame was handed over is #%u",
                 ordinals[last]);
    log_line("calls: over %lu frames —", (unsigned long)frames);
    for (int i = 0; i < hook_count; i++) {
        LONG n = InterlockedExchange(&counts[i], 0);
        if (!n) continue;
        log_line("    D2gfx #%u: %ld calls  (%.1f per frame)",
                 ordinals[i], n, (double)n / (double)frames);
    }
}

void calls_watch_remove(void)
{
    for (int i = 0; i < hook_count; i++) {
        if (!slots[i]) continue;
        DWORD was;
        if (VirtualProtect(slots[i], sizeof(void *), PAGE_READWRITE, &was)) {
            *slots[i] = originals[i];
            VirtualProtect(slots[i], sizeof(void *), was, &was);
        }
    }
    hook_count = 0;
}
