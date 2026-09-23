"""Quick Restart: getting a plugin DLL into the game, and taking it back out.

The plugin has to be inside the game process to add anything to the ESC menu, and there is
exactly one moment it can get in: process creation. Injecting into a game that is already
running does not work here — with the bottle's own session enumerated, every process is
openable except Game.exe, which refuses OpenProcess outright, down to
PROCESS_QUERY_LIMITED_INFORMATION (measured 2026-09-19).

So the game is made to load the DLL itself, by adding it to Game.exe's import table. The loader
then pulls it in before anything else runs, and the game keeps being started the way it always
is — through PD2Launcher, with no launcher of ours in the way.

The edit is additive and reversible: a new section carrying a rebuilt import directory is
appended, the original bytes are kept next to it, and turning the feature off puts them back.
Nothing belonging to Project Diablo 2 itself (ProjectDiablo.dll, BH.dll, the MPQs) is touched.

Two things follow from this being a patch to a file the mod updates:
  * a PD2 update overwrites Game.exe and silently takes the feature with it. So "is it on" is
    answered by reading the import table, never by a flag we stored — a flag would drift.
  * the game must not be running. That is the same precondition every other write in this app
    already has (see cain_write_worker.is_game_running).
"""
from __future__ import annotations

import json
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path

from . import d2gl_patcher, mpq_asset

PLUGIN_DLL_NAME = "pd2addons.dll"
# Any exported symbol will do — an import entry has to bind to something. The DLL does its work
# from DllMain; this exists only so there is a name to reference.
PLUGIN_ANCHOR = "pd2addons_anchor"
BACKUP_SUFFIX = ".pd2addons-backup"

# Game.exe FileVersion the plugin's addresses were derived for. 1.13d moved almost everything,
# so patching anything else in would be writing to addresses that mean something different.
SUPPORTED_GAME_VERSION = "1.0.13.60"

_IMAGE_DIRECTORY_ENTRY_IMPORT = 1
_IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT = 11
_SECTION_HEADER_SIZE = 40
_IMPORT_DESCRIPTOR_SIZE = 20


class GamePatchError(Exception):
    pass


# The plugin ships built, next to its own sources — same reasoning as mule_template: installing
# the feature must not require a Windows cross-compiler on the user's machine.
PLUGIN_SOURCE = (
    Path(__file__).resolve().parent.parent / PLUGIN_DLL_NAME
)

# The RESTART label, and how it reaches the game. A menu entry names a cell file, which the game
# loads out of an MPQ — loose files on disk are only read with `-direct`, which PD2Launcher does
# not offer. So the graphic is added to an archive the game already loads.
#
# That archive is pd2assets.mpq, for one reason: it is where Project Diablo 2 keeps its OWN menu
# labels (PD2Options.dc6, PD2Hotkeys.dc6), which the Options submenu visibly draws — so it is
# proven to be searched for exactly this kind of file. patch_d2.mpq was tried first, being far
# smaller to back up, and rejected on evidence: its data\global\excel\Misc.txt is the stock
# 152-row 1.13c table with none of PD2's own codes in it, so it is the untouched vanilla patch
# archive and sits below PD2's own. Guessing wrong here is not cheap — a menu entry naming a
# graphic the game cannot find takes the game down with it.
LABEL_SOURCE = (
    Path(__file__).resolve().parent.parent / "assets" / "Restart.dc6"
)
LABEL_CELL_FILE = "Restart"
LABEL_ARCHIVE_NAME = "pd2assets.mpq"
LABEL_ARCHIVE_PATH = "data\\local\\ui\\eng\\Restart.dc6"
# The plugin reads this to learn the label is really installed. Without it, it leaves the entry
# with the cell file it copied — a wrong name here would mean the game looking for a graphic that
# is not there, which it does not survive politely.
LABEL_MARKER_NAME = "pd2addons.label"


def find_game_exe(save_directory: str | None) -> Path | None:
    """Where Game.exe lives, worked out from the save directory the app is already configured
    with. Project Diablo 2 keeps the game one level up in ProjectD2/; a plain Diablo II install
    has it beside the Save folder. Both are checked, and nothing is guessed beyond that."""
    if not save_directory:
        return None
    root = Path(save_directory).parent
    for candidate in (root / "ProjectD2" / "Game.exe", root / "Game.exe"):
        if candidate.is_file():
            return candidate
    return None


