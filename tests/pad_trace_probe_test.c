/* The pad trace's entry probes rewrite the first bytes of seven functions inside PD2's own
 * module. A wrong stub there does not fail politely: the game dies at the first window message.
 * So the stub builder is run here against a stand-in module, with each site's real opening bytes
 * at its real offset followed by a body that returns, and the checks are the ones that matter:
 *
 *   - the function still runs and returns what it would have (registers and stack intact);
 *   - a site that opens with a relative call (the mode change) still reaches that call's target;
 *   - the probe saw the arguments: ecx/edx for the cursor move, the stack for the window
 *     procedure, the return address for the mode change.
 *
 * Build and run (32-bit, from the repository root):
 *   gcc -m32 -std=c99 -Isrc -o pad_trace_probe_test.exe tests/pad_trace_probe_test.c && ./pad_trace_probe_test.exe
 */
#include "../src/pad_trace.c"

d2_modules d2;
volatile LONG frame_serial;
void log_line(const char *fmt, ...) { (void)fmt; }
const char *log_dir(void) { return ".\\"; }
void **redirect_import(HMODULE module, const char *dll, const char *symbol, void *with, void **out)
{
    (void)module; (void)dll; (void)symbol; (void)with; (void)out;
    return NULL;
}

static int failures;
#define CHECK(cond, what) do { if (cond) printf("ok    %s\n", what); \
                               else { printf("FAIL  %s\n", what); failures++; } } while (0)

static const trace_ev *last_of(WORD kind)
{
    for (LONG i = ring_next - 1; i >= 0; i--)
        if (ring[i].kind == kind) return &ring[i];
    return NULL;
}

static void place(BYTE *module, DWORD rva, const BYTE *bytes, size_t n) { memcpy(module + rva, bytes, n); }

typedef int (__fastcall *move_fn)(int x, int y);
typedef LRESULT (__stdcall *wndproc_fn)(HWND, UINT, WPARAM, LPARAM);
typedef void (*plain_fn)(void);

static volatile LONG target_ran;

