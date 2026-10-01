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
#include <stddef.h>
#include "log.h"

/* Where the damage lands, as an offset from ProjectDiablo.dll's own base. The DLL's preferred
 * base is 0x10000000 and the instruction sits at 0x1026F5B6; resolved at runtime rather than
 * assumed, because a relocated module would otherwise be a wild write. */
#define RVA_DAMAGE_ADD 0x26F5B6

static const BYTE EXPECTED[] = { 0x01, 0x98, 0xA8, 0x01, 0x00, 0x00 };
#define PATCH_LEN (sizeof EXPECTED)

/* The gate that kept the damage path from ever reaching that instruction offline.
 *
 * ProjectDiablo.dll+0x26F527, seven bytes: 83 b9 a4 01 00 00 00 = `cmp [ecx+0x1a4], 0`, with a
 * `jbe` on the next line that skips the whole accounting when it is zero. At this point ECX is
 * the SERVER's pPlayerData — the function loaded it two instructions earlier out of the attacking
 * unit.
 *
 * Online, the client asks for the meter by sending packet 0x5C and the realm sets that flag on
 * its own copy. Offline the packet goes nowhere, the flag stays zero, and the function turns
 * around at this line — which is why the damage hook installed fine and then never fired once
 * through a whole fight.
 *
 * So this is the piece of "the server" actually worth implementing: set the flag the realm would
 * have set. Everything past the gate is PD2's own code doing PD2's own arithmetic. The same hook
 * hands us the server-side pPlayerData pointer, which is the other thing we need — the widget
 * reads the CLIENT's copy, and somebody has to carry the number across. */
#define RVA_DPS_GATE 0x26F527

static const BYTE GATE_EXPECTED[] = { 0x83, 0xB9, 0xA4, 0x01, 0x00, 0x00, 0x00 };
#define GATE_LEN (sizeof GATE_EXPECTED)

/* --- WHAT TOOK THE BLOW -----------------------------------------------------------------------
 *
 * The amount alone cannot answer the question a record is actually asked. A second worth 100k
 * reads as a good setup, and it is a different thing entirely when it was one unique absorbing a
 * whole screen of damage than when it was a pack of fallen dying to splash (user, 2026-10-01). So
 * each blow is written down with the unit it landed on.
 *
 * WHERE THE TARGET IS, AND HOW THAT WAS ESTABLISHED
 * ------------------------------------------------
 * EDI, at the hooked instruction, holds the damage context this function was handed, and
 * `[edi+0x0C]` is the unit being hit. Not a published layout - read out of this install's own
 * ProjectDiablo.dll (base 0x10000000), where the same word is used for three things only a target
 * can be:
 *
 *   1026f512  mov eax,[edi+0xc] / mov [esp+0xc],eax     ; kept in a local for the rest
 *   1026f59a  push [esp+0x14]  -> call 0x10273cb0(_, 6, 0)
 *                                                       ; stat 6 is `hitpoints`, in 256ths, and
 *   1026f5a3  shr eax,8 / cmp ebx,eax / cmovg ebx,eax   ; the damage is CLAMPED to it
 *   1026f5e9  cmp [eax],1 / mov eax,[eax+4]             ; type 1 = monster, then its txt row,
 *             cmp eax,0x3a7 / cmp eax,0x3a8             ; compared against MonStats 935/936 -
 *                                                       ; rathmaBoneClone and rathmaPoisonClone,
 *                                                       ; whose health the next lines link
 *
 * The clamp is the proof: a damage function clamps to the TARGET's remaining life and nothing
 * else's. The Rathma pair then confirms the two words read here - `+0x00` is the unit type and
 * `+0x04` is the row in its own txt file - against the game's own comparisons rather than against
 * a document, and `+0x08` is the unit id in the same published layout (UnitAny) the rest of this
 * plugin already reads `+0x14` and `+0x2C` out of.
 *
 * It also means the pointer needs no validating. The game dereferences it itself,
 * unconditionally, eight instructions earlier - the clamp call reads its stat list - so by the
 * time this runs it has already been proven to be a real unit. The null test below is insurance.
 *
 * WHY THIS IS STILL ASSEMBLY, AND WHY IT TOUCHES ONLY THREE REGISTERS
 * ------------------------------------------------------------------
 * A call into C from a trampoline in the middle of someone else's arithmetic cost this game its
 * monster density for a day: the corruption hook sits between `movss xmm1,[eax]` and
 * `mulss xmm0,xmm1`, and the callee clobbered xmm1 - every XMM register is caller-saved on 32-bit
 * x86, so nothing was being violated except an assumption. The cure there was fxsave/fxrstor.
 * The cure here is to have nothing to save: no call, so no ABI to obey, so no register the game
 * is holding can be touched by anything but these fourteen instructions.
 *
 * They push the three they use and pop them back, so none of the liveness at the site has to be
 * reasoned about correctly for this to be safe. Flags are destroyed, and that was checked at the
 * site: the next instruction to read them (0x1026f5d9) is its own `test`.
 *
 * A ring rather than a growing list: the game's thread writes, the plugin's thread reads 66 times
 * a second, and neither waits for the other. `head` only ever goes up, so a reader that fell more
 * than a ring behind can SEE that it did instead of silently reading torn entries - which is why
 * the head is published as a count rather than as an index. */
