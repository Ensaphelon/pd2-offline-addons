"""The table the plugin reads to name the affix behind each line of an item's tooltip.

Same question as the loot filter block answers -- which affix gave this, and what could it have
rolled -- but put where it belongs: at the end of the stat's own line, beside the game's own
`[10 - 200]`, instead of in a block above the item.

The filter could never do that. A loot filter supplies an item's name and one description block,
and the engine draws that block above the stat lines; there is no token that addresses a stat
line at all. The plugin is inside the process and hooks the function that writes those lines, so
it can simply append.

What crosses the boundary is this file. Everything that needs the game's tables -- which affix
grants which stat, over what range, under what name -- is worked out here, where the tables are
already parsed and under test, and handed over as flat text. The C side does no table reading:
it looks a row up by (kind, affix id, stat id) and prints it.

ONE LINE PER AFFIX AND STAT, not per affix. A property can write several stats from a single
roll -- `res-all` writes all four resistances from one 21-30 -- and each of those stats is drawn
on its own line in the tooltip, so each needs its own row to be found by.
"""

from __future__ import annotations

import re
from pathlib import Path

from .affix_labels import EXCEL, _clean, _table, property_stats

TABLE_NAME = "affixes.txt"
CONFIG_NAME = "affix-ranges.cfg"

# Written once, on the first install, and never again -- the point of it is to be edited. The
# plugin re-reads it while the game runs, so a change takes effect on the next tooltip.
CONFIG_DEFAULT = """# When the affix ranges are shown on an item's stat lines.
#   hold: always   every tooltip, all the time
#   hold: shift    only while shift is held -- the lines are wide, and this keeps them out of
#                  the way while playing without losing them when reading an item
#   hold: ctrl     the same, on control
#   hold: alt      the same, on alt
hold: shift
"""

# Long enough for every affix the game ships ("of the Vampiremagi" is 18) with room to spare, and
# short enough that the plugin can hold the table in a fixed array rather than a heap of its own.
MAX_NAME = 31


# Which of D2's description functions put a per cent sign on the number. The game string does
# not carry it -- "Enhanced Defense" is stored without one -- so the only way to know whether a
# range is 51-65 or 51%-65% is the function that will draw it. Checked against the tooltip: 4 is
# "+87% Enhanced Defense" and "Fire Resist +35%", 2 is "18% Increased Chance of Blocking", while
# 1 is "+69 Maximum Stamina" and 11 is the self-repair line, and neither of those is a per cent.
_PERCENT_DESCFUNCS = {2, 4, 5, 7, 8, 10, 18, 20, 22, 23}


def _stat_ids(archive: Path) -> dict[str, tuple[int, bool]]:
    """stat name -> (the id the game passes around, whether its number is a per cent)."""
    out: dict[str, tuple[int, bool]] = {}
    for row in _table(archive, "ItemStatCost"):
        stat = (row.get("Stat") or "").strip()
        raw = (row.get("ID") or "").strip()
        func = (row.get("descfunc") or "").strip()
        if stat and raw.isdigit():
            out[stat] = (int(raw), func.isdigit() and int(func) in _PERCENT_DESCFUNCS)
    return out


def prefix_base(archive: Path) -> int:
    """What a prefix's row index has added to it to become the id the game keeps on the item.

    The game does not store prefixes and suffixes in two numbering spaces, it stores them in one:
    a suffix is its own row index, and a prefix is its row index plus the length of the suffix
    table less one. Measured, not assumed -- a crafted Light Plate whose rows are prefixes 147,
    327 and 390 and suffix 402 carries 1190, 1370, 1433 and 402 in ItemData+0x38/+0x3E, and
    1190-147 == 1370-327 == 1433-390 == 1043, which is the suffix table's 1044 rows less one.

    Derived from the table rather than written down, so a season that adds suffixes moves it.
    """
    return len(_table(archive, "MagicSuffix")) - 1


