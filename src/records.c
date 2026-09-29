/* What the meter measured, written down beside the gear it was measured with.
 *
 * The number on screen is gone the moment the fight is. This writes each area's best reading to
 * a file that pd2-holy-inventory reads on its next scan, which is how "14,484 in Chaos Sanctum"
 * becomes a row you can compare a different setup against.
 *
 * WHAT GOES IN A LINE, AND WHO IS THE AUTHORITY FOR IT
 * ---------------------------------------------------
 *     <unix seconds>\t<character>\t<area id>\t<difficulty>\t<players>\t<peak dps>\t<guid,...>\t
 *     <corrupted>\t<map stat id:value,...>\t<seconds>\t<damage>\t<hits>\t<game started at>\t
 *     <damage per second,...>
 *
 * Tabs, because a character name can hold a space. The fields are split between the two halves by
 * who can actually know them:
 *
 *   dps, area, players   only this side knows. None of the three survive into the save file.
 *   character            read here so the reading names itself rather than being matched to
 *                        whichever save happened to be written at the same moment.
 *   difficulty           read here from the game's own GetDifficulty. The save also carries it
 *                        (byte 0xA8, one per difficulty, high bit on the active one), so the two
 *                        halves can check each other if it ever matters.
 *   guids                left empty on purpose — see "What is not written" at the end.
 *   corrupted            1 when the game corrupted this area for this game, 0 when it did not.
 *                        Read from the game marking it; see corrupted.c for why nothing cheaper
 *                        works. It covers the CALENDAR's corruption only.
 *   map stats            what the map this instance came from is worth, as `id:value`. A map
 *                        corrupted with a Worldstone Shard says so here — the calendar never
 *                        touches a map — and the ids are left for the side that can read
 *                        ItemStatCost to name.
 *   seconds/damage/hits  the run itself. Damage is PD2's own clamped number — what the blows
 *                        actually removed, overkill already off.
 *   damage per second    one figure per second of the run. Every rate anyone wants is worked out
 *                        from these on the other side, because a peak alone cannot tell a plateau
 *                        from a ten-second spike, and deciding WHICH rate matters should not mean
 *                        rebuilding a DLL.
 *   game started at      which game this fragment belongs to. Leaving a map for town and coming
 *                        back is one run of one map instance, not two — see game_started_at.
 *
 * WHEN A LINE IS WRITTEN
 * ----------------------
 * On leaving an area, and on leaving the game. Not per reading: PD2's number is a 5-second
 * cumulative mean that moves continuously through a fight, and writing every sample would be
 * thousands of lines saying the same thing more and more weakly. The best the meter reached while
 * in that area is the one worth keeping, which is also what "a record" means on the other side.
 *
 * Leaving an area is also the moment the game writes the save, so by the time the app reads this
 * line it has just re-read the gear the line is about. That timing is not a coincidence to be
 * relied on quietly — it is the whole reason the flush happens here rather than on a timer.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "d2.h"
#include "log.h"

/* Which areas this game corrupted (corrupted.c). A corrupted zone hits differently, so it is
 * part of what identifies a record rather than a note beside it. */
int corrupted_hook_install(void);
int corrupted_is(DWORD level);

/* The damage counter (hook.c). PD2's own number at the instruction that lands it, already
 * clamped to what the blow actually removed — so this is damage DEALT, with overkill taken off,
 * which is the one thing a meter cannot reconstruct afterwards.
 *
 * Its trampoline is four instructions of assembly with no call in it, so nothing there can
 * clobber a register the game is holding. It does set flags, which the original `add` also sets —
 * checked at the site: the next instruction to read flags is preceded by its own `test`, so there
 * is nothing to preserve. */
int hook_install(void);
extern volatile DWORD hook_damage_total;
extern volatile DWORD hook_hit_count;
void corrupted_report_once(void);
void corrupted_probe_area(DWORD level);
void corrupted_map_stats(char *out, int size);

