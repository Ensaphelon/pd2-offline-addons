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

/* Levels.txt tops out around 200 ids; 256 covers it with room, and a flat array is cheaper to
 * read from the polling thread than anything cleverer. Written by the game's own thread from
 * inside the trampoline, read by ours — plain bytes, so a torn read is impossible. */
#define MAX_LEVEL 256
static volatile BYTE corrupted[MAX_LEVEL];
static volatile DWORD mark_count;

static int installed;

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
        DWORD which = 0;
        if (game) which = *(DWORD *)((BYTE *)game + PGAME_CORRUPTED_ZONE);
        log_line("corrupted: a new game corrupts group %lu", (unsigned long)which);
    }
    last_mark = now;

    if (level < MAX_LEVEL) {
        corrupted[level] = 1;
        mark_count++;
    }
}

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

    BYTE *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log_line("corrupted: could not allocate a trampoline");
        return 0;
    }

    int n = 0;
    tramp[n++] = 0x60;                                      /* pushad                 */
    tramp[n++] = 0x9C;                                      /* pushfd                 */
    tramp[n++] = 0x57;                                      /* push edi (pGame)       */
    tramp[n++] = 0x56;                                      /* push esi (the level)   */
    tramp[n++] = 0xE8;                                      /* call on_level_marked   */
    write_u32(tramp + n, (DWORD)on_level_marked - (DWORD)(tramp + n + 4)); n += 4;
    tramp[n++] = 0x83; tramp[n++] = 0xC4; tramp[n++] = 0x08; /* add esp,8             */
    tramp[n++] = 0x9D;                                      /* popfd                  */
    tramp[n++] = 0x61;                                      /* popad                  */
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
