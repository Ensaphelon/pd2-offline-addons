#include <windows.h>
#include <string.h>
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

/* Glide is a C API and every entry point we need is exported by name, so drawing needs no
   offsets either. The vertex is whatever layout the game last set; x and y at the front is the
   one thing every layout agrees on, which is all a flat-coloured line needs. */
typedef void (__stdcall *gr_draw_line_fn)(const void *, const void *);
typedef void (__stdcall *gr_constant_colour_fn)(unsigned long);
typedef void (__stdcall *gr_colour_combine_fn)(unsigned long, unsigned long, unsigned long,
                                               unsigned long, int);
typedef void (__stdcall *gr_alpha_blend_fn)(unsigned long, unsigned long, unsigned long,
                                            unsigned long);

static gr_draw_line_fn gr_draw_line;
static gr_constant_colour_fn gr_constant_colour;
static gr_colour_combine_fn gr_colour_combine;
static gr_alpha_blend_fn gr_alpha_blend;

/* Glide constants, from its own header. Spelled out rather than included because the SDK is not
   here and these five numbers are the whole of what is needed. */
#define GR_COMBINE_FUNCTION_LOCAL        0x2
#define GR_COMBINE_FACTOR_NONE           0x0
#define GR_COMBINE_LOCAL_CONSTANT        0x0
#define GR_COMBINE_OTHER_NONE            0x0
#define GR_BLEND_SRC_ALPHA               0x4
#define GR_BLEND_ONE_MINUS_SRC_ALPHA     0x5
#define GR_BLEND_ONE                     0x1
#define GR_BLEND_ZERO                    0x0

typedef struct { float x, y, pad[14]; } glide_vertex;

/* A plain vertical line at a fixed spot, drawn before the frame is handed over. Nothing is
   computed here on purpose: if this appears, drawing works, and position becomes the only thing
   left to get wrong. */
static void draw_test_line(void)
{
    if (!gr_draw_line) return;
    gr_colour_combine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                      GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, 0);
    gr_alpha_blend(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                   GR_BLEND_ONE, GR_BLEND_ZERO);
    gr_constant_colour(0xC0FFE8A0);      /* a warm, mostly-opaque gold */

    glide_vertex top, bottom;
    memset(&top, 0, sizeof(top));
    memset(&bottom, 0, sizeof(bottom));
    top.x = 400.0f;  top.y = 80.0f;
    bottom.x = 400.0f; bottom.y = 400.0f;
    gr_draw_line(&top, &bottom);
}

/* Drawing a line through Glide asks D2GL to agree with us about vertex layout and render state,
   and the first attempt produced nothing at all. Writing pixels into the back buffer asks it for
   none of that — lock, poke, unlock. D2GL implements the calls (checked: real code, not stubs),
   and if this shows up while the line does not, the problem was state rather than the hook. */
typedef int (__stdcall *gr_lfb_lock_fn)(unsigned long, unsigned long, unsigned long,
                                        unsigned long, int, void *);
typedef int (__stdcall *gr_lfb_unlock_fn)(unsigned long, unsigned long);
static gr_lfb_lock_fn gr_lfb_lock;
static gr_lfb_unlock_fn gr_lfb_unlock;

typedef struct {
    unsigned long size;
    void *ptr;
    unsigned long stride;
    unsigned long write_mode;
    unsigned long origin;
} lfb_info;

#define GR_LFB_WRITE_ONLY      1
#define GR_BUFFER_BACKBUFFER   1
#define GR_LFBWRITEMODE_ANY    0xFF
#define GR_ORIGIN_UPPER_LEFT   0

static void draw_test_pixels(void)
{
    if (!gr_lfb_lock || !gr_lfb_unlock) return;
    lfb_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!gr_lfb_lock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_ANY,
                     GR_ORIGIN_UPPER_LEFT, 0, &info)) {
        static int said;
        if (!said) { said = 1; log_line("frame: grLfbLock refused"); }
        return;
    }
    static int described;
    if (!described) {
        described = 1;
        log_line("frame: lfb ptr=%p stride=%lu writeMode=%lu origin=%lu",
                 info.ptr, info.stride, info.write_mode, info.origin);
    }
    if (info.ptr && info.stride) {
        /* A fat vertical bar near the left edge, in whatever the pixel format turns out to be —
           all-bits-set is white or near-white in every one of them, which is enough to see. */
        for (int y = 60; y < 360; y++) {
            BYTE *row = (BYTE *)info.ptr + (size_t)y * info.stride;
            memset(row + 40, 0xFF, 16);
        }
    }
    gr_lfb_unlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
}

