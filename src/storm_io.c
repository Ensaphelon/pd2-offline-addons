/* Storm's background reader, without the brake on it.
 *
 * WHAT WAS SLOW
 * -------------
 * Leaving a game in its first seconds hung for several before the menu came back. Measured by
 * sampling every thread during the exit (hangprobe.c): the main thread spent the first stretch in
 * ProjectDiablo.dll's sound code (client/Sound/SoundSample.cpp), freeing each sound sample in turn,
 * and for a sample still being loaded Fog waits for the load with no timeout (Fog+1E0CB,
 * WaitForSingleObject(..., INFINITE)) -- rightly, since the read is still writing into it.
 *
 * The loads were still in flight because Storm's async reader is throttled. Its thread
 * (Storm+29200) takes the next non-urgent request no sooner than
 *
 *     chunk * 1000 / rate   milliseconds after the last,
 *
 * and Fog sets both at startup (Fog+1DF3B): Storm #285(0x4000), a 16 KB chunk, and
 * Storm #284(0x40000), 256 KB a second. That is one request every 62 ms -- sixteen sound files a
 * second -- so after entering a game the queue takes the better part of a minute to drain, and an
 * exit before then waits on its tail. The sampler caught the reader spending most of the exit in
 * that pacing wait (Storm+29531), not in reading.
 *
 * THE FIX
 * -------
 * Storm's own setter, with 0: the reader skips the division when the rate is 0 (`test ecx,ecx /
 * je`) and the pause comes out as nothing. No code is patched. It is set once a second rather than
 * once, so that if anything sets Fog's 256 KB/s again, it lasts a second at most.
 *
 * The throttle was for the hard drives and CD-ROMs of 2000, where background reads competing with
 * the game's own made it stutter.
 */
#include <windows.h>
#include <string.h>
#include "log.h"

#define STORM_SET_ASYNC_RATE 284    /* void __stdcall(DWORD bytes_per_second); 0 = no limit */
#define RESET_MS             1000

typedef void(__stdcall *set_rate_fn)(DWORD);

/* `mov eax,[esp+4] / mov [rate],eax / ret 4` -- the whole of the setter in this Storm. The four
   bytes of the variable's address are not compared: they are the one part a rebuild may move. */
static const BYTE SETTER_HEAD[] = { 0x8B, 0x44, 0x24, 0x04, 0xA3 };
static const BYTE SETTER_TAIL[] = { 0xC2, 0x04, 0x00 };

void storm_io_tick(void)
{
    static set_rate_fn set_rate;
    static BOOL checked, said;
    static DWORD last;

    DWORD now = GetTickCount();
    if (checked && !set_rate) return;
    if (now - last < RESET_MS) return;
    last = now;

    if (!checked) {
        HMODULE storm = GetModuleHandleA("Storm.dll");
        if (!storm) return;
        checked = TRUE;
        BYTE *fn = (BYTE *)GetProcAddress(storm, MAKEINTRESOURCEA(STORM_SET_ASYNC_RATE));
        if (!fn || memcmp(fn, SETTER_HEAD, sizeof SETTER_HEAD) ||
            memcmp(fn + sizeof SETTER_HEAD + 4, SETTER_TAIL, sizeof SETTER_TAIL)) {
            log_line("storm io: Storm #%d is not the async rate setter this was written for -- "
                     "leaving the reader as it is", STORM_SET_ASYNC_RATE);
            return;
        }
        set_rate = (set_rate_fn)fn;
    }
    set_rate(0);
    if (!said) {
        log_line("storm io: background reads unthrottled (Storm #%d set to 0, was Fog's 256 KB/s)",
                 STORM_SET_ASYNC_RATE);
        said = TRUE;
    }
}