def rows(archive: Path) -> list[tuple[str, int, int, int, int, int, str]]:
    """(kind, id the game uses, stat id, low, high, per cent, name) per affix and stat."""
    ids = _stat_ids(archive)
    prop_stats = property_stats(archive)
    base = prefix_base(archive)
    seen: set[tuple[str, int, int]] = set()
    out: list[tuple[str, int, int, int, int, int, str]] = []
    for kind, table in (("p", "MagicPrefix"), ("s", "MagicSuffix")):
        for row_index, row in enumerate(_table(archive, table)):
            affix_id = row_index + base if kind == "p" else row_index
            # Row 0 is the format's "no affix here", never a real one -- see affix_labels.
            if row_index == 0 or (row.get("spawnable") or "").strip() != "1":
                continue
            name = _clean(row.get("Name") or "")[:MAX_NAME]
            if not name:
                continue
            for slot in (1, 2, 3):
                code = (row.get(f"mod{slot}code") or "").strip()
                if not code:
                    continue
                low = (row.get(f"mod{slot}min") or "").strip()
                high = (row.get(f"mod{slot}max") or "").strip()
                lo = int(low) if low.lstrip("-").isdigit() else 0
                hi = int(high) if high.lstrip("-").isdigit() else 0
                for stat in prop_stats.get(code, ()):
                    found = ids.get(stat)
                    if found is None:
                        continue
                    stat_id, percent = found
                    key = (kind, affix_id, stat_id)
                    # A property that names the same stat twice, or two mods of one affix feeding
                    # one stat, would otherwise print the affix twice on that line.
                    if key in seen:
                        continue
                    seen.add(key)
                    out.append((kind, affix_id, stat_id, lo, hi, int(percent), name))
    return out


def _recipes(archive: Path) -> list[tuple[str, str, list[tuple[int, int, int, int]]]]:
    """Every distinct craft recipe as (family, item type, mods), numbered by position.

    One place decides what a recipe is and what number it has, because two places deciding that
    is how a table and the thing that reads it drift apart without anybody noticing.

    A recipe is spelled out in CubeMain once per input quality and again for the ethereal
    variant; those are one recipe. Two item classes of one family are NOT -- a Brilliant helm and
    a Brilliant body share their resistances and differ in everything else, and merging them
    would put a helm's faster hit recovery on a breastplate.

    `ethereal` is dropped: a flag, not a number, and it could never match a line of a tooltip.
    """
    ids = _stat_ids(archive)
    prop_stats = property_stats(archive)
    seen: dict[tuple, int] = {}
    out: list[tuple[str, str, list[tuple[int, int, int, int]]]] = []
    for row in _table(archive, "CubeMain"):
        description = (row.get("description") or "").strip().lower()
        match = re.search(r"\+ (\w+) craft", description)
        if not match:
            continue
        family = match.group(1).capitalize() + " craft"
        kind = (row.get("input 1") or "").strip('"').split(",")[0].strip()
        mods: list[tuple[int, int, int, int]] = []
        for slot in range(1, 6):
            code = (row.get(f"mod {slot}") or "").strip()
            if not code or code == "ethereal":
                continue
            low = (row.get(f"mod {slot} min") or "").strip()
            high = (row.get(f"mod {slot} max") or "").strip()
            lo = int(low) if low.lstrip("-").isdigit() else 0
            hi = int(high) if high.lstrip("-").isdigit() else 0
            for stat in prop_stats.get(code, ()):
                found = ids.get(stat)
                if found:
                    mods.append((found[0], lo, hi, int(found[1])))
        mods = sorted(set(mods))
        if not mods:
            continue
        key = (family, kind, tuple(mods))
        if key in seen:
            continue
        seen[key] = len(out)
        out.append((family, kind, mods))
    return out


def craft_rows(archive: Path) -> list[tuple[int, int, int, int, int, str]]:
    """(recipe, stat id, low, high, per cent, family) for the fixed mods of a craft recipe.

    A crafted item carries more than its affixes: the recipe adds mods of its own, and they are
    the reason a crafted item's numbers do not add up from the affixes alone. The Brilliant body
    recipe gives block 10-20, all resistances 5-10 and enhanced defence 25-50 -- which is where
    the missing five points of resistance on a real crafted Light Plate came from, and the
    missing third of its defence.

    NOTHING ON THE ITEM SAYS WHICH RECIPE MADE IT. The recipe's mods are applied at the cube and
    then are ordinary item stats, indistinguishable from any other. So the recipe is identified
    by what the item could have been made from and what it carries: the recipes that apply to its
    base item -- seven, one per family -- scored by how many of their mods the item has. On a
    real crafted Light Plate that is Brilliant with five of six against one for the runner-up.

    Not all six can be looked for: `item_armor_percent` is drawn on the tooltip but is not in the
    item's stat list at all, so a rule of "every mod present" matches nothing. Hence a score and
    a clear winner rather than a complete match.
    """
    out: list[tuple[int, int, int, int, int, str]] = []
    for recipe, (family, _kind, mods) in enumerate(_recipes(archive)):
        out += [(recipe, stat, lo, hi, pct, family) for stat, lo, hi, pct in mods]
    return out


