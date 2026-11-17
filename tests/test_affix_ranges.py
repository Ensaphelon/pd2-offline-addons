"""The table the plugin reads is flat text crossing a language boundary, so what has to hold is
that every row the C side can be handed is one it can use: a stat id it will be given, a name
that survives having spaces in it, and one row per stat rather than per affix -- because the
tooltip draws each stat on its own line, and an affix that writes four of them from one roll has
to be findable from all four.

Tables are built here rather than read from a game install, like the affix-label tests next door.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from pd2_offline_addons import affix_labels, affix_ranges, mpq_asset
from test_affix_labels import _empty_archive, _tsv

AFFIX_COLUMNS = ["Name", "spawnable",
                 "mod1code", "mod1min", "mod1max", "mod2code", "mod2min", "mod2max"]

PREFIXES = [
    # row 0 is the format's "no affix here" and must never produce a row
    {"Name": "Reserved", "spawnable": "1", "mod1code": "ac%", "mod1min": "1", "mod1max": "2"},
    {"Name": "Blessed", "spawnable": "1", "mod1code": "ac%", "mod1min": "51", "mod1max": "65"},
    {"Name": "Chromatic", "spawnable": "1", "mod1code": "res-all",
     "mod1min": "21", "mod1max": "30"},
    {"Name": "Unused", "spawnable": "0", "mod1code": "ac%", "mod1min": "1", "mod1max": "2"},
]
SUFFIXES = [
    {"Name": "Reserved", "spawnable": "1", "mod1code": "ac%", "mod1min": "1", "mod1max": "1"},
    # no roll of its own: the rate comes from the property's parameter, not from min/max
    {"Name": "of Self-Repair", "spawnable": "1", "mod1code": "rep-dur"},
]

PROPERTIES = [
    {"code": "ac%", "stat1": "item_armor_percent"},
    {"code": "rep-dur", "stat1": "item_replenish_durability"},
    {"code": "res-all", "stat1": "fireresist", "stat2": "lightresist",
     "stat3": "coldresist", "stat4": "poisonresist"},
]
# descfunc is how the game decides to draw the number: 4 puts a per cent sign on it, 11 is the
# self-repair line and 1 is a plain number.
STATS = [
    {"ID": "16", "Stat": "item_armor_percent", "descfunc": "4"},
    {"ID": "39", "Stat": "fireresist", "descfunc": "4"},
    {"ID": "41", "Stat": "lightresist", "descfunc": "4"},
    {"ID": "43", "Stat": "coldresist", "descfunc": "4"},
    {"ID": "45", "Stat": "poisonresist", "descfunc": "4"},
    {"ID": "252", "Stat": "item_replenish_durability", "descfunc": "11"},
    {"ID": "11", "Stat": "maxstamina", "descfunc": "1"},
]


# Two spellings of one recipe (the magic and the ethereal input) plus a different family, so
# the de-duplication and the fingerprinting both have something to bite on.
CUBE_COLUMNS = ["description", "input 1", "output",
                "mod 1", "mod 1 min", "mod 1 max", "mod 2", "mod 2 min", "mod 2 max",
                "mod 3", "mod 3 min", "mod 3 max"]
# Corruption recipes: a type number, the throwaway roll, and the mod with its range. The same
# type is spelled once per item class, and a bow's socket corruption differs from a weapon's.
CORRUPTIONS = [
    {"description": "corrupt body poison", "input 1": '"tors,nos"',
     "mod 1": "corrupt", "mod 1 min": "39", "mod 1 max": "39",
     "mod 2": "corruptnum", "mod 2 min": "1", "mod 2 max": "1000",
     "mod 3": "res-all", "mod 3 min": "30", "mod 3 max": "35"},
    {"description": "corrupt helm poison", "input 1": '"helm,nos"',
     "mod 1": "corrupt", "mod 1 min": "39", "mod 1 max": "39",
     "mod 2": "res-all", "mod 2 min": "10", "mod 2 max": "15"},
    # two spellings of one type and class, as sockets are: the span they cover between them
    {"description": "corrupt body armour low", "input 1": '"tors,nos"',
     "mod 1": "corrupt", "mod 1 min": "33", "mod 1 max": "33",
     "mod 2": "ac%", "mod 2 min": "50", "mod 2 max": "60"},
    {"description": "corrupt body armour high", "input 1": '"tors,nos"',
     "mod 1": "corrupt", "mod 1 min": "33", "mod 1 max": "33",
     "mod 2": "ac%", "mod 2 min": "70", "mod 2 max": "80"},
]

RECIPES = [
    {"description": "magic field plate + brilliant craft ingredient -> brilliant body",
     "input 1": '"tors,mag,noe"', "mod 1": "res-all", "mod 1 min": "5", "mod 1 max": "10",
     "mod 2": "ac%", "mod 2 min": "25", "mod 2 max": "50"},
    {"description": "magic field plate + brilliant craft ingredient -> brilliant body",
     "input 1": '"tors,mag,eth"', "mod 1": "res-all", "mod 1 min": "5", "mod 1 max": "10",
     "mod 2": "ac%", "mod 2 min": "25", "mod 2 max": "50",
     "mod 3": "ethereal", "mod 3 min": "1", "mod 3 max": "1"},
    {"description": "magic belt + blood craft ingredient -> blood belt",
     "input 1": '"belt,mag,noe"', "mod 1": "rep-dur"},
    # not a craft at all: a cube recipe with no family in its description
    {"description": "3 perfect gems + magic item -> rare item", "input 1": '"weap,mag"',
     "mod 1": "ac%", "mod 1 min": "1", "mod 1 max": "1"},
]


# A recipe names an item TYPE; the type expands through ItemTypes to the base items that carry
# it, and that is what keeps one family's helm mods off its body armour.
ITEM_TYPES = [
    {"Code": "tors", "Equiv1": "armo"},
    {"Code": "cloa", "Equiv1": "tors"},     # a sub-type: a cloak is a torso
    {"Code": "belt", "Equiv1": "armo"},
    {"Code": "helm", "Equiv1": "armo"},
]
ARMOR = [
    # the separator the game does not number, exactly as the real tables carry it
    {"code": "", "type": "", "name": "Expansion"},
    {"code": "ltp", "type": "tors"},
    {"code": "clk", "type": "cloa"},
    {"code": "vbl", "type": "belt"},
    {"code": "cap", "type": "helm"},
]


@pytest.fixture()
def archive(tmp_path: Path) -> Path:
    path = tmp_path / "pd2data.mpq"
    path.write_bytes(_empty_archive())
    mpq_asset.add(path, affix_labels.EXCEL + "MagicPrefix.txt", _tsv(AFFIX_COLUMNS, PREFIXES))
    mpq_asset.add(path, affix_labels.EXCEL + "MagicSuffix.txt", _tsv(AFFIX_COLUMNS, SUFFIXES))
    mpq_asset.add(path, affix_labels.EXCEL + "Properties.txt",
                  _tsv(["code", "stat1", "stat2", "stat3", "stat4"], PROPERTIES))
    mpq_asset.add(path, affix_labels.EXCEL + "ItemStatCost.txt",
                  _tsv(["ID", "Stat", "descfunc"], STATS))
    mpq_asset.add(path, affix_labels.EXCEL + "CubeMain.txt",
                  _tsv(CUBE_COLUMNS, RECIPES + CORRUPTIONS))
    mpq_asset.add(path, affix_labels.EXCEL + "ItemTypes.txt",
                  _tsv(["Code", "Equiv1", "Equiv2"], ITEM_TYPES))
    mpq_asset.add(path, affix_labels.EXCEL + "Armor.txt", _tsv(["code", "type", "type2", "name"], ARMOR))
    # The generator reads all three item tables; a craft can be a weapon or a jewel too. Empty
    # here, present on purpose: a missing one should be an error, not a quietly shorter table.
    for empty in ("Weapons", "Misc"):
        mpq_asset.add(path, affix_labels.EXCEL + empty + ".txt",
                      _tsv(["code", "type", "type2"], []))
    path.with_name(path.name + mpq_asset.BACKUP_SUFFIX).unlink(missing_ok=True)
    return path


def test_one_roll_that_writes_four_stats_is_findable_from_all_four(archive) -> None:
    """`res-all` is a single 21-30, drawn on four separate lines of the tooltip."""
    chromatic = [row for row in affix_ranges.rows(archive) if row[6] == "Chromatic"]
    assert sorted(row[2] for row in chromatic) == [39, 41, 43, 45]
    assert {(row[3], row[4]) for row in chromatic} == {(21, 30)}


def test_affix_id_zero_never_gets_a_row(archive) -> None:
    assert not [row for row in affix_ranges.rows(archive) if row[1] == 0]


def test_an_unspawnable_affix_gets_no_row(archive) -> None:
    assert not [row for row in affix_ranges.rows(archive) if row[6] == "Unused"]


def test_an_affix_with_no_roll_of_its_own_comes_back_as_zero_to_zero(archive) -> None:
    """The plugin prints the name alone for these; a range would be invented."""
    assert [row[3:5] for row in affix_ranges.rows(archive)
            if row[6] == "of Self-Repair"] == [(0, 0)]


def test_prefixes_and_suffixes_are_told_apart(archive) -> None:
    kinds = {row[6]: row[0] for row in affix_ranges.rows(archive)}
    assert kinds["Blessed"] == "p" and kinds["of Self-Repair"] == "s"


def test_a_prefix_is_numbered_past_the_suffixes(archive) -> None:
    """The game keeps one numbering for both, which is what the plugin reads off the item: a
    suffix is its row, a prefix is its row plus the suffix table's length less one."""
    base = affix_ranges.prefix_base(archive)
    assert base == len(SUFFIXES) - 1
    ids = {row[6]: row[1] for row in affix_ranges.rows(archive)}
    assert ids["Blessed"] == 1 + base           # row 1 of MagicPrefix
    assert ids["Chromatic"] == 2 + base         # row 2
    assert ids["of Self-Repair"] == 1           # row 1 of MagicSuffix, unshifted
    assert ids["Blessed"] != ids["of Self-Repair"]


