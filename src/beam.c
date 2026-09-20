#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "art.h"

/* The light over a drop, drawn with the game's own art.
 *
 * The first version stacked six blended rectangles and looked like a stack of six blended
 * rectangles. This one hands the game a sprite instead: D2Cmp turns our DC6 into a CellFile and
 * D2Gfx draws one frame of it per game frame. Nothing here is renderer-specific — D2Gfx is what
 * D2Client itself draws through, so DDraw, Direct3D, Glide and D2GL all get it, and D2GL's own
 * improvements apply to it exactly as they apply to the game's own effects.
 *
 *   D2Cmp #10006  InitCellFile(buffer, &out, source, line, version, name)
 *   D2Gfx #10041  DrawAutomapCell2(context, x, y, bright2, bright, colour table)
 *
 * The CellContext is bigger than BH and d2bs describe it. D2Cmp's own cell lookup — the function
 * whose assertion the game halts on, at D2CMP+0x122E0 — reads four things and refuses anything
 * else: the cell file at +0x34, its version, which must be 6, a DIRECTION at +0x40, which must be
 * under 64, and the frame number at +0x00, which must not exceed the file's cell count. A context
 * declared as 0x38 bytes leaves that direction off the end of it, reading whatever the stack
 * happened to hold — and a number over 64 there is the halt.
 *
 * Both from SlashDiablo Maphack's D2Ptrs.h (AGPL, so published), and the CellContext shape —
 * frame number at +0x00, the CellFile at +0x34 — from its CommonStructs.h. */

typedef void(__stdcall *init_cell_fn)(void *file, void **out, const char *source, DWORD line,
                                      DWORD version, const char *name);
/* D2Gfx #10019, with the arguments read off the game itself: it draws a 48x48 cell, frame two of
   its cell file, with (context, 117, 600, -1, 5, 1). So the last argument is a small number and
   not a pointer — which is what a 256-byte colour table in that slot got wrong — and the y it is
   given is the BOTTOM of the sprite, since a cell 80 high is drawn at y 600 on a 600-high
   screen. */
typedef void(__stdcall *draw_cell_fn)(void *context, int x, int y, int light, int trans,
                                      int colour);
typedef void *(__stdcall *get_player_fn)(void);
typedef int(__fastcall *get_coord_fn)(void *unit);

/* After InitCellFile the frame offsets in the buffer have become GfxCell pointers. Reading one
   back is how we know the game accepted the file at all — and drawing through a pointer it did
   not accept is exactly the crash that cost a test round. */
typedef struct {
    DWORD flags, width, height, xoffs, yoffs, _pad, parent, length;
} gfx_cell;

/* Reading a live game's memory through ReadProcessMemory rather than testing the pointer first:
   IsBadReadPtr and VirtualQuery-then-read have each taken the game down here already. */
static int safe_read(const void *at, void *into, SIZE_T bytes)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, bytes, &got) && got == bytes;
}

#define OFF_GETPLAYERUNIT 0xA4D60
#define OFF_GETUNITX 0x1630
#define OFF_GETUNITY 0x1660
#define OFF_SCREENSIZEX 0xDBC48
#define OFF_SCREENSIZEY 0xDBC4C
#define OFF_VIEW_OFFSET 0x11C1F8  /* POINT: where the world's origin sits on screen */
#define OFF_VIEW_DIVISOR 0xF16B0
#define OFF_UNIT_TABLE 0x10A608
#define OFF_MOUSEOFFSETY 0x11995C
#define OFF_MOUSEOFFSETX 0x119960
#define OFF_PANELOFFSETX 0x11B9A0
#define OFF_GETMOUSEXOFF 0x3F6C0
#define OFF_GETMOUSEYOFF 0x3F6D0

typedef int(__fastcall *get_offset_fn)(void);

/* Everything the look depends on lives in beam.txt beside the DLL and is re-read while the game
   runs, so trying another blend or nudging the sprite is a text edit and not a rebuild. */
