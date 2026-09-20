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

#define MAX_HOOKS 768

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
/* Which call was last in the PREVIOUS frame — so the draw goes out after everything the game
   itself drew, without anyone having to guess which ordinal that is. */
static volatile LONG draw_on = -1;
void calls_draw_test_box(void);

/* inc dword ptr [counter]   FF 05 <abs32>
   jmp dword ptr [original]  FF 25 <abs32>  */
/* Drawing has to happen INSIDE the game's frame — twice now it was done at the Glide swap, when
   the frame is already composed, and twice nothing appeared. So the thunk gains a call to us.
   Registers and flags are saved and the stack is never touched, which means this works for any
   ordinal whatever its signature: the original still sees exactly the arguments it was passed.

     60              pushad
     9C              pushfd
     68 <index>      push index
     E8 <rel32>      call calls_at
     83 C4 04        add esp,4
     9D              popfd
     61              popad
     FF 05 <counter> inc
     FF 25 <orig>    jmp                                                                    */
void __cdecl calls_at(int index);

static void write_draw_thunk(BYTE *at, int index, volatile LONG *counter, void **original)
{
    int i = 0;
    at[i++] = 0x60;
    at[i++] = 0x9C;
    at[i++] = 0x68; memcpy(at + i, &index, 4); i += 4;
    at[i++] = 0xE8;
    {
        LONG rel = (LONG)((BYTE *)calls_at - (at + i + 4));
        memcpy(at + i, &rel, 4); i += 4;
    }
    at[i++] = 0x83; at[i++] = 0xC4; at[i++] = 0x04;
    at[i++] = 0x9D;
    at[i++] = 0x61;
    at[i++] = 0xFF; at[i++] = 0x05; memcpy(at + i, &counter, 4); i += 4;
    at[i++] = 0xFF; at[i++] = 0x25; memcpy(at + i, &original, 4);
}

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
        write_draw_thunk(thunk, i, &counts[i], &originals[i]);
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
    draw_on = last;
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

/* Ordinals and offsets from SlashDiablo Maphack's D2Ptrs.h, which is BH's own source and AGPL —
   public by licence. Six failed attempts at drawing were all searches for numbers that have been
   written down for twenty years:

     D2Gfx  #10014  DrawRectangle(x1, y1, x2, y2, colour, transparency)
     D2Gfx  #10010  DrawLine(x1, y1, x2, y2, colour, unknown)     <- what was tried, with
                                                                     rectangle arguments
     D2Client+0x1630 / +0x1660   GetUnitX / GetUnitY   (__fastcall)
     D2Client+0x3F6C0 / +0x3F6D0 GetMouseXOffset / GetMouseYOffset
     D2Client+0xDBC48 / +0xDBC4C ScreenSizeX / ScreenSizeY

   Colour is a palette index, not RGB, and transparency 0 is solid while 5 blends. */
typedef void (__stdcall *gfx_draw6_fn)(int, int, int, int, int, int);

void __cdecl calls_at(int index)
{
    last_index = index;
    if (index == draw_on) calls_draw_test_box();
}

/* World to screen. The camera is centred on the player, so a point's place on screen follows
   from how far it is from the player: D2's ground is isometric, one subtile being 16 pixels
   across and 8 down. Drawn at a FIXED point in the world rather than a fixed point on screen —
   if the transform is right the box stays on its patch of ground while the player walks, and if
   it is wrong it slides, which is a difference nobody can mistake. */
typedef void *(__stdcall *get_player_fn)(void);
typedef int (__fastcall *get_coord_fn)(void *unit);

#define OFF_GETPLAYERUNIT 0xA4D60
#define OFF_GETUNITX      0x1630
#define OFF_GETUNITY      0x1660
#define OFF_SCREENSIZEX   0xDBC48
#define OFF_SCREENSIZEY   0xDBC4C

