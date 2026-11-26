#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "d2.h"
#include "log.h"
#include "craft.h"

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
 * The client side is the item menu (item_menu.c): it asks what can be made of an item (action 0,
 * answered into shared memory, see craft.h) and then asks for one of them. The recipes come from
 * craft.txt beside the DLL, written from the Craft Center catalog by tools/make_craft_table.py. */

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
#define D2COMMON_UNIT_STAT 10973     /* DWORD __stdcall(unit, stat, layer) */
#define STAT_CORRUPTED     360       /* PD2's ItemStatCost row "corrupted" */
#define STAT_GOLD          14        /* gold carried */
#define D2COMMON_IS_SELLABLE 10705   /* BOOL __stdcall(item): no "can't sell" flag, not a quest item */
#define D2COMMON_IS_TYPE   10744     /* BOOL __stdcall(item, ItemTypes row), with the type's parents */
#define D2COMMON_PRICE     10107     /* int __stdcall(player, item, difficulty, quest flags, npc class, 1 = sell) */
#define D2COMMON_MAX_GOLD  10049     /* int __stdcall(player): gold the player can carry */

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
typedef DWORD (__stdcall *unit_stat_fn)(void *unit, DWORD stat, DWORD layer);
typedef void (__stdcall *send_fn)(DWORD len, DWORD flags, const BYTE *pkt);

static packet_fn real_on_button, on_pick, on_put, on_material;
static find_unit_fn find_unit;
static transmute_fn transmute;
static item_text_fn item_text;
static unit_stat_fn unit_stat;
static send_fn send_packet;
static BYTE *pd2;

/* --- craft.txt -------------------------------------------------------------------------------- */

#define MAX_BASES 1024
#define MAX_RECIPES 128
typedef struct { DWORD code; char category[8]; } base_row;
typedef struct { char category[8]; char family[16]; DWORD rune, gem; char effect[96]; } recipe_row;
typedef struct { char category[8]; int brick; } corrupt_row;
static corrupt_row corruptible[16];
static int corruptible_count;
static base_row bases[MAX_BASES];
static recipe_row recipes[MAX_RECIPES];
static int base_count, recipe_count;

/* The actions 1..8 (craft.h). */
static const char *ACTIONS[] = { NULL, "blood", "caster", "hitpower", "safety", "brilliant",
                                 "bountiful", "vampiric", "corrupt", "sell" };
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
        } else if (n >= 4 && !strcmp(kind, "mods")) {
            /* "mods <category> <family> <text to the end of the line>" */
            for (int i = 0; i < recipe_count; i++) {
                if (strcmp(recipes[i].category, a) || strcmp(recipes[i].family, b)) continue;
                const char *text = strstr(line, b);
                text = text ? text + strlen(b) : "";
                while (*text == ' ') text++;
                _snprintf(recipes[i].effect, sizeof recipes[i].effect, "%s", text);
                recipes[i].effect[sizeof recipes[i].effect - 1] = '\0';
                char *end = recipes[i].effect + strlen(recipes[i].effect);
                while (end > recipes[i].effect && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
            }
        } else if (n >= 3 && !strcmp(kind, "corrupt") && corruptible_count < 16) {
            _snprintf(corruptible[corruptible_count].category, 8, "%s", a);
            corruptible[corruptible_count].category[7] = '\0';
            corruptible[corruptible_count].brick = atoi(b);
            corruptible_count++;
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

static const corrupt_row *corruption_for(const char *category)
{
    for (int i = 0; category && i < corruptible_count; i++)
        if (!strcmp(corruptible[i].category, category)) return &corruptible[i];
    return NULL;
}

static const recipe_row *recipe_for(const char *category, const char *family)
{
    for (int i = 0; i < recipe_count; i++)
        if (!strcmp(recipes[i].category, category) && !strcmp(recipes[i].family, family))
            return &recipes[i];
    return NULL;
}

/* --- What the menu is told ------------------------------------------------------------------- */

static craft_info info;
static volatile LONG info_seq;
static char status[128];
static volatile LONG status_seq;

static void say(const char *fmt, ...)
{
    char text[128];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    text[sizeof text - 1] = '\0';
    log_line("craft: %s", text);
    memcpy(status, text, sizeof status);
    InterlockedIncrement(&status_seq);
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
        say("the cube is not empty");
        return;
    }

    DWORD base = code_of(item);
    char base_name[5];
    code_text(base, base_name);
    DWORD want[3];
    int n = 0;
    if (action == ACTION_CORRUPT) {
        if (unit_stat && unit_stat(item, STAT_CORRUPTED, 0)) { say("already corrupted"); return; }
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
        say("no %s to be had -- nothing was moved", name);
        return;
    }

    BYTE *cube = NULL;
    for (BYTE *it = first_item(player); it && !cube; it = next_item(it))
        if (code_of(it) == code4("box") && page_of(it) != PAGE_CUBE) cube = it;
    if (!cube) { say("no Horadric Cube -- nothing was moved"); return; }
    /* PD2 +0x380FB0, which the material tab's handler asks, finds the cube only in the inventory
       — or anywhere while a PD2 flag is up that is up at the stash (2026-11-26: a cube kept in
       the stash worked there and failed in the field, the handler returning 0 untouched). */
    BOOL uses_material = FALSE;
    for (int i = 0; i < n; i++) uses_material |= plan[i].material >= 0;
    if (uses_material && page_of(cube) != PAGE_INVENTORY && !at_stash(player)) {
        say("the cube is in the stash; away from it, it has to be in the inventory");
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
            say("%s: done", ACTIONS[action]);
            if (page != PAGE_CUBE) return_item(game, player, result, page, x, y);
        } else {
            log_line("craft: transmute left %d items in the cube; the item goes back", left);
            say("%s: the game made nothing of it", ACTIONS[action]);
            ok = FALSE;
        }
    }
    if (!ok && find_unit(game, item_id, 4) && page_of(item) == PAGE_CUBE && page != PAGE_CUBE)
        return_item(game, player, item, page, x, y);
    if (tab_set) *(DWORD *)(pdata + PDATA_STASH_TAB) = tab_was;
    if (faked) set_interaction(player, was);
}