static struct {
    int art;      /* 0 beam, 1 jet, 2 both */
    int trans;    /* the blend. All eight were drawn side by side over grass: 0 is nearly
                     invisible, 3 glows and lets the ground through, 5 is the flat opaque one
                     the game uses for its own panels, and the rest are slabs. Light wants 3. */
    int bright;   /* its first; d2bs passes -1 */
    int ordinal;  /* which D2Gfx call does the drawing */
    int colour;   /* the draw's last argument; the game passes small numbers here */
    int rate;     /* game frames per sprite frame */
    int dx, dy;   /* nudge, in pixels */
    int anchor;   /* 0 the mouse origin, which follows the view when a panel slides it;
                     1 relative to the player, which does not */
    int test;     /* 1 draws one at a fixed spot on screen, so the art can be judged and the
                     draw proved safe without anything having to be dropped first; 2 draws the
                     row of blends below */
    int capture;  /* frames to watch the game's own calls for, looking for the one that draws
                     a cell — set it again to take another look without restarting */
    int trace;    /* print the shape of one frame: which D2gfx call, and how far into the frame */
    int drawon;   /* the ordinal to draw on; 0 means the last call of the frame */
    int mark;     /* a dot at the exact point the transform works out, to see it against the
                     item's own sprite */
    int on;
} cfg = {0, 3, -1, 10019, 0, 4, 0, 0, 0, 0, 0, 1, 0, 1, 1};

static void capture_arm(void);
static void trace_arm(void);
static int world_to_screen(const BYTE *base, int world_x, int world_y, int *out_x, int *out_y);

static char config_path[MAX_PATH];
static FILETIME config_stamp;

void beam_init(void *module)
{
    if (!GetModuleFileNameA((HMODULE)module, config_path, MAX_PATH)) return;
    char *slash = strrchr(config_path, '\\');
    if (slash) strcpy(slash + 1, "beam.txt");
}

void beam_reload(void)
{
    if (!config_path[0]) return;

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(config_path, GetFileExInfoStandard, &info)) return;
    if (CompareFileTime(&info.ftLastWriteTime, &config_stamp) == 0) return;
    config_stamp = info.ftLastWriteTime;

    FILE *f = fopen(config_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char word[32];
        int value;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "art %31s", word) == 1) {
            cfg.art = !_stricmp(word, "jet") ? 1 : !_stricmp(word, "both") ? 2 : 0;
            continue;
        }
        if (sscanf(line, "anchor %31s", word) == 1) {
            cfg.anchor = !_stricmp(word, "player") ? 1 : 0;   /* anything else: mouse */
            continue;
        }
        if (sscanf(line, "trans %i", &value) == 1) { cfg.trans = value; continue; }
        if (sscanf(line, "bright %i", &value) == 1) { cfg.bright = value; continue; }
        if (sscanf(line, "draw %i", &value) == 1) { cfg.ordinal = value; continue; }
        if (sscanf(line, "colour %i", &value) == 1) { cfg.colour = value; continue; }
        if (sscanf(line, "rate %i", &value) == 1) { cfg.rate = value > 0 ? value : 1; continue; }
        if (sscanf(line, "dx %i", &value) == 1) { cfg.dx = value; continue; }
        if (sscanf(line, "dy %i", &value) == 1) { cfg.dy = value; continue; }
        if (sscanf(line, "test %i", &value) == 1) { cfg.test = value; continue; }
        if (sscanf(line, "capture %i", &value) == 1) { cfg.capture = value; capture_arm(); continue; }
        if (sscanf(line, "trace %i", &value) == 1) { cfg.trace = value; trace_arm(); continue; }
        if (sscanf(line, "drawon %i", &value) == 1) { cfg.drawon = value; continue; }
        if (sscanf(line, "mark %i", &value) == 1) { cfg.mark = value; continue; }
        if (sscanf(line, "on %i", &value) == 1) { cfg.on = value; continue; }
    }
    fclose(f);
    log_line("beam: art=%d draw=#%d drawon=#%d trans=%d bright=%d colour=%d rate=%d dx=%d dy=%d "
             "anchor=%d test=%d mark=%d capture=%d trace=%d on=%d", cfg.art, cfg.ordinal,
             cfg.drawon, cfg.trans, cfg.bright, cfg.colour, cfg.rate, cfg.dx, cfg.dy, cfg.anchor,
             cfg.test, cfg.mark, cfg.capture, cfg.trace, cfg.on);
}

/* One CellFile per sprite, built the first time it is wanted. The buffer has to stay: InitCellFile
   rewrites it in place into the structure the drawing side walks. */
