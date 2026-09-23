"""The RESTART menu label has to travel into the game inside an MPQ — loose files are only read
with `-direct`, which PD2Launcher does not offer. That makes writing into a real game archive part
of installing the feature, so it gets the same treatment as every other write this project makes:
prove it adds what it says, prove it leaves everything else alone, prove it rolls back exactly.

The fixture is a hand-built archive rather than the user's own: a test must not depend on a
particular install being present, and the layout is the one thing being exercised.
"""
from __future__ import annotations

import struct
from pathlib import Path

import pytest
from pd2_offline_addons import mpq_asset

NAME = "data\\local\\ui\\eng\\Restart.dc6"
OTHER = "data\\global\\excel\\Misc.txt"


def _empty_archive(hash_slots: int = 16) -> bytes:
    """A valid, empty MPQ v1: header, an all-free hash table, no blocks."""
    hash_pos = 32
    block_pos = hash_pos + hash_slots * 16
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, block_pos, 0, 3,
                         hash_pos, block_pos, hash_slots, 0)
    free = b"".join(struct.pack("<IIHHI", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)
                    for _ in range(hash_slots))
    return header + mpq_asset._crypt(free, mpq_asset._hash("(hash table)", 3), encrypt=True)


@pytest.fixture()
def archive(tmp_path: Path) -> Path:
    path = tmp_path / "patch_d2.mpq"
    path.write_bytes(_empty_archive())
    return path


def test_a_file_added_reads_back_exactly(archive: Path) -> None:
    payload = b"\x06\x00\x00\x00" + bytes(range(256)) * 4
    assert mpq_asset.contains(archive, NAME) is False
    mpq_asset.add(archive, NAME, payload)
    assert mpq_asset.contains(archive, NAME) is True
    assert mpq_asset.read(archive, NAME) == payload


def test_an_existing_file_keeps_its_bytes_and_position(archive: Path) -> None:
    # The whole point of appending: adding a label must not disturb the data tables PD2 reads.
    first = b"a real file's contents" * 40
    mpq_asset.add(archive, OTHER, first)
    before = mpq_asset.read(archive, OTHER)

    mpq_asset.add(archive, NAME, b"the label")
    assert mpq_asset.read(archive, OTHER) == before == first
    assert mpq_asset.read(archive, NAME) == b"the label"


def test_restore_puts_the_original_archive_back_byte_for_byte(archive: Path) -> None:
    original = archive.read_bytes()
    mpq_asset.add(archive, NAME, b"the label")
    assert archive.read_bytes() != original

    assert mpq_asset.restore(archive) is True
    assert archive.read_bytes() == original
    assert archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX).exists() is False
    assert mpq_asset.restore(archive) is False


def test_the_backup_is_taken_once_so_a_second_add_cannot_overwrite_it(archive: Path) -> None:
    # Same rule Game.exe's own patch follows: a second install must not make the backup a
    # patched archive, leaving nothing to go back to.
    original = archive.read_bytes()
    mpq_asset.add(archive, NAME, b"first")
    mpq_asset.add(archive, NAME, b"second")
    assert archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX).read_bytes() == original
    assert mpq_asset.read(archive, NAME) == b"second"


def test_a_file_that_is_not_an_archive_is_refused(tmp_path: Path) -> None:
    junk = tmp_path / "patch_d2.mpq"
    junk.write_bytes(b"not an archive at all")
    with pytest.raises(mpq_asset.MpqError):
        mpq_asset.contains(junk, NAME)