#define HOOK_TARGET_RING 1024          /* a power of two: the trampoline masks, it cannot branch */
#define HOOK_TARGET_MASK (HOOK_TARGET_RING - 1)

typedef struct {
    DWORD kind;          /* UnitAny+0x00: 0 player, 1 monster, 2 object, ... */
    DWORD txt_file_no;   /* UnitAny+0x04: the row in that kind's own table */
    DWORD unit_id;       /* UnitAny+0x08: which one of them, this game */
    DWORD damage;        /* what the blow actually removed, overkill already off */
} hook_target;

/* 16 bytes an entry, so the trampoline indexes with a shift instead of a multiply. */
hook_target hook_targets[HOOK_TARGET_RING];
volatile DWORD hook_target_head;

/* Where the target sits in the context EDI points at. Named rather than inlined so the
 * disassembly above and the bytes below cannot drift apart. */
#define CONTEXT_TARGET   0x0C
#define UNIT_KIND        0x00
#define UNIT_TXT_FILE_NO 0x04
#define UNIT_ID          0x08

/* The server's own player struct, as seen from inside the damage path. Published for the polling
 * thread; NULL until the first blow lands. */
void *volatile hook_server_player_data;

/* Written by the game's own thread from inside the trampoline, read by ours. Plain aligned
 * DWORDs: on x86 those reads and writes are atomic, and an occasional torn total is not worth a
 * lock in the middle of a damage path. */
volatile DWORD hook_damage_total;
volatile DWORD hook_hit_count;

static int installed;
static int gate_installed;

/* Called from the game's own thread, from inside the trampoline, on every damage event. Kept to
 * the two stores it needs: this runs in the middle of combat resolution. */
