/* Restock: the gamble window's refresh button, for an ordinary vendor too.
 *
 * Offline, Project Diablo 2's gamble window has a refresh button (and a hotkey, B by default) that
 * rolls the gambler's stock again. The trade window has nothing of the kind. This puts the same
 * button and the same key on it, and makes them restock the vendor.
 *
 * NOTHING HERE ROLLS AN ITEM
 * --------------------------
 * The game already restocks vendors by itself. D2Game+0x90320 walks every NPC record and, four
 * minutes after a vendor last restocked (`add ecx,0x3a980`, 240000 ms), sets one byte in its
 * record: +0x27, "restock pending". The next time that vendor's store is opened, OpenStore
 * (D2Game+0xBF810) sees the byte, clears it, empties the vendor's inventory (+0x902E0) and fills
 * it again (+0xBF2E0) -- the same code that stocked it in the first place. The same scheduler sets
 * the byte when an act changes while the vendor is busy talking, which is the deferred case
 * OpenStore was written for: a store opened, or reopened, with a restock waiting.
 *
 * So a restock is: set the byte, open the store again. Everything past that is the game's own
 * code doing what it does every four minutes anyway.
 *
 * HOW THE GAMBLE BUTTON WORKS, AND WHY THE SAME BUTTON IS USED
 * -----------------------------------------------------------
 * Read out of this install's ProjectDiablo.dll, both directions:
 *
 *   client  The button (drawn at +0x2EE377, hovered at +0x2EC160) and the hotkey (+0x2BCB4F) all
 *           end the same way: D2Client+0x460A0 empties the store as the client holds it -- the
 *           vanilla function, not a gamble one -- and packet 0x57 goes out carrying the NPC's id.
 *   server  0x57 is not a packet vanilla D2 has: slot 0x57 of D2Game's client-packet table
 *           (D2Game+0xFA078, eight bytes a slot) is empty as shipped, and PD2 fills it with its
 *           own handler at +0x396E90 through its patch list. That handler rolls the gambler's
 *           stock and calls the same OpenStore, with its last argument 1: the gamble window.
 *
 * Both halves are shown only while gambling, and both by the same test: whether D2Client's
 * gambling flag (D2Client+0x1197A8) is set. PD2 reads it through two pointers of its own, in its
 * own data, filled once at load -- +0x5DBE9C for the button and +0x5D8218 for the key. So the
 * client half of this is two pointers moved, not a byte of anyone's code: they point at a word
 * this file keeps instead, which says what the button should mean now. And the server half is
 * one slot in the packet table: 0x57 is answered here when the window is a trade window, and
 * handed to PD2 untouched when it is a gamble one.
 *
 * NOT ONLINE
 * ----------
 * The plugin is loaded online as well, and online that same packet would go to Project Diablo 2's
 * own server: a packet the realm's client never sends from a trade window. What is safe offline
 * is not something to try on someone's account.
 *
 * Telling the two apart needs no guess. Opening any store sends packet 0x38, and offline it is
 * answered by the D2Game inside this process -- online, never. So slot 0x38 is passed through
 * with one word written on the way: this store was opened here. The button only appears on a
 * trade window that has been through that, and the word is cleared when the store closes. In a
 * gamble window the button means exactly what PD2 made it mean, online or off.
 *
 * SAFETY
 * ------
 * Every address is checked against what is there before anything is written: slot 0x38 has to
 * hold the handler D2Game shipped, slot 0x57 something inside ProjectDiablo.dll, and both of
 * PD2's pointers the gambling flag. Anything else -- a PD2 update that moved them -- costs a line
 * in the log and this feature, not the game.
 */

#include <windows.h>
#include "d2.h"
#include "log.h"

/* --- D2Game.dll --------------------------------------------------------------------------------- */

/* {handler, flags} per client packet. In .rdata, so written through VirtualProtect, as PD2 does. */
#define RVA_CLIENT_PACKETS      0xFA078
#define SLOT_WORDS              2
#define PACKET_OPEN_STORE       0x38    /* {unit type, unit id, mode} */
#define PACKET_REFRESH          0x57    /* PD2's own: {npc unit id} */

