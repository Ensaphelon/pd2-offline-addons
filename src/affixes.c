/* Which affix gave this line, and what it could have rolled -- written at the end of the line
 * itself, beside the range the maphack already prints there.
 *
 *     +87% Enhanced Defense [10 - 200]  Blessed 51-65
 *     Lightning Resist +48% [6 - 70]  Chromatic 21-30, Ocher 11-20
 *
 * WHY NOT THE LOOT FILTER
 * -----------------------
 * This started as a block of filter rules and worked, but it could only ever appear as a lump
 * above the item's stats: a filter supplies a name and one description block, and nothing in the
 * language addresses a stat line. The second line above is also the reason the filter version is
 * not enough — a stat fed by two affixes can be named only where the stat is.
 *
 * WHERE THE HOOK GOES
 * -------------------
 * The ` [10 - 200]` is BH.dll's doing, not the game's: `always_show_stat_ranges` is one of its
 * settings, and the eight format strings behind it (`%s[%d - %d]%s` and relatives) sit in its
 * .rdata, referenced from one function only. That function is `_BHOnPropertyBuild@16`, which
 * BH EXPORTS -- Project Diablo 2 builds a property's line, hands it over, and gets the range
 * appended. The decorated name gives the calling convention away: @16 is four stdcall arguments.
 *
 * What those four are is settled by BH's own internal call to it, in `_BHOnDamagePropertyBuild`:
 *
 *     10099c83  push 0x0                  ; arg4
 *     10099c85  push [ebp-0x114]          ; arg3  UnitAny*  (the item)
 *     10099c8b  lea  eax,[ebp-0x104]
 *     10099c91  push edi                  ; arg2  the stat id
 *     10099c92  push eax                  ; arg1  the line, a 130-wchar stack buffer
 *     10099c93  call _BHOnPropertyBuild
 *
 * and its own prologue agrees: [ebp+8] is walked as a wide string, [ebp+0xC] is compared against
 * a stat table, and [ebp+0x10] is dereferenced at +0x5C into a StatList.
 *
 * Patching THAT call site was the first attempt and it was wrong: it is the damage path only,
 * and PD2 reaches the general one through the export, so the detour never ran once. Nothing in
 * the module imports BH, so there is no import slot to swap either -- PD2 resolves the address
 * itself. What is left is a detour on the exported function's own prologue, and six bytes of it
 * land on an instruction boundary:
 *
 *     55              push ebp
 *     8B EC           mov  ebp,esp
 *     83 E4 C0        and  esp,0xffffffc0
 *
 * The trampoline is those six bytes plus a jump back, so calling it IS calling the original.
 *
 * The function is found by name through GetProcAddress rather than by address, so a BH rebuild
 * that moves it is still found; the prologue is compared against the bytes above before anything
 * is written, so a BH rebuild that CHANGES it costs a line in the log and the feature, not the
 * game.
 *
 * WHAT IT KNOWS AND WHAT IT DOES NOT
 * ----------------------------------
 * The affix ids are on the item (ItemData+0x38 and +0x3E, three of each); the ranges come from
 * affixes.txt beside this DLL, generated from the game's own tables by the Python half. What is
 * deliberately NOT attempted is splitting the VALUE between two affixes: the item stores one
 * merged total per stat, so on the Lightning Resist line above there is no way to say which of
 * the 48 came from which. Naming both and their ranges is the whole of what the data supports.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d2.h"
#include "log.h"
#include "addons.h"

/* --- the table, as the generator lays it out ------------------------------------------------- */

/* One numbering for both kinds, which is the game's own: a suffix is its row in MagicSuffix, a
   prefix is its row in MagicPrefix plus the length of the suffix table less one. The generator
   has already done that arithmetic, so an id read off an item indexes this array directly.
   2048 covers the 1044 + 991 the game ships with room for a season that adds more. */
#define MAX_AFFIX   4096
#define MAX_MODS      12    /* a Brilliant body craft is six: block, four resistances, defence */
#define MAX_NAME      31

typedef struct {
    unsigned char count;
    unsigned short stat[MAX_MODS];
    short lo[MAX_MODS], hi[MAX_MODS];
    unsigned char pct[MAX_MODS];    /* the game will draw this number with a per cent sign */
    char name[MAX_NAME + 1];
} affix_entry;