def test_the_rendered_table_reads_back_the_way_the_plugin_reads_it(archive) -> None:
    """Seven fields, name last and unquoted -- which is what lets it hold spaces."""
    parsed = []
    for line in affix_ranges.render(archive).splitlines():
        if line.startswith("#"):
            continue
        fields = line.split(" ", 6)
        # x and k rows carry six numbers and no name; the rest carry five and a name
        while len(fields) < 7:
            fields.append("")
        kind, affix, stat, lo, hi, pct, name = fields
        parsed.append((kind, int(affix), int(stat), int(lo), int(hi), int(pct), name))
    assert [row for row in parsed if row[0] in ("p", "s")] == affix_ranges.rows(archive)
    assert [row[1:] for row in parsed if row[0] == "c"] == affix_ranges.craft_rows(archive)
    assert [(row[1], row[2], row[6]) for row in parsed
            if row[0] == "t"] == affix_ranges.craft_codes(archive)
    assert [row[1:6] for row in parsed if row[0] == "x"] == [
        tuple(r[:5]) for r in affix_ranges.corruption_rows(archive)]
    assert ("s", 1, 252, 0, 0, 0, "of Self-Repair") in parsed


def test_the_table_lands_beside_the_plugin(archive, tmp_path) -> None:
    written = affix_ranges.write_table(archive, tmp_path)
    table = tmp_path / affix_ranges.TABLE_NAME
    assert table.is_file()
    assert written == (len(affix_ranges.rows(archive))
                       + len(affix_ranges.craft_rows(archive))
                       + len(affix_ranges.craft_codes(archive))
                       + len(affix_ranges.corruption_rows(archive))
                       + len(affix_ranges.corruption_items(archive)))
    # Path.read_text takes no `newline` before 3.13, and folding CRLF away here would hide
    # exactly what is being checked: the plugin reads this with fgets.
    with open(table, encoding="latin1", newline="") as handle:
        assert "\r\n" not in handle.read()


