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
#define UNIT_PATH 0x2C   /* and a player's path opens with x then y, each 16.16 fixed */
#define OFF_MOUSEOFFSETY 0x11995C
#define OFF_MOUSEOFFSETX 0x119960
#define OFF_PANELOFFSETX 0x11B9A0
#define OFF_GETMOUSEXOFF 0x3F6C0
#define OFF_GETMOUSEYOFF 0x3F6D0
#define OFF_MOUSEY 0x11B824
#define OFF_MOUSEX 0x11B828
#define OFF_HOVERX 0xE0EB8       /* where the game puts the text for what is under the cursor */
#define OFF_HOVERY 0xE0EBC
#define OFF_GETSELECTED 0x51A80

typedef int(__fastcall *get_offset_fn)(void);
typedef void *(__stdcall *get_selected_fn)(void);

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
    int anchor;   /* 0 the player's exact position plus the view's slide, 1 the mouse origin
                     alone, 2 the player's rounded position alone */
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
} cfg = {0, 3, -1, 10019, 0, 4, 0, 0, 0, 0, 0, 1, 0, 10054, 1};

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
            cfg.anchor = !_stricmp(word, "player") ? 2 : !_stricmp(word, "mouse") ? 1 : 0;
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
 * came. The first attempt kept the FIRST hundred and sixty of those and spent every one of them
 * inside the floor-tile loop, where #10076 and #10023 alternate for thousands of calls. So it
 * keeps the last hundred and sixty instead — the end of the frame is the part in question. */
static volatile LONG trace_left;
static DWORD trace_first[128], trace_last[128], trace_hits[128];
static DWORD trace_calls;
static int trace_pending;

static void trace_arm(void)
{
    trace_pending = cfg.trace > 0;
}

int beam_tracing(void)
{
    return trace_left > 0;
}

int beam_draw_on(void)
{
    return cfg.drawon;
}

/* First call, last call and how many, per ordinal. Recording every turn instead filled the
   buffer with the floor-tile loop and then, keeping the tail, with the panel at the end; what
   the question actually needs is where in the frame each call LIVES, and fifty-eight of those
   fit on a screen where twelve thousand turns never will. */
void beam_trace(int ordinal)
{
    int slot = ordinal - 10000;
    trace_calls++;
    if (slot < 0 || slot >= 128) return;
    if (!trace_hits[slot]) trace_first[slot] = trace_calls;
    trace_last[slot] = trace_calls;
    trace_hits[slot]++;
}

static void trace_report(void)
{
    char line[220];
    int used = 0, left = 1;

    log_line("trace: one frame of %lu calls — ordinal first..last xhowmany, in order",
             (unsigned long)trace_calls);
    while (left) {
        DWORD best = 0xFFFFFFFF;
        int pick = -1;
        left = 0;
        for (int i = 0; i < 128; i++) {
            if (!trace_hits[i]) continue;
            left = 1;
            if (trace_first[i] < best) { best = trace_first[i]; pick = i; }
        }
        if (pick < 0) break;
        int wrote = snprintf(line + used, sizeof(line) - used, "%s#%d %lu..%lu x%lu",
                             used ? "   " : "    ", 10000 + pick,
                             (unsigned long)trace_first[pick], (unsigned long)trace_last[pick],
                             (unsigned long)trace_hits[pick]);
        trace_hits[pick] = 0;
        if (wrote < 0 || used + wrote >= (int)sizeof(line) - 1) {
            line[used] = 0;
            log_line("%s", line);
            used = 0;
            trace_hits[pick] = 1;      /* put it back and start a fresh line */
            trace_first[pick] = best;
            continue;
        }
        used += wrote;
    }
    if (used) { line[used] = 0; log_line("%s", line); }
}

/* One frame exactly: beam_draw runs once a frame, so arming on one visit and reporting on the
   next is a frame, and the first attempt — which stopped after two hundred of them — is why the
   count read two and a half million. */