static affix_entry table[MAX_AFFIX];
static int table_rows;

/* The craft recipes, found by fingerprint rather than by an id: see craft_for below. 65 distinct
   mod sets ship with the game. */
#define MAX_CRAFT 128
static affix_entry crafts[MAX_CRAFT];
static int craft_count;

/* Which base items each recipe applies to. Seven recipes reach any given item -- one per
   family -- and without this narrowing the field is every recipe in the game, which is how a
   helm's mods end up proposed for a breastplate. */
#define MAX_CODES 8192
static struct { unsigned short recipe; unsigned short item; } craft_code[MAX_CODES];
static int craft_code_count;

/* --- the game's own structures, only as far as this reads them -------------------------------- */

#define UNITANY_ITEMDATA   0x14     /* the union; for an item it is ItemData* */
#define UNITANY_STATS      0x5C     /* StatList* */
#define ITEMDATA_QUALITY   0x00     /* 8 is crafted */
#define ITEMDATA_PREFIX    0x38     /* WORD[3] */
#define ITEMDATA_SUFFIX    0x3E     /* WORD[3] */
/* TWO lists, and the one that matters is the second. +0x24 holds the item's base stats and on a
   crafted Light Plate that is three entries -- defence and the two durabilities. Everything a
   recipe or an affix put there lives in the list at +0x48, which is also the one BH itself walks
   in the function this plugin hooks. Both are searched; a base stat can be a recipe's mod too. */
#define STATLIST_PSTAT     0x24     /* Stat*, eight bytes each: index at +2, value at +4 */
#define STATLIST_COUNT     0x28     /* WORD */
#define STATLIST_PMOD      0x48
#define STATLIST_MODCOUNT  0x4C
#define QUALITY_CRAFTED    8

/* --- BH.dll ----------------------------------------------------------------------------------- */

#define BH_EXPORT "_BHOnPropertyBuild@16"

/* `push ebp; mov ebp,esp; and esp,0xffffffc0` -- six bytes, ending on an instruction boundary,
   which is what a five-byte jump needs with one to spare. */
static const BYTE PROLOGUE_EXPECTED[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0 };
#define STOLEN (sizeof PROLOGUE_EXPECTED)

typedef void(__stdcall *append_range_fn)(wchar_t *line, int stat_id, void *item, int flag);
static append_range_fn original;

/* The longest line BH builds here lands in a 130-wchar stack buffer (`lea eax,[ebp-0x104]`), and
   that buffer is this detour's first argument. Writing past it would be writing over BH's own
   frame, so nothing is appended that does not fit with room for the terminator. */
#define LINE_CAPACITY 130

/* D2's own colour escape: 0xFF, 'c', then the colour.
 *
 * Both are needed, not just the first. A colour in this game holds until something changes it,
 * and these lines are drawn one after another into the same tooltip -- so an unclosed orange
 * runs on through every stat line below it and out the other side. '3' is what BH itself
 * restores to after its own range, which is the blue the stat lines are drawn in. */
static const wchar_t COLOUR_ORANGE[] = { 0xFF, L'c', L'8', 0 };
static const wchar_t COLOUR_RESTORE[] = { 0xFF, L'c', L'3', 0 };
#define ESCAPE 3

/* --- loading ---------------------------------------------------------------------------------- */