/* Which D2gfx call actually puts a sprite on the screen.
 *
 * Two published candidates were tried by hand: #10019 took the game down on the first drop and
 * #10041 returned without drawing anything. The game itself makes hundreds of these calls a
 * frame, so rather than guess a third time, every hooked call is looked at for a short while and
 * the ones whose first argument is a CellContext — a pointer whose +0x34 leads to a cell file
 * whose first cell has a sensible width and height — are printed with all six of their real
 * arguments. That is the call to imitate, and those are the values to imitate it with. */
static volatile LONG capture_left;
static BYTE captured[256];

static void capture_arm(void)
{
    memset(captured, 0, sizeof(captured));
    capture_left = cfg.capture;
}

int beam_capturing(void)
{
    return capture_left > 0;
}

void beam_inspect(int ordinal, const DWORD *args)
{
    int slot = ordinal - 10000;
    if (capture_left <= 0 || slot < 0 || slot >= 256 || captured[slot]) return;

    DWORD a[6];
    DWORD context, file, version = 0, first = 0, frame = 0;
    gfx_cell cell;

    /* args points at the return address the caller pushed; the arguments follow it. */
    if (!safe_read(args + 1, a, sizeof(a))) return;
    context = a[0];
    if (context < 0x10000 || (context & 3)) return;             /* cheap, and rejects almost all */
    if (!safe_read((const void *)(UINT_PTR)(context + 0x34), &file, 4)) return;
    if (file < 0x10000 || (file & 3)) return;
    if (!safe_read((const void *)(UINT_PTR)file, &version, 4)) return;
    if (!safe_read((const void *)(UINT_PTR)(file + 0x18), &first, 4)) return;
    if (first < 0x10000 || !safe_read((const void *)(UINT_PTR)first, &cell, sizeof(cell))) return;
    if (cell.width < 1 || cell.width > 4096 || cell.height < 1 || cell.height > 4096) return;
    safe_read((const void *)(UINT_PTR)context, &frame, 4);

    captured[slot] = 1;
    log_line("capture: D2gfx #%d(%#lx, %ld, %ld, %#lx, %#lx, %#lx)  cell %lux%lu  frame %lu  "
             "file version %lu", ordinal, (unsigned long)a[0], (long)a[1], (long)a[2],
             (unsigned long)a[3], (unsigned long)a[4], (unsigned long)a[5],
             (unsigned long)cell.width, (unsigned long)cell.height, (unsigned long)frame,
             (unsigned long)version);
}

/* The shape of one frame.
 *
 * Drawing on the frame's LAST call puts the light over everything the game drew after the world:
 * the item's own name plate, and the mouse cursor. It also jitters while the character walks,
 * because by then the view has already been moved on for the next frame. All three want the same
 * thing — an earlier place in the frame — and which place that is, is a question about the order
 * the game does its drawing in.
 *
 * Eleven thousand calls a frame is too many to print, but almost all of them are runs of the same
 * ordinal, so only the changes are recorded: which call, and how many calls into the frame it
 * came. That is the frame's structure on one screen. */
static volatile LONG trace_left;
static struct { WORD ordinal; DWORD at; } trace_seen[160];
static int trace_count;
static DWORD trace_calls;

static void trace_arm(void)
{
    trace_count = 0;
    trace_calls = 0;
    trace_left = cfg.trace;
}

int beam_tracing(void)
{
    return trace_left > 0;
}

int beam_draw_on(void)
{
    return cfg.drawon;
}

void beam_trace(int ordinal)
{
    trace_calls++;
    if (trace_count && trace_seen[trace_count - 1].ordinal == (WORD)ordinal) return;
    if (trace_count >= (int)(sizeof(trace_seen) / sizeof(trace_seen[0]))) return;
    trace_seen[trace_count].ordinal = (WORD)ordinal;
    trace_seen[trace_count].at = trace_calls;
    trace_count++;
}

static void trace_report(void)
{
    char line[256];
    int used = 0;

    if (trace_left <= 0) return;
    trace_left = 0;
    log_line("trace: one frame, %lu calls — ordinal@how-far-in", (unsigned long)trace_calls);
    for (int i = 0; i < trace_count; i++) {
        int wrote = snprintf(line + used, sizeof(line) - used, "%s#%u@%lu",
                             used ? "  " : "    ", trace_seen[i].ordinal,
                             (unsigned long)trace_seen[i].at);
        if (wrote < 0 || used + wrote >= (int)sizeof(line) - 1) {
            line[used] = 0;
            log_line("%s", line);
            used = 0;
            i--;
            continue;
        }
        used += wrote;
    }
    if (used) { line[used] = 0; log_line("%s", line); }
}

