#include <windows.h>
#include "log.h"

/* A moment on every rendered frame, which is where anything drawn over the world has to happen.
 *
 * Got without hunting for a single offset. PD2 renders through D2GL, a Glide wrapper shipped as
 * glide3x.dll, and Glide's frame swap is an ordinary exported function — `_grBufferSwap@4`.
 * D2Gfx.dll imports it by name, so its import table holds a pointer we can replace with our own.
 * Everything here is documented PE structure, the same walk the installer already does to add an
 * import, and it undoes itself by putting the original pointer back.
 *
 * The alternative was finding D2Client's own per-frame draw by pattern, which is the kind of
 * search that costs days and breaks on the next game update. */

typedef void (__stdcall *buffer_swap_fn)(int);
static buffer_swap_fn original_swap;
static void **hooked_slot;

static DWORD frames;
static DWORD last_report;

void frame_tick(void);   /* what we actually want to do each frame; see probe.c */

static void __stdcall our_swap(int interval)
{
    frames++;
    DWORD now = GetTickCount();
    if (now - last_report >= 5000) {
        log_line("frame: %lu frames drawn (about %lu per second)",
                 (unsigned long)frames,
                 (unsigned long)(frames * 1000 / (now - last_report ? now - last_report : 1)));
        frames = 0;
        last_report = now;
    }
    frame_tick();
    if (original_swap) original_swap(interval);
}

/* Replaces one named import in `module`'s import table, and hands back where it was found so it
   can be put back. */
static void **redirect_import(HMODULE module, const char *dll, const char *symbol, void *with,
                              void **out_original)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *import = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    for (; import->Name; import++) {
        const char *name = (const char *)(base + import->Name);
        if (_stricmp(name, dll) != 0) continue;
        /* The lookup table keeps the NAMES; the address table keeps the pointers the code calls
           through. They run in step, so walking both finds the slot for one symbol. */
        IMAGE_THUNK_DATA *lookup = (IMAGE_THUNK_DATA *)
            (base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        IMAGE_THUNK_DATA *address = (IMAGE_THUNK_DATA *)(base + import->FirstThunk);
        for (; lookup->u1.AddressOfData; lookup++, address++) {
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *by_name =
                (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            if (strcmp((const char *)by_name->Name, symbol) != 0) continue;

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

BOOL frame_hook_install(void)
{
    if (hooked_slot) return TRUE;
    HMODULE gfx = GetModuleHandleA("D2Gfx.dll");
    if (!gfx) { log_line("frame: D2Gfx.dll is not loaded yet"); return FALSE; }

    void *previous = NULL;
    hooked_slot = redirect_import(gfx, "glide3x.dll", "_grBufferSwap@4", (void *)our_swap,
                                  &previous);
    if (!hooked_slot) {
        log_line("frame: D2Gfx.dll does not import _grBufferSwap@4 by name");
        return FALSE;
    }
    original_swap = (buffer_swap_fn)previous;
    last_report = GetTickCount();
    log_line("frame: hooked D2Gfx's call to glide3x!_grBufferSwap (was %p)", previous);
    return TRUE;
}

void frame_hook_remove(void)
{
    if (!hooked_slot) return;
    DWORD was;
    if (VirtualProtect(hooked_slot, sizeof(void *), PAGE_READWRITE, &was)) {
        *hooked_slot = (void *)original_swap;
        VirtualProtect(hooked_slot, sizeof(void *), was, &was);
    }
    hooked_slot = NULL;
}