static int parse_line(const char *line)
{
    char kind = 0;
    int affix = 0, stat = 0, lo = 0, hi = 0, pct = 0, consumed = 0;
    if (sscanf(line, " %c %d %d %d %d %d %n",
               &kind, &affix, &stat, &lo, &hi, &pct, &consumed) != 6)
        return 0;
    if ((kind != 'p' && kind != 's' && kind != 'c' && kind != 't') ||
        affix < 0 || affix >= MAX_AFFIX) return 0;
    if ((kind == 'p' || kind == 's') && affix < 1) return 0;
    if (stat < 0 || stat > 0xFFFF) return 0;

    if (kind == 't') {
        if (craft_code_count >= MAX_CODES || affix >= MAX_CRAFT) return 0;
        if (stat < 0 || stat > 0xFFFF) return 0;
        craft_code[craft_code_count].recipe = (unsigned short)affix;
        craft_code[craft_code_count].item = (unsigned short)stat;
        craft_code_count++;
        return 1;
    }

    affix_entry *e;
    if (kind == 'c') {
        if (affix >= MAX_CRAFT) return 0;
        e = &crafts[affix];
        if (affix >= craft_count) craft_count = affix + 1;
    } else {
        e = &table[affix];
    }
    if (e->count >= MAX_MODS) return 0;
    e->stat[e->count] = (unsigned short)stat;
    e->lo[e->count] = (short)lo;
    e->hi[e->count] = (short)hi;
    e->pct[e->count] = pct ? 1 : 0;
    e->count++;

    if (!e->name[0] && consumed > 0) {
        const char *name = line + consumed;
        size_t n = strlen(name);
        while (n && (name[n - 1] == '\r' || name[n - 1] == '\n' || name[n - 1] == ' ')) n--;
        if (n > MAX_NAME) n = MAX_NAME;
        memcpy(e->name, name, n);
        e->name[n] = 0;
    }
    return 1;
}

void affixes_init(void *module)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA((HMODULE)module, path, MAX_PATH)) return;
    char *slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy(slash + 1, "affixes.txt");

    FILE *f = fopen(path, "rb");
    if (!f) {
        log_line("affix ranges: no table at %s -- stat lines stay as they are", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        table_rows += parse_line(line);
    }
    fclose(f);
    log_line("affix ranges: %d rows loaded", table_rows);
}

/* --- the detour -------------------------------------------------------------------------------- */

static int safe_read(const void *at, void *into, SIZE_T bytes)
{
    SIZE_T got = 0;
    if (!at) return 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, bytes, &got) && got == bytes;
}

/* Reads one of the item's six affix slots. Zero is the format's "no affix here", not affix 0. */
static const affix_entry *affix_at(void *item, int slot)
{
    if (!item) return NULL;
    BYTE *data = *(BYTE **)((BYTE *)item + UNITANY_ITEMDATA);
    if (!data) return NULL;
    int n = slot % 3;
    WORD id = *(WORD *)(data + (slot < 3 ? ITEMDATA_PREFIX : ITEMDATA_SUFFIX) + n * 2);
    if (!id || id >= MAX_AFFIX) return NULL;
    const affix_entry *e = &table[id];
    return e->count ? e : NULL;
}

/* "Blessed [51 - 65]%", written the way the range already on the line is written -- same
   brackets, same spaces -- so the two read as one thought. The per cent sign comes from the
   stat's description function, because the game string does not carry it: "Enhanced Defense" is
   stored without one and drawn with one.

   An affix with no roll of its own gets its name alone: `rep-dur` takes a fixed rate from its
   parameter, and a range there would be an invention. */
static int describe(const affix_entry *e, int stat_id, char *out, size_t cap)
{
    for (int i = 0; i < e->count; i++) {
        if (e->stat[i] != stat_id) continue;
        const char *pc = e->pct[i] ? "%" : "";
        if (e->lo[i] == 0 && e->hi[i] == 0)
            return _snprintf(out, cap, "%s", e->name);
        if (e->lo[i] == e->hi[i])
            return _snprintf(out, cap, "%s [%d]%s", e->name, e->lo[i], pc);
        return _snprintf(out, cap, "%s [%d - %d]%s", e->name, e->lo[i], e->hi[i], pc);
    }
    return 0;
}

/* Bounded diagnostic: the first few lines of the first tooltip, and then silence.
   Behind the repo's development switch -- it reads item memory and writes a line per tooltip
   line, and what it answered (which stat id, which affix slots) is now settled. */
#if ADDONS_DEV
static int traced;
#endif


/* --- the craft recipe ---------------------------------------------------------------------- */

/* Is this stat on the item at all? The list hangs off UnitAny+0x5C; entries are eight bytes with
   the stat index at +2. Read through ReadProcessMemory for the same reason the rest of this
   repository does it -- IsBadReadPtr and VirtualQuery-then-read have each taken this game
   down -- and a count that is not plausible is treated as no list rather than walked. */