/* The accumulator's own window, as PD2 keeps it. See `armed` below for what it is for. */
#define PD_WINDOW_START 0x265

#define RECORDS_NAME "pd2dpsmeter-records.txt"

/* One damage total per second of a run. Half an hour of it; a run longer than that keeps its
 * totals and stops adding detail, which is a better failure than a line nothing can read. */
#define MAX_SAMPLES 1800
#define SAMPLE_MS 1000

/* Reading a live game's memory through ReadProcessMemory rather than testing the pointer first.
 * Same call, and the same reason, as beam.c: IsBadReadPtr and VirtualQuery-then-read have each
 * taken this game down. A walk through four structures this app does not own is exactly where
 * that matters. */
static int safe_read(const void *at, void *into, SIZE_T bytes)
{
    SIZE_T got = 0;
    if (!at) return 0;
    return ReadProcessMemory(GetCurrentProcess(), at, into, bytes, &got) && got == bytes;
}

static int read_ptr(const void *at, DWORD offset, DWORD *out)
{
    DWORD value = 0;
    if (!safe_read((const BYTE *)at + offset, &value, 4)) return 0;
    /* A pointer into the bottom 64K is never a real structure; rejecting those keeps a zeroed or
     * half-built field from being followed. */
    if (value < 0x10000) return 0;
    *out = value;
    return 1;
}

/* --- Where the player is ----------------------------------------------------------------------
 *
 * UnitAny+0x2C is the path, which beam.c already reads the player's sub-tile position out of, so
 * the front of this walk is not in question. Past that it runs
 * Path -> Room1 -> Room2 -> Level -> dwLevelNo, and those four links are the one part of this
 * feature that disassembly did not pin: they are published layouts rather than instructions read
 * out of this install.
 *
 * So they are not taken on trust. Two of the links prove themselves structurally, because the
 * rooms point back:
 *
 *   Room2+0x30 is the Room1 that owns it, so a candidate pair (Path->Room1, Room1->Room2) is
 *   accepted only when following it and coming back lands on the Room1 we started from.
 *
 *   Level+0x10 is the level's first Room2, and that room's own pLevel is the level again — so a
 *   candidate Room2->Level offset is checked with itself, on a different object.
 *
 * Neither check can pass by accident on a wrong offset; a wrong one lands on some other field and
 * the round trip does not close. What is left unproven is only which word of Level holds the
 * number, and that one is logged with all its candidates the first time it resolves, so a wrong
 * pick reads as a wrong area NAME on the records page rather than as anything dangerous. Every
 * read goes through safe_read, so the cost of being wrong anywhere here is a missing line. */
#define UNIT_TYPE        0x00
#define UNIT_PATH        0x2C     /* confirmed: beam.c reads the player's position through it */
#define UNIT_ACT         0x18     /* 0..4, logged beside the level id as a sanity reading */
#define ROOM1_IN_ROOM2   0x30
#define ROOM2FIRST_IN_LEVEL 0x10

static const DWORD ROOM1_IN_PATH[]    = { 0x20, 0x1C };
static const DWORD ROOM2_IN_ROOM1[]   = { 0x10, 0x18 };
static const DWORD LEVEL_IN_ROOM2[]   = { 0x58, 0x90 };
static const DWORD LEVELNO_IN_LEVEL[] = { 0x1B0, 0x1D0, 0x1D4 };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Settled once per session and then reused: these are a property of the build, not of where the
 * player is standing. -1 means "not worked out yet". */
static int off_room1 = -1, off_room2 = -1, off_level = -1, off_levelno = -1;

/* Follow one candidate shape down to the Level, or fail. `strict` also demands that the level
 * point back through its own first room; that is the stronger check, but it rests on
 * ROOM2FIRST_IN_LEVEL being right, so the resolver tries it first and then without it rather than
 * letting one unproven offset decide whether the feature works at all. */