/* What slot 0x38 holds as shipped. A fingerprint as much as a chain target: if this is not what
   is there, the table is not the one these offsets were read from. */
#define RVA_OPEN_STORE_PACKET   0xCFB30

/* OpenStore. eax = the NPC, then (game, player, free to trade, gamble window) on the stack, and
   it pops them itself. Called this way by both callers it has -- the vanilla trade/gamble path
   at D2Game+0xC01FA / +0xC026F, and PD2's refresh. */
#define RVA_OPEN_STORE          0xBF810

/* The server's own structures, as this D2Game reads them. */
#define GAME_NPC_CONTROL        0x1D24  /* -> +4: the NPC records */
#define GAME_MONSTERS           0x1320  /* 128 hash buckets; NPCs are monsters */
#define MONSTER_BUCKETS         0x7F

#define UNIT_TYPE               0x00
#define UNIT_CLASS              0x04
#define UNIT_ID                 0x0C
#define UNIT_INTERACT_ID        0x64    /* who the player is talking to */
#define UNIT_INTERACT_TYPE      0x68
#define UNIT_INTERACTING        0x6C    /* WORD */
#define UNIT_HASH_NEXT          0xE4
#define UNIT_MONSTER            1

#define NPC_RECORDS             0x40    /* D2Game+0x8F5C0 searches exactly this many */
#define NPC_RECORD_SIZE         0x44
#define RECORD_CLASS            0x00
#define RECORD_SESSIONS         0x18    /* one per player who has opened this store */
#define RECORD_SELLS            0x23    /* has a store at all -- hirelings' captains do not */
#define RECORD_RESTOCK          0x27    /* the byte the four-minute timer sets */
#define RECORD_UNIT_ID          0x40    /* OpenStore writes the NPC's id here every time */

#define SESSION_PLAYER          0x00
#define SESSION_GAMBLING        0x04    /* BYTE: OpenStore's own record of which window it opened */
#define SESSION_NEXT            0x08

/* --- ProjectDiablo.dll and D2Client.dll -------------------------------------------------------- */

#define RVA_PD2_BUTTON_GATE     0x5DBE9C
#define RVA_PD2_HOTKEY_GATE     0x5D8218
#define RVA_D2CLIENT_GAMBLING   0x1197A8
#define RVA_D2CLIENT_SHOP_OPEN  0xFADB0   /* UI state 0x0C, the NPC store panel */

typedef int (__fastcall *packet_fn)(BYTE *game, BYTE *player, BYTE *packet, DWORD size);

static BYTE *d2game;
static packet_fn pd2_refresh;
static packet_fn open_store_packet;

/* The store on screen was opened by this process's own server -- which is to say, offline. */
static volatile LONG opened_here;

/* What PD2's button and hotkey read instead of the gambling flag. */
static volatile DWORD shown;

static BOOL stood_down;

static BOOL inside(HMODULE module, const void *at)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    return (const BYTE *)at >= base && (const BYTE *)at < base + nt->OptionalHeader.SizeOfImage;
}

static BOOL write_slot(void **slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return FALSE;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return TRUE;
}

/* --- the server -------------------------------------------------------------------------------- */

static BYTE *find_monster(BYTE *game, DWORD id)
{
    BYTE *unit = *(BYTE **)(game + GAME_MONSTERS + (id & MONSTER_BUCKETS) * 4);
    for (int guard = 0; unit && guard < 4096; guard++) {
        if (*(DWORD *)(unit + UNIT_ID) == id) return unit;
        unit = *(BYTE **)(unit + UNIT_HASH_NEXT);
    }
    return NULL;
}

/* D2Game+0x8F5C0, in C: it is a linear search, and the original takes its arguments in EAX, ECX
   and ESI, which is not a call worth making for a loop this size. */
