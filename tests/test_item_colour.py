"""Recolouring a unique item means editing a table the game parses at startup, inside one of
PD2's own archives. Three things therefore have to be proven rather than assumed: that the two
colour bytes are the ones written, that nothing else in the 332-byte record moves, and that a
layout which is not the expected one is refused instead of written over.

The fixture builds the table by hand, for the same reason test_mpq_asset does: a test must not
need a particular game install to be present, and the layout is the thing under test.
"""
from __future__ import annotations

import struct
from pathlib import Path

import pytest
from pd2_offline_addons import item_colour, mpq_asset


def _empty_archive(hash_slots: int = 64) -> bytes:
    """A valid, empty MPQ v1: header, an all-free hash table, no blocks."""
    hash_pos = 32
    block_pos = hash_pos + hash_slots * 16
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, block_pos, 0, 3,
                         hash_pos, block_pos, hash_slots, 0)
    free = b"".join(struct.pack("<IIHHI", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)
                    for _ in range(hash_slots))
    return header + mpq_asset._crypt(free, mpq_asset._hash("(hash table)", 3), encrypt=True)


# Colors.txt as PD2 ships it: the row order is the index the compiled table stores.
CODES = ["whit", "lgry", "dgry", "blac", "lblu", "dblu", "cblu", "lred", "dred", "cred",
         "lgrn", "dgrn", "cgrn", "lyel", "dyel", "lgld", "dgld", "lpur", "dpur", "oran", "bwht"]
COLORS_TXT = ("Transform Color\tCode\r\n"
              + "".join(f"name {code}\t{code}\r\n" for code in CODES)).encode("latin1")

BWHT, WHIT, DGRY = CODES.index("bwht"), CODES.index("whit"), CODES.index("dgry")


def _record(name: str, chrtransform: int, invtransform: int) -> bytes:
    record = bytearray(item_colour._RECORD_SIZE)
    record[item_colour._NAME_AT : item_colour._NAME_AT + len(name)] = name.encode("latin1")
    record[item_colour._CHR_TRANSFORM_AT] = chrtransform
    record[item_colour._INV_TRANSFORM_AT] = invtransform
    # Something recognisable in every other byte, so a test can tell if one of them moved.
    for index in range(100, item_colour._RECORD_SIZE):
        record[index] = index % 251
    return bytes(record)


def _table(*records: bytes) -> bytes:
    return struct.pack("<I", len(records)) + b"".join(records)


STOCK = _table(
    _record("The Gnasher", 0xFF, 0xFF),
    _record("Toothrow", BWHT, WHIT),
    _record("Atma's Wail", DGRY, DGRY),
)


@pytest.fixture()
def install(tmp_path: Path) -> Path:
    archive = tmp_path / item_colour.ARCHIVE_NAME
    archive.write_bytes(_empty_archive())
    mpq_asset.add(archive, item_colour.COLOURS_PATH, COLORS_TXT)
    mpq_asset.add(archive, item_colour.TABLE_PATH, STOCK)
    # add() backed the archive up on its first write, and that backup is the EMPTY archive this
    # fixture started from. Dropping it leaves the finished archive standing in for the one PD2
    # ships, which is the state a first recolour actually meets.
    archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX).unlink()
    return tmp_path


def test_the_colour_the_game_will_draw_it_in_is_the_one_asked_for(install: Path) -> None:
    item_colour.enable(install, {"Toothrow": "dgry"})
    table = mpq_asset.read(install / item_colour.ARCHIVE_NAME, item_colour.TABLE_PATH)
    base = item_colour._FIRST_RECORD + item_colour._RECORD_SIZE
    assert table[base + item_colour._CHR_TRANSFORM_AT] == DGRY
    assert table[base + item_colour._INV_TRANSFORM_AT] == DGRY