def _type_family(archive: Path, root: str) -> set[str]:
    """A type code and every type that declares it as an equivalent, transitively."""
    children: dict[str, set[str]] = {}
    for row in _table(archive, "ItemTypes"):
        code = (row.get("Code") or "").strip()
        for column in ("Equiv1", "Equiv2"):
            parent = (row.get(column) or "").strip()
            if code and parent:
                children.setdefault(parent, set()).add(code)
    seen, stack = set(), [root]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        stack += list(children.get(current, ()))
    return seen


def _codes_of(archive: Path, types: set[str]) -> set[str]:
    """Every base item whose type, or second type, is one of these."""
    out: set[str] = set()
    for table in ("Armor", "Weapons", "Misc"):
        for row in _table(archive, table):
            code = (row.get("code") or "").strip()
            if code and {(row.get("type") or "").strip(),
                         (row.get("type2") or "").strip()} & types:
                out.add(code)
    return out


def item_numbers(archive: Path) -> dict[str, int]:
    """base item code -> the number the game carries on the unit, UnitAny's dwTxtFileNo.

    One table, in file order: weapons, then armour, then the rest -- and rows without a code are
    not counted. Each of the three tables holds exactly one of those, an "Expansion" line that
    separates the classic rows from the expansion's, and the game does not give it a number.

    Measured against the game rather than assumed: a Light Plate comes through the plugin as 340,
    which is what this returns, and 341 if the separators are counted.
    """
    out: dict[str, int] = {}
    number = 0
    for table in ("Weapons", "Armor", "Misc"):
        for row in _table(archive, table):
            code = (row.get("code") or "").strip()
            if not code:
                continue
            out.setdefault(code, number)
            number += 1
    return out


def craft_codes(archive: Path) -> list[tuple[int, int, str]]:
    """(recipe, item number, item code) -- which items each recipe can have been applied to.

    This is what keeps a Brilliant helm's mods off a Brilliant breastplate, and it narrows the
    field from every recipe in the game to the seven that could have made this item.

    The number is what crosses to the plugin, not the code: reading dwTxtFileNo off the unit is
    one four-byte load, where the code would mean calling D2Common's GetItemText through an
    ordinal and trusting a published struct offset -- and the first attempt at that read six
    bytes of zeroes, because the name field before the code is twice the length its comment
    claims. The code is carried along anyway so the table can be read by a person.
    """
    numbers = item_numbers(archive)
    members: dict[str, set[str]] = {}
    out: list[tuple[int, int, str]] = []
    for recipe, (_family, kind, _mods) in enumerate(_recipes(archive)):
        if kind not in members:
            members[kind] = _codes_of(archive, _type_family(archive, kind))
        out += [(recipe, numbers[code], code)
                for code in sorted(members[kind]) if code in numbers]
    return out


def _corruptions(archive: Path) -> tuple[list[str], dict[tuple[int, int], list[tuple]]]:
    """(the item classes corruption recipes name, (type, class) -> its mods).

    A corrupted item says outright what was done to it: stat 360 `corrupted` holds the type
    number, and CubeMain has a recipe per type and item class carrying the mod and its range.
    Type 39 on a body armour is poison resistance 30-35, which is what makes a crafted Light
    Plate's 64% add up: Chromatic's 21-30, the craft's 5-10 and the corruption's 30-35.

    No fingerprinting here, unlike the crafts -- the number is on the item.
    """
    ids = _stat_ids(archive)
    prop_stats = property_stats(archive)
    classes: list[str] = []
    out: dict[tuple[int, int], list[tuple]] = {}
    for row in _table(archive, "CubeMain"):
        kind = None
        mods: list[tuple[int, int, int, int]] = []
        for slot in range(1, 6):
            code = (row.get(f"mod {slot}") or "").strip()
            if not code:
                continue
            low = (row.get(f"mod {slot} min") or "").strip()
            high = (row.get(f"mod {slot} max") or "").strip()
            lo = int(low) if low.lstrip("-").isdigit() else 0
            hi = int(high) if high.lstrip("-").isdigit() else 0
            if code == "corrupt":
                kind = lo
                continue
            if code == "corruptnum":
                continue
            for stat in prop_stats.get(code, ()):
                found = ids.get(stat)
                if found:
                    mods.append((found[0], lo, hi, int(found[1])))
        if kind is None or not mods:
            continue
        token = (row.get("input 1") or "").strip('"').split(",")[0].strip()
        if token not in classes:
            classes.append(token)
        key = (kind, classes.index(token))
        # One type and class can be spelled by several recipes -- sockets are, once per count.
        # Folding them to the span they cover between them is the only honest single answer.
        merged = {stat: (lo, hi, pct) for stat, lo, hi, pct in out.get(key, [])}
        for stat, lo, hi, pct in mods:
            if stat in merged:
                was_lo, was_hi, _ = merged[stat]
                merged[stat] = (min(was_lo, lo), max(was_hi, hi), pct)
            else:
                merged[stat] = (lo, hi, pct)
        out[key] = [(stat, lo, hi, pct) for stat, (lo, hi, pct) in sorted(merged.items())]

    # Numbered narrowest first, because an item belongs to several of them and the narrowest is
    # the one that is right: a bow is both `weap` and `bow`, and only the `bow` recipe knows a
    # socket corruption gives it three to six rather than one to six. The plugin takes the
    # lowest-numbered class that matches, so the ordering carries that rule across.
    sizes = {token: len(_codes_of(archive, _type_family(archive, token))) or 1
             for token in classes}
    order = sorted(classes, key=lambda token: (sizes[token], token))
    moved = {classes.index(token): index for index, token in enumerate(order)}
    return order, {(kind, moved[cls]): mods for (kind, cls), mods in out.items()}