static void *make_cells(init_cell_fn init, const unsigned char *blob, unsigned int size,
                        int width, int height, const char *name)
{
    void *buffer = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return NULL;
    memcpy(buffer, blob, size);
    void *cells = buffer;
    init(buffer, &cells, "pd2holygrail", 0, (DWORD)-1, name);

    /* The cell pointers live at +0x18; before init they are offsets into the file, so a small
       number here means the game did not take the file and nothing may be drawn from it. */
    DWORD first = 0;
    gfx_cell cell;
    if (!safe_read((const BYTE *)cells + 0x18, &first, sizeof(first)) || first < 0x10000 ||
        !safe_read((const void *)(UINT_PTR)first, &cell, sizeof(cell))) {
        log_line("beam: %s was not accepted by InitCellFile (first cell reads as %#lx)",
                 name, (unsigned long)first);
        return NULL;
    }
    DWORD version = 0, count = 0;
    safe_read(cells, &version, 4);
    safe_read((const BYTE *)cells + 0x14, &count, 4);
    if (version != 6 || count < 1) {
        log_line("beam: %s reads as version %lu with %lu cells — D2Cmp wants version 6",
                 name, (unsigned long)version, (unsigned long)count);
        return NULL;
    }
    if ((int)cell.width != width || (int)cell.height != height) {
        log_line("beam: %s came back %lux%lu, expected %dx%d — not drawing it",
                 name, (unsigned long)cell.width, (unsigned long)cell.height, width, height);
        return NULL;
    }
    log_line("beam: %s is %u bytes of DC6, first cell %lux%lu at %#lx", name, size,
             (unsigned long)cell.width, (unsigned long)cell.height, (unsigned long)first);
    return cells;
}

static draw_cell_fn draw_cell;
static void *cells_beam, *cells_jet;

static int resolve(void)
{
    static int tried, ordinal;
    if (tried && ordinal == cfg.ordinal) return draw_cell != NULL && cells_beam != NULL;

    HMODULE gfx = GetModuleHandleA("D2gfx.dll");
    if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
    HMODULE cmp = GetModuleHandleA("D2CMP.dll");
    if (!gfx || !cmp) return 0;

    tried = 1;
    ordinal = cfg.ordinal;
    draw_cell = (draw_cell_fn)GetProcAddress(gfx, MAKEINTRESOURCEA(cfg.ordinal));
    log_line("beam: D2gfx #%d at %p", cfg.ordinal, (void *)draw_cell);
    if (!draw_cell) return 0;

    if (!cells_beam && !cells_jet) {
        init_cell_fn init = (init_cell_fn)GetProcAddress(cmp, MAKEINTRESOURCEA(10006));
        log_line("beam: D2CMP #10006 at %p", (void *)init);
        if (!init) { draw_cell = NULL; return 0; }
        cells_beam = make_cells(init, art_beam, art_beam_size,
                                art_beam_width, art_beam_height, "beam");
        cells_jet = make_cells(init, art_jet, art_jet_size,
                               art_jet_width, art_jet_height, "jet");
    }
    return cells_beam != NULL || cells_jet != NULL;
}

#define CTX_CELL 0      /* the frame to draw */
#define CTX_FILE 13     /* +0x34, the cell file */
#define CTX_DIR 16      /* +0x40, the direction — off the end of a 0x38-byte context */

static void draw_sprite(void *cells, int frame, int x, int y)
{
    /* Generous and entirely zeroed: the fields D2Cmp checks are all happy at zero, and the ones
       nobody has named are better zero than whatever was on the stack. */
    DWORD context[64];
    if (!cells) return;
    /* A coordinate the transform got wrong is the likeliest way to take the game down from here:
       a rectangle at an absurd place is simply clipped, a sprite is not. */
    if (x < -4096 || x > 8192 || y < -4096 || y > 8192) return;
    memset(context, 0, sizeof(context));
    context[CTX_CELL] = (DWORD)frame;
    context[CTX_FILE] = (DWORD)(UINT_PTR)cells;
    context[CTX_DIR] = 0;
    draw_cell(context, x, y, cfg.bright, cfg.trans, cfg.colour);
}