static int list_has_stat(BYTE *stats, int at_pointer, int at_count, int stat_id)
{
    BYTE *entries = NULL;
    WORD count = 0;
    if (!safe_read(stats + at_pointer, &entries, sizeof entries)) return 0;
    if (!safe_read(stats + at_count, &count, sizeof count)) return 0;
    if (!entries || count > 512) return 0;
    for (WORD i = 0; i < count; i++) {
        WORD index = 0;
        if (!safe_read(entries + i * 8 + 2, &index, sizeof index)) return 0;
        if (index == stat_id) return 1;
    }
    return 0;
}

static int item_has_stat(void *item, int stat_id)
{
    BYTE *stats = NULL;
    if (!safe_read((BYTE *)item + UNITANY_STATS, &stats, sizeof stats) || !stats) return 0;
    return list_has_stat(stats, STATLIST_PMOD, STATLIST_MODCOUNT, stat_id) ||
           list_has_stat(stats, STATLIST_PSTAT, STATLIST_COUNT, stat_id);
}

#define UNITANY_TXTFILENO  0x04     /* which row of the game's one item table this is */

/* Which craft recipe made this item, by fingerprint: the one whose every fixed mod is a stat the
   item carries. Nothing on the item records the recipe -- the cube applies its mods and they
   become ordinary stats -- so this is the only way to ask, and it is answered conservatively:
   two recipes matching means no answer at all rather than a guess.

   One item at a time is remembered, which is all a tooltip needs: the same item is asked about
   once per line it draws. */
static const affix_entry *craft_for(void *item)
{
    static void *cached_item;
    static const affix_entry *cached;

    BYTE *data = item ? *(BYTE **)((BYTE *)item + UNITANY_ITEMDATA) : NULL;
    if (!data || *(DWORD *)(data + ITEMDATA_QUALITY) != QUALITY_CRAFTED) return NULL;
    if (item == cached_item) return cached;

    /* Which base item this is. The unit carries the number itself, so no call into the game and
       no struct offset to be wrong about -- the table was generated against the same numbering
       and checked against a real item, which comes through as 340. */
    DWORD item_no = 0;
    const affix_entry *found = NULL;
    int best = 0, runner = 0;
    if (safe_read((BYTE *)item + UNITANY_TXTFILENO, &item_no, sizeof item_no)) {
        for (int c = 0; c < craft_code_count; c++) {
            if (craft_code[c].item != item_no) continue;
            const affix_entry *e = &crafts[craft_code[c].recipe];
            if (!e->count) continue;
            int hits = 0;
            for (int i = 0; i < e->count; i++)
                hits += item_has_stat(item, e->stat[i]);
            if (hits > best) { runner = best; best = hits; found = e; }
            else if (hits > runner) runner = hits;
        }
    }
    /* Two mods is the floor, and the winner has to be a winner: one family sharing a single
       resistance with another is not an identification. */
    if (best < 2 || best == runner) found = NULL;

    cached_item = item;
    cached = found;
    return found;
}