def corruption_rows(archive: Path) -> list[tuple[int, int, int, int, int, int]]:
    """(type, class, stat id, low, high, per cent) -- what a corruption of that type did."""
    _classes, table = _corruptions(archive)
    return [(kind, cls, stat, lo, hi, pct)
            for (kind, cls), mods in sorted(table.items())
            for stat, lo, hi, pct in mods]


def corruption_items(archive: Path) -> list[tuple[int, int]]:
    """(item number, class) for every base item a corruption recipe's class covers.

    Carried as classes rather than expanded per type: 334 type-and-class pairs against 836 items
    would be a table of tens of thousands of rows for the same handful of facts.

    Four of the tokens name an item outright rather than a type -- rings, the two larger charms,
    and Annihilus by its unique name -- so a token that expands to nothing is tried as a code.
    """
    classes, _table_ = _corruptions(archive)
    numbers = item_numbers(archive)
    out: list[tuple[int, int]] = []
    for index, token in enumerate(classes):
        codes = _codes_of(archive, _type_family(archive, token))
        if not codes and token in numbers:
            codes = {token}
        out += [(numbers[code], index) for code in sorted(codes) if code in numbers]
    return out


def render(archive: Path) -> str:
    """The table as the plugin reads it: `kind id stat low high percent name`, name last.

    Four kinds. `p` and `s` are affixes, found by the id the game keeps on the item. `c` is a
    craft recipe's fixed mods and `t` says which base items that recipe reaches -- both are
    needed because nothing on the item says which recipe made it, so it is identified from what
    it could have been made from and what it carries. See `craft_rows`.
    """
    lines = [
        "# pd2-offline-addons affix ranges -- generated, do not edit",
        "# kind(p|s|c) id stat_id low high percent name",
        "#   p/s: id is the one the game keeps on the item, prefixes shifted past the suffixes",
        "#   c:   id groups the fixed mods of one craft recipe",
        "#   t:   which base items a recipe applies to; stat_id column holds the item number",
        "#   x:   a corruption, by the type the item carries in stat 360 and the item class",
        "#   k:   which corruption classes a base item belongs to",
    ]
    lines += [f"{kind} {affix} {stat} {lo} {hi} {pct} {name}"
              for kind, affix, stat, lo, hi, pct, name in rows(archive)]
    lines += [f"c {recipe} {stat} {lo} {hi} {pct} {family}"
              for recipe, stat, lo, hi, pct, family in craft_rows(archive)]
    lines += [f"t {recipe} {number} 0 0 0 {code}"
              for recipe, number, code in craft_codes(archive)]
    lines += [f"x {kind} {cls} {stat} {lo} {hi} {pct}"
              for kind, cls, stat, lo, hi, pct in corruption_rows(archive)]
    # Padded to the same six columns as every other row so one parser reads them all.
    lines += [f"k {number} {cls} 0 0 0" for number, cls in corruption_items(archive)]
    return "\n".join(lines) + "\n"


def write_table(archive: Path, install_dir: Path) -> int:
    """Puts the table beside the plugin. Returns how many rows went in.

    The settings file goes beside it, but only if it is not already there: the table is
    generated and belongs to this program, while the settings file belongs to whoever edits it,
    and an install should not quietly undo their choice.
    """
    text = render(archive)
    (install_dir / TABLE_NAME).write_text(text, encoding="latin1", newline="\n")
    config = install_dir / CONFIG_NAME
    if not config.exists():
        config.write_text(CONFIG_DEFAULT, encoding="latin1", newline="\n")
    return sum(1 for line in text.splitlines() if not line.startswith("#"))