static int walk(const BYTE *unit, int i_room1, int i_room2, int i_level, int strict,
                DWORD *out_level)
{
    DWORD path, room1, room2, level, back;
    if (!read_ptr(unit, UNIT_PATH, &path)) return 0;
    if (!read_ptr((void *)(UINT_PTR)path, ROOM1_IN_PATH[i_room1], &room1)) return 0;
    if (!read_ptr((void *)(UINT_PTR)room1, ROOM2_IN_ROOM1[i_room2], &room2)) return 0;
    /* The room pair has to close the loop. */
    if (!read_ptr((void *)(UINT_PTR)room2, ROOM1_IN_ROOM2, &back)) return 0;
    if (back != room1) return 0;
    if (!read_ptr((void *)(UINT_PTR)room2, LEVEL_IN_ROOM2[i_level], &level)) return 0;
    if (strict) {
        /* And so does the level, through its own first room, using the same offset on a different
         * object — which is what makes this a check rather than a restatement. */
        DWORD first_room2 = 0, level_again = 0;
        if (!read_ptr((void *)(UINT_PTR)level, ROOM2FIRST_IN_LEVEL, &first_room2)) return 0;
        if (!read_ptr((void *)(UINT_PTR)first_room2, LEVEL_IN_ROOM2[i_level], &level_again))
            return 0;
        if (level_again != level) return 0;
    }
    *out_level = level;
    return 1;
}

/* The level the player is standing in, or 0 when it cannot be established. */
static DWORD current_area(const BYTE *unit)
{
    DWORD level = 0;

    if (off_room1 >= 0) {
        if (!walk(unit, off_room1, off_room2, off_level, 0, &level)) return 0;
        DWORD id = 0;
        if (!safe_read((const BYTE *)(UINT_PTR)level + LEVELNO_IN_LEVEL[off_levelno], &id, 4))
            return 0;
        return (id >= 1 && id <= 1000) ? id : 0;
    }

    for (int strict = 1; strict >= 0; strict--)
    for (int a = 0; a < COUNT(ROOM1_IN_PATH); a++)
    for (int b = 0; b < COUNT(ROOM2_IN_ROOM1); b++)
    for (int c = 0; c < COUNT(LEVEL_IN_ROOM2); c++) {
        if (!walk(unit, a, b, c, strict, &level)) continue;
        /* Every candidate word is logged, not just the winner: if the area names on the records
         * page come out wrong, this line is what says which of the three it should have been. */
        char seen[128];
        int used = 0;
        DWORD values[COUNT(LEVELNO_IN_LEVEL)] = { 0 };
        for (int d = 0; d < COUNT(LEVELNO_IN_LEVEL); d++) {
            safe_read((const BYTE *)(UINT_PTR)level + LEVELNO_IN_LEVEL[d], &values[d], 4);
            int wrote = _snprintf(seen + used, sizeof(seen) - used - 1, "%s+0x%x=%lu",
                                  used ? ", " : "", (unsigned)LEVELNO_IN_LEVEL[d],
                                  (unsigned long)values[d]);
            if (wrote < 0) break;
            used += wrote;
        }
        seen[used < (int)sizeof(seen) ? used : (int)sizeof(seen) - 1] = '\0';

        for (int d = 0; d < COUNT(LEVELNO_IN_LEVEL); d++) {
            if (values[d] < 1 || values[d] > 1000) continue;
            off_room1 = a; off_room2 = b; off_level = c; off_levelno = d;
            DWORD act = 0;
            safe_read(unit + UNIT_ACT, &act, 4);
            log_line("records: area chain Path+0x%x -> Room1+0x%x -> Room2+0x%x -> Level+0x%x "
                     "(%s) | act %lu | candidates %s",
                     (unsigned)ROOM1_IN_PATH[a], (unsigned)ROOM2_IN_ROOM1[b],
                     (unsigned)LEVEL_IN_ROOM2[c], (unsigned)LEVELNO_IN_LEVEL[d],
                     strict ? "the rooms and the level both pointed back"
                            : "the rooms pointed back; the level was not asked to",
                     (unsigned long)act, seen);
            return values[d];
        }
    }

    static int complained;
    if (!complained) {
        complained = 1;
        log_line("records: no candidate area chain closed its own back-pointers, with or "
                 "without the level check — readings will be written without an area");
    }
    return 0;
}