/* --- Selling ---------------------------------------------------------------------------------
 *
 * At a vendor (trade window open) the sale goes through the game's own packet 0x33 with that
 * vendor: whatever the packet table holds — PD2's wrapper (+0x3955A0), then D2Game's sale
 * (+0xBF990) — so the vendor's rules, price and shelf are the vendor's.
 *
 * Anywhere else (user, 2026-11-26: "do it anyway, but check first whether the item can really be
 * sold — far from every item can"), the sale is D2Game's own, done without a vendor. Read out of
 * +0xBF990 for an item stored in the inventory (mode 0):
 *
 *   refused unless   the item is the player's (in the inventory list or on the cursor),
 *                    its mode is the one the packet names,
 *                    D2Common #10705 says it can be sold (no "can't sell" flag; not a quest item);
 *   price            D2Common #10107(player, item, difficulty, quest flags, vendor class, 1);
 *   scrolls, tomes   (ItemTypes 22, 18) also take the matching skill's charges back, here not
 *                    done, so they are left to a vendor;
 *   the rest         ethereal, personalised, broken, socketed-and-filled and "Cracked" items are
 *                    sold too, they only do not go up on the vendor's shelf — there is no shelf;
 *   then             D2Game +0xD39B0 (eax item, ecx player, push game) takes the item out of the
 *                    inventory, tells the client and frees it — what PD2 itself calls when a
 *                    rune from the inventory goes into a material tab — and D2Game +0x12DF0
 *                    (esi player, push game, push gold) pays, up to the gold the player can
 *                    carry, the rest dropped at the player's feet, as the vendor does.
 *
 * PD2's wrapper adds one rule, kept here too: with player data +0x269 set, a charm (ItemTypes 13)
 * below the inventory's fourth row is not sold.
 *
 * The vendor whose prices are used away from one is Charsi (class 154); D2 1.13's vendors share
 * their price rules, they differ in what they stock. */
#define D2GAME_REMOVE_ITEM 0xD39B0
#define D2GAME_GIVE_GOLD   0x12DF0
#define VENDOR_CLASS       154       /* Charsi */
#define UNIT_MODE          0x10
#define UNIT_CLASS         0x04
#define GAME_DIFFICULTY    0x6D
#define PDATA_QUEST_FLAGS  0x10      /* one pointer per difficulty */
#define PDATA_PD2_CHARMS   0x269
#define TYPE_CHARM         13
#define TYPE_BOOK          18
#define TYPE_SCROLL        22

