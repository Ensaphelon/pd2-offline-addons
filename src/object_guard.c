#include <windows.h>
#include <string.h>
#include "log.h"

/* Object guard: an object with no data is answered, not crashed on.
 *
 * 2026-11-24, Harrogath: the game died reading address 0 at D2Common+0x2EADF. The function there,
 * D2Common #11059, takes a unit and, for an object (type 2), reads its ObjectData at +0x14 and the
 * Objects.txt record that starts it — no check on either:
 *
 *     6fd7ead7  cmp  [eax],2 / jne -> return 0      ; not an object: 0
 *     6fd7eadc  mov  eax,[eax+0x14]                  ; ObjectData
 *     6fd7eadf  mov  ecx,[eax]                       ; its txt record   <- crashed, eax = 0
 *     6fd7eae1  mov  eax,[ecx+0x168]
 *
 * The caller was ProjectDiablo.dll+0x2ED600, which every frame takes the unit PD2 keeps for what
 * the cursor is over (the global at +0x4ABE88, behind its hover label) and, for an object, asks
 * this. The unit in the crash dump was no live object: unit id -1, ObjectData 0 — the hovered
 * object had gone (a portal closing, an object removed) and PD2 still held the pointer.
 *
 * That pointer is PD2's to clear, and it does not. What can be done from here is make the one
 * question it asks safe: an object whose data or txt record is missing gets 0, the answer the
 * function already gives anything that is not an object. The hover label for a vanished object
 * is the only thing that can change; nothing else in the game asks about such a unit and lives.
 *
 * HOW: #11059 opens with `mov eax,[esp+4] / test eax,eax / jne`. Those first six bytes are
 * compared before anything is written, then replaced by a jump here; the trampoline runs the
 * same mov and test and goes back to the jne, whose flags they set. */

#define ORDINAL_OBJECT_FIELD 11059
#define UNIT_TYPE_OBJECT 2
#define UNIT_DATA 0x14
#define UNIT_ID 0x0C
#define UNIT_TXTFILENO 0x04

static const BYTE PROLOGUE[] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0 };
#define STOLEN (sizeof PROLOGUE)

typedef DWORD (__stdcall *object_field_fn)(void *unit);
static object_field_fn original;
static volatile LONG caught;

static DWORD __stdcall guarded(void *unit)
{
    if (unit && *(DWORD *)unit == UNIT_TYPE_OBJECT) {
        void **data = *(void ***)((BYTE *)unit + UNIT_DATA);
        if (!data || !*data) {
            LONG n = InterlockedIncrement(&caught);
            if (n <= 5)
                log_line("object guard: object class %lu (unit id %ld) has no %s; answered 0 "
                         "instead of crashing%s",
                         (unsigned long)*(DWORD *)((BYTE *)unit + UNIT_TXTFILENO),
                         (long)*(LONG *)((BYTE *)unit + UNIT_ID),
                         data ? "txt record" : "data",
                         n == 5 ? " — the last such line this session" : "");
            return 0;
        }
    }
    return original(unit);
}

/* From bootstrap, once the D2 modules are there. */
void object_guard_install(void)
{
    HMODULE common = GetModuleHandleA("D2Common.dll");
    BYTE *fn = common ? (BYTE *)GetProcAddress(common, (LPCSTR)(DWORD_PTR)ORDINAL_OBJECT_FIELD) : NULL;
    if (!fn) {
        log_line("object guard: no D2Common #%d -- standing down", ORDINAL_OBJECT_FIELD);
        return;
    }
    if (memcmp(fn, PROLOGUE, STOLEN) != 0) {
        log_line("object guard: D2Common #%d does not open as expected -- left alone",
                 ORDINAL_OBJECT_FIELD);
        return;
    }

    BYTE *tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, fn, STOLEN);
    tramp[STOLEN] = 0xE9;
    LONG back = (LONG)((fn + STOLEN) - (tramp + STOLEN + 5));
    memcpy(tramp + STOLEN + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    original = (object_field_fn)(void *)tramp;

    DWORD was;
    if (!VirtualProtect(fn, STOLEN, PAGE_EXECUTE_READWRITE, &was)) return;
    BYTE patch[STOLEN];
    LONG rel = (LONG)((BYTE *)guarded - (fn + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;
    memcpy(fn, patch, STOLEN);
    VirtualProtect(fn, STOLEN, was, &was);
    FlushInstructionCache(GetCurrentProcess(), fn, STOLEN);
    log_line("object guard: D2Common #%d answers 0 for an object with no data", ORDINAL_OBJECT_FIELD);
}