static void __stdcall on_stat_line(wchar_t *line, int stat_id, void *item, int flag)
{
    original(line, stat_id, item, flag);
#if ADDONS_DEV
    BYTE *trace_data = item ? *(BYTE **)((BYTE *)item + UNITANY_ITEMDATA) : NULL;
    if (traced < 60 && trace_data) {
        WORD a[6];
        for (int i = 0; i < 3; i++) {
            a[i] = *(WORD *)(trace_data + ITEMDATA_PREFIX + i * 2);
            a[3 + i] = *(WORD *)(trace_data + ITEMDATA_SUFFIX + i * 2);
        }
        /* Only items that actually carry an affix: an unaffixed one says nothing useful and
           24 lines of it is all a hover over the wrong slot costs. */
        if (a[0] || a[1] || a[2] || a[3] || a[4] || a[5]) {
            traced++;
            char narrow[160];
            int n = 0;
            if (line) for (; n < 150 && line[n]; n++)
                narrow[n] = (line[n] >= 32 && line[n] < 127) ? (char)line[n] : '.';
            narrow[n] = 0;
            log_line("affix trace: stat=%d pre=%u,%u,%u suf=%u,%u,%u line=\"%s\"", stat_id,
                     a[0], a[1], a[2], a[3], a[4], a[5], narrow);
            /* The published layout puts the six affix slots at +0x38 and +0x3E, and the suffix
               that lands there is right while the prefixes are not -- so the window itself is
               what has to be looked at, once per item rather than per line. */
            if (traced == 1) {
                char dump[512];
                int at = 0;
                for (int off = 0x28; off < 0x58; off += 2)
                    at += _snprintf(dump + at, sizeof dump - at, "%s%02X:%u",
                                    at ? " " : "", off, *(WORD *)(trace_data + off));
                log_line("affix window: %s", dump);
            }
        }
    }
#endif
    if (!table_rows || !line) return;

    char suffix[128];
    int used = 0;
    for (int slot = 0; slot < 6; slot++) {
        const affix_entry *e = affix_at(item, slot);
        if (!e) continue;
        char one[64];
        int n = describe(e, stat_id, one, sizeof one);
        if (n <= 0) continue;
        int written = _snprintf(suffix + used, sizeof suffix - used, "%s%s",
                                used ? ", " : "  ", one);
        if (written <= 0) break;
        used += written;
        if ((size_t)used >= sizeof suffix - 1) break;
    }
    /* The craft's own mods, after the affixes: on a crafted item they are most of why the
       number on the line is bigger than any affix could explain. */
    const affix_entry *craft = craft_for(item);
    if (craft) {
        char one[64];
        int n = describe(craft, stat_id, one, sizeof one);
        if (n > 0) {
            int written = _snprintf(suffix + used, sizeof suffix - used, "%s%s",
                                    used ? ", " : "  ", one);
            if (written > 0) used += written;
        }
    }
    if (!used) return;

    size_t at = wcslen(line);
    if (at + ESCAPE + used + ESCAPE + 1 > LINE_CAPACITY) return;

    memcpy(line + at, COLOUR_ORANGE, ESCAPE * sizeof(wchar_t));
    at += ESCAPE;
    for (int i = 0; i < used; i++) line[at + i] = (wchar_t)(unsigned char)suffix[i];
    at += used;
    memcpy(line + at, COLOUR_RESTORE, ESCAPE * sizeof(wchar_t));
    line[at + ESCAPE] = 0;
}

/* --- installing -------------------------------------------------------------------------------- */

/* BH.dll is loaded by Project Diablo 2 well after this plugin lands, so this is retried from the
   main loop until it reports success rather than run once at startup. */
static void write_u32(BYTE *at, DWORD value) { memcpy(at, &value, sizeof value); }

BOOL affixes_hook_install(void)
{
    static BOOL done;
    if (done) return TRUE;
    if (!table_rows) return FALSE;   /* nothing to print; leave BH's code alone entirely */

    HMODULE bh = GetModuleHandleA("BH.dll");
    if (!bh) return FALSE;

    BYTE *target = (BYTE *)GetProcAddress(bh, BH_EXPORT);
    if (!target) {
        log_line("affix ranges: BH.dll exports no %s -- standing down", BH_EXPORT);
        done = TRUE;
        return TRUE;
    }
    if (memcmp(target, PROLOGUE_EXPECTED, STOLEN) != 0) {
        log_line("affix ranges: REFUSING to hook %s at %p -- its prologue is not the one these "
                 "six bytes were read from", BH_EXPORT, (void *)target);
        done = TRUE;
        return TRUE;
    }

    BYTE *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log_line("affix ranges: could not allocate a trampoline");
        return FALSE;
    }
    memcpy(tramp, PROLOGUE_EXPECTED, STOLEN);
    tramp[STOLEN] = 0xE9;                                   /* jmp rel32 back into the body */
    write_u32(tramp + STOLEN + 1, (DWORD)(target + STOLEN) - (DWORD)(tramp + STOLEN + 5));
    original = (append_range_fn)tramp;

    DWORD previous;
    if (!VirtualProtect(target, STOLEN, PAGE_EXECUTE_READWRITE, &previous)) {
        log_line("affix ranges: could not make %p writable", (void *)target);
        return FALSE;
    }
    target[0] = 0xE9;
    write_u32(target + 1, (DWORD)(BYTE *)on_stat_line - (DWORD)(target + 5));
    memset(target + 5, 0x90, STOLEN - 5);                   /* pad the spare byte */
    VirtualProtect(target, STOLEN, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), target, STOLEN);

    log_line("affix ranges: hooked %s at %p, trampoline at %p", BH_EXPORT, (void *)target,
             (void *)tramp);
    done = TRUE;
    return TRUE;
}