int main(void)
{
    BYTE *module = VirtualAlloc(NULL, 0x64D000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE *client = VirtualAlloc(NULL, 0x120000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!module || !client) return 2;
    pd2 = module;
    d2.d2client = (HMODULE)client;
    qpc_freq = 1000;
    *(DWORD *)(client + D2C_MOUSE_X) = 400;
    *(DWORD *)(client + D2C_MOUSE_Y) = 300;

    /* Cursor move: real prologue, then mov eax,ecx / add eax,edx / mov esp,ebp / pop ebp / ret. */
    static const BYTE move_body[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
                                      0x89, 0xC8, 0x01, 0xD0, 0x8B, 0xE5, 0x5D, 0xC3 };
    place(module, SITES[P_MOVE].rva, move_body, sizeof move_body);

    /* Window procedure: real prologue, then eax = msg, mov esp,ebp / pop ebp / ret 16. */
    static const BYTE wnd_body[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
                                     0x8B, 0x45, 0x0C, 0x8B, 0xE5, 0x5D, 0xC2, 0x10, 0x00 };
    place(module, SITES[P_WNDPROC].rva, wnd_body, sizeof wnd_body);

    /* Mode change: the real `call +0x2CADB0`, then ret. The call's target counts that it ran. */
    BYTE mode_body[6];
    memcpy(mode_body, SITES[P_MODE].expect, 5);
    mode_body[5] = 0xC3;
    place(module, SITES[P_MODE].rva, mode_body, sizeof mode_body);
    BYTE target[7] = { 0xF0, 0xFF, 0x05, 0, 0, 0, 0 };   /* lock inc dword [target_ran] */
    DWORD counter = (DWORD)(UINT_PTR)&target_ran;
    memcpy(target + 3, &counter, 4);
    place(module, 0x2CADB0, target, sizeof target);
    module[0x2CADB0 + sizeof target] = 0xC3;

    /* A site whose bytes are wrong must be refused and left untouched. */
    static const BYTE wrong[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    place(module, SITES[P_BELT].rva, wrong, sizeof wrong);

    CHECK(install_probe(P_MOVE), "cursor move probe installs");
    CHECK(install_probe(P_WNDPROC), "window procedure probe installs");
    CHECK(install_probe(P_MODE), "mode change probe installs (relative call relocated)");
    CHECK(!install_probe(P_BELT), "a site that does not open as expected is refused");
    CHECK(memcmp(module + SITES[P_BELT].rva, wrong, sizeof wrong) == 0, "and is left as it was");
    tracing = 1;

    move_fn move = (move_fn)(void *)(module + SITES[P_MOVE].rva);
    int sum = move(123, 456);
    CHECK(sum == 579, "cursor move still computes ecx+edx through the probe");
    const trace_ev *ev = last_of(EV_MOVE);
    CHECK(ev && px(ev->a) == 123 && py(ev->a) == 456, "probe saw x=ecx, y=edx");
    CHECK(ev && px(ev->b) == 400 && py(ev->b) == 300, "probe read D2's cursor");

    wndproc_fn wnd = (wndproc_fn)(void *)(module + SITES[P_WNDPROC].rva);
    LRESULT r = wnd((HWND)0x1234, WM_MOUSEMOVE, 1, MAKELPARAM(10, 20));
    CHECK(r == WM_MOUSEMOVE, "window procedure returns and pops its four arguments");
    ev = last_of(EV_MSG);
    CHECK(ev && (ev->a & 0xFFFF) == WM_MOUSEMOVE && px(ev->c) == 10 && py(ev->c) == 20,
          "probe saw msg and lParam from the stack");
    CHECK(game_hwnd == (HWND)0x1234, "the game's window is learnt from the first message");

    plain_fn mode = (plain_fn)(void *)(module + SITES[P_MODE].rva);
    mode();
    CHECK(target_ran == 1, "the relocated call reached its target");
    ev = last_of(EV_MODE);
    CHECK(ev != NULL, "mode change was recorded");
    CHECK(inc.state == 2 && strcmp(inc.file, "to-mouse-unknown") == 0,
          "a switch to mouse from an unknown caller queues a file");

    /* D2Net's SendPacket: `mov eax,[imm32]` / ret 12, probed through the general installer. */
    static volatile DWORD net_word = 0x5EED;
    BYTE *net = module + 0x600000;
    net[0] = 0xA1;
    DWORD where_word = (DWORD)(UINT_PTR)&net_word;
    memcpy(net + 1, &where_word, 4);
    net[5] = 0xC2; net[6] = 0x0C; net[7] = 0x00;
    static const BYTE opens[] = { 0xA1 };
    CHECK(install_probe_at(P_SEND, net, opens, 1, 5, "send"), "send probe installs");
    typedef DWORD (__stdcall *send_fn)(DWORD, DWORD, const BYTE *);
    static const BYTE pkt[17] = { 0x18, 0x11, 0x22, 0x33, 0x44, 1, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0 };
    DWORD got = ((send_fn)(void *)net)(sizeof pkt, 0, pkt);
    CHECK(got == 0x5EED, "SendPacket still runs its own first instruction and pops 12 bytes");
    ev = last_of(EV_SEND);
    CHECK(ev && ev->a == 17 && (ev->b & 0xFF) == 0x18 && ((const BYTE *)&ev->b)[13] == 4,
          "probe copied the packet: id, length and the buffer byte at +13");

    char line[320];
    format_event(last_of(EV_SEND), line, sizeof line);
    printf("      %s\n", line);
    format_event(last_of(EV_MOVE), line, sizeof line);
    printf("      %s\n", line);
    format_event(last_of(EV_MSG), line, sizeof line);
    printf("      %s\n", line);
    format_event(last_of(EV_MODE), line, sizeof line);
    printf("      %s\n", line);

    /* PAD_TRACE_WRITE=1 also writes the queued file to .\pad-trace\, to look at its layout. */
    if (getenv("PAD_TRACE_WRITE")) write_incident();

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
