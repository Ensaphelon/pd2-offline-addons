/* Which areas this game corrupted, taken from the game marking them.
 *
 * Project Diablo 2 corrupts a group of zones per game — the client announces it in chat
 * ("Corruption spreads in the Worldstone Keep and Throne of Destruction...") — and a corrupted
 * zone hits differently, so a DPS reading taken in one is not comparable with a reading taken in
 * the same area uncorrupted. That makes it part of what identifies a record, not a note beside
 * it, which is why this exists at all.
 *
 * HOW IT IS KNOWN, AND WHAT WAS REJECTED FIRST
 * --------------------------------------------
 * Three cheaper answers were tried against real data and each is wrong:
 *
 *   * The AREA ID does not say. A corrupted zone keeps the ordinary level id — the user's own
 *     corrupted run came back as 129/130, the plain Worldstone Keep levels.
 *   * The ANNOUNCED NAME cannot be matched to level ids. Checked across all 37 groups against
 *     the installed Levels.txt: six match nothing, Uber Tristram is matched by mistake, and PD2
 *     has renamed "Frigid Highlands" to "Rigid Highlands" — so the match misses precisely the
 *     zone that prompted this.
 *   * The MONSTER LEVEL does not say either. Corruption forces the level to 85, and Hell's
 *     Worldstone Keep 2 is already 85 in Levels.txt, so "higher than the table" is blind exactly
 *     where it is needed.
 *
 * What does say is the game itself. Disassembling this install, ProjectDiablo.dll+0x26C310 picks
 * the group (`nCorruptedZone`, a random 1..n kept at pGame+0x26F2, chosen once per game), then
 * walks that group's level list and marks each one:
 *
 *     1026c4d7  mov eax, [edi + esi*4 + 0xf0]     ; edi = pGame, esi = the level
 *     1026c4de  mov dword [eax + 0x2dc], 0x55     ; <- hooked here; 85 is the corrupted level
 *     1026c4ef  mov dword [eax + 0x2e0], 0x55
 *
 * Hooking that store reads the game's own answer instead of reconstructing it: no group table to
 * find, no struct layout to assume past what is already patched, and nothing that a PD2 rename
 * can rot. ESI is the level at that instant and ESI is all this takes.
 *
 * Ten bytes is a roomy landing site — a rel32 jump is five — and `mov` sets no flags, so the
 * trampoline has nothing to preserve beyond the registers. The bytes are compared against the
 * expected opcode before anything is written, like every other hook here: a PD2 update that
 * moves this costs a line in the log instead of the game.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "d2.h"
#include "log.h"

/* `mov dword ptr [eax+0x2dc], 0x55` */
#define RVA_CORRUPT_MARK 0x26C4DE
static const BYTE MARK_EXPECTED[] = { 0xC7, 0x80, 0xDC, 0x02, 0x00, 0x00, 0x55, 0x00, 0x00, 0x00 };
#define MARK_LEN (sizeof MARK_EXPECTED)

/* Where nCorruptedZone sits on pGame — logged beside the levels so the group the game chose can
 * be checked against the line it printed in chat. Not used for anything else. */
#define PGAME_CORRUPTED_ZONE 0x26F2

/* The game struct the marking runs against, kept so the SERVER's own Level for an area can be
 * read — pGame+0xF0 is an array of Level* indexed by level id, which is how the marking loop
 * reaches them (`mov eax, [edi + esi*4 + 0xf0]`). Published for the probe below; NULL until the
 * first game has chosen its group. */
static void *volatile hook_game;
#define PGAME_LEVELS 0xF0

/* The properties of the MAP this game instance was opened from, as {WORD param, WORD stat id}
 * followed by the value, 8 bytes per entry, ending at a zero entry.
 *
 * Established by running the same Jungle Map corrupted and plain and diffing the two (2026-09-29).
 * The plain one carried a single entry, `quantity` = 29 — the stack the map came out of. The
 * corrupted one carried `quantity` = 1 (a corrupted map does not stack with a plain one) plus
 * seven more, among them `corrupted` and `corruptor`: the very stats any corrupted item carries.
 *
 * That is what makes this the right thing to read. The calendar's own marking never touches a
 * map, and the numbers a corrupted map raises are not dependable — a corruption that grants magic
 * find moves no monster level and no density at all. The flag is in the map's own properties, and
 * this is where the game keeps them. */