static void resolve_glide(void)
{
    if (gr_draw_line) return;
    HMODULE glide = GetModuleHandleA("glide3x.dll");
    if (!glide) return;
    gr_draw_line = (gr_draw_line_fn)GetProcAddress(glide, "_grDrawLine@8");
    gr_constant_colour = (gr_constant_colour_fn)GetProcAddress(glide, "_grConstantColorValue@4");
    gr_colour_combine = (gr_colour_combine_fn)GetProcAddress(glide, "_grColorCombine@20");
    gr_alpha_blend = (gr_alpha_blend_fn)GetProcAddress(glide, "_grAlphaBlendFunction@16");
    gr_lfb_lock = (gr_lfb_lock_fn)GetProcAddress(glide, "_grLfbLock@24");
    gr_lfb_unlock = (gr_lfb_unlock_fn)GetProcAddress(glide, "_grLfbUnlock@8");
    log_line("frame: glide draw entry points %s",
             (gr_draw_line && gr_constant_colour && gr_colour_combine && gr_alpha_blend)
             ? "all found" : "INCOMPLETE");
    if (!(gr_draw_line && gr_constant_colour && gr_colour_combine && gr_alpha_blend))
        gr_draw_line = NULL;
}

typedef void (__stdcall *buffer_swap_fn)(int);
static buffer_swap_fn original_swap;
static void **hooked_slot;

static DWORD frames;
static DWORD last_report;

void frame_tick(void);   /* what we actually want to do each frame; see probe.c */
void calls_report(DWORD frames);
BOOL calls_watch_install(void);

static void __stdcall our_swap(int interval)
{
    frames++;
    DWORD now = GetTickCount();
    if (now - last_report >= 5000) {
        log_line("frame: %lu frames drawn (about %lu per second)",
                 (unsigned long)frames,
                 (unsigned long)(frames * 1000 / (now - last_report ? now - last_report : 1)));
        calls_report(frames);
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

/* Which module actually calls Glide. D2Gfx was the guess and it was wrong — it is the renderer
   FRONT end, and each backend is its own DLL. D2Glide.dll is the one that imports glide3x, but
   the list is tried in order rather than hardcoded to one, since a player on a different
   renderer loads a different backend. */
static const char *const GLIDE_CALLERS[] = {
    "D2Glide.dll", "D2Gfx.dll", "D2Direct3D.dll", "D2DDraw.dll", "Game.exe",
};

/* What the game tells Glide about its own vertices. Our line was built on the guess that x and
   y sit at the front of the struct; this stops guessing by listening to the call that actually
   says so. grVertexLayout(param, offset, mode) is issued once per parameter at start-up.
     0x01 XY   0x02 Z   0x03 W   0x04 A   0x05 RGB   0x10.. texture coords  */
typedef void (__stdcall *gr_vertex_layout_fn)(unsigned long, long, unsigned long);
static gr_vertex_layout_fn original_layout;
static void **layout_slot;

static void __stdcall our_vertex_layout(unsigned long param, long offset, unsigned long mode)
{
    static const char *names[] = { "?", "XY", "Z", "W", "A", "RGB" };
    log_line("layout: param 0x%02lX (%s) at offset %ld, mode %lu",
             param, param < 6 ? names[param] : "tex/other", offset, mode);
    if (original_layout) original_layout(param, offset, mode);
}

BOOL frame_hook_install(void)
{
    if (hooked_slot) return TRUE;
    static DWORD last_complaint;

    void *previous = NULL;
    for (unsigned i = 0; i < sizeof(GLIDE_CALLERS) / sizeof(GLIDE_CALLERS[0]); i++) {
        HMODULE module = GetModuleHandleA(GLIDE_CALLERS[i]);
        if (!module) continue;
        hooked_slot = redirect_import(module, "glide3x.dll", "_grBufferSwap@4",
                                      (void *)our_swap, &previous);
        if (hooked_slot) {
            log_line("frame: hooked %s's call to glide3x!_grBufferSwap (was %p)",
                     GLIDE_CALLERS[i], previous);
            break;
        }
    }
    if (!hooked_slot) {
        /* Tried every frame until it works, so this must not shout every time. */
        DWORD now = GetTickCount();
        if (now - last_complaint > 10000) {
            last_complaint = now;
            log_line("frame: nothing loaded yet imports glide3x!_grBufferSwap — still waiting");
        }
        return FALSE;
    }
    original_swap = (buffer_swap_fn)previous;
    last_report = GetTickCount();

    /* Listen in on the vertex layout too, from the same module. */
    for (unsigned i = 0; i < sizeof(GLIDE_CALLERS) / sizeof(GLIDE_CALLERS[0]); i++) {
        HMODULE module = GetModuleHandleA(GLIDE_CALLERS[i]);
        if (!module) continue;
        void *was = NULL;
        layout_slot = redirect_import(module, "glide3x.dll", "_grVertexLayout@12",
                                      (void *)our_vertex_layout, &was);
        if (layout_slot) { original_layout = (gr_vertex_layout_fn)was; break; }
    }
    log_line("frame: vertex layout %s", layout_slot ? "being listened to" : "not imported by name");
    calls_watch_install();
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