static int world_to_screen(int world_x, int world_y, int *out_x, int *out_y)
{
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return 0;
    const BYTE *base = (const BYTE *)client;
    get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
    get_coord_fn get_x = (get_coord_fn)(base + OFF_GETUNITX);
    get_coord_fn get_y = (get_coord_fn)(base + OFF_GETUNITY);

    void *player = get_player();
    if (!player) return 0;
    int px = get_x(player), py = get_y(player);
    DWORD width = 800, height = 600;
    safe_read(base + OFF_SCREENSIZEX, &width, 4);
    safe_read(base + OFF_SCREENSIZEY, &height, 4);

    int dx = world_x - px, dy = world_y - py;
    *out_x = (int)(width / 2) + (dx - dy) * 16;
    *out_y = (int)(height / 2) + (dx + dy) * 8;
    return 1;
}

void calls_draw_test_box(void)
{
    static gfx_draw6_fn draw;
    if (!draw) {
        HMODULE gfx = GetModuleHandleA("D2gfx.dll");
        if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
        if (!gfx) return;
        draw = (gfx_draw6_fn)GetProcAddress(gfx, MAKEINTRESOURCEA(10014));
        log_line("draw: D2gfx #10014 DrawRectangle at %p", (void *)draw);
        if (!draw) return;
    }
    /* A spot in the world, chosen once: wherever the player was standing the first time this
       ran. It should stay put on the ground from then on. */
    static int anchor_x, anchor_y, anchored;
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return;
    if (!anchored) {
        get_player_fn get_player = (get_player_fn)((const BYTE *)client + OFF_GETPLAYERUNIT);
        get_coord_fn get_x = (get_coord_fn)((const BYTE *)client + OFF_GETUNITX);
        get_coord_fn get_y = (get_coord_fn)((const BYTE *)client + OFF_GETUNITY);
        void *player = get_player();
        if (!player) return;
        anchor_x = get_x(player); anchor_y = get_y(player);
        anchored = 1;
        log_line("draw: anchored to world (%d, %d) — the box should stay there", anchor_x, anchor_y);
    }
    int sx, sy;
    if (!world_to_screen(anchor_x, anchor_y, &sx, &sy)) return;
    draw(sx - 20, sy - 60, sx + 20, sy, 0x9A, 5);
}

/* ---------------------------------------------------------------------------------------- */
/* The server's side. In single player D2Game runs in this same process, and creating an object
   is its job — the client only draws what it is told about. D2Game reaches almost everything
   through D2Common, 716 imports of them, and which one makes an object is written down nowhere.

   So: count them all, take a baseline, do something in game that certainly creates an object —
   casting a town portal — and see which counters moved. A needle hunt becomes a subtraction. */

static void *game_originals[MAX_HOOKS];
static volatile LONG game_counts[MAX_HOOKS];
static LONG game_baseline[MAX_HOOKS];
static WORD game_ordinals[MAX_HOOKS];
static void **game_slots[MAX_HOOKS];
static int game_hooks;
static BYTE *game_thunks;

/* The client's own network layer. In single player the server still talks to the client through
   D2Net, and an object appears on the client because a packet told it to — the client then does
   all of it: allocates the unit, puts it in the room, starts the animation. If we can find that
   packet we can send one ourselves, and never draw a pixel or touch a server internal.
   Twelve imports; the same subtraction narrows them to the one that matters. */
static void *net_originals[16];
static volatile LONG net_counts[16];
static LONG net_baseline[16];
static WORD net_ordinals[16];
static int net_hooks;
static BYTE *net_thunks;

static BOOL net_watch_install(void)
{
    if (net_hooks) return TRUE;
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return FALSE;
    WORD wanted[16];
    int count = collect_ordinals(client, "D2Net.dll", wanted, 16);
    if (!count) return FALSE;
    net_thunks = (BYTE *)VirtualAlloc(NULL, count * 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!net_thunks) return FALSE;
    for (int i = 0; i < count; i++) {
        BYTE *thunk = net_thunks + i * 32;
        write_thunk(thunk, i, &net_counts[i], &net_originals[i]);
        void *previous = NULL;
        void **slot = redirect_ordinal(client, "D2Net.dll", wanted[i], thunk, &previous);
        if (!slot) continue;
        net_originals[i] = previous;
        net_ordinals[i] = wanted[i];
        net_hooks++;
    }
    log_line("net: watching %d of D2Client's %d calls into D2Net", net_hooks, count);
    return net_hooks > 0;
}

