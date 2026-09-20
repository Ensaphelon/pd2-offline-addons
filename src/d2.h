#ifndef PD2RESTART_D2_H
#define PD2RESTART_D2_H

#include <windows.h>

/* ---------------------------------------------------------------------------------------------
 * Diablo II 1.13c ONLY — Game.exe FileVersion 1.0.13.60, which is what Project Diablo 2 ships.
 * Checked at load; on anything else the plugin stands down. 1.13d moved almost everything
 * (D2Win!FirstControl alone goes 0x214A0 -> 0x8DB34), so a wrong build would not fail politely.
 *
 * Provenance of every address below is recorded on the line that defines it. Two sources:
 *
 *   BH      — Project Diablo 2's own maphack (github.com/Project-Diablo-2/BH), built against
 *             these exact DLLs. Its tables carry two columns and D2Version.h names them:
 *             VERSION_113c first, VERSION_113d second. Addresses are facts; no AGPL code here.
 *   found   — located by disassembling THIS install's D2Client.dll, then cross-checked against
 *             the 1.13d values published by D2Ex2 (Apache-2.0) for the same globals.
 * ------------------------------------------------------------------------------------------- */

#define D2_REQUIRED_VERSION "1.0.13.60"

/* --- D2Client.dll ---------------------------------------------------------------------------- */

#define OFF_D2CLIENT_EXITGAME       0x42850   /* BH.    void __fastcall(void)  — the game's own leave path */
#define OFF_D2CLIENT_GETDIFFICULTY  0x41930   /* BH.    BYTE __stdcall(void)   — 0 normal, 1 nightmare, 2 hell */

/* The ESC menu is a pair of globals: a header carrying the entry count, and the entry array.
 *
 * found: the menu code indexes entries with `imul reg, reg, 0x550` (sizeof D2MenuEntry). At
 * 0x6fb14d20 the pair is read together — `mov ecx,[0x6fbcc058]` (selected index),
 * `mov edx,[0x6fbcc060]` (entries), `imul eax,ecx,0x550`, `add eax,edx`. At 0x6fb14641 a
 * submenu is installed by writing both globals with static addresses 0x6fb92ac0 / 0x6fb92ad8 —
 * exactly 0x18 apart, which is sizeof(D2Menu). ImageBase is 0x6fab0000.
 *
 * Cross-check: in 1.13d D2Ex2 lists SelectedMenu 0x11C9E8 and D2MenuEntries 0x11C9F0 — the same
 * 8-byte relationship, with the header DWORD sitting between them, as found here. */
/* The ESC menu's own static header and entries, as shipped in D2Client's data. The game installs
   them by writing their addresses into the globals below; we read them once to build our own
   copy. */
#define OFF_D2CLIENT_STATIC_MENU    0xE2AC0
#define OFF_D2CLIENT_STATIC_ENTRIES 0xE2AD8

/* Where those two addresses are written into the code, as immediate operands. Both sites install
   the same ESC menu — one on opening it, one on coming back from a submenu. Patching the
   immediates once, at startup, means the game itself installs OUR menu from the first frame:
   no polling, nothing done when the menu opens, and no visible jump as a fourth line appears
   under an already-drawn menu.

   Each is the 4 bytes following a 6-byte `mov dword ptr [global], imm32` opcode. */
#define OFF_PATCH_MENU_A            0x64647
#define OFF_PATCH_ENTRIES_A         0x64651
#define OFF_PATCH_MENU_B            0x65B10
#define OFF_PATCH_ENTRIES_B         0x65B1A

#define OFF_D2CLIENT_SELECTEDMENU   0x11C058  /* found. int          — index of the highlighted entry */
#define OFF_D2CLIENT_MENU           0x11C05C  /* found. D2Menu*      — header; [0] is the entry count */
#define OFF_D2CLIENT_MENUENTRIES    0x11C060  /* found. D2MenuEntry* — the entries themselves */

/* --- ProjectDiablo.dll ------------------------------------------------------------------------
 * The ESC menu you actually see is NOT D2Client's. Project Diablo 2's own module keeps its own
 * header and entry array in its .data and installs them into D2Client's globals — which is why
 * patching D2Client's install sites changed nothing at all, and why a watcher that swapped the
 * globals after the fact was the only thing that ever worked.
 *
 * found (this install, ProjectDiablo.dll, preferred base 0x10000000): the globals' addresses are
 * held in the mod's own variables 0x104DE3F8 (header) and 0x104DE338 (entries), and both install
 * sites write through them with `mov dword ptr [reg], imm32`:
 *
 *   0x22FD43  mov [eax], 0x103A6FE0   |  0x230368  mov [edx], 0x103A6FE0   (a jump-table arm:
 *   0x22FD4E  mov [eax], 0x103A2FC0   |  0x23036E  mov [eax], 0x103A2FC0    0x103A6FC8 is the
 *                                     |                                     Options submenu)
 *   ^ menu init                       |  ^ switching back to the root menu
 *
 * The static entries are fully built in the file — cell files "Options"/"Exit"/"ReturnToGame",
 * on_press pointers already relocated — so our copy can be made the moment the module is loaded,
 * before the menu is ever opened. Every offset here is checked against the bytes actually found
 * at it before anything is written; a PD2 update that moves them makes the plugin fall back to
 * the watcher rather than corrupt somebody else's code. */

#define OFF_PD2MOD_STATIC_MENU      0x3A6FE0
#define OFF_PD2MOD_STATIC_ENTRIES   0x3A2FC0
#define PD2MOD_STATIC_MENU_VA       0x103A6FE0   /* as stored in the immediates, at preferred base */
#define PD2MOD_STATIC_ENTRIES_VA    0x103A2FC0

