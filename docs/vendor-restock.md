# Vendor restock

Offline, Project Diablo 2's gamble window has a refresh button — and a hotkey, B by default — that
rolls the gambler's stock again. This puts the same button and the same key on an ordinary
vendor's trade window, where they restock the vendor.

Offline only, by construction: see [Not online](#not-online).

## Nothing here rolls an item

The game already restocks vendors by itself, and this only asks it to do so now.

D2Game+0x90320 walks every NPC record. Four minutes after a vendor last restocked —
`add ecx,0x3a980`, 240 000 ms — it sets one byte in that vendor's record: **+0x27, restock
pending**. The same walk sets it when the act changes while the vendor is busy talking. The next
time the store is opened, OpenStore (D2Game+0xBF810) finds the byte, clears it, empties the
vendor's inventory (+0x902E0) and stocks it again (+0xBF2E0): the code that stocked it the first
time.

So a restock is one byte and one call — set +0x27, open the store again — and everything past that
is the game's own four-minute restock, early.

Items sold to that vendor go with the old stock, exactly as they do when the timer fires.

## How PD2's gamble refresh works

Both halves read out of this install's `ProjectDiablo.dll`.

**Client.** The button is drawn at +0x2EE377 and hovered at +0x2EC160; the hotkey is +0x2BCB4F.
All three end the same way: D2Client+0x460A0 empties the store as the client holds it — the
vanilla function, nothing gamble-specific about it — and **packet 0x57** goes out with the NPC's
unit id, behind a 275 ms cooldown.

**Server.** 0x57 is not a packet vanilla Diablo II has. D2Game's client-packet table
(D2Game+0xFA078, `{handler, flags}`, eight bytes a slot) ships with slot 0x57 empty; PD2 fills it,
along with 34 others, from a patch list of `{module, offset, value}` rows in its own data. Its
handler (+0x396E90) checks the player is talking to the NPC the packet names, rolls the gambler's
stock and calls the same OpenStore — last argument 1, the gamble window.

OpenStore's arguments are `eax` = the NPC, then `(game, player, free to trade, gamble window)` on
the stack, popped by the callee. That is how both of its callers use it: the vanilla trade and
gamble path (D2Game+0xC01FA and +0xC026F, which differ in that last argument alone) and PD2's
refresh.

## What this changes

**Two of PD2's pointers.** The button and the hotkey are both hidden outside a gamble window by
the same test — D2Client's gambling flag, D2Client+0x1197A8 — which PD2 reads through two pointers
in its own data, filled once at load: +0x5DBE9C for the button, +0x5D8218 for the hotkey. They are
pointed at a word this plugin keeps instead. Not a byte of anyone's code is patched.

A third reader of the same flag (+0x2ED82F, laying out the panel) goes through the first pointer
too and is left as it was — it calls two getters and discards what they return.

The hotkey cannot simply be let through: unlike the button it does not check that a store is open
at all, only that the gamble flag is set. So the word is "a store panel is open" (D2Client's UI
state 0x0C, D2Client+0xFADB0), not "always".

**One slot of D2Game's packet table.** Packet 0x57 is answered here when the player's open
window is a trade window, and handed to PD2's handler untouched when it is a gamble one. Which
window it is, the server already knows: OpenStore keeps a session per player in the NPC record
(+0x18, a list of `{player id, gambling byte, next}`) and writes the byte every time it opens one.

Before writing +0x27 the handler ties everything together twice over: the player is talking to
the NPC the packet names (PD2's own three tests), the NPC's class finds its record, the record's
+0x40 — which OpenStore stamps with the NPC's id — names the same unit, the record has a store at
all (+0x23), and the player's session there says trade.

## Not online

The plugin is loaded online too, and online packet 0x57 goes to Project Diablo 2's own server —
from a trade window, which the realm's own client never sends it from. What is fine offline is not
something to try on someone's account.

Telling the two apart needs no guess. Opening any store sends packet 0x38, and offline it is
answered by the D2Game inside this process — online, never. So slot 0x38 is passed through with
one word written on the way, *this store was opened here*. The button appears on a trade window
only when that word is set, and it is cleared when the panel closes.

It is cleared on the panel *closing*, not on its being closed: the server answers 0x38 a moment
before the panel opens, and a tick in between would otherwise lose it.

In a gamble window the word plays no part: the button means what PD2 made it mean, online or off.

## Safety

Everything is checked against what is there before anything is written. Slot 0x38 has to hold
the handler D2Game shipped (D2Game+0xCFB30) — PD2 does not replace that one, which makes it a
fingerprint for the table as well as something to chain to. Slot 0x57 has to hold an address
inside `ProjectDiablo.dll`. PD2's two pointers have to point at the gambling flag. Anything else —
a PD2 update that moved them — costs a line in the log and this feature, not the game.

Should PD2 ever fill its packet slot again, the handler it puts back is the one that is chained
to from then on.