BOOL server_watch_install(void)
{
    if (game_hooks) return TRUE;
    HMODULE game = GetModuleHandleA("D2Game.dll");
    if (!game) return FALSE;

    static WORD wanted[MAX_HOOKS];
    int count = collect_ordinals(game, "D2Common.dll", wanted, MAX_HOOKS);
    if (!count) { log_line("server: D2Game imports no D2Common ordinals"); return FALSE; }

    /* 32 bytes each, not 16: write_thunk emits twenty-two and the first version let them
       overlap, which turned the thunks into rubbish and crashed the game on entering it. */
    game_thunks = (BYTE *)VirtualAlloc(NULL, count * 32, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!game_thunks) { log_line("server: could not allocate thunks"); return FALSE; }

    for (int i = 0; i < count; i++) {
        BYTE *thunk = game_thunks + i * 32;
        /* Counting only — no call back into us. 716 of these run on the server thread and the
           point is to disturb nothing. */
        write_thunk(thunk, i, &game_counts[i], &game_originals[i]);
        void *previous = NULL;
        void **slot = redirect_ordinal(game, "D2Common.dll", wanted[i], thunk, &previous);
        if (!slot) continue;
        game_originals[i] = previous;
        game_slots[i] = slot;
        game_ordinals[i] = wanted[i];
        game_hooks++;
    }
    log_line("server: watching %d of D2Game's %d calls into D2Common", game_hooks, count);
    return game_hooks > 0;
}

/* A timer window was the wrong instrument: it could not say which of its windows held the portal,
   and it truncated the very one that mattered. The object appearing IS the event, and the plugin
   can see that for itself — so the baseline is re-taken every frame, and the moment the number
   of objects in the world goes up, what moved during THAT frame is printed. No keys, no timing,
   and the answer is one frame wide instead of four seconds. */
void server_on_new_object(int objects_now)
{
    static int objects_before = -1;
    if (!game_hooks && !server_watch_install()) return;
    net_watch_install();

    if (objects_before >= 0 && objects_now > objects_before) {
        log_line("server: an object appeared (%d -> %d). During that frame, D2Game called —",
                 objects_before, objects_now);
        for (int i = 0; i < game_hooks; i++) {
            LONG moved = game_counts[i] - game_baseline[i];
            if (moved > 0) log_line("    D2Common #%u  x%ld", game_ordinals[i], moved);
        }
        for (int i = 0; i < net_hooks; i++) {
            LONG moved = net_counts[i] - net_baseline[i];
            if (moved > 0) log_line("    D2Net #%u  x%ld", net_ordinals[i], moved);
        }
        log_line("server: that is the whole list for that frame");
    }
    objects_before = objects_now;
    for (int i = 0; i < game_hooks; i++) game_baseline[i] = game_counts[i];
    for (int i = 0; i < net_hooks; i++) net_baseline[i] = net_counts[i];
}

void server_baseline(void)
{
    if (!game_hooks && !server_watch_install()) { log_line("server: not watching yet"); return; }
    for (int i = 0; i < game_hooks; i++) game_baseline[i] = game_counts[i];
    log_line("server: baseline taken over %d calls — now do the thing in game", game_hooks);
}

void server_delta(void)
{
    if (!game_hooks) { log_line("server: not watching"); return; }
    log_line("server: what moved since the baseline —");
    int shown = 0;
    /* Smallest movements first: a function called once for one new object is the interesting
       kind, and the ones that tick thousands of times are the game simply running. */
    for (LONG threshold = 1; threshold <= 4 && shown < 40; threshold++) {
        for (int i = 0; i < game_hooks && shown < 40; i++) {
            LONG moved = game_counts[i] - game_baseline[i];
            if (moved != threshold) continue;
            log_line("    D2Common #%u moved by %ld", game_ordinals[i], moved);
            shown++;
        }
    }
    if (!shown) log_line("    nothing moved by four or less");
    log_line("server: done");
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
