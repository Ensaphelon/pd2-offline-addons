#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d2.h"
#include "log.h"

/* Craft from the item: the server half of the item context menu.
 *
 * The user's wish (2026-11-26): point at an item, pick a recipe, and have the crafted item lie
 * where the magic one lay, without walking anything into the Horadric Cube by hand.
 *
 * What PD2 does, read out of ProjectDiablo.dll (season 14) and a packet log of a manual craft:
 *
 *   * Transmute is PD2's own, +0x3670B0(game, player), __stdcall. Its inputs are every item of
 *     the player's on inventory page 3 (the cube); it does not ask whether a cube item exists.
 *     The button press (client packet 0x4F, button 0x18) reaches it through PD2's button
 *     handler, which is where "cube or stash open" is checked.
 *   * A material-tab ingredient goes into the cube by client packet 0x42: [WORD material][0][0]
 *     [4][8], material = its row among the misc items (PD2 +0x37F070 adds the weapon and armour
 *     counts to make an ItemsTxt index). That handler wants the stash open and a cube.
 *   * Items move with the game's own 0x19 (buffer -> cursor, [id]) and 0x18 (cursor -> buffer,
 *     [id][x][y][buffer]); buffer 0 inventory, 3 cube, 4 stash — the same numbers an item's
 *     page byte (ItemData+0x45) uses.
 *
 * So the craft runs on the server, as a sequence of the game's own packet handlers called with
 * packets built here: no rule PD2 enforces is skipped except the one button check, and the
 * client hears about every move the way it does when the moves are made by hand.
 *
 * How it gets to the server thread: the client sends packet 0x4F with a button number of ours
 * (0x7A00 + action) and the item's id as the parameter; D2Game's packet table entry for 0x4F is
 * pointed at on_button here, which keeps our buttons and hands every other one to the original.
 *
 * For now the client side is a hotkey — Ctrl+Alt+1..8 over an item — while the menu is built.
 * The recipes come from craft.txt beside the DLL, written from the Craft Center catalog by
 * tools/make_craft_table.py. */

#define CRAFT_BUTTON 0x7A00

/* D2Game.dll, 1.13c. found: the client packet table is {handler, flag} per packet id. */
#define D2GAME_PACKETS     0xFA078
#define D2GAME_ON_BUTTON   0xD1470   /* 0x4F, 7 bytes */
#define D2GAME_ON_PICK     0xD1370   /* 0x19, 5 bytes */
#define D2GAME_ON_PUT      0xCF0E0   /* 0x18, 17 bytes */

/* ProjectDiablo.dll, season 14. */
#define PD2_FIND_UNIT      0x3A1830  /* __fastcall(game, id, type) */
#define PD2_ON_MATERIAL    0x395AC0  /* packet 0x42's handler */
#define PD2_TRANSMUTE      0x3670B0  /* __stdcall(game, player) */
#define PD2_MATERIAL_SLOT  0x37E630  /* __fastcall(player, material) -> its WORD count, or 0 */

/* D2Common.dll, 1.13c (BH). */
#define D2COMMON_ITEM_TEXT 10695

/* D2Client.dll, 1.13c (BH). */
#define D2C_HOVERED_ITEM   0x11BC38  /* SelectedInvItem */

#define UNIT_TYPE      0x00
#define UNIT_TXT       0x04
#define UNIT_ID        0x0C
#define UNIT_DATA      0x14
#define UNIT_PATH      0x2C
#define UNIT_INVENTORY 0x60
#define DATA_PAGE      0x45
#define DATA_NEXT      0x64
#define PATH_X         0x0C
#define PATH_Y         0x10
#define INV_FIRST      0x0C
#define INV_CURSOR     0x20

#define PAGE_INVENTORY 0
#define PAGE_CUBE      3
#define PAGE_STASH     4

typedef int (__fastcall *packet_fn)(void *game, void *player, const BYTE *pkt, DWORD size);
typedef BYTE *(__fastcall *find_unit_fn)(void *game, DWORD id, DWORD type);
typedef void (__stdcall *transmute_fn)(void *game, void *player);
typedef BYTE *(__stdcall *item_text_fn)(DWORD index);
typedef void (__stdcall *send_fn)(DWORD len, DWORD flags, const BYTE *pkt);

