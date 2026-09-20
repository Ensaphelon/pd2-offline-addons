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
 * #10019 DrawCellContextEx was tried first and took the game down the moment something was
 * dropped. #10041 is the one d2bs actually ships an image through, arguments and all, and its
 * colour table is the difference: a 256-byte identity table rather than a bare zero where a
 * pointer belongs.
 *
 * Both from SlashDiablo Maphack's D2Ptrs.h (AGPL, so published), and the CellContext shape —
 * frame number at +0x00, the CellFile at +0x34 — from its CommonStructs.h. */

typedef void(__stdcall *init_cell_fn)(void *file, void **out, const char *source, DWORD line,
                                      DWORD version, const char *name);
/* D2Gfx #10041, the call d2bs uses to put an image of its own on the screen:
   (context, x, y, bright2, bright, colour table). The table is 256 bytes and identity leaves the
   art its own colours. Its two brightness arguments are what d2bs passes as -1 and 5. */
typedef void(__stdcall *draw_cell_fn)(void *context, int x, int y, int bright2, int bright,
                                      const BYTE *table);
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

/* Everything the look depends on lives in beam.txt beside the DLL and is re-read while the game
   runs, so trying another blend or nudging the sprite is a text edit and not a rebuild. */
static struct {
    int art;      /* 0 beam, 1 jet, 2 both */
    int trans;    /* the draw's second brightness argument; d2bs passes 5 */
    int bright;   /* its first; d2bs passes -1 */
    int ordinal;  /* which D2Gfx call does the drawing */
    int rate;     /* game frames per sprite frame */
    int dx, dy;   /* nudge, in pixels */
    int anchor;   /* 0 the view offset, 1 relative to the player */
    int test;     /* draw one at a fixed spot on screen, so the art can be judged and the draw
                     proved safe without anything having to be dropped first */
    int on;
} cfg = {0, 5, -1, 10041, 3, 0, 0, 0, 1, 1};

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
            cfg.anchor = !_stricmp(word, "player") ? 1 : 0;
            continue;
        }
        if (sscanf(line, "trans %i", &value) == 1) { cfg.trans = value; continue; }
        if (sscanf(line, "bright %i", &value) == 1) { cfg.bright = value; continue; }
        if (sscanf(line, "draw %i", &value) == 1) { cfg.ordinal = value; continue; }
        if (sscanf(line, "rate %i", &value) == 1) { cfg.rate = value > 0 ? value : 1; continue; }
        if (sscanf(line, "dx %i", &value) == 1) { cfg.dx = value; continue; }
        if (sscanf(line, "dy %i", &value) == 1) { cfg.dy = value; continue; }
        if (sscanf(line, "test %i", &value) == 1) { cfg.test = value; continue; }
        if (sscanf(line, "on %i", &value) == 1) { cfg.on = value; continue; }
    }
    fclose(f);
    log_line("beam: art=%d draw=#%d trans=%d bright=%d rate=%d dx=%d dy=%d anchor=%d test=%d on=%d",
             cfg.art, cfg.ordinal, cfg.trans, cfg.bright, cfg.rate, cfg.dx, cfg.dy,
             cfg.anchor, cfg.test, cfg.on);
}

/* One CellFile per sprite, built the first time it is wanted. The buffer has to stay: InitCellFile
   rewrites it in place into the structure the drawing side walks. */
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

static void draw_sprite(void *cells, int frame, int x, int y)
{
    /* Identity, so every palette index comes out as itself and the art keeps its own colours.
       d2bs keeps two of these and alternates because the renderer caches the pointer; nothing
       here ever changes the table, so one is enough. */
    static BYTE table[256];
    if (!table[1]) for (int i = 0; i < 256; i++) table[i] = (BYTE)i;

    /* CellContext: the frame number at the front, the CellFile at +0x34, zero in between. */
    DWORD context[14];
    if (!cells) return;
    memset(context, 0, sizeof(context));
    context[0] = (DWORD)frame;
    context[13] = (DWORD)(UINT_PTR)cells;
    draw_cell(context, x, y, cfg.bright, cfg.trans, table);
}

/* World to screen.
 *
 * The player-relative version was right until a panel opened: the game slides the whole view
 * sideways to make room and the sprite, pinned to half the screen width, stayed where it was.
 * The view's own origin is a variable — D2Client+0x11C1F8, the same POINT BH converts automap
 * coordinates through, and the automap is drawn in the world's projection, which is why its
 * arithmetic is ours. With the divisor at one it reduces to the transform already proven here:
 * sixteen pixels across per subtile and eight down. */
static int world_to_screen(const BYTE *base, int world_x, int world_y, int *out_x, int *out_y)
{
    if (cfg.anchor == 0) {
        const LONG *offset = (const LONG *)(base + OFF_VIEW_OFFSET);
        int divisor = *(const int *)(base + OFF_VIEW_DIVISOR);
        if (divisor < 1 || divisor > 4) divisor = 1;
        *out_x = ((world_x * 32 - world_y * 32) / 2 / divisor) - (int)offset[0] + 8;
        *out_y = ((world_x * 32 + world_y * 32) / 4 / divisor) - (int)offset[1] - 8;
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

/* x, y is where the light stands: the foot of the sprite. The game is given the top-left corner
   — that is what d2bs subtracts a cell's own offsets from to clip against the screen — so the
   height comes off the y and half the width off the x. */
static void draw_foot_at(int x, int y, int tick)
{
    if (cfg.art == 1 || cfg.art == 2)
        draw_sprite(cells_jet, (tick / cfg.rate) % art_jet_frames,
                    x - art_jet_width / 2, y - art_jet_height);
    if (cfg.art == 0 || cfg.art == 2)
        draw_sprite(cells_beam, (tick / cfg.rate) % art_beam_frames,
                    x - art_beam_width / 2, y - art_beam_height);
}

static void draw_over(const BYTE *base, int world_x, int world_y, int tick)
{
    int x, y;
    if (!world_to_screen(base, world_x, world_y, &x, &y)) return;
    draw_foot_at(x + cfg.dx, y + cfg.dy, tick);
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

    /* Once, at a fixed spot near the left edge, before any item is involved: if the game is
       going to fall over drawing this it should do it on the way in, not when something rare
       has just dropped. It also puts the art on screen next to the game's own graphics, which
       is the only way to judge it. Turn it off with `test 0`. */
    if (cfg.test) {
        static int said;
        if (!said) { said = 1; log_line("beam: drawing the fixed test sprite"); }
        draw_foot_at(160, 400, tick);
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
