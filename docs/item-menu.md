# Item menu

Hover over an item and press LB on the pad (the front bumper) or ~ on the keyboard. A menu opens beside the item with what can be done to it right there: craft it with each of its recipes, corrupt it, or sell it. Nothing has to be carried to the cube, the stash or a vendor first.

The crafts and corruption are [Craft from the item](craft-from-item.md); this page is the menu, and selling.

## The menu

The menu opens over an item in a panel: the inventory, the cube or the stash.

- **Rows.** Only what can be done to this item is listed; an item with nothing gets "No actions available".
  - Sell comes first, with the price. It is left out for an item the game will not buy.
  - Then one line per recipe family the item's type has: the family name, its three ingredients drawn as the game's own inventory pictures, each with how many there are to take, and what the recipe is sure to add, with its ranges.
  - The last line is Corrupt, with its Worldstone Shard and the chance of no outcome. An item already corrupted has no Corrupt line.
- **Colours.** A line that cannot be done is grey, with the reason in place of what it adds, and a missing ingredient's count is red. Anything that blocks every craft is written in red at the top: no cube, the cube in the stash away from it, the cube not empty, or something on the cursor. None of that stops a sale.
- **Controls.** Up and down (D-pad or arrows) or the mouse choose a line, and A, Enter or a click does it. Y on the pad (triangle on a PlayStation pad) or Space sells at once, whichever line is chosen. B, LB, Esc or ~ close the menu.
- **Input.** While the menu is open the game gets neither the pad nor those keys. Afterwards a line with the outcome shows for three seconds.

The lines come from the server. Opening the menu sends a question (packet 0x4F, button 0x7A00), and the server answers into memory the client reads (`craft.h`). The rules it answers with are the ones the craft and the sale themselves apply, so what is offered is what will work.

## Selling

**At a vendor** (trade window open) the sale is the game's own packet 0x33 to that vendor, through PD2's wrapper (`+0x3955A0`) and D2Game's sale (`+0xBF990`). The vendor's price and shelf apply. Only inventory items are sold this way, as with a drag to the vendor.

**Anywhere else** the plugin runs D2Game's sale itself, minus the vendor. The rules are copied from `+0xBF990` for an item stored in a grid:

- **Refused** unless the item is the player's, it lies in the inventory, the cube, or the stash with the stash open, and D2Common #10705 says it can be sold. #10705 fails for an item flagged "can't sell" and for a quest item. Such an item gets no Sell line at all.
- **Price** is D2Common #10107(player, item, difficulty, quest flags, vendor class, 1), with Charsi's class (154). D2 1.13's vendors share their price rules; they differ in what they stock.
- **Scrolls and tomes** (ItemTypes 22 and 18) are left to a vendor: the vendor's sale also takes back the matching skill's charges, and that part is not copied. Their line is grey and says "at a vendor".
- **Ethereal, personalised, broken, socketed-and-filled and "Cracked" items** sell normally. In D2Game those conditions only keep an item off the vendor's shelf; they never refuse the sale.
- **PD2's own rule** is kept: with player data `+0x269` set, a charm below the inventory's fourth row is not sold.
- **The sale itself** is two D2Game routines. `+0xD39B0` (eax item, ecx player, game on the stack) takes the item out of the inventory, tells the client and frees it. PD2 calls the same routine when a rune from the inventory goes into a material tab. `+0x12DF0` (esi player, game and gold on the stack) pays. It pays up to the gold the player can carry and drops the rest at their feet, as the vendor does. The menu says so beforehand.
- **The sound.** At a vendor, the coin sound comes with the vendor's answer, server packet 0x2A. On "sold" the client's handler (D2Client `+0xAF9B0`, by way of `+0x4BC80`) plays sound 0xDD through D2Client `+0x88A70`, which takes the sound id in ebx and four zeros on the stack. A sale away from a vendor has no such packet. Faking one would write into the trade window's state, so the client plays sound 0xDD itself, with the same call.

Each sale is logged in `pd2addons.log` under `craft:` with the item, where it lay, the price and the gold before and after.

## Drawing it, under D2GL

- **When.** The menu goes out just before the cursor: D2Client draws the cursor with D2gfx #10041 at the mouse, from `+0x168FC`, as the frame's very last call. Drawn where the Holy Grail beam is drawn (#10054), the inventory panel painted over its pictures.
- **The cursor.** D2GL still put the cursor under the menu, however late the menu went out. So while the mouse is over the menu, the cursor is drawn a second time with D2gfx #10019, from the game's own #10041 call: the same picture, frame and place.
- **Rectangles** (D2gfx #10014) are drawn over text. A blended rectangle only darkens, whatever its colour. So the chosen line is a solid frame around the line, in palette index 0x58 (160,120,64 in ACT1's pal.dat), and never a fill: a fill hid the line's own words.
- **Pictures.** D2Win #10111 loads `data\global\items\<invfile>`, and D2gfx #10019 draws it with `-1, 5, 4`, as the game does for its skill button.
- **The hovered item** is D2's `SelectedInvItem`, or PD2's hovered unit (`+0x4ABE88`) when it is an item in a panel.

## Checked against

Every address is compared against the bytes it was read from before it is used. A PD2 update turns selling away from a vendor (or the sound) off and says so in the log, rather than calling into the wrong place.
