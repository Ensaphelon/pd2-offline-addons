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
 *   D2Gfx #10019  DrawCellContextEx(context, x, y, light, transparency, colour)
 *
 * Both from SlashDiablo Maphack's D2Ptrs.h (AGPL, so published), and the CellContext shape —
 * frame number at +0x00, the CellFile at +0x34 — from its CommonStructs.h. */

typedef void(__stdcall *init_cell_fn)(void *file, void **out, const char *source, DWORD line,
                                      DWORD version, const char *name);
typedef void(__stdcall *draw_cell_fn)(void *context, int x, int y, int light, int trans,
                                      int colour);
typedef void *(__stdcall *get_player_fn)(void);
typedef int(__fastcall *get_coord_fn)(void *unit);

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
    int trans;    /* D2's blend level; 0 is solid, 5 blends */
    int colour;   /* palette shift, 0 for the art's own colours */
    int rate;     /* game frames per sprite frame */
    int dx, dy;   /* nudge, in pixels */
    int anchor;   /* 0 the view offset, 1 relative to the player */
    int on;
} cfg = {0, 5, 0, 3, 0, 0, 0, 1};

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
        if (sscanf(line, "colour %i", &value) == 1) { cfg.colour = value; continue; }
        if (sscanf(line, "rate %i", &value) == 1) { cfg.rate = value > 0 ? value : 1; continue; }
        if (sscanf(line, "dx %i", &value) == 1) { cfg.dx = value; continue; }
        if (sscanf(line, "dy %i", &value) == 1) { cfg.dy = value; continue; }
        if (sscanf(line, "on %i", &value) == 1) { cfg.on = value; continue; }
    }
    fclose(f);
    log_line("beam: art=%d trans=%d colour=%d rate=%d dx=%d dy=%d anchor=%d on=%d",
             cfg.art, cfg.trans, cfg.colour, cfg.rate, cfg.dx, cfg.dy, cfg.anchor, cfg.on);
}

/* One CellFile per sprite, built the first time it is wanted. The buffer has to stay: InitCellFile
   rewrites it in place into the structure the drawing side walks. */
static void *make_cells(init_cell_fn init, const unsigned char *blob, unsigned int size,
                        const char *name)
{
    void *buffer = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return NULL;
    memcpy(buffer, blob, size);
    void *cells = buffer;
    init(buffer, &cells, "pd2holygrail", 0, (DWORD)-1, name);
    log_line("beam: %s is %u bytes of DC6, CellFile at %p", name, size, cells);
    return cells;
}

static draw_cell_fn draw_cell;
static void *cells_beam, *cells_jet;

static int resolve(void)
{
    static int tried;
    if (tried) return draw_cell != NULL;
    tried = 1;

    HMODULE gfx = GetModuleHandleA("D2gfx.dll");
    if (!gfx) gfx = GetModuleHandleA("D2Gfx.dll");
    HMODULE cmp = GetModuleHandleA("D2CMP.dll");
    if (!gfx || !cmp) {
        log_line("beam: D2gfx=%p D2CMP=%p — not both loaded yet", (void *)gfx, (void *)cmp);
        tried = 0;
        return 0;
    }
    init_cell_fn init = (init_cell_fn)GetProcAddress(cmp, MAKEINTRESOURCEA(10006));
    draw_cell = (draw_cell_fn)GetProcAddress(gfx, MAKEINTRESOURCEA(10019));
    log_line("beam: D2CMP #10006 at %p, D2gfx #10019 at %p", (void *)init, (void *)draw_cell);
    if (!init || !draw_cell) { draw_cell = NULL; return 0; }

    cells_beam = make_cells(init, art_beam, art_beam_size, "beam");
    cells_jet = make_cells(init, art_jet, art_jet_size, "jet");
    return cells_beam != NULL;
}

static void draw_sprite(void *cells, int frame, int x, int y)
{
    /* CellContext: the frame number at the front, the CellFile at +0x34, zero in between. */
    DWORD context[14];
    memset(context, 0, sizeof(context));
    context[0] = (DWORD)frame;
    context[13] = (DWORD)(UINT_PTR)cells;
    draw_cell(context, x, y, -1, cfg.trans, cfg.colour);
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

static void draw_over(const BYTE *base, int world_x, int world_y, int tick)
{
    int x, y;
    if (!world_to_screen(base, world_x, world_y, &x, &y)) return;
    x += cfg.dx;
    y += cfg.dy;

    /* The sprite's bottom sits on the item and it rises from there, so the anchor is the middle
       of its foot: half a width left, and the y the game is given is the bottom edge. */
    if (cfg.art == 1 || cfg.art == 2)
        draw_sprite(cells_jet, (tick / cfg.rate) % art_jet_frames, x - art_jet_width / 2, y);
    if (cfg.art == 0 || cfg.art == 2)
        draw_sprite(cells_beam, (tick / cfg.rate) % art_beam_frames, x - art_beam_width / 2, y);
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