static packet_fn real_on_button, on_pick, on_put, on_material;
static find_unit_fn find_unit;
static transmute_fn transmute;
static item_text_fn item_text;
static send_fn send_packet;
static BYTE *pd2;

/* --- craft.txt -------------------------------------------------------------------------------- */

#define MAX_BASES 1024
#define MAX_RECIPES 128
typedef struct { DWORD code; char category[8]; } base_row;
typedef struct { char category[8]; char family[16]; DWORD rune, gem; } recipe_row;
static base_row bases[MAX_BASES];
static recipe_row recipes[MAX_RECIPES];
static int base_count, recipe_count;

/* The actions the hotkeys 1..8 stand for. */
static const char *ACTIONS[] = { NULL, "blood", "caster", "hitpower", "safety", "brilliant",
                                 "bountiful", "vampiric", "corrupt" };
#define ACTION_CORRUPT 8

static DWORD code4(const char *s)
{
    char c[4] = { ' ', ' ', ' ', ' ' };
    for (int i = 0; i < 4 && s[i]; i++) c[i] = s[i];
    DWORD v;
    memcpy(&v, c, 4);
    return v;
}

static void read_table(void)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%scraft.txt", log_dir());
    path[sizeof path - 1] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_line("craft: no craft.txt beside the DLL -- crafting from the item is off");
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char kind[16], a[16], b[16], c[16], d[16];
        int n = sscanf(line, "%15s %15s %15s %15s %15s", kind, a, b, c, d);
        if (n == 3 && !strcmp(kind, "base") && base_count < MAX_BASES) {
            bases[base_count].code = code4(a);
            _snprintf(bases[base_count].category, 8, "%s", b);
            bases[base_count].category[7] = '\0';
            base_count++;
        } else if (n == 5 && !strcmp(kind, "recipe") && recipe_count < MAX_RECIPES) {
            recipe_row *r = &recipes[recipe_count++];
            _snprintf(r->category, 8, "%s", a);
            r->category[7] = '\0';
            _snprintf(r->family, 16, "%s", b);
            r->family[15] = '\0';
            r->rune = code4(c);
            r->gem = code4(d);
        }
    }
    fclose(f);
    log_line("craft: %d bases, %d recipes from craft.txt", base_count, recipe_count);
}

static const char *category_of(DWORD base)
{
    for (int i = 0; i < base_count; i++)
        if (bases[i].code == base) return bases[i].category;
    return NULL;
}

static const recipe_row *recipe_for(const char *category, const char *family)
{
    for (int i = 0; i < recipe_count; i++)
        if (!strcmp(recipes[i].category, category) && !strcmp(recipes[i].family, family))
            return &recipes[i];
    return NULL;
}

/* --- Reading units ---------------------------------------------------------------------------- */

static BYTE *data_of(BYTE *item) { return *(BYTE **)(item + UNIT_DATA); }
static int page_of(BYTE *item) { return data_of(item)[DATA_PAGE]; }
static int x_of(BYTE *item) { return (int)*(DWORD *)(*(BYTE **)(item + UNIT_PATH) + PATH_X); }
static int y_of(BYTE *item) { return (int)*(DWORD *)(*(BYTE **)(item + UNIT_PATH) + PATH_Y); }
static DWORD id_of(BYTE *item) { return *(DWORD *)(item + UNIT_ID); }

static DWORD code_of(BYTE *item)
{
    BYTE *txt = item_text ? item_text(*(DWORD *)(item + UNIT_TXT)) : NULL;
    return txt ? *(DWORD *)(txt + 0x80) : 0;
}

static BYTE *first_item(BYTE *player)
{
    BYTE *inv = *(BYTE **)(player + UNIT_INVENTORY);
    return inv ? *(BYTE **)(inv + INV_FIRST) : NULL;
}