def test_a_per_cent_stat_is_marked_and_a_plain_one_is_not(archive) -> None:
    """The game string says "Enhanced Defense" and the game draws "+87% Enhanced Defense", so the
    sign can only come from the stat's description function."""
    pct = {(row[2], row[6]): row[5] for row in affix_ranges.rows(archive)}
    assert pct[(16, "Blessed")] == 1            # descfunc 4
    assert pct[(39, "Chromatic")] == 1
    assert pct[(252, "of Self-Repair")] == 0    # descfunc 11


def test_a_craft_recipe_becomes_one_row_per_stat_it_fixes(archive) -> None:
    rows = affix_ranges.craft_rows(archive)
    brilliant = [r for r in rows if r[5] == "Brilliant craft"]
    assert sorted(r[1] for r in brilliant) == [16, 39, 41, 43, 45]
    assert {(r[2], r[3]) for r in brilliant if r[1] == 16} == {(25, 50)}
    assert {(r[2], r[3]) for r in brilliant if r[1] == 39} == {(5, 10)}


def test_the_same_recipe_spelled_twice_is_one_fingerprint(archive) -> None:
    """The game lists a recipe once per input quality and again for the ethereal variant; they
    are one recipe as far as identifying an item goes."""
    rows = affix_ranges.craft_rows(archive)
    assert len({r[0] for r in rows if r[5] == "Brilliant craft"}) == 1


def test_a_cube_recipe_that_is_not_a_craft_is_left_alone(archive) -> None:
    assert not [r for r in affix_ranges.craft_rows(archive) if "rare" in r[5].lower()]


def test_ethereal_is_not_a_stat_and_does_not_become_a_row(archive) -> None:
    """It is a flag, not a number, and it would never match a line of the tooltip."""
    stats = {r[1] for r in affix_ranges.craft_rows(archive)}
    assert 0 not in stats and all(isinstance(s, int) and s > 0 for s in stats)