typedef BOOL (__stdcall *item_fn)(void *item);
typedef BOOL (__stdcall *is_type_fn)(void *item, DWORD type);
typedef int (__stdcall *price_fn)(void *player, void *item, DWORD difficulty, void *quests, DWORD npc_class, DWORD mode);
typedef int (__stdcall *max_gold_fn)(void *player);

static packet_fn on_sell;
static item_fn is_sellable;
static is_type_fn is_type;
static price_fn price_of;
static max_gold_fn max_gold;
static void *remove_item_fn, *give_gold_fn;
static volatile LONG sales_anywhere;

/* The two D2Game routines take their arguments in registers. Each trampoline is __stdcall
   (fn, game, player, x) and keeps every register the C side expects kept. */
void __stdcall call_remove_item(void *fn, void *game, void *player, void *item);
void __stdcall call_give_gold(void *fn, void *game, void *player, DWORD gold);
__asm__(
    ".text\n"
    ".globl _call_remove_item@16\n"
    "_call_remove_item@16:\n\t"
    "pushl %ebx\n\tpushl %esi\n\tpushl %edi\n\tpushl %ebp\n\t"
    "movl 32(%esp), %eax\n\t"      /* item */
    "movl 28(%esp), %ecx\n\t"      /* player */
    "movl 20(%esp), %edx\n\t"      /* fn */
    "pushl 24(%esp)\n\t"           /* game; the routine pops it */
    "call *%edx\n\t"
    "popl %ebp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\t"
    "ret $16\n"
    ".globl _call_give_gold@16\n"
    "_call_give_gold@16:\n\t"
    "pushl %ebx\n\tpushl %esi\n\tpushl %edi\n\tpushl %ebp\n\t"
    "movl 28(%esp), %esi\n\t"      /* player */
    "movl 20(%esp), %edx\n\t"      /* fn */
    "pushl 32(%esp)\n\t"           /* gold */
    "pushl 28(%esp)\n\t"           /* game (24 before the push above); the routine pops both */
    "call *%edx\n\t"
    "popl %ebp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\t"
    "ret $16\n");

static BOOL trading(BYTE *player)
{
    return player[0x6C] && *(DWORD *)(player + 0x68) == 1;
}

static BOOL owned(BYTE *player, BYTE *item)
{
    for (BYTE *it = first_item(player); it; it = next_item(it))
        if (it == item) return TRUE;
    return FALSE;
}

/* The price the game would pay, 0 with the reason in why if it would not buy the item. Away from
   a vendor npc_class is Charsi's; why_hard says the item cannot be sold anywhere at all (and the
   menu leaves the line out), rather than not here. */
static int sale_price(void *game, BYTE *player, BYTE *item, DWORD npc_class, char *why, size_t n, BOOL *why_hard)
{
    *why_hard = TRUE;
    why[0] = '\0';
    if (!is_sellable || !price_of) { _snprintf(why, n, "selling is off"); return 0; }
    if (!owned(player, item) || !is_sellable(item)) { _snprintf(why, n, "cannot be sold"); return 0; }
    int page = page_of(item);
    if (*(DWORD *)(item + UNIT_MODE) != 0
        || (page != PAGE_INVENTORY && page != PAGE_CUBE && page != PAGE_STASH)) {
        _snprintf(why, n, "not from where it is");
        return 0;
    }
    *why_hard = FALSE;
    BYTE *pdata = *(BYTE **)(player + UNIT_DATA);
    if (pdata && *(DWORD *)(pdata + PDATA_PD2_CHARMS) && page == PAGE_INVENTORY && y_of(item) > 3
        && is_type(item, TYPE_CHARM)) {
        _snprintf(why, n, "PD2 keeps this charm");
        return 0;
    }
    if (!trading(player) && (is_type(item, TYPE_SCROLL) || is_type(item, TYPE_BOOK))) {
        _snprintf(why, n, "at a vendor");
        return 0;
    }
    if (page == PAGE_STASH && !at_stash(player)) { _snprintf(why, n, "open the stash"); return 0; }
    if (trading(player) && page != PAGE_INVENTORY) { _snprintf(why, n, "only from the inventory"); return 0; }
    DWORD difficulty = ((BYTE *)game)[GAME_DIFFICULTY];
    void *quests = pdata && difficulty < 3 ? *(void **)(pdata + PDATA_QUEST_FLAGS + difficulty * 4) : NULL;
    int gold = price_of(player, item, difficulty, quests, npc_class, 1);
    if (gold <= 0) { *why_hard = TRUE; _snprintf(why, n, "worth nothing"); }
    return gold > 0 ? gold : 0;
}