#define PGAME_MAP_STATS 0x1DF8
#define MAP_STAT_ENTRIES 64

/* Levels.txt tops out around 200 ids; 256 covers it with room, and a flat array is cheaper to
 * read from the polling thread than anything cleverer. Written by the game's own thread from
 * inside the trampoline, read by ours — plain bytes, so a torn read is impossible. */
#define MAX_LEVEL 256
static volatile BYTE corrupted[MAX_LEVEL];
static volatile DWORD mark_count;
/* Bumped when a new game picks its group, so the probe below dumps an area once per GAME. It was
 * once per process, which meant a session that ran the same map twice — the whole point of the
 * comparison — only ever recorded the first. */
static volatile DWORD game_generation;

static int installed;

/* The FPU/SSE state, parked across the callback.
 *
 * pushad and pushfd save the general-purpose registers and the flags and NOTHING ELSE, and the
 * site this hooks sits between a load of xmm1 and its use:
 *
 *     1026c4d3  movss xmm1, [eax]        ; the density multiplier for a corrupted zone
 *     1026c4de  <this hook>
 *     1026c51e  mulss xmm0, xmm1         ; density * that
 *     1026c52a  mov [esi+0x2b8], eax     ; written back
 *
 * Every XMM register is caller-saved on 32-bit x86, so the callback's GetTickCount, memset,
 * _snprintf and WriteFile are all entitled to clobber xmm1 — and then the game multiplies a
 * corrupted zone's monster density by whatever was left there. It did: the user noticed density
 * dropping the day this hook shipped (2026-09-29).
 *
 * fxsave/fxrstor rather than picking out xmm1: liveness is the game's business, not something to
 * re-derive from a window of disassembly, and this hook runs about five times per game. Not used
 * for the gate hook, which fires on every blow — its callback is two stores and no calls at all.
 */
static __attribute__((aligned(16))) BYTE fpu_state[512];

/* Reading a live game's memory through ReadProcessMemory rather than testing the pointer first —
 * the same call, and the same reason, as beam.c and records.c: IsBadReadPtr and
 * VirtualQuery-then-read have each taken this game down. */
static int safe_read(const void *at, void *into, SIZE_T bytes)
{
    SIZE_T got = 0;
    if (!at) return 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, bytes, &got) && got == bytes;
}

int corrupted_is(DWORD level);

static void write_u32(BYTE *at, DWORD value)
{
    memcpy(at, &value, sizeof value);
}

/* Called from the game's own thread, once per level in the group, immediately before the game
 * marks it.
 *
 * The set is cleared when a mark arrives long after the last one. The group is written in one
 * tight loop, so its own marks land microseconds apart, while the next game's run is a whole
 * game away — which makes the gap the reliable boundary and avoids having to guess when a game
 * started from the outside. */
static void __cdecl on_level_marked(DWORD level, DWORD game)
{
    static DWORD last_mark;
    DWORD now = GetTickCount();

    if (now - last_mark > 1000) {
        memset((void *)corrupted, 0, sizeof corrupted);
        mark_count = 0;
        game_generation++;
        DWORD which = 0;
        if (game) which = *(DWORD *)((BYTE *)game + PGAME_CORRUPTED_ZONE);
        log_line("corrupted: a new game corrupts group %lu", (unsigned long)which);
    }
    last_mark = now;

    hook_game = (void *)game;
    if (level < MAX_LEVEL) {
        corrupted[level] = 1;
        mark_count++;
    }
}

/* A window of the SERVER's own Level for one area, once per area, into the log.
 *
 * Diagnostic, and here for one open question: a map corrupted with a Worldstone Shard reads as
 * NOT corrupted, because the calendar's marking loop is the only thing this hooks and a shard
 * does not go through it. Which field a corrupted map differs in is not established — the area
 * id does not say, the announced name says nothing about maps, and the monster level is already
 * at the cap for the zones where it would matter.
 *
 * So rather than guess at a field, this prints the neighbourhood of the one the calendar DOES
 * write (+0x2DC/+0x2E0, and the density at +0x2B8) for whatever area the player walks into. Play
 * a corrupted map and a plain one and the difference is in the diff. Every read goes through
 * ReadProcessMemory, so a wrong pointer costs a missing line.
 *
 * Off in a normal build: it is a question, not a feature. */
