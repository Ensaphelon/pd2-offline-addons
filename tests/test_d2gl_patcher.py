"""D2GL is the third binary this feature touches, and the one with the least margin for error: it
is rewritten in place, inside somebody else's code. So the rules it has to follow get pinned here —
it patches only what it recognises, it refuses rather than guesses, and it puts the file back
exactly as it was.

The fixture is a hand-built PE holding the same entry shape D2GL's own initialisation emits, rather
than a copy of the real DLL: a test must not need a particular install present, and the byte layout
is the thing under test.
"""
from __future__ import annotations

import struct
from pathlib import Path

import pytest
from pd2_quick_restart import d2gl_patcher

_IMAGE_BASE = 0x10000000
_SECTION_RVA = 0x1000
_SECTION_OFFSET = 0x400


def _fake_dll(*, text: str = d2gl_patcher.DONOR_TEXT,
              size: tuple[int, int] = d2gl_patcher.DONOR_SIZE,
              cell_num: int = d2gl_patcher.DONOR_CELL_NUM,
              entries: int = 1) -> bytes:
    """A PE with `entries` copies of the table entry, the last of which carries `text`."""
    body = bytearray()
    string_at = 0x200                     # inside the section, after the code
    for i in range(entries):
        address = _IMAGE_BASE + _SECTION_RVA + string_at + i * 0x80
        body += b"\xc6\x84\x24" + struct.pack("<I", 0x100) + bytes([cell_num])
        body += b"\xc7\x84\x24" + struct.pack("<I", 0x102) + struct.pack("<HH", *size)
        body += b"\xc7\x84\x24" + struct.pack("<I", 0x108) + struct.pack("<I", address)
    section = bytearray(body.ljust(0x400, b"\x90"))
    for i in range(entries):
        encoded = text.encode("utf-16-le") + b"\0\0"
        at = string_at + i * 0x80
        section[at : at + len(encoded)] = encoded

    pe_at = 0x80
    data = bytearray(b"\0" * _SECTION_OFFSET)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_at)
    data[pe_at : pe_at + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe_at + 6, 1)       # one section
    struct.pack_into("<H", data, pe_at + 20, 0xE0)   # optional header size
    struct.pack_into("<I", data, pe_at + 24 + 28, _IMAGE_BASE)
    header = pe_at + 24 + 0xE0
    data[header : header + 8] = b".text\0\0\0"
    struct.pack_into("<4I", data, header + 8, len(section), _SECTION_RVA,
                     len(section), _SECTION_OFFSET)
    return bytes(data) + bytes(section)


@pytest.fixture()
def dll(tmp_path: Path) -> Path:
    path = tmp_path / d2gl_patcher.D2GL_DLL_NAME
    path.write_bytes(_fake_dll())
    return path


def test_the_entry_is_repointed_at_our_label(dll: Path) -> None:
    d2gl_patcher.enable(dll, width=175, height=36)
    data = dll.read_bytes()
    entry = d2gl_patcher._find_entry(_fake_dll())          # offsets from the untouched original
    assert data[entry.cell_num_at] == 0                    # our graphic has one cell
    assert struct.unpack_from("<HH", data, entry.size_at) == (175, 36)
    assert data[entry.text_at:].startswith(
        d2gl_patcher.LABEL_TEXT.encode("utf-16-le") + b"\0\0")
    assert d2gl_patcher.is_patched(dll) is True


def test_disable_restores_the_library_byte_for_byte(dll: Path) -> None:
    original = dll.read_bytes()
    d2gl_patcher.enable(dll, width=175, height=36)
    assert dll.read_bytes() != original

    assert d2gl_patcher.disable(dll) is True
    assert dll.read_bytes() == original
    assert dll.with_name(dll.name + d2gl_patcher.BACKUP_SUFFIX).exists() is False
    assert d2gl_patcher.disable(dll) is False


def test_enabling_twice_leaves_the_backup_as_the_original(dll: Path) -> None:
    original = dll.read_bytes()
    d2gl_patcher.enable(dll, width=175, height=36)
    d2gl_patcher.enable(dll, width=175, height=36)
    assert dll.with_name(dll.name + d2gl_patcher.BACKUP_SUFFIX).read_bytes() == original


def test_a_build_whose_entry_is_not_there_is_left_alone(tmp_path: Path) -> None:
    # What a D2GL update looks like from here: the table is laid out differently, so there is
    # nothing this recognises. Writing anyway would be writing into whatever moved in.
    moved = tmp_path / d2gl_patcher.D2GL_DLL_NAME
    moved.write_bytes(_fake_dll(size=(999, 36)))
    before = moved.read_bytes()
    with pytest.raises(d2gl_patcher.D2glPatchError):
        d2gl_patcher.enable(moved, width=175, height=36)
    assert moved.read_bytes() == before


def test_several_candidates_are_refused_rather_than_guessed_between(tmp_path: Path) -> None:
    # The real library carries the same table again for other languages. Picking one of those at
    # random would rewrite a label some other player actually sees.
    ambiguous = tmp_path / d2gl_patcher.D2GL_DLL_NAME
    ambiguous.write_bytes(_fake_dll(entries=2))
    before = ambiguous.read_bytes()
    with pytest.raises(d2gl_patcher.D2glPatchError):
        d2gl_patcher.enable(ambiguous, width=175, height=36)
    assert ambiguous.read_bytes() == before
