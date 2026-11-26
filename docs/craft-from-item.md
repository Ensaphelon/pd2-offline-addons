# Craft from the item

You point at a magic or rare item, pick a recipe, and the crafted item lies where the first one lay. The ingredients come from wherever they are: the inventory, the cube, or PD2's material tab. You do not have to walk to the stash.

The recipe is picked from the [item menu](item-menu.md): LB or ~ over the item.

The recipes come from `craft.txt` beside the DLL. That file maps a base item code to a category, and a category and family to a rune and a gem. `tools/make_craft_table.py` writes it from pd2-holy-inventory's Craft Center catalog, and the weapons and quivers, which the catalog leaves out, straight from the game's CubeMain, Weapons and ItemTypes tables in `pd2data.mpq`.

## Requirements

- The Horadric Cube is empty and nothing is on the cursor.
- Away from the stash, the cube must be in the inventory. PD2 refuses the material tab otherwise. At the stash, the cube can be in the stash.
- Ingredients are taken in this order: from the inventory, from the cube, from the material tab. Items in the stash itself are taken only while the stash is open. Jewel Fragments are preferred, and a whole Jewel is used only if there are no fragments.

Everything is planned before anything moves. If an ingredient is missing, the log says so and nothing is touched. If a step fails after the item has gone into the cube, the item is put back in its slot.

## How it works

The craft runs on the game server, which offline runs inside the game process. It is a sequence of the game's own packet handlers, called with packets built by the plugin. The client therefore hears about every move just as it does when the moves are made by hand. Every step is logged in `pd2addons.log` under `craft:`.

**1. Getting onto the server thread**

- The client sends packet 0x4F (button) with button `0x7A00 + action` and the item id as the 32-bit parameter.
- D2Game's client packet table sits at `+0xFA078` and has one `{handler, flag}` entry per packet id. Entry 0x4F (D2Game `+0xD1470`) is pointed at the plugin's handler.
- That handler keeps its own buttons and passes every other button to the original.

**2. Moving items**

- D2Game's 0x19 handler (`+0xD1370`, buffer to cursor) and 0x18 handler (`+0xCF0E0`, cursor to buffer at x,y) do the moving.
- The buffers are 0 for the inventory, 3 for the cube and 4 for the stash, the same numbers as an item's page byte (ItemData `+0x45`).

**3. Interacting**

- The server lets an item be picked from the cube or the stash only while the player is interacting with something: unit `+0x6C` is set, and `+0x68` and `+0x64` give the type and id of what with.
- With nothing open, the plugin sets the state a right click on the cube would set: type 4, the cube's id. It restores the old state afterwards.
- On 2026-11-26 an item was put into a closed cube without this. It went in, because an item on the cursor passes the check too, but it could not be got out.

**4. Materials**

- PD2's packet 0x42 handler (`+0x395AC0`) takes `[WORD material][0][0][4][8]`, the same packet the manual "RT+LT+X" sends.
- `material` is the item's row among the misc items: its ItemsTxt index less the elixir's. Counted in the game's own table, there is no "Expansion" marker row, so a parse of Misc.txt is one higher past that point.
- The handler wants the player's current stash tab, player data `+0x1B4`, to be a material tab (0x0C–0x10, checked by PD2 `+0x2A6BE0`). The plugin sets one for the length of the craft and restores the old one.
- The handler also wants a cube: PD2 `+0x380FB0` finds one in the inventory, or anywhere while a PD2 flag is up. That flag is up at the stash.

**5. Transmute**

- PD2's own transmute, `+0x3670B0(game, player)`, is called directly.
- Its inputs are the player's items on page 3.
- The button's "cube or stash open" check is not involved.

**6. The result**

The only item left on page 3 is the result. It is picked and put at the original page and x,y.

## Checked against

Every address is compared against the bytes it was read from before it is used. A PD2 update turns the feature off and says so in the log, rather than calling into the wrong place.
