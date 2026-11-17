"""The affix-label block is generated code that lands inside somebody else's 6000-rule filter, so
what has to be proven is narrow and specific: that the host file comes back exactly as it was,
that the block goes somewhere the rules will actually be reached, that nothing the filter parser
treats as syntax can leak out of a game string, and that affix id 0 -- which is how the item
format spells "no affix" -- never gets a rule of its own.

Tables and string tables are built here rather than read from a game install: these tests must
run on a machine that has never had Diablo II on it, and the shape is the thing under test.
"""
from __future__ import annotations

import struct
from pathlib import Path

import pytest
from pd2_offline_addons import affix_labels, mpq_asset


def _empty_archive(hash_slots: int = 64) -> bytes:
    hash_pos = 32
    block_pos = hash_pos + hash_slots * 16
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, block_pos, 0, 3,
                         hash_pos, block_pos, hash_slots, 0)
    free = b"".join(struct.pack("<IIHHI", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)
                    for _ in range(hash_slots))
    return header + mpq_asset._crypt(free, mpq_asset._hash("(hash table)", 3), encrypt=True)


def _tbl(entries: dict[str, str]) -> bytes:
    """A Diablo II .tbl string table: 21-byte header, no index list, then 17-byte hash entries
    pointing into a string blob."""
    count = len(entries)
    base = 21
    body = bytearray()
    strings = bytearray()
    data_at = base + count * 17
    for key, value in entries.items():
        key_at = data_at + len(strings)
        strings += key.encode("latin1") + b"\x00"
        value_at = data_at + len(strings)
        strings += value.encode("latin1") + b"\x00"
        body += (bytes([1]) + struct.pack("<H", 0) + struct.pack("<I", 0)
                 + struct.pack("<II", key_at, value_at) + struct.pack("<H", len(value)))
    header = struct.pack("<HHI", 0, 0, count) + bytes(13)
    return header + bytes(body) + bytes(strings)


def _tsv(header: list[str], rows: list[dict[str, str]]) -> bytes:
    lines = ["\t".join(header)]
    lines += ["\t".join(row.get(column, "") for column in header) for row in rows]
    return ("\r\n".join(lines) + "\r\n").encode("latin1")


PREFIXES = [
    # row 0 is the "no affix" slot: spawnable, and still must never get a rule
    {"Name": "Reserved", "spawnable": "1", "mod1code": "ac%", "mod1min": "1", "mod1max": "2"},
    {"Name": "Sturdy", "spawnable": "1", "mod1code": "ac%", "mod1min": "21", "mod1max": "30"},
    {"Name": "Platinum", "spawnable": "1", "mod1code": "att", "mod1min": "101", "mod1max": "120"},
    {"Name": "Unused", "spawnable": "0", "mod1code": "att", "mod1min": "1", "mod1max": "2"},
    {"Name": "Brace{s} 50pct", "spawnable": "1", "mod1code": "ac%",
     "mod1min": "5", "mod1max": "5"},
]
SUFFIXES = [
    {"Name": "Reserved", "spawnable": "1", "mod1code": "red-dmg", "mod1min": "1", "mod1max": "1"},
    {"Name": "of Absorption", "spawnable": "1", "mod1code": "red-dmg",
     "mod1min": "3", "mod1max": "3"},
    {"Name": "of Piercing", "spawnable": "1", "mod1code": "pierce",
     "mod1min": "1", "mod1max": "1"},
    {"Name": "of the Glacier", "spawnable": "1",
     "mod1code": "cold-min", "mod1min": "14", "mod1max": "21",
     "mod2code": "cold-max", "mod2min": "45", "mod2max": "71"},
]
AFFIX_COLUMNS = ["Name", "spawnable",
                 "mod1code", "mod1min", "mod1max", "mod2code", "mod2min", "mod2max",
                 "mod3code", "mod3min", "mod3max"]

