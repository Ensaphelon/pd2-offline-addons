"""Read back the DC6 we generated, the way the game's own loader would, and render it."""
import struct, sys
from dcc import decode
from tools import raw_from_mpq, palette, png
from build_dc6 import build

name = sys.argv[1]
frames, w, h, box = decode(raw_from_mpq("d2data.mpq", f"data\\global\\overlays\\{name}.dcc"))[0]
blob = build(frames, w, h)

version, sub, zero, term, dirs, count = struct.unpack_from("<6I", blob, 0)
assert (version, dirs, term) == (6, 1, 0xEEEEEEEE), (version, dirs, hex(term))
pointers = struct.unpack_from(f"<{count}I", blob, 24)
out = []
for p in pointers:
    flip, fw, fh, ox, oy, _u, nxt, length = struct.unpack_from("<8I", blob, p)
    data = blob[p + 32:p + 32 + length]
    pixels = bytearray(fw * fh)
    x, y, i = 0, fh - 1, 0
    while i < len(data):
        b = data[i]; i += 1
        if b == 0x80:
            x, y = 0, y - 1
        elif b & 0x80:
            x += b & 0x7F
        else:
            pixels[y * fw + x:y * fw + x + b] = data[i:i + b]
            x += b; i += b
    out.append((fw, fh, pixels))

pal = palette()
cols = min(len(out), 8); rows = (len(out) + cols - 1) // cols
sw, sh = cols * w, rows * h
canvas = [[0, 0, 0, 255] * sw for _ in range(sh)]
for idx, (fw, fh, px) in enumerate(out):
    ox, oy = (idx % cols) * w, (idx // cols) * h
    for yy in range(fh):
        row = canvas[oy + yy]
        for xx in range(fw):
            v = px[yy * fw + xx]
            if v:
                r, g, b = pal[v]; p4 = (ox + xx) * 4
                row[p4], row[p4+1], row[p4+2], row[p4+3] = r, g, b, 255
png(f"{name}-roundtrip.png", sw, sh, canvas)
print("frames", len(out), "wrote", f"{name}-roundtrip.png")
