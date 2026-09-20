import struct, zlib, sys
sys.path.insert(0, "/Users/ramil/Projects/pd2-holy-inventory/.local/dependencies/cain")
from core.mpq import MPQArchive

BASE = "/Users/ramil/Library/Application Support/CrossOver/Bottles/Diablo II/drive_c/Program Files (x86)/Diablo II/"

def raw_from_mpq(archive: str, name: str) -> bytes:
    """Cain's reader assumes a sector table; a stored (uncompressed) entry has none."""
    a = MPQArchive(BASE + archive)
    block = a._find(name)
    if block is None:
        raise KeyError(name)
    pos, comp, uncomp, flags = a.block_table[block]
    if flags & 0x00000300:
        return a.read_file(name)
    return a.raw[pos + a.archive_offset: pos + a.archive_offset + comp]

def palette(act="ACT1"):
    data = raw_from_mpq("d2data.mpq", f"data\\global\\palette\\{act}\\pal.dat")
    return [(data[i*3+2], data[i*3+1], data[i*3]) for i in range(256)]

def png(path, width, height, rgba_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgba_rows)
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)
