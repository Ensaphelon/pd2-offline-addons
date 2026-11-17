# Affix labels

Which affixes an item rolled, and the range each one of them rolls in, in the game's own tooltip.
Magic, rare and crafted alike.

```sh
python3 -m pd2_offline_addons affixes   --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_offline_addons unaffixes --game "<path to>/ProjectD2/Game.exe"
```

Which file it writes into is PD2Launcher's own choice, read out of `launcherSettings.json`
rather than assumed. This matters: **the launcher copies the selected filter over `loot.filter`
when it starts the game**, so a block written into `loot.filter` while a *local* filter is
selected is overwritten on launch. A local selection records `selectedAuthor.url == "local"` and
an absolute path under `filters/local/`; anything else is a filter the launcher downloads, and
for those `loot.filter` is the only file worth editing. `--filter PATH` overrides.

`status` reports it out of the file itself:

```
affixes    1615 rules in local/Kassahi_Mystery_Hyper.filter
```

## What it looks like

Appended to the item's description, one line per affix:

```
pre Platinum #243       101-120 to Attack Rating
suf of Absorption #121  Physical Damage Taken Reduced by 3
pre Empowering #759     Enhanced Minimum Damage (40-60)
pre Sturdy #144         21-30 Enhanced Defense
```

One line per affix rather than per mod, deliberately: a rare carries six affixes of up to three
mods each, and a tooltip eighteen lines longer is worse than no tooltip addition. Several mods on
one affix are folded with `|` and wrapped onto a continuation line past ~100 characters, because
D2 does not wrap a tooltip line, it runs it off the edge.

## What it deliberately does not do

**Attribute a value to an affix.** The item stores one merged total per stat plus the affix ids;
the per-affix split is not stored anywhere, and when two affixes feed the same stat — routine on
rares and crafts — it cannot be recovered from the total. A filter can only print a total, so
"290 of 251-325" would be a lie on exactly the items that matter most. The rolled value is
already on the item's own lines directly above; the range is what was missing.

On one real crafted Hand Axe, two of its four affixes feed minimum and maximum damage between
them. Any per-affix number there would be invented.

**Name a crafted item's fixed recipe mods.** They are not in the affix slots at all — a Blood
craft's life, life-steal and heal-after-kill arrive from the recipe, and no rolled affix on the
item grants them. They are also the same on every craft of that family, so nothing is lost.

## Three things about the filter language this rests on

Each checked against the user's own 6000-rule filter rather than assumed.

* **`{...}` is the item description, and it shows in the inventory tooltip**, not just on the
  ground. The proof: a rule on `box` — the Horadric Cube — appends "Send Feedback via PD2
  Discord" to it, and the cube never lies on the ground.
* **`%NAME%` inside the braces is the description accumulated so far.** So a rule can append to
  it and `%CONTINUE%` on. 2513 rules in that filter do this; one puts `%NAME%` last to prepend
  instead. `%CL%` is a newline that collapses if the text is already at one.
* **An id is tested against every affix slot, not one.** The wiki's own example is
  `ItemDisplay[RARE SUFFIX=662 SUFFIX=158]`, which means nothing otherwise.

No quality condition is needed: `PREFIX`/`SUFFIX` are simply absent on set and unique items,
whose quality branch stores a set/unique id where the affixes would be. One rule set covers
magic, rare and crafted and skips everything else.

## The id numbering, and how it was settled

An affix id is a **zero-based row index** into `MagicPrefix.txt` / `MagicSuffix.txt`, each table
counted on its own. Settled from the live game, not the wiki — whose Info Codes table says
`PREFIX: 1-805` while its own worked example uses `PREFIX=1279`.

It had to be settled by **values**, because names cannot do it. Affixes come in tiers that share
a name, so reading the id as a 1-based row produced "of Nova" for `SUFFIX=873` and "of Damage
Amplification" for `=899` and `=906` — matching exactly what the season-14 filter labels those
ids (`a3 nov`, `a3 amp`) — and was still wrong by one.

What decided it: for every rare and crafted item in a real save, take each stat that exactly one
of the item's own affixes can grant, and ask whether the value falls inside that affix's
`min..max`.

| offset | inside | outside | |
|---|---|---|---|
| -1 | 11 | 85 | 11.5% |
| **0** | **69** | **33** | **67.6%** |
| +1 | 7 | 86 | 7.5% |
| +2 | 2 | 87 | 2.2% |

(gear only; over all 327 affixed items including maps, zero-based scores 90%.) The residue is
stats fed by more than one affix, plus packed stats like `item_addskill_tab`, which that test
does not try to split.