@dataclass(frozen=True)
class PatchStatus:
    """What the file on disk actually says, not what we remember doing to it."""

    installed: bool
    backup_present: bool
    game_version: str | None
    supported: bool


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


class _PE:
    """The few pieces of a PE32 this needs, read straight out of the bytes.

    Deliberately not pefile: the backend must not grow a dependency to add one section, and what
    is needed here is a handful of fields at fixed offsets.
    """

    def __init__(self, data: bytearray) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise GamePatchError("not an executable")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise GamePatchError("not a PE executable")
        self.optional = self.pe + 24
        magic = struct.unpack_from("<H", data, self.optional)[0]
        if magic != 0x10B:
            raise GamePatchError("not a 32-bit executable — this only knows PE32")
        self.section_count = struct.unpack_from("<H", data, self.pe + 6)[0]
        self.optional_size = struct.unpack_from("<H", data, self.pe + 20)[0]
        self.sections = self.optional + self.optional_size
        self.section_alignment = struct.unpack_from("<I", data, self.optional + 32)[0]
        self.file_alignment = struct.unpack_from("<I", data, self.optional + 36)[0]

    def directory(self, index: int) -> tuple[int, int]:
        offset = self.optional + 96 + index * 8
        return struct.unpack_from("<II", self.data, offset)

    def set_directory(self, index: int, rva: int, size: int) -> None:
        struct.pack_into("<II", self.data, self.optional + 96 + index * 8, rva, size)

    def section_headers(self) -> list[int]:
        return [self.sections + i * _SECTION_HEADER_SIZE for i in range(self.section_count)]

    def rva_to_offset(self, rva: int) -> int:
        for header in self.section_headers():
            virtual_address, = struct.unpack_from("<I", self.data, header + 12)
            virtual_size, = struct.unpack_from("<I", self.data, header + 8)
            raw_size, = struct.unpack_from("<I", self.data, header + 16)
            raw_pointer, = struct.unpack_from("<I", self.data, header + 20)
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_pointer + (rva - virtual_address)
        raise GamePatchError(f"address {rva:#x} is not inside any section")

    def imported_dlls(self) -> list[str]:
        rva, size = self.directory(_IMAGE_DIRECTORY_ENTRY_IMPORT)
        if not rva or not size:
            return []
        names: list[str] = []
        offset = self.rva_to_offset(rva)
        while True:
            descriptor = self.data[offset:offset + _IMPORT_DESCRIPTOR_SIZE]
            if len(descriptor) < _IMPORT_DESCRIPTOR_SIZE or descriptor == bytes(_IMPORT_DESCRIPTOR_SIZE):
                break
            name_rva = struct.unpack_from("<I", descriptor, 12)[0]
            if name_rva:
                name_offset = self.rva_to_offset(name_rva)
                end = self.data.index(b"\0", name_offset)
                names.append(self.data[name_offset:end].decode("latin-1"))
            offset += _IMPORT_DESCRIPTOR_SIZE
        return names