PROPERTIES = [
    {"code": "ac%", "func1": "2", "stat1": "item_armor_percent"},
    {"code": "att", "func1": "1", "stat1": "tohit"},
    {"code": "red-dmg", "func1": "1", "stat1": "normal_damage_reduction"},
    {"code": "pierce", "func1": "1", "stat1": "item_ignoretargetac"},
    {"code": "cold-min", "func1": "1", "stat1": "coldmindam"},
    {"code": "cold-max", "func1": "1", "stat1": "coldmaxdam"},
    {"code": "dmg%", "func1": "7"},  # names no stat; the property function writes them
]
STATS = [
    {"ID": "16", "Stat": "item_armor_percent", "descstrpos": "strModEnhancedDefense",
     "descval": "1"},
    {"ID": "19", "Stat": "tohit", "descstrpos": "strModAttackRating", "descval": "1"},
    {"ID": "34", "Stat": "normal_damage_reduction", "descstrpos": "strModDamageReduction",
     "descval": "2"},
    {"ID": "118", "Stat": "item_ignoretargetac", "descstrpos": "strModIgnoreAC", "descval": "0"},
    {"ID": "54", "Stat": "coldmindam", "descstrpos": "strModColdMin", "descval": "1"},
    {"ID": "55", "Stat": "coldmaxdam", "descstrpos": "strModColdMax", "descval": "1"},
]
STRINGS = {
    "strModEnhancedDefense": "Enhanced Defense",
    "strModAttackRating": "to Attack Rating",
    "strModDamageReduction": "Physical Damage Taken Reduced by",
    "strModIgnoreAC": "Ignore Target's Defense",
    # PD2's own added strings carry D2's colour escape UTF-8 encoded
    "strModColdMin": "\xc3\xbfc0to Minimum Cold Damage\xc3\xbfc3",
    "strModColdMax": "\xffc0to Maximum Cold Damage",
}

HOST_FILTER = "\r\n".join([
    "ItemDisplayFilterName[]: Base",
    "Alias[IN_TOWN]: (MAPID=1)",
    "ItemDisplay[box]: %NAME%{the cube}%CONTINUE%",
    "ItemDisplay[FILTLVL=0]: %NAME%{%NAME%}",
    "ItemDisplay[]: %NAME%{%NAME%}",
]) + "\r\n"


@pytest.fixture()
def install(tmp_path: Path) -> tuple[Path, Path, Path]:
    """(install_dir, filter, archive) laid out the way a PD2 install is."""
    root = tmp_path / "Diablo II"
    install_dir = root / "ProjectD2"
    install_dir.mkdir(parents=True)

    archive = install_dir / "pd2data.mpq"
    archive.write_bytes(_empty_archive())
    mpq_asset.add(archive, affix_labels.EXCEL + "MagicPrefix.txt", _tsv(AFFIX_COLUMNS, PREFIXES))
    mpq_asset.add(archive, affix_labels.EXCEL + "MagicSuffix.txt", _tsv(AFFIX_COLUMNS, SUFFIXES))
    mpq_asset.add(archive, affix_labels.EXCEL + "Properties.txt",
                  _tsv(["code", "func1", "stat1"], PROPERTIES))
    mpq_asset.add(archive, affix_labels.EXCEL + "ItemStatCost.txt",
                  _tsv(["ID", "Stat", "descstrpos", "descval"], STATS))
    mpq_asset.add(archive, affix_labels.STRING_TABLES[0], _tbl(STRINGS))
    archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX).unlink(missing_ok=True)

    loot = install_dir / "loot.filter"
    loot.write_text(HOST_FILTER, encoding="latin1", newline="")
    return install_dir, loot, archive


def _lines(install_dir: Path, archive: Path) -> dict[str, str]:
    return {f"{code}={affix_id}": text
            for code, affix_id, _name, text in affix_labels.affix_lines(install_dir, archive)}


def test_the_string_table_reader_gets_the_game_s_own_wording_back(install) -> None:
    install_dir, _loot, _archive = install
    assert affix_labels.game_strings(install_dir)["strmodattackrating"] == "to Attack Rating"


def test_affix_id_zero_never_gets_a_rule(install) -> None:
    """A magic item with no suffix carries suffix 0, so a rule on it would label half the game."""
    install_dir, _loot, archive = install
    keys = _lines(install_dir, archive)
    assert "PREFIX=0" not in keys and "SUFFIX=0" not in keys
    assert not any("Reserved" in text for text in keys.values())


def test_an_unspawnable_affix_gets_no_rule(install) -> None:
    install_dir, _loot, archive = install
    assert "PREFIX=3" not in _lines(install_dir, archive)


