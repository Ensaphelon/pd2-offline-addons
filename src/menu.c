#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "d2.h"
#include "log.h"

/* Putting a "Restart" line into the in-game ESC menu.
 *
 * The menu is a pair of globals in D2Client: a header carrying the entry count, and the entry
 * array. Whoever owns the menu installs it by writing two addresses into that pair. We keep our
 * own copy — the three entries that are there plus one more — and make the install sites write
 * OUR addresses instead, so the fourth line is present the first time the menu is drawn rather
 * than appearing a frame later under an already-drawn menu.
 *
 * WHO OWNS THE MENU. Not D2Client, on this install. D2Client does hold a static ESC menu and does
 * have two install sites for it, and patching those reported success and changed nothing at all —
 * because Project Diablo 2's own module overrides the whole thing. Established by logging what the
 * globals actually held when the menu opened: an address in neither D2Client nor this plugin, which
 * turned out to sit inside ProjectDiablo.dll's .data (its own "Options"/"Exit"/"ReturnToGame"
 * entries, with "PD2Hotkeys"/"PD2Options" in the submenu below them). Its install sites are the
 * ones listed in d2.h, and they are what this patches; D2Client's remain as the fallback for a
 * plain 1.13c install with no mod.
 *
 * WHY THE COPY IS KEPT IN SYNC. The owner initialises its own static entries at some point after
 * load (y_offset, for one, is filled in by the game rather than shipped), and after this patch the
 * globals no longer point at them, so that work would never reach the drawn menu. The watcher
 * therefore mirrors the source array whenever it changes — which is also what keeps this honest if
 * the owner ever rebuilds the menu for a reason we do not know about. */

extern BOOL __fastcall restart_pressed(d2_menu_entry *entry, void *msg);
extern void restart_set_leave_action(BOOL (__fastcall *fn)(d2_menu_entry *, void *), d2_menu_entry *entry);

#define MAX_ENTRIES 12
#define SOURCE_ENTRY_COUNT 3

/* Where our line goes: after "Save and Exit Game", so the menu reads Options / Save and Exit /
   Restart / Return to Game.

   NOT last, and that part is not a preference: pressing Escape while the menu is open activates
   the LAST entry, which is how "Return to Game" doubles as the close key. With Restart sitting
   there, a second Escape restarted the game instead of closing the menu — reported from real
   play. Any index below the last one is free to choose; the last one is not. */
#define OUR_ENTRY_INDEX 2

/* Source entry 1 is "Save and Exit Game" (its cell file is "Exit"). Leaving is whatever that
   handler does, in the thread a click happens on — calling D2Client's exit function from a worker
   thread did nothing at all when this was first tried. Its index in OUR array depends on whether
   we inserted ahead of it, which `leave_entry_index` below works out rather than assumes — we now
   sit after it, so it does not move, but that is the kind of thing a later edit gets wrong. */
#define LEAVE_SOURCE_INDEX 1

static d2_menu       our_header;
static d2_menu_entry our_entries[MAX_ENTRIES];

/* The statics we mirror, once an install has been made, plus the copy we last took of them — the
   globals no longer point at the source, so a change there has to be noticed rather than seen. */
static const d2_menu       *source_header;
static const d2_menu_entry *source_entries;
static d2_menu             source_header_snapshot;
static d2_menu_entry       source_snapshot[SOURCE_ENTRY_COUNT];
static int installed;
/* Set when the register/release passes were pointed at our array too. Then the owner loads and
   frees OUR entries' label graphics, our fourth line gets one like the rest — and mirroring the
   source afterwards would copy graphic-less entries over live ones, so it stops. */
static int owner_owns_our_array;

/* --- Diagnostics ------------------------------------------------------------------------------
 * Logs the globals every time they CHANGE, naming each address — including, for an address that
 * is neither the statics nor ours, which module owns it. That is the diagnostic that found the
 * real owner of this menu after two wrong theories about it. */

static void *last_header_seen;
static void *last_entries_seen;

static const char *owner_of(const void *address, char *buffer, size_t size)
{
    HMODULE module = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module) {
        char path[MAX_PATH] = {0};
        if (GetModuleFileNameA(module, path, sizeof(path))) {
            const char *slash = strrchr(path, '\\');
            snprintf(buffer, size, "%s+%lx", slash ? slash + 1 : path,
                     (unsigned long)((const BYTE *)address - (const BYTE *)module));
            buffer[size - 1] = 0;
            return buffer;
        }
    }
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
        snprintf(buffer, size, "%s, alloc base %p",
                 info.Type == MEM_IMAGE   ? "image" :
                 info.Type == MEM_MAPPED  ? "mapped file" :
                 info.Type == MEM_PRIVATE ? "heap/private" : "unknown type",
                 info.AllocationBase);
        buffer[size - 1] = 0;
        return buffer;
    }
    return "unreadable";
}