def test_the_table_carries_crafts_alongside_affixes(archive) -> None:
    kinds = {line.split(" ", 1)[0] for line in affix_ranges.render(archive).splitlines()
             if not line.startswith("#")}
    assert kinds == {"p", "s", "c", "t", "x", "k"}


def test_a_recipe_reaches_the_items_its_type_covers_and_no_others(archive) -> None:
    """The Brilliant recipe's input is a torso, so it reaches body armour and cloaks -- a cloak
    declares the torso type as its equivalent -- and not belts or helms."""
    codes = dict()
    for recipe, _number, code in affix_ranges.craft_codes(archive):
        codes.setdefault(recipe, set()).add(code)
    families = {recipe: family for recipe, _s, _lo, _hi, _p, family
                in affix_ranges.craft_rows(archive)}
    brilliant = next(r for r, f in families.items() if f == "Brilliant craft")
    blood = next(r for r, f in families.items() if f == "Blood craft")
    assert codes[brilliant] == {"ltp", "clk"}
    assert codes[blood] == {"vbl"}


def test_two_classes_of_one_family_stay_two_recipes(archive) -> None:
    """Merging them would put one class's mods on another's item, which is the whole reason the
    item code is carried at all."""
    recipes = {r for r, _n, _c in affix_ranges.craft_codes(archive)}
    assert len(recipes) == len({r[0] for r in affix_ranges.craft_rows(archive)})


def test_the_item_number_skips_the_rows_the_game_does_not_number(archive) -> None:
    """Each item table carries one "Expansion" separator with no code, and the game gives it no
    number -- counting it put a real Light Plate one place out."""
    numbers = affix_ranges.item_numbers(archive)
    assert numbers["ltp"] == 0 and numbers["clk"] == 1
    assert "" not in numbers


def test_a_corruption_is_read_off_the_item_rather_than_guessed(archive) -> None:
    """Type 39 on a body armour is poison resistance 30-35; on a helm the same type is 10-15,
    which is why the item's class is part of the key."""
    classes, _table = affix_ranges._corruptions(archive)
    rows = {(kind, classes[cls], stat): (lo, hi)
            for kind, cls, stat, lo, hi, _pct in affix_ranges.corruption_rows(archive)}
    assert rows[(39, "tors", 45)] == (30, 35)
    assert rows[(39, "helm", 45)] == (10, 15)


def test_two_spellings_of_one_corruption_fold_to_the_span_they_cover(archive) -> None:
    """Socket corruptions are written one recipe per count; a single honest answer is the span."""
    classes, _table = affix_ranges._corruptions(archive)
    rows = {(kind, classes[cls], stat): (lo, hi)
            for kind, cls, stat, lo, hi, _pct in affix_ranges.corruption_rows(archive)}
    assert rows[(33, "tors", 16)] == (50, 80)


def test_the_narrowest_class_is_numbered_first(archive) -> None:
    """An item belongs to several, and the narrowest is the one that knows what it is talking
    about -- the plugin takes the lowest-numbered match."""
    classes, _table = affix_ranges._corruptions(archive)
    sizes = [len(affix_ranges._codes_of(archive, affix_ranges._type_family(archive, token)))
             for token in classes]
    assert sizes == sorted(sizes)


def test_an_item_carries_every_corruption_class_it_belongs_to(archive) -> None:
    classes, _table = affix_ranges._corruptions(archive)
    numbers = affix_ranges.item_numbers(archive)
    mine = sorted(cls for number, cls in affix_ranges.corruption_items(archive)
                  if number == numbers["ltp"])
    assert [classes[c] for c in mine] == ["tors"]


def test_the_settings_file_is_written_once_and_then_left_alone(archive, tmp_path) -> None:
    """It is there to be edited; an install that reset it would undo the choice silently."""
    affix_ranges.write_table(archive, tmp_path)
    config = tmp_path / affix_ranges.CONFIG_NAME
    assert "hold: shift" in config.read_text(encoding="latin1")

    config.write_text("hold: always\n", encoding="latin1")
    affix_ranges.write_table(archive, tmp_path)
    assert config.read_text(encoding="latin1") == "hold: always\n"


def test_the_settings_file_names_every_mode_the_plugin_understands(archive, tmp_path) -> None:
    """A mode the plugin has and the file does not mention is a mode nobody will find."""
    affix_ranges.write_table(archive, tmp_path)
    text = (tmp_path / affix_ranges.CONFIG_NAME).read_text(encoding="latin1")
    plugin = Path("src/affixes.c").read_text(encoding="utf-8")
    for mode in ("always", "shift", "ctrl", "alt"):
        assert f"hold: {mode}" in text
        assert f'"{mode}"' in plugin