/* World to screen, settled by measurement rather than by a header.
 *
 * With the player at world 3993,5228 on a 1068x600 screen the transform needs an origin of
 * -20294,73468, and `GetMouseXOffset` returns exactly -20294. That is no coincidence: it is the
 * origin the game itself converts the mouse through, so it is the one that already knows the
 * view has slid. Opening the inventory moves it to -20027 — a quarter of the screen width, 267
 * pixels, which is precisely how far the world shifts to make room.
 *
 * The y wants a constant 24 on top: `GetMouseYOffset` reads 73492 where 73468 is needed. The
 * variables at +0x119960 and +0x11995C hold the same pair but do NOT move when a panel opens,
 * which is what made them the wrong answer and the functions the right one.
 *
 * D2Client+0x11C1F8, BH's automap origin, is not this: it reads 0,0 here. */
static int world_to_screen(const BYTE *base, int world_x, int world_y, int *out_x, int *out_y)
{
    if (cfg.anchor == 0) {
        get_offset_fn mouse_x = (get_offset_fn)(base + OFF_GETMOUSEXOFF);
        get_offset_fn mouse_y = (get_offset_fn)(base + OFF_GETMOUSEYOFF);
        *out_x = (world_x - world_y) * 16 - mouse_x();
        *out_y = (world_x + world_y) * 8 - mouse_y() + 24;
        return 1;
    }

    get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
    get_coord_fn get_x = (get_coord_fn)(base + OFF_GETUNITX);
    get_coord_fn get_y = (get_coord_fn)(base + OFF_GETUNITY);
    void *player = get_player();
    if (!player) return 0;
    int px = get_x(player), py = get_y(player);
    DWORD width = *(const DWORD *)(base + OFF_SCREENSIZEX);
    DWORD height = *(const DWORD *)(base + OFF_SCREENSIZEY);
    if (width < 320 || width > 4096) width = 800;
    if (height < 200 || height > 4096) height = 600;
    *out_x = (int)(width / 2) + (world_x - px - (world_y - py)) * 16;
    *out_y = (int)(height / 2) + (world_x - px + (world_y - py)) * 8;
    return 1;
}

/* x, y is where the light stands: the foot of the sprite. The game is given the bottom edge and
   the left one, so only half a width comes off the x. */
static void draw_foot_at(int x, int y, int tick)
{
    if (cfg.art == 1 || cfg.art == 2)
        draw_sprite(cells_jet, (tick / cfg.rate) % art_jet_frames, x - art_jet_width / 2, y);
    if (cfg.art == 0 || cfg.art == 2)
        draw_sprite(cells_beam, (tick / cfg.rate) % art_beam_frames, x - art_beam_width / 2, y);
}

/* Where the view's origin actually lives.
 *
 * D2Client+0x11C1F8 was taken for it on BH's word and is not: with the player at world 3993,5228
 * on a 1068x600 screen it read 0,0 and its divisor read 20, which put the light twenty thousand
 * pixels off. So instead of guessing a second variable, the number that is NEEDED is worked out —
 * with nothing open the player is at the middle of the screen, so the offset is simply the world
 * pixel minus half the screen — and printed beside every candidate BH names. Logged again
 * whenever any of them moves, which is what opening a panel does, so one session answers both
 * halves of the question. */
static void report_transform(const BYTE *base)
{
    static DWORD last_at;
    static int last[5];
    static int lines;

    if (lines > 14) return;
    if (last_at && GetTickCount() - last_at < 1000) return;

    get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
    void *player = get_player();
    if (!player) return;
    get_coord_fn get_x = (get_coord_fn)(base + OFF_GETUNITX);
    get_coord_fn get_y = (get_coord_fn)(base + OFF_GETUNITY);
    get_offset_fn mouse_x = (get_offset_fn)(base + OFF_GETMOUSEXOFF);
    get_offset_fn mouse_y = (get_offset_fn)(base + OFF_GETMOUSEYOFF);

    int now[5];
    now[0] = *(const int *)(base + OFF_MOUSEOFFSETX);
    now[1] = *(const int *)(base + OFF_MOUSEOFFSETY);
    now[2] = *(const int *)(base + OFF_PANELOFFSETX);
    now[3] = mouse_x();
    now[4] = mouse_y();

    int same = last_at != 0;
    for (int i = 0; i < 5; i++) if (now[i] != last[i]) same = 0;
    if (same) return;
    for (int i = 0; i < 5; i++) last[i] = now[i];
    last_at = GetTickCount();
    lines++;

    int px = get_x(player), py = get_y(player);
    int width = (int)*(const DWORD *)(base + OFF_SCREENSIZEX);
    int height = (int)*(const DWORD *)(base + OFF_SCREENSIZEY);
    log_line("origin: needs %d,%d (player %d,%d on %dx%d) — MouseOffset %d,%d  PanelOffsetX %d  "
             "GetMouseOffset %d,%d",
             (px - py) * 16 - width / 2, (px + py) * 8 - height / 2,
             px, py, width, height, now[0], now[1], now[2], now[3], now[4]);
}