static void trace_step(void)
{
    if (trace_left > 0) { trace_left = 0; trace_report(); trace_pending = 0; return; }
    if (!trace_pending) return;
    memset(trace_hits, 0, sizeof(trace_hits));
    trace_calls = 0;
    trace_left = 1;
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

/* Where the player really is, to the fraction of a subtile.
 *
 * `GetUnitX` truncates, and everything built on it inherits 16-pixel stairs. The path a player
 * walks along keeps the fraction: x then y, each a 16.16 fixed-point number, at the front of the
 * structure UnitAny+0x2C points at. */
static int player_precise(const BYTE *base, int *out_x, int *out_y)
{
    get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
    void *player = get_player();
    DWORD path = 0, x = 0, y = 0;

    if (!player) return 0;
    if (!safe_read((const BYTE *)player + UNIT_PATH, &path, 4) || path < 0x10000) return 0;
    if (!safe_read((const void *)(UINT_PTR)path, &x, 4)) return 0;
    if (!safe_read((const void *)(UINT_PTR)(path + 4), &y, 4)) return 0;
    *out_x = (int)x;
    *out_y = (int)y;
    return 1;
}

/* World to screen.
 *
 * The camera is centred on the player and the ground is isometric: a subtile is sixteen pixels
 * across and eight down. What took three goes to get right is WHICH player position to measure
 * from.
 *
 * `GetMouseXOffset` looked exact — it matched to the unit against a standing player — but it is
 * built from the truncated position, so it moves in sixteen-pixel steps while the camera glides.
 * The proof is in one log: for an item that never moved, the computed screen x read 486, then
 * 518, then 550, purely because the player walked. The player's own cross never wandered,
 * because both sides of that sum were rounded the same way.
 *
 * So the fraction is read straight off the player's path, and the only thing left for the mouse
 * offset is what it alone knows: how far the view has slid to make room for an open panel. That
 * is the difference between the function and the variable behind it, which does not move.
 *
 * D2Client+0x11C1F8, BH's automap origin, is none of this: it reads 0,0 here. */
static int world_to_screen(const BYTE *base, int world_x, int world_y, int *out_x, int *out_y)
{
    get_offset_fn mouse_x = (get_offset_fn)(base + OFF_GETMOUSEXOFF);
    get_offset_fn mouse_y = (get_offset_fn)(base + OFF_GETMOUSEYOFF);
    int width = (int)*(const DWORD *)(base + OFF_SCREENSIZEX);
    int height = (int)*(const DWORD *)(base + OFF_SCREENSIZEY);
    int px = 0, py = 0;

    if (width < 320 || width > 4096) width = 800;
    if (height < 200 || height > 4096) height = 600;

    if (cfg.anchor == 1) {
        *out_x = (world_x - world_y) * 16 - mouse_x();
        *out_y = (world_x + world_y) * 8 - mouse_y() + 24;
        return 1;
    }

    if (cfg.anchor == 0 && player_precise(base, &px, &py)) {
        long long dx = ((long long)world_x << 16) - px;
        long long dy = ((long long)world_y << 16) - py;
        int slid_x = mouse_x() - *(const int *)(base + OFF_MOUSEOFFSETX);
        int slid_y = mouse_y() - *(const int *)(base + OFF_MOUSEOFFSETY);
        *out_x = width / 2 + (int)(((dx - dy) * 16) >> 16) - slid_x;
        *out_y = height / 2 + (int)(((dx + dy) * 8) >> 16) - slid_y;
        return 1;
    }

    {
        get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
        get_coord_fn get_x = (get_coord_fn)(base + OFF_GETUNITX);
        get_coord_fn get_y = (get_coord_fn)(base + OFF_GETUNITY);
        void *player = get_player();
        if (!player) return 0;
        int ix = get_x(player), iy = get_y(player);
        *out_x = width / 2 + (world_x - ix - (world_y - iy)) * 16;
        *out_y = height / 2 + (world_x - ix + (world_y - iy)) * 8;
    }
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

/* A cross, in the colour and blend that have shown up on screen every time they were used. The
   first marker was a five-pixel box in colour 255 at blend 0 and was not visible at all. */
static void mark_spot(int x, int y)
{
    static draw_rect_fn rect;
    if (!rect) {
        HMODULE gfx = GetModuleHandleA("D2gfx.dll");
        if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
        if (gfx) rect = (draw_rect_fn)GetProcAddress(gfx, MAKEINTRESOURCEA(10014));
        if (!rect) return;
    }
    rect(x - 9, y - 1, x + 10, y + 2, 0x9A, 5);
    rect(x - 1, y - 9, x + 2, y + 10, 0x9A, 5);
}

/* The player's cross sits on the player's feet, so the transform is sound and whatever is wrong
   is in the item's own coordinates. Eyes and a name plate cannot measure that — the plate is the
   loot filter's and is not promised to be centred on anything — so the numbers are printed and
   the test is to stand ON the item, where the two sets must agree. */
static void report_item(const BYTE *base, const void *unit, int ix, int iy, int isx, int isy)
{
    static DWORD last_at;
    get_player_fn get_player = (get_player_fn)(base + OFF_GETPLAYERUNIT);
    get_selected_fn get_selected = (get_selected_fn)(base + OFF_GETSELECTED);
    void *player, *hovered;
    int px16 = 0, py16 = 0, mx, my, hx, hy;

    if (last_at && GetTickCount() - last_at < 1000) return;

    /* The only honest ground truth for where the game DREW an item is the game. Point the cursor
       at it and D2 says so itself: the unit under the cursor, and the place it puts the text for
       it. Screenshots and name plates cannot measure a transform; this can. */
    hovered = get_selected();
    if (hovered != unit) return;

    last_at = GetTickCount();
    player = get_player();
    if (!player || !player_precise(base, &px16, &py16)) return;
    mx = (int)*(const DWORD *)(base + OFF_MOUSEX);
    my = (int)*(const DWORD *)(base + OFF_MOUSEY);
    hx = (int)*(const DWORD *)(base + OFF_HOVERX);
    hy = (int)*(const DWORD *)(base + OFF_HOVERY);

    log_line("hover: item %d,%d  player %d.%05d,%d.%05d  ours %d,%d  mouse %d,%d  hover %d,%d",
             ix, iy, px16 >> 16, (px16 & 0xFFFF) * 100000 / 65536,
             py16 >> 16, (py16 & 0xFFFF) * 100000 / 65536, isx, isy, mx, my, hx, hy);
}

static void draw_over(const BYTE *base, const void *unit, int world_x, int world_y, int tick)
{
    int x, y;
    if (!world_to_screen(base, world_x, world_y, &x, &y)) return;
    if (cfg.mark) report_item(base, unit, world_x, world_y, x, y);
    x += cfg.dx;
    y += cfg.dy;
    draw_foot_at(x, y, tick);

    /* Where the transform says the item is, as a solid dot, so it can be judged against the
       item's own sprite rather than against its name plate. */
    if (cfg.mark) mark_spot(x, y);
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
    if (tick == 1) capture_arm();
    if (tick == 120) trace_arm();
    trace_step();
    report_transform((const BYTE *)client);
    if (capture_left > 0 && !--capture_left) log_line("capture: done looking");

    /* Once, at a fixed spot near the left edge, before any item is involved: if the game is
       going to fall over drawing this it should do it on the way in, not when something rare
       has just dropped. It also puts the art on screen next to the game's own graphics, which
       is the only way to judge it. Turn it off with `test 0`. */
    /* The player is drawn where the game puts it, so a cross at the player's own computed
       position says whether the transform is sound, without anything having to be dropped. */
    if (cfg.mark >= 2) {
        get_player_fn get_player = (get_player_fn)((const BYTE *)client + OFF_GETPLAYERUNIT);
        void *player = get_player();
        if (player) {
            get_coord_fn gx = (get_coord_fn)((const BYTE *)client + OFF_GETUNITX);
            get_coord_fn gy = (get_coord_fn)((const BYTE *)client + OFF_GETUNITY);
            int x, y;
            if (world_to_screen((const BYTE *)client, gx(player), gy(player), &x, &y))
                mark_spot(x, y);
        }
    }

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
                    draw_over(base, unit, get_x((void *)unit), get_y((void *)unit), tick);
            }
            address = unit[0x3B];         /* +0xEC, the next in this bucket */
        }
    }
}