static const char *describe(const void *address, const void *ours, char *buffer, size_t size)
{
    if (!address) return "none";
    if (address == ours) return "ours";
    return owner_of(address, buffer, size);
}

static void note_change(void)
{
    void *header = *D2CLIENT_VAR(void *, OFF_D2CLIENT_MENU);
    void *entries = *D2CLIENT_VAR(void *, OFF_D2CLIENT_MENUENTRIES);
    if (header == last_header_seen && entries == last_entries_seen) return;
    last_header_seen = header;
    last_entries_seen = entries;

    char header_owner[MAX_PATH + 32], entries_owner[MAX_PATH + 32];
    log_line("menu globals: header=%p (%s)  entries=%p (%s)  count=%lu",
             header, describe(header, &our_header, header_owner, sizeof(header_owner)),
             entries, describe(entries, our_entries, entries_owner, sizeof(entries_owner)),
             header ? (unsigned long)*(DWORD *)header : 0UL);
}

/* --- The label -------------------------------------------------------------------------------- */

/* A menu line is a pre-rendered graphic, not text: the entry names a cell file and the game loads
   data\local\ui\eng\<name>.dc6 for it. Ours says RESTART, and the app puts that graphic into an
   archive the game loads when it installs this plugin. It writes the name into a small marker file
   beside Game.exe as it does so, and that file is the ONLY reason to use the name: pointing the
   game at a graphic that is not there is not something it survives, so with no marker the entry
   keeps the label it was copied from. */
static char label_cell_file[64];

static void read_label_marker(void)
{
    /* Has to agree with game_patcher.LABEL_MARKER_NAME, which writes it. The merge renamed the
     * Python constant and left this behind, and nothing broke — because the old quick-restart
     * install had left its own marker on disk and this still found that one. A clean install is
     * what exposed it: on a machine that never had the old plugin there is only the new name,
     * this would not find it, and the menu entry would silently keep whatever label it copied. */
    FILE *file = fopen("pd2addons.label", "rb");
    if (!file) {
        log_line("label: no marker file — the entry keeps the label it copies");
        return;
    }
    size_t got = fread(label_cell_file, 1, sizeof(label_cell_file) - 1, file);
    fclose(file);
    label_cell_file[got] = 0;
    while (got && (label_cell_file[got - 1] == '\n' || label_cell_file[got - 1] == '\r' ||
                   label_cell_file[got - 1] == ' '))
        label_cell_file[--got] = 0;
    log_line("label: the entry will ask for \"%s\"", label_cell_file);
}


/* --- Our copy --------------------------------------------------------------------------------- */

/* Our line is a copy of one of the game's own — same type, same look — with the action replaced.
   Everything else in the menu keeps its order, just shifted down past the insertion point. */
static d2_menu *source_header_snapshot_ptr(void) { return &source_header_snapshot; }

static void build_our_menu(const d2_menu *header, const d2_menu_entry *entries, DWORD count)
{
    memcpy(&our_header, header, sizeof(our_header));
    memcpy(our_entries, entries, sizeof(d2_menu_entry) * OUR_ENTRY_INDEX);
    memcpy(&our_entries[OUR_ENTRY_INDEX + 1], &entries[OUR_ENTRY_INDEX],
           sizeof(d2_menu_entry) * (count - OUR_ENTRY_INDEX));

    /* Whatever the game has already loaded FOR US, kept across a re-sync — the copy below would
       otherwise hand our line the first entry's graphic again and undo the whole point. */
    void *ours_loaded = our_entries[OUR_ENTRY_INDEX].graphic;

    d2_menu_entry *ours = &our_entries[OUR_ENTRY_INDEX];
    memcpy(ours, &entries[0], sizeof(d2_menu_entry));
    ours->on_press = restart_pressed;
    ours->enable_check = NULL;
    ours->on_change = NULL;
    ours->validate_check = NULL;
    ours->y_offset = 0;
    /* The name is only worth setting if somebody is going to load it, which is true exactly when
       the register pass has been pointed at our array. Otherwise our line keeps the graphic it was
       copied with and reads the same as the game's first line — a cosmetic disappointment rather
       than a null pointer handed to the renderer, which is an access violation. */
    ours->graphic = ours_loaded;
    if (label_cell_file[0] && owner_owns_our_array) {
        memset(ours->cell_file, 0, sizeof(ours->cell_file));
        strncpy(ours->cell_file, label_cell_file, sizeof(ours->cell_file) - 1);
    }

    our_header.entry_count = count + 1;

    DWORD leave = LEAVE_SOURCE_INDEX + (LEAVE_SOURCE_INDEX >= OUR_ENTRY_INDEX ? 1 : 0);
    restart_set_leave_action(our_entries[leave].on_press, &our_entries[leave]);

    memcpy(source_header_snapshot_ptr(), header, sizeof(d2_menu));
    memcpy(source_snapshot, entries, sizeof(d2_menu_entry) * count);
}

