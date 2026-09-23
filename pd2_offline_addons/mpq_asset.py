"""Putting one extra file into a Diablo II MPQ archive, and taking it back out again.

Why this exists. The Quick Restart line in the ESC menu is drawn from a pre-rendered graphic, not
from text: a menu entry names a cell file ("Options", "Exit", "ReturnToGame") and the game loads
`data\\local\\ui\\eng\\<name>.dc6` for it. To have a line that says RESTART, that graphic has to be
somewhere the game's file system will find it — and the game's file system is MPQ archives only.
Loose files on disk are read only when the game is launched with `-direct`, which PD2Launcher does
not offer (measured 2026-09-19: its whole flag set is graphics/skiptobnet/sndbkg/disableAutoUpdate).
Project Diablo 2 ships its OWN menu labels the same way — PD2Options.dc6 and PD2Hotkeys.dc6 live in
pd2assets.mpq — so adding a file to an archive is the game's own mechanism for this, not a trick.

What it does NOT do. It never rewrites, moves, recompresses or removes anything that is already in
the archive: existing file data keeps its exact offsets, and the hash table keeps its size and every
occupied slot. The new file's bytes go where the hash table used to start, and the two tables are
written after it. Rollback is a file copy, same as Game.exe's.

The format handled here is the classic MPQ v1 layout every D2-era archive uses — [file data]
[hash table][block table], both tables encrypted with Blizzard's published algorithm. Anything else
is refused rather than guessed at.
"""
from __future__ import annotations

import shutil
import struct
from pathlib import Path

BACKUP_SUFFIX = ".pd2addons-backup"

_HASH_TABLE_OFFSET = 0
_HASH_NAME_A = 1
_HASH_NAME_B = 2
_HASH_FILE_KEY = 3

_SLOT_EMPTY = 0xFFFFFFFF
_SLOT_DELETED = 0xFFFFFFFE

# Stored, uncompressed, not encrypted. A file with no compression flag has no sector offset table —
# its bytes are simply there — which is the oldest and most widely supported shape there is, and
# the right choice against a Storm.dll from 2000 that predates flags like SINGLE_UNIT.
_FLAG_EXISTS = 0x80000000

_HEADER_FORMAT = "<4sIIHHIIII"


class MpqError(Exception):
    pass


def _build_crypt_table() -> list[int]:
    table = [0] * 0x500
    seed = 0x00100001
    for index1 in range(0x100):
        index2 = index1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 0x10
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[index2] = high | (seed & 0xFFFF)
            index2 += 0x100
    return table


_CRYPT = _build_crypt_table()


