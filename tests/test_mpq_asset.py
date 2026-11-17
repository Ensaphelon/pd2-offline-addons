"""The RESTART menu label has to travel into the game inside an MPQ — loose files are only read
with `-direct`, which PD2Launcher does not offer. That makes writing into a real game archive part
of installing the feature, so it gets the same treatment as every other write this project makes:
prove it adds what it says, prove it leaves everything else alone, prove it rolls back exactly.

The fixture is a hand-built archive rather than the user's own: a test must not depend on a
particular install being present, and the layout is the one thing being exercised.
"""
from __future__ import annotations

import struct
import zlib
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


def test_the_pkware_decoder_matches_the_published_vector() -> None:
    """zlib's blast.c ships one test vector, and this is it. The algorithm is not one to take on
    trust from a reading of the format: a wrong table or a reversed bit order decodes the first few
    symbols correctly and then quietly produces plausible rubbish."""
    assert mpq_asset._explode(b"\x00\x04\x82\x24\x25\x8f\x80\x7f", 13) == b"AIAIAIAIAIAIA"


def _add_compressed(archive: Path, name: str, payload: bytes) -> None:
    """Put a file in the way PD2's own archives hold theirs: split into sectors, each one deflated
    and tagged with its method, behind a table of offsets. `add` only ever writes files stored, so
    the shape `read` has to cope with for PD2's tables has to be built here by hand.

    The payload must actually compress — a sector that did not get smaller is stored verbatim with
    no method byte, which is a different case and not the one under test.
    """
    sector = 512 << 3  # the sector shift every D2-era archive uses
    packed = [b"\x02" + zlib.compress(payload[at : at + sector], 9)
              for at in range(0, len(payload), sector)]
    offsets = [4 * (len(packed) + 1)]
    for blob in packed:
        offsets.append(offsets[-1] + len(blob))
    body = struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(packed)
    assert all(len(blob) < sector for blob in packed), "fixture payload did not compress"

    mpq_asset.add(archive, name, body)  # places the bytes and takes the hash slot
    raw = bytearray(archive.read_bytes())
    mpq = mpq_asset._Archive(bytes(raw))
    index = mpq.index_of(name)
    mpq.block_table[index] = [mpq.block_table[index][0], len(body), len(payload),
                              mpq_asset._FLAG_EXISTS | mpq_asset._FLAG_COMPRESS]
    blocks = b"".join(struct.pack("<IIII", *entry) for entry in mpq.block_table)
    raw[mpq.block_pos :] = mpq_asset._crypt(
        blocks, mpq_asset._hash("(block table)", mpq_asset._HASH_FILE_KEY), encrypt=True)
    archive.write_bytes(bytes(raw))


def test_one_of_the_games_own_compressed_files_reads_back_whole(archive: Path) -> None:
    # Several sectors' worth, because the offset table in front of them is the part that has to be
    # read correctly — a one-sector file would pass with the table ignored.
    payload = (b"Toothrow\tbwht\twhit\t" * 900)[:20000]
    _add_compressed(archive, OTHER, payload)
    assert mpq_asset.read(archive, OTHER) == payload


def test_an_encrypted_file_is_refused_rather_than_half_read(archive: Path) -> None:
    mpq_asset.add(archive, OTHER, b"whatever")
    raw = bytearray(archive.read_bytes())
    mpq = mpq_asset._Archive(bytes(raw))
    index = mpq.index_of(OTHER)
    mpq.block_table[index][3] |= mpq_asset._FLAG_ENCRYPTED
    blocks = b"".join(struct.pack("<IIII", *entry) for entry in mpq.block_table)
    raw[mpq.block_pos :] = mpq_asset._crypt(
        blocks, mpq_asset._hash("(block table)", mpq_asset._HASH_FILE_KEY), encrypt=True)
    archive.write_bytes(bytes(raw))

    with pytest.raises(mpq_asset.MpqError, match="encrypted"):
        mpq_asset.read(archive, OTHER)


def test_a_file_that_is_not_an_archive_is_refused(tmp_path: Path) -> None:
    junk = tmp_path / "patch_d2.mpq"
    junk.write_bytes(b"not an archive at all")
    with pytest.raises(mpq_asset.MpqError):
        mpq_asset.contains(junk, NAME)