static DWORD vendor_class(void *game, BYTE *player)
{
    if (!trading(player)) return VENDOR_CLASS;
    BYTE *npc = find_unit(game, *(DWORD *)(player + 0x64), 1);
    return npc ? *(DWORD *)(npc + UNIT_CLASS) : VENDOR_CLASS;
}

static void sell_at_vendor(void *game, BYTE *player, BYTE *item)
{
    DWORD npc = *(DWORD *)(player + 0x64), id = id_of(item);
    DWORD before = unit_stat ? unit_stat(player, STAT_GOLD, 0) : 0;
    BYTE p[17] = { 0x33 };
    memcpy(p + 1, &npc, 4);
    memcpy(p + 5, &id, 4);
    int r = on_sell(game, player, p, sizeof p);
    DWORD after = unit_stat ? unit_stat(player, STAT_GOLD, 0) : 0;
    BOOL gone = !find_unit(game, id, 4) || page_of(item) != PAGE_INVENTORY;
    log_line("craft: sell item %lu to npc %lu -> %d, gold %lu -> %lu, %s", (unsigned long)id,
             (unsigned long)npc, r, (unsigned long)before, (unsigned long)after, gone ? "sold" : "still here");
    if (gone && after >= before) say("sold: +%lu gold", (unsigned long)(after - before));
    else if (gone) say("sold");
    else say("the vendor would not take it");
}

static void sell_on_server(void *game, BYTE *player, BYTE *item)
{
    char why[48], code[5];
    BOOL hard;
    int gold = sale_price(game, player, item, vendor_class(game, player), why, sizeof why, &hard);
    if (!gold) { say("not sold: %s", why); return; }
    if (trading(player)) { sell_at_vendor(game, player, item); return; }
    if (!remove_item_fn || !give_gold_fn) { say("not sold: selling away from a vendor is off"); return; }

    DWORD id = id_of(item);
    code_text(code_of(item), code);
    int page = page_of(item), x = x_of(item), y = y_of(item);
    DWORD before = unit_stat(player, STAT_GOLD, 0);
    int room = max_gold ? max_gold(player) - (int)before : gold;
    call_remove_item(remove_item_fn, game, player, item);   /* item is freed from here on */
    BOOL gone = !find_unit(game, id, 4) || !owned(player, find_unit(game, id, 4));
    if (!gone) {
        log_line("craft: sell anywhere: item %lu '%s' still in the inventory after removal -- no gold", (unsigned long)id, code);
        say("not sold: the item would not leave");
        return;
    }
    call_give_gold(give_gold_fn, game, player, (DWORD)gold);
    InterlockedIncrement(&sales_anywhere);
    DWORD after = unit_stat(player, STAT_GOLD, 0);
    log_line("craft: sold item %lu '%s' from page %d (%d,%d) for %d, gold %lu -> %lu%s", (unsigned long)id,
             code, page, x, y, gold, (unsigned long)before, (unsigned long)after,
             gold > room ? ", the rest dropped" : "");
    if (gold > room && room >= 0) say("sold: +%d gold, %d dropped at your feet", room, gold - room);
    else say("sold: +%d gold", gold);
}

/* What the menu may offer for this item: each recipe with what it needs and how many of each there
   are, and corruption. Same rules as the craft itself, so what is offered is what will work. */
static int have_of(BYTE *player, DWORD code, BYTE *target)
{
    int n = 0, m = material_of(code);
    if (m >= 0) n += material_count(player, m);
    for (BYTE *it = first_item(player); it; it = next_item(it)) {
        int pg = page_of(it);
        if (it != target && code_of(it) == code
            && (pg == PAGE_INVENTORY || pg == PAGE_CUBE || (pg == PAGE_STASH && at_stash(player))))
            n++;
    }
    return n;
}

static void need(craft_option *o, DWORD code, int have)
{
    o->need[o->needs].code = code;
    o->need[o->needs].have = have;
    o->needs++;
}