/* --- The rest of a line ----------------------------------------------------------------------- */

/* PlayerData opens with the 16-byte name, the same field the save file carries at 0x14. Checked
 * rather than trusted: a name that is not short printable ASCII means the struct is not what this
 * thinks it is, and a reading that cannot say whose it is is worth less than no reading. */
static int player_name(const BYTE *player_data, char *out, size_t size)
{
    char raw[16];
    if (!safe_read(player_data, raw, sizeof raw)) return 0;
    size_t n = 0;
    while (n < sizeof raw && raw[n] != '\0') {
        if (raw[n] < 0x20 || raw[n] > 0x7E) return 0;
        n++;
    }
    if (n == 0 || n >= sizeof raw || n + 1 > size) return 0;
    memcpy(out, raw, n);
    out[n] = '\0';
    return 1;
}

/* What /players is set to.
 *
 * found (this install, ProjectDiablo.dll, preferred base 0x10000000): the command's own two
 * sites. At 0x1023108E the value is raised — `mov ecx,[ecx+0x124]; inc ecx; mov [0x104E2500],ecx`
 * — and at 0x1023CD60 it is printed: `mov ecx,[0x104E2500]; cmp ecx,1; jle skip`, with the string
 * "players set to " loaded straight after. Two independent sites, one global.
 *
 * It matters here for a reason that is easy to miss: PD2's damage hook clamps every blow to the
 * target's REMAINING life, so on /players 1 most of a big hit is overkill nobody counts, while on
 * /players 8 the same hit lands in full against four times the life. The same gear reads
 * differently, which is why this is part of a record's identity and not a note beside it.
 *
 * Zero is what the global holds before the command is ever used, and that is /players 1. */
#define OFF_PD2MOD_PLAYERCOUNT 0x4E2500

static DWORD player_count(void)
{
    DWORD value = 0;
    if (!d2.pd2mod) return 1;
    if (!safe_read(PD2MOD_PTR(OFF_PD2MOD_PLAYERCOUNT), &value, 4)) return 1;
    if (value < 1) return 1;
    if (value > 8) return 8;
    return value;
}

static int difficulty(void)
{
    if (!d2.d2client) return -1;
    d2_get_difficulty_fn get = (d2_get_difficulty_fn)((BYTE *)d2.d2client + OFF_D2CLIENT_GETDIFFICULTY);
    BYTE value = get();
    return value <= 2 ? (int)value : -1;
}

/* --- Writing it down --------------------------------------------------------------------------
 *
 * Opened and closed per line, like the log: the app reads this file while the game holds it, and
 * an append-only file nobody keeps open is the simplest thing that survives both sides being
 * alive at once. */
static char records_path[MAX_PATH];

void records_init(void *module)
{
    char dir[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)module, dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/') len--;
    dir[len] = '\0';
    _snprintf(records_path, MAX_PATH, "%s%s", dir, RECORDS_NAME);
    records_path[MAX_PATH - 1] = '\0';
}

/* When the game now being played was entered, as unix seconds.
 *
 * A visit to an area is not a run. One map instance gets left for town and come back to, and that
 * came out as three separate runs of the same map with three separate peaks (user, 2026-09-30).
 * They are one run, fragmented — so each fragment carries the game it belongs to and the other
 * side sews them back together.
 *
 * Deliberately not joined here. A fragment is written when the area closes, which is the moment
 * the game writes the save and therefore the moment the gear behind it is certain; holding the
 * pieces until the game ends would trade that for nothing, and lose everything if the game
 * crashed. Time in town never enters it either way: nothing measures it, so there is nothing to
 * subtract.
 */