static BYTE *npc_record(BYTE *game, DWORD class_id)
{
    BYTE *control = *(BYTE **)(game + GAME_NPC_CONTROL);
    if (!control) return NULL;
    BYTE *records = *(BYTE **)(control + 4);
    if (!records) return NULL;
    for (int i = 0; i < NPC_RECORDS; i++) {
        BYTE *record = records + i * NPC_RECORD_SIZE;
        if (*(DWORD *)(record + RECORD_CLASS) == class_id) return record;
    }
    return NULL;
}

static BYTE *session_of(BYTE *record, DWORD player_id)
{
    BYTE *session = *(BYTE **)(record + RECORD_SESSIONS);
    for (int guard = 0; session && guard < 64; guard++) {
        if (*(DWORD *)(session + SESSION_PLAYER) == player_id) return session;
        session = *(BYTE **)(session + SESSION_NEXT);
    }
    return NULL;
}

/* Is this player at this vendor's trade window, right now? The first three tests are PD2's own,
   from its handler: the player is talking to a monster, and it is the one the packet names. The
   rest tie the NPC to its record two ways and ask the record which window was opened. */
static BYTE *trading_with(BYTE *game, BYTE *player, DWORD npc_id, BYTE **record_out)
{
    if (!game || !player) return NULL;
    if (!*(WORD *)(player + UNIT_INTERACTING)) return NULL;
    if (*(DWORD *)(player + UNIT_INTERACT_TYPE) != UNIT_MONSTER) return NULL;
    if (*(DWORD *)(player + UNIT_INTERACT_ID) != npc_id) return NULL;

    BYTE *npc = find_monster(game, npc_id);
    if (!npc || *(DWORD *)(npc + UNIT_TYPE) != UNIT_MONSTER) return NULL;

    BYTE *record = npc_record(game, *(DWORD *)(npc + UNIT_CLASS));
    if (!record || !record[RECORD_SELLS]) return NULL;
    if (*(DWORD *)(record + RECORD_UNIT_ID) != npc_id) return NULL;

    BYTE *session = session_of(record, *(DWORD *)(player + UNIT_ID));
    if (!session || session[SESSION_GAMBLING]) return NULL;

    *record_out = record;
    return npc;
}

/* OpenStore with a restock pending. "Free to trade" is 1 because that is what the vanilla path
   passes for a vendor standing still to talk, and the restock branch is behind it; "gamble window"
   is 0. The three pointers go in registers OpenStore preserves, and it pops its own arguments. */
static void reopen_store(BYTE *npc, BYTE *game, BYTE *player)
{
    void *fn = d2game + RVA_OPEN_STORE;
    __asm__ volatile(
        "pushl $0\n\t"
        "pushl $1\n\t"
        "pushl %2\n\t"
        "pushl %1\n\t"
        "call *%3\n\t"
        : "+a"(npc)
        : "r"(game), "r"(player), "r"(fn)
        : "ecx", "edx", "memory", "cc");
}

static int __fastcall on_refresh(BYTE *game, BYTE *player, BYTE *packet, DWORD size)
{
    BYTE *record = NULL;
    BYTE *npc = size >= 5 ? trading_with(game, player, *(DWORD *)(packet + 1), &record) : NULL;
    if (!npc) return pd2_refresh(game, player, packet, size);

    record[RECORD_RESTOCK] = 1;
    reopen_store(npc, game, player);
    log_line("restock: vendor %lu restocked", (unsigned long)*(DWORD *)(npc + UNIT_CLASS));
    return 0;
}

static int __fastcall on_open_store(BYTE *game, BYTE *player, BYTE *packet, DWORD size)
{
    InterlockedExchange(&opened_here, 1);
    return open_store_packet(game, player, packet, size);
}