#ifndef CORRUPTED_PROBE
#define CORRUPTED_PROBE 0
#endif

int corrupted_is(DWORD level)
{
    return level < MAX_LEVEL && corrupted[level] != 0;
}

/* Everything the game marked, once, for the log. Called after the first reading in a game so the
 * ids can be checked against the group the client announced in chat. */
void corrupted_report_once(void)
{
    static DWORD reported;
    if (mark_count == 0 || reported == mark_count) return;
    reported = mark_count;

    char line[256];
    int used = 0;
    for (int level = 0; level < MAX_LEVEL; level++) {
        if (!corrupted[level]) continue;
        int wrote = _snprintf(line + used, sizeof(line) - used - 1, "%s%d", used ? "," : "", level);
        if (wrote < 0 || used + wrote >= (int)sizeof(line) - 1) break;
        used += wrote;
    }
    line[used] = '\0';
    log_line("corrupted: this game's corrupted areas are %s", line);
}

#if CORRUPTED_PROBE
/* One line per 32 words, so a whole struct fits the logger's own line budget. */
static void dump_words(const char *what, DWORD base, DWORD at, int words)
{
    for (int start = 0; start < words; start += 32) {
        DWORD block[32];
        int n = words - start < 32 ? words - start : 32;
        if (!safe_read((void *)(UINT_PTR)(base + at + start * 4), block, n * 4)) return;
        char line[640];
        int used = _snprintf(line, sizeof(line) - 1, "probe: %s +%04x ", what,
                             (unsigned)(at + start * 4));
        if (used < 0) return;
        for (int i = 0; i < n; i++) {
            int wrote = _snprintf(line + used, sizeof(line) - used - 1, "%08lx ",
                                  (unsigned long)block[i]);
            if (wrote < 0 || used + wrote >= (int)sizeof(line) - 1) break;
            used += wrote;
        }
        line[used] = '\0';
        log_line("%s", line);
    }
}

void corrupted_probe_area(DWORD level_id)
{
    static DWORD reported[MAX_LEVEL];
    DWORD level_ptr = 0, density = 0, monlvl = 0, monlvl2 = 0, id_back = 0;

    if (!hook_game || level_id >= MAX_LEVEL) return;
    if (reported[level_id] == game_generation + 1) return;
    if (!safe_read((BYTE *)hook_game + PGAME_LEVELS + level_id * 4, &level_ptr, 4)) return;
    if (level_ptr < 0x10000) return;
    reported[level_id] = game_generation + 1;

    /* The three the dump already showed to be real, read out by name so the comparison does not
     * need hex: for a plain Jungle Map they came back at exactly its Levels.txt row — density
     * 1980, monster level 87 — and +0x2C0 held the level's own id, which is what says the struct
     * is the one it is meant to be. A corrupted map should differ from its table row somewhere
     * here; that is the whole question. */
    safe_read((void *)(UINT_PTR)(level_ptr + 0x2B8), &density, 4);
    safe_read((void *)(UINT_PTR)(level_ptr + 0x2C0), &id_back, 4);
    safe_read((void *)(UINT_PTR)(level_ptr + 0x2DC), &monlvl, 4);
    safe_read((void *)(UINT_PTR)(level_ptr + 0x2E0), &monlvl2, 4);
    log_line("probe: area %lu (marked=%d) density=%lu monlvl=%lu/%lu id_back=%lu | Level %p",
             (unsigned long)level_id, corrupted_is(level_id), (unsigned long)density,
             (unsigned long)monlvl, (unsigned long)monlvl2, (unsigned long)id_back,
             (void *)(UINT_PTR)level_ptr);
    /* The whole plausible Level. The calendar writes +0x2B8/+0x2DC/+0x2E0, but a shard's mark is
     * not established at all, so nothing is assumed about where to look: two maps of the same
     * type, one corrupted and one not, and the answer is in the diff. */
    dump_words("level", level_ptr, 0x000, 256);
    /* And the per-game fields the map machinery already uses — the event type at +0x1DF4 and the
     * event state at +0x2628 both live here, so a map's own corruption may well too. */
    dump_words("game", (DWORD)(UINT_PTR)hook_game, 0x1DC0, 48);
    dump_words("game", (DWORD)(UINT_PTR)hook_game, 0x2600, 48);
}
#else
void corrupted_probe_area(DWORD level_id) { (void)level_id; }
#endif