static DWORD game_started_at;

/* What has been seen since entering the area that is currently open.
 *
 * `armed` is what keeps the last area's fight from being credited to this one. PD2's number is a
 * five-second mean, so walking out of a fight and into town carries the tail of it through the
 * door — which is how a first real session put 36,544 dps in Harrogath, a town, where nothing can
 * be hit at all.
 *
 * It arms on PD2's OWN window boundary: the accumulator keeps the tick its current five-second
 * window began on, and when that value moves, that window has closed. Everything after it was
 * dealt since, so there is nothing left to carry over.
 *
 * Waiting for the number to read ZERO instead, which is what this did first, was wrong in a way
 * that only showed up in play: the average returns to zero only after a LULL, so walking into an
 * area and fighting immediately armed nothing until the fighting stopped — and the first fight,
 * the big one, was thrown away. A real Inner Cloister run recorded 6,131 when the player had
 * plainly seen more.
 *
 * The town case still holds, and for a better reason than before: nothing is hit in a town, so
 * the accumulator never runs, the window never moves, and nothing arms at all. */
static struct {
    int open;
    int armed;
    DWORD window_at_entry;
    char name[17];
    DWORD area;
    DWORD players;
    int difficulty;
    int corrupted;
    /* What the MAP this instance was opened from is worth, as `id:value` pairs — empty outside a
     * map. A corrupted map says so here and nowhere else; see corrupted.c. */
    char map_stats[192];
    DWORD best;

    /* The run itself. A peak alone cannot tell a plateau from a ten-second spike, which is the
     * question a record is actually asked (user, 2026-09-29) — so the damage is kept second by
     * second and every rate is worked out on the other side, where changing one's mind about a
     * metric does not mean rebuilding a DLL.
     *
     * Damage rather than the meter's own average: the average is a five-second smoothing that
     * carries across a door, while damage lands where it lands. The totals here need no arming
     * at all for that reason. */
    DWORD start_ms;
    DWORD damage_at_open;
    DWORD hits_at_open;
    DWORD last_sample_ms;
    DWORD last_sample_damage;
    DWORD sample_count;
    DWORD samples[MAX_SAMPLES];
} session;

static void flush(void)
{
    if (!session.open || session.best == 0 || records_path[0] == '\0') {
        session.open = 0;
        return;
    }
    /* Difficulty is part of a record's key on the other side, so a line that cannot name it would
     * only be thrown away after being written. Skipped here instead, with a reason. */
    if (session.difficulty < 0) {
        log_line("records: dropping %lu dps in area %lu — the game would not say which difficulty",
                 (unsigned long)session.best, (unsigned long)session.area);
        session.open = 0;
        return;
    }
    /* Big, and static: a half-hour run is some twelve thousand characters of samples, and this
     * runs on one thread. */
    static char line[32768];
    DWORD damage = hook_damage_total - session.damage_at_open;
    DWORD hits = hook_hit_count - session.hits_at_open;
    DWORD seconds = (GetTickCount() - session.start_ms) / 1000;
    int n = _snprintf(line, sizeof(line) - 1,
                      "%lu\t%s\t%lu\t%d\t%lu\t%lu\t\t%d\t%s\t%lu\t%lu\t%lu\t%lu\t",
                      (unsigned long)time(NULL), session.name,
                      (unsigned long)session.area, session.difficulty,
                      (unsigned long)session.players, (unsigned long)session.best,
                      session.corrupted, session.map_stats,
                      (unsigned long)seconds, (unsigned long)damage, (unsigned long)hits,
                      (unsigned long)game_started_at);
    if (n > 0) {
        for (DWORD i = 0; i < session.sample_count; i++) {
            int wrote = _snprintf(line + n, sizeof(line) - n - 2, "%s%lu",
                                  i ? "," : "", (unsigned long)session.samples[i]);
            if (wrote < 0 || n + wrote >= (int)sizeof(line) - 2) break;
            n += wrote;
        }
        line[n++] = '\n';
        line[n] = '\0';
        HANDLE h = CreateFileA(records_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line, (DWORD)n, &written, NULL);
            CloseHandle(h);
            log_line("records: %s in area %lu%s on /players %lu — peak %lu, %lu damage over "
                     "%lus in %lu hits, %lu samples",
                     session.name, (unsigned long)session.area,
                     session.corrupted ? " (corrupted)" : "", (unsigned long)session.players,
                     (unsigned long)session.best, (unsigned long)damage,
                     (unsigned long)seconds, (unsigned long)hits,
                     (unsigned long)session.sample_count);
        }
    }
    session.open = 0;
}

