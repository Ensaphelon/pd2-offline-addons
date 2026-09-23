/* A hook on the one instruction where PD2 lands a damage number.
 *
 * WHY HERE, AND NOT SOMEWHERE EASIER
 * ----------------------------------
 * Two sessions in a real game settled what the plain memory watch could and could not answer:
 *
 *   * writing 1337 into the client's PlayerData+0x1AC puts 1337 on screen, so the display path
 *     is certain;
 *   * during a fight, that same struct's `pending` (+0x1A8) and `window` (+0x265) never moved —
 *     while `n` (+0x1A4) DID move from 0 to 1 on its own.
 *
 * That last detail is the useful one. Something in PD2 writes to this struct (the handshake at
 * ProjectDiablo.dll+0x23CC30, copying the launcher's `dps` setting into the gate), so the struct
 * is live and reachable — yet the damage fields stay dead through combat. Which points at the
 * damage hook running against the SERVER's copy of the player: D2 keeps separate unit lists even
 * in single player, and offline the game server runs inside this same process.
 *
 * Hooking the instruction sidesteps the question entirely. Whatever struct the damage is being
 * added to, the amount is in EBX at that moment, and EBX is ours to read.
 *
 * WHAT THE INSTRUCTION IS
 * -----------------------
 * ProjectDiablo.dll+0x26F5B6, six bytes: 01 98 a8 01 00 00 = `add [eax+0x1a8], ebx`.
 *
 * It is the last step of PD2's own damage accounting (the function starts at +0x26F4FA): the raw
 * damage arrives in 256ths, is shifted down to whole points, and is then CLAMPED to the target's
 * remaining life before landing here. So EBX is not an estimate — it is damage actually removed,
 * with overkill already taken off. That clamp is the single hardest thing to get right when
 * reinventing this by diffing monster health, and here it comes for free.
 *
 * Six bytes is a comfortable landing site: a rel32 jump is five, leaving one to pad.
 *
 * SAFETY
 * ------
 * The six bytes are compared against the expected opcode before anything is written. A wrong
 * address then costs a line in the log instead of the game. This matters more than usual here:
 * the sibling plugin took the game down once already this year by getting an address wrong, and
 * that is not a debugging loop anyone wants to be in.
 */

#include <windows.h>
#include "log.h"

/* Where the damage lands, as an offset from ProjectDiablo.dll's own base. The DLL's preferred
 * base is 0x10000000 and the instruction sits at 0x1026F5B6; resolved at runtime rather than
 * assumed, because a relocated module would otherwise be a wild write. */
#define RVA_DAMAGE_ADD 0x26F5B6

static const BYTE EXPECTED[] = { 0x01, 0x98, 0xA8, 0x01, 0x00, 0x00 };
#define PATCH_LEN (sizeof EXPECTED)

/* Written by the game's own thread from inside the trampoline, read by ours. Plain aligned
 * DWORDs: on x86 those reads and writes are atomic, and an occasional torn total is not worth a
 * lock in the middle of a damage path. */
volatile DWORD hook_damage_total;
volatile DWORD hook_hit_count;

static int installed;

static void write_u32(BYTE *at, DWORD value)
{
    memcpy(at, &value, sizeof value);
}

int hook_install(void)
{
    if (installed) return 1;

    HMODULE pd2 = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2) return 0;                      /* not loaded yet; try again next tick */

    BYTE *target = (BYTE *)pd2 + RVA_DAMAGE_ADD;

    if (memcmp(target, EXPECTED, PATCH_LEN) != 0) {
        log_line("REFUSING to hook: %p holds %02x %02x %02x %02x %02x %02x, expected "
                 "01 98 a8 01 00 00 — this is not the build these offsets came from",
                 (void *)target, target[0], target[1], target[2], target[3], target[4],
                 target[5]);
        installed = 1;                       /* refused, but do not ask again every 40ms */
        return 0;
    }

    /* The trampoline: do exactly what was there, add the same damage to our own total, then
     * carry on. Hand-assembled rather than written as a C function — GCC has no `naked` on
     * i386, and a compiler-managed prologue in the middle of someone else's damage path is a
     * worse idea than twenty-three bytes of opcode. */
    BYTE *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log_line("could not allocate a trampoline");
        return 0;
    }

    int n = 0;
    memcpy(tramp + n, EXPECTED, PATCH_LEN); n += PATCH_LEN;   /* add [eax+0x1a8], ebx */
    tramp[n++] = 0x01; tramp[n++] = 0x1D;                     /* add [disp32], ebx      */
    write_u32(tramp + n, (DWORD)&hook_damage_total); n += 4;
    tramp[n++] = 0xFF; tramp[n++] = 0x05;                     /* inc DWORD [disp32]     */
    write_u32(tramp + n, (DWORD)&hook_hit_count); n += 4;
    tramp[n++] = 0xE9;                                        /* jmp rel32 back         */
    write_u32(tramp + n, (DWORD)(target + PATCH_LEN) - (DWORD)(tramp + n + 4)); n += 4;

    DWORD previous;
    if (!VirtualProtect(target, PATCH_LEN, PAGE_EXECUTE_READWRITE, &previous)) {
        log_line("could not make %p writable", (void *)target);
        return 0;
    }
    target[0] = 0xE9;                                         /* jmp rel32 to trampoline */
    write_u32(target + 1, (DWORD)tramp - (DWORD)(target + 5));
    target[5] = 0x90;                                         /* nop, the spare sixth byte */
    VirtualProtect(target, PATCH_LEN, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), target, PATCH_LEN);

    installed = 1;
    log_line("hooked ProjectDiablo.dll+0x%x at %p, trampoline at %p", RVA_DAMAGE_ADD,
             (void *)target, (void *)tramp);
    return 1;
}
