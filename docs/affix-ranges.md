# Affix ranges

Which affix gave a line of an item's tooltip, and what it could have rolled — written at the end
of that line, beside the range the maphack already prints there.

```
+87% Enhanced Defense [35 - 250]  Blessed [51 - 65]%, Brilliant [25 - 50]%
18% Increased Chance of Blocking  Brilliant [10 - 20]%
Lightning Resist +48% [11 - 80]  Ocher [11 - 20]%, Chromatic [21 - 30]%, Brilliant [5 - 10]%
Poison Resist +64%* [38 - 75]  Chromatic [21 - 30]%, Brilliant [5 - 10]%, Corrupted [30 - 35]%
Repairs 1 durability in 33 seconds  of Self-Repair
```

The range in the middle is the maphack's, corrected: it knows only affixes, so on a crafted or
corrupted item it printed a range the item's own number sat outside. See
[below](#the-range-already-on-the-line-is-corrected-rather-than-contradicted).

Nothing to turn on: `install` puts the table beside the plugin, and the plugin reads it.

**Shown only while shift is held, by default.** Four sources named on one stat line is most of
the screen, which is what you want while reading an item and not while playing. `install` writes
`affix-ranges.cfg` beside the DLL the first time and never again, since the point of it is to be
edited; `hold: always`, `shift`, `ctrl` or `alt`. The plugin re-reads it once a second while the
game runs, so a change takes effect on the next tooltip rather than the next session.

That this works at all is a measured fact rather than a hope: the hooked function runs once per
tooltip line per frame -- four calls in 25ms, from the trace -- so the key is read fresh every
time and there is no cached text to invalidate.

## Why it is not a loot filter

It began as one, and the filter version works — `affixes` still installs it, and it is the only
option without the plugin. But a filter can only ever produce a block above the item's stats: it
supplies a name and one description, and nothing in the language addresses a stat line. The third
line above is the other half of the reason. A stat fed by three sources can only be explained
where the stat is.

The filter version also runs into the engine: rules placed first in a large host filter produce
nothing, for a reason never found. See [affix-labels.md](affix-labels.md).

## Where the hook goes

The ` [10 - 200]` is BH.dll's, not the game's — `always_show_stat_ranges` is one of its settings,
and the eight format strings behind it live in its `.rdata`. They are referenced from one
function, which BH **exports**: `_BHOnPropertyBuild@16`. Project Diablo 2 builds a property's
line, hands it over and gets the range appended.

The decorated name gives the convention away — `@16` is four stdcall arguments — and BH's own
internal call settles what they are:

```
10099c83  push 0x0                  ; arg4
10099c85  push [ebp-0x114]          ; arg3  UnitAny*  (the item)
10099c8b  lea  eax,[ebp-0x104]
10099c91  push edi                  ; arg2  the stat id
10099c92  push eax                  ; arg1  the line, a 128-wchar stack buffer
10099c93  call _BHOnPropertyBuild
```

Patching **that call site** was the first attempt and it was wrong: it is `_BHOnDamagePropertyBuild`
calling into the general path, so it only ever fires for damage lines, and the detour never ran
once. Nothing imports BH either — PD2 resolves the address itself — so there is no import slot to
swap. What works is a detour on the exported function's own prologue, six bytes of which land on
an instruction boundary. The function is found by name through `GetProcAddress`, so a BH rebuild
that moves it is still found; its prologue is compared before anything is written, so a rebuild
that *changes* it costs a line in the log and the feature, not the game.

## The two numbering systems that are not the obvious ones

**An affix id on the item is not its row in `MagicPrefix.txt`.** The game keeps one numbering for
both kinds: a suffix is its own row, a prefix is its row plus the length of the suffix table less
one. A crafted Light Plate whose rows are prefixes 147, 327, 390 and suffix 402 carries 1190,
1370, 1433 and 402. Derived from the table rather than written down, so a season that adds
suffixes moves it.

**An item's `dwTxtFileNo` counts one table, skipping the rows that have no code.** Weapons, then
armour, then the rest; each of the three carries one `Expansion` separator, and the game does not
number it. Counting them put a Light Plate one place out — 341 against the 340 the game reports.

Both were settled against a real item rather than assumed, and both are covered by tests.

## Naming the craft

A crafted item carries more than its affixes: the recipe adds mods of its own, and they are why
the numbers do not add up from the affixes alone. On the item above, 35 resistance is Chromatic's
21-30 plus the craft's 5-10, and 87% defence is Blessed plus the craft's 25-50.

**Nothing on the item says which recipe made it.** The cube applies the mods and they become
ordinary stats, indistinguishable from any others. So the recipe is identified from what the item
could have been made from and what it carries:

1. The item's number narrows `CubeMain`'s recipes to the ones whose input type covers it — seven,
   one per family. Without this the field is every recipe in the game, and a Brilliant *helm*'s
   faster hit recovery gets proposed for a breastplate.
2. Each is scored — and **a mod being present is not evidence**, which cost a wrong answer on a
   real item. A Safety body armour was named Brilliant because Brilliant grants all resistances
   and the item had four of them. They came from Chromatic, its own prefix, which explains every
   point of them. What counts is a value the item's own affixes and corruption cannot reach:
   35 cold resistance under a Chromatic that tops out at 30, a blocking chance nothing on the
   item grants, physical damage reduction out of nowhere. That scores the crafted Light Plate
   Brilliant 3 against Safety 0, and the Safety plate the other way around.
3. A winner needs two such mods and a clear margin. A tie is not an answer, and the plugin
   prints nothing rather than choose.

Comparing a value against a range means both being in the same units, and they are not: life,
mana and stamina are stored shifted left by eight, so a `+27 to Life` reads 6912 — outside every
range there is, which would make every recipe with a life mod look like a match. The shifts come
over in the table with everything else.

Not all mods can be looked for: `item_armor_percent` is drawn on the tooltip but is not in the
item's stat list at all, which is why this is a score and not a complete match. The stats live in
the **second** list — `StatList+0x48`, the one BH itself walks. `+0x24` holds the base item's
three: defence and the two durabilities.

## Naming the corruption

A corrupted item says outright what was done to it, which makes this the one source on a line
that needs no working out at all: **stat 360 holds the corruption's TYPE, not a flag**, and
`CubeMain` has a recipe per type and item class carrying the mod and its range. Type 39 on a body
armour is poison resistance 30-35; the same type on a helm is 10-15, which is why the item's
class is half the key.

That is what closes the last gap. A crafted Light Plate showing 64% poison resistance is
Chromatic's 21-30, the craft's 5-10 and the corruption's 30-35 -- and until the corruption was
named, the line visibly did not add up.

Two things about the recipes:

* **An item belongs to several classes, and the narrowest is the one that knows.** A bow is both
  `weap` and `bow`, and only the `bow` recipe knows a socket corruption gives it three to six
  rather than one to six. The classes are numbered narrowest first and the plugin takes the
  lowest-numbered match, which carries that rule across without any logic in the plugin.
* **One type and class can be spelled by several recipes** -- sockets are, one per count. The
  span they cover between them is the only honest single answer.

The label is drawn red rather than orange: the corruption is the one source on the line that was
not there when the item was made. The escape goes into the narrow buffer the line is assembled
in -- 0xFF is not printable ASCII, so it survives the widening untouched.

### How it was nearly missed

The first search for this concluded the ranges were not in the game at all, on the strength of
every reference to stat 360 in every module being a read. That reasoning was wrong: a property is
applied through `Properties.txt` generically, taking the stat number from the table rather than
from an instruction, so a write would never appear as a literal 360 in the code. The ranges were
in `CubeMain` the whole time, in 995 rows that each set `corrupt N-N` alongside the mod.

## The range already on the line is corrected rather than contradicted

The two numbers measure different things, and until the sources were known there was nothing to
do about it but explain the difference.

**The maphack's** is what the item could have had over the whole tier ladder of the affixes it
carries. `res-all` spans 3-30 across its tiers, which is exactly what it prints on resistances
nothing else feeds. Lightning has a second ladder on a crafted Light Plate -- Ocher's group,
whose top tier on a body armour is Amber 31-40 -- so 3+3 and 30+40 make the 6 and the 70 it used
to print there.

**And it only knows affixes.** It has no table of craft recipes or corruptions, so on a crafted
or corrupted item it printed a range the item's own number sat outside -- `+35%` under
`[3 - 30]`, `+26%` under `[10 - 24]`. That gap is the reason this addon exists, and the sources
are known here, so the range is now corrected in place by the same numbers the labels name:

* **An affix on the item feeds this stat** -- its range is the ladder of that affix's group, and
  the craft and the corruption are added at both ends. `[3 - 30]` plus Brilliant's 5-10 is
  `[8 - 40]`, which is where the 35 always was.
* **None does** -- its range is then the whole span the stat could have reached on an item of
  this kind, which is how a corrupted `+26% Faster Hit Recovery` gets `[10 - 24]` with no hit
  recovery affix anywhere on it. That affix was never guaranteed, so the floor is what the craft
  and the corruption give on their own while the ceiling still adds to BH's: `[10 - 24]` becomes
  `[20 - 54]`, and a Safety plate's `[4 - 25]` of damage reduction becomes `[4 - 31]`.

The second rule was wrong in its first version -- it dropped the ceiling as well as the floor,
and turned `[4 - 25]` into `[4 - 6]`. That is how the two cases were told apart at all: the
maphack prints `[3 - 30]` for one chest's fire resistance and `[3 - 40]` for another's, which
only makes sense if it is reading the affix the item carries, and it prints nothing at all for
blocking chance on a body armour, which no affix there can grant.

**And the number the game drew has to fall inside the result, or nothing is written.** If it does
not, something here is wrong -- an affix missing from the table, a stat held in units the line
does not use, life and mana being stored shifted by eight -- and being wrong quietly is worse
than printing nothing. The maphack's own range is left exactly as it was.

That check is read off the line rather than out of the item, deliberately: the line carries the
number in the units it is written in, which is the units the range is in, and the stat in memory
often is not.

Two things follow from where this sits. The corrected range appears and disappears with the hold
key along with everything else, so letting go gives back the maphack's own output untouched. And
once corrected the number is this addon's, not BH's, though it keeps BH's colour -- extending the
range was the point, and a second colour there would make four on one line.

## What it deliberately does not do

**Split a value between sources.** The item stores one merged total per stat. On the lightning
line above there is no way to say which of the 48 came from Ocher, which from Chromatic and which
from the craft — naming all three and their ranges is the whole of what the data supports.

**Print a range an affix does not roll.** `rep-dur` takes a fixed rate from its parameter, so
`of Self-Repair` is printed with no range rather than an invented one.

## Colour

Three sources, three colours, because on one line they are three different claims: an affix is
what the item rolled, a craft recipe is what the cube added to every item of its kind, and a
corruption is what was done to it afterwards.

| | |
|---|---|
| orange | the affixes |
| purple | the craft recipe -- the family's name alone, the colour saying the rest |
| red | the corruption |

Taken from BH's own table rather than from memory -- `ÿc8`, `ÿc;`, `ÿc1`. The line ends
by restoring blue: a colour holds until something changes it, and these lines are drawn one after
another into the same tooltip, so an unclosed one runs on through every stat line below it.

The escapes go into the narrow buffer the suffix is assembled in -- 0xFF is not printable ASCII,
so it survives the widening untouched -- and are written as explicit bytes rather than string
escapes, which the first attempt got wrong and encoded as the two UTF-8 bytes of U+00FF.

### Fitting

The line is a 128-wchar buffer, and three escapes cost nine characters of it. It was taken for
130 until 2026-11-21. BH is built with /GS, though, and `[ebp-4]`, right below the saved frame
pointer, holds its security cookie. A long line with ranges appended wrote over the cookie, and
BH ended the game with a fail-fast (exit code `0xC0000409`, no crash report) on returning. A stat fed by three
affixes, a craft and a corruption overruns it -- which would have lost the whole annotation on
exactly the busiest lines, the ones worth reading.

So the pieces are fitted one at a time rather than assembled and then accepted or rejected whole.
A piece that does not fit is not written at all, half a range being worse than none, and what was
left out is marked with a red `+` rather than left to look complete.

A per cent sign comes from `ItemStatCost`'s `descfunc`, not from the game string: the table holds
"Enhanced Defense" and the game draws "+87% Enhanced Defense".

## The table

`affixes.txt`, beside the DLL, written by `install` and regenerated whenever it runs. Six kinds
of row: `p` and `s` are affixes found by the id on the item, `c` is a craft recipe's fixed mods
and `t` says which items that recipe reaches, `x` is a corruption by type and item class with
`k` saying which classes an item belongs to, and `v` is a stat whose stored value is shifted.

Everything that needs the game's tables is worked out in Python, where they are already parsed
and under test, and handed over as flat text. The plugin reads rows; it does not read tables.
Without the file it leaves the game's stat lines exactly as they were.
