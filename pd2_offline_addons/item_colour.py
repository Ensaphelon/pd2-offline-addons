r"""Changing the colour a unique item is drawn in.

Diablo II does not tint an item's artwork — it palette-shifts it, and which shift a unique gets is
a column in the game's own table. `UniqueItems.txt` has two: `chrtransform` for the item on the
character, `invtransform` for its inventory sprite, each holding a four-letter code from
`Colors.txt`. 351 of PD2's 485 uniques set them, so this is the game's ordinary mechanism and not
a trick; Toothrow sets them to `bwht` (Bright White) and `whit` (White), the brightest pair in the
table, which is why it glares.

PD2 has no dye or skin system that reaches this. Its Alternate Item Skins are a separate drop with
its own graphic, they exist for a short list of items that does not include Toothrow, and nothing
in the string table or in ProjectDiablo.dll offers a colour choice. So the colour has to come from
the table.

Why the .bin and not the .txt. The game compiles the excel tables and reads the compiled form:
`UniqueItems.bin` is in pd2data.mpq next to the .txt, 484 records of 332 bytes, and it is PD2's
own rebuild rather than a leftover — it carries PD2's uniques, which the stock 1.13 file does not.
Editing the .txt therefore changes nothing unless the game is started with `-direct -txt`, and
PD2Launcher has no way to pass either (its whole flag set is graphics/skiptobnet/sndbkg/
disableAutoUpdate). Patching the compiled table instead keeps the game being started the way it
always is, which is the same reason the RESTART label goes into an archive.

Why bytes 56 and 57 are known rather than assumed. They were not read off a struct listing. Every
one of the 332 byte offsets in a record was tested against what the .txt says that item's colour
is, across all 484 records at once: offset 56 agrees with `chrtransform` on every record and
nothing else does, 57 likewise with `invtransform`. An item with no colour set reads 0xFF. The
record size, the record count in the file's first four bytes, and those two columns are all
re-checked at install time, so a PD2 update that changes the layout is refused rather than
written over.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from . import mpq_asset

ARCHIVE_NAME = "pd2data.mpq"
TABLE_PATH = "data\\global\\excel\\UniqueItems.bin"
COLOURS_PATH = "data\\global\\excel\\Colors.txt"

_RECORD_SIZE = 332
_FIRST_RECORD = 4          # the file opens with its own record count
_NAME_AT = 2               # inside the record
_NAME_SIZE = 32
_CHR_TRANSFORM_AT = 56
_INV_TRANSFORM_AT = 57
_NO_COLOUR = 0xFF

# The colours this project applies, and the only opinion in this file. Toothrow is here because
# white-on-white at full brightness is the one piece of unique artwork that reads as a bug.
RECOLOURS = {"Toothrow": "dgry"}


class ItemColourError(Exception):
    pass


@dataclass(frozen=True)
class ColourStatus:
    """What the archive says right now — read out of it, never remembered from an earlier run."""

    archive: Path | None
    backup_present: bool
    #: item name -> the colour code the archive holds today, None if it sets none
    now: dict[str, str | None]
    #: the same items as Project Diablo 2 ships them
    stock: dict[str, str | None]

    @property
    def installed(self) -> bool:
        return any(self.now.get(name) != self.stock.get(name) for name in self.now)


def archive_in(install_dir: Path) -> Path | None:
    candidate = install_dir / ARCHIVE_NAME
    return candidate if candidate.is_file() else None


def colour_codes(archive: Path) -> list[str]:
    """The four-letter codes in Colors.txt order, because that order IS the index the compiled
    table stores. Taken from the game's own file rather than written down here, so a mod that
    reorders them cannot silently shift every colour by one."""
    raw = mpq_asset.read(archive, COLOURS_PATH)
    if raw is None:
        raise ItemColourError(f"{ARCHIVE_NAME} has no {COLOURS_PATH}")
    lines = raw.decode("latin1").replace("\r\n", "\n").strip("\n").split("\n")
    header = lines[0].split("\t")
    try:
        column = header.index("Code")
    except ValueError as error:
        raise ItemColourError("Colors.txt has no Code column") from error
    codes = [line.split("\t")[column].strip() for line in lines[1:]]
    if not codes or any(len(code) != 4 for code in codes):
        raise ItemColourError("Colors.txt does not read as a list of four-letter codes")
    return codes


def _records(table: bytes) -> list[tuple[int, str]]:
    """(offset, name) for every record, after checking the file really has the shape expected."""
    if len(table) < _FIRST_RECORD:
        raise ItemColourError("UniqueItems.bin is too short to be a table")
    count = struct.unpack_from("<I", table, 0)[0]
    if _FIRST_RECORD + count * _RECORD_SIZE != len(table):
        raise ItemColourError(
            f"UniqueItems.bin says {count} records but is {len(table)} bytes, which is not "
            f"{_FIRST_RECORD} + {count}x{_RECORD_SIZE} — the layout this reads is not this file's"
        )
    out = []
    for index in range(count):
        base = _FIRST_RECORD + index * _RECORD_SIZE
        name = table[base + _NAME_AT : base + _NAME_AT + _NAME_SIZE].split(b"\x00")[0]
        out.append((base, name.decode("latin1")))
    return out


def _check_colour_columns(table: bytes, records: list[tuple[int, str]], codes: list[str]) -> None:
    """Both columns must hold a real colour index, or none at all, in every single record. This is
    the guard that catches a changed record layout before anything is written."""
    for base, name in records:
        for offset, column in ((_CHR_TRANSFORM_AT, "chrtransform"),
                               (_INV_TRANSFORM_AT, "invtransform")):
            value = table[base + offset]
            if value != _NO_COLOUR and value >= len(codes):
                raise ItemColourError(
                    f"{name}'s {column} byte is {value}, which is not one of the {len(codes)} "
                    f"colours — UniqueItems.bin is not laid out the way this expects"
                )


def _colour_of(table: bytes, base: int, codes: list[str]) -> str | None:
    value = table[base + _CHR_TRANSFORM_AT]
    return None if value == _NO_COLOUR else codes[value]


def _pristine_table(archive: Path) -> bytes:
    """PD2's own table, from the backup when one exists.

    Reading the live archive would compound: recolouring twice would be reading back a colour this
    code had already written, and `stock` would stop meaning what PD2 ships.
    """
    backup = archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX)
    source = backup if backup.is_file() else archive
    table = mpq_asset.read(source, TABLE_PATH)
    if table is None:
        raise ItemColourError(f"{source.name} has no {TABLE_PATH}")
    return table


def status(install_dir: Path, wanted: dict[str, str] | None = None) -> ColourStatus:
    archive = archive_in(install_dir)
    names = list((wanted or RECOLOURS).keys())
    if archive is None:
        return ColourStatus(None, False, {}, {})

    codes = colour_codes(archive)
    live = mpq_asset.read(archive, TABLE_PATH)
    if live is None:
        raise ItemColourError(f"{ARCHIVE_NAME} has no {TABLE_PATH}")
    stock_table = _pristine_table(archive)

    now, stock = {}, {}
    for table, into in ((live, now), (stock_table, stock)):
        by_name = {name: base for base, name in _records(table)}
        for name in names:
            base = by_name.get(name)
            into[name] = _colour_of(table, base, codes) if base is not None else None
    return ColourStatus(
        archive=archive,
        backup_present=archive.with_name(archive.name + mpq_asset.BACKUP_SUFFIX).is_file(),
        now=now,
        stock=stock,
    )


def enable(install_dir: Path, wanted: dict[str, str] | None = None) -> ColourStatus:
    """Writes the wanted colours into the archive's compiled table and proves they read back.

    Always rebuilt from PD2's own table, so running this again with a different colour replaces
    the last one instead of layering on it.
    """
    archive = archive_in(install_dir)
    if archive is None:
        raise ItemColourError(f"no {ARCHIVE_NAME} in {install_dir}")

    wanted = dict(wanted or RECOLOURS)
    codes = colour_codes(archive)
    for name, code in wanted.items():
        if code not in codes:
            raise ItemColourError(
                f"{code!r} is not a colour in this game's Colors.txt. The {len(codes)} there are: "
                + " ".join(codes)
            )

    table = bytearray(_pristine_table(archive))
    records = _records(bytes(table))
    _check_colour_columns(bytes(table), records, codes)

    by_name: dict[str, list[int]] = {}
    for base, name in records:
        by_name.setdefault(name, []).append(base)
    for name, code in wanted.items():
        bases = by_name.get(name, [])
        if not bases:
            raise ItemColourError(f"no unique item called {name!r} in UniqueItems.bin")
        if len(bases) > 1:
            # Two records of the same name would make "recolour Toothrow" ambiguous, and quietly
            # doing one of them is worse than stopping.
            raise ItemColourError(f"{name!r} appears {len(bases)} times in UniqueItems.bin")
        for base in bases:
            table[base + _CHR_TRANSFORM_AT] = codes.index(code)
            table[base + _INV_TRANSFORM_AT] = codes.index(code)

    payload = bytes(table)
    mpq_asset.add(archive, TABLE_PATH, payload)
    # Read it back out of the archive rather than trusting the write: this is a table the game
    # parses at startup, and a half-written one is not something a player should have to diagnose.
    if mpq_asset.read(archive, TABLE_PATH) != payload:
        mpq_asset.restore(archive)
        raise ItemColourError(f"the table did not read back from {archive.name} — rolled back")
    return status(install_dir, wanted)


def disable(install_dir: Path) -> bool:
    """Puts PD2's own archive back. True if there was a backup to put back."""
    archive = archive_in(install_dir)
    if archive is None:
        return False
    return mpq_asset.restore(archive)