def _hash(text: str, hash_type: int) -> int:
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in text.upper():
        c = ord(ch)
        seed1 = (_CRYPT[(hash_type << 8) + c] ^ ((seed1 + seed2) & 0xFFFFFFFF)) & 0xFFFFFFFF
        seed2 = (c + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def _crypt(data: bytes, key: int, *, encrypt: bool) -> bytes:
    """Blizzard's table cipher. Both directions advance the state from the PLAINTEXT word, which is
    the only reason one function can do both."""
    count = len(data) // 4
    words = struct.unpack(f"<{count}I", data[: count * 4])
    seed = 0xEEEEEEEE
    key &= 0xFFFFFFFF
    out = []
    for word in words:
        seed = (seed + _CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        plain = word if encrypt else (word ^ (key + seed)) & 0xFFFFFFFF
        out.append(((plain ^ (key + seed)) & 0xFFFFFFFF) if encrypt else plain)
        key = ((((~key & 0xFFFFFFFF) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return struct.pack(f"<{count}I", *out) + data[count * 4 :]


class _Archive:
    def __init__(self, raw: bytes) -> None:
        offset = raw.find(b"MPQ\x1a")
        if offset < 0:
            raise MpqError("not an MPQ archive")
        (_magic, header_size, _archive_size, fmt, self.sector_shift,
         hash_pos, block_pos, self.hash_count, self.block_count) = struct.unpack_from(
            _HEADER_FORMAT, raw, offset)
        if fmt != 0:
            raise MpqError(f"MPQ format version {fmt + 1} is not supported")
        if offset != 0:
            raise MpqError("archive does not start at the beginning of the file")

        self.raw = raw
        self.header_size = header_size
        self.hash_pos = hash_pos
        self.block_pos = block_pos

        hash_bytes = _crypt(raw[hash_pos : hash_pos + self.hash_count * 16],
                            _hash("(hash table)", _HASH_FILE_KEY), encrypt=False)
        self.hash_table = [list(struct.unpack_from("<IIHHI", hash_bytes, i * 16))
                           for i in range(self.hash_count)]
        block_bytes = _crypt(raw[block_pos : block_pos + self.block_count * 16],
                             _hash("(block table)", _HASH_FILE_KEY), encrypt=False)
        self.block_table = [list(struct.unpack_from("<IIII", block_bytes, i * 16))
                            for i in range(self.block_count)]

        # Everything before the hash table is file data; the tables must be the tail, or appending
        # after the data would land on top of something.
        if self.block_pos < self.hash_pos or self.block_pos + self.block_count * 16 != len(raw):
            raise MpqError("unexpected archive layout — tables are not at the end")

    def index_of(self, name: str) -> int | None:
        start = _hash(name, _HASH_TABLE_OFFSET) % self.hash_count
        name_a, name_b = _hash(name, _HASH_NAME_A), _hash(name, _HASH_NAME_B)
        for step in range(self.hash_count):
            slot = self.hash_table[(start + step) % self.hash_count]
            if slot[4] == _SLOT_EMPTY:
                return None
            if slot[0] == name_a and slot[1] == name_b:
                return slot[4]
        return None

    def free_slot_for(self, name: str) -> int:
        start = _hash(name, _HASH_TABLE_OFFSET) % self.hash_count
        for step in range(self.hash_count):
            index = (start + step) % self.hash_count
            if self.hash_table[index][4] in (_SLOT_EMPTY, _SLOT_DELETED):
                return index
        raise MpqError("the archive's hash table is full")


def contains(archive: Path, name: str) -> bool:
    return _Archive(archive.read_bytes()).index_of(name) is not None


def read(archive: Path, name: str) -> bytes | None:
    """Reads back a file added by `add` — stored, unencrypted, no sector table. Not a general MPQ
    reader: it exists so an install can be verified against the archive it just wrote."""
    mpq = _Archive(archive.read_bytes())
    index = mpq.index_of(name)
    if index is None:
        return None
    position, compressed_size, size, flags = mpq.block_table[index]
    if flags & ~_FLAG_EXISTS:
        raise MpqError(f"{name} is not a plainly stored file (flags {flags:#x})")
    return mpq.raw[position : position + min(compressed_size, size)]


def add(archive: Path, name: str, payload: bytes) -> None:
    """Adds (or replaces) one stored file, backing the archive up first if it has not been already.

    Replacing reuses the hash slot and appends new bytes rather than writing over the old ones: the
    old data stays where it is, unreferenced, which keeps every other file's offset untouched.
    """
    raw = archive.read_bytes()
    mpq = _Archive(raw)

    slot_index = mpq.free_slot_for(name)
    existing = mpq.index_of(name)

    data_end = mpq.hash_pos                 # where the tables begin today
    block_index = existing if existing is not None else len(mpq.block_table)
    entry = [data_end, len(payload), len(payload), _FLAG_EXISTS]
    if existing is not None:
        mpq.block_table[existing] = entry
    else:
        mpq.block_table.append(entry)
        mpq.hash_table[slot_index] = [_hash(name, _HASH_NAME_A), _hash(name, _HASH_NAME_B),
                                      0, 0, block_index]

    new_hash_pos = data_end + len(payload)
    new_block_pos = new_hash_pos + mpq.hash_count * 16
    total = new_block_pos + len(mpq.block_table) * 16

    header = bytearray(raw[: mpq.header_size])
    struct.pack_into(_HEADER_FORMAT, header, 0, b"MPQ\x1a", mpq.header_size, total, 0,
                     mpq.sector_shift, new_hash_pos, new_block_pos,
                     mpq.hash_count, len(mpq.block_table))

    hash_bytes = b"".join(struct.pack("<IIHHI", *slot) for slot in mpq.hash_table)
    block_bytes = b"".join(struct.pack("<IIII", *block) for block in mpq.block_table)

    out = bytearray(raw[:data_end])
    out[: len(header)] = header
    out += payload
    out += _crypt(hash_bytes, _hash("(hash table)", _HASH_FILE_KEY), encrypt=True)
    out += _crypt(block_bytes, _hash("(block table)", _HASH_FILE_KEY), encrypt=True)

    backup = archive.with_name(archive.name + BACKUP_SUFFIX)
    if not backup.is_file():
        shutil.copy2(archive, backup)
    archive.write_bytes(bytes(out))


def restore(archive: Path) -> bool:
    """Puts the untouched archive back. True if a backup was there to put back."""
    backup = archive.with_name(archive.name + BACKUP_SUFFIX)
    if not backup.is_file():
        return False
    shutil.copy2(backup, archive)
    backup.unlink()
    return True