static int write_pointer(BYTE *at, const void *value)
{
    DWORD previous = 0;
    if (!VirtualProtect(at, sizeof(void *), PAGE_EXECUTE_READWRITE, &previous)) return 0;
    memcpy(at, &value, sizeof(void *));
    VirtualProtect(at, sizeof(void *), previous, &previous);
    return 1;
}

/* --- Installing ------------------------------------------------------------------------------- */

/* Repoints one module's own install sites at our copy. Every site is checked to actually hold the
   address of that module's static menu before a byte is written: if a PD2 or game update moves any
   of this, the right outcome is no entry and a line in the log, never a write into whatever else
   now lives at that offset. */
static int install_into(HMODULE module_handle, const char *what,
                        DWORD static_menu_off, DWORD static_entries_off,
                        const DWORD *menu_sites, const DWORD *entry_sites, unsigned site_count,
                        int sites_include_register_pass)
{
    if (!module_handle) return 0;
    BYTE *module = (BYTE *)module_handle;
    const d2_menu *header = (const d2_menu *)(module + static_menu_off);
    const d2_menu_entry *entries = (const d2_menu_entry *)(module + static_entries_off);

    log_line("%s: static menu at %p count=%lu, entries at %p (first cell file \"%.16s\")",
             what, (const void *)header, (unsigned long)header->entry_count,
             (const void *)entries, entries[0].cell_file);

    if (header->entry_count != SOURCE_ENTRY_COUNT) {
        log_line("%s: expected %d entries, leaving its install sites alone",
                 what, SOURCE_ENTRY_COUNT);
        return 0;
    }
    for (unsigned i = 0; i < site_count; i++) {
        const void *at_menu = *(const void **)(module + menu_sites[i]);
        const void *at_entries = *(const void **)(module + entry_sites[i]);
        if (at_menu != (const void *)header || at_entries != (const void *)entries) {
            log_line("%s: install site %u holds %p/%p, not %p/%p — standing down",
                     what, i, at_menu, at_entries, (const void *)header, (const void *)entries);
            return 0;
        }
    }

    owner_owns_our_array = sites_include_register_pass;
    build_our_menu(header, entries, SOURCE_ENTRY_COUNT);
    for (unsigned i = 0; i < site_count; i++) {
        if (!write_pointer(module + menu_sites[i], &our_header) ||
            !write_pointer(module + entry_sites[i], our_entries)) {
            log_line("%s: install site %u could not be written", what, i);
            owner_owns_our_array = 0;
            return 0;
        }
    }

    source_header = header;
    source_entries = entries;
    installed = 1;
    log_line("%s: %u sites now point at our copy (%p / %p), %lu entries, our label is \"%.16s\"",
             what, site_count, (void *)&our_header, (void *)our_entries,
             (unsigned long)our_header.entry_count, our_entries[OUR_ENTRY_INDEX].cell_file);
    return 1;
}

/* ProjectDiablo.dll is loaded long before the interface DLLs in practice, but "in practice" is
   how the last two theories about this menu went wrong. If it is not there yet, wait a moment
   rather than silently falling through to the no-mod path and installing into a menu nobody
   draws. */
static HMODULE wait_for_pd2mod(int timeout_ms)
{
    for (int waited = 0; waited <= timeout_ms; waited += 100) {
        HMODULE module = GetModuleHandleA("ProjectDiablo.dll");
        if (module) return module;
        Sleep(100);
    }
    return NULL;
}