/* The map's properties as `id:value` pairs, for the record line. Written out raw rather than
 * reduced to a yes/no here: the plugin has no tables, so WHICH stat means corrupted is a question
 * for the side that can read ItemStatCost — and the rest of the list is the map's own modifiers,
 * which are worth keeping now that they cost nothing. */
void corrupted_map_stats(char *out, int size)
{
    out[0] = '\0';
    if (!hook_game) return;

    int used = 0;
    for (int i = 0; i < MAP_STAT_ENTRIES; i++) {
        DWORD entry[2];
        if (!safe_read((BYTE *)hook_game + PGAME_MAP_STATS + i * 8, entry, sizeof entry)) return;
        if (entry[0] == 0 && entry[1] == 0) return;
        int wrote = _snprintf(out + used, size - used - 1, "%s%lu:%lu", used ? "," : "",
                              (unsigned long)(entry[0] >> 16), (unsigned long)entry[1]);
        if (wrote < 0 || used + wrote >= size - 1) return;
        used += wrote;
        out[used] = '\0';
    }
}

int corrupted_hook_install(void)
{
    if (installed) return 1;
    if (!d2.pd2mod) return 0;              /* PD2's own module is not loaded yet */

    BYTE *target = PD2MOD_PTR(RVA_CORRUPT_MARK);
    if (memcmp(target, MARK_EXPECTED, MARK_LEN) != 0) {
        log_line("corrupted: REFUSING to hook — %p does not hold the expected store, so this is "
                 "not the build these offsets came from", (void *)target);
        installed = 1;                     /* refused, but do not ask again every tick */
        return 0;
    }

    BYTE *tramp = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log_line("corrupted: could not allocate a trampoline");
        return 0;
    }

    int n = 0;
    tramp[n++] = 0x0F; tramp[n++] = 0xAE; tramp[n++] = 0x05; /* fxsave [fpu_state]    */
    write_u32(tramp + n, (DWORD)fpu_state); n += 4;
    tramp[n++] = 0x60;                                      /* pushad                 */
    tramp[n++] = 0x9C;                                      /* pushfd                 */
    tramp[n++] = 0x57;                                      /* push edi (pGame)       */
    tramp[n++] = 0x56;                                      /* push esi (the level)   */
    tramp[n++] = 0xE8;                                      /* call on_level_marked   */
    write_u32(tramp + n, (DWORD)on_level_marked - (DWORD)(tramp + n + 4)); n += 4;
    tramp[n++] = 0x83; tramp[n++] = 0xC4; tramp[n++] = 0x08; /* add esp,8             */
    tramp[n++] = 0x9D;                                      /* popfd                  */
    tramp[n++] = 0x61;                                      /* popad                  */
    tramp[n++] = 0x0F; tramp[n++] = 0xAE; tramp[n++] = 0x0D; /* fxrstor [fpu_state]   */
    write_u32(tramp + n, (DWORD)fpu_state); n += 4;
    memcpy(tramp + n, MARK_EXPECTED, MARK_LEN); n += MARK_LEN;   /* the store itself  */
    tramp[n++] = 0xE9;                                      /* jmp rel32 back         */
    write_u32(tramp + n, (DWORD)(target + MARK_LEN) - (DWORD)(tramp + n + 4)); n += 4;

    DWORD previous;
    if (!VirtualProtect(target, MARK_LEN, PAGE_EXECUTE_READWRITE, &previous)) {
        log_line("corrupted: could not make %p writable", (void *)target);
        return 0;
    }
    target[0] = 0xE9;
    write_u32(target + 1, (DWORD)tramp - (DWORD)(target + 5));
    memset(target + 5, 0x90, MARK_LEN - 5);                 /* pad the spare bytes    */
    VirtualProtect(target, MARK_LEN, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), target, MARK_LEN);

    installed = 1;
    log_line("corrupted: hooked ProjectDiablo.dll+0x%x at %p, trampoline at %p",
             RVA_CORRUPT_MARK, (void *)target, (void *)tramp);
    return 1;
}