static BOOL server_install(HMODULE pd2)
{
    HMODULE game = GetModuleHandleA("D2Game.dll");
    if (!game) return FALSE;
    void **table = (void **)((BYTE *)game + RVA_CLIENT_PACKETS);
    void **open_slot = &table[PACKET_OPEN_STORE * SLOT_WORDS];
    void **refresh_slot = &table[PACKET_REFRESH * SLOT_WORDS];

    if (*open_slot != (BYTE *)game + RVA_OPEN_STORE_PACKET) {
        log_line("restock: D2Game's packet 0x38 slot holds %p, not the shipped handler -- "
                 "standing down", *open_slot);
        stood_down = TRUE;
        return FALSE;
    }
    if (!*refresh_slot) return FALSE;      /* PD2 has not filled its slot yet */
    if (!inside(pd2, *refresh_slot)) {
        log_line("restock: packet 0x57 is handled at %p, outside ProjectDiablo.dll -- "
                 "standing down", *refresh_slot);
        stood_down = TRUE;
        return FALSE;
    }

    d2game = (BYTE *)game;
    open_store_packet = (packet_fn)*open_slot;
    pd2_refresh = (packet_fn)*refresh_slot;
    if (!write_slot(open_slot, (void *)on_open_store) ||
        !write_slot(refresh_slot, (void *)on_refresh)) {
        log_line("restock: could not write D2Game's packet table");
        stood_down = TRUE;
        return FALSE;
    }
    log_line("restock: packets 0x38 and 0x57 hooked, PD2's refresh kept at %p",
             (void *)pd2_refresh);
    return TRUE;
}

/* PD2 has been seen to fill its own slots at load; if it ever fills them again, it is PD2's
   handler that comes back, and that is the one to keep chaining to. */
static void server_keep(HMODULE pd2)
{
    void **table = (void **)(d2game + RVA_CLIENT_PACKETS);
    void **open_slot = &table[PACKET_OPEN_STORE * SLOT_WORDS];
    void **refresh_slot = &table[PACKET_REFRESH * SLOT_WORDS];
    if (*refresh_slot != (void *)on_refresh && inside(pd2, *refresh_slot)) {
        pd2_refresh = (packet_fn)*refresh_slot;
        write_slot(refresh_slot, (void *)on_refresh);
    }
    if (*open_slot == d2game + RVA_OPEN_STORE_PACKET)
        write_slot(open_slot, (void *)on_open_store);
}

/* --- the client -------------------------------------------------------------------------------- */

static BOOL client_install(HMODULE pd2)
{
    void *gambling = D2CLIENT_VAR(DWORD, RVA_D2CLIENT_GAMBLING);
    void **button = (void **)((BYTE *)pd2 + RVA_PD2_BUTTON_GATE);
    void **hotkey = (void **)((BYTE *)pd2 + RVA_PD2_HOTKEY_GATE);

    if (!*button || !*hotkey) return FALSE;    /* PD2 has not resolved them yet */
    if (*button != gambling || *hotkey != gambling) {
        log_line("restock: PD2's refresh gates point at %p and %p, not the gambling flag %p -- "
                 "standing down", *button, *hotkey, gambling);
        stood_down = TRUE;
        return FALSE;
    }
    shown = *(volatile DWORD *)gambling;
    *button = (void *)&shown;
    *hotkey = (void *)&shown;
    log_line("restock: PD2's refresh button and hotkey now also answer a trade window");
    return TRUE;
}

/* Every tick. The gamble case is the gambling flag exactly as PD2 read it; the trade case is a
   store panel that is open and was opened by this process's own server. The word is cleared on
   the panel CLOSING, not on its being closed: the server answers 0x38 a moment before the panel
   opens, and clearing it while still shut would lose it. */
static void client_update(void)
{
    static DWORD was_open;
    DWORD open = *D2CLIENT_VAR(volatile DWORD, RVA_D2CLIENT_SHOP_OPEN);
    DWORD gambling = *D2CLIENT_VAR(volatile DWORD, RVA_D2CLIENT_GAMBLING);
    if (was_open && !open) InterlockedExchange(&opened_here, 0);
    was_open = open;
    shown = gambling || (open && opened_here);
}

void vendor_tick(void)
{
    static BOOL server_ready, client_ready;
    if (stood_down) return;

    HMODULE pd2 = GetModuleHandleA("ProjectDiablo.dll");
    if (!pd2) return;

    if (!server_ready) server_ready = server_install(pd2);
    else server_keep(pd2);
    if (stood_down || !server_ready) return;

    if (!client_ready) client_ready = client_install(pd2);
    if (client_ready) client_update();
}