void menu_install(void)
{
    read_label_marker();

    /* The two that install the menu, then the two that load and free its label graphics. All four
       are checked before any one of them is written, so a layout that has moved is left alone. */
    static const DWORD pd2_menu_sites[]  = { OFF_PD2MOD_PATCH_MENU_A, OFF_PD2MOD_PATCH_MENU_B,
                                             OFF_PD2MOD_REGISTER_MENU, OFF_PD2MOD_RELEASE_MENU };
    static const DWORD pd2_entry_sites[] = { OFF_PD2MOD_PATCH_ENTRIES_A, OFF_PD2MOD_PATCH_ENTRIES_B,
                                             OFF_PD2MOD_REGISTER_ENTRIES, OFF_PD2MOD_RELEASE_ENTRIES };
    static const DWORD d2c_menu_sites[]  = { OFF_PATCH_MENU_A, OFF_PATCH_MENU_B };
    static const DWORD d2c_entry_sites[] = { OFF_PATCH_ENTRIES_A, OFF_PATCH_ENTRIES_B };

    if (!d2.pd2mod) d2.pd2mod = wait_for_pd2mod(5000);
    if (d2.pd2mod &&
        install_into(d2.pd2mod, "ProjectDiablo", OFF_PD2MOD_STATIC_MENU, OFF_PD2MOD_STATIC_ENTRIES,
                     pd2_menu_sites, pd2_entry_sites, 4, 1))
        return;

    /* No mod, or its layout has moved: a plain 1.13c install really does own its own ESC menu. */
    install_into(d2.d2client, "D2Client", OFF_D2CLIENT_STATIC_MENU, OFF_D2CLIENT_STATIC_ENTRIES,
                 d2c_menu_sites, d2c_entry_sites, 2, 0);
}


/* --- Watching --------------------------------------------------------------------------------- */

/* Once the owner's load pass has run, every line should have a graphic. If ours somehow does not —
   the archive the app wrote into is not one this install reads, say — put back the label it was
   copied with rather than leave the renderer a null pointer, which is an access violation. Best
   effort, and reported either way: the real guard is that the app reads the graphic back out of
   the archive before naming it. */
static void watch_our_label(void)
{
    static int settled;
    if (settled || !our_entries[0].graphic) return;
    settled = 1;

    d2_menu_entry *ours = &our_entries[OUR_ENTRY_INDEX];
    if (ours->graphic) {
        log_line("label: \"%.16s\" loaded, graphic %p", ours->cell_file, ours->graphic);
        return;
    }
    log_line("label: \"%.16s\" did not load — falling back to the copied label", ours->cell_file);
    memcpy(ours->cell_file, our_entries[0].cell_file, sizeof(ours->cell_file));
    ours->graphic = our_entries[0].graphic;
}


void menu_watch(void)
{
    note_change();

    if (installed && owner_owns_our_array) {
        watch_our_label();
        return;
    }

    if (installed) {
        /* Mirror the source if its owner has touched it (it still initialises its own array; the
           globals just no longer point there). Our own extra entry is rebuilt from it each time. */
        if (memcmp(source_snapshot, source_entries, sizeof(source_snapshot)) != 0 ||
            memcmp(&source_header_snapshot, source_header, sizeof(d2_menu)) != 0) {
            build_our_menu(source_header, source_entries, SOURCE_ENTRY_COUNT);
            log_line("menu: the owner changed its entries — our copy re-synced");
        }
        return;
    }

    /* Fallback for an install whose sites we could not patch: swap the globals after the menu is
       built. This is what the entry used to be added by, and it works — it is just visible, since
       the line appears a frame or so after the menu is drawn. */
    d2_menu **header_slot = D2CLIENT_VAR(d2_menu *, OFF_D2CLIENT_MENU);
    d2_menu_entry **entries_slot = D2CLIENT_VAR(d2_menu_entry *, OFF_D2CLIENT_MENUENTRIES);
    d2_menu *header = *header_slot;
    d2_menu_entry *entries = *entries_slot;

    if (!header || !entries) return;          /* nothing open */
    if (entries == our_entries) return;       /* already ours */

    /* Only the main ESC menu, which is the three-line one. Its submenus (Options and friends) are
       a different list living in the same globals, and a Restart line has no business being in
       them. */
    DWORD count = header->entry_count;
    if (count != SOURCE_ENTRY_COUNT || count + 1 > MAX_ENTRIES) return;

    build_our_menu(header, entries, count);
    *entries_slot = our_entries;
    *header_slot = &our_header;
}