static void __cdecl on_dps_gate(void *player_data)
{
    if (!player_data) return;
    DWORD *gate = (DWORD *)((BYTE *)player_data + 0x1A4);
    if (*gate == 0) *gate = 1;          /* what receiving packet 0x5C would have done */
    hook_server_player_data = player_data;
}

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
    BYTE *tramp = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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

    /* ...and who it landed on. Three pushes so nothing at the site has to be dead for this to be
     * correct; see "WHAT TOOK THE BLOW" above for where the target comes from. */
    tramp[n++] = 0x50;                                        /* push eax               */
    tramp[n++] = 0x53;                                        /* push ebx               */
    tramp[n++] = 0x52;                                        /* push edx               */
    tramp[n++] = 0x8B; tramp[n++] = 0x47; tramp[n++] = CONTEXT_TARGET;   /* mov eax,[edi+0xc] */
    tramp[n++] = 0x85; tramp[n++] = 0xC0;                     /* test eax, eax          */
    tramp[n++] = 0x74;                                        /* je over the block      */
    int skip_at = n++;                                        /* rel8, filled in below  */
    int block_from = n;
    tramp[n++] = 0x8B; tramp[n++] = 0x15;                     /* mov edx, [head]        */
    write_u32(tramp + n, (DWORD)&hook_target_head); n += 4;
    tramp[n++] = 0x81; tramp[n++] = 0xE2;                     /* and edx, imm32         */
    write_u32(tramp + n, HOOK_TARGET_MASK); n += 4;
    tramp[n++] = 0xC1; tramp[n++] = 0xE2; tramp[n++] = 0x04;  /* shl edx, 4             */
    tramp[n++] = 0x81; tramp[n++] = 0xC2;                     /* add edx, imm32         */
    write_u32(tramp + n, (DWORD)hook_targets); n += 4;
    /* The damage first, out of EBX while it still holds it; EBX is then the only scratch
     * register this needs, because x86 cannot move memory to memory. */
    tramp[n++] = 0x89; tramp[n++] = 0x5A;                     /* mov [edx+12], ebx      */
    tramp[n++] = (BYTE)offsetof(hook_target, damage);
    tramp[n++] = 0x8B; tramp[n++] = 0x18;                     /* mov ebx, [eax+0x00]    */
    tramp[n++] = 0x89; tramp[n++] = 0x5A;                     /* mov [edx+0], ebx       */
    tramp[n++] = (BYTE)offsetof(hook_target, kind);
    tramp[n++] = 0x8B; tramp[n++] = 0x58; tramp[n++] = UNIT_TXT_FILE_NO;  /* mov ebx,[eax+4] */
    tramp[n++] = 0x89; tramp[n++] = 0x5A;                     /* mov [edx+4], ebx       */
    tramp[n++] = (BYTE)offsetof(hook_target, txt_file_no);
    tramp[n++] = 0x8B; tramp[n++] = 0x58; tramp[n++] = UNIT_ID;           /* mov ebx,[eax+8] */
    tramp[n++] = 0x89; tramp[n++] = 0x5A;                     /* mov [edx+8], ebx       */
    tramp[n++] = (BYTE)offsetof(hook_target, unit_id);
    /* Published only now the entry is whole. Stores are not reordered with each other on x86, so
     * a reader that sees this count sees the entry behind it. */
    tramp[n++] = 0xFF; tramp[n++] = 0x05;                     /* inc DWORD [head]       */
    write_u32(tramp + n, (DWORD)&hook_target_head); n += 4;
    tramp[skip_at] = (BYTE)(n - block_from);
    tramp[n++] = 0x5A;                                        /* pop edx                */
    tramp[n++] = 0x5B;                                        /* pop ebx                */
    tramp[n++] = 0x58;                                        /* pop eax                */

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


int hook_install_gate(void)
{
    if (gate_installed) return 1;

    HMODULE pd2 = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2) return 0;

    BYTE *target = (BYTE *)pd2 + RVA_DPS_GATE;
    if (memcmp(target, GATE_EXPECTED, GATE_LEN) != 0) {
        log_line("REFUSING to hook the gate: %p does not hold the expected cmp", (void *)target);
        gate_installed = 1;
        return 0;
    }

    BYTE *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log_line("could not allocate a gate trampoline");
        return 0;
    }

    /* pushad/pushfd around the call, and the original cmp AFTER it rather than before: that cmp
     * sets the flags the `jbe` on the next line reads, so anything of ours running between them
     * would decide someone else's branch. popfd lands before it, so the flags the game sees are
     * the ones its own instruction just set. */
    int n = 0;
    tramp[n++] = 0x60;                                        /* pushad                  */
    tramp[n++] = 0x9C;                                        /* pushfd                  */
    tramp[n++] = 0x51;                                        /* push ecx (pPlayerData)  */
    tramp[n++] = 0xE8;                                        /* call on_dps_gate        */
    write_u32(tramp + n, (DWORD)on_dps_gate - (DWORD)(tramp + n + 4)); n += 4;
    tramp[n++] = 0x83; tramp[n++] = 0xC4; tramp[n++] = 0x04;  /* add esp,4               */
    tramp[n++] = 0x9D;                                        /* popfd                   */
    tramp[n++] = 0x61;                                        /* popad                   */
    memcpy(tramp + n, GATE_EXPECTED, GATE_LEN); n += GATE_LEN;
    tramp[n++] = 0xE9;                                        /* jmp rel32 back          */
    write_u32(tramp + n, (DWORD)(target + GATE_LEN) - (DWORD)(tramp + n + 4)); n += 4;

    DWORD previous;
    if (!VirtualProtect(target, GATE_LEN, PAGE_EXECUTE_READWRITE, &previous)) {
        log_line("could not make the gate at %p writable", (void *)target);
        return 0;
    }
    target[0] = 0xE9;
    write_u32(target + 1, (DWORD)tramp - (DWORD)(target + 5));
    memset(target + 5, 0x90, GATE_LEN - 5);                   /* pad the spare bytes     */
    VirtualProtect(target, GATE_LEN, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), target, GATE_LEN);

    gate_installed = 1;
    log_line("hooked the DPS gate at %p, trampoline at %p", (void *)target, (void *)tramp);
    return 1;
}