static void query_on_server(void *game, BYTE *player, DWORD item_id)
{
    craft_info a;
    memset(&a, 0, sizeof a);
    a.item_id = item_id;
    BYTE *item = find_unit(game, item_id, 4);
    if (!item || !data_of(item)) {
        _snprintf(a.problem, sizeof a.problem, "no such item");
    } else {
        a.quality = (int)*(DWORD *)data_of(item);
        DWORD base = code_of(item);
        const char *category = category_of(base);
        BOOL corrupted = unit_stat && unit_stat(item, STAT_CORRUPTED, 0) != 0;
        const corrupt_row *corruption = corrupted ? NULL : corruption_for(category);
        {
            craft_option *o = &a.option[a.count];
            memset(o, 0, sizeof *o);
            o->action = CRAFT_ACTION_SELL;
            _snprintf(o->family, sizeof o->family, "sell");
            BOOL hard;
            int gold = sale_price(game, player, item, vendor_class(game, player), o->why, sizeof o->why, &hard);
            int room = max_gold && unit_stat ? max_gold(player) - (int)unit_stat(player, STAT_GOLD, 0) : gold;
            if (gold && gold > room && room >= 0)
                _snprintf(o->effect, sizeof o->effect, "+%d gold, %d to the ground", room, gold - room);
            else if (gold)
                _snprintf(o->effect, sizeof o->effect, "+%d gold%s", gold, trading(player) ? "" : " (no vendor)");
            o->effect[sizeof o->effect - 1] = '\0';
            o->possible = gold > 0;
            if (gold || !hard) a.count++;
        }
        for (int action = 1; action <= CRAFT_ACTION_CORRUPT; action++) {
            craft_option *o = &a.option[a.count];
            memset(o, 0, sizeof *o);
            o->action = action;
            _snprintf(o->family, sizeof o->family, "%s", ACTIONS[action]);
            if (action == CRAFT_ACTION_CORRUPT) {
                /* Only a type the corruption table has rows for, and not twice. */
                if (!corruption) continue;
                need(o, code4("wss"), have_of(player, code4("wss"), item));
                _snprintf(o->effect, sizeof o->effect, "a random mod, %d%% brick", corruption->brick);
            } else {
                /* Crafting takes a magic or a rare item (CubeMain has both, "mag" and "rar", for
                   every recipe); nothing else gets a line. */
                const recipe_row *r = category && (a.quality == 4 || a.quality == 6)
                                    ? recipe_for(category, ACTIONS[action]) : NULL;
                if (!r) continue;
                need(o, code4("jewf"), have_of(player, code4("jewf"), item) + have_of(player, code4("jew"), item));
                need(o, r->rune, have_of(player, r->rune, item));
                need(o, r->gem, have_of(player, r->gem, item));
                _snprintf(o->effect, sizeof o->effect, "%s", r->effect);
            }
            o->effect[sizeof o->effect - 1] = '\0';
            for (int i = 0; i < o->needs && !o->why[0]; i++)
                if (o->need[i].have < 1) _snprintf(o->why, sizeof o->why, "missing");
            o->possible = !o->why[0];
            a.count++;
        }
        BYTE *inv = *(BYTE **)(player + UNIT_INVENTORY);
        BYTE *cube = NULL, *other = NULL;
        for (BYTE *it = first_item(player); it && !cube; it = next_item(it))
            if (code_of(it) == code4("box") && page_of(it) != PAGE_CUBE) cube = it;
        int in_cube = count_in_cube(player, &other);
        if (!cube) _snprintf(a.problem, sizeof a.problem, "no Horadric Cube");
        else if (page_of(cube) != PAGE_INVENTORY && !at_stash(player))
            _snprintf(a.problem, sizeof a.problem, "the cube is in the stash: carry it, or open the stash");
        else if (in_cube > 1 || (in_cube == 1 && other != item))
            _snprintf(a.problem, sizeof a.problem, "empty the cube first");
        else if (inv && *(void **)(inv + INV_CURSOR))
            _snprintf(a.problem, sizeof a.problem, "put down what is on the cursor");
    }
    a.problem[sizeof a.problem - 1] = '\0';
    a.seq = InterlockedIncrement(&info_seq);
    memcpy(&info, &a, sizeof info);
}