One real crafted Light Plate showed the off-by-one directly — three values, all outside their
affix's range under the 1-based reading and all inside under the zero-based one:

| | 1-based | 0-based |
|---|---|---|
| tohit 106 | Gold, 81..100 | Platinum, 101..120 |
| enhanced defence 27 | Sturdy, 10..20 | Sturdy, 21..30 |
| damage reduction 3 | of Protection, 2..2 | of Absorption, 3..3 |

Every line prints the id as well as the name, so the mapping keeps checking itself in game: a
name that does not fit what the item does is one constant offset away, not a scramble.

**Id 0 never gets a rule.** It is how the item format spells "no affix here" — a magic item with
no suffix carries suffix 0 — so a rule on it would label every suffix-less item in the game with
whatever sits in row 0. Caught in testing, where a synthetic item with no suffix came back
labelled "suf of Health #0".

## Wording

The phrase comes from `ItemStatCost`'s `descstrpos` resolved through the game's own string
tables, so it reads the way the item's own lines do. Where the number goes is `descval`: 1 before
the phrase, 2 after it, 0 means the phrase carries no number. Guessing that gives "3 Physical
Damage Taken Reduced by".

Two things the tables do not hand over cleanly:

* 32 of 458 `Properties.txt` codes name no stat — the property function writes them in the game's
  code. `dmg%` is func 7, writing both the minimum and maximum damage percentages.
* a few string templates take several printf arguments (the cast-on-striking ones take three).
  Those are left as the raw property code plus the range, which still identifies them: `of Nova`
  comes out as `hit-skill 16-30`.

A `%` cannot survive into the output — it opens a filter token — so one in a game string is
dropped. "+21-30% Enhanced Defense" therefore reads "21-30 Enhanced Defense".

## Where the block goes, and what it cannot disturb

**After the host filter's first thousand rules, for a reason never found.** The end of the file
is dead for a reason that is in the engine's own code: a matching rule without `%CONTINUE%` ends
the description chain for that item, and a mainstream filter carries over a thousand of them, so
a block appended last never runs. The top is dead for no reason anyone could name. The same rules
that work in a filter of their own produce nothing when they sit first in a 6000-rule host, while
identical copies placed further in work.

Measured by putting one probe rule at seven depths of a real filter at once: the copies at rules
1001, 2001, 3001, 4001 and 5001 all printed; the copies at rule 1 and at the end did not. The
parser drops nothing — it appends every line to an ordered list, with no cap — nothing in the
skipped range replaces the description, and the description has no length limit worth the name:
383 bytes of it rendered fine. So the offset is a fact, not an explanation.

A filter too small to have an opening section takes the block at the top, where a small filter
does work.

Every generated rule carries `%CONTINUE%`, so the host filter still does
everything it did before; measured on the real file, its 6368 lines come back byte for byte when
the block is removed.

Nothing in the host filter overwrites the lines. The 58 rules with empty output (which hide an
item) that could touch an affixed one are all gated on `GROUND`, so they do not apply in the
inventory; and all 126 rules that set a description without `%NAME%` in it are gated on a specific
item code — jewel fragments, uber keys, perfect gems, crafting recipes — never affixed gear.

## What it leaves behind

`loot.filter` is backed up to `loot.filter.pd2addons-backup` on the first write and never again.
`unaffixes` removes the block and leaves the rest of the file exactly as it was; `uninstall` does
it too.

**PD2Launcher rewrites `loot.filter` from whichever filter is selected in it**, byte for byte, so
a block in `loot.filter` is only as durable as the next launch. A filter copied into
`filters/local/` and selected there is not downloaded again, so the block survives — at the cost
of no longer receiving that author's updates through the launcher. Either way `status` reads the
file instead of remembering what it wrote, and re-running `affixes` after an update is the fix.

One thing the launcher does leave alone: its client-file updater has an explicit skip list —
`D2.LNG`, `BnetLog.txt`, `ProjectDiablo.cfg`, `ddraw.ini`, `default.filter`, `loot.filter`,
`UI.ini`, `d2gl.yaml`, `PD2Shared.dll`, `launcherSettings.json`, `.backup` — so syncing the
client from `pd2-client-files.projectdiablo2.com` never touches a filter.

The block is 1615 rules on top of the host's ~6000, and every one is evaluated for every item.
The affix comparison is the whole condition, so there is nothing to short-circuit on an item
without that affix; it is a flat cost per rule.