typedef void(__stdcall *draw_rect_fn)(int, int, int, int, int, int);

static void draw_over(const BYTE *base, int world_x, int world_y, int tick)
{
    int x, y;
    if (!world_to_screen(base, world_x, world_y, &x, &y)) return;
    x += cfg.dx;
    y += cfg.dy;
    draw_foot_at(x, y, tick);

    /* Where the transform says the item is, as a solid dot, so it can be judged against the
       item's own sprite rather than against its name plate. */
    if (cfg.mark) {
        static draw_rect_fn rect;
        if (!rect) {
            HMODULE gfx = GetModuleHandleA("D2gfx.dll");
            if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
            if (gfx) rect = (draw_rect_fn)GetProcAddress(gfx, MAKEINTRESOURCEA(10014));
        }
        if (rect) rect(x - 2, y - 2, x + 3, y + 3, 0xFF, 0);
    }
}

/* Every item lying on the ground, out of the client's own unit table: six rows of 128 buckets,
   items being row four, each bucket a list linked through +0xEC. */
void beam_draw(void)
{
    static int tick;
    if (!cfg.on || !resolve()) return;
    HMODULE client = GetModuleHandleA("D2Client.dll");
    if (!client) return;
    tick++;
    if (tick == 1) { capture_arm(); trace_arm(); }
    if (tick > 200) trace_report();
    report_transform((const BYTE *)client);
    if (capture_left > 0 && !--capture_left) log_line("capture: done looking");

    /* Once, at a fixed spot near the left edge, before any item is involved: if the game is
       going to fall over drawing this it should do it on the way in, not when something rare
       has just dropped. It also puts the art on screen next to the game's own graphics, which
       is the only way to judge it. Turn it off with `test 0`. */
    if (cfg.test) {
        static int said;
        if (!said) { said = 1; log_line("beam: drawing the fixed test sprite"); }
        if (cfg.test >= 2) {
            /* Every blend the draw takes, side by side, left to right. Which number makes this
               art look like light rather than a black slab is a question a screenshot answers in
               one go, and eight guesses answer in eight sessions. The art is 81% near-black, so
               it is drawn to be added to what is behind it, not laid over it. */
            int was = cfg.trans;
            for (int blend = 0; blend < 8; blend++) {
                cfg.trans = blend;
                draw_foot_at(120 + blend * 80, 460, tick);
            }
            cfg.trans = was;
        } else {
            draw_foot_at(160, 400, tick);
        }
        if (said == 1) { said = 2; log_line("beam: the draw returned — it is safe"); }
    }

    const BYTE *base = (const BYTE *)client;
    const BYTE *row = base + OFF_UNIT_TABLE + 4 * (128 * 4);
    get_coord_fn get_x = (get_coord_fn)(base + OFF_GETUNITX);
    get_coord_fn get_y = (get_coord_fn)(base + OFF_GETUNITY);

    for (int bucket = 0; bucket < 128; bucket++) {
        DWORD address = *(const DWORD *)(row + bucket * 4);
        int depth = 0;
        while (address && depth++ < 64) {
            const DWORD *unit = (const DWORD *)(UINT_PTR)address;
            if (unit[0] != 4) break;      /* dwType: item */
            DWORD mode = unit[4];         /* dwMode: 3 is lying on the ground */
            DWORD item_data = unit[5];
            if (mode == 3 && item_data) {
                DWORD quality = *(const DWORD *)(UINT_PTR)item_data;
                if (quality == 5 || quality == 7)   /* set, unique */
                    draw_over(base, get_x((void *)unit), get_y((void *)unit), tick);
            }
            address = unit[0x3B];         /* +0xEC, the next in this bucket */
        }
    }
}