/* Called every tick with whatever the meter currently reads. `unit` and `player_data` are NULL
 * outside a game, which is itself a flush: the save has just been written and the character is
 * about to be somebody else. */
void records_tick(const BYTE *unit, const BYTE *player_data, DWORD average, DWORD window)
{
    if (!unit || !player_data) {
        flush();
        /* Out of a game. The next player to appear is a new one. */
        game_started_at = 0;
        return;
    }
    if (game_started_at == 0) game_started_at = (DWORD)time(NULL);

    char name[17];
    if (!player_name(player_data, name, sizeof name)) return;

    /* The best is tracked every tick, because the number moves every tick. The area is not: it
     * costs five reads through four structures and can only change when the player walks through
     * a door, so checking it a few times a second is plenty and keeps the walk out of a loop that
     * runs every 15ms for three other features. */
    static DWORD last_area_check;
    static DWORD cached_area;
    DWORD now = GetTickCount();
    if (!session.open || now - last_area_check >= 250) {
        last_area_check = now;
        cached_area = current_area(unit);
    }
    DWORD area = cached_area;

    /* A different area, or a different character, ends what was open. The player count is read
     * per tick rather than per area because /players can be changed without moving. */
    if (session.open && (session.area != area || strcmp(session.name, name) != 0)) flush();

    if (!session.open) {
        session.open = 1;
        session.armed = 0;
        session.window_at_entry = window;
        session.area = area;
        session.best = 0;
        session.start_ms = now;
        session.damage_at_open = hook_damage_total;
        session.hits_at_open = hook_hit_count;
        session.last_sample_ms = now;
        session.last_sample_damage = hook_damage_total;
        session.sample_count = 0;
        memcpy(session.name, name, sizeof name);
    }
    session.players = player_count();
    session.difficulty = difficulty();
    /* Read per tick rather than once: the mark for a game arrives with the chat announcement,
     * which can land after the player is already standing somewhere. */
    session.corrupted = corrupted_is(area);
    corrupted_map_stats(session.map_stats, sizeof session.map_stats);
    corrupted_probe_area(area);
    /* A second's worth of damage, whether or not the meter has armed: arming is about the
     * meter's smoothed average, and this is not that. */
    while (now - session.last_sample_ms >= SAMPLE_MS) {
        DWORD total = hook_damage_total;
        if (session.sample_count < MAX_SAMPLES) {
            session.samples[session.sample_count++] = total - session.last_sample_damage;
        }
        session.last_sample_damage = total;
        session.last_sample_ms += SAMPLE_MS;
    }

    if (!session.armed) {
        if (window == session.window_at_entry) return;
        session.armed = 1;
    }
    corrupted_report_once();
    if (average > session.best) session.best = average;
}

/* WHAT IS NOT WRITTEN
 * -------------------
 * The last field is for the guids of what the player had equipped, and it goes out empty. The app
 * falls back to the save for that, which is right nearly always and wrong in one case: a weapon
 * swapped mid-fight and still held when the area changes is recorded as having been worn for the
 * whole reading. Filling it properly means walking the inventory list, and that is a second walk
 * through structures this does not own, for a correction the save already makes on the next area
 * change. It is left undone deliberately rather than forgotten. */