def test_nothing_in_the_record_moves_but_those_two_bytes(install: Path) -> None:
    # The record carries the item's level, its properties and its code; a recolour that shifted
    # any of it would change what the item IS, silently and across a whole save.
    item_colour.enable(install, {"Toothrow": "dgry"})
    table = mpq_asset.read(install / item_colour.ARCHIVE_NAME, item_colour.TABLE_PATH)
    assert len(table) == len(STOCK)
    changed = [index for index in range(len(STOCK)) if table[index] != STOCK[index]]
    base = item_colour._FIRST_RECORD + item_colour._RECORD_SIZE
    assert changed == [base + item_colour._CHR_TRANSFORM_AT, base + item_colour._INV_TRANSFORM_AT]


def test_the_other_uniques_keep_their_own_colours(install: Path) -> None:
    item_colour.enable(install, {"Toothrow": "blac"})
    state = item_colour.status(install, {"The Gnasher": "", "Atma's Wail": "", "Toothrow": ""})
    assert state.now == {"The Gnasher": None, "Atma's Wail": "dgry", "Toothrow": "blac"}


def test_recolouring_twice_replaces_rather_than_layers(install: Path) -> None:
    # The second run has to start from PD2's table, not from the one the first run wrote —
    # otherwise "what PD2 ships" drifts and there is nothing to go back to.
    item_colour.enable(install, {"Toothrow": "blac"})
    state = item_colour.enable(install, {"Toothrow": "lgry"})
    assert state.now == {"Toothrow": "lgry"}
    assert state.stock == {"Toothrow": "bwht"}
    assert state.installed is True


def test_status_reads_the_archive_rather_than_remembering(install: Path) -> None:
    before = item_colour.status(install)
    assert before.now == {"Toothrow": "bwht"} == before.stock
    assert before.installed is False

    item_colour.enable(install)
    assert item_colour.status(install).now == {"Toothrow": item_colour.RECOLOURS["Toothrow"]}

    assert item_colour.disable(install) is True
    assert item_colour.status(install).now == {"Toothrow": "bwht"}


def test_the_archive_comes_back_byte_for_byte(install: Path) -> None:
    archive = install / item_colour.ARCHIVE_NAME
    original = archive.read_bytes()
    item_colour.enable(install, {"Toothrow": "blac"})
    assert archive.read_bytes() != original
    assert item_colour.disable(install) is True
    assert archive.read_bytes() == original


def test_a_colour_the_game_does_not_have_is_refused(install: Path) -> None:
    archive = install / item_colour.ARCHIVE_NAME
    original = archive.read_bytes()
    with pytest.raises(item_colour.ItemColourError, match="not a colour"):
        item_colour.enable(install, {"Toothrow": "pink"})
    assert archive.read_bytes() == original


def test_an_item_the_table_does_not_have_is_refused(install: Path) -> None:
    with pytest.raises(item_colour.ItemColourError, match="no unique item called"):
        item_colour.enable(install, {"Bul-Kathos' Tribal Guardian": "dgry"})


def _ship(install: Path, table: bytes) -> None:
    """Make `table` the one the install came with, the way a PD2 update would."""
    archive = install / item_colour.ARCHIVE_NAME
    mpq_asset.add(archive, item_colour.TABLE_PATH, table)
    backup = archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX)
    if backup.is_file():
        backup.unlink()


def test_a_table_whose_records_are_a_different_size_is_refused(install: Path) -> None:
    """The guard that matters most: a PD2 update that adds a column moves every byte after it, and
    writing offset 56 into that would set something other than a colour on the wrong item."""
    _ship(install, STOCK + b"\x00" * 8)
    with pytest.raises(item_colour.ItemColourError, match="not this file's"):
        item_colour.enable(install, {"Toothrow": "dgry"})


def test_a_colour_byte_that_is_not_a_colour_is_refused(install: Path) -> None:
    """The second guard: the record size can stay the same while the columns inside it move."""
    _ship(install, _table(_record("Toothrow", len(CODES) + 3, WHIT)))
    with pytest.raises(item_colour.ItemColourError, match="not one of the 21 colours"):
        item_colour.enable(install, {"Toothrow": "dgry"})


def test_an_install_with_no_data_archive_says_so(tmp_path: Path) -> None:
    assert item_colour.status(tmp_path).archive is None
    assert item_colour.disable(tmp_path) is False
    with pytest.raises(item_colour.ItemColourError, match="no pd2data.mpq"):
        item_colour.enable(tmp_path)