def _game_version(data: bytes) -> str | None:
    """Game.exe's FileVersion, read from its VS_FIXEDFILEINFO.

    The same check Project Diablo 2's own BH makes, and for the same reason: 1.13c and 1.13d are
    different games as far as any of these addresses are concerned. Located by its signature
    rather than by walking the resource tree — one number is all that is wanted here.
    """
    marker = struct.pack("<I", 0xFEEF04BD)
    at = data.find(marker)
    if at < 0:
        return None
    ms, ls = struct.unpack_from("<II", data, at + 8)
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def _build_import_section(pe: _PE, dll_name: str, symbol: str, section_rva: int) -> bytes:
    """A complete replacement import directory, laid out to live at `section_rva`.

    Every original descriptor is copied across unchanged — they keep pointing at the strings and
    thunks already in the file — and one more is appended for our DLL, with its own thunks and
    name. The loader reads this instead of the original array; the original bytes stay where they
    were and are never written to.
    """
    original_rva, _ = pe.directory(_IMAGE_DIRECTORY_ENTRY_IMPORT)
    if not original_rva:
        raise GamePatchError("this executable imports nothing at all, which cannot be right")

    offset = pe.rva_to_offset(original_rva)
    descriptors: list[bytes] = []
    while True:
        chunk = bytes(pe.data[offset:offset + _IMPORT_DESCRIPTOR_SIZE])
        if len(chunk) < _IMPORT_DESCRIPTOR_SIZE or chunk == bytes(_IMPORT_DESCRIPTOR_SIZE):
            break
        descriptors.append(chunk)
        offset += _IMPORT_DESCRIPTOR_SIZE

    # Layout: [descriptors][terminator][OFT][FT][hint/name][dll name]
    table_size = (len(descriptors) + 2) * _IMPORT_DESCRIPTOR_SIZE
    oft_rva = section_rva + table_size
    ft_rva = oft_rva + 8               # one thunk plus its null terminator
    hint_rva = ft_rva + 8
    name_rva = hint_rva + 2 + len(symbol) + 1

    ours = struct.pack("<IIIII", oft_rva, 0, 0, name_rva, ft_rva)

    blob = bytearray()
    for descriptor in descriptors:
        blob += descriptor
    blob += ours
    blob += bytes(_IMPORT_DESCRIPTOR_SIZE)          # terminator
    blob += struct.pack("<II", hint_rva, 0)         # OFT: one name thunk, then null
    blob += struct.pack("<II", hint_rva, 0)         # FT: same, patched by the loader
    blob += struct.pack("<H", 0) + symbol.encode("ascii") + b"\0"
    blob += dll_name.encode("ascii") + b"\0"
    return bytes(blob)


def _append_section(pe: _PE, name: bytes, payload: bytes) -> tuple[bytearray, int]:
    """Adds one section carrying `payload`, returning the new file and the section's RVA.

    There has to be room in the headers for another section header — if the first section's data
    starts right after the last header, adding one would overwrite code. Every Game.exe seen has
    the usual slack, but it is checked rather than assumed.
    """
    headers = pe.section_headers()
    last = headers[-1]
    last_rva, = struct.unpack_from("<I", pe.data, last + 12)
    last_vsize, = struct.unpack_from("<I", pe.data, last + 8)
    last_raw, = struct.unpack_from("<I", pe.data, last + 16)
    last_ptr, = struct.unpack_from("<I", pe.data, last + 20)

    new_header = pe.sections + pe.section_count * _SECTION_HEADER_SIZE
    first_raw = min(
        struct.unpack_from("<I", pe.data, h + 20)[0]
        for h in headers
        if struct.unpack_from("<I", pe.data, h + 20)[0]
    )
    if new_header + _SECTION_HEADER_SIZE > first_raw:
        raise GamePatchError("no room in the headers for another section")

    section_rva = _align(last_rva + max(last_vsize, last_raw), pe.section_alignment)
    raw_pointer = _align(last_ptr + last_raw, pe.file_alignment)

    data = bytearray(pe.data)
    if len(data) < raw_pointer:
        data += bytes(raw_pointer - len(data))
    data = data[:raw_pointer]

    raw_size = _align(len(payload), pe.file_alignment)
    data += payload + bytes(raw_size - len(payload))

    struct.pack_into(
        "<8sIIIIIIHHI", data, new_header,
        name.ljust(8, b"\0"), len(payload), section_rva, raw_size, raw_pointer,
        0, 0, 0, 0,
        0xC0000040,  # initialised data, readable, writable — the loader writes the thunks
    )
    struct.pack_into("<H", data, pe.pe + 6, pe.section_count + 1)

    size_of_image = _align(section_rva + len(payload), pe.section_alignment)
    struct.pack_into("<I", data, pe.optional + 56, size_of_image)
    return data, section_rva


