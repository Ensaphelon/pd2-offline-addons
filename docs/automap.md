# The automap open on joining

D2 shuts the automap at the start of every game, so with PD2's corner mini map (`automapmode`
1 in `ProjectDiablo.json`) the first thing each game asked for was a press of its key. Neither PD2
nor BH has an option to keep it open.

## Why it closes

The handler for the server's game-logon packet (0x01, `D2Client+0xAFA90`) ends by jumping into
the function that zeroes all 38 UI flags (`D2Client+0xC3540`). The automap's is flag 0x0A,
`D2Client+0xFADA8`, and that function and the toggle are the only two places in D2Client that
store to it.

## How it is opened

By calling the game's own toggle, `D2Client+0x3B8B0` — the function the automap key's entry in
D2Client's hotkey table points at. Writing the flag would show the map, but the toggle does two
more things on the way from shut to open:

- closes any panel that would sit over the map;
- registers five keydown handlers on the game window, listed at `D2Client+0xD0780`: the four
  arrows and End, which pan and re-centre the map.

With only the flag set those keys would stay dead until the map had been closed and reopened by
hand.

The call is made from the frame hook ([`frame.c`](../src/frame.c)), on the game's own thread
between two frames — where a key press would have run it — on the first frame that has a player
unit after one that did not. The logon packet that shuts the map arrives before the one that
assigns the player (0x59), so by then the map has been shut for this game and nothing will shut it
again.

Once per game, and only if the map is shut: one closed by hand stays closed until the next game.
The toggle's first bytes are compared before the first call, the flag's address included, and a
D2Client where they differ is left alone with a line in the log.