/* Each is the imm32 of a 6-byte `C7 /0 imm32`, i.e. the instruction's address + 2. */
#define OFF_PD2MOD_PATCH_MENU_A     0x22FD45
#define OFF_PD2MOD_PATCH_ENTRIES_A  0x22FD50
#define OFF_PD2MOD_PATCH_MENU_B     0x23036A
#define OFF_PD2MOD_PATCH_ENTRIES_B  0x230370

/* Installing the menu is not all of it. A separate pass registers a menu and loads a label
   graphic for each of its entries, storing the result in the entry itself (+0x53C, see the struct
   below) — and it walks the array it is HANDED, which is the module's own static one. So with only
   the install sites patched, our fourth line never got a graphic: the drawing code takes that
   pointer straight to D2CMP, and a null one is an access violation, which is exactly what
   happened. Pointing the register and release passes at our array too is what gets our own line
   loaded, and released again, along with the rest.

   found: two symmetrical sites, each an imm32 of a 5-byte `68 imm32` push (address + 1):

     0x22FC42  push entries / 0x22FC47 push header / call 0x1029E760   — register and load
     0x22FE26  push header  / 0x22FE2B push entries / call 0x102F01D0  — release (args reversed)

   Both are followed at the same call sites by the identical pair for the Options submenu, which
   is left alone. */
#define OFF_PD2MOD_REGISTER_ENTRIES 0x22FC43
#define OFF_PD2MOD_REGISTER_MENU    0x22FC48
#define OFF_PD2MOD_RELEASE_MENU     0x22FE27
#define OFF_PD2MOD_RELEASE_ENTRIES  0x22FE2C

/* --- D2Win.dll ------------------------------------------------------------------------------- */

#define OFF_D2WIN_FIRSTCONTROL      0x214A0   /* BH.    head of the out-of-game UI control list */

/* --- Structures the addresses above point at -------------------------------------------------
 * Layouts are D2's own, as published by D2Ex2 (CommonStructs.h). Only the fields this plugin
 * actually reads or writes are named; the rest is padding kept at the right size so an entry
 * copied out of the game's own array stays byte-identical. */

typedef struct d2_menu {
    DWORD entry_count;      /* 0x00 */
    DWORD interline;        /* 0x04 */
    DWORD text_height;      /* 0x08 */
    DWORD menu_offset;      /* 0x0C */
    DWORD bar_height;       /* 0x10 */
    DWORD _unused;          /* 0x14 */
} d2_menu;                  /* 0x18 */

typedef struct d2_menu_entry d2_menu_entry;
struct d2_menu_entry {
    DWORD menu_type;        /* 0x00  -1 static text, 0 selectable, 1 switch, 2 bar */
    DWORD expansion_only;   /* 0x04 */
    DWORD y_offset;         /* 0x08  filled in by the game */
    union {
        char  cell_file[260];   /* 0x0C */
        WCHAR item_name[130];
    };
    BOOL (__fastcall *enable_check)(d2_menu_entry *, DWORD item_no);  /* 0x110 false -> greyed out */
    BOOL (__fastcall *on_press)(d2_menu_entry *, void *msg);          /* 0x114 the action */
    BOOL (__fastcall *on_change)(d2_menu_entry *);                    /* 0x118 */
    BOOL (__fastcall *validate_check)(d2_menu_entry *);               /* 0x11C */
    DWORD max_value;        /* 0x120 */
    DWORD current_value;    /* 0x124 */
    DWORD bar_type;         /* 0x128 */
    BYTE  rest[0x53C - 0x12C];
    /* 0x53C  The loaded label graphic. NOT in any published layout — found by logging which words
       of the game's own entries change, and this is the only one: it goes from 0 to a heap address
       the first time the menu opens, which is also what makes it a lazy load. That matters, because
       it means clearing it is how an entry is made to load its cell file again, by name. */
    void *graphic;
    BYTE  tail[0x550 - 0x540];
};

typedef struct d2_modules {
    HMODULE d2client;
    HMODULE d2win;
    HMODULE d2launch;
    HMODULE pd2mod;     /* ProjectDiablo.dll — absent on a plain 1.13c install, which is fine */
} d2_modules;

/* Resolved after the interface DLLs are actually loaded — Game.exe brings them in at runtime
   (it holds a table of them: none/D2Client/D2Server/D2Multi/D2Launch/D2EClient), so a plugin
   that lands early must wait rather than assume. */
extern d2_modules d2;

int d2_version_is_supported(void);
int d2_wait_for_modules(int timeout_ms);

#define D2CLIENT_VAR(type, off) ((type *)((BYTE *)d2.d2client + (off)))
#define D2WIN_VAR(type, off)    ((type *)((BYTE *)d2.d2win + (off)))
#define PD2MOD_PTR(off)         ((BYTE *)d2.pd2mod + (off))

#endif

/* Out-of-game UI control, as published by D2Ex2 (CommonStructs.h). Only the fields this plugin
   reads are named; the tail is padded so pNext lands where the game puts it. */
typedef struct d2_control d2_control;
struct d2_control {
    DWORD type;             /* 0x00  6 = button */
    void *cell_file;        /* 0x04 */
    DWORD state;            /* 0x08  5 enabled, 4 disabled, below that not visible */
    DWORD x, y, w, h;       /* 0x0C..0x18 */
    void *draw, *draw_ex, *push, *mouse, *list_check, *key;  /* 0x1C..0x30 */
    BOOL (__stdcall *on_press)(d2_control *);                /* 0x34 */
    void *draw_anim;        /* 0x38 */
    d2_control *next;       /* 0x3C */
};

typedef void   (__fastcall *d2_exit_game_fn)(void);
typedef BYTE   (__stdcall  *d2_get_difficulty_fn)(void);