def launcher_restores_game_exe(game_exe: Path) -> bool | None:
    """Whether PD2Launcher will put its own Game.exe back over ours on the next launch.

    It does exactly that with its file check on (measured 2026-09-19: the patched file came back
    as the pristine original, original timestamp and all, and the plugin never loaded). So the
    patch only survives while the launcher's auto-update is off, and saying so up front is worth
    more than letting someone discover it as "the feature silently does nothing".

    None when there is no launcher settings file to read — an install that is not driven by
    PD2Launcher has nothing to warn about.
    """
    settings = game_exe.parent / "AppData" / "launcherSettings.json"
    if not settings.is_file():
        return None
    try:
        data = json.loads(settings.read_text())
        return not bool(data.get("LauncherArgs", {}).get("disableAutoUpdate"))
    except (OSError, ValueError):
        return None


def status(game_exe: Path) -> PatchStatus:
    """What the files say right now. Reading the import table rather than trusting a stored flag
    is the point: a PD2 update replaces Game.exe and takes the patch with it, and a flag would
    keep claiming the feature is on."""
    if not game_exe.is_file():
        return PatchStatus(installed=False, backup_present=False, game_version=None, supported=False)

    data = game_exe.read_bytes()
    version = _game_version(data)
    try:
        imports = _PE(bytearray(data)).imported_dlls()
    except GamePatchError:
        imports = []

    return PatchStatus(
        installed=any(name.lower() == PLUGIN_DLL_NAME for name in imports),
        backup_present=game_exe.with_suffix(game_exe.suffix + BACKUP_SUFFIX).is_file(),
        game_version=version,
        supported=version == SUPPORTED_GAME_VERSION,
    )


def enable(game_exe: Path, plugin_dll: Path) -> PatchStatus:
    """Adds the plugin to Game.exe's imports and puts the DLL beside it.

    The original Game.exe is copied aside first and never overwritten afterwards: a second
    enable on an already-patched file would otherwise make the backup a patched one, and there
    would be nothing left to go back to.
    """
    current = status(game_exe)
    if not current.supported:
        raise GamePatchError(
            f"Game.exe is version {current.game_version or 'unknown'}; this plugin is built for "
            f"{SUPPORTED_GAME_VERSION} and would be writing to the wrong addresses"
        )
    if not plugin_dll.is_file():
        raise GamePatchError(f"the plugin is missing: {plugin_dll}")
    if current.installed:
        # The import is already there, but the label may not be — and the two have to agree. A
        # marker naming a graphic that is not in any archive is worse than no marker at all: the
        # plugin would point the game at a label it cannot load. So the label step runs anyway.
        destination = game_exe.parent / PLUGIN_DLL_NAME
        if plugin_dll.resolve() != destination.resolve():
            shutil.copy2(plugin_dll, destination)
        _install_label(game_exe.parent)
        return current

    backup = game_exe.with_suffix(game_exe.suffix + BACKUP_SUFFIX)
    if not backup.exists():
        shutil.copy2(game_exe, backup)

    pe = _PE(bytearray(game_exe.read_bytes()))
    # The payload's own addresses depend on where the section lands, and where it lands does not
    # depend on the payload — so the section is sized from a first pass and then built for real.
    probe_rva = _align(
        max(
            struct.unpack_from("<I", pe.data, h + 12)[0]
            + max(struct.unpack_from("<I", pe.data, h + 8)[0],
                  struct.unpack_from("<I", pe.data, h + 16)[0])
            for h in pe.section_headers()
        ),
        pe.section_alignment,
    )
    payload = _build_import_section(pe, PLUGIN_DLL_NAME, PLUGIN_ANCHOR, probe_rva)

    patched, section_rva = _append_section(pe, b".pd2rst", payload)
    if section_rva != probe_rva:  # pragma: no cover - the two agree by construction
        raise GamePatchError("section landed somewhere unexpected")

    out = _PE(patched)
    out.set_directory(_IMAGE_DIRECTORY_ENTRY_IMPORT, section_rva, len(payload))
    # A bound-import directory describes the OLD import list and would be stale now; the loader
    # falls back to the real imports when it is absent.
    out.set_directory(_IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT, 0, 0)

    destination = game_exe.parent / PLUGIN_DLL_NAME
    if plugin_dll.resolve() != destination.resolve():
        shutil.copy2(plugin_dll, destination)
    game_exe.write_bytes(bytes(out.data))
    _install_label(game_exe.parent)
    return status(game_exe)