static int __fastcall on_button(void *game, void *player, const BYTE *pkt, DWORD size)
{
    if (size == 7 && pkt) {
        WORD button = *(const WORD *)(pkt + 1);
        if ((button & 0xFF00) == CRAFT_BUTTON) {
            DWORD id = ((DWORD)*(const WORD *)(pkt + 3) << 16) | *(const WORD *)(pkt + 5);
            int action = button & 0xFF;
            BYTE *item = action == CRAFT_ACTION_SELL ? find_unit(game, id, 4) : NULL;
            if (action == 0) query_on_server(game, (BYTE *)player, id);
            else if (action == CRAFT_ACTION_SELL) { if (item && data_of(item)) sell_on_server(game, (BYTE *)player, item); }
            else craft_on_server(game, (BYTE *)player, id, action);
            return 0;
        }
    }
    return real_on_button(game, player, pkt, size);
}

/* --- Client side: what the menu calls ------------------------------------------------------- */

static void send_button(DWORD item_id, int action)
{
    BYTE p[7] = { 0x4F };
    WORD button = (WORD)(CRAFT_BUTTON | action), hi = (WORD)(item_id >> 16), lo = (WORD)item_id;
    memcpy(p + 1, &button, 2);
    memcpy(p + 3, &hi, 2);
    memcpy(p + 5, &lo, 2);
    send_packet(sizeof p, 1, p);
}

BOOL craft_ready(void) { return send_packet && real_on_button; }

void craft_query(DWORD item_id) { if (craft_ready()) send_button(item_id, 0); }

void craft_request(DWORD item_id, int action)
{
    if (!craft_ready() || action < 1 || action > CRAFT_ACTIONS) return;
    log_line("craft: asked for %s on item %lu", ACTIONS[action], (unsigned long)item_id);
    send_button(item_id, action);
}

BOOL craft_info_get(craft_info *out)
{
    LONG seq = info_seq;
    if (!seq) return FALSE;
    memcpy(out, &info, sizeof *out);
    return out->seq == seq && info_seq == seq;
}

BOOL craft_sale_sound_get(LONG *seen)
{
    LONG n = sales_anywhere;
    if (n == *seen) return FALSE;
    *seen = n;
    return TRUE;
}

BOOL craft_status_get(LONG *seen, char *out, size_t size)
{
    LONG seq = status_seq;
    if (seq == *seen) return FALSE;
    *seen = seq;
    _snprintf(out, size, "%s", status);
    out[size - 1] = '\0';
    return TRUE;
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
    on_sell = *(packet_fn *)(g + D2GAME_PACKETS + 0x33 * 8);
    find_unit = (find_unit_fn)(void *)(pd2 + PD2_FIND_UNIT);
    transmute = (transmute_fn)(void *)(pd2 + PD2_TRANSMUTE);
    item_text = (item_text_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_ITEM_TEXT);
    unit_stat = (unit_stat_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_UNIT_STAT);
    send_packet = (send_fn)(void *)GetProcAddress(net, (LPCSTR)(DWORD_PTR)10024);
    is_sellable = (item_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_IS_SELLABLE);
    is_type = (is_type_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_IS_TYPE);
    price_of = (price_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_PRICE);
    max_gold = (max_gold_fn)(void *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)D2COMMON_MAX_GOLD);
    if (!is_type) is_sellable = NULL;
    {
        /* Selling away from a vendor calls into the middle of D2Game: only if it is the 1.13c
           these were read from. */
        static const BYTE REMOVE_OPENS[] = { 0x51, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x85, 0xFF, 0x8B, 0xF0 };
        static const BYTE GOLD_OPENS[] = { 0x51, 0x55, 0x57, 0x56, 0xE8, 0x61, 0x76, 0xFF, 0xFF, 0x6A };
        if (memcmp(g + D2GAME_REMOVE_ITEM, REMOVE_OPENS, sizeof REMOVE_OPENS) == 0
            && memcmp(g + D2GAME_GIVE_GOLD, GOLD_OPENS, sizeof GOLD_OPENS) == 0 && unit_stat) {
            remove_item_fn = g + D2GAME_REMOVE_ITEM;
            give_gold_fn = g + D2GAME_GIVE_GOLD;
        } else {
            log_line("craft: D2Game's item removal or gold routine is not the one read -- selling only at a vendor");
        }
    }
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
    log_line("craft: on -- LB or ~ over an item opens its menu");
    return TRUE;
}