def test_the_number_goes_where_the_game_puts_it(install) -> None:
    """descval decides this, and guessing it gives "3 Physical Damage Taken Reduced by"."""
    install_dir, _loot, archive = install
    lines = _lines(install_dir, archive)
    assert lines["PREFIX=2"] == "pre Platinum #2  101-120 to Attack Rating"      # descval 1
    assert lines["SUFFIX=1"] == "suf of Absorption #1  Physical Damage Taken Reduced by 3"  # 2
    assert lines["SUFFIX=2"] == "suf of Piercing #2  Ignore Target's Defense"    # descval 0


def test_both_spellings_of_the_colour_escape_are_stripped(install) -> None:
    # The classic tables carry 0xFF; PD2's own added strings carry the same escape UTF-8
    # encoded, which arrives as "\xc3\xbf" after a latin-1 decode. Missing the second spelling
    # leaves "c0to Minimum Cold Damage c3" on screen.
    install_dir, _loot, archive = install
    text = _lines(install_dir, archive)["SUFFIX=3"]
    assert text == "suf of the Glacier #3  14-21 to Minimum Cold Damage | 45-71 to Maximum Cold Damage"
    assert "c0" not in text and "\xff" not in text and "\xc3" not in text


def test_nothing_the_parser_reads_as_syntax_survives_a_game_string(install) -> None:
    # A stray % would open a token and swallow the rest of the line; a brace would close the
    # description early. Both come from table text this code does not control.
    install_dir, _loot, archive = install
    text = _lines(install_dir, archive)["PREFIX=4"]
    assert "%" not in text and "{" not in text and "}" not in text
    assert "Brace(s) 50pct" in text


def test_the_block_goes_ahead_of_the_host_s_rules(install) -> None:
    """A rule without %CONTINUE% ends processing, and the host has them -- its "filter off" rule
    is one -- so a block appended at the end would never run."""
    install_dir, loot, archive = install
    affix_labels.enable(loot, install_dir, archive)
    lines = loot.read_text(encoding="latin1").splitlines()
    ours = next(i for i, line in enumerate(lines) if affix_labels.BEGIN in line)
    theirs = next(i for i, line in enumerate(lines) if line.startswith("ItemDisplay[box]"))
    assert ours < theirs


def test_every_rule_carries_continue_so_the_host_filter_still_runs(install) -> None:
    install_dir, loot, archive = install
    affix_labels.enable(loot, install_dir, archive)
    _before, block, _after = affix_labels._split(loot.read_text(encoding="latin1"))
    rules = [line for line in block.splitlines() if line.startswith("ItemDisplay")]
    assert rules and all(line.endswith("%CONTINUE%") for line in rules)


def test_the_host_filter_comes_back_byte_for_byte(install) -> None:
    install_dir, loot, archive = install
    original = loot.read_bytes()
    affix_labels.enable(loot, install_dir, archive)
    assert loot.read_bytes() != original
    assert affix_labels.disable(loot) is True
    assert loot.read_bytes() == original
    assert affix_labels.disable(loot) is False


def test_installing_twice_replaces_the_block_rather_than_stacking_it(install) -> None:
    install_dir, loot, archive = install
    first = affix_labels.enable(loot, install_dir, archive)
    second = affix_labels.enable(loot, install_dir, archive)
    assert first.rules == second.rules
    assert loot.read_text(encoding="latin1").count(affix_labels.BEGIN) == 1


def test_status_reads_the_file_rather_than_remembering(install) -> None:
    install_dir, loot, archive = install
    assert affix_labels.status(loot).installed is False
    affix_labels.enable(loot, install_dir, archive)
    assert affix_labels.status(loot).installed is True
    # PD2Launcher rewrites loot.filter from the selected filter, which drops the block silently.
    loot.write_text(HOST_FILTER, encoding="latin1", newline="")
    assert affix_labels.status(loot).installed is False


def test_a_half_removed_block_is_refused_rather_than_guessed_at(install) -> None:
    install_dir, loot, archive = install
    affix_labels.enable(loot, install_dir, archive)
    text = loot.read_text(encoding="latin1").replace(affix_labels.END, "")
    loot.write_text(text, encoding="latin1", newline="")
    with pytest.raises(affix_labels.AffixLabelError, match="closing one"):
        affix_labels.enable(loot, install_dir, archive)


def test_a_missing_filter_is_refused(install) -> None:
    install_dir, loot, archive = install
    loot.unlink()
    assert affix_labels.status(loot).filter_path is None
    assert affix_labels.disable(loot) is False
    with pytest.raises(affix_labels.AffixLabelError, match="no filter"):
        affix_labels.enable(loot, install_dir, archive)