def _install_label(install_dir: Path) -> bool:
    """Puts the RESTART graphic into the archive and tells the plugin it is there.

    A failure here is not a failure of the feature: the entry simply keeps the label it copied
    from the game's own first line. So this reports rather than raises — losing Quick Restart
    entirely over a cosmetic step would be the worse trade.
    """
    marker = install_dir / LABEL_MARKER_NAME
    archives = _label_archives(install_dir)
    if not archives or not LABEL_SOURCE.is_file():
        marker.unlink(missing_ok=True)
        return False
    try:
        payload = LABEL_SOURCE.read_bytes()
        for archive in archives:
            mpq_asset.add(archive, LABEL_ARCHIVE_PATH, payload)
            # Read it back out of the archive rather than trusting the write: the plugin is about
            # to name this graphic, and the game does not survive being pointed at one that is
            # missing.
            if mpq_asset.read(archive, LABEL_ARCHIVE_PATH) != payload:
                raise mpq_asset.MpqError(f"the label did not read back from {archive.name}")
    except (mpq_asset.MpqError, OSError):
        for archive in archives:
            mpq_asset.restore(archive)
        marker.unlink(missing_ok=True)
        return False
    _teach_d2gl(install_dir)
    marker.write_text(LABEL_CELL_FILE)
    return True


def _label_size(dc6: bytes) -> tuple[int, int]:
    """The label graphic's own width and height, read from it rather than written down twice."""
    frame = struct.unpack_from("<I", dc6, 24)[0]
    width, height = struct.unpack_from("<2i", dc6, frame + 4)
    return width, height


def _teach_d2gl(install_dir: Path) -> bool:
    """Makes D2GL render our line the way it renders the others.

    With its hd_text on — the default — D2GL does not draw these labels from their graphics at all;
    it re-renders them as text, larger and cleaner, and leaves anything it does not recognise as the
    plain sprite. That is the whole of the difference people see. It recognises a label by the size
    of the graphic's last cell, so this registers ours. Failing is not fatal: the line then looks
    the way it did before, which is a cosmetic difference and nothing more.
    """
    width, height = _label_size(LABEL_SOURCE.read_bytes())
    patched = False
    for dll in _d2gl_libraries(install_dir):
        try:
            d2gl_patcher.enable(dll, width=width, height=height)
            patched = True
        except (d2gl_patcher.D2glPatchError, OSError):
            d2gl_patcher.disable(dll)
    return patched


def _d2gl_libraries(install_dir: Path) -> list[Path]:
    return [path for path in (install_dir / d2gl_patcher.D2GL_DLL_NAME,
                              install_dir / "Live" / d2gl_patcher.D2GL_DLL_NAME) if path.is_file()]


def _label_archives(install_dir: Path) -> list[Path]:
    """Every copy of the archive the game might end up reading.

    PD2Launcher keeps a pristine set under Live/ and copies it over the install on launch — that is
    how a patched Game.exe came back pristine and the plugin silently stopped loading. The same
    would happen to the label, except worse: the marker file would survive, the plugin would go on
    naming a graphic that had just been restored away, and the game does not survive that. So the
    Live copy is patched too, and a restore puts both back.
    """
    return [path for path in (install_dir / LABEL_ARCHIVE_NAME,
                              install_dir / "Live" / LABEL_ARCHIVE_NAME) if path.is_file()]


def disable(game_exe: Path) -> PatchStatus:
    """Puts the original Game.exe back and removes the plugin.

    The backup is kept until the game is genuinely unpatched, so a failure halfway leaves
    something to recover from rather than a half-restored executable and no original.
    """
    backup = game_exe.with_suffix(game_exe.suffix + BACKUP_SUFFIX)
    if backup.is_file():
        shutil.copy2(backup, game_exe)

    plugin = game_exe.parent / PLUGIN_DLL_NAME
    plugin.unlink(missing_ok=True)

    (game_exe.parent / LABEL_MARKER_NAME).unlink(missing_ok=True)
    for archive in _label_archives(game_exe.parent):
        mpq_asset.restore(archive)
    for dll in _d2gl_libraries(game_exe.parent):
        d2gl_patcher.disable(dll)

    result = status(game_exe)
    if not result.installed and backup.is_file():
        backup.unlink()
        result = status(game_exe)
    return result
