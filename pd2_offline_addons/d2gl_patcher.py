"""Teaching D2GL to draw the Restart line like the menu's other lines.

Why this is needed at all. Project Diablo 2 renders through D2GL (glide3x.dll), and with its
`hd_text` feature on — it is on by default — D2GL does not draw the ESC menu's labels from their
graphics. It re-renders them as text with its own font, larger (hd_text_scale) and cleaner. A label
it does not recognise is left as the plain sprite, which is why the Restart line looked dimmer and
smaller than the three beside it. Confirmed live: with hd_text off, all four lines match exactly.

How D2GL recognises a label. Not by name — by the SIZE of the sprite's last cell (d2gl's
hd_text.cpp): for each entry of its `g_options_texts` table it checks
`cell_num == numcells - 1 && cell->width == size.x && cell->height == size.y`, and draws that
entry's text. So the table is a list of {which cell, its dimensions, the words to draw}.

What this changes. One entry, in place. The table is built on the stack by the module's own
initialisation code, so each entry is three immediates: the cell index, the two dimensions packed
into one dword, and a pointer to its wide string. This rewrites the entry for `cfgoptions.dc6`
("CONFIGURE CONTROLS", 167x36) to name our label instead — PD2 has no such line in its menus, it
offers PD2Hotkeys and PD2Options in that place, so the entry is dead weight in this install. No
code moves, nothing is inserted, and the replacement text is written over the old string inside the
room the old string already occupies.

Everything is verified against the bytes actually found before anything is written: the entry's own
dimensions, its cell index, and that its pointer really leads to the string this expects. A D2GL
update that moves any of it leaves the file untouched and the feature simply keeps the plain
sprite, which is how it looked before this existed.
"""
from __future__ import annotations

import re
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path

BACKUP_SUFFIX = ".pd2addons-backup"

D2GL_DLL_NAME = "glide3x.dll"
# The entry being repurposed, as D2GL ships it.
DONOR_TEXT = "CONFIGURE CONTROLS"
DONOR_SIZE = (167, 36)
DONOR_CELL_NUM = 1

LABEL_TEXT = "RESTART"

# `mov dword [esp+disp32], imm32` twice in a row: the packed size, then the string pointer.
_ENTRY = re.compile(
    rb"\xc7\x84\x24....(" + re.escape(struct.pack("<HH", *DONOR_SIZE)) + rb")"
    rb"\xc7\x84\x24....(....)",
    re.DOTALL,
)
# `mov byte [esp+disp32], imm8` — the cell index, immediately before the pair above.
_CELL_NUM = re.compile(rb"\xc6\x84\x24....(.)$", re.DOTALL)


class D2glPatchError(Exception):
    pass


@dataclass(frozen=True)
class _Entry:
    cell_num_at: int
    size_at: int
    text_at: int
    text_capacity: int


def _sections(data: bytes) -> tuple[int, list[tuple[int, int, int, int]]]:
    """(image base, [(virtual address, virtual size, raw offset, raw size)]) — enough to turn a
    pointer into a file offset, and no more."""
    if data[:2] != b"MZ":
        raise D2glPatchError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise D2glPatchError("not a PE file")
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional_size
    sections = []
    for i in range(section_count):
        header = table + i * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<4I", data, header + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))
    return image_base, sections


def _offset_of(data: bytes, address: int) -> int | None:
    image_base, sections = _sections(data)
    rva = address - image_base
    for virtual_address, virtual_size, raw_offset, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_offset + (rva - virtual_address)
    return None


def _find_entry(data: bytes) -> _Entry:
    """The one table entry this repurposes, or an explanation of why it is not there."""
    donor = DONOR_TEXT.encode("utf-16-le") + b"\0\0"
    found = []
    for match in _ENTRY.finditer(data):
        address = struct.unpack("<I", match.group(2))[0]
        offset = _offset_of(data, address)
        if offset is None or data[offset : offset + len(donor)] != donor:
            continue  # another language's copy of the table, or not an entry at all
        before = _CELL_NUM.search(data, max(0, match.start() - 8), match.start())
        if before is None or before.group(1)[0] != DONOR_CELL_NUM:
            continue
        found.append(_Entry(cell_num_at=before.start(1), size_at=match.start(1),
                            text_at=offset, text_capacity=len(donor)))
    if not found:
        raise D2glPatchError(
            f"no {DONOR_TEXT!r} entry of {DONOR_SIZE[0]}x{DONOR_SIZE[1]} found — this D2GL build "
            "lays its label table out differently")
    if len(found) > 1:
        raise D2glPatchError(f"{len(found)} candidate entries — refusing to guess which is the one")
    return found[0]


def is_patched(dll: Path) -> bool:
    data = dll.read_bytes()
    return LABEL_TEXT.encode("utf-16-le") + b"\0\0" in data


def enable(dll: Path, *, width: int, height: int) -> None:
    """Points D2GL's label table at our graphic's dimensions, with our word."""
    data = bytearray(dll.read_bytes())
    if is_patched(dll):
        return
    entry = _find_entry(bytes(data))

    replacement = LABEL_TEXT.encode("utf-16-le") + b"\0\0"
    if len(replacement) > entry.text_capacity:
        raise D2glPatchError(f"{LABEL_TEXT!r} does not fit where {DONOR_TEXT!r} was")

    backup = dll.with_name(dll.name + BACKUP_SUFFIX)
    if not backup.is_file():
        shutil.copy2(dll, backup)

    data[entry.cell_num_at] = 0                      # our graphic has a single cell
    struct.pack_into("<HH", data, entry.size_at, width, height)
    data[entry.text_at : entry.text_at + entry.text_capacity] = replacement.ljust(
        entry.text_capacity, b"\0")
    dll.write_bytes(bytes(data))


def disable(dll: Path) -> bool:
    backup = dll.with_name(dll.name + BACKUP_SUFFIX)
    if not backup.is_file():
        return False
    shutil.copy2(backup, dll)
    backup.unlink()
    return True