static BYTE *next_item(BYTE *item) { return *(BYTE **)(data_of(item) + DATA_NEXT); }

static void code_text(DWORD code, char out[5])
{
    memcpy(out, &code, 4);
    out[4] = '\0';
    for (int i = 3; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
}

/* The material tab's number for an item code: its row among the misc items, which is its ItemsTxt
   index less that of the first misc item, the elixir (PD2 +0x37F070 adds the weapon and armour
   counts back). Counted in the game's own table, which — unlike a parse of Misc.txt — has no row
   for the "Expansion" marker: Cain's rows past it are one higher. -1 when there is no such item. */
static int items_txt_index(DWORD code)
{
    for (DWORD i = 0; item_text && i < 8192; i++) {
        BYTE *txt = item_text(i);
        if (!txt) break;
        if (*(DWORD *)(txt + 0x80) == code) return (int)i;
    }
    return -1;
}

static int material_of(DWORD code)
{
    /* Only a success is remembered: at install the item tables are not loaded yet. */
    static int first_misc = -1;
    if (first_misc < 0) first_misc = items_txt_index(code4("elx"));
    int index = items_txt_index(code);
    return first_misc >= 0 && index >= first_misc ? index - first_misc : -1;
}

/* --- The game's own handlers, called with packets built here ---------------------------------- */

static int pick(void *game, void *player, DWORD id)
{
    BYTE p[5] = { 0x19 };
    memcpy(p + 1, &id, 4);
    return on_pick(game, player, p, sizeof p);
}

static int put(void *game, void *player, DWORD id, DWORD x, DWORD y, DWORD buffer)
{
    BYTE p[17] = { 0x18 };
    memcpy(p + 1, &id, 4);
    memcpy(p + 5, &x, 4);
    memcpy(p + 9, &y, 4);
    memcpy(p + 13, &buffer, 4);
    return on_put(game, player, p, sizeof p);
}

static int take_material(void *game, void *player, int material)
{
    BYTE p[7] = { 0x42, (BYTE)material, (BYTE)(material >> 8), 0, 0, 4, 8 };
    return on_material(game, player, p, sizeof p);
}

static int count_in_cube(BYTE *player, BYTE **only)
{
    int n = 0;
    for (BYTE *it = first_item(player); it; it = next_item(it))
        if (page_of(it) == PAGE_CUBE) { n++; if (only) *only = it; }
    return n;
}

/* --- Interacting ------------------------------------------------------------------------------
 *
 * The game lets an item be picked from the cube or the stash only while the player is
 * "interacting": unit +0x6C set, with +0x68/+0x64 the type and id of what with — the stash object
 * when it is open, the cube item when the cube was opened with a right click. D2Game +0x588C0
 * looks that unit up; if it is gone it clears the flag and the pick quietly does nothing.
 * (Putting INTO the cube passes without it, because an item on the cursor counts too — which is
 * how the first attempt, 2026-11-26, got an item into a closed cube and could not get it out.)
 *
 * With nothing open, the craft sets the state a right click on the cube would have set, and puts
 * the old one back afterwards. */
#define UNIT_INTERACT_ID   0x64
#define UNIT_INTERACT_TYPE 0x68
#define UNIT_INTERACTING   0x6C

typedef struct { BYTE on; DWORD type, id; } interaction;

static interaction interaction_of(BYTE *player)
{
    interaction i = { player[UNIT_INTERACTING], *(DWORD *)(player + UNIT_INTERACT_TYPE),
                      *(DWORD *)(player + UNIT_INTERACT_ID) };
    return i;
}

static void set_interaction(BYTE *player, interaction i)
{
    *(DWORD *)(player + UNIT_INTERACT_TYPE) = i.type;
    *(DWORD *)(player + UNIT_INTERACT_ID) = i.id;
    player[UNIT_INTERACTING] = i.on;
}

/* The stash is open: the player interacts with an object, and the stash is the one object an item
   can be moved while interacting with. */
static BOOL at_stash(BYTE *player)
{
    return player[UNIT_INTERACTING] && *(DWORD *)(player + UNIT_INTERACT_TYPE) == 2;
}

/* The material tab's handler also wants the stash showing one of its material tabs: PD2 +0x2A6BE0
   accepts the player's current stash tab (player data +0x1B4) only if it is 0x0C..0x10. The
   manual craft switched tabs before each material (client packet 0x55); here the tab is set for
   the length of the craft and put back, like the interaction above. */
#define PDATA_STASH_TAB 0x1B4
#define FIRST_MATERIAL_TAB 0x0C
#define LAST_MATERIAL_TAB 0x10

/* --- Planning: everything is found before anything moves -------------------------------------- */

typedef BYTE *(__fastcall *material_slot_fn)(BYTE *player, DWORD material);
static material_slot_fn material_slot;

typedef struct { DWORD code; BYTE *item; int material; } source;

static int material_count(BYTE *player, int material)
{
    BYTE *slot = material_slot ? material_slot(player, (DWORD)material) : NULL;
    return slot ? *(WORD *)slot : 0;
}

static BOOL plan_source(BYTE *player, DWORD code, BYTE *target, source *taken, int n_taken, source *out)
{
    out->code = code;
    out->item = NULL;
    out->material = -1;
    static const int PAGES[] = { PAGE_INVENTORY, PAGE_CUBE, PAGE_STASH };
    for (int k = 0; k < 3; k++) {
        if (PAGES[k] == PAGE_STASH) {
            /* The material tab before the stash's own items: a stack of counters, not somebody's
               item. Anywhere, not only at the stash (user, 2026-11-26: "I don't want to walk to
               the stash every time") — PD2's handler asks for a material tab, which is set below,
               and not for the stash to be open. The stash's items do need it open. */
            int m = material_of(code);
            if (m >= 0 && material_count(player, m) > 0) { out->material = m; return TRUE; }
            if (!at_stash(player)) break;
        }
        for (BYTE *it = first_item(player); it; it = next_item(it)) {
            if (it == target || page_of(it) != PAGES[k] || code_of(it) != code) continue;
            BOOL used = FALSE;
            for (int j = 0; j < n_taken; j++) used |= taken[j].item == it;
            if (!used) { out->item = it; return TRUE; }
        }
    }
    return FALSE;
}

/* Where the player's items are: inventory, cube, stash, anything else, and the cursor. */
static void census(BYTE *player, char *out, size_t size)
{
    int inv = 0, cube = 0, stash = 0, other = 0;
    for (BYTE *it = first_item(player); it; it = next_item(it)) {
        int pg = page_of(it);
        if (pg == PAGE_INVENTORY) inv++;
        else if (pg == PAGE_CUBE) cube++;
        else if (pg == PAGE_STASH) stash++;
        else other++;
    }
    BYTE *inventory = *(BYTE **)(player + UNIT_INVENTORY);
    BYTE *cursor = inventory ? *(BYTE **)(inventory + INV_CURSOR) : NULL;
    char held[5] = "-";
    if (cursor) code_text(code_of(cursor), held);
    _snprintf(out, size, "inv %d cube %d stash %d other %d cursor '%s'", inv, cube, stash, other, held);
    out[size - 1] = '\0';
}

/* Puts the item back where it was found. */
static BOOL return_item(void *game, BYTE *player, BYTE *item, int page, int x, int y)
{
    DWORD id = id_of(item);
    int a = pick(game, player, id), b = put(game, player, id, (DWORD)x, (DWORD)y, (DWORD)page);
    BOOL home = page_of(item) == page && x_of(item) == x && y_of(item) == y;
    log_line("craft: item %lu back to page %d (%d,%d): pick %d put %d -> page %d (%d,%d)%s",
             (unsigned long)id, page, x, y, a, b, page_of(item), x_of(item), y_of(item),
             home ? "" : "  NOT WHERE IT WAS");
    return home;
}

static void craft_on_server(void *game, BYTE *player, DWORD item_id, int action)
{
    if (action < 1 || action > ACTION_CORRUPT) return;
    BYTE *item = find_unit(game, item_id, 4);
    if (!item || !data_of(item)) { log_line("craft: no item %lu", (unsigned long)item_id); return; }
    BYTE *inv = *(BYTE **)(player + UNIT_INVENTORY);
    if (!inv || *(void **)(inv + INV_CURSOR)) { log_line("craft: something is on the cursor; not now"); return; }
    BYTE *other = NULL;
    int in_cube = count_in_cube(player, &other);
    if (in_cube > 1 || (in_cube == 1 && other != item)) {
        char what[5];
        code_text(other ? code_of(other) : 0, what);
        log_line("craft: the cube is not empty (%d items, e.g. '%s' id %lu); empty it first", in_cube,
                 what, other ? (unsigned long)id_of(other) : 0UL);
        return;
    }

    DWORD base = code_of(item);
    char base_name[5];
    code_text(base, base_name);
    DWORD want[3];
    int n = 0;
    if (action == ACTION_CORRUPT) {
        want[n++] = code4("wss");
    } else {
        const char *category = category_of(base);
        const recipe_row *r = category ? recipe_for(category, ACTIONS[action]) : NULL;
        if (!r) {
            log_line("craft: no %s recipe for '%s' (%s)", ACTIONS[action], base_name,
                     category ? category : "a type craft.txt does not list");
            return;
        }
        want[n++] = code4("jewf");   /* fragments first; a whole jewel might be a good one */
        want[n++] = r->rune;
        want[n++] = r->gem;
    }

    interaction was = interaction_of(player);
    int page = page_of(item), x = x_of(item), y = y_of(item);
    log_line("craft: %s on '%s' (item %lu, page %d at %d,%d); interacting %u with type %lu id %ld",
             ACTIONS[action], base_name, (unsigned long)item_id, page, x, y, was.on,
             (unsigned long)was.type, (long)was.id);
    if (was.on && was.type != 2 && was.type != 4) {
        log_line("craft: busy with something else (a trade, an NPC); close it first");
        return;
    }

    /* Plan. */
    source plan[3];
    for (int i = 0; i < n; i++) {
        if (plan_source(player, want[i], item, plan, i, &plan[i])) continue;
        if (want[i] == code4("jewf") && plan_source(player, code4("jew"), item, plan, i, &plan[i])) continue;
        char name[5];
        code_text(want[i], name);
        log_line("craft: no %s to be had -- nothing was moved", name);
        return;
    }

    BYTE *cube = NULL;
    for (BYTE *it = first_item(player); it && !cube; it = next_item(it))
        if (code_of(it) == code4("box") && page_of(it) != PAGE_CUBE) cube = it;
    if (!cube) { log_line("craft: no Horadric Cube -- nothing was moved"); return; }
    /* PD2 +0x380FB0, which the material tab's handler asks, finds the cube only in the inventory
       — or anywhere while a PD2 flag is up that is up at the stash (2026-11-26: a cube kept in
       the stash worked there and failed in the field, the handler returning 0 untouched). */
    BOOL uses_material = FALSE;
    for (int i = 0; i < n; i++) uses_material |= plan[i].material >= 0;
    if (uses_material && page_of(cube) != PAGE_INVENTORY && !at_stash(player)) {
        log_line("craft: the cube is in the stash; away from it PD2 wants it in the inventory -- nothing was moved");
        return;
    }
    BOOL faked = !was.on;
    if (faked) {
        interaction open = { 1, 4, id_of(cube) };
        set_interaction(player, open);
    }

    BYTE *pdata = *(BYTE **)(player + UNIT_DATA);
    DWORD tab_was = pdata ? *(DWORD *)(pdata + PDATA_STASH_TAB) : 0;
    BOOL tab_set = FALSE;
    for (int i = 0; i < n && pdata; i++) {
        if (plan[i].material >= 0 && (tab_was < FIRST_MATERIAL_TAB || tab_was > LAST_MATERIAL_TAB)) {
            *(DWORD *)(pdata + PDATA_STASH_TAB) = FIRST_MATERIAL_TAB;
            tab_set = TRUE;
            break;
        }
    }

    /* Act. */
    BOOL ok = TRUE;
    if (page != PAGE_CUBE) {
        int a = pick(game, player, item_id), b = put(game, player, item_id, 0, 0, PAGE_CUBE);
        log_line("craft: item -> cube (0,0): pick %d put %d, now page %d", a, b, page_of(item));
        ok = page_of(item) == PAGE_CUBE;
        if (!ok && *(BYTE **)(inv + INV_CURSOR) == item) return_item(game, player, item, page, x, y);
    }
    int cell = 0;
    for (int i = 0; ok && i < n; i++) {
        char name[5];
        code_text(plan[i].code, name);
        if (plan[i].item) {
            BYTE *it = plan[i].item;
            int from = page_of(it);
            if (from != PAGE_CUBE) {
                int a = pick(game, player, id_of(it));
                int b = put(game, player, id_of(it), 2, (DWORD)cell, PAGE_CUBE);
                log_line("craft: %s from page %d -> cube (2,%d): pick %d put %d", name, from, cell, a, b);
                ok = page_of(it) == PAGE_CUBE;
                cell++;
            }
        } else {
            int before = count_in_cube(player, NULL), had = material_count(player, plan[i].material);
            char was_where[96], now_where[96];
            census(player, was_where, sizeof was_where);
            int r = take_material(game, player, plan[i].material);
            int after = count_in_cube(player, NULL), has = material_count(player, plan[i].material);
            census(player, now_where, sizeof now_where);
            log_line("craft: %s from the material tab (#%d) -> %d, cube %d -> %d items, count %d -> %d; "
                     "before: %s; after: %s; tab %lu", name, plan[i].material, r, before, after, had, has,
                     was_where, now_where, pdata ? (unsigned long)*(DWORD *)(pdata + PDATA_STASH_TAB) : 0UL);
            ok = after > before;
        }
    }

    if (ok) {
        transmute(game, player);
        BYTE *result = NULL;
        int left = count_in_cube(player, &result);
        if (left == 1 && result) {
            char made[5];
            code_text(code_of(result), made);
            log_line("craft: transmute made '%s' (item %lu)", made, (unsigned long)id_of(result));
            if (page != PAGE_CUBE) return_item(game, player, result, page, x, y);
        } else {
            log_line("craft: transmute left %d items in the cube; the item goes back", left);
            ok = FALSE;
        }
    }
    if (!ok && find_unit(game, item_id, 4) && page_of(item) == PAGE_CUBE && page != PAGE_CUBE)
        return_item(game, player, item, page, x, y);
    if (tab_set) *(DWORD *)(pdata + PDATA_STASH_TAB) = tab_was;
    if (faked) set_interaction(player, was);
}

static int __fastcall on_button(void *game, void *player, const BYTE *pkt, DWORD size)
{
    if (size == 7 && pkt) {
        WORD button = *(const WORD *)(pkt + 1);
        if ((button & 0xFF00) == CRAFT_BUTTON) {
            DWORD id = ((DWORD)*(const WORD *)(pkt + 3) << 16) | *(const WORD *)(pkt + 5);
            craft_on_server(game, (BYTE *)player, id, button & 0xFF);
            return 0;
        }
    }
    return real_on_button(game, player, pkt, size);
}

/* --- Client side: the hotkey ------------------------------------------------------------------ */

static void request(DWORD item_id, int action)
{
    BYTE p[7] = { 0x4F };
    WORD button = (WORD)(CRAFT_BUTTON | action), hi = (WORD)(item_id >> 16), lo = (WORD)item_id;
    memcpy(p + 1, &button, 2);
    memcpy(p + 3, &hi, 2);
    memcpy(p + 5, &lo, 2);
    send_packet(sizeof p, 1, p);
}

/* Called every frame, from the game's own thread (frame.c). */
void craft_frame(void)
{
    static BOOL was_down[9];
    if (!send_packet || !real_on_button) return;
    BOOL mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    for (int k = 1; k <= ACTION_CORRUPT; k++) {
        BOOL down = mods && (GetAsyncKeyState('0' + k) & 0x8000);
        static DWORD last_sent;
        if (down && !was_down[k] && GetTickCount() - last_sent > 1500) {
            last_sent = GetTickCount();
            BYTE *item = *D2CLIENT_VAR(BYTE *, D2C_HOVERED_ITEM);
            if (item && *(DWORD *)item == 4) {
                log_line("craft: asked for %s on item %lu", ACTIONS[k], (unsigned long)id_of(item));
                request(id_of(item), k);
            }
        }
        was_down[k] = down;
    }
}

/* --- Installing ------------------------------------------------------------------------------- */

/* From bootstrap's loop until it returns TRUE. */
BOOL craft_install(void)
{
    HMODULE game = GetModuleHandleA("D2Game.dll");
    HMODULE mod = GetModuleHandleA("ProjectDiablo.dll");
    HMODULE common = GetModuleHandleA("D2Common.dll");
    HMODULE net = GetModuleHandleA("D2Net.dll");
    if (!game || !mod || !common || !net || !d2.d2client) return FALSE;
    read_table();
    if (!recipe_count) return TRUE;

    BYTE *g = (BYTE *)game;
    pd2 = (BYTE *)mod;
    void **slot = (void **)(g + D2GAME_PACKETS + 0x4F * 8);
    if (*slot != (void *)(g + D2GAME_ON_BUTTON)) {
        log_line("craft: D2Game's 0x4F entry is %p, not D2Game+0x%X -- left alone", *slot, D2GAME_ON_BUTTON);
        return TRUE;
    }
    on_pick = (packet_fn)(void *)(g + D2GAME_ON_PICK);
    on_put = (packet_fn)(void *)(g + D2GAME_ON_PUT);
    on_material = (packet_fn)(void *)(pd2 + PD2_ON_MATERIAL);
    find_unit = (find_unit_fn)(void *)(pd2 + PD2_FIND_UNIT);
    transmute = (transmute_fn)(void *)(pd2 + PD2_TRANSMUTE);
    item_text = (item_text_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_ITEM_TEXT);
    send_packet = (send_fn)(void *)GetProcAddress(net, (LPCSTR)(DWORD_PTR)10024);
    if (!item_text || !send_packet) {
        log_line("craft: a D2 export is missing -- off");
        return TRUE;
    }

    /* Each check against the bytes this was read from: a PD2 update moves code, and a call into
       the wrong place is a crash rather than a failed craft. */
    static const BYTE MATERIAL_OPENS[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 };
    static const BYTE TRANSMUTE_OPENS[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    static const BYTE FIND_OPENS[] = { 0x8B, 0x44, 0x24, 0x04, 0xFF, 0x15 };
    static const BYTE SLOT_OPENS[] = { 0x56, 0x57, 0x8B, 0xFA, 0x8B, 0xF1, 0x83, 0xFF, 0xFF };
    if (memcmp(pd2 + PD2_MATERIAL_SLOT, SLOT_OPENS, sizeof SLOT_OPENS) == 0)
        material_slot = (material_slot_fn)(void *)(pd2 + PD2_MATERIAL_SLOT);
    if (memcmp(pd2 + PD2_ON_MATERIAL, MATERIAL_OPENS, sizeof MATERIAL_OPENS)
        || memcmp(pd2 + PD2_TRANSMUTE, TRANSMUTE_OPENS, sizeof TRANSMUTE_OPENS)
        || memcmp(pd2 + PD2_FIND_UNIT, FIND_OPENS, sizeof FIND_OPENS)) {
        log_line("craft: ProjectDiablo.dll is not the build this was read from -- off");
        return TRUE;
    }

    DWORD was;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &was)) return TRUE;
    real_on_button = (packet_fn)*slot;
    InterlockedExchangePointer(slot, (void *)on_button);
    VirtualProtect(slot, sizeof *slot, was, &was);
    log_line("craft: on -- Ctrl+Alt+1..8 over an item: blood, caster, hitpower, safety, brilliant, "
             "bountiful, vampiric, corrupt");
    return TRUE;
}
